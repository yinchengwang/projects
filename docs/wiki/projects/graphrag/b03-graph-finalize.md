---
project: graphrag
feature: graph-finalize
title: 图收尾与度数计算
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/workflows/finalize_graph.py
    symbols: [finalize_graph, _build_degree_map]
  - path: packages/graphrag/graphrag/index/operations/finalize_entities.py
    symbols: [finalize_entities]
  - path: packages/graphrag/graphrag/index/operations/compute_edge_combined_degree.py
    symbols: [compute_edge_combined_degree]
  - path: packages/graphrag/graphrag/graphs/edge_weights.py
    symbols: [calculate_pmi_edge_weights, calculate_rrf_edge_weights]
---

# 图收尾与度数计算

## 一句话职责

`finalize_graph` 把「抽取出来的一堆行」变成「一张可用的图」：先流式扫一遍关系表算出每个节点的度数（无向、去重），再让实体行拿到 `degree`、关系行拿到 `combined_degree`，同时给两类行分配 `human_readable_id` 与 uuid；而 `edge_weights.py` 里的 PMI 与 RRF 是两种「把共现次数变成有意义的权重」的算法——**前者在生产路径上用，后者全仓零调用点**。

## 机制是什么

抽取阶段产出的表有两个问题，必须在聚类之前解决：

**问题一：行级记录不是图。** 抽取输出的是「每个文本单元提到的一串实体 + 一串关系」，同一个实体出现 N 次就有 N 条记录。聚类算法需要的是**一个节点一个条目、一条边一个条目**。所以 finalize 的第一件事是去重：实体按 `title`、关系按 `(source, target)`。

**问题二：图算法与提示词都需要「重要性」，而原始数据里没有这个字段。** Leiden 用边权做划分依据，社区报告的提示词要按重要度排序挑选实体与关系。于是 finalize 要造出两个数字：

- `degree`：节点的度数（有多少条去重后的边连到它）
- `combined_degree`：边两端节点度数之和

这两个数字的算法里藏着一个必须明确的选择：**算度数时把图当有向还是无向？** 代码选无向——`_build_degree_map` 里先把每对端点排序成 `(lo, hi)` 再判断是否已见。这个选择与 c01 讲的「Leiden 前把图强制无向」一致，两边必须一致，否则度数会与聚类依据错位。

## 怎么实现的

**`_build_degree_map` 是「无向 + 去重 + 一次流式扫」。**

```
async def _build_degree_map(relationships_table: Table) -> dict[str, int]:
    """Stream relationship rows to compute node degrees.

    Normalizes each edge to an undirected pair and deduplicates
    on the fly, matching the behavior of ``compute_degree`` but
    without materializing a DataFrame.
    """
    seen: set[tuple[str, str]] = set()
    degree: Counter[str] = Counter()
    async for row in relationships_table:
        lo, hi = sorted((row["source"], row["target"]))
        if (lo, hi) not in seen:
            seen.add((lo, hi))
            degree[lo] += 1
            degree[hi] += 1
    return dict(degree)
```

四点：

1. **`sorted((source, target))` 把 `(A,B)` 与 `(B,A)` 归一成同一对。** 于是反向边不会重复计数。
2. **`seen` 集合去重。** 抽取阶段已经合并过 `(source, target)`，理论上不该有重复；但 `_merge_relationships` 用 `groupby(["source", "target"])`，**它不排序**——所以 `(A,B)` 与 `(B,A)` 是两个不同的组，都会保留。这正是 `sorted` 在这里必须存在的原因。
3. **`degree[lo] += 1` 与 `degree[hi] += 1` 各一次。** 这是「无向图里一条边给两端各贡献 1 度」的定义。
4. **docstring 说「matching the behavior of `compute_degree`」**，但 `compute_degree` 这个函数在当前代码里已经不存在（被这处流式实现取代）。这是一条指向已删除实现的注释——它说的「语义对齐」是真的，但读者找不到那个函数来对照。

**`finalize_entities` 做四件事，其中两件是「造 id」。**

```
async def finalize_entities(entities_table: Table, degree_map: dict[str, int]) -> list[dict]:
    sample_rows: list[dict[str, Any]] = []
    seen_titles: set[str] = set()
    human_readable_id = 0

    async for row in entities_table:
        title = row.get("title")
        if not title or title in seen_titles:
            continue
        seen_titles.add(title)
        row["degree"] = degree_map.get(title, 0)
        row["human_readable_id"] = human_readable_id
        row["id"] = str(uuid4())
        human_readable_id += 1
        out = {col: row.get(col) for col in ENTITIES_FINAL_COLUMNS}
        await entities_table.write(out)
        if len(sample_rows) < 5:
            sample_rows.append(row)
    return sample_rows
```

四处：

**第一，去重键是 `title`，而抽取阶段的合并键是 `["title", "type"]`。** 这是又一处不一致：若同一 title 有 org 与 person 两条记录，`_merge_entities` 会保留两条，而 `finalize_entities` **只留第一条**（第二条进 `continue`，`type` 被丢掉）。于是 a03 里说的「同名异类型会变成两条」实际到不了最终表——**最后一步又把它压成一条了，且丢掉的是后出现的那条的类型。**

**第二，`degree` 是从 `degree_map` 里按 title 查的，缺失给 0。** 一个实体若没有任何边（孤儿节点），度数就是 0。这些节点会留在表里（不去掉），但聚类时它们不参与任何边。

**第三，`id` 是 `uuid4()` —— 每次重跑都不同。**

对比 `create_base_text_units` 里 `id = sha512(text)`（内容决定 id），实体 id 是**随机**的。这个选择有明确后果：**同一份文档重跑索引，实体 id 全变**。于是任何外部系统（用户标注、外部映射表）按实体 id 引用都会失效。而 `CommunityReport.id` 又是 `sha512(full_content)`（内容决定）——**三类产物的 id 策略各不相同**。

**第四，输出列被 `ENTITIES_FINAL_COLUMNS` 白名单裁剪。**

```
out = {col: row.get(col) for col in ENTITIES_FINAL_COLUMNS}
```

`ENTITIES_FINAL_COLUMNS` 是 `[id, human_readable_id, title, type, description, text_unit_ids, frequency, degree]`。**这个裁剪会丢掉所有不在白名单里的列**——包括 `extract_graph` 写出的任何额外列。这是一个「输出 schema 的强制点」：无论上游写了什么，最终表只有这八列。

**`finalize_relationships` 与实体版几乎同构，但它的度数不是查出来的。**

```
key = (row.get("source", ""), row.get("target", ""))
if key in seen:
    continue
seen.add(key)
row["combined_degree"] = degree_map.get(key[0], 0) + degree_map.get(key[1], 0)
```

**关键：去重键是 `(source, target)` 有序对，不加 `sorted`。** 于是 `(A,B)` 与 `(B,A)` 是两条**不同的**边，都会被保留、各自算一个 `combined_degree`。

这与 `_build_degree_map` 的处理直接冲突：后者用 `sorted` 把反向边当同一条，前者不用 `sorted` 让它们变成两条。**结果是：**

- 图里有 `(A,B)` 与 `(B,A)` 两条边时，`_build_degree_map` 只计数一次（degree 正确，符合无向语义）；
- `finalize_relationships` 保留两条行，两条的 `combined_degree` 相同。

**于是最终表里存在「看起来是两条不同的边、度数却一样」的重复边。** 聚类前 `cluster_graph` 会再按 `sorted` 去重一次（c01 讲的），所以 Leiden 不受影响；但**检索期读 `relationships` 表时，同一条无向关系会出现两遍**——local search 拼提示词时会重复描述同一个关系。

**`compute_edge_combined_degree` 是被上面两处内联取代的老实现，但它的公式就是被内联的那两行。**

```
def compute_edge_combined_degree(
    edge_df: pd.DataFrame,
    node_degree_df: pd.DataFrame,
    node_name_column: str,
    node_degree_column: str,
    edge_source_column: str,
    edge_target_column: str,
) -> pd.Series:
    """Compute the combined degree for each edge in a graph."""

    def join_to_degree(df: pd.DataFrame, column: str) -> pd.DataFrame:
        degree_column = _degree_colname(column)
        result = df.merge(
            node_degree_df.rename(
                columns={node_name_column: column, node_degree_column: degree_column}
            ),
            on=column,
            how="left",
        )
        result[degree_column] = result[degree_column].fillna(0)
        return result

    output_df = join_to_degree(edge_df, edge_source_column)
    output_df = join_to_degree(output_df, edge_target_column)
    output_df["combined_degree"] = (
        output_df[_degree_colname(edge_source_column)]
        + output_df[_degree_colname(edge_target_column)]
    )
    return cast("pd.Series", output_df["combined_degree"])
```

**全仓零调用点**——`finalize_relationships` 用 `degree_map.get(key[0], 0) + degree_map.get(key[1], 0)` 单行完成了同一件事。这个函数是 DataFrame 时代的遗留，它做的两次 `merge` 在流式实现里被一个字典查表替代。保留它的价值只在「读文档时能对上公式」，实际是死代码。

`_degree_colname` 那层包装（`f"{column}_degree"`）也只是为了避免列名冲突——两次 merge 都用 `node_degree_df`，若不改名第二次会覆盖第一次的 `degree` 列。

**`edge_weights.py` 里的 PMI 是「用点互信息修正共现」。**

```
def calculate_pmi_edge_weights(nodes_df, edges_df, node_name_col="title",
                              node_freq_col="frequency", edge_weight_col="weight",
                              edge_source_col="source", edge_target_col="target") -> pd.DataFrame:
    """Calculate pointwise mutual information (PMI) edge weights.

    Uses a variant of PMI that accounts for bias towards low-frequency events.
    pmi(x,y) = p(x,y) * log2(p(x,y)/ (p(x)*p(y))
    p(x,y) = edge_weight(x,y) / total_edge_weights
    p(x) = freq_occurrence(x) / total_freq_occurrences.
    """
    copied_nodes_df = nodes_df[[node_name_col, node_freq_col]]

    total_edge_weights = edges_df[edge_weight_col].sum()
    total_freq_occurrences = nodes_df[node_freq_col].sum()
    copied_nodes_df["prop_occurrence"] = (
        copied_nodes_df[node_freq_col] / total_freq_occurrences
    )
    copied_nodes_df = copied_nodes_df.loc[:, [node_name_col, "prop_occurrence"]]

    edges_df["prop_weight"] = edges_df[edge_weight_col] / total_edge_weights
    edges_df = (
        edges_df
        .merge(copied_nodes_df, left_on=edge_source_col, right_on=node_name_col, how="left")
        .drop(columns=[node_name_col])
        .rename(columns={"prop_occurrence": "source_prop"})
    )
    edges_df = (
        edges_df
        .merge(copied_nodes_df, left_on=edge_target_col, right_on=node_name_col, how="left")
        .drop(columns=[node_name_col])
        .rename(columns={"prop_occurrence": "target_prop"})
    )
    edges_df[edge_weight_col] = edges_df["prop_weight"] * np.log2(
        edges_df["prop_weight"] / (edges_df["source_prop"] * edges_df["target_prop"])
    )

    return edges_df.drop(columns=["prop_weight", "source_prop", "target_prop"])
```

**公式值得逐项读懂，因为它是「惩罚高频」的核心：**

标准 PMI 是 `log2(p(x,y) / (p(x)·p(y)))`。这里多乘了一个 `p(x,y)`，得到 `p(x,y) · log2(p(x,y)/(p(x)p(y)))`。

**这个变体叫「Pointwise Mutual Information 乘以联合概率」，作用是压低低频项的偏置。** 标准 PMI 有个著名缺陷：只出现过一次的稀有对会给出极高的 PMI（因为 `p(x,y)` 虽小但分母更小），于是「一次偶然共现」会排在「百次稳定共现」之前。乘上 `p(x,y)` 之后，低频对的分数被它的联合概率直接压下。

**所以这里的权重回答的问题是「这两个词共同出现得好，而且出现得够多」，而不是纯 PMI 的「共同出现得比偶然好」。** 对共现图来说这是对的：我们要的是真实的语义联系，不是统计上的巧合。

**三处实现细节：**

1. **`total_edge_weights` 与 `total_freq_occurrences` 是两个不同的归一化基准。** 边的分母是所有边的权重之和，节点的分母是所有节点频次之和。这两个总量不在同一量纲上——但它们只用来算概率，各自的内部一致性就够。
2. **两次 `merge` 都先 `drop(columns=[node_name_col])` 再 `rename`。** 因为 `merge` 会把右表的键列也带进来（与左表列名不同，所以不冲突但会多一列），drop 掉保持列干净。
3. **`how="left"` 让端点缺失的边保留。** 缺失时 `source_prop` 为 `NaN`，于是 `np.log2(...)` 得到 `NaN`，最终该边的权重是 `NaN`。**没有兜底**——一条端点在节点表里找不到的边会带着 `NaN` 权重流出。这个情况本应被 `filter_orphan_relationships` 挡住（b02），但那一步只在标准路径的抽取后跑，NLP 路径没有。

**RRF 是「把两种排序结果融合」，而它的实现有一个明显的缺陷。**

```
def calculate_rrf_edge_weights(nodes_df, edges_df, node_name_col="title", node_freq_col="freq",
                               edge_weight_col="weight", edge_source_col="source",
                               edge_target_col="target", rrf_smoothing_factor=60) -> pd.DataFrame:
    """Calculate reciprocal rank fusion (RRF) edge weights.

    Combines PMI weight and combined freq of source and target.
    """
    edges_df = calculate_pmi_edge_weights(nodes_df, edges_df, node_name_col, node_freq_col,
                                          edge_weight_col, edge_source_col, edge_target_col)

    edges_df["pmi_rank"] = edges_df[edge_weight_col].rank(method="min", ascending=False)
    edges_df["raw_weight_rank"] = edges_df[edge_weight_col].rank(method="min", ascending=False)
    edges_df[edge_weight_col] = edges_df.apply(
        lambda x: (
            (1 / (rrf_smoothing_factor + x["pmi_rank"]))
            + (1 / (rrf_smoothing_factor + x["raw_weight_rank"]))
        ),
        axis=1,
    )

    return edges_df.drop(columns=["pmi_rank", "raw_weight_rank"])
```

**`pmi_rank` 与 `raw_weight_rank` 是两个完全相同的表达式，对同一列排名两次。**

```
edges_df["pmi_rank"] = edges_df[edge_weight_col].rank(...)
edges_df["raw_weight_rank"] = edges_df[edge_weight_col].rank(...)
```

**两次调用的输入列、方法、方向全都一样** ——第二行执行时 `edge_weight_col` 还没被覆盖（覆盖在下一行），所以两者的值完全相等。

于是 RRF 的公式退化成：

```
1/(60 + rank) + 1/(60 + rank) = 2/(60 + rank)
```

**这不是「融合两个排序」，而是「一个排序的两倍」。** docstring 说「Combines PMI weight and combined freq of source and target」，但代码里既没有 `combined freq` 的排名，也没有第二个独立的信号源——`raw_weight_rank` 本该是「原始权重（PMI 之前的共现次数）的排名」，而那个值在调用 `calculate_pmi_edge_weights` 时已经被覆盖掉了。

**要修它，必须在调 PMI 之前先把原始权重存一列。** 现在的实现没有存，所以第二个信号源不存在。

**而 `calculate_rrf_edge_weights` 与 `compute_edge_combined_degree` 一样：全仓零调用点。**

于是 `edge_weights.py` 的两个函数里，只有一个（PMI）在生产路径上被用（`build_noun_graph` 的 `normalize_edge_weights=True`），RRF 是死代码且带实现缺陷。

**PMI 的命名参数默认值不一致，这会在不显式传参时出错。**

```
calculate_pmi_edge_weights(... node_freq_col: str = "frequency" ...)   # 默认 "frequency"
calculate_rrf_edge_weights(...  node_freq_col: str = "freq" ...)       # 默认 "freq"
```

**RRF 调用 PMI 时把自己的 `node_freq_col` 传下去了**（位置参数，第四个），所以两者用同一列。但两个函数的**默认值不同**：`"frequency"` 与 `"freq"`。若有人直接调 `calculate_rrf_edge_weights(nodes, edges)` 而不传 `node_freq_col`，它会把 `"freq"` 传给 PMI，而节点表的列名是 `frequency`——`nodes_df[["title", "freq"]]` 会抛 `KeyError`。

这大概是「`freq` 是早期列名、后来改成 `frequency` 时漏改了这个默认值」的痕迹。

## 关键取舍

**源码事实：** `finalize_graph` 先 `_build_degree_map` 流式扫一遍关系表（把每对端点 `sorted` 归一、用 `seen` 去重、给两端各 +1，即无向度数），再让 `finalize_entities` / `finalize_relationships` 各扫一遍写回。

**源码事实：** `finalize_entities` 去重键是 `title`，而抽取阶段的合并键是 `["title", "type"]`——同名异类型的记录在最后一步被压成一条，后者（及其 `type`）被丢弃。

**源码事实：** `finalize_relationships` 的去重键是**有序** `(source, target)`，不做 `sorted`。与 `_build_degree_map` 的无向归并相冲突，因此 `(A,B)` 与 `(B,A)` 会作为两行保留，且两行的 `combined_degree` 相同。

**源码事实：** 实体与关系的 `id` 是 `str(uuid4())`——每次重跑都不同；而 text unit 的 id 是 `sha512(text)`、社区报告的 id 是 `sha512(full_content)`（内容决定）。三类产物的 id 稳定性策略不一致。

**源码事实：** `human_readable_id` 按流式读入顺序从 0 递增，是给提示词/展示用的短名，不是稳定标识。

**源码事实：** 两类行的输出都经 `ENTITIES_FINAL_COLUMNS` / `RELATIONSHIPS_FINAL_COLUMNS` 白名单裁剪，上游写的额外列被丢掉。

**源码事实：** `compute_edge_combined_degree`（两次 merge + 列名改写）全仓**零调用点**；其公式被 `finalize_relationships` 里 `degree_map.get(key[0], 0) + degree_map.get(key[1], 0)` 内联取代。

**源码事实：** `calculate_pmi_edge_weights` 用的是「PMI 乘以联合概率」的变体（`p(x,y)·log2(p(x,y)/(p(x)p(y)))`），docstring 说明这是为了修正对低频事件的偏置；端点缺失时 `source_prop` 为 `NaN`，最终权重为 `NaN` 且无兜底。

**源码事实：** `calculate_rrf_edge_weights` 先调 PMI 覆盖权重列，然后对**同一列**用完全相同的参数算两次排名（`pmi_rank` 与 `raw_weight_rank`），两者数值相等，公式退化为 `2/(60+rank)`；docstring 声称融合「PMI weight 与 combined freq」，第二个信号源实际不存在。该函数全仓零调用点。

**源码事实：** `calculate_pmi_edge_weights` 的 `node_freq_col` 默认 `"frequency"`，`calculate_rrf_edge_weights` 的默认 `"freq"`——后者不显式传参会因列名不匹配抛 `KeyError`。

**架构推论：** 「度数」这个看似简单的量，其正确性完全依赖「图的边按无向处理」这个约定，而这个约定必须由**所有**读取关系表的地方共同遵守。这里 `_build_degree_map` 遵守了（`sorted`），`finalize_relationships` 没遵守（不 `sorted`），于是度数对、边表重复。**任何跨模块的图语义约定，都应当由一个共享函数强制，而不是靠每个调用点各自记得。**

**架构推论：** 三类产物用三种不相容的 id 策略（随机 uuid / 内容哈希 / 内容哈希），说明「id 的稳定性」这个属性从未被整体设计过。它带来的实际约束是：**只有随机 uuid 那两类产物不能在重跑之间被外部引用**。若将来要做「用户对实体打的分批标注」这类功能，第一步必然是把实体 id 换成内容派生——而那会改变所有已落盘的索引。

**架构推论：** PMI 的正确性依赖「共现次数」这个原始信号，而 RRF 想融合「PMI 排名」与「原始权重排名」——**这要求保留原始信号**。当前实现先覆盖再排名，把第二个信号源头抹掉了。这是一个「融合型算法在管道式实现里丢失中间值」的典型错误；**凡是融合类算法，中间结果必须显式保存，不能就地覆盖。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/workflows/finalize_graph.py` | `finalize_graph` | 度数计算 → 实体收尾 → 关系收尾；可选 graphml 快照 |
| `packages/graphrag/graphrag/index/workflows/finalize_graph.py` | `_build_degree_map` | 流式扫关系表算无向度数（`sorted` 归一 + `seen` 去重） |
| `packages/graphrag/graphrag/index/operations/finalize_entities.py` | `finalize_entities` | 按 `title` 去重、填 `degree` / `human_readable_id` / `uuid4` id、白名单裁列 |
| `packages/graphrag/graphrag/index/operations/finalize_relationships.py` | `finalize_relationships` | 按有序对去重、`combined_degree` 为两端度数和、填 id、白名单裁列 |
| `packages/graphrag/graphrag/index/operations/compute_edge_combined_degree.py` | `compute_edge_combined_degree` | 两次 merge 求两端度数和（零调用的老实现） |
| `packages/graphrag/graphrag/graphs/edge_weights.py` | `calculate_pmi_edge_weights` | PMI × 联合概率变体，惩罚低频共现；端点缺失产出 `NaN` |
| `packages/graphrag/graphrag/graphs/edge_weights.py` | `calculate_rrf_edge_weights` | 名义上的排名融合；实际对同一列排名两次（零调用） |
| `packages/graphrag/graphrag/index/operations/build_noun_graph/build_noun_graph.py` | `build_noun_graph` | PMI 的唯一生产调用点（`normalize_edge_weights` 为真时） |

## 参考

- [graphrag 源码：finalize_graph.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/workflows/finalize_graph.py)
- [graphrag 源码：edge_weights.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/graphs/edge_weights.py)
- [互信息（PMI）定义](https://en.wikipedia.org/wiki/Pointwise_mutual_information)
- [Reciprocal Rank Fusion 论文（Cormack et al., 2009）](https://plg.uwaterloo.ca/~gvcormac/cormacksigir09-rrf.pdf)

## 与主核 / AI 的关系

图收尾在主核（多模态数据库内核）里对应的是**索引构建后的统计信息收集**——都要扫一遍数据算出「每条记录该带什么派生量」。四条经验。

**第一，跨模块的语义约定必须由共享函数强制，不能靠各调用点自觉。** 「图是无向的」这个约定在 `_build_degree_map` 里用 `sorted` 落实，在 `finalize_relationships` 里被违反，结果度数正确而边表重复。主核里同类约定（「NULL 值不参与索引」「向量已归一化」）都应当有一个唯一的强制点；**任何在两处各写一遍的归一化，迟早只有一处是正确的。**

**第二，id 的稳定性是必须整体设计的属性。** 三类产物用了三种策略，于是「哪些东西能跨重跑引用」这个问题的答案分散在三个文件里。主核里凡是生成持久标识的地方（行版本、事务 id、对象 oid），必须有统一策略并写在一处；**否则「能不能引用」会变成每个消费方自己猜的事。**

**第三，融合型算法必须显式保存中间值。** RRF 想融合两个排名信号，却先在原地覆盖了其中一个信号，于是退化成单信号的两倍，而函数名、docstring、参数全都还声称它在融合。主核里做「多路归并」「多策略打分合并」时同理：**每个参与融合的分数在合并前必须是独立的变量或列**，就地更新会让「我以为在融合两者」变成无人察觉的伪实现。

**第四，死代码要删除，尤其当它「看起来正确」时。** `compute_edge_combined_degree` 与 `calculate_rrf_edge_weights` 都零调用，前者公式正确、后者有缺陷。它们的危害是**搜索时会先撞到它们、读代码时会以为它们是当前实现**。一个错误的但是活的函数会以 bug 的形式暴露；一个错误但是死的函数只会以「误导下一个人」的形式造成损失——后者更难发现，所以更该删。
