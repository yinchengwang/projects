---
project: sqlite
feature: wal-readers
page_type: feature
schema_version: 1
refs:
  - path: src/wal.c
    symbols: [sqlite3WalBeginReadTransaction, sqlite3WalEndReadTransaction, sqlite3WalSnapshotGet, sqlite3WalSnapshotOpen]
---

# WAL 读事务与快照

## 一句话职责

读者在 WAL 中定位自己可见的帧范围（锁定某个 read-mark 对应的读锁并冻结可见帧号），建立与结束只读事务；并支持把当前视图固化成快照对象，供另一个连接按该快照开启读事务。

## 机制是什么

WAL 的并发模型建立在 wal-index 头里的 read-mark 数组上。每个读者在读事务开始时要回答两个问题：我的可见帧上界是多少？哪些页可以放心从数据库文件直接读而不用查 WAL？

`src/wal.c` 里共享内存的锁字节一共 8 个（`SQLITE_SHM_NLOCK` 在 `src/sqlite.h.in` 中定义为 8）。锁序由常量固定：`WAL_WRITE_LOCK` 是 0，`WAL_CKPT_LOCK` 是 1，`WAL_RECOVER_LOCK` 是 2，`WAL_READ_LOCK(I)` 宏展开为 `3+(I)`，`WAL_NREADER` 宏展开为 `(SQLITE_SHM_NLOCK-3)`，即 5。因此存在 `WAL_READ_LOCK(0)` 到 `WAL_READ_LOCK(4)` 共 5 个读锁，与 wal-index 头中 5 个 read-mark 一一对应。

读事务的可见性契约由 `pWal->readLock` 表达。`src/wal.c` 中 `walTryBeginRead` 的注释写明：`pWal->readLock` 取值范围是 `0 <= pWal->readLock < WAL_NREADER`，取 -1 表示当前没有持有任何读锁。`readLock==0` 是一个特殊状态——读者承诺完全不读 WAL，全部内容从数据库文件取；`readLock>0` 则表示读者会使用 WAL，且不会读取帧号大于 `aReadMark[pWal->readLock]` 的帧。函数注释同时明确了另一侧：`useWal` 参数为 1 时 WAL 不会被忽略，成功后 `pWal->readLock` 必定大于 0。

第二个约束是 `pWal->minFrame`。`walTryBeginRead` 在拿到读锁后把它设为 `AtomicLoad(&pInfo->nBackfill)+1`，含义是「早于这个帧号的 WAL 内容已被回填进数据库文件，可以直接从数据库文件读」。`src/wal.c` 的注释解释了为什么这一步要放在 `walShmBarrier` 之前读 `nBackfill`、之后比对 wal-index 头：保证设置 `nBackfill` 的 checkpoint 进程看不到比 `pWal->hdr.mxFrame` 更新的帧，从而不会出现「读者以为某旧版本页在数据库文件里、实际已被后续版本覆盖」的错读。

第三个机制是显式快照（需要 `SQLITE_ENABLE_SNAPSHOT` 编译选项）。`sqlite3WalSnapshotGet` 把当前私有 wal-index 头 `pWal->hdr` 整体复制一份交给调用方；`sqlite3WalSnapshotOpen` 则把外部传入的快照指针存进 `pWal->pSnapshot`，让下一次读事务从该快照的帧号开始。二者之间的有效性判定由 `sqlite3WalSnapshotCheck` 完成。

本机制只负责读者的可见性定位与读事务生命周期。它不负责把帧回写进数据库文件（那是 checkpoint，见 h03），也不负责保存点回滚对帧的截断（见 h05）。

## 怎么实现的

### 建立读事务：`sqlite3WalBeginReadTransaction` → `walBeginReadTransaction` → `walTryBeginRead`

`sqlite3WalBeginReadTransaction` 本身只是一层薄包装：把 `walBeginReadTransaction` 包在 `SEH_TRY` / `SEH_EXCEPT` 中，异常时调用 `walHandleException(pWal)` 得到错误码返回。`walBeginReadTransaction` 才是主体，调用顺序如下：

1. **快照前置加锁（仅 `SQLITE_ENABLE_SNAPSHOT`）**：若 `pWal->pSnapshot` 非空，先用 `memcmp(pSnapshot, &pWal->hdr, sizeof(WalIndexHdr))` 比较两份头，若不同则记 `bChanged=1`。随后 `walEnableBlocking(pWal)` → `walLockShared(pWal, WAL_CKPT_LOCK)` → `walDisableBlocking(pWal)`，取一把共享的 checkpoint 锁。源码注释给出的理由是消除与并发 checkpoint 进程的竞态：checkpoint 可能已经决定要 checkpoint 某个比 `pSnapshot` 更靠后的快照位置，但还没来得及设置 `pInfo->nBackfillAttempted`。取锁失败直接返回该错误码。

2. **重试循环**：`do { rc = walTryBeginRead(pWal, pChanged, 0, &cnt); } while( rc==WAL_RETRY );`。`WAL_RETRY` 宏定义为 `(-1)`，是 `walTryBeginRead` 表示「可以安全立即重试」的内部信号，不是 SQLite 公开错误码。

3. **快照有效性复核（仅 `SQLITE_ENABLE_SNAPSHOT`）**：若第 1 步发现头已变化，在 `walTryBeginRead` 返回 `SQLITE_OK` 后再次比对。判定条件是两条，必须同时满足才认为快照仍然有效：盐值未变（`memcmp(pSnapshot->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))==0`，即 WAL 文件没有在快照之后被重置）和 `pSnapshot->mxFrame >= pInfo->nBackfillAttempted`（没有 checkpoint 尝试过把 `pSnapshot->mxFrame` 之后的帧写进数据库文件）。满足则 `memcpy(&pWal->hdr, pSnapshot, sizeof(WalIndexHdr))` 覆盖私有头，并把 `*pChanged` 设为 `bChanged`；不满足则 `rc = SQLITE_ERROR_SNAPSHOT`。函数注释还记录了为何此时把 `pWal->minFrame` 强制设为 1：使用非当前快照的客户端不能忽略 WAL 文件开头的任何帧，因为 checkpoint 可能因为存在快照之后的同页新版本而跳过 checkpoint 更早的帧。校验失败时调用 `sqlite3WalEndReadTransaction(pWal)` 收尾。

4. **释放共享 checkpoint 锁**：若第 1 步成功取到锁，`walUnlockShared(pWal, WAL_CKPT_LOCK)`。

### `walTryBeginRead` 内部的定位过程

`walTryBeginRead` 是整个读事务建立的核心，分三段：

**第一段：重试退避与读取 wal-index 头。** 每次调用先把 `*pCnt` 加一。当 `*pCnt>5` 时开始退避：初始 `nDelay=1`（1 微秒）；若 `*pCnt>=10`，延迟变为 `(cnt-9)*(cnt-9)*39` 微秒；若 `cnt` 超过 `WAL_RETRY_PROTOCOL_LIMIT`（定义为 100），直接返回 `SQLITE_PROTOCOL`。源码注释说明：第 100 次（也是最后一次）重试的延迟是 323 毫秒，总放弃时间小于 10 秒。随后 `sqlite3OsSleep(pWal->pVfs, nDelay)`。

接着在 `useWal==0` 时调用 `walIndexReadHdr(pWal, pChanged)` 把 wal-index 头读进私有的 `pWal->hdr`。若这一步返回 `SQLITE_BUSY`，代码做三段判断：若 `pWal->apWiData[0]==0`（`xShmMap` 返回 `SQLITE_BUSY`），视为瞬时条件转为 `WAL_RETRY`；否则尝试 `walLockShared(pWal, WAL_RECOVER_LOCK)`，成功即说明别的进程在跑 recovery，转为 `WAL_RETRY`；再失败且仍是 `SQLITE_BUSY` 则转成 `SQLITE_BUSY_RECOVERY`。若 `pWal->bShmUnreliable` 为真，则走 `walBeginShmUnreliable(pWal, pChanged)`。

**第二段：完全回填时的快速路径。** 判定条件是 `AtomicLoad(&pInfo->nBackfill)==pWal->hdr.mxFrame`（WAL 已被完全回填或本来就是空的），且在 `SQLITE_ENABLE_SNAPSHOT` 下额外要求没有待用快照。此时取共享锁 `WAL_READ_LOCK(0)`，`walShmBarrier(pWal)` 后比对实时 wal-index 头与私有头：若不同，说明在取到 `READ_LOCK(0)` 之前已有帧被追加或 WAL 已被回绕，数据库文件里可能留有崩溃 checkpoint 造成的损坏镜像，于是 `walUnlockShared` 后返回 `WAL_RETRY`；若相同则设 `pWal->readLock = 0` 并返回 `SQLITE_OK`。这条路径的语义是「读者完全不用 WAL，直接读数据库文件」。

**第三段：选槽位并加读锁。** 计算可见帧上界 `mxFrame = pWal->hdr.mxFrame`；若 `pWal->pSnapshot` 非空且 `pWal->pSnapshot->mxFrame < mxFrame`，把 `mxFrame` 收窄为快照的帧号。随后从 `i=1` 到 `i<WAL_NREADER` 遍历 read-mark 数组 `pInfo->aReadMark`，用 `AtomicLoad` 读出 `thisMark`，条件是 `mxReadMark<=thisMark && thisMark<=mxFrame`，取满足条件的最大者为 `mxReadMark`，记下槽位号 `mxI`。

若共享内存可写（`(pWal->readOnly & WAL_SHM_RDONLY)==0`）且 `mxReadMark<mxFrame` 或 `mxI==0`，则尝试独占占用某个槽位：对 `i=1` 到 `i<WAL_NREADER` 依次 `walLockExclusive(pWal, WAL_READ_LOCK(i), 1)`，成功后 `AtomicStore(pInfo->aReadMark+i, mxFrame)` 把 read-mark 抬到当前可见帧上界，随即 `walUnlockExclusive` 释放独占锁——源码注释说明这样做的目的是「让 checkpoint 进程尽可能多地干活」，同时保证 read-mark 的修改始终在持有对应读锁独占态时进行。

若共享内存只读（`WAL_SHM_RDONLY`，常量值为 2）且 `mxI==0`，则 `walShmBarrier` 后比对实时头与私有头：相同则返回 `SQLITE_READONLY_CANTINIT`，不同则落到下面 `mxI==0` 的 `WAL_RETRY` 分支。

拿到槽位后 `walLockShared(pWal, WAL_READ_LOCK(mxI))` 取共享读锁。若返回 `SQLITE_BUSY` 则转为 `WAL_RETRY`；在 `SQLITE_ENABLE_SETLK_TIMEOUT` 构建下 `SQLITE_BUSY_TIMEOUT` 会被记录到 `*pCnt` 的 `WAL_RETRY_BLOCKED_MASK` 位（该掩码定义为 `0x10000000`）。

取到锁之后做最终校验：`pWal->minFrame = AtomicLoad(&pInfo->nBackfill)+1`，`walShmBarrier(pWal)`，然后同时检查 `AtomicLoad(pInfo->aReadMark+mxI)!=mxReadMark` 与 `memcmp((void *)walIndexHdr(pWal), &pWal->hdr, sizeof(WalIndexHdr))`。任一不成立就 `walUnlockShared` 后返回 `WAL_RETRY`；两者都成立才设 `pWal->readLock = (i16)mxI`。源码注释给出的理由是：读锁取得之前日志可能被回绕，或 `mxFrame` 之后的帧可能已被 checkpoint 写进数据库文件，此时沿用旧的 `pWal->hdr.mxFrame` 会读到损坏的快照，所以宁可重试。

### 结束读事务：`sqlite3WalEndReadTransaction`

函数体极短：断言 `pWal->writeLock==0 || pWal->readLock<0`（`SQLITE_ENABLE_SETLK_TIMEOUT` 构建下不检查），然后在 `pWal->readLock>=0` 时先调 `sqlite3WalEndWriteTransaction(pWal)`（其作用是释放 `WAL_WRITE_LOCK` 独占锁并清零 `pWal->writeLock`、`pWal->iReCksum`、`pWal->truncateOnCommit`），再 `walUnlockShared(pWal, WAL_READ_LOCK(pWal->readLock))` 释放读锁，最后把 `pWal->readLock` 置为 -1。

### 快照：`sqlite3WalSnapshotGet` / `sqlite3WalSnapshotOpen`

`sqlite3WalSnapshotGet` 断言 `pWal->readLock>=0 && pWal->writeLock==0`，即必须在读事务内、且不在写事务中。先检查 `memcmp(&pWal->hdr.aFrameCksum[0], aZero, 16)==0`（`aZero` 是静态常量数组 `{0,0,0,0}`，按 4 字节 u32 展开即 16 字节）：若头部的帧校验和全为零，说明 WAL 是空的，没有可快照的内容，此时把 `*ppSnapshot` 置 0 并返回 `SQLITE_ERROR`。否则用 `sqlite3_malloc(sizeof(WalIndexHdr))` 分配一块内存，分配失败返回 `SQLITE_NOMEM_BKPT`；成功则 `memcpy(pRet, &pWal->hdr, sizeof(WalIndexHdr))` 拷贝整份头，把 `*ppSnapshot` 指向它。快照对象的内容对 WAL 模块之外完全不透明——源码注释明确说明「快照的内容对其他所有子系统都是不透明的，所以 WAL 模块可以在对象里放任何需要的东西」。

`sqlite3WalSnapshotOpen` 做一次分派：若传入的 `pSnapshot` 非空且 `((WalIndexHdr*)pSnapshot)->iVersion==0`，说明这是一个「占位」调用而不是真快照，于是只设 `pWal->bGetSnapshot = 1`。源码注释解释了它的用途：这是 `sqlite3_snapshot_get()` 发起的一次调用，如果不设这个标志，那么即将开始的读事务在 WAL 已被完全 checkpoint 时会取 `WAL_READ_LOCK(0)`；取 `READ_LOCK(0)` 虽然能工作，但那样后续写者可能在读者持有读事务期间销毁快照，这与用户预期相悖，因此通过 `bGetSnapshot` 避免取 `READ_LOCK(0)`。注释里也写明触发这条路径的调用方是 `sqlite3_snapshot_get()`。否则（真快照）则设 `pWal->pSnapshot = (WalIndexHdr*)pSnapshot` 并把 `bGetSnapshot` 清零。

### 真实调用链

- `src/pager.c` 的 `pagerBeginReadTransaction` 是 `sqlite3WalBeginReadTransaction` 的调用方。它先调 `sqlite3WalEndReadTransaction(pPager->pWal)` 收掉上一个事务遗留的读锁（源码注释说明 `locking_mode=EXCLUSIVE` 时上一次事务的 `EndRead` 没有被调用过，所以这里必须补调），再调 `sqlite3WalBeginReadTransaction(pPager->pWal, &changed)`。若返回码非 `SQLITE_OK` 或 `changed` 为真，则调 `pager_reset(pPager)` 清空页缓存，并在 `USEFETCH(pPager)` 为真时调 `sqlite3OsUnfetch(pPager->fd, 0, 0)` 丢弃预取页。`pagerBeginReadTransaction` 的调用方是 `src/pager.c` 的 `sqlite3PagerSharedLock`。
- `src/pager.c` 的 `sqlite3PagerSnapshotGet` 与 `sqlite3PagerSnapshotOpen` 是一对薄封装：前者仅在 `pPager->pWal` 非空时调 `sqlite3WalSnapshotGet(pPager->pWal, ppSnapshot)`，否则返回 `SQLITE_ERROR`；后者仅在 `pPager->pWal` 非空时调 `sqlite3WalSnapshotOpen(pPager->pWal, pSnapshot)`，否则返回 `SQLITE_ERROR`。
- `src/main.c` 的 `sqlite3_snapshot_get` 是公开 API 入口：要求 `db->autoCommit==0`（否则返回 `SQLITE_ERROR`），取到 `Pager` 后先调 `sqlite3PagerSnapshotOpen(pPager, (sqlite3_snapshot*)&dummy)`（传一个全零的占位结构，`iVersion==0` 从而触发上面说的 `bGetSnapshot` 路径），再调 `sqlite3BtreeBeginTrans(pBt, 0, 0)` 开启读事务，接着调 `sqlite3PagerSnapshotOpen(pPager, 0)` 清掉占位，最后调 `sqlite3PagerSnapshotGet(sqlite3BtreePager(pBt), ppSnapshot)` 取出快照。
- `src/main.c` 的 `sqlite3_snapshot_open` 是「按快照开读事务」的公开入口：若当前已有事务且 `db->nVdbeActive==0`，先调 `sqlite3PagerSnapshotCheck(pPager, pSnapshot)` 校验快照是否还可用，成功则记 `bUnlock=1` 并调 `sqlite3BtreeCommit(pBt)` 结束旧事务；随后调 `sqlite3PagerSnapshotOpen(pPager, pSnapshot)` 装入快照，再调 `sqlite3BtreeBeginTrans(pBt, 0, 0)` 开启新读事务并清掉快照引用；若 `bUnlock` 为真则调 `sqlite3PagerSnapshotUnlock(pPager)` 释放校验时取得的锁。
- `src/wal.c` 的 `sqlite3WalSnapshotCheck` 负责「快照是否还可用」的判定：取共享 `WAL_CKPT_LOCK`，然后检查 `memcmp(pNew->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))`（盐值变了说明 WAL 已被重置）或 `pNew->mxFrame<walCkptInfo(pWal)->nBackfillAttempted`（有 checkpoint 已经尝试回填越过该快照的帧），任一成立则返回 `SQLITE_ERROR_SNAPSHOT` 并在返回前释放 `WAL_CKPT_LOCK`。

## 关键取舍

- **可见性用「锁 + read-mark」表达，而不是给每个读者单独维护帧号（源码事实）**：读者持有的是 `WAL_READ_LOCK(N)` 的共享锁，帧上界隐含在共享的 `aReadMark[N]` 里。这让 checkpoint 只需扫描 read-mark 数组就能知道所有读者的位置，不必维护每读者一份状态。代价是同一时刻最多只有 `WAL_NREADER`（由 `SQLITE_SHM_NLOCK-3` 得出为 5）个活跃读事务槽位。
- **read-mark 数组的下标 0 被保留为「完全不用 WAL」的语义（源码事实）**：`WAL_READ_LOCK(0)` 对应的 read-mark 恒为 0，持有它意味着读者承诺只从数据库文件读内容。源码注释说明这让 checkpoint 可以自由推进 `nBackfill`，而不会踩到正在读数据库文件的读者。代价是取得 `READ_LOCK(0)` 前必须再比对一次实时 wal-index 头（`walShmBarrier` 之后），否则会读到崩溃 checkpoint 留下的损坏镜像，此时只能返回 `WAL_RETRY` 重来。
- **用「取锁后复核」代替「取锁前预测」来防竞态（源码事实）**：`walTryBeginRead` 在取得读锁后要同时复核 `aReadMark[mxI]` 和整份 wal-index 头，任何一项变化都退回 `WAL_RETRY`。源码注释给出的理由是要覆盖「日志被回绕」和「`mxFrame` 之后的帧被 checkpoint 写进数据库文件」两种情况。代价是竞态时要多走一轮锁与内存屏障。
- **重试采用「先快后慢」的退避而不是忙等（源码事实）**：`walTryBeginRead` 在第 5 次重试后开始 `sqlite3OsSleep`，延迟从 1 微秒起、第 10 次起按 `(cnt-9)*(cnt-9)*39` 微秒增长、上限由 `WAL_RETRY_PROTOCOL_LIMIT`（100）封顶，超限返回 `SQLITE_PROTOCOL`。源码注释说明第 100 次的延迟是 323 毫秒、总放弃时间小于 10 秒，而 `SQLITE_PROTOCOL` 意味着有别的进程没有遵守锁协议。
- **BUSY 有多个不同来源，靠读锁状态区分（源码事实）**：`walTryBeginRead` 把 `SQLITE_BUSY` 分为三类——`xShmMap` 返回 BUSY、`WAL_RECOVER_LOCK` 能取到（说明有 recovery 在跑）、以及两者都不成立的 `SQLITE_BUSY_RECOVERY`；而取读锁失败时的 BUSY 被转成 `WAL_RETRY`。`SQLITE_READONLY_CANTINIT` 则是共享内存只读且 wal-index 头没变时的专门错误码。
- **快照复制整份 wal-index 头，而不是只记一个帧号（源码事实）**：`sqlite3WalSnapshotGet` 拷贝的是 `WalIndexHdr` 全文（含 salt、`mxFrame`、`aFrameCksum`、`nPage` 等）。代价是每个快照占一份头部大小的堆内存；换来的是快照自带判据，判定失效时不必再回查共享内存。
- **快照失效必须显式报错，不静默降级（源码事实）**：盐值变化或 `nBackfillAttempted` 越界时，`walBeginReadTransaction` 返回 `SQLITE_ERROR_SNAPSHOT`（定义为 `SQLITE_ERROR | (3<<8)`），`sqlite3WalSnapshotCheck` 同样返回该错误码。没有任何一处回退到「读当前最新状态」的路径。
- **回退到「忽略 WAL 只读数据库文件」（源码事实）**：`walTryBeginRead` 在 `nBackfill==mxFrame` 且无待用快照时走 `WAL_READ_LOCK(0)` 分支，设 `pWal->readLock=0`；此后 `walFindFrame`（见 h02）在 `pWal->readLock==0` 时直接返回帧号 0，调用方 `readDbPage` 随即改走 `sqlite3OsRead(pPager->fd, ...)` 从数据库文件读同一页。
- **回退到「先重试再放弃」的读事务建立（源码事实）**：所有瞬时竞态（头被改动、read-mark 被改、读锁 BUSY）都统一转成 `WAL_RETRY`，由 `walBeginReadTransaction` 的 `do...while` 循环消化；只有超过 `WAL_RETRY_PROTOCOL_LIMIT` 才升级为 `SQLITE_PROTOCOL`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/wal.c` | `sqlite3WalBeginReadTransaction` | 建立读事务的公开入口，把 `walBeginReadTransaction` 包在 `SEH_TRY` 中，异常交给 `walHandleException` |
| `src/wal.c` | `walBeginReadTransaction` | 主体：快照前置取 `WAL_CKPT_LOCK`、驱动 `walTryBeginRead` 重试循环、复核快照有效性、释放锁 |
| `src/wal.c` | `walTryBeginRead` | 读事务定位核心：退避重试、读 wal-index 头、选 read-mark 槽位、取读锁、取锁后复核 `aReadMark` 与头 |
| `src/wal.c` | `walIndexReadHdr` | 把共享 wal-index 头读进私有 `pWal->hdr`，头损坏时走 `walIndexRecover` 重建 |
| `src/wal.c` | `sqlite3WalEndReadTransaction` | 结束读事务：先调 `sqlite3WalEndWriteTransaction`，再释放 `WAL_READ_LOCK(pWal->readLock)`，把 `readLock` 置 -1 |
| `src/wal.c` | `sqlite3WalEndWriteTransaction` | 释放 `WAL_WRITE_LOCK` 独占锁并清零 `writeLock`、`iReCksum`、`truncateOnCommit` |
| `src/wal.c` | `sqlite3WalSnapshotGet` | 复制私有 `pWal->hdr` 生成快照对象；头部帧校验和全零时返回 `SQLITE_ERROR` |
| `src/wal.c` | `sqlite3WalSnapshotOpen` | 装入外部快照到 `pWal->pSnapshot`；占位调用（`iVersion==0`）只设 `bGetSnapshot` |
| `src/wal.c` | `sqlite3WalSnapshotCheck` | 取共享 `WAL_CKPT_LOCK` 校验快照是否仍有效，失效返回 `SQLITE_ERROR_SNAPSHOT` |
| `src/wal.c` | `sqlite3WalSnapshotUnlock` | 释放 `sqlite3WalSnapshotCheck` 取得的 `WAL_CKPT_LOCK` |
| `src/wal.c` | `sqlite3_snapshot_cmp` | 比较两个快照的新旧：先比 `aSalt[0]`，再比 `mxFrame` |
| `src/wal.c` | `walLockShared` | 取共享 `xShmLock`；`pWal->exclusiveMode` 非零时直接返回 `SQLITE_OK` |
| `src/wal.c` | `walLockExclusive` | 取独占 `xShmLock`；`pWal->exclusiveMode` 非零时直接返回 `SQLITE_OK` |
| `src/pager.c` | `pagerBeginReadTransaction` | `sqlite3WalBeginReadTransaction` 的调用方，负责 `pager_reset` 与 `sqlite3OsUnfetch` 处理缓存失效 |
| `src/pager.c` | `sqlite3PagerSnapshotGet` | `sqlite3WalSnapshotGet` 的调用方；非 WAL 库返回 `SQLITE_ERROR` |
| `src/pager.c` | `sqlite3PagerSnapshotOpen` | `sqlite3WalSnapshotOpen` 的调用方；非 WAL 库返回 `SQLITE_ERROR` |
| `src/main.c` | `sqlite3_snapshot_get` | 公开 API：用占位快照 + `sqlite3BtreeBeginTrans` 开启读事务后取出当前快照 |
| `src/main.c` | `sqlite3_snapshot_open` | 公开 API：校验快照后 `sqlite3PagerSnapshotOpen` 装入并开启新的读事务 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的 `txn/` 层同样需要「读者看到一致视图」的机制。SQLite 这套方案的可借鉴点是：把「可见上界」表达成共享内存里的 read-mark，而不是每个读者一份私有帧号——这样 checkpoint 只需扫描一份固定长度的数组（长度由 `WAL_NREADER` 决定）就能算出所有读者的水位，不需要额外的注册表或链表维护。**架构推论：** 但主核若没有进程间共享内存（SQLite 依赖 `-shm` 文件 mmap，且官方文档明确说明 WAL 模式要求所有进程同主机、不能跨网络文件系统），read-mark 就无法跨进程共享，快照能力必须另行设计。

**架构推论：** SQLite 把「取锁后复核」作为一致性的最后一道防线——拿锁之后要同时比对 read-mark 与整份 wal-index 头，任何一项变化都放弃重来。主核若要复制这个模式，复核对象不能只是「版本号」这类单字段，而要覆盖「是否发生过回绕」与「是否有 checkpoint 越过」两个独立维度（对应源码里的 salt 比对与 `nBackfillAttempted` 比对）。

## 参考

- [SQLite 官方文档：WAL-mode File Format](https://sqlite.org/walformat.html) —— 给出 wal-index 头部的 read-mark 数组、`nBackfill` / `nBackfillAttempted` 语义、5 个读锁与 read-mark 的对应关系以及锁矩阵，可与源码中的 `WAL_READ_LOCK` / `WAL_NREADER` 对照。
- [SQLite 官方文档：The Write-Ahead Log (WAL)](https://sqlite.org/wal.html) —— 说明读者「记住最后一次有效提交记录的位置（end mark）」的并发模型，以及读者与写者互不阻塞的原则。
- [SQLite 官方文档：Database File Format（帧格式小节）](https://sqlite.org/fileformat2.html#walformat) —— 定义 WAL 文件头部与帧头字节布局，含提交帧的「数据库页数」字段语义。
- [SQLite 官方文档：C API sqlite3_snapshot_get](https://sqlite.org/c3ref/snapshot.html) —— 说明快照接口的公开语义与 `SQLITE_ERROR_SNAPSHOT` 的返回条件。
- [GitHub 官方镜像源码：src/wal.c](https://github.com/sqlite/sqlite/blob/master/src/wal.c) —— 支撑本文关于 `walTryBeginRead` 选槽、退避与快照复核的实现事实。
