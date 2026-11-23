---
project: postgres
feature: path-ranking
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/util/pathnode.c
    symbols: [add_path, set_cheapest, compare_path_costs]
---

# 路径支配与最优选择

## 一句话职责

路径排序机制为同一关系维护一组不可互相替代的物理实现候选，并在关系路径构造结束时标记后续连接或顶层规划所需的低成本代表。

## 机制是什么

`RelOptInfo.pathlist` 不是“按成本排序后的唯一路径”，而是依支配规则删减后的候选集。`add_path` 比较禁用节点数、启动/总成本、输出排序属性、外部参数化集合、行数和并行安全性：一个方案虽总成本低但启动成本高、排序较差或要求更多外关系时，另一个方案仍可能在某些上层组合中有价值。`Path` 因而保留的不是局部唯一赢家，而是后续可能需要的非支配方案。

本机制负责路径候选支配、`cheapest_*` 字段设置，不生成访问/连接候选（e05/e06）、不计算成本和基数（e09）、不构造 executor Plan（e12）。参数化路径只在兼容的 `required_outer` 条件下比较；`add_path` 将参数化路径的 pathkeys 视作空，避免把不适用于其调用场景的排序优势当作支配依据。

## 怎么实现的

1. 每个候选由上游路径生成器提供；`add_path` 先对启动/总成本做模糊比较，源码定义 `STD_FUZZ_FACTOR` 为 1.01，避免微小浮点差异造成不稳定删选。`disabled_nodes` 数较少者优先于成本值。
2. 成本未能分出支配关系时，再比较 `pathkeys`、`PATH_REQ_OUTER`、行数和 `parallel_safe`。路径只有在成本、排序、参数需求、行数及并行安全等相关维度上不劣且至少有优势，才能淘汰旧项；交叉优势的路径会并存。
3. 被支配的旧路径从 `pathlist` 删除，新路径若被支配则不插入。列表维持按 disabled-node 数与 total cost 排序，以让后续比较较早退出；安全可释放的 Path 节点立即回收，IndexPath 及仍属于其他 parent 的 Path 不擅自释放。
4. `set_cheapest` 遍历留存列表，分开选无参数化路径的最低 startup 与 total cost；成本相等时偏好更好的 pathkeys。参数化候选按 `required_outer` 的包含关系比较，收集不同有用参数化的代表。
5. 若没有无参数化路径，`cheapest_total_path` 回退为参数最少且总成本最低的参数化候选；`cheapest_startup_path` 仍为 NULL。上层连接搜索可从参数化候选中选择可由其外侧关系提供所需参数的路径。

## 关键取舍

- **源码事实：**成本比较采用 1.01 的模糊门槛，降低舍入差异导致的候选抖动；代价是小于阈值的实际成本差异可能被视为相等。若精确比较所有成本，保留路径和最终计划更容易受浮点误差及平台细节影响。
- **源码事实：**以多维支配而非仅保留最低 total cost，保留低 startup、不同排序或参数化需求的候选；代价是 planner 要持有并比较更多 Path。若只留单一总成本赢家，LIMIT/merge join/参数化内连接可能失去更适合的输入路径。
- **源码事实：**参数化路径不以 pathkeys 获胜，且启动成本只在 `consider_param_startup` 开启时用于保留；这是压缩参数化候选集的策略。源码 caller `set_cheapest` 将参数最少路径作为 fallback，供无无参数化路径时继续规划；若把所有参数化路径都视为全功能候选，会扩大搜索和内存开销。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/util/pathnode.c` | `add_path` | 对新候选执行多维支配判定、删旧项或按顺序插入 |
| `src/backend/optimizer/util/pathnode.c` | `set_cheapest` | 从 surviving pathlist 中填充 startup/total/parameterized 代表 |
| `src/backend/optimizer/util/pathnode.c` | `compare_path_costs` | 按 disabled-node 优先级及指定 startup/total 准则做精确排序比较 |

## 与主核 / AI 的关系

暂无直接对应。可将其理解为保留“对后续运算仍有价值”的多方案，而非每步贪心选一个局部赢家；不能据此推断主核具有同样的成本维度或模糊比较常量。

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 说明规划器在执行前选择计划的职责。
- [PostgreSQL 官方源码：pathnode.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/util/pathnode.c) —— 支撑路径支配、成本比较与 cheapest 字段设置。
