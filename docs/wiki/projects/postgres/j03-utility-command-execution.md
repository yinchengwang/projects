---
project: postgres
feature: utility-command-execution
page_type: feature
schema_version: 1
refs:
  - path: src/backend/tcop/utility.c
    symbols: [ProcessUtility, standard_ProcessUtility]
  - path: src/backend/tcop/pquery.c
    symbols: [PortalRun, CreateQueryDesc]
---

# utility 命令执行

## 一句话职责

utility 命令执行负责把“不是普通 SELECT/DML”的 SQL 语句（事务控制、DDL、维护命令、SET 等）在正确的上下文与事务边界里分派到具体实现，并让 portal 层既能跑执行器查询也能跑这类命令。

## 机制是什么

`ProcessUtility` 是一个薄分派层：所有 utility 语句的 `PlannedStmt` 都带着 `commandType = CMD_UTILITY` 和 `utilityStmt` 子节点，调用方（simple 协议的 `exec_simple_query` 或 extended 协议的 portal 执行）只负责把上下文信息（`ProcessUtilityContext`、参数、`QueryEnvironment`、输出 `DestReceiver`、`QueryCompletion`）传下来。该层唯一的结构性工作是提供 `ProcessUtility_hook` 挂点，让扩展能接管或包住整个分派，然后默认落到 `standard_ProcessUtility`。

`standard_ProcessUtility` 再做第二次划分：它能处理不需要 event trigger 基础设的语句（事务控制、游标、PREPARE/EXECUTE/DEALLOCATE、SET/RESET、COPY、LOCK、LISTEN/NOTIFY、CHECKPOINT 等），而把有 event trigger 支持的语句整体交给 `ProcessUtilitySlow`。这个划分不只是性能考虑：源码指出必须避免在处理 `START TRANSACTION` 时触发 event trigger 代码，因为刷新 event trigger cache 需要处于有效事务中。

事务边界的协调分两层实现：会话层（`exec_simple_query`）在每条语句前后 `start_xact_command`/`finish_xact_command` 并按语句类型决定是否提交；portal 层（`PortalRun`）则在 `PG_TRY` 里临时替换 `ActivePortal`、`CurrentResourceOwner`、`PortalContext`，并专门处理“utility 命令内部自己开关事务”（如 VACUUM/CLUSTER）会把 `TopTransactionResourceOwner` 换掉的情况。本页只讲这条分派与边界机制，不讲各 DDL 命令的实现（对象 DDL 页）、不讲 COPY 的具体读写循环、也不讲执行器内部的计划执行。

## 怎么实现的

1. `ProcessUtility` 先断言 `pstmt` 是 `PlannedStmt`、`commandType` 为 `CMD_UTILITY`、`queryString` 非空、`qc` 尚未填充；有 `ProcessUtility_hook` 时交给它（插件通常自行回调 `standard_ProcessUtility`），否则直接调用。源码特别提示：一条含多个分号语句的查询串会让同一个 `queryString` 被多次传入，调用者要用 `pstmt->stmt_location`/`stmt_len` 定位当前子串；某些 utility 语句（如 `CREATE SCHEMA`）还会递归调用本函数处理子语句。
2. `standard_ProcessUtility` 先计算 `isTopLevel`（上下文为 `PROCESS_UTILITY_TOPLEVEL`）与 `isAtomicContext`（顶层或非原子查询之外，或已在事务块内）。`readOnlyTree` 为真时先 `copyObject(pstmt)`，防止 parse 变换破坏调用方持有的（可能来自 plancache 的）树。
3. 再用 `ClassifyUtilityCommandAsReadOnly` 得到只读属性标志，若非“严格只读”且当前 `XactReadOnly` 或处于并行模式，则按位检查：不满足 `COMMAND_OK_IN_READ_ONLY_TXN` 就 `PreventCommandIfReadOnly`；不满足 `COMMAND_OK_IN_PARALLEL_MODE` 就 `PreventCommandIfParallelMode`；不满足 `COMMAND_OK_IN_RECOVERY` 就 `PreventCommandDuringRecovery`。这样同一张标志表同时约束只读事务、并行 worker 与 recovery 三种禁止环境。
4. 建立 `ParseState`（`p_sourcetext` 设成 `queryString`、`p_queryEnv` 设成调用方环境）后按 `nodeTag(parsetree)` 大分派。事务控制分支直接调 `BeginTransactionBlock`/`EndTransactionBlock`/`RollbackTransactionBlock` 等，并处理 `COMMIT` 返回假时把“未成功提交”写进 `qc`；游标、`PREPARE`、`EXECUTE`、`DEALLOCATE`、`SET`/`RESET`、`COPY`、`LOCK` 等各自调用对应实现。
5. 一批语句（`CommentStmt`、`SecLabelStmt` 等）会按 `EventTriggerSupportsObjectType` 判断：支持 event trigger 就转 `ProcessUtilitySlow`，否则直接调简单实现。`T_WaitStmt` 走 `ExecWaitStmt` 并显式传入 `isTopLevel`。其余默认分支统一交给 `ProcessUtilitySlow`。
6. `standard_ProcessUtility` 结束时 `free_parsestate(pstate)` 并 `CommandCounterIncrement()`，使命令效果对后续步骤（如 `PreCommit_on_commit_actions`）可见；源码以 bug #15631 为例说明这一步的必要性。
7. `ProcessUtilitySlow` 只接收有 event trigger 支持的语句：`isCompleteQuery`（非 `PROCESS_UTILITY_SUBCOMMAND`）为真时 `EventTriggerBeginCompleteQuery()` 并在 `PG_TRY` 中先发 `EventTriggerDDLCommandStart(parsetree)`，退出时保证 `EventTriggerEndCompleteQuery()` 被调用。
8. 大分派内部按语句类型调用 DDL 主流程：`CreateSchemaCommand`、`DefineRelation`（`CREATE TABLE`/`CREATE FOREIGN TABLE`）、`TransformCreateStmt` 展开出的多语句逐条执行并在子语句之间 `CommandCounterIncrement`，`LIKE` 子句通过 `expandTableLikeClause` 插入更多子语句到 `stmts` 头部；无法直接处理的子语句用 `makeNode(PlannedStmt)` 包一层并递归 `ProcessUtility(..., PROCESS_UTILITY_SUBCOMMAND, ..., None_Receiver, NULL)`。
9. 每个成功执行的命令对象交给 `EventTriggerCollectSimpleCommand`（或由被调实现内部收集），以便 `EventTriggerDDLCommandEnd` 阶段拿到完整对象列表；`commandCollected` 用于标记已由被调函数自行收集、避免重复。注意递归子命令走 `canSetTag = false`。
10. portal 侧：`PortalRun` 先 `MarkPortalActive`，保存并替换 `ActivePortal`、`CurrentResourceOwner`、`PortalContext`、`CurrentMemoryContext` 等全局指针，`PG_TRY` 内按 `portal->strategy` 分派：`PORTAL_ONE_SELECT`/`PORTAL_ONE_RETURNING`/`PORTAL_ONE_MOD_WITH`/`PORTAL_UTIL_SELECT` 走 `FillPortalStore`（非 ONE_SELECT 时）再 `PortalRunSelect`；`PORTAL_MULTI_QUERY` 走 `PortalRunMulti` 并 `MarkPortalDone`。`PG_CATCH` 里 `MarkPortalFailed` 并恢复全局指针后重抛。
11. `PortalRun` 保存/恢复逻辑有两处特殊处理，源码明确点出原因：VACUUM、CLUSTER 这类 utility 命令会**内部启动并提交事务**，导致 `TopTransactionResourceOwner`/`TopTransactionContext` 被销毁并替换，所以恢复时必须比较“保存值是否等于当时的顶层对象”，相等就改指向新的 `TopTransactionResourceOwner`/`TopTransactionContext`。
12. `PortalRunMulti` 遍历 `portal->stmts`：可计划语句需要快照时首次 `GetTransactionSnapshot()` 并 `PushCopiedSnapshot`（若 `setHoldSnapshot` 则另注册一份存 `portal->holdSnapshot`，因为同一份不能既是活跃快照又是 hold 快照），后续语句只 `UpdateActiveSnapshotCommandId()`；`canSetTag` 为真用主 `dest` 与 `qc`，假则用 `altdest` 且不写 tag。utility 语句不在此设快照，交给 `PortalRunUtility` 按 `PlannedStmtRequiresSnapshot` 自行 `PushActiveSnapshotWithLevel` 并在返回后 `PopActiveSnapshot`（源码提示 VACUUM/WAIT FOR 可能把整个快照栈弹空，因此允许栈为空）。
13. `PortalRunUtility` 调 `ProcessUtility` 时把 `readOnlyTree` 设为 `(portal->cplan != NULL)`：树若在 plancache 里就不可修改；上下文取 `isTopLevel ? PROCESS_UTILITY_TOPLEVEL : PROCESS_UTILITY_QUERY`。
14. `FillPortalStore`/`PortalStart` 路径创建执行器描述符：`CreateQueryDesc` 用 `plannedstmt->commandType` 作 `operation`，把 plan、源文本、参数、`queryEnv`、输出目标和 instrument 选项装进 `QueryDesc`，并 `RegisterSnapshot` 主快照与 crosscheck 快照，同时把 `tupDesc`/`estate`/`planstate`/`query_instr` 置空、`already_executed = false`，等 `ExecutorStart` 填好；`FreeQueryDesc` 反注册两个快照并释放 extension state 与结构本身。

## 关键取舍

- 源码事实：`ProcessUtility` 与 `standard_ProcessUtility` 被拆成两层，后者又把语句分成“无需 event trigger 支持”和“交给 `ProcessUtilitySlow`”两类，源码明确说明这一划分使 `START TRANSACTION` 不会触发 event trigger 缓存刷新。架构推论：把 event trigger 的启动前置条件（必须在有效事务中）编码进分派结构，而不是在每个处理函数里各自检查；若全部语句都走慢路径，事务控制语句就会在无事务状态下尝试刷新缓存而失败。
- 源码事实：`standard_ProcessUtility` 在 `readOnlyTree` 时整棵 `pstmt` 做 `copyObject`，并承认“昂贵语句正是需要拷贝的语句”，因此只能省下边际成本。架构推论：以可预期的不变性保护换取可能的整树拷贝开销；若假定调用方不会改树，plancache 中的计划会被 parse 变换改坏，之后再执行同一 prepared statement 会得到错误结果。
- 源码事实：`PortalRun` 专门比较“保存的 `CurrentResourceOwner` 是否等于保存的 `TopTransactionResourceOwner`”来决定恢复到新还是旧顶层对象。架构推论：这是为少数会自行开关事务的 utility 命令（VACUUM、CLUSTER）做的显式例外；若统一按普通语句恢复旧指针，就会指向已被内部提交销毁的 ResourceOwner，导致资源泄漏或崩溃。
- 源码事实：`PortalRunMulti` 对 `setHoldSnapshot` 的路径额外再复制一份快照，因为“hold 快照不能被同时当作活跃快照”。架构推论：用一次多余拷贝换取 `UpdateActiveSnapshotCommandId` 可正常工作；若复用同一对象，命令 ID 更新会与 hold 快照语义冲突。
- 源码事实：递归子命令通过新包一层 `PlannedStmt` 并显式传 `PROCESS_UTILITY_SUBCOMMAND`、`None_Receiver`、`qc = NULL`。架构推论：进程内递归分派让 `CREATE TABLE ... LIKE` 这类展开语句复用同一套语义与 event trigger 收集，代价是子命令不能设置命令标签、也不能输出结果，外层必须自行决定标签。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/tcop/utility.c` | `ProcessUtility` | utility 分派的插件挂点：断言调用约定后交给 `ProcessUtility_hook` 或 `standard_ProcessUtility`。 |
| `src/backend/tcop/utility.c` | `standard_ProcessUtility` | 第二次分派：算顶层/原子上下文、按只读分类禁命令、处理事务与其它无 event trigger 语句，其余转 `ProcessUtilitySlow`，收尾推进命令计数器。 |
| `src/backend/tcop/pquery.c` | `PortalRun` | portal 执行入口：标记 portal 状态、保存/替换全局 Owner 与 Context，按 strategy 分派到 select 或多语句路径，并在异常时标记失败后恢复环境重抛。 |
| `src/backend/tcop/pquery.c` | `CreateQueryDesc` | 组装执行器描述符：绑定计划、源文本、注册快照与 crosscheck 快照、输出目标与 instrument 选项，其余执行状态留给 `ExecutorStart`。 |

## 与主核 / AI 的关系

主核的命令执行不采用 `ProcessUtility`/`standard_ProcessUtility` 这种按 statement 节点类型做两段分派、并以 `ProcessUtilityContext` 表达“顶层/子命令/非原子”的结构。可借鉴的是把“事务边界敏感的命令”与“需要 DDL 触发钩子的命令”在分派层显式隔开；不要把 PostgreSQL 的 `PROCESS_UTILITY_SUBCOMMAND` 或 portal strategy 当作主核已有的命令语义。

## 参考

- [PostgreSQL 文档：SQL 语法与命令分类](https://www.postgresql.org/docs/current/sql-syntax.html) —— 说明 utility 命令与普通查询语句在语法层面的区别。
- [PostgreSQL 文档：事件触发器](https://www.postgresql.org/docs/current/event-triggers.html) —— 说明 DDL 命令的 event trigger 支持范围。
- [PostgreSQL 官方源码：utility 分派实现](https://github.com/postgres/postgres/blob/master/src/backend/tcop/utility.c) —— 支撑两段分派与只读/恢复限制的实现事实。
