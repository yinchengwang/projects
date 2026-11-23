---
project: postgres
feature: copy-commands
page_type: feature
schema_version: 1
refs:
  - path: src/backend/commands/copy.c
    symbols: [DoCopy]
  - path: src/backend/commands/copyfrom.c
    symbols: [BeginCopyFrom, CopyFrom]
  - path: src/backend/commands/copyto.c
    symbols: [BeginCopyTo, DoCopyTo]
---

# COPY 命令

## 一句话职责

COPY 命令提供表与外部数据源之间的高吞吐批量搬运通道，负责选模式、选格式、做行级解析与类型转换，并把结果按目标类型（表、查询结果、文件、程序或前端连接）落出去。

## 机制是什么

COPY 是一条 utility 命令（分派见 j03），但它有自己的一套执行骨架：入口 `DoCopy` 只做**决策**——源是表还是查询、去向是文件/程序/前端管道、要什么格式、要不要过滤；真正的搬运分别落在 `copyfrom.c`（读）与 `copyto.c`（写）两个方向上。

模式维度有三条互相正交的轴。**数据来源/去向**有三种：`CopySource`/`CopyDest` 枚举里的 `COPY_FILE`（服务器本地文件，或用 `popen` 拉起的外部程序）、`COPY_FRONTEND`（走前后端协议管道，客户端与应用服务器之间传数据）、`COPY_CALLBACK`（由调用方提供的回调函数供数据，扩展与并行场景用）。**数据格式**有三种：`CopyFromGetRoutine`/`CopyToGetRoutine` 在 text、CSV、binary 之间选一套例程，COPY TO 另有 JSON。**方向**是 FROM（读入表）与 TO（导出）。三个轴的正交性体现在：`DoCopy` 先定方向与源，再由 `BeginCopyFrom`/`BeginCopyTo` 依据选项定格式例程，最后由例程把具体字节解析/生成。

本域**不负责**：COPY 语句的语法与选项解析成语法树（解析阶段完成）、权限判定算法本身（ACL 在 G 组）、以及 `COPY FROM` 之后的并发可见性语义——写进去的行仍受普通事务与 MVCC 规则约束，COPY 没有额外的可见性保证。它与执行器的关系是：COPY TO 查询形式会构造一个 `DestReceiver` 并驱动 `ExecutorRun`，COPY FROM 则借用执行器的索引维护与约束检查机制但不经过计划树。

## 怎么实现的

1. `DoCopy` 的第一件事是**权限闸门**：只要目标不是管道（`filename == NULL`），就要求对应角色——`is_program` 时要 `pg_execute_server_program`，COPY FROM 文件要 `pg_read_server_files`，COPY TO 文件要 `pg_write_server_files`；走 stdin/stdout 管道则不需要额外角色，提示信息里也明说「任何人可以 COPY 到 stdout 或从 stdin 读」。
2. 表形式时 `DoCopy` 用 `table_openrv` 打开并加锁，锁模式按方向取：FROM 用 `RowExclusiveLock`，TO 用 `AccessShareLock`。它把关系挂到 `ParseNamespaceItem` 上，填 `perminfo->requiredPerms`（FROM 是 `ACL_INSERT`，TO 是 `ACL_SELECT`），再用 `CopyGetAttnums` 得到要处理的列号，把它们并入 `insertedCols`/`selectedCols` 后交给 `ExecCheckPermissions`。
3. `COPY ... WHERE` 的处理在 `DoCopy` 内完成语义准备：`transformExpr` 配 `EXPR_KIND_COPY_WHERE` 做表达式变换，再 `coerce_to_boolean` 强制布尔语义、`assign_expr_collations` 定排序规则、`pull_varattnos` 收集引用到的列。系统列被显式拒绝（`attno < 0`），生成列也被拒绝，因为筛选发生时这两类列还没算出来。最后 `eval_const_expressions` → `canonicalize_qual` → `make_ands_implicit` → `fix_opfuncids` 把条件规整成标准的 AND 列表。
4. 行级安全走岔路：`check_enable_rls` 返回 `RLS_ENABLED` 时，COPY FROM 直接报错（提示改用 INSERT）；COPY TO 则被改写成一条 `SELECT`——构造 `SelectStmt` + `RangeVar`，其中 `inh` 标志只在源是分区表时置真（普通表要 `ONLY`，分区表要带上子表），并把关系关掉但保留锁，等查询式 COPY 时再打开。
5. `BeginCopyFrom` 建立 `CopyFromState`：`palloc0_object(CopyFromStateData)` 后切进一个新的 `AllocSetContextCreate(CurrentMemoryContext, "COPY")`，源码注释说明这是为了避免一条查询里反复 COPY 造成内存泄漏。`ProcessCopyOptions` 把语法树上的选项解析成 `CopyFormatOptions`，`CopyFromGetRoutine` 选出 text/CSV/binary 例程，`CopyGetAttnums` 生成列清单。`FORCE_NOT_NULL`/`FORCE_NULL` 的列名清单被转成按列的 bool 数组，并校验被点名的列确实在 COPY 的列清单里，否则报错。
6. `ON_ERROR` 不是 stop 时，`BeginCopyFrom` 建立 `ErrorSaveContext` 做**软错误捕获**；若取 `SET_NULL`，还要额外准备 `domain_with_constraint` 数组——因为 `ExecConstraints` 不检查 domain 约束，带约束的 domain 的空值检查必须提前到字符串转 datum 的阶段做。
7. `BeginCopyFrom` 再为每一列取输入函数与元素类型（`in_functions`/`typioparams`），并对不在列清单里的列取列默认值：`build_column_default` → `expression_planner` → `ExecInitExpr`，默认值表达式初始化在 copycontext 里。
8. `CopyFrom` 的准备工作先做目标合法性检查：目标必须是普通表、外表、分区表，或者带 `INSTEAD OF INSERT` 行触发器的对象；视图、物化视图、序列分别报专门的错误。`COPY FREEZE` 有三重限制——不能在分区表和外表上做（源码说分区表只是「还没打开各分区判断优化是否适用，开销可能不小」），要求表是在当前子事务里创建或 truncate 的，并且此前没有已注册快照或待决 portal，否则报「因为先前的事务活动」。
9. `CopyFrom` 复用执行器的插入机制：`CreateExecutorState` → `ExecInitRangeTable` → `ResultRelInfo` → `CheckValidResultRel` → `ExecOpenIndices`，再造一个 `ModifyTableState` 让 FDW 有地方初始化。批量插入方式由 `CopyInsertMethod` 决定，源码逐条列出了必须退回单行插入的情形：目标有 BEFORE 或 INSTEAD OF 行触发器（触发器可能查询正在插入的表，必须能看见已处理过的行）、FDW 不支持批处理、分区表上有语句级 INSERT 触发器、列默认值里含 volatile 表达式（同样可能回查目标表）、`WHERE` 子句里含 volatile 函数。分区表用中间档 `CIM_MULTI_CONDITIONAL`，等真正定位到某个分区后再按该分区的触发器与 FDW 能力决定。
10. `CopyFrom` 的主循环每轮先 `ResetPerTupleExprContext` 回收上一行的表达式内存，切到 per-tuple 上下文调 `NextCopyFrom` 取下一行；`ON_ERROR IGNORE` 时若本行出了软错误就跳过本行、累加错误计数并更新进度视图，超过 `REJECT_LIMIT` 则报错终止。行被存成虚拟元组后，先过 `WHERE` 过滤（被过滤的行计入 `PROGRESS_COPY_TUPLES_EXCLUDED`），再由 `ExecFindPartition` 决定落到哪个分区，最后按插入方式写入。错误定位靠挂在 `error_context_stack` 上的 `CopyFromErrorCallback`，它能报出当前行号、列名与列值。
11. `BeginCopyTo` 先校验源：`relkind` 必须是普通表，视图、物化视图、外表、序列分别报错并提示改用 `COPY (SELECT ...) TO`；分区表则用 `find_all_inheritors` 收集所有子表，逐个检查是否是外表、是否在恢复期不可访问，并剔除本身还带分区的中间节点。恢复期访问临时表或 unlogged 表直接报错。
12. `DoCopyTo` 的协议侧：只有「管道 + 输出目的地是远端」时才走前端协议，先 `SendCopyBegin` 发 `CopyOutResponse` 消息（整体格式 + 逐列格式；JSON 格式固定报告 1 列文本格式，因为每个 `CopyData` 消息装的是一整个 JSON 对象），结束时 `SendCopyEnd` 发 `CopyDone`。数据先攒进 `fe_msgbuf`，到行尾才真正写出或发出一条 `CopyData`。
13. `DoCopyTo` 的数据侧分两路：源是表时 `CopyRelationTo` 直接顺序扫描；源是查询时构造 `DestReceiver`（`CreateCopyDestReceiver`）并 `ExecutorRun`，元组由接收器回调送出。分区表逐分区扫描，并用 `build_attrmap_by_name_if_req` 把分区行转换成根表的行类型，因为分区列顺序可能与根表不同。每行在独立的 `rowcontext` 里求值后整体重置，源码说明这样比逐个 `pfree` 更快，也能兜住数据类型输出函数内部的泄漏。

## 关键取舍

- 源码事实：`CopyFrom` 在多种情况下把批量插入降级为单行插入（`CIM_SINGLE`），每一条降级理由都写在源码注释里，核心是「触发器或 volatile 默认表达式可能查询正在插入的表」。架构推论：放弃批处理带来的吞吐，换取触发器语义正确；批量路径下已准备但未插入的行对触发器不可见，触发器据此做出的决定就会与逐行插入时不同。代价是带 BEFORE 触发器的表 COPY 速度显著下降。
- 源码事实：分区表使用中间档 `CIM_MULTI_CONDITIONAL`，源码注释解释各分区的 `ResultRelInfo` 只在真正要往该分区插第一行时才初始化，所以分区级判断只能推迟到那时。架构推论：把判断推迟换取不预先打开全部分区；替代方案（预先打开所有分区决定）会让小批量 COPY 也付出打开全部分区的开销。
- 源码事实：`COPY FREEZE` 在分区表上直接报错，源码给出的理由是「我们还没有打开各分区来判断这个优化是否适用，可能开销相当大；而且最终可能只路由到少数几个分区」。架构推论：放弃「小表批量导入免 freeze」的收益，换取实现简单与语义正确；替代方案（逐分区判断并施加 freeze）需要把冻结语义扩展到分区路由路径，风险更高。
- 源码事实：`COPY FROM` 不支持行级安全策略，直接报「COPY FROM not supported with row-level security」并提示改用 INSERT。架构推论：放弃 COPY 的批量吞吐换取策略语义正确；替代方案（把 COPY FROM 改写成 INSERT 循环）会丢掉 COPY 独有的批量插入优化，实际吞吐未必更好。
- 源码事实：COPY TO 的前端协议模式只在「管道且目的地是远端」时才发 `CopyOutResponse`/`CopyDone` 消息，写文件或写程序时不发。架构推论：协议消息只服务于前端管道这一条路径，让文件与程序路径不背协议负担；代价是同一条 COPY 语句在两种目的地下行为不同，错误处理也要分开。
- 源码事实：`DoCopy` 在 RLS 生效时把 COPY TO 改写成 `SELECT`，且对分区表把 `RangeVar` 的 `inh` 置真、对普通表用 `ONLY`。架构推论：用「改写成查询」复用现有查询路径的策略过滤与分区展开，换取 COPY 与普通查询在可见性上完全一致；代价是 COPY TO 失去直接顺序扫描表的简单路径，要多走一层计划与执行器。
- 源码事实：`BeginCopyFrom` 为每次 COPY 建独立的 `AllocSetContext`，`DoCopyTo` 为每一行建 `rowcontext` 并整体重置。架构推论：用上下文批量回收代替逐对象 `pfree`，换取输出/输入函数内部不可控分配的兜底能力与更好的速度；替代方案（逐个 `pfree`）一旦数据类型函数自己分配了内存就会泄漏或需要逐个追踪。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/commands/copy.c` | `DoCopy` | COPY 入口决策层：角色权限闸门、按方向选锁模式与所需 ACL、准备 `WHERE` 条件（拒绝系统列与生成列）、按 RLS 是否启用决定报错还是改写成 `SELECT`、最后分派到 `BeginCopyFrom`/`CopyFrom` 或 `BeginCopyTo`/`DoCopyTo`。 |
| `src/backend/commands/copyfrom.c` | `BeginCopyFrom` | 读方向的初始化：建立独立的 COPY 内存上下文、解析格式选项、选定 text/CSV/binary 例程、生成列清单并展开 `FORCE_NOT_NULL`/`FORCE_NULL` 标志、为 `ON_ERROR` 建软错误捕获上下文、取每列的输入函数与列默认值表达式。 |
| `src/backend/commands/copyfrom.c` | `CopyFrom` | 读方向的主循环：校验目标类型与 `COPY FREEZE` 前提、复用执行器的 `ResultRelInfo` 与索引维护、按触发器/FDW/volatile 表达式决定批量插入档位、逐行读取、过滤、分区路由与写入，并用 `CopyFromErrorCallback` 定位报错行。 |
| `src/backend/commands/copyto.c` | `BeginCopyTo` | 写方向的初始化：校验源关系种类、分区表收集子表并逐个检查、解析格式与编码选项、构造查询形式的 `DestReceiver` 与 `QueryDesc`、展开 `FORCE_QUOTE` 标志。 |
| `src/backend/commands/copyto.c` | `DoCopyTo` | 写方向的主循环：需要时发前端协议 `CopyOutResponse`/`CopyDone`、表源逐分区顺序扫描并转换行类型、查询源驱动 `ExecutorRun`、每行在独立上下文中求值后整体重置。 |

## 与主核 / AI 的关系

主核 `engine/db` 有自己的批量导入路径，但形态与 COPY 不同：它是引擎内建的向量化导入与 blob 批量写入，不存在「三种来源 × 三种格式 × 两个方向」的选项矩阵，也没有前后端协议管道这一维。可借鉴的是 COPY 把「方向决策」与「格式例程」分层——用一张 `CopyFromRoutine`/`CopyToRoutine` 函数表把 text/CSV/binary 的差异关进例程里，外层流程只依赖例程签名；不要把 PostgreSQL 的 `CopyFormatOptions` 选项集、`COPY FREEZE` 前提或 `CopyInsertMethod` 降级判据当作主核已有的导入语义。

## 参考

- [PostgreSQL 文档：COPY](https://www.postgresql.org/docs/current/sql-copy.html) —— 说明 COPY 的语法面与三种来源/去向形态，与 `DoCopy` 的决策项相对应。
- [PostgreSQL 文档：psql 的 \copy](https://www.postgresql.org/docs/current/app-psql.html) —— 说明 `\copy` 与服务端 COPY 的差别，对应「管道模式不需要额外角色」这一事实。
- [PostgreSQL 官方源码：COPY 命令入口](https://github.com/postgres/postgres/blob/master/src/backend/commands/copy.c) —— 支撑权限闸门、锁模式、RLS 分支的实现事实。
- [PostgreSQL 官方源码：COPY FROM 实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/copyfrom.c) —— 支撑批量插入降级判据、软错误处理与 `COPY FREEZE` 前提的事实。
- [PostgreSQL 官方源码：COPY TO 实现](https://github.com/postgres/postgres/blob/master/src/backend/commands/copyto.c) —— 支撑前端协议消息与分区行类型转换的事实。
