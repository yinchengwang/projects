---
project: haystack
feature: joiners
title: 结果合并与分支汇合
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/joiners/document_joiner.py
    symbols: [DocumentJoiner, JoinMode, _finite_score]
  - path: haystack/components/joiners/branch.py
    symbols: [BranchJoiner]
  - path: haystack/components/joiners/answer_joiner.py
    symbols: [AnswerJoiner]
---

# 结果合并与分支汇合

## 一句话职责

三个 joiner 解决「多条支流汇成一条」这件不同的事：`DocumentJoiner` **合并多路检索结果**（五种合并模式，从简单拼接与加权求和到 RRF 与按分布归一），`BranchJoiner` **把多条支路接到同一个下游**（用于路由与循环回边），`AnswerJoiner` 只做 `Answer` 对象的拼接。

## 机制是什么

| | `DocumentJoiner` | `BranchJoiner` | `AnswerJoiner` |
|---|---|---|---|
| 输入 | `Variadic[list[Document]]`（多路各一个列表） | `GreedyVariadic[type_]`（**多个同类型单值**） | `Variadic[list[Answer]]` |
| 输出 | 一个文档列表 | **一个值** | 一个 Answer 列表 |
| 是否等所有输入 | 是（每路都到一个列表） | **否（收到一个就跑）** | 是 |
| 合并模式 | 五种 | 无（选第一个） | 一种 |
| 主要用途 | 混合检索后的结果融合 | 路由汇合、循环回边 | 多生成器的答案汇总 |

**最关键的差别在第二行与第三行**：`DocumentJoiner` / `AnswerJoiner` 是「归约器」（等齐再算），`BranchJoiner` 是「汇合点」（来一个就走）。而 `BranchJoiner` 的文档把这条限制写成了警告：

> 每次运行只应有一条连接的分支产出输出。`BranchJoiner` 一收到值就运行，所以如果多条分支都产出，**它会为每个值运行一次，它之后的组件也会运行多次**。

也就是说它**不是**「把多个值合并」，而是「在多条可能的来路中接住**那一条**」。要收集多条支路的输出，文档明确指向 `ListJoiner` 或 `DocumentJoiner`。

## 怎么实现的

### 一、`DocumentJoiner` 的五种模式：各自的「分数」定义不同

**五种模式可以按「怎么处理重复文档的分数」分成两族。**

| 模式 | 重复文档的处理 | 分数从哪来 |
|---|---|---|
| `concatenate` | 保留**分数最高**的那一份 | 原始分数 |
| `merge_embeddings` | 合并**嵌入字段**，其余取第一份 | 第一份的分数 |
| `merge` | 分数**加权求和** | 新分数 |
| `reciprocal_rank_fusion` | 按**排名**重算 | 新分数（RRF） |
| `distribution_based_rank_fusion` | 每路**按自身分布归一**后取最高 | 新分数（归一后） |

**`concatenate` 是默认模式，而它的实现是「按 id 分组后取最高分」。**

```
def _concatenate(document_lists):
    output = []
    docs_per_id = defaultdict(list)
    for doc in itertools.chain.from_iterable(document_lists):
        docs_per_id[doc.id].append(doc)
    for docs in docs_per_id.values():
        doc_with_best_score = max(docs, key=lambda doc: doc.score if doc.score is not None else -inf)
        output.append(doc_with_best_score)
    return output
```

**两个细节：**

1. **用 dict 保序** —— Python 3.7+ 的 `dict` 保持插入顺序，于是输出的顺序是「每个 id 首次出现的顺序」。这让结果可复现。
2. **`score is None` 用 `-inf`** —— 与 `f01`/`f03` 里一致的兜底。

**注意 `concatenate` 名字里的「拼接」其实不准确**：它做了去重（按 id），只保留分数最高的那份。真正的「纯拼接不去重」只有 `AnswerJoiner` 才有。

**`merge`：加权求和，而它的除零保护写在了注释里。**

```
# This check prevents a division by zero when no documents are passed
if not document_lists: return []
scores_map = defaultdict(int)
documents_map = {}
weights = self.weights if self.weights else [1 / len(document_lists)] * len(document_lists)
for documents, weight in zip(document_lists, weights, strict=True):
    for doc in documents:
        scores_map[doc.id] += (doc.score if doc.score is not None else 0) * weight
        documents_map[doc.id] = doc
return [replace(doc, score=scores_map[doc.id]) for doc in documents_map.values()]
```

**三个要点：**

1. **`weights` 未给时均匀分配**（`1/n`）——于是默认行为是「各路等权平均」。而这也是它与 `concatenate` 的实质差别：`merge` 把「同一文档在多路都被命中」当作**更强的信号**（分数累加），而 `concatenate` 只取最好的一份。
2. **`documents_map[doc.id] = doc` 会让「最后出现的那份」决定其他字段** —— 而分数是累加的。于是内容的来源是「最后一次出现」，分数是「全部出现之和」。这是一个微妙的不一致（若不同路的同 id 文档内容不同，取哪个取决于顺序）。
3. **`zip(..., strict=True)`** —— 于是 `weights` 长度与 `document_lists` 不一致时立即报错，而不是静默截断。

**`merge_embeddings` 是唯一一个会因「冲突」而报错的模式。**

```
for field in ("embedding", "sparse_embedding", "multivector_embedding"):
    old, new = getattr(current, field), getattr(doc, field)
    if old is not None and new is not None and old != new:
        raise ValueError(f"Conflicting {field} for document '{doc.id}'")
    updates[field] = old if old is not None else new
```

**判定条件是「两者都非 `None` 且不相等」** —— 于是「一边有、一边没有」是正常的（取有的那份），只有「两边都有但不同」才算冲突。

**它保留了第一份的 `content` / `meta` / `score`。**

文档写明："Keep all other fields, including `content`, `meta`, and `score`, from the first document."。**这与 `merge` 的行为相反**（后者取最后一份的内容、累加分数）。

**这个模式的实际用途是「补全同 id 文档的不同向量表示」** —— 同一个块被稠密、稀疏、多向量三个 embedder 各嵌入一次，写了三条文档（同 id、各自的嵌入字段只有一个非空），用这个模式合成一条完整的。

**`distribution_based_rank_fusion` 是五种里最讲究的一个，而它的核心是「按每路自身的分布归一」。**

```
scores_list = [_finite_score(doc.score) for doc in documents]
mean_score = sum(scores_list) / len(scores_list)
std_dev = (sum((x - mean_score) ** 2 for x in scores_list) / len(scores_list)) ** 0.5
min_score = mean_score - 3 * std_dev
max_score = mean_score + 3 * std_dev
delta_score = max_score - min_score
scores_vary = min(scores_list) != max(scores_list)
```

**用 `mean ± 3σ` 当归一区间的上下界，而不是用实际的 `min`/`max`。** 这是一个抗离群值的选择：一个极端分数不会把整个区间挤扁。代价是**超出这个区间的分数会被映射到 [0,1] 之外**（没有截断）。

**而 `scores_vary` 与 `delta_score != 0` 是两个独立守卫。**

```
if scores_vary and delta_score != 0.0:
    score = (score - min_score) / delta_score
```

- **`scores_vary`**：所有分数相同时**保留原分数**——注释写着「如果所有文档分数相同，归一化无法增加信息，保留它们的原始分数」。
- **`delta_score != 0.0`**：防止除零。而 `delta_score = 6σ`，只有在 `σ == 0` 时为 0。

**这两个条件在这里其实是等价的**（`σ == 0` ⇔ 所有相同），所以 `and` 是冗余的。但它把两个不同的意图分别表达了出来，读代码时能看出「这两种情况都想过」。

**归一后它调的是 `_concatenate`。**

```
return DocumentJoiner._concatenate(document_lists=rescaled_lists)
```

**也就是说重复文档按「归一后分数最高的那份」保留。** 而因为每路的归一化是**独立**的，同一个文档在 A 路归一后 0.9、在 B 路归一后 0.4 是可能的——于是 `concatenate` 会取 A 路那份，`score=0.9`。

**这个模式的定位在注释给的链接里说明了**：DBSF 是一种「不依赖排名、也不假设分数同分布」的融合方式。它与 RRF 的差别是**保留了分数的相对距离信息**（RRF 只看排名）。

**`_finite_score` 是一个只有三行但有几句话注释的函数。**

```
def _finite_score(score: float | None) -> float:
    """
    Return the score to use in the arithmetic of a join mode.

    Missing and non-finite (NaN or infinity) scores are both treated as 0. A non-finite score that reaches the
    arithmetic of a join mode does not stay local to its own Document, it turns the scores of the other Documents
    into NaN as well.
    """
    return score if score is not None and isfinite(score) else 0.0
```

**注释里那句「它不会只影响自己那篇文档，而会把其他文档的分数也变成 NaN」是这一页最值得记的一句。** 具体机制：`NaN` 参与任何算术都产出 `NaN`，而求 `mean` / `std_dev` 会把整个列表的统计量污染成 `NaN`，于是**整路的归一化全部失效**。

而它在 `distribution_based_rank_fusion` 里被用在两处：算 `scores_list`（喂给统计量）与**警告里单独识别哪些文档是非有限分数**。

```
non_finite_ids = [doc.id for doc in documents if doc.score is not None and not isfinite(doc.score)]
if non_finite_ids:
    logger.warning("... They were treated as if their score was 0 while rescaling the scores of the list, otherwise every score in the list would become NaN. Documents with IDs: {document_ids}", ...)
```

**警告文案把「为什么」与「哪些」都给了。** 这是一个好的错误消息：既解释后果（否则全部变 NaN），又列出受影响的 id。

**而 `_finite_score` 只被这一个模式用** —— 其它模式要么不参与算术（`concatenate` 只比较、`merge_embeddings` 不碰分数），要么用 `doc.score if ... else 0` 的内联写法（`merge`）。**于是同一个 `None` 处理在不同模式里以三种形式出现**：`_finite_score`、内联三元、与 `-inf` 排序键。这是这个文件里一个不一致之处。

### 二、`run` 的公共收尾：排序与截断

**五种模式返回后，`run` 做统一的排序与截断。**

```
if self.sort_by_score:
    output_documents = sorted(output_documents, key=lambda doc: doc.score if doc.score is not None else -inf, reverse=True)
    if any(doc.score is None for doc in output_documents): logger.info("Some of the Documents ... have score=None. ...")
```

**`sort_by_score` 默认 `True`** —— 于是默认行为是「合并后按分数降序」。这与 `f01` 里 `MultiRetriever` 的默认行为（RRF）不同，但两者的结果都是有序的。

**而那个 `logger.info` 的措辞很具体：「Those with score=None were sorted as if they had a score of -infinity」** —— 它说明的不是「发生了什么」而是「怎么处理的」。

**`top_k` 的两级检查有一个不对称。**

```
if top_k is not None:
    if top_k < 0: raise ValueError("top_k must not be negative.")
    output_documents = output_documents[:top_k]
elif self.top_k is not None:
    output_documents = output_documents[: self.top_k]
```

**运行期的 `top_k` 只拒绝负数，允许 0**（= 返回空列表）。而构造期的 `top_k` 需要读 `__init__` 才知道是否允许 0 —— 这与 `f01` 里 `MultiRetriever` 的 `_resolve_top_k` 是**同一个模式**（构造严格、运行宽松）。这个模式在两个文件里重复出现，说明它是本仓的一个约定。

### 三、`BranchJoiner`：类型即配置

**它的全部配置就是一个类型。**

```
def __init__(self, type_: type) -> None:
    self.type_ = type_
    component.set_input_types(self, value=GreedyVariadic[type_])
    component.set_output_types(self, value=type_)
```

**它用 `component.set_input_types` 在运行期设置 socket 类型**，而不是靠类上的装饰器固定。原因是类型来自构造参数——**一个 `BranchJoiner(list[ChatMessage])` 与一个 `BranchJoiner(str)` 的 socket 类型不同**。

**`GreedyVariadic[type_]` 是关键**：greedy 意味着「来一个就跑」（不等所有上游），这正是汇合点需要的语义。而 `a03`（组件就绪判定）那页里讲的「贪婪 vs 懒惰可变 socket」在这里被用上了。

**`run` 的实现只有两行，而校验用的是「恰好一个」。**

```
if (inputs_count := len(kwargs["value"])) != 1:
    raise ValueError(f"BranchJoiner expects only one input, but {inputs_count} were received.")
return {"value": kwargs["value"][0]}
```

**为什么是「恰好一个」而不是「至少一个」？** 因为 greedy socket 的语义是「有一个就跑」，于是正常情况下 `value` 列表里只有一个元素。出现多个意味着**多条分支同时产出了** —— 那是一个使用错误，而不是需要处理的情形。

**而错误消息带上实际数量** —— 于是「2 were received」直接告诉你「你接了两条会同时产出的分支」。

**注意这与文档里那句「它一收到值就运行，所以如果多条分支都产出，它会为每个值运行一次」并不完全一致。** 文档描述的是 pipeline 层面的行为（每次运行消费一个值，下一个值触发下一次运行），而 `run` 里的这个校验是**单次调用**的防御。两者不矛盾，但读的时候需要分清层次：**单次调用要求恰好一个，多次调用可能因为多个值而发生多次。**

**它的序列化有一个关于「不要改调用方数据」的注释。**

```
# Copy so the caller's data keeps the serialized type and can be deserialized again
init_parameters = dict(data["init_parameters"])
init_parameters["type_"] = deserialize_type(init_parameters["type_"])
return default_from_dict(cls, {**data, "init_parameters": init_parameters})
```

**它 `dict(...)` 浅拷贝 `init_parameters` 再改**，于是 `data` 里的原始序列化类型（字符串）保持不变。若不拷贝，`data["init_parameters"]["type_"]` 会被替换成类型对象，**这个 dict 就再也无法被反序列化第二次**（因为它已经是对象而不是字符串了）。

**这是一个「反序列化要可重入」的正确做法**，而注释明确写出了原因。而它同时构造了新的顶层 dict（`{**data, ...}`），于是 `data` 本身也没被改。

**`serialize_type` / `deserialize_type` 这对函数处理「类型对象的序列化」** —— 因为 `type` 不能直接进 JSON/YAML。这解释了为什么 `to_dict` 里写 `serialize_type(self.type_)` 而 `from_dict` 里要还原。

**它最有价值的用法是「循环回边」。** 文档示例是「生成 → 校验 → 失败则把错误送回 joiner → 重新生成」：

```
pipe.connect("joiner", "generator")
pipe.connect("generator.replies", "validator.messages")
pipe.connect("validator.validation_error", "joiner")
```

**这个拓扑能成立靠两件事**：`BranchJoiner` 的 greedy 语义（有输入就跑，于是循环能推进）与「每次运行只有一个值」（于是不会因为回边与初始输入同时到达而失控）。

### 四、`AnswerJoiner`：一个只有一种模式的归约器

**它的 `JoinMode` 枚举只有一个值。**

```
class JoinMode(Enum):
    CONCATENATE = "concatenate"
```

**为什么要定义只有一项的枚举？** 从代码看是为了**保持接口一致**（与 `DocumentJoiner` 同样的构造参数形态）与**给未来留位置**。而 `AnswerJoiner` 文档写明了「目前这个组件只支持一种 join mode」。

**而 `_concatenate` 是真拼接，不去重。**

```
def _concatenate(self, answer_lists):
    return list(itertools.chain.from_iterable(answer_lists))
```

**这与 `DocumentJoiner._concatenate`（按 id 去重取最高分）完全不同** —— 同名方法、同样叫 concatenate，行为相反。

理由是数据不同：`Document` 有 `id`，同一份文档被多路检索命中是常态、去重有意义；而 `Answer` 没有跨路可比的 id（每次生成都是新的答案），于是「来自两个生成器的两个答案」本来就该都保留。

**它的排序用了一个 walrus 表达式。**

```
output_answers = sorted(output_answers, key=lambda answer: score if (score := getattr(answer, "score", None)) is not None else -inf, reverse=True)
```

`getattr(answer, "score", None)` 而不是 `answer.score` —— 因为 `AnswerType` 是 `GeneratedAnswer | ExtractedAnswer` 的联合，而**`ExtractedAnswer` 可能没有 `score` 属性**。用 `getattr` 带默认值比 `isinstance` 分支更简洁。

**`sort_by_score` 默认 `False`** —— 与 `DocumentJoiner` 的默认 `True` 相反。理由可能是：答案的分数本来就不一定可比（不同生成器的评分标准不同），默认不排序更安全。

**而 `top_k` 的两级检查在这里是「构造期只允许 `None` 或正数，运行期只拒绝负数」**：

```
if top_k is not None and top_k <= 0: raise ValueError("top_k must be greater than 0.")
...
if top_k is not None:
    if top_k < 0: raise ValueError("top_k must not be negative.")
```

**构造期拒绝 0、运行期允许 0** —— 与 `DocumentJoiner` 和 `MultiRetriever` 完全相同的模式，第三次出现。

## 关键取舍

**源码事实：** `DocumentJoiner` 有五种模式，按「怎么处理重复文档的分数」分成两族：保留原始分数（`concatenate` / `merge_embeddings`）与重算分数（`merge` / `reciprocal_rank_fusion` / `distribution_based_rank_fusion`）。

**源码事实：** `concatenate`（默认）**不是纯拼接**——它按 id 去重并保留分数最高的那份；真正的纯拼接只在 `AnswerJoiner` 里。

**源码事实：** `merge` 把「同一文档在多路被命中」当作更强信号（分数累加），默认权重均匀（`1/n`）；但内容的来源是「最后出现的那份」而分数是「全部之和」——两者来自不同的实例。

**源码事实：** `merge_embeddings` 只在「两个都非 `None` 且不相等」时报冲突，保留第一份的 `content`/`meta`/`score`；用途是补全同一块的不同向量表示。

**源码事实：** `distribution_based_rank_fusion` 用 `mean ± 3σ` 当归一区间（抗离群值，但超界分数不被截断），并在「分数全同」时保留原值（注释：「归一化无法增加信息」）；归一后调 `_concatenate` 取最高份。

**源码事实：** `_finite_score` 的注释指出关键后果：非有限分数**不只影响自己那篇**，会把同路其它文档的分数一起污染成 `NaN`（因为统计量被污染）。

**源码事实：** 同一个「缺分数」的处理在这个文件里有三种形态：`_finite_score` 函数、内联三元、`-inf` 排序键——不一致。

**源码事实：** `run` 的 `sort_by_score` 默认 `True`；`top_k` 构造期严格（拒绝 `<=0`）、运行期宽松（只拒绝 `<0`，允许 0）；这个不对称在 `MultiRetriever` 与 `AnswerJoiner` 里第三次出现。

**源码事实：** `BranchJoiner` 用 `component.set_input_types` 在运行期设 socket 类型为 `GreedyVariadic[type_]`——类型来自构造参数，所以不能用类装饰器固定。

**源码事实：** `BranchJoiner.run` 要求输入**恰好一个**（不是至少一个），因为 greedy socket 语义下多个值意味着多条分支同时产出，是使用错误。

**源码事实：** 文档层面「多条分支都产出时会为每个值运行一次」与 `run` 层面「单次调用要求恰好一个」是不同层次的约束，不矛盾但需要分清。

**源码事实：** `BranchJoiner.from_dict` 浅拷贝 `init_parameters` 再改 `type_`，注释说明是为了「调用方的数据保持序列化类型、可再次反序列化」——反序列化的可重入性。

**源码事实：** `AnswerJoiner` 的 `JoinMode` 只有一项；它的 `_concatenate` 与 `DocumentJoiner._concatenate` 同名但行为相反（一个纯拼接、一个去重取最高），因为 `Answer` 没有跨路可比的 id。

**源码事实：** `AnswerJoiner` 用 `getattr(answer, "score", None)` 而不是 `answer.score`，因为联合类型里 `ExtractedAnswer` 可能没有 `score`；它的 `sort_by_score` 默认 `False`（与 `DocumentJoiner` 相反）。

**架构推论：** 这三个 joiner 的分野在于**「归约」与「汇合」是两种不同的同步语义**。

归约器（`DocumentJoiner` / `AnswerJoiner`）需要**所有输入到齐**才能算出正确结果（因为要比较与累加），所以它们用 variadic socket 等齐。汇合点（`BranchJoiner`）只需要**一个输入**就能往下走，所以它用 greedy socket——代价是它无法合并多个值，而正是这个「缺点」让循环回边成为可能（每次运行只推进一个值）。

**把这两者混淆是数据流管道里最常见的错误**：用 `BranchJoiner` 去接两条会同时产出的分支，会得到「下游跑两次」；用 `DocumentJoiner` 去接路由的两个互斥分支，会永远等不齐。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/joiners/document_joiner.py` | `DocumentJoiner` | 五种模式的结果融合，统一排序与截断 |
| `haystack/components/joiners/document_joiner.py` | `JoinMode` | 五种模式的枚举，各带 `__str__` / `from_str` |
| `haystack/components/joiners/document_joiner.py` | `_finite_score` | 把缺分数与非有限分数都当 0，并说明污染后果 |
| `haystack/components/joiners/document_joiner.py` | `_concatenate` | 按 id 去重、保留分数最高份 |
| `haystack/components/joiners/document_joiner.py` | `_merge` | 加权求和，默认均匀权重，`zip(strict=True)` |
| `haystack/components/joiners/document_joiner.py` | `_merge_embeddings` | 合并三种嵌入字段，冲突则报错 |
| `haystack/components/joiners/document_joiner.py` | `_rrf` | 转调共享的 `_reciprocal_rank_fusion` |
| `haystack/components/joiners/document_joiner.py` | `_distribution_based_rank_fusion` | `mean ± 3σ` 归一，全同则保留原值 |
| `haystack/components/joiners/branch.py` | `BranchJoiner` | 汇合点，`GreedyVariadic` socket |
| `haystack/components/joiners/branch.py` | `set_input_types` / `set_output_types` | 运行期按构造参数设 socket 类型 |
| `haystack/components/joiners/branch.py` | `run` | 要求恰好一个输入，多个则报错 |
| `haystack/components/joiners/branch.py` | `from_dict` | 浅拷贝避免污染调用方数据，保证可重入反序列化 |
| `haystack/components/joiners/answer_joiner.py` | `AnswerJoiner` | 纯拼接 Answer 列表，可选按分数排序 |
| `haystack/components/joiners/answer_joiner.py` | `_concatenate` | 真拼接不去重，与 DocumentJoiner 同名异义 |

## 参考

- [haystack 源码：joiners/document_joiner.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/joiners/document_joiner.py)
- [haystack 源码：joiners/branch.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/joiners/branch.py)
- [论文：Reciprocal Rank Fusion](https://plg.uwaterloo.ca/~gvcormac/cormacksigir09-rrf.pdf)
- [DBSF：Distribution-Based Score Fusion](https://medium.com/plain-simple-software/distribution-based-score-fusion-dbsf-a-new-approach-to-vector-search-ranking-f87c37488b18)
- [haystack 文档：Joiners](https://docs.haystack.deepset.ai/docs/joiners)

## 与主核 / AI 的关系

`DocumentJoiner` 的几种模式与主核里的**多索引扫描结果归并**是同一类问题：数据库也要把多个索引扫描的结果合并成一个有序流。而主核的做法几乎总是**按行标识去重 + 按排序键归并**（等价于这里的 `concatenate`），因为它有明确的行标识与排序键。

**RRF 与 DBSF 在主核里没有对应物**，因为它们要解决的是「两个排序的分数不可比」——数据库没有「相关度」这个概念，所有比较都是确定性的。这类融合只在「相关性由模型或启发式给出」的系统里才有意义。

而 `BranchJoiner` 的汇合语义在主核里对应的是**控制流汇合**（两个条件分支汇到同一点）——数据库的执行计划里也有这种结构（`Merge Join` 的两个子计划、`CASE` 表达式）。而它的 greedy 语义正是「谁来就用谁」，与 `DocumentJoiner` 的「等齐」形成对照。

对 agent 工程的直接价值有三条。其一，**「归约」与「汇合」不能混用**。接路由分支要用 `BranchJoiner`（等一条），接并行检索结果要用 `DocumentJoiner`（等所有）——用错的表现是「下游跑多次」或「永远等不齐」，而两种都不会报错。其二，**非有限分数会污染整路**。一个 `NaN` 进到求均值里，整路的归一化全部失效，所以必须在进入算术前替换掉——`_finite_score` 的注释把这条后果写清楚了，这正是它存在的理由。其三，**同名函数可以语义相反**。`DocumentJoiner._concatenate` 去重、`AnswerJoiner._concatenate` 不去重，两者都叫 concatenate。判断依据不是名字而是数据类型本身有没有跨路可比的身份。
