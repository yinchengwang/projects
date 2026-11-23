---
project: postgres
feature: subquery-transform
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/prep/prepjointree.c
    symbols: [pull_up_sublinks, pull_up_subqueries]
  - path: src/backend/optimizer/plan/subselect.c
    symbols: [convert_ANY_sublink_to_join, make_subplan]
---

# 子查询与子链接转换

## 一句话职责

子查询转换在语义允许时把嵌套查询并入外层连接树，否则保留独立子计划接口。

## 机制是什么

`SubLink` 出现在 WHERE 或 JOIN 条件时，某些 ANY/EXISTS 可被转成半连接或反连接，消除表达式子查询边界，让后续连接优化器统一考虑。FROM 子查询也可被拉平，但必须满足简单子查询条件并维护 Var、PlaceHolderVar、外连接空值标记等语义。不是所有嵌套结构都能拉平：例如任意布尔上下文中的 ANY、引用不安全外侧关系的相关项或语义敏感的子查询都需保留。

转换失败不代表查询错误。`make_subplan` 递归调用 `subquery_planner`，构造 `SubPlan` 或 `InitPlan`，相关参数通过 `parParam`/`args` 传入；无相关参数且结果形态合适时可以用执行参数返回结果。这里不负责最终连接顺序，也不负责子计划执行时的缓存与重扫细节。

## 怎么实现的

1. `subquery_planner` 在表达式预处理前调用 `pull_up_sublinks`，仅沿 jointree 的合适 qual 位置递归检查 ANY/EXISTS；外连接 ON 中还按可用关系集合限制拉升位置。
2. `convert_ANY_sublink_to_join` 检查相关 Var 是否都在允许集合、测试表达式是否引用外层关系，以及易变表达式等条件；`NOT ANY` 还需证明相关值非空，否则不能以反连接替代 SQL 三值逻辑。
3. 成功后子查询作为新的 RTE 加入外层 range table，测试表达式中的子查询 Param 替换成输出 Var，形成 `JOIN_SEMI` 或 `JOIN_ANTI`；调用者将 JoinExpr 插入 jointree，并递归处理新加入的子树。
4. `pull_up_subqueries` 递归判断 FROM 子查询是否简单可合并，拉升时替换输出 Var 并维护 nullingrels、lateral 与占位表达式；随后简单 UNION ALL 可改为 appendrel。不能安全拉平的子查询保留为 RTE。
5. 表达式阶段 `make_subplan` 复制输入 Query（规划会改写它），依据类型设定预期取数比例并递归规划；收集父层 `plan_params` 后构造子计划。独立 EXISTS/标量/数组等可成为 InitPlan，相关项保留 SubPlan；无关 ANY 在条件满足时可用 hash table，否则可能加 Materialize。
6. 子计划、路径和子层 `PlannerInfo` 追加到共享 `PlannerGlobal` 列表，最终引用修整时同步处理；规划失败时由子规划的内存上下文清理临时结构。

## 关键取舍

- **源码事实：**`pull_up_sublinks` 只在顶部 qual/可安全插入的位置转换；`convert_ANY_sublink_to_join` 对 `NOT IN` 采取可空性证明，否则返回失败。**架构推论：**宁可保留 SubPlan，也不因忽略 NULL 语义造成结果变化；不受限地拉升会让外连接或 OR/NOT 上下文产生错误的真假/未知值行为。
- **源码事实：**`subselect.c` 对可执行为 InitPlan 的无相关 EXISTS、标量或数组子查询，以 PARAM_EXEC 表示结果；相关子计划携带父参数，而 ANY/ALL 仍需逐外层行检验，可选择哈希或物化。**架构推论：**两种表示分别适合一次求值和逐行依赖；强行统一为 InitPlan 会丢失相关性，统一为逐行 SubPlan 则会增加重复执行成本。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/prep/prepjointree.c` | `pull_up_sublinks` | 驱动 qual 中可转换子链接的安全位置搜索 |
| `src/backend/optimizer/prep/prepjointree.c` | `pull_up_subqueries` | 递归尝试合并 FROM 子查询并替换其输出引用 |
| `src/backend/optimizer/plan/subselect.c` | `convert_ANY_sublink_to_join` | 验证 ANY 相关性与空值安全并生成半/反连接 |
| `src/backend/optimizer/plan/subselect.c` | `make_subplan` | 子查询递归规划并选择 SubPlan/InitPlan 表示 |

## 参考

- [PostgreSQL 文档：子查询表达式](https://www.postgresql.org/docs/current/functions-subquery.html) —— EXISTS、IN/ANY 的 SQL 语义
- [PostgreSQL 源码：subselect.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/plan/subselect.c) —— 子计划转换与 InitPlan 构造

## 与主核 / AI 的关系

半连接、反连接和子计划是主核 SQL 查询优化常见概念；主核实现需独立核对。暂无直接 AI 对应。
