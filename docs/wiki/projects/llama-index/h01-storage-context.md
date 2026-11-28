---
project: llama-index
feature: storage-context
title: 存储上下文与三类存储契约
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/storage/storage_context.py
    symbols: [StorageContext]
  - path: llama-index-core/llama_index/core/storage/kvstore/types.py
    symbols: [BaseKVStore, BaseInMemoryKVStore, MutableMappingKVStore]
---

# 存储上下文与三类存储契约

## 一句话职责

用一个 `@dataclass` 把「节点存哪、索引存哪、向量存哪、图存哪」四件事收成一个可持久化的容器，并规定**向量存储按命名空间复数化**——默认空间与图片空间互不干扰，而这一切最底下都落在同一个「带 collection 的键值存储」抽象上。

## 机制是什么

`StorageContext` 是个 `@dataclass`，五个字段：

```python
docstore: BaseDocumentStore
index_store: BaseIndexStore
vector_stores: Dict[str, SerializeAsAny[BasePydanticVectorStore]]
graph_store: GraphStore
property_graph_store: Optional[PropertyGraphStore] = None
```

类 docstring 列了五项，其中 `property_graph_store` 后面标着「（惰性初始化）」——对应它在 `from_defaults` 里被包在 `try/except FileNotFoundError` 里。

底层的 `BaseKVStore` 有八个抽象方法，实际是四对同步/异步：`put`/`aput`、`get`/`aget`、`get_all`/`aget_all`、`delete`/`adelete`。每个方法的签名都带一个 `collection: str = DEFAULT_COLLECTION`——所以「键值存储」在这个抽象里其实是**多 collection 的**，`DEFAULT_COLLECTION = "data"`。

`BaseInMemoryKVStore` 在其上加了两个抽象：`persist(persist_path, fs)` 与 `from_persist_path` classmethod。所以「内存存储」在这个框架里不是「不持久化」，而是「持久化靠把整份内存 dump 出去」。

`MutableMappingKVStore(Generic[MutableMappingT], BaseKVStore)` 是一个**通用实现骨架**：它接受一个 `mapping_factory: Callable[[], MutableMapping[str, dict]]`，然后每个 collection 各建一份 mapping。

## 怎么实现的

**三层关系是这个框架的骨架。** 从 `StorageContext` 往下看：

1. `StorageContext` 组装四个（其实五个）存储对象。
2. 其中 `docstore` 与 `index_store` 各自内部都建在 `BaseKVStore` 上（docstore 见 h02）。
3. `vector_stores` 是 `Dict[str, ...]` ——**按命名空间复数化**。

这个层级的含义是：**「存什么」与「存在哪儿」被分成两个维度**。上层（索引）只关心「我需要一个 docstore 来记节点」，不关心它是 json 文件、Redis 还是 Postgres —— 那由底层 KV 的具体实现决定。

`from_defaults` 的参数表暴露了「什么都没有就给你内存版」的策略。当 `persist_dir is None`：

```python
docstore = docstore or SimpleDocumentStore()
index_store = index_store or SimpleIndexStore()
graph_store = graph_store or SimpleGraphStore()
image_store = image_store or SimpleVectorStore()
if vector_store: vector_stores = {DEFAULT_VECTOR_STORE: vector_store}
else: vector_stores = vector_stores or {DEFAULT_VECTOR_STORE: SimpleVectorStore()}
if image_store: vector_stores[IMAGE_VECTOR_STORE_NAMESPACE] = image_store
```

所以「无持久化目录」= 「全部用 Simple* 内存实现」。这解释了 `BaseInMemoryKVStore` 那个 `persist` 抽象的必要性：内存版也能被显式 `persist()` 出去。

有 `persist_dir` 时改成 `XxxStore.from_persist_dir(persist_dir, fs=fs)`，且多了一个 `try/except`：

```python
try:
    property_graph_store = (property_graph_store or SimplePropertyGraphStore.from_persist_dir(persist_dir, fs=fs))
except FileNotFoundError:
    property_graph_store = None
```

所以**属性图存储是唯一「可能不存在」的一类**——它的 `default=None` 与这个 `except` 是同一件事的两面。其余四类都必然有值（因为若没有持久化文件，Simple 实现的构造不会抛 `FileNotFoundError`……实际上会，但 `from_persist_dir` 若找不到文件仍会尝试加载；这里的差异在于 `SimplePropertyGraphStore.from_persist_dir` 是唯一会显式抛出并被接住的）。

`vector_stores` 在有 `persist_dir` 时的三分支值得逐一看：

```python
if vector_store: vector_stores = {DEFAULT_VECTOR_STORE: vector_store}
elif vector_stores: vector_stores = vector_stores
else: vector_stores = SimpleVectorStore.from_namespaced_persist_dir(persist_dir, fs=fs)
```

第三分支的 `from_namespaced_persist_dir` 说明**向量存储的持久化是按命名空间分文件的**——所以「从目录加载」需要扫描出所有命名空间，而不是读一个固定文件名。这也解释了后面的 `persist` 为什么按命名空间逐个写。

**`persist` 的路径拼接有两条分支，原因是 fsspec。** 实现：

```python
if fs is not None:
    persist_dir = str(persist_dir)  # NOTE: doesn't support Windows here
    docstore_path = concat_dirs(persist_dir, docstore_fname)
    ...
else:
    persist_dir = Path(persist_dir)
    docstore_path = str(persist_dir / docstore_fname)
    ...
```

有 `fs` 时用 `concat_dirs`（因为 fsspec 的路径不是本地 `Path`），无 `fs` 时用 `Path`。而那句 `# NOTE: doesn't support Windows here` 是一条明确的自述限制——用 `str` 拼接在远端文件系统上没问题，在 Windows 上则可能因分隔符而不对。

而两个分支里 `property_graph_store` 的处理**不对称**：`docstore` / `index_store` / `graph_store` 三者无条件 `persist`，而属性图存储包在 `if self.property_graph_store:` 里。所以「可能为 None」这个事实在这条路径上得到了尊重。

**向量存储的持久化路径带命名空间前缀。** 循环：

```python
for vector_store_name, vector_store in self.vector_stores.items():
    vector_store_path = ... f"{vector_store_name}{NAMESPACE_SEP}{vector_store_fname}"
    vector_store.persist(persist_path=vector_store_path, fs=fs)
```

所以一个目录里会同时存在 `default__vector_store.json` 与 `image__vector_store.json`（`NAMESPACE_SEP` 是两个下划线之类）。因此「加一个命名空间」不需要改任何文件名约定——`add_vector_store(vs, namespace)` 只需往 dict 里塞一项，`persist` 自动处理。

`vector_store` 那个 property 是兼容层：`return self.vector_stores[DEFAULT_VECTOR_STORE]`，docstring 写着 `Backwrds compatibility`（连拼写错误都在）。所以旧代码里的 `ctx.vector_store` 仍然能用，但它只指默认命名空间那一份。

**`to_dict` 有四道类型检查加四个 assert。** 实现先算 `all_simple`：

```python
all_simple = (
    isinstance(self.docstore, SimpleDocumentStore)
    and isinstance(self.index_store, SimpleIndexStore)
    and isinstance(self.graph_store, SimpleGraphStore)
    and isinstance(self.property_graph_store, (SimplePropertyGraphStore, type(None)))
    and all(isinstance(vs, SimpleVectorStore) for vs in self.vector_stores.values())
)
if not all_simple:
    raise ValueError("to_dict only available when using simple doc/index/vector stores")
```

然后四个 `assert isinstance(...)` ——**把 `and` 链换成逐项断言**，让类型检查器能缩窄类型。

注意 `property_graph_store` 的类型元组是 `(SimplePropertyGraphStore, type(None))` ——所以「None 也算合格」。这与持久化那条路径的对称处理一致。

`to_dict` 的返回用五个常量键：`VECTOR_STORE_KEY` / `DOC_STORE_KEY` / `INDEX_STORE_KEY` / `GRAPH_STORE_KEY` / `PG_STORE_KEY`。而 `from_dict` 反过来按这些键取，且 `PG_STORE_KEY` 也做了 None 判定。

所以 `to_dict` / `from_dict` 是一个**只支持「全 Simple」情形**的序列化入口——一旦任一存储换成外部实现（Redis、Postgres），它就直白地报错而不是静默降级。这与 `persist` 的形状不同：`persist` 对任何实现都可用（只要它实现了 `persist`）。

**`MutableMappingKVStore` 的两个魔法方法解决「工厂函数不好序列化」这个问题。** 实现：

```python
def __getstate__(self) -> dict:
    state = self.__dict__.copy()
    state["factory_fn"] = {"fn": self._mapping_factory}
    del state["_mapping_factory"]
    return state

def __setstate__(self, state: dict) -> None:
    self._collections_mappings = state["_collections_mappings"]
    self._mapping_factory = state["factory_fn"]["fn"]
```

所以它把工厂函数挪到一个 `{"fn": ...}` 的字典里再 pickle。为什么？因为裸的 callable 在 pickle 时可能失败（lambda、闭包、绑定方法都不行），而 `{"fn": callable}` 这种「只含一个键的 dict」形态在某些序列化框架（包括 pydantic / 某些 JSON 编码器）里能被特殊处理。

而 `_collections_mappings` 里存的是**每个 collection 的 mapping 实例**——它们本身也可能不可 pickle（例如 `defaultdict` 带 lambda）。所以这段代码只能救「工厂函数」，救不了「已经产出的 mapping」。这是一个部分解决。

**collection 是懒创建的。** `_get_collection_mapping`：

```python
if collection not in self._collections_mappings:
    self._collections_mappings[collection] = self._mapping_factory()
return self._collections_mappings[collection]
```

所以 `get` 一个不存在的 collection 会**建一个空 mapping 再返回空结果**。这有两个后果：一是「读」有副作用（内存里多一个空 collection）；二是「删不存在的 collection」不会被发现（`delete` 只看 key 在不在）。

**`put` 与 `get` 都做 `.copy()`，方向不同。** `put` 是 `mapping[key] = val.copy()` ——**存一份副本**，所以调用方之后改自己的 dict 不会影响存储。`get` 是 `return mapping[key].copy()` ——**返一份副本**，所以调用方改返回的 dict 不会污染存储。

但 `get_all` 是 `dict(self._get_collection_mapping(collection))` ——只拷了外层字典，**内层 val 是同一批对象**。所以「单条读写是隔离的，批量读不隔离」。这是一个不一致：通过 `get_all` 拿到的 dict 若被修改，会同时改到存储里的值。

`delete` 用 `try/except KeyError` 而不是 `if key in mapping` ——先做再捕获。对 `MutableMapping` 来说两种都行，但前者少一次查找。

**`persist` 与 `from_persist_path` 在基类里被写成「抛 NotImplementedError」。** 两个方法各自带一句注释「这个方法是放在这里以避免 TypeChecker 报错」。所以它们的唯一作用是**让子类覆写时类型签名一致**——基类不提供实现，只提供形状。

而错误消息直接把替代方案说出来：「请使用 `MutableMappingKVStore` 的子类（例如 `SimpleKVStore`）来调用这个方法」。

**`put_all` / `aput_all` 的默认实现是「逐条调 `put`」，且显式拒绝批量。** 实现：

```python
if batch_size != 1:
    raise NotImplementedError("Batching not supported by this key-value store.")
else:
    for key, val in kv_pairs:
        self.put(key, val, collection=collection)
```

`DEFAULT_BATCH_SIZE = 1` ——所以默认就是「一次一条」。而传 `batch_size != 1` 会**明确拒绝**而不是静默退化成逐条。这比「假装支持批量但其实循环」要好：调用方以为自己在用批接口省了开销，实际没有，而性能问题只在生产环境暴露。拒绝则立刻可见。

## 关键取舍

**源码事实：** `StorageContext` 是 `@dataclass`，`vector_stores` 是 `Dict[str, ...]` 而非单值；`vector_store` 是兼容旧接口的 property。

**源码事实：** `property_graph_store` 是唯一 `Optional` 的存储，且在 `from_defaults` 里被 `try/except FileNotFoundError` 包住。

**源码事实：** `persist` 分 `fs` / 非 `fs` 两条路径拼接路径，且自述「不支持 Windows」。

**源码事实：** 向量存储按命名空间分文件持久化，前缀是 `{namespace}{NAMESPACE_SEP}`。

**源码事实：** `to_dict` 要求全部使用 Simple 实现，否则 `raise ValueError`；且用四个 assert 做类型缩窄。

**源码事实：** `MutableMappingKVStore` 的 `put`/`get` 各做一次 `.copy()`，而 `get_all` 只拷外层。

**源码事实：** collection 懒创建，所以 `get` 不存在的 collection 有副作用。

**源码事实：** `put_all` 传 `batch_size != 1` 会 `NotImplementedError` 而非静默退化为循环。

**架构推论：** 「命名空间化的向量存储」是这个设计里最有解释力的一处。RAG 系统天然需要多个向量空间：文本一份、图片一份、可能还有「摘要」或「不同嵌入模型」各一份。把它们做成 `Dict[str, store]` 而不是「一个 store 里带 namespace 字段」，是因为**不同命名空间可能用完全不同的后端**——文本向量放 pgvector、图片向量放本地内存，这在 dict 里天然成立，在单 store 里就得靠 store 内部支持。

**架构推论：** 「collection 是 KV 层的一等概念」这一条很关键，因为它让**上层三个存储共用一份后端配置**。`docstore` 与 `index_store` 默认各用一个 collection，所以你若把它们指向同一个 Redis 实例，数据不会混——但如果你只有一个 collection，它们就会互相覆盖。这也解释了 `DEFAULT_COLLECTION = "data"`：默认值存在，但上层几乎总会显式传一个不同的。

**架构推论：** 「`get_all` 不深拷贝」这个不一致暴露的是「隔离到什么程度」这个没有统一答案的问题。单条 `get` 做拷贝是因为成本低（一个 dict 浅拷），批量不做是因为「整份数据拷贝」在真实规模下代价明显。而结果是：「改一条」安全，「改批量结果」会写回存储。这类不一致不会有人主动发现，只能用文档提示。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext` | 五字段容器：文档、索引、向量（多命名空间）、图、属性图 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext.from_defaults` | 无目录给内存版，有目录从各存储的持久化文件加载 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext.persist` | 按命名空间逐个写向量存储，两条路径拼接 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext.to_dict` | 只支持全 Simple 实现，否则报错 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext.vector_store` | 兼容旧接口，只指默认命名空间 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext.add_vector_store` | 往命名空间 dict 里加一份 |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `BaseKVStore` | 四对同步/异步方法，每个带 collection |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `BaseKVStore.put_all` | 默认逐条，显式拒绝非 1 的 batch_size |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `BaseInMemoryKVStore` | 内存实现也要 `persist` / `from_persist_path` |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `MutableMappingKVStore` | 通用骨架，collection 懒创建 |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `MutableMappingKVStore.__getstate__` | 把工厂函数挪进 dict 以改善序列化 |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `MutableMappingKVStore.get_all` | 只拷外层，内层共享引用 |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `DEFAULT_COLLECTION` | `"data"`，collection 概念的默认值 |
| `llama-index-core/llama_index/core/storage/kvstore/types.py` | `DEFAULT_BATCH_SIZE` | `1`，批量接口的默认粒度 |

## 参考

- [llama-index 源码：storage/storage_context.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/storage/storage_context.py)
- [llama-index 源码：storage/kvstore/types.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/storage/kvstore/types.py)
- [llama-index 文档：存储与持久化](https://docs.llamaindex.ai/en/stable/module_guides/storing/)

## 与主核 / AI 的关系

主核里最接近的是「表空间 / 存储引擎的分层」：一套数据目录里可以放多个表空间，每个表空间可以指向不同的物理文件；而访问数据时用「表空间 + 表 id」定位。`StorageContext` 就是这套分层的对应物，连「向量存储按命名空间分成不同文件」这个做法，都与主核「每个表空间一个目录」同构。

差别在于**向量存储这一层多了一次「复数化」**。主核的表空间是部署级的配置（很少变），而这里的命名空间是应用级的选择（文本一份、图片一份、可能还有按嵌入模型分的若干份）。所以它被做成运行时可变的 dict 而不是固定字段。

与 g03（向量存储契约，那是 langchain 项目）的对照在这里很自然：那篇讲的是「一个向量库该实现什么」，这一篇讲的是「多个向量库怎么被组织进一个上下文」。而共同点是「必选极少、其余按需」——`BaseKVStore` 四个操作对，`VectorStore` 一个必选检索，都是尽量小的契约。

与 h02（文档存储）的关系是直接的上层：`docstore` 字段的类型就是 h02 的 `BaseDocumentStore`，而 `KVDocumentStore` 又建立在 `BaseKVStore` 上。

对 AI 工程最实用的一条是「批量接口要么真批量、要么明确拒绝」。`put_all` 对 `batch_size != 1` 抛 `NotImplementedError`，这个选择的收益在真实部署里很具体：调用方如果以为有批量优化，会为了「省请求」把 1000 条塞进去；若底层只是循环，那 1000 次网络往返会全部发生，而症状是「批量写入比逐条还慢」。明确拒绝能让人立刻换方案。

另一条是「collection 让多个逻辑存储共用一份后端」。当你有节点、索引元数据、图关系三类数据时，最省事的做法是分三个数据库，但那意味着三套连接配置、三份运维。用「同一个 KV 后端 + 不同 collection」可以只配一次，而 `delete`/`get_all` 的 collection 隔离保证它们不互相破坏。

最后一条是 `to_dict` 那个「只支持全 Simple 就报错」的直白。在把对象序列化成 JSON 这件事上，静默降级（例如「丢掉了不支持的部分」）是最糟的处理方式——它产出一份看起来完整、实际缺数据的产物，而错误只会在恢复时暴露。明确报错要求调用方要么全用内存实现、要么用 `persist`，是把「能力边界」写在接口上的做法。
