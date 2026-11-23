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

GUC 子系统维护一份带作用域的配置变量表，把所有配置来源（配置文件、命令行、会话命令、环境派生值）归一成同一条「赋值 + 校验 + 生效」路径，并决定每个变量在什么层级可见、可改、何时回调。

## 机制是什么

PostgreSQL 的运行参数不只有一种来源，也不只有一个生效范围。GUC 把这两件事拆开表达：**来源**由 `GucSource` 枚举描述（默认值、配置文件、命令行、数据库级、角色级、会话命令、客户端反馈等），**作用范围**由 `GucContext` 枚举描述（内部、postmaster、sighup、backend、超级用户可设、普通用户可设）。两者是正交的两个轴：一个值能被判为「合法」，必须先满足它所在来源允许的上下文，再通过变量自身的校验钩子。

变量本身有三种登记形态。内建变量在 `InitializeGUCOptions` 期间由静态表建立；扩展模块可以在加载期用 `define_custom_variable` 注册带占位值的自定义变量，之后再由主人（通常是某个扩展的 `_PG_init`）提交真实值；这两种都进同一张哈希表与同一套查找结构（按名字精确查找与按前缀批量查找并存）。

它管理的是**配置值本身的状态机**，不负责执行某个参数所影响的业务逻辑。`shared_buffers` 改完由缓冲池启动路径去分配内存，`work_mem` 改完由 executor 在每次建排序时读一次——GUC 只保证「值合法、被记录、变更时触发它自己的回调」，不保证下游在什么时刻读到它。它也不负责把会话级设置传播到并行 worker：那是并行执行侧的 `SerializeGUCState` / `RestoreGUCState` 读取 GUC 状态再做应用，属于另一个机制。

## 怎么实现的

1. `InitializeGUCOptions` 在 postmaster 启动早期运行：先建立变量哈希表与按前缀的辅助结构，再遍历内建的静态配置表（`ConfigureNamesBool` / `ConfigureNamesInt` / `ConfigureNamesReal` / `ConfigureNamesString` / `ConfigureNamesEnum` 五类），为每一项分配 `config_generic` 记录并写入默认值。默认值写入走的是「内部来源 + 内部上下文」，不触发任何回调。
2. `SetConfigOption` 是会话级赋值的主入口。它先按名字查变量，再做四道判断：变量是否存在、当前 `GucContext` 是否允许改它、来源是否足够高、值能否被 `parse_and_validate_value` 通过。任一道不过就按 `elevel` 返回错误或警告，不写值。
3. 写值这一步分成「记录候选状态」与「提交」两段：先构造新的 `config_var_value` 状态，再在需要时经 `set_config_option` 的内部路径更新生效值。`changeValPending` 与 `GUC_need_restart` 这类标志就是「候选状态未提交」的显式表示——需要重启的参数可以记录新值但不立即生效。
4. 变量变更后，GUC 按 `GucAction`（set / assign / reset）与新旧值是否相同决定要不要调钩子：`assign_hook`、`check_hook`、`show_hook`。只有值真的变化且不是「恢复到默认」的幂等路径时才调，避免无关回调带来副作用。
5. `define_custom_variable` 把外部变量的名字、上下文、类型、默认值与回调打包成一个 `config_generic`，先以占位值登记。占位期内该变量可被查询但不能被当作有效配置源；主人提交真实值后占位标记清除，此后它和任何内建变量走同一条查找与校验路径。
6. `ExecSetVariableStmt` 是 SQL 层的接线：`SET name = value`、`RESET name`、`SET LOCAL` 三种语句最终都归到 `SetConfigOption`，只是上下文与 `GucSource` 不同——`SET LOCAL` 记在事务级的回滚栈上，事务结束由 `AtEOXact_GUC` 回退或提交；`RESET` 走 reset 动作把变量恢复到上一层的值而不是回默认值。

## 关键取舍

- **源码事实：变量用哈希表精确查找 + 前缀结构批量查找，而不是每次遍历静态表。** 静态表条目数在千级，但 `SHOW ALL`、`pg_settings` 全量读取、按前缀匹配（如 `SET pgsql.` 的自定义变量组）都需要批量路径；两者共存要维护两份索引的一致性。若只保留线性遍历，单次查找代价随变量数增长，而且无法支撑前缀语义。
- **源码事实：`SetConfigOption` 先校验后写值，校验钩子可以在写值前拒绝。** 这保证任何被记录的值都是变量作者认可的值；代价是校验钩子必须自己无副作用（不能在校验阶段就修改共享状态），否则被拒绝的赋值会留下痕迹。替代方案是「先写后校验、失败回滚」，会引入短暂的不一致窗口。
- **源码事实：需要重启的参数允许记录待生效值。** 会话里改 `shared_buffers` 不会静默丢弃，而是写进候选状态并标记 `GUC_need_restart`，由 `pg_settings.pending_restart` 暴露。代价是同一变量存在两个值（当前生效 vs 已记录），所有读取路径都必须清楚自己该读哪个；如果统一成「不可改」，用户就必须手工改配置文件并重启，丢失运行时编排能力。
- **源码事实：自定义变量先占位再提交。** 占位是为了让扩展的默认值在 `_PG_init` 之前也不至于让配置文件里的 `SET` 报「未知变量」。代价是存在一个「存在但无效」的中间态，需要额外的有效性判断；若不允许占位，配置文件里写扩展参数就会在启动阶段直接失败。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/utils/misc/guc.c` | `InitializeGUCOptions` | 启动期建立变量表并把五类内建静态表灌入默认值 |
| `src/backend/utils/misc/guc.c` | `SetConfigOption` | 会话级赋值入口：查变量、判上下文与来源、校验、写值、触发钩子 |
| `src/backend/utils/misc/guc.c` | `define_custom_variable` | 扩展注册自定义变量，先登记占位值等待主人提交 |
| `src/backend/utils/misc/guc_funcs.c` | `ExecSetVariableStmt` | SQL `SET` / `RESET` / `SET LOCAL` 三种语句到 `SetConfigOption` 的接线与事务级回滚登记 |

## 参考

- [PostgreSQL 文档：参数设置与 GUC 作用范围](https://www.postgresql.org/docs/current/config-setting.html) —— 支撑配置来源、会话级 `SET` 与 `RESET` 的可见语义
- [PostgreSQL 官方源码：GUC 核心实现](https://github.com/postgres/postgres/blob/master/src/backend/utils/misc/guc.c) —— 支撑变量表、上下文与来源判定、校验与钩子调用顺序
- [PostgreSQL 官方源码：SET/RESET 语句接线](https://github.com/postgres/postgres/blob/master/src/backend/utils/misc/guc_funcs.c) —— 支撑三种 SQL 语句分别映射到哪种动作

## 与主核 / AI 的关系

主核当前没有等价的「多来源 + 多作用域」配置子系统，配置多以启动参数与编译期常量表达。对照时可借鉴两点：把「值的合法性判定」与「值生效后触发谁」拆开；需要重启的参数要有显式的待生效状态，而不是静默忽略。但不应据此推断主核存在同名变量表或相同的钩子机制。
