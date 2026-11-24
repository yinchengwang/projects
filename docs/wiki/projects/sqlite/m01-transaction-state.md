---
project: sqlite
feature: transaction-state
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_get_autocommit, sqlite3_txn_state, sqlite3RollbackAll]
---

# 事务状态机

## 一句话职责

SQLite 用两个连接级状态位——`db->autoCommit` 与每个 Btree 的 `inTrans`——表示「当前是否在事务中、事务处于只读还是可写」，并提供查询接口与全库回滚入口。

## 机制是什么

SQLite 的事务状态分成两层，两层含义不同，不能混为一谈：

- **自动提交标志**：`db->autoCommit` 是连接（`sqlite3`）级的一个 `u8` 字段（定义在 `src/sqliteInt.h` 的 `sqlite3` 结构里，注释为 "The auto-commit flag."）。它是**布尔语义**：`1` 表示处于自动提交模式，`0` 表示已经进入显式事务。`src/main.c` 的 `sqlite3_get_autocommit` 在持有 `db->mutex` 的前提下直接返回该字段的值；源码注释明确写着「Autocommit mode is on by default. Autocommit is disabled by a BEGIN statement and reenabled by the next COMMIT or ROLLBACK.」——即 `BEGIN` 关掉它，`COMMIT`/`ROLLBACK` 再打开它。
- **事务层级状态**：每个数据库文件（Btree）各自持有一个事务状态，取值为 `SQLITE_TXN_NONE`（0，无事务）、`SQLITE_TXN_READ`（1，只读事务）、`SQLITE_TXN_WRITE`（2，写事务），这三个常量定义在 `src/sqlite.h.in`。Btree 内部用 `TRANS_NONE`/`TRANS_READ`/`TRANS_WRITE` 表达同样的三个值，`src/btreeInt.h` 里用 `#if TRANS_NONE!=SQLITE_TXN_NONE` 这类编译期断言强制两组常量数值一致，因此 `src/btree.c` 的 `sqlite3BtreeTxnState` 可以直接把 `p->inTrans` 返回给上层。

两者可以脱钩：自动提交模式下，一条写语句执行期间该 Btree 处于 `SQLITE_TXN_WRITE`，但 `db->autoCommit` 仍是 1；反之显式 `BEGIN` 之后 `db->autoCommit` 为 0，但在真正读到任何页之前，各 Btree 仍可能是 `SQLITE_TXN_NONE`。

`sqlite3_txn_state` 把这两层合成为对外可查询的状态：`zSchema` 非空时只查该具名数据库，`zSchema` 为 NULL 时返回**所有已挂载数据库的最大事务状态**（取各个 Btree 状态的最大值，不是求和也不是任一命中）。具名查找走 `sqlite3FindDbName`，它按 `sqlite3_stricmp` 大小写不敏感匹配 `db->aDb[]` 中的库名，并特殊处理一点：即便主库被 `SQLITE_DBCONFIG_MAINDBNAME` 改名，`"main"` 仍永远可作为主库别名。

回滚全部由 `sqlite3RollbackAll` 承担，它遍历连接上所有数据库文件，把每个 Btree 回滚到事务起点，并在必要时重置模式（schema）。

本页只讲连接级的事务状态判定与回滚全部，不讲文件锁与忙等待，也不讲回滚日志与 WAL 的写入实现。

## 怎么实现的

### 1. 查询自动提交：`sqlite3_get_autocommit`

`src/main.c` 的 `sqlite3_get_autocommit` 实现极简：先在 `SQLITE_ENABLE_API_ARMOR` 编译开关下调用 `sqlite3SafetyCheckOk(db)` 做参数合法性检查，失败时触发 `SQLITE_MISUSE_BKPT` 并返回 0；随后 `sqlite3_mutex_enter(db->mutex)`、取出 `db->autoCommit`、`sqlite3_mutex_enter` 对应的 `sqlite3_mutex_leave` 释放锁、返回该值。它**不做任何状态推导**，纯粹是字段读取。

### 2. 查询事务层级：`sqlite3_txn_state`

`src/main.c` 的 `sqlite3_txn_state` 流程：

1. 同样先走 `sqlite3SafetyCheckOk(db)`，失败返回 -1（-1 不等于任何 `SQLITE_TXN_*` 值）。
2. `sqlite3_mutex_enter(db->mutex)` 之后确定要遍历的库下标区间：`zSchema` 非空时 `iDb = nDb = sqlite3FindDbName(db, zSchema)`，若 `sqlite3FindDbName` 返回 -1（库名不存在）则执行 `nDb--`，使循环不执行，最终返回初始值 -1；`zSchema` 为 NULL 时 `iDb = 0`、`nDb = db->nDb-1`，即遍历全部库。
3. 循环体对每个下标取 `db->aDb[iDb].pBt`，若该 Btree 指针非空则取 `sqlite3BtreeTxnState(pBt)`，否则按源码写法取常量 `SQLITE_TXN_NONE`；用 `if( x>iTxn ) iTxn = x;` 取最大值。累加器 `iTxn` 初值为 -1，因此没有任何可查库时返回 -1。
4. `sqlite3_mutex_leave(db->mutex)` 后返回 `iTxn`。

`sqlite3BtreeTxnState`（`src/btree.c`）的实现是 `return p ? p->inTrans : 0;`，即空指针返回 0，而 0 正是 `SQLITE_TXN_NONE`——**SQLITE_TXN_NONE 的数值 0 与「空指针返回 0」是配套设计的**。

### 3. 回滚全部：`sqlite3RollbackAll`

`src/main.c` 的 `sqlite3RollbackAll(db, tripCode)` 是本篇的核心动作，函数注释说明了 `tripCode` 的语义：当 `tripCode` 不是 `SQLITE_OK` 时，所有写游标被「trip」（熔断），后续再使用这些游标会返回 `tripCode`；读游标仍保持打开和有效，但被「保存」起来以应对表页被搬走的情况。执行顺序：

1. `assert( sqlite3_mutex_held(db->mutex) )` 要求调用方已持连接互斥锁；随后 `sqlite3BeginBenignMalloc()` 开启良性 malloc 区间。
2. `sqlite3BtreeEnterAll(db)` **一次性取得所有 b-tree 互斥锁**。源码注释解释了为什么必须先全部拿锁：若被回滚的事务修改过数据库模式，不先持有 b-tree 锁的话，共享缓存下的另一个连接可能在「数据库回滚」与「模式重置」之间插进来，导致误报数据损坏。
3. 计算 `schemaChange = (db->mDbFlags & DBFLAG_SchemaChange)!=0 && db->init.busy==0;`——只有事务确实改过模式且初始化未在忙状态时，才需要在回滚后重置模式。
4. 遍历 `i` 从 0 到 `db->nDb-1`：对每个 `db->aDb[i].pBt` 非空的库，若 `sqlite3BtreeTxnState(p)==SQLITE_TXN_WRITE` 则置 `inTrans = 1`；随后调用 `sqlite3BtreeRollback(p, tripCode, !schemaChange)`——第三个参数 `writeOnly` 取 `!schemaChange`：模式没变时只 trip 写游标，模式变了时所有游标都要 trip。
5. `sqlite3VtabRollback(db)` 让虚拟表模块回滚自己的状态。
6. `sqlite3EndBenignMalloc()` 结束良性 malloc 区间。
7. 若 `schemaChange` 为真，调用 `sqlite3ExpirePreparedStatements(db, 0)` 让已准备语句失效，再调用 `sqlite3ResetAllSchemasOfConnection(db)` 重置连接上所有模式。
8. `sqlite3BtreeLeaveAll(db)` 释放全部 b-tree 锁。
9. 清理延迟约束计数：`db->nDeferredCons = 0;`、`db->nDeferredImmCons = 0;`，并清掉 `SQLITE_DeferFKs` 与 `SQLITE_CorruptRdOnly` 两个标志位（`db->flags &= ~(u64)(SQLITE_DeferFKs|SQLITE_CorruptRdOnly)`）。
10. 回滚钩子：`if( db->xRollbackCallback && (inTrans || !db->autoCommit) ) db->xRollbackCallback(db->pRollbackArg);`——只有「确实处于写事务」或「当前不在自动提交模式」时才回调。

底层 `sqlite3BtreeRollback`（`src/btree.c`）在 `tripCode==SQLITE_OK` 时先 `saveAllCursors(pBt, 0, 0)` 保存所有游标，若保存失败则把 `writeOnly` 置 0（改为 trip 全部游标）；若 `tripCode` 非 0，则调用 `sqlite3BtreeTripAllCursors(p, tripCode, writeOnly)`。接着在 `p->inTrans==TRANS_WRITE` 时调用 `sqlite3PagerRollback(pBt->pPager)` 真正回滚，并重新取第 1 页以修正 `pPage1->aData`（回滚可能销毁该值）；之后把 `pBt->inTransaction` 从 `TRANS_WRITE` 降为 `TRANS_READ`，最后 `btreeEndTransaction(p)` 结束事务。

### 4. 状态机在哪里被驱动

事务状态的两个字段由 VDBE 层驱动，`src/vdbe.c` 与 `src/vdbeaux.c` 是实际调用方：

- **`OP_Transaction`**（`src/vdbe.c`）负责真正开启事务：它调用 `sqlite3BtreeBeginTrans(pBt, pOp->p2, &iMeta)`，`p2` 为 0 时开只读事务、非 0 时开写事务（已存在只读事务则升级为写事务）。当 `p->usesStmtJournal && pOp->p2 && (db->autoCommit==0 || db->nVdbeRead>1)` 时额外开语句级子事务：`db->nStatement++`，`p->iStatement = db->nSavepoint + db->nStatement`，并调用 `sqlite3BtreeBeginStmt(pBt, p->iStatement)`。若 `p5` 非零还会比对模式 cookie 与代次，不一致则报 `SQLITE_SCHEMA`。
- **`OP_AutoCommit`**（`src/vdbe.c`）负责翻转 `db->autoCommit`：取 `desiredAutoCommit = pOp->p1`、`iRollback = pOp->p2`。当 `desiredAutoCommit != db->autoCommit` 时：若 `iRollback` 为真（只允许 `desiredAutoCommit==1`），调用 `sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)` 后置 `db->autoCommit = 1`；否则若 `desiredAutoCommit` 且 `db->nVdbeWrite>0`，报「cannot commit transaction - SQL statements in progress」并返回 `SQLITE_BUSY`；再否则先 `sqlite3VdbeCheckFkDeferred(p)` 检查延迟外键，通过后置 `db->autoCommit = (u8)desiredAutoCommit`。若 `sqlite3VdbeHalt(p)` 返回 `SQLITE_BUSY`，则把 `p->pc` 回退到当前指令、`db->autoCommit` 还原为 `1-desiredAutoCommit` 并返回 `SQLITE_BUSY`。当 `desiredAutoCommit == db->autoCommit` 时按状态给出三种错误信息：「cannot start a transaction within a transaction」「cannot rollback - no transaction is active」「cannot commit - no transaction is active」，返回 `SQLITE_ERROR`。
- **语句结束时**（`src/vdbeaux.c`）：出错且不是 `SQLITE_NOMEM`/`SQLITE_FULL` 的语句回滚场景下调用 `sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)`，随后 `sqlite3CloseSavepoints(db)` 并置 `db->autoCommit = 1`；自动提交且无其它写者时，成功路径走 `vdbeCommit(db, p)`，提交失败则调用 `sqlite3RollbackAll(db, SQLITE_OK)`；非自动提交路径下若 `sqlite3VdbeCloseStatement(p, eStatementOp)` 失败，同样调用 `sqlite3RollbackAll(db, SQLITE_ABORT_ROLLBACK)`。
- **连接关闭时**（`src/main.c` 的 `sqlite3LeaveMutexAndCloseZombie`）：在释放资源前调用 `sqlite3RollbackAll(db, SQLITE_OK)`，注释说明这一调用同时确保未提交事务修改过的模式被重置，并保证所需的 b-tree 互斥锁被持有，使 pager 回滚与模式重置成为原子操作。

### 5. 状态被其它模块读取的位置

除公开接口外，`db->autoCommit` 还被多处逻辑直接读取：

- `src/main.c` 的 `sqlite3_snapshot_get` 与 `sqlite3_snapshot_open` 都以 `if( db->autoCommit==0 )` 作为「已显式开事务」的前提条件；`sqlite3_snapshot_get` 还要求 `sqlite3BtreeTxnState(pBt) != SQLITE_TXN_WRITE`，否则不取快照。
- `src/vdbe.c` 的 `OP_Savepoint` 分支在 `db->autoCommit` 为真时建立「事务保存点」，置 `db->autoCommit = 0` 并标记 `db->isTransactionSavepoint = 1`；保存点释放若判定为事务保存点（`pSavepoint->pNext==0 && db->isTransactionSavepoint`）则置 `db->autoCommit = 1`。
- `src/pragma.c`、`src/vacuum.c` 也直接读 `db->autoCommit` 作为「当前是否在事务中」的判据。

## 关键取舍

- **两个状态位而不是一个（源码事实）**：`db->autoCommit` 是连接级的布尔意图开关，`Btree.inTrans` 是文件级的实际事务层级。合并成一个字段就无法表达「显式 BEGIN 之后尚未触页」和「自动提交下单条写语句正在写」这两种真实存在但语义不同的状态；代价是任何读事务状态的代码都必须同时考虑两层，`sqlite3_txn_state` 才需要做「取各库最大值」的聚合。
- **`sqlite3_txn_state` 取最大值而非逐库精确值（源码事实）**：NULL 参数下返回所有库的最大状态，是一个聚合视图而非逐库明细；代价是调用方无法从这一个返回值区分出「哪个库在写」。要区分必须带 `zSchema` 逐个查。
- **先拿全部 b-tree 锁再回滚（源码事实）**：`sqlite3RollbackAll` 用 `sqlite3BtreeEnterAll(db)` 一次性获取所有 b-tree 互斥锁，源码注释指明这是为防止共享缓存下另一个连接在「数据库回滚」与「模式重置」之间插入。代价是回滚期间其他连接对该共享缓存的所有数据库文件都被阻塞，回滚延迟被放大到最大数据库数而非单个文件。
- **`writeOnly` 取 `!schemaChange`（源码事实）**：模式没变时只 trip 写游标，读游标保持打开并被保存；模式变了时全部游标都要 trip。代价是同一条回滚路径要按模式是否变更分成两种语义，游标失效行为对调用方不可预测地依赖模式变更标志。
- **回滚钩子的触发条件收紧（源码事实）**：只有 `inTrans`（存在写事务）或 `!db->autoCommit`（显式事务中）时才调用 `db->xRollbackCallback`；纯读事务的自动提交回滚不触发回调，避免把「没写过任何东西」也报成回滚事件。
- **`SQLITE_TXN_NONE` 取 0 的连带设计（源码事实）**：`sqlite3BtreeTxnState` 对空指针返回 0，而 0 就是 `SQLITE_TXN_NONE`；`sqlite3_txn_state` 对不存在的库名同样以 -1 表示「查不到」，两者靠「合法状态非负、错误为负」区分。
- **多库事务的状态聚合只在连接层做（架构推论）**：SQLite 的多库事务不是两阶段提交，而是「主库 + 附加库各自独立持有事务状态，连接层统一回滚」；因此 `sqlite3RollbackAll` 必须遍历全部 `db->aDb[]` 而不是只回滚主库。这是把跨库原子性交给用户用 `ATTACH` 顺序约定去保证的设计，而不是引擎强制的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/main.c` | `sqlite3_get_autocommit` | 加锁读取 `db->autoCommit`，返回自动提交模式标志 |
| `src/main.c` | `sqlite3_txn_state` | 遍历目标库或全部库的 Btree 事务状态并取最大值 |
| `src/main.c` | `sqlite3RollbackAll` | 取全部 b-tree 锁后逐库回滚、重置模式、清延迟约束计数、触发回滚钩子 |
| `src/main.c` | `sqlite3LeaveMutexAndCloseZombie` | 关闭连接时调用 `sqlite3RollbackAll(db, SQLITE_OK)` 保证未提交事务被回滚 |
| `src/main.c` | `sqlite3_snapshot_get` | 以 `db->autoCommit==0` 与 `SQLITE_TXN_WRITE` 判定为前提决定是否取快照 |
| `src/main.c` | `sqlite3_snapshot_open` | 同样以 `db->autoCommit==0` 为前提打开指定快照 |
| `src/btree.c` | `sqlite3BtreeTxnState` | 返回 Btree 的 `inTrans`（空指针返回 0，即 `SQLITE_TXN_NONE`） |
| `src/btree.c` | `sqlite3BtreeRollback` | 保存或 trip 游标、调用 pager 回滚、把 `inTransaction` 从写降为读 |
| `src/btree.c` | `sqlite3BtreeBeginTrans` | 由 `OP_Transaction` 调用，开启只读或写事务 |
| `src/btree.c` | `sqlite3BtreeBeginStmt` | 开启语句级子事务（匿名保存点） |
| `src/btreeInt.h` | `TRANS_NONE` / `TRANS_READ` / `TRANS_WRITE` | 与 `SQLITE_TXN_*` 数值一致的编译期断言常量 |
| `src/sqlite.h.in` | `SQLITE_TXN_NONE` / `SQLITE_TXN_READ` / `SQLITE_TXN_WRITE` | 对外公开的三个事务状态取值 |
| `src/sqliteInt.h` | `sqlite3.autoCommit` | 连接级自动提交标志字段 |
| `src/vdbe.c` | `OP_Transaction` | 驱动 Btree 事务开启与语句级子事务 |
| `src/vdbe.c` | `OP_AutoCommit` | 翻转 `db->autoCommit`，处理回滚、忙与嵌套事务错误 |
| `src/vdbeaux.c` | `sqlite3VdbeHalt` 路径中的回滚分支 | 语句失败时调用 `sqlite3RollbackAll` 并重置 `db->autoCommit` |
| `src/build.c` | `sqlite3FindDbName` | 按库名查出 `db->aDb[]` 下标，`"main"` 恒为主库别名 |

## 与主核 / AI 的关系

**架构推论：** 主核的事务状态若也采用「连接级意图标志 + 文件级实际层级」的双层结构，则 SQLite 的两个字段可直接对应到主核的「自动提交标志」与「每文件事务层级」；但主核若有独立的 WAL 或多版本快照体系，`SQLITE_TXN_WRITE` 这一「写事务」判定在主核里未必等价于「已持有写锁」，不能由本页直接推定。

对本项目而言，本页给出的可复用点是**判定口径**而非代码：`sqlite3_txn_state` 的「取各库最大值」与 `sqlite3RollbackAll` 的「先取全部锁再统一回滚」都是可以在主核的事务管理器里对照的设计；主核若有类似的全库回滚入口，也应按同样口径处理「模式已变更」这一分支。

对 AI 协作而言，本页提供的是**读源码得到的断言**：`db->autoCommit` 为 0 表示处于显式事务中，`sqlite3_txn_state` 返回 `SQLITE_TXN_WRITE` 表示至少有一个数据库文件处于写事务。这些结论在评审时都应回到 `src/main.c`、`src/vdbe.c` 的具体判断分支核对，而不是凭 SQLite 的通用印象推断。

## 参考

- [SQLite 官方文档：C API 的事务状态接口](https://sqlite.org/c3ref/txn_state.html) —— 说明 `sqlite3_txn_state()` 的返回值语义与 `SQLITE_TXN_NONE`/`SQLITE_TXN_READ`/`SQLITE_TXN_WRITE` 三个状态的含义。
- [SQLite 官方文档：`sqlite3_get_autocommit()`](https://sqlite.org/c3ref/get_autocommit.html) —— 说明自动提交模式的查询接口与 BEGIN/COMMIT 对该标志的影响。
- [SQLite 官方源码：`src/main.c`](https://github.com/sqlite/sqlite/blob/master/src/main.c) —— 支撑 `sqlite3_get_autocommit`、`sqlite3_txn_state`、`sqlite3RollbackAll` 的实现细节与调用顺序。
- [SQLite 官方源码：`src/vdbe.c`](https://github.com/sqlite/sqlite/blob/master/src/vdbe.c) —— 支撑 `OP_Transaction` 与 `OP_AutoCommit` 对事务状态的驱动与错误分支。
- [SQLite 官方文档：事务](https://sqlite.org/lang_transaction.html) —— 提供 BEGIN/COMMIT/ROLLBACK 语义背景，机制实现以源码为准。
