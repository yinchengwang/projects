---
project: sqlite
feature: btree-integrity-check
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeIntegrityCheck, sqlite3BtreeCount, sqlite3BtreeRowCountEst]
---

# B-tree 完整性检查

## 一句话职责

`sqlite3BtreeIntegrityCheck` 负责把一个数据库文件里的所有 B-tree 从根页往下走一遍，逐页核对页头字段、单元偏移、单元键序、覆盖完整性、溢出页链与指针映射（ptrmap），并统计「每棵树被引用了几次」，把发现的每一条问题累积成一段错误文本返回；它只做检查与报告，不做任何修复，也不负责页缓存的淘汰。

## 机制是什么

完整性检查在 SQLite 里分成三层，边界由「谁来收集错误」和「谁来遍历树」划分：

**第一层：SQL 层入口。** `PRAGMA integrity_check` / `PRAGMA quick_check` 由 `src/pragma.c` 的 `PragTyp_INTEGRITY_CHECK` 分支处理。它做两件事：
- **收集根页号。** 遍历 `db->aDb[i].pSchema->tblHash` 里所有表与索引，把有 rowid 的表的 `pTab->tnum` 和每个索引的 `pIdx->tnum` 填进 `aRoot[]`；`aRoot[0]` 放的是根页个数 `cnt`（不是页号）。带表名参数（`PRAGMA integrity_check(TABLENAME)`）时，`aRoot[1]` 先被填成 **0**，这个 0 是「部分检查」的信号。
- **生成检查指令。** `sqlite3VdbeAddOp4(v, OP_IntegrityCk, 1, cnt, 8, (char*)aRoot, P4_INTARRAY)` 发出 `OP_IntegrityCk`，错误数上限写进寄存器 1：`sqlite3VdbeAddOp2(v, OP_Integer, mxErr-1, 1)`。`mxErr` 默认取 `SQLITE_INTEGRITY_CHECK_ERROR_MAX`，该宏在 `src/pragma.c` 里定义为 **100**；`PRAGMA integrity_check(N)` 传的数字 ≤ 0 时也回落到这个默认值。

**第二层：VDBE 指令。** `OP_IntegrityCk`（`src/vdbe.c`）是唯一的调用方：`rc = sqlite3BtreeIntegrityCheck(db, db->aDb[pOp->p5].pBt, &aRoot[1], &aMem[pOp->p3], nRoot, (int)pnErr->u.i+1, &nErr, &z);`。注意参数是 `&aRoot[1]`——`aRoot[0]`（根页个数）在这一层被剥掉，只把真正的根页号数组传下去。错误上限传的是 `(int)pnErr->u.i+1`，因为寄存器里存的是「剩余错误数」（已减去 1），加回来才是真实上限。指令末尾把错误文本 `z` 写进 `pIn1`（寄存器 P1+1）；`nErr==0` 时该寄存器被置为 NULL（对应 `PRAGMA integrity_check` 输出 `ok`），`rc!=0` 时释放 `z` 并跳 `abort_due_to_error`。

**第三层：B-tree 层的实际检查。** `sqlite3BtreeIntegrityCheck`（`src/btree.c`）是本机制的主体，负责准备上下文、遍历、收集错误、产出报告。

**本机制的边界：** 不负责损坏后的修复（`REINDEX`、`VACUUM` 属别的机制）、不负责页缓存的强制淘汰（I 组）、不负责 UNIQUE / CHECK / NOT NULL 约束的语义校验（那是 `src/pragma.c` 里 `OP_IntegrityCk` 之后由 VDBE 逐行做的检查，依赖 `sqlite3GenerateIndexKey` 与 `OP_Found` / `OP_IFindKey`）、不负责外键检查（`PRAGMA foreign_key_check` 是独立路径）。

## 怎么实现的

### `sqlite3BtreeIntegrityCheck`：准备阶段

签名（`src/btree.c`）：`int sqlite3BtreeIntegrityCheck(sqlite3 *db, Btree *p, Pgno *aRoot, Mem *aCnt, int nRoot, int mxErr, int *pnErr, char **pzOut)`。

1. **判定部分检查。** `if( aRoot[0]==0 ){ bPartial = 1; if( aRoot[1]!=1 ) bCkFreelist = 0; }`。`aRoot[0]==0` 表示根页列表不完整（单表检查）；此时只有当 `aRoot[1]==1`（被检查的表是 `sqlite_schema`）时才继续检查 freelist，否则 `bCkFreelist` 置 0。
2. **进入 B-tree。** `sqlite3BtreeEnter(p)`，并断言 `p->inTrans>TRANS_NONE && pBt->inTransaction>TRANS_NONE`——函数头注释写明「A read-only or read-write transaction must be opened before calling this function」。
3. **初始化上下文。** `memset(&sCheck, 0, sizeof(sCheck))` 后填 `sCheck.db`、`sCheck.pBt`、`sCheck.pPager`；`sCheck.nCkPage = btreePagecount(sCheck.pBt)`；`sCheck.mxErr = mxErr`；`sqlite3StrAccumInit(&sCheck.errMsg, 0, zErr, sizeof(zErr), SQLITE_MAX_LENGTH)`——`zErr` 是栈上 `char zErr[100]`，作为 `StrAccum` 的小缓冲后备，超出则走堆分配；`SQLITE_MAX_LENGTH` 在 `src/sqliteLimit.h` 里的默认值是 **1000000000**。
4. **空库直接返回。** `if( sCheck.nCkPage==0 ) goto integrity_ck_cleanup;`。
5. **分配位图与最小堆。** `sCheck.aPgRef = sqlite3MallocZero((sCheck.nCkPage / 8)+ 1)`——每页 1 bit 的引用位图，长度按 `nCkPage/8+1` 字节分配。`sCheck.heap = (u32*)sqlite3Malloc( pBt->pageSize*2 )`——覆盖度检查用的最小堆，容量按 **页大小 × 2** 个 `u32` 分配；`SQLITE_DEBUG` 编译下还设 `sCheck.mxHeap = pBt->pageSize/2 - 1`，用于断言堆未溢出。
6. **标记保留页。** `i = PENDING_BYTE_PAGE(pBt); if( i<=sCheck.nCkPage ) setPageReferenced(&sCheck, i);`——`PENDING_BYTE_PAGE` 指定的页不应被报成「never used」。
7. **检查 freelist。** `checkList(&sCheck, 1, get4byte(&pBt->pPage1->aData[32]), get4byte(&pBt->pPage1->aData[36]))`——链首页号取自数据库页 1 的偏移 32，链长取自偏移 36。
8. **autovacuum 模式下的根页一致性。** 非部分检查时：auto-vacuum 模式下取 `aRoot[]` 里最大的根页号 `mx`，与页 1 偏移 52 处的 `mxInHdr` 比较，不等则报「max rootpage (%u) disagrees with header (%u)」；非 auto-vacuum 模式下检查页 1 偏移 64 处是否为 0，非 0 则报「incremental_vacuum enabled with a max rootpage of zero」。
9. **关掉单元尺寸检查标志。** `pBt->db->flags &= ~(u64)SQLITE_CellSizeCk;`，检查结束后 `pBt->db->flags = savedDbFlags;` 还原。

### `sqlite3BtreeIntegrityCheck`：遍历阶段

对每个根页 `aRoot[i]`（`aRoot[i]==0` 的跳过）：

1. `sCheck.nRow = 0;`，auto-vacuum 模式下对 `aRoot[i]>1` 的根页调 `checkPtrmap(&sCheck, aRoot[i], PTRMAP_ROOTPAGE, 0)`。
2. `sCheck.v0 = aRoot[i];` 后调 `checkTreePage(&sCheck, aRoot[i], &notUsed, LARGEST_INT64)`——**根页的键上界是 `LARGEST_INT64`**，即根页上第一个键没有上界限制。
3. `sqlite3MemSetArrayInt64(aCnt, i, sCheck.nRow);` 把这棵树的行数写进 `aCnt` 数组的第 i 项。

遍历结束后，若不是部分检查，扫全部页 `i=1..sCheck.nCkPage`：
- 未被引用的页报「Page %u: never used」；auto-vacuum 模式下指针映射页本身不算「从未使用」（条件是 `PTRMAP_PAGENO(pBt, i)!=i || !pBt->autoVacuum`）。
- 被引用但恰好是指针映射页的报「Page %u: pointer map referenced」（条件是 `PTRMAP_PAGENO(pBt, i)==i && pBt->autoVacuum`）。

最后 `integrity_ck_cleanup:` 处释放 `sCheck.heap` 与 `sCheck.aPgRef`，`*pnErr = sCheck.nErr`，`nErr==0` 时 `*pzOut = 0`，否则 `*pzOut = sqlite3StrAccumFinish(&sCheck.errMsg)`，返回 `sCheck.rc`。

### `checkTreePage`：单页检查（核心）

签名（`src/btree.c`）：`static int checkTreePage(IntegrityCk *pCheck, Pgno iPage, i64 *piMinKey, i64 maxKey)`，返回子树深度（根页返回 0，根的子页返回 1，依此类推）。

函数头注释列出五类检查：单元与空闲块不重叠且合起来完全覆盖整页；整数单元键有序；溢出页完整性；递归调用 `checkTreePage` 检查所有子页；所有子页深度一致。

执行顺序：

1. **前置检查。** `checkProgress(pCheck);`（处理中断与进度回调）；`if( pCheck->mxErr==0 ) return 0;`（错误数用尽即停）。`if( iPage==0 ) return 0;`。`if( checkRef(pCheck, iPage) ) return 0;`——`checkRef` 检查页号越界（`iPage>pCheck->nCkPage || iPage==0`，报「invalid page number %u」）与重复引用（报「2nd reference to page %u」）。
2. **取页并强制重新解析。** `btreeGetPage(pBt, iPage, &pPage, 0)`，失败报「unable to get the page. error code=%d」。随后 `pCheck->nAbove++`，并检查 `if( pCheck->nAbove > BTCURSOR_MAX_DEPTH )`——`BTCURSOR_MAX_DEPTH` 在 `src/btreeInt.h` 里定义为 **20**，越界报「btree depth exceeds %d」。
3. **清掉 `isInit` 强制走损坏检测。** `savedIsInit = pPage->isInit; pPage->isInit = 0;` 再调 `btreeInitPage(pPage)`——源码注释写明「Clear MemPage.isInit to make sure the corruption detection code in btreeInitPage() is executed」；失败时断言 `rc==SQLITE_CORRUPT` 并报「btreeInitPage() returns error code %d」。接着调 `btreeComputeFreeSpace(pPage)`，失败报「free space corruption」。
4. **读页头。** `contentOffset = get2byteNotZero(&data[hdr+5])`（页头偏移 5，2 字节）；`nCell = get2byte(&data[hdr+3])`（页头偏移 3，2 字节，单元个数）；`cellStart = hdr + 12 - 4*pPage->leaf`（单元指针数组起点，叶页少 4 字节）。
5. **统计行数。** `if( pPage->leaf || pPage->intKey==0 ){ pCheck->nRow += nCell; }`——叶页或非整数键（索引）树的单元计入行数。
6. **非叶页先递归查最右孩子。** `pgno = get4byte(&data[hdr+8])`（页头偏移 8 处的最右孩子），auto-vacuum 下先 `checkPtrmap(pCheck, pgno, PTRMAP_BTREE, iPage)`，然后 `depth = checkTreePage(pCheck, pgno, &maxKey, maxKey);`，并置 `keyCanBeEqual = 0;`。叶页则 `heap = pCheck->heap; heap[0] = 0;` 初始化最小堆。
7. **倒序遍历单元**（`for(i=nCell-1; i>=0 && pCheck->mxErr; i--)`）。每个单元依次做：
   - **偏移范围检查。** `pc = get2byteAligned(pCellIdx);` 若 `pc<contentOffset || pc>usableSize-4`，报「Offset %u out of range %u..%u」并 `doCoverageCheck = 0;`（放弃本页覆盖度检查，因为字节区间已不可信）。
   - **解析单元。** `pPage->xParseCell(pPage, pCell, &info);`；若 `pc+info.nSize>usableSize` 报「Extends off end of page」并放弃覆盖度检查。
   - **载荷头检查。** `if( info.nPayload && info.pPayload[0]<2 )` 报「Bad cell header size」。
   - **整数键有序性。** `if( pPage->intKey )` 时：`keyCanBeEqual ? (info.nKey > maxKey) : (info.nKey >= maxKey)` 命中则报「Rowid %lld out of order」；随后 `maxKey = info.nKey; keyCanBeEqual = 0;`（源码注释：「Only the first key on the page may ==maxKey」）。
   - **溢出页链。** `if( info.nPayload>info.nLocal )`：页数 `nPage = (info.nPayload - info.nLocal + usableSize - 5)/(usableSize - 4)`，链首页号 `pgnoOvfl = get4byte(&pCell[info.nSize - 4])`，auto-vacuum 下 `checkPtrmap(pCheck, pgnoOvfl, PTRMAP_OVERFLOW1, iPage)`，然后 `checkList(pCheck, 0, pgnoOvfl, nPage)`。
   - **内部页的左孩子。** `pgno = get4byte(pCell)`（单元头 4 字节是左孩子页号），auto-vacuum 下 `checkPtrmap(pCheck, pgno, PTRMAP_BTREE, iPage)`，然后 `d2 = checkTreePage(pCheck, pgno, &maxKey, maxKey); keyCanBeEqual = 0;`；`d2!=depth` 报「Child page depth differs」并把 `depth` 改成 `d2`。
   - **叶页则把单元区间塞进最小堆。** `btreeHeapInsert(heap, (pc<<16)|(pc+info.nSize-1));`——堆元素是「起始地址 << 16 | 结束地址」。
8. **覆盖度检查**（`doCoverageCheck && pCheck->mxErr>0` 时）。内部页在此补齐单元入堆；然后沿 freeblock 链把每个空闲块也插入堆：`i = get2byte(&data[hdr+1])`（页头偏移 1，首个空闲块偏移），`size = get2byte(&data[i+2])`，`btreeHeapInsert(heap, (((u32)i)<<16)|(i+size-1))`，下一块偏移 `j = get2byte(&data[i])`。最后从最小堆里逐个弹出比较：`prev = contentOffset - 1` 作为隐含的起始项；若 `(prev&0xffff)>=(x>>16)` 报「Multiple uses for byte %u of page %u」并 break；否则累加碎片字节 `nFrag += (x>>16) - (prev&0xffff) - 1`。收尾 `nFrag += usableSize - (prev&0xffff) - 1`，然后与页头偏移 7 处的碎片字节数比较：`if( heap[0]==0 && nFrag!=data[hdr+7] )` 报「Fragmentation of %u bytes reported as %u on page %u」。
9. **收尾。** `if( !doCoverageCheck ) pPage->isInit = savedIsInit;`（放弃覆盖度检查时恢复 `isInit`），`releasePage(pPage);`，恢复 `zPfx` / `v1` / `v2`，`pCheck->nAbove--;`，`return depth+1;`。

### 错误收集与辅助函数

- **`checkAppendMsg`**（`src/btree.c`）：每条错误先调 `checkProgress(pCheck)`；`if( !pCheck->mxErr ) return;` 后 `pCheck->mxErr--; pCheck->nErr++;`，把 `pCheck->zPfx` 按 `v0`/`v1`/`v2` 三个占位符填充后拼上消息体。消息之间用 `"\n"` 分隔（`pCheck->errMsg.nChar` 非零时先追加换行）。
- **`checkProgress`**（`src/btree.c`）：`AtomicLoad(&db->u1.isInterrupted)` 为真时置 `pCheck->rc = SQLITE_INTERRUPT` 并 `pCheck->mxErr = 0`；有进度回调时按 `pCheck->nStep % db->nProgressOps` 触发，回调返回非 0 同样置 `SQLITE_INTERRUPT` 并停止。
- **`checkOom`**（`src/btree.c`）：`rc` 是 `SQLITE_NOMEM` 或 `SQLITE_IOERR_NOMEM` 时置 `pCheck->rc = SQLITE_NOMEM`、`pCheck->mxErr = 0`，并在 `nErr==0` 时把 `nErr` 增到 1（保证「出错但消息为空」的情形也能被调用方看见）。
- **`checkRef`**（`src/btree.c`）：页号越界或页号 0 报「invalid page number %u」；已被引用过报「2nd reference to page %u」。这是「一页被用两次」的检测点。
- **`getPageReferenced` / `setPageReferenced`**（`src/btree.c`）：对 `pCheck->aPgRef` 做位操作——`pCheck->aPgRef[iPg/8] & (1 << (iPg & 0x07))`。
- **`checkList`**（`src/btree.c`）：检查 freelist（`isFreeList` 为真）或溢出页链。逐页 `checkRef` 标记引用；freelist 叶页上 `n = (u32)get4byte(&pOvflData[4])` 是叶子数，若 `n>pCheck->pBt->usableSize/4-2` 报「freelist leaf count too big on page %u」，否则逐个检查 `get4byte(&pOvflData[8+i*4])` 指向的自由页。溢出链在 auto-vacuum 下检查后继页的 ptrmap 条目：`checkPtrmap(pCheck, i, PTRMAP_OVERFLOW2, iPage)`。链长不符报「%s is %u but should be %u」。
- **`checkPtrmap`**（`src/btree.c`）：`ptrmapGet(pCheck->pBt, iChild, &ePtrmapType, &iPtrmapParent)` 读指针映射，读失败报「Failed to read ptrmap key=%u」；类型或父页不符报「Bad ptr map entry key=%u expected=(%u,%u) got=(%u,%u)」。`eType` 取 `src/btreeInt.h` 里的宏：`PTRMAP_ROOTPAGE`=**1**、`PTRMAP_FREEPAGE`=**2**、`PTRMAP_OVERFLOW1`=**3**、`PTRMAP_OVERFLOW2`=**4**、`PTRMAP_BTREE`=**5**。
- **`btreeHeapInsert` / `btreeHeapPull`**（`src/btree.c`）：最小堆实现。堆元素格式是 `(起始地址<<16)|结束地址`；`aHeap[0]` 是堆元素个数，`aHeap[1]` 是堆顶。

### `sqlite3BtreeCount`：精确计数

签名（`src/btree.c`）：`int sqlite3BtreeCount(sqlite3 *db, BtCursor *pCur, i64 *pnEntry)`。这是 `sqlite3BtreeIntegrityCheck` 之外、与完整性相关的第二个 B-tree 层接口。

- 先 `moveToRoot(pCur)`；返回 `SQLITE_EMPTY` 时 `*pnEntry = 0; return SQLITE_OK;`（空表，计数 0）。
- 主循环条件 `while( rc==SQLITE_OK && !AtomicLoad(&db->u1.isInterrupted) )`，源码注释写明「the following loop runs one iteration for each page in the B-Tree structure (not including overflow pages)」。
- 计数规则：`if( pPage->leaf || !pPage->intKey ){ nEntry += pPage->nCell; }`——叶页或非整数键树计入单元数。
- 遍历方式：叶页时用 `do { ... moveToParent(pCur); }while ( pCur->ix>=pCur->pPage->nCell );` 向上回退到父页的第一个内部单元；然后 `pCur->ix++`，再 `moveToChild(pCur, get4byte(&pPage->aData[pPage->hdrOffset+8]))`（`iIdx==pPage->nCell` 时走最右孩子）否则 `moveToChild(pCur, get4byte(findCell(pPage, iIdx)))`。
- 全部页访问完（`pCur->iPage==0`）时 `*pnEntry = nEntry; return moveToRoot(pCur);`——返回前把游标移回根页。

### `sqlite3BtreeRowCountEst`：估算计数

签名（`src/btree.c`）：`i64 sqlite3BtreeRowCountEst(BtCursor *pCur)`。函数头注释写明「Return an estimate for the number of rows in the table that pCur is pointing to. Return a negative number if no estimate is currently available.」

- `if( pCur->eState!=CURSOR_VALID ) return 0;`（游标无效）。
- `if( NEVER(pCur->pPage->leaf==0) ) return -1;`（不是叶页则没有估算值，返回 -1）。源码注释说明该接口目前只被 `OP_IfSizeBetween` 指令与 `P3=1` 的 `OP_Count` 指令调用。
- 估算公式：`n = pCur->pPage->nCell;` 然后 `for(i=0; i<pCur->iPage; i++){ n *= pCur->apPage[i]->nCell+1; }`——把路径上每层父页的单元数加一后连乘。

## 关键取舍

- **错误收集用「预算 + 字符串累加器」而非「逐条返回」。** `mxErr` 是剩余错误预算，`checkAppendMsg` 每写一条就 `mxErr--` 并 `nErr++`；预算耗尽后所有检查点都提前返回。`sqlite3BtreeIntegrityCheck` 返回的 `*pnErr` 是错误总数，`*pzOut` 是全部错误拼成的文本——调用方一次拿到全部上下文，而不是逐条询问。
- **`mxErr` 的默认值是 100。** `src/pragma.c` 里 `SQLITE_INTEGRITY_CHECK_ERROR_MAX` 定义为 **100**；`PRAGMA integrity_check(N)` 传 N ≤ 0 时回落到这个值。寄存器里存的是 `mxErr-1`，`OP_IntegrityCk` 调用时再加回 1，形成「寄存器存剩余额度、函数收绝对上限」的编码。
- **遍历用递归而不是显式栈。** `checkTreePage` 直接递归调用自身，靠 `pCheck->nAbove` 计数并与 `BTCURSOR_MAX_DEPTH`（**20**）比较来防超深树导致的栈溢出——超界时报「btree depth exceeds %d」而不是崩溃。
- **「重复引用」与「从未使用」靠一张按页位图发现。** `aPgRef` 每页 1 bit，`checkRef` 在遍历中标记，遍历结束后统一扫一遍找未标记的页。**架构推论：** 这是一种「先收集、后判定」的两阶段设计，代价是必须保留整库页数大小的位图（`nCkPage/8+1` 字节），收益是无需在遍历时立刻决定「这一页是不是多余的」。
- **覆盖度检查用最小堆而不是排序数组。** 单元区间与空闲块区间混在同一个堆里弹出比较，堆顶始终是当前最小起始地址，因而一次扫描就能同时发现「字节被多用」和「碎片字节数」。`btreeHeapInsert` / `btreeHeapPull` 的堆元素把起止地址压进一个 `u32`（高 16 位起始、低 16 位结束），因此堆容量按 `pageSize*2` 个 `u32` 分配。
- **页级检查强制重新解析页头。** `pPage->isInit = 0` 后再调 `btreeInitPage`，源码注释明确目的是让 `btreeInitPage()` 内部的损坏检测代码被执行——也就是说，完整性检查不复用普通读路径的缓存解析结果。
- **一部分错误会主动放弃本页的覆盖度检查。** 单元偏移越界、单元越出页尾、单元头异常这三种情况都设 `doCoverageCheck = 0`，因为此时字节区间已经不可信，再算碎片数只会产生误报。放弃时还要把 `pPage->isInit` 恢复成 `savedIsInit`。
- **整数键的上界比较区分「首个键」与「后续键」。** `keyCanBeEqual` 初始为 1（一页的第一个键允许等于上界），处理过第一个键后就置 0（后续键必须严格小于上界）。递归进入子页后同样把 `keyCanBeEqual` 置 0。
- **autovacuum 检查全部被 `#ifndef SQLITE_OMIT_AUTOVACUUM` 包起来。** 指针映射校验、根页号与页头的一致性校验、「pointer map referenced」检查都在这个条件编译块内；非 autovacuum 库走的是简化路径。
- **部分检查时跳过 freelist 与「每页都被引用」两项。** 原因写在 `sqlite3BtreeIntegrityCheck` 的函数头注释里：无法知道未被检查的 B-tree 覆盖了哪些页，所以这两项在部分检查下必然误报；只有 `aRoot[1]==1`（被检查的是 `sqlite_schema`）时才保留 freelist 检查。
- **`quick_check` 是 `integrity_check` 的裁剪版。** `src/pragma.c` 的注释写明「quick_check is linear time whereas integrity_check is O(NlogN)」；两者的 B-tree 层检查完全相同，差异在 `OP_IntegrityCk` 之后的 VDBE 部分——`if( !isQuick )` 才执行索引条目与表行的交叉校验（`sqlite3GenerateIndexKey` + `OP_Found` / `OP_IFindKey`），`quick_check` 跳过这一段。
- **中断与进度回调被织进检查循环。** `checkProgress` 在 `checkAppendMsg` 与 `checkTreePage` 的开头都会被调用，`db->u1.isInterrupted` 一旦置位就返回 `SQLITE_INTERRUPT` 并停掉检查——长时间运行的完整性检查因此可被用户取消。
- **`sqlite3BtreeCount` 与 `sqlite3BtreeRowCountEst` 是两种不同的计数语义。** 前者精确遍历全树（源码注释：「one iteration for each page in the B-Tree structure (not including overflow pages)」），后者只按游标当前位置的路径做乘法估算。`OP_Count` 指令按 `pOp->p3` 选择用哪个：`pOp->p3` 非零走 `sqlite3BtreeRowCountEst`，否则走 `sqlite3BtreeCount`。
- **`sqlite3BtreeCount` 结束前把游标移回根页。** 返回路径是 `*pnEntry = nEntry; return moveToRoot(pCur);`，不留下悬空的游标位置。

## 与主核 / AI 的关系

本机制是 SQLite 面向「数据库文件可能被外部破坏」这一前提的自检能力：从 SQL 层（`PRAGMA integrity_check`）到 VDBE 指令（`OP_IntegrityCk`）再到 B-tree 层（`sqlite3BtreeIntegrityCheck`），形成一条三层链路，且每一层只做自己那一层的事。

**架构推论：** 主数据库内核 `engine/db/storage/` 若要提供同类自检能力，最值得借鉴的是「检查器只报告、不修复」这条边界——`sqlite3BtreeIntegrityCheck` 在发现问题的分支上只调 `checkAppendMsg` 累积文本，绝不写回页内容，修复被完全外置到 `REINDEX` / `VACUUM` 等独立机制。主核若把「检测」与「修复」写在同一个函数里，会让自检路径带上写锁需求。

**架构推论：** 「错误预算 + 一次性文本报告」这套接口（`mxErr` / `*pnErr` / `*pzOut`）比「逐条回调」更适合嵌入式场景：调用方只需一个 `char *` 与一个 `int`，不必提供回调栈。对应地，主核若采用回调式自检 API，在跨 FFI 边界时要多付一次函数指针调用的成本。

**架构推论：** 覆盖度检查用「起始地址 << 16 | 结束地址」把区间压进一个 32 位整数，再交给最小堆——这是为「页大小上限 65536 字节」这一约束做的编码取舍（16 位足够表示页内任意偏移）。主核若支持更大页尺寸，就得改成 64 位元素或用两个 `u32`。

**架构推论：** `sqlite3BtreeIntegrityCheck` 在遍历时把「页被引用几次」记进位图、在遍历结束后统一判定「从未使用的页」——这个两阶段设计让「孤儿页」检测与「树结构遍历」解耦。主核的 WAL 与 checkpoint 层若有类似的页归属判定，可沿用同一结构。

**架构推论：** `sqlite3BtreeRowCountEst` 的乘法估算（每层 `nCell+1` 连乘）只对游标停在叶页时成立，否则返回 -1。这种「能估就估、不能估就返回明确的负值」而不是返回 0 的约定，让调用方能区分「表为空」与「无法估算」。主核的统计接口（catalog 层）沿用同一约定可避免调用方误判。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `sqlite3BtreeIntegrityCheck` | 完整性检查主入口：准备 `IntegrityCk` 上下文、分配页引用位图与最小堆、检查 freelist、逐棵遍历 B-tree、扫全文件找孤儿页与被误引用的指针映射页、产出错误文本 |
| src/btree.c | `checkTreePage` | 单页检查：读页头、逐单元查偏移范围与越界、查整数键序、查溢出页链、递归子页并比对深度、用最小堆做覆盖度与碎片检查 |
| src/btree.c | `checkAppendMsg` | 错误收集：扣减 `mxErr` 预算、累加 `nErr`、按 `zPfx` 填充 `v0`/`v1`/`v2` 三个占位符后拼入 `errMsg` |
| src/btree.c | `checkProgress` | 中断与进度回调：检测 `db->u1.isInterrupted`，按 `db->nProgressOps` 触发 `db->xProgress` |
| src/btree.c | `checkOom` | OOM 处理：把 `SQLITE_NOMEM` / `SQLITE_IOERR_NOMEM` 归一为 `SQLITE_NOMEM` 并停止检查 |
| src/btree.c | `checkRef` | 页引用检查：页号越界报「invalid page number」，重复引用报「2nd reference to page」 |
| src/btree.c | `getPageReferenced` / `setPageReferenced` | 页引用位图的读写：`aPgRef[iPg/8]` 的第 `iPg & 0x07` 位 |
| src/btree.c | `checkList` | freelist 与溢出页链检查：逐页标记引用、核对链长、auto-vacuum 下核对指针映射 |
| src/btree.c | `checkPtrmap` | 指针映射核对：`ptrmapGet` 读出 (类型, 父页) 与期望值比对 |
| src/btree.c | `btreeHeapInsert` / `btreeHeapPull` | 覆盖度检查用的最小堆；元素格式为 `(起始地址<<16)|结束地址` |
| src/btree.c | `sqlite3BtreeCount` | 精确计数：遍历 B-tree 的每一页累加可计数单元数，结束前把游标移回根页 |
| src/btree.c | `sqlite3BtreeRowCountEst` | 估算计数：按游标路径上每层 `nCell+1` 连乘；非叶页返回 -1 |
| src/btree.c | `btreeInitPage` | 页头解析：完整性检查通过清 `isInit` 强制其损坏检测代码执行 |
| src/btree.c | `btreeComputeFreeSpace` | 页内空闲空间统计：失败被报为「free space corruption」 |
| src/btreeInt.h | `IntegrityCk` | 检查上下文结构：`aPgRef` 页引用位图、`nCkPage` 受检页数、`mxErr` 错误预算、`nErr` 错误计数、`rc` 结果码、`nAbove` 当前递归深度、`zPfx` 与 `v0`/`v1`/`v2` 消息前缀占位符、`heap` 最小堆 |
| src/btreeInt.h | `BTCURSOR_MAX_DEPTH` | 宏，取值 20；`checkTreePage` 用它限制递归深度 |
| src/btreeInt.h | `PTRMAP_ROOTPAGE` / `PTRMAP_FREEPAGE` / `PTRMAP_OVERFLOW1` / `PTRMAP_OVERFLOW2` / `PTRMAP_BTREE` | 指针映射条目类型宏，取值分别为 1 / 2 / 3 / 4 / 5 |
| src/btree.h | `sqlite3BtreeIntegrityCheck` | 公开给 VDBE 层的声明（`src/btree.h`），与 `src/btree.c` 的实现配对 |
| src/vdbe.c | `OP_IntegrityCk` | 唯一的 B-tree 层调用方：调 `sqlite3BtreeIntegrityCheck`，把错误文本写进寄存器 P1+1，无错时置 NULL |
| src/pragma.c | `PragTyp_INTEGRITY_CHECK` | SQL 层入口：收集各表与索引的根页号填入 `aRoot[]`，发出 `OP_IntegrityCk`，并在 VDBE 侧做索引条目与约束的交叉校验 |
| src/pragma.c | `SQLITE_INTEGRITY_CHECK_ERROR_MAX` | 宏，取值 100；完整性检查的默认错误数上限 |

## 参考

- [PRAGMA integrity_check / quick_check 官方文档](https://sqlite.org/pragma.html#pragma_integrity_check)
- [SQLite 文件格式：B-tree 页与空闲页（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html)
- [SQLite B-tree 实现（sqlite.org 官方文档）](https://sqlite.org/btreetree.html)
- [SQLite 官方文档：VDBE 指令一览（sqlite.org）](https://sqlite.org/opcode.html)
- [SQLite 源码浏览：src/btree.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/btree.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
