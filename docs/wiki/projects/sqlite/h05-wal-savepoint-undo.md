---
project: sqlite
feature: wal-savepoint-undo
page_type: feature
schema_version: 1
refs:
  - path: src/wal.c
    symbols: [sqlite3WalSavepoint, sqlite3WalSavepointUndo, sqlite3WalUndo]
---

# WAL 保存点与撤销

## 一句话职责

在 WAL 模式下，把「写事务推进到了 WAL 的第几帧」这一位置记成保存点，并在 `ROLLBACK TO SAVEPOINT` 时把 WAL 的写指针退回到该位置，使保存点之后写入的帧不再可见。

## 机制是什么

WAL 是只追加的日志：写事务把被修改的页一帧一帧追加到 WAL 尾部，事务提交时才追加一条提交记录。写事务因此天然是「线性增长」的，事务中已写入的帧位于 WAL 文件的固定偏移上。

保存点要解决的是**部分回滚**：`ROLLBACK TO SAVEPOINT sp` 要求撤销 `sp` 之后的所有修改，但**不结束事务**（事务仍在进行，可以继续写入并最终提交）。在回滚日志模式下这是靠 sub-journal 里的「页原始内容副本」实现的——每个保存点记录哪些页被改过，回滚时把原始内容回放回去。

WAL 模式没有 sub-journal，因为 WAL 里的帧本身就是新内容，旧内容在数据库文件里（尚未被 checkpoint 时）。因此 WAL 模式的保存点回滚走的是另一条路：**不回放内容，而是回退写指针**。把 WAL 的写指针退回到保存点记录的帧号之后，保存点之后追加的那些帧就退出了本事务的可见范围，后续写入会覆盖掉它们的位置。

这个"截断"是逻辑性的，不是文件层面的 `ftruncate`：WAL 文件的字节长度不变，只是 `Wal.hdr.mxFrame`（当前有效帧数）被改小。

WAL 模式下有两个层次不同的"撤销"：

- **保存点撤销**（`sqlite3WalSavepointUndo`）：退回到某个保存点，事务继续。
- **整事务撤销**（`sqlite3WalUndo`）：把本写事务写入的全部未提交帧作废，事务结束。

## 怎么实现的

### 保存点上下文的采集：`sqlite3WalSavepoint`

`src/wal.c` 的 `sqlite3WalSavepoint(Wal *pWal, u32 *aWalData)` 把当前写位置打包进调用方提供的数组，共 `WAL_SAVEPOINT_NDATA` 个 `u32`（该常量定义在 `src/wal.h`，值为 4）：

| 下标 | 内容 |
|---|---|
| `aWalData[0]` | `pWal->hdr.mxFrame`，当前 WAL 的帧数 |
| `aWalData[1]` | `pWal->hdr.aFrameCksum[0]`，帧校验和第一个分量 |
| `aWalData[2]` | `pWal->hdr.aFrameCksum[1]`，帧校验和第二个分量 |
| `aWalData[3]` | `pWal->nCkpt`，checkpoint 序号 |

函数体开头是 `assert( pWal->writeLock )`——必须在持有写锁时采集，否则采集到的写位置没有意义。

调用方是 `src/pager.c` 的 `pagerOpenSavepoint`：在 `if( pagerUseWal(pPager) )` 分支里调用 `sqlite3WalSavepoint(pPager->pWal, aNew[ii].aWalData)`，把上下文写进 `PagerSavepoint.aWalData`（该字段是 `u32 aWalData[WAL_SAVEPOINT_NDATA]`，仅在 `#ifndef SQLITE_OMIT_WAL` 下存在）。

### 保存点回滚：`sqlite3WalSavepointUndo`

`src/wal.c` 的 `sqlite3WalSavepointUndo(Wal *pWal, u32 *aWalData)` 做三件事：

**第一步，处理 WAL 被重置的情况。** 函数体里的判断是 `if( aWalData[3]!=pWal->nCkpt )`。`nCkpt` 是 WAL 头里的 checkpoint 序号，每次 WAL 从头重写（`walRestartHdr`）就 `pWal->nCkpt++`。保存点采集之后、撤销之前如果发生过 WAL 重置，`nCkpt` 就变了，此时保存点记录的帧号已无意义——源码注释写明这种情况是「保存点紧接着写事务开始后打开，随后写者决定回绕到日志开头」。处理方式是把 `aWalData[0]` 置 0、`aWalData[3]` 置为当前的 `pWal->nCkpt`，即认定「保存点之后没有任何有效帧」。

**第二步，回退写指针。** 判断是 `if( aWalData[0]<pWal->hdr.mxFrame )`，即只有当保存点记录的帧数**小于**当前帧数时才需要动。此时执行：

```c
pWal->hdr.mxFrame = aWalData[0];
pWal->hdr.aFrameCksum[0] = aWalData[1];
pWal->hdr.aFrameCksum[1] = aWalData[2];
```

帧校验和必须一并回退，因为校验和是**链式**的：`walWriteToLog` 计算第 N 帧的校验和时以第 N-1 帧的校验和为输入，只改 `mxFrame` 而不改 `aFrameCksum`，后续帧的校验和就对不上了。

**第三步，清理 wal-index 哈希表。** 紧跟着调用 `walCleanupHash(pWal)`。该函数把哈希表中指向「帧号大于 `pWal->hdr.mxFrame`」的条目清零（判断 `if( sLoc.aHash[i]>iLimit )`），并清掉页号数组中对应条目。`walCleanupHash` 只处理 `mxFrame` 所在的那一张哈希表，更靠后的哈希表会在 `mxFrame` 重新推进到那个位置时自动重建。

**第四步，修正校验和重算起点。** 判断 `if( pWal->iReCksum>pWal->hdr.mxFrame )` 则 `pWal->iReCksum = 0`。`iReCksum` 的语义是「提交时从第几帧开始重新计算校验和」（见 `Wal.iReCksum` 的字段注释 `On commit, recalculate checksums from here`）。回退后如果该起点已超出新的 `mxFrame`，就必须清零，让下次提交从头重算。

**错误分支：** `walCleanupHash` 的调用被包在 `SEH_TRY { ... } SEH_EXCEPT( rc = SQLITE_IOERR_IN_PAGE; )` 里——Windows 的结构化异常处理下，共享内存（wal-index）访问越界会被转成 `SQLITE_IOERR_IN_PAGE` 返回。

函数开头的断言 `assert( aWalData[3]!=pWal->nCkpt || aWalData[0]<=pWal->hdr.mxFrame )` 明确了不变量：如果 `nCkpt` 没变（没有发生 WAL 重置），那么保存点帧号必须不大于当前帧号。

### 整事务撤销：`sqlite3WalUndo`

`src/wal.c` 的 `sqlite3WalUndo(Wal *pWal, int (*xUndo)(void *, Pgno), void *pUndoCtx)` 处理「写事务整体回滚」：

1. 入口判断 `if( ALWAYS(pWal->writeLock) )`——必须持有写锁。
2. 记下 `Pgno iMax = pWal->hdr.mxFrame`，即本事务写入的最后一帧。
3. `memcpy(&pWal->hdr, (void *)walIndexHdr(pWal), sizeof(WalIndexHdr))`：把内存里缓存的 wal-index 头**整个替换回**共享内存里的当前值。这一步是整个撤销的基石——共享内存里的头才是权威值，内存里的 `pWal->hdr` 可能因为本事务的写入而领先。
4. 循环 `for(iFrame=pWal->hdr.mxFrame+1; rc==SQLITE_OK && iFrame<=iMax; iFrame++)`，对每个被撤销的帧调用 `xUndo(pUndoCtx, walFramePgno(pWal, iFrame))`，把该页从页缓存里踢掉或重新从数据库文件读回。帧对应的页号由 `walFramePgno` 从 wal-index 的页号数组里取出。
5. 循环结束后 `if( iMax!=pWal->hdr.mxFrame ) walCleanupHash(pWal);`：撤销了帧就必须清理哈希表。
6. `pWal->iReCksum = 0;`——撤销后校验和重算起点失效。

源码注释给出了一条关键不变量：`assert( walFramePgno(pWal, iFrame)!=1 )`。第 1 页（数据库头页）在事务提交前从不写入 WAL，所以撤销循环里不会碰到它；而回滚时「除第 1 页外没有其他页有未释放的引用」，因此 `xUndo` 不会失败。

`xUndo` 回调的实际实现是 `src/pager.c` 的 `pagerUndoCallback(void *pCtx, Pgno iPg)`：它 `sqlite3PagerLookup` 查该页，若引用计数为 1 则 `sqlite3PcacheDrop` 丢弃缓存页，否则 `readDbPage` 从数据库文件重新读入内容并 `pPager->xReiniter(pPg)` 重新初始化。

`sqlite3WalUndo` 的实际调用方是 `src/pager.c` 的 `pagerRollbackWal(Pager *pPager)`：`rc = sqlite3WalUndo(pPager->pWal, pagerUndoCallback, (void *)pPager);`，之后遍历 `sqlite3PcacheDirtyList(pPager->pPCache)` 返回的脏页链表，对每个脏页再调一次 `pagerUndoCallback`。`pagerRollbackWal` 又由 `pagerPlaybackSavepoint` 在「无保存点且处于 WAL 模式」的分支里调用（`if( !pSavepoint && pagerUseWal(pPager) ) return pagerRollbackWal(pPager);`），而 `pagerPlaybackSavepoint` 的调用方是 `sqlite3PagerSavepoint` 的 `SAVEPOINT_ROLLBACK` 分支。

### 从 SQL 到 WAL 层的完整链路

- `src/vdbe.c` 的 `OP_Savepoint` 指令处理 `SAVEPOINT` / `RELEASE` / `ROLLBACK TO` 三种操作。
- `src/btree.c` 的 `sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint)` 是 Btree 层的入口，`op` 只能是 `SAVEPOINT_ROLLBACK` 或 `SAVEPOINT_RELEASE`；它先调 `saveAllCursors(pBt, 0, 0)`，再调 `sqlite3PagerSavepoint(pBt->pPager, op, iSavepoint)`。
- `src/pager.c` 的 `sqlite3PagerSavepoint` 计算回滚后应保留的保存点个数 `nNew = iSavepoint + (( op==SAVEPOINT_RELEASE ) ? 0 : 1);`，销毁多余的保存点，然后在 `else if( pagerUseWal(pPager) || isOpen(pPager->jfd) )` 分支里调 `pagerPlaybackSavepoint(pPager, pSavepoint)`。
- `src/pager.c` 的 `pagerPlaybackSavepoint` 在处理子日志回放的最后阶段，于 `if( pagerUseWal(pPager) )` 分支里调用 `rc = sqlite3WalSavepointUndo(pPager->pWal, pSavepoint->aWalData);`——**这是 `sqlite3WalSavepointUndo` 的唯一调用点**。同一分支里紧接着还会继续回放子日志中该保存点之后的记录（`for(ii=pSavepoint->iSubRec; rc==SQLITE_OK && ii<pPager->nSubRec; ii++)`），所以 WAL 模式的保存点回滚实际是「WAL 写指针回退」+「子日志回放」两件事的叠加。
- 整事务回滚另有路径：`src/pager.c` 的 `sqlite3PagerRollback(Pager *pPager)` 在 `rc = sqlite3PagerSavepoint(pPager, SAVEPOINT_ROLLBACK, -1);` 处传入 `iSavepoint = -1`，表示撤销整个事务。

## 关键取舍

**取舍一：用「退指针」代替「回放内容」。** 回滚日志模式下保存点回滚要读取并写回原始页内容；WAL 模式只改几个内存字段。代价是：WAL 模式下一个保存点之内的修改**不能被回滚到任意更早的保存点后再恢复**——被撤销的帧在 WAL 文件里仍然占着字节位置，但已经不可见，后续写入会覆盖它。

**取舍二：保存点上下文只有 4 个 u32。** `WAL_SAVEPOINT_NDATA` 定义为 4，保存点不记录「哪些页被改过」这样的集合信息（对比：`PagerSavepoint.pInSavepoint` 是记录页集合的位图）。WAL 模式省掉了位图，代价是无法像子日志那样逐页回放，只能整体回退写指针。

**取舍三：`nCkpt` 作为 WAL 重置的失效标记。** 保存点只存了帧号和校验和，没存别的失效条件。源码用「`nCkpt` 是否变化」来判断 WAL 是否被重置过，这是 `walRestartHdr` 里 `pWal->nCkpt++` 这一个自增所支撑的隐式协议——**架构推论：** WAL 重置与保存点失效之间没有显式契约，靠 `nCkpt` 单调递增这一间接信号来推断；这使得任何绕过 `walRestartHdr` 直接改写 WAL 头的路径都会破坏这个推断。

**取舍四：`iReCksum` 清零而非重算。** 回退写指针后，校验和链的起点可能失效。源码选择在 `iReCksum>pWal->hdr.mxFrame` 时直接清零，让提交时从头重算，代价是重算开销（要读回并重写所有帧的校验和），换来的是实现简单、无需反向计算校验和。

**取舍五：撤销路径依赖「第 1 页不写入 WAL」这一不变量。** `sqlite3WalUndo` 的循环里 `assert( walFramePgno(pWal, iFrame)!=1 )`，注释明确说「页 1 在事务提交前从不会写入日志」，因此 `xUndo` 不会失败。这个不变量同时支撑了 `sqlite3WalUndo` 返回 `SQLITE_OK` 而不返回 `SQLITE_DONE` 的语义。

**取舍六：WAL 写入位置保存在内存副本里，但撤销时以共享内存为准。** `sqlite3WalUndo` 用 `memcpy` 把 `pWal->hdr` 整个覆盖回 `walIndexHdr(pWal)` 的值。注释在 `walRestartLog` 里解释了理由：重启 WAL 时虽然只改内存里的头就够，但把真实的 wal-index 头也一起改掉，可以避免 `sqlite3WalUndo` 需要为「回滚」这个特例写额外分支。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/wal.c | `sqlite3WalSavepoint` | 采集保存点上下文：`mxFrame`、两个 `aFrameCksum` 分量、`nCkpt`，共 `WAL_SAVEPOINT_NDATA`（4）个 u32 |
| src/wal.c | `sqlite3WalSavepointUndo` | 把 WAL 写指针退回保存点记录的帧号，同步回退帧校验和，调用 `walCleanupHash` 清理哈希表，必要时清零 `iReCksum` |
| src/wal.c | `sqlite3WalUndo` | 整事务撤销：把 `pWal->hdr` 覆盖回 `walIndexHdr(pWal)`，逐帧调用 `xUndo` 回调清理页缓存，清零 `iReCksum` |
| src/wal.c | `walCleanupHash` | 清理 wal-index 哈希表中指向帧号大于 `mxFrame` 的条目 |
| src/wal.c | `walFramePgno` | 由帧号取出对应的数据库页号 |
| src/wal.c | `walRestartHdr` | WAL 重置时 `pWal->nCkpt++`，使既有保存点失效 |
| src/wal.c | `walRestartLog` | 写事务开始前判断能否回绕到 WAL 开头以重置日志 |
| src/wal.h | `WAL_SAVEPOINT_NDATA` | 保存点上下文数组长度，值为 4 |
| src/pager.c | `pagerOpenSavepoint` | 打开保存点，在 WAL 模式下调用 `sqlite3WalSavepoint` 采集上下文 |
| src/pager.c | `pagerPlaybackSavepoint` | 保存点回放，在 `pagerUseWal` 分支调用 `sqlite3WalSavepointUndo` |
| src/pager.c | `pagerRollbackWal` | WAL 模式整事务回滚，调用 `sqlite3WalUndo` 并传入 `pagerUndoCallback` |
| src/pager.c | `pagerUndoCallback` | 撤销回调：丢弃或重新读回被撤销的页 |
| src/pager.c | `sqlite3PagerSavepoint` | Pager 层保存点操作入口，`SAVEPOINT_ROLLBACK` 时调 `pagerPlaybackSavepoint` |
| src/pager.c | `sqlite3PagerRollback` | 整事务回滚入口，以 `iSavepoint = -1` 调用 `sqlite3PagerSavepoint` |
| src/pager.c | `PagerSavepoint` | 保存点结构体，`aWalData` 字段保存 WAL 保存点上下文 |
| src/btree.c | `sqlite3BtreeSavepoint` | Btree 层保存点入口，调用 `sqlite3PagerSavepoint` |
| src/vdbe.c | `OP_Savepoint` | VDBE 指令，处理 `SAVEPOINT` / `RELEASE` / `ROLLBACK TO` |

## 与主核 / AI 的关系

**架构推论：** `engine/db/` 主核的 WAL 层与 SQLite 的 WAL 层不共用任何机制——主核的 `wal/` 是自研实现，其保存点语义是否与 SQLite 一致无法从本页引用的源码推出，需要单独查阅主核 `engine/db/wal/` 下的实现。

本页所描述的 SQLite 机制对 AI 应用有直接价值：LLM 应用常见的「多轮对话写库、中途失败要回滚到某个检查点」场景，映射到 SQLite 就是 `SAVEPOINT` + `ROLLBACK TO`。SQLite 官方文档明确说明 `ROLLBACK TO` 不取消事务，事务会在保存点处重新开始，被 `ROLLBACK TO` 撤销的内层保存点全部作废。

值得注意的是 SQLite 官方文档对 `RELEASE` 的表述：内层事务的 `RELEASE` 不会把任何内容写入数据库文件，只是把保存点从栈上移除，使其不再可被 `ROLLBACK TO` 命中。**架构推论：** 这与源码中的实现一致——`sqlite3PagerSavepoint` 的 `SAVEPOINT_RELEASE` 分支只销毁 `pInSavepoint` 位图并截断内存子日志，不触碰 `aWalData`，因此 WAL 写指针在 `RELEASE` 时保持不变。

对 AI 编排的一个实际约束：WAL 模式下 `ROLLBACK TO` 只回退 WAL 写指针，**已经 checkpoint 回数据库文件的帧不受影响**——checkpoint 由独立线程/连接执行，可能在 `ROLLBACK TO` 之前就已把某些帧搬进数据库文件。**架构推论：** 因此在有并发 checkpoint 的场景下，WAL 模式保存点回滚与回滚日志模式的回滚并非完全等价；SQLite 源码中 checkpoint 由 `sqlite3WalCheckpoint` 实现，与保存点撤销通过 `Wal.hdr.mxFrame` 与 `WalCkptInfo.nBackfill` 两条独立路径协调。

## 参考

- SQLite 官方文档 · Savepoints：<https://sqlite.org/lang_savepoint.html>
- SQLite 官方文档 · Write-Ahead Logging：<https://sqlite.org/wal.html>
- SQLite 源码 · src/wal.c（GitHub 镜像）：<https://github.com/sqlite/sqlite/blob/master/src/wal.c>
- SQLite 源码 · src/pager.c（GitHub 镜像）：<https://github.com/sqlite/sqlite/blob/master/src/pager.c>
- SQLite 源码 · src/btree.c（GitHub 镜像）：<https://github.com/sqlite/sqlite/blob/master/src/btree.c>
