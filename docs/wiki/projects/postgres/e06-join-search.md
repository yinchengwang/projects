---
project: postgres
feature: join-search
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/path/joinrels.c
    symbols: [join_search_one_level, make_join_rel]
  - path: src/backend/optimizer/path/joinpath.c
    symbols: [add_paths_to_joinrel]
  - path: src/backend/optimizer/path/allpaths.c
    symbols: [standard_join_search]
---

# 连接枚举与算法候选

## 一句话职责

连接搜索逐层构造合法的关系组合，并为每个组合生成可比较的连接算法路径。

## 机制是什么

标准搜索使用动态规划：`join_rel_level[k]` 保存由 k 个初始 jointree 项组成的连接关系，逐层尝试关联连接条件或连接顺序约束允许的组合。`join_is_legal` 在 `make_join_rel` 内检查外连接、半连接和 LATERAL 的限制；`RelOptInfo` 以 relids 集合作为 joinrel 身份，多个构造顺序可汇入同一关系对象。

在每个合法 joinrel 上，`add_paths_to_joinrel` 再考虑 nestloop、mergejoin、hashjoin 等实现并按成本、路径键和参数化信息保留非支配候选。搜索不是对任意 SQL 都穷举所有排列：约束使部分顺序不合法，启发式延迟无条件笛卡尔组合。规划规模过大时调用端可改走 GEQO；本页不讨论其遗传算法细节。

## 怎么实现的

1. `standard_join_search` 将 `initial_rels` 放进 `join_rel_level[1]`，从二元层开始逐层调用 `join_search_one_level`，处理完该层关系后为其生成分区连接/并行 gather 等路径并确定 cheapest。
2. `join_search_one_level` 优先对有 joininfo、等价类连接或顺序约束的关系配对；满足条件时也考虑 bushy 组合。若当前层完全无解，最后尝试左深/右深笛卡尔组合；存在特殊连接约束时允许中间层暂时空缺。
3. 每次候选配对调用 `make_join_rel`，先合并 relids 并由 `join_is_legal` 判定方向、特殊连接类型及 LATERAL 参数关系；非法候选返回 NULL，不构造路径。合法后创建或查找对应 joinrel 与 RestrictInfo 列表。
4. 对 INNER join 两种输入方向都会尝试；外/半/反连接依据语义决定可用方向，FULL JOIN 只有可支持其条件的实现。特殊情况可先唯一化半连接 RHS 再按普通连接处理。
5. `add_paths_to_joinrel` 选择可用 merge 条件并计算参数来源约束，调用不同路径构造逻辑：nestloop 可利用参数化内侧路径，mergejoin 需要可用排序或加排序，hashjoin 根据哈希连接条件建立候选；可并行及 FDW 路径也按条件加入。
6. 成本下界预检和 `add_path` 剔除被现有路径支配者；每个 joinrel 同一规划调用内由 `PlannerInfo` 持有，多个等价组合累计各自候选路径，不涉及跨后端同步锁。

## 关键取舍

- **源码事实：**逐层 DP 会生成不同子集关系，bushy 组合只在有相关连接条件或顺序约束时考虑；笛卡尔积延迟到无连接边或最后补救阶段。**架构推论：**限制组合规模以换取规划时间可控；全面枚举无连接条件的 bushy 子树会显著扩大候选数，而过度限制又可能漏掉仅能以笛卡尔子问题满足的合法计划。
- **源码事实：**`join_is_legal` 检查 `SpecialJoinInfo` 的最小左右关系集和 LATERAL 依赖，路径生成还拒绝参数化依赖与当前外连接输出冲突的候选。**架构推论：**连接算法候选必须服从 SQL 语义和可供参数顺序；若只按估算成本排列而不验证这些依赖，低成本方案也可能无法保持空值扩展或相关引用语义。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/path/joinrels.c` | `join_search_one_level` | 枚举指定关系数层级的合法连接组合 |
| `src/backend/optimizer/path/joinrels.c` | `make_join_rel` | 语义验证后取得 joinrel 并触发路径生成 |
| `src/backend/optimizer/path/joinpath.c` | `add_paths_to_joinrel` | 为已确定的 joinrel 组合算法和路径属性 |
| `src/backend/optimizer/path/allpaths.c` | `standard_join_search` | 初始化分层集合并驱动标准动态规划搜索 |

## 参考

- [PostgreSQL 文档：查询规划器](https://www.postgresql.org/docs/current/planner-optimizer.html) —— 规划器选择查询执行方案
- [PostgreSQL 源码：joinrels.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/path/joinrels.c) —— 连接顺序合法性与关系枚举

## 与主核 / AI 的关系

动态规划枚举关系子集、比较不同 join 算法，是数据库优化器通用概念；主核是否使用同样的合法性结构与算法需单独核实。暂无直接 AI 对应。
