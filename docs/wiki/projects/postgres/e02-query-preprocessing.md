---
project: postgres
feature: query-preprocessing
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/prep/prepjointree.c
    symbols: [preprocess_relation_rtes, preprocess_function_rtes]
  - path: src/backend/optimizer/prep/preptlist.c
    symbols: [preprocess_targetlist]
  - path: src/backend/optimizer/prep/prepagg.c
    symbols: [preprocess_aggrefs]
---

# 查询树预处理

## 一句话职责

查询树预处理把解析重写后的 Query 整理成后续优化能够统一分析的关系、表达式和目标列结构。

## 机制是什么

预处理不是路径选择，而是为优化器后续识别关系、约束和表达式语义准备输入。关系 RTE 处理会检查继承标记、收集 NOT NULL 属性并展开虚拟生成列；函数 RTE 会做常量化简并尝试内联为子查询。目标列表处理依语句类型补齐或调整目标列，也会加入执行所需的 junk 列；聚合预处理则为相同或可共享状态的 Aggref 编号。

这些步骤有明确边界：此机制不决定扫描/连接算法，也不执行聚合计算。它允许原地改写 Query 与 Aggref；后续 `PlannerInfo` 构建和路径生成依赖整理后的表达式与目标列，执行器最终消费计划所需的输出列。

## 怎么实现的

1. `preprocess_relation_rtes` 遍历初始 range table，只处理关系 RTE；以已有锁状态打开关系，检查继承子类、缓存 not-null 属性，并在需要时以生成表达式替换虚拟生成列引用。
2. 处理 jointree 与子链接之前，`preprocess_function_rtes` 对函数 RTE 的表达式常量化简，并在可行时把 SQL 函数等改写为子查询形式，使新产生的查询也能进入后续优化。
3. `preprocess_targetlist` 按命令类型整理目标列：INSERT 按表属性顺序补齐缺项，UPDATE 保存目标列号并重编号；UPDATE/DELETE/MERGE 等还注册行身份列，rowmark、MERGE action 与 RETURNING 也可能要求追加 resjunk 项。
4. `preprocess_aggrefs` 遍历表达式并从 `pg_aggregate` 读取转移/最终函数与初始值，解析多态转移类型。`AggInfo` 表示聚合调用，`AggTransInfo` 表示可复用的转移状态；将结果编号写回 Aggref 的 `aggno`、`aggtransno`。
5. 合并仅在参数、排序、DISTINCT、FILTER、转移函数及状态等条件满足时进行；含易变函数或不可共享 final function 状态时不复用。信息挂在当前 `PlannerInfo` 生命周期内供成本估算和上层规划使用。

## 关键取舍

- **源码事实：**目标列表预处理加入 `ctid`、整行及其他 resjunk 信息，以适配更新删除、行锁和 EvalPlanQual。**架构推论：**把执行辅助数据显式放进目标列表让后续计划节点可传递行身份；若完全等到执行期临时推导，继承子表和行版本重查会缺少规划时已确定的列约定。
- **源码事实：**`prepagg.c` 拒绝对含 volatile 表达式的聚合复用，并要求 `aggorder`、`aggdistinct`、`aggfilter` 等一致；状态可共享时多个 final function 仍可分别执行。**架构推论：**保守限制避免改变调用次数及聚合语义；不做合并会重复状态转移成本，放宽条件则可能改变可观察副作用或状态内容。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/prep/prepjointree.c` | `preprocess_relation_rtes` | 读取关系元数据并展开规划期虚拟生成列 |
| `src/backend/optimizer/prep/prepjointree.c` | `preprocess_function_rtes` | 常量化简函数 RTE 并尝试内联转换 |
| `src/backend/optimizer/prep/preptlist.c` | `preprocess_targetlist` | 整理语句目标列和执行所需 junk 项 |
| `src/backend/optimizer/prep/prepagg.c` | `preprocess_aggrefs` | 识别聚合复用关系并标注聚合与状态编号 |

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 查询树经优化后形成计划
- [PostgreSQL 源码：prepagg.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/prep/prepagg.c) —— 聚合状态预处理与共享条件

## 与主核 / AI 的关系

查询树标准化和聚合状态去重可作为主核查询规划的概念参照；实现并不相同。暂无直接 AI 对应。
