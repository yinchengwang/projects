---
project: llama-index
feature: keyword-and-docsummary-index
title: 关键词索引与文档摘要索引
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/keyword_table/base.py
    symbols: [KeywordTableIndex, BaseKeywordTableIndex, KeywordTableRetrieverMode]
  - path: llama-index-core/llama_index/core/indices/document_summary/base.py
    symbols: [DocumentSummaryIndex, DocumentSummaryRetrieverMode]
---

# 关键词索引与文档摘要索引

## 一句话职责

`KeywordTableIndex` 用「关键词 → 节点 id」的倒排表当索引（构建时每个节点抽关键词，查询时问题抽关键词再查表）；`DocumentSummaryIndex` 用「每份文档一段摘要」当索引（构建时按文档聚合节点生成摘要，检索时先在摘要层匹配、再回到该文档的全部节点）。

## 机制是什么

这两个索引的共同思路都是**用一份比原文更小的东西来定位**，但「更小的东西」是什么，决定了两套完全不同的维护成本：

| | `KeywordTableIndex` | `DocumentSummaryIndex` |
|---|---|---|
| 索引条目 | 关键词（从节点文本抽出） | 文档摘要（LLM 生成） |
| 粒度 | **节点级** | **文档级** |
| 查询时匹配 | 关键词精确匹配（集合相交） | 摘要的嵌入相似度 或 LLM 判断 |
| 构建时 LLM 调用 | 每节点一次（抽关键词） | 每文档一次（合成摘要） |
| 删除 | **可以**：从倒排表里摘掉 id | **可以**：按文档整片删除 |
| 删除的代价 | 遍历全表找该 id | 无（整片） |

而两者的关键差别在**索引条目是否可逆**：关键词是从文本里抽出来的（抽偏了不影响文本本身），而摘要是**新生成的节点**（它会进 docstore，成为真实数据的一部分）。

所以 `KeywordTableIndex` 的删除能精确做到「从每个关键词的 id 集合里移除」——因为关键词表只是引用；而 `DocumentSummaryIndex` 的删除必须**整份文档删**（连同它生成的摘要节点），无法只删某个节点——因为摘要覆盖的是整份文档的全部节点，少了任何一个摘要就不再准确。

## 怎么实现的

**`KeywordTableRetrieverMode` 三个值，映射到三个检索器，而 `DEFAULT` 是 LLM 那个。**

```python
class KeywordTableRetrieverMode(str, Enum):
    DEFAULT = "default"
    SIMPLE = "simple"
    RAKE = "rake"
```

```python
if retriever_mode == KeywordTableRetrieverMode.DEFAULT:
    return KeywordTableGPTRetriever(self, object_map=self._object_map, llm=self._llm, **kwargs)
elif retriever_mode == KeywordTableRetrieverMode.SIMPLE:
    return KeywordTableSimpleRetriever(self, object_map=self._object_map, **kwargs)
elif retriever_mode == KeywordTableRetrieverMode.RAKE:
    return KeywordTableRAKERetriever(self, object_map=self._object_map, **kwargs)
else:
    raise ValueError(f"Unknown retriever mode: {retriever_mode}")
```

注意 **`DEFAULT`（即默认）需要 LLM**，而 `SIMPLE` 与 `RAKE` 不需要——后者用的是本地算法（RAKE 是关键词抽取的经典无监督法）。所以「默认路径最贵」在这里成立，这与 `SummaryIndex` 恰好相反（那里 `DEFAULT` 是零成本）。

**`__init__` 里有一处「提前 partial 模板」的做法。**

```python
self.max_keywords_per_chunk = max_keywords_per_chunk
self.keyword_extract_template = keyword_extract_template or DEFAULT_KEYWORD_EXTRACT_TEMPLATE
self.keyword_extract_template = self.keyword_extract_template.partial_format(
    max_keywords=self.max_keywords_per_chunk
)
```

注释写「NOTE: Partially format keyword extract template here.」——即在构造时就把 `max_keywords` 这个变量填进去。

三处：

1. **`partial_format` 在构造时执行一次**，所以之后每次抽取都不必再传这个变量。这与「每次调用都传」的写法功能等价，但少一处遗漏的机会。
2. **`self.keyword_extract_template` 被赋值两次**（先赋值再覆盖）。第一行是取出（用户给的或默认），第二行是 partial 后重新赋值。这类写法让「原值」不再可得——若某个子类想用未 partial 的模板，它拿不到。
3. **默认 `max_keywords_per_chunk=10`**。这个数决定了关键词表的宽度，进而决定查询命中的粒度：太少则命中率低，太多则一个关键词指向过多节点。

**`_extract_keywords` 是抽象方法，而 `KeywordTableIndex` 用 LLM 实现它。**

```python
def _extract_keywords(self, text: str) -> Set[str]:
    response = self._llm.predict(self.keyword_extract_template, text=text)
    return extract_keywords_given_response(response, start_token="KEYWORDS:")
```

即「让 LLM 输出 `KEYWORDS: a, b, c` 这样一行，再解析」。而解析函数的行为值得精确理解：

```python
def extract_keywords_given_response(response: str, lowercase: bool = True, start_token: str = "") -> Set[str]:
    results = []
    response = response.strip()
    if response.startswith(start_token):
        response = response[len(start_token):]
    keywords = response.split(",")
    for k in keywords:
        rk = k
        if lowercase:
            rk = rk.lower()
        results.append(rk.strip())
    return expand_tokens_with_subtokens(set(results))
```

四处：

1. **`start_token` 缺失时不报错**——文档说「Raises exception if response doesn't start with `<start_token>`」，但实现里**只是不剥前缀**。于是 `"foo, bar"` 会被解析成关键词 `"foo"` 与 `"bar"`。这与文档相反，是一个真实的文档与实现不符。

2. **`split(",")` 只看逗号**。所以中文输出用顿号「、」分隔时会得到**一整个长关键词**。而默认提示词要求逗号分隔，所以这依赖模型遵守格式——一个不遵守的模型会污染整个关键词表。

3. **`lowercase=True` 默认**，且用 `rk.lower()`。所以 `"Python"` 与 `"python"` 被归一。这是必需的（查询时也要小写匹配），但代价是**大小写敏感的词**（如 `ReLU` 与 `relu`）被合并。

4. **`expand_tokens_with_subtokens(set(results))`** 是最后一步扩充：把「多个词的短语」拆成子词并去掉停用词。所以一个关键词 `"machine learning"` 会额外产生 `"machine"` 与 `"learning"`。这让匹配更宽松（命中率高），但也让表更大（一个节点可能贡献十几个条目）。

   注意 `set(results)` 在扩充前先去重，但**空字符串也会被保留**：若响应以 `KEYWORDS:` 开头且后面为空，`split(",")` 得到 `[""]`，`strip()` 得到 `""`。所以一个空的 `""` 关键词可能进表——而它会与任何「抽不出关键词」的查询相互匹配。这是一个安静的正确性问题。

**`_add_nodes_to_index` 与 `_async_add_nodes_to_index` 是一对，差别只在 `await`。**

```python
for n in nodes_with_progress:
    keywords = self._extract_keywords(n.get_content(metadata_mode=MetadataMode.LLM))
    index_struct.add_node(list(keywords), n)
```

三处：

1. **`get_content(MetadataMode.LLM)`** 而不是 `NONE`——所以**元数据也参与抽关键词**。这与 c02 的向量索引用 `EMBED` 模式形成对照：那里关心「参与嵌入的文本」，这里关心「给模型看的文本」。这个选择是对的——文件名、章节标题往往是很好的关键词。
2. **`list(keywords)` 把集合转成列表再传**。而 `KeywordTable.add_node` 的签名要求 `List[str]`。所以集合的**遍历顺序会影响**……不，实际上不影响结果（`add_node` 只是把每个关键词挂上 id）。但列表化意味着「顺序不确定」这个属性被带进了接口。
3. **`_async_extract_keywords` 默认实现是「调同步版」**，而子类可以覆写成真正的异步（`KeywordTableIndex` 就覆写了，用 `await self._llm.apredict(...)`）。所以这是一个**可选覆写**的钩子。

**`_build_index_from_nodes` 用 `run_async_tasks`，而它的用法有一处可疑。**

```python
index_struct = KeywordTable(table={})
if self._use_async:
    tasks = [self._async_add_nodes_to_index(index_struct, nodes, self._show_progress)]
    run_async_tasks(tasks)
else:
    self._add_nodes_to_index(index_struct, nodes, self._show_progress)
return index_struct
```

注意 `tasks` 是**只有一个元素的列表**——即把一个协程包成单任务列表交给 `run_async_tasks`。所以这里有「并行」的形式但没有并行：`_async_add_nodes_to_index` 内部是 `for` 循环逐节点串行 await 的。

而 `index_struct` 是在 `run_async_tasks` **之前**创建的，之后被协程捕获并修改。所以 `run_async_tasks` 完成后 `index_struct` 里已经有数据——这依赖「协程真的跑完了」而不是「被调度了」。若 `run_async_tasks` 的实现是 fire-and-forget，这里就会拿到一个空表。这是**隐式时序依赖**，源码没有注释说明。

c2 的向量索引里 `_build_index_from_nodes` 用了完全相同的写法（`tasks = [self._async_add_nodes_to_index(...)]; run_async_tasks(tasks)`）——所以这是这个项目里的一处惯用模式。

**`_insert` 与 `_build_index_from_nodes` 走同一条抽取逻辑，但 `_insert` 是纯同步的。**

```python
def _insert(self, nodes, **insert_kwargs) -> None:
    for n in nodes:
        keywords = self._extract_keywords(n.get_content(metadata_mode=MetadataMode.LLM))
        self._index_struct.add_node(list(keywords), n)
```

即**没有 `use_async` 分支**。所以 `use_async=True` 的索引在增量插入时会退化成同步——一次 LLM 调用一条。这与 c02 里 `insert_nodes` 有同步/异步两版形成对比。

**`_delete_node` 是这套索引里唯一「不靠 LLM 也能精确维护」的实现。**

```python
keywords_to_delete = set()
for keyword, existing_node_ids in self._index_struct.table.items():
    if node_id in existing_node_ids:
        existing_node_ids.remove(node_id)
        if len(existing_node_ids) == 0:
            keywords_to_delete.add(keyword)

for keyword in keywords_to_delete:
    del self._index_struct.table[keyword]
```

四处：

1. **遍历整个表**（O(关键词数) × O(集合查询)）。所以删除一个大索引里的单个节点是昂贵的。
2. **`existing_node_ids.remove(node_id)` 就地修改集合**。所以这里改的是 `_index_struct` 内部的数据——依赖 `items()` 返回的集合是可变的引用。
3. **空集合的关键词被删掉**。这一步是必需的：留着空集合会让「关键词存在但没有任何节点」的状态累积，而查询命中一个空集合会返回空结果——看起来像「没找到」而不是「这个关键词已失效」。
4. **`del self._index_struct.table[keyword]` 在遍历结束之后才执行**（先收集再删）。这是必需的，否则「遍历中删除字典」会抛 `RuntimeError`。

**`KeywordTable` 的 `node_ids` 属性有一处空表会抛错。**

```python
@property
def node_ids(self) -> Set[str]:
    return set.union(*self.table.values())
```

`set.union(*[])`（即空表时）会抛 `TypeError: unary union not supported` 或 `descriptor 'union' ... needs an argument`。所以**空关键词表调用 `node_ids` 会崩**。

而 `ref_doc_info` 里用了同样危险的写法但加了空表处理吗？

```python
node_doc_ids_sets = list(self._index_struct.table.values())
node_doc_ids = list(set().union(*node_doc_ids_sets))
```

这里用的是 `set().union(*sets)`——**先有一个空集合作为起点**，所以空列表时返回 `set()`，不崩。这是同一个操作在 `KeywordTable.node_ids`（崩）与 `ref_doc_info`（不崩）两处的不同写法。前者是 bug 级的差异。

**`DocumentSummaryRetrieverMode` 只有两个值，而 `EMBEDDING` 是默认且需要前置条件。**

```python
class DocumentSummaryRetrieverMode(str, Enum):
    EMBEDDING = "embedding"
    LLM = "llm"
```

```python
if retriever_mode == _RetrieverMode.EMBEDDING:
    if not self._embed_summaries:
        raise ValueError("Cannot use embedding retriever if embed_summaries is False")
    return EmbeddingRetriever(self, object_map=self._object_map, embed_model=self._embed_model, **kwargs)
if retriever_mode == _RetrieverMode.LLM:
    return LLMRetriever(self, object_map=self._object_map, llm=self._llm, **kwargs)
else:
    raise ValueError(f"Unknown retriever mode: {retriever_mode}")
```

四处：

1. **`_embed_summaries=False` 时 `EMBEDDING` 模式直接报错**。这是一个**构建时选择与查询时必须匹配**的耦合：`embed_summaries` 是构造参数，而用错模式要到查询时才炸。

2. **两个 `if` 而不是 `if/elif`，且第二个带 `else`**。所以第一个 `if` 命中后 return，不命中则落到第二个 `if`；若第二个也不命中则进它的 `else`。逻辑上正确，但结构上是「两个独立 if」，读者需要注意到第一个分支里的 return 才能明白。

3. **`_RetrieverMode = DocumentSummaryRetrieverMode` 这个别名**。所以类型注解里用的是 `_RetrieverMode` 而类名是 `DocumentSummaryRetrieverMode`。别名以 `_` 开头（惯例上是私有的），但它出现在**公开方法签名里**。这是一个小的不一致——用户在文档里看到的是 `DocumentSummaryRetrieverMode`，而签名里写 `_RetrieverMode`。

4. **`LLMRetriever = DocumentSummaryIndexLLMRetriever` 两个局部别名**：

```python
LLMRetriever = DocumentSummaryIndexLLMRetriever
EmbeddingRetriever = DocumentSummaryIndexEmbeddingRetriever
```

   这两个别名只在接下来的几行里用一次。作用是缩短行宽。但它们**遮蔽了模块级的同名概念**（没有，只是局部变量）——无害但无收益。

**`_add_nodes_to_index` 是 `DocumentSummaryIndex` 的核心，而它的三步结构很清楚。**

第一步：**按文档聚合**。

```python
doc_id_to_nodes = defaultdict(list)
for node in nodes:
    if node.ref_doc_id is None:
        raise ValueError("ref_doc_id of node cannot be None when building a document summary index")
    doc_id_to_nodes[node.ref_doc_id].append(node)
```

两处：

- **`node.ref_doc_id` 必须有**（即 `source_node` 必须存在）。这是 b01 的 `NodeRelationship.SOURCE` 关系的用途——没有它就不知道该把节点归到哪份文档。**报错而不是跳过**是对的：一个没有归属的节点无法参与文档级摘要。
- **`defaultdict(list)` + `append`**，所以同一文档的节点按输入顺序累积。

第二步：**每份文档合成一段摘要**。

```python
for doc_id, nodes in iterable_with_progress:
    print(f"current doc id: {doc_id}")
    nodes_with_scores = [NodeWithScore(node=n) for n in nodes]
    summary_response = self._response_synthesizer.synthesize(query=self._summary_query, nodes=nodes_with_scores)
    summary_response = cast(Response, summary_response)
    docid_first_node = doc_id_to_nodes.get(doc_id, [TextNode()])[0]
    summary_node_dict[doc_id] = TextNode(
        text=summary_response.response,
        relationships={NodeRelationship.SOURCE: RelatedNodeInfo(node_id=doc_id)},
        metadata=docid_first_node.metadata,
        excluded_embed_metadata_keys=docid_first_node.excluded_embed_metadata_keys,
        excluded_llm_metadata_keys=docid_first_node.excluded_llm_metadata_keys,
    )
    self.docstore.add_documents([summary_node_dict[doc_id]])
```

五处：

1. **`print(f"current doc id: {doc_id}")` 是无条件的**（不受 `show_progress` 控制）。所以构建一个文档摘要索引会把每份文档的 id 打到 stdout。这是库代码里很不合适的输出，而且它与 `show_progress=True` 时 tqdm 的进度条混在一起。

2. **摘要节点通过 `relationships={SOURCE: RelatedNodeInfo(node_id=doc_id)}` 反向指回原文档**。所以摘要节点与普通节点在结构上一样——都靠 SOURCE 关系表达归属。这让 `ref_doc_info` 之类的通用逻辑能继续工作。

3. **`metadata` 与两个 `excluded_*_keys` 从该文档的第一个节点复制**。所以摘要节点带上与首块相同的元数据（如文件名）。这个选择让摘要节点的元数据过滤行为与原节点一致。注意 `metadata=docid_first_node.metadata` 是**直接引用同一个字典**（b01 讲过 `as_related_node_info` 也这么做）——所以改一处会影响另一处。

4. **`doc_id_to_nodes.get(doc_id, [TextNode()])[0]`** 的 `default` 永远不会生效（因为 `doc_id` 来自这个字典的键）。所以 `[TextNode()]` 是死代码。

5. **摘要节点立即写入 docstore**（`self.docstore.add_documents([...])`），在循环内部。所以**中间失败会留下部分摘要节点**——而这些节点没有对应的 index_struct 记录（`add_summary_and_nodes` 在循环之后）。

第三步：**先持久化摘要节点，再写 index_struct，最后可选嵌入**。

```python
for doc_id, nodes in doc_id_to_nodes.items():
    index_struct.add_summary_and_nodes(summary_node_dict[doc_id], nodes)

if self._embed_summaries:
    summary_nodes = list(summary_node_dict.values())
    id_to_embed_map = embed_nodes(summary_nodes, self._embed_model, show_progress=show_progress)
    summary_nodes_with_embedding = []
    for node in summary_nodes:
        node_with_embedding = node.model_copy()
        node_with_embedding.embedding = id_to_embed_map[node.node_id]
        summary_nodes_with_embedding.append(node_with_embedding)
    self._vector_store.add(summary_nodes_with_embedding)
```

三处：

- **`add_summary_and_nodes` 一次建立三向映射**（`IndexDocumentSummary` 里见下）。
- **嵌入与 c02 一样是「批量算 + `model_copy` 写回」**，但这里**直接写 `self._vector_store.add(...)`**（没有分批、没有 id 校验）。
- **`id_to_embed_map[node.node_id]` 用方括号**——与 c02 同样的 `KeyError` 风险。

**`IndexDocumentSummary` 的三张映射是这套索引的全部骨架。**

```python
summary_id_to_node_ids: Dict[str, List[str]]   # 摘要 id → 该文档的所有节点 id
node_id_to_summary_id: Dict[str, str]          # 节点 id → 所属摘要 id
doc_id_to_summary_id: Dict[str, str]           # 文档 id → 摘要 id
```

三张表分别服务三个方向：**「这摘要覆盖哪些节点」（检索后取内容）、「这个节点属于哪个摘要」（删节点时找摘要）、「这份文档的摘要是什么」（按文档删 / 查摘要）**。

`add_summary_and_nodes` 同时填三张：

```python
summary_id = summary_node.node_id
ref_doc_id = summary_node.ref_doc_id
if ref_doc_id is None:
    raise ValueError("ref_doc_id of node cannot be None when building a document summary index")
self.doc_id_to_summary_id[ref_doc_id] = summary_id
for node in nodes:
    node_id = node.node_id
    if summary_id not in self.summary_id_to_node_ids:
        self.summary_id_to_node_ids[summary_id] = []
    self.summary_id_to_node_ids[summary_id].append(node_id)
    self.node_id_to_summary_id[node_id] = summary_id
```

注意**摘要节点的 `ref_doc_id` 又被检查了一次**——它在 `_add_nodes_to_index` 里刚被手工构造出来（`relationships={SOURCE: ...}`），所以这个检查在这里是冗余的。但它作为**结构不变量**是必要的：任何构造摘要节点的地方都必须打 SOURCE 关系。

`IndexDocumentSummary.delete` 是**按文档整片删**：

```python
def delete(self, doc_id: str) -> None:
    summary_id = self.doc_id_to_summary_id[doc_id]
    del self.doc_id_to_summary_id[doc_id]
    node_ids = self.summary_id_to_node_ids[summary_id]
    for node_id in node_ids:
        del self.node_id_to_summary_id[node_id]
    del self.summary_id_to_node_ids[summary_id]
```

四处：

1. **`self.doc_id_to_summary_id[doc_id]` 用方括号**——文档不存在则 `KeyError`。（而 `delete_ref_doc` 在调它之前已经用 `get_ref_doc_info` 检查过了，所以实际不会。）
2. **`node_ids = self.summary_id_to_node_ids[summary_id]` 是引用**，随后的 `del` 只删别的表，不影响它。
3. **摘要节点自身**（`summary_id`）**没有被删**——它还在 docstore 里。所以「删文档」之后 docstore 里留着孤儿摘要节点。只有 `delete_from_docstore=True` 时 `docstore.delete_ref_doc(ref_doc_id)` 可能会把它带走（取决于该摘要节点是否被登记为该文档的节点）。
4. **摘要节点与它覆盖的节点都没有被删**——它们可能是别的文档的节点吗？不可能（一个节点只属于一份文档）。所以实际的节点清理依赖 docstore 层面的删除。

`delete_nodes` 是**单节点删除**，而它的实现绕了两圈：

```python
index_nodes = set(self._index_struct.node_id_to_summary_id.keys())
valid_node_ids = []
for node in node_ids:
    if node not in index_nodes:
        logger.warning(f"node_id {node} not found, will not be deleted.")
    else:
        valid_node_ids.append(node)

self._index_struct.delete_nodes(valid_node_ids)

remove_summary_ids = [summary_id for summary_id in self._index_struct.summary_id_to_node_ids
                      if len(self._index_struct.summary_id_to_node_ids[summary_id]) == 0]
remove_docs = [doc_id for doc_id in self._index_struct.doc_id_to_summary_id
               if self._index_struct.doc_id_to_summary_id[doc_id] in remove_summary_ids]
for doc_id in remove_docs:
    self.delete_ref_doc(doc_id)
```

四处：

1. **先过滤出存在于索引的 id**，不在的只警告。所以**部分删除**是可能的：传十个 id，可能只删了七个。
2. **`self._index_struct.delete_nodes(...)`** 把节点从两张表里摘掉（`summary_id_to_node_ids[summary_id].remove(node_id)` 与 `del node_id_to_summary_id[node_id]`）。注意 `.remove()` 在元素不存在时抛 `ValueError`——但前面已经过滤过了，所以不会。
3. **然后找「节点集合已空」的摘要**，再找「摘要属于这些」的文档，最后**逐文档调 `delete_ref_doc`**。所以「删光一份文档的所有节点」会**自动删掉整份文档**（含它的摘要）。
4. **这段逻辑有一个隐含承诺**：摘要覆盖的节点集合空了 ⇒ 该文档应该整个消失。但这是**推断**而不是声明——若某个文档还有节点但不在索引里（例如构建时被跳过），它也会被删。

而 `delete_ref_doc` 本身：

```python
self._index_struct.delete(ref_doc_id)
self._vector_store.delete(ref_doc_id)
```

**`self._vector_store.delete(ref_doc_id)` 传的是文档 id**——向量库里存的却是**摘要节点**的向量（`add` 时用的是 `summary_nodes_with_embedding`，其 id 是摘要节点 id）。所以**这个删除调用用的是错误的 id**：它应该传摘要节点 id 或摘要节点的 id 列表。

这条路径只能在后端「按元数据或按已知映射删」时生效——而摘要节点的 `metadata` 里**没有** `ref_doc_id`（只有从首块复制的元数据）。所以这次 `self._vector_store.delete(ref_doc_id)` 大概率**什么都没删**，留下孤儿向量。

这是一个真实的缺陷：`delete_ref_doc` 会更新 index_struct（映射表清空）但不清理向量库。后果是「删了文档，重启后摘要检索仍可能命中它（若向量库持久化）」。

**`ref_doc_info` 在文档摘要索引里是最简单的一个实现。**

```python
ref_doc_ids = list(self._index_struct.doc_id_to_summary_id.keys())
all_ref_doc_info = {}
for ref_doc_id in ref_doc_ids:
    ref_doc_info = self.docstore.get_ref_doc_info(ref_doc_id)
    if not ref_doc_info:
        continue
    all_ref_doc_info[ref_doc_id] = ref_doc_info
return all_ref_doc_info
```

**不需要遍历节点找 SOURCE 关系**——因为索引结构的第一张表已经直接记录了「文档 id」。所以这比 c02/c03 的两个实现都便宜（O(文档数) 而非 O(节点数)）。

这揭示了「索引结构里存什么」的一个直接后果：**存了「文档 id → 摘要 id」这份映射，就把「按文档聚合」从 O(节点) 降到 O(文档)**。

**`_delete_node` 是空实现，与 c02 的 `VectorStoreIndex._delete_node` 一样。**

```python
def _delete_node(self, node_id: str, **delete_kwargs: Any) -> None:
    pass
```

所以父类的 `BaseIndex.delete_nodes` 在这个索引上也「成功但什么都不删」。真正能工作的是覆写版（见上）。**两个索引犯同一个错**，说明这是从 c01 的抽象方法演化下来的一处系统性遗留。

## 关键取舍

**源码事实：** `KeywordTableRetrieverMode` 的 `DEFAULT` 映射到需要 LLM 的 `KeywordTableGPTRetriever`，而 `SIMPLE`（关键词精确匹配）与 `RAKE`（本地无监督抽取）都不需要模型——「默认最贵」与列表索引的「默认零成本」相反。

**源码事实：** 关键词抽取的模板在**构造时**就 `partial_format(max_keywords=...)`，之后每次调用不必再传；`max_keywords_per_chunk` 默认 10。

**源码事实：** `extract_keywords_given_response` 的文档说「response 不以 `start_token` 开头就抛异常」，而实现只是不剥前缀、继续按逗号切分；它还会 `lowercase` 并把短语拆成子词（`expand_tokens_with_subtokens`），空字符串可能作为关键词进表。

**源码事实：** 构建时用 `get_content(MetadataMode.LLM)` 作为抽关键词的文本（元数据参与），与向量索引用 `MetadataMode.EMBED` 不同。

**源码事实：** `_build_index_from_nodes` 把单个协程包成单元素任务列表交给 `run_async_tasks`（有并行形式无并行实质），且依赖它在返回前完成任务；`_insert` **没有** async 分支，与 `_build` 不对称。

**源码事实：** `KeywordTableIndex._delete_node` 遍历全表摘除 id、收集空集合的关键词、最后再 `del`（避免遍历中删字典）；空表时 `set.union(*[])` 会抛错，而 `ref_doc_info` 用 `set().union(*sets)` 有初始空集合所以安全——同一操作两处写法不一致。

**源码事实：** `DocumentSummaryIndex` 的 `embed_summaries` 是构造期开关，而 `EMBEDDING` 检索模式在查询时报错提示二者不匹配。

**源码事实：** `_add_nodes_to_index` 无条件 `print(f"current doc id: {doc_id}")`；摘要节点带 `SOURCE` 回指原文档、元数据与排除键从首块复制（`metadata` 为同引用）；摘要节点在循环内即写入 docstore，而 index_struct 在循环后才更新。

**源码事实：** `IndexDocumentSummary` 三张映射（摘要→节点、节点→摘要、文档→摘要）；`delete` 只清映射表，摘要节点本身留在 docstore。

**源码事实：** `DocumentSummaryIndex.delete_nodes` 对不存在的 id 只警告（允许部分删除），并在「某摘要的节点集合空了」时**推断**该文档应整体删除；`delete_ref_doc` 调 `self._vector_store.delete(ref_doc_id)` 传的是**文档 id**，而向量库里存的是摘要节点的向量——这次调用删不掉任何东西，留下孤儿向量。

**源码事实：** `DocumentSummaryIndex._delete_node` 与 `VectorStoreIndex._delete_node` 都是 `pass`，使父类版 `delete_nodes` 在这两个索引上静默无效。

**源码事实：** `DocumentSummaryIndex.ref_doc_info` 直接读 `doc_id_to_summary_id` 的键，是三个索引里唯一 O(文档数) 的实现。

**架构推论：** 关键词索引的核心取舍是**「索引条目是不是引用」**。关键词表里存的只是节点 id（引用），所以删除能精确到「从每个关键词的集合里摘一颗」；而摘要索引存的摘要是**新内容**（要进 docstore、要进向量库），所以删除只能是整片的。这条差别可以推广成一条设计判据：**索引里存引用就能细粒度维护，存派生内容就只能粗粒度维护**——而派生内容越贵（LLM 生成），维护能力越弱。

**架构推论：** `DocumentSummaryIndex.delete_ref_doc` 里那个错误的 `vector_store.delete(ref_doc_id)` 揭示了一个更普遍的问题：**同一个标识符空间在多个存储里含义不同**。docstore 里的键是节点 id，向量库里的键是「你 add 时用的那个 id」，而 index_struct 里记的是映射。任何一处用错 id 空间，删除就会静默失效。这比抛异常更危险——用户会看到「删除成功」。

**架构推论：** `_delete_node` 在两处都是 `pass`，说明 c01 的抽象方法设计有一个缺口：**它假定所有索引都支持单节点删除**，而实际上「按文档删」才是这些索引真正的粒度。更贴切的抽象是有 `_delete_node` 的默认实现抛 `NotImplementedError`（像 `TreeIndex` 那样），让「不支持」显式化——`TreeIndex` 就是这么做的，而这两个索引用 `pass` 绕过去了。

**架构推论：** `print` 出现在库代码里（关键词构建的提示、文档摘要的 doc id）是一类共同的问题：**库不该直接写 stdout**。它无法被日志配置过滤、会污染使用者的输出、且在服务器环境下可能阻塞。这与 a02 里 `print` 警告、c02 里 `print("Some nodes are missing content")` 是同一个模式——说明这个项目对「如何报告非致命情况」没有统一约定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `KeywordTableRetrieverMode` | 三模式：LLM（默认）/ 简单匹配 / RAKE |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `BaseKeywordTableIndex` | 倒排表索引基类，抽词动作抽象给子类 |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `BaseKeywordTableIndex.__init__` | 构造时 `partial_format(max_keywords=...)` |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `BaseKeywordTableIndex._add_nodes_to_index` | 按 `MetadataMode.LLM` 抽词并写入倒排表 |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `BaseKeywordTableIndex._build_index_from_nodes` | 单任务列表 + `run_async_tasks` 的伪并行 |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `BaseKeywordTableIndex._delete_node` | 遍历全表摘 id，收集空集后统一删键 |
| `llama-index-core/llama_index/core/indices/keyword_table/base.py` | `KeywordTableIndex` | 用 LLM 抽词的实现（同步与异步各一版） |
| `llama-index-core/llama_index/core/indices/keyword_table/utils.py` | `extract_keywords_given_response` | 逗号切分 + 小写 + 子词扩充；文档与实现不符 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `KeywordTable` | `table` 倒排表；`node_ids` 在空表时抛错 |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryRetrieverMode` | `embedding`（默认）/ `llm`，前者需 `embed_summaries` |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryIndex` | 文档级摘要索引，构建时每文档合成一段摘要 |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryIndex._add_nodes_to_index` | 按 `ref_doc_id` 聚合 → 合成摘要 → 写 docstore → 填映射 → 可选嵌入 |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryIndex.delete_ref_doc` | 清映射并调 `vector_store.delete(ref_doc_id)`（id 空间错配） |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryIndex.ref_doc_info` | 直接读 `doc_id_to_summary_id`，O(文档数) |
| `llama-index-core/llama_index/core/data_structs/document_summary.py` | `IndexDocumentSummary` | 三张映射与两种删除（按文档 / 按节点） |

## 参考

- [llama-index 源码：keyword_table/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/keyword_table/base.py)
- [llama-index 源码：document_summary/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/document_summary/base.py)
- [llama-index 文档：关键词表索引](https://docs.llamaindex.ai/en/stable/module_guides/indexing/index_guide/)
- [RAKE 论文：无监督关键词抽取](https://aclanthology.org/W10-1108/)

## 与主核 / AI 的关系

`KeywordTableIndex` 与主核（多模态数据库内核）的**倒排索引**完全同构：关键词 ↔ 键，节点 id ↔ 行标识。两者的共同优势是**精确匹配**（不像向量检索是近似的），共同劣势是**词形敏感**（`relu` 与 `ReLU` 若不归一就是两个键）。

真正值得对照的是**删除的实现**。主核删一行时要更新所有索引条目，靠的是「索引条目里带行标识」；关键词表也是这样——它遍历全表找那个 id，然后摘掉。区别在效率：主核的倒排表带摘要或按值排序，能直接定位；这里只能全表扫。这就是「有索引结构」与「索引结构是个普通字典」的差别。

`DocumentSummaryIndex` 的摘要与主核的**物化视图**同构，而两者的差异恰好在「确定性」。主核的物化视图由确定的 SQL 定义，所以重算得到相同结果，可以增删改后增量刷新。摘要由 LLM 生成，重算会得到不同文本，所以它只能整片重建——这也解释了为什么 `delete_ref_doc` 是唯一的删除粒度。

`delete_ref_doc` 里传错 id 空间这件事，在主核里是**不可能发生的**：主核的所有索引都持行标识，而删除 API 只接受行标识。这类错误之所以在这类框架里会发生，是因为「标识符空间」没有在类型上区分——`str` 就是 `str`，向量库 id、节点 id、文档 id 都是字符串。

对 AI 工程的实际意义在于：这两类索引都是「用更小的东西定位」，而选型的关键是**那份更小的东西能否增量维护**。这份实现给出的三条经验可直接用：**存引用就能细粒度删、存生成内容就只能整片删**、**同一标识符空间在多个存储里应显式区分**（否则删除会静默失效），以及**库代码不要写 stdout**（`print` 无法被日志配置管理）。
