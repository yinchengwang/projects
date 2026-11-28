---
project: llama-index
feature: graph-and-vector-store
title: 图存储与向量存储契约
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/graph_stores/types.py
    symbols: [GraphStore, PropertyGraphStore, LabelledNode, Relation]
  - path: llama-index-core/llama_index/core/vector_stores/types.py
    symbols: [VectorStore, BasePydanticVectorStore, VectorStoreQuery]
  - path: llama-index-core/llama_index/core/vector_stores/types.py
    symbols: [MetadataFilters, MetadataFilter, VectorStoreQueryMode]
---

# 图存储与向量存储契约

## 一句话职责

给两类检索后端各定一份「必选极少」的契约：图存储演化成「三元组协议」与「带属性图抽象」两代并存，向量存储则把「用向量找、按元数据筛」压成一个查询对象——而元数据过滤被写成**可嵌套的表达式树**，因为这是唯一能同时表达各后端支持的 AND/OR/NOT 又不写死语法的方式。

## 机制是什么

四层结构：

**图的节点模型**——`LabelledNode(BaseModel)` 三字段 `label` / `embedding` / `properties`，另有 `__str__` 与 `id` 两个 `@abstractmethod`。两个子类 `EntityNode`（`id` 是去掉引号的 `name`）与 `ChunkNode`（`id` 是显式 `id_` 否则 `hash(text)`）。`Relation` 是一个 `label` + `source_id` + `target_id` + `properties`，它的 `id` 就是 `label`。

**两代图契约**——`GraphStore(Protocol)` 是三元组式的：`upsert_triplet(subj, rel, obj)` / `get(subj)` / `get_rel_map(subjs, depth, limit)` / `delete(subj, rel, obj)` / `get_schema` / `query`。`PropertyGraphStore(ABC)` 是属性图式的：用 `LabelledNode` 对象而不是三个字符串，且 `get` / `get_triplets` / `delete` 都按「属性 或 id 或名字」筛选。

**向量查询对象**——`VectorStoreQuery` 是一个 `@dataclass`，十四个字段。

**两套向量契约**——`VectorStore(Protocol)` 带 `@runtime_checkable`；`BasePydanticVectorStore(BaseComponent, ABC)` 是它的 Pydantic 版，注释写着：「为 pydantic 临时复制的 `VectorStore`，不能与 `runtime_checkable` 混用」。

## 怎么实现的

**图的两代契约是并存而非替代，差异在「用什么表达节点」。** `GraphStore` 的方法签名全是字符串：

```python
def upsert_triplet(self, subj: str, rel: str, obj: str) -> None: ...
def get(self, subj: str) -> List[List[str]]: ...
```

而 `PropertyGraphStore` 用对象：

```python
def upsert_nodes(self, nodes: Sequence[LabelledNode]) -> None: ...
def upsert_relations(self, relations: List[Relation]) -> None: ...
def get(self, properties: Optional[dict] = None, ids: Optional[List[str]] = None) -> List[LabelledNode]: ...
```

所以前者只能表达「三个名字组成的关系」，后者能表达「节点有属性、关系有属性、节点可带 embedding」。`LabelledNode` 上那个 `embedding: Optional[List[float]]` 字段是一个关键信号：**属性图的节点自己也可能是向量检索的目标**。

`GraphStore` 是 `Protocol` 而 `PropertyGraphStore` 是 `ABC`，这个差别的实际后果是：前者**不需要继承**（鸭子类型，任何实现了那组方法的类都算），后者**必须继承**。这解释了为什么新契约选了 `ABC`——它需要在基类上挂三个类属性：

```python
supports_structured_queries: bool = False
supports_vector_queries: bool = False
text_to_cypher_template: PromptTemplate = DEFAULT_CYPHER_TEMPALTE
```

`Protocol` 里也能写类属性（`GraphStore` 有 `schema: str = ""`），但 `ABC` 的类属性带默认值更容易被子类覆盖并被框架读取。而这两个 `supports_*` 布尔与 h01 那套「能力上界」是同一个手法。

`supports_structured_queries` / `supports_vector_queries` 为 `False` 时，对应的抽象方法仍然存在（`structured_query` / `vector_query` 都是 `@abstractmethod`），所以**实现方必须写一个「不支持」的实现**。这与「可选钩子」的做法相反——这里选择了「必须声明不支持」而不是「不实现」。

**`property_graph_store` 的便利方法都建在抽象方法之上。** 四个非抽象方法：`get_llama_nodes` / `upsert_llama_nodes` / `delete_llama_nodes` / `get_schema_str`。其中三个负责**在属性图模型与 llama-index 节点模型之间转换**：

`upsert_llama_nodes` 把 `BaseNode` 转成 `ChunkNode`：

```python
metadata_dict = node_to_metadata_dict(llama_node, remove_text=True)
converted_nodes.append(ChunkNode(
    text=llama_node.get_content(metadata_mode=MetadataMode.NONE),
    id_=llama_node.id_,
    properties=metadata_dict,
    embedding=llama_node.embedding,
))
```

`remove_text=True` 是关键——文本单独放进 `text` 字段，不从 `properties` 里重复一遍。

`get_llama_nodes` 反向转，且**吞掉所有异常**：

```python
for node in nodes:
    try:
        converted_nodes.append(metadata_dict_to_node(node.properties))
        converted_nodes[-1].set_content(node.text)
    except Exception:
        continue
```

所以一个 `properties` 里元数据结构不对的节点被**静默跳过**。这是宽容方向：反序列化失败不该让整批读取失败。代价是「部分节点不见了」这件事只能靠数量对比发现。

`delete_llama_nodes` 是这条链上最值得细看的一个。它要按两种 id 删：

```python
node_ids = node_ids or []
for id_ in node_ids:
    nodes.extend(self.get(properties={TRIPLET_SOURCE_KEY: id_}))
if len(node_ids) > 0:
    nodes.extend(self.get(ids=node_ids))

ref_doc_ids = ref_doc_ids or []
for id_ in ref_doc_ids:
    nodes.extend(self.get(properties={"ref_doc_id": id_}))
if len(ref_doc_ids) > 0:
    nodes.extend(self.get(ids=ref_doc_ids))

self.delete(ids=[node.id for node in nodes])
```

每个 id 都要查**两次**：一次按属性（`TRIPLET_SOURCE_KEY` 或 `"ref_doc_id"`）找到「由它派生出的图节点」，一次按 id 找到「它自己就是图节点」的那种。然后合并去重（靠 `delete` 按 id 列表天然覆盖）再一次性删。

这个双重查找反映了一个真实的结构问题：**llama-index 的节点与图存储的节点是两套对象**。一份文档切出的 chunk 在 docstore 里是一个 `TextNode`，在属性图里可能是 `ChunkNode`（用同一个 id），而由这个 chunk 抽取出的实体是 `EntityNode`（不同 id，但 properties 里记着源 chunk id）。所以「删一个 chunk」= 「删那个 `ChunkNode`」+「删所有指向它的 `EntityNode`」。

**`Relation.id` 就是 `label`**——这在图里是语义上不完整的。两条不同的边若 label 相同（例如两个都是 `KNOWS`），它们的 `id` 就相同。所以 `Relation` 的 id 不是唯一标识，而是「关系类型名」。用 `delete(ids=...)` 时若传 `Relation.id` 就会混淆 —— 而 `delete_llama_nodes` 传的只有 `node.id`（来自 `LabelledNode`），所以没踩这个坑。

`EntityNode.id` 是 `self.name.replace('"', " ")` ——把双引号换成空格。这个处理的原因是：图数据库的查询语句常把 id 拼进带引号的字符串里，若 id 自身含双引号就会破坏语法。这是**在 id 层做输入清洗**，代价是「name 里含引号的两个不同实体可能撞 id」。而 `ChunkNode.id` 用 `hash(self.text)` —— 同一段文本两次得到同一 id（进程内一致，但跨进程因 `PYTHONHASHSEED` 而不保证）。

**`PropertyGraphStore` 有第二套异步方法，默认实现全是「直接调同步」。** `aget` / `aget_triplets` / `aget_rel_map` 的实现分别是 `return self.get(...)` 这样的一行包装。注意它们**不是 `async def` 里 await 同步**（那会阻塞事件循环），也不是 `run_in_executor`，而是直接同步调用。

所以这套「异步」方法在默认实现下就是「假异步」——调用 `await store.aget(...)` 会同步阻塞。这与向量存储那边 `async_add` 的注释「若未实现就同步调 `add`」是同一个模式，且同样把这件事写在 doc 里。

**`VectorStoreQuery` 的十四个字段本身就是一份「各家后端的交集清单」。** 逐组看：

- 检索输入：`query_embedding`、`query_str`（文本检索用）。
- 数量：`similarity_top_k`（默认 1）、`sparse_top_k`（注释：目前只有 postgres 混合检索用）、`hybrid_top_k`（注释：混合检索返回的 top k，而 `similarity_top_k` 用于稠密部分）。
- 限定：`doc_ids`、`node_ids`（两个不同粒度）。
- 输出控制：`output_fields`、`embedding_field`。
- 模式：`mode: VectorStoreQueryMode`。
- 混合权重：`alpha`（注释：0 是 bm25、1 是向量）。
- 过滤：`filters: Optional[MetadataFilters]`。
- MMR：`mmr_threshold`。

三处 `# NOTE` 注释（`sparse_top_k` / `hybrid_top_k` / `alpha`）都在说明「这个字段是某个后端专属的」。这暴露了这一层的实际状态：**一个通用查询对象承载了若干后端专属字段，靠注释标明归属**。这是「用一个类型覆盖所有实现」的必然产物——否则就要为每类后端定义独立查询类型，而那样上层代码无法通用。

`VectorStoreQueryMode` 十一个值分四组：基础（`DEFAULT` / `SPARSE` / `HYBRID` / `TEXT_SEARCH` / `SEMANTIC_HYBRID`）、learner（`SVM` / `LOGISTIC_REGRESSION` / `LINEAR_REGRESSION`）、以及单独一组的 `MMR`。

`SVM` / `LOGISTIC_REGRESSION` / `LINEAR_REGRESSION` 这三个「fit learners」模式说明这个枚举的历史：早期有人实现了「把检索当分类问题」的方案。它们留在枚举里，但显然不是主流路径。

**元数据过滤是这一页的重点，而它的形状是一个递归类型。**

```python
class MetadataFilters(BaseModel):
    filters: List[Union[MetadataFilter, ExactMatchFilter, "MetadataFilters"]]
    condition: Optional[FilterCondition] = FilterCondition.AND
```

`filters` 的元素类型里含 `"MetadataFilters"` 自身——**所以它是递归的**。于是 `(A AND B) OR (C AND D)` 这种嵌套可以用「一个 `MetadataFilters` 里放另一个 `MetadataFilters`」表达，层级无限。

而 `condition` 只有三值：`AND` / `OR` / `NOT`，注释里挂着一句 `# TODO add more conditions`。

`NOT` 是**一元**运算，但它被放在「组合条件」这个位置上——所以一个 `condition=NOT` 的 `MetadataFilters` 语义上是对它的整个 `filters` 列表取反。这在表达上不自然（三值里两个是多元、一个是一元），但比引入两种结构简单。

`MetadataFilter` 三字段 `key` / `value` / `operator`。`value` 的类型用了一串 `StrictInt` / `StrictFloat` / `StrictStr` / 三种列表的 Union，类 docstring 解释了为什么用 Strict：

「值使用 Strict 类型，因为 int、float 和 str 是兼容类型，之前都被转成了字符串」。

这句话点出一个真实的历史 bug：Pydantic 的宽松转换会把 `1` 与 `"1"` 视为同一值，于是「用字符串 `"1"` 过滤整数列 `1`」会静默匹配。Strict 类型禁掉这个转换。而「之前都被转成了字符串」说明这是从旧行为修正过来的。

`FilterOperator` 十四值，每个都带注释说明适用类型：

`EQ`（默认，string/int/float）、`GT`/`LT`/`GTE`/`LTE`（int/float）、`NE`、`IN`/`NIN`（数组）、`ANY`/`ALL`（字符串数组）、`TEXT_MATCH`/`TEXT_MATCH_INSENSITIVE`（全文子串/短语）、`CONTAINS`（元数据数组包含值）、`IS_EMPTY`（字段不存在或为空）。

`IS_EMPTY` 的注释写「字段**不存在**或为空（null 或空数组）」——所以它同时覆盖「键缺失」与「值为空」两种情形。这在语义上是个有用的合并（用户通常不关心是哪种），但也意味着无法表达「只要键存在且非空」之外的区别。

`from_dict` 与 `from_dicts` 两个构造器分工清楚：

- `MetadataFilters.from_dict(filter_dict)` 已被 `@deprecated` 标注，它把 `{key: value}` 的扁平字典转成**全部 `EQ`** 的过滤器列表，"and" 条件。所以它是一个「简化的精确匹配」入口。
- `MetadataFilters.from_dicts(filter_dicts, condition=AND)` 接收「每个 dict 是一个 `MetadataFilter`」的列表，走 `MetadataFilter.from_dict`。这个能表达操作符。
- `MetadataFilter.from_dict(filter_dict)` 只是 `model_validate`。

而 `legacy_filters()` 是给旧后端的降级器：

```python
for filter in self.filters:
    if isinstance(filter, MetadataFilters) or filter.operator != FilterOperator.EQ:
        raise ValueError("Vector Store only supports exact match filters. Please use ExactMatchFilter or FilterOperator.EQ instead.")
    filters.append(ExactMatchFilter(key=filter.key, value=filter.value))
```

所以「只支持精确匹配的后端」拿到一个含 `GT` 或嵌套结构时，**明确拒绝**而不是丢弃那些条件。这与 h01 的 `put_all` 拒绝批量是同一原则：不能悄悄忽略一部分语义，否则调用方以为筛过了。

而 `ExactMatchFilter = MetadataFilter` 是一个**别名**（原类是注释掉的）。所以「旧类型」与「新类型」在运行时是同一个类——这意味着 `isinstance(filter, ExactMatchFilter)` 与 `isinstance(filter, MetadataFilter)` 完全等价。`legacy_filters` 里那个 `isinstance(filter, MetadataFilters)` 检查才是唯一有效的结构判别。

**两套向量契约的字段与默认值完全对齐。** `VectorStore(Protocol)` 与 `BasePydanticVectorStore` 都有 `stores_text: bool` 与 `is_embedding_query: bool = True`。抽象方法也一一对应：

| 方法 | Protocol | Pydantic 版 |
|---|---|---|
| `client` | `...`（抽象） | `@abstractmethod` |
| `add` | `...`（抽象） | `@abstractmethod` |
| `delete` | `...`（抽象） | `@abstractmethod` |
| `query` | `...`（抽象） | `@abstractmethod` |
| `get_nodes` / `delete_nodes` / `clear` | 不存在 | 存在但 `raise NotImplementedError` |
| `persist` | `return None` | `return None` |
| 所有 `a*` 方法 | 同步调同步 | 同步调同步 |

`BasePydanticVectorStore` 多了三个「存在但未实现」的方法（`get_nodes` / `delete_nodes` / `clear`），且用 `raise NotImplementedError("xxx not implemented")` 而不是像 `persist` 那样静默返回 `None`。

这个区别是刻意的：`persist` 是「可选增强」（内存向量库不需要持久化，静默无操作合理），而「按 id 取节点」「按条件删节点」「清空」都是**调用方会依赖的操作** ——静默无操作会让他们以为成功。所以明确报错。

而 `get_nodes` 的签名里带 `filters: Optional[MetadataFilters]` ——**同一个过滤表达式既用于 `query` 也用于按 id 取节点与删除**。这让「元数据过滤」成为这一层的通用语言，而不是仅属于检索。

`@runtime_checkable` 那个注释「不能与 `runtime_checkable` 混用」说明为什么要有两份：`Protocol` 可以用 `isinstance()` 检查（运行时只看方法存在性），而 Pydantic 的 `BaseModel` 子类不能同时是 `Protocol`（两者的元类冲突）。所以**Pydantic 模型作为向量存储实现时必须继承 `BasePydanticVectorStore`**，因为它需要被塞进 `StorageContext` 那种 Pydantic/dataclass 结构里。这就解释了 h01 里 `vector_stores` 的类型标注为什么是 `BasePydanticVectorStore` 而不是 `VectorStore`。

## 关键取舍

**源码事实：** `GraphStore` 是 `Protocol`（无需继承），`PropertyGraphStore` 是 `ABC`（必须继承），且后者带 `supports_structured_queries` / `supports_vector_queries` 两个能力位。

**源码事实：** 能力位为 `False` 时对应方法仍是 `@abstractmethod`——必须写一个「不支持」的实现，而不是省略。

**源码事实：** `EntityNode.id` 把双引号换成空格（防拼查询语句时破坏语法）；`ChunkNode.id` 用 `hash(text)`（跨进程不保证稳定）；`Relation.id` 就是 `label`（不唯一）。

**源码事实：** `PropertyGraphStore.delete_llama_nodes` 对每个 id 查两次（按属性找派生节点、按 id 找自身），再合并一次性删。

**源码事实：** `get_llama_nodes` 吞掉所有反序列化异常并跳过该节点。

**源码事实：** `PropertyGraphStore` 的 `a*` 方法默认实现是直接同步调用（真阻塞，不是伪异步）。

**源码事实：** `MetadataFilters` 的 `filters` 元素类型含自身，所以是可递归嵌套的表达式树；`condition` 三值里 `NOT` 是一元运算但占多元位置。

**源码事实：** `MetadataFilter.value` 用 `Strict` 类型，docstring 说明是因为「int、float、str 兼容且之前都被转成字符串」。

**源码事实：** `legacy_filters()` 遇到非 `EQ` 或嵌套结构时抛 `ValueError`，不丢弃条件。

**源码事实：** `ExactMatchFilter` 只是 `MetadataFilter` 的别名。

**源码事实：** `BasePydanticVectorStore` 的 `get_nodes` / `delete_nodes` / `clear` 存在但 `raise NotImplementedError`，而 `persist` 静默返回 `None`。

**架构推论：** 「元数据过滤做成递归表达式树」是这一层最重要的设计判断。若只支持扁平列表 + 一个 AND/OR（早期做法），就无法表达「A OR (B AND C)」；若为每个后端定义自己的过滤 DSL，上层代码就无法通用。递归类型是唯一同时满足两者的方案——**结构足够通用，而抽象方法只需要求实现方支持一个子集**。`legacy_filters()` 那条「不支持就报错」正是这个子集机制的出口。

**架构推论：** 「两代图契约并存」反映的是图存储在 RAG 里的定位演化。三元组协议对应「知识图谱问答」：关系是主体与客体之间的边，没有属性、没有向量。属性图协议对应「把图谱当另一种索引」：节点有 embedding、关系有属性、节点可能就是 chunk。前者是「从文档抽出图再查图」，后者是「图与向量混合检索」。两者不能合并，因为三元组后端（例如某些只支持 SPO 的图库）无法承载属性与 embedding。

**架构推论：** `Relation.id` 用 `label` 这件事说明「图里没有天然的关系 id」。关系由「起点 + 终点 + 类型」共同确定，若只取类型作 id，那同类型的所有边共享 id。所以任何需要「按关系 id 删一条边」的场景都必须退回到三元组签名（`subj, rel, obj`）——而 `GraphStore.delete` 的签名确实是这样。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/graph_stores/types.py` | `GraphStore` | 三元组式图存储协议，无需继承 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `PropertyGraphStore` | 属性图抽象，带两个能力位与一套伪异步 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `LabelledNode` | 图节点基类：label、可选 embedding、properties |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `EntityNode` | 实体节点，id 由名字清洗而来 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `ChunkNode` | 文本块节点，id 可显式或由哈希得出 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `Relation` | 关系的 label、两端 id、属性，id 即 label |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `upsert_llama_nodes` | llama-index 节点转 `ChunkNode`，文本不入 properties |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `get_llama_nodes` | 反向转换，吞异常跳过坏节点 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `delete_llama_nodes` | 每个 id 查两次再合并删除 |
| `llama-index-core/llama_index/core/graph_stores/types.py` | `TRIPLET_SOURCE_KEY` | 属性键，指向来源节点 id |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `VectorStore` | 带 `runtime_checkable` 的向量库协议 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `BasePydanticVectorStore` | Pydantic 版，为塞进存储上下文而生 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `VectorStoreQuery` | 十四字段查询对象，含三处后端专属注释 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `VectorStoreQueryResult` | 节点、相似度、id 三路返回 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `VectorStoreQueryMode` | 十一值，含 fit learners 与 MMR |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `MetadataFilters` | 可递归嵌套的过滤表达式树 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `MetadataFilter` | 单条过滤：键、Strict 类型值、操作符 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `FilterOperator` | 十四种操作符及各适用类型 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `MetadataFilters.legacy_filters` | 降级到精确匹配，不支持就报错 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `ExactMatchFilter` | `MetadataFilter` 的别名 |

## 参考

- [llama-index 源码：graph_stores/types.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/graph_stores/types.py)
- [llama-index 源码：vector_stores/types.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/vector_stores/types.py)
- [llama-index 文档：向量存储](https://docs.llamaindex.ai/en/stable/module_guides/storing/vector_stores/)

## 与主核 / AI 的关系

主核里最接近的是「查询条件的表达」：一个 WHERE 子句可以嵌套 AND/OR/NOT，可以比较、可以 IN、可以判断 NULL。`MetadataFilters` 就是这套表达式树在向量检索上的对应物，连「递归嵌套 + 顶层组合符」这个形状都一样。

而差别在**执行位置与能力差异**。主核的 WHERE 由自己的优化器完整支持，任何写出来的条件都会被执行。而向量库的过滤能力差异极大：有的只支持精确匹配，有的支持比较，有的支持全文，有的根本不支持过滤。所以这里必须有一个「降级」机制——`legacy_filters()` 就是那个出口，它把「我支持不到这个」变成一次明确报错。

这与 h01（存储上下文）、h02（文档存储）构成 H 组的三层：上下文组装、文档层的关系维护、检索后端的两套契约。三者共同的结构原则都是「抽象面尽量小、能力差异用显式位或显式报错表达」。

与本项目之外的对照：langchain 那边（见 `vectorstore-contract` 那一页）用「三个归一化公式 + 一个必须覆盖的选择器」处理相似度尺度；llama-index 这边根本不表达「相似度怎么归一」——`VectorStoreQueryResult.similarities` 就是一个 `List[float]`，语义由后端自己定义。差别在于一个把归一放在抽象层，一个把归一留在后端。

对 AI 工程最实用的一条是「过滤条件用递归表达式树而不是平面列表」。RAG 的实际查询里，「某部门 AND (某年份 OR 某季度)」这类嵌套非常常见。若抽象层只有「一个 AND 列表」，上层就只能拆成多次查询再自行合并——而那会让「取 top k」失去意义（每个子查询各取 k，合并后就不是全局 top k 了）。

另一条是 `Relation.id` 那个教训。图里「关系」没有天然唯一 id，用 `label` 当 id 会让同类型的边互相覆盖。任何「给非实体对象编 id」的设计都要问一句：这个 id 在同类对象之间唯一吗？若不唯一，那所有按 id 索引的操作都会静默出错。

最后一条是 `delete_llama_nodes` 的双重查找。当一个实体同时存在于「两套 id 空间」（框架的节点世界与后端的图世界）时，删除必须两边都查。这提示一个通用做法：**跨两套模型做删除时，按「在任一侧可能出现的所有 id 形态」各查一遍，再合并去重**。只按一侧查会留下孤儿记录，而孤儿记录不会报错，只会在统计或存储成本上慢慢显现。
