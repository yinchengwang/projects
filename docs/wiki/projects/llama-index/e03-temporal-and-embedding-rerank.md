---
project: llama-index
feature: temporal-and-embedding-rerank
title: 时序权重与向量模型重排
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/postprocessor/node_recency.py
    symbols: [TimeWeightedPostprocessor, FixedRecencyPostprocessor, EmbeddingRecencyPostprocessor]
  - path: llama-index-core/llama_index/core/postprocessor/sbert_rerank.py
    symbols: [SentenceTransformerRerank]
  - path: llama-index-core/llama_index/core/postprocessor/optimizer.py
    symbols: [SentenceEmbeddingOptimizer]
---

# 时序权重与向量模型重排

## 一句话职责

五种后处理器，分两组思路：**时序组**用「文档有多旧」参与排序（`FixedRecency` 只按日期、`EmbeddingRecency` 按日期去重、`TimeWeighted` 把新鲜度加成进分数），**模型组**用更强的模型重算相关度（`SentenceTransformerRerank` 用交叉编码器）——还有一个特殊的 `SentenceEmbeddingOptimizer`，它**不重排不裁剪，而是把节点内容改短**。

## 机制是什么

| 类 | 输入 → 输出 | 单节点被改了没有 |
|---|---|---|
| `FixedRecencyPostprocessor` | 按 `metadata[date_key]` 倒序，取前 `top_k` | 否，只选 |
| `EmbeddingRecencyPostprocessor` | 按日期倒序后**去重**：越新的留下、与之高度相似的旧的丢掉 | 否，只选 |
| `TimeWeightedPostprocessor` | 分数 + 时间衰减项，再排序取 `top_k` | 否（但会**写** `__last_accessed__`） |
| `SentenceTransformerRerank` | 交叉编码器重打分，覆盖原 `score` | 否（除非开 `keep_retrieval_score`） |
| `SentenceEmbeddingOptimizer` | **按句子级相似度筛句，改写节点内容** | **是，`set_content`** |

最后一行是关键：前四个都只操作「节点列表」，最后一个**改节点本身的内容**。这让它在链里的位置变得特殊——放在它之后的任何处理器看到的都不是原始文本。

## 怎么实现的

### 一、时序三件：日期是元数据里的一个键

**三者的共同前提是一个约定：`metadata[date_key]` 里放**能**被 `pandas.to_datetime` 解析的值。** 默认键名是 `"date"`。

**`FixedRecencyPostprocessor` 是三者中最简单的，而它的排序有一个容易误解的地方。**

```
node_dates = pd.to_datetime([node.node.metadata[self.date_key] for node in nodes])
sorted_node_idxs = np.flip(node_dates.argsort())
sorted_nodes = [nodes[idx] for idx in sorted_node_idxs]
return sorted_nodes[: self.top_k]
```

`argsort()` 给的是**升序的索引**（最旧在前），`np.flip` 把它反转成**降序**（最新在前）。于是 `top_k=1` 拿到的是最新的一条。

**这里用的是 `metadata[self.date_key]` 的直接索引而不是 `.get`。** 一条缺这个键的节点会抛 `KeyError`——而这个抛错发生在**列表推导式的中途**，也就是一条坏节点会让整批处理失败。

**`EmbeddingRecencyPostprocessor` 的逻辑是「去重」而不是「排序取前 k」——它的目标不是「给我最新的几条」，而是「删掉那些被更新的文档取代了的旧文档」。**

三步：

1. **按日期降序**（与 `FixedRecency` 相同）。
2. **对每个节点算「查询嵌入」**，而那个嵌入的输入是**一个模板包好的节点内容**：

```
query_text = self.query_embedding_tmpl.format(context_str=node.node.get_content(metadata_mode=MetadataMode.EMBED))
query_embedding = self.embed_model.get_query_embedding(query_text)
```

模板 `DEFAULT_QUERY_EMBEDDING_TMPL` 的内容说明意图：

> 给定当前文档，我们希望找到包含**相似上下文**的文档。注意这些文档**比当前文档更旧**，意味着某些细节可能已改变。然而高层上下文应当相似。

也就是说这一步在做「**按语义找同一主题的新旧版本**」。模板里那句「比当前文档更旧」是给嵌入模型的提示——让它倾向于匹配「同一话题的另一篇」而不是「字面相近的另一篇」。

3. **双向遍历，把「更新的那个已经占了位置」的旧的标为跳过**：

```
for idx, node in enumerate(sorted_nodes):
    if node.node.node_id in node_ids_to_skip: continue
    query_embedding = ...（按当前节点算）
    for idx2 in range(idx + 1, len(sorted_nodes)):
        if sorted_nodes[idx2].node.node_id in node_ids_to_skip: continue
        if np.dot(query_embedding, text_embeddings[idx2]) > self.similarity_cutoff:
            node_ids_to_skip.add(node2.node.node_id)
```

**内层只从 `idx + 1` 开始**——也就是只比较「比当前更旧的」那些。这个方向性来自「新文档不会被旧文档取代」这个前提：一个旧的若与更新的相似，旧的就该让位。

4. 返回所有未被跳过的节点，**保持日期降序**。

**这个处理器有两个隐含的代价**：每个节点都要额外算一次「查询嵌入」（`get_query_embedding`），而所有节点的「文本嵌入」是批量算的（`get_text_embedding_batch`）。所以一次调用是 **1 次批量 + N 次单独**的嵌入请求——**N 次单独请求是这一页最贵的部分**，而它与「去重」的收益需要权衡。

**`TimeWeightedPostprocessor` 是三者中唯一「改分数」的，而它的公式值得写清。**

```
last_accessed = node.metadata.get(self.last_accessed_key, None)
if last_accessed is None: last_accessed = now
hours_passed = (now - last_accessed) / 3600
time_similarity = (1 - self.time_decay) ** hours_passed
similarity = score + time_similarity
```

四个要点：

1. **衰减基于「最后访问时间」而不是「创建时间」**。键名默认 `__last_accessed__`，双下划线是为了标识「这是框架写的，不是用户的业务元数据」。
2. **没访问记录时当作「刚访问过」**（`last_accessed = now`），于是 `hours_passed = 0`、`time_similarity = 1`。也就是说**新节点得到最大的新鲜度加成**。
3. **公式是 `(1 - time_decay) ** hours_passed`，`time_decay` 默认 0.99 → 底数为 0.01。** 这是一个**指数衰减**：0 小时得 1.0、1 小时得 0.01、2 小时得 0.0001。**也就是说衰减极快——一小时之后时间加成几乎消失。**

   这一点很值得记：`time_decay=0.99` 看上去像「每秒衰减 1%」（也就是极慢），但代入公式后底数 0.01 意味着**每小时衰减到百分之一**。参数名与它的实际效果之间**没有直观对应**。
4. **时间加成是「加」到原分数上的，而两者的量纲完全不同。** `score` 是检索相似度（通常在 0–1 或 0–10），`time_similarity` 是 0–1 的衰减项。**直接相加意味着两者的权重隐含是 1:1**，而没有任何参数可以调这个比例。

**`time_access_refresh` 默认 `True`，于是这个处理器有副作用：它会写回 `metadata[__last_accessed__] = now`。**

```
if self.time_access_refresh:
    for node_with_score in result_nodes:
        node_with_score.node.metadata[self.last_accessed_key] = now
```

也就是说**一次查询会改变后续查询的结果**——这是这个处理器最反直觉的性质。`top_k=1` 时尤其明显：被返回的那条会被标记为「刚访问」，于是下一次它几乎必定还排第一（加成 1.0）。

这是一个**自我强化**的机制，而它是有意设计的（`time_access_refresh` 的语义就是「访问就刷新」）。要拿到稳定的结果必须显式关掉它，或提供 `now` 以固定时间基准（字段注释写着 `makes it easier to test`）。

**三个时序处理器共用 `pandas` 依赖，且都在函数内导入并在失败时抛带 pip 命令的 `ImportError`。** 用 `pandas` 而不是标准库是为了 `to_datetime` 的多格式解析能力——`metadata["date"]` 可能是 `"2024-01-01"`、可能是 ISO 时间戳、也可能是 Unix 数字，`pd.to_datetime` 全都能处理。

### 二、`SentenceTransformerRerank`：交叉编码器，一次前向

**它与 LLM 重排的根本差别是：交叉编码器把「查询 + 文档」**拼在一起**输入一个模型，得到单个相关度分数。** 而向量检索是「双塔」——查询与文档各自编码、再算余弦。交叉编码器更准（因为它能看到两者的交互），但**无法预计算**：每条候选都要一次前向。

**构造时的默认值有三处值得注意。**

```
def __init__(self, top_n: int = 2, model: str = "cross-encoder/stsb-distilroberta-base", device: Optional[str] = None, keep_retrieval_score: bool = False, trust_remote_code: bool = True):
```

1. **`top_n` 默认 2**——比重排类常见的 5/10 小得多。这是因为交叉编码器最贵，默认取最少。
2. **`device=None` 时 `infer_torch_device()`**——自动选 GPU/CPU。
3. **`trust_remote_code` 的默认值在两个地方不一致**：`__init__` 的参数默认 `True`，而 pydantic 字段默认 `False`，且 `__init__` 调用 `super().__init__` 时**没有传 `trust_remote_code`**。于是**字段值永远是 `False`，而传给 `CrossEncoder` 的是参数值 `True`**——两者脱钩。这是一个真实的 bug 级不一致：字段上写着 `False`，实际模型加载用的是 `True`。

**`keep_retrieval_score` 是一个「保信息」开关。**

```
for node, score in zip(nodes, scores):
    if self.keep_retrieval_score:
        node.node.metadata["retrieval_score"] = node.score
    node.score = score
```

开的时候，原来的检索器分数被**搬进元数据**（键 `retrieval_score`）再被覆盖。关的时候，原分数**直接丢失**。

这个设计是对的——重排分数与原分数不同量纲，若要比较或调参就得两者都留。但注意它写的是 `node.node.metadata`，也就是**真的修改了节点**：一次重排之后节点的元数据多了一个键，这个改变会随节点被持久化。

**它用一个 callback 事件包住了实际推理。**

```
with self.callback_manager.event(CBEventType.RERANKING, payload={...}) as event:
    scores = self._model.predict(query_and_nodes)
    assert len(scores) == len(nodes)
    ...
    event.on_end(payload={EventPayload.NODES: new_nodes})
```

`on_end` 的 payload **只带节点列表、不带分数**——与 `start` 的 payload（带 `MODEL_NAME`、`QUERY_STR`、`TOP_K`）不对称。也就是说从事件流里能看到「重排了哪些节点」，看不到「重排后的分数」。

**那个 `assert len(scores) == len(nodes)` 是一个「不该发生」的断言**，而不是异常。`predict` 对每个输入都产出分数，长度不匹配说明模型实现有问题。用 `assert` 意味着它在 `python -O` 下会被移除——这不是一个用于生产的检查。

**排序表达式 `key=lambda x: -x.score if x.score else 0` 有一个优先级陷阱。**

Python 的条件表达式优先级低于一元负号，所以实际是 `(-x.score) if x.score else 0`。`x.score` 为 `None` 或 `0` 时取 0，否则取负分（升序排即降序结果）。逻辑上是对的——**但 `score == 0` 与 `score is None` 被合并成同一种情况**，而 `0` 是一个合法的相关度值（比如交叉编码器输出的 0）。

### 三、`SentenceEmbeddingOptimizer`：不改顺序，改内容

**它做的是「按句子级相似度筛句」：把节点内容拆成句子，只保留与查询最相关的那几句（加上前后文），再把节点内容替换掉。**

**筛选有两个并行可用的旋钮。**

```
if self.percentile_cutoff is not None: num_top_k = int(len(split_text) * self.percentile_cutoff)
if self.threshold_cutoff is not None: threshold = self.threshold_cutoff
```

- `percentile_cutoff=0.5` → 保留前 50% 的句子（按相似度）。
- `threshold_cutoff=0.7` → 只保留相似度 > 0.7 的句子。
- **两者可以同时用**（文档里写明），此时 `get_top_k_embeddings` 同时受「取前 k」与「阈值」约束。

**它的构造里有一个值得注意的默认行为：`Settings.embed_model` 为 `None` 时自动去 import OpenAI 嵌入。**

```
self._embed_model = embed_model or Settings.embed_model
if self._embed_model is None:
    try:
        from llama_index.embeddings.openai import OpenAIEmbedding
        self._embed_model = OpenAIEmbedding()
    except ImportError: raise ImportError("`llama-index-embeddings-openai` package not found, ...")
```

这是一个**隐式依赖 + 隐式默认**的组合：没配嵌入模型就默认用 OpenAI 的（需要有 API key）。这与另外几个处理器「拿 `Settings.llm` 但不自己造」的做法不同——这里会在缺失时尝试创建。

**查询嵌入是懒算的，且算一次就缓存进 `query_bundle`。**

```
if query_bundle.embedding is None:
    query_bundle.embedding = self._embed_model.get_agg_embedding_from_queries(query_bundle.embedding_strs)
```

`get_agg_embedding_from_queries`（复数 queries）意味着**多个查询串会被聚合成一个向量**——这是为了支持「改写后的多查询」场景。

注意它**修改了 `query_bundle` 对象**（把 `embedding` 写进去）。于是同一个 `query_bundle` 后续被别的处理器用时，嵌入已经算好、不必重算。这是一个**跨处理器共享的缓存**，代价是它让 `query_bundle` 变成了可变状态。

**句子筛选之后是「加前后文」的扩展。**

```
rangeMin, rangeMax = 0, len(split_text)
if self.context_before is None: self.context_before = 1
if self.context_after is None: self.context_after = 1

top_sentences = [" ".join(split_text[max(idx - self.context_before, rangeMin): min(idx + self.context_after + 1, rangeMax)]) for idx in top_idxs]
```

`context_before` / `context_after` 默认都是 1（在被选中句子的前后各取 1 句）。而 `max(..., rangeMin)` 与 `min(..., rangeMax)` 是**边界裁剪**——第一句与最后一句不会越界。

**这里有一个副作用：它在 `_postprocess_nodes` 里给 `self` 的字段赋默认值**（`if self.context_before is None: self.context_before = 1`）。也就是说**处理一批节点会永久改变这个处理器实例的配置**。功能上无害（赋的是同一个默认值），但让实例在调用前后不再相同。

**边界处理是「全杀就报错」。**

```
if len(top_idxs) == 0: raise ValueError("Optimizer returned zero sentences.")
```

这个错误是对的：一个阈值定得太高、或 `percentile_cutoff` 算出来是 0，会让某个节点一句都不剩。此时若静默返回空内容，下游会拿到一个空节点而不知原因；**抛错至少能让调用方看见问题**。

**最后一步是唯一一个改写节点内容的动作。**

```
nodes[node_idx].node.set_content(" ".join(top_sentences))
```

`set_content` 是**原地改写**。这意味着：

- 原来的节点文本**永久丢失**（除非别处还留着）；
- 若同一个节点被多个查询命中，第二次查询看到的是被改短后的内容——**这是一个不可逆的信息损失**。

**这一步的收益是实在的：** 一个 5000 token 的节点，只有 3 句相关，把它缩到 300 token **能让同样的上下文预算装进更多节点**。这是「用句子级精度换取上下文密度」的交换。

代价是它**破坏了 `a02` 那页强调的「块与原文的偏移对应」**——`start_index` 之类的位置信息在 `set_content` 之后彻底失效。

## 关键取舍

**源码事实：** 三个时序处理器的共同前提是 `metadata[date_key]`（默认 `"date"`）里放能被 `pandas.to_datetime` 解析的值，也因此都在函数内导入 pandas。

**源码事实：** `FixedRecencyPostprocessor` 用 `metadata[self.date_key]` 直接索引，一条缺该键的节点会让整批 `KeyError`。

**源码事实：** `EmbeddingRecencyPostprocessor` 的模板刻意为「更旧的文档」措辞（「细节可能改变、高层上下文应相似」），目的是让嵌入偏向「同话题的另一版本」而非「字面相近」；内层只比较更旧的节点（`idx + 1` 起）。

**源码事实：** 它一次调用是「1 次批量文本嵌入 + N 次单独查询嵌入」，N 次单独请求是主要成本。

**源码事实：** `TimeWeightedPostprocessor` 的衰减是 `(1 - time_decay) ** hours_passed`，`time_decay=0.99` 意味着底数 0.01、**每小时衰减到百分之一**——参数名与实际速率没有直观对应。

**源码事实：** 时间加成与检索分数**直接相加**且无权重参数，隐含 1:1；无访问记录时按「刚访问」处理从而得最大加成。

**源码事实：** `time_access_refresh` 默认 `True` 会写回 `__last_accessed__`，于是**一次查询改变后续查询的排序**（自我强化）。

**源码事实：** `SentenceTransformerRerank` 的 `trust_remote_code` 在 `__init__` 默认 `True` 而 pydantic 字段默认 `False`，且 `super().__init__` 未传该值——字段与传给模型的值脱钩。

**源码事实：** 它的 `keep_retrieval_score` 会把原分数搬进 `node.node.metadata["retrieval_score"]`，因此**改动了节点**；`on_end` 事件的 payload 只带节点不带分数，与 `start` 不对称。

**源码事实：** 排序键 `-x.score if x.score else 0` 把 `score == 0` 与 `score is None` 归为同一类。

**源码事实：** `SentenceEmbeddingOptimizer` 在 `Settings.embed_model` 缺失时**自动构造 OpenAI 嵌入**（隐式默认），会在 `query_bundle.embedding` 上缓存聚合嵌入从而让 `query_bundle` 变成可变状态。

**源码事实：** 它用 `set_content` 原地改写节点内容，原文本永久丢失；同时在 `_postprocess_nodes` 里给 `self.context_before` / `context_after` 赋默认值，改变实例状态。

**源码事实：** 一句都没选上时抛 `ValueError("Optimizer returned zero sentences.")` 而非静默返回空内容。

**架构推论：** 这五个处理器构成了一条清晰的成本阶梯：`FixedRecency`（零模型调用）→ `EmbeddingRecency`（N+1 次嵌入）→ `TimeWeighted`（零调用但有副作用）→ `SentenceTransformerRerank`（N 次交叉编码器前向）→ `SentenceEmbeddingOptimizer`（N 段嵌入 + 改写内容）。

而它们各自「改什么」的差别比成本差别更重要：**只改顺序的、改分数的、改内容的**——三类改动对下游的影响完全不同。前两类可以串联（因为都只动元信息），第三类必须慎重（它把原文本不可逆地换掉了）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/postprocessor/node_recency.py` | `FixedRecencyPostprocessor` | 按 `metadata["date"]` 倒序取前 `top_k` |
| `llama-index-core/llama_index/core/postprocessor/node_recency.py` | `EmbeddingRecencyPostprocessor` | 按日期倒序 + 语义去重（更新的取代相似的更旧的） |
| `llama-index-core/llama_index/core/postprocessor/node_recency.py` | `DEFAULT_QUERY_EMBEDDING_TMPL` | 为「同话题旧版本」措辞的嵌入查询模板 |
| `llama-index-core/llama_index/core/postprocessor/node_recency.py` | `TimeWeightedPostprocessor` | 分数 + `(1-time_decay)^hours` 的新鲜度加成 |
| `llama-index-core/llama_index/core/postprocessor/node_recency.py` | `__last_accessed__` | 框架私有的「最后访问时间」元数据键，会被写回 |
| `llama-index-core/llama_index/core/postprocessor/sbert_rerank.py` | `SentenceTransformerRerank` | 交叉编码器重打分，覆盖 `node.score` |
| `llama-index-core/llama_index/core/postprocessor/sbert_rerank.py` | `DEFAULT_SENTENCE_TRANSFORMER_MAX_LENGTH` | 交叉编码器输入上限 512 |
| `llama-index-core/llama_index/core/postprocessor/sbert_rerank.py` | `keep_retrieval_score` | 把原分数搬到 `metadata["retrieval_score"]` |
| `llama-index-core/llama_index/core/postprocessor/optimizer.py` | `SentenceEmbeddingOptimizer` | 句子级筛选并 `set_content` 改写节点 |
| `llama-index-core/llama_index/core/postprocessor/optimizer.py` | `percentile_cutoff` / `threshold_cutoff` | 两个可并用的筛句旋钮 |
| `llama-index-core/llama_index/core/postprocessor/optimizer.py` | `context_before` / `context_after` | 选中句的邻句扩展，边界裁剪 |

## 参考

- [llama-index 源码：postprocessor/node_recency.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/postprocessor/node_recency.py)
- [llama-index 源码：postprocessor/sbert_rerank.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/postprocessor/sbert_rerank.py)
- [sentence-transformers 文档：CrossEncoder](https://sbert.net/docs/cross_encoder/usage/usage.html)
- [llama-index 文档：节点后处理器](https://docs.llamaindex.ai/en/stable/module_guides/querying/node_postprocessors/)

## 与主核 / AI 的关系

「用时间来加权」与主核里的 **TTL（生存时间）** 是同一类设计：两者都让「时间」参与数据可见性的判断，而差别在于 TTL 是硬性的（过期即不可见）而这里是软性的（越旧分越低）。

而 `TimeWeightedPostprocessor` 那个「访问即刷新」的副作用，与主核里 **LRU 缓存的热度统计**完全对应：读操作改变后续的排序结果。数据库对此的处理通常是「把统计与排序分开」（统计归统计、`ORDER BY` 归 `ORDER BY`），而这里直接把它们混在了一个处理器的返回里。

对 agent 工程的直接价值有三条。其一，**「改顺序 / 改分数 / 改内容」是三种强度递增的改动，应当分开考虑**。前两者可逆（原分数可存进元数据、原顺序可记录），第三个不可逆——`set_content` 之后原文就没了，任何依赖原文的功能都会失效。其二，**参数名与数学效果要能对上**。`time_decay=0.99` 看起来是「衰减很慢」，实际是每小时衰减到 1%——这类名实不符是最容易让人调错的一项。其三，**默认值里的显式副作用必须可选**。`time_access_refresh` 默认开意味着「默认行为会改变后续查询的结果」，而一个只在读的处理器改动状态，会让整条流水线难以复现。
