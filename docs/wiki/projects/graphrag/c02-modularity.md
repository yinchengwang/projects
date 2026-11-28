---
project: graphrag
feature: modularity
title: 模块度计算
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/graphs/modularity.py
    symbols: [modularity, calculate_graph_modularity, calculate_root_modularity]
  - path: packages/graphrag/graphrag/graphs/modularity.py
    symbols: [calculate_leaf_modularity, calculate_lcc_modularity]
---

# 模块度计算

## 一句话职责

给层次 Leiden 产出的划分打一个质量分，但它**一次给出五个不同口径的分**而不是一个：同一个簇映射可以在「最粗一层」与「最细一层」、在「整图」与「最大连通分量」、在「单次划分」与「按连通分量大小加权」这几种组合下分别评价——因为「这个划分好不好」这个问题在层次聚类里本身就有多个正确答案。

## 机制是什么

模块度（Newman）的定义是：**社区内部的实际边权重，减去「若边随机连接时该社区内部应有的边权重」**。公式上等价于把每个社区的贡献相加，而单个社区的贡献是

```
(intra - resolution × d_c² / 2m) / 2m
```

其中 `intra` 是社区内部节点的度之和（内部边算两次）、`d_c` 是社区内所有节点的度之和、`m` 是图的边权重总和。取值域是 `(-1, 1)`：全图一个社区得 0、每个点各成一社区得负、划分贴紧真实结构得正。

c01 讲的层次 Leiden 负责「怎么切」，这一页负责「切得好不好」。而这里真正的设计判断是：**一个数不够**。

原因在层次聚类上体现得最直接。模块度**奖励「社区少而大」**——把整个图当一个社区，模块度恰好 0；把图切成两个均衡的社区，模块度可能到 0.4。所以：

- **最粗一层（root）**天然容易拿到更高的模块度，因为簇更少更大；
- **最细一层（leaf）**因为簇更小更多，模块度必然被压低。

于是「root 的模块度是 0.6」这件事不能推出「划分是好的」——它可能只是「簇很少」。反过来「leaf 的模块度是 0.2」也不能推出「划分是坏的」——细粒度本来就该低。**只有把两个数放在一起看，才能判断这次分裂是否值得**：如果 leaf 相对 root 掉得很少，说明细分出来的子社区是真实的；如果掉得很多，说明分裂只是把一致的社区硬切开了。

这就是为什么 `calculate_root_modularity` 与 `calculate_leaf_modularity` 是两个函数而不是一个带 `level` 参数的函数——它们回答的是**两个不同的问题**，而调用方（以及读报告的运维）需要同时看到两个答案。

在此之上还有第二根轴：**评价范围**。孤立的小碎片会让模块度失真（一个只有两条边的连通分量内部再怎样紧密也是噪声），所以 `calculate_lcc_modularity` 只在最大连通分量上评价。于是形成一张二维表：

| | 整图 | 仅最大连通分量 |
|---|---|---|
| 最粗一层 | `calculate_graph_modularity(use_root_modularity=True)` | `calculate_lcc_modularity(use_root_modularity=True)` |
| 最细一层 | `calculate_graph_modularity(use_root_modularity=False)` | `calculate_lcc_modularity(use_root_modularity=False)` |

四个组合由两个「层级」函数 × 两个「范围」函数交叉得到，而两个入口函数各自只有一个布尔开关——把二维选择压成两个独立的一维选择，是这套 API 的基本形状。

第五个口径 `calculate_weighted_modularity` 是**另一个维度的思路**：不再挑一个范围，而是**对每个连通分量分别算模块度，再按分量节点数加权平均**，且只统计节点数超过 `min_connected_component_size` 的分量。如果一个都不超过，就退化成「整图」。它的理由是：图常常是**多个互不相连的子语料**（几篇不相关的文档抽出的实体各自成岛），此时「整图模块度」会被跨分量的那一大堆缺失边严重惩罚，而加权分量口径把每个岛当成自己的问题来评价。

最后 `calculate_modularity` 是分发器，按 `ModularityMetric` 枚举在 Graph / LCC / WeightedComponents 三者间选，**默认是 `WeightedComponents`**。这个默认值本身就是一句立场：多数真实语料是多岛的，所以默认口径是多岛友好的那个。

## 怎么实现的

**`_modularity_component` 与 `_modularity_components` 的拆分，是这一页唯一真正重要的结构决定。**

`_modularity_component(intra, total, network_degree_sum, resolution)` 只算一个社区，四个参数全是标量，**不碰任何数据结构**。它是纯数学，可以直接对着公式验证。

`_modularity_components(...)` 负责遍历边、累积两个字典、返回 `dict[int, float]` —— **每个社区一个分**。而 `modularity(...)` 只是 `sum(components.values())`。

为什么值得这样拆成三层？三个理由，第三个最实在：

其一，纯函数的边界让公式可以被独立校验（测试文件里那份 NX 参考实现就是逐字对照着它写的）。

其二，`resolution` 沿参数链一路传下来，只在最后一步参与乘法——所以「换分辨率」不需要碰累积逻辑。

其三，**返回字典而不是标量，保留了「哪个社区在拖后腿」这个信息**。一个整图模块度 0.1 是很差的分数，但你可能想知道：是所有社区都平庸，还是某一个巨型社区贡献了 -0.5 而其余都很好？前者说明抽取质量差，后者说明**有一个枢纽把本该分开的东西连起来了**——而后者正是 c03 里「剪掉高度数节点」要解决的情况。标量把这两种情况合并成一个数字，字典没有。

**累积逻辑里三行聚在一起，构成了整页最需要解释的细节。**

```python
if src_comm == tgt_comm:
    if src == tgt:
        degree_sums_within[src_comm] += weight
    else:
        degree_sums_within[src_comm] += weight * 2.0

degree_sums_for[src_comm] += weight
degree_sums_for[tgt_comm] += weight
total_edge_weight += weight
```

四个变量各自在累积什么：

- `degree_sums_within`（社区内部度）：只在两端同社区时累加。非同社区时**什么都不加**——这是对的，因为跨社区的边不是「社区内部的边」。
- 非自环乘 `weight * 2.0`：无向边对**两个端点**的度都有贡献，而这条边在 DataFrame 里只出现一行，所以手工翻倍。
- 自环只加 `weight`（不翻倍）：一个自环在边表里也只有一行，但它的两端是同一个节点——而 `degree_sums_for` 那两行（`src_comm` 与 `tgt_comm` 是同一个社区）会把 `weight` 加**两次**。所以自环在「总度」里算两次、在「内部度」里算一次。这个不对称是刻意的：它复刻 NetworkX 的行为，因为 NX 的 `edges(data=...)` 对自环也只 yield 一次。代码与 NX 逐字对齐，所以这里不能「顺手修正」。
- `total_edge_weight` 是 `m`（边权重总和，自环算一次），而公式里除的是 `2m`——这就是无向图模块度那个经典的「2m」的来源。

**两张字典都用 `defaultdict(float)`，而末尾有一次除以零的守卫。**

```python
if total_edge_weight <= 0.0:
    return dict.fromkeys(communities, 0.0)
```

边全为零权（或者根本没有边）时，`2m = 0` 会让公式除零。这里的处理是**给每个社区返回 0.0**，而不是抛错也不是返回 NaN。理由是语义的：「没有任何边的图，任何划分都不携带信息」——0 是模块度在「没有信息」时的自然取值（全图一社区也是 0）。

注意 `dict.fromkeys(communities, 0.0)` 用的是 `communities = set(partitions.values())`，也就是**划分里声明过的所有社区**，包括只由孤立节点组成的社区。所以「孤立节点组成的社区」会得到一个 0.0 的条目，而不是缺席。这与下一个细节呼应：

**孤立节点在数学上贡献 0，而不是被惩罚。**

`degree_sums_for[comm]` 是 `defaultdict(float)`，所以一个从未在边里出现过的社区查出来是 `0.0`；`_modularity_component(0, 0, m, res)` = `(0 - res × 0)/2m = 0`。所以：

- 一个只含孤立节点的社区：贡献恰为 0；
- 一个既有连通节点又有孤立节点的社区：孤立节点不改变任何一项。

从「模块度应当惩罚松散社区」的角度看，这似乎太宽容了——一个社区里塞满孤立节点应该拉低分数才对。但这是模块度定义的性质而非实现缺陷：模块度只看**度**，孤立节点的度是 0，所以它在分子分母里都无痕。要惩罚它得换目标函数（例如加一个覆盖度项），而那会偏离「与 NX 一致」这个前提。

**无向化 + `keep="last"` 与 c01 完全一致，但这里是第二次实现。**

```python
lo = df[[source_column, target_column]].min(axis=1)
hi = df[[source_column, target_column]].max(axis=1)
df = df.assign(**{source_column: lo, target_column: hi})
df = df.drop_duplicates(subset=[source_column, target_column], keep="last")
```

两端取 `min`/`max` 排序成规范方向，再按 `keep="last"` 去重——与 `cluster_graph` 里那段逐字相同，注释也逐字相同（「matching NX behavior」）。必要性也一样：若 `(A,B)` 与 `(B,A)` 权重不同，谁留下决定了模块度。

**而这段归一化在同一个文件里出现了两次。**

`_df_to_edge_list` 做了一遍（为了产出给 Leiden 的 `list[tuple[str, str, float]]`），`_modularity_components` 又做了一遍（为了遍历 DataFrame）。两者都保留完整四步、都用 `keep="last"`，所以当前行为是一致的。

这是一个真实的维护风险：**两处独立实现必须同步修改**。若将来有人只改 `_df_to_edge_list`（例如换 `keep="first"`），Leiden 会在一种图上聚类、模块度会在另一种图上评分，而两者都不会报错——只会让分数变得无法解释。`_df_to_edge_list` 除了这两个调用点之外没有别的使用者，所以它其实可以退化成「调用一个共享的归一化函数再转 list」；当前没这么写，代价就是这个双份。

**`calculate_root_modularity` 与 `calculate_leaf_modularity` 的差别只有一行，但那行的语义差别很大。**

```python
root_clusters = first_level_hierarchical_clustering(hcs)   # level == 0
...
leaf_clusters = final_level_hierarchical_clustering(hcs)   # is_final_cluster
```

两者的区别 c01 已经讲透（`level == 0` 是「最粗一层」，`is_final_cluster` 是「该节点所在的最后一个簇」），这里只需补一句**为什么这个区别对模块度特别重要**：`final_level_hierarchical_clustering` 返回的映射里，不同节点可能停在不同 `level`——没有被继续分裂的小簇停在上层，被反复分裂的大簇停在下层。所以 leaf 划分**不是「某一层」而是一个跨层的拼合**，它的模块度评价的是「这棵层次树把所有叶子合起来看」的质量，而不是任何单一层的质量。这也是为什么它必须和 root 分开报告：它们评价的根本不是同一个对象。

**两个函数各自重复了「构建 edge_list + 跑 Leiden」这四行，而这四行的重复是可以理解的。**

```python
edge_list = _df_to_edge_list(edges)
hcs = hierarchical_leiden(edge_list, max_cluster_size=max_cluster_size, random_seed=random_seed)
```

没有抽成一个「先聚类再评价」的公共函数，代价是两处重复；收益是每个 `calculate_*` 函数从签名到返回值都是自解释的（「给我边表，我给你这个口径的分」）。而 Leiden 本身不便宜，所以真要在一次运行里同时要 root 与 leaf 的分，当前实现会**跑两遍 Leiden**——这是可优化点，但优化的前提是调用方确实要两个数（见后文关键取舍）。

**`use_root_modularity` 默认 `True` 让 `calculate_graph_modularity` 的名字变得有点反直觉。**

`calculate_graph_modularity` 读起来像「图的模块度」，但它实际上做的是「按 `use_root_modularity` 选择口径」：

```python
if use_root_modularity:
    return calculate_root_modularity(...)
return calculate_leaf_modularity(...)
```

所以它的默认行为是 **root**。这意味着「graph modularity」这个名字里真正被强调的是 **graph**（整图范围，对照 LCC 与加权分量），而不是「哪个层级」——层级由开关决定，且默认最粗。

这个默认值的选择有一致性：模块度奖励「少而大」的社区，而 root 划分正是「少而大」，所以 root 口径给出的是**这把尺子最偏爱的那个读数**。把它设成默认，等于「默认报告最乐观的读数」——这不是错误，但读报告的人需要知道这一点，否则会以为默认的 0.6 与 leaf 的 0.2 之间是「质量下降」而不是「尺子不同」。

**`calculate_lcc_modularity` 的「范围收窄」发生在聚类之前，而不是之后。**

```python
lcc_nodes = largest_connected_component(edges)
lcc_edges = edges[edges["source"].isin(lcc_nodes) & edges["target"].isin(lcc_nodes)]
if use_root_modularity:
    return calculate_root_modularity(lcc_edges, ...)
```

顺序是「先筛边，再交给我已经写好的那两个函数」。这个顺序是对的且必须如此：如果反过来（先在整图上聚类，再把不在 LCC 的节点从划分里剔掉），那么**聚类本身已经被那些碎片影响了**——Leiden 会把碎片当社区、会把它们的度计入总度，于是 LCC 内部的社区边界可能因为图外的东西而改变。先筛再聚类，保证「评价对象的划分」与「评价对象」是同一张图。

而 `largest_connected_component` 用的是 `connected_components` 那个 union-find 实现（不是 `stable_lcc`）——这一点在 c03 里展开，那里会看到「剪枝用的 LCC」与「聚类用的 LCC」是两个不同函数，且稳定性不同。

**`calculate_weighted_modularity` 的加权公式与它的回退分支。**

```python
total_modularity += mod * len(component) / total_nodes
```

即 `Σ(分量模块度 × 分量节点数) / 参与统计的节点总数`——按节点数加权的算术平均。权重选节点数而不是边数，理由是「模块度是每个节点的归属质量问题」，所以按人头加权。

回退分支：

```python
filtered = [c for c in components if len(c) > min_connected_component_size]
if len(filtered) == 0:
    # Fall back to the whole graph
    filtered = [set(edges["source"].unique()).union(set(edges["target"].unique()))]
```

「所有分量都太小」时退化成「整图当一个分量」。这个回退避免了「返回 0 / 返回 NaN / 除零」三种坏结果，而且语义上合理：如果图碎得没有任何一块值得单独评价，那就只能整体评价。

注意比较用的是 `>` 而不是 `>=`（`min_connected_component_size` 默认 10），并且这个阈值参数**只存在于这个函数**，没有出现在 `ModularityMetric` 枚举或配置模型里——也就是说通过 `calculate_modularity` 调用时无法调整它。

**`calculate_modularity` 用 `match` 语句分发，并给每个分支打一行 `logger.info`。**

```python
match modularity_metric:
    case ModularityMetric.Graph: ...
    case ModularityMetric.LCC: ...
    case ModularityMetric.WeightedComponents: ...
```

`match` 对枚举穷举，所以新增一个 `ModularityMetric` 成员会**落到无分支的路径**——没有 `case _` 兜底，所以函数会继续往下走并在末尾隐式返回 `None`。这是一个真实的脆弱点（返回 `None` 而不是抛错，调用方会在算术运算处才炸）。当前枚举只有三个成员，所以还没被触发；但从「加一个枚举值」这个最常见的演进动作看，这里缺少一道 `case _: raise`。

**整个文件是「与 NetworkX 逐字对齐」的产物，而这一点由测试文件确认。**

`tests/unit/graphs/test_modularity.py` 里有一份**照抄的 NX 参考实现**（注释写明「copied from graphrag.index.utils.graphs」），然后对八个小场景逐一 `assert abs(nx_result - df_result) < 1e-10`：两个清晰社区、全图一社区（应为 0）、每点一社区（应为负）、反向边不影响、加权边影响、自定义分辨率、重复边、反向重复边，最后还有一个 fixture 图在 2/3/5 种划分下的一致性。

这解释了这个模块的技术定位：**它不是一条性能优化过的热路径，而是一个「去掉 networkx 依赖」的等价替换**。证据有三——`_df_to_edge_list` 与 `_modularity_components` 都用 `df.iterrows()` 逐行 Python 循环（而 `compute_degree`、`cluster_graph` 都用向量化 pandas）；公式逐项对齐 NX 的自环特殊处理；测试是并排对拍而不是独立断言具体数值。所以「跑得慢」在这里是可接受的，因为它的调用频率是「一次索引一次」，而且当前流水线里根本不调它。

**而「流水线里不调它」是一个需要如实说明的事实。**

在当前树里，`calculate_modularity` / `calculate_graph_modularity` / `calculate_lcc_modularity` / `calculate_weighted_modularity` 的调用点**全部在 `modularity.py` 内部**，没有任何 `index/workflows/` 或 `index/operations/` 里的工作流调用它们；仓库里唯一的外部使用者是 `tests/unit/graphs/test_modularity.py`（且只测 `modularity` 本身）。

所以这一层的当前身份是：**一个可供运维/GUI/研究脚本调用的质量评估能力 + 一份 NX 等价实现**，而不是索引流水线的一个阶段。这个区分很重要，因为它决定了「多跑一遍 Leiden」这个代价现在没人付——一旦有调用方真的要在一次索引里同时报告 root 与 leaf，就需要把聚类结果传进去而不是让两个 `calculate_*` 各自重跑。

## 关键取舍

**源码事实：** `modularity` 的方向归一化与去重（两端取 min/max 排序、`drop_duplicates(keep="last")`）与 `cluster_graph` 里那段逐字相同，注释同样说明「matching NX behavior」。

**源码事实：** 同一个归一化在 `_df_to_edge_list` 与 `_modularity_components` 里各实现一次；两处独立，改一处不改另一处不会报错，只会让「用来聚类的图」与「用来评分的图」不同。

**源码事实：** `_modularity_component` 是纯标量函数（只算一个社区），`_modularity_components` 返回 `dict[int, float]`，`modularity` 只是 `sum(...)`。三层拆分保留了「哪个社区贡献多少」这个信息。

**源码事实：** 非自环的社区内部边乘 `weight * 2.0`，自环只加 `weight`；而 `degree_sums_for` 对自环会加两次（`src_comm` 与 `tgt_comm` 同社区）。这个不对称复刻 NetworkX，因为 NX 的 `edges(data=...)` 对自环只 yield 一次。

**源码事实：** `total_edge_weight <= 0` 时返回「每个社区 0.0」，而不是抛错或 NaN——「没有边的图，任何划分都不携带信息」是 0。

**源码事实：** 孤立节点在模块度里贡献恰为 0（度为 0，分子分母都无痕），而不是被惩罚。这是模块度只看度的性质，不是实现缺陷。

**源码事实：** `calculate_graph_modularity` 的 `use_root_modularity` 默认 `True`，所以「graph modularity」默认报的是 **root（最粗一层）**——模块度最偏爱的那种划分。

**源码事实：** `calculate_lcc_modularity` 先筛边到 LCC、再调用 root/leaf 函数；顺序必须如此，否则聚类已经被碎片影响。

**源码事实：** `calculate_root_modularity` 与 `calculate_leaf_modularity` 各自重跑一次 Leiden（四行重复），所以同时要两个口径的分 = 跑两遍聚类。

**源码事实：** `calculate_weighted_modularity` 按「分量模块度 × 分量节点数 / 总节点数」加权，只统计 `> min_connected_component_size`（默认 10）的分量；一个都不够时回退成「整图当一个分量」。该阈值只存在于这个函数，`ModularityMetric` 与配置模型都调不到。

**源码事实：** `calculate_modularity` 用 `match` 对三个枚举穷举，**没有 `case _` 兜底**，所以新增枚举成员会让函数隐式返回 `None`。

**源码事实：** 全仓库没有任何工作流调用 `calculate_*`；唯一的外部使用者是测试文件，且只测 `modularity` 本身。

**架构推论：** 「一个划分的质量」在层次聚类里不是标量，而是**一个二维读数**（层级 × 范围）。把 root 与 leaf 分开报告，本质上是承认「模块度这把尺子本身偏向大社区」，于是用它单独评价细粒度划分必然得出悲观结论。任何用单一目标函数评价多粒度结果的地方都有这个问题；解法不是换更强的函数，而是**把同一把尺子在两个位置上的读数并排呈现**，让读者看到差异。

**架构推论：** 返回「每个社区的分」而不是只返回总和，是一个成本极低但信息量很大的接口选择。当一个总分很低时，`dict` 能区分「所有社区平庸」与「一个社区极度负贡献」——而这两种情况的修法完全相反（前者要改抽取质量，后者要剪枢纽）。**这就是为什么接口形状本身携带诊断能力**：把内部已经算出来的中间结果暴露出来，几乎不花成本，却让「下一步查什么」变得可判断。

**架构推论：** 与 NetworkX 逐字对齐（包括自环的特殊处理、`keep="last"` 的去重语义）在这里是硬要求而不是洁癖。一旦允许「顺手修正一下自环」，这个模块的输出就与所有基于 NX 的历史结果、与文档里的示例数值、与用户手里的旧报告全部失去可比性。**当替换一个有既定行为的依赖时，「行为等价」比「行为正确」更重要**——需要的正确性修正应当作为独立改动、配套迁移说明，而不是藏在一次依赖替换里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/graphs/modularity.py` | `modularity` | 公开的单值入口，等于各社区贡献之和 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `_modularity_component` | 纯标量公式，单社区贡献，`resolution` 只在此参与 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `_modularity_components` | 遍历边累积两张度字典，返回 `dict[社区, 分]` |
| `packages/graphrag/graphrag/graphs/modularity.py` | `_df_to_edge_list` | 归一化 + 去重 + 转 `list[(src, tgt, weight)]` 给 Leiden |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_root_modularity` | 最粗一层（`level == 0`）的模块度 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_leaf_modularity` | 跨层拼合的叶子划分的模块度 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_graph_modularity` | 整图范围，层级由 `use_root_modularity` 选（默认 root） |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_lcc_modularity` | 先把边筛到最大连通分量再评价 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_weighted_modularity` | 按连通分量大小加权，仅统计超过阈值的分量 |
| `packages/graphrag/graphrag/graphs/modularity.py` | `calculate_modularity` | `ModularityMetric` 分发器，默认加权分量 |
| `packages/graphrag/graphrag/config/enums.py` | `ModularityMetric` | 三个口径的枚举：`graph` / `lcc` / `weighted_components` |
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `first_level_hierarchical_clustering` | 提供 root 划分（c01 详解） |
| `packages/graphrag/graphrag/graphs/hierarchical_leiden.py` | `final_level_hierarchical_clustering` | 提供 leaf 划分（c01 详解） |
| `packages/graphrag/graphrag/graphs/connected_components.py` | `largest_connected_component` | LCC 口径用的分量选取 |
| `tests/unit/graphs/test_modularity.py` | `nx_modularity` | 并排对照的 NX 参考实现，八个场景逐一比对 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：graphs/modularity.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/graphs/modularity.py)
- [NetworkX：模块度与社区质量函数](https://networkx.org/documentation/stable/reference/algorithms/community.html)
- [Newman 模块度原始论文](https://arxiv.org/abs/cond-mat/0308217)

## 与主核 / AI 的关系

主核里最接近的是**查询代价估计与实际执行统计的对照**：`EXPLAIN` 给出一个预测代价，执行后给出实际的 `actual rows` / `actual time`，两个数字放在一起才能判断「统计信息是不是过时了」。单独看任何一个都不够——预测误差大可能是统计陈旧，也可能是代价模型对某类算子不准；正如 root 与 leaf 的模块度必须并排看，才能判断「分裂是否有意义」。

**差别在于「真值」是否存在。** 数据库的执行统计有客观真值（实际跑了多少行），所以「估计 vs 实际」的比值有明确含义。而模块度没有真值——它只是一个目标函数，本身既是评价标准又是聚类时的优化目标（`use_modularity=True`）。所以这里的「质量」是**自指的**：用同一个尺子既去切又去量。这解释了为什么这个模块只提供多个口径的读数而不给「通过/不通过」的判定——一个自指的指标不适合当门禁。

与 c01 的分工是最直接的一条：那一页讲「怎么切」（软上限触发分裂、无向化、父指针 `-1`），这一页讲「切完怎么量」。而两页共享同一个归一化语义（`min`/`max` 排序 + `keep="last"`），因为聚类与评分必须看同一张无向图。

与 c03 的关系是这一页的 `_modularity_components` 埋下的伏笔：**返回每社区分数的能力，正是判断「是不是有枢纽在破坏划分」的手段**。而 c03 的 `_get_upper_threshold_by_std` 就是针对那种情况的处置。

对 agent / RAG 工程最实用的一条是**「用单一指标评价多粒度结果必然误导」**。任何分层的系统（层次聚类、多级摘要、粗到细的检索）都会遇到：粗粒度的结果在「紧凑度」类指标上天生占优，细粒度的结果天生吃亏。所以报告里必须给出**至少两个粒度上的同一指标**，并明确说明「这个指标的偏好方向」。否则读者只会记住那个更好看的数字。

另一条是「回退分支要选一个有意义的数值而不是 0 或 NaN」。`total_edge_weight <= 0` 返回「每社区 0.0」、加权口径「全部分量都太小」返回「整图当作一个分量」——两个回退都不是「报错」，因为这两种输入都不是错误，只是**没有信息的输入**。而模块度在「没有信息」时的正确取值恰好是 0（全图一个社区也是 0），所以 0 是自洽的选择，不是随便填的默认值。
