---
project: llama-index
feature: index-retrievers
title: 各类索引的检索实现
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/list/retrievers.py
    symbols: [SummaryIndexRetriever, SummaryIndexEmbeddingRetriever]
  - path: llama-index-core/llama_index/core/indices/keyword_table/retrievers.py
    symbols: [KeywordTableGPTRetriever, KeywordTableRAKERetriever]
  - path: llama-index-core/llama_index/core/indices/document_summary/retrievers.py
    symbols: [DocumentSummaryIndexLLMRetriever, DocumentSummaryIndexEmbeddingRetriever]
---

# 各类索引的检索实现

## 一句话职责

同一套索引结构可以配多种检索器，而检索器的选择决定**代价、精度与是否需要模型**。这六个实现覆盖了三类索引 × 两三种模式，它们的差别集中在一处：**用什么当「相关性」的判据**。

## 机制是什么

所有检索器都继承 `BaseRetriever`（d01），所以它们的公共骨架是「归一化输入 → `_retrieve` → 递归展开 → 去重」。本篇关心的是 `_retrieve` 里的分歧，而分歧来自一个根本问题：

**相关性由什么判定？**

| 判据 | 实现 | 一次检索的模型调用 |
|---|---|---|
| 不判定（全给） | `SummaryIndexRetriever` | 0 |
| 嵌入相似度 | `SummaryIndexEmbeddingRetriever`、`DocumentSummaryIndexEmbeddingRetriever` | 0（但要嵌入） |
| LLM 打分 | `SummaryIndexLLMRetriever`、`DocumentSummaryIndexLLMRetriever`、`KeywordTableGPTRetriever` | 每批 1 次 |
| 关键词命中数 | `BaseKeywordTableRetriever`（共同骨架） | 0 |
| RAKE 短语 | `KeywordTableRAKERetriever` | 0（但要 nltk） |

于是「换一个检索器」的语义后果远大于换一个参数：**它可能从零成本变成每批一次 LLM 调用，也可能从需要嵌入模型变成需要 nltk**。

## 怎么实现的

**`SummaryIndexRetriever` 是最诚实的实现：它把 `query_bundle` 直接 `del` 掉。**

```python
def _retrieve(self, query_bundle: QueryBundle) -> List[NodeWithScore]:
    """Retrieve nodes."""
    del query_bundle

    node_ids = self._index.index_struct.nodes
    nodes = self._index.docstore.get_nodes(node_ids)
    return [NodeWithScore(node=node) for node in nodes]
```

三处：

1. **`del query_bundle`** 是一个显式的「我不用它」，同时让 linter 不报未使用参数。这是一个比 `pass` 或注释更硬的表态——它说明这个检索器**与查询内容无关**。
2. **直接把 `index_struct.nodes`（全部 id）读完**。所以「检索」在这里退化成「读取整个索引」。对 `SummaryIndex` 来说这没问题（它的索引就是全部节点），但对大索引就意味着把全库喂给下游。
3. **不设分数**。所以返回的列表里所有 `score` 都是 `None`。下游（如 `RetrieverQueryEngine` 的 `response_mode="compact"`）会按顺序全部处理——这正是这个检索器的用途：配合「遍历全部内容再合成」的响应模式。

**`SummaryIndexEmbeddingRetriever` 的嵌入是惰性算的，而它会把结果写回节点。**

```python
def _get_embeddings(self, query_bundle: QueryBundle, nodes: List[BaseNode]) -> Tuple[List[float], List[List[float]]]:
    if query_bundle.embedding is None:
        query_bundle.embedding = self._embed_model.get_agg_embedding_from_queries(query_bundle.embedding_strs)

    node_embeddings: List[List[float]] = []
    nodes_embedded = 0
    for node in nodes:
        if node.embedding is None:
            nodes_embedded += 1
            node.embedding = self._embed_model.get_text_embedding(
                node.get_content(metadata_mode=MetadataMode.EMBED)
            )
        node_embeddings.append(node.embedding)
    return query_bundle.embedding, node_embeddings
```

四处：

1. **`query_bundle.embedding` 被就地写回**。所以同一次调用里若有多处用到 `query_bundle`，嵌入只算一次。但这意味着**这个检索器修改了调用方传进来的对象**——一个 `QueryBundle` 被 `retrieve` 之后再传给别的检索器时，嵌入已经在里面了。

2. **`node.embedding = ...` 也是就地写回节点**。所以第一次检索会把全部节点的嵌入**写进 docstore 里那些节点对象**（可能进而被持久化）。这实际上是一种**隐式的索引构建**：无需向量库也能有嵌入。

   代价是：`nodes_embedded` 计数器被算了但**从未使用**（没有日志、没有返回值）。所以这段代码本想报告「这次算了多少个」，但那个信息丢了。

3. **`get_content(metadata_mode=MetadataMode.EMBED)`** —— 与 c02 的向量索引用同一模式，保证「送入嵌入的文本」一致。

4. **对每个节点一次 `get_text_embedding`（单条接口）而不是批量接口**。所以嵌入是 N 次调用（可能是 N 次网络往返）。这是这个检索器最重要的成本特征：**没有批量**。

`_retrieve` 随后用 `get_top_k_embeddings(..., embedding_ids=list(range(len(nodes)))` —— 注意这里的 id 是**数组下标**而不是 node_id。所以 `top_idxs` 直接用 `nodes[i]` 取。这个选择是合理的（下标只在这一层有效），但让「命中结果」与「节点身份」在这一步解耦。

而 `logger.debug(f"> Top {len(top_idxs)} nodes:\n")` 与下一行用 `nl.join(...)` 拼出全部命中节点的内容——**只在 DEBUG 级别**。所以生产环境看不到，而打开 DEBUG 会把整段内容打进日志。这是「调试日志包含了内容本身」的常见问题（可能泄漏数据）。

**`SummaryIndexLLMRetriever` 是「分批让 LLM 选」的第一个实例。**

```python
for idx in range(0, len(node_ids), self._choice_batch_size):
    node_ids_batch = node_ids[idx : idx + self._choice_batch_size]
    nodes_batch = self._index.docstore.get_nodes(node_ids_batch)

    query_str = query_bundle.query_str
    fmt_batch_str = self._format_node_batch_fn(nodes_batch)
    raw_response = self._llm.predict(self._choice_select_prompt, context_str=fmt_batch_str, query_str=query_str)

    raw_choices, relevances = self._parse_choice_select_answer_fn(raw_response, len(nodes_batch))
    choice_idxs = [int(choice) - 1 for choice in raw_choices]
    choice_node_ids = [node_ids_batch[idx] for idx in choice_idxs]

    choice_nodes = self._index.docstore.get_nodes(choice_node_ids)
    relevances = relevances or [1.0 for _ in choice_nodes]
    results.extend([NodeWithScore(node=node, score=relevance) for node, relevance in zip(choice_nodes, relevances)])
```

六处：

1. **`choice_batch_size` 默认 10**。所以每个 LLM 调用要看 10 条候选。这个数决定了「上下文里能放多少候选」与「要调多少次」的乘积——两个目标相反。

2. **`format_node_batch_fn` 与 `parse_choice_select_answer_fn` 都可注入**。这是必要的：不同模型的提示格式与输出格式不同。

3. **`choice_idxs = [int(choice) - 1 for choice in raw_choices]`** —— 有 `int()`，说明 `raw_choices` 可能是字符串。而 **`DocumentSummaryIndexLLMRetriever` 里同一处是 `choice - 1`（没有 `int()`）**。两个检索器假定不同的返回类型，但都调用**同一个默认解析函数**（`default_parse_choice_select_answer_fn`）。这意味着其中一处的 `int()` 是多余的，或者两处的默认解析器不同。源码没说明。

4. **`node_ids_batch[idx]` 无边界检查**。LLM 若返回 `11`（批次只有 10 条），`IndexError`。这是 d01 里已指出的问题在本文件的具体形态。

5. **`relevances = relevances or [1.0 for _ in choice_nodes]`** —— 解析函数没给分数时兜底成全 1.0。注意这里用的又是 `or`：**空列表会被兜底**（正确，空列表意味着没给），但这也意味着「给了一个空列表以表示全部不相关」会被解读成「全部 1.0」。

6. **`zip(choice_nodes, relevances)`** —— 若 `relevances` 长度不等于 `choice_nodes`，会截断。所以「解析器给了分数但数量不对」的结果是**部分节点没有分数地消失**。

注意**每批都独立返回结果，最后全部合并**（`results.extend`）。所以这个检索器不做「全局排序取 top k」——`choice_batch_size=10` 且总共 25 条候选时，它会返回**最多 30 条**（每批至多 10 条）。而 `SummaryIndexEmbeddingRetriever` 会取全局 top k（默认 1）。

**这是两个检索器在语义上的关键差别**：嵌入版给「最相关的一条」，LLM 版给「每一批里所有被认为相关的」。所以 LLM 版的输出规模取决于批次数，不受 `similarity_top_k` 控制。

**`BaseKeywordTableRetriever` 的骨架是所有关键词检索器共享的，而它的排序是「命中计数」。**

```python
chunk_indices_count: Dict[str, int] = defaultdict(int)
keywords = [k for k in keywords if k in self._index_struct.keywords]
for k in keywords:
    for node_id in self._index_struct.table[k]:
        chunk_indices_count[node_id] += 1
sorted_chunk_indices = sorted(chunk_indices_count.keys(), key=lambda x: chunk_indices_count[x], reverse=True)
sorted_chunk_indices = sorted_chunk_indices[: self.num_chunks_per_query]
sorted_nodes = self._docstore.get_nodes(sorted_chunk_indices)
return [NodeWithScore(node=node) for node in sorted_nodes]
```

五处：

1. **计数而非打分**。`chunk_indices_count[node_id]` 是「这个节点命中了几个查询关键词」的整数。所以相关性被量化成**离散的命中数**——没有归一化，也没有权重（一个罕见关键词与一个常见关键词贡献相同）。

2. **`sorted(..., key=lambda x: chunk_indices_count[x], reverse=True)` 没有次序键**。所以命中数相同的节点顺序**不可复现**——它取决于 `chunk_indices_count` 的插入顺序，而后者取决于关键词的遍历顺序，而关键词来自 `list(keywords)`（集合转列表，顺序不确定）。

   这是一个真实的稳定性问题：同一个查询两次检索可能返回**不同的顺序**。对「取前 10 条」这种截断来说，顺序不同意味着**入选的节点不同**。

3. **`keywords = [k for k in keywords if k in self._index_struct.keywords]`** 过滤未知关键词。静默丢弃——调用方不知道有多少查询关键词没在表里出现。

4. **`sorted_nodes = self._docstore.get_nodes(sorted_chunk_indices)`** —— 依赖 `get_nodes` **按输入顺序返回**。这与 c03 里 `SummaryIndex._delete_node` 的假设相同。若某个 docstore 实现不保序，这里的排序就白做了。

5. **返回不设分数**。所以「命中关键词数」这个唯一的排序信息在结果里**丢失**——调用方只能看到顺序，无法做阈值过滤，也无法知道「第 5 条与第 6 条差别多大」。

**三个子类只覆写 `_get_keywords`，而它们的代价差别极大。**

```python
class KeywordTableGPTRetriever(BaseKeywordTableRetriever):
    def _get_keywords(self, query_str: str) -> List[str]:
        response = self._llm.predict(self.query_keyword_extract_template, max_keywords=self.max_keywords_per_query, question=query_str)
        keywords = extract_keywords_given_response(response, start_token="KEYWORDS:")
        return list(keywords)
```

```python
class KeywordTableSimpleRetriever(BaseKeywordTableRetriever):
    def _get_keywords(self, query_str: str) -> List[str]:
        return list(simple_extract_keywords(query_str, max_keywords=self.max_keywords_per_query))
```

```python
class KeywordTableRAKERetriever(BaseKeywordTableRetriever):
    def _get_keywords(self, query_str: str) -> List[str]:
        return list(rake_extract_keywords(query_str, max_keywords=self.max_keywords_per_query))
```

三处：

1. **GPT 版用 `query_keyword_extract_template`（`DQKET`），而不是 `keyword_extract_template`**。`BaseKeywordTableIndex` 构造时 partial 的是后者（c04 讲过），而检索器用的是前者。所以**构建期与查询期用了两套提示**：

   `DQKET` 的模板文本（源码注释说「query 的抽词可以和建索引时一样，但这里调优了」）：

```
"A question is provided below. Given the question, extract up to {max_keywords} "
"keywords from the text. Focus on extracting the keywords that we can use "
"to best lookup answers to the question. Avoid stopwords.\n"
"---------------------\n"
"{question}\n"
"---------------------\n"
"Provide keywords in the following comma-separated format: 'KEYWORDS: <keywords>'\n"
```

   注意 `max_keywords` 在提示里是**模板变量**，且由检索器每次调用时传（`max_keywords=self.max_keywords_per_query`）。所以检索器不 partial——与索引侧的「构造时 partial」是两种风格。

2. **Simple 版用 `simple_extract_keywords`，它是「词频 top N」**：

```python
def simple_extract_keywords(text_chunk: str, max_keywords: Optional[int] = None, filter_stopwords: bool = True) -> Set[str]:
    tokens = [t.strip().lower() for t in re.findall(r"\w+", text_chunk)]
    if filter_stopwords:
        tokens = [t for t in tokens if t not in globals_helper.stopwords]
    token_counts = Counter(tokens)
    keywords = [keyword for keyword, count in token_counts.most_common(max_keywords)]
    return set(keywords)
```

   即「正则取词 → 小写 → 去停用词 → 频率最高的 N 个」。**它完全不看语义**——一个问句里出现两次的词会被优先选。这对「查询」这种短文本效果可疑（短句里很少重复词），但对长文本（建索引时）是合理的。

   注意**返回值是 `Set[str]`，而调用方又 `list(...)` 转回列表**。所以集合的无序性被带到了后续排序的不可复现性上（见上）。

3. **RAKE 版依赖 nltk 与 rake_nltk**，且两个 import 都带友善的错误消息：

```python
try:
    import nltk
except ImportError:
    raise ImportError("Please install nltk: `pip install nltk`")
try:
    from rake_nltk import Rake
except ImportError:
    raise ImportError("Please install rake_nltk: `pip install rake_nltk`")
```

   所以选 `retriever_mode="rake"` 是一个**可能有额外依赖**的选择——而错误只在运行时暴露（没有在包元数据里声明）。

   它还接收 `expand_with_subtokens=True` 默认，即把短语拆成子词。所以 RAKE 的输出比 GPT 版**更宽**（一个短语贡献多个关键词）。

**`DocumentSummaryIndexLLMRetriever` 与列表版的差别不止「选的是摘要」。**

| | `SummaryIndexLLMRetriever` | `DocumentSummaryIndexLLMRetriever` |
|---|---|---|
| 候选单元 | 节点 | **摘要节点** |
| 每批选择后 | 直接加入结果 | 累积 id 与分数 |
| 是否全局排序 | **否**（每批独立） | **是**（`sorted_list[:choice_top_k]`） |
| 是否展开 | 否（就是节点） | **是**（`summary_id_to_node_ids` → 该文档全部节点） |
| `int()` 转换 | 有 | **无** |

所以「候选单元的粒度」决定了后续两步：因为摘要不是最终结果（用户要的是正文），所以必须**再展开一次**；而因为要展开，就必须**先全局选出最好的几个摘要**（否则会展开出大量文档的正文）。

展开这一步：

```python
for summary_id, relevance in top_k_list:
    node_ids = self._index.index_struct.summary_id_to_node_ids[summary_id]
    nodes = self._index.docstore.get_nodes(node_ids)
    results.extend([NodeWithScore(node=n, score=relevance) for n in nodes])
```

三处：

1. **`summary_id_to_node_ids[summary_id]` 用方括号**——摘要不存在则 `KeyError`。理论上不会（id 来自 `summary_ids`），但若索引结构不一致就会。

2. **`choice_top_k` 默认 1**。所以默认只取一份文档——但展开后返回的节点数是**那份文档的全部块数**（可能是几十条）。所以「top k = 1」与「返回 1 条节点」完全不是一回事。

   这是这套设计里最容易误判的一处：**k 的语义在摘要层与节点层不同**。

3. **同一文档的所有节点共享同一个分数**。所以节点级排序信息丢失。

**`DocumentSummaryIndexEmbeddingRetriever` 是唯一一个「明确选择丢弃分数」的实现。**

```python
query = VectorStoreQuery(query_embedding=query_bundle.embedding, similarity_top_k=self._similarity_top_k)
query_result = self._vector_store.query(query)

top_k_summary_ids: List[str]
if query_result.ids is not None:
    top_k_summary_ids = query_result.ids
elif query_result.nodes is not None:
    top_k_summary_ids = [n.node_id for n in query_result.nodes]
else:
    raise ValueError("Vector store query result should return at least one of nodes or ids.")

results = []
for summary_id in top_k_summary_ids:
    node_ids = self._index_struct.summary_id_to_node_ids[summary_id]
    nodes = self._docstore.get_nodes(node_ids)
    results.extend([NodeWithScore(node=n) for n in nodes])
```

四处：

1. **`query_result.ids` 优先**。所以向量库若返回了节点对象，它也会被降级成 id——因为后续只需要 id。这是一个「只要 id 就够」的简化。

2. **`query_result.similarities` 完全没有被读**。所以向量库算出的相似度被丢弃。`NodeWithScore(node=n)` 不带分数。

   这对下游的影响是：`similarity_cutoff` 之类的参数**失效**（没有分数可过滤），且「按相似度排序」这个唯一的信息只能靠 `results` 的顺序推断——但在展开之后那个顺序也被打乱了（一份文档的多条节点被连续排列）。

3. **`if self._vector_store.is_embedding_query`** —— 只有这类向量库才计算查询嵌入。对「不用嵌入的向量查询」（例如某些做关键词混合的后端），`query_embedding=None` 被传给后端。

4. **`raise ValueError("... should return at least one of nodes or ids.")`** —— 这是本文件里唯一一处显式的契约校验。它也说明「向量库必须返回 id 或节点」是这个检索器对后端的硬要求。

**这六个实现里「是否设置分数」的分布，揭示了一个系统性的不一致。**

| 检索器 | 设置的分数 | 来源 |
|---|---|---|
| `SummaryIndexRetriever` | `None` | — |
| `SummaryIndexEmbeddingRetriever` | 相似度 | `get_top_k_embeddings` |
| `SummaryIndexLLMRetriever` | LLM 给的 relevance（兜底 1.0） | 解析函数 |
| `KeywordTableGPTRetriever` | `None` | —（命中计数未传出） |
| `KeywordTableSimpleRetriever` | `None` | — |
| `KeywordTableRAKERetriever` | `None` | —（同上） |
| `DocumentSummaryIndexLLMRetriever` | LLM 给的 relevance | 解析函数 |
| `DocumentSummaryIndexEmbeddingRetriever` | `None` | —（相似度被丢） |

所以**八个里只有三个给真正的分数**。而这三个的分数含义还不同（余弦相似度 vs LLM 相对排序）。这意味着**跨检索器的分数不可比**，而 `NodeWithScore.score` 这个字段的存在暗示着可比性。

**两个「embedding 检索器」对「向量从哪来」的处理相反。**

`SummaryIndexEmbeddingRetriever`：**没有向量库**，它把节点的嵌入现算并写回节点，然后本地做 `get_top_k_embeddings`（纯内存计算）。

`DocumentSummaryIndexEmbeddingRetriever`：**必须有向量库**（`self._vector_store = self._index.vector_store`），查询交给后端。

所以这两者是两套完全不同的架构：

| | 列表索引版 | 文档摘要版 |
|---|---|---|
| 向量存哪 | 节点字段（内存/ docstore） | 向量库 |
| 相似度计算 | 本地（Python） | 后端 |
| 扩展性 | 受内存限制 | 受后端能力 |
| 是否需要嵌入模型 | 是（现算） | 是（查查询向量，但节点向量可能已在库里） |

这也解释了为什么 `SummaryIndexEmbeddingRetriever` 的构造函数不需要 `vector_store` 参数，而 `DocumentSummaryIndexEmbeddingRetriever` 从索引里取。

**`BaseKeywordTableRetriever` 的构造函数有一处多余的 callback 兜底。**

```python
super().__init__(
    callback_manager=callback_manager or Settings.callback_manager,
    object_map=object_map,
    verbose=verbose,
)
```

四处子类（`KeywordTableGPTRetriever` 与两个内置的）都重复了同一句 `callback_manager or Settings.callback_manager`——**而基类的 `super().__init__` 又在 `__init__` 里再兜一次**（`callback_manager or CallbackManager()`，见 d01）。

所以链路是：子类传 `callback_manager or Settings.callback_manager` → 基类再 `or CallbackManager()`。第二步永远不会触发（第一步已经保证了非空，除非 `Settings.callback_manager` 本身是 `None`）。这个冗余反映了「到底谁该兜底」的责任不清。

而 `KeywordTableSimpleRetriever` 与 `KeywordTableRAKERetriever` **没有 `__init__`**——它们完全继承基类的。所以它们**无法接收 `llm`**（基类没有这个参数）。这是对的（它们不用 LLM），但也意味着「传入 `llm=` 会被 `**kwargs` 吞掉」：

```python
def __init__(self, index, keyword_extract_template=None, ..., **kwargs: Any) -> None:
```

基类的 `**kwargs` 接收但不使用，也不报错。所以 `KeywordTableSimpleRetriever(index, llm=some_llm)` 会静默忽略。

## 关键取舍

**源码事实：** `SummaryIndexRetriever` 用 `del query_bundle` 明示「与查询无关」，返回全部节点且不设分数。

**源码事实：** `SummaryIndexEmbeddingRetriever` 的嵌入是**惰性且就地写回**的：查询嵌入写进 `query_bundle.embedding`、节点嵌入写进 `node.embedding`（进而可能随 docstore 持久化）；`nodes_embedded` 计数器算了但从未使用；嵌入是逐条调用（无批量），且 DEBUG 日志内含全部命中节点正文。

**源码事实：** `SummaryIndexLLMRetriever` 按 `choice_batch_size`（默认 10）分批调 LLM，**每批独立返回**、不做全局 top k；`choice_idxs` 用 `int(choice) - 1` 且无边界检查；`relevances or [...]` 的兜底会把「解析器返回空列表」读成「全部 1.0」；`zip(choice_nodes, relevances)` 会截断长度不匹配的结果。

**源码事实：** `BaseKeywordTableRetriever` 用「命中几个查询关键词」作为**隐式整数打分**并排序，但**无次序键**，故同分节点的顺序取决于关键词遍历顺序（而关键词来自集合转列表，不确定）——同一查询两次可能选出不同节点；它也**不设分数**，所以唯一的排序信息在结果里丢失。

**源码事实：** 三个关键词子类只覆写 `_get_keywords`：GPT 版用 `DQKET`（查询专用模板，`max_keywords` 作为每次调用的模板变量）与 `extract_keywords_given_response`；Simple 版用正则 + 去停用词 + 词频 top N（对短查询效果可疑）；RAKE 版依赖 `nltk` + `rake_nltk`（仅运行时 import，未在包元数据声明）并默认把短语拆成子词。

**源码事实：** `DocumentSummaryIndexLLMRetriever` 是唯一做「分批累积 + 全局排序取 `choice_top_k` + 展开成文档全部节点」的实现；`choice_top_k` 默认 1 指的是**文档数**而非节点数；同一文档的所有节点共享一个分数；其 `choice_idxs` 就是 `choice - 1`（不同于列表版的 `int(choice) - 1`）。

**源码事实：** `DocumentSummaryIndexEmbeddingRetriever` 读 `ids` 优先、`nodes` 次之，两路都无才抛 `ValueError`；而 `query_result.similarities` **从未被读取**，故分数被丢弃、下游的相似度阈值失效；它只在 `vector_store.is_embedding_query` 时才计算查询嵌入。

**源码事实：** 八个检索器里只有三个真正给出分数（两个 embedding 版里的列表版、两个 LLM 版），且分数含义不同（余弦相似度 vs LLM 相对排序），所以跨检索器的分数不可比。

**源码事实：** `SummaryIndexEmbeddingRetriever` 的向量在节点字段里（本地做 top-k），`DocumentSummaryIndexEmbeddingRetriever` 的向量在向量库（后端做 top-k）——同名的「embedding 检索器」是两套架构。

**源码事实：** 两个内置关键词检索器没有 `__init__`，因此 `llm=` 之类参数会被基类的 `**kwargs` 静默吞掉。

**架构推论：** 「换检索器」在这套框架里是一次**语义变更而非配置变更**。因为 `k` 的含义、分数的有无、返回规模、所需模型都随实现改变，用户无法只改一个参数就得到「同样的检索、不同的精度」。更可用的设计应当先固定契约（例如「返回按分数降序、分数在 [0,1]」），再让实现满足它——而这正是本篇看到的缺失。

**架构推论：** 「LLM 分批选而不做全局排序」与「分批 + 全局决赛」这两种策略的差别，本质是**上下文预算与选择质量的对立**。一次给 LLM 看 10 条已经占很多 token；要看 100 条就必须分 10 批。分批独立选的问题是「每批内的相对排序无法与别批比较」，而全局决赛（摘要索引版）解决了它但需要额外的排序与截断步骤。列表索引版没做这一步，所以它的输出规模随索引增大而线性增长——这是一个容易被忽略的行为：`choice_batch_size=10` 且索引有 200 个节点时，它会返回**最多 200 条**（每批都可能有命中）。

**架构推论：** `relevances or [1.0 ...]` 与 `n.score or 1.0`（d01）是同一类错误的两个实例：**用真值判断代替存在判断**。在分数/列表这类「空值合法」的场合，`or` 会把合法的「零/空」替换成默认值。这个错误在 Python 里高频，因为 `0`、`""`、`[]` 都是 falsy。

**架构推论：** 「向量相似度被丢弃」是本篇最具体的一处信息损失。它让 `DocumentSummaryIndexEmbeddingRetriever` 与 `SummaryIndexRetriever` 在**输出形状上变得相同**（一串无分数节点），尽管前者做了真正的相似度排序而后者全给。于是「有没有做检索」这件事在下游无法判断——只能靠结果的数量猜。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexRetriever` | 全量返回，`del query_bundle` 明示与查询无关 |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexEmbeddingRetriever` | 惰性逐条嵌入 + 本地 top-k，分数来自相似度 |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexLLMRetriever` | 分批 LLM 选、每批独立、无边界检查 |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexRetriever`（兼作旧名 `ListIndexRetriever` 别名） | 全量返回、不打分 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `BaseKeywordTableRetriever` | 命中计数排序、不设分数、同分顺序不可复现 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableGPTRetriever` | `DQKET` 查询专用模板 + LLM 抽词 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableSimpleRetriever` | 正则 + 去停用词 + 词频 top N，无 `__init__` |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableRAKERetriever` | RAKE 短语抽词，需 `nltk` + `rake_nltk` |
| `llama-index-core/llama_index/core/indices/keyword_table/utils.py` | `simple_extract_keywords` | 正则取词 → 小写 → 去停用词 → `Counter.most_common` |
| `llama-index-core/llama_index/core/indices/keyword_table/utils.py` | `rake_extract_keywords` | 运行时 import 两个可选依赖 |
| `llama-index-core/llama_index/core/indices/document_summary/retrievers.py` | `DocumentSummaryIndexLLMRetriever` | 分批累积 + 全局决赛 + 展开为文档全部节点 |
| `llama-index-core/llama_index/core/indices/document_summary/retrievers.py` | `DocumentSummaryIndexEmbeddingRetriever` | 向量库查摘要，丢弃 `similarities` |
| `llama-index-core/llama_index/core/indices/document_summary/retrievers.py` | `DocumentSummaryIndexRetriever` 别名 | 旧名指向 LLM 版 |

## 参考

- [llama-index 源码：indices/list/retrievers.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/list/retrievers.py)
- [llama-index 源码：indices/document_summary/retrievers.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/document_summary/retrievers.py)
- [llama-index 文档：检索器模式](https://docs.llamaindex.ai/en/stable/module_guides/querying/retriever/retrievers/)
- [RAKE 论文：无监督关键词抽取](https://aclanthology.org/W10-1108/)

## 与主核 / AI 的关系

这六个检索器与主核（多模态数据库内核）的**访问方法（access method）**是同构的：同一个表可以走顺序扫描、索引扫描、位图扫描，而「选哪个」决定了代价与结果质量。主核的关键设计是**规划器按代价估算选方法**，而估算依赖统计信息；这套框架没有规划器，所以「选哪个检索器」由用户手写。

主核那边能自动选择，是因为**所有访问方法返回同一形状的结果**（按执行器契约），且它们的代价有统一的度量。这里恰恰缺这两条：返回形状不同（分数有无、数量语义不同）、代价没有度量（一次 LLM 调用相当于多少次嵌入？）。

第二处对照是**「分批处理的合并策略」**。主核的 `MergeAppend` 节点能对多个有序子结果做归并，因为每个子结果都保证**同一排序键定义**。这里的两个 LLM 检索器就是一个没有 `MergeAppend` 的情形：列表版把各批结果直接拼接（等价于没有归并的 `Append`），而摘要版自己实现了一个「全局排序」（等价于 `Sort` 后再 `Limit`）。

第三处是**分数语义**。主核的代价估算有明确单位（页数、元组数），所以可比较；这里的分数有三种互不兼容的含义。`NodeWithScore` 这个类型名暗示了可比性，但实际不可比——这与「同一个列名在不同表里是不同类型」是同一类陷阱。

对 AI 工程的实际意义在于：检索器选型的后果比通常认为的更大。这份实现给出的三条经验可直接用：**先固定检索器契约再写实现**（否则 `k` 的语义会随实现漂移）、**分数缺失要显式表达**（`None` 与 0 必须分开，否则阈值过滤失效）、以及**分批选择的输出规模会随数据增长**（每批都可能有命中的设计在大索引上会失控）。
