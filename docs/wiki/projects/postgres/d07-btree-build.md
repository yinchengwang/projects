---
project: postgres
feature: btree-build
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/nbtree/nbtsort.c
    symbols: [btbuild, _bt_parallel_build_main]
---

# B-tree 构建

## 一句话职责

B-tree 构建把 heap 中的索引键收集、排序并批量形成有序叶页，再逐层建立内部搜索结构。

## 机制是什么

`btbuild` 是 AM 的构建入口，扫描 heap 计算索引表达式/属性值，形成待排序的索引 tuple；排序结果随后顺序写入叶页，并由上层构建过程补齐父级层次。唯一索引还需处理重复键校验，NULLS NOT DISTINCT 配置会改变重复判定语义。

这条 bulk build 路径不同于逐项 `btinsert`：它不通过常规插入路径为每个 tuple 单独搜索树和逐次分裂。并行构建把 heap 扫描与排序分配给 leader/workers，再由 leader 负责汇总排序结果及完成索引页输出；是否并行由索引能力与构建环境条件决定，不能把并行 callback 存在等同于每次构建都并行。

## 怎么实现的

1. `btbuild` 初始化 `BTBuildState`，记录唯一性策略、heap/index 关系、排序 spool、tuple 计数和并行 leader 状态，并确认目标索引尚无已写入页。
2. 根据 `IndexInfo` 和并行可用性选择普通或并行构建路径；heap 扫描回调计算索引属性/表达式，创建带 heap TID 的 IndexTuple 并送入 tuplesort spool。唯一索引可能使用第二个 spool 暂存重复检查所需数据。
3. 普通构建将 spool 排序；内存不足时 tuplesort 按其外部排序策略落盘。并行路径由 `_bt_parallel_build_main` 在 worker 中打开共享关系资源，读取共享状态和 tuplesort 配置，参与 heap 扫描与排序。
4. leader 等待 worker 完成并合并并行排序产物，然后按键顺序消费 tuple。排序避免逐项随机定位树页，使构建过程可批量填充连续叶页。
5. 输出叶层时按页容量与 B-tree 填充规则选择页面边界，生成必要的 high key、sibling links 和页统计；重复键可形成 posting list 表示，唯一性约束在有序流中检查。
6. 完成叶层后构建父层 downlink，直至根页与 metapage 根信息确定；写入相关页面的 WAL，并更新 `IndexBuildResult` 中 heap tuple 与有效索引 tuple 统计。worker 失败或构建异常由并行/事务错误路径中止，不留下可用索引状态。

## 关键取舍

- **源码事实：**bulk build 先排序再写页，排序由 tuplesort 承担；其内存不足可使用外部排序。**架构推论：**以排序工作空间和可能的临时 I/O，换取顺序页输出与较少随机树搜索；逐项插入免去全量排序，但每条记录都需定位并可能触发 split，通常会放大随机访问。
- **源码事实：**`_bt_parallel_build_main` 让 worker 执行共享构建任务，leader 协调并消费排序数据；并行能力由 AM callback 与构建条件共同决定。**架构推论：**并行化 heap 扫描/排序可缩短大索引构建时间，但需共享状态、worker 启动/同步与合并开销；小表上这些固定成本可能超过收益。
- **源码事实：**排序构建仍必须执行唯一性检查，并按 NULLS NOT DISTINCT 与索引定义区分重复键行为。**架构推论：**排序只改变输入顺序与页填充效率，不削弱约束语义；若为了快速 bulk load 跳过检查，将允许与普通插入语义不一致的索引内容。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/nbtree/nbtsort.c` | `btbuild` | 配置扫描、spool、排序及最终层级构建。 |
| `src/backend/access/nbtree/nbtsort.c` | `_bt_parallel_build_main` | 初始化并执行 worker 侧共享并行构建任务。 |

## 参考

- [PostgreSQL 文档：并行索引构建](https://www.postgresql.org/docs/current/parallel-plans.html) —— 说明并行规划与索引创建的并行处理语境。
- [PostgreSQL 官方源码：B-tree 排序构建](https://github.com/postgres/postgres/blob/master/src/backend/access/nbtree/nbtsort.c) —— 对照 spool、排序和并行 worker 构建过程。

## 与主核 / AI 的关系

暂无直接对应。可借鉴“先排序、后顺序落页”的 bulk-load 方案；主核是否并行构建需由其线程/事务、临时排序和 WAL 协议分别验证。
