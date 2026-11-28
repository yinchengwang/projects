---
project: llama-index
feature: property-graph-index
title: 属性图索引
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/indices/property_graph/base.py
    symbols: [PropertyGraphIndex]
  - path: llama-index-core/llama_index/core/graph_stores/types.py
    symbols: [PropertyGraphStore, LabelledPropertyGraph, EntityNode]
---

# 属性图索引

## 一句话职责

`PropertyGraphIndex` 用 LLM 从文本里抽「实体 + 关系」三元组，写进一个属性图存储；再让检索沿图走（同义词扩展、向量近邻、路径展开）。它的特殊之处是**索引结构本身是空的**——真正的数据全在图存储里。

## 机制是什么

这个索引与前面几个索引有一个根本差别：**`IndexLPG` 什么都不存**。

```python
class IndexLPG(IndexStruct):
    """An index struct for LPG index (doesn't actually store anything)."""

    def add_node(self, node: BaseNode) -> None:
        pass
```

所以「索引」这个概念在这里被推到了极致：`BaseIndex` 期望子类产出一个 `IndexStruct` 作为骨架，而这个子类的骨架是空的——所有数据都在 `property_graph_store` 里。它靠着两处惯性融入父类框架：

1. **`index_store` 里存一个空结构**，让 `BaseIndex` 的「持久化 index_struct」逻辑有个对象可写。
2. **`docstore` 不是主存储**，因为原始节点被转成 `ChunkNode` 并塞进了图存储（`upsert_llama_nodes`）。

于是这个索引维护的不再是「三处状态」，而是「一处状态 + 两个空壳」。

第二件要处理的事是**三元组抽取的中间表示**。`kg_extractors` 是 `TransformComponent` 列表，它们把三元组写到**节点的 `metadata`** 里（键是 `KG_NODES_KEY` / `KG_RELATIONS_KEY`），再由 `_insert_nodes` 取出来。所以「抽取」与「落库」之间靠 metadata 当摆渡——而不是靠某种专门的中间类型。这个设计复用了他已有的 `TransformComponent` 抽象，代价是 **metadata 成了一个临时通道**，而 metadata 本来是给「用户元数据」用的。

## 怎么实现的

**构造函数有四处值得逐一看的初始化。**

```python
storage_context = storage_context or StorageContext.from_defaults(property_graph_store=property_graph_store)

if property_graph_store is not None:
    storage_context.property_graph_store = property_graph_store
elif storage_context.property_graph_store is None:
    storage_context.property_graph_store = SimplePropertyGraphStore()

if vector_store is not None:
    storage_context.vector_stores[DEFAULT_VECTOR_STORE] = vector_store

if embed_kg_nodes and (storage_context.property_graph_store.supports_vector_queries or embed_kg_nodes):
    self._embed_model = resolve_embed_model(embed_model) if embed_model else Settings.embed_model
else:
    self._embed_model = None
```

四处：

1. **图存储的三个来源有优先级**：显式传的 `property_graph_store` > `storage_context` 里已有的 > 新建 `SimplePropertyGraphStore`。注意前两者都有时**显式传的会覆盖 storage_context 里的**（`storage_context.property_graph_store = property_graph_store`）——即「显式参数优先于上下文」。
2. **`vector_store` 被写进 `storage_context.vector_stores[DEFAULT_VECTOR_STORE]`**。所以它复用了通用的多向量库容器，用一个固定键占位。
3. **`if embed_kg_nodes and (supports_vector_queries or embed_kg_nodes)`** —— 这个条件里 `embed_kg_nodes` 出现了两次。化简后等价于 `embed_kg_nodes`（因为 `X and (Y or X) == X and Y or X == X` 在当前取值下退化成 `embed_kg_nodes`）。所以 `supports_vector_queries` 这一支**永远不影响结果**。这是一个冗余条件，可能是从更早的版本（那时意图是「只有图存储支持向量查询才需要嵌入」）残留的。

   它的实际语义因此是「只要 `embed_kg_nodes` 为真就解析嵌入模型」。

4. **`self._llm = llm`（不 fallback 到 `Settings.llm`）**，而 `kg_extractors` 的默认值里才做 `llm or Settings.llm`。所以 `self._llm` 可能是 `None`，而它被传给 `as_retriever` 里的 `LLMSynonymRetriever(llm=self._llm)`。那个检索器大概率会自己 fallback——但索引层不做兜底，是一处职责不清。

**`_override_vector_store` 是一个由两个来源推出的事实。**

```python
self._override_vector_store = (
    vector_store is not None
    or not storage_context.property_graph_store.supports_vector_queries
)
```

即「要么用户显式给了向量库，要么图存储不支持向量查询」——两种情况都需要/用外部向量库。而它只在一处被消费：

```python
@property
def vector_store(self) -> Optional[BasePydanticVectorStore]:
    if self._embed_kg_nodes and self._override_vector_store:
        return self.storage_context.vector_store
    else:
        return None
```

所以「有没有可用的向量库」= `embed_kg_nodes` ∧ `_override_vector_store`。两处条件组合出四种情形：

| `embed_kg_nodes` | `_override_vector_store` | 结果 |
|---|---|---|
| True | True | 用外部向量库（嵌入写入它） |
| True | False | 用图存储自带的向量能力 |
| False | 任意 | `None`（不嵌入） |

注意第三种情形的后果：**`_embed_model` 也是 `None`**，于是 `_insert_nodes` 里 `if self._embed_kg_nodes:` 整块跳过。所以「关掉嵌入」是一个彻底的开关，不会留下半成品。

**`from_existing` 是「接管已有图存储」的入口，而它只做一件事：传 `nodes=[]`。**

```python
return cls(nodes=[], property_graph_store=property_graph_store, vector_store=vector_store, ...)
```

注释写「no nodes to insert」。`nodes=[]` 与 `None` 的区别在 c01 讲过（空列表让「至少给一个」的校验通过）——不过这里还传了 `property_graph_store`，所以 `BaseIndex.__init__` 的校验其实由 `storage_context` 那一路隐含满足。

这个入口的价值是：**从持久化的图加载后不用重建**。因为没有索引操作结构，所以「加载」就只是「接上图存储」。这与 `VectorStoreIndex.from_vector_store` 相比更彻底——后者还要校验 `stores_text`，而这里几乎什么都不用校验。

**`_insert_nodes` 是这个类的全部逻辑，而它的六步顺序是有讲究的。**

**第一步：跑抽取器。**

```python
if self._use_async:
    nodes = asyncio.run(arun_transformations(nodes, self._kg_extractors, show_progress=self._show_progress))
else:
    nodes = run_transformations(nodes, self._kg_extractors, show_progress=self._show_progress)
```

注意 **`asyncio.run(...)`**——在已经在事件循环里调用会抛 `RuntimeError`。所以 `use_async=True`（默认！）时，若用户在一个 async 函数里调用同步的 `index.insert(...)`，就会直接崩。这是一个真实的坑，而默认值恰好是最容易踩的那一侧。

**第二步：断言每个节点都被写了三元组。**

```python
assert all(
    node.metadata.get(KG_NODES_KEY) is not None
    or node.metadata.get(KG_RELATIONS_KEY) is not None
    for node in nodes
)
```

用 `assert` 而不是抛 `ValueError`。后果是**在 `python -O` 下这条检查会被整个去掉**。而且它检查的是「至少有一个键」，若抽取器什么都没写（例如文本里没有实体），这里会 `AssertionError` —— 错误信息完全看不出是哪个节点、哪个抽取器出了问题。

**第三步：把三元组从 metadata 里搬出来，并打上来源标记。**

```python
for node in nodes:
    kg_nodes = node.metadata.pop(KG_NODES_KEY, [])
    kg_rels = node.metadata.pop(KG_RELATIONS_KEY, [])
    for kg_node in kg_nodes:
        kg_node.properties[TRIPLET_SOURCE_KEY] = node.id_
    for kg_rel in kg_rels:
        kg_rel.properties[TRIPLET_SOURCE_KEY] = node.id_
    kg_nodes_to_insert.extend(kg_nodes)
    kg_rels_to_insert.extend(kg_rels)
```

三处：

1. **用 `pop` 而不是 `get`** —— 所以抽取结果被从 metadata 里**移走**。这避免了 metadata 被污染（否则三元组会跟着节点一起落进其他存储）。但这也意味着**抽取只能做一次**——若节点已经跑过抽取器，再跑一次会拿到空结果。
2. **`TRIPLET_SOURCE_KEY` 记的是 `node.id_`** —— 即「这个实体/关系是从哪个节点抽出来的」。这是**图与原文之间的唯一连接**。它后来被用来实现「删节点时连带删掉它抽出的实体」。
3. **`kg_rels_to_insert.extend(kg_rels)`** —— 关系单独收集。所以实体与关系在后续写入时是**两个独立批次**。

**第四步：去重，而实体的去重与节点的去重用了不同判据。**

```python
kg_node_ids = {node.id for node in kg_nodes_to_insert}
existing_kg_nodes = self.property_graph_store.get(ids=list(kg_node_ids))
existing_kg_node_ids = {node.id for node in existing_kg_nodes}
kg_nodes_to_insert = [node for node in kg_nodes_to_insert if node.id not in existing_kg_node_ids]

existing_nodes = self.property_graph_store.get_llama_nodes([node.id_ for node in nodes])
existing_node_hashes = {node.hash for node in existing_nodes}
nodes = [node for node in nodes if node.hash not in existing_node_hashes]
```

三处：

1. **实体按 `id`（即名字）去重**。见下 `EntityNode.id` 的实现——它是 `name.replace('"', " ")`，所以**同名实体被视为同一个**。这让图能自然合并同义实体（「苹果」出现十次只有一个节点）。
2. **节点按 `hash` 去重**——与 b01 的 `Node.hash` 一致（含元数据）。所以「同一份文本但元数据变了」会被当成新节点插进去。
3. **两个去重都依赖「先把候选 id 查一遍库」**，即**每条插入都要先读**。所以批量插入的代价是「N 次读 + 一次写」。这对本地存储无所谓，对远端图库是明显的往返开销。

注意**关系没有任何去重**。所以同一对实体之间的同种关系会重复插入。`LabelledPropertyGraph.add_triplet` 里有一层 `if (subj.id, rel.id, obj.id) in self.triplets: return` 的守卫，但那只在内存实现里；远端存储的行为取决于后端（大概是 `upsert` 语义，所以重复也不产生副本）。

**第五步：嵌入，两批。**

```python
if self._embed_kg_nodes:
    node_texts = [node.get_content(metadata_mode=MetadataMode.EMBED) for node in nodes]
    if self._use_async:
        embeddings = asyncio.run(self._embed_model.aget_text_embedding_batch(node_texts, ...))
    else:
        embeddings = self._embed_model.get_text_embedding_batch(node_texts, ...)
    for node, embedding in zip(nodes, embeddings):
        node.embedding = embedding

    kg_node_texts = [str(kg_node) for kg_node in kg_nodes_to_insert]
    ...
```

四处：

1. **两批分别嵌入**：原始节点用 `get_content(MetadataMode.EMBED)`，图实体用 `str(kg_node)`。后者对 `EntityNode` 是 `"name (properties)"`（见下）。所以**实体参与向量检索的文本是「名字+属性」**。
2. **又一处 `asyncio.run`**——与第一步同样的坑。
3. **`zip(nodes, embeddings)` 依赖两者等长同序**。没有校验长度。
4. **`node.embedding = embedding` 直接写到节点上**（不是 `model_copy`）。所以这里的节点被就地改——与 c02 的向量索引（复制后改）相反。原因大概是这些 `nodes` 是 `run_transformations` 的产物，本就没有别的持有者。

**第六步：写入，而写入的顺序有注释强调。**

```python
if self.vector_store is not None and len(kg_nodes_to_insert) > 0:
    self._insert_nodes_to_vector_index(kg_nodes_to_insert)

if len(nodes) > 0:
    self.property_graph_store.upsert_llama_nodes(nodes)

if len(kg_nodes_to_insert) > 0:
    self.property_graph_store.upsert_nodes(kg_nodes_to_insert)

# important: upsert relations after nodes
if len(kg_rels_to_insert) > 0:
    self.property_graph_store.upsert_relations(kg_rels_to_insert)

if self.property_graph_store.supports_structured_queries:
    self.property_graph_store.get_schema(refresh=True)
```

四处：

1. **先写向量库、再写节点、再写关系**。而「关系必须在节点之后」有注释强调（`# important: upsert relations after nodes`）——因为关系引用 `source_id`/`target_id`，后端可能要求两端实体已存在。
2. **`upsert_llama_nodes(nodes)` 在 `upsert_nodes(kg_nodes_to_insert)` 之前**。而这两者写的是不同类别的节点（`ChunkNode` vs `EntityNode`），顺序大概无关，但它确保「原文先进库」。
3. **`get_schema(refresh=True)` 只在支持结构查询时调用** —— 因为 schema 是给 Cypher 生成用的，纯向量检索不需要。
4. **五处 `if len(...) > 0`** 是必要的防御：空的 `upsert` 调用对某些后端可能是无效请求或错误。

**`_insert_nodes_to_vector_index` 有一处「按后端能力决定 id」的细节。**

```python
llama_nodes: List[TextNode] = []
for node in nodes:
    if node.embedding is not None:
        llama_nodes.append(TextNode(
            text=str(node),
            metadata={VECTOR_SOURCE_KEY: node.id, **node.properties},
            embedding=[*node.embedding],
        ))
        if not self.vector_store.stores_text:
            llama_nodes[-1].id_ = node.id

    node.embedding = None
```

四处：

1. **只处理有嵌入的实体**（`if node.embedding is not None`）。所以关掉嵌入时这里什么都不做——但由于调用点已经检查了 `len(kg_nodes_to_insert) > 0`，它仍会被调用然后空转。

2. **`if not stores_text: llama_nodes[-1].id_ = node.id`** —— 只在向量库**不存文本**时用图实体的 id 作为向量库 id。理由是：不存文本时向量库里只留 id + 向量，那 id 必须是能回溯到图实体的那一个。而存文本时用默认 UUID（文本本身在向量库里，检索命中后能读文本）。

   这个设计让**同一个图实体在两套存储里的 id 关系随后端变化**：不存文本时 id 相同（可直接跳转），存文本时 id 不同（靠 `VECTOR_SOURCE_KEY` 元数据关联）。

3. **`metadata={VECTOR_SOURCE_KEY: node.id, **node.properties}`** —— 把图实体的 id 与全部属性塞进元数据。所以向量检索命中后能靠 `VECTOR_SOURCE_KEY` 找回图实体。注意 `**node.properties` 展开在**后面**，所以若 properties 里恰好有 `VECTOR_SOURCE_KEY` 同名的键，它会**覆盖**掉那个 id。这是一个可能的冲突：`TRIPLET_SOURCE_KEY` 就在 properties 里（前面刚写进去），所以 properties 里确实有框架自己的键。

4. **`node.embedding = None` 在 `if` 之外** —— 所以**每个实体**（无论有没有嵌入）的 `embedding` 都被清空。注释说「clear the embedding to save memory, its not used now」。

   但这里有一个顺序问题：这个循环在 `upsert_nodes(kg_nodes_to_insert)` **之前**执行（第六步的第一句）。所以图存储收到的实体**不带嵌入**。而 `_embed_kg_nodes=True` 时嵌入刚被算出来就是为了写进向量库——图存储那份确实不需要。但对**不支持向量查询**的图存储来说，`vector_store` 会是一个外部向量库（`_override_vector_store` 为真），所以嵌入去了向量库；而对**支持**向量查询的图存储，`_override_vector_store` 为假（除非用户传了 vector_store），于是 `self.vector_store` 为 `None`，`_insert_nodes_to_vector_index` 不被调用——**嵌入被算出来、然后被清空、且没有写进图存储**。

   即「图存储自带向量能力」这个分支下，嵌入被完全浪费。除非图存储的 `upsert_nodes` 会把 `embedding` 自动存（但它收到的是清空后的对象）。这是一个真实的逻辑漏洞。

**`EntityNode.id` 是「名字去掉双引号」，而这条规则承担了实体同一性。**

```python
class EntityNode(LabelledNode):
    name: str = Field(description="The name of the entity.")
    label: str = Field(default="entity", description="The label of the node.")
    properties: Dict[str, Any] = Field(default_factory=dict)

    def __str__(self) -> str:
        if self.properties:
            return f"{self.name} ({self.properties})"
        return self.name

    @property
    def id(self) -> str:
        return self.name.replace('"', " ")
```

三处：

1. **`id` = 名字（双引号替换成空格）**。所以「同名即同一实体」。替换引号是为了让 id 能安全地嵌进 Cypher 查询字符串（双引号是 Cypher 的标识符引号）。这是一个**为下游语法服务的转义**，而它把转义做在了 id 本身上——所以「名字里有引号的两个不同实体」会被合并。
2. **`label` 在子类里重新声明**，默认值从 `"node"` 改成 `"entity"`。这是 pydantic 的字段重声明（合法），但基类里的 `label` 已有定义——子类重复声明了三次字段（`label`、`properties` 都在基类有）。所以 `EntityNode` 实际上只在基类基础上加了 `name`，其余两个是冗余重声明。
3. **`__str__` 把 properties 拼进去**。所以 `str(entity)` = `"Apple ({"type": "company"})"` —— 一个 Python 字典字面量。这正是嵌入用的文本（第五步），所以**嵌入内容含 Python 而非自然语言的属性表示**。

**`Relation` 与 `ChunkNode` 的 id 各有问题。**

```python
class Relation(BaseModel):
    label: str
    source_id: str
    target_id: str
    properties: Dict[str, Any] = Field(default_factory=dict)

    @property
    def id(self) -> str:
        return self.label
```

**`Relation.id` 就是 `label`** —— 即「关系类型名」。所以「Apple 创立了 iPhone」与「Microsoft 创立了 Windows」两个 `founded` 关系的 `id` 相同。而三元组的存储用的是 `(subj.id, rel.id, obj.id)`：

```python
self.triplets.add((subj.id, rel.id, obj.id))
```

所以这里实际上是对的（三元组带两端），但**单独用 relation 的 id 做键就会冲突**：

```python
self.relations[self._get_relation_key(relation=rel)] = rel
```

`_get_relation_key` 是 `f"{relation.source_id}_{relation.label}_{relation.target_id}"` —— 三元拼接。所以关系字典的键是三元组派生键，不是 `rel.id`。这个设计是对的，但它与 `Relation.id` 返回 `label` 这件事**产生了语义分裂**：`id` 说它是唯一标识，实际不唯一。

`ChunkNode`：

```python
text: str = Field(description="The text content of the chunk.")
id_: Optional[str] = Field(default=None, description="The id of the node. Defaults to a hash of the text.")

@property
def id(self) -> str:
    return str(hash(self.text)) if self.id_ is None else self.id_
```

**`hash(self.text)`** 是 Python 内建 `hash()`，即**进程内随机化的字符串哈希**（`PYTHONHASHSEED` 未设时每次启动不同）。所以「默认按文本哈希生成 id」这条注释虽然准确，但那个哈希**跨进程不稳定**。后果是同一段文本在两个进程里得到不同 id——而这会让「重启后按 hash 去重」失效，产生重复的 `ChunkNode`。

这是一个真实的缺陷：把 `hash()` 用作持久化 id。

**`LabelledPropertyGraph` 是内存版图的完整实现，而它的三处删除有一处过头。**

```python
nodes: SerializeAsAny[Dict[str, LabelledNode]]
relations: SerializeAsAny[Dict[str, Relation]]
triplets: Set[Tuple[str, str, str]]
```

`add_triplet` 做三件事：去重检查、加三元组、把两端节点与关系写进各自的字典。

```python
def add_triplet(self, triplet: Triplet) -> None:
    subj, rel, obj = triplet
    if (subj.id, rel.id, obj.id) in self.triplets:
        return
    self.triplets.add((subj.id, rel.id, obj.id))
    self.nodes[subj.id] = subj
    self.nodes[obj.id] = obj
    self.relations[self._get_relation_key(relation=rel)] = rel
```

注意三元组里存的是 **`rel.id`（即 label）**。而按 label 做键意味着「同一对实体之间的同种关系」被认为相同——哪怕它的 properties 不同。所以两个属性不同的 `founded` 关系会被合并。这是一个**有损去重**：第二个关系的属性被直接丢弃（因为三元组已存在就 `return`）。

而 `delete_triplet` 的过头在于：

```python
self.triplets.remove((subj.id, rel.id, obj.id))
if subj.id in self.nodes:
    del self.nodes[subj.id]
if obj.id in self.nodes:
    del self.nodes[obj.id]
```

**删一个三元组会把两端节点整个删掉**——哪怕那个实体还参与别的关系。所以「删掉 Apple 的一条关系」会让 Apple 这个实体消失。这与图数据库的语义相反（应当只删边）。

`add_relation` 里有一段「自动补节点」：

```python
if relation.source_id not in self.nodes:
    self.nodes[relation.source_id] = EntityNode(name=relation.source_id)
if relation.target_id not in self.nodes:
    self.nodes[relation.target_id] = EntityNode(name=relation.target_id)
```

即「只有关系没有实体时，按 id 造一个空实体」。这保证了图的一致性（边必须有端点）。但造出来的 `EntityNode` 只有名字、没有 label 与属性——所以它是一个占位。

**`PropertyGraphStore` 抽象类的「可选方法」承担了能力分级。**

```python
class PropertyGraphStore(ABC):
    supports_structured_queries: bool = False
    supports_vector_queries: bool = False
    text_to_cypher_template: PromptTemplate = DEFAULT_CYPHER_TEMPALTE
```

两个能力位是**类属性默认 False**，由子类覆写。这是全部能力分级的开关，与 c02 的 `stores_text` 是同一手法。

而抽象方法只有六个：`get`、`get_triplets`、`get_rel_map`、`upsert_nodes`、`upsert_relations`、`delete`（加 `structured_query`、`vector_query`）。**其余都是具体的默认实现**：

- **`get_llama_nodes`**：`self.get(ids=node_ids)` → `metadata_dict_to_node(node.properties)` → `set_content(node.text)`。所以「把 `ChunkNode` 还原成 `TextNode`」靠的是**把节点信息以 metadata 形式存进图存储**——这正是 `upsert_llama_nodes` 做的事（`node_to_metadata_dict(llama_node, remove_text=True)`）。
  注意 **`remove_text=True`**：正文单独放在 `ChunkNode.text` 里（不重复进 metadata），还原时再 `set_content`。
  而 `except Exception: continue` —— 单个节点还原失败被**静默跳过**。所以一次「取十个节点」可能只返回九个。

- **`upsert_llama_nodes`**：把每个 `BaseNode` 转成 `ChunkNode`，`id_=llama_node.id_`（保留原 id），`properties=metadata_dict`，`embedding=llama_node.embedding`。所以**原文节点在图存储里的身份仍是它的原 id**——这让 `delete_llama_nodes` 能按 `node_ids` 找到它们。

- **`delete_llama_nodes`** 是四条查询的并集：

```python
for id_ in node_ids:
    nodes.extend(self.get(properties={TRIPLET_SOURCE_KEY: id_}))
if len(node_ids) > 0:
    nodes.extend(self.get(ids=node_ids))
for id_ in ref_doc_ids:
    nodes.extend(self.get(properties={"ref_doc_id": id_}))
if len(ref_doc_ids) > 0:
    nodes.extend(self.get(ids=ref_doc_ids))
self.delete(ids=[node.id for node in nodes])
```

  四处查询覆盖四个情形：**这个节点抽出的实体/关系**（按 `TRIPLET_SOURCE_KEY`）、**这个节点本身的 `ChunkNode`**（按 id）、**这份文档覆盖的节点**（按 `ref_doc_id` 属性）、以及**把文档 id 当节点 id 查**（最后一句 `get(ids=ref_doc_ids)`，语义可疑——文档 id 与节点 id 是不同空间）。

  所以删一个节点会连带删掉「从它抽出的实体」——**这会让那些实体从别处抽取的部分也消失**（因为它们被合并成同一个 `EntityNode` 了）。这是「实体共享」与「按来源删除」之间的固有冲突。

- **`persist` 返回 `None` 什么也不做**；**`get_schema` 返回 `None`**。所以这两个是「可选实现」的默认空壳——调用方无法区分「不支持持久化」与「持久化成功」。这正是 c02 里 `_delete_node` 为 `pass` 的同一类问题。

`text_to_cypher_template: PromptTemplate = DEFAULT_CYPHER_TEMPALTE` —— 注意常量名拼错成 `TEMPALTE`（应为 `TEMPLATE`）。这个拼写错误在 `graph_stores/prompts.py` 里定义、在 `types.py` 里被引用，所以**两处都用了错的拼写**（一致地错）。这与 b01 里 `metadata_seperator` 是同一类现象：拼写错误一旦进入 API 面就会被固化。

## 关键取舍

**源码事实：** `IndexLPG` 是空索引结构（`add_node` 为 `pass`），所以这个索引真正维护的状态只有 `property_graph_store`；`docstore` 与 `index_store` 都退化成空壳。

**源码事实：** 构造函数的嵌入开关条件是 `embed_kg_nodes and (supports_vector_queries or embed_kg_nodes)`，化简后等价于 `embed_kg_nodes`——`supports_vector_queries` 这一支实际不影响结果。

**源码事实：** `self._llm = llm` 不做 `Settings.llm` 兜底，而默认 `kg_extractors` 里才做兜底；`self._llm` 随后被传给 `LLMSynonymRetriever`。

**源码事实：** `_use_async` 默认 **True**，而同步入口内部用 `asyncio.run(...)`——在已有事件循环里调用同步的 `insert` 会抛 `RuntimeError`。

**源码事实：** 三元组抽取结果通过节点的 `metadata`（`KG_NODES_KEY` / `KG_RELATIONS_KEY`）摆渡，用 `pop` 取出（故抽取不可重复执行）；每个实体与关系都被打上 `TRIPLET_SOURCE_KEY = node.id_` 作为「来源」标记。

**源码事实：** 实体按 `id`（名字，双引号换空格）去重，节点按 `hash`（含元数据）去重，**关系不去重**；两处去重都先查库（N 次读 + 一次写）。

**源码事实：** 嵌入分两批（节点用 `MetadataMode.EMBED` 文本、实体用 `str(entity)` 即「名字 (properties 字典)」），`zip` 依赖等长同序且不校验；节点被就地写入 `embedding`（不像向量索引那样复制）。

**源码事实：** 写入顺序是「向量库 → llama 节点 → 图实体 → 关系」（关系必须在节点之后，有注释），最后按 `supports_structured_queries` 刷新 schema。

**源码事实：** `_insert_nodes_to_vector_index` 只在 `stores_text=False` 时把向量库 id 设成图实体 id（否则用默认 UUID，靠 `VECTOR_SOURCE_KEY` 元数据关联）；`metadata={VECTOR_SOURCE_KEY: node.id, **node.properties}` 里 properties 展开在后，**可覆盖**那个 id；且它在 `upsert_nodes` 之前清空全部实体的 `embedding`。

**源码事实：** 「图存储自带向量查询且没有显式传 `vector_store`」这一分支下，嵌入被算出来、写进实体的 `embedding`、然后被清空，而图存储收到的实体已经不带嵌入——嵌入被完全浪费。

**源码事实：** `EntityNode.id` = `name.replace('"', " ")`，故「名字含引号的不同实体」被合并；`Relation.id` = `label`（不唯一），而关系字典的键是 `source_id_label_target_id` 三元拼接；`ChunkNode.id` 默认值为内建 `hash(self.text)`，**跨进程不稳定**。

**源码事实：** `LabelledPropertyGraph.delete_triplet` 会**连同两端节点一起删除**（而非只删边）；`add_triplet` 用 `(subj.id, rel.id, obj.id)` 去重，故「两端相同但属性不同的同种关系」被合并、后者属性丢弃；`add_relation` 在端点缺失时造一个只有名字的 `EntityNode` 占位。

**源码事实：** `PropertyGraphStore` 用两个类属性 `supports_structured_queries` / `supports_vector_queries` 做能力分级；`get_llama_nodes` 用 `except Exception: continue` 静默跳过还原失败的节点；`delete_llama_nodes` 用四条查询取并集；`persist` 与 `get_schema` 默认什么也不做。

**源码事实：** 常量名 `DEFAULT_CYPHER_TEMPALTE` 拼错（`TEMPLATE` → `TEMPALTE`），定义处与引用处一致地错。

**架构推论：** 「索引结构为空」是一个**极端但诚实**的选择：它承认图存储本身就是索引，不需要额外的骨架。代价是它必须靠 `BaseIndex` 的其余部分空转来维持形式上的兼容（`index_store` 里存一个空结构、`docstore` 里的东西其实也在图存储里）。这类「为兼容抽象而空转」的状态是长期演进项目里常见的妥协，而它带来的风险是**使用者会误以为 `index.docstore` 里有数据**。

**架构推论：** 用 `metadata` 摆渡三元组是一个巧妙的复用，但也把 metadata 的两重身份混在一起：它既是「用户元数据」，又是「抽取器的输出通道」。而 `pop` 取出这一步恰好说明作者意识到这一点（用完就清）。这个设计之所以成立，只是因为 `TransformComponent` 的契约本来就是「节点进、节点出」——没有别的通道可用。

**架构推论：** `Relation.id = label` 这件事是「让短名当唯一标识」的典型错误。唯一标识必须是**全局唯一**的，而关系类型名天然不唯一（无数条 `founded`）。代码在真正需要唯一的场合（`relations` 字典的键）用了三元拼接，说明作者知道它不唯一——但 `id` 这个属性名仍然误导。

**架构推论：** `ChunkNode` 用内建 `hash()` 生成持久化 id 是本页最严重的缺陷。`hash()` 的设计用途就是进程内快速查找，它显式不保证跨进程稳定。把它当持久化 id 会让「去重」在重启后失效，产生重复数据。正确的做法是 `sha256(text)`（b01 里所有节点 hash 都这么算，所以这是同一份代码里两套做法的对照）。

**架构推论：** 「图存储自带向量能力」那一支的浪费，暴露了**能力分级与实际写入路径之间的缝隙**：能力位说「我自己能存向量」，所以索引层不另建向量库；但写入路径却假定「向量一定要送去某个向量库」，于是算出的嵌入无处可去。这类缝隙在「多后端 + 能力位」的架构里很常见，检测方式只能是端到端跑一遍并验证「向量真的可检索」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `PropertyGraphIndex` | 图索引主体，全部逻辑集中在 `_insert_nodes` |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `PropertyGraphIndex.__init__` | 图存储三来源优先级、`_override_vector_store` 推导 |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `PropertyGraphIndex.from_existing` | 接管已有图存储（`nodes=[]`） |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `PropertyGraphIndex.vector_store` | `embed_kg_nodes ∧ _override_vector_store` 才返回向量库 |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `_insert_nodes` | 六步：抽取 → 断言 → 搬出并打来源 → 去重 → 嵌入 → 顺序写入 |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `_insert_nodes_to_vector_index` | 按 `stores_text` 决定向量库 id，随后清空实体嵌入 |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `as_retriever` | 默认 `[LLMSynonymRetriever]`，条件性追加 `VectorContextRetriever` |
| `llama-index-core/llama_index/core/indices/property_graph/base.py` | `ref_doc_info` | 抛 `NotImplementedError`（「所有插入已是 upsert」） |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `LabelledNode` | 图节点抽象：label + embedding + properties |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `EntityNode` | 实体的 `id` = 名字去双引号；`__str__` 含 properties 字典 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `ChunkNode` | 原文块，默认 id 用内建 `hash(text)`（跨进程不稳定） |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `Relation` | 关系的 `id` = label（不唯一） |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `LabelledPropertyGraph` | 内存版图：三张表与增删；`delete_triplet` 连带删两端 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `PropertyGraphStore` | 抽象存储：两个能力位 + 六个具体默认实现 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `delete_llama_nodes` | 四条查询取并集后统一删 |
| `llama-index-core/llama_index/core/data_structs/data_structs.py` | `IndexLPG` | 空索引结构（`add_node` 为 `pass`） |

## 参考

- [llama-index 源码：indices/property_graph/base.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/indices/property_graph/base.py)
- [llama-index 源码：graph_stores/types.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/graph_stores/types.py)
- [llama-index 文档：属性图索引](https://docs.llamaindex.ai/en/stable/module_guides/indexing/lpg_index_guide/)
- [属性图模型与 Cypher 查询语言](https://neo4j.com/docs/cypher-manual/current/)

## 与主核 / AI 的关系

属性图索引与主核（多模态数据库内核）的**图索引扩展**是同一条线。主核的图能力建立在「一个顶点表 + 一个边表」，边用两列外键指回顶点；属性图把这两者收在一处，用 `Properties` 挂着。而本页的 `LabelledPropertyGraph` 用**三张表**（`nodes` / `relations` / `triplets`）表达同一结构，其中 `triplets` 存的是 `(subj_id, rel_id, obj_id)` 三元组——这正是主核边表的内容。

最值得对照的是 `delete_triplet` 删两端的做法。主核里删一条边绝不会删顶点（外键的 `ON DELETE` 语义显式声明在 schema 里）。而这里「删边即删点」等于**把边的删除语义绑成了 `ON DELETE CASCADE` 的反方向**——它是「删子即删父」。这在图数据库里是不成立的操作，会让「删一条错误的关系」导致实体消失。

`EntityNode.id = name` 这件事与主核的**主键语义**对照也很直接。主核里主键是用户声明的一列或几列，其唯一性由约束保证；而图实体用「名字」做主键，等于把「同名即同物」当成公理。这在知识图谱里是个合理假设（实体识别本来就靠名字归一），但它的代价是**无法表达同名不同物**（两个都叫「Apple」的公司）。主核通过复合主键能表达这种情形，而这里做不到。

`delete_llama_nodes` 的四条查询取并集，是「按来源删除」与「实体共享」冲突的直接表现。主核的对应问题在**引用完整性**上：删一行前要先检查有没有别的地方引用它。这里反过来——实体被多条来源共享，所以删任何一条来源都会伤到共享的实体。**这是「去重带来共享」的必然代价**，而主核的解法是靠外键检查把冲突显式化，这里则是静默连带删除。

对 AI 工程的实际意义在于：图索引是 RAG 里「关联推理」能力的载体，而它的工程正确性取决于几个容易忽略的细节。这份实现给出的三条经验可直接用：**唯一标识必须全局唯一**（关系用类型名当 id 会让不同关系混成一个）、**持久化 id 不能用内建 `hash()`**（跨进程不稳定会静默产生重复）、以及**能力位要与实际写入路径端到端对齐**（「图存储自带向量」那一支目前会把嵌入算完丢掉）。
