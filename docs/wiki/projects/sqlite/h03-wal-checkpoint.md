---
project: sqlite
feature: wal-checkpoint
page_type: feature
schema_version: 1
refs:
  - path: src/wal.c
    symbols: [sqlite3WalCheckpoint, sqlite3WalExclusiveMode, sqlite3WalWriteLock]
  - path: src/pager.c
    symbols: [sqlite3PagerCheckpoint]
---

# WAL 检查点

## 一句话职责

把 WAL 文件里已提交的帧按页回写进数据库文件，并按需截断或重置 WAL，同时在四种检查点模式之间做阻塞程度的取舍。

## 机制是什么

检查点（checkpoint）把 WAL 里的事务内容搬回数据库主文件，让 WAL 文件可以被重用。回写不是按帧顺序照抄，而是按数据库页号升序做，这样对数据库文件的写尽量是顺序写。

四个检查点模式由 `src/sqlite.h.in` 的 `SQLITE_CHECKPOINT_*` 常量定义：`SQLITE_CHECKPOINT_NOOP` 为 `-1`（什么都不做）、`SQLITE_CHECKPOINT_PASSIVE` 为 `0`（尽力而为，不阻塞）、`SQLITE_CHECKPOINT_FULL` 为 `1`（等写者）、`SQLITE_CHECKPOINT_RESTART` 为 `2`（像 FULL 但还要等读者）、`SQLITE_CHECKPOINT_TRUNCATE` 为 `3`（像 RESTART，额外把 WAL 截断到 0 字节）。

回写进度记在 wal-index 的 `WalCkptInfo.nBackfill` 字段里，它是「已经搬进数据库文件的帧数」。`walCheckpoint` 的注释明确写着：`nBackfill` 是唯一会增加这个字段的地方。读者只能看到 `nBackfill` 之前的帧，因此 checkpoint 不会覆盖某个读者正在使用的数据库页——这正是 `mxSafeFrame` 存在的理由。

锁的分配在 `src/wal.c` 的宏定义里：`WAL_WRITE_LOCK` 为 `0`、`WAL_CKPT_LOCK` 为 `1`、`WAL_READ_LOCK(I)` 为 `3+(I)`、`WAL_NREADER` 为 `SQLITE_SHM_NLOCK-3`。`SQLITE_SHM_NLOCK` 在 `src/sqlite.h.in` 中定义为 `8`，所以 `WAL_NREADER` 为 `5`：槽位 0 专供 checkpoint 用，槽位 1 到 4 是读者槽位。

本机制只管「把 WAL 内容搬回数据库文件并处理 WAL 文件本身的生命周期」。帧的编码与追加属于 h02，WAL 模式开关与 wal-index 建立属于 h01。

## 怎么实现的

### 对外入口

`sqlite3PagerCheckpoint`（`src/pager.c`）是 pager 层的唯一入口。若 `pPager->pWal` 为空但 `journalMode` 已是 `PAGER_JOURNALMODE_WAL`，它先执行一次 `sqlite3_exec(db, "PRAGMA table_list",0,0,0)` 借事务把 `pWal` 初始化出来（源码注释说明这只在零字节数据库刚切到 WAL 又立刻 checkpoint 时发生）。随后调用 `sqlite3WalCheckpoint`，传入的 busy handler 取决于模式：`eMode<=SQLITE_CHECKPOINT_PASSIVE` 时传 `0`（不调用 busy handler），否则传 `pPager->xBusyHandler`。

`sqlite3_wal_checkpoint`（`src/main.c`）等价于 `sqlite3_wal_checkpoint_v2(db, zDb, SQLITE_CHECKPOINT_PASSIVE, 0, 0)`，源码以 `EVIDENCE-OF: R-41613-20553` 标注。`sqlite3_wal_checkpoint_v2` 先把 `*pnLog` 与 `*pnCkpt` 初始化为 `-1`，再断言五个模式常量取值合法；若 `eMode` 越界返回 `SQLITE_MISUSE_BKPT`。`zDb` 为空串时 `iDb` 取 `SQLITE_MAX_DB`，表示对所有附加数据库都做 checkpoint。

`sqlite3Checkpoint`（`src/main.c`）遍历 `db->nDb`，对每个匹配的库调 `sqlite3BtreeCheckpoint`；遇到 `SQLITE_BUSY` 时记 `bBusy=1` 并把 `rc` 重置为 `SQLITE_OK`，继续处理下一个库，最后统一返回 `SQLITE_BUSY`。`sqlite3BtreeCheckpoint`（`src/btree.c`）在该 Btree 处于事务中时返回 `SQLITE_LOCKED`，否则转调 `sqlite3PagerCheckpoint`。

`PRAGMA wal_checkpoint`（`src/pragma.c`）解析 `passive|full|restart|truncate|noop` 关键字为 `eMode`，默认值是 `SQLITE_CHECKPOINT_PASSIVE`，然后生成 `OP_Checkpoint` 操作码。`OP_Checkpoint`（`src/vdbe.c`）调用 `sqlite3Checkpoint`，返回三列结果；遇到 `SQLITE_BUSY` 时不报错，而是把第一列置 1。

### 顶层：`sqlite3WalCheckpoint`

`sqlite3WalCheckpoint`（`src/wal.c`）是核心调度函数：

1. 断言 `pWal->ckptLock==0` 且 `pWal->writeLock==0`；断言 `SQLITE_CHECKPOINT_NOOP<SQLITE_CHECKPOINT_PASSIVE`，且 `eMode>SQLITE_CHECKPOINT_PASSIVE` 时才允许带 busy handler。
2. `pWal->readOnly` 为真时直接返回 `SQLITE_READONLY`。
3. 若带了 busy handler，先调 `walEnableBlocking` 打开阻塞锁。
4. 模式不是 `SQLITE_CHECKPOINT_NOOP` 时，取 `WAL_CKPT_LOCK` 的排他锁（`walLockExclusive`）。这一步不调用 busy handler：源码用 `EVIDENCE-OF: R-53820-33897` 明确记录「即使配了 busy handler 也不会在这里被调用」。取锁失败即返回 `SQLITE_BUSY`。
5. 模式不是 `SQLITE_CHECKPOINT_PASSIVE` 时，还要取 `WAL_WRITE_LOCK` 的排他锁（`walBusyLock`，会走 busy handler）。若这一步返回 `SQLITE_BUSY`，则把实际模式 `eMode2` 降级为 `SQLITE_CHECKPOINT_PASSIVE`、把 `xBusy2` 置 0、把 `rc` 重置为 `SQLITE_OK`——即**回退到 PASSIVE 语义**继续做部分回写。
6. `SEH_TRY` 块内先调 `walDisableBlocking`（PASSIVE 模式的 checkpoint 不应阻塞也不应触发 busy handler），再调 `walIndexReadHdr` 读 wal-index 头；若 `eMode2>SQLITE_CHECKPOINT_PASSIVE` 则重新 `walEnableBlocking`。若头发生变化且 VFS 的 `iVersion>=3`，调 `sqlite3OsUnfetch` 丢弃被读缓存的数据库页。
7. 若 `pWal->hdr.mxFrame` 非零但 `walPagesize(pWal)!=nBuf`，返回 `SQLITE_CORRUPT_BKPT`；否则在 `eMode2!=SQLITE_CHECKPOINT_NOOP` 时调 `walCheckpoint`。
8. `rc` 为 `SQLITE_OK` 或 `SQLITE_BUSY` 时回填输出参数：`*pnLog = pWal->hdr.mxFrame`，`*pnCkpt = walCkptInfo(pWal)->nBackfill`。
9. `SEH_EXCEPT` 分支把异常交给 `walHandleException`，该函数按 `pWal->readLock`/`writeLock`/`ckptLock` 的当前值释放应有的锁、释放 `pWal->pFree`，并返回 `SQLITE_IOERR`。
10. 若头发生变化，把 `pWal->hdr` 整体清零（`memset`），使页缓存知道 wal-index 头需要重新读。
11. 收尾：`walDisableBlocking`、`sqlite3WalDb(pWal, 0)`、`sqlite3WalEndWriteTransaction` 释放写锁、释放 `WAL_CKPT_LOCK`。
12. 返回值有一处特殊处理：`return (rc==SQLITE_OK && eMode!=eMode2 ? SQLITE_BUSY : rc)`。即请求的是 FULL/RESTART/TRUNCATE 但因写锁抢不到而降级成 PASSIVE 时，最终对调用者报 `SQLITE_BUSY`。

### 内层：`walCheckpoint`

`walCheckpoint`（`src/wal.c`）真正做回写：

1. `szPage = walPagesize(pWal)`，页大小按 `(pWal->hdr.szPage&0xfe00) + ((pWal->hdr.szPage&0x0001)<<16)` 还原（65536 被编码成 1）。
2. 只有 `pInfo->nBackfill<pWal->hdr.mxFrame` 时才需要干活。`pInfo` 来自 `walCkptInfo(pWal)`。
3. 计算 `mxSafeFrame`：初值是 `pWal->hdr.mxFrame`。随后遍历读槽位 `i` 从 1 到 `WAL_NREADER-1`（即 1 到 4），取 `AtomicLoad(pInfo->aReadMark+i)`。若 `mxSafeFrame>y`，则调 `walBusyLock(pWal, xBusy, pBusyArg, WAL_READ_LOCK(i), 1)` 去抢该读者槽位：抢到则把 `aReadMark[i]` 设为 `mxSafeFrame`（`i==1` 时）或 `READMARK_NOT_USED`（`READMARK_NOT_USED` 定义为 `0xffffffff`），再释放锁；返回 `SQLITE_BUSY` 则把 `mxSafeFrame` 收缩到 `y` 并把 `xBusy` 置 0；其他错误直接跳到 `walcheckpoint_out`。
4. 若 `pInfo->nBackfill<mxSafeFrame`，调 `walIteratorInit(pWal, pInfo->nBackfill, &pIter)` 分配迭代器。`walIteratorInit` 有两种算法：算法 WALITER-2 用于 `(iLast-iZero)*sizeof(ht_slot) > pWal->hdr.nPage*sizeof(u32)` 的情形（页映射比哈希表省内存），算法 WALITER-1 用于其余情形。内存用 `sqlite3_malloc64(nByte + nExtra)` 分配，失败返回 `SQLITE_NOMEM_BKPT`。
5. 只有迭代器分配成功且抢到 `WAL_READ_LOCK(0)` 的排他锁时才继续。抢到读槽位 0 后比较 `walIndexHdr(pWal)` 的 salt 与 `pWal->hdr.aSalt`：`memcmp` 结果非零说明 WAL 在读头之后被回绕过，此时直接跳过回写——源码注释解释，如果继续做，WAL 文件里的有效帧数可能少于 `pWal->hdr.mxFrame`。
6. salt 相同则记 `pInfo->nBackfillAttempted = mxSafeFrame`，然后 `sqlite3OsSync(pWal->pWalFd, CKPT_SYNC_FLAGS(sync_flags))` 把 WAL 落盘。`CKPT_SYNC_FLAGS(X)` 定义在 `src/wal.h`，取 `((X)>>2)&0x03`。
7. 预估数据库文件大小：`nReq = ((i64)mxPage * szPage)`，其中 `mxPage` 取 `pWal->hdr.nPage`。用 `sqlite3OsFileControl(pWal->pDbFd, SQLITE_FCNTL_CKPT_START, 0)` 通知 VFS，并用 `sqlite3OsFileSize` 取当前大小。若当前文件小于 `nReq` 且 `nSize+65536+(i64)pWal->hdr.mxFrame*szPage<nReq`，判定为损坏，返回 `SQLITE_CORRUPT_BKPT`（源码注释说明 65536 是 pending-byte 页的最大字节数）；否则用 `sqlite3OsFileControlHint(pWal->pDbFd, SQLITE_FCNTL_SIZE_HINT, &nReq)` 给出预分配提示。
8. 逐页回写循环：`walIteratorNext(pIter, &iDbpage, &iFrame)` 产出下一个页号与帧号。`walIteratorNext` 在 WALITER-2 下按 `aSegment[0].aPgno` 顺序前进，在 WALITER-1 下遍历各 segment 取页号最小者，因此输出是页号升序。
   - 循环体先检查 `AtomicLoad(&db->u1.isInterrupted)`，被打断则 `rc = db->mallocFailed ? SQLITE_NOMEM_BKPT : SQLITE_INTERRUPT` 并退出循环。
   - 跳过条件是 `iFrame<=nBackfill || iFrame>mxSafeFrame || iDbpage>mxPage`：已回写的帧不动，超出安全边界的帧不动，超出数据库页数的帧不动。
   - 通过的帧：从 WAL 读 `sqlite3OsRead(pWal->pWalFd, zBuf, szPage, iOffset)`，`iOffset = walFrameOffset(iFrame, szPage) + WAL_FRAME_HDRSIZE`（`walFrameOffset` 宏定义在 `src/wal.c`）；再写数据库文件 `sqlite3OsWrite(pWal->pDbFd, zBuf, szPage, iOffset)`，`iOffset = (iDbpage-1)*(i64)szPage`。读写任一步出错即 `break`。
9. 循环结束后调 `sqlite3OsFileControl(pWal->pDbFd, SQLITE_FCNTL_CKPT_DONE, 0)`。
10. 若全程无错且 `mxSafeFrame==walIndexHdr(pWal)->mxFrame`（即整个 WAL 都搬完了），则按 `szDb = pWal->hdr.nPage*(i64)szPage` 调 `sqlite3OsTruncate(pWal->pDbFd, szDb)` 截断数据库文件到正确大小，再 `sqlite3OsSync(pWal->pDbFd, CKPT_SYNC_FLAGS(sync_flags))`。
11. 无错时 `AtomicStore(&pInfo->nBackfill, mxSafeFrame)`，推进回写进度。
12. 释放读槽位 0 的排他锁 `walUnlockExclusive(pWal, WAL_READ_LOCK(0), 1)`。
13. 若 `rc==SQLITE_BUSY`，重置为 `SQLITE_OK`——源码注释说明这是为了不因为有活跃读者就把 checkpoint 报成失败。
14. 非 PASSIVE 模式的收尾（`eMode!=SQLITE_CHECKPOINT_PASSIVE`）：
    - 若 `pInfo->nBackfill<pWal->hdr.mxFrame`（没搬完），返回 `SQLITE_BUSY`。
    - 若 `eMode>=SQLITE_CHECKPOINT_RESTART`：用 `sqlite3_randomness(4, &salt1)` 取新 salt，调 `walBusyLock(..., WAL_READ_LOCK(1), WAL_NREADER-1)` 把槽位 1 到 4 全部排他锁住。锁到之后：
      - `eMode==SQLITE_CHECKPOINT_TRUNCATE` 时调 `walRestartHdr(pWal, salt1)` 重置 WAL 头（`nCkpt` 加一、`mxFrame` 置 0、第一个 salt 加一、第二个 salt 换成新值、`nBackfill` 与 `nBackfillAttempted` 清零、`aReadMark[1]` 置 0、`aReadMark[2..]` 置 `READMARK_NOT_USED`），再 `sqlite3OsTruncate(pWal->pWalFd, 0)` 把 WAL 文件截为 0 字节。
      - 然后 `walUnlockExclusive(pWal, WAL_READ_LOCK(1), WAL_NREADER-1)` 释放。
15. `walcheckpoint_out` 处 `SEH_FREE_ON_ERROR(pIter, 0)` 与 `walIteratorFree(pIter)` 释放迭代器，返回 `rc`。

### 独占模式与写锁

`sqlite3WalExclusiveMode`（`src/wal.c`）管理 WAL 子系统进出 `locking_mode=EXCLUSIVE`：
- `op==0`（退出独占）：若当前不是 `WAL_NORMAL_MODE`，先置 `WAL_NORMAL_MODE`，再 `walLockShared(pWal, WAL_READ_LOCK(pWal->readLock))` 取当前读槽位的共享锁；取锁失败则回退成 `WAL_EXCLUSIVE_MODE`。返回是否成功切换。
- `op>0`（进入独占）：断言当前是 `WAL_NORMAL_MODE`，`walUnlockShared` 释放读锁共享锁，置 `WAL_EXCLUSIVE_MODE`，返回 1。
- `op<0`（干跑，不实际切换）：只返回当前是否处于 `WAL_NORMAL_MODE`。

调用方都在 `src/pager.c`：`sqlite3PagerBegin` 在 `pPager->exclusiveMode` 为真时先调 `sqlite3WalExclusiveMode(pPager->pWal, -1)` 试问能否进独占，能则先 `pagerLockDb(pPager, EXCLUSIVE_LOCK)` 再调 `sqlite3WalExclusiveMode(pPager->pWal, 1)` 真正切换。`pagerUnlockAndRollback`（提交路径的解锁段）在 `!pPager->exclusiveMode` 且（`!pagerUseWal(pPager)` 或 `sqlite3WalExclusiveMode(pPager->pWal, 0)`）成立时才 `pagerUnlockDb(pPager, SHARED_LOCK)`。

`sqlite3WalWriteLock`（`src/wal.c`）用于 `SQLITE_ENABLE_SETLK_TIMEOUT` 构建：它开关阻塞锁并取放 `WAL_WRITE_LOCK`。`bLock` 为真时先 `walEnableBlocking`，成功后 `walLockExclusive(pWal, WAL_WRITE_LOCK, 1)` 并置 `pWal->writeLock=1`，再 `walDisableBlocking`；`walEnableBlocking` 返回假表示不能启用阻塞锁，此时连取锁都不尝试，直接返回 `SQLITE_OK`。`bLock` 为假且 `pWal->writeLock` 非零时释放该锁。唯一调用方是 `src/pager.c` 的 `sqlite3PagerWalWriteLock`，它在 `pagerUseWal(pPager) && pPager->exclusiveMode==0` 时才转调 `sqlite3WalWriteLock`。

### 自动 checkpoint 与关闭时 checkpoint

自动 checkpoint 由 `src/main.c` 的 `sqlite3WalDefaultHook` 实现：它是 `sqlite3_wal_autocheckpoint` 注册的 wal hook，回调里判断 `nFrame>=SQLITE_PTR_TO_INT(pClientData)`（`pClientData` 就是 `sqlite3_wal_autocheckpoint` 传入的阈值），成立则调 `sqlite3_wal_checkpoint(db, zDb)`（即 PASSIVE 模式）。整个调用包在 `sqlite3BeginBenignMalloc`/`sqlite3EndBenignMalloc` 之间。

关闭时的最后一次 checkpoint 在 `sqlite3WalClose`（`src/wal.c`）里：若 `zBuf!=0` 且能通过 `sqlite3OsLock(pWal->pDbFd, SQLITE_LOCK_EXCLUSIVE)` 拿到数据库文件的排他锁，则调 `sqlite3WalCheckpoint(pWal, db, SQLITE_CHECKPOINT_PASSIVE, 0, 0, sync_flags, nBuf, zBuf, 0, 0)`。成功后按 `SQLITE_FCNTL_PERSIST_WAL` 的提示决定：非持久 WAL 模式置 `isDelete=1`（后面 `sqlite3OsDelete` 删掉 WAL 文件）；持久 WAL 模式且 `pWal->mxWalSize>=0` 时调 `walLimitSize(pWal, 0)` 把 WAL 截断到 0 字节。

`sqlite3PagerCloseWal`（`src/pager.c`）是切回回滚模式前的收口：先 `pagerExclusiveLock(pPager)` 拿数据库排他锁，再调 `sqlite3WalClose(pPager->pWal, db, pPager->walSyncFlags, pPager->pageSize, (u8*)pPager->pTmpSpace)`，最后把 `pPager->pWal` 置 0。

### 同步标志

`src/wal.h` 定义 `WAL_SYNC_FLAGS(X)` 为 `((X)&0x03)`（事务提交用），`CKPT_SYNC_FLAGS(X)` 为 `(((X)>>2)&0x03)`（checkpoint 用）。`src/pager.c` 里 `pPager->walSyncFlags = (pPager->syncFlags<<2)`，并在 `synchronous=FULL` 时额外置上 `SQLITE_SYNC_FULL<<2`；而 `sqlite3PagerCheckpoint` 把 `pPager->walSyncFlags` 整体传给 `sqlite3WalCheckpoint`，后者再用 `CKPT_SYNC_FLAGS` 取低 2 位。

## 关键取舍

- **回写按页号升序而非帧号顺序（源码事实）**：`walIteratorNext` 在 WALITER-1 下遍历各 segment 取页号最小者，WALITER-2 下沿 `aPgno` 顺序前进，两种算法都产出页号升序。这样对数据库文件的写尽量顺序，代价是 WAL 文件里同一页的多帧需要靠 wal-index 哈希表定位最新可见帧。
- **用 `mxSafeFrame` 限制回写边界，而不是锁住所有读者（源码事实）**：只有当读者槽位的 `aReadMark` 高于当前 `mxSafeFrame` 时才去抢该槽位的锁；抢不到就把 `mxSafeFrame` 收缩到该读者的 `aReadMark`，让本轮 checkpoint 少搬一些帧。代价是长读事务会让 checkpoint 反复做部分工作。
- **RESTART/TRUNCATE 用「锁住全部读槽位」而不是「逐个等待」（源码事实）**：`walBusyLock(pWal, xBusy, pBusyArg, WAL_READ_LOCK(1), WAL_NREADER-1)` 一次性锁住槽位 1 到 4（数量 `WAL_NREADER-1` 为 4），一次性把剩余读者全部挡在外面，代价是这段时间内新读者会被阻塞。
- **TRUNCATE 模式先改 wal-index 头再截断文件（源码事实）**：`walRestartHdr` 把 wal-index 头改成「零有效帧」后才 `sqlite3OsTruncate(pWal->pWalFd, 0)`。源码注释解释：只截文件不改头会让 wal-index 头与文件系统内容不一致；先改头再截断则保证后续读者与写者都能看到「整个日志已 checkpoint」。
- **TRUNCATE 是 RESTART 的超集而非替代（源码事实）**：`sqlite3WalCheckpoint` 里 `eMode>=SQLITE_CHECKPOINT_RESTART` 的分支同时覆盖 RESTART 与 TRUNCATE，TRUNCATE 只是在该分支内多加一次 `walRestartHdr` 与 `sqlite3OsTruncate(pWal->pWalFd, 0)`。
- **降级为 PASSIVE 而不是直接失败（源码事实）**：抢 `WAL_WRITE_LOCK` 失败时，`sqlite3WalCheckpoint` 把 `eMode2` 改成 `SQLITE_CHECKPOINT_PASSIVE` 并继续做部分回写，但函数末尾 `eMode!=eMode2` 时对调用者返回 `SQLITE_BUSY`。这样实际工作没浪费，调用者仍知道没达到要求的强度。
- **回退到 PASSIVE 语义时同时停用 busy handler（源码事实）**：`xBusy2 = 0` 与 `eMode2 = SQLITE_CHECKPOINT_PASSIVE` 一起设置，保证降级后不会再去阻塞等待任何锁。
- **PASSIVE 模式全程不触发 busy handler（源码事实）**：`sqlite3WalCheckpoint` 里断言 `eMode>SQLITE_CHECKPOINT_PASSIVE || xBusy==0`，`sqlite3PagerCheckpoint` 传入的 handler 也只在 `eMode>SQLITE_CHECKPOINT_PASSIVE` 时才非空；`walCheckpoint` 里对 PASSIVE 模式另有一条断言 `eMode!=SQLITE_CHECKPOINT_PASSIVE || xBusy==0`。
- **checkpoint 锁不参与 busy handler 等待（源码事实）**：取 `WAL_CKPT_LOCK` 用的是 `walLockExclusive` 而非 `walBusyLock`，源码以 `EVIDENCE-OF: R-53820-33897` 标注此行为。
- **salt 不匹配就放弃本轮回写（源码事实）**：`walCheckpoint` 用 `memcmp(pLive->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))` 检测 WAL 是否被回绕。代价是这一轮 checkpoint 完全没干活，好处是绝不会把已被回绕覆盖的帧内容写进数据库文件。
- **回退到「不搬这一页」的判断是三重条件（源码事实）**：`iFrame<=nBackfill || iFrame>mxSafeFrame || iDbpage>mxPage` 三个条件任一成立就 `continue`。这三条对应「已搬过」「超出读者安全边界」「超出数据库页数」。
- **回退到只提示不截断的策略（源码事实）**：当 `nSize<nReq` 但 `nSize+65536+(i64)pWal->hdr.mxFrame*szPage>=nReq` 时，不报损坏，而是走 `sqlite3OsFileControlHint(pWal->pDbFd, SQLITE_FCNTL_SIZE_HINT, &nReq)`。这是**回退到** `SQLITE_FCNTL_SIZE_HINT` 提示路径，不是直接扩展文件。
- **截断数据库文件只在整个 WAL 搬完时做（源码事实）**：`sqlite3OsTruncate(pWal->pDbFd, szDb)` 的前置条件是 `mxSafeFrame==walIndexHdr(pWal)->mxFrame`。部分回写时不动数据库文件大小。
- **关闭时优先删文件而非截断（源码事实）**：`sqlite3WalClose` 里 `bPersist!=1` 时置 `isDelete=1` 并在最后 `sqlite3OsDelete`；`bPersist==1` 且 `pWal->mxWalSize>=0` 时改走 `walLimitSize(pWal, 0)` 截断。源码注释说明持久 WAL 模式不能按 `journal_size_limit` 截断到那个尺寸，否则会在磁盘上留下损坏的 WAL 文件。
- **写锁降级后不释放 checkpoint 锁（源码事实）**：`sqlite3WalCheckpoint` 收尾时先 `sqlite3WalEndWriteTransaction(pWal)` 释放写锁，再释放 `WAL_CKPT_LOCK`，两把锁是分别释放的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/wal.c` | `sqlite3WalCheckpoint` | checkpoint 顶层调度：取 checkpoint 锁与写锁、读 wal-index 头、降级处理、调用 `walCheckpoint`、回填输出参数、异常交给 `walHandleException` |
| `src/wal.c` | `walCheckpoint` | 回写主体：计算 `mxSafeFrame`、分配迭代器、逐页读 WAL 写数据库、截断数据库文件、更新 `nBackfill`、RESTART/TRUNCATE 的读者槽位全锁与 WAL 截断 |
| `src/wal.c` | `sqlite3WalExclusiveMode` | WAL 子系统进出 `locking_mode=EXCLUSIVE`：`op==0` 退出并取读槽位共享锁、`op>0` 进入并释放读锁、`op<0` 只做干跑查询 |
| `src/wal.c` | `sqlite3WalWriteLock` | 在 `SQLITE_ENABLE_SETLK_TIMEOUT` 构建下开关阻塞锁并获取或释放 `WAL_WRITE_LOCK` |
| `src/wal.c` | `walIteratorInit` | 分配回写迭代器，按空间估算在 WALITER-1 与 WALITER-2 两种算法间选择，分配失败返回 `SQLITE_NOMEM_BKPT` |
| `src/wal.c` | `walIteratorNext` | 产出下一个待回写的页号与帧号，两种算法下均保证页号升序 |
| `src/wal.c` | `walRestartHdr` | 重置 WAL 头：`nCkpt` 加一、`mxFrame` 置 0、salt 更新、`nBackfill` 与 `nBackfillAttempted` 清零、读标记复位 |
| `src/wal.c` | `walPagesize` | 从 wal-index 头还原真实页大小，处理 65536 被编码成 1 的情形 |
| `src/wal.c` | `walIndexReadHdr` | 读取 wal-index 头；共享内存不可写时置 `bShmUnreliable` 并转 `WAL_HEAPMEMORY_MODE` |
| `src/wal.c` | `walHandleException` | `SEH_EXCEPT` 处理器：按当前持有的读锁/写锁/checkpoint 锁状态释放锁、释放 `pWal->pFree`、返回 `SQLITE_IOERR` |
| `src/wal.c` | `sqlite3WalClose` | 关闭 WAL 句柄：拿数据库排他锁后做一次 PASSIVE checkpoint，再按 `SQLITE_FCNTL_PERSIST_WAL` 决定删文件还是截断 |
| `src/wal.c` | `walLimitSize` | 把 WAL 文件截到指定上限，截断失败只记 `sqlite3_log` 不报错 |
| `src/pager.c` | `sqlite3PagerCheckpoint` | pager 层 checkpoint 入口：必要时先用 `PRAGMA table_list` 初始化 `pWal`，再转调 `sqlite3WalCheckpoint`，按模式决定是否传 busy handler |
| `src/pager.c` | `sqlite3PagerWalWriteLock` | `SQLITE_ENABLE_SETLK_TIMEOUT` 下把写锁请求转发给 `sqlite3WalWriteLock` |
| `src/pager.c` | `sqlite3PagerCloseWal` | 切回回滚模式前：取数据库排他锁、调 `sqlite3WalClose` 做最后 checkpoint、置 `pPager->pWal=0` |
| `src/btree.c` | `sqlite3BtreeCheckpoint` | Btree 层 checkpoint：事务中返回 `SQLITE_LOCKED`，否则转调 `sqlite3PagerCheckpoint` |
| `src/main.c` | `sqlite3_wal_checkpoint_v2` | 公共 API：校验模式范围、解析库名、遍历所有附加库调用 `sqlite3Checkpoint` |
| `src/main.c` | `sqlite3Checkpoint` | 遍历 `db->nDb` 调用 `sqlite3BtreeCheckpoint`，遇到 `SQLITE_BUSY` 继续处理并最终统一返回 `SQLITE_BUSY` |
| `src/main.c` | `sqlite3WalDefaultHook` | 自动 checkpoint 钩子：帧数达到阈值时调用 `sqlite3_wal_checkpoint`（PASSIVE） |
| `src/pragma.c` | `PragTyp_WAL_CHECKPOINT` | 解析 `PRAGMA wal_checkpoint` 的模式关键字并生成 `OP_Checkpoint` 操作码 |
| `src/vdbe.c` | `OP_Checkpoint` | 执行 checkpoint 并回填三列结果，遇到 `SQLITE_BUSY` 不报错而是把首列置 1 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的 `wal/` 层与 `storage/` 层之间也需要一个「把日志内容搬回主存储」的环节，对应 SQLite 的 checkpoint。但 SQLite 把 checkpoint 的边界判定完全建立在 wal-index 的 `aReadMark` 与 `nBackfill` 这两个共享内存字段上——读者把自己的可见帧号登记进 `aReadMark`，检查点据此算出 `mxSafeFrame`。主核若采用每事务独立的日志文件或以日志段为单位回放，就不需要这套「读者槽位」机制，但也就要另找办法保证「回写不会覆盖读者正在读的主存储页」。

**架构推论：** SQLite 的四种模式是用一个整数 `eMode` 表达的强度阶梯（`NOOP` 到 `TRUNCATE` 取值 `-1` 到 `3`），强度提升的代价逐级递增：PASSIVE 不等任何人，FULL 多等一个写锁，RESTART 再多锁住全部读槽位，TRUNCATE 再多截断 WAL 文件。主核若要让上层选择 checkpoint 强度，照搬这个「单参数编码强度」的接口形态比引入多个独立开关更省事；照搬时要注意 SQLite 把「抢写锁失败就降级为 PASSIVE 但对调用者报 BUSY」这一处「内部尽力 + 外部如实」的组合也一并移植，否则调用方会误以为要求已被满足。

**架构推论：** 主核的 WAL 层若不把「帧写入」与「日志回放」共用一套校验和/序号体系，则 SQLite 里 `walRestartHdr` 把 `nCkpt` 加一并让下一个写者从文件开头重写的「日志复用」机制无法直接移植——该机制依赖 WAL 头里的 salt 与 `mxFrame` 能唯一标识一轮日志生命周期。

## 参考

- [SQLite 官方文档：Write-Ahead Logging](https://sqlite.org/wal.html) —— 说明 checkpoint 的语义、读者 end mark 对回写的限制、自动 checkpoint 阈值与 WAL 文件复用策略。
- [SQLite 官方文档：Checkpoint Mode Values](https://sqlite.org/c3ref/c_checkpoint_full.html) —— 给出 `SQLITE_CHECKPOINT_NOOP`/`PASSIVE`/`FULL`/`RESTART`/`TRUNCATE` 五个常量的官方定义与含义。
- [SQLite 官方文档：PRAGMA wal_checkpoint](https://sqlite.org/pragma.html#pragma_wal_checkpoint) —— 说明 `PRAGMA wal_checkpoint` 的三种模式关键字与返回的三列结果。
- [SQLite 官方文档：WAL-mode File Format](https://sqlite.org/walformat.html) —— 给出 wal-index 头部字段、`mxFrame`/`nBackfill`/`aReadMark` 语义与锁矩阵，可与源码中的 `WalIndexHdr`、`WalCkptInfo` 对照。
- [GitHub 官方镜像源码：src/wal.c](https://github.com/sqlite/sqlite/blob/master/src/wal.c) —— 支撑本文关于 `sqlite3WalCheckpoint`、`walCheckpoint`、`sqlite3WalExclusiveMode`、`sqlite3WalWriteLock` 的实现事实。
- [GitHub 官方镜像源码：src/pager.c](https://github.com/sqlite/sqlite/blob/master/src/pager.c) —— 支撑本文关于 `sqlite3PagerCheckpoint` 的调用方与参数传递事实。
