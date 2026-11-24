---
project: sqlite
feature: db-close
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_close, sqlite3_close_v2, sqlite3LeaveMutexAndCloseZombie]
---

# 连接关闭与僵尸连接

## 一句话职责

`sqlite3_close()` / `sqlite3_close_v2()` 是 `sqlite3` 连接的析构器：前者遇到未 finalize 的语句或未完成的 backup 就返回 `SQLITE_BUSY` 并保持连接打开；后者无条件返回 `SQLITE_OK`，把连接转成 `SQLITE_STATE_ZOMBIE` 状态，等最后一个语句或 backup 释放后再由 `sqlite3LeaveMutexAndCloseZombie()` 真正回收资源。

## 机制是什么

**「关闭」在 SQLite 里被拆成两段：一次逻辑关闭 + 一次物理回收。** 源码把真正的清理逻辑集中在 `sqlite3LeaveMutexAndCloseZombie()` 一个函数里，`sqlite3_close()` 与 `sqlite3_close_v2()` 只是以不同的 `forceZombie` 参数调用同一个 `sqlite3Close()` 静态辅助函数：

- `sqlite3_close(db)` → `sqlite3Close(db, 0)`，即 `forceZombie = 0`；
- `sqlite3_close_v2(db)` → `sqlite3Close(db, 1)`，即 `forceZombie = 1`。

**两种 API 的差别只体现在 `sqlite3Close()` 里的一个判断上**：`if( !forceZombie && connectionIsBusy(db) )`。`connectionIsBusy(db)` 的判定条件有两个（`db->pVdbe` 非空表示还有活着的 VDBE 语句；遍历 `db->aDb[]` 中每个 `Btree` 调 `sqlite3BtreeIsInBackup(pBt)`，该函数返回 `p->nBackup!=0`，表示该 Btree 上还有 backup 在跑）。命中且 `forceZombie` 为 0 时，`sqlite3Close()` 调 `sqlite3ErrorWithMsg(db, SQLITE_BUSY, "unable to close due to unfinalized statements or unfinished backups")`，释放互斥量后返回 `SQLITE_BUSY`，**连接完全不动**——不设僵尸状态，不释放任何资源。`forceZombie` 为 1 时这个判断整体被跳过。

**僵尸（zombie）是一个显式的连接状态。** `src/sqliteInt.h` 里定义了六个状态常量：`SQLITE_STATE_OPEN`（0x76）、`SQLITE_STATE_CLOSED`（0xce）、`SQLITE_STATE_SICK`（0xba）、`SQLITE_STATE_BUSY`（0x6d）、`SQLITE_STATE_ERROR`（0xd5）、`SQLITE_STATE_ZOMBIE`（0xa7）。源码注释写明这些数值是随机选的，目标是任意两个状态之间至少要改三位二进制位。`sqlite3Close()` 在准备把连接置为僵尸时执行 `db->eOpenState = SQLITE_STATE_ZOMBIE;`，然后调 `sqlite3LeaveMutexAndCloseZombie(db)`。

**僵尸状态下连接不再可用，但仍然占着资源。** `sqlite3LeaveMutexAndCloseZombie()` 的第一段就是一个提前返回：

```
if( db->eOpenState!=SQLITE_STATE_ZOMBIE || connectionIsBusy(db) ){
  sqlite3_mutex_leave(db->mutex);
  return;
}
```

也就是说：如果连接不是僵尸（还没调过 close），或者还有语句/backup 没释放，这个函数只做「释放互斥量」这一件事就返回，什么都不清理。真正的清理要等到两个条件同时满足：连接已被标记为 `SQLITE_STATE_ZOMBIE`，且 `connectionIsBusy(db)` 返回 0。

**触发第二次检查的调用方有三个，都是「最后一个持有者释放资源」的自然时机：**

1. `src/vdbeapi.c` 的 `sqlite3_finalize()`：函数体末尾调 `sqlite3VdbeDelete(v)` 之后调 `sqlite3LeaveMutexAndCloseZombie(db)`。`sqlite3VdbeDelete()` 内部把该 VDBE 从 `db->pVdbe` 链表上摘除（`*p->ppVPrev = p->pVNext`），所以最后一个语句 finalize 之后 `db->pVdbe` 就变回 NULL，`connectionIsBusy()` 的第一个条件不再成立。
2. `src/backup.c` 的 `sqlite3_backup_finish()`：函数体里对目标库调 `sqlite3LeaveMutexAndCloseZombie(p->pDestDb)`，对源库调 `sqlite3LeaveMutexAndCloseZombie(pSrcDb)`。
3. `src/main.c` 的 `sqlite3Close()` 自身——它先把状态置为 `SQLITE_STATE_ZOMBIE` 再调，所以当连接本来就没有未释放资源时，这条路径会一路走到完整清理。

**连接一旦进入僵尸态，普通 API 会拒绝它。** `src/util.c` 的 `sqlite3SafetyCheckOk()` 判定 `eOpenState != SQLITE_STATE_OPEN` 时返回 0（除非 `sqlite3SafetyCheckSickOrOk()` 认可）；而 `sqlite3SafetyCheckSickOrOk()` 只接受 `SQLITE_STATE_SICK`、`SQLITE_STATE_OPEN`、`SQLITE_STATE_BUSY` 三种状态——`SQLITE_STATE_ZOMBIE` 不在其中，所以对僵尸连接调用普通 API 会命中 `SQLITE_MISUSE_BKPT`。`sqlite3_interrupt()` 和 `sqlite3_is_interrupted()` 是显式例外：它们的 `SQLITE_ENABLE_API_ARMOR` 分支写的是 `!sqlite3SafetyCheckOk(db) && (db==0 || db->eOpenState!=SQLITE_STATE_ZOMBIE)`，即僵尸连接被这两者放行。

## 怎么实现的

### `sqlite3Close()` 的执行顺序

`src/main.c` 的静态函数 `sqlite3Close(sqlite3 *db, int forceZombie)` 按下面的顺序执行：

1. `if( !db ) return SQLITE_OK;` —— 传 NULL 指针是无害的 no-op，源码里标注了对应的需求编号 `R-63257-11740`。
2. `if( !sqlite3SafetyCheckSickOrOk(db) ) return SQLITE_MISUSE_BKPT;` —— 状态校验放在最前面，此时还没进互斥量。
3. `SQLITE_DISPOSAL_NOTIFIER(1,db);` —— 只在定义了 `SQLITE_DISPOSAL_NOTIFIER` 时编译，是给测试用的析构通知钩子。
4. `sqlite3_mutex_enter(db->mutex);` —— 从这里开始临界区覆盖整个关闭过程。
5. 若 `db->mTrace & SQLITE_TRACE_CLOSE`，调 `db->trace.xV2(SQLITE_TRACE_CLOSE, db->pTraceArg, db, 0)` 发一条 trace 回调。
6. `disconnectAllVtab(db);` —— 强制对所有虚拟表调 `xDisconnect`。该函数遍历 `db->aDb[]` 每个 Schema 的 `tblHash`，对 `IsVirtual(pTab)` 为真的表调 `sqlite3VtabDisconnect(db, pTab)`；再遍历 `db->aModule` 哈希，对每个带 `pEpoTab` 的模块调 `sqlite3VtabDisconnect(db, pMod->pEpoTab)`；最后调 `sqlite3VtabUnlockList(db)` 并 `sqlite3BtreeLeaveAll(db)`。整段包在 `#ifndef SQLITE_OMIT_VIRTUALTABLE` 里。
7. `sqlite3VtabRollback(db);` —— 对 `db->aVTrans[]` 里的虚拟表调 `xRollback`。源码注释解释了为什么这一步必须在第 8 步的语句检查之前：虚拟表实现可能在内部持有 prepared statement，不先回滚就会让后面的检查误判。
8. `if( !forceZombie && connectionIsBusy(db) )` —— 上面说过的那一段，返回 `SQLITE_BUSY` 时**在此处终止**，连接状态未变。
9. `sqlite3GlobalConfig.xSqllog(...)` —— 只在定义了 `SQLITE_ENABLE_SQLLOG` 时编译，通知 SQL 日志钩子「连接句柄正在关闭」。
10. 遍历 `db->pDbData` 链表：对每个 `DbClientData` 调 `p->xDestructor(p->pData)`，然后 `sqlite3_free(p)`。
11. `db->eOpenState = SQLITE_STATE_ZOMBIE;`
12. `sqlite3LeaveMutexAndCloseZombie(db);`
13. `return SQLITE_OK;`

### `sqlite3LeaveMutexAndCloseZombie()` 的清理顺序

这个函数体在通过「是僵尸且不忙」检查后，按下面的顺序释放资源：

1. `sqlite3RollbackAll(db, SQLITE_OK);` —— 回滚所有数据库文件的事务。源码注释说明这一步同时也把未提交事务改动过的模式复位，并且保证 pager 回滚与模式复位是一个原子操作。
2. `sqlite3CloseSavepoints(db);` —— 释放所有未关闭的 `Savepoint` 结构，并把 `db->nSavepoint`、`db->nStatement`、`db->isTransactionSavepoint` 全部清零。
3. 遍历 `db->nDb` 关闭所有 Btree：对每个非空的 `pDb->pBt` 调 `sqlite3BtreeClose(pDb->pBt)` 并把 `pDb->pBt` 置 0；`j != 1` 时把 `pDb->pSchema` 也置 0（下标 0 是 main、下标 1 是 temp，temp 的 schema 要单独处理）。
4. `sqlite3SchemaClear(db->aDb[1].pSchema);` —— 单独清 TEMP schema，源码注释说明 temp schema 的分配方式与其他 schema 不同（用 `sqliteMalloc()` 而不是 `sqlite3BtreeSchema()`），所以要在这里单独释放。
5. `sqlite3VtabUnlockList(db);`
6. `sqlite3CollapseDatabaseArray(db);` —— 压缩附加数据库数组。该函数遍历下标 2 起的 `db->aDb[]`，把 `pBt == 0` 的已关闭项移除；若压缩后 `db->nDb <= 2` 且 `db->aDb` 不是静态数组，就把内容拷回 `db->aDbStatic` 并释放堆上的数组。
7. `sqlite3ConnectionClosed(db);` —— 通知 `src/notify.c` 该连接不再持有锁，不再需要 unlock-notify 回调。
8. 遍历 `db->aFunc` 哈希，对每个 `FuncDef` 调 `functionDestroy(db, p)`（它会在析构函数引用计数 `nRef` 归零时调 `xDestroy`），然后 `sqlite3HashClear(&db->aFunc)`。
9. 遍历 `db->aCollSeq` 哈希：对每个 `CollSeq` 的三个元素分别检查 `xDel` 并调用它，然后 `sqlite3DbFree(db, pColl)`；最后 `sqlite3HashClear(&db->aCollSeq)`。
10. 遍历 `db->aModule` 哈希（包在 `#ifndef SQLITE_OMIT_VIRTUALTABLE` 里）：调 `sqlite3VtabEponymousTableClear(db, pMod)` 与 `sqlite3VtabModuleUnref(db, pMod)`，然后 `sqlite3HashClear(&db->aModule)`。
11. `sqlite3Error(db, SQLITE_OK);` —— 释放缓存的错误字符串；`sqlite3ValueFree(db->pErr);`。
12. `sqlite3CloseExtensions(db);` —— 卸载所有已加载扩展。该函数遍历 `db->nExtension` 个元素调 `sqlite3OsDlClose(db->pVfs, db->aExtension[i])`，然后 `sqlite3DbFree(db, db->aExtension)`。它在 `src/loadext.c` 中实现，被 `SQLITE_OMIT_LOAD_EXTENSION` 裁掉时，声明在 `src/sqliteInt.h` 里退化为空宏 `# define sqlite3CloseExtensions(X)`。
13. `db->eOpenState = SQLITE_STATE_ERROR;`
14. `sqlite3DbFree(db, db->aDb[1].pSchema);` —— 释放 temp schema 结构体本身。
15. `if( db->xAutovacDestr ) db->xAutovacDestr(db->pAutovacPagesArg);`
16. `sqlite3_mutex_leave(db->mutex);`
17. `db->eOpenState = SQLITE_STATE_CLOSED;` —— 注意这一句在互斥量释放**之后**，因为它写的是已释放互斥量保护的字段，但此时连接已独占，不再有并发访问者。
18. `sqlite3_mutex_free(db->mutex);`
19. `assert( sqlite3LookasideUsed(db,0)==0 );` —— 断言 lookaside 缓冲区已全部归还。
20. `if( db->lookaside.bMalloced ) sqlite3_free(db->lookaside.pStart);` —— 若 lookaside 内存是堆上分配的则释放。
21. `sqlite3_free(db);` —— 释放连接对象本身。

### `sqlite3RollbackAll()` 的回滚细节

`src/main.c` 的 `sqlite3RollbackAll(sqlite3 *db, int tripCode)` 是关闭路径上的事务回滚实现：

- 先 `sqlite3BtreeEnterAll(db);` 一次性拿住所有 Btree 互斥量。源码注释说明这是为了防止共享缓存模式下另一个连接在「数据库回滚」与「模式复位」之间插进来，导致误报的损坏错误。
- `schemaChange = (db->mDbFlags & DBFLAG_SchemaChange)!=0 && db->init.busy==0;`
- 遍历 `db->aDb[]`：若 `sqlite3BtreeTxnState(p) == SQLITE_TXN_WRITE` 则置 `inTrans = 1`，然后调 `sqlite3BtreeRollback(p, tripCode, !schemaChange)`。
- `sqlite3VtabRollback(db);` —— 对虚拟表调 `xRollback`。
- 若 `schemaChange` 为真：调 `sqlite3ExpirePreparedStatements(db, 0)` 让已编译语句失效，再调 `sqlite3ResetAllSchemasOfConnection(db)` 复位模式。
- `sqlite3BtreeLeaveAll(db);`
- 清理延迟约束计数：`db->nDeferredCons = 0; db->nDeferredImmCons = 0;`，并清掉 `db->flags` 上的 `SQLITE_DeferFKs` 与 `SQLITE_CorruptRdOnly`。
- 若 `db->xRollbackCallback` 非空，且（`inTrans` 为真或 `db->autoCommit` 为假），则调 `db->xRollbackCallback(db->pRollbackArg)`。

### 关闭时的错误分支汇总

| 分支 | 触发条件 | 返回值 | 连接状态 |
| --- | --- | --- | --- |
| 空指针 | `db == 0` | `SQLITE_OK` | 无 |
| 状态非法 | `sqlite3SafetyCheckSickOrOk(db)` 返回 0 | `SQLITE_MISUSE_BKPT` | 无 |
| 资源未释放 | `!forceZombie && connectionIsBusy(db)` | `SQLITE_BUSY` | 保持 `SQLITE_STATE_OPEN` |
| 正常关闭 | 以上都不命中 | `SQLITE_OK` | 先 `SQLITE_STATE_ZOMBIE`，清理完成后 `SQLITE_STATE_CLOSED` |

## 关键取舍

**取舍一：报错 vs. 延迟回收。** `sqlite3_close()` 选择「发现问题就拒绝」——返回 `SQLITE_BUSY` 且连接原封不动，调用方必须自己追踪并释放所有语句。`sqlite3_close_v2()` 选择「先答应下来，之后再说」——`sqlite3Close()` 里的 `!forceZombie` 短路让 `SQLITE_BUSY` 分支在 v2 路径上永远不执行，函数无条件走到 `return SQLITE_OK`。官方文档说明 v2 的设计意图是给带垃圾回收的宿主语言用：析构函数调用顺序不确定，强制要求「先关连接再 finalize 语句」是不现实的。代价是资源释放时机变成隐式的——资源什么时候真正回收，取决于最后一个语句或 backup 何时释放。

**取舍二：把清理逻辑收敛到一个函数。** 完整清理只在 `sqlite3LeaveMutexAndCloseZombie()` 里写了一遍，`sqlite3Close()` 和 `sqlite3_finalize()` / `sqlite3_backup_finish()` 共用。这样做的直接好处是「僵尸连接的最后一次释放」和「普通连接的关闭」走完全相同的资源释放序列，不会出现两条路径漏掉某个资源类型。代价是这个函数同时承担了「判定僵尸并提前返回」和「完整清理」两种职责，函数体较长。

**取舍三：先解虚拟表，再查语句是否忙。** `disconnectAllVtab()` 和 `sqlite3VtabRollback()` 都被放在 `connectionIsBusy()` 检查之前。源码注释给出的理由是虚拟表实现可能在内部保存 prepared statement，若先做忙碌检查会因为这些内部语句而误判连接无法关闭。反过来，若 `sqlite3_close()` 返回 `SQLITE_BUSY`，此时虚拟表已经被 `xDisconnect` 过了——这是这个顺序带来的副作用。

**取舍四：状态常量用随机数值而非枚举序。** `src/sqliteInt.h` 的注释写明这些值是随机选的，目标是任意两状态之间至少改三位二进制位，降低状态误判导致的内存破坏风险。

**取舍五：错误信息走 `sqlite3ErrorWithMsg` 而非裸返回码。** `SQLITE_BUSY` 分支不只返回码，还调 `sqlite3ErrorWithMsg(db, SQLITE_BUSY, "unable to close due to unfinalized statements or unfinished backups")`，让调用方能通过 `sqlite3_errmsg()` 拿到可读原因。

**架构推论：** `sqlite3Close()` 中 `SQLITE_DISPOSAL_NOTIFIER` 被编译成两个带数字的参数（连接关闭传 1，语句 finalize 传 2），说明这个通知器的设计意图是让测试框架能观察析构事件发生的顺序，从而验证「先 finalize 语句、后关闭连接」这条约定在实现层确实被遵守。这是从宏的调用形态推断的，宏本身定义在 SQLite 源码树之外。

## 源码落点

| 路径 | 符号 | 作用 |
| --- | --- | --- |
| src/main.c | `sqlite3_close` | 公开析构器，转调 `sqlite3Close(db, 0)`（`forceZombie = 0`），资源未释放时返回 `SQLITE_BUSY` |
| src/main.c | `sqlite3_close_v2` | 公开析构器，转调 `sqlite3Close(db, 1)`（`forceZombie = 1`），无条件返回 `SQLITE_OK` |
| src/main.c | `sqlite3LeaveMutexAndCloseZombie` | 僵尸判定与完整资源回收：回滚事务、清 savepoint、关 Btree、清模式、卸载扩展、释放互斥量与连接对象 |
| src/main.c | `sqlite3Close` | 关闭流程的编排者：状态校验 → 解虚拟表 → 忙碌判定 → 置 `SQLITE_STATE_ZOMBIE` → 调 `sqlite3LeaveMutexAndCloseZombie` |
| src/main.c | `connectionIsBusy` | 忙碌判定：`db->pVdbe` 非空，或任一 Btree 的 `sqlite3BtreeIsInBackup` 为真 |
| src/main.c | `disconnectAllVtab` | 对所有虚拟表调 `xDisconnect`，并调 `sqlite3VtabUnlockList` |
| src/main.c | `sqlite3RollbackAll` | 关闭路径上的事务回滚，含模式复位、延迟约束清理与 rollback 回调 |
| src/main.c | `sqlite3CloseSavepoints` | 释放未关闭的 `Savepoint` 结构，清零 `nSavepoint` / `nStatement` / `isTransactionSavepoint` |
| src/main.c | `functionDestroy` | 释放 `FuncDef`，在其析构函数引用计数 `nRef` 归零时调 `xDestroy` |
| src/vdbeapi.c | `sqlite3_finalize` | 语句释放入口；`sqlite3VdbeDelete(v)` 后调 `sqlite3LeaveMutexAndCloseZombie(db)`，是僵尸连接被回收的主要触发点 |
| src/backup.c | `sqlite3_backup_finish` | backup 释放入口；对目标库与源库分别调 `sqlite3LeaveMutexAndCloseZombie` |
| src/loadext.c | `sqlite3CloseExtensions` | 遍历 `db->nExtension` 调 `sqlite3OsDlClose` 卸载扩展，然后释放 `db->aExtension` |
| src/build.c | `sqlite3CollapseDatabaseArray` | 压缩附加数据库数组；`db->nDb <= 2` 时把内容拷回静态数组并释放堆数组 |
| src/util.c | `sqlite3SafetyCheckOk` | 只接受 `SQLITE_STATE_OPEN`，否则返回 0 |
| src/util.c | `sqlite3SafetyCheckSickOrOk` | 接受 `SQLITE_STATE_SICK` / `SQLITE_STATE_OPEN` / `SQLITE_STATE_BUSY`，不包含 `SQLITE_STATE_ZOMBIE` |
| src/btree.c | `sqlite3BtreeIsInBackup` | 返回 `p->nBackup != 0`，即该 Btree 上是否还有 backup 在运行 |
| src/vtab.c | `sqlite3VtabRollback` | 对 `db->aVTrans[]` 中的虚拟表调 `xRollback` 后清空数组 |
| src/notify.c | `sqlite3ConnectionClosed` | 通知 unlock-notify 子系统该连接已关闭，从阻塞列表中移除 |
| src/vdbeaux.c | `sqlite3VdbeCreate` | 新建 VDBE 时挂到 `db->pVdbe` 链表头，是「有未释放语句」这一状态的来源 |
| src/vdbeaux.c | `sqlite3VdbeDelete` | 从 `db->pVdbe` 链表上摘除该 VDBE，使其不再被 `connectionIsBusy` 计入 |
| src/sqliteInt.h | `SQLITE_STATE_ZOMBIE` 等状态常量 | 定义 `SQLITE_STATE_OPEN`(0x76) / `CLOSED`(0xce) / `SICK`(0xba) / `BUSY`(0x6d) / `ERROR`(0xd5) / `ZOMBIE`(0xa7) |
| src/sqlite.h.in | `sqlite3_close` / `sqlite3_close_v2` 声明 | 公开 API 契约文档：说明 NULL 参数无害、事务会被自动回滚、v2 面向垃圾回收宿主语言 |

## 与主核 / AI 的关系

**架构推论：** 主核的嵌入式 SDK 依赖 SQLite 作为 SQL 引擎适配层，若 SDK 把 SQLite 连接放在一个被 GC 管理的长生命周期对象里，析构顺序就不可控——这正是 `sqlite3_close_v2()` 存在的理由。同理，主核在关闭连接时应优先用 `sqlite3_close_v2()`，再用 `sqlite3_finalize()` 清理语句，避免因为「语句还没 finalize」而在 `SQLITE_BUSY` 分支上拿不到关闭。

**架构推论：** 僵尸连接是一个「已关闭但未回收」的中间态，对 AI 辅助调试有诊断价值：出现 `SQLITE_MISUSE` 且连接句柄疑似悬空时，`db->eOpenState` 的取值可直接区分「连接已完全关闭」（`SQLITE_STATE_CLOSED`）与「连接已标记关闭但仍有语句未释放」（`SQLITE_STATE_ZOMBIE`）。这两条都是从 `src/main.c` 的状态机设计推出的可用观察点，源码没有把它们暴露成公开 API。

## 参考

- [SQLite 官方文档：Closing A Database Connection](https://sqlite.org/c3ref/close.html) —— `sqlite3_close()` / `sqlite3_close_v2()` 的公开契约，说明 NULL 参数无害、事务自动回滚、v2 的僵尸语义。
- [SQLite 官方文档：sqlite3_close / sqlite3_close_v2 源码注释](https://github.com/sqlite/sqlite/blob/master/src/sqlite.h.in) —— `src/sqlite.h.in` 中 `CAPI3REF: Closing A Database Connection` 一节的原文。
- [GitHub 源码页：src/main.c](https://github.com/sqlite/sqlite/blob/master/src/main.c) —— `sqlite3Close`、`sqlite3_close`、`sqlite3_close_v2`、`sqlite3LeaveMutexAndCloseZombie` 的实现。
- [GitHub 源码页：src/vdbeapi.c](https://github.com/sqlite/sqlite/blob/master/src/vdbeapi.c) —— `sqlite3_finalize()` 末尾触发僵尸回收的调用点。
- [GitHub 源码页：src/backup.c](https://github.com/sqlite/sqlite/blob/master/src/backup.c) —— `sqlite3_backup_finish()` 中对源库与目标库调用 `sqlite3LeaveMutexAndCloseZombie` 的位置。
- [SQLite 官方文档：sqlite3_finalize](https://sqlite.org/c3ref/finalize.html) —— 语句释放接口，与关闭路径的耦合点。
- [SQLite 官方文档：sqlite3_backup_finish](https://sqlite.org/c3ref/backup_finish.html) —— backup 释放接口。
