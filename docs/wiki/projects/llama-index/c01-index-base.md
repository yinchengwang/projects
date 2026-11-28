---
project: llama-index
feature: index-base
title: 索引基类与构建流程
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/base.py
    symbols: [BaseIndex]
  - path: llama-index-core/llama_index/core/indices/common/struct_store/base.py
    symbols: [SQLDocumentContextBuilder, BaseStructDatapointExtractor]
---

# 索引基类与构建流程

## 一句话职责

`BaseIndex` 定义所有索引的共同形状：**「节点进、索引结构出」**，并把存储拆成三处（docstore 存原节点、index_store 存索引结构、vector_store/graph_store 存检索索引）；所有具体索引只实现「怎么从节点长出结构」与「怎么增删」。

## 机制是什么

索引层要处理的核心问题是**三份状态的一致性**：

```python
self._docstore = self._storage_context.docstore          # 原始节点
self._index_struct = index_struct                        # 索引结构（如向量 id 列表、关键词表）
self._storage_context.index_store.add_index_struct(...)  # 索引结构的持久化副本
```

这三处不是冗余的——它们各存不同的东西：

| 存储 | 存什么 | 谁在用 |
|---|---|---|
| docstore | 完整节点（含文本、元数据、关系） | 检索到 id 之后取原文 |
| index_struct | 索引的骨架（有序 id、关键词→id 映射、树结构） | 每次检索都读 |
| index_store | index_struct 的可持久化序列化 | 跨进程加载 |
| vector_store / graph_store | 向量 / 图 | 具体索引自己管 |

所以 `BaseIndex` 的职责不是「实现索引」，而是**维护这三者的同步**。这就是为什么它的方法几乎都是「先改 docstore、再调用子类的钩子、最后把 index_struct 写回 index_store」这个三段式。

而「构建」与「增量插入」被刻意分成两条路径（`build_index_from_nodes` vs `insert_nodes`），因为它们对 `index_struct` 的影响不同：构建是**替换**，插入是**追加**。

## 怎么实现的

**构造函数的五条前置校验，每一条都对应一类误用。**

```python
if index_struct is None and nodes is None and objects is None:
    raise ValueError("One of nodes, objects, or index_struct must be provided.")
if index_struct is not None and nodes is not None and len(nodes) >= 1:
    raise ValueError("Only one of nodes or index_struct can be provided.")
if nodes is not None and len(nodes) >= 1 and not isinstance(nodes[0], BaseNode):
    if isinstance(nodes[0], Document):
        raise ValueError("The constructor now takes in a list of Node objects. "
                         "Since you are passing in a list of Document objects, "
                         "please use `from_documents` instead.")
    else:
        raise ValueError("nodes must be a list of Node objects.")
```

三处：

1. **「三者至少给一个」**：node、对象、已有索引结构，哪样都行——这对应三种真实用法（从节点建、从「索引节点」建、加载已有索引）。
2. **「node 与 index_struct 互斥」**：同时给意味着「既想建又想加载」，语义不明。注意条件写的是 `len(nodes) >= 1`——**空的 nodes 列表不算「给了」**。所以在 `index_struct` 存在时传 `nodes=[]` 是合法的（等价于不传）。
3. **一个专门的「你是不是用了旧 API」检查**：传进来的是 `Document` 而不是 `Node` 时，错误消息直接指出正确方法（`from_documents`）。这与 d02（langchain 的 `tool`）里「把修法写进错误」是同一种做法。

注意第三条**只检查 `nodes[0]`**（第一个元素）。所以一个混着 `Node` 与 `Document` 的列表，只要第一个是 `Node` 就能过——后续处理会在 `docstore.add_documents` 阶段出问题。这是一个「抽查式校验」：成本 O(1)，代价是漏检。

**`objects` 那条路径有一处不易察觉的破坏性操作。**

```python
objects = objects or []
self._object_map = {obj.index_id: obj.obj for obj in objects}
for obj in objects:
    obj.obj = None  # clear the object to avoid serialization issues
```

即**把 `IndexNode.obj` 抠出来放进 `self._object_map`，然后把节点上的 `obj` 清空**。注释直说理由：「to avoid serialization issues」——因为 `obj` 可能是不可序列化的任意对象（b02 讲过 `IndexNode` 的序列化单向有损）。

三处后果：

1. **调用方传进来的 `IndexNode` 对象被就地修改**。所以调用方之后再看那些节点，`obj` 已经没了。这是一个**有副作用**的构造函数。
2. **`_object_map` 是内存里的字典**，不随 index_store 持久化。所以索引重建后，`objects` 里的那些引用就彻底丢——只有 `index_id` 还在。
3. **`_object_map` 在整个 `BaseIndex` 上只有赋值、没有读取**（`grep` 里搜不到读点）。所以当前这个字典是「存了但没用」的状态。这大概是为将来的按 `index_id` 找回对象预留的，目前是一条死路径。

**构建与插入都是三段式，但顺序不完全相同。**

```python
def build_index_from_nodes(self, nodes, **build_kwargs) -> IS:
    self._docstore.add_documents(nodes, allow_update=True)
    return self._build_index_from_nodes(nodes, **build_kwargs)
```

构建只做两件事：**先把节点写进 docstore，再让子类长结构**。没有 `index_store` 的写入——因为构造函数在这之后才统一写：

```python
with self._callback_manager.as_trace("index_construction"):
    if index_struct is None:
        nodes = nodes or []
        index_struct = self.build_index_from_nodes(nodes + objects, **kwargs)
    self._index_struct = index_struct
    self._storage_context.index_store.add_index_struct(self._index_struct)
```

注意 `nodes + objects`：两个序列被**拼接后一起**传给构建。所以「索引节点」（`IndexNode`）与普通节点在同一条路径上处理，区别只在于子类怎么解释它们。这也解释了为什么 `IndexNode.obj` 要被清空——它已经在 `_object_map` 里了，构建时只需 `index_id`。

而插入是完整的三段：

```python
def insert_nodes(self, nodes, **insert_kwargs) -> None:
    for node in nodes:
        if isinstance(node, IndexNode):
            try:
                node.dict()
            except ValueError:
                self._object_map[node.index_id] = node.obj
                node.obj = None
    with self._callback_manager.as_trace("insert_nodes"):
        self.docstore.add_documents(nodes, allow_update=True)
        self._insert(nodes, **insert_kwargs)
        self._storage_context.index_store.add_index_struct(self._index_struct)
```

四处：

1. **`node.dict()` 只用来「试一下能不能序列化」**，结果被丢弃。若抛 `ValueError`（`IndexNode._serialize_obj` 在不可序列化时抛的就是它），才把它挪进 `_object_map` 并清空。所以这是一个**「先试后修」**的模式。
2. **`self.docstore`（属性）与 `self._docstore`（字段）两个名字并存**。属性返回同一个对象，所以功能等价——但代码里两种写法混用（构建用 `_docstore`、插入用 `docstore`）。
3. **顺序是「docstore → 子类钩子 → index_store」**。若子类钩子抛错，docstore 里已经有节点了，而 index_store 没更新——**留下一份不一致状态**。没有事务包裹。
4. **`_insert(nodes, **insert_kwargs)` 是子类唯一能改 index_struct 的地方**；写完由基类负责持久化。

**`delete_nodes` 的默认参数 `delete_from_docstore=False` 是一个重要取舍。**

```python
def delete_nodes(self, node_ids, delete_from_docstore=False, **delete_kwargs) -> None:
    for node_id in node_ids:
        self._delete_node(node_id, **delete_kwargs)
        if delete_from_docstore:
            self.docstore.delete_document(node_id, raise_error=False)
    self._storage_context.index_store.add_index_struct(self._index_struct)
```

三处：

- **默认只从索引里删，docstore 里的原文留着**。理由可以从用途推断：删了索引但保留原文，意味着「以后可以重建索引」或「可以换一个索引类型」。代价是**docstore 会积累孤儿节点**。
- **`delete_document(node_id, raise_error=False)`**：找不到也不报错。所以「删一个已被删过的 id」是幂等的。
- **循环里没有错误处理**：某个 `_delete_node` 抛错时，前面已删的不回滚，后面的不执行。与 g01 的「失败也带 delta」形成对照——这里**没有 delta**，调用方不知道删到哪一步。

**`delete_ref_doc` 是「按文档删」的入口，它靠 docstore 的 `ref_doc_info` 找回该文档的所有节点。**

```python
ref_doc_info = self.docstore.get_ref_doc_info(ref_doc_id)
if ref_doc_info is None:
    logger.warning(f"ref_doc_id {ref_doc_id} not found, nothing deleted.")
    return
self.delete_nodes(ref_doc_info.node_ids, delete_from_docstore=delete_from_docstore, **delete_kwargs)
if delete_from_docstore:
    self.docstore.delete_ref_doc(ref_doc_id, raise_error=False)
```

两处：

1. **找不到就 `warning` + 返回**，不报错。所以「删一个不存在的文档」是成功的。这是一个有意选择的宽松——重复删除不该失败。
2. **`delete_from_docstore` 要传两遍**：一次给 `delete_nodes`（逐节点删），一次给这里（删文档级记录）。只传一次会留下不一致（文档记录还在但节点全没了，或反之）。这处重复是「两级存储」的必然代价。

**`update_ref_doc` 的做法是「删了再插入」，而它的注释把这句话写得很直白。**

```python
"""This is equivalent to deleting the document and then inserting it again."""
```

实现确实如此：

```python
self.delete_ref_doc(document.id_, delete_from_docstore=True, **update_kwargs.pop("delete_kwargs", {}))
self.insert(document, **update_kwargs.pop("insert_kwargs", {}))
```

四处：

1. **`delete_from_docstore=True` 硬编码**。所以更新必然同时删 docstore——这是正确的（旧内容必须清掉），但它让调用方无法选择「保留旧节点作为历史」。
2. **`update_kwargs.pop("delete_kwargs", {})` 与 `pop("insert_kwargs", {})`**：用一个扁平字典装两组嵌套参数，通过 `pop` 分流。这样调用方写 `index.update_ref_doc(doc, delete_kwargs={...}, insert_kwargs={...})`。
   注意 `pop` 会**修改调用方的字典**（若传的是同一个对象引用）——但 Python 的 `**update_kwargs` 在函数入口已经构造了新字典，所以实际安全。
3. **两步之间没有事务**。`delete_ref_doc` 成功后 `insert` 失败，文档就彻底消失了（节点与 docstore 记录都没了）。这是一个真实的失败模式：**「更新」比「不更新」更危险**。
4. **`insert` 里会重新跑 `run_transformations`**，所以更新会重新切分文档并重新调用嵌入模型——这正是 `refresh_ref_docs` 想省的开销（见下）。

**`refresh_ref_docs` 是「只更新变了的」，而它的判据是 docstore 里的 hash。**

```python
refreshed_documents = [False] * len(documents)
for i, document in enumerate(documents):
    existing_doc_hash = self._docstore.get_document_hash(document.id_)
    if existing_doc_hash is None:
        self.insert(document, **update_kwargs.get("insert_kwargs", {}))
        refreshed_documents[i] = True
    elif existing_doc_hash != document.hash:
        self.update_ref_doc(document, **update_kwargs.get("update_kwargs", {}))
        refreshed_documents[i] = True
return refreshed_documents
```

四处：

1. **返回 `List[bool]`**，与输入按位置对应。所以调用方能精确知道哪些被处理了。这个返回类型比「返回计数」更有用（能重建调用方的状态）。
2. **`existing_doc_hash is None` → 插入**（新文档）；**hash 不等 → 更新**；**hash 相等 → 什么都不做且返回 `False`**。这是那次「省 LLM 调用」的全部机制。
3. **hash 的来源是 `document.hash`**，而 `Document.hash` 走 `Node.hash`（b01 讲过：`get_metadata_str(MetadataMode.ALL)` + 四个 resource hash）。所以**元数据变化也会触发更新**——包括那些只用于过滤的键。
4. **这里用的是 `update_kwargs.get("update_kwargs", {})` 而上面 `update()` 用的是 `pop`**。两处对同一个嵌套参数的取法不同（`get` 不消费、`pop` 消费）。行为上等价（因为各自拿到的是新字典），但风格不一致。

**`docstore.set_document_hash` 在两处被调用，这是 `refresh` 能工作的前提。**

`from_documents` 里：

```python
for doc in documents:
    docstore.set_document_hash(doc.id_, doc.hash)
```

`insert` 里：

```python
self.insert_nodes(nodes, **insert_kwargs)
self.docstore.set_document_hash(document.id_, document.hash)
```

注意 `insert` 里这一句**在 `insert_nodes` 之后**。所以若 `insert_nodes` 抛错，hash 不会被记——下次 `refresh` 会把这份文档当成新的再插一遍。

同时 **`from_documents` 里的记录发生在 `run_transformations` 之前**。所以若切分失败，hash 已经记了——下次 `refresh` 会认为「这份文档没变」而跳过它。这是一个**顺序造成的持久不一致**：hash 记录了「见过」，而节点并没有真的进索引。

**`ref_doc_info` 是抽象属性，它的存在说明「按文档聚合节点」是索引的必备能力。**

```python
@property
@abstractmethod
def ref_doc_info(self) -> Dict[str, RefDocInfo]:
    """Retrieve a dict mapping of ingested documents and their nodes+metadata."""
```

`delete_ref_doc` 与 `refresh` 都依赖它（前者靠 `docstore.get_ref_doc_info` 而不是这个属性，但语义相同）。所以每个具体索引都必须能回答「这份文档切出了哪些节点」——而这恰好是 `b01` 的 `NodeRelationship.SOURCE` 关系的用途。

**`as_retriever` 是抽象，而 `as_query_engine` / `as_chat_engine` 是基类给的默认实现。**

`as_query_engine`：

```python
retriever = self.as_retriever(**kwargs)
llm = resolve_llm(llm, callback_manager=self._callback_manager) if llm else Settings.llm
return RetrieverQueryEngine.from_args(retriever, llm=llm, **kwargs)
```

**同一个 `**kwargs` 被传了两次**——一次给 `as_retriever`，一次给 `from_args`。所以 kwargs 里的键必须能被两方都接受，或者各自忽略不认识的。这是一个**共享参数空间**的设计：`similarity_top_k` 归 retriever、`response_mode` 归 query engine，但它们走同一个字典。

`as_chat_engine` 按 `ChatMode` 分派到四种引擎，而 `ChatMode.REACT`/`OPENAI` **直接抛错**：

```python
if chat_mode in [ChatMode.REACT, ChatMode.OPENAI]:
    raise ValueError("ChatMode.REACT and ChatMode.OPENAI are now deprecated and removed. "
                     "Please use the ReActAgent or FunctionAgent classes from "
                     "llama_index.core.agent.workflow to create an agent with a query engine tool.")
```

这是**「已移除」而不是「已废弃」**——配置里若还写着 `react`，这里会炸。错误信息给出了替代方案（`ReActAgent`/`FunctionAgent`），是「把修法写进错误」的又一实例。注意文档字符串里仍列着这两个模式（「ChatMode.REACT: Chat engine that uses a react agent...」）——**文档与行为的又一处不同步**。

`ChatMode.BEST` 与 `ChatMode.CONDENSE_PLUS_CONTEXT` **走同一个分支**：

```python
elif chat_mode in [ChatMode.CONDENSE_PLUS_CONTEXT, ChatMode.BEST]:
    return CondensePlusContextChatEngine.from_defaults(retriever=self.as_retriever(**kwargs), ...)
```

所以所谓「BEST」在当前实现里等于「CondensePlusContext」。而文档说 BEST 是「uses an agent (react or openai) with a query engine tool」——即**文档描述的是旧行为**。这是一个真实的不一致：名字保留了「最佳」的承诺，实现已经换成了固定的一种。

**`SQLDocumentContextBuilder` 与 `BaseStructDatapointExtractor` 是「结构化索引」的两半，而它们的方向相反。**

前者是**文本 → 上下文**（读）：从一个表的所有节点里生成一段「这个表大概是关于什么」的描述，用于给 SQL 生成提示。

```python
for table_name in self._sql_database.get_usable_table_names():
    context_dict[table_name] = self.build_table_context_from_documents(documents_dict[table_name], table_name)
```

单表的实现走了一条完整链路：取 schema → 把它 partial 进提示 → 用 `prompt_helper` 推出切分器 → 切分（发 `CBEventType.CHUNKING` 事件）→ 用 `ResponseSynthesizer` 生成一段描述。

四处：

- **切分器的来源是「按提示预算反推」**（`get_text_splitter_given_prompt(prompt_with_schema, llm=self._llm)`），而不是用户给的固定大小。所以「schema 越长，块越小」——这是合理的：schema 占了预算，内容只能让位。
- **`prompt_with_schema.metadata["prompt_type"] = PromptType.QUESTION_ANSWER`**：给提示打上类型标记，供追踪/缓存区分。refine 提示则标 `REFINE`。
- **`get_response_synthesizer(..., text_qa_template=..., refine_template=...)`**：用两套提示，即「逐块回答 + 逐步精炼」的合成策略。
- **`cast(str, table_context)`**：`get_response` 的返回类型不确定，这里用 `cast` 断言成 `str`。运行期不校验——若真是别的类型，错误会在下游出现。

后者是**文本 → 结构化数据**（写）：调用 LLM 从非结构化文本里抽字段。

```python
for i, text_chunk in enumerate(text_chunks):
    response_str = self._llm.predict(self._schema_extract_prompt, text=text_chunk, schema=schema_text)
    cur_fields = self._output_parser(response_str)
    if cur_fields is None:
        continue
    new_cur_fields = self._clean_and_validate_fields(cur_fields)
    fields.update(new_cur_fields)
struct_datapoint = StructDatapoint(fields)
```

四处：

1. **逐块抽取、结果累积**（`fields.update`）。所以**后面的块会覆盖前面的块**里同名字段。这个顺序即优先级——但源码没说明为什么是后者优先。对「一份文档里同一个字段出现多次」的情形，最后一次出现赢了，这不一定是对的。
2. **`cur_fields is None` 就跳过**（解析器判为不可解析），不报错。所以「LLM 输出不可解析」是静默跳过。
3. **`_clean_and_validate_fields` 是一个「字段过滤 + 类型强转」的组合**，见下。
4. **`struct_datapoint is not None` 恒为真**（`StructDatapoint(fields)` 不会返回 `None`）。所以这一行是**冗余检查**，它给了代码一种「可能为空」的假象。

`_clean_and_validate_fields` 是这一页里逻辑最密的一段：

```python
for field, value in fields.items():
    clean_value = value
    if field not in col_types_map:
        continue
    expected_type = col_types_map[field]
    if expected_type is int:
        try:
            clean_value = int(value)
        except ValueError:
            continue
    elif expected_type is float:
        try:
            clean_value = float(value)
        except ValueError:
            continue
    else:
        if len(value) == 0:
            continue
        if not isinstance(value, col_types_map[field]):
            continue
    new_fields[field] = clean_value
```

四处：

- **不在 schema 里的字段直接丢弃**。所以 LLM 抽出的多余字段被静默丢掉——这是防止注入未知列的必要一步。
- **`int`/`float` 尝试强转，失败就丢字段**。注意 `int("abc")` 抛 `ValueError` 被捕，但 `int(None)` 抛的是 `TypeError`——**没被捕获**。所以一个 `null` 值会让整个抽取过程崩。这是一个真实的缺陷：LLM 很容易输出 `null`。
- **其它类型要求 `len(value) == 0` 判断在前**。这一行对非字符串值（如数字）会抛 `TypeError`（`len(5)`）。所以在「schema 声明的是 `str` 但 LLM 给了数字」这种情形下，**在到达 `isinstance` 检查之前就崩了**。顺序是对的坏事：先判空再判类型，但判空本身要求可 `len`。
- **`expected_type is int` 用身份比较**。对内置类型有效，但对 `Optional[int]` 这类复合注解完全失效（它既不是 `int` 也不是 `float`，会落进 `else` 分支，然后 `len(value)` 或 `isinstance` 失败）。所以这套清洗只支持「纯基础类型」的 schema。

这段代码的总体性格是**「静默丢弃」**：任何不符合的字段都被 `continue` 掉，不警告、不计数。所以抽取质量下降时，上层看到的只是「字段少了」，没有原因。

## 关键取舍

**源码事实：** `BaseIndex` 维护三处状态（docstore 原文、内存 `index_struct`、index_store 持久化副本），以及可选的 vector_store/graph_store；每条写入路径都是「docstore → 子类钩子 → index_store」三段式，**无事务**。

**源码事实：** 构造函数要求「nodes / objects / index_struct 至少给一个」，且 `nodes` 与 `index_struct` 互斥（判据是 `len(nodes) >= 1`，空列表不算给了）；传 `Document` 时报错并指引改用 `from_documents`。

**源码事实：** 构造时把每个 `IndexNode.obj` 挪进 `_object_map` 并把节点上的 `obj` 置 `None`——**就地修改调用方传入的对象**；而 `_object_map` 只有写入没有读取，是当前未使用的路径。

**源码事实：** `insert_nodes` 用 `node.dict()` 试探可序列化性，抛 `ValueError` 才把 `obj` 挪走；顺序是 docstore 先写、子类钩子次之、index_store 最后，钩子抛错会留下不一致状态。

**源码事实：** `delete_nodes` 默认 `delete_from_docstore=False`（只删索引、留原文），docstore 删除用 `raise_error=False`（幂等）；循环内无回滚，也没有 delta 报告删到哪一步。

**源码事实：** `delete_ref_doc` 找不到 `ref_doc_id` 时只打 `warning` 并返回（重复删除不失败）；`delete_from_docstore` 需要同时传给 `delete_nodes` 与文档级删除两处。

**源码事实：** `update_ref_doc` 就是「删了再插」，且 `delete_from_docstore=True` 硬编码；两步之间无事务，删除成功后插入失败会导致文档彻底消失。`insert` 内部会重跑 `run_transformations`（重新切分 + 重新嵌入）。

**源码事实：** `refresh_ref_docs` 靠 docstore 里的 `document.hash` 判定「新文档 / 变了 / 没变」，返回与输入位置对齐的 `List[bool]`；`document.hash` 含元数据，故元数据变化也会触发重建。

**源码事实：** `from_documents` 在 `run_transformations` **之前**记录 hash，`insert` 在 `insert_nodes` **之后**记录 hash —— 两条路径的记录时机不同，失败时的后果也不同。

**源码事实：** `as_query_engine` 把同一个 `**kwargs` 同时传给 `as_retriever` 与 `RetrieverQueryEngine.from_args`；`as_chat_engine` 对 `ChatMode.REACT`/`OPENAI` 直接抛错并给出 `ReActAgent`/`FunctionAgent` 替代，而 docstring 仍在描述这两个模式；`ChatMode.BEST` 与 `CONDENSE_PLUS_CONTEXT` 走同一分支。

**源码事实：** `SQLDocumentContextBuilder` 的切分器由「提示预算反推」得出（schema 越长块越小），并在 `CBEventType.CHUNKING` 事件里上报；`BaseStructDatapointExtractor` 逐块抽取并 `fields.update` 累积，后者覆盖前者。

**源码事实：** `_clean_and_validate_fields` 丢弃不在 schema 的字段、对 int/float 试强转失败即丢；但 `int(None)` 抛 `TypeError` 未被捕获，且非 int/float 分支先做 `len(value)` 会对数字抛 `TypeError`——两处都可能让整个抽取崩掉。

**架构推论：** 「三处状态 + 三段式 + 无事务」这套结构决定了**失败的形态**：任何一段失败都会留下部分更新的状态，而调用方无法知道到了哪一步。这与 g01（langchain 的索引）里「失败也返回 delta」形成鲜明对比——那里把「已发生什么」当作一等输出，这里什么都没有。差别在于责任归属：g01 的调用方是用户脚本（需要精确恢复），这里的调用方通常是「一次性构建」的流程（失败就重建）。

**架构推论：** 拆出「构建」与「插入」两条路径，实质是承认 `index_struct` 有两种变更语义：替换与追加。用同一套接口做两件事会让子类必须区分「第一次构建」与「后续插入」，而那正是最容易出 bug 的地方。分开之后，子类只需保证 `_build_index_from_nodes` 处理空输入时正确（因为 `nodes + objects` 可能为空）。

**架构推论：** `update_ref_doc` 的「删了再插」把「更新」降级成两个不安全操作的组合，而 `refresh_ref_docs` 的存在说明**这个降级是可以被规避的**——若 hash 没变就完全不碰。所以真正的问题不是「更新不安全」，而是「在 hash 变了的时候没有更细粒度的更新」。一个文本改动只影响某几个块的场景，这里会重建整份文档的全部块并重跑嵌入。这是成本上的真实浪费，也是「按块增量更新」这类优化要解决的。

**架构推论：** `_clean_and_validate_fields` 那两处 `TypeError` 暴露了一个更普遍的问题：**用 `is`/`len`/`isinstance` 这类基础操作处理「可能为 `None` 或类型不定」的值时，必须先答「这个值存在吗」**。代码隐含假设了「字段存在就有值且是字符串」，而 LLM 的输出恰恰经常破坏这个假设。这类缺陷在测试里很容易漏，因为测试通常用「标准形状」的 LLM 输出。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex` | 索引基类：三处状态 + 构建/增删改/查询接口 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.__init__` | 前置校验、`_object_map` 提取、构建并写 index_store |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.from_documents` | 文档 → 变换 → 构造；hash 记录在变换之前 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.build_index_from_nodes` | 只写 docstore 再调子类钩子（index_store 由构造器统一写） |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.insert_nodes` | 三段式插入 + `IndexNode` 的可序列化试探 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.delete_nodes` | 默认不删 docstore，删除幂等，无部分失败报告 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.delete_ref_doc` | 按 `ref_doc_info` 找节点，找不到只警告 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.update_ref_doc` | 「删了再插」的两步组合，硬编码删 docstore |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.refresh_ref_docs` | 按 hash 三分类，返回位置对齐的布尔列表 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.as_query_engine` | kwargs 在 retriever 与 query engine 间共享 |
| `llama-index-core/llama_index/core/indices/base.py` | `BaseIndex.as_chat_engine` | 按 `ChatMode` 分派，REACT/OPENAI 已移除，BEST 等同 CONDENSE_PLUS_CONTEXT |
| `llama-index-core/llama_index/core/indices/common/struct_store/base.py` | `SQLDocumentContextBuilder` | 表节点 → 表描述（读方向），切分器按提示预算反推 |
| `llama-index-core/llama_index/core/indices/common/struct_store/base.py` | `BaseStructDatapointExtractor` | 文本 → 结构化字段（写方向），逐块抽取并累积覆盖 |
| `llama-index-core/llama_index/core/indices/common/struct_store/base.py` | `_clean_and_validate_fields` | 字段过滤 + 数值强转，静默丢弃且两处可能抛 `TypeError` |

## 参考

- [llama-index 源码：indices/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/base.py)
- [llama-index 文档：索引](https://docs.llamaindex.ai/en/stable/module_guides/indexing/)
- [llama-index 文档：文档管理与刷新](https://docs.llamaindex.ai/en/stable/module_guides/indexing/document_management/)
- [llama-index 源码：struct_store/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/common/struct_store/base.py)

## 与主核 / AI 的关系

`BaseIndex` 的三处状态与主核（多模态数据库内核）的**表 / 索引 / 元组**三层完全同构：

| 概念 | llama-index | 主核 |
|---|---|---|
| 原始内容 | docstore 里的 `Node` | 堆文件里的元组 |
| 索引结构 | `index_struct`（内存） | B-tree 的页面 |
| 索引持久化 | `index_store` | WAL 与索引页落盘 |
| 检索外存 | `vector_store` / `graph_store` | 向量索引 / 图索引扩展 |

对照价值最大的一处是**「无事务」这个差别**。主核的插入与删除都在事务里：要么全成要么全回滚，崩溃后靠 WAL 恢复到某个一致点。`BaseIndex` 的三段式恰好相反——每段独立生效，失败就停在那里。这不是疏忽，而是权衡：向量库与文档库往往在不同进程/不同服务后面，跨它们做事务代价极高（需要两阶段提交）。所以这里的实际策略是「**接受不一致，靠重建修复**」——这与 g01 里 langchain 靠「上一轮记录」修复是同一族思路。

`refresh_ref_docs` 的 hash 判据与主核的**行版本比较**同理：主核靠 `xmin`/`ctid` 判断一行是否是我们上次读到的那个版本；这里靠 `document.hash`。差别在于主核的版本是**引擎维护的、不可伪造的**，而 `document.hash` 是**框架算出来的、把元数据也算进去**。后者的问题在 b01 已经说清（改元数据就换身份），它在 refresh 上的具体后果是：**改一个只影响过滤的元数据键会触发整份文档重新切分并重新嵌入**。

`_clean_and_validate_fields` 的静默丢弃，与主核的**类型转换失败策略**形成对照。主核在 `INSERT` 时遇到类型不匹配会报错（除非显式用 `CAST`），因为它必须保证「入库的数据是声明类型的」。这里选择丢弃，因为数据来源是 LLM——报错会让整条流水线停在一个不可控的输入上。**判据是「错误能否被修复」**：主核的输入由程序构造（能改），LLM 的输出不可控（只能过滤）。

对 AI 工程的实际意义在于：索引层是 RAG 里唯一需要同时维护多份状态的层，它的失败模式决定了运维成本。这份实现给出的三条经验可直接用：**构建与增量插入要分两条路径**（否则子类必须自己区分「首次」与「后续」）、**删除的默认语义要考虑「保留原文以便重建」**（默认不删 docstore 就是这个理由）、以及**字段清洗必须先处理缺失与类型不定**（LLM 输出里的 `null` 与数字会让「先判空再判类型」的顺序直接崩掉）。
