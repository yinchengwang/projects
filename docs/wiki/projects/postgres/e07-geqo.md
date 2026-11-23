---
project: postgres
feature: geqo
page_type: feature
schema_version: 1
refs:
  - path: src/backend/optimizer/geqo/geqo_main.c
    symbols: [geqo]
  - path: src/backend/optimizer/geqo/geqo_eval.c
    symbols: [geqo_eval]
  - path: src/backend/optimizer/geqo/geqo_pool.c
    symbols: [random_init_pool, alloc_pool]
---

# 遗传查询优化

## 一句话职责

GEQO 在关系数较多时以遗传搜索近似探索连接顺序，并把优胜染色体转为连接关系。

## 机制是什么

当 `allpaths.c` 判定启用 GEQO 且关系项数量达到阈值时，连接搜索改调 `geqo`，不再执行标准全层动态规划。染色体编码初始关系访问次序，种群保存多种候选；每代选择父本、交叉生成子代并按规划成本评估，优胜个体保留。最终最优 tour 由 `gimme_tree` 按其顺序构建合法 joinrel。

它降低大连接问题的搜索压力，但不保证找到全局最低成本，也不支持标准路径选择中的所有维度：`geqo_eval` 明确不优化部分结果快速取数，也不利用参数化路径信息。关系合法性仍由共同 `make_join_rel` 等接口约束；本机制不替代扫描路径或连接算法生成。

## 怎么实现的

1. `geqo` 获取 planner extension 私有状态并保存初始关系列表，设置随机种子；根据关系数与 effort 或用户配置决定 pool 大小和代数，分配 `Pool` 中每个染色体的 Gene 数组。
2. `random_init_pool` 反复生成随机 tour 并调用 `geqo_eval` 计算适应度；无效个体不占种群位置。连续大量初始尝试仍无有效项时报告错误，避免无限重试。
3. 初始 pool 按 fitness 排序；每代 `geqo_selection` 选择父母，编译期选定的交叉算子产生子代，`geqo_eval` 评估并由 `spread_chromo` 按适应度插入、替换当前最差成员。
4. `geqo_eval` 为每次评估建立临时 MemoryContext，保存并暂时断开既有 join-rel hash，记录 join_rel_list 长度；`gimme_tree` 依据 tour 将关系聚为 clump 并尝试合法合并，必要时强制尝试剩余合法组合。
5. 若能形成单一 joinrel，以其 cheapest total path 的 cost 与 disabled_nodes 作为 fitness；非法 tour 返回最大惩罚。退出时恢复 join_rel_list/hash 并删除临时上下文，避免每个评估的临时路径元数据累积。
6. 主循环结束后取排序首位 chromosome，再调用 `gimme_tree` 构造最终关系；清理 pool、父子染色体和 planner extension 私有状态，返回最优 joinrel。

## 关键取舍

- **源码事实：**遗传搜索用有限 pool/代数近似替代标准 DP；随机初始化会丢弃无效 tour，`geqo_eval` 对无效连接顺序返回最大 fitness。**架构推论：**用不保证最优的搜索换取大规模连接规划的可控开销；若完整枚举可能保留更优解，但连接组合增长会拖慢优化器。
- **源码事实：**每次适应度计算使用独立临时 MemoryContext，并恢复 `join_rel_list` 和原 hash；`gimme_tree` 对关系按 desirable join 合并，最后才尝试强制合并剩余 clump。**架构推论：**这控制重复评估的内存与笛卡尔候选生成；若评估中不恢复状态会让候选互相污染、内存随种群评估次数增长，若一开始总强制合并则会耗费更多路径构造预算。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/optimizer/geqo/geqo_main.c` | `geqo` | 管理种群初始化、迭代、遗传算子和最佳 tour 输出 |
| `src/backend/optimizer/geqo/geqo_eval.c` | `geqo_eval` | 在隔离规划临时状态下将 tour 评为成本适应度 |
| `src/backend/optimizer/geqo/geqo_pool.c` | `alloc_pool` | 分配固定种群容量及 chromosome Gene 存储 |
| `src/backend/optimizer/geqo/geqo_pool.c` | `random_init_pool` | 随机填充有效个体并排除非法 tour |

## 参考

- [PostgreSQL 文档：查询规划器配置](https://www.postgresql.org/docs/current/runtime-config-query.html) —— GEQO 相关规划配置入口
- [PostgreSQL 源码：geqo_main.c](https://github.com/postgres/postgres/blob/master/src/backend/optimizer/geqo/geqo_main.c) —— 遗传查询优化主循环

## 与主核 / AI 的关系

遗传算法作为连接顺序近似搜索可与主核优化策略作概念对照；主核是否存在相同机制需核验源码。暂无直接 AI 对应。
