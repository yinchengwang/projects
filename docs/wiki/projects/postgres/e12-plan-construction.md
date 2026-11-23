---
project: postgres
feature: plan-construction
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/plan/createplan.c
    symbols: [create_plan, create_scan_plan, create_join_plan]
---

# Plan 树构造

## 一句话职责

Plan 构造把已选定的 Path 树落实为 executor 可识别的 Plan 节点，装配其子计划、输出列、过滤条件和节点专属参数。

## 机制是什么

Path 描述规划器候选的执行形态与属性，Plan 则是将交给执行器消费的节点树；构造过程按 `pathtype` 分派到扫描、连接、排序、聚合、Append、Limit 等具体 builder。Path 树每个节点决定相应 Plan 节点类型，目标列表与 quals 依据路径目标、关系属性和节点是否自行投影而整理，计划节点同时复制成本/行数等 explain 信息。

本页只负责 Path→Plan 转换和节点塑形，不选择或支配 Path（e08）、不计算成本/基数（e09），也不做最终 Var/Param 执行引用定型（e13 的 setrefs pass）。本阶段 Plan 中的 Var 仍可能沿用 parser rangetable 编号；之后由 `set_plan_references` 重写，不能把 create_plan 的输出误称为完全就绪的 executor reference 结构。

## 怎么实现的

1. `create_plan` 初始化递归工作状态，调用 `create_plan_recurse` 按 `best_path->pathtype` 分派。扫描类交 `create_scan_plan`，JoinPath 交 `create_join_plan`，其他上层 Path 调专用 builder；未识别节点类型报错。
2. `create_scan_plan` 从 `baserestrictinfo` 或 index path 的 `indrestrictinfo` 取过滤条件；参数化扫描并入 `ppi_clauses`。伪常量作为 gating qual 时可在扫描节点上方插入 Result；根据 CP flags 和 scan 能力决定使用物理 tuple tlist、index tlist 或 path target list。
3. 按实际 Path 类型建立 SeqScan/IndexScan/BitmapHeapScan/TidScan 等节点。构造 index scan 时把 IndexClause 表达式准备成 index Var 引用，移除已由 index 保证的重复/蕴含条件，保留特殊或 lossy 情形需要 executor recheck 的 qpqual，并在内连接参数化场景将外侧变量替换为 NestLoop Param。
4. `create_join_plan` 根据 NestPath、MergePath、HashPath 选择相应 join builder，递归构造 inner/outer 子树，按外连接语义分离 join clause 与其他过滤条件；对于 nestloop inner path 先处理参数化并记录 `curOuterRels`，使子表达式能转换为正确的运行期参数。
5. 上层节点 builder 可插入 Sort/IncrementalSort 或 Group/Unique/Agg，并把 Path 的 pathkeys 转换为排序列/操作符；Limit builder携带 offset/count 及 WITH TIES 比较列。Projection、Gather、Materialize 等是否引入额外节点取决于路径所需投影/并行/游标语义。
6. 完成当前 query level 顶层后附加 initPlans、装饰输出列并清理临时参数状态。后续规划调用方依次执行 `SS_finalize_plan` 与 `set_plan_references`；后两者不是 Plan 节点类型选择的一部分。

## 关键取舍

- **源码事实：**Plan 节点按 path type 专用 builder 分派，复用 Path 已确定的连接算法和输出属性；代价是每类节点 builder 必须保持与对应 Path 结构同步。若延迟到执行器再解释任意 Path，会令执行器依赖规划期对象和 optimizer 私有细节。
- **源码事实：**scan target list 在执行器可使用物理列布局时优先构造物理 tlist，以便扫描节点避免额外投影；索引仅扫描使用 index tlist，普通关系若含 dropped column 等情况则回退到 path tlist。强行统一采用逻辑目标列表可能失去扫描投影优化，始终用物理 tlist又无法表达复杂表达式/列布局缺口。
- **源码事实：**IndexScan 的 qpqual 去掉已由索引条件证明的限制，但 lossy index clause 必须保留原条件重检；谓词蕴含检查仅针对 immutable 条件。若一概删去“像是重复”的 qual，lossy bitmap/index 可能产生 false positives 而输出错误行。
- **源码事实：**`create_plan` 明确保留 planner 格式 Var，引用编号之后集中修正；把两阶段合并会让 build 时子计划输出 tlist 尚未完全确定的处理更复杂，并打破 setrefs 最终化约定。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/plan/createplan.c` | `create_plan` | 以获选顶层 Path 启动递归 Plan 树生成并完成该层收尾 |
| `src/backend/optimizer/plan/createplan.c` | `create_scan_plan` | 组合扫描目标列表、限制条件、参数化 quals 与扫描节点 builder |
| `src/backend/optimizer/plan/createplan.c` | `create_join_plan` | 将 JoinPath 分派成实际 join Plan 并附加一次性 gating 条件 |

## 与主核 / AI 的关系

暂无直接对应。Path 到 executor 节点树的分层可以作为优化器接口设计参照，但主核自己的算子节点与表达式引用规则必须由其执行层源码确认。

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 说明规划器产出供执行器使用的计划。
- [PostgreSQL 官方源码：createplan.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/createplan.c) —— 支撑 Path 节点分派、扫描/连接 builder 与 tlist/qual 塑形。
- [PostgreSQL 官方源码：pathnodes.h](https://github.com/postgres/postgres/blob/master/src/include/nodes/pathnodes.h) —— 支撑 Path/RelOptInfo 数据结构边界。
