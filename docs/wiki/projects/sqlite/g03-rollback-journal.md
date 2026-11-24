---
project: sqlite
feature: rollback-journal
page_type: feature
schema_version: 1
refs:
  - path: src/pager.c
    symbols: [sqlite3PagerWrite, sqlite3PagerRollback, sqlite3PagerCommitPhaseOne, sqlite3PagerCommitPhaseTwo]
---

# 回滚日志写入

## 一句话职责

`sqlite3PagerWrite` 负责在页被改写之前把**修改前的原始页镜像**写进回滚日志并把该页标记为可写；`sqlite3PagerCommitPhaseOne` 负责把「日志已同步 + 脏页已落库 + 数据库文件已同步」这一串动作做完，使事务达到「日志仍可用于回滚、数据已进主库」的临界点；`sqlite3PagerCommitPhaseTwo` 负责终结日志文件（删除 / 截断 / 清零头部），宣告事务不可逆提交；`sqlite3PagerRollback` 负责在事务被撤销时从日志读回原像，把数据库文件与页缓存恢复到事务开始前的状态。

## 机制是什么

回滚日志（rollback journal）是 SQLite 在**非 WAL 模式**下实现事务原子性的手段。它的核心不变式是：**只要数据库文件被改过，原始内容就一定已经在日志里且已同步落盘**。有了这条不变式，崩溃后只要拿日志把原像写回数据库文件，数据库就回到了事务开始前的状态。

整个机制由四个状态阶段串起，对应 `Pager.eState` 的四个取值（定义在 `src/pager.c` 的状态机注释里）：

| 阶段 | `Pager.eState` | 含义 | 进入方式 |
|---|---|---|---|
| 只读 | `PAGER_READER` | 没有写事务 | 常态 |
| 已加写锁 | `PAGER_WRITER_LOCKED` | 已拿到 `RESERVED_LOCK`，日志尚未打开 | `sqlite3PagerBegin` |
| 缓存已改 | `PAGER_WRITER_CACHEMOD` | 日志已打开、页已被标记可写、但页数据尚未写进主库 | `pager_open_journal` / `pager_write` |
| 主库已改 | `PAGER_WRITER_DBMOD` | 脏页已写进数据库文件、日志已同步 | `syncJournal` |
| 已终结 | `PAGER_WRITER_FINISHED` | 提交第一阶段完成，等待日志被终结 | `sqlite3PagerCommitPhaseOne` |

日志文件本身的布局由三个宏固定（均在 `src/pager.c`）：

- `JOURNAL_PG_SZ(pPager)` = `(pPager->pageSize) + 8` —— 一条页记录的长度：4 字节页号 + 页数据 + 4 字节校验和。
- `JOURNAL_HDR_SZ(pPager)` = `(pPager->sectorSize)` —— 一条日志头占据的空间（通常等于一个磁盘扇区）。
- `aJournalMagic[]` = `0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7` —— 8 字节日志格式魔数。

日志头（由 `writeJournalHdr` 写出）的字段布局由函数头注释给出：8 字节魔数、4 字节记录数 `nRec`（或 `-1` 表示 no-sync 模式）、4 字节随机校验和种子 `cksumInit`、4 字节事务开始时的库页数 `dbOrigSize`、4 字节写入方的扇区大小、4 字节写入方的页大小，其后是 `(JOURNAL_HDR_SZ - 28)` 字节的未用填充。

**本机制的边界**（与页面清单一致）：负责把修改前的原始页写入回滚日志并同步落盘，以及从日志读回原像做回滚；不负责主日志（G05）、不负责 WAL（H 组）。

## 怎么实现的

### `sqlite3PagerWrite`：把原始页写进日志

签名（`src/pager.c`）：`int sqlite3PagerWrite(PgHdr *pPg)`。函数头注释给出了一条硬约束：「The caller must check the return value of this function and be careful not to change any page data unless this routine returns SQLITE_OK.」——调用方在返回非 `SQLITE_OK` 时不得改动页内容。

它的函数体是四路分派：

1. **页已可写且在库大小范围内**：`if( (pPg->flags & PGHDR_WRITEABLE)!=0 && pPager->dbSize>=pPg->pgno )`。此时若 `pPager->nSavepoint` 非零（存在 savepoint），转调 `subjournalPageIfRequired(pPg)`（该页还要进子日志）；否则直接返回 `SQLITE_OK`。函数头注释特别说明它与 `pager_write` 的区别：「this function also deals with the special case where 2 or more pages fit on a single disk sector」。
2. **已有错误**：`else if( pPager->errCode )` —— 直接把 `pPager->errCode` 返回。
3. **扇区大于页**：`else if( pPager->sectorSize > (u32)pPager->pageSize )` —— 转调 `pagerWriteLargeSector(pPg)`（`SQLITE_NOINLINE`）。
4. **其余**：转调 `pager_write(pPg)`。

#### `pager_write`：真正把页写进日志

`pager_write`（`src/pager.c`，静态）的执行顺序：

1. 若 `pPager->eState==PAGER_WRITER_LOCKED`，先调 `pager_open_journal(pPager)` 打开日志。源码注释说明了为什么必须**先开日志再标脏**：「Otherwise, if it were done after calling sqlite3PcacheMakeDirty(), then an error might occur and the pager would end up in WRITER_LOCKED state with pages marked as dirty in the cache.」
2. `sqlite3PcacheMakeDirty(pPg)` —— 把页标脏。
3. 若 `pPager->pInJournal!=0`（即用了回滚日志）且 `sqlite3BitvecTestNotNull(pPager->pInJournal, pPg->pgno)==0`（该页还没进过日志）：
   - 若 `pPg->pgno<=pPager->dbOrigSize`（事务开始时就存在的页）：调 `pagerAddPageToRollbackJournal(pPg)` 把原像写进日志。
   - 否则（事务期间新追加的页）：若 `pPager->eState!=PAGER_WRITER_DBMOD` 则置 `pPg->flags |= PGHDR_NEED_SYNC`。源码注释解释：这类页没有原像可备份，但必须保证主库写出去时数据已落盘。
4. **此时才**置 `pPg->flags |= PGHDR_WRITEABLE`。源码注释写明理由：「Wait until now, after the page has been successfully journalled, before setting the PGHDR_WRITEABLE bit that indicates that the page can be safely modified.」
5. 若 `pPager->nSavepoint>0`，调 `subjournalPageIfRequired(pPg)` 把页也写进语句子日志。
6. 若 `pPager->dbSize<pPg->pgno`，更新 `pPager->dbSize = pPg->pgno`。

#### `pagerAddPageToRollbackJournal`：一条页记录的字节布局

`pagerAddPageToRollbackJournal`（`src/pager.c`，静态，`SQLITE_NOINLINE`）按顺序写三段：

- `write32bits(pPager->jfd, iOff, pPg->pgno)` —— 4 字节页号。
- `sqlite3OsWrite(pPager->jfd, pData2, pPager->pageSize, iOff+4)` —— 页数据。
- `write32bits(pPager->jfd, iOff+pPager->pageSize+4, cksum)` —— 4 字节校验和。

校验和由 `pager_cksum` 计算。函数头注释明确说明它「is not a real checksum. It is really just the sum of the random initial value (pPager->cksumInit) and every 200th byte of the page data」，并警告：「Changing the formula used to compute this checksum results in an incompatible journal file format.」函数体是 `for( i=pPager->pageSize-200; i>0; i-=200 ) cksum += aData[i];`。

写完之后：`pPager->journalOff += 8 + pPager->pageSize`（8 = 4 字节页号 + 4 字节校验和），`pPager->nRec++`，并用 `sqlite3BitvecSet(pPager->pInJournal, pPg->pgno)` 在「已进日志」位图上打标，最后 `rc |= addToSavepointBitvecs(pPager, pPg->pgno)` 同步给所有打开的 savepoint。

函数里还有一条重要的防御：`pPg->flags |= PGHDR_NEED_SYNC` 在**写盘之前**就置上。源码注释解释：「Even if an IO or diskfull error occurs while journalling the page in the block above, set the need-sync flag for the page. Otherwise, when the transaction is rolled back, the logic in playback_one_page() will think that the page needs to be restored in the database file. And if an IO error occurs while doing so, then corruption may follow.」

#### `pagerWriteLargeSector`：扇区大于页时的连带备份

`pagerWriteLargeSector`（`src/pager.c`，静态，`SQLITE_NOINLINE`）只在 `pPager->sectorSize > pPager->pageSize` 时被调用。函数头注释说明理由：「SQLite makes the (reasonable) assumption that all bytes of a sector are written together by hardware. Hence, all bytes of a sector need to be journalled in case of a power loss in the middle of a write.」

它先置 `pPager->doNotSpill |= SPILLFLAG_NOSYNC`，注释说明原因：「we cannot allow a journal header to be written between the pages journaled by this function」。然后用 `nPagePerSector = (pPager->sectorSize/pPager->pageSize)` 算出扇区内页数，用 `pg1 = ((pPg->pgno-1) & ~(nPagePerSector-1)) + 1` 定位扇区首页（该「位与技巧」依赖页大小与扇区大小都是 2 的幂）。随后循环对扇区内每一页调 `pager_write(pPage)`，并检查 `pPage->flags&PGHDR_NEED_SYNC`。最后若 `needSync` 为真，给扇区内所有页都置上 `PGHDR_NEED_SYNC`，注释说明：「Because writing to any of these nPage pages may damage the others, the journal file must contain sync()ed copies of all of them before any of them can be written out to the database file.」

### `sqlite3PagerCommitPhaseOne`：提交第一阶段

签名（`src/pager.c`）：`int sqlite3PagerCommitPhaseOne(Pager *pPager, const char *zSuper, int noSync)`。函数头注释把这一步的职责说得很清楚：这一阶段保证「the journal file is synced and the database file contains all the changes」、「the only thing that remains to commit the transaction is to finalize (delete, truncate or zero the first part of) the journal file」。

函数体的分支顺序：

1. **已有错误**：`if( NEVER(pPager->errCode) ) return pPager->errCode;`。
2. **故障注入点**：`if( sqlite3FaultSim(400) ) return SQLITE_IOERR;` —— 源码注释：「Provide the ability to easily simulate an I/O error during testing」。
3. **无改动早退**：`if( pPager->eState<PAGER_WRITER_CACHEMOD ) return SQLITE_OK;` —— 事务没碰过缓存页，直接成功。
4. `if( 0==pagerFlushOnCommit(pPager, 1) )` —— 内存库或没有页被写过，实质是 no-op，只需 `sqlite3BackupRestart(pPager->pBackup)` 重启进行中的备份。
5. **WAL 分支**：`if( pagerUseWal(pPager) )` —— 取 `sqlite3PcacheDirtyList(pPager->pPCache)`，若列表为空则 `sqlite3PagerGet(pPager, 1, &pPageOne, 0)` 取页 1 顶上（源码注释引用了 ticket `[2d1a5c67dfc2363e44f29d9bbd57f]`，说明「Must have at least one page for the WAL commit flag」），然后调 `pagerWalFrames(pPager, pList, pPager->dbSize, 1)` 写 WAL 帧。本页不展开 WAL 细节。
6. **回滚日志分支**（本页主线）：
   - **变更计数器**：`pager_incr_changecounter(pPager, 0)` —— 间接模式：先 `sqlite3PagerWrite` 页 1 再改页 1 内容，等事务提交时才随脏页落盘。若编译期打开了 `SQLITE_ENABLE_ATOMIC_WRITE` 且满足运行期条件（`!zSuper && isOpen(pPager->jfd) && pPager->journalOff==jrnlBufferSize(pPager) && pPager->dbSize>=pPager->dbOrigSize` 且脏页列表首项的 `pDirty` 为 **0**），则改走 `pager_incr_changecounter(pPager, 1)` 的直接模式，直接 `sqlite3OsWrite` 页 1。源码注释说明直接模式的安全前提：「Because of the atomic-write property of the host file-system, this is safe.」
   - **写主日志名**：`writeSuperJournal(pPager, zSuper)`。
   - **同步日志**：`syncJournal(pPager, 0)`。函数头注释解释了它的顺序要求：「Because the change-counter page was just modified, unless the atomic-update optimization is used it is almost certain that the journal requires a sync here.」
   - **写脏页**：`pList = sqlite3PcacheDirtyList(pPager->pPCache);` 然后 `pager_write_pagelist(pPager, pList)`。
   - **清脏标记**：`sqlite3PcacheCleanAll(pPager->pPCache)`。
   - **补齐文件长度**：若 `pPager->dbSize>pPager->dbFileSize`，调 `pager_truncate(pPager, nNew)`，其中 `Pgno nNew = pPager->dbSize - (pPager->dbSize==PAGER_SJ_PGNO(pPager));`。源码注释说明触发条件：「This can happen if the database image was extended as part of the current transaction and then the last page in the db image moved to the free-list.」
   - **同步主库**：`if( !noSync ){ rc = sqlite3PagerSync(pPager, zSuper); }`。
7. 出口 `commit_phase_one_exit:`：若 `rc==SQLITE_OK && !pagerUseWal(pPager)`，置 `pPager->eState = PAGER_WRITER_FINISHED`。

#### `syncJournal`：把 `nRec` 落定并同步日志

`syncJournal`（`src/pager.c`，静态）是「日志同步」这一步的实现。它先调 `sqlite3PagerExclusiveLock(pPager)` 把锁升到 `EXCLUSIVE_LOCK`。

若 `!pPager->noSync` 且日志已打开且 `pPager->journalMode!=PAGER_JOURNALMODE_MEMORY`，则：

- **防「上一个事务残留的日志头」**：`iNextHdrOffset = journalHdrOffset(pPager);` 读 8 字节，若与 `aJournalMagic` 相同则写一个 `0x00` 字节覆盖它。源码注释详细解释了这个「obscure problem」：上一个连接若以 persistent-journal 模式运行，日志文件可能比 `Pager.journalOff` 更大，后面残留的旧日志头会让恢复时的 hot-journal 回滚「roll back all of this connections data, then proceed to rolling back the old, out-of-date data that follows it. Database corruption.」
- **写 `nRec`**：若 `pPager->fullSync && 0==(iDc&SQLITE_IOCAP_SEQUENTIAL)`，先 `sqlite3OsSync(pPager->jfd, pPager->syncFlags)` 再写头。源码注释说明原因：「This ensures that all data has really hit the disk before nRec is updated to mark it as a candidate for rollback.」若设备支持 `SQLITE_IOCAP_SAFE_APPEND`，则不需要这一步——`writeJournalHdr` 已经把 `nRec` 写成 `0xffffffff`（源码注释：「the nRec field is populated with 0xFFFFFFFF when the journal header is written and never needs to be updated」）。
- **同步日志**：`if( 0==(iDc&SQLITE_IOCAP_SEQUENTIAL) )` 则 `sqlite3OsSync(pPager->jfd, pPager->syncFlags|(pPager->syncFlags==SQLITE_SYNC_FULL?SQLITE_SYNC_DATAONLY:0))`。
- 最后 `sqlite3PcacheClearSyncFlags(pPager->pPCache)` 清掉所有页的 `PGHDR_NEED_SYNC`，并置 `pPager->eState = PAGER_WRITER_DBMOD`。

#### `pager_write_pagelist`：把脏页写进主库

`pager_write_pagelist`（`src/pager.c`，静态）遍历脏页链表逐页写盘。写盘前有两条跳过规则（源码注释）：「The page number is greater than Pager.dbSize」或「The PGHDR_DONT_WRITE flag is set on the page」。命中跳过规则的页走 `PAGERTRACE(("NOSTORE ..."))` 分支，不写盘。

对要写的页：`i64 offset = (pgno-1)*(i64)pPager->pageSize;` —— 页在文件里的偏移；然后 `sqlite3OsWrite(pPager->fd, pData, pPager->pageSize, offset)`。若页号是 **1**，写之前先调 `pager_write_changecounter(pList)` 更新变更计数器。

#### `writeSuperJournal`：把主日志名写进日志尾部

`writeSuperJournal`（`src/pager.c`，静态）的格式由函数头注释给出：4 字节 `PAGER_SJ_PGNO` + N 字节 UTF-8 主日志文件名 + 4 字节文件名长度 N + 4 字节文件名校验和 + 8 字节 `aJournalMagic`。校验和定义在注释里：「The super-journal page checksum is the sum of the bytes in the super-journal name, where each byte is interpreted as a signed 8-bit integer.」

若 `zSuper` 为 `NULL`、`pPager->journalMode==PAGER_JOURNALMODE_MEMORY` 或日志未打开，函数直接返回 `SQLITE_OK`（无操作）。若 `pPager->fullSync`，先把 `pPager->journalOff` 推到下一个扇区边界 `journalHdrOffset(pPager)`，注释说明：「In case the previous page written to the journal has already been synced.」写完之后 `pPager->journalOff += (nSuper+20)`（20 = 4 + 4 + 4 + 8）。

### `sqlite3PagerCommitPhaseTwo`：提交第二阶段

签名（`src/pager.c`）：`int sqlite3PagerCommitPhaseTwo(Pager *pPager)`。函数头注释给出定位：「This function finalizes the journal file, either by deleting, truncating or partially zeroing it, so that it cannot be used for hot-journal rollback. Once this is done the transaction is irrevocably committed.」

函数体：

1. `if( NEVER(pPager->errCode) ) return pPager->errCode;` —— 已有错误则原样返回，不做任何事。
2. `pPager->iDataVersion++;` —— 数据版本号自增。
3. **持久日志的快速路径**：若 `pPager->eState==PAGER_WRITER_LOCKED && pPager->exclusiveMode && pPager->journalMode==PAGER_JOURNALMODE_PERSIST`，则直接把 `pPager->eState = PAGER_READER` 并返回 `SQLITE_OK`。源码注释解释：这种日志开头只有一个 `nRec` 为 **0** 的日志头，回滚它不会改动数据库文件，所以「there is no need to zero the journal header」。
4. 否则 `rc = pager_end_transaction(pPager, pPager->setSuper, 1);` 然后 `return pager_error(pPager, rc);`。

#### `pager_end_transaction`：日志终结的四条分支

`pager_end_transaction`（`src/pager.c`，静态）是日志终结的实现，函数头注释按 journal-mode 列出了四种终结方式：

| `journalMode` | 终结动作 | 出处 |
|---|---|---|
| `MEMORY` | 直接 `sqlite3OsClose(pPager->jfd)`，内存日志随之消失 | `pager_end_transaction` |
| `TRUNCATE` | `sqlite3OsTruncate(pPager->jfd, 0)`；若 `pPager->fullSync` 再 `sqlite3OsSync(pPager->jfd, pPager->syncFlags)` | `pager_end_transaction` |
| `PERSIST`（或 exclusive 模式下的 DELETE） | `zeroJournalHdr(pPager, hasSuper||pPager->tempFile)`，把日志头清零 | `pager_end_transaction` |
| `DELETE` / `MEMORY`（hot-journal 回放后） | `sqlite3OsClose(pPager->jfd)` 后 `sqlite3OsDelete(pPager->pVfs, pPager->zJournal, pPager->extraSync)` | `pager_end_transaction` |

`TRUNCATE` 分支里的 `sqlite3OsSync` 有明确理由，源码注释引用了 Mozilla Bugzilla 1072773：「Make sure the new file size is written into the inode right away. Otherwise the journal might resurrect following a power loss and cause the last transaction to roll back.」

终结之后：`sqlite3BitvecDestroy(pPager->pInJournal)` 清「已进日志」位图，`pPager->nRec = 0`；若 `rc==SQLITE_OK`，则 `sqlite3PcacheCleanAll` 或 `sqlite3PcacheClearWritable`，再 `sqlite3PcacheTruncate(pPager->pPCache, pPager->dbSize)`；最后置 `pPager->eState = PAGER_READER`，并在非 exclusive 模式下 `pagerUnlockDb(pPager, SHARED_LOCK)` 降锁。

### `sqlite3PagerRollback`：回滚

签名（`src/pager.c`）：`int sqlite3PagerRollback(Pager *pPager)`。函数头注释把回滚拆成两件事：「1) It rolls back the journal file, restoring all database file and in-memory cache pages to the state they were in when the transaction was opened, and 2) It finalizes the journal file, so that it is not used for hot rollback at any point in the future.」并注明「Finalization of the journal file (task 2) is only performed if the rollback is successful.」

函数体的分支：

1. `if( pPager->eState==PAGER_ERROR ) return pPager->errCode;` —— 已在错误态则不尝试回滚。
2. `if( pPager->eState<=PAGER_READER ) return SQLITE_OK;` —— 无写事务时是 no-op。
3. **WAL 分支**：`if( pagerUseWal(pPager) )` —— 调 `sqlite3PagerSavepoint(pPager, SAVEPOINT_ROLLBACK, -1)` 再 `pager_end_transaction(pPager, pPager->setSuper, 0)`。
4. **日志未打开或只加了写锁**：`else if( !isOpen(pPager->jfd) || pPager->eState==PAGER_WRITER_LOCKED )` —— 调 `pager_end_transaction(pPager, 0, 0)`。若 `!MEMDB && eState>PAGER_WRITER_LOCKED`（即 `journal_mode=off` 的情形），把 `pPager->errCode = SQLITE_ABORT`、`pPager->eState = PAGER_ERROR` 并 `setGetterMethod(pPager)` 后返回。源码注释说明原因：「Move the pager to the error state to indicate that the contents of the cache may not be trusted. Any active readers will get SQLITE_ABORT.」
5. **正常回滚**：`else { rc = pager_playback(pPager, 0); }`。
6. 出口 `return pager_error(pPager, rc);` —— 源码注释：「If an error occurs during a ROLLBACK, we can no longer trust the pager cache. So call pager_error() on the way out to make any error persistent.」

函数里的断言限定了可能的返回码集合：`SQLITE_OK` / `SQLITE_FULL` / `SQLITE_CORRUPT` / `SQLITE_NOMEM` / `SQLITE_CANTOPEN` / `(rc&0xFF)==SQLITE_IOERR`。

#### `pager_playback`：从日志读回原像

`pager_playback`（`src/pager.c`，静态）执行顺序：

1. `rc = sqlite3OsFileSize(pPager->jfd, &szJ);` 取日志文件大小。
2. **检查主日志**：`rc = readSuperJournal(pPager->jfd, 1+(i64)pPager->pVfs->mxPathname, &zSuper);` 若读到了主日志名，再用 `sqlite3OsAccess(pVfs, zSuper, SQLITE_ACCESS_EXISTS, &res)` 检查它是否真的存在。若 `rc!=SQLITE_OK || !res`，直接跳到 `end_playback` —— 源码注释：「If a super-journal file name is specified, but the file is not present on disk, then the journal is not hot and does not need to be played back.」
3. **回放循环** `while(1)`：
   - `rc = readJournalHdr(pPager, isHot, szJ, &nRec, &mxPg);` —— 读下一个日志头。返回 `SQLITE_DONE` 表示日志到此为止（`rc` 被改写成 `SQLITE_OK` 后跳到 `end_playback`）；返回其它错误码则同样跳出。
   - **no-sync 模式的记录数**：`if( nRec==0xffffffff )` 则 `nRec = (int)((szJ - JOURNAL_HDR_SZ(pPager))/JOURNAL_PG_SZ(pPager));` —— 源码注释：「If nRec is 0xffffffff, then this journal was created by a process working in no-sync mode.」
   - **未同步的最后一段**：`if( nRec==0 && !isHot && pPager->journalHdr+JOURNAL_HDR_SZ(pPager)==pPager->journalOff )` 则 `nRec = (int)((szJ - pPager->journalOff) / JOURNAL_PG_SZ(pPager));`。源码注释引用了 ticket `#2565`，说明这是「ROLLBACK 时最后一段尚未同步，故按文件大小推算页数」的修正。
   - **截回原始长度**：若 `pPager->journalOff==JOURNAL_HDR_SZ(pPager)`（第一次读头），调 `pager_truncate(pPager, mxPg)` 并置 `pPager->dbSize = mxPg`。
   - **逐页回放**：`rc = pager_playback_one_page(pPager,&pPager->journalOff,0,1,0);`。返回 `SQLITE_DONE` 时置 `pPager->journalOff = szJ` 并 break；返回 `SQLITE_IOERR_SHORT_READ` 时把 `rc` 改成 `SQLITE_OK` 并结束回放（源码注释：「If the journal has been truncated, simply stop reading and processing the journal.」）；其它错误则中止并返回错误。
4. **收尾 `end_playback:`**：
   - `sqlite3PagerSetPagesize(pPager, &savedPageSize, -1)` 恢复页大小。
   - `pPager->changeCountDone = pPager->tempFile;` —— 源码注释说明原因：「If this happens in exclusive mode, then subsequent transactions performed by the connection will not update the change-counter at all. This may lead to cache inconsistency problems for other processes at some point in the future. So, just in case this has happened, clear the changeCountDone flag now.」
   - 若 `rc==SQLITE_OK && (pPager->eState>=PAGER_WRITER_DBMOD || pPager->eState==PAGER_OPEN)`，调 `sqlite3PagerSync(pPager, 0)`。
   - `pager_end_transaction(pPager, zSuper!=0, 0)` 终结日志。
   - 若有主日志且回放成功，调 `pager_delsuper(pPager, zSuper)` 尝试删除主日志。
   - `freeSuperJournal(zSuper);` + `setSectorSize(pPager);` —— 源码注释：「The Pager.sectorSize variable may have been updated while rolling back a journal created by a process with a different sector size value. Reset it to the correct value for this process.」

#### `pager_playback_one_page`：一条记录的回放

`pager_playback_one_page`（`src/pager.c`，静态）读一条页记录并回放：

- `read32bits(jfd, *pOffset, &pgno)` 读页号；`sqlite3OsRead(jfd, (u8*)aData, pPager->pageSize, (*pOffset)+4)` 读页数据；偏移推进 `*pOffset += pPager->pageSize + 4 + isMainJrnl*4` —— 主日志多出 4 字节是校验和，子日志没有。
- **合法性检查**：`if( pgno==0 || pgno==PAGER_SJ_PGNO(pPager) ) return SQLITE_DONE;`；`if( pgno>(Pgno)pPager->dbSize || sqlite3BitvecTest(pDone, pgno) ) return SQLITE_OK;`（跳过）。
- **校验和比对**：主日志且非 savepoint 回滚时，`if( !isSavepnt && pager_cksum(pPager, (u8*)aData)!=cksum ) return SQLITE_DONE;`。源码注释说明目的：「If a power failure occurs while the journal is being written, it could cause invalid data to be written into the journal. We need to detect this invalid data (with high probability) and ignore it.」
- **恢复 `nReserve`**：`if( pgno==1 && pPager->nReserve!=((u8*)aData)[20] )` 则 `pPager->nReserve = ((u8*)aData)[20];`。

#### `readJournalHdr`：读日志头与它的合法性边界

`readJournalHdr`（`src/pager.c`，静态）先调 `journalHdrOffset(pPager)` 把 `journalOff` 对齐到下一个扇区边界，再判断 `pPager->journalOff+JOURNAL_HDR_SZ(pPager) > journalSize` —— 若日志剩余空间装不下一个完整日志头，返回 `SQLITE_DONE`。

读 8 字节魔数，若与 `aJournalMagic` 不符则返回 `SQLITE_DONE`。随后读 `nRec`、`cksumInit`、`dbSize` 三个 32 位字段。

第一次读头（`pPager->journalOff==0`）时还会读页大小与扇区大小字段，并做范围检查：`iPageSize<512 || iSectorSize<32 || iPageSize>SQLITE_MAX_PAGE_SIZE || iSectorSize>MAX_SECTOR_SIZE || ((iPageSize-1)&iPageSize)!=0 || ((iSectorSize-1)&iSectorSize)!=0` —— 源码注释说明判据：「both values need to be a power of two greater than or equal to 512 or 32, and not greater than their respective compile time maximum limits」。任一条不满足就返回 `SQLITE_DONE`，注释解释：「the process that wrote the journal-header must have crashed before the header was synced.」

### 关键调用方（真实调用点）

| 调用方 | 文件 | 调用方式与语义 |
|---|---|---|
| `sqlite3BtreeCommitPhaseOne` | `src/btree.c` | `rc = sqlite3PagerCommitPhaseOne(pBt->pPager, zSuperJrnl, 0);` —— `noSync` 传 **0**（不省略主库 `xSync`）；`zSuperJrnl` 是多文件事务的主日志名，单文件时为 **0** |
| `sqlite3BtreeCommitPhaseTwo` | `src/btree.c` | `rc = sqlite3PagerCommitPhaseTwo(pBt->pPager);` —— 无参数 |
| `sqlite3BtreeRollback` | `src/btree.c` | `rc2 = sqlite3PagerRollback(pBt->pPager);` —— 只在 `p->inTrans==TRANS_WRITE` 分支里调用 |
| `backupStep` | `src/backup.c` | 目标端 `rc = sqlite3PagerCommitPhaseOne(pDestPager, 0, 1);`（`noSync` 传 **1**）与 `rc = sqlite3PagerCommitPhaseOne(pDestPager, 0, 0);` |
| `backupStep` | `src/backup.c` | `rc = sqlite3PagerWrite(pDestPg);` —— 备份目标端改页前先写原像 |
| `sqlite3BtreeCommit` / B-tree 各类页改写点 | `src/btree.c` | `rc = sqlite3PagerWrite(pPage->pDbPage);` —— B-tree 在插入、删除、平衡、freelist 维护、auto-vacuum 搬页等处改页前统一调它 |
| `sqlite3_dbpage` 虚拟表 | `src/dbpage.c` | `if( (rc = sqlite3PagerWrite(pDbPage))==SQLITE_OK && pData ){` —— 外部直接改页也必须走同一条路径 |
| `pagerUnlockAndRollback` | `src/pager.c` | `sqlite3PagerRollback(pPager);` —— 关闭连接或出错时回滚；另有一条 `journalMode==PAGER_JOURNALMODE_MEMORY` 的特例直接调 `pager_playback(pPager, 1)` |
| `pager_incr_changecounter` | `src/pager.c` | `rc = sqlite3PagerWrite(pPgHdr);` —— 间接模式下靠它把变更计数器所在的页 1 也备份进日志 |
| `pager_write` | `src/pager.c` | `rc = sqlite3PagerWrite(pPg);`（在 `pagerWriteLargeSector` 中为扇区内每页调用） |

### 关键常量与数值出处

| 数值 / 宏 | 含义 | 出处 |
|---|---|---|
| `JOURNAL_PG_SZ(pPager)` = `pageSize + 8` | 一条页记录长度（4 字节页号 + 页数据 + 4 字节校验和） | `src/pager.c`：`#define JOURNAL_PG_SZ(pPager) ((pPager->pageSize) + 8)` |
| `JOURNAL_HDR_SZ(pPager)` = `sectorSize` | 一条日志头占据的字节数 | `src/pager.c`：`#define JOURNAL_HDR_SZ(pPager) (pPager->sectorSize)` |
| `0xd9, 0xd5, 0x05, 0xf9, 0x20, 0xa1, 0x63, 0xd7` | 日志格式魔数 `aJournalMagic[]` | `src/pager.c` 的 `aJournalMagic[]` 定义 |
| `8` | 页记录中页号 + 校验和的字节数 | `pagerAddPageToRollbackJournal` 的 `pPager->journalOff += 8 + pPager->pageSize;` |
| `28` | 日志头中有效字段的字节数（8 + 4×5） | `writeJournalHdr` 的 `memset(&zHeader[sizeof(aJournalMagic)+20], 0, nHeader-(sizeof(aJournalMagic)+20));` 与 `zeroJournalHdr` 的注释「the 28-byte header」 |
| `20` | 主日志名记录中除文件名外的固定字节数（4 + 4 + 4 + 8） | `writeSuperJournal` 的 `pPager->journalOff += (nSuper+20);` |
| `0xffffffff` | no-sync 模式下 `nRec` 的哨兵值 | `writeJournalHdr` 的 `put32bits(&zHeader[sizeof(aJournalMagic)], 0xffffffff);` |
| `200` | 校验和的取样步长（每 200 字节取 1 字节） | `pager_cksum` 的 `for( i=pPager->pageSize-200; i>0; i-=200 )` |
| `512` / `32` | 日志头中页大小 / 扇区大小的下限 | `readJournalHdr` 的 `if( iPageSize<512 || iSectorSize<32 ... )` |
| `512` | `SQLITE_IOCAP_ATOMIC512` 对应的位偏移常量 | `src/pager.c` 的 `assert(SQLITE_IOCAP_ATOMIC512==(512>>8));` |
| `25` | 临时文件 pager 刷脏页的百分比阈值 | `pagerFlushOnCommit` 的 `return (sqlite3PCachePercentDirty(pPager->pPCache)>=25);` |
| `400` | 提交第一阶段的故障注入点编号 | `sqlite3PagerCommitPhaseOne` 的 `if( sqlite3FaultSim(400) ) return SQLITE_IOERR;` |
| `20`（页号） | 数据库页 1 恢复时 `nReserve` 所在的字节偏移 | `pager_playback_one_page` 的 `pPager->nReserve = ((u8*)aData)[20];` |
| `24` | 变更计数器在页 1 中的字节偏移 | `pager_write_changecounter` 的 `put32bits(((char*)pPg->pData)+24, change_counter);` |
| `92` / `96` | 次级变更计数器与 SQLite 版本号在页 1 中的字节偏移 | `pager_write_changecounter` 的 `put32bits(...+92, ...)` 与 `put32bits(...+96, SQLITE_VERSION_NUMBER)` |

## 关键取舍

- **先备份、后置可写标志。** `pager_write` 把 `PGHDR_WRITEABLE` 的置位放在 `pagerAddPageToRollbackJournal` 成功之后，源码注释写明：「Wait until now, after the page has been successfully journalled, before setting the PGHDR_WRITEABLE bit that indicates that the page can be safely modified.」这条顺序保证「页被改」永远蕴含「原像已在日志里」。
- **日志先开、页后标脏。** `pager_write` 在 `sqlite3PcacheMakeDirty` **之前**调 `pager_open_journal`，源码注释解释：「Otherwise, if it were done after calling sqlite3PcacheMakeDirty(), then an error might occur and the pager would end up in WRITER_LOCKED state with pages marked as dirty in the cache.」错误状态下的缓存内容不再可信。
- **写入失败也要置 `PGHDR_NEED_SYNC`。** `pagerAddPageToRollbackJournal` 在写盘之前就置这个标志，注释说明这是为了避免回放时把「本该从日志恢复」的页当成「库里的原像」，进而在 I/O 错误时造成损坏。
- **校验和是抽样求和而非真校验。** `pager_cksum` 只累加每 200 字节中的 1 字节。源码注释解释这是刻意的取舍：「If journal corruption occurs due to a power failure, the most likely scenario is that one end or the other of the record will be changed. It is much less likely that the two ends of the journal record will be correct and the middle be corrupt. Thus, this "checksum" scheme, though fast and simple, catches the mostly likely kind of corruption.」同时它警告改公式会导致日志格式不兼容。
- **校验和种子随机化。** `writeJournalHdr` 用 `sqlite3_randomness(sizeof(pPager->cksumInit), &pPager->cksumInit)` 生成随机种子。源码注释说明动机：「the random initializer is important, because garbage data that appears at the end of a journal is likely data that was once in other files that have now been deleted. If the garbage data came from an obsolete journal file, the checksums might be correct.」
- **nRec 先写 0、同步后再写真值。** `writeJournalHdr` 初始把 `nRec` 写成 **0**（`memset(zHeader, 0, sizeof(aJournalMagic)+4);`），只有 `syncJournal` 在日志同步之后才把它改写成真实记录数。源码注释：「This ensures that all data has really hit the disk before nRec is updated to mark it as a candidate for rollback.」代价是多一次 `sqlite3OsWrite`，换来「崩溃后不会把没落盘的页当成可回滚的页」。
- **设备支持 `SQLITE_IOCAP_SAFE_APPEND` 时跳过 nRec 回写。** 这种设备保证垃圾数据不会被追加进日志文件，所以 `nRec` 直接写 `0xffffffff`，回放时按文件大小推算页数，省掉一次写。
- **回滚日志要连带备份整个扇区。** `pagerWriteLargeSector` 在 `sectorSize > pageSize` 时把整个扇区的页都写进日志，理由是硬件按扇区整体写入。代价是日志体积放大，收益是掉电时不会半扇区损坏。
- **提交第一阶段的顺序不可换。** 变更计数器（`pager_incr_changecounter`）→ 主日志名（`writeSuperJournal`）→ 日志同步（`syncJournal`）→ 脏页写回（`pager_write_pagelist`）→ 主库同步（`sqlite3PagerSync`）。把日志同步放在脏页写回之前，是「日志里有原像」这条不变式在崩溃场景下仍然成立的前提。
- **批量原子写（batch-atomic-write）失败时降级回滚日志。** 编译期打开 `SQLITE_ENABLE_BATCH_ATOMIC_WRITE` 时，`sqlite3PagerCommitPhaseOne` 的 `bBatch` 变量由 `zSuper==0 && (sqlite3OsDeviceCharacteristics(fd) & SQLITE_IOCAP_BATCH_ATOMIC) && !pPager->noSync && sqlite3JournalIsInMemory(pPager->jfd)` 四个条件共同决定。若 `sqlite3OsFileControl(fd, SQLITE_FCNTL_BEGIN_ATOMIC_WRITE, 0)` 之后的写入返回 `(rc&0xFF)==SQLITE_IOERR && rc!=SQLITE_IOERR_NOMEM`，则走降级路径：`sqlite3JournalCreate(pPager->jfd)` 真实创建日志文件 → `syncJournal(pPager, 0)` 同步 → `bBatch = 0` → 回到 `pager_write_pagelist` 的常规路径。源码注释：「The journal file was just created on disk. Sync it now before any pages are written to the database file.」
- **提交第二阶段对 persistent-journal 有快速路径。** `sqlite3PagerCommitPhaseTwo` 在「exclusive 模式 + PERSIST + 只加写锁未改页」时直接返回 `SQLITE_OK` 并把状态置为 `PAGER_READER`，因为这种日志的 `nRec` 本来就是 **0**，回滚它不会改动数据库。
- **回滚失败即进入错误态。** `sqlite3PagerRollback` 出口无条件走 `pager_error(pPager, rc)`，源码注释：「If an error occurs during a ROLLBACK, we can no longer trust the pager cache. So call pager_error() on the way out to make any error persistent.」
- **`journal_mode=off` 下回滚等于放弃缓存。** `sqlite3PagerRollback` 在「日志未打开且状态已越过写锁」时把 `pPager->errCode` 置为 `SQLITE_ABORT`、状态置为 `PAGER_ERROR`，源码注释：「Move the pager to the error state to indicate that the contents of the cache may not be trusted. Any active readers will get SQLITE_ABORT.」
- **回滚后必须重新确认扇区大小。** `pager_playback` 结尾调 `setSectorSize(pPager)`，源码注释：「The Pager.sectorSize variable may have been updated while rolling back a journal created by a process with a different sector size value.」
- **热日志回放与普通回放对 `nRec==0` 的处理不同。** `pager_playback` 里 `if( nRec==0 && !isHot && pPager->journalHdr+JOURNAL_HDR_SZ(pPager)==pPager->journalOff )` 才按文件大小推算页数。源码注释引用 ticket `#2565` 说明：热日志回放时 `nRec==0` 一律表示「下一段零页要回滚」，而普通 ROLLBACK 时最后一段可能尚未同步。

## 与主核 / AI 的关系

回滚日志层是 SQLite「逻辑事务」与「物理原子性」之间的最后一环：上层的 `BEGIN` / `COMMIT` / `ROLLBACK` 语义最终都落到 `sqlite3PagerWrite` / `sqlite3PagerCommitPhaseOne` / `sqlite3PagerCommitPhaseTwo` / `sqlite3PagerRollback` 这四个函数上。

**架构推论：** 这四个函数把「提交」切成了**两段式**，而且两段之间的分界线不是「数据是否落盘」，而是「日志是否还能用于回滚」。这说明原子性边界被放在**日志终结**这个动作上，而不是放在 `fsync` 上。主核 `engine/db/` 若实现原子提交，把边界放在「写完数据并 fsync」上是不够的——崩溃后仍需要一个可识别的「已提交」标记，日志文件的终结本身就是这个标记。

**架构推论：** `sqlite3PagerWrite` 把「备份原像」与「标记页可写」合并成一个不可分割的 API（调用方必须先调它、且在它返回 `SQLITE_OK` 之前不得改页），说明**存储层的原子性义务必须由存储层自己封装**，而不是指望每个调用点自觉。主核的分页器若暴露「给我一个可写页」的裸接口，每个上层模块都要各自记得备份，漏一处就是损坏。

**架构推论：** `pager_cksum` 用抽样求和替代真校验，是一种**基于损坏概率分布**的工程取舍：它假设「记录首尾被破坏」比「中间被破坏」更常见。主核的 WAL 或回滚日志若采用更强校验（如 CRC32），性能代价更高但覆盖更全；选用哪种取决于主核对「单页写 + 掉电」这一失效模型的假设是否与 SQLite 相同。

**架构推论：** `sqlite3PagerCommitPhaseOne` 里的批量原子写降级路径（`SQLITE_FCNTL_BEGIN_ATOMIC_WRITE` 失败则回退到真实日志文件并重新同步），展示了**新机制必须自带旧机制兜底**的工程模式。主核若引入新的存储提交路径，同样需要为「文件系统不支持该能力」准备一条完整的回退链路，而不只是返回错误。

**架构推论：** `pager_playback` 里对 `nRec` 的多种取值（**0**、**0xffffffff**、真实值）分别对应「未同步的最后一段」「no-sync 模式」「正常情况」，回放端必须能同时兼容这三种。这是一个**日志格式向前兼容**的约束：新增的日志语义不能改变既有字段的取值范围，否则旧版本进程无法回放新版本写的日志。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/pager.c | `sqlite3PagerWrite` | 把页标记为可写的对外入口：按「已可写 / 已有错误 / 扇区大于页 / 常规」四路分派，返回非 `SQLITE_OK` 时调用方不得改页 |
| src/pager.c | `sqlite3PagerCommitPhaseOne` | 提交第一阶段：变更计数器 → 写主日志名 → `syncJournal` 同步日志 → `pager_write_pagelist` 写脏页 → 补齐文件长度 → `sqlite3PagerSync` 同步主库；成功后退到 `PAGER_WRITER_FINISHED` |
| src/pager.c | `sqlite3PagerCommitPhaseTwo` | 提交第二阶段：`pager_end_transaction` 终结日志（删除 / 截断 / 清零头部），并对 exclusive + PERSIST 走免终结快速路径 |
| src/pager.c | `sqlite3PagerRollback` | 回滚：按 WAL / 日志未开 / 正常回放三路分派；失败时经 `pager_error` 把错误固化为错误态 |
| src/pager.c | `pager_write` | 单页写入的真正实现：先 `pager_open_journal` 再标脏，页未进日志则调 `pagerAddPageToRollbackJournal`，最后才置 `PGHDR_WRITEABLE` |
| src/pager.c | `pagerAddPageToRollbackJournal` | 写一条页记录：4 字节页号 + 页数据 + 4 字节 `pager_cksum` 校验和，并更新 `journalOff`、`nRec` 与已进日志位图 |
| src/pager.c | `pagerWriteLargeSector` | 扇区大于页时的连带备份：按 `sectorSize/pageSize` 算出扇区首页，把整扇区页写进日志并统一置 `PGHDR_NEED_SYNC` |
| src/pager.c | `pager_open_journal` | 打开日志文件：建 `pInJournal` 位图、开日志句柄、`writeJournalHdr` 写首个日志头，状态从 `PAGER_WRITER_LOCKED` 推进到 `PAGER_WRITER_CACHEMOD` |
| src/pager.c | `writeJournalHdr` | 写日志头：魔数、`nRec`、`cksumInit`、`dbOrigSize`、扇区大小、页大小；no-sync 或 `SQLITE_IOCAP_SAFE_APPEND` 时把 `nRec` 写成 `0xffffffff` |
| src/pager.c | `readJournalHdr` | 读日志头：校验魔数、读 `nRec` / `cksumInit` / `dbSize`，并检查页大小与扇区大小是否为合法 2 的幂 |
| src/pager.c | `syncJournal` | 同步日志：升级到 `EXCLUSIVE_LOCK`、抹掉上一个事务残留的日志头、full-sync 时先同步再写 `nRec`、最后同步整个日志并清 `PGHDR_NEED_SYNC` |
| src/pager.c | `pager_write_pagelist` | 把脏页链表逐页写进数据库文件：跳过页号大于 `dbSize` 或带 `PGHDR_DONT_WRITE` 的页；页 1 写前更新变更计数器 |
| src/pager.c | `pager_playback` | 回放日志：读主日志名确认是否 hot、循环读日志头与逐页回放、按 `nRec` 的三种取值推算页数、回放后同步并终结日志 |
| src/pager.c | `pager_playback_one_page` | 回放单条页记录：读页号与页数据、比对校验和、恢复页 1 的 `nReserve`、写回数据库文件与页缓存 |
| src/pager.c | `pager_end_transaction` | 事务收尾：按 journal-mode 分四种方式终结日志（关内存日志 / 截断 / 清零头 / 删除），清位图、降锁、把状态置为 `PAGER_READER` |
| src/pager.c | `writeSuperJournal` | 把主日志名写进日志尾部：页号 `PAGER_SJ_PGNO` + 文件名 + 长度 + 校验和 + 魔数；full-sync 时先对齐到扇区边界 |
| src/pager.c | `readSuperJournal` | 从日志尾部读出主日志名并校验长度、校验和与魔数；任一项不过即视为无主日志 |
| src/pager.c | `pager_cksum` | 页记录校验和：`cksumInit` 加上页数据中每第 200 字节的累加和；改公式即破坏日志格式兼容 |
| src/pager.c | `pager_incr_changecounter` | 更新变更计数器：间接模式调 `sqlite3PagerWrite` 改页 1；直接模式（需 `SQLITE_ENABLE_ATOMIC_WRITE`）直接 `sqlite3OsWrite` 页 1 |
| src/pager.c | `pager_truncate` | 把数据库文件长度调整到指定页数，用于回放时截回原始长度与提交时补齐文件长度 |
| src/pager.c | `pagerUnlockAndRollback` | 关闭或出错时的回滚入口：调 `sqlite3PagerRollback`；内存日志的 I/O 错误特例直接调 `pager_playback(pPager, 1)` |
| src/pager.c | `hasHotJournal` | 热日志判定：日志存在、无其它连接持 `RESERVED_LOCK`、且日志首字节非零，才算 hot |
| src/pager.c | `JOURNAL_PG_SZ` / `JOURNAL_HDR_SZ` / `aJournalMagic` | 日志格式的三个固定量：页记录长度、日志头长度、8 字节格式魔数 |
| src/pager.c | `sqlite3PagerSync` | 同步数据库文件：`SQLITE_FCNTL_SYNC` 文件控制 + `sqlite3OsSync(pPager->fd, pPager->syncFlags)` |
| src/pager.c | `pagerFlushOnCommit` | 判断提交时是否需要刷盘：非临时文件恒为真；临时文件按脏页百分比阈值 **25** 判定 |
| src/btree.c | `sqlite3BtreeCommitPhaseOne` / `sqlite3BtreeCommitPhaseTwo` / `sqlite3BtreeRollback` | B-tree 层对 pager 提交与回滚三阶段的调用方 |
| src/backup.c | `backupStep` | 备份目标端的提交调用方，分别以 `noSync` 为 **1** 和 **0** 调 `sqlite3PagerCommitPhaseOne` |
| src/dbpage.c | `sqlite3_dbpage` 虚拟表 | 直接改页的外部调用方，改页前调 `sqlite3PagerWrite` |
| src/main.c | `sqlite3RollbackAll` | 连接级回滚的编排者：遍历 `db->aDb` 逐个 `sqlite3BtreeRollback` |

## 参考

- [SQLite Atomic Commit In SQLite（sqlite.org 官方文档）](https://sqlite.org/atomiccommit.html)
- [SQLite Write-Ahead Logging（sqlite.org 官方文档）](https://sqlite.org/wal.html）
- [SQLite PRAGMA journal_mode（sqlite.org 官方文档）](https://sqlite.org/pragma.html#pragma_journal_mode)
- [SQLite Temp File Storage（sqlite.org 官方文档）](https://sqlite.org/tempfiles.html)
- [SQLite 源码浏览：src/pager.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/pager.c)
- [sqlite/sqlite 仓库中的 src/pager.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/pager.c)
