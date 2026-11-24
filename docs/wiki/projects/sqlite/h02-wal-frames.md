---
project: sqlite
feature: wal-frames
page_type: feature
schema_version: 1
refs:
  - path: src/wal.c
    symbols: [sqlite3WalFrames, sqlite3WalReadFrame, sqlite3WalFindFrame, sqlite3WalFramesize]
---

# WAL 帧写入与读取

## 一句话职责

把修改后的数据库页编码成带校验和的 WAL 帧追加到 WAL 文件（提交帧额外携带数据库页数），并按帧号把帧内的页数据读回。

## 机制是什么

WAL 文件由一个固定的头部加若干帧组成。头部由 `wal.c` 的 `WAL_HDRSIZE` 定义为 32 字节，含魔数 `WAL_MAGIC`（`0x377f0682`）、版本号 `WAL_MAX_VERSION`（`3007000`）、页大小、checkpoint 序号 `nCkpt`、两个 salt 值和一段校验和。每帧由 24 字节帧头（`WAL_FRAME_HDRSIZE`）加一整页数据组成，帧头依次是页号、提交时的数据库页数（非提交帧为 0）、两个 salt、两个校验和字。帧在 WAL 文件中的起始偏移由宏 `walFrameOffset` 决定：`WAL_HDRSIZE + (iFrame-1) * (szPage + WAL_FRAME_HDRSIZE)`。

校验和由 `walChecksumBytes` 生成：它对输入按 32 位字成对累加，得到 `s1`、`s2` 两个累加量；`nByte` 必须是 8 的倍数且在 8 到 65536 之间（函数内的断言写明该范围）。若本机字节序与 WAL 文件记录的字节序不一致，则对每个 32 位字先做 `BYTESWAP32` 转换再累加。

写帧的入口是 `sqlite3WalFrames`，它把 `walFrames` 包在 `SEH_TRY`/`SEH_EXCEPT` 里，异常时由 `walHandleException` 统一释放锁并返回 `SQLITE_IOERR`。读帧有两个入口：`sqlite3WalFindFrame` 查某页号对应的帧号，`sqlite3WalReadFrame` 按帧号把页数据读进调用方缓冲区。

本机制只负责帧的编码、追加与读回。它不负责把帧回写进数据库文件（那是 checkpoint，见 h03），也不负责回滚日志路径（G 组）。WAL 模式本身的开关、WAL 文件与 wal-index 的建立属于 h01。

## 怎么实现的

写路径（`sqlite3WalFrames` → `walFrames`）：

1. `walFrames` 先断言 `pWal->writeLock` 非零（写锁由 `sqlite3WalBeginWriteTransaction` 获取），并断言 `(isCommit!=0)==(nTruncate!=0)`：要么是提交帧且带页数，要么都不是。
2. 比较私有的 `pWal->hdr` 与共享的 wal-index 头 `walIndexHdr(pWal)`；两者不同说明别的连接改过 wal-index，此时把 `iFirst` 设为 `pLive->mxFrame+1`，表示从这一帧起都可能被覆盖。
3. 调用 `walRestartLog` 尝试把 WAL 重置（回绕到文件开头）。它只在 `pWal->readLock==0` 且 `walCkptInfo(pWal)->nBackfill>0` 时尝试，先取 `WAL_READ_LOCK(1)` 的排他锁，成功则调 `walRestartHdr` 重置头部：`nCkpt` 加一、`mxFrame` 置 0、第一个 salt 加一、第二个 salt 换成新随机值（`sqlite3_randomness` 产生），并把 `nBackfill` 与 `nBackfillAttempted` 清零。取锁失败且错误码不是 `SQLITE_BUSY` 时直接返回错误。
4. 若 `pWal->hdr.mxFrame==0`（本次是 WAL 的第一帧），先写 WAL 头部：魔数写成 `WAL_MAGIC | SQLITE_BIGENDIAN`，版本写 `WAL_MAX_VERSION`，页大小写 `szPage`，`nCkpt` 写 `pWal->nCkpt`；若 `nCkpt==0` 则用 `sqlite3_randomness(8, ...)` 生成 salt。头部前 `WAL_HDRSIZE-2*4` 字节参与校验和，写在偏移 24 和 28。写完头部后，若 `pWal->syncHeader` 为真则调 `sqlite3OsSync(pWal->pWalFd, CKPT_SYNC_FLAGS(sync_flags))`；`syncHeader` 在 `sqlite3WalOpen` 里默认置 1，仅当 VFS 具备 `SQLITE_IOCAP_SEQUENTIAL` 时才清零。紧接着若 `(int)pWal->szPage!=szPage` 则返回 `SQLITE_CORRUPT_BKPT`。
5. 逐页遍历脏页链表 `pList`（按 `p->pDirty` 前进）。若 `iFirst` 非零且该页满足 `p->pDirty || isCommit==0`，先用 `walFindFrame` 查该页是否已有帧：命中且帧号 `>= iFirst` 时，只把页数据覆盖写到 `walFrameOffset(iWrite, szPage) + WAL_FRAME_HDRSIZE` 处，并把 `pWal->iReCksum` 设为该帧号（取更小者），清掉该页的 `PGHDR_WAL_APPEND` 标志后跳过追加。
6. 其余情况调用 `walWriteOneFrame` 追加新帧。`walWriteOneFrame` 先用 `walEncodeFrame` 填帧头，再分两次写：先写 `WAL_FRAME_HDRSIZE` 字节帧头，再写 `p->szPage` 字节页数据，两次都经 `walWriteToLog` 以便在跨越同步点时先 fsync 再续写。
7. `walEncodeFrame` 在 `pWal->iReCksum==0`（正常追加）时，把 8 字节 salt 拷进帧头偏移 8，再用当前 `aFrameCksum` 作为初值对帧头前 8 字节和整页数据各做一次 `walChecksumBytes`，结果写入帧头偏移 16 和 20。当 `pWal->iReCksum!=0`（本次事务覆盖过旧帧）时，帧头偏移 8 起的 16 字节全部清零，留待重算。
8. 提交时若 `pWal->iReCksum` 非零，调 `walRewriteChecksums` 重算校验和：起始校验和取自 `pWal->iReCksum==1` 时的 WAL 头部偏移 24，否则取上一帧帧头偏移 16；然后从 `pWal->iReCksum` 起逐帧读回，用 `walEncodeFrame` 重算并写回帧头。缓冲区由 `sqlite3_malloc(szPage + WAL_FRAME_HDRSIZE)` 分配，失败返回 `SQLITE_NOMEM_BKPT`。
9. 提交且 `WAL_SYNC_FLAGS(sync_flags)!=0` 时处理落盘。若 `pWal->padToSectorBoundary` 为真，用 `sqlite3SectorSize(pWal->pWalFd)` 得到扇区大小，把同步点 `iSyncPoint` 向上取整到扇区边界，并重复写入最后一页（带提交标记）直到跨过该边界，重复帧数记在 `nExtra`；只有 `iSyncPoint==iOffset` 时才直接 `sqlite3OsSync`。`padToSectorBoundary` 在 `sqlite3WalOpen` 里默认置 1，仅当 VFS 具备 `SQLITE_IOCAP_POWERSAFE_OVERWRITE` 时清零。
10. 提交且 `pWal->truncateOnCommit` 且 `pWal->mxWalSize>=0` 时，用 `walLimitSize` 按 `PRAGMA journal_size_limit` 截断 WAL；实际截断尺寸取 `walFrameOffset(iFrame+nExtra+1, szPage)` 与 `pWal->mxWalSize` 中较大的一个。
11. 最后把新帧登记进 wal-index：只对带 `PGHDR_WAL_APPEND` 标志的页调 `walIndexAppend`，重复帧按 `pLast->pgno` 追加。成功后更新私有头：`hdr.szPage` 写成字节交换后的页大小，`hdr.mxFrame` 设为当前帧号；提交时 `hdr.iChange` 加一、`hdr.nPage` 设为 `nTruncate`，并调 `walIndexWriteHdr` 把头写进共享内存，同时把 `pWal->iCallback` 设为当前帧号供 `sqlite3WalCallback` 返回。

读路径：

- `sqlite3WalFindFrame` 把 `walFindFrame` 包在 `SEH_TRY` 里，异常时返回 `SQLITE_IOERR_IN_PAGE`。`walFindFrame` 从 `pWal->hdr.mxFrame` 取得可见的最后一帧号 `iLast`；若 `iLast==0` 或 `pWal->readLock==0 && pWal->bShmUnreliable==0`，直接返回帧号 0（表示 WAL 里没有该页）。否则从 `walFramePage(iLast)` 往 `walFramePage(pWal->minFrame)` 逐个哈希表倒查，哈希键是 `walHash(pgno)`，槽位冲突时按 `walNextHash` 线性推进；候选帧必须满足帧号 `<= iLast`、帧号 `>= pWal->minFrame`、且 `aPgno` 里的页号等于 `pgno`。冲突计数耗尽（`nCollide` 递减到 0）时返回 `SQLITE_CORRUPT_BKPT`。
- `sqlite3WalReadFrame` 从 `pWal->hdr.szPage` 取页大小并按 `(sz&0xfe00) + ((sz&0x0001)<<16)` 还原真实页大小（`walFrames` 写入时用 `(szPage&0xff00) | (szPage>>16)` 编码，65536 被编码成 1）。若 `nOut` 大于真实页大小，多余部分清零并把 `nOut` 收窄为页大小。数据从 `walFrameOffset(iRead, sz) + WAL_FRAME_HDRSIZE` 处读出，直接返回 `sqlite3OsRead` 的结果。
- `sqlite3WalFramesize` 在 `SQLITE_ENABLE_ZIPVFS` 下才编译，返回 `pWal->szPage`；`pWal` 为空时返回 0。它的调用方是 `pager.c` 的 `sqlite3PagerGetPagesize` 一类的页大小查询路径（`sqlite3WalFramesize(pPager->pWal)`），供 zipvfs 判断 WAL 帧内容尺寸。

真实调用链（均在 `src/pager.c` 中核对过）：

- 写帧：`pagerWalFrames` → `sqlite3WalFrames(pPager->pWal, pPager->pageSize, pList, nTruncate, isCommit, pPager->walSyncFlags)`。`pagerWalFrames` 在提交时会把页号大于 `nTruncate` 的页从脏链表里剔除，因为这些页永远不会被读到。
- 读帧：`readDbPage` 先调 `sqlite3WalFindFrame(pPager->pWal, pPg->pgno, &iFrame)`，若返回的帧号非零则调 `sqlite3WalReadFrame(pPager->pWal, iFrame, pPager->pageSize, pPg->pData)`；帧号为 0 时才回退到 `sqlite3OsRead(pPager->fd, ...)` 直接读数据库文件。

## 关键取舍

- **用哈希表定位页，而不是线性扫描 WAL（源码事实）**：`walFindFrame` 只在 wal-index 的哈希表里查，页号到槽位的映射是 `walHash(pgno)`。代价是 wal-index 里必须维护 `aPgno` 与 `aHash` 两套数组，且查找条件比独占访问时更严（额外要求帧号 `<= iLast`），用来过滤读事务开始后才写入的、可能已被并发写坏的槽位。
- **被覆盖的旧帧不重写整帧，只重写页数据（源码事实）**：命中已有帧时只写 `szPage` 字节页数据，帧头保持原样，并把 `pWal->iReCksum` 记下来。这省掉了一次页数据写，但代价是校验和链被打断，必须在提交时由 `walRewriteChecksums` 从 `iReCksum` 起逐帧回读重算。
- **提交帧用「数据库页数非零」标记，而不是额外标志位（源码事实）**：`walEncodeFrame` 把 `nTruncate` 写进帧头偏移 4，非提交帧该字段为 0。`walFrames` 里 `nDbSize = (isCommit && p->pDirty==0) ? nTruncate : 0`，即只有提交时链表上最后一页才带页数。代价是「最后一页」这个位置信息被编码进了页链结构里。
- **帧校验和与页校验和分开（源码事实）**：帧头只对前 8 字节（页号与页数）和整页数据做校验，salt 与校验和字段本身不参与计算。这让 `walRewriteChecksums` 能只重写帧头而不动页数据。
- **同步级别可按事务与 checkpoint 分别配置（源码事实）**：`wal.h` 里 `WAL_SYNC_FLAGS(X)` 取低 2 位、`CKPT_SYNC_FLAGS(X)` 取第 2 到 3 位；`pager.c` 里 `pPager->walSyncFlags = (pPager->syncFlags<<2)`，并在 `synchronous=FULL` 时额外置上 `SQLITE_SYNC_FULL<<2`。因此 `PRAGMA synchronous=OFF` 时 `WAL_SYNC_FLAGS(sync_flags)` 为 0，帧写入不做 padding 也不做 fsync。
- **扇区对齐补帧换取顺序写安全（源码事实）**：`padToSectorBoundary` 为真时重复写最后一页直到跨过扇区边界，保证提交帧完整落在一个扇区内。代价是 WAL 文件里会出现重复帧，`nExtra` 记录这份开销。
- **回退到读数据库文件路径（源码事实）**：`sqlite3WalFindFrame` 返回帧号 0 时，`readDbPage` 跳过 `sqlite3WalReadFrame`，改走 `sqlite3OsRead(pPager->fd, pPg->pData, pPager->pageSize, iOffset)` 从数据库文件读同一页。
- **回退到追加而非覆盖（源码事实）**：`walRestartLog` 拿不到 `WAL_READ_LOCK(1)` 的排他锁且错误码是 `SQLITE_BUSY` 时，函数不返回错误而是继续，帧仍按追加方式写到 `pWal->hdr.mxFrame` 之后。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/wal.c` | `sqlite3WalFrames` | 写帧公开入口，把 `walFrames` 包在 `SEH_TRY` 中，异常交给 `walHandleException` |
| `src/wal.c` | `walFrames` | 写帧主体：写 WAL 头部、逐页追加或覆盖帧、补扇区、重算校验和、更新 wal-index |
| `src/wal.c` | `walWriteOneFrame` | 组装并写出一帧：先写 24 字节帧头，再写整页数据 |
| `src/wal.c` | `walEncodeFrame` | 生成 24 字节帧头（页号、页数、salt、校验和），重算路径下把后 16 字节清零 |
| `src/wal.c` | `walChecksumBytes` | 生成或延展 8 字节校验和，字节序不匹配时先做 `BYTESWAP32` |
| `src/wal.c` | `walRewriteChecksums` | 事务内覆盖过旧帧时，从 `pWal->iReCksum` 起逐帧回读重算并写回校验和 |
| `src/wal.c` | `walRestartLog` | 写帧前尝试把 WAL 重置回文件开头，失败则继续追加 |
| `src/wal.c` | `walRestartHdr` | 重置 WAL 头部：递增 `nCkpt`、清零 `mxFrame`、换 salt、清 `nBackfill` |
| `src/wal.c` | `walIndexAppend` | 把帧号与页号登记进 wal-index 的 `aPgno` 与哈希表 |
| `src/wal.c` | `walFindFrame` | 在 wal-index 哈希表中定位某页号对应的最大可见帧号 |
| `src/wal.c` | `sqlite3WalFindFrame` | `walFindFrame` 的 `SEH_TRY` 包装，异常返回 `SQLITE_IOERR_IN_PAGE` |
| `src/wal.c` | `sqlite3WalReadFrame` | 按帧号把页数据读进调用方缓冲区，还原真实页大小并清零多余部分 |
| `src/wal.c` | `sqlite3WalFramesize` | 返回 WAL 帧内容尺寸即 `pWal->szPage`，仅在 `SQLITE_ENABLE_ZIPVFS` 下编译 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 的 `storage/` 与 `wal/` 分层同样要把「页内容 + 元信息 + 校验」打包成可追加的日志记录，其 WAL 层与 SQLite 的 WAL 在职责划分上同构——都把「写日志」与「把日志回放到主存储」分开，帧的编码细节与 checkpoint 策略彼此独立。**架构推论：** 但 SQLite 的帧头把提交标记（数据库页数）与 salt 直接编码进定长 24 字节头，帧之间靠校验和串成一条链；主核若采用变长记录或按记录独立校验，则不能直接照搬 `walEncodeFrame` 与 `walRewriteChecksums` 的链式重算策略。

## 参考

- [SQLite 官方文档：WAL-mode File Format](https://sqlite.org/walformat.html) —— 给出 wal-index 头部字段、mxFrame/nBackfill 语义、哈希表布局与锁矩阵，可与源码中的 `WalIndexHdr` 对照。
- [SQLite 官方文档：The Write-Ahead Log (WAL)](https://sqlite.org/wal.html) —— 说明 WAL 模式的工作方式、读者与写者关系及自动 checkpoint 的定位。
- [SQLite 官方文档：Database File Format（帧格式小节）](https://sqlite.org/fileformat2.html#walformat) —— 定义 WAL 文件头部与帧头的字节布局。
- [GitHub 官方镜像源码：src/wal.c](https://github.com/sqlite/sqlite/blob/master/src/wal.c) —— 支撑本文关于帧编码、校验和链与哈希查找的实现事实。
