---
project: sqlite
feature: vfs-abstraction
page_type: feature
schema_version: 1
refs:
  - path: src/os.c
    symbols: [sqlite3OsOpen, sqlite3OsRead, sqlite3OsWrite, sqlite3OsLock, sqlite3OsUnlock, sqlite3_vfs_register]
---

# VFS 抽象层

## 一句话职责

把 SQLite 上层（pager、wal、btree）对文件的读写与加锁操作，统一收敛到一组 `sqlite3OsXXX` 包装函数，再由这些包装函数转发到具体的 `sqlite3_io_methods` / `sqlite3_vfs` 实现；同时提供 VFS 的注册、查找与默认项切换机制。

## 机制是什么

SQLite 内部有两张互相配套的接口表：

1. **`sqlite3_vfs`（VFS 级）**：描述"这一套文件系统实现"本身，包含 `xOpen` / `xDelete` / `xAccess` / `xFullPathname` / `xRandomness` / `xSleep` / `xCurrentTime` 等方法，以及描述该 VFS 身份与能力的三个整数字段 `iVersion`、`szOsFile`、`mxPathname`。
2. **`sqlite3_io_methods`（文件级）**：描述"一个已打开的文件"能做什么，包含 `xRead` / `xWrite` / `xTruncate` / `xSync` / `xFileSize` / `xLock` / `xUnlock` / `xCheckReservedLock` / `xFileControl` / `xSectorSize` / `xDeviceCharacteristics` / `xShmMap` / `xShmLock` / `xShmBarrier` / `xShmUnmap` / `xFetch` / `xUnfetch`。

`src/os.c` 提供的是**架构无关的第三层**——薄包装层。`src/os.c` 的文件头注释说得很直接："This file contains OS interface code that is common to all architectures."，而包装函数群的注释又说："The following routines are convenience wrappers around methods of the sqlite3_file object. This is mostly just syntactic sugar."

于是形成三层调用链：**上层调用方 → `sqlite3OsXXX` 包装 → `sqlite3_file::pMethods->xXXX` / `sqlite3_vfs::xXXX` → 平台实现**。

注册侧则由 `src/os.c` 里的静态链表 `vfsList`（定义在 `src/os.c`，通过 `SQLITE_WSD` 宏声明）承载。`sqlite3_vfs_find` 按名字遍历它，`sqlite3_vfs_register` 往它头部插入，`sqlite3_vfs_unregister` 从它摘除。

## 怎么实现的

### 打开文件：sqlite3OsOpen

`src/os.c` 的 `sqlite3OsOpen(pVfs, zPath, pFile, flags, pFlagsOut)` 是"打开一个文件"的唯一入口：

```c
int sqlite3OsOpen(
  sqlite3_vfs *pVfs,
  const char *zPath,
  sqlite3_file *pFile,
  int flags,
  int *pFlagsOut
){
  int rc;
  DO_OS_MALLOC_TEST(0);
  assert( zPath || (flags & SQLITE_OPEN_EXCLUSIVE) );
  rc = pVfs->xOpen(pVfs, zPath, pFile, flags & 0x1087f7f, pFlagsOut);
  assert( rc==SQLITE_OK || pFile->pMethods==0 );
  return rc;
}
```

关键点：

- **它不持有 `sqlite3_vfs` 指针**，而是把它作为参数传进来。所以"用哪个 VFS"由调用方决定，不由 `src/os.c` 决定。
- **标志位被掩码 `0x1087f7f` 过滤后才下发给 VFS**。源码注释写明："0x87f7f is a mask of SQLITE_OPEN_ flags that are valid to be passed down into the VFS layer."。逐位拆开这个掩码，被挡在 VFS 之外的恰好是四把互斥/缓存模式标志与一个扩展结果码标志：`SQLITE_OPEN_NOMUTEX`(0x00008000)、`SQLITE_OPEN_FULLMUTEX`(0x00010000)、`SQLITE_OPEN_SHAREDCACHE`(0x00020000)、`SQLITE_OPEN_PRIVATECACHE`(0x00040000)、`SQLITE_OPEN_EXRESCODE`(0x02000000)。保留下来的是 `SQLITE_OPEN_READONLY`(0x1)、`READWRITE`(0x2)、`CREATE`(0x4)、`DELETEONCLOSE`(0x8)、`EXCLUSIVE`(0x10)、`AUTOPROXY`(0x20)、`URI`(0x40)、`MEMORY`(0x80)、`MAIN_DB`(0x100) 到 `SUPER_JOURNAL`(0x4000)、`WAL`(0x80000)、`NOFOLLOW`(0x1000000) 等文件类型与打开模式位。源码注释明确点出："Some SQLITE_OPEN_ flags (for example, SQLITE_OPEN_FULLMUTEX or SQLITE_OPEN_SHAREDCACHE) are blocked before reaching the VFS."
- **`pFile` 由调用方预先分配**，函数只负责把它填好。源码注释解释了这个分工的原因："The default SQLite sqlite3_vfs implementations do not allocate memory (actually, os_unix.c allocates a small amount of memory from within OsOpen()), but some third-party implementations may."——所以结构体大小由 VFS 的 `szOsFile` 决定，分配动作上移到调用方。
- **失败时 `pFile->pMethods` 必须是 0**，由 `assert( rc==SQLITE_OK || pFile->pMethods==0 )` 保证。
- **断言 `zPath || (flags & SQLITE_OPEN_EXCLUSIVE)`**：`zPath` 为 NULL 表示要开临时文件，此时必须同时带 `SQLITE_OPEN_EXCLUSIVE`。

`src/os.c` 的 `sqlite3OsOpenMalloc` 是它的"分配版"包装：先用 `sqlite3MallocZero(pVfs->szOsFile)` 分配 `szOsFile` 字节的 `sqlite3_file`，再调 `sqlite3OsOpen`；失败时 `sqlite3_free(pFile)` 并把 `*ppFile` 置 0，成功才回填指针；分配本身失败时返回 `SQLITE_NOMEM_BKPT`。

真实调用方（这些是"谁真正在打开文件"）：

- `src/pager.c` 的 `sqlite3PagerOpen` 路径：`sqlite3OsOpen(pPager->pVfs, 0, pFile, vfsFlags, 0)`——`zPath` 传 0，是打临时文件。
- `src/pager.c` 的主库打开路径：`sqlite3OsOpen(pVfs, pPager->zFilename, pPager->fd, vfsFlags, &fout)`。
- `src/pager.c` 的日志打开路径：`sqlite3OsOpen(pVfs, pPager->zJournal, pPager->jfd, f, &fout)`。
- `src/wal.c` 的 WAL 文件打开：`sqlite3OsOpen(pVfs, zWalName, pRet->pWalFd, flags, &flags)`。
- `src/memjournal.c` 的 `sqlite3OsOpen(pVfs, zName, pJfd, flags, 0)`。
- `src/vdbesort.c` 的 `sqlite3OsOpenMalloc(db->pVfs, 0, ppFd, ...)`——外排排序器的临时文件。

平台侧对应实现在 `src/os_unix.c` 的 `unixOpen`：它把 `SQLITE_OPEN_*` 标志翻译成 POSIX `open()` 的 `openFlags`——`isReadonly` 映射 `O_RDONLY`，`isReadWrite` 映射 `O_RDWR`，`isCreate` 映射 `O_CREAT`，`isExclusive` 映射 `O_EXCL|O_NOFOLLOW`，再无条件或上 `O_LARGEFILE|O_BINARY|O_NOFOLLOW`。

**降级路径（真实调用方）**：`src/os_unix.c` 的 `unixOpen` 在 `robust_open` 返回负 fd 时，若 `errno != EISDIR && isReadWrite`，会**回退到只读打开**——清掉 `SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE`、加上 `SQLITE_OPEN_READONLY`、把 `openFlags` 的 `O_RDWR|O_CREAT` 换成 `O_RDONLY`，再重试一次 `robust_open`。这条回退的另一个分支是：若 `isNewJrnl && errno==EACCES && osAccess(zName, F_OK)`，返回 `SQLITE_READONLY_DIRECTORY`（即目录不可写）。

### 读与写：sqlite3OsRead / sqlite3OsWrite

`src/os.c` 的两个函数体极短：

```c
int sqlite3OsRead(sqlite3_file *id, void *pBuf, int amt, i64 offset){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xRead(id, pBuf, amt, offset);
}
int sqlite3OsWrite(sqlite3_file *id, const void *pBuf, int amt, i64 offset){
  DO_OS_MALLOC_TEST(id);
  return id->pMethods->xWrite(id, pBuf, amt, offset);
}
```

两者都是**无条件转发**：不做参数校验、不做边界检查、不吞错误码，直接把 `id->pMethods->xRead` / `xWrite` 的返回值原样返回。注意它们**都不检查 `id->pMethods` 是否为 NULL**——这与 `sqlite3OsClose`、`sqlite3OsFileControl` 不同，后两者显式判空。

平台侧 `src/os_unix.c` 的 `unixRead` 有三个错误分支，值得逐个说清，因为它们直接决定上层看到什么错误码：

- 读满（`got == amt`）：返回 `SQLITE_OK`。
- 系统调用失败（`got < 0`）：若 `pFile->lastErrno` 是 `ERANGE`、`EIO`、`ENXIO`（条件编译）或 `EDEVERR`（条件编译），返回 `SQLITE_IOERR_CORRUPTFS`；**否则返回 `SQLITE_IOERR_READ`**。源码注释说明 `SQLITE_IOERR_CORRUPTFS` 会在返回给应用前经 `sqlite3ApiExit()` 转成 `SQLITE_CORRUPT`。
- 短读（`0 <= got < amt`）：先 `storeLastErrno(pFile, 0)`（不是系统错误），再把缓冲区未读到的部分 `memset` 成 0，返回 `SQLITE_IOERR_SHORT_READ`。

`src/os_unix.c` 的 `unixWrite` 用 `while` 循环反复调用 `seekAndWrite` 直到写完（每次推进 `amt`、`offset`、`pBuf`），失败时按 `wrote<0 && pFile->lastErrno!=ENOSPC` 判定为系统错误。

真实调用方（"谁真的在读文件"）：`src/pager.c` 的 `sqlite3PagerGet` 走 `sqlite3OsRead(pPager->fd, pPg->pData, pPager->pageSize, iOffset)`；`src/pager.c` 的 `sqlite3PagerWrite` 走 `sqlite3OsWrite(pPager->fd, (u8 *)aData, pPager->pageSize, ofst)`；`src/wal.c` 的帧读取走 `sqlite3OsRead(pWal->pWalFd, aFrame, szFrame, iOffset)`；`src/vdbesort.c` 的排序器读走 `sqlite3OsRead(p->pFd, p->aBuffer, nRead, p->iReadOff)`；`src/btree.c` 的 `sqlite3BtreeMoveto` 相关读取走 `sqlite3OsRead(fd, aWrite, a+4, (i64)pBt->pageSize*(nextPage-1))`；`src/backup.c` 的备份写走 `sqlite3OsWrite(pFile, zData, pgszSrc, iOff)`。

### 加锁与解锁：sqlite3OsLock / sqlite3OsUnlock

```c
int sqlite3OsLock(sqlite3_file *id, int lockType){
  DO_OS_MALLOC_TEST(id);
  assert( lockType>=SQLITE_LOCK_SHARED && lockType<=SQLITE_LOCK_EXCLUSIVE );
  return id->pMethods->xLock(id, lockType);
}
int sqlite3OsUnlock(sqlite3_file *id, int lockType){
  assert( lockType==SQLITE_LOCK_NONE || lockType==SQLITE_LOCK_SHARED );
  return id->pMethods->xUnlock(id, lockType);
}
```

两者的差别是**断言区间不同**，这不是风格问题而是契约：

- `sqlite3OsLock` 允许的档位是 `SQLITE_LOCK_SHARED`(1) 到 `SQLITE_LOCK_EXCLUSIVE`(4)，对应 `src/os.h` 的 `SHARED_LOCK`(1)、`RESERVED_LOCK`(2)、`PENDING_LOCK`(3)、`EXCLUSIVE_LOCK`(4)。`src/os.h` 的注释写明："PENDING_LOCK may not be passed directly to sqlite3OsLock()."——但 `src/os.c` 的断言区间**包含** 3，所以这里只做区间检查，语义约束由别处保证。
- `sqlite3OsUnlock` 只允许 `SQLITE_LOCK_NONE`(0) 与 `SQLITE_LOCK_SHARED`(1)，因为解锁只能降到共享或完全放开，不能一步降到保留级。

同样注意：**这两个函数都不判 `id->pMethods` 是否为空**，且 `sqlite3OsUnlock` **没有** `DO_OS_MALLOC_TEST`。

平台侧 `src/os_unix.c` 的 `unixLock` 把 SQLite 的四档锁映射到 POSIX 建议锁：`SHARED` 是对 pending byte 取读锁再对 shared byte range 取读锁；`RESERVED` 是对 reserved byte 取写锁；`PENDING` 是对 pending byte 取写锁；`EXCLUSIVE` 是对整个 shared byte range 取写锁。锁字节位置由 `src/os.h` 的 `PENDING_BYTE`（默认 `sqlite3PendingByte`，在 `SQLITE_OMIT_WSD` 下退化为常量 `0x40000000`）、`RESERVED_BYTE`(`PENDING_BYTE+1`)、`SHARED_FIRST`(`PENDING_BYTE+2`)、`SHARED_SIZE`(510) 定义。`unixUnlock` 转发给 `posixUnlock(id, eFileLock, 0)`。

真实调用方：`src/pager.c` 的 `pagerLockDb` 调 `sqlite3OsLock(pPager->fd, eLock)`（其断言写明 eLock 只能是 `SHARED_LOCK`、`RESERVED_LOCK` 或 `EXCLUSIVE_LOCK`），`src/pager.c` 的 `pagerUnlockDb` 调 `sqlite3OsUnlock(pPager->fd, eLock)`（其断言写明 eLock 只能是 `NO_LOCK` 或 `SHARED_LOCK`）。两处都先判 `pPager->noLock`——为真时直接返回 `SQLITE_OK`，不落到 VFS。

### 注册 VFS：sqlite3_vfs_register

`src/os.c` 的 `sqlite3_vfs_register(pVfs, makeDflt)` 做四件事：

```c
int sqlite3_vfs_register(sqlite3_vfs *pVfs, int makeDflt){
  ...
  sqlite3_mutex_enter(mutex);
  vfsUnlink(pVfs);
  if( makeDflt || vfsList==0 ){
    pVfs->pNext = vfsList;
    vfsList = pVfs;
  }else{
    pVfs->pNext = vfsList->pNext;
    vfsList->pNext = pVfs;
  }
  assert(vfsList);
  sqlite3_mutex_leave(mutex);
  return SQLITE_OK;
}
```

- **先 `vfsUnlink(pVfs)` 再插入**。`vfsUnlink` 是 `src/os.c` 的静态函数，遍历链表把目标摘掉。源码注释说："It is harmless to register the same VFS multiple times."——正是靠"先摘后插"实现的。`src/os.c` 的 `sqlite3_vfs_find` 按名字遍历同一张链表。
- **`makeDflt` 为真时插到链表头部**；链表头就是默认 VFS——`src/os.c` 的 `sqlite3_vfs_find` 在 `zVfs==0` 时立刻 `break`，返回的就是链表第一个元素。
- **`makeDflt` 为假时插到链表第二个位置**（`vfsList->pNext` 之后），即"插进去但不影响默认项"。
- **整个过程在 `SQLITE_MUTEX_STATIC_MAIN` 静态互斥量下完成**：`sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN)` + `sqlite3_mutex_enter` / `sqlite3_mutex_leave`。注意这与 `sqlite3_vfs_find` 用的是同一把静态锁。
- **返回值恒为 `SQLITE_OK`**（`SQLITE_ENABLE_API_ARMOR` 下 `pVfs==0` 时提前返回 `SQLITE_MISUSE_BKPT`）。

平台侧真实的注册调用方：

- `src/os_unix.c` 的 `sqlite3_os_init`：`sqlite3_vfs_register(&aVfs[i], i==0)`——第一个注册的成为默认项。
- `src/os_win.c` 的 `sqlite3_os_init`：连续注册 `winVfs`（`makeDflt=1`）、`winLongPathVfs`、`winNolockVfs`、`winLongPathNolockVfs`（后三个 `makeDflt=0`）。
- `src/os_kv.c` 的 `sqlite3_os_init`：`sqlite3_vfs_register(&sqlite3OsKvvfsObject, 1)`；同文件的 `sqlite3KvvfsInit` 则用 `makeDflt=0`。
- `src/memdb.c` 的 `sqlite3MemdbInit`：`sqlite3_vfs_register(&memdb_vfs, 0)`——注册为非默认项，memdb 是"垫在默认 VFS 之上"的 shim，它先 `sqlite3_vfs_find(0)` 拿到下层 VFS 并存进 `memdb_vfs.pAppData`，再把 `memdb_vfs.szOsFile` 取为 `max(下层 szOsFile, sizeof(MemFile))`。
- `src/main.c` 的 `sqlite3ParseUri`：`*ppVfs = sqlite3_vfs_find(zVfs)`，找不到时返回 `SQLITE_ERROR` 并给出 `"no such vfs: %s"` 的错误消息——这是"按名字选 VFS"的用户入口。
- `src/main.c` 的 `sqlite3_sleep`：`pVfs = sqlite3_vfs_find(0)` 拿默认 VFS。

`src/os.c` 另有 `sqlite3_vfs_unregister(pVfs)`，同样在静态锁下调用 `vfsUnlink`。

## 关键取舍

**用薄包装层换取平台无关性。** `src/os.c` 全部 447 行里没有一个 `#ifdef _WIN32` 或 `#ifdef __linux__`——平台差异全部下沉到 `src/os_unix.c` / `src/os_win.c` / `src/os_kv.c`。上层（pager、wal、btree）只认 `sqlite3OsXXX`，因此换操作系统只需换 VFS 实现，不必动上层任何一行。代价是每个操作多一层函数调用，收益是 VFS 层的错误码、断言与开关都集中在一处。

**包装层不做语义判断，只做类型与前置条件判断。** `sqlite3OsRead` / `sqlite3OsWrite` 不检查 `amt` 是否为正、不检查 `offset` 是否越界，直接转发。真正的断言放在被调用方——`src/os_unix.c` 的 `unixRead` 里有 `assert( amt>0 )`、`assert( offset>=0 )`。这样包装层保持零成本，参数契约由实现方声明。

**标志位在进入 VFS 前统一裁剪。** 掩码 `0x1087f7f` 把"只对 SQLite 核心有意义"的标志（互斥模式、共享缓存模式、扩展结果码）挡在 VFS 之外。这样 VFS 实现永远只需要面对文件语义相关的标志，不必知道 `SQLITE_OPEN_FULLMUTEX` 是什么。

**失败时 `pMethods` 必须置 0，而不是置为一个"错误方法表"。** `sqlite3OsOpen` 的 `assert( rc==SQLITE_OK || pFile->pMethods==0 )` 把这条变成可执行契约；`src/sqlite.h.in` 的注释也写明 xOpen 必须设置 `sqlite3_file.pMethods`，"xOpen must do this even if the open fails"。上层据此判断"这个文件句柄是否有效"。

**锁的档位在包装层就用断言收紧。** `sqlite3OsLock` 断言区间 `[SQLITE_LOCK_SHARED, SQLITE_LOCK_EXCLUSIVE]`，`sqlite3OsUnlock` 断言只能是 `SQLITE_LOCK_NONE` 或 `SQLITE_LOCK_SHARED`。`src/os.h` 的注释解释了为什么 `PENDING_LOCK` 不该直接传给 `sqlite3OsLock`：申请排他锁的进程实际可能先拿到 PENDING 锁，之后再通过另一次 `sqlite3OsLock` 调用升级成 EXCLUSIVE。

**VFS 注册表用单链表 + 静态互斥量保护，而不是哈希表。** `vfsList` 的元素个数是几个到十几个量级，链表查找足够；换来的是 `sqlite3_vfs_register` / `sqlite3_vfs_unregister` / `sqlite3_vfs_find` 三处共用同一把 `SQLITE_MUTEX_STATIC_MAIN`，加解锁边界清晰。

**"注册"和"设为默认"合成一个参数。** `makeDflt` 为真时插链表头，为假时插第二个位置。这一个布尔同时表达了两件事：是否入链、是否成为默认项，避免了"注册后再单独调一次设默认"的竞态。

**架构推论：** `src/os.c` 把"文件操作"和"文件控制"分成了两组入口——`sqlite3OsFileControl` / `sqlite3OsFileControlHint` / `sqlite3OsShmMap` / `sqlite3OsFetch` 等走的是另一套语义（尤其是 `sqlite3OsFileControlHint` 无返回值，其源码注释解释为"when simply tossing information over the wall to the VFS and we do not really care if the VFS receives and understands the information since it is only a hint"）。这一层属于另一页（`j02-os-file-control`）的范围，本页只覆盖读写与加锁这两组主路径。

**架构推论：** `src/os.c` 里的 `DO_OS_MALLOC_TEST` 宏在非 `SQLITE_TEST` 构建下展开为空。它在 `sqlite3OsRead`、`sqlite3OsWrite`、`sqlite3OsLock`、`sqlite3OsFileSize`、`sqlite3OsOpen` 等函数入口各做一次 10 字节分配再释放，用来在测试构建中模拟"第三方 VFS 分配内存失败"。也就是说，包装层承担了一部分本不属于它的测试职责——这是为了在测试中验证"错误码从 VFS 一路传回上层"这条链路而刻意付出的代价。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/os.c | sqlite3OsOpen | 用掩码 `0x1087f7f` 过滤标志后转发 `pVfs->xOpen`；断言失败时 `pFile->pMethods` 必为 0 |
| src/os.c | sqlite3OsRead | 转发 `id->pMethods->xRead`，原样返回其错误码 |
| src/os.c | sqlite3OsWrite | 转发 `id->pMethods->xWrite`，原样返回其错误码 |
| src/os.c | sqlite3OsLock | 转发 `id->pMethods->xLock`；断言 `lockType` 落在 `SQLITE_LOCK_SHARED`..`SQLITE_LOCK_EXCLUSIVE` |
| src/os.c | sqlite3OsUnlock | 转发 `id->pMethods->xUnlock`；断言 `lockType` 只能是 `SQLITE_LOCK_NONE` 或 `SQLITE_LOCK_SHARED` |
| src/os.c | sqlite3OsOpenMalloc | 用 `sqlite3MallocZero(pVfs->szOsFile)` 分配文件对象后再调 `sqlite3OsOpen` |
| src/os.c | sqlite3_vfs_register | 静态锁下先 `vfsUnlink` 再插入 `vfsList`；`makeDflt` 为真插链表头（成为默认 VFS） |
| src/os.c | sqlite3_vfs_unregister | 静态锁下调用 `vfsUnlink` 把 VFS 从 `vfsList` 摘除 |
| src/os.c | sqlite3_vfs_find | 按名字遍历 `vfsList`；名字为 NULL 时返回链表首元素（默认 VFS） |
| src/os.c | vfsUnlink | 静态函数，遍历链表摘除指定 VFS；支持摘链表头与链中节点 |
| src/os.c | sqlite3OsInit | 包住平台侧 `sqlite3_os_init()`，前置一次 10 字节分配以模拟分配失败 |
| src/os.c | sqlite3OsClose | 判 `pId->pMethods` 非空后调 `xClose` 并把 `pMethods` 置 0 |
| src/os.h | SHARED_LOCK / RESERVED_LOCK / PENDING_LOCK / EXCLUSIVE_LOCK | VFS 内部锁档位常量，取值 1/2/3/4 |
| src/os.h | PENDING_BYTE / RESERVED_BYTE / SHARED_FIRST / SHARED_SIZE | 锁字节位置常量：`PENDING_BYTE`（`SQLITE_OMIT_WSD` 下为 `0x40000000`）、`RESERVED_BYTE=PENDING_BYTE+1`、`SHARED_FIRST=PENDING_BYTE+2`、`SHARED_SIZE=510` |
| src/os.h | SQLITE_DEFAULT_SECTOR_SIZE | 扇区大小默认值 `4096`，被 `sqlite3OsSectorSize` 用作 `xSectorSize` 缺失时的兜底 |
| src/os_unix.c | unixOpen | 把 `SQLITE_OPEN_*` 翻译成 POSIX `open()` 标志；写打开失败时回退到只读打开 |
| src/os_unix.c | unixRead | mmap 命中则 `memcpy`；否则 `seekAndRead`，按 `lastErrno` 返回 `SQLITE_IOERR_CORRUPTFS` 或 `SQLITE_IOERR_READ`；短读补零并返回 `SQLITE_IOERR_SHORT_READ` |
| src/os_unix.c | unixWrite | mmap 命中则 `memcpy`；否则循环 `seekAndWrite` 直至写完 |
| src/os_unix.c | unixLock | 把四档 SQLite 锁映射到 POSIX 建议锁（pending byte / reserved byte / shared byte range） |
| src/os_unix.c | unixUnlock | 转发 `posixUnlock(id, eFileLock, 0)` |
| src/os_unix.c | IOMETHODS | 宏，生成 `sqlite3_io_methods` 常量对象与配套的 finder 函数 |
| src/os_unix.c | posixIoMethods / nolockIoMethods / dotlockIoMethods | 三套 `sqlite3_io_methods`，`iVersion` 分别为 3、3、1；前两者带 `xShmMap`，`dotlockIoMethods` 的 `xShmMap` 传 0 |
| src/pager.c | pagerLockDb | 调用 `sqlite3OsLock`；`pPager->noLock` 为真时跳过 |
| src/pager.c | pagerUnlockDb | 调用 `sqlite3OsUnlock`；`pPager->noLock` 为真时跳过 |
| src/wal.c | sqlite3WalOpen | 用 `sqlite3OsOpen` 打开 WAL 文件 |
| src/memjournal.c | sqlite3Memdb / journal 打开路径 | 用 `sqlite3OsOpen` 打开回滚日志与内存日志 |
| src/memdb.c | sqlite3MemdbInit | `sqlite3_vfs_find(0)` 取下层 VFS 存进 `pAppData`，再 `sqlite3_vfs_register(&memdb_vfs, 0)` |
| src/os_kv.c | sqlite3_os_init | `sqlite3_vfs_register(&sqlite3OsKvvfsObject, 1)` 把 KV VFS 注册为默认项 |
| src/os_win.c | sqlite3_os_init | 依次注册 `winVfs`(默认) 与三个非默认变体 |
| src/main.c | sqlite3ParseUri | `sqlite3_vfs_find(zVfs)` 按名字选 VFS，找不到时返回 `SQLITE_ERROR` 与 `no such vfs` 消息 |
| src/sqlite.h.in | sqlite3_vfs | VFS 结构体定义：`iVersion`、`szOsFile`、`mxPathname` 及各 `xXXX` 方法指针 |

## 与主核 / AI 的关系

**架构推论：** 本页描述的是 SQLite 原生 `src/os.c` 的 VFS 抽象，与本仓 `engine/db/` 主核的存储层不是同一套实现。主核的 `engine/db/storage/` 是自研的分层存储引擎，其文件访问不走 `sqlite3OsXXX` 这一层包装，也没有 `sqlite3_vfs` 这张方法表。可迁移的是本页的设计取舍经验，而不是代码。

对 AI 辅助开发有实际价值的有三点：

1. **"薄包装层 + 平台实现分离"的结构**。`src/os.c` 里没有一行平台 `#ifdef`，所有平台差异都在 `os_unix.c` / `os_win.c` 里。这意味着 AI 生成新平台支持时，改动面被限制在单个 VFS 文件内，不需要理解 pager/WAL 的语义。
2. **参数契约用断言表达**。`sqlite3OsLock` 与 `sqlite3OsUnlock` 用不同的断言区间区分"允许的档位"，`sqlite3OsOpen` 用 `assert( rc==SQLITE_OK || pFile->pMethods==0 )` 表达"失败时句柄必须为空"。这类契约是给人和给模型看的可执行说明——但要注意 SQLite 的 `assert()` 在未定义 `SQLITE_DEBUG` 时是空操作，所以它只能防住开发期错误，不能当作运行期校验。
3. **标志位裁剪点与错误码语义要显式**。掩码 `0x1087f7f` 挡掉哪几个标志，是上层与 VFS 之间的契约；`unixRead` 区分 `SQLITE_IOERR_CORRUPTFS` / `SQLITE_IOERR_READ` / `SQLITE_IOERR_SHORT_READ` 三种错误码，是 VFS 与上层之间的契约。本仓 `engine/db/storage/` 的存储层接口若要支持多种后端（文件、内存、对象存储），同样的做法是：在一层做统一的参数裁剪与错误码归一，把差异压到具体后端实现里。

另一处值得注意的实现细节是 `sqlite3_vfs_register` 的"先摘后插"：`vfsUnlink` 让同一个 VFS 重复注册变成幂等操作，`makeDflt` 一个布尔同时表达"是否入链"和"是否成为默认"。这个设计避免了"注册 + 单独设默认"两步之间的竞态窗口。

## 参考

- SQLite 官方文档：The SQLite OS Interface or "VFS"（[sqlite.org/vfs.html](https://sqlite.org/vfs.html)）—— 说明 VFS 在 SQLite 分层中的位置、VFS shim 概念，以及标准 unix / windows VFS 的列表。
- SQLite 官方文档：Virtual File System Objects（[sqlite.org/c3ref/vfs_find.html](https://sqlite.org/c3ref/vfs_find.html)）—— 说明 `sqlite3_vfs_find` / `sqlite3_vfs_register` / `sqlite3_vfs_unregister` 的公开语义与 `makeDflt` 参数。
- SQLite 官方文档：sqlite3_vfs（[sqlite.org/c3ref/vfs.html](https://sqlite.org/c3ref/vfs.html)）—— 说明 `iVersion` / `szOsFile` / `mxPathname` 三个字段的契约与版本演进规则。
- SQLite 官方文档：sqlite3_io_methods（[sqlite.org/c3ref/io_methods.html](https://sqlite.org/c3ref/io_methods.html)）—— 说明文件级方法表的完整语义。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/os.c](https://github.com/sqlite/sqlite/blob/master/src/os.c) —— 本页 refs 指向的文件。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/os.h](https://github.com/sqlite/sqlite/blob/master/src/os.h) —— 锁档位常量与锁字节位置常量。
- SQLite 源码镜像（GitHub）：[sqlite/sqlite 仓库的 src/os_unix.c](https://github.com/sqlite/sqlite/blob/master/src/os_unix.c) —— unix VFS 实现，含 `unixOpen` / `unixRead` / `unixWrite` / `unixLock` / `unixUnlock` 与 `IOMETHODS` 宏。
