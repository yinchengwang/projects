---
project: postgres
feature: planner-orchestration
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/plan/planner.c
    symbols: [standard_planner, subquery_planner, planner]
  - path: src/backend/optimizer/plan/planmain.c
    symbols: [query_planner]
---

# 查询规划编排

## 一句话职责

规划器把分析和重写后的查询树分层整理为候选路径，并最终交付可执行的 `PlannedStmt`。

## 机制是什么

`planner` 是公开分派入口，可由 `planner_hook` 接管，否则调用标准流程；`standard_planner` 建立一次规划共享的 `PlannerGlobal`，配置并行安全、候选路径策略和目标取数比例，再递归规划查询层。每层由 `PlannerInfo` 保存本层 Query、关系与路径状态，`PlannerGlobal` 聚合跨层子计划、参数、权限与关系信息。

本页聚焦编排边界：`subquery_planner` 做每个 Query 层的一次性预处理并进入上层规划，`query_planner` 负责基本扫描/连接关系路径，调用者继续处理聚合、排序等上层需求。不讨论索引匹配、连接枚举算法或具体执行器节点语义；这些由对应机制负责。

## 怎么实现的

1. `planner` 调用 hook 或 `standard_planner`，并在返回后报告 plan id；hook 需遵守输入树可能被原地改写的约定。
2. `standard_planner` 初始化 `PlannerGlobal`（子计划、关系表、参数类型、分区目录等），由 cursor 选项计算 `tuple_fraction`，并基于命令类型、并行 hazard 和 GUC 初始化并行状态及路径策略掩码。
3. 调用 `subquery_planner` 创建 `PlannerInfo`，设置 `parent_root`、层级、当前内存上下文与本层集合；CTE、RTE、子链接、表达式等按次序预处理，随后进入 `grouping_planner`。
4. `grouping_planner` 经 `query_planner` 获取基础连接的 `RelOptInfo`，再构造上层关系与路径；子查询递归使用共享 `PlannerGlobal`，父子 `PlannerInfo` 以参数项传递相关引用。
5. 顶层从 `UPPERREL_FINAL` 取最优分数路径，用 `create_plan` 变为 Plan；滚动游标必要时加 Materialize，再完成参数、引用与全局表收集，封装为 `PlannedStmt`。
6. 若创建过程报错，当前规划内存上下文负责回收；正常完成时关闭分区目录并调用 shutdown hook。规划本身在后端单次语句上下文中执行，不以锁来保护这些 PlannerInfo 局部结构。

## 关键取舍

- **源码事实：**全局状态拆在 `PlannerGlobal`，每个查询层另建 `PlannerInfo` 并保留父指针。**架构推论：**这种划分避免把嵌套查询的局部关系建模混入一份状态，同时让参数和子计划在最终计划中汇总；若每层各自独立全局状态，跨层 PARAM_EXEC 和子计划编号需额外合并。
- **源码事实：**`tuple_fraction` 对快速启动游标采用配置比例，普通查询按取全量处理，并传入最终路径选择。**架构推论：**同一候选集可按“先出若干行”或总成本作不同偏好；完全忽略消费模式会损失游标首行响应，始终按首行优化则可能牺牲全量吞吐。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/plan/planner.c` | `planner` | hook/标准实现分发及规划结果 plan id 上报 |
| `src/backend/optimizer/plan/planner.c` | `standard_planner` | 初始化全局状态，选择最终路径并组装 `PlannedStmt` |
| `src/backend/optimizer/plan/planner.c` | `subquery_planner` | 为一个 Query 层建立 `PlannerInfo`、完成预处理并调度 grouping planner |
| `src/backend/optimizer/plan/planmain.c` | `query_planner` | 建立基本关系与连接信息，产出顶层连接 `RelOptInfo` |

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 规划器在执行前选择查询计划
- [PostgreSQL 源码：planner.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/planner.c) —— 全局/逐层规划流程

## 与主核 / AI 的关系

这是 PostgreSQL 查询优化器的协调层。与主核查询优化能力只有概念层面的“查询树到候选执行方案”对应关系，不等同于主核的 SQL 执行规划实现；暂无直接 AI 对应。
