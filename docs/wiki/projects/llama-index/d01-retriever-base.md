---
project: llama-index
feature: retriever-base
title: 检索器契约
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/base/base_retriever.py
    symbols: [BaseRetriever]
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [NodeWithScore]
---

# 检索器契约

## 一句话职责

`BaseRetriever` 把「查节点」统一成 `QueryType → List[NodeWithScore]`，并在这一层加了三样东西：**查询串/查询包的归一**、**回调与事件的双重上报**、以及**沿 `IndexNode` 递归进入子索引**。

## 机制是什么

检索器要解决的问题看起来很简单：给定查询，返回节点。但源码把它拆成了三层，因为有三件事不能由每个具体检索器自己处理：

| 层 | 谁负责 | 做什么 |
|---|---|---|
| `retrieve` / `aretrieve` | 基类（公开入口） | 归一化输入、开回调事件、递归展开、去重 |
| `_retrieve` / `_aretrieve` | 子类（覆盖点） | 真正怎么找 |
| `_retrieve_from_object` | 基类（递归辅助） | 把「对象」转成节点列表 |

其中第三件事是这一页最有意思的部分：`IndexNode` 的内容不是文本而是**指向另一个可检索对象的引用**（b02 讲过）。所以检索命中一个 `IndexNode` 时，正确的行为不是「返回它」，而是「**进入它指向的东西继续检索**」。

这就使 `BaseRetriever` 具备了**递归**能力：一个检索器可以调另一个检索器（甚至自己）。而 `object_map` 是把 `index_id` 解析回实际对象的字典——它的来源是 `BaseIndex.__init__` 里那个 `_object_map`（c01 讲过）。

## 怎么实现的

**`retrieve` 的流程有六步，而其中三步是「上报」。**

```python
@dispatcher.span
def retrieve(self, str_or_query_bundle: QueryType) -> List[NodeWithScore]:
    self._check_callback_manager()
    dispatcher.event(RetrievalStartEvent(str_or_query_bundle=str_or_query_bundle))
    if isinstance(str_or_query_bundle, str):
        query_bundle = QueryBundle(str_or_query_bundle)
    else:
        query_bundle = str_or_query_bundle
    with self.callback_manager.as_trace("query"):
        with self.callback_manager.event(
            CBEventType.RETRIEVE,
            payload={EventPayload.QUERY_STR: query_bundle.query_str},
        ) as retrieve_event:
            nodes = self._retrieve(query_bundle)
            nodes = self._handle_recursive_retrieval(query_bundle, nodes)
            retrieve_event.on_end(payload={EventPayload.NODES: nodes})
    dispatcher.event(RetrievalEndEvent(str_or_query_bundle=str_or_query_bundle, nodes=nodes))
    return nodes
```

六步：**检查回调管理器 → 发开始事件 → 归一化查询 → 开 trace → 调 `_retrieve` 并递归展开 → 发结束事件**。

四处：

1. **`_check_callback_manager()` 是一个防御性补丁**：

```python
def _check_callback_manager(self) -> None:
    if not hasattr(self, "callback_manager"):
        self.callback_manager = Settings.callback_manager
```

   即「若子类没调 `super().__init__()`，就用全局的设置」。这意味着**存在子类不调 super 的情况**，而基类选择兼容而不是报错。`hasattr` 检查的是一个必定存在的属性（`__init__` 里赋的），所以这条分支只在「子类覆写了 `__init__` 却没调 super」时生效。

2. **`dispatcher.event(RetrievalStartEvent(...))` 与回调事件是两套并行的机制**。一个给 OpenTelemetry / OpenInference 之类的 instrumentation，一个给 callback handler。两者上报的内容不同（dispatcher 带原始入参与结果，callback 带 `query_str` 与 `NODES`）。

   这带来一个隐患：**两套都失败时的行为不同**。`callback_manager.event` 是上下文管理器，异常会传播；`dispatcher.event` 的实现取决于 instrumentation。源码没有统一这两者的错误策略。

3. **`@dispatcher.span` 装饰器**给出这个方法的 span 范围。所以一次检索在追踪里是一个独立的 span，且它嵌套在调用方（如 query engine）的 span 下。

4. **`retrieve_event.on_end(payload={EventPayload.NODES: nodes})` 是在 `with` 块内显式调用**，而不是靠上下文管理器退出时自动。所以**结束回调是手动的**——若 `_retrieve` 抛错，这个 `on_end` 不会执行（异常传播出去），回调系统看到的是一个没有 end 的事件。这与 `BaseIndex` 里那些手写 try/except/else 不同（那里有 `on_retriever_error`）。

**`_aretrieve` 的默认实现是「同步」而抽象标记被注释掉了。**

```python
# TODO: make this abstract
# @abstractmethod
async def _aretrieve(self, query_bundle: QueryBundle) -> List[NodeWithScore]:
    """Asynchronously retrieve nodes given query.

    Implemented by the user.
    """
    return self._retrieve(query_bundle)
```

三处：

1. **默认实现直接调同步的 `_retrieve`**——在事件循环里就是**阻塞调用**。所以一个只实现了 `_retrieve` 的检索器被 `await retriever.aretrieve(...)` 调用时，会把整个事件循环卡住。
2. **`# TODO: make this abstract` 与 `# @abstractmethod` 位置写反了**——`TODO` 在 `async def` 上方，而 `@abstractmethod` 被注释在下方（实际不生效）。本意大概是「将来会让异步实现也成为必须覆盖的」。
3. **因此「异步」在这个基类上是可选的**：`_retrieve` 必须实现（有 `@abstractmethod`），`_aretrieve` 不必。

这与 c02（langchain 的解析器）里「`ainvoke` 走 `aparse_result` 而非 `aparse`」是同一类问题的另一面：那里是「覆写了错的钩子」，这里是「默认钩子是阻塞的」。

**`_retrieve_from_object` 把四种对象各自转成节点，而它们的语义差别很大。**

```python
if isinstance(obj, NodeWithScore):
    return [obj]
elif isinstance(obj, BaseNode):
    return [NodeWithScore(node=obj, score=score)]
elif isinstance(obj, BaseQueryEngine):
    response = obj.query(query_bundle)
    return [NodeWithScore(node=TextNode(text=str(response), metadata=response.metadata or {}), score=score)]
elif isinstance(obj, BaseRetriever):
    return obj.retrieve(query_bundle)
else:
    raise ValueError(f"Object {obj} is not retrievable.")
```

四处：

1. **`NodeWithScore` 原样返回**——连它自己的分数都不改。
2. **`BaseNode` 包成 `NodeWithScore`，用传入的 `score`**。
3. **`BaseQueryEngine` 被**调用**并合成一个新节点**：

```python
response = obj.query(query_bundle)
return [NodeWithScore(node=TextNode(text=str(response), metadata=response.metadata or {}), score=score)]
```

   即「把一个查询引擎的输出当作一段文本节点」。`str(response)` 是响应对象的字符串形式（对 `Response` 来说通常是正文）。这个转换是**有损**的——响应里的结构化内容（源节点、元数据之外的字段）全部丢失。

   注意 `response.metadata` 被放进节点的 `metadata`。但 `BaseQueryEngine.query` 的返回值在这里假定是 `Response`（有 `.metadata` 属性）——若是别的类型会 `AttributeError`。

4. **`BaseRetriever` 递归调用**，并**丢弃了外层分数**（`obj.retrieve(query_bundle)` 返回的是它自己的分数）。

`_aretrieve_from_object` 与它完全对称，唯一差别是 `obj.query(...)` → `await obj.aquery(...)`、`obj.retrieve(...)` → `await obj.aretrieve(...)`。注意**同步版的 `_retrieve_from_object` 里没有 `print_text` 的 verbose 输出，而 `_handle_recursive_retrieval` 里有**——所以 verbose 的两条消息分布在不同函数里。

**`_handle_recursive_retrieval` 的递归规则，以及一处不易察觉的分数继承。**

```python
for n in nodes:
    node = n.node
    score = n.score or 1.0
    if isinstance(node, IndexNode):
        obj = node.obj or self.object_map.get(node.index_id, None)
        if obj is not None:
            retrieved_nodes.extend(self._retrieve_from_object(obj, query_bundle=query_bundle, score=score))
        else:
            retrieved_nodes.append(n)
    else:
        retrieved_nodes.append(n)
```

四处：

1. **`score = n.score or 1.0`** —— 注意 `or` 而不是 `is None` 判断。所以**分数为 `0.0` 的节点会被当成 1.0**。这是一个真实的 bug：一个「最不相关」（score=0）的节点在递归展开时会把 `1.0` 传给子检索结果作为分数。

2. **`node.obj or self.object_map.get(node.index_id, None)`** —— 先看节点自带的 `obj`（内存里的情况），再看 `object_map`（由 `BaseIndex` 填的那份）。`IndexNode` 的 `obj` 在索引构造时被清空（c01 讲过），所以实际路径**几乎总是走 `object_map`**。

3. **`obj is None` 时把 `IndexNode` 本身留下**。所以「找不到被引用的对象」不会报错，而是**返回一个内容是指针的节点**——下游拿到它只能当文本读（`IndexNode.get_content` 返回 `self.text`，即空串，因为构造时只设了 `index_id`）。这是一个静默的失败。

4. **分数被传递到子结果（`score=score`）**。所以「命中一个 `IndexNode` 后，它展开出来的所有节点都带那个 `IndexNode` 的分数」。而真正的子检索分数会覆盖——除非子对象是 `BaseRetriever`（那条分支直接用子结果自己的分数）。

这两条路径的分数处理不同：

| 子对象的类型 | 结果的分数 |
|---|---|
| `BaseRetriever` | 子检索器给的分数 |
| `BaseNode` | 外层 `IndexNode` 的分数 |
| `BaseQueryEngine` | 外层 `IndexNode` 的分数 |
| `NodeWithScore` | 它自己的分数 |

**去重用的是一行「`seen` 集合 + 赋值表达式」的技巧。**

```python
seen = set()
return [
    n
    for n in retrieved_nodes
    if not (
        n.node.node_id in seen or seen.add(n.node.node_id)  # type: ignore[func-returns-value]
    )
]
```

三处：

1. **`seen.add(...)` 返回 `None`（falsy）**，所以「第一次出现」时 `n.node.node_id in seen` 为假、`seen.add(...)` 为假，整个 `or` 为假，`not (...)` 为真——保留。第二次出现时前者为真，整个表达式为真，被过滤掉。

2. **`# type: ignore[func-returns-value]`** 承认了这个写法的类型问题（`set.add` 返回 `None` 却被用作布尔）。

3. **去重按 `node_id`，保留首次出现的顺序**。所以顺序是「第一次出现的位置」——而递归展开的结果被插在原 `IndexNode` 的位置上。

这个技巧紧凑但可读性差。等价的常规写法是 `if node_id not in seen: seen.add(node_id); keep`。源码选了前者，大概是为了在列表推导里保持单表达式。

**`__init__` 的三个参数决定了这个类能做什么。**

```python
def __init__(self, callback_manager=None, object_map=None, objects=None, verbose=False) -> None:
    self.callback_manager = callback_manager or CallbackManager()
    if objects is not None:
        object_map = {obj.index_id: obj.obj for obj in objects}
    self.object_map = object_map or {}
    self._verbose = verbose
```

三处：

1. **`callback_manager or CallbackManager()`**：默认是一个**新的空管理器**，而不是 `Settings.callback_manager`。所以直接构造一个检索器时不会继承全局回调——**除非子类自己传 `Settings.callback_manager`**（实际各检索器都这么做：`callback_manager=callback_manager or Settings.callback_manager`）。

   这个双重默认有一个后果：`_check_callback_manager()` 里的 fallback 只在「属性完全不存在」时生效，而 `__init__` 已经给了空管理器——所以**空管理器与全局管理器是两个不同状态**，而 `hasattr` 无法区分它们。即「子类调了 super 没传 callback_manager」会得到一个静默的空管理器（不上报任何回调），而「子类没调 super」才拿到全局的。

2. **`objects` 会覆盖 `object_map`**（若两者都给）。所以「传了 objects 就忽略 object_map」——与 `BaseIndex` 里的同名逻辑一样。

3. **`verbose` 存成 `self._verbose`**（下划线），而其它状态都是公开属性（`callback_manager`、`object_map`）。不一致但无害。

`BaseRetriever` 还继承了两个 mixin：`PromptMixin`（提示管理，四个方法里三个是空实现）与 `DispatcherSpanMixin`（span 支持）。所以「检索器可以持有提示」这个能力是**由基类强加的**——即使大多数检索器不需要提示。

三个 PromptMixin 方法是空实现：

```python
def _get_prompts(self) -> PromptDictType: return {}
def _get_prompt_modules(self) -> PromptMixinType: return {}
def _update_prompts(self, prompts: PromptDictType) -> None:  # 连 docstring 都没有
```

第三个方法**只有签名没有函数体**（函数体是空的，返回 `None`）。所以 `_update_prompts` 被调用时什么都不做——静默无操作。

**`NodeWithScore` 在检索侧的三个「静默降级」在 b02 已经讲过，这里只补一处。**

`retrieve` 返回的列表里，分数的类型是 `Optional[float]`。而具体检索器的行为不一致：

- `SummaryIndexRetriever` **完全不设分数**（`NodeWithScore(node=node)`）——所以整个列表的 `score` 都是 `None`。
- `SummaryIndexEmbeddingRetriever` 设相似度。
- `BaseKeywordTableRetriever` 也**不设分数**（`NodeWithScore(node=node)`）——尽管它实际上是按「命中关键词数」排序的，那个计数没有传出来。

所以「按关键词数排序」这个信息**在返回结果里丢失了**——调用方看到的是一串无分数节点，顺序是唯一的排序信息。

而 `BaseKeywordTableRetriever._retrieve` 的排序逻辑本身值得一提：

```python
chunk_indices_count: Dict[str, int] = defaultdict(int)
keywords = [k for k in keywords if k in self._index_struct.keywords]
for k in keywords:
    for node_id in self._index_struct.table[k]:
        chunk_indices_count[node_id] += 1
sorted_chunk_indices = sorted(chunk_indices_count.keys(), key=lambda x: chunk_indices_count[x], reverse=True)
sorted_chunk_indices = sorted_chunk_indices[: self.num_chunks_per_query]
```

三处：

1. **用「命中几个关键词」当排序依据**（`chunk_indices_count`）。这是一个**隐式的打分**——但没有归一化，所以「命中 2 个」与「命中 10 个」的差别在结果里只体现为顺序。

2. **`sorted(...)` 是稳定排序**，但没有次序键（ties 保留字典插入序，即**首次被访问的顺序**）。所以「命中数相同的两个节点」的顺序取决于哪个关键词先遍历到——而关键词来自集合转列表，**顺序不确定**（c04 里讲过 `list(keywords)`）。于是**分数相同的节点顺序不可复现**。

3. **`keywords = [k for k in keywords if k in self._index_struct.keywords]`** 过滤掉不在表里的关键词。这一步是必要的（否则 `self._index_struct.table[k]` 会 `KeyError`），但过滤是静默的——调用方不知道有多少查询关键词被丢掉了。

`DocumentSummaryIndex` 的两个检索器有一个共同的结构：**先在摘要层选，再回到该文档的全部节点**。

`DocumentSummaryIndexLLMRetriever` 的实现分两阶段：

```python
# 阶段一：逐批让 LLM 选
for idx in range(0, len(summary_ids), self._choice_batch_size):
    ...
    raw_choices, relevances = self._parse_choice_select_answer_fn(raw_response, len(summary_nodes))
    choice_idxs = [choice - 1 for choice in raw_choices]
    choice_summary_ids = [summary_ids_batch[ci] for ci in choice_idxs]
    all_summary_ids.extend(choice_summary_ids)
    all_relevances.extend(relevances)

# 阶段二：全局排序取 top k，再展开成该文档的全部节点
zipped_list = list(zip(all_summary_ids, all_relevances))
sorted_list = sorted(zipped_list, key=lambda x: x[1], reverse=True)
top_k_list = sorted_list[: self._choice_top_k]
for summary_id, relevance in top_k_list:
    node_ids = self._index.index_struct.summary_id_to_node_ids[summary_id]
    nodes = self._index.docstore.get_nodes(node_ids)
    results.extend([NodeWithScore(node=n, score=relevance) for n in nodes])
```

四处：

1. **`choice_idxs = [choice - 1 for choice in raw_choices]`**：把 LLM 给的 1-based 序号转成 0-based 索引。**没有边界检查**——若 LLM 输出 `11` 而批次只有 10 个，`summary_ids_batch[10]` 会 `IndexError`。这是一个真实的崩溃点，因为 LLM 很容易编出超范围的序号。

   注意 `SummaryIndexLLMRetriever` 里同一处代码是 `choice_idxs = [int(choice) - 1 for choice in raw_choices]`（多一次 `int()`），而这里直接 `choice - 1`。所以两者假定 `parse_choice_select_answer_fn` 返回的类型不同（前者假定字符串，后者假定已转好）。

2. **`all_summary_ids` 与 `all_relevances` 靠 `extend` 保持同步**。若某批次的 `raw_choices` 与 `relevances` 长度不同，`zip` 会截断——**没有任何校验**。

3. **阶段二是全局排序取 top k**。所以它是「分批选择 + 全局决赛」——不是「每批选 top k」。

4. **展开用的是 `summary_id_to_node_ids[summary_id]`**（c04 的三张映射之一），并且**给该文档的每个节点都打上摘要的相关性分数**。所以同一文档里所有节点的分数相同——这是对的（它们是同一批内容），但也意味着**节点级的排序信息不存在**。

`DocumentSummaryIndexEmbeddingRetriever` 走向量库：

```python
if self._vector_store.is_embedding_query:
    if query_bundle.embedding is None:
        query_bundle.embedding = self._embed_model.get_agg_embedding_from_queries(query_bundle.embedding_strs)

query = VectorStoreQuery(query_embedding=query_bundle.embedding, similarity_top_k=self._similarity_top_k)
query_result = self._vector_store.query(query)

top_k_summary_ids: List[str]
if query_result.ids is not None:
    top_k_summary_ids = query_result.ids
elif query_result.nodes is not None:
    top_k_summary_ids = [n.node_id for n in query_result.nodes]
else:
    raise ValueError("Vector store query result should return at least one of nodes or ids.")
```

三处：

1. **`if self._vector_store.is_embedding_query`** —— 若向量库不是「按嵌入查询」的类型，就不算查询嵌入。那 `query_bundle.embedding` 可能是 `None`，然后 `VectorStoreQuery(query_embedding=None, ...)` 被传给向量库。这**假定那类向量库不需要嵌入**（例如它自己做关键词检索）。这个分支的意义在源码里没有注释。

2. **结果从 `ids` 或 `nodes` 两路取**。所以向量库可以只返回 id、只返回节点、或都返回。`ids` 优先。

3. **最后 `NodeWithScore(node=n)` 不设分数** —— 所以向量相似度**被丢掉了**，尽管向量库把它算出来了。这是一个明显的信息损失：调用方拿到一批按相似度排序的节点，却不知道相似度是多少。

对比 `VectorStoreIndex` 的 `VectorIndexRetriever`（c02 提到但不在本篇范围）：它大概会保留分数。所以**同一份向量库查询结果，不同检索器对分数的处理不同**。

**这个篇目的三个 refs 文件里，四个「legacy 别名」显示了命名演进。**

`indices/list/retrievers.py` 有三个：

```python
ListIndexEmbeddingRetriever = SummaryIndexEmbeddingRetriever
ListIndexLLMRetriever = SummaryIndexLLMRetriever
ListIndexRetriever = SummaryIndexRetriever
```

`indices/document_summary/retrievers.py` 有一个：

```python
DocumentSummaryIndexRetriever = DocumentSummaryIndexLLMRetriever
```

所以「ListIndex」是「SummaryIndex」的旧名（c03 里 `ListIndex = SummaryIndex` 也是这个），而 `DocumentSummaryIndexRetriever`（不带 `LLM`）是 LLM 版的旧名。这些别名同时存在意味着**同一个概念有三套名字**（`SummaryIndex` / `ListIndex` / `GPTListIndex`），而新代码应该用最前者。

## 关键取舍

**源码事实：** `BaseRetriever` 分三层：公开 `retrieve`/`aretrieve`（归一化 + 上报 + 递归 + 去重）、子类覆盖的 `_retrieve`/`_aretrieve`、以及递归辅助 `_retrieve_from_object`。

**源码事实：** `retrieve` 同时上报两套：`dispatcher.event`（`RetrievalStartEvent`/`RetrievalEndEvent`，带原始入参与结果）与 `callback_manager.event`（`CBEventType.RETRIEVE`，带 `query_str` 与 `NODES`）；结束回调是手动调用，异常路径下不会执行，且**没有** error 回调（与 `BaseIndex` 的手写 try/except/else 不同）。

**源码事实：** `_aretrieve` 默认实现直接调同步 `_retrieve`（在事件循环里阻塞），且 `@abstractmethod` 被注释掉、`TODO` 与注释位置互换——异步实现是可选的。

**源码事实：** `_check_callback_manager` 用 `hasattr` 兜底到 `Settings.callback_manager`，但 `__init__` 默认给的是一个**新的空** `CallbackManager()`；所以「子类调了 super 但没传 callback_manager」会得到静默的空管理器，与「没调 super」的兜底结果不同。

**源码事实：** `_retrieve_from_object` 对四种对象分别处理：`NodeWithScore` 原样、`BaseNode` 包一层并带外层分数、`BaseQueryEngine` 调 `query` 后把 `str(response)` 与 `response.metadata` 合成新 `TextNode`（有损）、`BaseRetriever` 递归调用并用自己的分数。

**源码事实：** `_handle_recursive_retrieval` 用 `score = n.score or 1.0` —— `or` 会把分数 `0.0` 变成 `1.0`；`IndexNode` 的 `obj` 已在索引构造时被清空，故实际几乎总走 `object_map`；`obj is None` 时把 `IndexNode` 本身留下（静默失败）。

**源码事实：** 递归结果的分数处理按子对象类型分裂：`BaseRetriever` 用子分数，`BaseNode`/`BaseQueryEngine` 用外层 `IndexNode` 分数，`NodeWithScore` 用它自己的。

**源码事实：** 去重用「`seen` + `seen.add()` 返回 `None`」的单表达式技巧（含 `# type: ignore[func-returns-value]`），按 `node_id` 保留首次出现。

**源码事实：** `SummaryIndexRetriever` 与 `BaseKeywordTableRetriever` 都**不设分数**——前者的排序信息根本不存在，后者的「命中关键词数」这个隐式打分被丢弃；`DocumentSummaryIndexEmbeddingRetriever` 也丢弃向量相似度。

**源码事实：** `BaseKeywordTableRetriever` 用 `defaultdict(int)` 统计命中关键词数并稳定排序，但**无次序键**，故命中数相同的节点顺序取决于关键词的遍历顺序（而关键词来自集合转列表，不确定）。

**源码事实：** 两个 `choice_select` 检索器都把 LLM 给的 1-based 序号转 0-based 后**直接用作索引**，无边界检查；`all_summary_ids` 与 `all_relevances` 靠 `extend` 保持同步但无长度校验。

**源码事实：** `DocumentSummaryIndexEmbeddingRetriever` 只在 `vector_store.is_embedding_query` 时才计算查询嵌入，结果从 `ids` 优先、`nodes` 次之取，两路都没有时抛 `ValueError`。

**源码事实：** `BaseRetriever` 继承 `PromptMixin` 与 `DispatcherSpanMixin`；`_get_prompts`/`_get_prompt_modules` 返回空字典，`_update_prompts` 是空实现（静默无操作）。

**架构推论：** 把「递归展开 `IndexNode`」放在基类而不是让每个子类自己处理，是一个正确的分层决定——它让「索引嵌套」这个能力对所有检索器自动生效。但代价是**基类要知道 `IndexNode`、`BaseQueryEngine`、`BaseRetriever`、`BaseNode`、`NodeWithScore` 五种类型**，从而把这些类型都耦合进了检索器的核心。这解释了为什么 `_retrieve_from_object` 里的分派看起来像一个大杂烩。

**架构推论：** 「`or 1.0` 把 0 分变成 1 分」这条是一个典型的「用真值判断代替存在判断」的错误。分数为 0 是一个**完全合法的最不相关**值，而 `or` 把它与「没有分数」混在一起。正确写法是 `n.score if n.score is not None else 1.0`。这类错误在 Python 里极常见，因为 `0 == False`。

**架构推论：** 「检索器丢弃分数」这件事揭示了这套抽象的一个盲点：`List[NodeWithScore]` 里的 `NodeWithScore` 是**唯一的结果载体**，但很多检索器实际掌握的打分信息（关键词命中数、向量相似度）没有被搬进去。后果是**下游无法按分数做阈值过滤**——它只能相信顺序。更严重的是不同检索器的分数可比性完全不同：有的给余弦相似度、有的给 `None`、有的给常量 1.0。所以「换一个检索器」不是一个透明的操作。

**架构推论：** 序号转索引无边界检查，是「LLM 输出进入程序索引」这类问题的标准失败形态。`parse_choice_select_answer_fn` 的契约大概是「把 LLM 的文本转成数字列表」，但它无法保证范围。正确的做法是在使用索引前 `0 <= ci < len(batch)` 校验——这与 c05 里「实体 id 从名字生成」是同一类问题的不同表现：**凡是从不可信源派生的标识符/索引，都必须在用之前校验范围**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever` | 检索器基类：三层结构与双重上报 |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever.retrieve` / `aretrieve` | 归一化 + trace + `_retrieve` + 递归 + 结束事件 |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever._check_callback_manager` | 子类未调 super 时的兜底 |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever._retrieve_from_object` | 四种可检索对象的转换（引擎输出有损） |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever._handle_recursive_retrieval` | `IndexNode` 展开、`or 1.0` 的分数问题、按 node_id 去重 |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever._aretrieve` | 默认直接调同步版（阻塞），抽象标记被注释 |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexRetriever` | 全量返回，不设分数 |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexEmbeddingRetriever` | 惰性算嵌入（节点无嵌入时现算并写回） |
| `llama-index-core/llama_index/core/indices/list/retrievers.py` | `SummaryIndexLLMRetriever` | 按 `choice_batch_size` 分批让 LLM 选，序号转索引无边界检查 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `BaseKeywordTableRetriever` | 命中关键词数做隐式打分并排序，但分数不传出 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableGPTRetriever` | 用 LLM 抽查询关键词 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableSimpleRetriever` | 正则抽词，零模型成本 |
| `llama-index-core/llama_index/core/indices/keyword_table/retrievers.py` | `KeywordTableRAKERetriever` | RAKE 抽词，零模型成本 |
| `llama-index-core/llama_index/core/indices/document_summary/retrievers.py` | `DocumentSummaryIndexLLMRetriever` | 分批选摘要 + 全局决赛，再展开为文档全部节点 |
| `llama-index-core/llama_index/core/indices/document_summary/retrievers.py` | `DocumentSummaryIndexEmbeddingRetriever` | 摘要向量检索，丢弃相似度分数 |

## 参考

- [llama-index 源码：base/base_retriever.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/base/base_retriever.py)
- [llama-index 源码：indices/keyword_table/retrievers.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/keyword_table/retrievers.py)
- [llama-index 文档：检索器](https://docs.llamaindex.ai/en/stable/module_guides/querying/retriever/)
- [llama-index 文档：低级 API 与检索器组合](https://docs.llamaindex.ai/en/stable/module_guides/querying/retriever/retrievers/)

## 与主核 / AI 的关系

「公开入口 + 子类覆盖点」的三层结构，与主核（多模态数据库内核）的**执行器接口**同构：主核的执行器有统一的 `ExecInit`/`ExecRun`/`ExecEnd` 三段，具体算子只实现自己的节点逻辑，而**回调、资源释放、行计数**这些横切逻辑全在框架侧。本页的 `retrieve` 扮演那个框架层——它保证每次检索都有 trace、都展开嵌套、都去重。

最值得对照的一处是**「递归展开嵌套索引」**。主核里对应的是**子查询与 CTE**：`WITH x AS (...) SELECT ... FROM x` 里，`x` 本身是一个查询，外层查询把它当表用。主核靠**计划树**表达这种嵌套（子计划是父计划的子树），而这里靠**运行时递归**（`object_map` 查对象再调它的 `retrieve`）。计划的好处是嵌套在编译期就确定、可以被优化；运行时的好处是简单，但代价是**嵌套深度不受控**（`_handle_recursive_retrieval` 没有深度限制，自引用会无限递归直到栈溢出）。

「分数被丢弃」这条与主核里**估算值与真实值的区分**有意思地相反。主核会把「这是估算的代价」明确标出来（`EXPLAIN` 里的 `rows=100 (estimated)`），因为下游（规划器）必须知道这个数可不可信。这里则是反过来——很多检索器干脆不给分数，于是「分数是否可信」这个问题消失，但**阈值过滤**这个能力也一起消失了。

「序号转索引无边界检查」在主核里对应的是**参数绑定**：主核接受用户给的下标时会在执行前校验范围（否则报「下标越界」）。判据相同——**来自外部（客户端或 LLM）的索引值必须校验**。

对 AI 工程的实际意义在于：检索器是 RAG 里最容易被误解的一层——它看起来只是「查一批节点」，而实际承担着输入归一、追踪、嵌套展开、去重四件事。这份实现给出的三条经验可直接用：**把嵌套展开放在基类**（否则每个检索器都要重复）、**分数为 0 与没有分数必须分开**（`or` 会把它们混在一起）、以及**来自 LLM 的序号必须校验范围**（否则一次幻觉输出变成一次崩溃）。
