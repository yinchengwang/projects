---
project: graphrag
feature: graph-pruning
title: 图剪枝与稳定大组件
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/index/operations/prune_graph.py
    symbols: [prune_graph, _get_upper_threshold_by_std]
  - path: packages/graphrag/graphrag/graphs/stable_lcc.py
    symbols: [stable_lcc]
  - path: packages/graphrag/graphrag/graphs/connected_components.py
    symbols: [connected_components, largest_connected_component]
---

# 图剪枝与稳定大组件

## 一句话职责

在聚类之前把图**缩小**：按频次、度数上限、度数下限、边权重分位数逐层剔除节点与边，可选只保留最大连通分量——因为一张「什么都被抽成实体」的原始图直接喂给 Leiden，会把社区切得又碎又被少数枢纽主导；配套的两个图论工具（并查集求连通分量、稳定化后的 LCC）分别服务「判断连通性」与「产出确定性输入」这两个不同用途。

## 机制是什么

`prune_graph` 处在 `fast` 索引流水线的第四个位置：`create_base_text_units` → `create_final_documents` → `extract_graph_nlp` → **`prune_graph`** → `finalize_graph` → `create_communities` → …

**它只出现在 `_fast_workflows` 里，`_standard_workflows` 没有这一步。** 这个不对称是理解这一层的前提：标准流水线用 LLM 抽取，抽出的实体本来就被模型筛过一遍；而 fast 流水线用 NLP 抽取（词性/名词短语），会把大量候选一股脑倒进图里——所以需要一个纯统计的剪枝阶段把噪声压掉。剪枝在这里的角色是**补上「模型筛选」缺的那一道**，而不是通用的图优化。

七道过滤器，按代码里的实际执行顺序：

| 顺序 | 参数 | 判据 | 默认值 |
|---|---|---|---|
| 1 | `remove_ego_nodes` | 度最高的**那一个**节点 | `True` |
| 2 | `min_node_degree` | `degree < N` 删 | `1` |
| 3 | `max_node_degree_std` | `degree > mean + N·std` 删 | `None`（关） |
| 4 | `min_node_freq` | `frequency < N` 删 | `2` |
| 5 | `max_node_freq_std` | `frequency > mean + N·std` 删 | `None`（关） |
| 6 | `min_edge_weight_pct` | 边权重低于该**分位数**的删 | `40` |
| 7 | `lcc_only` | 只留最大连通分量 | `False` |

看这张表最该注意的一点：**两个「按标准差剪」的开关默认都是 `None`，也就是默认关闭。** 于是默认配置下唯一会砍掉高度数节点的动作是 `remove_ego_nodes`，而它只砍**一个**节点。默认真正生效的是「频次低于 2 的删」与「边权重后 40% 的删」——这两条都是**砍稀疏**（低频实体、弱边），不是砍枢纽。

所以「剪枝剪掉高度数节点」这个直觉只在显式配了 `max_node_degree_std` 时才成立。机制本身在那儿、可以直接用，但它是一个**需要运维主动打开的旋钮**而不是默认行为。这个区分很重要，因为「为什么默认不砍枢纽」和「为什么能砍枢纽」是两个不同的问题：前者是保守（砍掉枢纽会永久丢失一条真实的连接信息，不可回滚），后者是能力（真遇到枢纽主导时有一个可调的出口）。

## 怎么实现的

**度数的计算口径是这一页的第一个关键决定：度从原始边表算，而「实体表里有但边表里没有的孤立节点」被手工补成 0。**

```python
degree_df = compute_degree(relationships)
degree_map = dict(zip(degree_df["title"], degree_df["degree"], strict=True))
entity_titles = set(entities["title"])
for t in entity_titles:
    degree_map.setdefault(t, 0)
```

注释给了理由：「**仅实体节点（孤立，度 0）也必须出现，好让度数阈值在与之前相同的总体上来计算**」。

这句话是精确的：`_get_upper_threshold_by_std` 是均值和标准差，而**总体里有没有那批 0 会显著改变结果**。假设 500 个实体里 200 个是孤立点：若不算它们，均值是 6 左右、标准差 3，阈值约 15；若算上，均值降到 3.6、标准差 6，阈值约 21.6。同一张图、同一个 `std_trim`，阈值差 40%。所以「用哪个总体」不是实现细节，它直接决定剪掉多少节点。

而 `setdefault` 而不是 `degree_map[t] = 0`：后者会**覆盖**那些既在实体表里又有边的节点的真实度数。`setdefault` 只在键不存在时写入，所以两者都照顾到了。

**`compute_degree` 自己也做了一次无向化，而它的做法与 `modularity.py` 里的不同。**

```python
edges = relationships[[source_column, target_column]].copy()
edges["_lo"] = edges.min(axis=1)
edges["_hi"] = edges.max(axis=1)
edges = edges.drop_duplicates(subset=["_lo", "_hi"])

source_counts = edges[source_column].value_counts()
target_counts = edges[target_column].value_counts()
degree = source_counts.add(target_counts, fill_value=0).astype(int)
```

它归一化方向后去重（按 `_lo`/`_hi`，默认 `keep="first"`），然后**分别数 source 列与 target 列的出现次数再相加**。所以一条 `(A,B)` 贡献 A 一度、B 一度，总度 2——这是无向图度的正确口径。

值得跟 c02 对照：`modularity.py` 里同样用 `min`/`max` 归一化，但去重是 `keep="last"`（复刻 NX 的边属性保留语义）；`compute_degree` 只在数度数、不关心权重，所以用默认的 `keep="first"` 没有后果。**同一个归一化模式在三个地方各写一遍、去重策略各按自己的需要选**——这是这一组模块反复出现的形态，也意味着「改动归一化语义」需要同时想到三处。

**`remove_ego_nodes` 只删一个节点，而「ego」这个词的用法值得说明。**

```python
if remove_ego_nodes and degree_map:
    ego_node = max(degree_map, key=lambda n: degree_map[n])
    nodes_to_remove.add(ego_node)
```

`max(degree_map, key=...)` 返回**单个键**，所以默认配置下剪掉的正好是度数最大的那一个节点。

命名上「ego node」在图论里通常指「自我中心网络的中心」，即一个以自己为中心、邻居构成整个网络的节点。而这里选的是「度数最高者」，所以它实际上是「最像 ego 的那个节点」——代码没有给出更精确的判据（例如「邻居覆盖率超过 X%」），就是取 argmax。当存在并列时，`max` 返回**第一个**遇到的，而 `dict` 的插入顺序来自 `compute_degree` 的 `value_counts()`（按频次降序）——所以并列时实际取到的是先出现的那个，这一点在代码里没有承诺。

一个节点被当成「整张图的中心」而删掉，是一个不可回滚的信息损失：那个节点携带的所有关系从图里消失，社区报告再也无法提到它。所以默认 `True` 其实是一个不小的赌注——它的成立条件是「最大的那个实体几乎肯定是文档层面的主题词而不是一个有意义的实体」。这个假设在 fast 流水线（NLP 抽取）下比较可靠，在标准流水线（LLM 抽取）下就不一定了——而标准流水线不剪枝，两者自洽。

**`_get_upper_threshold_by_std` 只有三行，但它把「阈值」定义成了一个由数据自身决定的量。**

```python
mean = np.mean(data)
std = np.std(data)
return mean + std_trim * std
```

三点：

其一，**用总体标准差（`ddof=0`）而不是样本标准差**。`np.std` 的默认 `ddof=0`，而这里没有覆盖它。当总体是「图里所有节点」这个完整集合时，用总体标准差是对的（这不是从更大总体里抽的样本）。

其二，**阈值随图变化**：没有绝对值参数（例如「度数超过 100 就删」），只有「超过均值 N 个标准差」。所以同一份配置在不同规模的图上会剪掉不同绝对数量的节点，但**剪掉的永远是同一个「分布尾部比例」**。这是刻意的：绝对阈值在 1000 节点的图上合理，在 10 万节点的图上会剪掉一半。

其三，也是代价：**均值与标准差本身被尾部拉动**。枢纽节点度数极高时，均值被抬高、标准差被撑大，于是「均值 + N·std」可能高到没有任何节点越过它——**越是有枢纽要剪，阈值越是剪不掉它**。这是一个真实的统计学弱点（正态假设在幂律度数分布下不成立），而代码没有补偿（没有用中位数 + MAD 这类稳健统计量）。实际后果是 `max_node_degree_std` 需要配得比较小（例如 1.0 或 1.5）才有作用。

那个 `# type: ignore` 注释是因为 `np.mean` / `np.std` 的返回类型被标注为 `np.floating`，而函数声明返回 `float`——运行时没问题，只是类型检查器不认。

**两个标准差阈值的计算总体**不同**，这是同一文件里最容易被忽略的不对称。**

```python
# 度数：用 degree_values，即 map 的全部值（含孤立点、含即将被删的）
if max_node_degree_std is not None and degree_values:
    upper = _get_upper_threshold_by_std(degree_values, max_node_degree_std)

# 频次：用 remaining，即已经过度数剪枝与最小频次剪枝之后剩下的
if max_node_freq_std is not None and len(remaining) > 0:
    freq_values = remaining[freq_col].tolist()
    upper = _get_upper_threshold_by_std(freq_values, max_node_freq_std)
```

度数阈值用**原始全量**（在 `nodes_to_remove` 被填充之前就取好了 `degree_values`），频次阈值用**幸存者**。为什么？注释解释了频次那一侧：

> **在频次过滤之前应用度数删除。** NX 变异是顺序的，所以频次阈值是在度数剪枝之后幸存的实体节点集合上计算的。

所以这里的驱动因素是**复刻 NetworkX 的写时复制/顺序变异语义**：旧实现是一步步在 NX 图上 `remove_nodes_from` 再取统计量，所以每一步的统计量都基于「当前的图」。DataFrame 版本把同样的顺序照搬过来。

这个顺序有一个实际后果：**开启 `max_node_freq_std` 时，「频次异常高」的判据是在被削过的总体算的**，所以它比在原始总体上算更严格（均值更低、标准差更小 → 阈值更低 → 剪掉更多）。这是顺序带来的隐式耦合，同配置换顺序会得到不同的图。

**`remaining` 这个中间变量承担了「顺序」这个语义。**

```python
remaining = entities[~entities["title"].isin(nodes_to_remove)]

freq_col = schemas.NODE_FREQUENCY
if freq_col in remaining.columns:
    low_freq = remaining.loc[remaining[freq_col] < min_node_freq, "title"]
    nodes_to_remove.update(low_freq)
    remaining = remaining[~remaining["title"].isin(nodes_to_remove)]
    ...
```

注意 `if freq_col in remaining.columns` 这个守卫：**没有 `frequency` 列时整块频次过滤被跳过**，而不是报错。这很重要，因为不是所有实体表都带频次（`NODE_FREQUENCY` 是 `schemas` 里声明的可选列），而把「缺少这一列」当成错误会让某些历史数据迁移直接失败。代价是「配了 `min_node_freq=2` 却静默不生效」——一个真实的静默降级。

而 `nodes_to_remove.update(low_freq)` 之后又用**更新过的** `nodes_to_remove` 重算一次 `remaining`，因为 `freq_values` 必须来自「低频繁已剔除」的集合。第二次 `remaining` 只用于算 `max_node_freq_std` 的阈值，随后又被 `kept_titles` 重算覆盖——所以那行赋值的作用是「给阈值提供正确的总体」，不是「给最终结果」。

**最终结果由 `kept_titles` 这一个集合统一决定，而它同时过滤实体与边。**

```python
kept_titles = entity_titles - nodes_to_remove
pruned_entities = entities[entities["title"].isin(kept_titles)]
pruned_rels = relationships[
    relationships["source"].isin(kept_titles) & relationships["target"].isin(kept_titles)
]
```

用 `entity_titles - nodes_to_remove` 而不是「`remaining` 的标题集」，保证**保留下来的节点严格等于「实体表里有、且没被任何一道过滤器点名」**。边用 `isin & isin` 双向过滤——一条边只要有一端被删就一起消失，所以图不会是「边指向不存在的节点」这种不一致状态。

**边权重过滤用的是分位数而不是绝对值，而参数名叫 `pct` 容易误读。**

```python
min_weight = np.percentile(pruned_rels[schemas.EDGE_WEIGHT].to_numpy(), min_edge_weight_pct)
pruned_rels = pruned_rels[pruned_rels[schemas.EDGE_WEIGHT] >= min_weight]
```

`min_edge_weight_pct = 40` 的含义是「**权重低于第 40 百分位的边全删**」，不是「保留权重在 40 分以上的边」这句话的字面意思（其实是同一件事），更不是「权重值小于 40 的删」。配置描述写了「Use e.g, `40` for 40%」，但字段名叫 `min_edge_weight_pct` 而函数参数同名——把它误当绝对权重的风险是实在的。

用 `>=` 而不是 `>`：权重恰好等于该分位数的边被保留。由于百分位插值可能返回一个数据里不存在的中值，这通常等价于「保留 60% 的边」。

三个守卫一起组成条件：`len(pruned_rels) > 0 and min_edge_weight_pct > 0 and schemas.EDGE_WEIGHT in pruned_rels.columns`。所以 `pct = 0` 是「关闭这条过滤」而不是「删掉所有权重低于最小值的边」（后者等价于不删任何边，结果相同但少一次全表计算）；而缺少 `weight` 列时静默跳过。

**`lcc_only` 复用 `largest_connected_component` 而不是 `stable_lcc`，这个选择决定了剪枝结果不稳定。**

```python
if lcc_only and len(pruned_rels) > 0:
    lcc_nodes = largest_connected_component(pruned_rels)
    pruned_entities = pruned_entities[pruned_entities["title"].isin(lcc_nodes)]
    pruned_rels = pruned_rels[
        pruned_rels["source"].isin(lcc_nodes) & pruned_rels["target"].isin(lcc_nodes)
    ]
```

`largest_connected_component` 不归一化节点名、不排序、不保证并列分量的选择稳定——所以 `lcc_only=True` 的剪枝结果依赖输入行的顺序。而 **`stable_lcc` 是给聚类用的**（`cluster_graph` 在 `use_lcc=True` 时调它），不是给剪枝用的。

两者的分工可以这样概括：

| | `largest_connected_component` | `stable_lcc` |
|---|---|---|
| 输入 | 边表，原样 | 边表，**先归一化名字** |
| 输出 | `set[str]` 节点集 | **DataFrame**（过滤+归一化+去重+排序后） |
| 确定性 | 依赖行序 | 与行序无关 |
| 使用者 | `prune_graph` 的 `lcc_only`、`calculate_lcc_modularity`、`stable_lcc` 自身 | `cluster_graph`（`use_lcc=True` 时） |

所以**同一个概念（最大连通分量）有两个实现，因为它们输出不同类型、服务不同下游**。前者要一个节点集合（好 `isin` 过滤两列），后者要一个「可直接喂给 Leiden 的、顺序确定的 DataFrame」。

**`connected_components` 是并查集，只做了路径压缩、没有按秩合并。**

```python
def find(x: str) -> str:
    while parent[x] != x:
        parent[x] = parent[parent[x]]  # path compression
        x = parent[x]
    return x

def union(a: str, b: str) -> None:
    ra, rb = find(a), find(b)
    if ra != rb:
        parent[ra] = rb
```

`union` 直接把 `rb` 设成 `ra` 的父——没有比较两棵树的高度。所以理论上可能退化成深链；而 `find` 里的「路径压缩」写作 `parent[x] = parent[parent[x]]`（**路径减半**），它在每一轮循环里把当前节点挂到祖父上。路径减半 + 无按秩合并的组合在实践中接近 O(α(n))，足以应付「一次索引一次」的调用频率，所以这是一个合理的简化。

注意 `union` 加的是 `parent[ra] = rb`（ra 挂到 rb 下），而遍历按 `zip(edges[source], edges[target])` 的顺序进行——所以最终的根取决于边的处理顺序。这解释了为什么 `largest_connected_component` 的并列选择依赖行序：**根是谁由顺序决定**，而 `groups` 字典的插入顺序又由 `for node in parent`（即 `all_nodes` 的去重顺序）决定。

**`drop_duplicates` 在这里只是性能优化，不影响正确性。**

```python
edges = relationships.drop_duplicates(subset=[source_column, target_column])
```

注意它**没有**归一化方向，所以 `(A,B)` 与 `(B,A)` 都留着，仍然会被 union 两次——而 union 是幂等的，第二次是空操作。所以去重的收益只是「少做几次 find」，不是为了去重语义。这与 `compute_degree` / `modularity` 的归一化去重不同：那里去重是为了**正确的数值**（一条无向边只算一次），这里去重只是为了快。

**`all_nodes` 用 `pd.concat` 再 `unique()`，所以孤立节点（只在实体表里、不在边表里）不会出现在任何分量里。**

```python
all_nodes = pd.concat([edges[source_column], edges[target_column]], ignore_index=True).unique()
```

所以 `connected_components` 的返回**只包含至少有一条边的节点**。这对 `prune_graph` 的 `lcc_only` 有一个直接后果：**度数为 0 的孤立实体在 `lcc_only=True` 时会被全部剔除**，因为它们不在 LCC 的节点集里。这与「`lcc_only` 顾名思义只留大组件」一致，但它也意味着「孤立实体在 `lcc_only=False` 时会存活到最终图里」——尽管它们在任何社区划分中都是单点社区。前者是显式选择，后者是一个可能需要留意默认值的地方。

**`largest_connected_component` 只返回 `components[0]`，而排序依据只有长度。**

```python
return sorted(groups.values(), key=len, reverse=True)
...
return components[0]
```

`sorted` 是稳定排序，所以长度相同的分量**保持 `groups` 的插入顺序**——而插入顺序来自 `for node in parent` 的迭代顺序（`all_nodes` 的去重顺序，即边表里首次出现的顺序）。所以「两个分量一样大时留哪个」由输入行序决定，且代码没有对此做任何稳定化。

这正是 `stable_lcc` 要解决的问题的另一半（前半是名字归一化）。

**`stable_lcc` 的五步，而每一步都对应一种「同样的图、不同结果」的来源。**

```python
if relationships.empty:
    return relationships.copy()
```

第 0 步：空输入直接返回拷贝。注意是 `.copy()` 而不是原样返回——保持「总是返回一个新对象」的一致性，避免调用方原地改动污染入参。

```python
edges = relationships.copy()
edges[source_column] = edges[source_column].apply(_normalize_name)
edges[target_column] = edges[target_column].apply(_normalize_name)
```

第 1 步：**名字归一化**。`_normalize_name` 是 `html.unescape(name).upper().strip()` 三件事：

- **`html.unescape`** ——把 `&amp;` / `&lt;` / `&#39;` 解回字符。来源是 PDF/HTML 抽取的实体名里会带 HTML 实体，同一个实体可能一次被写成 `AT&amp;T`、一次写成 `AT&T`。
- **`.upper()`** ——大小写不敏感。`Apple` 与 `APPLE` 被当成同一个节点。
- **`.strip()`** ——去掉首尾空白。

**这三步把「拼写变体」合并成同一个节点，这是「稳定」的第二层含义**：不只是「同样输入给同样输出」，还让「同一个实体的不同写法」不会各自成为一个节点、各自被算一次度、各自成一个社区。

而这一步有一个**副作用需要留意**：`stable_lcc` 返回的是**归一化后**的 DataFrame，而 `cluster_graph` 拿到它之后直接喂给 `hierarchical_leiden`：

```python
if use_lcc:
    edge_df = stable_lcc(edge_df)
```

于是 `use_lcc=True` 时，**社区里的节点 id 是大写、未转义、去空白的名字**；`use_lcc=False` 时是原始名字。同一个数据集换一个开关，`communities` 表里的节点标识就变了。这是一个真实的行为差异，代码没有在 `cluster_graph` 的 docstring 里提示。

```python
swapped = edges[source_column] > edges[target_column]
edges.loc[swapped, [source_column, target_column]] = edges.loc[swapped, [target_column, source_column]].to_numpy()
```

第 3 步：**方向稳定化**——较小的节点名永远当 source。`>` 比较字符串，所以是字典序（归一化成大写后，字典序 = 大小写无关的字典序）。

`.to_numpy()` 是必需的：`edges.loc[swapped, [...]] = edges.loc[swapped, [...]].to_numpy()` 若右侧是 DataFrame，pandas 会尝试按索引对齐，而 `swapped` 是掩码、右侧的索引是原索引的子集——对齐可能导致部分 NaN。转成 ndarray 就绕开了对齐。

```python
edges = edges.drop_duplicates(subset=[source_column, target_column])
```

第 4 步：**去重**。注释说「去重那些在原数据里是反向对儿的边」。注意这里是 `keep="first"`（默认），而 `cluster_graph` 与 `modularity` 用的是 `keep="last"`。

但这一步的实际触发条件很窄：`cluster_graph` 在调 `stable_lcc` **之前**已经做过一次 `keep="last"` 的无向化去重，所以在正常情况下 `stable_lcc` 收到的是「已经没有重复对儿」的边表。这里能删掉的只可能是**归一化新造出来的重复**——例如 `("at&t", "apple")` 与 `("AT&T", "APPLE")` 归一化后都变成 `("AT&T", "APPLE")`，于是重复。

而这时的 `keep="first"` 与全局的 `keep="last"` **不一致**：两行归一化前权重可能不同，留哪一行决定边的权重。这是一个只在这种名字变体同时出现时才触发的差异，代码没有说明为什么这里选 `first`。

```python
return edges.sort_values([source_column, target_column]).reset_index(drop=True)
```

第 5 步：**排序 + 重置索引**。这是「同样的边集产生同样的行序」的最后一步，也是 c01 里「Leiden 对输入顺序敏感」那条的直接前置保障。`reset_index(drop=True)` 让输出有一个干净的 0..n-1 索引，好在后面 `zip(..., strict=True)` 里安全使用。

**注意 `stable_lcc` 保留了所有其他列**（`weight`、`text_unit_ids` 等），只重写了 source/target 并重排了行。所以它的输出可以直接继续走流水线，权重与溯源信息不丢。而这也意味着归一化后的名字**只在 source/target 两列里**——若下游有另一处按原始名字关联（例如关联实体表的 `title`），就会对不上。这是「归一化放在哪一层」的经典问题：`stable_lcc` 选择归一化边而不归一化实体表，所以 `use_lcc=True` 时边与实体表的名字可能不一致。当前 `cluster_graph` 只用边表、不 join 实体表，所以还没暴露。

**`largest_connected_component` 与 `stable_lcc` 的内部调用关系是单向的。**

`stable_lcc` 先归一化再调 `largest_connected_component`（用归一化后的名字算分量），所以「同一实体两种写法」在算连通性时已经是一个节点了。顺序是对的：若先算 LCC 再归一化，两个碎片可能因为名字不同而永远连不上，从而被剔出 LCC。

```python
lcc_nodes = largest_connected_component(edges, source_column=source_column, target_column=target_column)
```

注意它把列名参数透传下去——所以 `stable_lcc` 支持自定义列名，不是硬编码 `"source"`/`"target"`。而 `prune_graph`、`modularity.py` 里对 `largest_connected_component` 的调用都用默认列名。

**工作流包装层加了一道 `prune_graph` 自己没有的失败判定。**

```python
if len(pruned_entities) == 0:
    raise ValueError("Graph Pruning failed. No entities remain.")
if len(pruned_relationships) == 0:
    raise ValueError("Graph Pruning failed. No relationships remain.")
```

所以「剪得一个不剩」被当成硬失败而不是「得到一张空图继续跑」。理由很实际：空图会让后续 `create_communities` 产出零个社区，然后 `create_community_reports` 产出零份报告，索引「成功完成」但全局查询全部为空——那是最难归因的一类故障。**在剪枝这一步就炸，比在下游产出一份空索引好得多。**

而这个检查在 `index/workflows/prune_graph.py` 里、不在 `index/operations/prune_graph.py` 里，位置是刻意的：`operations` 层保持「纯函数」，`workflows` 层负责「这次运行的策略」。所以想「允许空图」的调用方可以直接用 `operations` 版绕过。

## 关键取舍

**源码事实：** `prune_graph` 只出现在 `_fast_workflows`（`extract_graph_nlp` 之后、`finalize_graph` 之前），`_standard_workflows` 没有它——LLM 抽取的产物不需要这一步统计剪枝。

**源码事实：** 七个参数里 `max_node_freq_std` 与 `max_node_degree_std` 默认 `None`（关闭）；默认真正生效的是 `remove_ego_nodes=True`（砍度数最高的**一个**节点）、`min_node_freq=2`、`min_edge_weight_pct=40`、`min_node_degree=1`、`lcc_only=False`。

**源码事实：** 度数从**原始**边表计算，且实体表里存在而边表里缺失的孤立节点用 `setdefault` 补 0——注释：「好让度数阈值在与之前相同的总体上来计算」。用 `setdefault` 而非直接赋值，否则会覆盖既有节点的真实度数。

**源码事实：** 两个标准差阈值的**计算总体不同**：度数用原始全量（含孤立点），频次用「度数与最小频次都已剔除之后」的幸存者。注释给出的理由是复刻 NX 的顺序变异语义。

**源码事实：** `_get_upper_threshold_by_std` 是 `mean + std_trim * std`，用 `np.std` 默认的总体标准差（`ddof=0`）。阈值没有绝对值参数，所以剪掉的是「分布尾部的一个比例」而不是绝对数量。

**源码事实：** 均值与标准差本身被尾部拉动，所以枢纽度数极高时「均值 + N·std」可能反而剪不掉它（幂律分布下正态假设不成立）；代码没有用中位数 + MAD 之类的稳健统计量补偿。

**源码事实：** `remove_ego_nodes` 只删 `max(degree_map, key=...)` 返回的**单个**节点；并列时取 `dict` 顺序里先出现的那个，代码对此无承诺。

**源码事实：** 频次过滤整块被 `if freq_col in remaining.columns` 守卫——缺少 `frequency` 列时**静默跳过**，于是「配了 `min_node_freq` 也不生效」。

**源码事实：** 边权重过滤用 `np.percentile` 取第 `min_edge_weight_pct` 百分位，保留 `>= min_weight` 的边；`pct = 0` 是「关闭」而不是「删掉所有权重低于最小值的边」。参数名容易被误读成绝对权重。

**源码事实：** 保留集统一由 `kept_titles = entity_titles - nodes_to_remove` 决定，边用 `source.isin(...) & target.isin(...)` 双向过滤，所以不会留下悬空边。

**源码事实：** `lcc_only` 用的是 `largest_connected_component`（不归一化、不排序）而不是 `stable_lcc`——所以剪枝结果依赖输入行序，而聚类用的 LCC 是稳定的。两者是不同函数，输出类型也不同（`set[str]` vs `DataFrame`）。

**源码事实：** `connected_components` 是并查集，只做**路径减半**（`parent[x] = parent[parent[x]]`）不做按秩合并；`union` 固定 `parent[ra] = rb`，所以根取决于边的处理顺序，进而「两个分量一样大时留哪个」由输入行序决定。

**源码事实：** `connected_components` 里的 `drop_duplicates` **不**归一化方向，因此 `(A,B)` 与 `(B,A)` 都会被 union 一次（幂等）——那里的去重只是性能优化，与 `compute_degree`/`modularity` 里为正确性而做的归一化去重不同。

**源码事实：** `all_nodes` 只由边表两列拼成，所以「只在实体表里的孤立节点」不出现在任何分量中——`lcc_only=True` 会连带剔除全部孤立实体。

**源码事实：** `stable_lcc` 的「稳定」由四步构成（名字归一化、方向归一化、去重、排序），且空输入直接 `.copy()` 返回以保持「总返回新对象」的一致性。

**源码事实：** `_normalize_name` 是 `html.unescape(...).upper().strip()`，所以 `AT&amp;T` 与 `AT&T`、`Apple` 与 `APPLE` 合并成同一节点；而它**只归一化边表的 source/target**，不归一化实体表，所以 `use_lcc=True` 时社区里的节点 id 是大写未转义的形式，而 `use_lcc=False` 时是原始名字。

**源码事实：** `stable_lcc` 的第 4 步去重用默认 `keep="first"`，而 `cluster_graph`/`modularity` 用 `keep="last"`；因为 `cluster_graph` 已在调用前做过 `keep="last"` 去重，`stable_lcc` 这里删掉的只可能是**归一化新造出来的**重复对儿——而那时留第一行还是最后一行会影响边的权重，代码未说明为何选 `first`。

**源码事实：** `stable_lcc` 先归一化名字、再算 LCC——顺序必须如此，否则「同一实体的两种写法」在算连通性时还是两个节点，可能被剔出 LCC。

**源码事实：** `stable_lcc` 保留其余所有列（权重、溯源），只重写 source/target 并重排行。

**源码事实：** 工作流层的 `prune_graph` 在剪枝后检查「实体或关系为空」并 `raise ValueError`，位置在 `workflows` 而 `operations` 保持纯函数——所以想让空图通过的调用方可直接用 `operations` 版。

**架构推论：** 剪枝这一层的真正价值不是「让图变小」，而是**把噪声从「下游无法处理」变成「上游已被决定」**。leiden 对枢纽节点、孤立节点、弱边都有定义，但那些定义在「实体关系图」这个语境下不一定有意义——一个出现在 2000 个块里的实体把社区边界抹平，一个只出现一次的实体形成单点社区。**与其在聚类结果上事后修正，不如在输入上先消除歧义**：这与 c01 里「在调用点消歧比在读结果时修正便宜」是同一条原则的第二次应用。

**架构推论：** 「阈值由数据自身决定」（均值 + N 个标准差）与「阈值绝对化」之间是一个真实的取舍。相对阈值的优点是跨规模可移植，缺点是**它对它要检测的现象不稳健**——幂律分布下尾部把统计量本身拉走。正确做法是稳健统计量，但那样就偏离了「与旧 NX 实现数值一致」这个约束。所以这里的选择是**保持数值等价、把旋钮默认关掉**：能力在，但需要运维知道它的边界才打开。

**架构推论：** 同一个图查询（LCC、无向化、去重）在这一组模块里有多个实现，每个都为自己的下游定制了输出类型与稳定性等级。这不是重复代码的疏忽，而是**「稳定性」是一个按需付费的属性**：算一次 `set` 最便宜（但不稳定），返回排序后的 `DataFrame` 贵（但可作为确定性输入）。判据是「下游会不会被顺序影响」——喂给 Leiden 会，所以那里必须 `stable_lcc`；只用来 `isin` 过滤不会，所以剪枝那里用便宜的那个。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/index/operations/prune_graph.py` | `prune_graph` | 七道过滤的总装配，返回剪枝后的实体表与关系表 |
| `packages/graphrag/graphrag/index/operations/prune_graph.py` | `_get_upper_threshold_by_std` | `mean + std_trim × std`，阈值由数据自身决定 |
| `packages/graphrag/graphrag/graphs/stable_lcc.py` | `stable_lcc` | 归一化 + LCC + 方向稳定 + 去重 + 排序，产出确定性边表 |
| `packages/graphrag/graphrag/graphs/stable_lcc.py` | `_normalize_name` | `html.unescape` + `upper` + `strip`，合并拼写变体 |
| `packages/graphrag/graphrag/graphs/connected_components.py` | `connected_components` | 并查集（路径减半）求全部分量，按大小降序 |
| `packages/graphrag/graphrag/graphs/connected_components.py` | `largest_connected_component` | 取 `components[0]`，不归一化不排序 |
| `packages/graphrag/graphrag/graphs/compute_degree.py` | `compute_degree` | 归一化去重后分别数 source/target 再相加 |
| `packages/graphrag/graphrag/index/workflows/prune_graph.py` | `prune_graph` | 配置解包 + 空结果判定（`ValueError`） |
| `packages/graphrag/graphrag/config/models/prune_graph_config.py` | `PruneGraphConfig` | 七个参数的配置面 |
| `packages/graphrag/graphrag/config/defaults.py` | `PruneGraphDefaults` | 默认值：`min_node_freq=2`、`remove_ego_nodes=True`、两个 `std` 为 `None` |
| `packages/graphrag/graphrag/index/workflows/factory.py` | `_fast_workflows` | `prune_graph` 只在此流水线中，位于 `extract_graph_nlp` 之后 |
| `packages/graphrag/graphrag/index/operations/cluster_graph.py` | `_compute_leiden_communities` | `stable_lcc` 的唯一调用点（`use_lcc=True` 时） |
| `packages/graphrag/graphrag/data_model/schemas.py` | `NODE_FREQUENCY` / `EDGE_WEIGHT` | 频次列与权重列的名字常量 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：index/operations/prune_graph.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/index/operations/prune_graph.py)
- [graphrag 源码：graphs/stable_lcc.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag/graphrag/graphs/stable_lcc.py)
- [并查集（Disjoint-set data structure）](https://en.wikipedia.org/wiki/Disjoint-set_data_structure)

## 与主核 / AI 的关系

主核里最接近的是**统计信息（statistics）与选择性估计**：优化器删不删一个索引、选不选索引扫描，取决于 `pg_statistic` 里的直方图与 ndistinct，而那些统计量本身是从数据算出来的、会被数据分布的形状影响。这里的 `mean + N·std` 是同一类东西的极简版——**用数据自身的分布决定一个截断点**。

与主核的 `VACUUM` / `ANALYZE` 对照更能说明这一层的取舍：`ANALYZE` 采样而不是全量，因为它要对大表便宜地拿到「足够好」的统计量；而 `_get_upper_threshold_by_std` 用全量 `np.mean`/`np.std`，因为它跑在「一次索引一次」的批处理里，成本不是问题。代价是它完全暴露在幂律分布的非稳健性下——而数据库的直方图正是为了解决同一个问题（用分桶代替矩统计，从而不被尾部拉动）。**同一个统计问题，一边用直方图，一边用均值方差，差别在于「能不能改输出语义」**：数据库的估计可以不准（有代价模型兜底），而这里的阈值直接决定删哪些节点，改语义要迁移说明。

与 c01（层次 Leiden）的分工：那一页讲「怎么切」，这一页讲「切之前洗什么」。而两页共享「在调用点消除歧义」这条原则——c01 是「强制无向 + 可选 LCC」，这里是「频次/度数/权重三道门槛」。

与 c02（模块度）的关系更紧：`_get_upper_threshold_by_std` 剪掉的正是「会让某个社区贡献极度负值」的那类节点，而 c02 的 `_modularity_components` 返回**每社区分数**的能力，恰好可以用来验证「剪完是否真的改善了分布」——而不是只看总分。两页合起来构成一条闭环：**剪枝 → 聚类 → 按社区评价 → 若某个社区负贡献异常则回头调剪枝参数**。

对 AI / agent 工程最实用的一条是**「剪枝要剪稀疏，而不是剪枢纽——除非你确认枢纽是噪声」**。默认配置选的是前者（高频实体、弱边、度数最低的），而「砍高度数节点」的旋钮存在但默认关闭。这个默认值值得照抄：枢纽节点承载的是真实的跨主题连接，删掉它会让图失去「这两个主题相关」这条信息，而这条信息通常是 RAG 里最有价值的部分。反过来，低频实体与弱边删掉了几乎无损失——它们本来就是抽取噪声。

另一条是「失败要在上游显式化」。剪枝后为空直接 `ValueError`，而不是让空图流到下游产出一份「成功但全空」的索引。任何多阶段管线都该在**每个阶段出口检查「输入是否已经退化到会让下游静默产出垃圾」**——因为下游的静默产出比上游的明确失败贵得多。

最后一条是那个 `use_lcc` 的副作用值得警惕：**归一化会改变标识符**。`stable_lcc` 为了让聚类稳定而把实体名大写化，于是开关一变，社区表里的节点名就变。任何「为了让结果稳定而做的归一化」都有这个风险——归一化发生在哪一层、影响哪些下游标识符，必须在引入时就想清楚，否则会出现「同一份数据、两个配置、两套 id」的长期混乱。
