---
project: postgres
feature: session-and-guc
page_type: feature
schema_version: 1
refs:
  - path: src/backend/utils/misc/guc.c
    symbols: [SetConfigOption, InitializeGUCOptions, define_custom_variable]
  - path: src/backend/utils/misc/guc_funcs.c
    symbols: [ExecSetVariableStmt]
---

# GUC 与会话配置

## 一句话职责

GUC 子系统维护一张按名字索引的配置变量表，把「来源」（`GucSource`）与「作用上下文」（`GucContext`）两个轴分开判定，让所有配置写入——配置文件、命令行、SQL `SET`、扩展注册——都走同一条「解析 → 校验 → 生效 → 触发钩子」路径。

## 机制是什么

配置变量不止一种来源，也不止一个生效范围。GUC 用两个枚举把这两件事拆开：

- `GucSource` 描述**值从哪来**：`PGC_S_DEFAULT`（`boot_val`）、`PGC_S_DYNAMIC_DEFAULT`（初始化期算出来的默认）、`PGC_S_ENV_VAR`、`PGC_S_FILE`、`PGC_S_ARGV`、`PGC_S_GLOBAL`、`PGC_S_DATABASE`、`PGC_S_USER`、`PGC_S_DATABASE_USER`、`PGC_S_CLIENT`、`PGC_S_OVERRIDE`（强制改默认）、`PGC_S_TEST`、`PGC_S_SESSION`（`SET` 命令）。`PGC_S_INTERACTIVE` 不是一个真正的来源值，它是「错误严重度分界线」：位于它之上的来源出错直接报 ERROR，之下的只记 DEBUG。
- `GucContext` 描述**谁有权改**：`PGC_INTERNAL`、`PGC_POSTMASTER`（只能改配置文件并重启）、`PGC_SIGHUP`（改配置文件后 SIGHUP 生效）、`PGC_SU_BACKEND`、`PGC_BACKEND`、`PGC_SUSET`（超级用户）、`PGC_USERSET`（普通用户）。

两个轴是正交的：一个写入请求能否成功，既要看该变量允许的上下文是否覆盖请求方，也要看当前已生效值的来源优先级是否被新请求超越（`record->source > source` 时，若不属于「设默认值」情形就静默忽略并返回 -1）。

变量表本身是**一个**数组 `ConfigureNames[]`（`struct config_generic` 的连续数组，由构建期脚本从配置定义文件生成，不属于 guc.c 的源码），启动时被灌进一张以变量名为键的哈希表。没有第二套按前缀的索引：按前缀操作（例如扩展保留自己的命名空间）靠对哈希表做全表扫描实现。

它管的是**配置值本身的状态机**，不负责某个参数所影响的业务逻辑。`shared_buffers` 改完由缓冲池启动路径去分配内存，`work_mem` 改完由执行器在每次建排序时读一次——GUC 只保证「值合法、被记录、变更时触发变量自己的钩子」，不保证下游在什么时刻读到它。它也不负责把会话设置传给并行 worker：那是并行执行侧读取 GUC 状态再重建，属于另一个机制。

## 怎么实现的

1. `InitializeGUCOptions` 在启动早期运行：先 `pg_timezone_initialize`（因为 `log_line_prefix` 早期就可能带时区，而 `elog` 会在 GUC 就绪前用到它），再 `build_guc_variables` 建立 `GUCMemoryContext` 与哈希表。`build_guc_variables` 先用 `for (int i = 0; ConfigureNames[i].name; i++)` 数出内建变量数以确定哈希容量，再逐条插入。
2. 建表之后，`InitializeGUCOptions` 用 `hash_seq_init` / `hash_seq_search` 遍历每个 `GUCHashEntry`，对每条变量调用 `InitializeOneGUCOption`。这一步**会调钩子**：以 `PGC_BOOL` 分支为例，它先 `call_bool_check_hook(..., PGC_S_DEFAULT, LOG)`，返回假就 `elog(FATAL, "failed to initialize %s to %d")`；通过后再调 `conf->assign_hook(newval, extra)`，最后把值同时写进当前值与 `reset_val`。也就是说默认值不是「绕过校验直接写入」，而是**经过与正常赋值同一组钩子**。
3. `SetConfigOption` 是 GUC 对外的一层薄包装，只做签名收敛：它固定 `GUC_ACTION_SET`、`changeVal = true`、`elevel = 0`、`is_reload = false`，然后转给 `set_config_option`。真正的判定逻辑在 `set_config_option` → `set_config_with_handle` 里。它**不是**「会话专用入口」——配置文件加载、`ALTER SYSTEM`、并行 worker 恢复都在用同一个下层函数，区别只在传入的 `source`、`context` 与 `action`。
4. `set_config_with_handle` 的判定顺序是：按名字查变量（查不到且允许建占位就建占位，否则报错）→ 检查变量是否 `GUC_NO_RESET`（`value == NULL` 的 reset 请求会被拒）→ 计算 `makeDefault`（`changeVal && source <= PGC_S_OVERRIDE && (value != NULL || source == PGC_S_DEFAULT)`，决定是否顺带写 `reset_val` 与栈值）→ 比较 `record->source > source` 决定是否忽略 → 按 `vartype` 分派解析与校验 → 类型 `check_hook` 与 `assign_hook` → 用 `GucAction` 决定往事务级栈上压值。
5. `GucAction` 只有三个值：`GUC_ACTION_SET`（普通 `SET`）、`GUC_ACTION_LOCAL`（`SET LOCAL`）、`GUC_ACTION_SAVE`（函数内的临时赋值，配合 `GUC_ACTION_SAVE` 时不能用于 `GUC_NO_RESET` 变量）。事务语义由 `push_old_value` 与 `GUCNestLevel` 实现：嵌套层级为 0 时什么都不压；同一层级重复设置时按动作合并状态——`SET` 覆盖同层更早的动作（若此前是 `GUC_SET_LOCAL` 状态，先丢掉被遮挡的旧值），`SET` 后紧跟 `SET LOCAL` 时把 `SET` 的值存进 `masked` 并把状态升为 `GUC_SET_LOCAL`。事务结束由 `AtEOXact_GUC(isCommit, nestLevel)` 按层弹出或提交。
6. `ExecSetVariableStmt` 是 SQL 层的接线。它先算 `action = stmt->is_local ? GUC_ACTION_LOCAL : GUC_ACTION_SET`，并行模式下直接拒绝（`cannot set parameters during a parallel operation`）。按 `stmt->kind` 分派：`VAR_SET_VALUE` / `VAR_SET_CURRENT` 调 `set_config_option` 传实际值；`VAR_SET_DEFAULT` 贯穿到 `VAR_RESET`，两者都传 `value = NULL`；`VAR_RESET_ALL` 走 `ResetAllOptions`。传 `NULL` 的含义是「回到 `reset_val`」，不是「无条件回编译期默认」——`reset_val` 在会话建立时已被数据库级/角色级默认改写过，所以 `RESET` 到此为止；只有当事务内发生过叠加赋值，才由 `AtEOXact_GUC` 还原 prior 值。语句末尾还会调 `InvokeObjectPostAlterHookArgStr(ParameterAclRelationId, ...)`，让 `ALTER` 类钩子能观察到这次参数变更。
7. 自定义变量走的是「先占位、后替换」而不是「先真空、后填充」：`find_option(..., create_placeholders = true)` 在遇到未知名字时调 `add_placeholder_variable`，建出一个 `vartype = PGC_STRING`、带 `GUC_CUSTOM_PLACEHOLDER`、`GUC_NO_SHOW_ALL`、`GUC_NOT_IN_SAMPLE` 标志的占位记录，值全是 NULL。等到扩展的 `_PG_init` 调 `define_custom_variable` 时，它先断言新变量自身 `check_GUC_init` 通过，再在哈希表里找同名占位；找到占位就检查它确实是 `GUC_CUSTOM_PLACEHOLDER`（否则 `attempt to redefine parameter` 报 ERROR），`InitializeOneGUCOption` 初始化新变量，用 `hentry->gucvar = variable` **原地替换**哈希槽位，`RemoveGUCFromLists(pHolder)` 摘掉占位登记，然后把占位期间攒下的 `reset_val` 与栈值用 `set_config_option_ext` 逐条重放到真实变量上——重放失败只报 WARNING 而不抛 ERROR，因为扩展可能正加载到一半。

## 关键取舍

- **源码事实：`ConfigureNames[]` 是单数组 + 一张哈希表，没有前缀索引。** `build_guc_variables` 线性数数组长度、`get_guc_variables` 全量取指针后 `qsort` 按名排序；`MarkGUCPrefixReserved` 用 `hash_seq_init` 全表扫描匹配前缀，`reserved_class_prefix` 只是一个保留前缀列表。放弃的是「按前缀 O(log n) 查找」，换来只有一份权威数据结构、不会出现两份索引不一致。若加前缀索引，`define_custom_variable` 原地替换槽位、`free_placeholder` 删除记录这些改动都要同步维护索引，而这类操作在运行期极稀疏。
- **源码事实：`InitializeOneGUCOption` 会调 `check_hook` 与 `assign_hook`，失败即 `elog(FATAL)`。** 这意味着编译期默认值也要过变量作者写的校验，一个写错的默认值在启动阶段就会明确失败，而不是带着非法值跑起来。代价是钩子必须能在「还没有任何会话、没有数据库连接」的极早期上下文里安全执行；如果钩子依赖某个尚未初始化的子系统，就只能把 `boot_val` 设成不触发那条路径的保守值。
- **源码事实：`SetConfigOption` 固定 `GUC_ACTION_SET`，事务级语义靠 `GucAction` 与 `push_old_value` 区分。** 把「改值」与「改值的可回滚性」拆成两个参数，好处是配置文件加载这类不具事务性的路径不必伪造一个事务上下文；代价是同名动作有两个同义词（`SetConfigOption` 与 `set_config_option(..., GUC_ACTION_SET, ...)`），调用点必须清楚自己走的是哪一条。
- **源码事实：`RESET` 传 `value = NULL` 回到 `reset_val`，而不是回 `boot_val`。** `reset_val` 在启动与会话建立时已被 `PGC_S_DATABASE` / `PGC_S_USER` 等更高优先级来源改写过，所以 `RESET` 的语义是「回到本会话看到的默认」。代价是同一句 `RESET` 在不同角色、不同数据库下结果不同；若统一回 `boot_val`，`ALTER DATABASE ... SET` 与 `ALTER ROLE ... SET` 就失去意义。
- **源码事实：自定义变量必须先占位再替换，且重放失败只报 WARNING。** 占位让配置文件里写扩展参数不至于在扩展加载前就报「未知变量」，而「只警告不报错」保证一个非法值不会打断扩展自身的加载流程。代价是存在「记录存在但无效」的中间态（`GUC_CUSTOM_PLACEHOLDER` 且值为 NULL），读取方必须能区分它与正常未设置；若不允许占位，那么加载顺序就必须严格到「配置文件解析晚于所有扩展 `_PG_init`」。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/misc/guc.c` | `InitializeGUCOptions` | 启动期建立变量哈希表，逐条调 `InitializeOneGUCOption` 灌入默认值并执行校验/赋值钩子 |
| `src/backend/utils/misc/guc.c` | `SetConfigOption` | 对外薄包装：固定 `GUC_ACTION_SET` 与 `changeVal = true` 后转给 `set_config_option` |
| `src/backend/utils/misc/guc.c` | `define_custom_variable` | 扩展注册：查同名占位、原地替换哈希槽位、重放占位期攒下的 reset 与栈值 |
| `src/backend/utils/misc/guc_funcs.c` | `ExecSetVariableStmt` | SQL `SET` / `SET LOCAL` / `RESET` / `RESET ALL` 到 `set_config_option` 的分派与并行模式拒绝 |

## 参考

- [PostgreSQL 文档：参数设置](https://www.postgresql.org/docs/current/config-setting.html) —— 支撑 `SET` / `RESET` / `SET LOCAL` 的用户可见语义与作用范围
- [PostgreSQL 文档：pg_settings 视图](https://www.postgresql.org/docs/current/view-pg-settings.html) —— 支撑 `pending_restart`、`source`、`context` 三列与 GUC 内部字段的对应关系
- [PostgreSQL 官方源码：GUC 核心实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/misc/guc.c) —— 支撑变量表构建、`GucSource`/`GucContext` 判定、`push_old_value` 与 `define_custom_variable` 的实际流程
- [PostgreSQL 官方源码：SET/RESET 语句接线](https://github.com/postgres/postgres/blob/master/src/backend/utils/misc/guc_funcs.c) —— 支撑四种语句分支与 `value = NULL` 的 reset 语义

## 与主核 / AI 的关系

主核当前没有等价的「多来源 + 多上下文」配置子系统，配置主要以启动参数与编译期常量为表达。对照时可借鉴两点：一是把「值的合法性判定」与「值生效后触发谁」拆开，合法性判定放在写入路径而不是读取路径；二是需要重启的参数要有显式的待生效状态（GUC 用 `GUC_PENDING_RESTART` 位标记并由 `pg_settings.pending_restart` 对外暴露），而不是静默丢弃用户请求。但不应据此推断主核存在同名变量表、相同钩子机制或事务级配置栈。
