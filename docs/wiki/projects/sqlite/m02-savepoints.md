---
project: sqlite
feature: savepoints
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3CloseSavepoints]
  - path: src/btree.c
    symbols: [sqlite3BtreeSavepoint]
---

# 保存点

## 一句话职责

保存点是 SQLite 在单个连接上提供的**具名、可嵌套的事务内标记**：用 `SAVEPOINT` 建立、`RELEASE` 合并、`ROLLBACK TO` 回退到该标记，标记以下的写入被撤销，而外层事务本身继续存活。

## 机制是什么

SQLite 的保存点不是独立机制，而是**普通事务的嵌套层次**。一条 `SAVEPOINT name` 语句在自动提交模式下等价于开启一个延迟事务（`db->autoCommit` 被清零）；在显式事务中则只是在事务内部多压一层标记。

从源码看，保存点在三个层次上各有一份状态：

- **连接层（`sqlite3` 结构体）**：`src/sqliteInt.h` 里 `sqlite3` 有 `Savepoint *pSavepoint`（活跃保存点链表，注释为 "List of active savepoints"）、`int nSavepoint`（非事务型保存点数量）、`int nStatement`（嵌套语句级子事务数量）、`u8 isTransactionSavepoint`（注释为 "True if the outermost savepoint is a TS"）。链表头是**最近打开**的保存点，父指针是 `Savepoint.pNext`。
- **VDBE 层**：`src/sqliteInt.h` 定义了 `struct Savepoint`，含 `char *zName`（以 NUL 结尾的名字）、`i64 nDeferredCons`（延迟外键违例数）、`i64 nDeferredImmCons`（延迟立即外键违例数）、`Savepoint *pNext`（父保存点）。三个操作码常量定义在同一头文件：`SAVEPOINT_BEGIN` 为 0、`SAVEPOINT_RELEASE` 为 1、`SAVEPOINT_ROLLBACK` 为 2；`src/build.c` 的 `sqlite3Savepoint` 里有一条编译期断言 `assert( !SAVEPOINT_BEGIN && SAVEPOINT_RELEASE==1 && SAVEPOINT_ROLLBACK==2 )` 把这三个数值钉死。
- **Btree / Pager 层**：每个数据库文件（Btree）各自持有一套 pager 保存点数组 `Pager.aSavepoint` 与计数 `Pager.nSavepoint`。

保存点的**语句级撤销**由两层组成：`ROLLBACK TO` 要撤销的是「该保存点建立之后、到回退命令之前」这一段区间内所有写入对数据库文件的改动，这由 pager 层的子日志（sub-journal）承担；而「一条 SQL 语句自身失败时只撤销这条语句、不动整个事务」这件事，则由**匿名保存点**实现——`src/btree.c` 的 `sqlite3BtreeBeginStmt` 注释明确写着 "A statement sub-transaction is implemented as an anonymous savepoint"，它在 pager 层的保存点序号大于所有显式 `SAVEPOINT` 创建的保存点。

**边界**（依据页面清单条目）：本页负责保存点的建立、释放与回滚到保存点时的语句级撤销；不负责 WAL 模式的帧截断（H05 页面），也不负责分页器层的保存点（G05 页面）。

## 怎么实现的

### 1. 语法层：三条语句编译成一条 VDBE 指令

`src/parse.y` 的语法动作把三种写法归一到同一个入口：

- `cmd ::= SAVEPOINT nm(X)` → 调用 `sqlite3Savepoint(pParse, SAVEPOINT_BEGIN, &X)`。
- `cmd ::= RELEASE savepoint_opt nm(X)` → 调用 `sqlite3Savepoint(pParse, SAVEPOINT_RELEASE, &X)`。语法里的 `savepoint_opt` 是可空的，意味着 `RELEASE name` 与 `RELEASE SAVEPOINT name` 两种拼写都合法。
- `cmd ::= ROLLBACK trans_opt TO savepoint_opt nm(X)` → 调用 `sqlite3Savepoint(pParse, SAVEPOINT_ROLLBACK, &X)`。

`src/build.c` 的 `sqlite3Savepoint` 做三件事：把名字 token 拷成 C 字符串（`sqlite3NameFromToken`）；在 `SQLITE_OMIT_AUTHORIZATION` 未定义时做授权检查，授权动作码是 `SQLITE_SAVEPOINT`，动作名数组 `az[]` 依次为 `"BEGIN"`、`"RELEASE"`、`"ROLLBACK"`，并由 `assert( !SAVEPOINT_BEGIN && SAVEPOINT_RELEASE==1 && SAVEPOINT_ROLLBACK==2 )` 保证下标与常量对齐——检查不通过或 `sqlite3GetVdbe` 返回空（无需生成代码）就直接释放名字返回；否则发出 `OP_Savepoint` 指令，`p1` 放操作码、`p4` 放名字并标记 `P4_DYNAMIC`。

### 2. VDBE 层：`OP_Savepoint` 的分支

`src/vdbe.c` 的 `case OP_Savepoint:` 分支开头有四条断言：`db->pSavepoint==0 || db->autoCommit==0`（没有打开的事务就不可能有保存点）、`p1` 必须是三个常量之一、`db->pSavepoint || db->isTransactionSavepoint==0`、以及 `checkSavepointCount(db)`。

`checkSavepointCount`（`src/vdbe.c`，只在 `!NDEBUG` 下定义）遍历 `db->pSavepoint` 链表数出节点个数，断言它等于 `db->nSavepoint + db->isTransactionSavepoint`——这正是「非事务型保存点计数 + 是否存在事务型保存点」这一表示法的定义式。

**建立（`p1==SAVEPOINT_BEGIN`）**：

1. 若 `db->nVdbeWrite>0`（有活跃的写语句，即未关闭的读写增量 BLOB 句柄），报错 "cannot open savepoint - SQL statements in progress" 并返回 `SQLITE_BUSY`。
2. 否则先调 `sqlite3VtabSavepoint(db, SAVEPOINT_BEGIN, db->nStatement+db->nSavepoint)` 通知虚拟表模块，失败则跳 `abort_due_to_error`。
3. 用 `sqlite3DbMallocRawNN` 分配 `sizeof(Savepoint)+nName+1` 字节，把名字 memcpy 到结构体尾部（`pNew->zName = (char *)&pNew[1]`）。
4. 若当前处于自动提交模式（`db->autoCommit` 为真），则清零 `db->autoCommit` 并置 `db->isTransactionSavepoint = 1`——这就是「事务型保存点」（transaction savepoint）；否则 `db->nSavepoint++`。
5. 头插进链表：`pNew->pNext = db->pSavepoint; db->pSavepoint = pNew;`，同时把当前的 `db->nDeferredCons` 与 `db->nDeferredImmCons` 快照进 `pNew->nDeferredCons` / `pNew->nDeferredImmCons`。

**查找与释放 / 回退（`p1==SAVEPOINT_RELEASE` 或 `SAVEPOINT_ROLLBACK`）**：

1. 从链表头开始用 `sqlite3StrICmp`（大小写不敏感）按名字查找，边走边累加 `iSavepoint` 计数。找不到则报错 "no such savepoint: %s" 并返回 `SQLITE_ERROR`。
2. 若 `db->nVdbeWrite>0` 且操作是 RELEASE，报错 "cannot release savepoint - SQL statements in progress" 并返回 `SQLITE_BUSY`。
3. 判定是否事务型保存点：`int isTransaction = pSavepoint->pNext==0 && db->isTransactionSavepoint;`——只有链表中最外层那一个、且标记位为真时才算。
4. 若 `isTransaction && p1==SAVEPOINT_RELEASE`（释放最外层保存点）：先 `sqlite3VdbeCheckFkDeferred(p)` 检查延迟外键；通过后置 `db->autoCommit = 1`，再调 `sqlite3VdbeHalt(p)`；若 `sqlite3VdbeHalt` 返回 `SQLITE_BUSY`，则把 `p->pc` 回退到当前指令、恢复 `db->autoCommit = 0` 并返回 `SQLITE_BUSY`；若 `p->rc` 非零同样恢复 `db->autoCommit = 0`，成功才清 `db->isTransactionSavepoint = 0`。
5. 其它情形进入统一分支：
   - 把计数换算成 pager 层的下标：`iSavepoint = db->nSavepoint - iSavepoint - 1;`（链表是头插的，所以要反向换算）。
   - 若操作是 ROLLBACK，先遍历 `ii=0..db->nDb-1` 对每个库调 `sqlite3BtreeTripAllCursors(db->aDb[ii].pBt, SQLITE_ABORT_ROLLBACK, isSchemaChange==0)` 把游标熔断；其中 `isSchemaChange` 取 `(db->mDbFlags & DBFLAG_SchemaChange)!=0`。若是 RELEASE，`isSchemaChange` 直接置 0。
   - 然后同样遍历所有库，调 `sqlite3BtreeSavepoint(db->aDb[ii].pBt, p1, iSavepoint)`，任一库失败即跳 `abort_due_to_error`。
   - 若 `isSchemaChange` 为真，调 `sqlite3ExpirePreparedStatements(db, 0)`、`sqlite3ResetAllSchemasOfConnection(db)`，并重新置上 `db->mDbFlags |= DBFLAG_SchemaChange`。
6. 无论 RELEASE 还是 ROLLBACK，都销毁被操作保存点**内部**的所有嵌套保存点：`while( db->pSavepoint!=pSavepoint )` 循环里逐个 `sqlite3DbFree` 并 `db->nSavepoint--`。
7. 若操作是 RELEASE，则把目标保存点本身也摘除并释放（`db->pSavepoint = pSavepoint->pNext; sqlite3DbFree(db, pSavepoint);`），非事务型时 `db->nSavepoint--`。若是 ROLLBACK，则**保留**该保存点，但把 `db->nDeferredCons` / `db->nDeferredImmCons` 恢复成建立时快照的值。
8. 最后在 `!isTransaction || p1==SAVEPOINT_ROLLBACK` 时调 `sqlite3VtabSavepoint(db, p1, iSavepoint)` 通知虚拟表模块。

### 3. Btree 层：`sqlite3BtreeSavepoint`

`src/btree.c` 的 `sqlite3BtreeSavepoint(Btree *p, int op, int iSavepoint)` 是本条目指定的源码落点之一。它的函数头注释写明：`op` 永远是 `SAVEPOINT_ROLLBACK` 或 `SAVEPOINT_RELEASE`；`iSavepoint` 正常 ≥ 0，但当 `op==SAVEPOINT_ROLLBACK` 时可以是 -1，含义是「回滚整个事务的内容」——注释强调这与常规事务回滚不同，因为**不释放锁、事务保持打开**。

函数体：

1. 先判 `if( p && p->inTrans==TRANS_WRITE )`——**只有处于写事务时才真正干活**，否则直接返回初值 `SQLITE_OK`。
2. 断言 `op` 是两个常量之一，且 `iSavepoint>=0 || (iSavepoint==-1 && op==SAVEPOINT_ROLLBACK)`。
3. `sqlite3BtreeEnter(p)` 取 b-tree 互斥锁。
4. 若 `op==SAVEPOINT_ROLLBACK`，先调 `saveAllCursors(pBt, 0, 0)` 保存所有游标位置（失败则整个操作中止）。`saveAllCursors`（`src/btree.c`）遍历 `pBt->pCursor` 链表找到需要保存的游标后交给 `saveCursorsOnList` 实际保存；常见情况下无需保存时它只清掉 `pExcept` 的 `BTCF_Multiple` 标志就返回，不做实际工作。
5. 成功则调 `sqlite3PagerSavepoint(pBt->pPager, op, iSavepoint)` 落到 pager 层。
6. pager 层成功后再做 btree 层的收尾：若 `iSavepoint<0`（整事务回滚）且 `pBt->btsFlags` 带 `BTS_INITIALLY_EMPTY`，则 `pBt->nPage = 0`；接着 `newDatabase(pBt)` 与 `btreeSetNPage(pBt, pBt->pPage1)` 修正页数，并断言（`CORRUPT_DB` 除外）`pBt->nPage>0`。
7. `sqlite3BtreeLeave(p)` 释放锁，返回 `rc`。

### 4. Pager 层：保存点如何真正撤销写入

`src/pager.c` 的 `PagerSavepoint` 结构体是这一层的落地物，字段包括：`i64 iOffset`（主日志中的起始偏移）、`i64 iHdrOffset`、`Bitvec *pInSavepoint`（落在本保存点内的页集合）、`Pgno nOrig`（建立保存点时文件的原始页数）、`Pgno iSubRec`（子日志中第一条记录的索引）、`int bTruncateOnRelease`、以及 WAL 模式下的 `u32 aWalData[WAL_SAVEPOINT_NDATA]`。

`sqlite3PagerOpenSavepoint(Pager *pPager, int nSavepoint)` 只在 `nSavepoint>pPager->nSavepoint && pPager->useJournal` 时才干活，转调内部函数 `pagerOpenSavepoint`。`pagerOpenSavepoint` 用 `sqlite3Realloc` 扩大 `pPager->aSavepoint` 数组（分配失败返回 `SQLITE_NOMEM_BKPT`），把新增部分 memset 清零，然后对每个新保存点：`nOrig = pPager->dbSize`；若日志文件已打开且 `pPager->journalOff>0` 则 `iOffset = pPager->journalOff`，否则 `iOffset = JOURNAL_HDR_SZ(pPager)`（该宏在 `src/pager.c` 定义为 `pPager->sectorSize`）；`iSubRec = pPager->nSubRec`；`pInSavepoint = sqlite3BitvecCreate(pPager->dbSize)`（分配失败同样返回 `SQLITE_NOMEM_BKPT`）；`bTruncateOnRelease = 1`；若 `pagerUseWal(pPager)` 则调 `sqlite3WalSavepoint(pPager->pWal, aNew[ii].aWalData)` 记录 WAL 侧上下文。

`sqlite3PagerSavepoint(Pager *pPager, int op, int iSavepoint)` 是撤销与释放的实际执行者：

1. 入口取 `rc = pPager->errCode`；在 `SQLITE_ENABLE_ZIPVFS` 编译下若操作是 RELEASE 则强制 `rc = SQLITE_OK`。
2. 断言 `op` 是两个常量之一，且 `iSavepoint>=0 || op==SAVEPOINT_ROLLBACK`。
3. 仅当 `rc==SQLITE_OK && iSavepoint<pPager->nSavepoint` 才继续（索引越界即空操作）。
4. 算出操作后剩余的保存点数 `nNew = iSavepoint + ((op==SAVEPOINT_RELEASE) ? 0 : 1)`——**RELEASE 连目标保存点一起销毁，ROLLBACK 保留目标保存点**。
5. 对 `ii=nNew..pPager->nSavepoint-1` 逐个 `sqlite3BitvecDestroy(pPager->aSavepoint[ii].pInSavepoint)`，然后 `pPager->nSavepoint = nNew`。
6. 若是 RELEASE 且 `pPager->aSavepoint[nNew].bTruncateOnRelease` 为真且子日志文件已打开，则**仅在子日志是内存日志时**（`sqlite3JournalIsInMemory(pPager->sjfd)`）用 `sqlite3OsTruncate(pPager->sjfd, (pPager->pageSize+4)*pRel->iSubRec)` 截断子日志，并把 `pPager->nSubRec` 恢复为 `pRel->iSubRec`。
7. 若是 ROLLBACK，则走 `pagerUseWal(pPager) || isOpen(pPager->jfd)` 分支，取 `pSavepoint = (nNew==0) ? 0 : &pPager->aSavepoint[nNew-1]`，调 `pagerPlaybackSavepoint(pPager, pSavepoint)` 真正回放日志。`pagerPlaybackSavepoint`（`src/pager.c`）先把 `pPager->dbSize` 恢复为 `pSavepoint->nOrig`（或事务起点值 `pPager->dbOrigSize`），再用 `sqlite3BitvecCreate(pSavepoint->nOrig)` 建立 `pDone` 位图确保同一页只回滚一次，然后把 `pPager->journalOff` 置为 `pSavepoint->iOffset` 逐页回放主日志，再从第一个日志头继续回放到主日志末尾，最后回放子日志中尚未回滚的页。
8. 在 `SQLITE_ENABLE_ZIPVFS` 编译下，若 `journalMode==PAGER_JOURNALMODE_OFF` 且 `eState>=PAGER_WRITER_CACHEMOD`，则置 `pPager->errCode = SQLITE_ABORT`、`pPager->eState = PAGER_ERROR` 并调 `setGetterMethod(pPager)`，让整个事务在 ZipVFS 层回滚。

### 5. 语句级撤销：匿名保存点

`src/vdbe.c` 的 `OP_Transaction` 分支负责开启语句级子事务：当 `p->usesStmtJournal && pOp->p2 && (db->autoCommit==0 || db->nVdbeRead>1)` 时，若 `p->iStatement==0` 则 `db->nStatement++` 并置 `p->iStatement = db->nSavepoint + db->nStatement`，随后调 `sqlite3VtabSavepoint(db, SAVEPOINT_BEGIN, p->iStatement-1)` 与 `sqlite3BtreeBeginStmt(pBt, p->iStatement)`。

`src/btree.c` 的 `sqlite3BtreeBeginStmt(Btree *p, int iStatement)` 的注释说明：语句子事务是**匿名保存点**，第二个参数是 B-Tree 上打开的保存点总数（含这个新建的匿名保存点）；若没有活跃保存点也没有其它语句事务，`iStatement` 为 1。函数体在 `sqlite3BtreeEnter(p)` 后调 `sqlite3PagerOpenSavepoint(pBt->pPager, iStatement)`。它带三条断言：`p->inTrans==TRANS_WRITE`、非只读、`iStatement>0`，以及 `iStatement>p->db->nSavepoint`——**这条断言正是「匿名保存点的序号必须大于所有显式保存点」的强制点**。

语句结束时，`src/vdbeaux.c` 的 `vdbeCloseStatement(Vdbe *p, int eOp)` 用 `const int iSavepoint = p->iStatement-1;` 定位该匿名保存点，对每个库先按 `eOp` 调 `sqlite3BtreeSavepoint(pBt, SAVEPOINT_ROLLBACK, iSavepoint)`、再调 `sqlite3BtreeSavepoint(pBt, SAVEPOINT_RELEASE, iSavepoint)`，随后 `db->nStatement--` 并置 `p->iStatement = 0`。若 `eOp==SAVEPOINT_ROLLBACK`，还会把 `db->nDeferredCons` / `db->nDeferredImmCons` 恢复为语句开始时的快照 `p->nStmtDefCons` / `p->nStmtDefImmCons`。

### 6. 释放与清理：`sqlite3CloseSavepoints`

`src/main.c` 的 `sqlite3CloseSavepoints(sqlite3 *db)` 是本条目指定的另一个源码落点。函数头注释写明：「Close all open savepoints. This function only manipulates fields of the database handle object, it does not close any savepoints that may be open at the b-tree/pager level.」函数体极简：`while( db->pSavepoint )` 循环里把 `db->pSavepoint` 移到 `pTmp->pNext` 并 `sqlite3DbFree(db, pTmp)` 释放每个 `Savepoint` 结构；循环结束后把 `db->nSavepoint = 0`、`db->nStatement = 0`、`db->isTransactionSavepoint = 0` 三个计数/标志一起清零。

它的**实际调用方**是 `src/main.c` 的 `sqlite3LeaveMutexAndCloseZombie`：该函数确认连接已是 `SQLITE_STATE_ZOMBIE` 且不忙之后，先调 `sqlite3RollbackAll(db, SQLITE_OK)` 回滚事务（注释说明这同时保证未提交事务修改过的模式被重置，且所需的 b-tree 互斥锁被持有，使 pager 回滚与模式重置成为原子操作），紧接着调 `sqlite3CloseSavepoints(db)` 释放所有残留的 `Savepoint` 结构，然后才逐个 `sqlite3BtreeClose` 关闭各库。

### 7. 虚拟表侧的联动

`src/vtab.c` 的 `sqlite3VtabSavepoint(sqlite3 *db, int op, int iSavepoint)` 遍历 `db->aVTrans` 里的每个虚拟表，只对 `pMod->iVersion>=2` 的模块发通知。`SAVEPOINT_BEGIN` 时调 `xSavepoint` 并置 `pVTab->iSavepoint = iSavepoint+1`；`SAVEPOINT_ROLLBACK` 时调 `xRollbackTo`；其余（RELEASE）调 `xRelease`。真正调用的条件是 `xMethod && pVTab->iSavepoint>iSavepoint`——**只有比目标更内层的虚拟表保存点才需要被通知**。调用期间会临时清掉 `SQLITE_Defensive` 标志再恢复。

## 关键取舍

- **保存点是连接级链表，Btree 层按库各持一份（源码事实）**：连接层用 `db->pSavepoint` 链表记录名字与外键计数，Btree 层用 `Pager.aSavepoint` 数组记录每个库的日志偏移与页位图。代价是同一次 `SAVEPOINT` 会在每个已挂载数据库上各占一个 pager 保存点槽位；`OP_Savepoint` 的释放/回退分支必须遍历 `db->nDb` 逐库调用 `sqlite3BtreeSavepoint` 才能保持一致。
- **保存点计数用「链表长度 = nSavepoint + isTransactionSavepoint」表示（源码事实）**：`checkSavepointCount` 把这个等式写成断言。它让「事务型保存点」不占 `nSavepoint` 的名额，因此最外层事务型保存点的下标换算 `iSavepoint = db->nSavepoint - iSavepoint - 1` 不会因为它而错位。代价是读代码时必须同时看两个字段才能知道实际嵌套深度。
- **头插链表 + 反向下标换算（源码事实）**：链表头是最近打开的保存点，查找时从头累计得到的是「距最新多远」，释放时要用 `db->nSavepoint - iSavepoint - 1` 换算成 pager 需要的「从最外层数第几个」。这个正反两个方向的换算是本页最易出错的地方。
- **RELEASE 销毁目标、ROLLBACK 保留目标（源码事实）**：pager 层的 `nNew = iSavepoint + ((op==SAVEPOINT_RELEASE) ? 0 : 1)` 一行决定了两者的差异；VDBE 层对应地，RELEASE 摘除 `pSavepoint` 本身并 `db->nSavepoint--`，ROLLBACK 保留它并恢复外键延迟计数快照。
- **有活跃写语句时禁止建立/释放保存点（源码事实）**：`db->nVdbeWrite>0` 时 SAVEPOINT 返回 `SQLITE_BUSY`、RELEASE 返回 `SQLITE_BUSY`，错误文案分别是 "cannot open savepoint - SQL statements in progress" 与 "cannot release savepoint - SQL statements in progress"。**ROLLBACK TO 不受此限制**——它的分支条件是 `db->nVdbeWrite>0 && p1==SAVEPOINT_RELEASE`，回退因此可以在写语句活跃时进行。
- **找不到同名保存点直接报错（源码事实）**：查找用 `sqlite3StrICmp` 大小写不敏感比较，找不到则 "no such savepoint: %s" + `SQLITE_ERROR`。因此保存点名字不区分大小写，且**同名保存点可以存在多层**，查找命中的是最近打开的那一个。
- **只在写事务里才执行释放/回退（源码事实）**：`sqlite3BtreeSavepoint` 的入口条件是 `p && p->inTrans==TRANS_WRITE`，否则静默返回 `SQLITE_OK`。这把「保存点操作在纯读事务中是空操作」固化在 btree 层，无需上层再判断。
- **回退会熔断游标并可能作废已准备语句（源码事实）**：`ROLLBACK TO` 分支对每个库调 `sqlite3BtreeTripAllCursors(..., SQLITE_ABORT_ROLLBACK, isSchemaChange==0)`；`isSchemaChange` 由 `DBFLAG_SchemaChange` 决定，为真时额外调 `sqlite3ExpirePreparedStatements(db, 0)` 与 `sqlite3ResetAllSchemasOfConnection(db)`。代价是回退之后此前打开的语句游标不可继续使用。
- **子日志截断只对内存子日志生效（源码事实）**：`pagerOpenSavepoint` 把 `bTruncateOnRelease` 置 1，但 `sqlite3PagerSavepoint` 的 RELEASE 分支额外要求 `sqlite3JournalIsInMemory(pPager->sjfd)` 才调 `sqlite3OsTruncate`。落盘的子日志不做截断，靠 `pPager->nSubRec = pRel->iSubRec` 记账。
- **WAL 模式下保存点靠帧上下文回退而非日志回放（源码事实）**：`pagerOpenSavepoint` 在 `pagerUseWal(pPager)` 时调 `sqlite3WalSavepoint(pPager->pWal, aNew[ii].aWalData)`，该函数（`src/wal.c`）把 `pWal->hdr.mxFrame`、两个 `aFrameCksum` 分量和 `pWal->nCkpt` 写进数组，数组长度由 `src/wal.h` 的 `#define WAL_SAVEPOINT_NDATA 4` 固定为 4。
- **释放最外层保存点 = 提交事务（源码事实）**：VDBE 层判定 `isTransaction = pSavepoint->pNext==0 && db->isTransactionSavepoint`，命中且操作是 RELEASE 时置 `db->autoCommit = 1` 并调 `sqlite3VdbeHalt(p)`；若 `sqlite3VdbeHalt` 返回 `SQLITE_BUSY`，则把 `p->pc` 回退到当前指令、恢复 `db->autoCommit = 0` 并返回 `SQLITE_BUSY`，保证提交失败时事务不被误关。
- **保存点结构与 pager 保存点分离（架构推论）**：`sqlite3CloseSavepoints` 的注释明确说它「只操作数据库句柄对象的字段，不会关闭 b-tree/pager 层可能仍然打开的保存点」。这意味着 SQL 层的命名与嵌套结构（`db->pSavepoint` 链表）与存储层的撤销能力（`Pager.aSavepoint`）是**两套独立记账**：前者管名字与语义，后者管日志偏移与页位图；两者靠 `OP_Savepoint` 分支里的下标换算对齐。这个分层使存储层可以在没有 SQL 层的场合（语句级子事务）独立使用保存点机制。
- **回滚保留锁与事务（源码事实）**：`sqlite3BtreeSavepoint` 的注释说明 `iSavepoint==-1` 时回滚整个事务内容，但**不释放锁、事务保持打开**，这与 `sqlite3PagerRollback` 不同。因此 `ROLLBACK TO` 之后事务仍在进行中，可以继续写入。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/main.c` | `sqlite3CloseSavepoints` | 释放 `db->pSavepoint` 链表上全部 `Savepoint` 结构，并把 `db->nSavepoint`、`db->nStatement` 清零、`db->isTransactionSavepoint` 置 0 |
| `src/main.c` | `sqlite3LeaveMutexAndCloseZombie` | `sqlite3CloseSavepoints` 的实际调用方：先 `sqlite3RollbackAll(db, SQLITE_OK)` 再释放保存点，然后关闭各库 Btree |
| `src/btree.c` | `sqlite3BtreeSavepoint` | 释放/回退保存点的 Btree 层入口：写事务内先 `saveAllCursors` 再转 `sqlite3PagerSavepoint`，成功后修正 `pBt->nPage` |
| `src/btree.c` | `sqlite3BtreeBeginStmt` | 开启语句级子事务（匿名保存点），调用 `sqlite3PagerOpenSavepoint(pBt->pPager, iStatement)` |
| `src/btree.c` | `saveAllCursors` | 回退前保存所有游标位置，无游标需保存时只清 `BTCF_Multiple` 标志 |
| `src/btree.c` | `sqlite3BtreeTripAllCursors` | 回退时熔断游标；`writeOnly` 为真时只熔断写游标，读游标改为保存位置 |
| `src/btree.c` | `btreeBeginTrans` / `sqlite3BtreeBeginTrans` | 开写事务时调 `sqlite3PagerOpenSavepoint(pPager, p->db->nSavepoint)` 保证 pager 保存点数量与 SQL 层一致 |
| `src/vdbe.c` | `OP_Savepoint` | 保存点语义的 VDBE 实现：建立、查找、释放、回退、事务型保存点判定与错误分支 |
| `src/vdbe.c` | `checkSavepointCount` | 断言链表长度等于 `db->nSavepoint + db->isTransactionSavepoint` |
| `src/vdbe.c` | `OP_Transaction` | 驱动语句级子事务：`db->nStatement++`、`p->iStatement = db->nSavepoint + db->nStatement`、调 `sqlite3BtreeBeginStmt` |
| `src/vdbeaux.c` | `vdbeCloseStatement` | 语句结束时对每个库调 `sqlite3BtreeSavepoint` 做 ROLLBACK 与 RELEASE，并恢复延迟外键计数 |
| `src/build.c` | `sqlite3Savepoint` | 语法入口：授权检查后发出 `OP_Savepoint`，动作名数组 `az[]` 为 `"BEGIN"`/`"RELEASE"`/`"ROLLBACK"` |
| `src/parse.y` | `cmd` 规则中的 SAVEPOINT / RELEASE / ROLLBACK TO | 三种 SQL 写法归一到 `sqlite3Savepoint` 的三个操作码 |
| `src/sqliteInt.h` | `struct Savepoint` | 名字、延迟外键计数快照与父指针 |
| `src/sqliteInt.h` | `SAVEPOINT_BEGIN` / `SAVEPOINT_RELEASE` / `SAVEPOINT_ROLLBACK` | 三个操作码常量，值为 0 / 1 / 2 |
| `src/sqliteInt.h` | `sqlite3.pSavepoint` / `nSavepoint` / `nStatement` / `isTransactionSavepoint` | 连接级保存点状态四件套 |
| `src/pager.c` | `sqlite3PagerOpenSavepoint` / `pagerOpenSavepoint` | 扩容 `Pager.aSavepoint` 并逐个初始化 `nOrig`、`iOffset`、`iSubRec`、`pInSavepoint`、`aWalData` |
| `src/pager.c` | `sqlite3PagerSavepoint` | 释放/回退的 pager 层实现：算 `nNew`、销毁位图、截断内存子日志、触发 `pagerPlaybackSavepoint` |
| `src/pager.c` | `pagerPlaybackSavepoint` | 回放主日志与子日志把页面恢复到 `pSavepoint->nOrig` 时的状态 |
| `src/pager.c` | `struct PagerSavepoint` | pager 层保存点结构：日志偏移、页位图、原始页数、子日志索引、WAL 上下文 |
| `src/vtab.c` | `sqlite3VtabSavepoint` | 向 `iVersion>=2` 的虚拟表模块派发 `xSavepoint` / `xRollbackTo` / `xRelease` |
| `src/wal.c` | `sqlite3WalSavepoint` | 记录 WAL 回退上下文（`mxFrame`、两个校验和分量、`nCkpt`） |
| `src/wal.h` | `WAL_SAVEPOINT_NDATA` | WAL 保存点上下文数组长度常量，值为 4 |

## 与主核 / AI 的关系

**架构推论：** 主核的事务管理器若要支持保存点，可以直接照搬 SQLite 这套「连接层命名链表 + 每文件独立撤销槽位」的双层记账：上层用链表管名字、嵌套与语义，下层用每文件一组槽位管撤销位置，两者靠一个下标换算绑定。主核已有 `txn/` 层（按 `engine/db/` 的分层纪律位于 L2），若在其中引入保存点，这套换算关系是可直接对照的。

对本项目而言，本页可复用的是**判定口径**而不是代码：`db->nVdbeWrite>0` 时禁止建立/释放保存点但允许 `ROLLBACK TO`、`isTransaction = pSavepoint->pNext==0 && db->isTransactionSavepoint` 这条判定、以及 `nNew = iSavepoint + ((op==SAVEPOINT_RELEASE) ? 0 : 1)` 这一行同时决定销毁范围的写法——都是可以在主核事务管理器里对照的设计口径。

对 AI 协作而言，本页提供的是**读源码得到的断言**，不是 SQLite 的通用印象：例如「保存点名字大小写不敏感」「同名保存点可以多层嵌套」「`ROLLBACK TO` 保留目标保存点而 `RELEASE` 销毁它」「保存点操作只在写事务内生效」这几条，都应回到 `src/vdbe.c` 的 `OP_Savepoint` 分支与 `src/btree.c` 的 `sqlite3BtreeSavepoint` 具体判断核对。特别地，`OP_Savepoint` 中「`ROLLBACK TO` 不受 `nVdbeWrite>0` 限制」这一点与直觉相反（直觉以为回退也是受限操作），评审时最值得逐行复核。

## 参考

- [SQLite 官方文档：保存点（Savepoints）](https://sqlite.org/lang_savepoint.html) —— 说明 `SAVEPOINT` / `RELEASE` / `ROLLBACK TO` 的语法、嵌套规则，以及「最外层保存点被 RELEASE 等价于 COMMIT」的语义背景；机制实现以源码为准。
- [SQLite 官方文档：事务（Transaction）](https://sqlite.org/lang_transaction.html) —— 提供 `BEGIN` / `COMMIT` / `ROLLBACK` 的语义背景，用于对照保存点与普通事务的关系。
- [SQLite 官方源码：`src/vdbe.c`](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— 支撑 `OP_Savepoint` 分支的建立、查找、释放、回退逻辑与 `SQLITE_BUSY` / `SQLITE_ERROR` 错误分支。
- [SQLite 官方源码：`src/btree.c`](https://github.com/sqlite/sqlite/blob/master/src/btree.c) —— 支撑 `sqlite3BtreeSavepoint` 与 `sqlite3BtreeBeginStmt` 的写事务判定、游标保存与匿名保存点序号约束。
- [SQLite 官方源码：`src/pager.c`](https://github.com/sqlite/sqlite/blob/master/src/pager.c) —— 支撑 `sqlite3PagerOpenSavepoint` / `sqlite3PagerSavepoint` / `pagerPlaybackSavepoint` 的保存点槽位、日志回放与子日志截断细节。
