---
project: sqlite
feature: page-writeback
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerFlush, sqlite3PagerSync, sqlite3PagerTruncateImage, sqlite3PagerUnrefNotNull]
---

# 脏页写回与同步

## 一句话职责

把页缓存里已经被标记为脏、且当前没有其他引用的页写回数据库文件，并按 `PRAGMA synchronous` 决定的强度把文件同步到磁盘；同时在事务提交时按需把文件截断到新的页数。

## 机制是什么

这一层解决的是「缓存里的新内容和磁盘上的旧文件如何一致」的问题，分三个动作：

1. **写回（writeback）**：把脏页内容写到数据库文件的对应页偏移上。
2. **同步（sync）**：对数据库文件描述符发起 `xSync`，把内容交给操作系统持久化。
3. **截断（truncate）**：当数据库镜像变小（auto-vacuum 把空闲页移到文件尾并裁掉，或事务扩展后再回缩）时，把磁盘文件裁到新的页数。

边界上，本机制不负责把修改前的原像写进回滚日志（那是 G03 的 `sqlite3PagerWrite` / `sqlite3PagerCommitPhaseOne` 里的 `syncJournal` 与 `pagerAddPageToRollbackJournal`），也不负责真正的 OS 层 `xWrite` / `xSync` 系统调用实现（J 组）；本机制只决定「写哪一页、写到哪个偏移、什么时候 sync、什么时候 truncate」。

**架构推论：** 由于写回与同步都发生在同一个 `Pager` 对象上、且都作用于同一个文件描述符 `pPager->fd`，本机制在 WAL 模式下被旁路——WAL 模式下脏页写往 WAL 帧而不是数据库文件，`sqlite3PagerSync` 对主库文件而言不再承担事务持久化的职责。

## 怎么实现的

### 写回入口：`sqlite3PagerFlush`

`sqlite3PagerFlush` 是「把所有未被引用的脏页刷到磁盘」的公开入口。函数体的逻辑很短：

- 先取 `int rc = pPager->errCode`，此前若已有错误则直接带着这个错误码返回。
- `if( !MEMDB )` 才继续：内存数据库没有文件可写。
- 调 `sqlite3PcacheDirtyList(pPager->pPCache)` 拿到按页号排序的脏页链表。
- 遍历链表，`if( pList->nRef==0 )` 即该页没有其他引用者，调用 `pagerStress((void*)pPager, pList)` 把它刷出去；`pList->nRef!=0` 的页被跳过——有引用的页正在被上层使用，不能在它脚下换内容。

### 真正的落盘：`pagerStress` → `pager_write_pagelist`

`pagerStress` 是页缓存交给分页器的「回调式回收钩子」（在 `sqlite3PagerOpen` 里以 `memDb?pagerStress:0` 的形式注册给 `sqlite3PcacheOpen`）。它的工作顺序：

- 早退条件一：`if( NEVER(pPager->errCode) ) return SQLITE_OK;`——已处于错误态时不回收，避免把半写状态落盘造成损坏。
- 早退条件二：`pPager->doNotSpill` 命中 `SPILLFLAG_ROLLBACK` 或 `SPILLFLAG_OFF` 时不回收；此外若该页带 `PGHDR_NEED_SYNC` 且处于 `SPILLFLAG_NOSYNC` 保护下也不回收。`SPILLFLAG_OFF` 由 `sqlite3PagerSetFlags` 在未设置 `PAGER_CACHESPILL` 时置位，对应 `PRAGMA cache_spill=OFF`。
- `pPager->aStat[PAGER_STAT_SPILL]++` 记录一次溢出回收（`PAGER_STAT_SPILL` 定义为 `3`）。
- WAL 模式走另一条路：先 `subjournalPageIfRequired(pPg)`，再 `pagerWalFrames(pPager, pPg, 0, 0)` 写一帧到 WAL。
- 回滚日志模式：`#ifdef SQLITE_ENABLE_BATCH_ATOMIC_WRITE` 下非临时文件先 `sqlite3JournalCreate(pPager->jfd)`；随后若 `pPg->flags&PGHDR_NEED_SYNC` 或 `pPager->eState==PAGER_WRITER_CACHEMOD`，先调 `syncJournal(pPager, 1)` 把日志同步。
- 最后 `rc = pager_write_pagelist(pPager, pPg)` 把页内容写进数据库文件。
- 成功时调 `sqlite3PcacheMakeClean(pPg)` 把页标记为干净。

`pager_write_pagelist` 是真正调用 OS 写的地方：

- 前置断言：`assert( !pagerUseWal(pPager) )`、`assert( pPager->eLock==EXCLUSIVE_LOCK )`——回滚日志模式下写数据库文件必须已持有排他锁。
- 若 `!isOpen(pPager->fd)`（临时文件尚未打开），先 `pagerOpentemp(pPager, pPager->fd, pPager->vfsFlags)`。
- **文件大小预告**：当 `pPager->dbHintSize<pPager->dbSize` 且当前页脏或页号超过 `dbHintSize` 时，用 `sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_SIZE_HINT, &szFile)` 告诉 VFS 最终文件大小为 `pPager->pageSize * pPager->dbSize`，并把 `pPager->dbHintSize` 更新为 `pPager->dbSize`。
- 逐页写入循环里有个关键判断：`if( pgno<=pPager->dbSize && 0==(pList->flags&PGHDR_DONT_WRITE) )`。页号超过 `pPager->dbSize` 的页**不写**——这正是 `sqlite3PagerTruncateImage` 收缩镜像后，被裁掉的页仍在缓存里但不应再落盘的机制；`PGHDR_DONT_WRITE` 由 `sqlite3PagerDontWrite` 置位，用于大 DELETE 时跳过不再需要的页。
- 写入偏移：`i64 offset = (pgno-1)*(i64)pPager->pageSize;`，写调用为 `sqlite3OsWrite(pPager->fd, pData, pPager->pageSize, offset)`，一次写满一页。
- 第 1 页特殊处理：写之前先 `pager_write_changecounter(pList)`，它把 `dbFileVers` 加一后用 `put32bits` 写入页内偏移 `24` 处，同时在偏移 `92` 写同样的计数器、偏移 `96` 写 `SQLITE_VERSION_NUMBER`。写完第 1 页后用 `memcpy(&pPager->dbFileVers, &pData[24], sizeof(pPager->dbFileVers))` 把内存里的版本号同步成文件里的值。
- 文件增长时更新 `pPager->dbFileSize`（`if( pgno>pPager->dbFileSize ) pPager->dbFileSize = pgno;`），并 `pPager->aStat[PAGER_STAT_WRITE]++`（`PAGER_STAT_WRITE` 定义为 `2`）。
- 写完通知备份：`sqlite3BackupUpdate(pPager->pBackup, pgno, (u8*)pList->pData)`。
- 无论写没写，都调 `pager_set_pagehash(pList)` 更新页哈希。

### 同步：`sqlite3PagerSync`

```c
int sqlite3PagerSync(Pager *pPager, const char *zSuper){
  int rc = SQLITE_OK;
  void *pArg = (void*)zSuper;
  rc = sqlite3OsFileControl(pPager->fd, SQLITE_FCNTL_SYNC, pArg);
  if( rc==SQLITE_NOTFOUND ) rc = SQLITE_OK;
  if( rc==SQLITE_OK && !pPager->noSync ){
    assert( !MEMDB );
    rc = sqlite3OsSync(pPager->fd, pPager->syncFlags);
  }
  return rc;
}
```

三步：

1. 先用 `sqlite3OsFileControl` 发 `SQLITE_FCNTL_SYNC`（操作码 `21`），参数是 `zSuper` 指针。这给自定义 VFS 一个「顺便处理同步」的机会。VFS 不认识这个操作码时返回 `SQLITE_NOTFOUND`，此时被就地归一成 `SQLITE_OK`，不算错误。
2. 只有上一步成功且 `!pPager->noSync` 时，才真正调 `sqlite3OsSync(pPager->fd, pPager->syncFlags)`。
3. `sqlite3OsSync` 在 `src/os.c` 里的实现是 `return flags ? id->pMethods->xSync(id, flags) : SQLITE_OK;`——`flags` 为 `0` 时直接返回 `SQLITE_OK`，根本不进 VFS。

`syncFlags` 的取值由 `sqlite3PagerSetFlags` 决定：`pPager->noSync` 为真时 `syncFlags = 0`（于是 `sqlite3OsSync` 直接短路）；否则若 `pgFlags & PAGER_FULLFSYNC` 则 `syncFlags = SQLITE_SYNC_FULL`（`0x00003`），否则 `syncFlags = SQLITE_SYNC_NORMAL`（`0x00002`）。`noSync` 在 `sqlite3PagerSetFlags` 开头被置位的情况有两种：`pPager->tempFile` 为真（临时文件不落盘），或 `level==PAGER_SYNCHRONOUS_OFF`（对应 `PRAGMA synchronous=OFF`）。

### 截断：`sqlite3PagerTruncateImage` 与 `pager_truncate`

`sqlite3PagerTruncateImage` **不改磁盘文件**，只改内存状态：函数体只有一句 `pPager->dbSize = nPage;`，并带两个断言 `pPager->dbSize>=nPage || CORRUPT_DB` 和 `pPager->eState>=PAGER_WRITER_CACHEMOD`。它的注释明确写了这个函数「只在提交事务前调用一次」，调用之后事务必须提交或回滚。它的作用是让 `pager_write_pagelist` 里那句 `pgno<=pPager->dbSize` 判断生效，把超出新镜像的脏页拦在磁盘之外。

真正改文件的是 `pager_truncate`：

- 只在 `isOpen(pPager->fd)` 且状态为 `pPager->eState>=PAGER_WRITER_DBMOD` 或 `pPager->eState==PAGER_OPEN` 时动手，并断言 `pPager->eLock==EXCLUSIVE_LOCK`。
- 先 `sqlite3OsFileSize(pPager->fd, &currentSize)` 取当前大小，`newSize = szPage*(i64)nPage`。
- `currentSize>newSize` 时调 `sqlite3OsTruncate(pPager->fd, newSize)` 真正截短。
- `currentSize<newSize` 且 `(currentSize+szPage)<=newSize` 时不改文件内容，而是用 `sqlite3OsFileControlHint(pPager->fd, SQLITE_FCNTL_SIZE_HINT, &newSize)` 预告大小，再把 `pPager->pTmpSpace` 清零后 `sqlite3OsWrite(pPager->fd, pTmp, szPage, newSize-szPage)` 补写最后一个零页——注释说明这是为了避开某些 OS 实现对「把文件截断到比当前更大」处理不当的问题。
- 成功后 `pPager->dbFileSize = nPage`。

`pager_truncate` 的调用点在 `sqlite3PagerCommitPhaseOne` 里：`if( pPager->dbSize>pPager->dbFileSize )` 时算出 `nNew = pPager->dbSize - (pPager->dbSize==PAGER_SJ_PGNO(pPager))` 再调 `pager_truncate(pPager, nNew)`。注释解释了这一分支的成因：本次事务扩展了数据库镜像、随后最后一页又被移入 freelist，导致最后一页从未写出，磁盘文件偏小，此时补截一次。

### 提交路径上的完整顺序

`sqlite3PagerCommitPhaseOne` 在非 WAL 分支里的顺序是：

1. `pager_incr_changecounter` 更新 change-counter（或在 atomic-write 优化可用时走 `pager_incr_changecounter(pPager, 1)` 的直接写模式）。
2. `writeSuperJournal(pPager, zSuper)` 把主日志名写进日志。
3. `syncJournal(pPager, 0)` 同步回滚日志。
4. `pList = sqlite3PcacheDirtyList(pPager->pPCache)` 取出脏页链表。
5. `pager_write_pagelist(pPager, pList)` 把脏页写进数据库文件。
6. `sqlite3PcacheCleanAll(pPager->pPCache)` 把所有页标记为干净。
7. 需要时 `pager_truncate` 补截文件。
8. `if( !noSync ) rc = sqlite3PagerSync(pPager, zSuper);` 最终同步数据库文件。

第 8 步的 `noSync` 是 `sqlite3PagerCommitPhaseOne` 的入参，注释写明：此时数据库文件本身不同步，调用方必须在 `sqlite3PagerCommitPhaseTwo` 删除日志文件之前自己调 `sqlite3PagerSync`。

### 引用释放：`sqlite3PagerUnrefNotNull`

`sqlite3PagerUnrefNotNull` 是「释放一个页引用」的内部版本：

- `if( pPg->flags & PGHDR_MMAP )` 时走 `pagerReleaseMapPage(pPg)`（内存映射页的释放路径不同），否则走 `sqlite3PcacheRelease(pPg)`。
- 函数注释明确约束：它只能用于「不是 page1 的最后一次引用」的场景，因为 btree 层在整个生命周期内持有 page1；末尾的 `assert( sqlite3PcacheRefCount(pPager->pPCache)>0 )` 标注了 `tag-20230419-2` 这个不变式。释放 page1 要用 `sqlite3PagerUnrefPageOne`，后者额外调 `pagerUnlockIfUnused(pPager)` 在页数归零时释放数据库锁。

在写回机制里它出现在 `pagerWriteLargeSector` 中：扇区大于页大小时，需要把同一扇区内的所有页一并登记进日志，循环里用 `sqlite3PagerGet` 取页后调 `pager_write(pPage)`，再用 `sqlite3PagerUnrefNotNull(pPage)` 释放引用。

## 关键取舍

**取舍一：只刷「无引用」的脏页。** `sqlite3PagerFlush` 的 `pList->nRef==0` 判断意味着被上层持有的脏页不会被后台刷盘。这换来的是「不会在别人脚下改内容」的内存安全，代价是脏页堆积在缓存里。

**取舍二：写回与日志同步的先后顺序不可颠倒。** `pagerStress` 里先 `syncJournal(pPager, 1)` 再 `pager_write_pagelist`，因为一旦页内容落到数据库文件，回滚日志里必须有对应的原像，否则崩溃后无法回滚。`PGHDR_NEED_SYNC` 这个页级标记正是用来表达「这一页的原像必须先落盘」的。

**取舍三：`sqlite3PagerSync` 把 `SQLITE_FCNTL_SYNC` 的 `SQLITE_NOTFOUND` 当成成功。** 源码注释在 `src/os.c` 的 `sqlite3OsFileControl` 处写明：核心必须用 `OsFileControl` 而不是 `OsFileControlHint`，因为自定义 VFS（如 zipvfs）在这里返回错误，通常意味着提交真的失败了。所以只有 `SQLITE_NOTFOUND`（「不认识这个操作码」）被放过，其他错误码照常向上传。

**取舍四：截断分「缩小」和「扩大」两种处理。** 缩小时真调 `sqlite3OsTruncate`；扩大时不动已有内容，只补写一个零页并用 `SQLITE_FCNTL_SIZE_HINT` 预告最终大小。这是为绕开某些 OS 实现对「扩大文件」语义不一致的处理。

**取舍五：`synchronous` 的强度在 `sqlite3PagerSetFlags` 里一次性定档。** `noSync` 决定 `syncFlags` 是否为 `0`（`0` 时 `sqlite3OsSync` 连 VFS 都不进），`PAGER_FULLFSYNC` 决定用 `SQLITE_SYNC_FULL` 还是 `SQLITE_SYNC_NORMAL`。`sqlite3PagerSetFlags` 的注释特别提醒不要把 `synchronous=FULL` 与 `SQLITE_SYNC_FULL` 混为一谈：后者指 macOS 风格的 `fcntl(F_FULLFSYNC)`，在非 macOS 平台上两者没有区别。

**取舍六：页级跳过写回。** `PGHDR_DONT_WRITE`（由 `sqlite3PagerDontWrite` 置位，条件是 `!pPager->tempFile && (pPg->flags&PGHDR_DIRTY) && pPager->nSavepoint==0`）让大 DELETE 场景下已经作废的页不必落盘。`sqlite3PagerDontWrite` 的注释称这一优化能让大 DELETE 提速四倍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerFlush` | 把缓存里所有无引用的脏页刷盘；取 `sqlite3PcacheDirtyList` 的有序脏页链表，只处理 `nRef==0` 的页 |
| src/pager.c | `sqlite3PagerSync` | 同步数据库文件：先发 `SQLITE_FCNTL_SYNC`，再按 `syncFlags` 调 `sqlite3OsSync`；`noSync` 为真时跳过 |
| src/pager.c | `sqlite3PagerTruncateImage` | 只改内存状态 `pPager->dbSize = nPage`，让超出新镜像的脏页在写回时被 `pgno<=pPager->dbSize` 拦掉 |
| src/pager.c | `sqlite3PagerUnrefNotNull` | 释放页引用；MMAP 页走 `pagerReleaseMapPage`，其余走 `sqlite3PcacheRelease`；不得用于 page1 的最后一次引用 |
| src/pager.c | `pagerStress` | 页缓存的回收钩子；检查 `doNotSpill`/`SPILLFLAG_*` 早退，WAL 下写帧，回滚日志下先 `syncJournal` 再写回，成功后 `sqlite3PcacheMakeClean` |
| src/pager.c | `pager_write_pagelist` | 真正落盘：`sqlite3OsWrite` 按 `(pgno-1)*pageSize` 偏移写满一页，第 1 页先 `pager_write_changecounter`，写后更新 `dbFileSize` 并通知 `sqlite3BackupUpdate` |
| src/pager.c | `pager_truncate` | 实际改文件大小：缩小时 `sqlite3OsTruncate`，扩大时补写一个零页并用 `SQLITE_FCNTL_SIZE_HINT` 预告大小 |
| src/pager.c | `sqlite3PagerCommitPhaseOne` | 提交编排：更新 change-counter → `writeSuperJournal` → `syncJournal` → `pager_write_pagelist` → `sqlite3PcacheCleanAll` → `pager_truncate` → `sqlite3PagerSync` |
| src/pager.c | `sqlite3PagerSetFlags` | 依据 `PAGER_SYNCHRONOUS_MASK`/`PAGER_FULLFSYNC`/`PAGER_CACHESPILL` 决定 `noSync`、`syncFlags`、`walSyncFlags` 与 `SPILLFLAG_OFF` |
| src/pager.c | `pager_write_changecounter` | 把 change-counter 写入页内偏移 `24`，偏移 `92` 写同一计数器，偏移 `96` 写 `SQLITE_VERSION_NUMBER` |
| src/pager.c | `sqlite3PagerDontWrite` | 置 `PGHDR_DONT_WRITE` 跳过无用的页写回；条件含 `pPager->nSavepoint==0` |
| src/pager.c | `pagerWriteLargeSector` | 扇区大于页大小时把同扇区所有页一并登记进日志，循环内用 `sqlite3PagerUnrefNotNull` 释放引用 |
| src/pcache.c | `sqlite3PcacheDirtyList` | 返回按页号排序的全部脏页链表，`sqlite3PagerFlush` 与 `sqlite3PagerCommitPhaseOne` 都用它取脏页 |
| src/os.c | `sqlite3OsSync` | `flags ? id->pMethods->xSync(id, flags) : SQLITE_OK`，`flags` 为 `0` 时不进 VFS |
| src/os.c | `sqlite3OsFileControl` | 派发 `xFileControl`；注释说明核心必须用本函数而非 `sqlite3OsFileControlHint`，以便捕获 VFS 的真实失败 |

## 与主核 / AI 的关系

本机制是主核 `engine/db/storage/` 一层可以直接对照的样板：主核自研分页器同样要面对「脏页何时落盘」「文件何时 fsync」「文件如何截断」三件事，而 SQLite 在这里给出的答案有两点值得移植——

- **写回与元数据同步分离**：`sqlite3PagerSync` 里 `SQLITE_FCNTL_SYNC` 与 `sqlite3OsSync` 分两步，先把「同步这件事」交给可替换的 VFS 层，再决定是否真的落盘。主核若要支持自定义存储后端，这个分层直接可用。
- **截断只是内存状态的推迟执行**：`sqlite3PagerTruncateImage` 只改 `dbSize`，真正动文件的时机由提交路径统一决定，避免了「页已经被判死却仍占着磁盘」的不一致窗口。

对 AI 检索而言，本页覆盖的是 SQLite 里「缓存 → 文件」这条数据落地的边界：与之相邻的 G02（页获取与引用计数）解释了脏页如何被标记，G03（回滚日志写入）解释了原像如何被保存，H 组（WAL）则是同一批脏页的另一种落地方式。

## 参考

- [PRAGMA synchronous](https://sqlite.org/pragma.html#pragma_synchronous) —— 官方文档中同步级别（OFF / NORMAL / FULL / EXTRA）与各模式行为的定义。
- [PRAGMA cache_spill](https://sqlite.org/pragma.html#pragma_cache_spill) —— 官方文档中缓存溢出（spill）行为与 `PRAGMA cache_spill=N` 阈值语义的定义。
- [PRAGMA journal_mode](https://sqlite.org/pragma.html#pragma_journal_mode) —— 官方文档中 DELETE / TRUNCATE / PERSIST / MEMORY / WAL / OFF 各日志模式的说明。
- [Atomic Commit In SQLite](https://sqlite.org/atomiccommit.html) —— 官方文档中回滚日志与原子提交的整体流程。
- [sqlite/sqlite — src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c) —— 分页器源码，本页所有函数体均以此为准。
