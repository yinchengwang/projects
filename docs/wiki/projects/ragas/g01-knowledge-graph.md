---
project: ragas
feature: knowledge-graph
title: 文档知识图谱
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/testset/graph.py
    symbols: [KnowledgeGraph, Node, Relationship, NodeType]
---

# 文档知识图谱

## 一句话职责

`KnowledgeGraph` 把「文档片段 + 片段之间的关系」存成一张带 UUID 标识的有向图，并提供**按路径长度切出子集**的能力——造多跳问题时需要「一串相关的片段」，这个能力就是它存在的理由。

## 机制是什么

三个数据类加一个枚举，构成整个结构：

- `Node`：`id`（UUID，缺省自动生成）、`properties`（任意字典）、`type`（`NodeType`）。
- `Relationship`：`id`、`type`（字符串，如「引用」「从属」）、`source`、`target`（都是 `Node` 对象）、`bidirectional`、`properties`。
- `KnowledgeGraph`：两个列表 `nodes` 与 `relationships`，加一组查询方法。
- `NodeType`：`UNKNOWN`（空串）、`DOCUMENT`、`CHUNK`——只用两个真实类型，说明它区分的是「整篇文档」与「切出来的片段」。

`Node` 与 `Relationship` 都自定义了 `__hash__`（按 `id`）与 `__eq__`（按 `id` 相等），所以它们能直接放进 `set`、能作为 `dict` 键、能给 `networkx` 当节点标识。

`Relationship` 还有一处关键简化：`field_serializer("source", "target")` 把两端节点**序列化成 id**。于是关系序列化后只剩三个 UUID（自身、源、目标）加类型与属性，不必重复序列化节点内容。加载时反向重建：先建全部节点，再用 `{str(id): node}` 映射把关系两端挂回去。

## 怎么实现的

**存取是纯 JSON，靠一个自定义 encoder 处理 UUID。** `save` 把节点与关系各自 `model_dump()`，再用 `json.dump(..., cls=UUIDEncoder)` 写出——`UUIDEncoder.default` 只做一件事：遇到 `uuid.UUID` 就 `str()`，其余交给父类。所以「关系里 source/target 被序列化成 id」与「id 本身是 UUID 对象要转字符串」这两层是叠加的：最终 JSON 里两端存的是纯字符串。

`load` 的还原顺序是硬约束：**先建节点，再建关系**。因为关系需要 `nodes_map` 才能挂上对象。代码里 `Node(**node_data)` 直接把字符串 id 传给 pydantic，由 pydantic 的 UUID 解析自动转回对象。

**节点属性访问做了大小写归一。** `add_property` 存 `key.lower()`，已存在则抛 `ValueError`；`get_property` 也查小写键。`Relationship` 只提供 `get_property`，没有 `add_property`——关系属性在构造时就给全。

`add_property` 拒绝同名重复，而不是覆盖。这条约束让「给节点打标签」变成一次性动作，写错键名会立刻暴露而不是静默改写。

**`KnowledgeGraph.add` 是唯一写入口。** 它按类型分派到 `_add_node` / `_add_relationship`，两者都只是 `append`。**没有任何去重或存在性校验**——同一个 Node 可以被添加多次，关系也不检查两端是否在 `nodes` 里。

**`remove_node` 有两个模式。** `inplace=True` 直接改自己（从 `nodes` 删掉，并过滤掉所有触及它的关系）；`inplace=False` 用 `deepcopy` 造副本再改。节点不在图里则抛 `ValueError`。注意它用 `rel.source != node and rel.target != node` 过滤——依靠 `Node.__eq__` 按 id 比较，所以只要 id 相同就能删掉关系，即使传进来的不是同一个对象实例。

**核心能力是「切子集」，有两套实现并存。**

第一套 `find_indirect_clusters(relationship_condition, depth_limit)`：先 import `networkx`，再用 `sknetwork` 的 Leiden 算法做社区发现（固定 `random_state=42` 保证可复现），把节点分到若干簇；对每簇转成 `nx.DiGraph`，然后按「预期路径数」分流——`max_simple_paths(簇节点数, 深度) < 1000` 时用 `nx.all_simple_paths` 穷举（对每对有序节点都试），否则用 `nx.generate_random_paths` 随机采样 1000 条。最后把每条路径转成 `Node` 集合并用 `frozenset` 去重。

它自己有一段注释专门澄清命名：方法名里的 "indirect clusters" 指的是「不直接相连但通过其他节点共享关系的节点组」，而 Leiden 是**社区发现**算法（按连接密度分簇）——两者不是同一个概念。这段澄清值得记住，因为它解释了为什么分簇之后还要再做路径枚举：Leiden 只给出「大致同一话题」，路径才是真正的「多跳关系」。

穷举与采样之间有明确的成本门槛（1000 条），`max_simple_paths` 用连乘估算全连通图的路径数量（`n-k-1 <= 0` 时返回 0）。采样的实现里有一处必要的预处理：**先反复删掉所有出度为零的节点**直到稳定——因为 `generate_random_paths` 遇到没有出边的节点会出错。删空则直接返回空。

路径长度按「边数」而非节点数的措辞在多处混用：`depth_limit=2` 的注释说「3 个节点、2 条边」，与 `nx` 的 `path_length` 语义一致。

第二套 `find_n_indirect_clusters(n, relationship_condition, depth_limit)`：完全不依赖 Leiden 与 networkx，纯 Python 用邻接表 + DFS 找路径，从随机起点分组、轮流取、去重、并用「超集吸收子集」的规则消除冗余。它的 docstring 明确说要「maximize diversity」，且参数里有目标数量 `n`。这套实现是为大数据集优化的——不引入 sknetwork 那一整套依赖，也不做全图社区发现。

两套实现的存在说明同一个概念经历过一次重写：旧的那套（Leiden）依赖重、产出的是「所有可能的簇」，新的那套目标明确（给我 n 个多样化的簇）且只依赖内置数据结构。

**`find_two_nodes_single_rel` 产出三元组。** 它遍历满足条件的关系，跳过自环（`source != target`），并把两端按 id 大小做规范化：id 小的一律排前面。若原关系方向相反，就**构造一个新的 `Relationship`** 把方向摆正（丢掉原 id，因为没传），这样同一个语义关系不管存储方向如何都只占一个三元组位置。最后用 `set` 去重后转 list。

## 关键取舍

**源码事实：** `add` 不做去重、不校验关系两端是否已登记。图的完整性依赖调用方（或上层的 transform）自己保证。

**源码事实：** `Relationship` 序列化时把 source/target 替换成 id，`save`/`load` 因此能扁平存储，但**加载顺序被钉死**：必须先节点后关系。

**源码事实：** `Node.properties` 的键强制小写，`add_property` 同名重复即抛错，`get_property` 查小写键。关系侧没有 `add_property`。

**源码事实：** 存在两套「切子集」实现（Leiden + networkx 的 `find_indirect_clusters`，以及纯 Python 邻接表的 `find_n_indirect_clusters`），前者产出全量簇、后者产出指定数量的多样化簇。

**源码事实：** 穷举与采样按「全连通假设下路径数是否小于 1000」分流，采样前反复剔除出度为零的节点。

**架构推论：** 把 `Node` 与 `Relationship` 的相等性定义在 id 上，使它们可以自由跨容器传递——`set` 去重、`dict` 索引、networkx 节点标识全都成立。代价是**内容改了身份不变**：同一个 id 换个属性值，它在集合里仍然「相同」，因此不能用集合来检测内容变更。

**架构推论：** 关系方向可规范化（`find_two_nodes_single_rel` 里按 id 排序）说明「方向」在这个数据模型里不是硬语义，而是一种存储细节。这对造多跳问题很重要：把「A 引用 B」与「B 被 A 引用」视作同一关系，可以避免同一内容对产出两条重复的题目。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/testset/graph.py` | `UUIDEncoder` | 只把 UUID 转字符串的 JSON 编码器 |
| `src/ragas/testset/graph.py` | `NodeType` | 节点类型：未知、文档、片段 |
| `src/ragas/testset/graph.py` | `Node` | 节点：UUID + 属性字典 + 类型，按 id 定相等 |
| `src/ragas/testset/graph.py` | `Node.add_property` | 属性键小写归一，重复即抛 |
| `src/ragas/testset/graph.py` | `Relationship` | 关系：类型 + 两端节点 + 方向 + 属性 |
| `src/ragas/testset/graph.py` | `Relationship.serialize_node` | 序列化时把两端替换成 id |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.save` | 写出节点与关系两份扁平 JSON |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.load` | 先建节点再按下标挂关系 |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.remove_node` | 删节点并过滤触及它的关系，含副本模式 |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.find_indirect_clusters` | Leiden 分簇 + 穷举/采样路径 |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.find_n_indirect_clusters` | 纯邻接表 DFS，产出 n 个多样化簇 |
| `src/ragas/testset/graph.py` | `KnowledgeGraph.find_two_nodes_single_rel` | 规范化方向后的三元组集合 |

## 参考

- [ragas 源码：testset/graph.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/graph.py)
- [ragas 文档：测试集生成](https://docs.ragas.io/en/stable/concepts/test_data_generation/)
- [Leiden 算法论文：社区发现的高质量划分](https://www.nature.com/articles/s41598-019-41695-z)
- [NetworkX 文档：simple paths 与随机路径](https://networkx.org/documentation/stable/reference/algorithms/simple_paths.html)

## 与主核 / AI 的关系

「节点按 id 定身份、内容不进身份」与主核里「页由 page id 标识、内容可被更新」是同一种取舍。主核用 LSN 记录「内容变了但身份没变」，这里的图**没有版本机制**——同一个 Node 被改属性后，任何基于 id 去重的结构都不会察觉。这在造题流程里是隐患：如果 transform 阶段原地改了片段内容，之前算出的簇与路径仍然指向它，不会失效。

对 agent 工程最实用的三条。其一：**节点类型只有「文档」与「片段」两种**，想要更多粒度得靠 `properties` 里的标签，而不是新增类型——这意味着下游筛选几乎总是按属性字典来做（`persona` 那个 `default_filter` 就是查 `summary_embedding` 是否存在）。其二：**多跳题的质量直接取决于这里切出的路径**，而路径来自「Leiden 分簇 + 图内枚举」，所以**关系建得对不对**（哪些片段间有引用）是决定性的；关系稀疏时簇内没有路径，产出就会退化成单跳。其三：`find_two_nodes_single_rel` 的方向规范化是必须的——否则同一段内容会因为关系存储方向不同而造出两道重复题。
