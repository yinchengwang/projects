---
project: sqlite
feature: btree-search
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeIndexMoveto, sqlite3BtreeTableMoveto, btreeMoveto]
---

# B-tree 键定位

## 一句话职责

`sqlite3BtreeIndexMoveto` 与 `sqlite3BtreeTableMoveto` 负责在 B-tree 里按比较结果定位到目标键所在的单元位置：两者都是自根页向下的逐页二分查找（每页内用二分缩小 `lwr`/`upr` 区间），查不到时把游标停在「若该键存在就应落在的那一页」上，并通过 `*pRes` 回传游标当前所指键与目标键的大小关系（小于 0 / 等于 0 / 大于 0）；`btreeMoveto` 是二者的统一入口，按 `pKey` 是否为 NULL 决定走索引路径还是表路径。

## 机制是什么

B-tree 的键定位被拆成三层，边界由「键的形态」划分：

**第一层：统一入口 `btreeMoveto`。** `btreeMoveto`（`src/btree.c`）不自己做查找，只做分派：
- `pKey != NULL`（索引 B-tree，键是打包后的索引记录）：调 `sqlite3VdbeAllocUnpackedRecord(pKeyInfo)` 分配 `UnpackedRecord`，失败返回 `SQLITE_NOMEM_BKPT`；再调 `sqlite3VdbeRecordUnpack((int)nKey, pKey, pIdxKey)` 解包；随后检查 `pIdxKey->nField==0 || pIdxKey->nField>pKeyInfo->nAllField`，命中即 `rc = SQLITE_CORRUPT_BKPT`（记录字段数与 KeyInfo 不符即视为库损坏），否则调 `sqlite3BtreeIndexMoveto(pCur, pIdxKey, pRes)`。最后统一 `sqlite3DbFree(pCur->pKeyInfo->db, pIdxKey)` 释放。
- `pKey == NULL`（rowid 表，键是整数）：直接调 `sqlite3BtreeTableMoveto(pCur, nKey, bias, pRes)`。

**第二层：两条查找路径。** 表路径与索引路径的差别不在于树形结构（B-tree 的页结构是一样的），而在于**键的比较方式**：
- 表路径（`sqlite3BtreeTableMoveto`）：键是 rowid，直接用整数比较。页内每个单元的键是从单元体里读出的 varint，`sqlite3VarintValue(pCell)` 解码后与 `intKey` 比大小。
- 索引路径（`sqlite3BtreeIndexMoveto`）：键是 `UnpackedRecord`，比较函数是运行时选出来的 `RecordCompare`（由 `sqlite3VdbeFindCompare(pIdxKey)` 按键的首字段类型与 `KeyInfo.nAllField` 决定），页内单元与 `pIdxKey` 逐字段比较。

**第三层：结果语义。** 两条路径都通过 `*pRes` 回传三态：`*pRes<0` 表示游标停在一个**小于**目标键的单元上（或表为空时游标不指向任何东西）；`*pRes==0` 表示游标停在**精确匹配**的单元上；`*pRes>0` 表示游标停在一个**大于**目标键的单元上。查不到精确匹配时，游标**总是**停在「如果该键存在就应落在的那一页」上，源码注释明确写道：「the cursor is always left pointing at a leaf page which would hold the entry if it were present」。

**本机制的边界：** 不负责游标的移动（`sqlite3BtreeNext` / `sqlite3BtreePrevious`，见 f05）、不负责比较函数本身的语义（`sqlite3VdbeFindCompare` / `sqlite3VdbeRecordCompare`，见 C03）、不负责页缓存的引用计数（I 组）。

## 怎么实现的

### `sqlite3BtreeTableMoveto`：rowid 表的整数键查找

签名（`src/btree.c`）：`int sqlite3BtreeTableMoveto(BtCursor *pCur, i64 intKey, int biasRight, int *pRes)`。前置断言包括 `assert(pCur->pKeyInfo==0)`（表 B-tree 没有 KeyInfo）、`assert(pCur->eState!=CURSOR_VALID || pCur->curIntKey!=0)`。

执行顺序：

1. **命中缓存则直接返回。** 若 `pCur->eState==CURSOR_VALID && (pCur->curFlags & BTCF_ValidNKey)!=0`：
   - `pCur->info.nKey==intKey` → `*pRes = 0`，直接 `return SQLITE_OK`。
   - `pCur->info.nKey<intKey` 且 `pCur->curFlags & BTCF_AtLast` → `*pRes = -1`，直接 `return SQLITE_OK`（已到最后一条，比目标键小）。
   - `pCur->info.nKey+1==intKey` → **改用 `sqlite3BtreeNext(pCur, 0)` 走一步**，再 `getCellInfo(pCur)` 复查 `pCur->info.nKey==intKey` 则返回 `SQLITE_OK`；若 `sqlite3BtreeNext` 返回 `SQLITE_DONE` 则继续往下走（不返回错误），返回其它错误码则原样返回。源码注释明确写这是「optimization only」，去掉它结果依然正确，只是慢一点。
2. **回到根页。** 调 `moveToRoot(pCur)`。返回 `SQLITE_EMPTY` 时 `*pRes = -1` 并 `return SQLITE_OK`（空表，视作「小于目标键」）；其它错误码原样返回。
3. **逐层下降。** 外层 `for(;;)` 处理每一页，内层 `for(;;)` 做页内二分：
   - 初始 `lwr = 0; upr = pPage->nCell-1;`。起始下标 `idx = upr>>(1-biasRight)`，源码注释说明这等价于 `idx = biasRight ? upr : (lwr+upr)/2`——即 `biasRight` 为真时从最右端起查，为假时取中点。
   - 页内取单元：`pCell = findCellPastPtr(pPage, idx)`。若 `pPage->intKeyLeaf`（rowid 表的叶页），需要跳过单元头部的载荷长度 varint：`while( 0x80 <= *(pCell++) ){ if( pCell>=pPage->aDataEnd ){ return SQLITE_CORRUPT_PAGE(pPage); } }`——越界即报页损坏。
   - 解出键：`nCellKey = sqlite3VarintValue(pCell)`。
   - 三分支：`nCellKey<intKey` → `lwr = idx+1`，若 `lwr>upr` 则 `c = -1` 并跳出内层循环；`nCellKey>intKey` → `upr = idx-1`，若 `lwr>upr` 则 `c = +1` 并跳出；相等 → 命中：
     - 若 `!pPage->leaf`（内部页），记 `lwr = idx` 后 `goto moveto_table_next_layer` 下降到该分隔单元指向的子页；
     - 若 `pPage->leaf`（叶页），设 `pCur->ix = (u16)idx`，置 `BTCF_ValidNKey`、写 `pCur->info.nKey = nCellKey`、`pCur->info.nSize = 0`，`*pRes = 0`，`return SQLITE_OK`。
   - 未命中时中点推进：`idx = (lwr+upr)>>1`。
4. **叶页收尾。** 内层循环因 `lwr>upr` 退出后，若 `pPage->leaf` 则设 `pCur->ix = (u16)idx`、`*pRes = c`（即前面的 **-1** 或 **+1**）、`rc = SQLITE_OK`，跳到 `moveto_table_finish`。
5. **内部页选子页。** `moveto_table_next_layer` 处：若 `lwr>=pPage->nCell`（目标键比本页所有分隔键都大），则取**最右孩子** `chldPg = get4byte(&pPage->aData[pPage->hdrOffset+8])`；否则取第 `lwr` 个单元里的左孩子指针 `chldPg = get4byte(findCell(pPage, lwr))`。设 `pCur->ix = (u16)lwr`，调 `moveToChild(pCur, chldPg)`，失败则跳出外层循环。
6. **统一收尾。** `moveto_table_finish` 处设 `pCur->info.nSize = 0`，并断言 `(pCur->curFlags & BTCF_ValidOvfl)==0`。

表路径不调用任何外部比较函数——整数键的大小比较是内联的。

### `sqlite3BtreeIndexMoveto`：索引 B-tree 的记录键查找

签名（`src/btree.c`）：`int sqlite3BtreeIndexMoveto(BtCursor *pCur, UnpackedRecord *pIdxKey, int *pRes)`。前置断言 `assert(pCur->pKeyInfo!=0)`。

执行顺序：

1. **选比较函数。** `xRecordCompare = sqlite3VdbeFindCompare(pIdxKey)`，并把 `pIdxKey->errCode = 0` 清零。源码断言 `pIdxKey->default_rc` 只能是 1 / 0 / -1 三者之一。
2. **两个「跳过大量工作」的快捷路径。** 前提是 `pCur->eState==CURSOR_VALID && pCur->pPage->leaf && cursorOnLastPage(pCur)`（游标已停在最后一页）：
   - 若 `pCur->ix==pCur->pPage->nCell-1` 且 `indexCellCompare(pCur->pPage, pCur->ix, pIdxKey, xRecordCompare)<=0` 且 `pIdxKey->errCode==SQLITE_OK` → `*pRes = c`，直接 `return SQLITE_OK`（游标已经在正确位置，无需移动）。
   - 若 `pCur->iPage>0` 且 `indexCellCompare(pCur->pPage, 0, pIdxKey, xRecordCompare)<=0` 且 `pIdxKey->errCode==SQLITE_OK` → 清掉 `BTCF_ValidOvfl|BTCF_AtLast`；若 `!pCur->pPage->isInit` 返回 `SQLITE_CORRUPT_BKPT`；否则 `goto bypass_moveto_root`——**直接从当前页开始查找，不回根页**。
3. **回到根页。** 调 `moveToRoot(pCur)`；`SQLITE_EMPTY` 时 `*pRes = -1` 并 `return SQLITE_OK`；其它错误码原样返回。
4. **逐层下降。** 外层 `for(;;)` 与内层 `for(;;)` 的骨架与表路径一致（`lwr=0`、`upr=pPage->nCell-1`、`idx = upr>>1`），差别在**比较单元的方式**，分三种情形：
   - **单字节载荷长度 varint，载荷完整落在页内。** 条件是 `nCell<=pPage->max1bytePayload`（`max1bytePayload` 的定义见 `src/btreeInt.h`：`min(maxLocal,127)`）。边界检查 `if( pCell + nCell >= pPage->aDataEnd ){ rc = SQLITE_CORRUPT_PAGE(pPage); goto moveto_index_finish; }`，随后 `c = xRecordCompare(nCell, (void*)&pCell[1], pIdxKey)`。
   - **两字节载荷长度 varint，载荷完整落在页内。** 条件是 `!(pCell[1] & 0x80)`（第二字节最高位为 0，说明长度 varint 未超过 2 字节）且 `nCell = ((nCell&0x7f)<<7) + pCell[1] <= pPage->maxLocal` 且 `pCell + nCell < pPage->aDataEnd`，随后 `c = xRecordCompare(nCell, (void*)&pCell[2], pIdxKey)`。
   - **载荷溢出到溢出页。** 走 `pPage->xParseCell(pPage, pCellBody, &pCur->info)` 解析整个单元，取 `nCell = (int)pCur->info.nKey`。合法性检查 `if( nCell<2 || nCell/pCur->pBt->usableSize>pCur->pBt->nPage )` 命中即 `rc = SQLITE_CORRUPT_PAGE(pPage)`（**2** 是最小合法索引键长度）。随后 `sqlite3Malloc((u64)nCell+(u64)nOverrun)` 分配缓冲，`nOverrun` 取 **18**（源码注释说明这是给可能读过界的 varint 准备的填充字节），失败则 `rc = SQLITE_NOMEM_BKPT`；再 `accessPayload(pCur, 0, nCell, (unsigned char*)pCellKey, 0)` 把记录读进缓冲，成功后 `memset(((u8*)pCellKey)+nCell,0,nOverrun)` 补零，最后 `c = sqlite3VdbeRecordCompare(nCell, pCellKey, pIdxKey)`，用完 `sqlite3_free(pCellKey)`。
     源码注释解释了为什么只做前两种「不解析完整单元」的优化：页大小上限是 **65536** 字节，因此索引 B-tree 页内能存的记录字节数小于 **16384**，可以用 2 字节 varint 表示长度，于是只查单元头 2 字节就能判断记录是否完整落在页内。
   - 比较结果三分支与表路径同构：`c<0` → `lwr = idx+1`；`c>0` → `upr = idx-1`；`c==0` → `*pRes = 0`、`pCur->ix = (u16)idx`，若 `pIdxKey->errCode` 非零则 `rc = SQLITE_CORRUPT_BKPT`，跳到 `moveto_index_finish`。`lwr>upr` 则跳出内层循环。
   - 叶页收尾：`pCur->ix = (u16)idx`、`*pRes = c`、`rc = SQLITE_OK`，跳到 `moveto_index_finish`。非叶页则按 `lwr>=pPage->nCell` 决定取最右孩子还是第 `lwr` 个单元的左孩子指针。
   - **下降子页用的是内联展开的 `moveToChild`。** 源码注释写明「This block is similar to an in-lined version of: pCur->ix = (u16)lwr; rc = moveToChild(pCur, chldPg); if( rc ) break;」。展开的部分做三件额外的事：清 `pCur->info.nSize = 0`、清 `BTCF_ValidNKey|BTCF_ValidOvfl`、做**深度检查** `if( pCur->iPage>=(BTCURSOR_MAX_DEPTH-1) ){ return SQLITE_CORRUPT_BKPT; }`（`BTCURSOR_MAX_DEPTH` 定义在 `src/btreeInt.h`，取值 **20**）。随后 `getAndInitPage(pCur->pBt, chldPg, &pCur->pPage, pCur->curPagerFlags)` 取页；若取页成功但 `pCur->pPage->nCell<1 || pCur->pPage->intKey!=pCur->curIntKey`（子页单元数为 0，或子页类型与父页不匹配），则 `releasePage(pCur->pPage)` 并 `rc = SQLITE_CORRUPT_PGNO(chldPg)`；失败时回退 `pCur->pPage = pCur->apPage[--pCur->iPage]`。
5. **统一收尾。** `moveto_index_finish` 处设 `pCur->info.nSize = 0`，断言 `(pCur->curFlags & BTCF_ValidOvfl)==0`。

### `indexCellCompare`：快捷路径用的比较

`indexCellCompare(MemPage *pPage, int idx, UnpackedRecord *pIdxKey, RecordCompare xRecordCompare)`（`src/btree.c`）只在两种情形下真正比较：单字节长度 varint 且 `nCell<=pPage->max1bytePayload`；两字节长度 varint 且 `nCell<=pPage->maxLocal`。两者都要先做 `if( pCell + nCell >= pPage->aDataEnd ) return 99;` 的越界检查。返回 **99** 表示「关系未知」——源码注释说明这是一个优化，返回正值是安全的，因为这会导致该优化被跳过。

### `btreeMoveto` 的两个真实调用方

- `btreeRestoreCursorPosition`（`src/btree.c`）：`rc = btreeMoveto(pCur, pCur->pKey, pCur->nKey, 0, &skipNext)`。这是**游标位置保存/恢复**路径——`saveCursorPosition` 把键拷进 `pCur->pKey` / `pCur->nKey` 并把状态置为 `CURSOR_REQUIRESEEK`，此后任何一次需要重定位的操作都会通过 `restoreCursorPosition` 宏（`p->eState>=CURSOR_REQUIRESEEK ? btreeRestoreCursorPosition(p) : SQLITE_OK`）走这条路。恢复成功后按 `skipNext` 把游标状态改成 `CURSOR_SKIPNEXT`。
- `sqlite3BtreeInsert`（`src/btree.c`）：索引 / WITHOUT ROWID 表的插入路径，`rc = btreeMoveto(pCur, pX->pKey, pX->nKey, (flags & BTREE_APPEND)!=0, &loc)`。注意 `sqlite3BtreeInsert` 里 `pX->nMem` 非零时**不走 `btreeMoveto`**，而是直接构造 `UnpackedRecord r` 调 `sqlite3BtreeIndexMoveto(pCur, &r, &loc)`，以避免重复解码键。

### `sqlite3BtreeTableMoveto` / `sqlite3BtreeIndexMoveto` 的其他真实调用方

| 调用方 | 文件 | 调用方式 |
|---|---|---|
| `OP_SeekGE` / `OP_SeekGT` / `OP_SeekLT` / `OP_SeekLE`（rowid 表分支） | `src/vdbe.c` | `rc = sqlite3BtreeTableMoveto(pC->uc.pCursor, (u64)iKey, 0, &res)`；随后 `pC->movetoTarget = iKey`（注释 "Used by OP_Delete"） |
| `OP_SeekGE` / `OP_SeekGT` / `OP_SeekLT` / `OP_SeekLE`（索引分支） | `src/vdbe.c` | 先设 `r.default_rc = ((1 & (oc - OP_SeekLT)) ? -1 : +1)`，再 `rc = sqlite3BtreeIndexMoveto(pC->uc.pCursor, &r, &res)` |
| `OP_NotExists` / `OP_SeekRowid` | `src/vdbe.c` | `rc = sqlite3BtreeTableMoveto(pCrsr, iKey, 0, &res)`；`res!=0` 时若 `pOp->p2==0` 则 `rc = SQLITE_CORRUPT_BKPT`，否则跳转到 `p2` |
| `OP_Found` / `OP_NotFound` / `OP_NoConflict` / `OP_IfNoHope` | `src/vdbe.c` | 键在寄存器数组中时 `rc = sqlite3BtreeIndexMoveto(pC->uc.pCursor, &r, &pC->seekResult)`；键是 `OP_MakeRecord` 产物时先 `sqlite3VdbeAllocUnpackedRecord` + `sqlite3VdbeRecordUnpack` 再调 `sqlite3BtreeIndexMoveto` |
| `OP_Delete`（`OP_IdxDelete`） | `src/vdbe.c` | `rc = sqlite3BtreeIndexMoveto(pCrsr, &r, &res)`；`res!=0` 时改走 `sqlite3VdbeFindIndexKey`，仍 `res!=0` 且 `!sqlite3WritableSchema(db)` 则报 `SQLITE_CORRUPT_INDEX` |
| `sqlite3VdbeFinishMoveto` | `src/vdbeaux.c` | 延迟定位的落地执行：`rc = sqlite3BtreeTableMoveto(p->uc.pCursor, p->movetoTarget, 0, &res)`；`res!=0` 直接 `return SQLITE_CORRUPT_BKPT` |
| `sqlite3BtreeInsert`（表 B-tree 分支） | `src/btree.c` | `rc = sqlite3BtreeTableMoveto(pCur, pX->nKey, (flags & BTREE_APPEND)!=0, &loc)` |
| `sqlite3BtreeInsert`（索引 / WITHOUT ROWID 分支） | `src/btree.c` | `pX->nMem` 非零时 `rc = sqlite3BtreeIndexMoveto(pCur, &r, &loc)`，否则 `rc = btreeMoveto(pCur, pX->pKey, pX->nKey, (flags & BTREE_APPEND)!=0, &loc)` |
| `sqlite3BtreeNext` | `src/btree.c` | `sqlite3BtreeTableMoveto` 内部在「目标键恰好是当前键 +1」时调 `sqlite3BtreeNext(pCur, 0)` 代替完整二分 |

### 真实调用方：`sqlite3VdbeFinishMoveto` 的延迟定位

`OP_DeferredSeek` / `OP_IdxRowid` 只记录 `pC->movetoTarget` 并置 `pC->deferredMoveto = 1`，不立即定位；真正读该行时才由 `sqlite3VdbeFinishMoveto`（`src/vdbeaux.c`）调 `sqlite3BtreeTableMoveto` 落地。源码注释说明这样做的目的是「if no reads occur, no unnecessary I/O happens」。`sqlite3VdbeFinishMoveto` 里 `res!=0` 直接返回 `SQLITE_CORRUPT_BKPT`——延迟定位不允许「找不到」。

### 关键魔数与常量出处

| 数值 | 含义 | 出处 |
|---|---|---|
| 20 | B-tree 最大搜索深度 | `src/btreeInt.h` 的 `#define BTCURSOR_MAX_DEPTH 20`；`sqlite3BtreeIndexMoveto` 的下降检查 `if( pCur->iPage>=(BTCURSOR_MAX_DEPTH-1) )` 与 `moveToChild` 的同类检查都用它 |
| 18 | 溢出路径下读取记录时额外分配的填充字节 | `sqlite3BtreeIndexMoveto` 的 `const int nOverrun = 18;` 与 `sqlite3Malloc((u64)nCell+(u64)nOverrun)` |
| 2 | 最小合法索引键长度 | `sqlite3BtreeIndexMoveto` 的 `if( nCell<2 || nCell/pCur->pBt->usableSize>pCur->pBt->nPage )` |
| 127 | `max1bytePayload` 的上限 | `src/btreeInt.h`：`u8 max1bytePayload; /* min(maxLocal,127) */` |
| 99 | 「关系未知」的哨兵返回值 | `indexCellCompare` 的 `return 99;`，源码注释 "Return positive if unknown" |
| 65536 | 页大小上限 | `sqlite3BtreeIndexMoveto` 的注释："The maximum supported page-size is 65536 bytes" |
| 16384 | 索引 B-tree 页内记录字节数上限（因而长度可用 2 字节 varint 表示） | 同上注释："the maximum number of record bytes stored on an index B-Tree page is less than 16384 bytes and may be stored as a 2-byte varint" |
| 0x80 | varint 续位标志 | `sqlite3BtreeTableMoveto` 的 `while( 0x80 <= *(pCell++) )` 与 `sqlite3BtreeIndexMoveto` 的 `!(pCell[1] & 0x80)` |
| -1 / 0 / +1 | `*pRes` 的三态语义 | `sqlite3BtreeTableMoveto` / `sqlite3BtreeIndexMoveto` 的函数头注释 |
| 1 / 0 / -1 | `UnpackedRecord.default_rc` 的合法取值 | `sqlite3BtreeIndexMoveto` 的 `assert( pIdxKey->default_rc==1 || pIdxKey->default_rc==0 || pIdxKey->default_rc==-1 );` |

## 关键取舍

- **查不到精确匹配时不停在「最近的一页」，而是停在「本应落在这里的那一页」。** 源码注释对两条路径都写了同一句话：「If an exact match is not found, then the cursor is always left pointing at a leaf page which would hold the entry if it were present」。这让 `*pRes` 的三态语义成为调用方唯一需要的信息——调用方不必再关心游标停在树的哪个位置。
- **`*pRes` 只表达三态，不表达「在哪一页」。** 位置信息留在 `pCur->ix` / `pCur->pPage` 里，由调用方按需取用。
- **表路径不做任何缓存优化，索引路径做两处。** `sqlite3BtreeTableMoveto` 只有「游标已在目标键上」这一处短路；`sqlite3BtreeIndexMoveto` 额外有「游标已在最后一页且最后一个单元 ≤ 目标键」和「游标在最后一页且第一个单元 ≤ 目标键、可从当前页直接开始二分」两条。第二条的实现方式很特别——用 `goto bypass_moveto_root` 跳过 `moveToRoot`，而不是复制一遍查找循环。
- **「目标键 = 当前键 + 1」用一次 `sqlite3BtreeNext` 代替完整二分。** 源码注释明确说这是优化而非必需：「The correct answer is still obtained without this case, only a little more slowly」。注意这条捷径只在 `sqlite3BtreeTableMoveto` 里，索引路径没有对应逻辑。
- **比较单元时优先「只看单元头」而不是「解析整个单元」。** 索引路径按三种情形分级：单字节长度 varint 且载荷在页内、两字节长度 varint 且载荷在页内、载荷溢出。第三种才付出完整解析（`xParseCell`）+ 缓冲区分配（`sqlite3Malloc` + **18** 字节填充）+ `accessPayload` 读页的代价。
- **为可能读过界的 varint 预留填充字节。** 溢出路径分配的缓冲比记录本身多 **18** 字节，源码注释说明原因是「the xRecordCompare routine may read up to two varints past the end of the buffer」。
- **快捷路径允许「不知道」。** `indexCellCompare` 在无法确定关系时返回 **99**（正值），源码注释说「It is always safe to return a positive value as that will cause the optimization to be skipped」——这是「宁可放弃优化也不给错答案」的设计。
- **索引路径把 `moveToChild` 内联展开，是为了多做一个深度检查。** 展开版多出 `if( pCur->iPage>=(BTCURSOR_MAX_DEPTH-1) ){ return SQLITE_CORRUPT_BKPT; }`，用来挡住损坏库里构造出的超深树。
- **子页类型必须与父页一致。** 取到子页后检查 `pCur->pPage->nCell<1 || pCur->pPage->intKey!=pCur->curIntKey`，不满足就 `releasePage(pCur->pPage)` 并报 `SQLITE_CORRUPT_PGNO(chldPg)`。表路径里这一检查由 `moveToChild` 承担。
- **`sqlite3BtreeIndexMoveto` 的错误一律走 `moveto_index_finish` 统一收尾。** 损坏、OOM、读页失败都 `goto moveto_index_finish`，保证 `pCur->info.nSize = 0` 且 `BTCF_ValidOvfl` 被清掉，游标状态不会停在半途。
- **内部页上「目标键比本页所有分隔键都大」时走最右孩子。** 判定是 `lwr>=pPage->nCell`，此时 `chldPg = get4byte(&pPage->aData[pPage->hdrOffset+8])`（页头偏移 8 处存放最右孩子页号），否则取第 `lwr` 个单元里的左孩子指针。两条路径的写法完全一致。
- **表路径与索引路径在「偏右查找」上行为不同。** 表路径的 `biasRight` 通过 `idx = upr>>(1-biasRight)` 实现（偏右时从最右单元起查）；索引路径没有 `biasRight` 参数，`sqlite3BtreeInsert` 传下来的 `BTREE_APPEND` 意图在索引路径上是通过 `btreeMoveto` 的 `bias` 参数生效的，但 `btreeMoveto` 在索引分支里并未把这个 `bias` 继续传给 `sqlite3BtreeIndexMoveto`——`sqlite3BtreeIndexMoveto` 的签名里根本没有 `bias` 参数。
- **延迟定位不允许「找不到」。** `sqlite3VdbeFinishMoveto` 里 `sqlite3BtreeTableMoveto` 返回的 `res!=0` 直接变成 `SQLITE_CORRUPT_BKPT`，而不是当作「行不存在」处理。
- **`sqlite3BtreeIndexMoveto` 的比较错误会被翻译成损坏错误。** 比较后若 `pIdxKey->errCode` 非零，`rc = SQLITE_CORRUPT_BKPT`；源码断言 `(pIdxKey->errCode!=SQLITE_CORRUPT || c==0)`，即比较函数报告损坏时，比较结果必须是「相等」。

## 与主核 / AI 的关系

本机制是 SQLite「从逻辑键到物理单元位置」的唯一转换点，也是 B-tree 层对外暴露的读路径基础：VDBE 的 `OP_SeekGE` 系列、`OP_NotExists`、`OP_NotFound` 全部收敛到这三个函数。

**架构推论：** `sqlite3BtreeTableMoveto` 与 `sqlite3BtreeIndexMoveto` 把「树形下降」和「键比较」彻底解耦——前者把比较内联成整数比较，后者通过 `sqlite3VdbeFindCompare` 返回的函数指针把比较外置。主数据库内核 `engine/db/storage/` 层若实现同类定位接口，同样需要把「下降策略」与「比较器」分成两处，否则接入新的比较规则（复合键、变长编码、反向序）时会被迫改动下降逻辑。

**架构推论：** 「查不到时停在应落之页 + `*pRes` 三态」这套返回契约，是把「不精确匹配」的判断责任完全推给上层的关键设计。VDBE 的 `OP_SeekGE` 正是靠这个三态决定要不要再调一次 `sqlite3BtreeNext` 前进。主核若采用「精确匹配才返回成功、否则返回错误」的定位契约，就无法实现 `>=` / `>` 这类范围扫描。

**架构推论：** 索引路径里「只读单元头判断载荷是否在页内」的三级分级（依赖 `max1bytePayload = min(maxLocal,127)` 这个常量），是一条纯粹的读放大优化路径——它把绝大多数比较的成本从「解析记录」降到「读 1~2 字节」。主核的索引实现若直接在比较时解析完整记录，在大记录场景下的定位成本会明显更高。

**架构推论：** `btreeMoveto` 作为统一入口并对「记录字段数与 `KeyInfo.nAllField` 不符」直接报 `SQLITE_CORRUPT_BKPT`，是把「键的形状校验」放在入口而非分散到各比较函数里。AI 辅助读码时容易漏掉这一层校验，从而低估损坏库在定位阶段被拦截的概率。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `btreeMoveto` | 统一入口：`pKey` 非空时解包索引记录后转 `sqlite3BtreeIndexMoveto`，字段数与 `pKeyInfo->nAllField` 不符报 `SQLITE_CORRUPT_BKPT`；`pKey` 为空时转 `sqlite3BtreeTableMoveto` |
| src/btree.c | `sqlite3BtreeTableMoveto` | rowid 表定位：命中缓存短路（含「键 +1 用 `sqlite3BtreeNext`」优化）→ `moveToRoot` → 逐页二分 → 内部页按 `lwr>=pPage->nCell` 选最右孩子或第 `lwr` 个单元的左孩子 |
| src/btree.c | `sqlite3BtreeIndexMoveto` | 索引定位：`sqlite3VdbeFindCompare` 选比较函数 → 两条「留在最后一页」的快捷路径 → `moveToRoot` → 页内三级比较（单字节 varint / 两字节 varint / 溢出页）→ 内联展开的 `moveToChild`（含 `BTCURSOR_MAX_DEPTH-1` 深度检查） |
| src/btree.c | `indexCellCompare` | 快捷路径用的单元比较：只在记录完整落在页内时真正比较，越界或记录溢出时返回 99 表示「关系未知」 |
| src/btree.c | `btreeRestoreCursorPosition` | `restoreCursorPosition` 宏的实际实现，调 `btreeMoveto` 恢复被 `saveCursorPosition` 保存的游标位置，并把状态改回 `CURSOR_VALID` 或 `CURSOR_SKIPNEXT` |
| src/btree.c | `sqlite3BtreeInsert` | 定位结果的两个消费者之一：表分支调 `sqlite3BtreeTableMoveto`，索引分支在 `pX->nMem` 非零时调 `sqlite3BtreeIndexMoveto`、否则调 `btreeMoveto` |
| src/btree.c | `moveToRoot` | 定位的起点：把游标移到根页；空表时置 `CURSOR_INVALID` 并返回 `SQLITE_EMPTY` |
| src/btree.c | `moveToChild` | 定位的下降一步：压栈父页、`getAndInitPage` 取子页、检查子页 `nCell<1` 或 `intKey` 类型不匹配则报 `SQLITE_CORRUPT_PGNO`；含 `BTCURSOR_MAX_DEPTH-1` 深度检查 |
| src/btree.c | `findCellPastPtr` | 取页内第 `idx` 个单元的数据体指针（跳过单元头部的左孩子指针） |
| src/vdbeaux.c | `sqlite3VdbeFinishMoveto` | 延迟定位的落地执行：调 `sqlite3BtreeTableMoveto`，`res!=0` 直接返回 `SQLITE_CORRUPT_BKPT` |
| src/vdbeaux.c | `sqlite3VdbeAllocUnpackedRecord` / `sqlite3VdbeRecordUnpack` | 索引键的解包：`btreeMoveto` 与 `OP_NotFound` 路径用它把打包记录还原成 `UnpackedRecord` |
| src/vdbeaux.c | `sqlite3VdbeFindCompare` | 按 `KeyInfo` 与首字段类型挑出合适的 `RecordCompare`（`sqlite3VdbeRecordCompare` 或整数/字符串特化版本），返回给 `sqlite3BtreeIndexMoveto` |
| src/vdbe.c | `OP_SeekGE` / `OP_SeekGT` / `OP_SeekLT` / `OP_SeekLE` | 定位的主要使用者：rowid 表分支调 `sqlite3BtreeTableMoveto`，索引分支调 `sqlite3BtreeIndexMoveto`，再按 `res` 决定要不要 `sqlite3BtreeNext` / `sqlite3BtreePrevious` |
| src/vdbe.c | `OP_NotExists` / `OP_SeekRowid` | 按 rowid 定位：`sqlite3BtreeTableMoveto(pCrsr, iKey, 0, &res)`，`res!=0` 时若 `pOp->p2==0` 报 `SQLITE_CORRUPT_BKPT`，否则跳转 |
| src/vdbe.c | `OP_Found` / `OP_NotFound` / `OP_NoConflict` / `OP_IfNoHope` | 索引定位：结果写进 `pC->seekResult`，`alreadyExists = (pC->seekResult==0)` |
| src/vdbe.c | `OP_Delete`（`OP_IdxDelete` 分支） | `sqlite3BtreeIndexMoveto` 后若 `res!=0` 改走 `sqlite3VdbeFindIndexKey`，仍 `res!=0` 且 `!sqlite3WritableSchema(db)` 则报 `SQLITE_CORRUPT_INDEX` |
| src/btreeInt.h | `BTCURSOR_MAX_DEPTH` | 宏，取值 20，B-tree 搜索深度上限；`moveToChild` 与 `sqlite3BtreeIndexMoveto` 的下降检查都用它 |
| src/btreeInt.h | `BTCF_ValidNKey` / `BTCF_ValidOvfl` / `BTCF_AtLast` | 游标标志位，取值 0x02 / 0x04 / 0x08；分别表示 `info.nKey` 有效、`info.aOverflow` 有效、游标停在最后一条 |
| src/btreeInt.h | `MemPage.max1bytePayload` | 字段，含义 `min(maxLocal,127)`；索引路径判定「单字节长度 varint」的阈值 |

## 参考

- [SQLite B-tree 实现（sqlite.org 官方文档）](https://sqlite.org/btreetree.html)
- [SQLite 文件格式：B-tree 页（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html)
- [SQLite 官方文档：VDBE 指令一览（sqlite.org）](https://sqlite.org/opcode.html)
- [SQLite 源码浏览：src/btree.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/btree.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
