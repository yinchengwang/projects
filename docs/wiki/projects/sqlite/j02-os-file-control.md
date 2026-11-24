---
project: sqlite
feature: os-file-control
page_type: feature
schema_version: 1
refs:
  - path: src/os.c
    symbols: [sqlite3OsFileControl, sqlite3OsFileSize, sqlite3OsShmMap, sqlite3OsShmLock, sqlite3OsFetch, sqlite3OsUnfetch]
---

# 文件控制与共享内存

## 一句话职责

给已经打开的 `sqlite3_file` 提供三类非常规操作：按操作码下发文件控制指令（`sqlite3OsFileControl`）、查询文件当前字节数（`sqlite3OsFileSize`），以及 WAL 模式专用的共享内存映射与加锁（`sqlite3OsShmMap` / `sqlite3OsShmLock`）和内存映射读页的取还（`sqlite3OsFetch` / `sqlite3OsUnfetch`）。

## 机制是什么

这一层是 VFS 之上的**薄封装**，本身不含策略，只做三件事：把调用转发到 `sqlite3_file.pMethods` 里的对应函数指针、给出统一的错误语义、在特定编译开关下插入测试用的故障注入。

它与普通读写（`sqlite3OsRead` / `sqlite3OsWrite`）的差别在于"控制面"语义：

1. **文件控制（file control）**是一个**带操作码的旁路通道**。`sqlite3_file_control()` 这个公共 API（`src/main.c`）与内核内部都通过 `sqlite3OsFileControl(id, op, pArg)` 往 VFS 传一个整数操作码和一个泛型指针。操作码常量 `SQLITE_FCNTL_*` 定义在 `src/sqlite.h.in`，当前从 `SQLITE_FCNTL_LOCKSTATE` 的 `1` 排到 `SQLITE_FCNTL_FILESTAT` 的 `45`。**内核保留所有小于 100 的操作码**，自定义 VFS 应使用大于 100 的值，未识别的操作码一律返回 `SQLITE_NOTFOUND`。
2. **共享内存（shm）**是 WAL 模式的旁路文件。`sqlite3OsShmMap` / `sqlite3OsShmLock` 走 `sqlite3_io_methods` 的 version 2 方法族（`xShmMap` / `xShmLock` / `xShmBarrier` / `xShmUnmap`），只对 WAL 有效。
3. **内存映射读页（fetch/unfetch）**是 version 3 方法族（`xFetch` / `xUnfetch`），只在 `SQLITE_MAX_MMAP_SIZE>0` 时才有真实实现，否则是空壳。

`src/os.c` 在这些包装之外还提供了一对孪生入口：`sqlite3OsFileControl` 用于**可能失败且需要知道失败**的操作，`sqlite3OsFileControlHint` 用于**只是把信息丢给 VFS、不在乎对方是否理解**的提示。后者返回 `void`——源码注释明确说明"返回值本来就没有意义"。

## 怎么实现的

### sqlite3OsFileControl 与 sqlite3OsFileControlHint

`sqlite3OsFileControl(id, op, pArg)` 的函数体只有三段：

- `id->pMethods==0` 时直接返回 `SQLITE_NOTFOUND`（文件没打开成功，没有任何方法表）。
- 在 `SQLITE_TEST` 编译下，除了四个特例操作码外都要先跑一次 `DO_OS_MALLOC_TEST(id)`。这四个被排除在故障注入之外的操作码是 `SQLITE_FCNTL_COMMIT_PHASETWO`、`SQLITE_FCNTL_LOCK_TIMEOUT`、`SQLITE_FCNTL_CKPT_DONE`、`SQLITE_FCNTL_CKPT_START`。排除理由写在源码注释里：`COMMIT_PHASETWO` 是在事务已提交之后调用的，注入 OOM 会让 `COMMIT` 语句返回 `SQLITE_NOMEM` 但事务实际已提交，测试脚本会因此混乱；后两个是给 cksumvfs 的只写信号，返回码本来就被忽略。
- 最后 `return id->pMethods->xFileControl(id, op, pArg);` 原样返回 VFS 的结果。

`sqlite3OsFileControlHint(id, op, pArg)` 更简单：`if( id->pMethods ) (void)id->pMethods->xFileControl(id, op, pArg);`——返回值被强制丢弃，文件没打开时直接跳过。

**真实调用方**（都能在源码里查到）：

- `src/pager.c` 的 `sqlite3PagerSync` 用 `sqlite3OsFileControl(pPager->fd, SQLITE_FCNTL_SYNC, pArg)` 发同步信号；返回 `SQLITE_NOTFOUND` 时改用 `sqlite3OsSync(pPager->fd, pPager->syncFlags)` 走普通同步路径。
- `src/pager.c` 的 `sqlite3PagerCommitPhaseTwo` 用 `sqlite3OsFileControl(pPager->fd, SQLITE_FCNTL_COMMIT_PHASETWO, 0)`。
- `src/pager.c` 的 `sqlite3PagerRollback` 一带用 `sqlite3OsFileControl(pPager->fd, SQLITE_FCNTL_HAS_MOVED, &bHasMoved)` 检测文件是否被改名或删除。
- `src/pager.c` 的 `pagerFixMaplimit` 用 `sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_MMAP_SIZE, &sz)` 下发 mmap 上限。
- `src/wal.c` 的 checkpoint 流程用 `sqlite3OsFileControl(pWal->pDbFd, SQLITE_FCNTL_CKPT_START, 0)` 与 `sqlite3OsFileControl(pWal->pDbFd, SQLITE_FCNTL_CKPT_DONE, 0)`，中间还用 `sqlite3OsFileControl(pWal->pDbFd, SQLITE_FCNTL_SIZE_HINT, &nReq)` 提示 checkpoint 后文件会增长到多大。

**VFS 侧未识别即 `SQLITE_NOTFOUND`**：`src/os_unix.c` 的 `unixFileControl` 是一个大 `switch`，覆盖 `SQLITE_FCNTL_BEGIN_ATOMIC_WRITE`、`COMMIT_ATOMIC_WRITE`、`ROLLBACK_ATOMIC_WRITE`、`NULL_IO`、`LOCKSTATE`、`LAST_ERRNO`、`CHUNK_SIZE`、`SIZE_HINT`、`PERSIST_WAL`、`POWERSAFE_OVERWRITE`、`VFSNAME`、`TEMPFILENAME`、`HAS_MOVED`、`LOCK_TIMEOUT`、`BLOCK_ON_CONNECT`、`MMAP_SIZE`、`DB_UNCHANGED`、`SET_LOCKPROXYFILE`、`GET_LOCKPROXYFILE`、`EXTERNAL_READER`、`FILESTAT`；`switch` 之外统一 `return SQLITE_NOTFOUND`。

一个直接后果是：**`SQLITE_FCNTL_SYNC` 在 unix 与 windows VFS 里都没有实现**。全仓只有 `src/os_kv.c` 的 `kvvfsFileControlDb` 处理这个操作码——它在 `pFile->szDb>0` 时调 `kvvfsWriteFileSize` 把文件大小写回存储，失败返回 `SQLITE_IOERR`。因此 unix/windows 上 `sqlite3PagerSync` 拿到的必然是 `SQLITE_NOTFOUND`，随即回退到 `sqlite3OsSync`；源码注释也说明"不需要这个信号的 VFS 应静默忽略该操作码"。

### sqlite3OsFileSize

函数体只有两行有效逻辑：先 `DO_OS_MALLOC_TEST(id)`（`SQLITE_TEST` 构建下注入一次 OOM），再 `return id->pMethods->xFileSize(id, pSize);`。

平台实现里有一个值得注意的语义修正：`src/os_unix.c` 的 `unixFileSize` 用 `osFstat` 拿 `st_size`，失败时 `storeLastErrno` 后返回 `SQLITE_IOERR_FSTAT`；成功后若 `*pSize==1` 则把它改成 `0`。源码注释解释了原因：`findInodeInfo()` 会往零长度数据库里写一个字节来绕过 OS-X msdos 文件系统的 bug，上层若看到长度为 1 会误判（Ticket #3260）。

**真实调用方**：`src/pager.c` 在多处调用 `sqlite3OsFileSize`——`pager_playback` 读 journal 长度（`sqlite3OsFileSize(pJrnl, &szJ)`）、`sqlite3PagerCommitPhaseOne` 读数据库当前大小（`sqlite3OsFileSize(pPager->fd, &currentSize)`）、`pagerUndoCallback` 读 journal 头长度、`sqlite3PagerRollback` 读 super-journal 长度（`sqlite3OsFileSize(pSuper, &nSuperJournal)`）。

### sqlite3OsShmMap

`sqlite3OsShmMap(id, iPage, pgsz, bExtend, pp)` 在 `SQLITE_OMIT_WAL` 未定义时才有定义（整段包在 `#ifndef SQLITE_OMIT_WAL` 里），函数体是 `DO_OS_MALLOC_TEST(id)` 加一次 `id->pMethods->xShmMap(id, iPage, pgsz, bExtend, pp)` 转发。

`src/os_unix.c` 的 `unixShmMap` 的关键行为：

- 若 `pDbFd->pShm==0`，先调 `unixOpenSharedMemory(pDbFd)` 打开 shm 文件。shm 文件名默认是数据库路径加 `-shm` 后缀（`unixOpenSharedMemory` 里 `sqlite3_snprintf(nShmFilename, zShm, "%s-shm", zBasePath)`），若定义了 `SQLITE_SHM_DIRECTORY` 则改为该目录下的 `sqlite-shm-<ino>-<dev>`。
- 进 `pShmNode->pShmMutex`；若 `pShmNode->isUnlocked` 为真，先调 `unixLockSharedMemory` 拿到 DMS 锁并把 `isUnlocked` 清零。
- 映射粒度由 `unixShmRegionPerMap()` 决定：shm 区域固定为 `32*1024` 字节，系统页大小小于它时返回 `1`，否则返回 `pgsz/shmsz`。也就是 64KB 页系统上一次映射两个区域。
- 需要映射的最小区域数 `nReqRegion = ((iRegion+nShmPerMap) / nShmPerMap) * nShmPerMap`，向上取整到 `nShmPerMap` 的整数倍。
- 若 `pShmNode->hShm>=0`（有真实 shm 文件），先 `osFstat` 拿文件大小；`fstat` 失败返回 `SQLITE_IOERR_SHMSIZE`。文件不够大时：若 `bExtend` 为假，直接跳到出口（`*pp` 置 0，返回 `SQLITE_OK`）；若 `bExtend` 为真，则逐页扩展——写死页大小 `pgsz = 4096`（`unixShmMap` 内的 `static const int pgsz = 4096`），对每个新页的最后一字节写一个空字节，目的是让操作系统立刻分配物理页，减少后续访问映射区时 SIGBUS 的概率；写失败返回 `SQLITE_IOERR_SHMSIZE`。
- 映射用 `osMmap(0, nMap, PROT_READ|PROT_WRITE, MAP_SHARED, hShm, ...)`；`MAP_FAILED` 时返回 `SQLITE_IOERR_SHMMAP`。若 `hShm<0`（进程内共享内存模式），改用 `sqlite3_malloc64(nMap)` 并 `memset` 清零，失败返回 `SQLITE_NOMEM_BKPT`。
- 出口处若 `pShmNode->nRegion>iRegion` 则 `*pp = pShmNode->apRegion[iRegion]`，否则 `*pp = 0`；若 `pShmNode->isReadonly` 且 `rc==SQLITE_OK`，把返回码改成 `SQLITE_READONLY`。

`src/os_win.c` 的 `winShmMap` 结构类似，但扩展 shm 文件用的是 `winHandleTruncate(hShared, nByte)` 而不是逐页写字节；`nByte` 的算法也不同，为 `((i64)iRegion+1)*(i64)szRegion`（unix 是向上取整后的 `nReqRegion*szRegion`）。

**真实调用方**：`src/wal.c` 的 `walIndexPage` 调 `sqlite3OsShmMap(pWal->pDbFd, iPage, WALINDEX_PGSZ, pWal->writeLock, (void volatile **)&pWal->apWiData[iPage])`。`WALINDEX_PGSZ` 定义在 `src/wal.c`，等于 `sizeof(ht_slot)*HASHTABLE_NSLOT + HASHTABLE_NPAGE*sizeof(u32)`。在 `exclusiveMode==WAL_HEAPMEMORY_MODE` 时不走 shm 映射，改用 `sqlite3MallocZero(WALINDEX_PGSZ)` 在堆上分配。另一个调用点是 `src/wal.c` 的 `walIndexRecover`：它用 `sqlite3OsShmMap(pWal->pDbFd, 0, WALINDEX_PGSZ, 0, &pDummy)` 只为触发一次映射以确认 shm 可用。

`walIndexPage` 对 `SQLITE_READONLY` 有专门处理：置 `pWal->readOnly |= WAL_SHM_RDONLY`，再把 `rc` 改回 `SQLITE_OK`——只读库连不上 shm 不算致命错误。

### sqlite3OsShmLock

`sqlite3OsShmLock(id, offset, n, flags)` 直接转发 `id->pMethods->xShmLock(id, offset, n, flags)`，无额外包装。合法标志组合只有四种（定义在 `src/sqlite.h.in`）：`SQLITE_SHM_LOCK|SQLITE_SHM_SHARED`、`SQLITE_SHM_LOCK|SQLITE_SHM_EXCLUSIVE`、`SQLITE_SHM_UNLOCK|SQLITE_SHM_SHARED`、`SQLITE_SHM_UNLOCK|SQLITE_SHM_EXCLUSIVE`。`SQLITE_SHM_NLOCK` 为 `8`，即共享内存锁槽数量；内核承诺不会访问这个范围之外的槽位。

`src/os_unix.c` 的 `unixShmLock` 的核心逻辑：

- `pDbFd->pShm==0` 或 `pShmNode==0` 直接返回 `SQLITE_IOERR_SHMLOCK`。
- 掩码 `mask = (1<<(ofst+n)) - (1<<ofst)`，表示这次操作涉及的连续槽位。
- 三种情形才真正动手：解锁且确实持有、请求共享锁且尚未持有、请求独占锁。其余情形直接返回 `SQLITE_OK`（幂等）。源码注释还断言内核不会请求已持有的独占锁。
- 解锁共享锁时若 `aLock[ofst]>1`，说明同进程内还有其他连接持有同一把共享锁，此时只把计数减一、**不**真正释放 POSIX 文件锁。
- 请求共享锁时：若 `aLock[ofst]<0`（别的连接持独占锁）返回 `SQLITE_BUSY`；否则在 `aLock[ofst]==0` 时调 `unixShmSystemLock(pDbFd, F_RDLCK, ofst+UNIX_SHM_BASE, n)` 拿系统级读锁，然后 `sharedMask |= mask` 并把计数加一。
- 请求独占锁时：先扫描 `[ofst, ofst+n)`，任一槽的 `aLock[ii]` 非零就返回 `SQLITE_BUSY`；否则调 `unixShmSystemLock(pDbFd, F_WRLCK, ofst+UNIX_SHM_BASE, n)`，成功后 `exclMask |= mask` 并把每个 `aLock[ii]` 置 `-1`。
- POSIX 锁的字节偏移基址 `UNIX_SHM_BASE` 定义为 `((22+SQLITE_SHM_NLOCK)*4)`，即 `120`；`UNIX_SHM_DMS` 是 `UNIX_SHM_BASE+SQLITE_SHM_NLOCK`，即 `128`（死锁开关字节）。`src/os_unix.c` 末尾 `unix_os_init` 里有断言 `SQLITE_SHM_NLOCK==8` 与 `UNIX_SHM_BASE==120`。
- 源码注释给出了锁序约定（阻塞式加锁时不得持有更靠后的锁）：恢复锁（`ofst==2`）→ 检查点锁（`ofst==1`）→ 写锁（`ofst==0`）→ 读锁（`ofst>=3 && ofst<SQLITE_SHM_NLOCK`）。

**真实调用方**：`src/wal.c` 的四个包装函数，全部通过 `sqlite3OsShmLock` 发起——`walLockShared`（`SQLITE_SHM_LOCK|SQLITE_SHM_SHARED`，`n=1`）、`walUnlockShared`（`SQLITE_SHM_UNLOCK|SQLITE_SHM_SHARED`）、`walLockExclusive`（`SQLITE_SHM_LOCK|SQLITE_SHM_EXCLUSIVE`）、`walUnlockExclusive`（`SQLITE_SHM_UNLOCK|SQLITE_SHM_EXCLUSIVE`）。四个函数开头都有 `if( pWal->exclusiveMode ) return SQLITE_OK;`——WAL 独占模式下所有共享内存锁都退化为空操作。槽位索引由 `src/wal.c` 的 `WAL_WRITE_LOCK`（`0`）、`WAL_CKPT_LOCK`（`1`）、`WAL_RECOVER_LOCK`（`2`）、`WAL_READ_LOCK(I)`（`3+(I)`）定义。

### sqlite3OsFetch 与 sqlite3OsUnfetch

这两个符号在 `src/os.c` 里有**两套定义**，由 `SQLITE_MAX_MMAP_SIZE>0` 切换：

- 开启时（`SQLITE_MAX_MMAP_SIZE>0`）：`sqlite3OsFetch` 做 `DO_OS_MALLOC_TEST(id)` 后转发 `id->pMethods->xFetch(id, iOff, iAmt, pp)`；`sqlite3OsUnfetch` 直接转发 `id->pMethods->xUnfetch(id, iOff, p)`。
- 关闭时：`sqlite3OsFetch` 把 `*pp` 置 `0` 并返回 `SQLITE_OK`；`sqlite3OsUnfetch` 直接返回 `SQLITE_OK`。源码注释标注为"禁用内存映射 I/O 时使用的空壳"。

`SQLITE_MAX_MMAP_SIZE` 的默认值定义在 `src/sqliteInt.h`：在 `__linux__` / `_WIN32` / `__APPLE__` 且 `__MACH__` / `__sun` / `__FreeBSD__` / `__DragonFly__` 上为 `0x7fff0000`（源码注释写明是 `2147418112`），其他平台为 `0`；`__OpenBSD__` 与 `__QNXNTO__` 被强制为 `0`。另外 `SQLITE_DEFAULT_MMAP_SIZE` 的默认值是 `0`——即便编译期上限更大，默认的 mmap 上限仍是零。

平台实现的语义（`src/os_unix.c` 的 `unixFetch` / `unixUnfetch` 与 `src/os_win.c` 的 `winFetch` / `winUnfetch` 结构一致）：

- `unixFetch` 先把 `*pp` 置 `0`。只有在 `pFd->mmapSizeMax>0` 时才尝试：若尚未映射则调 `unixMapfile(pFd, -1)` 建立映射；只有当 `pFd->mmapSize >= (iOff+nAmt+nEofBuffer)` 时才给出指针并把 `pFd->nFetchOut++`。`nEofBuffer` 是常量 `256`，源码注释说明原因是数据库损坏时 SQLite 可能会略微读过页尾，多留 256 字节可寻址空间作为缓冲。
- 取不到指针时不是错误：`*pp` 保持 `0`，返回 `SQLITE_OK`。调用方据此回退到普通的 `xRead`。
- `unixUnfetch(fd, iOff, p)` 的 `p` 非零表示归还一次 fetch 引用，把 `pFd->nFetchOut--`；`p` 为零表示"整个映射可能已失效，请解除"，此时调 `unixUnmapfile(pFd)`。函数里有断言 `(p==0)==(pFd->nFetchOut==0)` 以及 `p==0 || p==&((u8 *)pFd->pMapRegion)[iOff]`，即 `iOff` 必须与当初 fetch 时一致。

**真实调用方**：`src/pager.c` 的 `sqlite3PagerGet` 在 `bMmapOk && iFrame==0`（非 WAL 帧）时调 `sqlite3OsFetch(pPager->fd, (i64)(pgno-1) * pPager->pageSize, pPager->pageSize, &pData)`。若 `pData` 非空但页缓存里已有该页，则立刻用 `sqlite3OsUnfetch(pPager->fd, (i64)(pgno-1)*pPager->pageSize, pData)` 归还引用。归还引用的其他位置还有 `src/pager.c` 的 `sqlite3PagerUnref` 一带（`sqlite3OsUnfetch(pPager->fd, (i64)(pPg->pgno-1)*pPager->pageSize, pPg->pData)`）和 `sqlite3PagerWrite` 一带。

是否启用 fetch 由 `src/pager.c` 的 `pagerFixMaplimit` 决定：它设置 `pPager->bUseFetch = (sz>0)`，并用 `USEFETCH(pPager)` 宏（定义为 `((x)->bUseFetch)`，在 `SQLITE_MAX_MMAP_SIZE==0` 时恒为 `0`）做判断。`pagerFixMaplimit` 还有一道门槛：`fd->pMethods->iVersion>=3`——xFetch/xUnfetch 是 version 3 方法族，VFS 没提供就不走这条路。

## 关键取舍

- **控制面分两档：可失败与纯提示**。`sqlite3OsFileControl` 返回错误码、`sqlite3OsFileControlHint` 返回 `void`。这让内核能在同一套操作码上表达"必须成功"和"随便听听"两种意图，而不必给每个操作码单独设计一个入口。代价是两套入口的调用点必须严格区分——`SQLITE_FCNTL_SIZE_HINT`、`MMAP_SIZE`、`BUSYHANDLER`、`DB_UNCHANGED` 都走 Hint 通道，因为它们失败也不该让事务失败。
- **未识别即 `SQLITE_NOTFOUND`，而不是 `SQLITE_OK`**。这个约定让调用方能明确区分"VFS 不支持"与"支持但失败了"。`sqlite3PagerSync` 正是靠它来判断要不要回退到 `sqlite3OsSync`。代价是每个新操作码都必须同步改 unix、windows、kv 三份实现，否则该操作码在这些平台上静默失效。
- **shm 锁只做状态机转换，不做升级**。`sqlite3.h.in` 的注释明确写着：xShmLock 可以在"未锁↔共享"和"未锁↔独占"之间转换，但不能从共享直接转独占。这条限制被 `src/os_unix.c` 的 `unixShmLock` 用断言固定下来（`assert( n==1 || (flags & SQLITE_SHM_EXCLUSIVE)!=0 )`）。理由是共享内存锁的获取顺序必须全局一致，否则多个进程会互相死锁；代价是内核需要"先解锁再加独占"两步走。
- **独占锁先在进程内检查再进系统**。`unixShmLock` 请求独占时先扫一遍 `aLock[]`，发现同进程有兄弟连接持锁就直接 `SQLITE_BUSY`，不去抢 POSIX 锁。同理，共享锁被释放时若计数仍大于零就不真正解 POSIX 锁。这把"同进程多连接"的情况从内核锁里挪到了进程内计数表，避免同进程内的连接互相抢锁。代价是 `aLock[]` 必须与真实 POSIX 锁严格一致，源码因此用 `assertLockingArrayOk` 断言校验。
- **fetch 拿不到映射不是错误**。`unixFetch` / `winFetch` 在超出 mmap 范围时把 `*pp` 置 `0` 并返回 `SQLITE_OK`，由调用方（`src/pager.c` 的 `sqlite3PagerGet`）自行回退到普通读路径。这是"性能优化不得改变正确性"的体现——映射只是加速手段，不是正确性前提。代价是调用方必须检查 `pData` 是否为空，多一次分支。
- **256 字节尾缓冲是针对损坏文件的防御**。`nEofBuffer` 常量取 `256`，源码注释说是为了应对数据库损坏时 SQLite 可能略微越过页尾的读取。固定取整的 256 是"安全的整数"而非精确边界——多留一点地址空间换取不越界。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/os.c | sqlite3OsFileControl | 把带操作码的文件控制请求转发给 `xFileControl`；`pMethods==0` 时返回 `SQLITE_NOTFOUND`；`SQLITE_TEST` 下对四个写信号类操作码跳过 OOM 故障注入 |
| src/os.c | sqlite3OsFileSize | 转发 `xFileSize` 查询文件字节数；`SQLITE_TEST` 下带 OOM 故障注入 |
| src/os.c | sqlite3OsShmMap | 转发 `xShmMap` 请求共享内存区域映射；仅在未定义 `SQLITE_OMIT_WAL` 时存在 |
| src/os.c | sqlite3OsShmLock | 转发 `xShmLock` 施加/释放共享内存锁；仅在未定义 `SQLITE_OMIT_WAL` 时存在 |
| src/os.c | sqlite3OsFetch | 转发 `xFetch` 取内存映射页指针；`SQLITE_MAX_MMAP_SIZE==0` 时退化为置空指针并返回 `SQLITE_OK` |
| src/os.c | sqlite3OsUnfetch | 转发 `xUnfetch` 归还映射页引用或解除整段映射；`SQLITE_MAX_MMAP_SIZE==0` 时退化为返回 `SQLITE_OK` |
| src/os.c | sqlite3OsFileControlHint | 同一控制通道的"提示"变体：丢弃 VFS 返回值，文件未打开时直接跳过 |
| src/os.h | SQLITE_FCNTL_DB_UNCHANGED | 文件控制操作码常量 `0xca093fa0`（其余 `SQLITE_FCNTL_*` 常量在 `src/sqlite.h.in`） |
| src/sqlite.h.in | SQLITE_FCNTL_* / SQLITE_SHM_* | 文件控制操作码（`SQLITE_FCNTL_LOCKSTATE`=1 至 `SQLITE_FCNTL_FILESTAT`=45）与共享内存锁标志（`SQLITE_SHM_UNLOCK`=1、`LOCK`=2、`SHARED`=4、`EXCLUSIVE`=8）、`SQLITE_SHM_NLOCK`=8 的定义处 |
| src/sqlite.h.in | sqlite3_io_methods | VFS 文件方法表结构：xFileControl/xFileSize 属 version 1，xShmMap/xShmLock/xShmBarrier/xShmUnmap 属 version 2，xFetch/xUnfetch 属 version 3 |
| src/main.c | sqlite3_file_control | 公共 file-control 入口：先在内核里拦截 `FILE_POINTER`/`VFS_POINTER`/`JOURNAL_POINTER`/`DATA_VERSION`/`RESERVE_BYTES`/`RESET_CACHE`，其余转发到 VFS 的 `xFileControl` |
| src/os_unix.c | unixFileControl | unix VFS 的文件控制实现，覆盖 `SQLITE_FCNTL_SIZE_HINT`/`MMAP_SIZE`/`PERSIST_WAL`/`HAS_MOVED` 等操作码；未命中一律 `SQLITE_NOTFOUND` |
| src/os_unix.c | unixFileSize | 用 `osFstat` 取 `st_size`；失败返回 `SQLITE_IOERR_FSTAT`；长度为 1 时改报为 0（Ticket #3260） |
| src/os_unix.c | unixShmMap | shm 区域映射：按 `unixShmRegionPerMap()` 的粒度、`4096` 字节页逐页扩展 shm 文件、`osMmap` 映射（进程内模式改用 `sqlite3_malloc64`） |
| src/os_unix.c | unixShmLock | shm 锁状态机：进程内 `aLock[]` 计数 + POSIX 锁（`UNIX_SHM_BASE`=`((22+SQLITE_SHM_NLOCK)*4)`），冲突返回 `SQLITE_BUSY` |
| src/os_unix.c | unixFetch / unixUnfetch | 内存映射取页/还页：`mmapSizeMax>0` 且 `mmapSize >= iOff+nAmt+256` 时给出指针，`nFetchOut` 计数配对 |
| src/os_win.c | winShmMap | windows VFS 的 shm 映射：用 `winHandleTruncate` 扩展 shm 文件，`nByte=((i64)iRegion+1)*(i64)szRegion` |
| src/os_win.c | winFileControl / winFileSize / winFetch / winUnfetch | windows VFS 的对应实现，语义与 unix 版一致 |
| src/os_kv.c | kvvfsFileControlDb | 唯一实现 `SQLITE_FCNTL_SYNC` 的 VFS：把文件大小写回 key-value 存储，失败返回 `SQLITE_IOERR` |
| src/pager.c | sqlite3PagerSync | 用 `SQLITE_FCNTL_SYNC` 发同步信号，收到 `SQLITE_NOTFOUND` 时回退到 `sqlite3OsSync` |
| src/pager.c | pagerFixMaplimit | 设置 `pPager->bUseFetch=(sz>0)` 并用 `SQLITE_FCNTL_MMAP_SIZE` 下发 mmap 上限；要求 `pMethods->iVersion>=3` |
| src/pager.c | sqlite3PagerGet | 通过 `sqlite3OsFetch` 取映射页指针；取不到时回退普通读路径 |
| src/wal.c | walIndexPage / walIndexRecover | 通过 `sqlite3OsShmMap` 取得 wal-index 页指针；`WALINDEX_PGSZ` 为 wal-index 页大小 |
| src/wal.c | walLockShared / walUnlockShared / walLockExclusive / walUnlockExclusive | 四个共享内存锁包装，经 `sqlite3OsShmLock` 发起；`exclusiveMode` 下全部退化为空操作 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 有自己的一套 OS 层（`engine/db/src/storage/` 下的文件与页管理），与 SQLite 的 `src/os.c` 不共享代码。SQLite 这一层的形态是"函数指针表 + 一层封装"，把平台差异全部推给 `sqlite3_io_methods`；主核则是自持实现。要在主核里复用 SQLite 这套能力，可行的接入点是 `sqlite3_vfs` 注册机制（对应本组 j01）——把主核的存储引擎包装成一个 `sqlite3_file` + `sqlite3_io_methods`，而不是把 `sqlite3OsFileControl` 这层包装搬过去。

**架构推论：** 主核的嵌入式 SDK（`engine/src/sdk/`）直接依赖 SQLite。`sqlite3OsFetch` / `sqlite3OsUnfetch` 提供的"映射页拿不到就回退普通读"语义，对 SDK 而言是有价值的模式：它让内存映射只作为加速项存在，正确性不依赖映射是否成功，SDK 可以在受限环境里安全地关掉 mmap（`SQLITE_MAX_MMAP_SIZE=0` 时 `src/os.c` 的空壳实现保证行为正确）。

**架构推论：** `sqlite3OsFileControl` 的"操作码 + 泛型指针"通道是 SQLite 唯一向自定义 VFS 开放能力的正式接口，AI 生成的集成代码若要在主核或 SDK 里扩展 SQLite 行为，应当走 `sqlite3_file_control()` 公开 API 加自定义操作码（大于 100），而不是绕过 VFS 直接改 `src/os.c`。

## 参考

- SQLite 文件控制操作码总表：<https://sqlite.org/c3ref/c_fcntl_begin_atomic_write.html>
- `sqlite3_io_methods` 结构与 VFS 方法约定：<https://sqlite.org/c3ref/io_methods.html>
- `sqlite3_file_control()` API 说明：<https://sqlite.org/c3ref/file_control.html>
- SQLite WAL 模式文档（wal-index 与共享内存）：<https://sqlite.org/wal.html>
- SQLite VFS 文档：<https://sqlite.org/vfs.html>
- 源码镜像 `src/os.c`：<https://github.com/sqlite/sqlite/blob/master/src/os.c>
- 源码镜像 `src/os_unix.c`：<https://github.com/sqlite/sqlite/blob/master/src/os_unix.c>
- 源码镜像 `src/sqlite.h.in`（操作码与锁标志定义）：<https://github.com/sqlite/sqlite/blob/master/src/sqlite.h.in>
