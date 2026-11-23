---
project: postgres
feature: cost-cardinality
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/path/costsize.c
    symbols: [cost_seqscan, set_baserel_size_estimates, calc_joinrel_size_estimate]
  - path: src/backend/optimizer/path/clausesel.c
    symbols: [clauselist_selectivity, clause_selectivity]
---

# 成本与基数估算

## 一句话职责

成本与基数模型把统计信息、谓词选择率、输出宽度和执行工作量换算为每条 Path 的行数与 startup/total 成本估计。

## 机制是什么

`costsize.c` 为扫描、连接、排序、聚合、并行等候选估算 I/O、CPU、表达式和节点额外工作的成本，同时估计路径输出行数；`clausesel.c` 依据表达式类别、操作符选择率函数、单列/扩展统计和连接类型估算谓词通过率。典型基础关系行数为基表 tuples 乘 base restriction 选择率；连接基数以两侧行数乘 join 条件选择率，并按外连接、半连接或反连接语义调整。

本页只负责估值，不负责生成路径候选（e05/e06）、非支配候选筛选（e08）或最终计划树构造（e12）。`selfuncs` 中的类型/操作符选择率估计器是选择率接口的 ADT 域协作点；这里说明 planner 如何调用它们，不重写其数据类型专用分布模型。估值是优化器用于相对比较的预测，不是执行器工作量保证。

## 怎么实现的

1. 基表目录信息先提供 pages、tuples、宽度、统计信息和限制条件。`set_baserel_size_estimates` 调 `clauselist_selectivity` 估算所有 base restrictions 的合并通过率，写入 `RelOptInfo.rows`，计算限制表达式成本，并估算输出 tuple width。
2. `clauselist_selectivity` 对单条条件直接调用 `clause_selectivity`；多条件时先识别单关系扩展统计可处理的子集，剩余条件再估值。对可识别的同变量范围条件组合上下界，避免把 `< hi` 与 `> lo` 错误地当成独立概率简单相乘；其余未解释条件使用选择率乘积这一近似。
3. `clause_selectivity` 按常量、Var、Param、NOT/AND/OR、操作符、函数、数组与行比较等节点分派：操作符经 restriction 或 join estimator 接口到类型相关选择率函数；`RestrictInfo` 可缓存普通/外连接选择率，伪常量与已证明冗余条件分别按其语义处理。
4. `cost_seqscan` 用 relation pages 乘 tablespace sequential page cost 得磁盘成本，用扫描 tuples 乘 tuple 与 qual per-tuple cost 得 CPU 成本，并按输出 rows 计目标列表投影。并行时通过 parallel divisor 分摊 CPU、将 path rows 调为每 worker 处理估计；I/O 成本暂不按 worker 分摊。
5. `calc_joinrel_size_estimate` 先从可匹配外键约束的等值条件提取 FK 选择率，避免多列外键被独立性假设低估/高估；再区分 outer join 自身 joinquals 与 pushed-down quals，按 INNER/LEFT/FULL/SEMI/ANTI 语义计算并调用 `clamp_row_est` 限制不合理结果。
6. 排序成本根据估计输入字节与 `sort_mem` 选择内存 quicksort、bounded top-K heap 或外部排序成本；节点把估值写回 Path，之后由路径比较机制使用。参数化路径使用其 ParamPathInfo 的 `ppi_rows`，不能把无参数化基表行数直接当成参数过滤后的输出量。

## 关键取舍

- **源码事实：**多谓词通常把剩余选择率相乘，只有可用扩展统计或可识别的范围对时才修正依赖性。假设独立计算开销低，却会在相关列谓词上偏离真实基数；完整联合分布建模需要统计数据支持和额外分析成本。
- **源码事实：**连接规模对外键匹配条件单独估算，原因是多列外键上的独立选择率假设表现差；其他条件仍依通用估计器。架构推论：这是局部使用约束语义纠正估值，不是对任意连接相关性的通用模型。
- **源码事实：**`cost_seqscan` 并行分摊 CPU 但假设磁盘 I/O 不可摊薄，因为系统已通常预取；若按 worker 平均摊薄 I/O，可能过度乐观，若实际并行 I/O 确有收益则该模型也可能保守。
- **源码事实：**排序成本用 `sort_mem` 与行宽估算内存/外部排序，并对有用 LIMIT 采用保留前 K 行的 bounded heap 模型；把所有排序按全量外排计费会惩罚 top-N，始终按内存排序则漏计临时文件 I/O。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/path/costsize.c` | `cost_seqscan` | 将 relation 页/元组、qual 与 target 成本写入顺序扫描 Path |
| `src/backend/optimizer/path/costsize.c` | `set_baserel_size_estimates` | 由表元组基数乘限制选择率，设置行数、限制成本及宽度 |
| `src/backend/optimizer/path/costsize.c` | `calc_joinrel_size_estimate` | 将 join/FK/pushed-down 选择率按连接类型换算为输出行数 |
| `src/backend/optimizer/path/clausesel.c` | `clauselist_selectivity` | 聚合谓词选择率并尝试统计依赖、范围约束组合 |
| `src/backend/optimizer/path/clausesel.c` | `clause_selectivity` | 为单表达式按节点和上下文分派选择率估计器 |

## 与主核 / AI 的关系

暂无直接对应。估算模型的通用作用是为候选提供可比较的代价指标；主核若有规划成本模型，具体单位、参数与统计口径需单独从源码确认。

## 参考

- [PostgreSQL 文档：规划器使用的统计信息](https://www.postgresql.org/docs/current/planner-stats.html) —— 支撑统计信息、扩展统计与基数估算背景。
- [PostgreSQL 官方源码：costsize.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/costsize.c) —— 支撑扫描、连接、排序成本与关系行数计算。
- [PostgreSQL 官方源码：clausesel.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/clausesel.c) —— 支撑谓词选择率合并、范围配对和扩展统计调用。
