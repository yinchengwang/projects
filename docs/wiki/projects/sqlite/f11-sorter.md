---
project: sqlite
feature: sorter
page_type: feature
schema_version: 1
refs:
  - path: src/vdbesort.c
    symbols: [sqlite3VdbeSorterInit, sqlite3VdbeSorterWrite, sqlite3VdbeSorterNext, sqlite3VdbeSorterClose]
---

# 外排排序器

## 一句话职责

`sqlite3VdbeSorterInit` / `sqlite3VdbeSorterWrite` / `sqlite3VdbeSorterNext` / `sqlite3VdbeSorterClose` 这组接口构成 SQLite 的 `VdbeSorter` 对象：在 ORDER BY 走不了索引、或 CREATE INDEX 需要对大表排序时，把待排序的记录先攒在内存里（内存超阈值则落盘成 PMA 块），再按需归并读回有序结果。

## 机制是什么

`VdbeSorter` 是 VDBE 层的「外排排序器」（external merge sort），源码全部在 `src/vdbesort.c`。

**它解决什么问题。** 文件头注释写明它用于两类场景：一是 `CREATE INDEX` 语句需要排序大量键；二是带 `ORDER BY` 子句、且无法用索引满足、同时没有 LIMIT 子句的 SELECT。

**接口与调用顺序是硬约束。** 文件头注释明确列出：

```
Init()
for each record: Write()
Rewind()
  Rowkey()/Compare()
Next()
Close()
```

即 `Write()` 只能发生在 `Init()`/`Reset()` 与 `Rewind()` 之间；`Next()`、`Rowkey()`、`Compare()` 只能发生在 `Rewind()` 与 `Close()`/`Reset()` 之间。

**核心数据单位是 PMA（Packed Memory Array）。** 记录先以未排序状态放在主内存里；若内存占用不超过阈值，`Rewind()` 时用内存归并排序完成排序，不产生临时文件，后续 `Rowkey()`/`Next()`/`Compare()` 直接从内存链表读取。

若内存占用超过阈值，则把当前内存里的记录排序后以 PMA 格式写入临时文件，这次生成的 PMA 称为「level-0 PMA」。更高层的 PMA 由已有的 PMA 归并产生：两个或多个 level-0 PMA 归并成一个 level-1 PMA，依此类推。

**内存阈值的来源。** 文件头注释写明：阈值大致等于主库页缓存上限，具体是 `PRAGMA main.page_size` 乘以 `PRAGMA main.cache_size`（字节）。对应到代码里（`sqlite3VdbeSorterInit`），`pSorter->mxPmaSize` 由 `db->aDb[0].pSchema->cache_size` 与 `pgsz` 算出。

**单线程与多线程模式。** 单线程模式下，所有 PMA 追加写入同一个临时文件。多线程模式下，最多可打开 N+1 个临时文件（N 是配置的 worker 线程数）；此时调用线程改为启动一个后台线程去做排序与写盘；若已有 N 个 worker 线程在跑，则主线程自己做这件事。多线程模式的两个前提是：编译期 `SQLITE_MAX_WORKER_THREADS` 大于 0，且运行时通过 `PRAGMA threads=N`（N>0）启用了 worker 线程。

**Rewind 之后的归并策略。** 若 PMA 总数少于 `SORTER_MAX_MERGE_COUNT` 且是单线程模式，则这些 PMA 在 VDBE 取键时由 `MergeEngine` 增量归并。多线程模式下则启动后台线程归并已有 PMA：后台线程每归并出 T 字节数据到一个有序 PMA，主线程就开始读这个 PMA，同时后台线程继续归并下 T 字节。T 取 Write() 判定内存阈值所用值的一半。

若 PMA 总数超过 `SORTER_MAX_MERGE_COUNT`，则使用分层增量归并：先把磁盘上前 `SORTER_MAX_MERGE_COUNT` 个 PMA 的 T 字节数据归并在一起，再处理下一组，如此往复，保证任何一次归并最多只涉及 `SORTER_MAX_MERGE_COUNT` 个 PMA，以改善局部性。

**本机制的边界：** 不负责排序键的表达（键的记录格式在别处，见 `sqlite3VdbeSorterWrite` 的调用方 `OP_SorterInsert` 传入的 `OP_MakeRecord` 产物）、不负责临时文件的 OS 层实现（`vdbeSorterOpenTempFile` 只调 `sqlite3OsOpenMalloc`，真正的文件语义在 VFS 层）。

## 怎么实现的

### 关键常量与结构

`src/vdbesort.c` 顶部的常量：

- `SQLITE_MAX_PMASZ` = `(1<<29)`，即 512 MiB。注释写明：这是「在落盘成 level-0 PMA 之前允许在内存中累积的数据量上限」，目的是防止各种整数溢出。
- `SORTER_MAX_MERGE_COUNT` = **16**。注释写明：「一个单独的 MergeEngine 能归并的最大 PMA 数量」。
- `SORTER_TYPE_INTEGER` = **0x01**、`SORTER_TYPE_TEXT` = **0x02**、`SORTER_TYPE_REAL` = **0x04**。是 `VdbeSorter.typeMask` 的位标志，用来挑选比较函数。
- `SZ_VDBESORTER(N)` = `(offsetof(VdbeSorter,aTask)+(N)*sizeof(SortSubtask))`，用于计算 N 个子任务下的 sorter 对象大小。

`src/global.c` 中 `sqlite3GlobalConfig.szPma` 的默认值来自 `SQLITE_SORTER_PMASZ`（`src/global.c` 里 `#ifndef SQLITE_SORTER_PMASZ` / `# define SQLITE_SORTER_PMASZ 250`）。运行期可由 `sqlite3_config(SQLITE_CONFIG_PMASZ, ...)` 改写（`src/main.c` 中 `sqlite3GlobalConfig.szPma = va_arg(ap, unsigned int);`）。

### `sqlite3VdbeSorterInit`：创建 sorter 对象

签名（`src/vdbesort.c`）：`int sqlite3VdbeSorterInit(sqlite3 *db, int nField, VdbeCursor *pCsr)`。

1. **确定 worker 线程数。** 编译期 `SQLITE_MAX_WORKER_THREADS==0` 时 `nWorker` 直接定义为 0；否则若 `sqlite3TempInMemory(db)` 或 `sqlite3GlobalConfig.bCoreMutex==0`，`nWorker=0`，否则 `nWorker = db->aLimit[SQLITE_LIMIT_WORKER_THREADS]`。
2. **限制线程总数。** 编译期 `SQLITE_MAX_WORKER_THREADS>=SORTER_MAX_MERGE_COUNT` 时，若 `nWorker>=SORTER_MAX_MERGE_COUNT` 则夹到 `SORTER_MAX_MERGE_COUNT-1`（即 15）——注释写明「不允许线程总数（主线程 + 所有 worker）超过最大归并数」。
3. **分配对象。** `sz = SZ_VDBESORTER(nWorker+1)`，用 `sqlite3DbMallocZero` 一次性分配 `sz + szKeyInfo`，`szKeyInfo = SZ_KEYINFO(pCsr->pKeyInfo->nAllField)`。分配失败返回 `SQLITE_NOMEM_BKPT`。
4. **复制 KeyInfo。** `pSorter->pKeyInfo` 指向紧跟在 `VdbeSorter` 之后的内存，`memcpy` 自 `pCsr->pKeyInfo`，并把 `pKeyInfo->db` 置 0。若 `nField && nWorker==0`（单线程模式），把 `pKeyInfo->nKeyField` 收窄为 `nField`。
5. **取页大小。** `sqlite3BtreeEnter(pBt)` → `pgsz = sqlite3BtreeGetPageSize(pBt)` → `sqlite3BtreeLeave(pBt)`，存入 `pSorter->pgsz`。
6. **设置任务数。** `pSorter->nTask = nWorker + 1`（主线程也算一个任务），`pSorter->iPrev = nWorker - 1`，`pSorter->bUseThreads = (pSorter->nTask>1)`。随后循环给 `pSorter->aTask[i]` 的每个 `SortSubtask` 设置 `pTask->pSorter = pSorter`。
7. **计算 PMA 大小阈值（仅当临时存储不落内存时）。** 外层条件是 `if( !sqlite3TempInMemory(db) )`：
   - `pSorter->mnPmaSize = sqlite3GlobalConfig.szPma * pgsz`（即 `SQLITE_SORTER_PMASZ` 默认 250 乘页大小）。
   - `mxCache = db->aDb[0].pSchema->cache_size`；若为负，按注释「负的 cache-size 值 C 表示缓存是 abs(C) KiB」，换算为 `mxCache * -1024`；否则换算为 `mxCache * pgsz`。
   - `mxCache = MIN(mxCache, SQLITE_MAX_PMASZ)`（上限 512 MiB）。
   - `pSorter->mxPmaSize = MAX(pSorter->mnPmaSize, (int)mxCache)`。
8. **预分配首批内存（仅当 `sqlite3GlobalConfig.bSmallMalloc==0`）。** 注释写明「若应用请求了 `SQLITE_CONFIG_SMALL_MALLOC`，避免大块内存分配」，因此此时跳过；否则 `pSorter->nMemory = pgsz`、`pSorter->list.aMemory = sqlite3Malloc(pgsz)`，分配失败返回 `SQLITE_NOMEM_BKPT`。
9. **预判排序键类型。** 条件是 `pKeyInfo->nAllField<13 && (pKeyInfo->aColl[0]==0 || pKeyInfo->aColl[0]==db->pDfltColl) && (pKeyInfo->aSortFlags[0] & KEYINFO_ORDER_BIGNULL)==0`，成立则 `pSorter->typeMask = SORTER_TYPE_INTEGER|SORTER_TYPE_TEXT|SORTER_TYPE_REAL`，即可以走更快的专用比较函数。

### `sqlite3VdbeSorterWrite`：写入一条待排序记录

签名（`src/vdbesort.c`）：`int sqlite3VdbeSorterWrite(const VdbeCursor *pCsr, Mem *pVal)`。

1. **更新类型掩码。** 从 `pVal->z[1]` 读出首字段的 serial type（`getVarint32NR`）：若 `t>0 && t<10`，则 `t==7`（real）时保留 `SORTER_TYPE_REAL`，否则保留 `SORTER_TYPE_INTEGER|SORTER_TYPE_REAL`；若 `t>10 && (t & 0x01)`（文本）则保留 `SORTER_TYPE_TEXT`；其余情况直接 `pSorter->typeMask = 0`。
2. **判断是否需要落盘。** `nReq = pVal->n + sizeof(SorterRecord)`（内存占用）、`nPMA = pVal->n + sqlite3VarintLen(pVal->n)`（PMA 占用）。仅当 `pSorter->mxPmaSize` 非零时才判断：
   - **单块内存模式**（`pSorter->list.aMemory` 非零）：`bFlush = pSorter->iMemory && (pSorter->iMemory+nReq) > pSorter->mxPmaSize`。源码注释写明：「若使用单块大内存分配模式，则在 (a) 内存中已至少有一个值、(b) 新值放不进内存时把内存内容刷成 PMA」。
   - **逐条分配模式**：`bFlush = (pSorter->list.szPMA > pSorter->mxPmaSize) || (pSorter->list.szPMA > pSorter->mnPmaSize && sqlite3HeapNearlyFull())`。注释写明：或「内存中列表占用的总内存超过 (页大小 × 10)」且 `sqlite3HeapNearlyFull()` 返回真时也要刷。
   - 需要刷时调 `vdbeSorterFlushPMA(pSorter)`，随后清零 `pSorter->list.szPMA` 与 `pSorter->iMemory`。
3. **累加统计。** `pSorter->list.szPMA += nPMA`；若 `nPMA>pSorter->mxKeysize` 则更新 `pSorter->mxKeysize`（结构体注释写明这是「目前见过的最大记录在盘上的大小」）。
4. **分配并挂链。**
   - 单块内存模式：按需 `sqlite3Realloc` 扩容，新大小从 `2 * pSorter->nMemory` 起倍增，但不超过 `pSorter->mxPmaSize`，且不小于实际需求 `nMin`；realloc 失败返回 `SQLITE_NOMEM_BKPT`。随后在新块尾部放记录，并用 `u.iNext`（块内偏移）串链——注释说明用偏移而非指针，是因为 `aMemory[]` 扩容时块的首地址会变，块内偏移不受影响。
   - 逐条分配模式：`sqlite3Malloc(nReq)`，失败返回 `SQLITE_NOMEM_BKPT`，用 `u.pNext` 串链。
5. **复制数据。** `memcpy(SRVAL(pNew), pVal->z, pVal->n)`，并置 `pNew->nVal = pVal->n`。

**调用方：** `src/vdbe.c` 的 `OP_SorterInsert` 指令：`rc = sqlite3VdbeSorterWrite(pC, pIn2);`，失败时 `goto abort_due_to_error`。传入的 `pIn2` 已在该指令里经 `ExpandBlob(pIn2)` 展开，且断言 `pIn2->flags & MEM_Blob`。

### `sqlite3VdbeSorterNext`：读出下一条有序记录

签名（`src/vdbesort.c`）：`int sqlite3VdbeSorterNext(sqlite3 *db, const VdbeCursor *pCsr)`。返回 `SQLITE_OK` 表示成功，`SQLITE_DONE` 表示数据结束，其他为错误码。

1. **已落盘（`pSorter->bUsePMA` 为真）时走归并路径。**
   - 多线程模式（`pSorter->bUseThreads`）：`rc = vdbePmaReaderNext(pSorter->pReader)`；若成功且 `pSorter->pReader->pFd==0`（读到 EOF），转为 `SQLITE_DONE`。
   - 单线程模式：`rc = vdbeMergeEngineStep(pSorter->pMerger, &res)`；若成功且 `res` 为真（到 EOF），转为 `SQLITE_DONE`。
2. **未落盘时走内存链表路径。** `pFree = pSorter->list.pList`，把链表头推进到 `pFree->u.pNext`，并清掉 `pFree->u.pNext`；若 `pSorter->list.aMemory==0`（逐条分配模式）则 `vdbeSorterRecordFree(db, pFree)` 释放该条；最后 `rc = pSorter->list.pList ? SQLITE_OK : SQLITE_DONE`。

**调用方：** `src/vdbe.c` 的 `OP_SorterNext` 指令：`rc = sqlite3VdbeSorterNext(db, pC);`。

### `sqlite3VdbeSorterClose`：销毁 sorter 对象

签名（`src/vdbesort.c`）：`void sqlite3VdbeSorterClose(sqlite3 *db, VdbeCursor *pCsr)`。

1. 断言 `pCsr->eCurType==CURTYPE_SORTER`；若 `pSorter` 非空则继续。
2. **统计落盘字节数。** 循环 `for(ii=0; ii<pSorter->nTask; ii++){ db->nSpill += pSorter->aTask[ii].nSpill; }`，注释写明是把「本次排序写入临时文件的总字节数」累加到 `db->nSpill`。
3. 调 `sqlite3VdbeSorterReset(db, pSorter)` 释放所有资源。
4. `sqlite3_free(pSorter->list.aMemory)`、`sqlite3DbFree(db, pSorter)`，并把 `pCsr->uc.pSorter` 置 0。

**调用方：** `src/vdbeaux.c` 中关闭游标时调 `sqlite3VdbeSorterClose(p->db, pCx);`。

### `sqlite3VdbeSorterReset`：复位以备复用

签名（`src/vdbesort.c`）：`void sqlite3VdbeSorterReset(sqlite3 *db, VdbeSorter *pSorter)`。文件头注释写明：「把 VdbeSorter 恢复到可复用状态，类似 Close() 后接 Init()，但快得多。」

它做的是：先 `vdbeSorterJoinAll(pSorter, SQLITE_OK)` 等所有后台线程结束；多线程模式下清 `pSorter->pReader`（`vdbePmaReaderClear` + `sqlite3DbFree`）；`vdbeMergeEngineFree(pSorter->pMerger)` 并置 0；逐个 `vdbeSortSubtaskCleanup(db, pTask)` 释放各子任务的 `UnpackedRecord`、内存列表与两个临时文件句柄（`pTask->file`、`pTask->file2`），再重新设 `pTask->pSorter = pSorter`；最后清 `list.pList`、`list.szPMA`、`bUsePMA`、`iMemory`、`mxKeysize` 与 `pUnpacked`。

**调用方：** `src/vdbe.c` 中重置语句时调 `sqlite3VdbeSorterReset(db, pC->uc.pSorter);`；`sqlite3VdbeSorterClose` 内部也调它。

### 落盘路径：`vdbeSorterFlushPMA` 与 `vdbeSorterListToPMA`

`vdbeSorterFlushPMA(VdbeSorter *pSorter)`（`src/vdbesort.c`）负责把内存列表变成一个 PMA：

- 编译期 `SQLITE_MAX_WORKER_THREADS==0` 时，直接 `pSorter->bUsePMA = 1;` 并调 `vdbeSorterListToPMA(&pSorter->aTask[0], &pSorter->list)`。
- 否则先把 `pSorter->bUsePMA` 置 1，再在 `nWorker` 个后台子任务间轮转挑选一个（`iTest = (pSorter->iPrev + i + 1) % nWorker`），若该子任务的后台线程已结束（`pTask->bDone`）则 `vdbeSorterJoinThread(pTask)` 回收它；若其 `pThread==0` 就选中它。源码注释写明：前 `nWorker` 个子任务优先，因为「它们使用后台线程——最后一个子任务用的是主线程」。
- 若前 `nWorker` 个子任务都还忙（循环结束时 `i==nWorker`），则退回到前台：`vdbeSorterListToPMA(&pSorter->aTask[nWorker], &pSorter->list)`。
- 选中后台任务时，把 `pSorter->list` 整体移交给该任务（`pTask->list = pSorter->list`），把 `pSorter->list.pList` 与 `szPMA` 清零，并调 `vdbeSorterCreateThread(pTask, vdbeSorterFlushThread, pCtx)` 启动后台线程。

`vdbeSorterListToPMA(SortSubtask *pTask, SorterList *pList)` 负责实际的排序与写盘：

- 若 `pTask->file.pFd==0` 则调 `vdbeSorterOpenTempFile(db, 0, &pTask->file.pFd)` 打开临时文件。
- 调 `vdbeSorterExtendFile` 尝试让文件被内存映射。
- 调 `vdbeSorterSort(pTask, pList)` 排序内存列表。
- 写入格式由源码注释给出：先一个 varint 表示 PMA 内容总字节数（不含这个 varint 本身），随后是若干条按升序紧密排列的记录，每条记录由「一个 varint 表示数据字节数」加「数据 blob」构成。
- 写盘用 `vdbePmaWriterInit` / `vdbePmaWriteVarint` / `vdbePmaWriteBlob` / `vdbePmaWriterFinish`。`PmaWriter` 的缓冲大小取 `pSorter->pgsz`，源码注释写明「PMA 逐条记录写入，但按页对齐的块写出 I/O 效率更高」。

### 归并路径：`MergeEngine` 与 `IncrMerger`

`MergeEngine`（`src/vdbesort.c`）是归并核心。源码注释描述其结构：`aReadr[]` 每个元素对应一个被归并的 PMA（要么指向有效键，要么处于 EOF），数组长度 N 取「不小于 PMA 个数的最小 2 的幂」；`aTree[]` 同样长 N，`aTree[1]` 保存当前最小键所在的 `PmaReader` 索引，`aTree[0]` 不使用。注释给出的例子说明 `aTree[] = { X, 5, 0, 5, 0, 3, 5, 6 }` 对应当前最小键是 "Apple"（索引 5）。注释结论：每次前进需要 `log2(N)` 次键比较。

`vdbeMergeEngineStep(MergeEngine *pMerger, int *pbEof)` 做单步推进：先 `vdbePmaReaderNext(&pMerger->aReadr[iPrev])` 推进当前 reader，再从 `iPrev` 与其兄弟节点开始，沿 `i=(pMerger->nTree+iPrev)/2` 向上逐层更新 `aTree[]`；比较时把 EOF 视为大于任何有效键（`pReadr1->pFd==0` 时 `iRes=+1`，`pReadr2->pFd==0` 时 `iRes=-1`）。

`vdbePmaReaderNext(PmaReader *pReadr)` 是单条记录的读取：若已读到 `pReadr->iEof`，且该 reader 挂了 `pIncr`（增量归并器），则调 `vdbeIncrSwap(pIncr)` 换文件；换成功后 `vdbePmaReaderSeek` 定位到新文件开头；仍无数据则 `vdbePmaReaderClear(pReadr)` 并返回 EOF。否则 `vdbePmaReadVarint` 读出记录长度、`vdbePmaReadBlob` 读出数据。

`IncrMerger`（`src/vdbesort.c`）分两种：单线程版（`bUseThread==0`）不另开文件，直接复用 `pTask->file2` 的空间；多线程版用两个临时文件 `aFile[0]`（读）与 `aFile[1]`（写），且源码注释写明「两个文件都不允许增长到超过 mxSz 字节」——`aFile[0]` 读完后阻塞等后台线程填好 `aFile[1]`，两者交换内容，再启动新后台线程填新的 `aFile[1]`。

### 分层归并的规模控制

`vdbeSorterMergeTreeBuild(VdbeSorter *pSorter, MergeEngine **ppOut)` 构建归并树：

- 若 `pSorter->nTask>1`（多线程），先建一个顶层 `MergeEngine`，它从每个子任务各取一个 `PmaReader`。
- 每个子任务内部：若 `pTask->nPMA<=SORTER_MAX_MERGE_COUNT`（即 16），直接 `vdbeMergeEngineLevel0(pTask, pTask->nPMA, &iReadOff, &pRoot)` 一把梭；否则建一个容量为 `SORTER_MAX_MERGE_COUNT` 的根 `MergeEngine`，再按每组 `SORTER_MAX_MERGE_COUNT` 个 PMA 建一个 level-0 归并器（`nReader = MIN(pTask->nPMA - i, SORTER_MAX_MERGE_COUNT)`），逐个 `vdbeSorterAddToTree` 挂到根上。
- `vdbeSorterTreeDepth(int nPMA)` 给出树深，源码注释列出：`nPMA<=16` 深度 0，`nPMA<=256` 深度 1，`nPMA<=65536` 深度 2。

### `sqlite3VdbeSorterRewind`：排序完成、准备读出

`sqlite3VdbeSorterRewind(const VdbeCursor *pCsr, int *pbEof)`（`src/vdbesort.c`）是排序与读取的转折点：

- 若 `pSorter->bUsePMA==0`（从未落盘）：直接 `vdbeSorterSort(&pSorter->aTask[0], &pSorter->list)` 做内存排序，`*pbEof = 0`；若列表为空则 `*pbEof = 1`。源码注释写明：「若没有数据写到磁盘，现在也不写。改为排序 VdbeSorter.pRecord 列表，vdbe 层直接从内存列表读数据。」此路径不产生临时文件。
- 否则：`vdbeSorterFlushPMA(pSorter)` 把残余内存列表落成最后一个 PMA（源码注释解释：因为 `sqlite3VdbeSorterWrite` 每次刷盘后都会立即建立一个只含一条新键的列表，所以此��列表永不为空），然后 `vdbeSorterJoinAll` 等所有后台线程，最后 `vdbeSorterSetupMerge(pSorter)` 建立归并结构。

**调用方：** `src/vdbe.c` 的 `OP_SorterSort` 指令：`rc = sqlite3VdbeSorterRewind(pC, &res);`。

### `sqlite3VdbeSorterRowkey` 与 `sqlite3VdbeSorterCompare`

`sqlite3VdbeSorterRowkey(const VdbeCursor *pCsr, Mem *pOut)` 把当前键复制到 `pOut`：内部 `vdbeSorterRowkey` 在已落盘时取 `pSorter->pReader`（多线程）或 `pSorter->pMerger->aReadr[pSorter->pMerger->aTree[1]]`（单线程），未落盘时取 `pSorter->list.pList`；随后 `sqlite3VdbeMemClearAndResize(pOut, nKey)`（失败返回 `SQLITE_NOMEM_BKPT`）、置 `MEM_Blob` 类型标志并 memcpy。

`sqlite3VdbeSorterCompare(const VdbeCursor *pCsr, Mem *pVal, int nKeyCol, int *pRes)` 用于唯一性校验（对应 `OP_SorterCompare` 指令）。它用 `sqlite3VdbeAllocUnpackedRecord(pKeyInfo)` 分配解包记录（失败返回 `SQLITE_NOMEM_BKPT`），`sqlite3VdbeRecordUnpack` 解包当前键，然后逐列比较；源码注释写明「若 sorter 游标的键含任何 NULL 值，就认为它小于 pVal，即使 pVal 也含 NULL 值」——对应代码 `if( r2->aMem[i].flags & MEM_Null ){ *pRes = -1; return SQLITE_OK; }`。

### 比较函数的挑选

`vdbeSorterGetCompare(VdbeSorter *p)`（`src/vdbesort.c`）按 `typeMask` 依次返回：`SORTER_TYPE_INTEGER` → `vdbeSorterCompareInt`；否则 `SORTER_TYPE_TEXT` → `vdbeSorterCompareText`；否则 `SORTER_TYPE_REAL` → `vdbeSorterCompareReal`；都没有则退回通用的 `vdbeSorterCompare`。

### 临时文件的打开

`vdbeSorterOpenTempFile(sqlite3 *db, i64 nExtend, sqlite3_file **ppFd)`（`src/vdbesort.c`）调 `sqlite3OsOpenMalloc(db->pVfs, 0, ppFd, SQLITE_OPEN_TEMP_JOURNAL | SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE | SQLITE_OPEN_EXCLUSIVE | SQLITE_OPEN_DELETEONCLOSE, &rc)`。`SQLITE_OPEN_DELETEONCLOSE` 意味着临时文件在关闭时被自动删除。打开后用 `SQLITE_FCNTL_MMAP_SIZE` file control 把 mmap 上限设为 `SQLITE_MAX_MMAP_SIZE`。

## 关键取舍

- **内存优先，落盘兜底。** 排序数据先全部放内存，只有当占用超过 `mxPmaSize` 才落盘；且落盘后 `sqlite3VdbeSorterRewind` 依然会把残余内存刷成最后一个 PMA（而不是直接丢弃）。**架构推论：** 「内存排序」与「外排归并」共用同一套 `sqlite3VdbeSorterWrite`/`sqlite3VdbeSorterNext` 接口，只由 `pSorter->bUsePMA` 这一个布尔位分流，代价是 `Next()` 必须同时维护两条推进路径。
- **阈值跟随页缓存配置。** `mxPmaSize` 由 `db->aDb[0].pSchema->cache_size` × 页大小算出，并用 `MAX(mnPmaSize, mxCache)` 保证不低于 `szPma * pgsz`（默认 `SQLITE_SORTER_PMASZ` 250 页）。**架构推论：** 把排序器的内存预算与页缓存预算绑定，意味着用户调 `PRAGMA cache_size` 会同时改变排序器的落盘时机，两者共享同一块内存预算，避免两个子系统各自吃一份内存。
- **硬上限 512 MiB。** `SQLITE_MAX_PMASZ` = `(1<<29)`，`mxCache` 被 `MIN(mxCache, SQLITE_MAX_PMASZ)` 夹住。源码注释写明这个常量的目的是「防止各种整数溢出」——即它是溢出保护，不是性能策略。
- **一次归并最多 16 个 PMA。** `SORTER_MAX_MERGE_COUNT` = 16，超过就分层（`vdbeSorterTreeDepth` 给出 16 / 256 / 65536 对应深度 0 / 1 / 2）。**架构推论：** 限制扇出是为了改善磁盘局部性——每层只读相邻的一组 PMA，代价是 PMA 极多时要多走一层归并。
- **线程数受归并上限反约束。** `sqlite3VdbeSorterInit` 中当编译期 `SQLITE_MAX_WORKER_THREADS>=SORTER_MAX_MERGE_COUNT` 时，`nWorker` 被夹到 `SORTER_MAX_MERGE_COUNT-1`（15）。**架构推论：** 这是让「一个 MergeEngine 能吃下的 reader 数」与「可用的 worker 数」保持一致，避免线程数超过归并树的扇出能力而浪费资源。
- **临时存储在内存时禁用多线程。** `sqlite3VdbeSorterInit` 中 `sqlite3TempInMemory(db)` 为真时 `nWorker=0`；且 `if( !sqlite3TempInMemory(db) )` 才计算 PMA 阈值与预分配内存块。**架构推论：** 当 `PRAGMA temp_store` 指向内存时，排序器的所有临时文件实际都在内存里，再开后台线程只会增加调度开销而没有 I/O 可并行，代价是内存模式下排序速度低于文件模式。
- **`SQLITE_CONFIG_SMALL_MALLOC` 下放弃大块预分配。** `sqlite3GlobalConfig.bSmallMalloc!=0` 时不预分配 `pgsz` 大小的 `list.aMemory`，改走逐条 `sqlite3Malloc` 的模式。**架构推论：** 这是给内存受限嵌入式场景准备的开关，代价是逐条分配路径的比较与刷盘条件与单块内存路径不同（`bFlush` 判定条件不同），两条路径要各自维护。
- **堆接近满时提前落盘。** 逐条分配模式下，除了 `list.szPMA > mxPmaSize`，还有 `list.szPMA > mnPmaSize && sqlite3HeapNearlyFull()` 也触发刷盘。**架构推论：** 这让排序器在系统内存紧张时主动降级到外排，避免排序阶段把整个进程撑爆，代价是刷盘时机依赖全局堆状态而非纯本地阈值。
- **比较函数按数据实际类型收敛。** `typeMask` 初始为 `INTEGER|TEXT|REAL`（当字段数小于 13、首个排序键用默认排序规则、且没有 `KEYINFO_ORDER_BIGNULL` 时），每写一条记录就按首字段 serial type 收窄掩码，最后由 `vdbeSorterGetCompare` 选定专用比较函数。**架构推论：** 一旦确认全部记录都是整数，就换成整数专用比较，省掉通用比较的解包与类型分派开销，代价是必须先写够样本才能确定，且一旦收窄就不可逆。
- **排序键的 NULL 语义被显式规定。** `sqlite3VdbeSorterCompare` 中遇到含 NULL 的键一律判为「小于」比较对象（`*pRes = -1`），源码注释写明「即使 pVal 也含 NULL 值」。**架构推论：** 这是为 UNIQUE 索引校验定下的确定性规则，避免 NULL 参与唯一性判断时出现未定义顺序。
- **spill 字节数被统计到连接上。** `sqlite3VdbeSorterClose` 把各子任务的 `nSpill` 累加到 `db->nSpill`。**架构推论：** 这是给应用观测排序临时空间用量的出口（配合 `sqlite3_status` 的 spill 计数），代价是统计值只有关闭游标时才更新。
- **后台线程不做内存屏障同步。** `SortSubtask` 的注释写明：「主线程即使在线程结束后仍看到 `bDone==0` 也不会有严重后果，所以我们不操心这里的内存屏障之类的事。」**架构推论：** 这是用「良性竞态」换掉屏障开销——`bDone` 只用于挑选可用子任务，不承担正确性保证，代价是线程完成状态的可见性是最终一致的。

## 与主核 / AI 的关系

主核 `engine/db/` 是从零手写的多模态数据库内核，与 SQLite 的 `VdbeSorter` 没有直接的代码对应关系——主核目前没有实现 ORDER BY 的外排路径。

**架构推论：** 若主核要在查询层支持大结果集排序，`src/vdbesort.c` 里最值得借鉴的是「单接口 + 单一布尔位分流」的结构：`sqlite3VdbeSorterWrite` / `sqlite3VdbeSorterNext` 对内存排序与外排归并是同一套接口，`pSorter->bUsePMA` 决定走哪条路径。主核若为两条路径各写一套接口，调用方就要同时处理两套生命周期（Init/Write/Rewind/Next/Close 的顺序约束会分叉成两份）。

**架构推论：** PMA 格式的设计（长度 varint + 紧凑排列的记录）说明「排序中间结果」是可以自带结构的，不一定需要走通用的表/行格式。主核若为向量检索的 top-k 或大结果集排序设计落盘中间格式，这种「自描述的紧凑块」比借用通用序列化更省空间与解析开销。

**架构推论：** `sqlite3VdbeSorterInit` 里「排序器的内存预算与页缓存预算绑定」（`cache_size` × 页大小）是一条值得主核参考的资源治理原则：让查询层的临时内存开销与存储层的缓存开销共用一个可配置预算，避免两条线各自膨胀。

**架构推论：** `sqlite3VdbeSorterCompare` 对 NULL 一律判小的规则说明排序键的 NULL 语义必须由「排序器」而非「表达式求值」统一裁定。主核在多模态场景下混排标量与向量得分时，同样需要一个明确的空值序，否则不同算子给出的 NULL 位置会互相矛盾。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/vdbesort.c | `sqlite3VdbeSorterInit` | 创建 sorter：定 worker 线程数（夹到 `SORTER_MAX_MERGE_COUNT-1`）、分配 `VdbeSorter` 与 `KeyInfo`、取页大小、按 `cache_size`×页大小算出 `mxPmaSize`、预判 `typeMask` |
| src/vdbesort.c | `sqlite3VdbeSorterWrite` | 写入一条记录：按 serial type 收窄 `typeMask`、按 `mxPmaSize`/`sqlite3HeapNearlyFull` 判定是否刷 PMA、单块内存或逐条分配两种挂链方式 |
| src/vdbesort.c | `sqlite3VdbeSorterNext` | 读出下一条：已落盘时走 `vdbePmaReaderNext`（多线程）或 `vdbeMergeEngineStep`（单线程），未落盘时推进内存链表；末尾返回 `SQLITE_DONE` |
| src/vdbesort.c | `sqlite3VdbeSorterClose` | 销毁 sorter：把各子任务 `nSpill` 累加到 `db->nSpill`，调 `sqlite3VdbeSorterReset` 后释放 `list.aMemory` 与 `VdbeSorter` 本体 |
| src/vdbesort.c | `sqlite3VdbeSorterReset` | 复位复用：等所有后台线程、释放 `pReader`/`pMerger`/各子任务资源、清空统计字段 |
| src/vdbesort.c | `sqlite3VdbeSorterRewind` | 排序→读取的转折点：未落盘则纯内存排序，已落盘则刷最后一个 PMA、等线程、建归并结构 |
| src/vdbesort.c | `sqlite3VdbeSorterRowkey` | 取出当前键复制到 `Mem`（置 `MEM_Blob`），内存模式与 PMA 模式分别取不同来源 |
| src/vdbesort.c | `sqlite3VdbeSorterCompare` | 当前键与 `pVal` 比较，忽略尾部 rowid；键含 NULL 一律判小（`*pRes=-1`）；`OP_SorterCompare` 的实现基础 |
| src/vdbesort.c | `vdbeSorterFlushPMA` | 把内存列表变成 PMA：在 worker 子任务间轮转挑选，全忙时退回前台主线程，否则启后台线程 |
| src/vdbesort.c | `vdbeSorterListToPMA` | 实际排序并按 PMA 格式写盘（长度 varint + 逐条「长度 varint + 数据」），用 `PmaWriter` 做页对齐缓冲 |
| src/vdbesort.c | `vdbeSorterSort` | 内存归并排序：用 `aSlot[64]` 做倍增式归并，末尾统一归并回单链表 |
| src/vdbesort.c | `vdbeSorterGetCompare` | 按 `typeMask` 选比较函数：整数 → `vdbeSorterCompareInt`，文本 → `vdbeSorterCompareText`，实数 → `vdbeSorterCompareReal`，否则通用比较 |
| src/vdbesort.c | `MergeEngine` / `vdbeMergeEngineStep` / `vdbeMergeEngineNew` | 归并树节点与单步推进：`aTree[1]` 存当前最小键索引，EOF 视为大于任何键，每步需 `log2(N)` 次比较 |
| src/vdbesort.c | `vdbePmaReaderNext` / `vdbePmaReaderInit` / `vdbePmaReaderClear` | PMA 读取器：读长度 varint 与数据；到 EOF 时借助 `pIncr` 换文件续读 |
| src/vdbesort.c | `IncrMerger` / `vdbeIncrPopulate` / `vdbeIncrSwap` | 增量归并器：多线程版用 `aFile[0]`/`aFile[1]` 两个不超过 `mxSz` 的临时文件交替，单线程版复用 `pTask->file2` 空间 |
| src/vdbesort.c | `vdbeSorterMergeTreeBuild` / `vdbeMergeEngineLevel0` / `vdbeSorterTreeDepth` | 建归并树：PMA 数不超过 16 时单层归并，否则按每组 16 个建 level-0 归并器并挂成多层树 |
| src/vdbesort.c | `vdbeSorterOpenTempFile` | 打开临时文件：`SQLITE_OPEN_TEMP_JOURNAL|READWRITE|CREATE|EXCLUSIVE|DELETEONCLOSE`，并设置 mmap 上限 |
| src/vdbesort.c | `SORTER_MAX_MERGE_COUNT` | 宏，取值 16；单个 `MergeEngine` 能归并的最大 PMA 数，也是 PMA 归并树的扇出上限 |
| src/vdbesort.c | `SQLITE_MAX_PMASZ` | 宏，取值 `(1<<29)` 即 512 MiB；`mxCache` 的硬上限，用于防止整数溢出 |
| src/vdbesort.c | `SORTER_TYPE_INTEGER` / `SORTER_TYPE_TEXT` / `SORTER_TYPE_REAL` | 宏，取值 0x01 / 0x02 / 0x04；`VdbeSorter.typeMask` 的位标志，用于挑选专用比较函数 |
| src/vdbesort.c | `SZ_VDBESORTER(N)` | 宏；N 个子任务下 `VdbeSorter` 对象的字节大小 |
| src/global.c | `SQLITE_SORTER_PMASZ` | 宏，默认 250；`sqlite3GlobalConfig.szPma` 的初值，`mnPmaSize = szPma * pgsz` 的来源 |
| src/global.c | `sqlite3GlobalConfig.szPma` | 全局配置字段；运行期可由 `sqlite3_config(SQLITE_CONFIG_PMASZ, ...)` 覆盖 |
| src/main.c | `sqlite3_config`（`SQLITE_CONFIG_PMASZ` 分支） | 写 `sqlite3GlobalConfig.szPma = va_arg(ap, unsigned int);`，是 PMA 大小的运行期调节口 |
| src/main.c | `sqlite3TempInMemory` | 判定临时存储是否在内存；sorter 用它决定是否禁用多线程、是否计算 PMA 阈值与预分配内存 |
| src/sqlite.h.in | `SQLITE_CONFIG_PMASZ` | 宏，取值 25；`sqlite3_config` 中设置 PMA 大小的配置选项编号 |
| src/sqliteInt.h | `db->aLimit[SQLITE_LIMIT_WORKER_THREADS]` | 连接级 worker 线程上限；`sqlite3VdbeSorterInit` 用它确定 `nWorker` |
| src/vdbe.c | `OP_SorterOpen` | 指令实现：`allocateCursor(..., CURTYPE_SORTER)` 后调 `sqlite3VdbeSorterInit(db, pOp->p3, pCx)` |
| src/vdbe.c | `OP_SorterInsert` | 指令实现：`ExpandBlob(pIn2)` 后调 `sqlite3VdbeSorterWrite(pC, pIn2)`，失败 `goto abort_due_to_error` |
| src/vdbe.c | `OP_SorterSort` | 指令实现：调 `sqlite3VdbeSorterRewind(pC, &res)`，触发排序与归并结构建立 |
| src/vdbe.c | `OP_SorterNext` | 指令实现：调 `sqlite3VdbeSorterNext(db, pC)` |
| src/vdbe.c | `OP_SorterCompare` / `OP_SorterData` | 指令实现：分别调 `sqlite3VdbeSorterCompare` 与 `sqlite3VdbeSorterRowkey` |
| src/vdbe.c | `sqlite3VdbeSorterReset` 调用点 | 语句重置时把 sorter 复位以备复用 |
| src/vdbeaux.c | `sqlite3VdbeSorterClose` 调用点 | 关闭游标时释放 sorter 对象 |

## 参考

- [The SQLite Bytecode Engine（sqlite.org 官方文档）](https://sqlite.org/opcode.html) —— 说明 VDBE 指令体系与 `OP_SorterOpen` / `OP_SorterInsert` / `OP_SorterSort` / `OP_SorterNext` / `OP_SorterCompare` / `OP_SorterData` 这组排序器指令在字节码中的位置。
- [PRAGMA temp_store（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_temp_store) —— 说明 `PRAGMA temp_store` 的取值语义，它直接决定 `sqlite3TempInMemory()` 的返回值，进而决定 sorter 是否启用后台线程与 PMA 阈值计算。
- [PRAGMA cache_size（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_cache_size) —— 说明 `cache_size` 的负值表示 KiB 的约定，这正是 `sqlite3VdbeSorterInit` 里 `mxCache = mxCache * -1024` 分支所依赖的语义。
- [PRAGMA threads（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_threads) —— 说明运行时启用 worker 线程的方式，这是 sorter 进入多线程模式的第二个前提。
- [sqlite/sqlite 仓库中的 src/vdbesort.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbesort.c) —— 支撑本页关于 `sqlite3VdbeSorterInit` / `sqlite3VdbeSorterWrite` / `sqlite3VdbeSorterNext` / `sqlite3VdbeSorterClose` 及 PMA、MergeEngine、IncrMerger 的行为描述。
- [sqlite/sqlite 仓库中的 src/vdbe.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— 支撑本页关于 `OP_SorterOpen` / `OP_SorterInsert` / `OP_SorterSort` / `OP_SorterNext` 等指令与 sorter 接口的调用关系描述。
