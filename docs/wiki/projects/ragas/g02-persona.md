---
project: ragas
feature: persona
title: 问题角色生成
page_type: feature
schema_version: 1
refs:
  - path: src/ragas/testset/persona.py
    symbols: [generate_personas_from_kg, Persona, PersonaList]
---

# 问题角色生成

## 一句话职责

`generate_personas_from_kg` 从知识图谱里挑出若干互不相似的文档摘要，各生成一个「谁会来问这类问题」的角色画像——目的是让后面造出来的问题有多个提问视角，而不是同一视角造一批。

## 机制是什么

`Persona` 只有两个字段：`name`（角色名，如「数字营销专员」）与 `role_description`（一句话说明他是谁、关心什么）。`PersonaList` 是它的容器，`__getitem__` 按名字查，查不到抛 `KeyError`。

生成函数做四件事，顺序值得记清：

1. **筛节点**：`filter_fn`（默认 `default_filter`）留下「类型是 `DOCUMENT` 或 `CHUNK`」且**带 `summary_embedding` 属性**的节点。
2. **取摘要**：读每个节点的 `summary` 属性，且**只保留 `str` 类型的**。
3. **聚类去重**：把节点摘要向量两两求相似，按阈值贪心分组，每组只留**最长的那条摘要**作为代表。
4. **逐个生成**：把代表摘要并发喂给 `PersonaGenerationPrompt`，得到角色列表。

第 3 步是这个设计的核心。若不做去重，从几十个摘要里随机抽三条，很可能三条讲的是同一件事，于是最终造出的问题视角单一。**先聚类再取代表，等于强制让被选中的摘要来自不同话题区。**

## 怎么实现的

**筛选条件把「有没有摘要向量」当成「这条节点可用」的判据。** `default_filter` 只看 `summary_embedding` 是否存在，**不看 `summary` 本身是否可用**。而第 2 步又会把非字符串的摘要丢掉。两件事的不一致埋下了后面会说的下标错位问题。

**去重是贪心、无归一化的内积、硬编码阈值。** 三步：

- `embeddings = np.array([...])`，`cosine_similarities = np.dot(embeddings, embeddings.T)`——注意这里**没有做归一化**，所以严格说算的是内积而不是余弦。如果提供方返回的向量本身是单位化的（大多数嵌入模型如此），两者相等；否则阈值 0.75 的含义会随向量模长漂移。
- 阈值 `0.75` 是写死的字面量，不是参数，也没有暴露出去。
- 分组循环：对每个未被访问的 `i` 起一组，把其后所有「相似度 > 阈值」的 `j` 都吞进来并标记已访问。所以是**单向的、以先出现者为锚**的贪心分组，而非对称的连通分量——如果 `i` 与 `j` 相似但 `i` 已被前面的组吞掉，`j` 就可能落到另一组。

**每组取最长摘要而不是第一条或中心。** `max([summaries[i] for i in group], key=len)`。理由是长摘要信息更全，适合当提示的输入。这条选择有副作用：代表不是「最典型」的，而是「最长的」，长度与代表性无关。

**并行生成用 `run_async_batch`，温度固定 1.0。** `temperature=1.0` 是这三处里最容易被忽略的一个细节——角色生成要的是**多样性**，所以刻意拉满温度。这也解释了为什么示例只有一个（`PersonaGenerationPrompt` 只给了一条营销专家的示例）：示例太多会把输出风格统一，与「多视角」目标相悖。

**`num_personas` 会被两处修正。** 先是 `num_personas = min(num_personas, len(summaries))`——摘要不够就不硬凑。然后是 `if len(top_summaries) <= num_personas` 时用 `np.random.choice` **补足到 num_personas 条**。第二处是允许重复的：`np.random.choice` 默认 `replace=True`，所以当簇数少于目标数时，同一个摘要会被重复选，从而生成多个**只差温度**的角色。

也就是说「摘要太少时」不去减少角色数，而是重复用同一份摘要造多个角色。这与第一处修正的方向相反，读代码时容易看漏。

**`generate_personas_from_kg` 的返回值是原始列表，不包容器。** `PersonaList` 不是这个函数的返回类型，它是后续合成器里用来按名字查角色的工具（`SingleHopQuerySynthesizer.prepare_combinations` 与 `MultiHopQuerySynthesizer` 都用它把「概念映射」里的名字换成 `Persona` 对象）。所以 `PersonaList` 的职责是**查表适配器**，不是「角色集合」的权威形态。

## 关键取舍

**源码事实：** 筛选依据是 `summary_embedding` 是否存在，而下游只用 `str` 类型的 `summary`。两者不一致：一个节点可能有向量但没有可用摘要。

**源码事实：** 向量相似用未归一化的内积 `np.dot(embeddings, embeddings.T)`，阈值 `0.75` 硬编码。向量本身是单位向量时它等价于余弦相似度。

**源码事实：** 分组是单向贪心（以首个未访问节点为锚，吞掉其后所有超阈值的节点），不是对称连通分量。

**源码事实：** 每组选「最长摘要」作代表，与代表性无关。

**源码事实：** 簇数不足 `num_personas` 时用 `np.random.choice` 补足，默认允许重复，因此同一摘要可能生成多个角色。

**源码事实：** 角色生成温度固定 `1.0`，示例只给一条。

**架构推论：** 「先聚类再取代表」把「多样性」从一个随机性期望变成结构性保证：只要阈值合理，被选中的摘要必然来自不同簇。代价是**阈值决定一切**——太高则每簇只有一条、等于不做去重；太低则所有节点并成一簇、只剩一个代表。而阈值是写死的，不同嵌入模型的最优阈值并不相同。

**架构推论：** 用「摘要的语义相似」来推断「提问视角是否重复」，隐含一个假设：**同一话题的读者会问同一类问题**。这个假设对「同一份文档的两个不同方面」并不成立——同话题不同侧面本可以产生不同视角的问题，而这里会被合并掉。反过来，不同话题里可能藏着同一个角色关心的东西，这里会拆开。所以这套去重是「话题级」而非「角色级」的近似。

**架构推论：** 返回裸列表而不返回 `PersonaList`，说明角色在这一步还只是「数据」，到合成器那一步才需要「按名字索引」。把容器的创建推到真正需要索引的地方，避免了为一个临时用途造类型。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/ragas/testset/persona.py` | `Persona` | 角色画像：名字 + 一句职责描述 |
| `src/ragas/testset/persona.py` | `PersonaList` | 按角色名索引的容器，查不到抛 `KeyError` |
| `src/ragas/testset/persona.py` | `PersonaGenerationPrompt` | 单示例的结构化提示：摘要 → 角色 |
| `src/ragas/testset/persona.py` | `default_filter` | 只留文档/片段且带摘要向量的节点 |
| `src/ragas/testset/persona.py` | `generate_personas_from_kg` | 筛选、聚类取代表、并发生成角色 |
| `src/ragas/testset/synthesizers/single_hop/base.py` | `SingleHopQuerySynthesizer.prepare_combinations` | 用 `PersonaList` 把概念映射换成角色 |
| `src/ragas/testset/synthesizers/multi_hop/base.py` | `MultiHopQuerySynthesizer.prepare_combinations` | 同上，多跳场景 |

## 参考

- [ragas 源码：testset/persona.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/persona.py)
- [ragas 文档：测试集生成](https://docs.ragas.io/en/stable/concepts/test_data_generation/)
- [ragas 源码：testset/graph.py](https://github.com/explodinggradients/ragas/blob/main/src/ragas/testset/graph.py)
- [论文：按读者画像构造多样化评测问题](https://arxiv.org/abs/2309.15217)

## 与主核 / AI 的关系

「先按相似度分组、再从每组取一个代表」与主核里「按分区做抽样」是同一种抽样思路：都为了避免样本集中到少数几个簇。主核的分区由存储布局决定（按范围或哈希），这里的分区由向量阈值决定。差别在于主核的分区是确定的、可复现的；这里的阈值与贪心顺序会让分区随向量分布漂移，且**没有随机种子**（`np.random.choice` 那里没设 seed）。

对 agent 工程最实用的三条。其一：**造评测集时最容易被忽略的质量问题是视角单一**，而单靠提高温度解决不了——它会让你得到同一视角的十种说法。必须从「输入素材的多样性」入手，这正是这一步做的。其二：阈值 `0.75` 与「不归一化」这两点要一起看，换嵌入模型后建议先打印一下相似度分布再信任这个阈值，否则去重可能形同虚设或过度合并。其三：`default_filter` 与摘要类型过滤的不一致意味着**上游 transform 必须保证「有 `summary_embedding` 的节点也有字符串 `summary`」**，否则这一步的下标会对齐错误；接自己的 transform 时要保证这条不变量。
