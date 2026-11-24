---
project: sqlite
feature: wal-mode-and-index
page_type: feature
schema_version: 1
refs:
  - path: src/wal.c
    symbols: [sqlite3WalOpen, sqlite3WalClose, sqlite3WalBeginReadTransaction]
  - path: src/pager.c
    symbols: [sqlite3PagerOpenWal, sqlite3PagerCloseWal]
---

# WAL 模式与索引文件

## 一句话职责

该机制负责 WAL 模式的开启与关闭、WAL 文件本身以及其共享内存索引（wal-index，即 `-shm` 文件）的建立与映射；帧的写入与读回（h02）、帧回写进数据库并截断 WAL 的检查点（h03）不在本页范围内。

## 机制是什么

WAL 模式下，一个数据库在盘上由三个文件描述：主库文件本身、`-wal` 预写日志文件、以及 `-shm` 共享内存索引。`-shm` 不是真的「读文件」——每个连接把它 mmap 进来当作共享内存，用来在多进程之间协调访问、并作为「帧号 → 数据库页号」映射的缓存。`-shm` 里不含数据库内容，也不是崩溃恢复所必需的，因此它从不落盘 fsync。

wal-index 的内存布局由 `wal.c` 中的一组常量钉死：`WalIndexHdr` 是 48 字节（`assert( 48 == sizeof(WalIndexHdr) )`），`WalCkptInfo` 是 40 字节（`assert( 40 == sizeof(WalCkptInfo) )`）。头部总长 `WALINDEX_HDR_SIZE` 为 136 字节（`assert( 136 == WALINDEX_HDR_SIZE )`），其构成是两份 `WalIndexHdr` 拷贝加一份 `WalCkptInfo`。8 个锁字节从 `WALINDEX_LOCK_OFFSET` 起算，该常量是 120（`assert( 120 == WALINDEX_LOCK_OFFSET )`），由 `sizeof(WalIndexHdr)*2+offsetof(WalCkptInfo,aLock)` 算出。

索引页大小 `WALINDEX_PGSZ` 由 `sizeof(ht_slot)*HASHTABLE_NSLOT + HASHTABLE_NPAGE*sizeof(u32)` 算出，等于 32768 字节（`assert( 32768 == WALINDEX_PGSZ )`）。其中 `HASHTABLE_NPAGE` 为 4096（必须是 2 的幂），`HASHTABLE_NSLOT` 为 `HASHTABLE_NPAGE*2`，即 8192；`HASHTABLE_HASH_1` 为 383。首个索引块被头部挖掉一段，只容纳 `HASHTABLE_NPAGE_ONE`（4062）个页号槽。

锁的编号也写死在常量里：`WAL_WRITE_LOCK` 为 0，`WAL_CKPT_LOCK` 为 1，`WAL_RECOVER_LOCK` 为 2，`WAL_READ_LOCK(I)` 为 `3+(I)`。读者锁的数量 `WAL_NREADER` 定义为 `SQLITE_SHM_NLOCK-3`，在默认 `SQLITE_SHM_NLOCK` 为 8 时是 5（`assert( 5 == WAL_NREADER )`）。

## 怎么实现的

### 打开：PAGER 层先判能力，再交给 WAL 层

`sqlite3PagerOpenWal` 是 Pager 层对外的入口。它先排除两类「无需动作」的情形：临时库（`pPager->tempFile`）和已经打开过 WAL 的连接（`pPager->pWal`），命中这两种情况时把 `*pbOpen` 置 1 直接返回 `SQLITE_OK`。其余情况下先做能力检查——`sqlite3PagerWalSupported` 的判定条件是：非 `noLock`、且（处于 `exclusiveMode` 或 VFS 的 `pMethods->iVersion>=2` 且存在 `xShmMap`）。不满足时返回 `SQLITE_CANTOPEN`，不进入 WAL 模式。随后无条件调 `sqlite3OsClose(pPager->jfd)` 关掉回滚日志句柄，再调用内部的 `pagerOpenWal`；成功时把 `pPager->journalMode` 设为 `PAGER_JOURNALMODE_WAL`，`pPager->eState` 置为 `PAGER_OPEN`。

`pagerOpenWal` 是真正的建连处：若 `pPager->exclusiveMode` 为真，先取数据库文件的 EXCLUSIVE 锁（`pagerExclusiveLock`），使「独占模式下用堆内存代替共享内存」的安排安全成立；然后调用 `sqlite3WalOpen(pPager->pVfs, pPager->fd, pPager->zWal, pPager->exclusiveMode, pPager->journalSizeLimit, &pPager->pWal)`，最后调 `pagerFixMaplimit`。

已存在 WAL 文件的库在打开时也走这条路：`pagerOpenWalIfPresent` 先用 `sqlite3OsAccess` 判断 `-wal` 是否存在，存在且数据库非空（`pagerPagecount` 得到的 `nPage` 不为 0）时调用 `sqlite3PagerOpenWal(pPager, 0)`；若 `nPage` 为 0 则删掉 `-wal`；若 `-wal` 不存在但 `journalMode` 已是 `PAGER_JOURNALMODE_WAL`，则改回 `PAGER_JOURNALMODE_DELETE`。

### `sqlite3WalOpen`：分配 Wal 对象并打开日志文件

`sqlite3WalOpen` 用 `sqlite3MallocZero(sizeof(Wal) + pVfs->szOsFile)` 分配内存，失败返回 `SQLITE_NOMEM_BKPT`。`pRet->pWalFd` 被安置在这块多分配内存的尾部（`(sqlite3_file *)&pRet[1]`），`readLock` 初始化为 -1 表示未持有任何读者锁。

日志文件以 `SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE|SQLITE_OPEN_WAL` 打开。若 VFS 回填的 `flags` 带 `SQLITE_OPEN_READONLY`，则置 `pRet->readOnly = WAL_RDONLY`。

模式选择在这里定死：`pRet->exclusiveMode = (bNoShm ? WAL_HEAPMEMORY_MODE: WAL_NORMAL_MODE)`。`WAL_HEAPMEMORY_MODE` 为 2、`WAL_NORMAL_MODE` 为 0、`WAL_EXCLUSIVE_MODE` 为 1。`bNoShm` 来自 `pPager->exclusiveMode`，即 `PRAGMA locking_mode=EXCLUSIVE` 下的连接——此时不用共享内存，索引改为堆内存。

打开成功后按设备能力调整两个标志：若 `sqlite3OsDeviceCharacteristics(pDbFd)` 带 `SQLITE_IOCAP_SEQUENTIAL` 则 `syncHeader` 置 0；带 `SQLITE_IOCAP_POWERSAFE_OVERWRITE` 则 `padToSectorBoundary` 置 0。失败时调 `walIndexClose(pRet, 0)`、关闭 `pWalFd` 并释放 `pRet`，`*ppWal` 保持未修改。

函数开头还有一整块 `assert`，逐条钉住所有会改变 `-shm` 磁盘格式的常量：`sizeof(WalIndexHdr)` 为 48、`sizeof(WalCkptInfo)` 为 40、`WALINDEX_LOCK_OFFSET` 为 120、`WALINDEX_HDR_SIZE` 为 136、`HASHTABLE_NPAGE` 为 4096、`HASHTABLE_NPAGE_ONE` 为 4062、`HASHTABLE_NSLOT` 为 8192、`HASHTABLE_HASH_1` 为 383、`WALINDEX_PGSZ` 为 32768、`SQLITE_SHM_NLOCK` 为 8、`WAL_NREADER` 为 5、`WAL_FRAME_HDRSIZE` 为 24、`WAL_HDRSIZE` 为 32，以及 8 个锁字节的绝对偏移 `WALINDEX_LOCK_OFFSET + WAL_WRITE_LOCK` 为 120、+`WAL_CKPT_LOCK` 为 121、+`WAL_RECOVER_LOCK` 为 122、+`WAL_READ_LOCK(0..4)` 为 123..127。

### 索引的映射：`walIndexPage` 与两条来源

索引页的实际取用走 `walIndexPage`，它先看缓存：`pWal->nWiData<=iPage || (*ppPage = pWal->apWiData[iPage])==0` 成立时才落到 `walIndexPageRealloc`。

`walIndexPageRealloc` 处理两件事。其一是按需扩容指针数组 `pWal->apWiData`：用 `sqlite3Realloc` 重分配 `sizeof(u32*)*(1+(i64)iPage)` 字节，失败则把 `*ppPage` 置 0 并返回 `SQLITE_NOMEM_BKPT`；成功后把新增区间清零并更新 `pWal->nWiData = iPage+1`。其二是按模式取页：处于 `WAL_HEAPMEMORY_MODE` 时用 `sqlite3MallocZero(WALINDEX_PGSZ)` 在堆上分配，分配失败返回 `SQLITE_NOMEM_BKPT`；否则调用 `sqlite3OsShmMap(pWal->pDbFd, iPage, WALINDEX_PGSZ, pWal->writeLock, ...)` 让 VFS 做共享内存映射。

映射失败时还有一条降级分支值得注意：若返回码低 8 位是 `SQLITE_READONLY`，则置 `pWal->readOnly |= WAL_SHM_RDONLY`；若恰好等于 `SQLITE_READONLY`（不是更具体的错误），则把错误码改写成 `SQLITE_OK`——也就是把「共享内存只读」当作可继续的状态而非错误。

`walIndexHdr` 返回首块首地址（`(volatile WalIndexHdr*)pWal->apWiData[0]`），`walCkptInfo` 返回首块内偏移 `sizeof(WalIndexHdr)/2` 处的 `WalCkptInfo`，也就是两份头之后的那份检查点信息。

### 读事务：`sqlite3WalBeginReadTransaction` 与快照

`sqlite3WalBeginReadTransaction` 是给 Pager 层用的公开入口，它本身只做异常包装：把真正的逻辑委托给 `walBeginReadTransaction`，并用 `SEH_TRY` / `SEH_EXCEPT` 兜住访问 mmap 文件时抛出的结构化异常，异常路径上由 `walHandleException` 处理。

`walBeginReadTransaction` 反复调用 `walTryBeginRead`，直到返回值不再是 `WAL_RETRY`（`WAL_RETRY` 定义为 -1）。`walTryBeginRead` 里的重试节奏有明确数字：前 5 次重试不睡眠；从第 6 次起开始调 `sqlite3OsSleep`，第一次延迟 1 微秒；第 10 次及以后延迟按 `(cnt-9)*(cnt-9)*39` 微秒递增。源码注释写明：第 100 次（也是最后一次）重试延迟 323 毫秒，总延迟不到 10 秒。超过 `WAL_RETRY_PROTOCOL_LIMIT`（100）时返回 `SQLITE_PROTOCOL`。

`walTryBeginRead` 在 `!useWal` 时先调 `walIndexReadHdr(pWal, pChanged)` 读取索引头。若返回 `SQLITE_BUSY`，函数会区分情形：`pWal->apWiData[0]==0` 时改判为 `WAL_RETRY`（VFS 的 `xShmMap` 返回忙被视为瞬态）；否则尝试取 `WAL_RECOVER_LOCK` 的共享锁——能取到说明别处正在恢复，同样改判 `WAL_RETRY`；取不到则返回 `SQLITE_BUSY_RECOVERY`。

`walIndexReadHdr` 的降级链条是本页最完整的分支：先用 `walIndexPage(pWal, 0, &page0)` 映射首块；若这一步返回 `SQLITE_READONLY_CANTINIT`（共享内存可打开但不可写，且本线程无法确认存在另一个可写连接在维护它），则置 `pWal->bShmUnreliable = 1`、`pWal->exclusiveMode = WAL_HEAPMEMORY_MODE`、`*pChanged = 1`——即退化为私有堆内存索引。其它非 OK 返回直接作为错误上抛。

随后 `walIndexTryHdr` 尝试读两份头部拷贝并验校验和。首次失败时，源码注释给出的两种成因是「头部损坏」或「与写者竞争」：在 `bShmUnreliable==0 && (readOnly & WAL_SHM_RDONLY)` 时取 `WAL_WRITE_LOCK` 的共享锁重试一次；仍失败则返回 `SQLITE_READONLY_RECOVERY`。否则（可写情形）取 `WAL_WRITE_LOCK` 排他锁重试；若拿到锁时原先没持写锁，把 `pWal->writeLock` 置为 2（让 `walIndexPage` 表现得像持锁一样、并在 SEH 异常时解锁）；重试仍失败说明头部确实损坏，于是先 `walDisableBlocking(pWal)` 再调 `walIndexRecover(pWal)` 重建索引，并置 `*pChanged = 1`。

头读成功后还要校验版本号：`pWal->hdr.iVersion != WALINDEX_MAX_VERSION` 时返回 `SQLITE_CANTOPEN_BKPT`。`WALINDEX_MAX_VERSION` 为 3007000，与 WAL 文件格式版本 `WAL_MAX_VERSION` 同值。

`walTryBeginRead` 后续按 `pInfo->nBackfill` 与 `pWal->hdr.mxFrame` 比较判断「WAL 是否已被完全回填」：相等时取 `WAL_READ_LOCK(0)` 的共享锁并重新比对头部，若头部已变则不能继续（读者持 `READ_LOCK(0)` 意味着完全忽略 WAL 文件）。

### 关闭：`sqlite3PagerCloseWal` 与 `sqlite3WalClose`

从 WAL 模式退回回滚日志模式走 `sqlite3PagerCloseWal`。它先做一件容易被忽略的事：如果 `pPager->pWal` 为空但 `-wal` 文件确实存在（`sqlite3OsAccess` 的 `SQLITE_ACCESS_EXISTS` 判定为真），就先取 `SHARED_LOCK` 再调 `pagerOpenWal(pPager)` 把日志打开——必须这样才能在切模式前把残留的 WAL 检查点回主库。

随后取 `pagerExclusiveLock(pPager)`，成功则调 `sqlite3WalClose(pPager->pWal, db, pPager->walSyncFlags, pPager->pageSize, (u8*)pPager->pTmpSpace)`，并把 `pPager->pWal` 置 0、调 `pagerFixMaplimit(pPager)`。若关闭失败且 `pPager->exclusiveMode` 为假，则 `pagerUnlockDb(pPager, SHARED_LOCK)` 降级回共享锁。

`sqlite3WalClose` 的清理逻辑依赖「能否拿到数据库文件的 EXCLUSIVE 锁」：只有 `zBuf!=0` 且 `sqlite3OsLock(pWal->pDbFd, SQLITE_LOCK_EXCLUSIVE)` 成功时才进入删除流程——此时置 `pWal->exclusiveMode = WAL_EXCLUSIVE_MODE`（原为 `WAL_NORMAL_MODE` 时），调 `sqlite3WalCheckpoint(pWal, db, SQLITE_CHECKPOINT_PASSIVE, 0, 0, sync_flags, nBuf, zBuf, 0, 0)`。检查点成功后再读 `SQLITE_FCNTL_PERSIST_WAL` 提示：值不等于 1 时置 `isDelete = 1`（后续删 `-wal`）；等于 1 且 `pWal->mxWalSize>=0` 时改走 `walLimitSize(pWal, 0)` 把 WAL 截断到 0 字节。

无论是否删除，最后都调 `walIndexClose(pWal, isDelete)`、关闭 `pWalFd`；`isDelete` 为真时额外 `sqlite3OsDelete(pWal->pVfs, pWal->zWalName, 0)` 删掉 WAL 文件。释放 `pWal->apWiData` 与 `pWal` 本身。

`walIndexClose` 在 `pWal->exclusiveMode==WAL_HEAPMEMORY_MODE` 或 `pWal->bShmUnreliable` 时逐页 `sqlite3_free` 堆内存块；`pWal->exclusiveMode != WAL_HEAPMEMORY_MODE` 时调 `sqlite3OsShmUnmap(pWal->pDbFd, isDelete)` 解除共享内存映射并按标志决定是否删除 `-shm` 文件。

## 关键取舍

- **共享内存还是堆内存（源码事实）**：`sqlite3WalOpen` 用 `bNoShm` 一个布尔参数决定 `exclusiveMode` 取 `WAL_HEAPMEMORY_MODE` 还是 `WAL_NORMAL_MODE`。堆内存路径省掉了 `-shm` 文件和跨进程协调，代价是同一进程内必须独占数据库——官方文档在「Implementation Of Shared-Memory For The WAL-Index」一节明确说明：当 `locking_mode=EXCLUSIVE` 时 `-shm` 被省略，用堆内存替代 mmap 的 shm 文件。
- **能力检查放在 Pager 层而非 WAL 层（源码事实）**：`sqlite3PagerWalSupported` 判定的三个条件（`noLock` 为假、`exclusiveMode` 为真、或 `iVersion>=2 && xShmMap` 存在）决定了 WAL 能否启用。VFS 缺 `xShmMap` 时，`sqlite3PagerOpenWal` 返回 `SQLITE_CANTOPEN`，数据库停留在原日志模式——官方文档「Activating And Configuring WAL Mode」一节说明转换失败时 journal_mode 保持原值。
- **`SQLITE_READONLY` 被改写成 `SQLITE_OK`（源码事实）**：`walIndexPageRealloc` 中 `sqlite3OsShmMap` 返回 `SQLITE_READONLY` 时把 `pWal->readOnly |= WAL_SHM_RDONLY` 并令 `rc = SQLITE_OK`。这是把「共享内存只读」当作合法的降级状态而非失败，代价是要靠 `bShmUnreliable` 这条独立标记继续区分「只读但可信」与「只读且不可信」。
- **共享内存不可写时退化为私有索引（源码事实）**：`walIndexReadHdr` 收到 `SQLITE_READONLY_CANTINIT` 时，把 `exclusiveMode` 改为 `WAL_HEAPMEMORY_MODE` 并置 `bShmUnreliable = 1`。此路径下 `walTryBeginRead` 不再调 `walIndexReadHdr`，改走 `walBeginShmUnreliable`。代价是这种连接看不到别的连接刚写入的内容，官方文档「Read-Only Databases」一节说明只读媒体上的 WAL 库需要 `-shm`/`-wal` 已存在、目录可写、或使用 `immutable` 查询参数。
- **索引头双拷贝 + 校验和（源码事实）**：`walIndexTryHdr` 读两份 `WalIndexHdr` 拷贝并比对，同时验校验和；源码注释（tag-20200519-1）说明读顺序是「先读 [0] 再读 [1]，写则相反」，并用内存屏障阻止编译器与硬件重排。多写一份头换来的是无锁读的一致性检测。
- **切模式的排他锁代价（源码事实）**：`sqlite3PagerCloseWal` 必须先拿 EXCLUSIVE 锁才能关 WAL，否则返回 `SQLITE_BUSY` 且不关闭日志连接。这与官方文档「WAL Locks」一节一致：`SQLITE_LOCK_EXCLUSIVE` 阻止其他连接获取共享锁，从而防止在别的连接脚下删掉 WAL 文件；代价是切模式时其它连接必须全部断开。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/wal.c` | `sqlite3WalOpen` | 分配 `Wal` 对象、打开 `-wal` 文件、按 `bNoShm` 选择 `WAL_HEAPMEMORY_MODE` 或 `WAL_NORMAL_MODE`、按设备能力调整 `syncHeader` 与 `padToSectorBoundary`；开头以 `assert` 钉住全部 wal-index 格式常量 |
| `src/wal.c` | `sqlite3WalClose` | 取数据库 EXCLUSIVE 锁后跑一次 `SQLITE_CHECKPOINT_PASSIVE` 检查点，按 `SQLITE_FCNTL_PERSIST_WAL` 决定删除 `-wal` 还是截断到 0 字节，再经 `walIndexClose` 解除映射并释放 `Wal` |
| `src/wal.c` | `sqlite3WalBeginReadTransaction` | 读事务入口，用 `SEH_TRY`/`SEH_EXCEPT` 包装 `walBeginReadTransaction`，异常时交给 `walHandleException` |
| `src/pager.c` | `sqlite3PagerOpenWal` | Pager 层开启 WAL：排除临时库与已打开情形，经 `sqlite3PagerWalSupported` 判能力（失败返回 `SQLITE_CANTOPEN`），关掉 `jfd` 后调 `pagerOpenWal`，成功则置 `PAGER_JOURNALMODE_WAL` 与 `PAGER_OPEN` |
| `src/pager.c` | `sqlite3PagerCloseWal` | 退出 WAL 模式：必要时先 `pagerOpenWal` 打开残留 `-wal` 以便检查点，取 EXCLUSIVE 锁后调 `sqlite3WalClose`，失败且非独占模式时 `pagerUnlockDb` 降级回共享锁 |

## 与主核 / AI 的关系

暂无直接对应。主核 `engine/db/` 自带 `wal/` 层且分级规则规定 L1 的 `wal/` 只依赖 `common/`，与 SQLite 的 `wal.c` 无共享代码路径。

**架构推论：** SQLite 这套设计的可借鉴之处是「模式切换与能力探测下沉到 Pager 与 VFS 边界」——`sqlite3PagerWalSupported` 把「VFS 是否提供 `xShmMap`」这一唯一硬约束收敛成一处判定，其余代码不必再关心 VFS 差异；而 `walIndexPageRealloc` 里把 `SQLITE_READONLY` 改写成 `SQLITE_OK`、以及 `walIndexReadHdr` 中 `SQLITE_READONLY_CANTINIT` 退化到堆内存索引，代表了「共享内存不可用时降级而非失败」这一类容错策略的分级。但**架构推论：** 主核的 WAL 是否存在等价机制，无法从本页源码推断——本页读的是 SQLite 的 `src/wal.c` 与 `src/pager.c`，主核的 `wal/` 目录不在本页 refs 范围内，任何对应关系都需要另行取证。

## 参考

- [SQLite 官方文档：Write-Ahead Logging](https://sqlite.org/wal.html) —— 说明 WAL 模式的启用方式、共享内存的用途与 `locking_mode=EXCLUSIVE` 下省略 `-shm` 的行为，以及只读库的限制。
- [SQLite 官方文档：WAL-mode File Format](https://sqlite.org/walformat.html) —— 说明 wal-index 的 136 字节头部分区、8 个锁字节的偏移，以及 `WAL_READ_LOCK(N)` 与 read-mark 的对应关系。
- [SQLite 官方源码：src/wal.c](https://github.com/sqlite/sqlite/blob/master/src/wal.c) —— 支撑 `sqlite3WalOpen` / `sqlite3WalClose` / `sqlite3WalBeginReadTransaction` 的实现事实与常量取值。
- [SQLite 官方源码：src/pager.c](https://github.com/sqlite/sqlite/blob/master/src/pager.c) —— 支撑 `sqlite3PagerOpenWal` / `sqlite3PagerCloseWal` 的能力检查、模式切换与降级路径。
