---
project: sqlite
feature: autovacuum
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeIncrVacuum, sqlite3BtreeGetAutoVacuum, sqlite3BtreeSetAutoVacuum]
---

# 自动整理空闲页

## 一句话职责

`sqlite3BtreeIncrVacuum` 在增量 vacuum（incremental vacuum）模式下，把数据库文件尾部的空闲页搬回文件内部的空闲区，从而让文件尾部被截短；`sqlite3BtreeGetAutoVacuum` / `sqlite3BtreeSetAutoVacuum` 则负责读写这个库的 auto-vacuum 模式位（none / full / incremental）。

## 机制是什么

SQLite 的 auto-vacuum 有三档模式，定义在 `src/btree.h`：

- `BTREE_AUTOVACUUM_NONE` = **0**：关闭。删除数据后空闲页进入 freelist，文件大小不变。
- `BTREE_AUTOVACUUM_FULL` = **1**：每次事务提交时自动把空闲页挪到文件末尾并截断文件。
- `BTREE_AUTOVACUUM_INCR` = **2**：增量模式，空闲页同样被挪走，但**不会**在提交时自动发生，必须显式执行 `PRAGMA incremental_vacuum` 才会逐步推进。

B-tree 层内部用两个布尔量表示这个三态：`BtShared.autoVacuum`（`src/btreeInt.h` 中注释为 "True if auto-vacuum is enabled"）与 `BtShared.incrVacuum`（"True if incr-vacuum is enabled"）。`autoVacuum==0` 对应 NONE；`autoVacuum==1 && incrVacuum==0` 对应 FULL；`autoVacuum==1 && incrVacuum==1` 对应 INCR。

**前置条件：指针映射页（ptrmap）。** auto-vacuum 需要能反查「每一页被谁引用」，这靠文件里的 ptrmap 页实现。`src/btree.c` 的 `sqlite3BtreeIncrVacuum` 里对页号的处理全部绕不开两个宏（`src/btreeInt.h`）：`PTRMAP_ISPAGE(pBt, pgno)` 判断某页是否是 ptrmap 页，`PENDING_BYTE_PAGE(pBt)` 给出锁字节页的页号——`#define PENDING_BYTE_PAGE(pBt) ((Pgno)((PENDING_BYTE/((pBt)->pageSize))+1))`。这两个页号在搬页过程中不能当作普通空闲页处理。

**本机制的边界：** 不负责 `VACUUM` 命令本身（`src/vdbe.c` 里 `OP_Vacuum` 调的是 `sqlite3RunVacuum`，走的是完全不同的路径）、不负责页的写回（那是 pager 层 `sqlite3PagerWrite` / `sqlite3PagerMovepage` 的事）、不负责 `PRAGMA auto_vacuum` / `PRAGMA incremental_vacuum` 这两条 SQL 语句本身的解析与指令生成（那在 `src/pragma.c`）。

## 怎么实现的

### `sqlite3BtreeSetAutoVacuum`：设置模式

签名（`src/btree.c`）：`int sqlite3BtreeSetAutoVacuum(Btree *p, int autoVacuum)`。

1. 编译期开关：`#ifdef SQLITE_OMIT_AUTOVACUUM` 时直接 `return SQLITE_READONLY`，整个功能被裁掉。
2. 进入 B-tree：`sqlite3BtreeEnter(p)`；`u8 av = (u8)autoVacuum;` 把入参收敛成一个字节。
3. **页大小已固定的库不允许改模式。** 条件是 `(pBt->btsFlags & BTS_PAGESIZE_FIXED)!=0 && (av ?1:0)!=pBt->autoVacuum`。`BTS_PAGESIZE_FIXED` 在 `src/btree.c` 的打开路径里被置位——当文件头里的页大小合法时（页大小是 2 的幂且在 `[512, SQLITE_MAX_PAGE_SIZE]` 内）就 `pBt->btsFlags |= BTS_PAGESIZE_FIXED`。此时改 auto-vacuum 模式会返回 `SQLITE_READONLY`。
4. 成功则写两个标志位：`pBt->autoVacuum = av ?1:0;`，`pBt->incrVacuum = av==2 ?1:0;`。注意 `incrVacuum` 只在 `av` **恰好等于 2** 时才置 1，其余取值（0、1、以及 2 以外的大于 2 的值）都置 0。
5. `sqlite3BtreeLeave(p)` 后返回 `rc`。

调用方是 `src/pragma.c` 的 `PragTyp_AUTO_VACUUM` 分支：`rc = sqlite3BtreeSetAutoVacuum(pBt, eAuto);`，其中 `eAuto` 由 `getAutoVacuum(zRight)` 解析——字符串 "none" / "full" / "incremental" 分别对应 `BTREE_AUTOVACUUM_NONE` / `BTREE_AUTOVACUUM_FULL` / `BTREE_AUTOVACUUM_INCR`，数字形式由 `sqlite3Atoi(z)` 解析并夹到 `[0,2]`，超出范围归 0。

### `sqlite3BtreeGetAutoVacuum`：读取模式

签名（`src/btree.c`）：`int sqlite3BtreeGetAutoVacuum(Btree *p)`。

`SQLITE_OMIT_AUTOVACUUM` 下返回 `BTREE_AUTOVACUUM_NONE`。否则进入 B-tree 后按两个标志位三选一返回：`(!p->pBt->autoVacuum) ? BTREE_AUTOVACUUM_NONE : (!p->pBt->incrVacuum) ? BTREE_AUTOVACUUM_FULL : BTREE_AUTOVACUUM_INCR`。

调用方是 `src/pragma.c` 的 `PragTyp_AUTO_VACUUM` 分支的无参形式：`returnSingleInt(v, sqlite3BtreeGetAutoVacuum(pBt));`。

### 模式位的来源与持久化

- **编译期默认值：** `src/btree.h` 里 `#ifndef SQLITE_DEFAULT_AUTOVACUUM` / `#define SQLITE_DEFAULT_AUTOVACUUM 0`，即默认关闭。
- **新建库：** `src/btree.c` 的打开路径中，若页大小不合法（走的是新库分支）且 `zFilename && !isMemdb`，则 `pBt->autoVacuum = (SQLITE_DEFAULT_AUTOVACUUM ? 1 : 0);`、`pBt->incrVacuum = (SQLITE_DEFAULT_AUTOVACUUM==2 ? 1 : 0);`。`:memory:` 库明确保持 `autoVacuum` 为 0。
- **已有库：** 从文件头读。`pBt->autoVacuum = (get4byte(&zDbHeader[36 + 4*4])?1:0);` 读的是文件头偏移 **52**（36 是 freelist 页数所在偏移，52 = 36 + 4*4），对应「最大根页号」字段——该字段非零即表示这是 auto-vacuum 库、文件里含 ptrmap 页；`pBt->incrVacuum = (get4byte(&zDbHeader[36 + 7*4])?1:0);` 读的是偏移 **64**（36 + 7*4），对应增量 vacuum 标志位。
- **写入文件头：** `src/pragma.c` 的 `PragTyp_AUTO_VACUUM` 分支在 `eAuto==1 || eAuto==2` 时生成一段 VDBE 指令序列：`OP_Transaction` → `OP_ReadCookie`（`BTREE_LARGEST_ROOT_PAGE`）→ `OP_If` → `OP_Halt` → `OP_SetCookie`（`BTREE_INCR_VACUUM`，定义为 **7**，见 `src/btree.h`）。`OP_SetCookie` 的 P3 写的是 `eAuto - 1`。

### `sqlite3BtreeIncrVacuum`：做一步增量整理

签名（`src/btree.c`）：`int sqlite3BtreeIncrVacuum(Btree *p)`。函数头注释写明：「A write-transaction must be opened before calling this function. It performs a single unit of work towards an incremental vacuum.」

1. 进入 B-tree，并断言处于写事务：`assert( pBt->inTransaction==TRANS_WRITE && p->inTrans==TRANS_WRITE );`。
2. **未启用 auto-vacuum 直接完成。** `if( !pBt->autoVacuum ){ rc = SQLITE_DONE; }`。
3. 取三个数：`Pgno nOrig = btreePagecount(pBt);`（即 `pBt->nPage`，文件当前页数）；`Pgno nFree = get4byte(&pBt->pPage1->aData[36]);`（文件头偏移 36，freelist 中的空闲页总数）；`Pgno nFin = finalDbSize(pBt, nOrig, nFree);`（整理完成后的目标页数）。
4. **损坏检测。** `if( nOrig<nFin || nFree>=nOrig ){ rc = SQLITE_CORRUPT_BKPT; }`——目标页数不得大于当前页数，空闲页数不得达到或超过总页数。
5. **没有空闲页则完成。** `else if( nFree>0 )` 之外走 `else{ rc = SQLITE_DONE; }`。
6. 有空闲页时依次做三件事：
   - `rc = saveAllCursors(pBt, 0, 0);` —— 保存所有游标位置（`saveAllCursors` 遍历 `pBt->pCursor` 链表，命中时转调 `saveCursorsOnList`）。
   - `invalidateAllOverflowCache(pBt);` —— 让所有游标的溢出页链缓存失效（`invalidateOverflowCache(pCur)` 清 `curFlags` 的 `BTCF_ValidOvfl` 位）。
   - `rc = incrVacuumStep(pBt, nFin, nOrig, 0);` —— 真正搬页（最后一个参数 `bCommit` 传 **0**，表示这是增量 vacuum 而非提交时自动整理）。
7. 成功后更新页 1：`sqlite3PagerWrite(pBt->pPage1->pDbPage);` 然后 `put4byte(&pBt->pPage1->aData[28], pBt->nPage);`——把文件头偏移 **28**（in-header database size）改写为新的页数。

**调用链：** 唯一的调用方是 `src/vdbe.c` 的 `OP_IncrVacuum` 指令：`rc = sqlite3BtreeIncrVacuum(pBt);`。`rc==SQLITE_DONE` 时跳转到指令的 P2 地址，否则 `goto abort_due_to_error`。而 `OP_IncrVacuum` 指令又只由 `src/pragma.c` 的 `PragTyp_INCREMENTAL_VACUUM` 分支发出：`sqlite3VdbeAddOp1(v, OP_IncrVacuum, iDb);`，外面套一个 `OP_Integer` / `OP_AddImm` / `OP_IfPos` 的计数循环。循环上限来自 `int iLimit = 0; if( zRight==0 || !sqlite3GetInt32(zRight, &iLimit) || iLimit<=0 ){ iLimit = 0x7fffffff; }`——未给参数、参数不是整数、或参数 ≤ 0 时上限取 **0x7fffffff**。

### `incrVacuumStep`：一次搬页

签名（`src/btree.c`）：`static int incrVacuumStep(BtShared *pBt, Pgno nFin, Pgno iLastPg, int bCommit)`。这是 `sqlite3BtreeIncrVacuum` 与提交时自动整理（`autoVacuumCommit`）共用的搬页内核。

1. 断言 `iLastPg>nFin`。
2. `if( !PTRMAP_ISPAGE(pBt, iLastPg) && iLastPg!=PENDING_BYTE_PAGE(pBt) )`——只有既不是 ptrmap 页、也不是锁字节页时才处理。
3. 读 freelist 页数：`nFreeList = get4byte(&pBt->pPage1->aData[36]);` 若为 0 则 `return SQLITE_DONE;`（没活可干）。
4. `rc = ptrmapGet(pBt, iLastPg, &eType, &iPtrPage);` 读该页在 ptrmap 里的条目。
5. **`eType==PTRMAP_ROOTPAGE` 是损坏。** `if( eType==PTRMAP_ROOTPAGE ){ return SQLITE_CORRUPT_BKPT; }`——根页不能被搬走。
6. **`eType==PTRMAP_FREEPAGE`（空闲页）**：当 `bCommit==0`（增量模式）时，需要把这一页从文件 freelist 链里摘掉：调 `allocateBtreePage(pBt, &pFreePg, &iFreePg, iLastPg, BTALLOC_EXACT)` 精确分配同一页号，随后 `releasePage(pFreePg)`。`BTALLOC_EXACT` 定义在 `src/btree.c` 为 **1**（`BTALLOC_ANY` = **0**，`BTALLOC_LE` = **2**）。当 `bCommit!=0` 时不做这一步——源码注释说明：这种情况下 freelist 之后会被整体清零，留着无妨。
7. **其他类型（btree 页 / 溢出页）**：先 `btreeGetPage(pBt, iLastPg, &pLastPg, 0)` 取出该页。然后进入分配循环：
   - `bCommit==0` 时：源码注释写明「this loop runs exactly once and page pLastPg is swapped with the first free page pulled off the free list」，此时 `eMode = BTALLOC_LE; iNear = nFin;`。
   - `bCommit!=0` 时：`eMode` 保持 `BTALLOC_ANY`，`iNear` 保持 **0**，循环条件 `while( bCommit && iFreePg>nFin )` 反复取页，直到找到一个位于文件前 `nFin` 页之内的空闲页。
   - 循环体：`dbSize = btreePagecount(pBt);` 然后 `allocateBtreePage(pBt, &pFreePg, &iFreePg, iNear, eMode);`；若 `iFreePg>dbSize` 则 `return SQLITE_CORRUPT_BKPT;`。
   - 最后 `relocatePage(pBt, pLastPg, eType, iPtrPage, iFreePg, bCommit);` 真正搬页。
8. `bCommit==0` 时的收尾：循环 `do{ iLastPg--; }while( iLastPg==PENDING_BYTE_PAGE(pBt) || PTRMAP_ISPAGE(pBt, iLastPg) );` 跳过 ptrmap 页与锁字节页，然后 `pBt->bDoTruncate = 1;`（标记需要截断文件）与 `pBt->nPage = iLastPg;`（更新当前页数）。
9. 成功返回 `SQLITE_OK`。

### `finalDbSize`：算出整理后的目标页数

签名（`src/btree.c`）：`static Pgno finalDbSize(BtShared *pBt, Pgno nOrig, Pgno nFree)`。

1. `nEntry = pBt->usableSize/5;` —— 一个 ptrmap 页能装的条目数；每个 ptrmap 条目是 5 字节（1 字节类型 + 4 字节父页号）。
2. `nPtrmap = (nFree-nOrig+PTRMAP_PAGENO(pBt, nOrig)+nEntry)/nEntry;` —— 整理过程中会腾出多少个 ptrmap 页。
3. `nFin = nOrig - nFree - nPtrmap;` —— 目标页数 = 原页数 − 空闲页数 − 腾出的 ptrmap 页数。
4. 若 `nOrig>PENDING_BYTE_PAGE(pBt) && nFin<PENDING_BYTE_PAGE(pBt)` 则 `nFin--;`（不能把文件缩到锁字节页之前）。
5. `while( PTRMAP_ISPAGE(pBt, nFin) || nFin==PENDING_BYTE_PAGE(pBt) ){ nFin--; }` —— 再跳过 ptmap 页与锁字节页。

### `relocatePage`：把一页搬到新位置

签名（`src/btree.c`）：`static int relocatePage(BtShared *pBt, MemPage *pDbPage, u8 eType, Pgno iPtrPage, Pgno iFreePage, int isCommit)`。

- `if( iDbPage<3 ) return SQLITE_CORRUPT_BKPT;` —— 页 1、2 不能搬。
- `sqlite3PagerMovepage(pPager, pDbPage->pDbPage, iFreePage, isCommit);` 由 pager 层真正改文件位置；随后 `pDbPage->pgno = iFreePage;`。
- 若 `eType==PTRMAP_BTREE || eType==PTRMAP_ROOTPAGE`，调 `setChildPtrmaps(pDbPage)` 更新子页与溢出页的 ptrmap 条目；否则（溢出页）读 `nextOvfl = get4byte(pDbPage->aData);`，非 0 时 `ptrmapPut(pBt, nextOvfl, PTRMAP_OVERFLOW2, iFreePage, &rc);`。
- 修父页里的指针：`btreeGetPage(pBt, iPtrPage, &pPtrPage, 0)` → `sqlite3PagerWrite(pPtrPage->pDbPage)` → `modifyPagePointer(pPtrPage, iDbPage, iFreePage, eType)` → `ptrmapPut(pBt, iFreePage, eType, iPtrPage, &rc)`。

### ptrmap 条目类型

`src/btreeInt.h` 定义的宏：`PTRMAP_ROOTPAGE` = **1**、`PTRMAP_FREEPAGE` = **2**、`PTRMAP_OVERFLOW1` = **3**、`PTRMAP_OVERFLOW2` = **4**、`PTRMAP_BTREE` = **5**。

`src/btree.c` 的 `ptrmapPageno(BtShared *pBt, Pgno pgno)` 算出某个页号对应哪个 ptrmap 页：`if( pgno<2 ) return 0;`；`nPagesPerMapPage = (pBt->usableSize/5)+1;`；`iPtrMap = (pgno-2)/nPagesPerMapPage;`；`ret = (iPtrMap*nPagesPerMapPage) + 2;`；若结果正好是锁字节页则 `ret++;`。

### FULL 模式：提交时自动整理

`src/btree.c` 的 `static int autoVacuumCommit(Btree *p)` 是 FULL 模式的执行者，函数头注释写明「This routine is called prior to sqlite3PagerCommit when a transaction is committed for an auto-vacuum database.」

- 入口条件：`if( !pBt->incrVacuum )` —— 只有 FULL 模式（`incrVacuum==0`）走这条路径；增量模式跳过。
- 损坏检测：`nOrig = btreePagecount(pBt); if( PTRMAP_ISPAGE(pBt, nOrig) || nOrig==PENDING_BYTE_PAGE(pBt) ) return SQLITE_CORRUPT_BKPT;`——文件最后一页不能是 ptrmap 页或锁字节页。
- **用户可介入回收量。** 若 `db->xAutovacPages` 非空，则调 `db->xAutovacPages(db->pAutovacPagesArg, db->aDb[iDb].zDbSName, nOrig, nFree, pBt->pageSize)` 拿到建议回收页数 `nVac`；`if( nVac>nFree ) nVac = nFree;` 夹住上限；`if( nVac==0 ) return SQLITE_OK;`——回调要求一页都不回收就直接返回。若没有回调，`nVac = nFree;` 全量回收。`db->xAutovacPages` 的签名见 `src/sqliteInt.h`：`unsigned int (*xAutovacPages)(void*,const char*,u32,u32,u32);`，由 `src/main.c` 的 `sqlite3_file_control` 路径设置。
- 循环搬页：`for(iFree=nOrig; iFree>nFin && rc==SQLITE_OK; iFree--){ rc = incrVacuumStep(pBt, nFin, iFree, nVac==nFree); }`——注意这里 `bCommit` 传的是 `nVac==nFree`，即只有「全量回收」时才走提交模式。
- 收尾：`sqlite3PagerWrite(pBt->pPage1->pDbPage);`；若全量回收则把文件头偏移 **32**（freelist 首 trunk 页号）与偏移 **36**（freelist 页数）都清零；再把偏移 **28**（in-header database size）写为 `nFin`；然后 `pBt->bDoTruncate = 1; pBt->nPage = nFin;`。
- 失败回滚：`if( rc!=SQLITE_OK ){ sqlite3PagerRollback(pPager); }`。

**`autoVacuumCommit` 的调用方**是 `src/btree.c` 的 `sqlite3BtreeCommitPhaseOne(Btree *p, const char *zSuperJrnl)`：`if( pBt->autoVacuum ){ rc = autoVacuumCommit(p); ... }`，紧接着 `if( pBt->bDoTruncate ){ sqlite3PagerTruncateImage(pBt->pPager, pBt->nPage); }` 执行实际截断。**回退路径：** `autoVacuumCommit` 内部出错时调 `sqlite3PagerRollback(pPager)` 回滚 pager 层；`sqlite3BtreeCommitPhaseOne` 中 `autoVacuumCommit` 返回非 `SQLITE_OK` 时直接 `sqlite3BtreeLeave(p); return rc;`，不再往下走 `sqlite3PagerCommitPhaseOne`。

## 关键取舍

- **把 auto-vacuum 做成三态而非布尔。** `src/btree.h` 用 `BTREE_AUTOVACUUM_NONE`（0）/ `BTREE_AUTOVACUUM_FULL`（1）/ `BTREE_AUTOVACUUM_INCR`（2）三个常量，而 `BtShared` 里只用两个布尔位 `autoVacuum` + `incrVacuum` 编码。**架构推论：** 用两个布尔而不是一个三态字段，能让 `if( !pBt->autoVacuum )` 这类「是否启用」的判断在 FULL 与 INCR 两种模式下共用同一分支，同时把「要不要在提交时自动做」单独隔出来。代价是读代码时必须记住「autoVacuum 为真不等于 FULL」，`sqlite3BtreeGetAutoVacuum` 的三选一表达式就是这个编码的解码器。
- **增量模式不自动触发。** `autoVacuumCommit` 的入口条件 `if( !pBt->incrVacuum )` 决定了 INCR 模式在提交时什么都不做，必须由 `PRAGMA incremental_vacuum` 通过 `OP_IncrVacuum` 显式驱动。**架构推论：** 这把「回收空闲页」的成本从每次提交的临界路径上摘下来，换来的是文件不会自动缩小、需要应用层记得调用。代价是增量模式的文件大小回收依赖外部调度。
- **一次只搬一页。** `sqlite3BtreeIncrVacuum` 的函数头注释明确写「It performs a single unit of work towards an incremental vacuum」，返回值区分 `SQLITE_DONE`（完成）与 `SQLITE_OK`（还有活）。`PRAGMA incremental_vacuum(N)` 的循环由 `OP_Integer` / `OP_AddImm` / `OP_IfPos` 三条指令驱动，参数缺省或非法时上限取 `0x7fffffff`。**架构推论：** 把长循环放在 VDBE 层而不是 B-tree 层，让单次调用的栈占用与事务时长都可控，用户可以用 `N` 限制单次工作量。代价是每步都要重新进入一次 VDBE 指令循环。
- **搬页前先保存游标、清溢出缓存。** `sqlite3BtreeIncrVacuum` 在搬页前依次调 `saveAllCursors(pBt, 0, 0)` 与 `invalidateAllOverflowCache(pBt)`；`autoVacuumCommit` 里也调了 `invalidateAllOverflowCache(pBt)`。**架构推论：** 游标位置与溢出页链缓存都缓存了「页在原位置」这一前提，页一旦换位置这些缓存就失效，必须显式作废。代价是搬页会让已打开游标的状态保存变多。
- **freelist 清理区分增量与提交两种模式。** `incrVacuumStep` 中 `eType==PTRMAP_FREEPAGE` 分支只在 `bCommit==0` 时调 `allocateBtreePage(..., BTALLOC_EXACT)` 把该页从 freelist 摘掉；源码注释写明 `bCommit` 非零时「the free-list will be truncated to zero after this function returns, so it doesn't matter if it still contains some garbage entries」。**架构推论：** 提交路径在 `autoVacuumCommit` 末尾把文件头偏移 32 与 36 直接清零，因此逐页摘除是多余的；增量路径不能清零 freelist（后续还要用），所以必须逐页精确摘除。代价是同一段搬页逻辑要写两套 freelist 处理。
- **空闲页候选用 BTALLOC_LE 约束在 nFin 之内。** `bCommit==0` 时 `eMode = BTALLOC_LE; iNear = nFin;`，源码注释说明「this loop runs exactly once and page pLastPg is swapped with the first free page pulled off the free list」。`BTALLOC_LE` 的语义见 `allocateBtreePage` 的注释：「the page returned will be less than or equal to nearby if any such page exists」；且 `allocateBtreePage` 中 `eMode==BTALLOC_LE` 时无条件 `searchList = 1;`，强制遍历整条 freelist 去找符合条件的页。**架构推论：** 用「≤ nFin」这个上界保证搬完之后目标页号仍在文件有效范围内，代价是必须扫描整条 freelist 而不能就地取链首。
- **损坏检测集中在少数几个判断上。** `sqlite3BtreeIncrVacuum` 判 `nOrig<nFin || nFree>=nOrig`；`incrVacuumStep` 判 `eType==PTRMAP_ROOTPAGE` 不可搬、页号 `<3` 不可搬（`relocatePage` 中 `if( iDbPage<3 ) return SQLITE_CORRUPT_BKPT;`）、`iFreePg>dbSize` 即 `SQLITE_CORRUPT_BKPT`；`autoVacuumCommit` 判最后一页不能是 ptrmap 页或锁字节页。**架构推论：** 「文件是不可信输入」这一前提被落实成一组集中的 `SQLITE_CORRUPT_BKPT` 出口，而不是散落在搬页路径各处。代价是这些判断构成了一条必须逐条维护的完整性约束链。
- **页 1 与锁字节页、ptrmap 页被系统性排除。** `PTRMAP_ISPAGE(pBt, pgno)` 与 `PENDING_BYTE_PAGE(pBt)` 在 `incrVacuumStep`、`finalDbSize`、`autoVacuumCommit`、`ptrmapPageno` 四处都被显式排除。**架构推论：** ptrmap 页自身也要被搬（腾空间），但不能被当作普通空闲页，否则自举会失效。代价是搬页逻辑里散布着这些特判。
- **`xAutovacPages` 回调把回收量的决定权交给应用。** `autoVacuumCommit` 中 `db->xAutovacPages` 存在时，回收量由应用给出（夹在 `nFree` 以内），为 0 时直接 `return SQLITE_OK` 放弃本次回收。**架构推论：** 这是一个「应用可以按负载特征调节回收力度」的挂钩点，SQLite 核心不替应用决定。代价是回收行为变成外部可配置的，测试必须覆盖回调有无两条路径。
- **改模式受页大小固定标志约束。** `sqlite3BtreeSetAutoVacuum` 在 `BTS_PAGESIZE_FIXED` 且目标模式与当前不同时返回 `SQLITE_READONLY`。**架构推论：** 这不是错误处理上的偷懒，而是把「这个库的物理布局已经定死」这一事实编码成明确的错误码，让上层（如 `src/pragma.c`）能给出可诊断的反馈。

## 与主核 / AI 的关系

主核 `engine/db/` 是从零手写的多模态数据库内核，与 SQLite 的 B-tree 层没有直接的代码对应关系，也没有 SQLite 那套「页可在文件内搬移」的 ptrmap 机制。

**架构推论：** 若主核要提供类似的空间回收能力，最值得借鉴的是 `incrVacuumStep` 里 `bCommit` 这一个布尔参数所体现的取舍：同一个搬页内核，在「提交时自动全量回收」与「用户显式驱动的分步回收」两种语义下共用，只靠这一个标志切换 freelist 处理方式与空闲页分配模式（`BTALLOC_ANY` vs `BTALLOC_LE`）。主核若为两种语义各写一套搬页逻辑，两条路径很快会分叉。

**架构推论：** `sqlite3BtreeIncrVacuum` 的返回码约定（`SQLITE_DONE` 表示完成、`SQLITE_OK` 表示还有活）是「单步推进 + 上层计数循环」的标准范式：底层只保证一步的原子性，循环次数与终止条件交给上层指令序列。主核的 checkpoint 或日志回收若也采用分步推进，宜沿用同一种「完成用 DONE 而非 OK 表达」的约定，避免调用方需要额外的状态位。

**架构推论：** `xAutovacPages` 那种「核心留一个回调钩子、由应用决定回收力度」的设计，可以平移到主核的日志回收上（例如让上层按业务峰值决定每轮回收多少），而不必把策略硬编码进存储引擎。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `sqlite3BtreeIncrVacuum` | 增量 vacuum 单步入口：校验 `nOrig`/`nFree`/`nFin`，保存游标、清溢出缓存，调 `incrVacuumStep` 搬页，最后改写文件头偏移 28 的页数 |
| src/btree.c | `sqlite3BtreeGetAutoVacuum` | 读取 auto-vacuum 模式：按 `autoVacuum` 与 `incrVacuum` 两个标志位返回 `BTREE_AUTOVACUUM_NONE` / `BTREE_AUTOVACUUM_FULL` / `BTREE_AUTOVACUUM_INCR` |
| src/btree.c | `sqlite3BtreeSetAutoVacuum` | 设置 auto-vacuum 模式：`BTS_PAGESIZE_FIXED` 且模式变化时返回 `SQLITE_READONLY`，否则写 `autoVacuum` 与 `incrVacuum`（后者仅在入参恰为 2 时置 1） |
| src/btree.c | `incrVacuumStep` | 搬页内核：排除 ptrmap 页与锁字节页，按 ptrmap 类型分派（根页判损坏、空闲页精确摘除、其余页搬走），`bCommit==0` 时把 `pBt->nPage` 降到新末尾并置 `bDoTruncate` |
| src/btree.c | `finalDbSize` | 计算整理后的目标页数：`nFin = nOrig - nFree - nPtrmap`，再跳过 ptrmap 页与锁字节页 |
| src/btree.c | `relocatePage` | 真正搬一页：调 `sqlite3PagerMovepage` 改页号，更新子页/溢出页的 ptrmap 条目，修父页指针并回填 ptrmap |
| src/btree.c | `autoVacuumCommit` | FULL 模式的提交时整理：读 `xAutovacPages` 回调定回收量，循环调 `incrVacuumStep`，清文件头偏移 32/36，置 `bDoTruncate` 与 `nPage`，失败调 `sqlite3PagerRollback` |
| src/btree.c | `sqlite3BtreeCommitPhaseOne` | `autoVacuumCommit` 的调用方：`pBt->autoVacuum` 为真时先做自动整理，再按 `bDoTruncate` 调 `sqlite3PagerTruncateImage` |
| src/btree.c | `allocateBtreePage` | 空闲页分配：`BTALLOC_EXACT` 时若 ptrmap 表明目标页在 freelist 上则全表搜索；`BTALLOC_LE` 时无条件 `searchList = 1` |
| src/btree.c | `ptrmapPageno` | 由页号算出其所属 ptrmap 页号：每页 `usableSize/5` 条目，页号 < 2 返回 0，结果撞上锁字节页则加 1 |
| src/btree.c | `invalidateAllOverflowCache` | 遍历 `pBt->pCursor` 链表清各游标的 `BTCF_ValidOvfl` 位，使溢出页链缓存失效 |
| src/btree.c | `saveAllCursors` | 搬页前保存所有游标位置；命中需要保存的游标时转调 `saveCursorsOnList` |
| src/btree.c | `btreePagecount` | 返回 `pBt->nPage`，即当前数据库文件的页数 |
| src/btree.c | `sqlite3BtreeOpen`（打开路径） | 模式位来源：新建库时取 `SQLITE_DEFAULT_AUTOVACUUM`（`:memory:` 库恒为 0）；已有库时从文件头偏移 52 与偏移 64 读出 |
| src/btreeInt.h | `PTRMAP_ISPAGE` / `PTRMAP_PAGENO` / `PENDING_BYTE_PAGE` | 宏：判断页是否为 ptrmap 页、求 ptrmap 页号、求锁字节页页号；三者在搬页路径上被系统性排除 |
| src/btreeInt.h | `PTRMAP_ROOTPAGE` / `PTRMAP_FREEPAGE` / `PTRMAP_OVERFLOW1` / `PTRMAP_OVERFLOW2` / `PTRMAP_BTREE` | 宏，取值分别为 1 / 2 / 3 / 4 / 5；ptrmap 条目的类型码 |
| src/btreeInt.h | `BtShared.autoVacuum` / `BtShared.incrVacuum` | 结构体字段：两个 `u8` 标志位，编码 none / full / incremental 三态 |
| src/btree.h | `BTREE_AUTOVACUUM_NONE` / `BTREE_AUTOVACUUM_FULL` / `BTREE_AUTOVACUUM_INCR` | 宏，取值分别为 0 / 1 / 2；auto-vacuum 三档模式的对外表示 |
| src/btree.h | `SQLITE_DEFAULT_AUTOVACUUM` | 宏，默认值 0；新库默认关闭 auto-vacuum |
| src/btree.h | `BTREE_INCR_VACUUM` | 宏，取值 7；`OP_SetCookie` 写增量 vacuum 标志位时使用的 cookie 编号 |
| src/btree.h | `sqlite3BtreeIncrVacuum` / `sqlite3BtreeGetAutoVacuum` / `sqlite3BtreeSetAutoVacuum` | 三个接口在 `src/btree.h` 中的公开声明，与 `src/btree.c` 的实现配对 |
| src/pragma.c | `PragTyp_AUTO_VACUUM` | `PRAGMA auto_vacuum` 的处理分支：无参时调 `sqlite3BtreeGetAutoVacuum` 读模式，有参时调 `getAutoVacuum` 解析后调 `sqlite3BtreeSetAutoVacuum`，并生成写文件头偏移 64 的 VDBE 指令序列 |
| src/pragma.c | `getAutoVacuum` | 把 "none" / "full" / "incremental" 或数字解析成 0 / 1 / 2，数字超出 `[0,2]` 归 0 |
| src/pragma.c | `PragTyp_INCREMENTAL_VACUUM` | `PRAGMA incremental_vacuum(N)` 的处理分支：缺省或非法参数时上限取 `0x7fffffff`，生成 `OP_IncrVacuum` 指令与计数循环 |
| src/vdbe.c | `OP_IncrVacuum` | `sqlite3BtreeIncrVacuum` 的唯一调用方；返回 `SQLITE_DONE` 时跳转到指令 P2，否则 `goto abort_due_to_error` |
| src/vdbe.c | `OP_Vacuum` | `VACUUM` 命令的指令实现，调 `sqlite3RunVacuum`；与本页的增量 vacuum 路径无关 |
| src/sqliteInt.h | `db->xAutovacPages` | 函数指针，签名 `unsigned int (*)(void*,const char*,u32,u32,u32)`；`autoVacuumCommit` 用它让应用决定本次回收的页数 |

## 参考

- [PRAGMA auto_vacuum（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_auto_vacuum) —— 说明 none / full / incremental 三档模式的语义差异，以及「从 none 改到 full 或 incremental 只能在空库或 VACUUM 之后进行」的限制。
- [PRAGMA incremental_vacuum（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_incremental_vacuum) —— 说明增量 vacuum 的显式触发方式与参数含义。
- [SQLite 文件格式：增量 vacuum 设置与 ptrmap 页（sqlite.org 官方文档）](https://sqlite.org/fileformat2.html) —— 说明文件头偏移 52（最大根页号）、偏移 64（增量 vacuum 标志）以及 ptrmap 页的磁盘格式。
- [SQLite 官方文档：VACUUM 命令](https://sqlite.org/lang_vacuum.html) —— 与本页的增量 vacuum 路径作对照：VACUUM 重建整个文件，而 auto-vacuum 只截掉 freelist。
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— 支撑本页关于 `sqlite3BtreeIncrVacuum` / `incrVacuumStep` / `finalDbSize` / `autoVacuumCommit` 的行为描述。
- [sqlite/sqlite 仓库中的 src/pragma.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pragma.c) —— 支撑 `PRAGMA auto_vacuum` 与 `PRAGMA incremental_vacuum` 的参数解析与指令生成描述。
