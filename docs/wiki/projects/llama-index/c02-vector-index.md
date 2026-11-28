---
project: llama-index
feature: vector-index
title: 向量索引
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/vector_store/base.py
    symbols: [VectorStoreIndex]
  - path: llama-index-core/llama_index/core/indices/vector_store/base.py
    symbols: [VectorStoreIndex]
---

# 向量索引

## 一句话职责

`VectorStoreIndex` 把节点算成向量、写进向量库，并把「向量库 id ↔ 节点 id」的映射记进 `IndexDict`；它的所有分支判断都围绕一个问题展开：**向量库自己存不存文本**。

## 机制是什么

向量索引看似只是「算嵌入 + 插入 + 检索」，而实际复杂度集中在一个能力差异上：

| 向量库能力 | `stores_text = True` | `stores_text = False` |
|---|---|---|
| 原文存哪 | 向量库自己带着 | 必须由 docstore 存 |
| `IndexDict` 需要记什么 | 只记图像/索引节点 | 记全部节点 |
| `ref_doc_info` 能否工作 | **抛 `NotImplementedError`** | 正常 |
| 删除时清理 docstore | 无意义 | 必需 |

所以这个类里几乎每个方法都有一句 `if not self._vector_store.stores_text or self._store_nodes_override:`。`store_nodes_override` 是**给 `stores_text=True` 的库强行开启「也存节点」的开关**——它存在的理由是可以让 `ref_doc_info` 这类依赖 docstore 的功能在「文本在向量库」的部署里也能用。

这意味着同一个类实际上承载了**两种不同的部署形态**，而它们走的是同一份代码的不同分支。理解这一点之后，那些重复的 if 就不再是冗余，而是一个二分结构的两侧。

## 怎么实现的

**`__init__` 多出四个字段，其中 `insert_batch_size` 默认 2048。**

```python
index_struct_cls = IndexDict

def __init__(self, nodes=None, *, use_async=False, store_nodes_override=False,
             embed_model=None, insert_batch_size=2048, ...):
    self._use_async = use_async
    self._store_nodes_override = store_nodes_override
    self._embed_model = resolve_embed_model(embed_model or Settings.embed_model, callback_manager=callback_manager)
    self._insert_batch_size = insert_batch_size
    super().__init__(...)
```

四处：

1. **`index_struct_cls = IndexDict`** 是类的**类属性**赋值，而 `BaseIndex` 里它标注为 `index_struct_cls: Type[IS]`。所以 `_build_index_from_nodes` 里 `self.index_struct_cls()` 能实例化出正确的结构类型——这是「子类只改一个属性就能换索引结构」的最小实现。
2. **`use_async` 决定构建时走同步还是异步路径**（见下）。它是一个**构建期开关**，不是「方法是否 async」的区别。
3. **`store_nodes_override` 是那个二分结构的第三个取值**：无视 `stores_text` 一律存节点。
4. **`insert_batch_size=2048`** 与 `_add_nodes_to_index` 里的 `iter_batch(nodes, self._insert_batch_size)` 配合。2048 这个数量级说明批处理的目的是**摊薄嵌入调用与向量库写入的固定开销**，而不是内存控制。

**`from_vector_store` 是「接上已有向量库」的入口，而它拒绝一类库。**

```python
if not vector_store.stores_text:
    raise ValueError("Cannot initialize from a vector store that does not store text.")
kwargs.pop("storage_context", None)
storage_context = StorageContext.from_defaults(vector_store=vector_store)
return cls(nodes=[], embed_model=embed_model, storage_context=storage_context, **kwargs)
```

三处：

1. **不存文本的向量库被直接拒绝**。理由很实在：`from_vector_store` 的语义是「接管一个已有数据库」，而若那个库里只有向量没有原文，接管之后**检索到的 id 无法解析成任何文本**——没有 docstore 可以提供原文。所以这个入口必然要求 `stores_text=True`。
2. **`kwargs.pop("storage_context", None)` 把调用方传的 `storage_context` 丢掉，换成自己造的**。所以 `from_vector_store(vs, storage_context=...)` 里的那个参数是被**静默忽略**的。这是一个防误用与静默的组合：防的是「用户以为能指定 docstore」，静默的是这个参数明明在签名里（通过 `**kwargs`）。
3. **`nodes=[]` 而非 `None`**。这与 c01 讲的「空列表不算给了」配合——于是在 `index_struct` 缺失时，`BaseIndex.__init__` 会用空节点构建一个新的空索引。注意 `nodes=[]` 与 `nodes=None` 在 `BaseIndex` 的校验里语义不同（前者让「必须给一个」的检查通过），这里必须用 `[]`。

**`as_retriever` 把三样东西传给检索器。**

```python
return VectorIndexRetriever(
    self,
    node_ids=list(self.index_struct.nodes_dict.values()),
    callback_manager=self._callback_manager,
    object_map=self._object_map,
    **kwargs,
)
```

三处：

- **`node_ids` 是 `nodes_dict.values()`**——即**节点 id 而不是向量库 id**。所以检索器拿到的是一份「这个索引里有这些节点」的清单，而它做什么用取决于检索器实现（通常用于空索引判断或过滤）。
- **`object_map=self._object_map`** 第一次被消费。c01 里说过 `_object_map` 只有写入没有读取——读点在这里。它让检索器能把 `IndexNode` 的 `index_id` 找回对应的对象。
- **`list(...)` 把字典值物化成列表**。这一步是必要的：`nodes_dict` 在索引增长时会变，而传一个快照能避免检索期间的并发修改问题（同时也带来「拿到的是一份旧快照」的代价）。

**嵌入的计算方式是「批量算 + 逐节点复制」。**

```python
def _get_node_with_embedding(self, nodes, show_progress=False) -> List[BaseNode]:
    id_to_embed_map = embed_nodes(nodes, self._embed_model, show_progress=show_progress)
    results = []
    for node in nodes:
        embedding = id_to_embed_map[node.node_id]
        result = node.model_copy()
        result.embedding = embedding
        results.append(result)
    return results
```

四处：

1. **`embed_nodes` 返回的是 id → 向量的字典**，而不是「与输入同序的列表」。所以要按键查回。
2. **`id_to_embed_map[node.node_id]` 用方括号**——缺键就 `KeyError`。这里假定 `embed_nodes` 一定为每个节点都产出向量。这是一个强假设：若某个节点因内容为空被跳过，这里就会崩。而 `build_index_from_nodes` 里确实过滤了空内容节点（见下），但 `insert` 路径没有过滤——所以**通过 insert 添加一个空内容节点会在嵌入后这里崩**（取决于 `embed_nodes` 的实现是否跳过空内容）。
3. **`node.model_copy()` 复制节点再改 `embedding`**。所以**传给向量库的是副本，原节点不变**。这避免了「嵌入被写到 docstore 的节点上」——同时也意味着调用方手里的节点仍然没有 `embedding`。
4. **`result.embedding = embedding`** 是把向量塞进节点字段，而不是作为单独的数组传给向量库。所以向量库接口期望的是「带 `embedding` 的节点列表」。

**`_add_nodes_to_index` 是整个类的核心，而它的两个分支各自做了一件精细的事。**

```python
for nodes_batch in iter_batch(nodes, self._insert_batch_size):
    nodes_batch = self._get_node_with_embedding(nodes_batch, show_progress)
    new_ids = self._vector_store.add(nodes_batch, **insert_kwargs)

    if not self._vector_store.stores_text or self._store_nodes_override:
        for node, new_id in zip(nodes_batch, new_ids):
            node_without_embedding = node.model_copy()
            node_without_embedding.embedding = None
            index_struct.add_node(node_without_embedding, text_id=new_id)
            self._docstore.add_documents([node_without_embedding], allow_update=True)
    else:
        for node, new_id in zip(nodes_batch, new_ids):
            if isinstance(node, (ImageNode, IndexNode)):
                node_without_embedding = node.model_copy()
                node_without_embedding.embedding = None
                index_struct.add_node(node_without_embedding, text_id=new_id)
                self._docstore.add_documents([node_without_embedding], allow_update=True)
```

五处：

1. **批处理的边界在「嵌入 + 写入」这一对操作上**，而不是分别批处理。所以每一批都是「算这批的向量 → 写这批」，失败时前面的批已经生效。
2. **`new_ids = self._vector_store.add(...)` 返回 id 列表**，与 `nodes_batch` 用 `zip` 配对。**依赖「返回顺序与输入一致」**——这是向量库接口的隐含契约，源码没有校验 `len(new_ids) == len(nodes_batch)`。若某个后端少返回一个 id，`zip` 会**静默截断**，于是部分节点不会被记进 `IndexDict`。
3. **`node_without_embedding = node.model_copy(); node_without_embedding.embedding = None`**：注释说「remove embedding from node to avoid duplication」。所以 docstore 里存的节点**不带向量**——因为向量已经在向量库里，重复存一份会让 docstore 体积翻倍。
4. **`index_struct.add_node(node, text_id=new_id)`** 记的是「向量库 id → 节点 id」的映射。注意 `IndexDict.add_node` 的实现是 `self.nodes_dict[vector_id] = node.node_id`——所以**键是向量库 id、值是节点 id**。这个方向决定了「按向量命中后怎么找节点」。
5. **第二个分支只记 `ImageNode` 与 `IndexNode`**。理由在注释里：「if the vector store keeps text, we only need to add image and index nodes」。即文本节点在向量库里已自足（文本在那边），但**图像节点与索引节点的内容不是纯文本**，向量库装不下（或装不全），所以要额外存进 docstore。

   这个判断是**按类型而非按内容**的。一个 `TextNode` 若带了非文本的多模态内容（在 `Node` 体系下可能），在这里不会被额外存——因为 `isinstance(node, (ImageNode, IndexNode))` 用的是旧类型。

**`build_index_from_nodes` 被覆写，而覆写的唯一目的是过滤空内容。**

```python
content_nodes = [node for node in nodes if node.get_content(metadata_mode=MetadataMode.EMBED) != ""]
if len(content_nodes) != len(nodes):
    print("Some nodes are missing content, skipping them...")
return self._build_index_from_nodes(content_nodes, **insert_kwargs)
```

四处：

1. **判据是 `get_content(metadata_mode=MetadataMode.EMBED) != ""`**。用 `EMBED` 模式意味着判据是**参与嵌入的那份文本**——所以一个节点若正文非空但被 `excluded_embed_metadata_keys` 排除后仍非空就算有内容；反之若正文为空且元数据全被排除，它就是空。
2. **跳过而不是报错**。这是合理的选择：空节点无法产生有意义的向量。
3. **提示用 `print` 而不是 `logging`**。这与 a02 里 `print` 警告、b01 里 `logging.warning` 并列——同一个项目里三种提示方式并存。对库代码而言 `print` 是侵入性的（无法被日志配置过滤）。
4. **`len(content_nodes) != len(nodes)` 判「有跳过」**，但没有说明**跳过几个**。用户只看到「有一些被跳过了」。

注意这条过滤**只在 `build` 路径上**。`insert_nodes` 覆写里没有这段过滤——所以通过 `insert` 添加空节点不会被拦（与上面 `_get_node_with_embedding` 的 `KeyError` 风险相连）。这是两条路径行为不一致的一处实例。

**`insert_nodes` 的两个覆写各自跳过了父类的一部分逻辑。**

同步版：

```python
def insert_nodes(self, nodes, **insert_kwargs) -> None:
    self._validate_serializable(nodes)
    with self._callback_manager.as_trace("insert_nodes"):
        self._insert(nodes, **insert_kwargs)
        self._storage_context.index_store.add_index_struct(self._index_struct)
```

异步版：

```python
async def ainsert_nodes(self, nodes, **insert_kwargs) -> None:
    self._validate_serializable(nodes)
    with self._callback_manager.as_trace("insert_nodes"):
        await self._async_add_nodes_to_index(self._index_struct, nodes, **insert_kwargs)
        self._storage_context.index_store.add_index_struct(self._index_struct)
```

**两者都没有 `self.docstore.add_documents(nodes, allow_update=True)`**——而 `BaseIndex.insert_nodes` 是有这一句的。所以这里把「docstore 写入」下推到了 `_add_nodes_to_index` 内部（它按分支决定写不写）。这是必要的：若在父类那里无条件写 docstore，`stores_text=True` 且无 override 时就会白存一份。

注意**异步版的 `add_index_struct` 是同步调用**（`self._storage_context.index_store.add_index_struct(...)`，没有 `await`），而 c01 里 `BaseIndex.ainsert_nodes` 用的是 `await ... .async_add_index_struct(...)`。所以这里**异步路径里夹了一个同步持久化**。可能是刻意（index_store 的异步接口未必实现），也可能是遗漏——源码无注释。后果是这个 `await` 的调用方会在这一步阻塞。

**`_insert` 只有一行，但它是 `_add_nodes_to_index` 的转发。**

```python
def _insert(self, nodes, **insert_kwargs) -> None:
    self._add_nodes_to_index(self._index_struct, nodes, **insert_kwargs)
```

注意**没有传 `show_progress`**——所以 `_add_nodes_to_index` 用默认 `False`。即插入路径上永远不会显示进度条，而构建路径会（因为它显式传了 `self._show_progress`）。这是一个不对称：大文档初次插入有进度条，后续增量插入没有。

**`_delete_node` 是一个空实现，而它让父类的 `delete_nodes` 失效。**

```python
def _delete_node(self, node_id: str, **delete_kwargs: Any) -> None:
    pass
```

这是 `BaseIndex` 要求的抽象方法，而这里**实现为 no-op**。所以若调用 `BaseIndex.delete_nodes`（父类版本），它会逐 id 调 `_delete_node`（什么都不做）然后写 index_store——即**看起来成功了，什么都没删**。真正的删除必须走 `VectorStoreIndex.delete_nodes`（覆写版）：

```python
def delete_nodes(self, node_ids, delete_from_docstore=False, **delete_kwargs) -> None:
    self._vector_store.delete_nodes(node_ids, **delete_kwargs)
    if (not self._vector_store.stores_text or self._store_nodes_override) and delete_from_docstore:
        for node_id in node_ids:
            self._index_struct.delete(node_id)
            self._docstore.delete_document(node_id, raise_error=False)
        self._storage_context.index_store.add_index_struct(self._index_struct)
```

四处：

1. **`self._vector_store.delete_nodes(node_ids, ...)` 传的是 node id**。而向量库里的 id 是 `IndexDict.nodes_dict` 的**键**（可能不等于 node id，因为 `add_node` 用了 `text_id` 作键）。所以这里有一个**潜在的不一致**：若向量库返回的 id 与 node id 不同，`delete_nodes([node_id])` 就删不掉那条向量。只有当向量库的 `add` 返回的 id 恰好等于 node id 时才正确。
2. **`self._index_struct.delete(node_id)` 用的也是 node_id**，而 `IndexDict.delete` 是 `del self.nodes_dict[doc_id]`——**键是向量库 id**。所以这里同样假定了两者相等。
3. **`IndexDict.delete` 用 `del`，键不存在会抛 `KeyError`**（见下）。而 `delete_nodes` 没有 try 包裹，所以**重复删除会抛 `KeyError`**——与 `docstore.delete_document(raise_error=False)` 的幂等形成对照。
4. **`self._storage_context.index_store.add_index_struct(...)` 在 if 内部**。所以当 `stores_text=True` 且无 override 时，它**不会被调用**——那是对的（因为 index_struct 没变），但若向量库删了而 index_struct 没变，重启后 `nodes_dict` 还留着已删的记录。

`IndexDict.delete` 的实现：

```python
def delete(self, doc_id: str) -> None:
    """Delete a Node."""
    del self.nodes_dict[doc_id]
```

`del` 在键不存在时抛 `KeyError`，所以这里是**不幂等**的。这与父类 `delete_nodes` 里 `docstore.delete_document(node_id, raise_error=False)` 的幂等形成一对矛盾：同一个删除操作，一处容错一处不容错。

**`delete_ref_doc` 的三步各有条件，而条件表达式重复了四次。**

```python
def delete_ref_doc(self, ref_doc_id, delete_from_docstore=False, **delete_kwargs) -> None:
    self._vector_store.delete(ref_doc_id, **delete_kwargs)
    self._delete_from_index_struct(ref_doc_id)
    if delete_from_docstore:
        self._delete_from_docstore(ref_doc_id)
    self._storage_context.index_store.add_index_struct(self._index_struct)
```

而两个辅助函数各自重复了同一个条件：

```python
def _delete_from_index_struct(self, ref_doc_id: str) -> None:
    if not self._vector_store.stores_text or self._store_nodes_override:
        ref_doc_info = self._docstore.get_ref_doc_info(ref_doc_id)
        if ref_doc_info is not None:
            for node_id in ref_doc_info.node_ids:
                self._index_struct.delete(node_id)
```

即**只有当「节点被额外存了」时，才需要清理 index_struct**。逻辑正确，但条件是字符串级的重复（四处）。若某天新增一个「存节点」的形态，四处都得改。

而 `self._vector_store.delete(ref_doc_id, ...)` 传的是 **`ref_doc_id`**——父文档 id，不是节点 id。所以向量库的 `delete` 方法必须支持「按 ref_doc_id 删掉它派生出的所有向量」。这与 `delete_nodes` 传 node id 是**两种不同的删除粒度**，而它们都调向量库的不同方法（`delete` vs `delete_nodes`）。

**异步的 `adelete_ref_doc` 用 `asyncio.gather` 并行三个删除，这是唯一一处真并行。**

```python
tasks = [
    self._vector_store.adelete(ref_doc_id, **delete_kwargs),
    self._adelete_from_index_struct(ref_doc_id),
]
if delete_from_docstore:
    tasks.append(self._adelete_from_docstore(ref_doc_id))
await asyncio.gather(*tasks)
```

三处：

1. **三个删除并行**。与同步版逐个顺序执行形成对比。这带来一个行为差异：同步版若向量库删除失败会停在第一步，异步版则可能全部部分成功。
2. **`asyncio.gather` 默认在第一个异常时**不会取消其他任务，而是等所有任务结束后抛出第一个异常。所以「部分成功」是可能的状态，而调用方只看到一个异常。
3. **`_adelete_from_index_struct` 与 `_adelete_from_docstore` 都读 `_index_struct`**，而它们并行执行——若其中任一真的修改了 `_index_struct`，另一个可能看到中间状态。实际上前者会 `self._index_struct.delete(node_id)`，后者只操作 docstore，所以没有竞争。但这个结论需要读者自己论证。

**`ref_doc_info` 在「文本在向量库」的部署下直接抛错。**

```python
@property
def ref_doc_info(self) -> Dict[str, RefDocInfo]:
    if not self._vector_store.stores_text or self._store_nodes_override:
        node_doc_ids = list(self.index_struct.nodes_dict.values())
        nodes = self.docstore.get_nodes(node_doc_ids)
        all_ref_doc_info = {}
        for node in nodes:
            ref_node = node.source_node
            if not ref_node:
                continue
            ref_doc_info = self.docstore.get_ref_doc_info(ref_node.node_id)
            if not ref_doc_info:
                continue
            all_ref_doc_info[ref_node.node_id] = ref_doc_info
        return all_ref_doc_info
    else:
        raise NotImplementedError("Vector store integrations that store text in the vector store are not supported by ref_doc_info yet.")
```

四处：

1. **`else` 分支抛 `NotImplementedError`**，措辞是「not supported ... **yet**」——即承认这是缺失而非设计。后果是这个部署形态下：`delete_ref_doc` 无法按文档删（因为 `_delete_from_index_struct` 需要 `ref_doc_info`）、`refresh_ref_docs` 的 `update` 路径也走不通。所以选择 `stores_text=True` 会失去一整类能力。

   **这就是 `store_nodes_override` 存在的真正理由**——它是从「文本在向量库」这个形态里爬回完整能力的唯一开关。

2. **`self.index_struct.nodes_dict.values()` 是 node id 列表**（因为 `add_node` 存的是 `nodes_dict[vector_id] = node.node_id`）。所以 `docstore.get_nodes(node_doc_ids)` 拿的是节点 id——方向正确。
3. **`node.source_node`** 依赖 b01 的 `NodeRelationship.SOURCE`。所以**没有打 SOURCE 关系的节点会被静默跳过**（`continue`）。而 `run_transformations` 通常会自动打这个关系，所以正常工作。
4. **结果是 `Dict[str, RefDocInfo]`，键是 `ref_node.node_id`（源文档 id）**。多个节点指向同一个源文档时会**反复覆盖**同一个键——所以这个字典的构建成本随节点数增长，但输出大小只随文档数增长。

**`IndexDict` 的三个字段里两个已废弃。**

```python
nodes_dict: Dict[str, str] = field(default_factory=dict)          # vector store id -> node doc_id
doc_id_dict: Dict[str, List[str]] = field(default_factory=dict)   # TODO: deprecated, not used
embeddings_dict: Dict[str, List[float]] = field(default_factory=dict)  # TODO: deprecated, not used
```

只有 `nodes_dict` 在用（注释也标着 `TODO: slightly deprecated, should likely be a list or set now`）。所以这个结构实际上是「一张 id 映射表 + 两块遗留字段」。后两块保留的理由大概是**反序列化旧存档时需要字段存在**——若删掉，旧 JSON 反序列化会因未知字段报错（取决于配置是否 `extra="forbid"`）。

注意 `nodes_dict` 的注释说「slightly deprecated, should likely be a list or set now」——即作者认为**它需要的信息只是一个 id 集合，不需要映射**。但映射是必需的（向量库 id 可能 ≠ 节点 id）。所以这条 TODO 与 `add_node` 的实现互相矛盾：`add_node` 明确用了两个不同的 id 来建立映射。这大概是「在理想情况下向量库 id 就该等于节点 id」的遗留愿望。

## 关键取舍

**源码事实：** 整个类由 `stores_text` 一个能力位二分：`True` 时文本在向量库、`IndexDict` 只记 `ImageNode`/`IndexNode`；`False` 时全部节点进 docstore 与 `IndexDict`。`store_nodes_override` 是给 `True` 强行开启「也存节点」的第三态。

**源码事实：** `from_vector_store` 拒绝 `stores_text=False` 的库（否则 id 无法解析成文本），并静默丢弃调用方传的 `storage_context`；它必须传 `nodes=[]` 而非 `None` 才能通过父类的「至少给一个」校验。

**源码事实：** 嵌入是「批量算 + 逐节点 `model_copy` 后写 `embedding`」，所以传给向量库的是副本，原节点不带向量；`id_to_embed_map[node.node_id]` 用方括号，缺键抛 `KeyError`。

**源码事实：** `_add_nodes_to_index` 按 `insert_batch_size` 分批，每批「嵌入 + 写入」；`zip(nodes_batch, new_ids)` 依赖向量库返回同序同长的 id，**未校验长度**，少返回会静默截断。

**源码事实：** 存进 `IndexDict` 与 docstore 的节点都先把 `embedding` 置 `None`（注释：「avoid duplication」）；`IndexDict.nodes_dict` 的键是向量库 id、值是节点 id。

**源码事实：** `VectorStoreIndex.build_index_from_nodes` 覆写只为过滤 `get_content(MetadataMode.EMBED) == ""` 的节点，用 `print` 提示且不报数量；`insert_nodes` 路径**没有**这条过滤。

**源码事实：** 两个 `insert_nodes` 覆写都**跳过了父类的 `docstore.add_documents`**，把 docstore 写入下推到 `_add_nodes_to_index` 的分支里；异步版的 `add_index_struct` 是同步调用（没有 `await`）。

**源码事实：** `_delete_node` 是空实现（`pass`），所以父类版的 `delete_nodes` 在这个子类上「成功但什么都没删」；真正的删除在覆写版里，且 `IndexDict.delete` 用 `del` 故不幂等（重复删会 `KeyError`），而同一路径里 `docstore.delete_document(raise_error=False)` 是幂等的。

**源码事实：** `delete_nodes` 与 `delete_ref_doc` 是两种粒度：前者传 node id 调 `vector_store.delete_nodes`，后者传 `ref_doc_id` 调 `vector_store.delete`；两者都假定「向量库 id == 节点 id」才能正确联动 `IndexDict`。

**源码事实：** `delete_ref_doc` 的条件 `not stores_text or store_nodes_override` 在四个位置重复；异步版用 `asyncio.gather` 并行三个删除，同步版顺序执行。

**源码事实：** `ref_doc_info` 在 `stores_text=True` 且无 override 时抛 `NotImplementedError`（措辞含「yet」），因此该部署下按文档删与 refresh 的更新路径都不可用；`store_nodes_override` 是唯一的逃生开关。

**架构推论：** 「一个能力位二分整个类」的代价是**每个方法都要重复同一条条件**。好处是不引入两套实现（避免代码分叉），坏处是这条条件是**字符串级的重复**而非一次性的策略对象——新增第三态会让四五处同步修改。更紧的做法是把「节点该不该额外存」抽成一个策略（如 `_should_mirror_node(node) -> bool`），让分支判断集中一处。

**架构推论：** `_delete_node` 空实现这件事说明「抽象方法」与「这个子类不支持」之间存在缝隙。父类用 `@abstractmethod` 要求子类实现，而子类用 `pass` 满足它——形式上有实现、语义上没有。这类缝隙在 Python 里无法被类型系统阻止，只能靠文档或运行期检查。**它造成的具体风险是「删除静默失效」**，而这比抛 `NotImplementedError` 危险得多。

**架构推论：** `ref_doc_info` 抛 `NotImplementedError` 而 `_delete_node` 静默通过，是同一个类里**两种「不支持」的处理方式并存**。前者的理由清楚（错误消息带「yet」表示是待补的缺口），后者的理由不明。判据应该是「不说话会不会造成错误的结果」：`ref_doc_info` 若返回空字典，用户会以为「这个索引没有文档」；`_delete_node` 什么都不做，用户会以为删掉了。两者其实同样危险，但只有一处报了错。

**架构推论：** 「嵌入存进向量库、节点不带向量」这个去重设计，与 c01 提到的「三处状态」配合得刚好：向量的家在 `vector_store`，原文的家在 `docstore`，两处靠 `nodes_dict` 的映射对齐。代价是这个映射必须与两处都保持同步——`delete_nodes` 里「向量库删了但 index_struct 没删」的情形就是同步失败的一种。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex` | 向量索引主体，按 `stores_text` 二分所有行为 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.from_vector_store` | 接管已有向量库，拒绝不存文本的库 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.as_retriever` | 传 node id 快照与 `object_map` 给 `VectorIndexRetriever` |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex._get_node_with_embedding` | 批量嵌入 + 逐节点复制写回 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex._add_nodes_to_index` | 分批「嵌入 + 写入」，两个分支决定哪些节点额外进 docstore |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex._build_index_from_nodes` | 同步/异步两路构建，`use_async` 决定走哪条 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.build_index_from_nodes` | 覆写以过滤空内容节点（`print` 提示、不报数量） |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex._validate_serializable` | 把不可序列化的 `IndexNode.obj` 挪进 `_object_map` |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.insert_nodes` / `ainsert_nodes` | 覆写跳过父类 docstore 写入，异步版 `add_index_struct` 未 await |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex._delete_node` | 空实现，使父类版 `delete_nodes` 静默无效 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.delete_nodes` / `delete_ref_doc` | 两种删除粒度，条件在四处重复 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex.ref_doc_info` | 依赖 `source_node` 回溯源文档；`stores_text=True` 时抛 `NotImplementedError` |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexDict` | `nodes_dict`（向量库 id → 节点 id）+ 两块已废弃字段 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexDict.delete` | `del` 语义，键不存在抛 `KeyError` |

## 参考

- [llama-index 源码：indices/vector_store/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/vector_store/base.py)
- [llama-index 文档：向量存储索引](https://docs.llamaindex.ai/en/stable/module_guides/indexing/vector_store_index/)
- [llama-index 文档：向量存储与 stores_text](https://docs.llamaindex.ai/en/stable/module_guides/storing/vector_stores/)
- [llama-index 源码：data_structs.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/data_structs/data_structs.py)

## 与主核 / AI 的关系

「由下游能力位二分整个实现」这件事在主核里有直接对应：主核的存储引擎会按**表是否带索引、是否分区**来分流不同的执行路径，而每条路径都要重复「有无索引」的判断。主核的解法是靠**规划器一次性决定计划节点**，让执行器只看到「现在该干什么」，而不是每个操作都重判。`VectorStoreIndex` 没有规划层，所以判断就散落到每个方法里——这正是这页反复看到同一条件的原因。

`stores_text` 这个二分与主核的**「值内联 vs 溢出页」**也有可比的点。主核按大小阈值自动选择，调用方不必知道；这里把选择权交给了**部署时选的向量库**，然后反过来影响十几种方法的行为。结果是「换一个向量库」这件事的语义后果远大于换一个存储后端——因为 `ref_doc_info` 会从「能用」变成「抛 NotImplementedError」。**能力的可见范围应当与配置的粒度匹配**：一个部署级的开关不该悄悄关掉一整类 API。

`IndexDict` 的三字段里两个废弃，与主核的**系统列演进**同源。主核要新增一个系统列时，旧元组里没有那个位置——读取时要能识别「这条是旧格式」。区别在于主核靠 `xmin`/版本号明确判断，而这里靠「字段还在但没人用」的默契。后者的问题是**新人无法区分「废弃字段」与「暂未使用的字段」**（`_object_map` 就是后者）。

`_delete_node` 的 `pass` 值得单独对照主核的一条纪律：**不支持的操作必须报错而不是静默通过**。主核里若某个存储格式不支持某个操作，会在打开时拒绝而不是在执行时什么都不做——因为静默通过会让上层以为状态符合预期。

对 AI 工程的实际意义在于：向量索引是 RAG 里最常见也最「看起来简单」的一层，而它的真实复杂度全在「下游能力差异」。这份实现给出的三条经验可直接用：**把下游能力位收敛成一个策略判断而不是散落的条件**（否则新增形态要改四五处）、**不支持的能力要显式抛错而不是空实现**（静默失效是最难排查的 bug）、以及**依赖下游返回顺序的地方要校验长度**（`zip` 的静默截断会让部分数据消失得无声无息）。
