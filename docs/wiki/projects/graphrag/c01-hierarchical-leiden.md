---
project: graphrag
feature: hierarchical-leiden
title: 层次 Leiden 社区划分
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/graphs/hierarchical_leiden.py
    symbols: [hierarchical_leiden, first_level_hierarchical_clustering]
  - path: packages/graphrag/graphrag/graphs/hierarchical_leiden.py
    symbols: [final_level_hierarchical_clustering]
---

# 层次 Leiden 社区划分

## 一句话职责

把一张实体关系图切成多层的嵌套社区（底层是细粒度簇、上层是粗粒度簇），每个簇带一个父簇指针，从而让「全局性问题」可以在不同抽象层级上被回答。

## 机制是什么

graphrag 与普通 RAG 最根本的差别就在这一步。普通 RAG 检索的是**文本块**，所以「这个数据集的主要主题是什么」这类问题无法回答——没有任何单个文本块包含全局视角。

graphrag 的答案是：**先用图算法把实体聚类，再对每个簇生成一份报告，查询时检索报告而不是文本块**。于是「全局性问题」变成了「读若干份已经总结好的社区报告」。

这个设计的核心依赖是**层次**。单层聚类只能给出一种粒度：太细（每簇三五实体）总结不出主题，太粗（整个图一个簇）等于没切。层次 Leiden 一次运行同时产出多层——底层簇小而精确，上层簇大而概括。查询时可以从任一层取，或者把多层一起喂给模型。

## 怎么实现的

**`hierarchical_leiden` 是一个极薄的封装。** 它只做一件事：把参数固定成 graphrag 需要的那组值，然后调 `graspologic_native` 的实现。固定的参数里最值得注意的是三个：

`max_cluster_size=10`（默认）是**软上限**——它不是硬性约束，而是递归分裂的触发条件：一个簇超过这个规模就往下再分一层。这才是「层次」的来源，不是跑三次 Leiden，而是**同一次运行里对过大的簇继续分裂**。

`resolution=1.0` 是模块度优化的分辨率参数，固定在原位而不暴露给配置。`randomness=0.001` 与 `iterations=1` 一起说明这次运行**接受轻微随机性且只迭代一轮**——Leiden 的多轮迭代能提升模块度，但耗时随轮数增长；graphrag 选的是「一次跑完，靠 seed 保可复现」。

`use_modularity=True` 说明分裂判据是模块度增益而不是别的目标函数。

**`seed` 默认是 `0xDEADBEEF`。** 这个值出现在函数签名里而不是配置里，说明可复现性被当成默认要求而不是可选项。

**两个访问器体现了「多层」的具体形态。** `first_level_hierarchical_clustering` 取 `level == 0` 的条目，`final_level_hierarchical_clustering` 取 `is_final_cluster` 为真的条目。这两个判据不同——**`level == 0` 是「最粗那一层」，`is_final_cluster` 是「该节点所在的最后一个簇」**。对大多数节点两者一致；对落在被反复分裂的大簇里的节点，它们指向不同的簇。

**真正把这套算法接到数据上的是 `cluster_graph`。** 它的实现里有两处值得单独看：

**先把图强制变成无向图并去重。** `lo = min(source, target)` / `hi = max(...)` 把每条边的两端排序成规范形式，再 `drop_duplicates(keep="last")`。注释点明了这是为了**复刻 NetworkX 的语义**：NX 在加入反向重复边时保留最后一条的属性。这一步的重要性在于——如果 `(A,B)` 与 `(B,A)` 两条边权重不同，谁留下决定了聚类结果。选 `keep="last"` 是跟着 NX 走，不是随便选的。

**可选地先取最大连通分量**（`stable_lcc`）。这是「稳定」二字的来源：孤立的小碎片会让 Leiden 产出大量无意义的单点社区，去掉它们能让社区更有意义，也能让结果在数据小幅变动时更稳定。

**排序再交给算法。** `sorted(zip(...))` 那一行不是装饰：**Leiden 的实现对输入顺序敏感**（同样的图不同边序可能给出不同划分）。排序把输入规范化，让「同样的图 + 同样的 seed」真的产出同样的结果。`strict=True` 让 zip 在长度不匹配时报错而不是静默截断。

**层级的记录方式是「簇 id → 父簇 id」。** `hierarchy[partition.cluster] = partition.parent_cluster or -1`。用 `-1` 而不是 `None` 是因为下游要把它写进 DataFrame 的整数列。**顶层簇的父是 `-1`**，这个哨兵值决定了「哪一层是根」。

`cluster_graph` 最后把「层 → 簇 → 节点列表」重新组织成 `(level, cluster_id, parent_id, nodes)` 四元组列表——这正是后续社区报告生成所需的形状：报告按簇生成，而 `parent_id` 让报告之间也能建立从属关系。

## 关键取舍

**源码事实：** `hierarchical_leiden` 固定 `resolution=1.0`、`randomness=0.001`、`iterations=1`、`use_modularity=True`，不通过配置暴露。

**源码事实：** `max_cluster_size`（默认 10）是**软上限**：超过就继续分裂一层，这是层次结构的来源。

**源码事实：** `seed` 默认 `0xDEADBEEF`，写死在签名里而非配置里。

**源码事实：** 边在进入算法前被规范化成无向形式（两端排序）并 `drop_duplicates(keep="last")`，注释说明这是为复刻 NetworkX 保留最后一条边属性的语义。

**源码事实：** 边列表在交给 Leiden 前被 `sorted`，`zip(..., strict=True)` 保证长度一致。排序的必要性来自「Leiden 对输入顺序敏感」。

**源码事实：** 层级的父指针用 `-1` 表示「无父」，因为要写进整数列。

**架构推论：** 用「过大的簇继续分裂」而不是「跑多次不同分辨率的 Leiden」来构造层次，关键好处是**一致性**：多次独立运行会产生互不嵌套的划分，而分裂式运行天然保证上层簇是下层簇的并集。代价是每层的粒度不完全均匀（受分裂过程影响），但对于「总结主题」这个用途，嵌套一致性比粒度均匀更重要。

**架构推论：** 排序 + 固定 seed 这套组合说明作者把「可复现」当成硬要求。这在知识图谱构建里不是洁癖：**同一份文档重跑索引应该得到同一张图**，否则任何增量更新都会在旧图上叠加不一致的划分。代价是放弃了并行化与随机优化的收益。

**架构推论：** 强制无向 + 取最大连通分量，是把「图算法需要的输入假设」在边界处显式满足的做法。Leiden 本身对有向图与孤立点有定义，但那些定义在「实体关系图」这个语境下不一定有意义（A 提到 B 与 B 提到 A 该不该算不同关系？单点社区值得生成报告吗？）。**在调用点消除歧义比在读结果时修正便宜得多。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `hierarchical_leiden` | 固定参数的 Leiden 调用 |
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `first_level_hierarchical_clustering` | 取最粗一层（`level == 0`） |
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `final_level_hierarchical_clustering` | 取每个节点的最终簇 |
| `packages/graphrag/graphrag/index/operations/cluster_graph.py` | `cluster_graph` | 图 → 四元组社区列表 |
| `packages/graphrag/graphrag/index/operations/cluster_graph.py` | `_compute_leiden_communities` | 无向化、去重、取 LCC、排序、建父子映射 |
| `packages/graphrag/graphrag/graphs/stable_lcc.py` | `stable_lcc` | 稳定地取最大连通分量 |
| `packages/graphrag/graphrag/config/models/cluster_graph_config.py` | `ClusterGraphConfig` | `max_cluster_size` 与 `use_lcc` 的配置面 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 文档：索引](https://microsoft.github.io/graphrag/index/overview/)
- [Leiden 算法论文（Traag et al., 2019）](https://www.nature.com/articles/s41598-019-41695-z)
- [graphrag 源码：hierarchical_leiden.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/graphs/hierarchical_leiden.py)

## 与主核 / AI 的关系

社区划分与主核（多模态数据库内核）的**索引构建**是同一类问题：都是「把数据重新组织成适合查询的结构」。

**聚类的图 vs B-tree。** 两者都在给数据建「跳表式」的抽象层：B-tree 的内层节点是叶子的汇总，社区的上层簇是下层簇的并集。差别在**汇总依据**：B-tree 按键的**全序**切，社区按**连接强度**（模块度）切。全序是有客观答案的（键值大小），模块度没有——同一个图在不同分辨率下都算「正确划分」。**这是确定性索引与非确定性索引的分野**：数据库索引可证明等价，社区划分只能靠质量指标评价。

**层次与数据库的多级索引。** 数据库的多级索引（例如 B+tree 的三层）是为「精确查找」服务的，层数由数据量自动决定。社区的层次是为「不同抽象粒度的问题」服务的，层数由 `max_cluster_size` 触发分裂决定——**同样的数据结构，服务的目的完全不同**：一个是导航，一个是概括。

**「父指针用 -1 而不是 None」这个细节**在数据库里有大量同构：外键用 0 或 -1 表示「无父」，树结构的根节点用自引用。原因都是一样的——**空值的表示要么进类型系统（可空列），要么进哨兵值（-1）**，进哨兵值的好处是存储紧凑、索引友好，代价是每个读取点都要知道哨兵的语义。

**锁 vs 复现性。** graphrag 用排序 + 固定 seed 保证「同输入同输出」，这在概念上等同于数据库的**确定性执行**（replay-based replication 依赖它）。差别是数据库的确定性要求针对任意并发交错，而这里只要针对单次批处理——**门槛低得多，所以一个 sort 就够了**。

对 agent 工程的直接价值有两条。其一：**「全局性问题」在纯 RAG 里无解**，因为没有任何文本块包含全局视角；先聚类再总结是唯一可行的路径。其二：**层次结构的价值在于查询时可以选择抽象层级**，这与人类回答「具体哪家公司」和「这个行业怎么样」用不同粒度的知识是同一件事。
