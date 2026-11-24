---
project: sqlite
feature: btree-insert
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeInsert, balance_nonroot, sqlite3BtreeClearTable]
---

# B-tree 插入与单元搬移

## 一句话职责

`sqlite3BtreeInsert` 负责把一条新记录变成「单元（cell）」并塞进 B-tree 所在的那一页：当页内空间不够时先记一条 overflow 单元，再由 `balance` 触发 `balance_nonroot` 把本页与最多两个兄弟页的单元重新分配、必要时分出新页并往父页插入分隔单元；`sqlite3BtreeClearTable` 负责整表清空（递归清页并把页还回 freelist）。

## 机制是什么

B-tree 的插入在这套源码里被拆成三层，层次边界由函数划分得很清楚：

**第一层：定位。** 决定新单元应该插到哪一页的哪个槽位。这由 `sqlite3BtreeInsert` 自己完成——对 rowid 表走 `sqlite3BtreeTableMoveto`，对索引 / WITHOUT ROWID 表走 `sqlite3BtreeIndexMoveto` 或 `btreeMoveto`。定位结果用 `loc` 表达：`loc==0` 表示「游标正好指向要被覆盖的单元」，`loc<0` 表示「游标指向的键小于新键」，`loc>0` 表示「游标指向的键大于新键」。

**第二层：页内搬移。** 决定新单元在目标页里放在哪、页内剩余空间怎么记。这落在 `insertCellFast` 与 `allocateSpace` 上。`insertCellFast`（`src/btree.c`）是 `insertCell` 的裁剪版——注释明确说它假设 `pTemp` 与 `iChild` 均为零，**只从 `sqlite3BtreeInsert` 这一个地方调用**，因而可以被内联。

**第三层：平衡。** 当目标页装不下新单元时，把内容在兄弟页之间重新摊平。落在 `balance` → `balance_nonroot` / `balance_deeper` / `balance_quick` 这一组上。

三层的边界由「页内剩余空间」这个量串起来：`MemPage.nFree` 是页内可用字节数，`insertCellFast` 里 `if( sz+2>pPage->nFree )` 判定「放不下」；`sz+2` 里的 **2** 是单元指针在页头单元索引区占的字节数（页头 `cellOffset` 起、每单元 2 字节指针，`allocateSpace` 里 `gap = pPage->cellOffset + 2*pPage->nCell` 就是这个数组的末端）。

**溢出的中间态。** 放不下时 `insertCellFast` 不报错，而是把单元指针登记进 `pPage->apOvfl[]`（内容）与 `pPage->aiOvfl[]`（槽位序号），并把 `pPage->nOverflow` 加一。`apOvfl` / `aiOvfl` 数组长度是 **4**，但源码断言 `j < ArraySize(pPage->apOvfl)-1`，即实际最多用到 **3** 个槽位，第 4 个是"以防万一"预留的。`nOverflow` 非零的页会触发 `sqlite3BtreeInsert` 末尾的 `balance(pCur)`。

**本机制的边界：** 不负责页写回磁盘（那是 pager 层，`sqlite3PagerWrite` 只是被调用）、不负责事务可见性（那是 M 组，`sqlite3BtreeInsert` 只 `assert` 自己在 `TRANS_WRITE` 事务里）。

## 怎么实现的

### `sqlite3BtreeInsert`：主流程

函数签名（`src/btree.c`）：`int sqlite3BtreeInsert(BtCursor *pCur, const BtreePayload *pX, int flags, int seekResult)`。`flags` 只能取 `src/btree.h` 里的三个位：`BTREE_SAVEPOSITION`（**0x02**，插入后游标保持有效）、`BTREE_APPEND`（**0x08**，大概率是尾部追加）、`BTREE_PREFORMAT`（**0x80**，新单元已预先格式化好）。`src/btree.h` 里断言 `BTREE_PREFORMAT==OPFLAG_PREFORMAT`，即 VDBE 的同名标志位与它同值。

执行顺序：

1. **保存其他游标位置。** 若 `pCur->curFlags & BTCF_Multiple`，调 `saveAllCursors(p->pBt, pCur->pgnoRoot, pCur)`。`saveAllCursors`（`src/btree.c`）遍历 `pBt->pCursor` 链表找同 root 页的其他游标，找到才调 `saveCursorsOnList` → `saveCursorPosition`；找不到就把 `pExcept->curFlags` 上的 `BTCF_Multiple` 清掉。源码注释说明这一步不能省：`sqlite3BtreeInsert` 内部的 `btreeMoveto` 有可能是空操作（自增整型键插入时 VDBE 先调 `sqlite3BtreeLast` 取键），清掉游标会破坏这个优化。
2. **游标状态无效则回到根。** `pCur->eState>=CURSOR_REQUIRESEEK` 时调 `moveToRoot(pCur)`，并把 `loc` 置 0（表示"位置未知"）。`rc!=SQLITE_EMPTY` 的错误直接返回。
3. **区分表 B-tree 与索引 B-tree。** `pCur->pKeyInfo==0` 表示 rowid 表；否则表示索引或 WITHOUT ROWID 表。
   - **rowid 表路径**：若 `p->hasIncrblobCur`，调 `invalidateIncrblobCursors(p, pCur->pgnoRoot, pX->nKey, 0)` 让正在写的行上的 incrblob 游标失效。若 `pCur->curFlags&BTCF_ValidNKey` 且 `pX->nKey==pCur->info.nKey`（游标已指向将被覆盖的行），且 `pCur->info.nSize!=0` 且 `pCur->info.nPayload==(u32)pX->nData+pX->nZero`（新旧同尺寸），则**直接走 `btreeOverwriteCell(pCur, pX)` 返回**——这是一条"同尺寸覆盖"的快捷路径。否则若 `loc==0`，调 `sqlite3BtreeTableMoveto(pCur, pX->nKey, (flags & BTREE_APPEND)!=0, &loc)` 重新定位。
   - **索引路径**：`loc==0` 且未设 `BTREE_SAVEPOSITION` 时，若 `pX->nMem` 非零则用 `sqlite3BtreeIndexMoveto`（用解包后的记录 `UnpackedRecord r` 避免重复解码键），否则用 `btreeMoveto`。随后若 `loc==0` 且 `pCur->info.nKey==pX->nKey`（键相同），走 `btreeOverwriteCell(pCur, &x2)` 覆盖返回。
4. **准备单元字节序列。** `newCell = p->pBt->pTmpSpace`。
   - 若设了 `BTREE_PREFORMAT`：`szNew = p->pBt->nPreformatSize`；若 `szNew<4` 则强制为 **4** 并把 `newCell[3]` 置 0（单元最小 4 字节）。这是 `sqlite3BtreeTransferRow` 预格式化的接入路径。
   - 否则调 `fillInCell(pPage, newCell, pX, &szNew)`。`fillInCell`（`src/btree.c`）写单元头：`intKey` 页写 `nPayload` 变长整数加 `nKey` 变长整数，非 `intKey` 页只写 `nPayload`。若 `nPayload<=pPage->maxLocal`（整份载荷能放本页），就地拷入并用 0 补齐 `nPayload-nSrc`，单元大小 `n = nHeader + nPayload`；`n<4` 时补到 4。超限时走溢出路径：先算 `n = mn + (nPayload - mn) % (pPage->pBt->usableSize - 4)`，其中 `mn = pPage->minLocal`；若 `n > pPage->maxLocal` 则退回 `n = mn`；单元大小为 `*pnSize = n + nHeader + 4`，末尾 **4** 字节存首个溢出页号。
5. **覆盖分支。** `loc==0` 时：先 `sqlite3PagerWrite(pPage->pDbPage)` 把页置脏；`oldCell = findCell(pPage, idx)`；非叶页额外 `memcpy(newCell, oldCell, 4)` 保住左孩子指针；`BTREE_CLEAR_CELL` 清掉旧单元并取回 `info`。
   - 若 `info.nSize==szNew && info.nLocal==info.nPayload && (!ISAUTOVACUUM(p->pBt) || szNew<pPage->minLocal)`——即新旧单元等长且旧单元没有溢出部分——则**直接 `memcpy(oldCell, newCell, szNew)` 就地覆盖并返回 `SQLITE_OK`**。源码注释明确说这条优化在 autovacuum 库上不能用于「新单元带溢出页」的情形，因为需要 `insertCell` 去写 `PTRMAP_OVERFLOW1` 指针映射项。覆盖前还有两道损坏检查：`oldCell < pPage->aData+pPage->hdrOffset+10` 与 `oldCell+szNew > pPage->aDataEnd` 命中即返回 `SQLITE_CORRUPT_PAGE(pPage)`。
   - 否则调 `dropCell(pPage, idx, info.nSize, &rc)` 把旧单元摘掉。`dropCell` 走 `freeSpace` 把空间还给 freelist，`pPage->nCell` 减一；减到 0 时清空页头并把 `pPage->nFree` 重置为 `pPage->pBt->usableSize - pPage->hdrOffset - pPage->childPtrSize - 8`。
6. **插入新单元。** `loc<0 && pPage->nCell>0` 时 `idx = ++pCur->ix`（插到当前单元之前）并清掉 `BTCF_ValidNKey|BTCF_ValidOvfl`；其余情况 `idx` 保持 `pCur->ix`。然后 `insertCellFast(pPage, idx, newCell, szNew)`。
7. **触发平衡。** `if( pPage->nOverflow )` 非零时清 `BTCF_ValidNKey|BTCF_ValidOvfl`，调 `balance(pCur)`；**无论 `balance` 成功与否都强制 `pCur->pPage->nOverflow = 0` 并把 `pCur->eState = CURSOR_INVALID`**——源码注释说明这是为了避免 `saveCursorPosition` 去保存一个已被 `balance` 破坏的位置。若设了 `BTREE_SAVEPOSITION` 且 `rc==SQLITE_OK`，则 `btreeReleaseAllCursorPages(pCur)`、把键拷贝进 `pCur->pKey`（`sqlite3Malloc(pX->nKey)`，失败则 `rc = SQLITE_NOMEM`），最后 `pCur->eState = CURSOR_REQUIRESEEK`。

### `insertCellFast` 与 `allocateSpace`：页内搬移

`insertCellFast` 的两个分支由 `sz+2>pPage->nFree` 决定：

- **放不下**：登记 overflow 槽位（`pPage->apOvfl[j]=pCell; pPage->aiOvfl[j]=(u16)i;`），`pPage->nOverflow++`。这里 `pCell` 指向的是 `p->pBt->pTmpSpace` 里的临时缓冲，源码靠 `balance_nonroot` 在同一轮里把它搬走来保证生命周期。
- **放得下**：`sqlite3PagerWrite(pPage->pDbPage)` → `allocateSpace(pPage, sz, &idx)` → `memcpy(&data[idx], pCell, sz)` → 在单元索引区 `memmove(pIns+2, pIns, 2*(pPage->nCell - i))` 后 `put2byte(pIns, idx)` → `pPage->nCell++` → 页头单元计数字节 `data[pPage->hdrOffset+4]` 加一，若溢出成 0 则 `data[pPage->hdrOffset+3]` 加一。最后在 autovacuum 库上调 `ptrmapPutOvflPtr(pPage, pPage, pCell, &rc2)` 写溢出页指针映射。

`allocateSpace`（`src/btree.c`）负责在页内找到一段能放下 `nByte` 字节的位置：

- `gap = pPage->cellOffset + 2*pPage->nCell` 是单元索引区末端；`top = get2byte(&data[hdr+5])` 是单元内容区起始偏移。
- 若 `gap>top` 且 `top==0 && pPage->pBt->usableSize==65536`，把 `top` 修正为 **65536**（因为 65536 存不进 2 字节，页头用 0 表示）；否则返回 `SQLITE_CORRUPT_PAGE(pPage)`。若 `top>(int)pPage->pBt->usableSize` 同样报损坏。
- 若页内 freelist 非空（`data[hdr+2] || data[hdr+1]`）且 `gap+2<=top`，调 `pageFindSlot(pPage, nByte, &rc)` 找空槽。
- 若 `gap+2+nByte>top`，调 `defragmentPage(pPage, MIN(4, pPage->nFree - (2+nByte)))` 做碎片整理，然后重读 `top`。

### `balance`：什么时候需要平衡

`balance(BtCursor *pCur)`（`src/btree.c`）是一个 do-while 循环，逐层向上处理。判定条件写在循环体里：

- `pPage->nOverflow==0 && pPage->nFree*3 <= (int)pCur->pBt->usableSize*2` —— 即**没有溢出单元，且剩余空间不超过可用空间的 2/3** 时 break，不需平衡。源码注释把这两个条件写成 "(1) There are no overflow cells (2) The amount of free space on the page is less than 2/3rds of the total usable space"。
- 若是根页（`iPage==0`）且有 overflow，先用 `anotherValidCursor(pCur)` 检查是否有别的游标也指向本页（有则说明库损坏，返回 `SQLITE_CORRUPT_PAGE`），然后调 `balance_deeper(pPage, &pCur->apPage[1])` 把根页内容挪到新子页，根页变成只有右孩子的内部页。
- 否则若 `sqlite3PagerPageRefcount(pPage->pDbPage)>1`，返回 `SQLITE_CORRUPT_PAGE(pPage)`（页是自己的祖先，库损坏）。
- 其余情况：取父页 `pCur->apPage[iPage-1]` 与槽位 `pCur->aiIdx[iPage-1]`，`sqlite3PagerWrite(pParent->pDbPage)`，然后在 `SQLITE_OMIT_QUICKBALANCE` 未定义且满足 `pPage->intKeyLeaf && pPage->nOverflow==1 && pPage->aiOvfl[0]==pPage->nCell && pParent->pgno!=1 && pParent->nCell==iIdx` 时走 `balance_quick(pParent, pPage, aBalanceQuickSpace)`；否则走 `balance_nonroot(pParent, iIdx, pSpace, iPage==1, pCur->hints&BTREE_BULKLOAD)`，其中 `pSpace = sqlite3PageMalloc(pCur->pBt->pageSize)`，**每次 `balance_nonroot` 分配一页大小的缓冲，上一轮的缓冲在下一轮调用后释放**。

### `balance_nonroot`：兄弟页之间的重新分配

`balance_nonroot(MemPage *pParent, int iParentIdx, u8 *aOvflSpace, int isRoot, int bBulk)`（`src/btree.c`）。**`aOvflSpace` 为 0 直接返回 `SQLITE_NOMEM_BKPT`**——这个参数就是 `balance` 里那个 `sqlite3PageMalloc(pageSize)` 缓冲，用来在 secure-delete 模式下临时保存分隔单元。

核心常量：`src/btree.c` 里 `#define NB 3`，注释是 "(NN*2+1): Total pages involved in the balance"——即平衡最多涉及 **3** 个旧页（自己和两个兄弟），加上最多 **2** 个分隔单元。

流程：

1. **定兄弟范围与分隔单元位置。** `i = pParent->nOverflow + pParent->nCell`；`i<2` 时 `nxDiv=0`，否则按 `iParentIdx` 是首、末还是中间分别取 `nxDiv = 0`、`i-2+bulk`、`iParentIdx-1`。然后从父页右孩子指针或对应分隔单元出发，沿 `pgno = get4byte(pRight)` 一路向左收集兄弟页 `apOld[]`，每收一个就把父页里对应的分隔单元 `dropCell` 掉（源码注释解释这样父页里就不会残留 overflow 单元）。
   - **secure-delete 例外**：若 `pBt->btsFlags & BTS_FAST_SECURE`，`dropCell` 会把整个单元清零，因此源码先把分隔单元 `memcpy` 到 `aOvflSpace[]` 里再 drop。
2. **准备内存。** `nMaxCells` 累加各兄弟页的单元数后按 4 取整（`(nMaxCells + 3)&~3`，为了保 8 字节对齐）；`szScratch = nMaxCells*sizeof(u8*) + nMaxCells*sizeof(u16) + pBt->pageSize`，断言 `szScratch<=7*(int)pBt->pageSize`。用 `sqlite3StackAllocRaw` 分配，失败返回 `SQLITE_NOMEM_BKPT`。
3. **收集所有单元指针。** 逐个兄弟页把单元指针读进 `b.apCell[]`，溢出单元按 `pOld->aiOvfl[0]` 插在正确位置。源码断言「多个 overflow 单元必然连续且有序」（`assert( k==0 || pOld->aiOvfl[k-1]+1==pOld->aiOvfl[k] )`，注释标为 "NOTE 1"）。**顺序不能变**：一旦开始平衡，页的单元索引区会被覆写，必须先把每个单元的指针存下来。兄弟页类型不一致（`pOld->aData[0]!=apOld[0]->aData[0]`）直接返回 `SQLITE_CORRUPT_PAGE(pOld)`。
4. **分隔单元入列。** 非叶数据树（`leafData` 为假，即内部页）时把父页的分隔单元也加进 `b.apCell[]`。叶页分隔单元要剥掉 **4** 字节的左孩子指针（`leafCorrection = b.pRef->leaf*4`）；若剥完不足 4 字节，用 `0x00` 补齐到 4。
5. **计算每个新页的容量。** `usableSpace = pBt->usableSize - 12 + leafCorrection`（**12** 是页头加右孩子指针的固定开销，**4** 是叶页分隔单元省下的左指针）。用两个循环把单元往右页挪（`szNew[i] > usableSpace` 时把最后一个单元推到下一页）和往左页补（`cntNew[i] < b.nCell` 且装得下时把下一个单元拉到本页）。新页总数 `k` 超过 `NB+2`（即 **5**）返回 `SQLITE_CORRUPT_BKPT`。
6. **左倾调整为均衡分布。** 源码注释指出前一步的装箱"偏向左边（键较小的兄弟页），左边兄弟几乎总是满的，而最右的兄弟可能几乎是空的"，并强调**这一步不是可选的优化**：最右兄弟完全空掉是非法的。这一段从右往左，把左页的单元一个个挪给右页，直到 `szRight+szD+2 > szLeft-(szR+(i==k-1?0:2))` 为止。
7. **分配新页。** `i<nOld` 的复用旧页（`sqlite3PagerWrite(pNew->pDbPage)` 置脏，并检查 `sqlite3PagerPageRefcount` 是否为 `1+(i==(iParentIdx-nxDiv))`，不是则 `SQLITE_CORRUPT_BKPT`）；`i>=nOld` 的调 `allocateBtreePage(pBt, &pNew, &pgno, (bBulk ? 1 : pgno), 0)` 新分配，`zeroPage(pNew, pageFlags)` 初始化，autovacuum 库上调 `ptrmapPut(pBt, pNew->pgno, PTRMAP_BTREE, pParent->pgno, &rc)`。
8. **页号重排。** 用 O(N*N) 排序把新页页号排成升序（源码注释说 N 不超过 `NB+2` 即 **5**，"不是性能问题"），借助 `sqlite3PagerRekey` 三次交换实现。源码注释给出的收益是：「当 NB==3 时，这个单独优化让大插入和大删除场景下的数据库快约 **25%**」。排序用的临时页号是 `(PENDING_BYTE/pBt->pageSize)+1`。
9. **重建各页内容。** 源码注释指出关键约束：**如果兄弟页是叶页，分隔单元的孩子指针必须从单元里剥掉**，这样 `b.apCell[]` 里的单元形态一致，重建时才能统一处理。

### `balance_deeper` 与 `balance_quick`

`balance_deeper(MemPage *pRoot, MemPage **ppChild)`：根页过载时，给根页新建一个右孩子。`sqlite3PagerWrite(pRoot->pDbPage)` → `allocateBtreePage(pBt,&pChild,&pgnoChild,pRoot->pgno,0)` → `copyNodeContent(pRoot, pChild, &rc)` → 把根页的 overflow 单元数组整个 `memcpy` 到子页 → `zeroPage(pRoot, pChild->aData[0] & ~PTF_LEAF)` 把根页清成内部页 → `put4byte(&pRoot->aData[pRoot->hdrOffset+8], pgnoChild)` 写右孩子。

`balance_quick(MemPage *pParent, MemPage *pPage, u8 *pSpace)`：只处理一种极窄的场景——`intKey` 叶页恰好有 1 个 overflow 单元、该单元在单元列表末尾、父页不是页 1 且父页单元数等于目标槽位。此时新建一个右兄弟页，用 `rebuildPage(&b, 0, 1, pNew)` 把 overflow 单元搬过去，再往父页插一个分隔单元。`pSpace` 参数实际未被使用（源码里 `u8 *pOut = &pSpace[4];` 之后的 `pOut` 也未被赋值使用）。`balance_quick` 里 `if( pPage->nCell==0 ) return SQLITE_CORRUPT_BKPT;` 并带 `dbfuzz001.test` 注释标记。

### `sqlite3BtreeClearTable`：整表清空

`sqlite3BtreeClearTable(Btree *p, int iTable, i64 *pnChange)`（`src/btree.c`）：`sqlite3BtreeEnter(p)` → `assert( p->inTrans==TRANS_WRITE )` → `saveAllCursors(pBt, (Pgno)iTable, 0)`（注意第二个参数是 0，含义是"保存所有游标"）→ 若 `p->hasIncrblobCur` 则 `invalidateIncrblobCursors(p, (Pgno)iTable, 0, 1)` → `clearDatabasePage(pBt, (Pgno)iTable, 0, pnChange)` → `sqlite3BtreeLeave(p)`。

`clearDatabasePage(BtShared *pBt, Pgno pgno, int freePageFlag, i64 *pnChange)` 是递归函数：`freePageFlag` 的**位 0** 表示"回收该页"，**位 1 及以上**表示递归深度；深度超过 `BTCURSOR_MAX_DEPTH` 返回 `SQLITE_CORRUPT_PGNO(pgno)`。它遍历页内单元，非叶页对每个单元的左孩子递归调用 `clearDatabasePage(pBt, get4byte(pCell), (freePageFlag+2)|1, pnChange)`，叶页则用 `BTREE_CLEAR_CELL` 清单元。**内部页时 `pnChange` 被置 0**（源码 `if( pPage->intKey ) pnChange = 0;`），因此只有 rowid 表才统计被删行数。最后 `freePageFlag&1` 为真时 `freePage(pPage, &rc)` 把页还回 freelist，否则 `zeroPage(pPage, pPage->aData[hdr] | PTF_LEAF)` 清空页内容。

### 真实调用方

| 调用方 | 文件 | 调用方式 |
|---|---|---|
| `OP_Insert` | `src/vdbe.c` | `sqlite3BtreeInsert(pC->uc.pCursor, &x, (pOp->p5 & (OPFLAG_APPEND\|OPFLAG_SAVEPOSITION\|OPFLAG_PREFORMAT)), seekResult)`；`seekResult` 取自 `OPFLAG_USESEEKRESULT` |
| `OP_IdxInsert` | `src/vdbe.c` | `sqlite3BtreeInsert(pC->uc.pCursor, &x, (pOp->p5 & (OPFLAG_APPEND\|OPFLAG_SAVEPOSITION\|OPFLAG_PREFORMAT)), ...)`；这里 `x.nKey = pIn2->n; x.pKey = pIn2->z;` |
| `OP_Clear` | `src/vdbe.c` | `sqlite3BtreeClearTable(db->aDb[pOp->p2].pBt, (u32)pOp->p1, &nChange)`；返回的行数累加进 `p->nChange` 与 `aMem[pOp->p3]` |
| `sqlite3BtreeClearTableOfCursor` | `src/btree.c` | `sqlite3BtreeClearTable(pCur->pBtree, pCur->pgnoRoot, 0)`，注释说只适用于临时表 |
| `btreeDropTable` | `src/btree.c` | `sqlite3BtreeClearTable(p, iTable, 0)`，删表路径 |
| `sqlite3BtreeTransferRow` → `sqlite3BtreeInsert` | `src/btree.c` | 先把行预格式化进 `BtShared.pTmpSpace` 并设置 `BtShared.nPreformatSize`，再由调用方以 `BTREE_PREFORMAT` 调 `sqlite3BtreeInsert` |
| `test3.c` 的测试辅助 | `src/test3.c` | `sqlite3BtreeInsert(pCur, &x, 0, 0)`，直接测试用 |

### 关键魔数与常量出处

| 数值 | 含义 | 出处 |
|---|---|---|
| 2 | 单元指针在页头单元索引区占用的字节数 | `allocateSpace` 的 `gap = pPage->cellOffset + 2*pPage->nCell` |
| 4 | 单元最小长度；也是内部页分隔单元左孩子指针长度 | `insertCellFast` 的 `szNew<4` 强制补齐；`leafCorrection = b.pRef->leaf*4` |
| 8 | 页头固定开销（页头 + 右孩子指针） | `usableSpace = pBt->usableSize - 12 + leafCorrection` 中的 12 与 4 之差 |
| 12 | `usableSpace` 计算中的页头开销 | 同上 |
| 3 | 平衡最多涉及的旧页数 | `src/btree.c` 的 `#define NB 3`，注释 "(NN*2+1): Total pages involved in the balance" |
| 5 | 平衡后新页数上限（`NB+2`） | `balance_nonroot` 里 `if( k>NB+2 ){ rc = SQLITE_CORRUPT_BKPT; ... }` |
| 3 | overflow 槽位实际可用数（数组长度为 4，保留 1 个） | `insertCellFast` 的 `assert( j < ArraySize(pPage->apOvfl)-1 )`，注释 "never need more than 3 overflow slots but 4 are allocated, just to be safe" |
| 2/3 | 触发平衡的剩余空间阈值 | `balance` 里 `pPage->nFree*3 <= (int)pCur->pBt->usableSize*2` |
| 65536 | 页大小上限时单元内容区偏移的哨兵值 | `allocateSpace` 里 `if( top==0 && pPage->pBt->usableSize==65536 ) top = 65536;` |
| 0x02 / 0x08 / 0x80 | `BTREE_SAVEPOSITION` / `BTREE_APPEND` / `BTREE_PREFORMAT` | `src/btree.h` |
| 0x00000001 | `BTREE_BULKLOAD`（`balance_nonroot` 的 `bBulk` 参数来源） | `src/btree.h` |
| `usableSize-12` 的 64/255、32/255 | `maxLocal` / `minLocal` 的计算式 | `src/btree.c`：`pBt->maxLocal = (u16)((pBt->usableSize-12)*64/255 - 23); pBt->minLocal = (u16)((pBt->usableSize-12)*32/255 - 23);` |
| `usableSize - 35` | `maxLeaf`（叶页单元最大本地载荷） | `src/btree.c`：`pBt->maxLeaf = (u16)(pBt->usableSize - 35);` |
| 480 | `usableSize` 允许的最小值（页大小 512 时保留空间最多 32） | `src/btree.c`：`if( usableSize<480 ){ goto page1_init_failed; }` |
| 25% | 页号升序重排带来的大插入/大删除性能提升 | `balance_nonroot` 注释："When NB==3, this one optimization makes the database about 25% faster for large insertions and deletions" |

## 关键取舍

- **同尺寸覆盖走快捷路径，不同尺寸就老老实实删了再插。** `sqlite3BtreeInsert` 在 `info.nSize==szNew && info.nLocal==info.nPayload` 时直接 `memcpy` 覆盖。源码注释解释为什么不把「旧单元更小」也优化掉：「实验表明这样做并不比跳过这个优化、直接调 `dropCell()` 和 `insertCell()` 更快。」
- **autovacuum 库上禁用带溢出页的覆盖优化。** 源码注释说得很直接：这个优化在 autovacuum 库上不能用于新单元带溢出页的情形，因为 `insertCell` 之后还要写 `PTRMAP_OVERFLOW1` 指针映射项。判定条件写成 `(!ISAUTOVACUUM(p->pBt) || szNew<pPage->minLocal)`。
- **页放不下时不报错，记 overflow 单元。** `insertCellFast` 把放不下的单元登记进 `apOvfl`/`aiOvfl`，把"页满"这件事推迟到 `balance` 阶段处理。这让 `insertCellFast` 本身没有失败分支（除了 pager 写失败），但代价是页上会短暂存在一种中间态数据结构。
- **平衡时先收集全部单元指针，再动页。** `balance_nonroot` 的注释说明了原因：「一旦平衡开始，b-tree 页的单元偏移区会被覆写，届时我们再也无法找到那些单元——除非每个单元的指针都已提前保存。」
- **左倾装箱必须再调整一次。** 源码注释强调这一步「不只是优化」：前一步算出的装箱可能失衡到非法的程度，最右兄弟页可能完全为空。所以有一段专门从右往左回补单元的代码。
- **最多参与 3 个兄弟页。** `NB` 定义为 3，注释说这是 "NN*2+1"。换来的是重排页号的 O(N*N) 排序（源码注释说 N 不超过 5，不是性能问题）与 25% 的性能收益。
- **secure-delete 模式下要额外拷贝分隔单元。** 普通模式下 `dropCell` 只覆写单元前 4 字节，`apDiv[i]` 指针仍可安全使用；secure-delete 模式下整个单元被清零，所以要先 `memcpy` 到 `aOvflSpace[]`。源码注释把这两种情形的差别写得很清楚。
- **`balance` 用 do-while 逐层向上，且每轮换一块缓冲。** 每轮 `balance_nonroot` 新分配一页大小的 `pSpace`，上一轮的缓冲在下一轮调用完成后释放——因为上一轮的 overflow 单元内容已经被复制进真实页或新的 `pSpace`。
- **`balance` 失败后仍强制清 `nOverflow`。** 源码注释说明：「必须确保 `nOverflow` 被重置为 0，即使 `balance()` 失败。否则内部数据结构会损坏。」同时把游标状态置为 `CURSOR_INVALID`，阻止 `saveCursorPosition` 去保存一个已被 `balance` 破坏的位置。
- **`sqlite3BtreeClearTable` 只统计 rowid 表的被删行数。** `clearDatabasePage` 里 `if( pPage->intKey ) pnChange = 0;` 把计数清零——非 rowid 表没有"行"的概念，`OP_Clear` 拿到的 `nChange` 因此只对 rowid 表有意义。
- **清表时先保存所有游标位置并作废 incrblob 游标。** `sqlite3BtreeClearTable` 调 `saveAllCursors(pBt, (Pgno)iTable, 0)`（`iRoot` 传 0 表示保存全部）与 `invalidateIncrblobCursors(p, (Pgno)iTable, 0, 1)`。

## 与主核 / AI 的关系

本机制是 SQLite「把一次逻辑写操作变成物理页布局」的全部转换过程，是 B-tree 存储层区别于纯内存结构的核心。

**架构推论：** `balance_nonroot` 里"先收集全部单元指针，再覆写页"的顺序（`b.apCell[]` 缓存），对主数据库内核 `engine/db/storage/` 层的 B-tree 实现是一个可直接照搬的模板——它把「页内布局」与「页内容搬移」解耦，使得搬移算法可以在页头被破坏的情况下继续工作。对 AI 辅助读码而言，这意味着 `balance_nonroot` 的阅读顺序必须从 `b.apCell[]` 的填充块开始，跳过它去读后面的装箱逻辑会完全看不懂。

**架构推论：** `balance` 的"页空闲空间超过 2/3 才触发平衡"这一阈值（`pPage->nFree*3 <= usableSize*2`）是一个写放大 / 读放大 / 空间浪费三者之间的显式折中点。**架构推论：** 主核若要实现类似的多页平衡，需要自行定义这个阈值并把它的取值写进设计文档——SQLite 是把它硬编码在 `balance` 的条件表达式里的，没有暴露成配置项。

**架构推论：** 「同尺寸覆盖」与「页满记 overflow 再平衡」这两级优化，等价于给写入路径做了一条快速通道。AI 辅助定位问题时容易只看 `insertCellFast` 而漏掉 `balance` 这一层，从而低估写放大——本机制中一次 `INSERT` 可能触发多轮 `balance`，每轮都可能分配新页并改写父页。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `sqlite3BtreeInsert` | 插入主流程：`saveAllCursors` → `moveToRoot` → 表/索引两条定位路径 → `btreeOverwriteCell` 快捷覆盖 → `fillInCell` 造单元 → `dropCell` + `insertCellFast` → `balance` → `BTREE_SAVEPOSITION` 时重建游标键并置 `CURSOR_REQUIRESEEK` |
| src/btree.c | `balance_nonroot` | 兄弟页重新分配：`NB` 个兄弟页收集单元指针 → 计算 `szNew`/`cntNew` → 左倾回补 → 复用旧页或 `allocateBtreePage` 新建 → 页号升序重排 → 重建各页 |
| src/btree.c | `sqlite3BtreeClearTable` | 整表清空：`saveAllCursors` + `invalidateIncrblobCursors` + `clearDatabasePage` |
| src/btree.c | `balance` | 平衡分派与触发条件：无 overflow 且剩余空间 ≤ 2/3 可用空间则 break；根页走 `balance_deeper`；父页非页 1 且子页是 `intKey` 叶页带 1 个尾部 overflow 时走 `balance_quick`；否则 `balance_nonroot`，每轮换一块 `sqlite3PageMalloc(pageSize)` 缓冲 |
| src/btree.c | `balance_deeper` | 根页过载时新建右孩子：`allocateBtreePage` + `copyNodeContent` + 搬运 overflow 数组 + `zeroPage` 清根页 + 写右孩子页号 |
| src/btree.c | `balance_quick` | 窄场景快速平衡：新建右兄弟页、`rebuildPage` 搬走 overflow 单元、往父页插分隔单元 |
| src/btree.c | `insertCellFast` | `insertCell` 的裁剪版（`pTemp`/`iChild` 均为 0），只被 `sqlite3BtreeInsert` 调用故可内联；放不下时登记 `apOvfl`/`aiOvfl` 溢出槽位，放得下时 `allocateSpace` + `memcpy` + 单元索引区 `memmove` |
| src/btree.c | `insertCell` | 通用版插入单元，额外支持 `pTemp` 暂存与 `iChild` 左孩子指针写入；`balance_nonroot` 往父页插分隔单元时用它 |
| src/btree.c | `allocateSpace` | 页内找空间：`gap = pPage->cellOffset + 2*pPage->nCell`；`top` 为 0 且 `usableSize==65536` 时修正为 65536；必要时 `pageFindSlot` 找空槽或 `defragmentPage` 整理碎片 |
| src/btree.c | `fillInCell` | 构造单元字节序列：写单元头（`intKey` 页写 payload 长度 + rowid），载荷 ≤ `maxLocal` 时就地拷贝，否则算溢出分配并留 4 字节溢出页号 |
| src/btree.c | `dropCell` | 摘除单元：`freeSpace` 还空间给 freelist，`nCell` 减一；减到 0 时清页头并按 `usableSize - hdrOffset - childPtrSize - 8` 重置 `nFree` |
| src/btree.c | `btreeOverwriteCell` | 同键覆盖快捷路径：全本地时 `btreeOverwriteContent`，带溢出时 `btreeOverwriteOverflowCell`；越界返回 `SQLITE_CORRUPT_PAGE` |
| src/btree.c | `allocateBtreePage` | 从 freelist 取页或追加新页；`eMode` 取 `BTALLOC_ANY`/`BTALLOC_EXACT`/`BTALLOC_LE`；autovacuum 下指针映射页需连取两页 |
| src/btree.c | `clearDatabasePage` | 递归清页：`freePageFlag` 位 0 表示回收页、位 1 起为深度，超过 `BTCURSOR_MAX_DEPTH` 报 `SQLITE_CORRUPT_PGNO`；内部页把 `pnChange` 置 0 |
| src/btree.c | `saveAllCursors` | 保存同 root 页其他游标的位置；找不到其他游标时清掉 `pExcept` 的 `BTCF_Multiple` 标志 |
| src/btree.c | `invalidateIncrblobCursors` | 作废指定行上的 incrblob 游标；`sqlite3BtreeInsert` 覆盖行与 `sqlite3BtreeClearTable` 清表时调用 |
| src/btree.c | `sqlite3BtreeTransferRow` | 预格式化路径：把源行写进 `BtShared.pTmpSpace` 并设 `BtShared.nPreformatSize`，供调用方以 `BTREE_PREFORMAT` 调 `sqlite3BtreeInsert` |
| src/btree.c | `NB` | 宏，取值 3，注释 "(NN*2+1): Total pages involved in the balance" |
| src/btree.c | `BTALLOC_ANY` / `BTALLOC_EXACT` / `BTALLOC_LE` | `allocateBtreePage` 的三种取页模式，取值 0 / 1 / 2 |
| src/btree.c | `pBt->maxLocal` / `pBt->minLocal` / `pBt->maxLeaf` | 由 `usableSize` 推出：`(usableSize-12)*64/255 - 23`、`(usableSize-12)*32/255 - 23`、`usableSize - 35` |
| src/btree.c | `sqlite3BtreeClearTableOfCursor` | 游标版清表，`sqlite3BtreeClearTable(pCur->pBtree, pCur->pgnoRoot, 0)`，仅适用于临时表 |
| src/btree.c | `btreeDropTable` | 删表路径，先调 `sqlite3BtreeClearTable(p, iTable, 0)` 再把根页还回 freelist |
| src/btree.h | `BTREE_SAVEPOSITION` / `BTREE_APPEND` / `BTREE_PREFORMAT` | `sqlite3BtreeInsert` 的 flags 位，取值 0x02 / 0x08 / 0x80 |
| src/btree.h | `BTREE_BULKLOAD` | 批量装载提示，取值 0x00000001，经 `pCur->hints` 传到 `balance_nonroot` 的 `bBulk` 参数 |
| src/btreeInt.h | `MX_CELL_SIZE(pBt)` | 宏 `(int)(pBt->pageSize-8)`，单元长度上限；`sqlite3BtreeInsert` 断言 `szNew <= MX_CELL_SIZE(p->pBt)` |
| src/btreeInt.h | `MX_CELL(pBt)` | 宏 `((pBt->pageSize-8)/6)`，单页单元数上限；`insertCellFast` 断言 `MX_CELL(pPage->pBt)<=10921` |
| src/vdbe.c | `OP_Insert` | rowid 表插入指令，调用 `sqlite3BtreeInsert`；`seekResult` 由 `OPFLAG_USESEEKRESULT` 决定是否复用上一次定位结果 |
| src/vdbe.c | `OP_IdxInsert` | 索引插入指令，调用 `sqlite3BtreeInsert`；`x.nKey = pIn2->n; x.pKey = pIn2->z;` |
| src/vdbe.c | `OP_Clear` | 清表指令，调用 `sqlite3BtreeClearTable`；返回的被删行数累加进 `p->nChange` 与 `aMem[pOp->p3]` |

## 参考

- [SQLite B-tree 实现（sqlite.org 官方文档）](https://sqlite.org/btreetree.html)
- [SQLite 文件格式：B-tree 页（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html)
- [Architecture of SQLite（SQLite 官方架构文档）](https://sqlite.org/arch.html)
- [SQLite 源码浏览：src/btree.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/btree.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
