---
project: sqlite
feature: btree-transaction
page_type: feature
schema_version: 1
refs:
  - path: src/btree.c
    symbols: [sqlite3BtreeBeginTrans, sqlite3BtreeCommit, sqlite3BtreeCommitPhaseOne, sqlite3BtreeCommitPhaseTwo, sqlite3BtreeRollback]
---

# B-tree 事务与提交

## 一句话职责

`sqlite3BtreeBeginTrans` 负责在 B-tree 层开启读事务或写事务，`sqlite3BtreeCommitPhaseOne` / `sqlite3BtreeCommitPhaseTwo` 负责把写事务按「两阶段提交」拆开落地（第一阶段把数据刷盘，第二阶段把日志清零/删除以宣告提交），`sqlite3BtreeCommit` 是把这两阶段串起来的一次性入口，`sqlite3BtreeRollback` 负责撤销当前事务并把游标从被回滚的数据上「拉下来」（trip）。

## 机制是什么

B-tree 层的事务状态机由**两级状态**共同描述：

- **句柄级 `Btree.inTrans`**：取值来自 `src/btreeInt.h` 的三个宏 —— `TRANS_NONE`（**0**）、`TRANS_READ`（**1**）、`TRANS_WRITE`（**2**）。
- **共享级 `BtShared.inTransaction` 与 `BtShared.nTransaction`**：`inTransaction` 记录共享 B-tree 上当前最高的事务级别，`nTransaction` 记录当前打开的事务个数（读 + 写）。

B-tree 层的事务职责可以拆成四件事，边界由「谁负责把数据真正写进文件」划出：

1. **开事务（`sqlite3BtreeBeginTrans`）**：拿锁、读页 1、进入读/写事务状态。这是唯一会调 `sqlite3PagerBegin` 的地方。
2. **提交第一阶段（`sqlite3BtreeCommitPhaseOne`）**：在 `p->inTrans==TRANS_WRITE` 时才做事，做两件额外的事（auto-vacuum 收尾 + 截断镜像），然后调 `sqlite3PagerCommitPhaseOne` 把脏页和日志刷到磁盘。
3. **提交第二阶段（`sqlite3BtreeCommitPhaseTwo`）**：调 `sqlite3PagerCommitPhaseTwo`，把共享事务状态从 `TRANS_WRITE` 降回 `TRANS_READ`，然后统一收尾（`btreeEndTransaction`）。
4. **回滚（`sqlite3BtreeRollback`）**：调 `sqlite3PagerRollback` 让 pager 层恢复数据库文件与缓存页，并把共享事务状态降回 `TRANS_READ`。

**本机制的边界：** 不负责分页器层面的日志与提交（`sqlite3PagerCommitPhaseOne` / `sqlite3PagerCommitPhaseTwo` / `sqlite3PagerRollback`，G 组）、不负责 WAL 的帧写入（H 组）。B-tree 层在这里只是**事务状态的持有者与调度者**，真正的落盘原子性由 pager 层与 WAL 实现提供。

**一个容易混淆的边界：** 「语句级子事务」（statement subtransaction）在 B-tree 层不是独立机制，而是**匿名 savepoint**。`sqlite3BtreeBeginStmt`（`src/btree.c`）的函数头注释明确写道：「A statement sub-transaction is implemented as an anonymous savepoint」，它转调 `sqlite3PagerOpenSavepoint(pBt->pPager, iStatement)`，索引 `iStatement` 大于所有显式 SAVEPOINT 的索引。`sqlite3BtreeSavepoint` 则用 `op` 参数区分 `SAVEPOINT_RELEASE`（释放）与 `SAVEPOINT_ROLLBACK`（回滚），并支持 `iSavepoint==-1` 时的「整事务内容回滚但不释放锁、事务保持打开」语义（源码注释：「this is different from a normal transaction rollback, as no locks are released and the transaction remains open」）。

## 怎么实现的

### `sqlite3BtreeBeginTrans`：开启事务

签名（`src/btree.c`）：`int sqlite3BtreeBeginTrans(Btree *p, int wrflag, int *pSchemaVersion)`。`wrflag` 的语义由函数头注释给出：非零开写事务，为 **0** 开读事务；**2** 或更大则开**独占**事务（exclusive），且注释明确说明「A preexisting transaction may not be upgraded to exclusive by calling this routine a second time」——独占标志只对新事务有效。

它本身是一个**分派器**，真正的实现是静态函数 `btreeBeginTrans`（`src/btree.c`，标记为 `SQLITE_NOINLINE`）。分派条件：

```
if( p->sharable
 || p->inTrans==TRANS_NONE
 || (p->inTrans==TRANS_READ && wrflag!=0)
){
  return btreeBeginTrans(p,wrflag,pSchemaVersion);
}
```

即：句柄是**可共享的**（`p->sharable`）、**当前无事务**、或**当前是读事务但本次要求写事务**（升级）时，才真正走 `btreeBeginTrans`。否则（已是写事务，或已是读事务且本次仍只要读事务）走**快速路径**：

- `*pSchemaVersion = get4byte(&pBt->pPage1->aData[40])` —— 从数据库头偏移 **40** 处读 4 字节的 schema 版本号。
- 若 `wrflag` 非零，调 `sqlite3PagerOpenSavepoint(pBt->pPager, p->db->nSavepoint)`，注释说明这是「makes sure that the pager has the correct number of open savepoints」；否则返回 `SQLITE_OK`。

**`btreeBeginTrans` 的执行顺序：**

1. `sqlite3BtreeEnter(p)` + `btreeIntegrity(p)`。`btreeIntegrity` 是 `src/btreeInt.h` 里的断言宏，展开后是两条断言：`p->pBt->inTransaction!=TRANS_NONE || p->pBt->nTransaction==0` 与 `p->pBt->inTransaction>=p->inTrans`。
2. **已是写事务，或已是读事务且本次只要读 → 直接 `goto trans_begun`**（源码注释："this is a no-op"）。
3. 若 `db->flags` 带 `SQLITE_ResetDatabase` 且 pager 非只读，则清掉 `BTS_READ_ONLY` 标志。
4. **只读库不能开写事务**：`if( (pBt->btsFlags & BTS_READ_ONLY)!=0 && wrflag ){ rc = SQLITE_READONLY; goto trans_begun; }`。
5. **共享缓存下的写冲突**：若 `wrflag && pBt->inTransaction==TRANS_WRITE`（别的句柄已开写事务）或 `pBt->btsFlags & BTS_PENDING`（正在等读锁散开），则 `pBlock = pBt->pWriter->db`；若 `wrflag>1`（要开独占事务），还要遍历 `pBt->pLock` 链找别的 `Btree`。命中则 `sqlite3ConnectionBlocked(p->db, pBlock)` + `rc = SQLITE_LOCKED_SHAREDCACHE`。
6. **取 schema 根页的读锁**：`rc = querySharedCacheTableLock(p, SCHEMA_ROOT, READ_LOCK)`。源码注释说明理由：「Any read-only or read-write transaction implies a read-lock on page 1」。
7. 清 `BTS_INITIALLY_EMPTY`；若 `pBt->nPage==0` 则重新置上 `BTS_INITIALLY_EMPTY`。
8. 进入 `do { ... } while(...)` 循环：
   - `sqlite3PagerWalDb(pPager, p->db)` 告诉 WAL 层本连接进入 WAL 模式。
   - `while( pBt->pPage1==0 && SQLITE_OK==(rc = lockBtree(pBt)) );` —— 反复调 `lockBtree` 直到页 1 取到或出错。源码注释说明 `lockBtree` 可能返回 `SQLITE_OK` 但 `pBt->pPage1` 仍为 0（此时文件里的页大小与 `pBt->pageSize` 不一致，`lockBtree` 会把 `pBt->pageSize` 更新成文件里的实际值）。
   - 若是写事务：再查一次 `BTS_READ_ONLY`（命中则 `rc = SQLITE_READONLY`），否则 `rc = sqlite3PagerBegin(pPager, wrflag>1, sqlite3TempInMemory(p->db))`；成功则 `rc = newDatabase(pBt)`（把空文件初始化成一个空库，见 `newDatabase`：`memcpy(data, zMagicHeader, sizeof(zMagicHeader))`，`sizeof(zMagicHeader)==16`）；若返回 `SQLITE_BUSY_SNAPSHOT` 且 `pBt->inTransaction==TRANS_NONE`，则把错误码改写成 `SQLITE_BUSY`（源码注释：「if there was no transaction opened when this function was called and SQLITE_BUSY_SNAPSHOT is returned, change the error code to SQLITE_BUSY」）。
   - 若 `rc!=SQLITE_OK`：调 `(void)sqlite3PagerWalWriteLock(pPager, 0)` 并 `unlockBtreeIfUnused(pBt)`。
   - 循环条件：`(rc&0xFF)==SQLITE_BUSY && pBt->inTransaction==TRANS_NONE && btreeInvokeBusyHandler(pBt)` —— 即遇到 `SQLITE_BUSY` 且尚未开事务时，反复调 busy handler 重试。`btreeInvokeBusyHandler` 转调 `sqlite3InvokeBusyHandler(&pBt->db->busyHandler)`。
   - 循环结束后 `sqlite3PagerWalDb(pPager, 0)` 撤销第 8 步开头的那次声明。
9. 成功后的状态更新：
   - 若 `p->inTrans==TRANS_NONE` 则 `pBt->nTransaction++`；`p->sharable` 时把 `&p->lock`（`iTable==1`、`eLock=READ_LOCK`）挂到 `pBt->pLock` 链表头。
   - `p->inTrans = (wrflag?TRANS_WRITE:TRANS_READ)`；若 `p->inTrans>pBt->inTransaction` 则抬高共享状态。
   - 若是写事务：`pBt->pWriter = p`、清 `BTS_EXCLUSIVE`；`wrflag>1` 时置 `BTS_EXCLUSIVE`。此外校正页 1 里的库大小字段：若 `pBt->nPage!=get4byte(&pPage1->aData[28])`，则 `sqlite3PagerWrite(pPage1->pDbPage)` 后 `put4byte(&pPage1->aData[28], pBt->nPage)`。源码注释说明原因：「Doing this sooner rather than later means the database size can safely re-read the database size from page 1 if a savepoint or transaction rollback occurs within the transaction」。
10. `trans_begun:` 标签处：若 `rc==SQLITE_OK` 且 `pSchemaVersion` 非空，则 `*pSchemaVersion = get4byte(&pBt->pPage1->aData[40])`；若 `wrflag` 非零则 `rc = sqlite3PagerOpenSavepoint(pPager, p->db->nSavepoint)`。
11. 收尾 `btreeIntegrity(p)` + `sqlite3BtreeLeave(p)`，返回 `rc`。

### `sqlite3BtreeCommitPhaseOne`：提交第一阶段

签名（`src/btree.c`）：`int sqlite3BtreeCommitPhaseOne(Btree *p, const char *zSuperJrnl)`。函数头注释写得很直白：「When this is called, the super-journal should already have been created, populated with this journal pointer and synced to disk. Once this is routine has returned, the only thing required to commit the write-transaction for this database file is to delete the journal.」

执行顺序：

1. `if( p->inTrans==TRANS_WRITE )` —— **不是写事务就直接跳过**，函数返回 `SQLITE_OK`（`rc` 初值即 `SQLITE_OK`）。
2. `sqlite3BtreeEnter(p)`。
3. **auto-vacuum 收尾**：在 `#ifndef SQLITE_OMIT_AUTOVACUUM` 内，若 `pBt->autoVacuum` 则 `rc = autoVacuumCommit(p)`；失败则 `sqlite3BtreeLeave(p)` 并直接返回该错误码。
4. **截断镜像**：`if( pBt->bDoTruncate ) sqlite3PagerTruncateImage(pBt->pPager, pBt->nPage);`。
5. `rc = sqlite3PagerCommitPhaseOne(pBt->pPager, zSuperJrnl, 0);` —— 第三个参数 `noSync` 传 **0**，即**不省略**对数据库文件的 `xSync`。这条调用把「写脏页 + 刷日志」下沉给 pager 层。
6. `sqlite3BtreeLeave(p)`，返回 `rc`。

### `autoVacuumCommit`：提交前的 auto-vacuum 收尾

`autoVacuumCommit`（`src/btree.c`，静态）只在 `pBt->autoVacuum` 为真且 `!pBt->incrVacuum`（**非增量** auto-vacuum）时才做事。执行顺序：

- `invalidateAllOverflowCache(pBt)`。
- 读 `nOrig = btreePagecount(pBt)`（提交前的库大小）；若 `PTRMAP_ISPAGE(pBt, nOrig) || nOrig==PENDING_BYTE_PAGE(pBt)` 则返回 `SQLITE_CORRUPT_BKPT`（源码注释：「it is not possible to create a database for which the final page is either a pointer-map page or the pending-byte page. If one is encountered, this indicates corruption.」）。
- `nFree = get4byte(&pBt->pPage1->aData[36])` —— 页 1 偏移 **36** 处的空闲页计数。
- 若连接注册了 `db->xAutovacPages` 回调，则用它算出要回收的页数 `nVac`，并把 `nVac` 截断到不超过 `nFree`；`nVac==0` 时直接返回 `SQLITE_OK`。否则 `nVac = nFree`（**全量回收**）。
- `nFin = finalDbSize(pBt, nOrig, nVac)`；`nFin>nOrig` 返回 `SQLITE_CORRUPT_BKPT`；`nFin<nOrig` 则先 `saveAllCursors(pBt, 0, 0)` 保存游标位置。
- 循环 `for(iFree=nOrig; iFree>nFin && rc==SQLITE_OK; iFree--) rc = incrVacuumStep(pBt, nFin, iFree, nVac==nFree);` —— 从尾部往回收。
- 成功后写页 1：若 `nVac==nFree`（全量回收）则 `put4byte(&pPage1->aData[32], 0)`（空闲页链表头）与 `put4byte(&pPage1->aData[36], 0)`（空闲页总数）清零；再 `put4byte(&pPage1->aData[28], nFin)`（页 1 偏移 **28** 是库大小字段）。随后 `pBt->bDoTruncate = 1`（供提交第一阶段调用 `sqlite3PagerTruncateImage`）与 `pBt->nPage = nFin`。
- 失败时 `sqlite3PagerRollback(pPager)`。

### `sqlite3BtreeCommitPhaseTwo`：提交第二阶段

签名（`src/btree.c`）：`int sqlite3BtreeCommitPhaseTwo(Btree *p, int bCleanup)`。函数头注释说明它的定位：「This routine implements the second phase of a 2-phase commit. The sqlite3BtreeCommitPhaseOne() routine does the first phase ... All this routine has to do is delete or truncate or zero the header in the rollback journal (which causes the transaction to commit) and drop locks.」

执行顺序：

1. `if( p->inTrans==TRANS_NONE ) return SQLITE_OK;` —— 无事务直接返回。
2. `sqlite3BtreeEnter(p)` + `btreeIntegrity(p)`。
3. 若 `p->inTrans==TRANS_WRITE`：
   - 断言 `pBt->inTransaction==TRANS_WRITE` 且 `pBt->nTransaction>0`。
   - `rc = sqlite3PagerCommitPhaseTwo(pBt->pPager);`
   - **错误处理取决于 `bCleanup`**：`if( rc!=SQLITE_OK && bCleanup==0 ){ sqlite3BtreeLeave(p); return rc; }`。源码注释解释：`bCleanup` 非零表示本事务是**多文件事务**的一部分，此时事务其实已经通过删除 super-journal 提交了，调用方会忽略返回码；即便 pager 层出错，也要把 B-tree 的内部状态重置成「写事务已关闭」，因为 pager 已经转入错误状态。
   - `p->iBDataVersion--;` —— 源码注释：「Compensate for pPager->iDataVersion++;」（pager 在 `sqlite3PagerCommitPhaseTwo` 里做了 `pPager->iDataVersion++`）。
   - `pBt->inTransaction = TRANS_READ;`（写降为读）。
   - `btreeClearHasContent(pBt);` —— 销毁 `BtShared.pHasContent` 位图（该位图记录「本事务内被移到空闲链表上的页」，函数注释说它「should be invoked at the conclusion of each write-transaction」）。
4. `btreeEndTransaction(p);` —— 统一收尾。
5. `sqlite3BtreeLeave(p);` 返回 `SQLITE_OK`。

### `btreeEndTransaction`：提交与回滚共用的收尾

`btreeEndTransaction`（`src/btree.c`，静态）由 `sqlite3BtreeCommitPhaseTwo` 与 `sqlite3BtreeRollback` 共同调用，源码注释即写明这一点。

- 先清 `pBt->bDoTruncate = 0`（在 `#ifndef SQLITE_OMIT_AUTOVACUUM` 内）。
- **若 `p->inTrans>TRANS_NONE && db->nVdbeRead>1`**（同一连接上还有其它活跃语句在读）：调 `downgradeAllSharedCacheTableLocks(p)` 并把 `p->inTrans = TRANS_READ` —— 降级为只读事务，让其它语句还能继续读。源码注释：「The other statements may still be reading from the database.」
- 否则：若 `p->inTrans!=TRANS_NONE`，则 `clearAllSharedCacheTableLocks(p)`、`pBt->nTransaction--`；若 `0==pBt->nTransaction` 则 `pBt->inTransaction = TRANS_NONE`。然后 `p->inTrans = TRANS_NONE;` 并 `unlockBtreeIfUnused(pBt)`（`unlockBtreeIfUnused` 在 `pBt->inTransaction==TRANS_NONE && pBt->pPage1!=0` 时释放页 1，从而释放读锁）。
- 最后 `btreeIntegrity(p)`。

### `sqlite3BtreeCommit`：一次性提交

签名（`src/btree.c`）：`int sqlite3BtreeCommit(Btree *p)`。实现极短：`sqlite3BtreeEnter(p)` → `rc = sqlite3BtreeCommitPhaseOne(p, 0)` → 若 `rc==SQLITE_OK` 则 `rc = sqlite3BtreeCommitPhaseTwo(p, 0)` → `sqlite3BtreeLeave(p)` → 返回 `rc`。**注意第二个参数 `zSuperJrnl` 传 `0`**（无 super-journal），且 `bCleanup` 传 **0**。

### `sqlite3BtreeRollback`：回滚

签名（`src/btree.c`）：`int sqlite3BtreeRollback(Btree *p, int tripCode, int writeOnly)`。断言 `tripCode` 只能是 `SQLITE_ABORT_ROLLBACK` 或 `SQLITE_OK`。

执行顺序：

1. `sqlite3BtreeEnter(p)`。
2. **游标处理**：
   - 若 `tripCode==SQLITE_OK`：先 `rc = tripCode = saveAllCursors(pBt, 0, 0);`（保存所有游标位置），失败则 `writeOnly = 0`（降级为「全部游标都要拉下来」）。
   - 否则 `rc = SQLITE_OK`。
   - 若 `tripCode` 非零：`rc2 = sqlite3BtreeTripAllCursors(p, tripCode, writeOnly);`，若 `rc2!=SQLITE_OK` 则 `rc = rc2`。
3. `btreeIntegrity(p)`。
4. **若 `p->inTrans==TRANS_WRITE`**：
   - `rc2 = sqlite3PagerRollback(pBt->pPager);`，失败则 `rc = rc2`。
   - **回滚可能销毁了页 1 的内容**，所以重新取一次页 1：`if( btreeGetPage(pBt, 1, &pPage1, 0)==SQLITE_OK ){ btreeSetNPage(pBt, pPage1); releasePageOne(pPage1); }`。`btreeSetNPage` 从 `get4byte(&pPage1->aData[28])` 读库大小；若读出为 **0** 则调 `sqlite3PagerPagecount(pBt->pPager, &nPage)` 取实际文件页数。
   - 断言 `countValidCursors(pBt, 1)==0`。
   - `pBt->inTransaction = TRANS_READ;` 与 `btreeClearHasContent(pBt);`。
5. `btreeEndTransaction(p);` → `sqlite3BtreeLeave(p);` → 返回 `rc`。

### `sqlite3BtreeTripAllCursors`：游标「拉下来」

`sqlite3BtreeTripAllCursors`（`src/btree.c`）遍历 `pBtree->pBt->pCursor` 链表：

- 若 `writeOnly` 且游标**不是**写游标（`(p->curFlags & BTCF_WriteFlag)==0`）：若游标状态是 `CURSOR_VALID` 或 `CURSOR_SKIPNEXT`，则 `saveCursorPosition(p)` 保存位置；失败则递归调 `sqlite3BtreeTripAllCursors(pBtree, rc, 0)` 把**所有**游标都拉下来，然后 break。
- 否则：`sqlite3BtreeClearCursor(p)`、`p->eState = CURSOR_FAULT`、`p->skipNext = errCode`。
- 每个游标最后都 `btreeReleaseAllCursorPages(p)`。

`writeOnly` 的语义由函数头注释给出：回滚的事务若**改动了数据库 schema**（B-tree 根页可能被移动或删除），则读游标继续运行不安全，此时 `writeOnly` 为假、所有游标都被拉倒；否则只拉倒写游标，读游标保存位置后可继续跟随回滚。

### 关键调用方（真实调用点）

| 调用方 | 文件 | 调用方式与语义 |
|---|---|---|
| `sqlite3VdbeHalt`（VDBE 指令循环终止） | `src/vdbe.c` | `rc = sqlite3BtreeBeginTrans(pBt, pOp->p2, &iMeta);` —— `pOp->p2` 就是 `wrflag`，`iMeta` 取回 schema 版本 |
| `sqlite3VdbeHalt` | `src/vdbe.c` | `rc = sqlite3BtreeBeginTrans(pCx->ub.pBtx, 1, 0);` —— 写事务 |
| `sqlite3VdbeCommit` / `vdbeCommit`（多文件事务） | `src/vdbeaux.c` | 对 `db->nDb` 里的每个 `pBt` 依次 `rc = sqlite3BtreeCommitPhaseOne(pBt, zSuper);`（传入 super-journal 名）；删除 super-journal 后再对每个 `pBt` 调 `sqlite3BtreeCommitPhaseTwo(pBt, 1);` —— `bCleanup` 传 **1**，源码注释说明此处出错可以忽略：「All files and directories have already been synced, so the following calls to sqlite3BtreeCommitPhaseTwo() are only closing files and deleting or truncating journals.」 |
| `sqlite3VdbeCommit`（单库路径） | `src/vdbeaux.c` | `rc = sqlite3BtreeCommitPhaseOne(pBt, 0);`，随后 `rc = sqlite3BtreeCommitPhaseTwo(pBt, 0);` |
| `sqlite3RollbackAll` | `src/main.c` | `sqlite3BtreeRollback(p, tripCode, !schemaChange);` —— `schemaChange = (db->mDbFlags & DBFLAG_SchemaChange)!=0 && db->init.busy==0`，即「改了 schema 则拉倒所有游标，否则只拉倒写游标」 |
| `sqlite3BtreeClose` | `src/btree.c` | `sqlite3BtreeRollback(p, SQLITE_OK, 0);` —— 关闭时回滚；源码注释说明这个调用同时「drops any table-locks held by this handle」 |
| `backupStep` | `src/backup.c` | 目标端 `rc = sqlite3BtreeCommitPhaseTwo(p->pDest, 0);`；失败时 `sqlite3BtreeRollback(p->pDest, SQLITE_OK, 0);` |
| `backupStep` | `src/backup.c` | 源端 `TESTONLY( rc2 = ) sqlite3BtreeCommitPhaseOne(p->pSrc, 0);` 与 `TESTONLY( rc2 |= ) sqlite3BtreeCommitPhaseTwo(p->pSrc, 0);` |
| `sqlite3Prepare` | `src/prepare.c` | `rc = sqlite3BtreeBeginTrans(pDb->pBt, 0, 0);` / `sqlite3BtreeCommit(pDb->pBt);` —— 读事务 |
| `sqlite3RunParser` / `sqlite3InitOne` | `src/main.c` | `rc = sqlite3BtreeBeginTrans(pBt, 0, 0);` 与 `rc = sqlite3BtreeCommit(pBt);` —— 解析 SQL 时的 schema 读事务 |
| `sqlite3BtreeNewDb` 之外的空库初始化 | `src/btree.c` | `sqlite3BtreeBeginTrans` 内部写事务分支里 `rc = newDatabase(pBt);` —— 把空文件初始化成一个只含页 1 的空库 |
| `vacuumStep` / `vacuum` | `src/vacuum.c` | `rc = sqlite3BtreeBeginTrans(pMain, pOut==0 ? 2 : 0, 0);` —— `pOut==0` 时用 `wrflag==2`（独占事务）；另 `rc = sqlite3BtreeCommit(pTemp);` |
| `sqlite3_dbpage` 虚拟表 | `src/dbpage.c` | `if( pBt ) rc = sqlite3BtreeBeginTrans(pBt, 1, 0);` —— 写事务 |
| `sqlite3BtreeGetRequestedCacheSize` 一侧的 `sqlite3BtreeBeginTrans` | `src/btree.c` | `rc = sqlite3BtreeBeginTrans(pBtree, 0, 0);`（读）与 `rc = sqlite3BtreeBeginTrans(pBtree, 2, 0);`（独占写） |

### 关键常量与数值出处

| 数值 / 宏 | 含义 | 出处 |
|---|---|---|
| `TRANS_NONE` = **0** | 无事务 | `src/btreeInt.h`：`#define TRANS_NONE 0` |
| `TRANS_READ` = **1** | 只读事务 | `src/btreeInt.h`：`#define TRANS_READ 1` |
| `TRANS_WRITE` = **2** | 读写事务 | `src/btreeInt.h`：`#define TRANS_WRITE 2` |
| **0** / **1** / **2** | `wrflag` 的三档含义（读 / 写 / 独占写） | `sqlite3BtreeBeginTrans` 的函数头注释与 `btreeBeginTrans` 中 `wrflag>1` 的多处判断 |
| **40** | 数据库头里 schema 版本号的字节偏移 | `btreeBeginTrans` 与 `sqlite3BtreeBeginTrans` 中的 `get4byte(&pBt->pPage1->aData[40])` |
| **28** | 数据库头里「库页数」的字节偏移 | `btreeBeginTrans` 的 `get4byte(&pPage1->aData[28])`、`btreeSetNPage` 的 `get4byte(&pPage1->aData[28])`、`autoVacuumCommit` 的 `put4byte(&pPage1->aData[28], nFin)` |
| **32** / **36** | 数据库头里「空闲页链表头」与「空闲页总数」的字节偏移 | `autoVacuumCommit` 的 `put4byte(&pPage1->aData[32], 0)` 与 `put4byte(&pPage1->aData[36], 0)`；空闲页总数也由 `get4byte(&pBt->pPage1->aData[36])` 读出 |
| **16** | 数据库头魔数长度 | `newDatabase` 的 `assert( sizeof(zMagicHeader)==16 );` 与 `lockBtree` 的 `memcmp(page1, zMagicHeader, 16)` |
| `BTS_READ_ONLY` = **0x0001** | 底层文件只读 | `src/btreeInt.h`：`#define BTS_READ_ONLY 0x0001` |
| `BTS_INITIALLY_EMPTY` = **0x0010** | 事务开始时数据库是空的 | `src/btreeInt.h`：`#define BTS_INITIALLY_EMPTY 0x0010` |
| `BTS_EXCLUSIVE` = **0x0040** | 当前写者持有独占锁 | `src/btreeInt.h`：`#define BTS_EXCLUSIVE 0x0040` |
| `BTS_PENDING` = **0x0080** | 正在等读锁散开 | `src/btreeInt.h`：`#define BTS_PENDING 0x0080` |
| `BTCF_WriteFlag` = **1** | 游标是写游标的标志 | `sqlite3BtreeTripAllCursors` 的 `assert( (writeOnly==0 || writeOnly==1) && BTCF_WriteFlag==1 );` |
| **1** / **2** | `sqlite3BtreeRollback` 的 `writeOnly` 合法取值 | `sqlite3BtreeRollback` 的 `assert( writeOnly==1 || writeOnly==0 );` |
| `SQLITE_ABORT_ROLLBACK` / `SQLITE_OK` | `sqlite3BtreeRollback` 的 `tripCode` 合法取值 | 同函数的 `assert( tripCode==SQLITE_ABORT_ROLLBACK || tripCode==SQLITE_OK );` |
| `SQLITE_RESET_DATABASE`（`SQLITE_ResetDatabase`） | 允许清 `BTS_READ_ONLY` 的连接级标志 | `btreeBeginTrans` 的 `if( (p->db->flags & SQLITE_ResetDatabase) && sqlite3PagerIsreadonly(pPager)==0 )` |
| `DBFLAG_SchemaChange` | 决定回滚时是否拉倒所有游标 | `src/main.c` 的 `sqlite3RollbackAll`：`schemaChange = (db->mDbFlags & DBFLAG_SchemaChange)!=0 && db->init.busy==0;` |

## 关键取舍

- **两阶段提交拆在 B-tree 层，而不是全部塞给 pager。** `sqlite3BtreeCommitPhaseOne` 只在 `p->inTrans==TRANS_WRITE` 时才做事，非写事务直接返回 `SQLITE_OK`。这让「提交」在 B-tree 层成为一个**可以被外部编排**的协议：`sqlite3BtreeCommitPhaseOne` 之后、super-journal 删除之前，多文件事务可以插入自己的逻辑（`sqlite3VdbeCommit` 就是这么做的）。
- **提交的错误处理按「事务是否已提交」分叉。** `sqlite3BtreeCommitPhaseTwo` 的 `bCleanup` 参数是这条分叉的开关：`bCleanup==0` 时 pager 层出错就原样返回错误（上层会尝试回滚）；`bCleanup!=0` 时事务已经通过删除 super-journal 提交了，即使出错也要把 B-tree 状态重置为「写事务已关闭」，因为「the pager will have transitioned to the error state」。
- **`sqlite3BtreeCommit` 走的是单文件路径。** 它给 `sqlite3BtreeCommitPhaseOne` 传的 `zSuperJrnl` 是 `0`，给 `sqlite3BtreeCommitPhaseTwo` 传的 `bCleanup` 是 **0**。多文件路径不走这个函数，而是由 `sqlite3VdbeCommit`（`src/vdbeaux.c`）逐个 `pBt` 分别调两个阶段函数。
- **回滚时「拉倒」游标的粒度由 schema 是否变化决定。** `sqlite3RollbackAll` 传的是 `!schemaChange`：改过 schema 就拉倒全部游标（B-tree 根页可能已被移动或删除，读游标继续运行不安全），否则只拉倒写游标、读游标保存位置后可继续跟随回滚。源码注释把这写成了 `sqlite3BtreeTripAllCursors` 的契约。
- **回滚后必须重新读页 1。** `sqlite3BtreeRollback` 里 `sqlite3PagerRollback` 之后有一段专门的 `btreeGetPage(pBt, 1, &pPage1, 0)` + `btreeSetNPage` + `releasePageOne`，源码注释说明原因：「The rollback may have destroyed the pPage1->aData value.」
- **`p->iBDataVersion--` 是对 pager 侧自增的补偿。** `sqlite3PagerCommitPhaseTwo` 里做了 `pPager->iDataVersion++`，B-tree 侧则相应地 `p->iBDataVersion--`，源码注释直接写明「Compensate for pPager->iDataVersion++;」。
- **写事务的开销被尽量推迟。** `btreeBeginTrans` 把「校正页 1 的库大小字段」提前到开事务时完成，源码注释说明动机：「Doing this sooner rather than later means the database size can safely re-read the database size from page 1 if a savepoint or transaction rollback occurs within the transaction.」
- **`SQLITE_BUSY_SNAPSHOT` 被降级成 `SQLITE_BUSY`。** 只在「调用时根本没有开事务」的情况下改写（`pBt->inTransaction==TRANS_NONE`）；已经有事务时不改写，保留快照冲突的语义。
- **共享缓存下的写冲突返回专用的 `SQLITE_LOCKED_SHAREDCACHE`。** 且在返回前调 `sqlite3ConnectionBlocked(p->db, pBlock)`，把「谁挡了我」告诉上层，让上层能给出可操作的提示。
- **提交第二阶段对「已经提交」的多文件事务宽容，对单文件事务严格。** 差别就一个 `bCleanup` 参数，但语义差别很大：前者出错也要清状态并返回 `SQLITE_OK`，后者出错直接返回错误让上层回滚。
- **auto-vacuum 的回收动作放在提交第一阶段之前。** `autoVacuumCommit` 在 `sqlite3PagerCommitPhaseOne` 之前跑完，把尾部空闲页搬回文件内部并更新页 1 的空闲页计数与库大小，然后置 `pBt->bDoTruncate = 1`；提交第一阶段看到 `bDoTruncate` 就调 `sqlite3PagerTruncateImage` 把文件尾部截短。
- **auto-vacuum 失败时直接回滚 pager。** `autoVacuumCommit` 末尾的 `if( rc!=SQLITE_OK ) sqlite3PagerRollback(pPager);` 保证失败的搬移不会留下半搬状态。

## 与主核 / AI 的关系

B-tree 事务层是 SQLite「逻辑事务」与「物理原子性」之间的桥：SQL 层的 `BEGIN` / `COMMIT` / `ROLLBACK` 语义最终都落到这五个函数上，而真正的原子性由 pager 层（回滚日志）或 WAL 层实现。

**架构推论：** `sqlite3BtreeCommitPhaseOne` / `sqlite3BtreeCommitPhaseTwo` 的两阶段切分，加上 `bCleanup` 这个「事务是否已被外部提交」的开关，是把**多文件事务的原子性**放在 B-tree 之上的编排层来做的。SQLite 的做法是：每个 B-tree 各自刷盘（第一阶段），由 `sqlite3VdbeCommit` 创建并删除 super-journal 来统一宣告提交（第二阶段）。主数据库内核 `engine/db/` 若支持多表/多文件的事务，也需要类似的「刷盘」与「宣告提交」两段式接口，否则无法实现跨文件的原子性。

**架构推论：** `sqlite3BtreeTxnState`（`src/btree.c`）把事务状态对外暴露为 `SQLITE_TXN_NONE` / `SQLITE_TXN_READ` / `SQLITE_TXN_WRITE` 三态，而 `src/btreeInt.h` 里的 `TRANS_NONE` / `TRANS_READ` / `TRANS_WRITE` 三宏还额外断言了「`TRANS_NONE!=SQLITE_TXN_NONE` 等必须不成立」——即内部编码必须与公开编码逐值对齐。主核的 `txn/` 层若自行定义一套内部状态码而没有这条对齐断言，就容易出现「内部认为已提交、外部认为未提交」的错位。

**架构推论：** `btreeEndTransaction` 里「若还有其它活跃读语句则把写事务降级为 `TRANS_READ`」这条规则（判定条件 `db->nVdbeRead>1`），是把「连接级」与「语句级」的事务生命周期分离的产物——写事务的结束不等于连接上所有读者都结束了。主核的事务管理器若只维护连接级状态，在多语句并发场景下会过早释放锁。

**架构推论：** 回滚时「保存游标位置」与「拉倒游标」的区分（`sqlite3BtreeTripAllCursors` 的 `writeOnly` 分支）说明：事务回滚不一定要作废所有读语义，关键在于**被回滚的数据结构是否还物理存在**。这要求底层存储在事务回滚后仍能定位到「回滚前那个版本」的单元。主核的 B-tree 若采用「页级 undo 日志 + 原地覆盖」的方案，同一条规则同样适用。

**架构推论：** `sqlite3BtreeBeginTrans` 把读锁申请（`querySharedCacheTableLock(p, SCHEMA_ROOT, READ_LOCK)`）放在 pager 事务之前，把「页 1 校验」（`lockBtree` 的魔数比对、`page1[18]`/`page1[19]` 的格式版本检查）放在取锁之后 —— 这是一条「先声明意图，再验证对象」的固定顺序。主核在实现锁与校验时若颠倒顺序，容易在损坏库上先拿到写锁再发现问题。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/btree.c | `sqlite3BtreeBeginTrans` | 开事务的对外入口：按 `p->sharable` / `p->inTrans` / `wrflag` 分派到 `btreeBeginTrans` 或走快速路径（读 schema 版本 + `sqlite3PagerOpenSavepoint`） |
| src/btree.c | `btreeBeginTrans` | 真正的开事务实现：只读检查、共享缓存写冲突检查、`querySharedCacheTableLock(SCHEMA_ROOT, READ_LOCK)`、`lockBtree` 循环、`sqlite3PagerBegin`、`newDatabase`、BUSY 重试循环、状态与 `pBt->pWriter` 更新、页 1 库大小校正 |
| src/btree.c | `sqlite3BtreeCommitPhaseOne` | 提交第一阶段：auto-vacuum 收尾 → `sqlite3PagerTruncateImage` → `sqlite3PagerCommitPhaseOne`；非写事务直接返回 `SQLITE_OK` |
| src/btree.c | `sqlite3BtreeCommitPhaseTwo` | 提交第二阶段：`sqlite3PagerCommitPhaseTwo` → 按 `bCleanup` 决定是否吞掉错误 → `p->iBDataVersion--` → 共享状态降为 `TRANS_READ` → `btreeClearHasContent` → `btreeEndTransaction` |
| src/btree.c | `sqlite3BtreeCommit` | 一次性提交：`sqlite3BtreeCommitPhaseOne(p, 0)` + `sqlite3BtreeCommitPhaseTwo(p, 0)` |
| src/btree.c | `sqlite3BtreeRollback` | 回滚：保存/拉倒游标 → `sqlite3PagerRollback` → 重新读页 1 并 `btreeSetNPage` → 共享状态降为 `TRANS_READ` → `btreeEndTransaction` |
| src/btree.c | `btreeEndTransaction` | 提交与回滚共用的收尾：有其它活跃读语句则降级为 `TRANS_READ`，否则清表锁、减 `nTransaction`、`unlockBtreeIfUnused` |
| src/btree.c | `autoVacuumCommit` | 提交前的非增量 auto-vacuum 收尾：算 `nFin`、循环 `incrVacuumStep`、更新页 1 的空闲页与库大小字段、置 `bDoTruncate`；失败调 `sqlite3PagerRollback` |
| src/btree.c | `sqlite3BtreeTripAllCursors` | 回滚时处理游标：`writeOnly` 时只保存读游标位置并拉倒写游标，否则全部置 `CURSOR_FAULT` |
| src/btree.c | `btreeClearHasContent` | 写事务结束时销毁 `BtShared.pHasContent` 位图 |
| src/btree.c | `saveAllCursors` | 回滚 / savepoint 前批量保存游标位置 |
| src/btree.c | `unlockBtreeIfUnused` | 无事务且页 1 已取时释放页 1，从而释放读锁 |
| src/btree.c | `newDatabase` | 把空文件初始化成一个只含页 1 的空库（写魔数、页大小、空闲页计数，`pBt->nPage = 1`） |
| src/btree.c | `lockBtree` | 开事务时取页 1 并校验文件头：魔数比对、格式版本（`page1[18]`/`page1[19]`）、页大小 |
| src/btree.c | `btreeSetNPage` | 按页 1 的偏移 28 字段更新 `pBt->nPage`；字段为 0 时改用 `sqlite3PagerPagecount` |
| src/btree.c | `sqlite3BtreeTxnState` | 对外暴露事务三态（`SQLITE_TXN_NONE` / `_READ` / `_WRITE`） |
| src/btree.c | `btreeInvokeBusyHandler` | 开事务遇 `SQLITE_BUSY` 时调用的重试钩子，转调 `sqlite3InvokeBusyHandler` |
| src/btree.c | `sqlite3BtreeBeginStmt` | 语句级子事务：实现为匿名 savepoint，转调 `sqlite3PagerOpenSavepoint` |
| src/btree.c | `sqlite3BtreeSavepoint` | SAVEPOINT 的释放 / 回滚；支持 `iSavepoint==-1` 的「回滚内容但事务保持打开」 |
| src/btreeInt.h | `TRANS_NONE` / `TRANS_READ` / `TRANS_WRITE` | 事务状态三态，取值 0 / 1 / 2；并断言与 `SQLITE_TXN_*` 逐值对应 |
| src/btreeInt.h | `BTS_READ_ONLY` / `BTS_INITIALLY_EMPTY` / `BTS_EXCLUSIVE` / `BTS_PENDING` | BtShared 的四个状态位，取值 0x0001 / 0x0010 / 0x0040 / 0x0080 |
| src/btreeInt.h | `btreeIntegrity` | 断言宏：共享事务状态与句柄事务状态的一致性 |
| src/main.c | `sqlite3RollbackAll` | 回滚的真实调用方：遍历 `db->aDb`，按 `!schemaChange` 决定是否拉倒全部游标 |
| src/vdbeaux.c | `sqlite3VdbeCommit` | 多文件事务的编排者：逐个 `pBt` 调 `sqlite3BtreeCommitPhaseOne(pBt, zSuper)`，删除 super-journal 后逐个调 `sqlite3BtreeCommitPhaseTwo(pBt, 1)` |
| src/vdbe.c | `sqlite3VdbeHalt` | 事务的发起者：`sqlite3BtreeBeginTrans(pBt, pOp->p2, &iMeta)` |
| src/pager.c | `sqlite3PagerCommitPhaseOne` / `sqlite3PagerCommitPhaseTwo` / `sqlite3PagerRollback` | B-tree 层调用的下层实现：真正的刷盘、日志处理与回放 |
| src/btmutex.c | `sqlite3BtreeEnter` | B-tree 互斥锁的获取；`src/btree.h` 在 `SQLITE_OMIT_SHARED_CACHE` 下把它定义为空宏 |

## 参考

- [SQLite Atomic Commit In SQLite（sqlite.org 官方文档）](https://sqlite.org/atomiccommit.html)
- [SQLite Write-Ahead Logging（sqlite.org 官方文档）](https://sqlite.org/wal.html)
- [SQLite Transaction（sqlite.org 官方文档）](https://sqlite.org/lang_transaction.html)
- [SQLite B-tree 实现（sqlite.org 官方文档）](https://sqlite.org/btreetree.html)
- [SQLite 源码浏览：src/btree.c（官方 Fossil 仓库）](https://sqlite.org/src/file/src/btree.c)
- [sqlite/sqlite 仓库中的 src/btree.c（GitHub 镜像）](https://github.com/sqlite/sqlite/blob/master/src/btree.c)
