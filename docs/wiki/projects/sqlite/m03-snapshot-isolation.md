---
project: sqlite
feature: snapshot-isolation
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_snapshot_get]
  - path: src/pager.c
    symbols: [sqlite3PagerSnapshotGet, sqlite3PagerSnapshotCheck, sqlite3PagerSnapshotRecover]
---

# 快照与并发读

## 一句话职责

SQLite 在 WAL 模式下把「某一时刻的 WAL 状态」固化成不透明的 `sqlite3_snapshot` 句柄，允许别的连接事后回到那个历史点继续读；并用「快照帧号 vs. checkpoint 已回填帧号」的比较来判断快照是否已被覆盖。

## 机制是什么

SQLite 的快照能力与事务状态机（M01）是两件事。事务状态机回答「这个连接现在处在事务的哪个状态」，快照回答「我要读到哪个历史版本的数据」。二者通过 `db->autoCommit` 与 `sqlite3BtreeTxnState()` 这两个既有判据耦合，但不是同一套机制。

快照的全部实现都被 `SQLITE_ENABLE_SNAPSHOT` 编译期开关圈起来，源码里每一处都写成 `#ifdef SQLITE_ENABLE_SNAPSHOT`；同时整块代码又被 `#ifndef SQLITE_OMIT_WAL` 包住——**快照是 WAL 模式的专属能力**，回滚日志模式下没有这条路径。

快照对象本身在公开头文件里是一个不透明结构体：

```
typedef struct sqlite3_snapshot {
  unsigned char hidden[48];
} sqlite3_snapshot;
```

`src/sqlite.h.in` 中的 `hidden[48]` 是给实现留的固定尺寸空间；实际内容由 WAL 模块决定——`src/wal.c` 的 `sqlite3WalSnapshotGet` 把一份 `WalIndexHdr`（结构体 `src/wal.c`）按 `memcpy` 填进去，因此 `sizeof(WalIndexHdr)` 恰好落在 48 字节内。`src/wal.c` 的注释直接写明「The content of a snapshot is opaque to every other subsystem, so the WAL module can put whatever it needs in the object.」——上层只当指针用，不解析内容。

快照的内容是一份 `WalIndexHdr`，其关键字段（`src/wal.c`）：

- `u32 iVersion`：wal-index 版本号。这个字段在快照机制里被当作**「这是不是一个真快照」的标记**。
- `u32 mxFrame`：快照时刻 WAL 中的最后一帧序号。
- `u32 aSalt[2]`：从 WAL 文件头复制来的两个 salt 值。`src/wal.c` 的注释指出 `aSalt[0]` 每次 WAL 文件重启就自增一次。
- `u32 aFrameCksum[2]`：最后一帧的校验和。

快照的可用性判定归结为两个条件的比较，由 `src/wal.c` 的 `sqlite3WalSnapshotCheck` 完成：

1. **salt 是否一致**——`memcmp(pNew->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))`。salt 变了说明 WAL 文件被重置/回绕过，旧的快照不再对应任何现存帧。
2. **mxFrame 是否还大于等于已尝试回填的帧号**——`pNew->mxFrame < walCkptInfo(pWal)->nBackfillAttempted`。`src/wal.c` 中 `nBackfillAttempted` 的注释写明它是「checkpoint 已尝试达到的最大 nBackfill 值」，且「nBackfillAttempted 在任何回填动作之前就被设上，nBackfill 只在全部回填完成后才设上」，因此 checkpoint 中途崩溃会让 `nBackfillAttempted` 大于 `nBackfill`。用 `nBackfillAttempted`（而不是 `nBackfill`）做比较正是为了覆盖「checkpoint 正在写但没写完」这个窗口。

两个条件都不满足时，`sqlite3WalSnapshotCheck` 返回 `SQLITE_ERROR_SNAPSHOT`，该常量在 `src/sqlite.h.in` 定义为 `(SQLITE_ERROR | (3<<8))`。

本组机制的**边界**（按页面清单条目）：负责显式快照的取得与释放、以及快照期间对模式变更的拒绝；不负责 WAL 帧的读取（H02）、不负责文件锁（G06）。也就是说，帧的定位与读出在 `src/wal.c` 的 `walFindFrame` 一侧完成，文件锁在 `src/pager.c` 的锁管理一侧完成，本页只覆盖「快照句柄的取得、校验、恢复」这条主干。

## 怎么实现的

### 1. 取快照：`sqlite3_snapshot_get`（`src/main.c`）

`src/main.c` 的 `sqlite3_snapshot_get` 函数体是本篇的主干，它把「检查前置条件 → 临时开一次读事务 → 抓快照 → 收尾」串在一起：

1. `rc` 初始化为 `SQLITE_ERROR`——**任何一步不满足都返回 `SQLITE_ERROR`**，这是该 API 的默认失败码。
2. 整段逻辑被 `#ifndef SQLITE_OMIT_WAL` 包住，WAL 被编译掉时该函数直接返回 `SQLITE_ERROR`。
3. `SQLITE_ENABLE_API_ARMOR` 打开时先调 `sqlite3SafetyCheckOk(db)`，不通过返回 `SQLITE_MISUSE_BKPT`。
4. `sqlite3_mutex_enter(db->mutex)` 进入连接互斥锁。
5. **第一个前置条件**：`if( db->autoCommit==0 )`——只有连接已处于显式事务中（自动提交已关闭）才继续，否则整个块被跳过，`rc` 保持 `SQLITE_ERROR`。这个判据直接复用 M01 讲的 `db->autoCommit` 字段。
6. `iDb = sqlite3FindDbName(db, zDb)`（`src/build.c`）按库名查出 `db->aDb[]` 下标。该函数用 `sqlite3_stricmp` 做大小写不敏感匹配，并保证 `"main"` 即使被 `SQLITE_DBCONFIG_MAINDBNAME` 改过名也永远可作主库别名。
7. **第二个前置条件**：`if( iDb==0 || iDb>1 )`。注意 `sqlite3FindDbName` 找不到时返回 -1，而 `-1` 既不满足 `iDb==0` 也不满足 `iDb>1`，于是被过滤掉；下标 1 是 `temp` 库（`iDb==1` 恰好落在两个条件的空隙里），也被排除。因此**快照只对主库与附加库有效，`temp` 库被显式排除**。
8. **第三个前置条件**：`if( SQLITE_TXN_WRITE!=sqlite3BtreeTxnState(pBt) )`——该库上不能有写事务。`sqlite3BtreeTxnState`（`src/btree.c`）的函数体是 `return p ? p->inTrans : 0;`，返回的 `inTrans` 取值对应 `src/btreeInt.h` 的 `TRANS_NONE`/`TRANS_READ`/`TRANS_WRITE`（0/1/2），`src/btreeInt.h` 用 `#if TRANS_WRITE!=SQLITE_TXN_WRITE` 这样的编译期断言强制它们与 `src/sqlite.h.in` 的 `SQLITE_TXN_*` 数值一致。
9. **取快照的具体手法**：取 `Pager *pPager = sqlite3BtreePager(pBt)`（`src/btree.c`，函数体是 `return p->pBt->pPager;`），声明 `i64 dummy = 0;`，然后：

   ```
   sqlite3PagerSnapshotOpen(pPager, (sqlite3_snapshot*)&dummy);
   rc = sqlite3BtreeBeginTrans(pBt, 0, 0);
   sqlite3PagerSnapshotOpen(pPager, 0);
   if( rc==SQLITE_OK ){
     rc = sqlite3PagerSnapshotGet(sqlite3BtreePager(pBt), ppSnapshot);
   }
   ```

   这里有两处需要说清楚：

   - **`dummy` 的作用**：它是一个清零的 `i64`，被强转成 `sqlite3_snapshot*` 传给 `sqlite3PagerSnapshotOpen`。`src/wal.c` 的 `sqlite3WalSnapshotOpen` 判断 `if( pSnapshot && ((WalIndexHdr*)pSnapshot)->iVersion==0 )`——因为 `WalIndexHdr` 的第一个字段就是 `u32 iVersion`（`src/wal.c`），且 `dummy` 全零，所以 `iVersion==0` 成立，命中「取快照」分支，把 `pWal->bGetSnapshot` 置 1。这个标志的作用在 `src/wal.c` 的注释里写明：让接下来的读事务**不去取 read-lock 0**，因为 `src/wal.c` 的注释指出持有 `READ_LOCK(0)` 时 reader 会忽略整个 WAL，而一旦拿到 0 号读锁，后续写者就能在这个连接持有读事务期间摧毁快照——这违背用户预期。真正的快照句柄此时并不存在，它是在 `sqlite3BtreeBeginTrans` 之后由 `sqlite3PagerSnapshotGet` 另行抓取的。
   - **开的是只读事务**：`sqlite3BtreeBeginTrans(pBt, 0, 0)` 的第二参数 `wrflag` 为 0。`src/btree.c` 的 `sqlite3BtreeBeginTrans` 判定 `if( p->sharable || p->inTrans==TRANS_NONE || (p->inTrans==TRANS_READ && wrflag!=0) )` 才真正调 `btreeBeginTrans`；`wrflag==0` 表示只读。第三个参数 `pSchemaVersion` 传 0，表示不取回 schema 版本号。

10. `sqlite3PagerSnapshotOpen(pPager, 0)` 把标志复位（传入 0/NULL 后，`src/wal.c` 的 `sqlite3WalSnapshotOpen` 走 else 分支，把 `pWal->pSnapshot` 置 NULL、`bGetSnapshot` 置 0）。
11. 若 `rc==SQLITE_OK`，调 `sqlite3PagerSnapshotGet(sqlite3BtreePager(pBt), ppSnapshot)` 真正抓取快照句柄。
12. `sqlite3_mutex_leave(db->mutex)`，返回 `rc`。

### 2. 取快照的分页器层：`sqlite3PagerSnapshotGet`（`src/pager.c`）

`src/pager.c` 的 `sqlite3PagerSnapshotGet` 函数体极短：初值 `rc = SQLITE_ERROR`；`if( pPager->pWal )` 则 `rc = sqlite3WalSnapshotGet(pPager->pWal, ppSnapshot)`，否则保持 `SQLITE_ERROR`。**非 WAL 库在这里直接失败**——分页器层用 `pPager->pWal` 是否为空来判断「这是不是一个 WAL 数据库」。

`src/wal.c` 的 `sqlite3WalSnapshotGet` 做三件事：

1. `assert( pWal->readLock>=0 && pWal->writeLock==0 )`——必须在读事务内、且没有写锁。
2. **空 WAL 的判定**：`static const u32 aZero[4] = { 0, 0, 0, 0 };` 然后 `if( memcmp(&pWal->hdr.aFrameCksum[0],aZero,16)==0 )` —— `aFrameCksum` 是 `u32[2]`，共 8 字节，`aZero` 是 `u32[4]` 共 16 字节，比较的是前 16 字节中属于 `aFrameCksum` 的 8 字节加上紧随其后的 `aSalt[2]`（`WalIndexHdr` 字段顺序：`mxFrame`、`nPage`、`aFrameCksum[2]`、`aSalt[2]`）。校验和与 salt 全零表示这个 WAL 还从未写入过任何帧，此时 `*ppSnapshot = 0; return SQLITE_ERROR;`。这正对应 `src/sqlite.h.in` 中「必须至少有一个事务写入当前 WAL 文件后才能取快照」这条要求。
3. 分配 `pRet = (WalIndexHdr*)sqlite3_malloc(sizeof(WalIndexHdr))`，失败返回 `SQLITE_NOMEM_BKPT`；成功则 `memcpy(pRet, &pWal->hdr, sizeof(WalIndexHdr))` 并把 `*ppSnapshot = (sqlite3_snapshot*)pRet`。

### 3. 校验快照是否仍可用：`sqlite3PagerSnapshotCheck`（`src/pager.c`）

`src/pager.c` 的 `sqlite3PagerSnapshotCheck` 与 `sqlite3PagerSnapshotGet` 同一个形状：`if( pPager->pWal )` 则转发给 `sqlite3WalSnapshotCheck(pPager->pWal, pSnapshot)`，否则返回 `SQLITE_ERROR`。

`src/wal.c` 的 `sqlite3WalSnapshotCheck` 的函数体（注释已在 `src/wal.c` 中写明）：

1. `rc = walLockShared(pWal, WAL_CKPT_LOCK)`——取 CHECKPOINTER 槽的共享锁。`src/wal.c` 中 `WAL_CKPT_LOCK` 定义为 1，`WAL_WRITE_LOCK` 为 0，`WAL_RECOVER_LOCK` 为 2，`WAL_READ_LOCK(I)` 为 `3+(I)`；`SQLITE_SHM_NLOCK` 在 `src/sqlite.h.in` 中定义为 8。
2. 取锁成功后，比较两项：`memcmp(pNew->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))` 非零，**或** `pNew->mxFrame < walCkptInfo(pWal)->nBackfillAttempted`——任一成立即 `rc = SQLITE_ERROR_SNAPSHOT;` 并 `walUnlockShared(pWal, WAL_CKPT_LOCK)` 立即释放锁。
3. 整个比较块包在 `SEH_TRY { ... } SEH_EXCEPT( rc = walHandleException(pWal); )` 中，异常路径由 `walHandleException` 兜底。

调用方在 `src/main.c` 的 `sqlite3_snapshot_open` 里：只有当 `sqlite3BtreeTxnState(pBt)!=SQLITE_TXN_NONE`（已有读事务）且 `db->nVdbeActive==0`（没有活跃语句）时才调 `sqlite3PagerSnapshotCheck`；校验通过则先 `sqlite3BtreeCommit(pBt)` 把旧读事务提交掉，再 `sqlite3PagerSnapshotOpen(pPager, pSnapshot)` 挂上目标快照，然后 `sqlite3BtreeBeginTrans(pBt, 0, 0)` 以该快照重开读事务，最后 `sqlite3PagerSnapshotUnlock(pPager)` 释放校验时取到的 CHECKPOINTER 共享锁（`src/wal.c` 的 `sqlite3WalSnapshotUnlock` 函数体是 `walUnlockShared(pWal, WAL_CKPT_LOCK);`）。

### 4. 从 WAL 文件恢复历史快照：`sqlite3PagerSnapshotRecover`（`src/pager.c`）

`src/pager.c` 的 `sqlite3PagerSnapshotRecover` 同样是转发壳：`if( pPager->pWal )` 则 `rc = sqlite3WalSnapshotRecover(pPager->pWal)`，否则 `rc = SQLITE_ERROR`。

`src/wal.c` 的 `sqlite3WalSnapshotRecover` 的实现（其注释把算法说得很清楚）：

1. `assert( pWal->readLock>=0 )`；`rc = walLockExclusive(pWal, WAL_CKPT_LOCK, 1)` 取 CHECKPOINTER 槽的排他锁。
2. 分配两个 `pWal->szPage` 大小的缓冲 `pBuf1`、`pBuf2`，任一为 0 则 `rc = SQLITE_NOMEM`。
3. 置 `pWal->ckptLock = 1`，在 `SEH_TRY` 中调 `walSnapshotRecover(pWal, pBuf1, pBuf2)`，异常时 `rc = SQLITE_IOERR_IN_PAGE`；随后 `pWal->ckptLock = 0`。
4. 释放两个缓冲，`walUnlockExclusive(pWal, WAL_CKPT_LOCK, 1)`。

`src/wal.c` 的 `walSnapshotRecover` 的循环体是真正的恢复算法：从 `pInfo->nBackfillAttempted` 递减到 `AtomicLoad(&pInfo->nBackfill)`，对每个帧 i：

- `walHashGet(pWal, walFramePage(i), &sLoc)` 找到该帧对应的页号 `pgno = sLoc.aPgno[i-sLoc.iZero-1]`；
- 计算数据库文件内偏移 `iDbOff = (i64)(pgno-1) * szPage`，WAL 内偏移 `iWalOff = walFrameOffset(i, szPage) + WAL_FRAME_HDRSIZE`；
- 若 `iDbOff+szPage<=szDb`（该页已在库文件里），先从 WAL 读 `pBuf1`、再从库文件读 `pBuf2`，**若两次读都成功且 `0==memcmp(pBuf1, pBuf2, szPage)`（两者内容完全相同），就 `break`**；
- 否则执行 `pInfo->nBackfillAttempted = i-1;`——即把「已尝试回填」的帧号往下调一格。

注释说明了 break 的含义：某帧的内容与库文件里对应页**已经一致**时，说明这一帧的信息早已落进库文件，再往前推进 `nBackfillAttempted` 是安全的。注释同时明确指出这套做法「只在文件系统保证早期 checkpoint 的页写入是原子操作时才真正安全，而这一点并不总是成立」，且 `nBackfillAttempted` 也可能被留在比预期更大的值——因为某一帧的内容可能与同页的较早版本重复。**恢复失败不是错误**：`src/wal.c` 的注释写明「nBackfillAttempted 无法减小时不算错误」。

调用方 `src/main.c` 的 `sqlite3_snapshot_recover`：先 `sqlite3FindDbName` 找库，再要求 `SQLITE_TXN_NONE==sqlite3BtreeTxnState(pBt)`（**必须没有事务在进行**），然后 `sqlite3BtreeBeginTrans(pBt, 0, 0)` 开只读事务 → `sqlite3PagerSnapshotRecover(sqlite3BtreePager(pBt))` → `sqlite3BtreeCommit(pBt)` 收尾。

### 5. 快照期间的模式变更与写入拒绝

这一侧的拒绝发生在两个不同层面。

**（a）写事务被拒绝：`SQLITE_BUSY_SNAPSHOT`**

`src/wal.c` 的 `sqlite3WalBeginWriteTransaction` 在拿到 `WAL_WRITE_LOCK` 之后比较 `memcmp(&pWal->hdr, (void *)walIndexHdr(pWal), sizeof(WalIndexHdr))`：若当前连接持有的 WAL 头与 wal-index 中的最新头不同，说明**别的连接在本连接读事务开始之后写过库**，此时 `rc = SQLITE_BUSY_SNAPSHOT;` 并释放写锁、置 `pWal->writeLock = 0`。该常量在 `src/sqlite.h.in` 定义为 `(SQLITE_BUSY | (2<<8))`。

`src/btree.c` 的 `btreeBeginTrans` 收到这个码后有一层转换：`else if( rc==SQLITE_BUSY_SNAPSHOT && pBt->inTransaction==TRANS_NONE )` 则 `rc = SQLITE_BUSY;`——只有在**调用时根本没有事务**的情况下才降级成普通的 `SQLITE_BUSY`；已经有事务时保留 `SQLITE_BUSY_SNAPSHOT` 以便调用方区分。

**（b）模式变更被拒绝**

`src/btree.c` 的 `btreeBeginTrans` 在开启写事务时先检查 `(pBt->btsFlags & BTS_READ_ONLY)!=0`，是则 `rc = SQLITE_READONLY;`，否则才调 `sqlite3PagerBegin(pPager, wrflag>1, sqlite3TempInMemory(p->db))`。**架构推论：** 快照读事务（`inTrans==TRANS_READ`）在 `src/btree.c` 的 `sqlite3BtreeBeginTrans` 判定 `p->inTrans==TRANS_READ && wrflag!=0` 时会被升级为写事务，此时若另一连接持锁则由上面的 `SQLITE_BUSY_SNAPSHOT` 路径拒绝；而对模式（schema）的实际改写由 DDL 语句走 `OP_Transaction` 的写事务路径，因此模式变更被拒绝的落点是同一个写事务获取点。但 `src/btree.c` 中并没有一条专门以「存在快照」为条件的模式变更拒绝分支——本页不对该点做超出源码的断言。

### 6. 快照在 WAL 读事务建立时的应用

`src/wal.c` 的 `walBeginReadTransaction` 有一段 `SQLITE_ENABLE_SNAPSHOT` 专有逻辑：

- 若 `pWal->pSnapshot` 非空，先比较 `memcmp(pSnapshot, &pWal->hdr, sizeof(WalIndexHdr))`，不等则置 `bChanged = 1`。
- 然后取 CHECKPOINTER 槽的共享锁：`walEnableBlocking(pWal); rc = walLockShared(pWal, WAL_CKPT_LOCK); walDisableBlocking(pWal);`。源码注释解释了原因：并发运行的 checkpointer 可能已经决定要 checkpoint 一个比 `pSnapshot` 更靠后的快照 X，却还没来得及把 `pInfo->nBackfillAttempted` 设上去——取共享 CKPT 锁可以消除这个竞态窗口。
- 随后调 `walTryBeginRead` 取得读锁（循环直到返回值不再是 `WAL_RETRY`）。
- 读事务建好后，若 `pSnapshot` 与 `pWal->hdr` 不等，做两项验证：`!memcmp(pSnapshot->aSalt, pWal->hdr.aSalt, sizeof(pWal->hdr.aSalt))` 且 `pSnapshot->mxFrame>=pInfo->nBackfillAttempted`。注释列出了两条快照失效的原因：**(1)** WAL 文件在取快照之后被重置（此时 salt 已变）；**(2)** 曾经发生过一次 checkpoint，把 `pSnapshot->mxFrame` 之后的帧写进了数据库文件——**注释特别指出该 checkpoint 不需要完成就会造成问题**。
- 两项验证通过则 `memcpy(&pWal->hdr, pSnapshot, sizeof(WalIndexHdr))`，即把 WAL 头回退成快照的内容；否则 `rc = SQLITE_ERROR_SNAPSHOT;`，并且 `sqlite3WalEndReadTransaction(pWal)` 结束刚开好的读事务。
- 随后无条件设置 `pWal->minFrame = 1;`。注释说明原因：使用非当前快照的客户端不能忽略 WAL 文件开头的任何帧，因为当 `minFrame < iSnapshot < maxFrame` 时，checkpointer 可能因为存在 `iSnapshot` 之后一个同页帧而漏掉 checkpoint 早于 `minFrame` 的帧。

另一处相关逻辑在 `src/wal.c` 的 `walTryBeginRead` 开头：条件 `AtomicLoad(&pInfo->nBackfill)==pWal->hdr.mxFrame && ((pWal->bGetSnapshot==0 && pWal->pSnapshot==0) || pWal->hdr.mxFrame==0)` 成立时才走「WAL 已被完全回填、可以忽略」的 `WAL_READ_LOCK(0)` 快路径。**这正是前面 `sqlite3_snapshot_get` 里 `bGetSnapshot` 标志存在的意义**：取了快照的连接不允许走 0 号读锁快路径，否则快照会被后续写者摧毁。

还有一处：`src/wal.c` 的 `walTryBeginRead` 在选 `aReadMark[]` 时，若 `pWal->pSnapshot && pWal->pSnapshot->mxFrame<mxFrame` 则把 `mxFrame` 压到 `pWal->pSnapshot->mxFrame`，即**快照读事务只能看到快照帧号及以前的帧**。

### 7. 快照的比较与释放

`src/wal.c` 的 `sqlite3_snapshot_cmp` 按固定次序比较两个快照：先比 `aSalt[0]`（`src/wal.c` 注释指出它每次 WAL 重启就自增），相等则比 `mxFrame`；返回正/负/0 表示新旧关系。`src/main.c` 的 `sqlite3_snapshot_free` 函数体只有一句 `sqlite3_free(pSnapshot);`。

## 关键取舍

- **快照只存在于 WAL 模式（源码事实）**：`src/main.c` 的三个 `sqlite3_snapshot_*` 接口整段包在 `#ifndef SQLITE_OMIT_WAL` 里；`src/pager.c` 的三个 `sqlite3PagerSnapshot*` 一律先判 `if( pPager->pWal )`，非 WAL 库一律返回 `SQLITE_ERROR`。代价是回滚日志模式的用户完全拿不到这套 API。
- **快照对象不透明且由 WAL 层独占（源码事实）**：`src/sqlite.h.in` 把 `sqlite3_snapshot` 定义成 `unsigned char hidden[48]`，实际内容由 `src/wal.c` 填入 `WalIndexHdr`。上层（`src/main.c`）只当指针传递，不解析任何字段。代价是快照的可用性判断必须逐层下放到 WAL 层，跨层无法在 `src/main.c` 侧完成。
- **用 salt + mxFrame 两个量判定有效性，而不是版本号（源码事实）**：`src/wal.c` 的 `sqlite3WalSnapshotCheck` 只比较 `aSalt` 与 `mxFrame>=nBackfillAttempted`。前者识别「WAL 被重置」，后者识别「帧已被 checkpoint 覆盖」。代价是快照的有效性依赖 WAL 未被回绕、且 checkpoint 未越过它，一旦越过只能靠 `sqlite3_snapshot_recover` 尝试回退而不能保证成功。
- **判定用 `nBackfillAttempted` 而非 `nBackfill`（源码事实）**：`src/wal.c` 的注释明确 `nBackfillAttempted` 在回填开始前就被设上，`nBackfill` 只在全部回填完成后才设上。用前者是比较保守的一侧，能覆盖「checkpoint 正在写但没写完」的窗口；代价是快照的存活期比理论最优更短——即使 checkpoint 最终会回滚，已被标记为 attempted 的帧也不再能支撑旧快照。
- **`iVersion==0` 被征用为「这不是真快照」的标记（源码事实）**：`src/main.c` 的 `sqlite3_snapshot_get` 用一个清零的 `i64 dummy` 强转成 `sqlite3_snapshot*` 传给 `sqlite3PagerSnapshotOpen`，`src/wal.c` 的 `sqlite3WalSnapshotOpen` 据此判定 `iVersion==0` 并置 `bGetSnapshot=1`。源码注释说明 `bGetSnapshot` 的用途是让读事务不去取 0 号读锁。代价是这个字段承担了双重语义（wal-index 版本号 + 快照标记），读懂 `sqlite3_snapshot_get` 必须先读 `src/wal.c` 才能理解。
- **取快照要临时开一次读事务再抓（源码事实）**：`src/main.c` 的 `sqlite3_snapshot_get` 先 `sqlite3PagerSnapshotOpen(pPager, &dummy)` → `sqlite3BtreeBeginTrans(pBt, 0, 0)` → `sqlite3PagerSnapshotOpen(pPager, 0)` → `sqlite3PagerSnapshotGet(...)`。代价是调用一次 `sqlite3_snapshot_get` 会短暂改变连接的事务状态（开一个只读事务再由后续流程收尾），这在显式事务中表现为事务起点被推进。
- **前置条件收得极紧（源码事实）**：`db->autoCommit==0`（必须在显式事务中）、`iDb==0 || iDb>1`（排除 `temp` 库与不存在的库）、`SQLITE_TXN_WRITE!=sqlite3BtreeTxnState(pBt)`（该库上不能有写事务）、WAL 必须已写入过帧（`aFrameCksum`/`aSalt` 全零判定）。任一不满足一律返回 `SQLITE_ERROR`。代价是调用方必须自己保证所有前置条件，否则只会拿到一个语焉不详的 `SQLITE_ERROR`。
- **快照校验与快照使用分成两步（源码事实）**：`src/main.c` 的 `sqlite3_snapshot_open` 先用 `sqlite3PagerSnapshotCheck` 校验并取 CHECKPOINTER 共享锁，再 `sqlite3BtreeCommit(pBt)` 结束旧读事务、`sqlite3PagerSnapshotOpen` 挂载、`sqlite3BtreeBeginTrans(pBt, 0, 0)` 重开，最后 `sqlite3PagerSnapshotUnlock` 释放校验锁。代价是校验与使用之间存在窗口——`src/wal.c` 的 `walBeginReadTransaction` 因此又做了一次同样的 salt + `nBackfillAttempted` 比较，二次校验失败时返回 `SQLITE_ERROR_SNAPSHOT` 并 `sqlite3WalEndReadTransaction` 撤销刚开的读事务。
- **恢复是尽力而为而非保证（源码事实）**：`src/wal.c` 的 `walSnapshotRecover` 逐帧比较 WAL 内容与库文件内容，只有**完全相同**（`0==memcmp`）才继续下调 `nBackfillAttempted`；源码注释明确写出这套做法只在文件系统保证早期 checkpoint 页写入为原子操作时才真正安全，且 `nBackfillAttempted` 可能被留在比预期更大的值。代价是恢复后仍可能有一些历史快照打不开。
- **写冲突降级为 `SQLITE_BUSY`（源码事实）**：`src/btree.c` 的 `btreeBeginTrans` 只在 `pBt->inTransaction==TRANS_NONE` 时把 `SQLITE_BUSY_SNAPSHOT` 转成 `SQLITE_BUSY`；有事务时保留原码。代价是调用方要区分「快照冲突」与「普通锁忙」必须看扩展码，而不是只看主码。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/main.c` | `sqlite3_snapshot_get` | 取快照主干：校验自动提交/库下标/事务状态，临时开只读事务后抓取快照句柄 |
| `src/main.c` | `sqlite3_snapshot_open` | 挂载指定快照：先 `sqlite3PagerSnapshotCheck` 校验，再提交旧事务、挂快照、重开只读事务、释放校验锁 |
| `src/main.c` | `sqlite3_snapshot_recover` | 要求无事务，`sqlite3BtreeBeginTrans` 后调 `sqlite3PagerSnapshotRecover` 再 `sqlite3BtreeCommit` |
| `src/main.c` | `sqlite3_snapshot_free` | `sqlite3_free(pSnapshot)` 释放快照句柄 |
| `src/main.c` | `sqlite3_snapshot_cmp` 声明处 | 公开 API 声明与语义说明位于 `src/sqlite.h.in` |
| `src/pager.c` | `sqlite3PagerSnapshotGet` | 非 WAL 库返回 `SQLITE_ERROR`，否则转发 `sqlite3WalSnapshotGet` |
| `src/pager.c` | `sqlite3PagerSnapshotCheck` | 非 WAL 库返回 `SQLITE_ERROR`，否则转发 `sqlite3WalSnapshotCheck` |
| `src/pager.c` | `sqlite3PagerSnapshotRecover` | 非 WAL 库返回 `SQLITE_ERROR`，否则转发 `sqlite3WalSnapshotRecover` |
| `src/pager.c` | `sqlite3PagerSnapshotOpen` | 把快照指针交给 WAL 层，`pPager->pWal` 为空时返回 `SQLITE_ERROR` |
| `src/pager.c` | `sqlite3PagerSnapshotUnlock` | 释放校验时取得的 CHECKPOINTER 共享锁 |
| `src/wal.c` | `sqlite3WalSnapshotGet` | 校验 WAL 帧校验和/salt 非全零，分配并 `memcpy` 一份 `WalIndexHdr` 作为快照 |
| `src/wal.c` | `sqlite3WalSnapshotOpen` | 按 `iVersion==0` 区分「置 `bGetSnapshot`」与「挂载 `pSnapshot`」两条路径 |
| `src/wal.c` | `sqlite3WalSnapshotCheck` | 取 `WAL_CKPT_LOCK` 共享锁，比较 salt 与 `mxFrame>=nBackfillAttempted`，失效返回 `SQLITE_ERROR_SNAPSHOT` |
| `src/wal.c` | `sqlite3WalSnapshotUnlock` | `walUnlockShared(pWal, WAL_CKPT_LOCK)` |
| `src/wal.c` | `sqlite3WalSnapshotRecover` | 取 `WAL_CKPT_LOCK` 排他锁后调 `walSnapshotRecover`，异常按 `SQLITE_IOERR_IN_PAGE` 处理 |
| `src/wal.c` | `walSnapshotRecover` | 逐帧比较 WAL 与库文件内容，据此下调 `nBackfillAttempted` |
| `src/wal.c` | `walBeginReadTransaction` | 挂载快照时取 CKPT 共享锁消除竞态，二次校验 salt/`nBackfillAttempted`，并强制 `minFrame = 1` |
| `src/wal.c` | `walTryBeginRead` | 按 `bGetSnapshot`/`pSnapshot` 决定是否允许 0 号读锁快路径，并把读帧上限压到 `pSnapshot->mxFrame` |
| `src/wal.c` | `sqlite3WalBeginWriteTransaction` | WAL 头与 wal-index 头不一致时返回 `SQLITE_BUSY_SNAPSHOT` |
| `src/wal.c` | `WalIndexHdr` | 快照承载的 WAL 索引头结构：`iVersion`/`mxFrame`/`aSalt[2]`/`aFrameCksum[2]` |
| `src/wal.c` | `sqlite3_snapshot_cmp` | 按 `aSalt[0]` 再按 `mxFrame` 比较两个快照的新旧 |
| `src/btree.c` | `sqlite3BtreeTxnState` | 返回 `p->inTrans`，供上层判定 `SQLITE_TXN_NONE`/`READ`/`WRITE` |
| `src/btree.c` | `sqlite3BtreeBeginTrans` | `wrflag==0` 开只读事务；`wrflag!=0` 时按已有事务状态决定升级 |
| `src/btree.c` | `sqlite3BtreePager` | `return p->pBt->pPager;`，取分页器句柄 |
| `src/btree.c` | `btreeBeginTrans` | 把 `SQLITE_BUSY_SNAPSHOT` 在无事务时降级为 `SQLITE_BUSY` |
| `src/build.c` | `sqlite3FindDbName` | 按库名查 `db->aDb[]` 下标，找不到返回 -1，`"main"` 恒为别名 |
| `src/sqlite.h.in` | `sqlite3_snapshot` | 不透明快照结构体，定义为 `unsigned char hidden[48]` |
| `src/sqlite.h.in` | `SQLITE_ERROR_SNAPSHOT` | 快照已被 checkpoint 覆盖的错误码，定义为 `(SQLITE_ERROR \| (3<<8))` |
| `src/sqlite.h.in` | `SQLITE_BUSY_SNAPSHOT` | 读事务期间库被他人写入的错误码，定义为 `(SQLITE_BUSY \| (2<<8))` |
| `src/sqlite.h.in` | `SQLITE_TXN_NONE` / `SQLITE_TXN_READ` / `SQLITE_TXN_WRITE` | 事务状态三值，分别定义为 0 / 1 / 2 |
| `src/btreeInt.h` | `TRANS_NONE` / `TRANS_READ` / `TRANS_WRITE` | Btree 内部同值常量，用编译期断言与 `SQLITE_TXN_*` 对齐 |
| `src/wal.c` | `WAL_CKPT_LOCK` / `WAL_READ_LOCK(I)` / `WAL_NREADER` | WAL 槽位常量，分别定义为 1 / `3+(I)` / `SQLITE_SHM_NLOCK-3` |
| `src/sqlite.h.in` | `SQLITE_SHM_NLOCK` | 共享内存锁槽总数，定义为 8 |
| `src/pager.h` | 快照相关声明 | `sqlite3PagerSnapshot*` 五个函数在 `SQLITE_ENABLE_SNAPSHOT` 下声明 |
| `src/wal.h` | 快照相关声明 | `sqlite3WalSnapshot*` 五个函数在 `SQLITE_ENABLE_SNAPSHOT` 下声明 |

## 与主核 / AI 的关系

**架构推论：** 主核若要做「历史版本读」，可以照搬 SQLite 的三段式判定——「WAL 是否被重置」（salt 比对）+「目标帧是否已被回填」（`mxFrame >= nBackfillAttempted`）+「WAL 是否有任何帧」（校验和/salt 非全零）。这三个判据都与具体的存储格式无关，只依赖「日志有单调递增的帧号」与「回填有单调递增的水位」这两个前提，主核的多版本实现同样具备。

对本项目而言，本页可复用的**判定口径**是：`nBackfillAttempted`（已尝试回填水位）而非 `nBackfill`（已完成回填水位）才是与读者安全相关的那一个，主核若有 WAL 与 checkpoint 等价物，版本可见性的判定必须用「已尝试」这一侧。这与主核的 WAL/检查点实现是否存在，需要对照主核源码核实，不能由本页推定。

对 AI 协作而言，本页提供的是**读源码得到的断言**：`sqlite3_snapshot_get` 的四个前置条件（`db->autoCommit==0`、库下标非 1、`sqlite3BtreeTxnState` 非 `SQLITE_TXN_WRITE`、WAL 已写入过帧）与 `SQLITE_ERROR_SNAPSHOT` / `SQLITE_BUSY_SNAPSHOT` 两个扩展错误码的确切出处。评审时都应回到 `src/main.c`、`src/pager.c`、`src/wal.c` 的具体判断分支核对，而不是凭 SQLite 的通用印象推断。

## 参考

- [SQLite 官方文档：sqlite3_snapshot_get](https://sqlite.org/c3ref/snapshot_get.html) —— 说明取快照的四个前置条件与 `SQLITE_ERROR` 的返回约定。
- [SQLite 官方文档：sqlite3_snapshot_open](https://sqlite.org/c3ref/snapshot_open.html) —— 说明打开历史快照的自动提交要求、活跃语句限制与 `SQLITE_ERROR_SNAPSHOT` 的触发条件。
- [SQLite 官方文档：sqlite3_snapshot_recover](https://sqlite.org/c3ref/snapshot_recover.html) —— 说明从残留 WAL 文件恢复历史快照的接口与前提。
- [SQLite 官方文档：sqlite3_snapshot_free](https://sqlite.org/c3ref/snapshot_free.html) —— 说明快照句柄的释放接口与 `SQLITE_ENABLE_SNAPSHOT` 编译开关要求。
- [SQLite 官方文档：sqlite3_snapshot_cmp](https://sqlite.org/c3ref/snapshot_cmp.html) —— 说明两个快照句柄的新旧比较规则与失效条件。
- [SQLite 官方源码：`src/wal.c`](https://github.com/sqlite/sqlite/blob/master/src/wal.c) —— 支撑 `WalIndexHdr` 字段含义、`sqlite3WalSnapshotGet`/`Check`/`Recover` 的判定与 `walBeginReadTransaction` 的快照应用路径。
- [SQLite 官方源码：`src/pager.c`](https://github.com/sqlite/sqlite/blob/master/src/pager.c) —— 支撑 `sqlite3PagerSnapshotGet`/`Check`/`Recover` 的 WAL 判定与转发行为。
- [SQLite 官方源码：`src/main.c`](https://github.com/sqlite/sqlite/blob/master/src/main.c) —— 支撑 `sqlite3_snapshot_get` 的前置条件、临时读事务与 `bGetSnapshot` 的配合。
- [SQLite 官方文档：WAL 模式](https://sqlite.org/wal.html) —— 提供 WAL 与快照的机制背景，行为细节以源码为准。
