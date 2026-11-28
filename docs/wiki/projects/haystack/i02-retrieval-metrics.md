---
project: haystack
feature: retrieval-metrics
title: 检索指标
page_type: feature
schema_version: 1
refs:
  - path: haystack/components/evaluators/document_recall.py
    symbols: [DocumentRecallEvaluator, RecallMode]
  - path: haystack/components/evaluators/document_mrr.py
    symbols: [DocumentMRREvaluator]
  - path: haystack/components/evaluators/document_map.py
    symbols: [DocumentMAPEvaluator]
  - path: haystack/components/evaluators/answer_exact_match.py
    symbols: [AnswerExactMatchEvaluator]
---

# 检索指标

## 一句话职责

四个**完全确定性**的评测组件：三个把「每个问题一组正确答案 / 一组检索结果」折成 `0..1` 的分数（Recall 有单命中与全命中两种口径、MRR 看第一个命中排多高、MAP 看所有命中的排名质量），第四个把「预测答案与标准答案」做逐条字符串相等——它们都不调模型，所以结果是可复现的。

## 机制是什么

四个组件共享同一条数据形状：`list[list[Document]]` × `list[list[Document]]`（外层是问题、内层是该问题的文档），输出都是 `{"score": float, "individual_scores": list[float]}`，其中 `score` 是**未加权算术平均**。第四个不同：输入是 `list[str]` × `list[str]`，逐条比一次。

三个文档类指标共享同一套「怎么判断两个文档相同」的机制——`document_comparison_field` 三选一：

- `"content"` → `doc.content`
- `"id"` → `doc.id`
- `"meta.<key>"` → 沿 `doc.meta` 逐层下钻（支持嵌套路径如 `meta.source.url`）

三处 `_get_comparison_value` 的实现几乎逐字相同（每个文件各有一份），差别只有一处——`DocumentRecallEvaluator` 在返回前多调一次 `_make_metadata_value_hashable`。

三个组件另有一个共同的可选参数 `top_k`：设了就只评每个问题的前 k 条检索结果。

## 怎么实现的

**`RecallMode` 的两种口径决定了 Recall 的两个不同问题。**

```python
SINGLE_HIT = "single_hit"   # Score is based on whether any document is retrieved.
MULTI_HIT = "multi_hit"     # Score is based on how many documents were retrieved.
```

实现差异只有一行：

```python
def _recall_single_hit(self, ground_truth_documents, retrieved_documents) -> float:
    unique_truths, unique_retrievals = self._comparison_sets(ground_truth_documents, retrieved_documents)
    if not unique_truths or not unique_retrievals:
        return 0.0
    return float(len(unique_truths.intersection(unique_retrievals)) > 0)

def _recall_multi_hit(self, ground_truth_documents, retrieved_documents) -> float:
    unique_truths, unique_retrievals = self._comparison_sets(ground_truth_documents, retrieved_documents)
    if not unique_truths or not unique_retrievals:
        return 0.0
    return len(unique_truths.intersection(unique_retrievals)) / len(unique_truths)
```

所以 `single_hit` 问的是「**有没有捞到任意一条**」（二值），`multi_hit` 问的是「**捞到了标准的百分之几**」（比例）。这就是为什么 `run` 的 docstring 专门注明：「若模式是 `single_hit`，逐条分数是 0 或 1」。

`RecallMode.from_str` 是给反序列化用的：

```python
enum_map = {e.value: e for e in RecallMode}
mode = enum_map.get(string)
if mode is None:
    raise ValueError(f"Unknown recall mode '{string}'. Supported modes are: {list(enum_map.keys())}")
```

错误消息里列出所有支持值，所以拼错时不必查文档。而 `__str__` 返回 `self.value` 让 `to_dict` 能写 `mode=str(self.mode)` ——**序列化的是字符串而不是枚举成员名**，所以 `"single_hit"` 而不是 `"SINGLE_HIT"`。

**「去重」是所有三个指标的共同前提，而它有个容易忽略的副作用。** `_unique_comparison_values`：

```python
return {value for doc in documents if (value := self._get_comparison_value(doc)) not in ("", None)}
```

两句合成一句：**用海象运算符取值并同时过滤空值**。所以「缺失」（`None`）与「空字符串」（`""`）都被排除，不参与计算。

去重的必要性在 `multi_hit` 里最明显：若标准答案里同一篇文档出现两次（例如两个 chunk 内容相同），不去重的话分母会翻倍而分子最多算一次，分数被压低。而检索结果侧的重复更常见——多路召回合并时同一条可能来两次。

去掉重复项带来一个副作用：**「检索返回了 10 条但只有 3 条唯一」时，Recall 与 MRR 的计算依据是那 3 条唯一的**。而 MRR 更微妙——它要的是「第一个命中的排名」，去重会改变排名。

**`_make_metadata_value_hashable` 只有一个指标用了，而它的存在理由很实在。** 这个函数把不可哈希的元数据值转成可哈希的：

```python
if isinstance(value, list):
    return ("list", tuple(_make_metadata_value_hashable(item) for item in value))
if isinstance(value, tuple):
    return ("tuple", tuple(_make_metadata_value_hashable(item) for item in value))
if isinstance(value, dict):
    return ("dict", frozenset((_make_metadata_value_hashable(key), _make_metadata_value_hashable(item)) for key, item in value.items()))
return value
```

三种容器各有一个**类型标签**（`"list"` / `"tuple"` / `"dict"`）作为元组的首元素——所以 `[1, 2]` 与 `(1, 2)` 不会被当成相等（它们的转换结果首元素不同）。字典转 `frozenset` 意味着**字典的键顺序不影响相等性**，这是正确的语义。

而 `DocumentRecallEvaluator` 之所以需要它，是因为它把值放进 `set` 里做交集运算：

```python
unique_truths = self._unique_comparison_values(ground_truth_documents)
...
return len(unique_truths.intersection(unique_retrievals)) / len(unique_truths)
```

`set` 要求元素可哈希，而元数据值可能是列表或字典。而 MRR 与 MAP 用的是 `list` / `in` 而不是 `set`，所以**不需要**这一步——但代价是复杂度从 O(1) 查找变成 O(n) 线性扫描。

这个不一致是有意的取舍：Recall 只需要「交集大小」，用集合最自然；MRR 需要「第一个命中的**位置**」，必须按顺序遍历，用列表；MAP 需要「按序累积并从未命中池里移除」，也只能用列表。**三个指标的数据结构由各自的算法需求决定，而不是统一**。

而 MAP 里那段注释把这个取舍写明了：

```python
# A list keeps the deduplication working for unhashable comparison values, for example when
# document_comparison_field points to a meta key holding a list.
uncredited_ground_truth_values: list[Any] = []
```

所以它是**为了支持不可哈希值而故意不用集合**——牺牲查找速度换取「能处理列表/字典作为比较值」。

**MRR 的实现只有十几行，但三处判据都有讲究。**

```python
for ground_truth, retrieved in zip(ground_truth_documents, retrieved_documents, strict=True):
    reciprocal_rank = 0.0
    ground_truth_values = []
    for doc in ground_truth:
        value = self._get_comparison_value(doc)
        if value in ("", None):
            continue
        ground_truth_values.append(value)
    if not ground_truth_values:
        logger.warning("There are no ground truth documents or none of them contain a valid comparison value. Score will be set to 0.")

    for rank, retrieved_document in enumerate(retrieved[: self.top_k]):
        retrieved_value = self._get_comparison_value(retrieved_document)
        if retrieved_value is None:
            continue
        if retrieved_value in ground_truth_values:
            reciprocal_rank = 1 / (rank + 1)
            break
    individual_scores.append(reciprocal_rank)
```

三处：

1. **标准答案侧过滤 `("", None)`，检索侧只过滤 `None`。** 不对称是刻意的：标准答案里的空值应该被忽略（它不能标识相关文档），而检索结果里的空字符串仍占一个排名位置——若跳过它不计数，后面文档的排名会前移，与真实排序不符。所以检索侧 `""` 参与排名但不可能命中（因为标准答案侧已经排除了 `""`）。

2. **`rank` 是 `enumerate` 的下标，所以 `1/(rank+1)` 是「第 1 名得 1.0、第 2 名得 0.5」。** 这正是 MRR 的定义。

3. **`break` 后不再看其余检索结果** ——MRR 只关心第一个命中。

而 `retrieved[: self.top_k]` 在 `top_k=None` 时切片 `[:None]` 等于整个列表，所以这一行同时处理了「不限」与「截断」两种情况，不需要 `if`。

**MAP 的实现要维护一个「还没被计入的标准答案」池，而它的移除操作决定了正确性。**

```python
for rank, retrieved_document in enumerate(retrieved[: self.top_k]):
    retrieved_value = self._get_comparison_value(retrieved_document)
    if retrieved_value is None:
        continue
    if retrieved_value in uncredited_ground_truth_values:
        uncredited_ground_truth_values.remove(retrieved_value)
        retrieved_relevant_documents += 1
        average_precision_numerator += retrieved_relevant_documents / (rank + 1)
if total_relevant_documents:
    average_precision = average_precision_numerator / total_relevant_documents
```

关键在于 `remove` ——**一条标准答案只能被计入一次**。若同一篇相关文档在检索结果里出现两次而不移除，第二次命中会再次累加精度，虚高分数。所以「移除」不是优化而是正确性要求，这也是为什么它不能简单地用一个 `set` 做成员检查而必须真的从池里删掉。

分子累加的是 `retrieved_relevant_documents / (rank + 1)` ——即「到当前排名为止的精度」。所以第 1 名命中得 1/1、第 3 名命中（且之前命中过 1 次）得 2/3。最后除以标准答案总数，得到平均精度。

`if total_relevant_documents:` 保护了除零，而 `total_relevant_documents == 0` 时 `average_precision` 保持初值 `0.0`。

**`top_k` 对 MAP 的语义与对 Recall/MRR 不同，docstring 专门说明了。**

「若设了，只有每个问题的前 `top_k` 条检索文档被打分，这给出截断 k 处的指标。**平均精度仍然除以相关文档的数量，所以排在 `top_k` 之后的相关文档算作漏掉**」。

这句话点出了一个真实的陷阱：`top_k=10` 时，若标准答案有 5 条而检索结果的前 10 条里只命中 2 条、另外 3 条在第 15 条，那 MAP@10 的分母仍是 5，分数会比「只看命中的 2 条」低。**这正是「截断」应有的语义**（@k 指标就该惩罚排在 k 之后的答案），但它与「只看前 k 条重新计算」这种直觉不同。

**`AnswerExactMatchEvaluator` 是最短的一个，而它的一处注释说错了自己的语义。**

实现：

```python
matches = []
for truth, extracted in zip(ground_truth_answers, predicted_answers, strict=True):
    if truth == extracted:
        matches.append(1)
    else:
        matches.append(0)
average = sum(matches) / len(predicted_answers)
return {"individual_scores": matches, "score": average}
```

`truth == extracted` 是精确字符串比较，没有任何归一化（不 strip、不小写、不处理标点）。类 docstring 也明说「exactly」。而注释：

```python
# The proportion of questions where any predicted answer matched one of the ground truth answers
```

**这句注释与实现不符**：实现是「逐条位置对齐比较」，一个预测答案只与对应位置的那一个标准答案比，不是「与任意一个标准答案比」。所以若 `ground_truth_answers=["Berlin", "Paris"]` 而 `predicted_answers=["Paris", "Berlin"]`，结果是 `[0, 0]` 而不是 `[1, 1]`。

而它的 docstring 又确实写着「检查预测答案是否与**任意一个**标准答案匹配」——所以**文档与注释都在描述一个更宽松的语义，而代码实际是严格的位置对齐**。这是一个真实的文档与实现不一致，值得记下来。

从使用方式看，实现是合理的：两个等长列表按位置配对，`strict=True` 保证长度相等（虽然它前面已经检查过）。若真要做「任意匹配」，两个列表就不该要求等长。所以是文档滞后于实现。

**三个文档指标与答对指标共享一组「输入前置检查」，措辞逐字相同。**

```python
if len(ground_truth_documents) == 0 or len(retrieved_documents) == 0:
    raise ValueError("ground_truth_documents and retrieved_documents must be provided.")
if len(ground_truth_documents) != len(retrieved_documents):
    raise ValueError("The length of ground_truth_documents and retrieved_documents must be the same.")
```

先判空再判长度——顺序有意义：若两个都是空列表，长度相等，所以只判长度会放过它，然后 `sum(scores) / len(...)` 就除零了。所以「判空」必须在前面。

`AnswerExactMatchEvaluator` 的第二处判断写成了 `if not len(...) == len(...)` 而不是 `!=` ——语义相同，风格差异。

**`top_k` 的校验被复制了三份，且都防住了 `bool`。**

```python
if top_k is not None and (isinstance(top_k, bool) or not isinstance(top_k, int) or top_k < 1):
    msg = f"top_k must be a positive integer or None, got {top_k!r}."
    raise ValueError(msg)
```

`isinstance(top_k, bool)` 这一条是必要的：**Python 里 `bool` 是 `int` 的子类**，所以 `top_k=True` 会通过 `isinstance(top_k, int)` 检查并等于 1。显式排除 `bool` 才能让 `True` 报错而不是被当成 1。这是一个很容易漏的防御。

## 关键取舍

**源码事实：** `RecallMode` 两口径：`single_hit` 判「有没有捞到任意一条」（二值），`multi_hit` 判「捞到标准的百分之几」（比例）。

**源码事实：** 所有比较值先过滤 `("", None)` 再进集合——缺失与空字符串都不参与计算。

**源码事实：** `DocumentRecallEvaluator` 用 `_make_metadata_value_hashable` 把嵌套的 list/tuple/dict 转成带类型标签的可哈希结构，而 MRR/MAP 不用（它们需要顺序或就地移除，用 list 扫描）。

**源码事实：** `_make_metadata_value_hashable` 给三种容器加类型标签，所以 `[1,2]` 与 `(1,2)` 不相等；字典转 `frozenset` 所以键顺序不影响相等。

**源码事实：** MRR 在标准答案侧过滤 `("", None)`，在检索侧只过滤 `None` —— 空字符串仍占排名位。

**源码事实：** MAP 必须把命中的标准答案从池里 `remove`，否则重复命中会虚高分数。

**源码事实：** `top_k` 对 MAP 的语义是「标准答案总数仍作分母」，所以排在 k 之后的相关文档算漏掉。

**源码事实：** `top_k` 校验显式排除 `bool`，因为 `True` 会通过 `isinstance(int)` 并等于 1。

**源码事实：** `AnswerExactMatchEvaluator` 是位置对齐的严格字符串相等，而它的内联注释与类 docstring 都描述成「与任意一个标准答案匹配」——文档与实现不一致。

**源码事实：** 三个文档指标都先判空再判等长，顺序不可换（两个空列表长度相等，只判等长会漏到除零）。

**架构推论：** 三个文档指标用三种数据结构（集合、列表、可变池），不是风格不统一，而是**算法需求驱动**。Recall 只要交集大小 → 集合；MRR 要第一个命中的位置 → 顺序遍历；MAP 要「按序累积且每条只用一次」→ 可移除的池。把三者统一成集合会丢掉顺序，统一成列表会失去 O(1) 查找。认清「指标定义决定了必要的数据结构」是这一层的核心。

**架构推论：** 「去重」在这类指标里是隐藏的语义决定。Recall 与 MRR 去重之后，「检索器返回了 20 条但只有 5 条唯一」这个事实就消失了——分数按 5 条算。这对「衡量召回多样性」是合适的，但对「衡量检索器实际开销」就不合适。任何集合型指标都隐含一个去重假说，而这个假说很少被写进文档。

**架构推论：** 「判空」与「判等长」这两道检查的顺序，是这类「批量指标」的通用模式：**空输入会在后续所有除法处爆炸，所以它必须是第一道**。而「长度不等」的检查即使有 `zip(..., strict=True)` 兜底也该保留——因为它的错误消息能告诉用户「两个列表不一样长」，而 `strict` 抛的是 `ValueError: zip() argument 2 is shorter than argument 1`，信息量小得多。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/components/evaluators/document_recall.py` | `DocumentRecallEvaluator` | 两种口径的召回率 |
| `haystack/components/evaluators/document_recall.py` | `RecallMode` | `single_hit` / `multi_hit` 两值枚举 |
| `haystack/components/evaluators/document_recall.py` | `RecallMode.from_str` | 反序列化并把支持值列进错误消息 |
| `haystack/components/evaluators/document_recall.py` | `_get_comparison_value` | 三种比较字段，含 `meta.` 嵌套下钻 |
| `haystack/components/evaluators/document_recall.py` | `_unique_comparison_values` | 海象取值 + 过滤空值 + 去重 |
| `haystack/components/evaluators/document_recall.py` | `_comparison_sets` | 两侧各自去重并在此处告警 |
| `haystack/components/evaluators/document_recall.py` | `_recall_single_hit` | 交集非空即 1 |
| `haystack/components/evaluators/document_recall.py` | `_recall_multi_hit` | 交集大小除以标准数 |
| `haystack/components/evaluators/document_recall.py` | `DocumentRecallEvaluator.run` | 前置检查、按模式选函数、平均 |
| `haystack/components/evaluators/document_mrr.py` | `DocumentMRREvaluator` | 第一个命中的倒数排名 |
| `haystack/components/evaluators/document_mrr.py` | `DocumentMRREvaluator.run` | 两侧过滤不对称，空串仍占位 |
| `haystack/components/evaluators/document_map.py` | `DocumentMAPEvaluator` | 平均精度，需就地移除已计入项 |
| `haystack/components/evaluators/document_map.py` | `DocumentMAPEvaluator.run` | 用 list 而非 set 以支持不可哈希值 |
| `haystack/components/evaluators/answer_exact_match.py` | `AnswerExactMatchEvaluator` | 位置对齐的严格字符串比较 |
| `haystack/utils/misc.py` | `_make_metadata_value_hashable` | 嵌套容器转成带类型标签的可哈希结构 |

## 参考

- [haystack 源码：components/evaluators/document_recall.py](https://github.com/deepset-ai/haystack/blob/main/haystack/components/evaluators/document_recall.py)
- [haystack 文档：评测器](https://docs.haystack.deepset.ai/docs/evaluators)
- [Pinecone：离线评测的 MRR 与 MAP 算法](https://www.pinecone.io/learn/offline-evaluation/)

## 与主核 / AI 的关系

主核里最接近的是「查询质量的离线评估」：给一组已知答案的查询，跑一遍检索计划，算出召回率与平均精度，用来比较不同索引或不同统计信息的优劣。

差别在于**确定性**。主核的评估是「同一份数据同一份计划，结果完全一样」；这一页也是如此——所有四个指标都不调模型，所以**结果可复现**。这正是它们与本组 i01（LLM 评测器）的分水岭：那边必须用 `nan` 表示失败，因为模型输出不确定；这边不需要，因为任何失败都是真正的程序错误。

与 i01 的对照还有一个具体差别：**这一页的失败处理是「告警 + 计 0」，而 i01 是「`nan` + 排除」。** 差别在于「标准答案缺失」这件事的性质不同——在 i01 里「评测没跑成」与「评测结果是 0 分」必须区分；在这一页里「没有标准答案」本身就意味着「这条无法得分」，计 0 是一个明确的约定（且伴随警告）。所以**同一个数值问题，在不同可靠性假设下有不同的正确答案**。

与本组 i05（token 计数与评测结果聚合）的连接：`EvaluationRunResult` 消费的正是这一页与 i01 输出的 `individual_scores` ——所以那一页要求逐条分数**长度与输入行对齐、失败位用 `nan`**，正是为了让这一页与 i01 两种语义能汇到同一张表里。

对 AI 工程最实用的一条是「去重的隐藏假说」。所有基于集合的召回指标都会静默去重，而这会让「检索器返回的条数」与「参与计算的条数」不一致。若你在调检索参数（例如把 `top_k` 从 5 调到 20），而结果集里有大量重复，那 Recall 可能**完全不提升**——原因是唯一文档数没变。排查时先看唯一值数量。

另一条是 MAP 的「移除」操作。任何「每条标准答案只能被计入一次」的指标，都要求实现真的把它从可用池里删掉，而不是只做成员检查。用「检查存在」代替「移除」会让重复命中重复得分，而分数是偏高方向——这类 bug 很难被发现，因为结果看起来「还不错」。

最后一条是 `top_k` 对 MAP 的语义。@k 指标的通行定义是「把 k 之后的都当漏掉」，所以分母不变。若你按「只在前 k 条里重新算」的直觉理解它，就会高估分数。判据很清楚：**@k 指标的分母是「全部相关项」，而不是「前 k 条里的相关项」**。
