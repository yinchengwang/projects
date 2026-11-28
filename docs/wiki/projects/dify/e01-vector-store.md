---
project: dify
feature: vector-store
title: 向量库工厂与懒加载
page_type: feature
schema_version: 1
refs:
  - path: api/core/rag/datasource/vdb/vector_factory.py
    symbols: [AbstractVectorFactory, Vector]
  - path: api/core/rag/datasource/vdb/vector_type.py
    symbols: [VectorType]
---

# 向量库工厂与懒加载

## 一句话职责

`VectorType` 枚举列出所有支持的向量库名字，`AbstractVectorFactory` 定下「怎么造一个后端」的契约，`Vector` 则把「嵌入 + 批处理 + 后端方法转发」收成一个门面——上层只跟 `Vector` 打交道，不知道底下是哪个库。

## 机制是什么

三层结构：

- **`VectorType`（`StrEnum`，32 个取值）**：名字清单。取值有两种风格——多数是库名直译（`chroma`、`milvus`、`qdrant`），少数带连字符或下划线（`pgvecto-rs`、`elasticsearch-ja`、`tidb_on_qdrant`）。**枚举值就是配置里 `VECTOR_STORE` 的取值**，所以这些字符串是外部契约。
- **`AbstractVectorFactory`**：唯一的抽象方法是 `init_vector(dataset, attributes, embeddings, *, session)`，返回一个 `BaseVector`。它的 docstring 交代了一条重要约定：`session` 可能是 `None`，「调用方在自己的事务之外初始化后端」时就是 `None`；而传进来的 session **归调用方所有，实现不得 commit / rollback / close**。
- **`Vector`**：门面。构造时选后端、持有一个懒加载的嵌入对象；对外提供 `create` / `add_texts` / `search_by_vector` / `search_by_file` / `search_by_full_text` / `delete*` / `text_exists`，并通过 `__getattr__` 把后端特有方法也透出去。

后端的发现不在这个文件里：`get_vector_factory_class` 先查 `dify.vector_backends` 这个 entry point 组，找不到才回退到内置映射表（当前为空），两者都没有就抛 `ValueError` 并把**已安装的后端名单列出来**——这条错误消息给出了具体的安装命令，排障很有用。

## 怎么实现的

**懒加载的嵌入对象是为了让「删除」不被计费接口拖死。** `Vector.__init__` 里不构造真实嵌入模型，而是塞一个 `_LazyEmbeddings(dataset)`：

```python
self._embeddings: Embeddings = _LazyEmbeddings(dataset)
```

类 docstring 把原因写全了：真正构造嵌入会经 `ModelManager.get_model_instance` → `FeatureService.get_features` → `BillingService` 的 HTTP 请求。而清理路径（`delete_by_ids` / `delete` / `text_exists`）根本不需要嵌入——若构造时就取，一次计费接口抖动就会让删除任务失败、留下悬空的 `document_segments` 与 `child_chunks`。

**这个懒代理有一处比真正的嵌入更聪明的地方**：`embed_query` 先尝试直接查 Redis 缓存，**命中就完全不构造模型**；只有未命中才 `_ensure()`。

其它方法（`embed_documents`、多模态两条、异步两条）一律转发给 `_ensure()` 后的真实对象。

**`attributes` 的缺省列表带着一段血泪注释。** 缺省是七个字段：`doc_id`、`dataset_id`、`document_id`、`doc_hash`、`doc_type`、`is_summary`、`original_chunk_id`。注释说明：后两个是摘要向量用的，而某些后端（特别点名 Weaviate）把这个列表当成**显式返回字段投影**——不在列表里就不返回。少了它们，摘要命中会静默塌进 `is_summary = False` 分支，**摘要检索路径变成空操作**。注释还挂了 issue 编号。

也就是说「缺省字段列表」不只是「常用字段」，在特定后端上它是**语义的一部分**。这类差异无法从接口层看出来。

**`resolve_vector_type` 的三级优先。** 顺序是：

1. 数据集上若已有 `index_struct_dict`，用里面的 `type`——**已建好的库不可换后端**，这条优先于全局配置。
2. 否则用全局 `dify_config.VECTOR_STORE`。
3. 若开了白名单开关且该租户在白名单里，强改为 `TIDB_ON_QDRANT`。

第 3 条容易被忽略：**白名单会覆盖全局配置**。它读 `Whitelist` 表里 `category == "vector_db"` 的记录，存在即改。这让「某些租户用特定后端」成为可能，但也意味着同一个部署里可以有多个后端同时存在。

`Vector.__init__` 还允许显式传 `vector_type`：传了就直接用（并跳过 `session` 校验），没传就必须有 session（因为要走 `resolve_vector_type` 查表）。这条分支存在的原因是「清理任务里可能只有数据集元数据、没有活 session」。

**`create` 与 `add_texts` 是两个不同层次的入口。** `create` 自己分批（固定 1000）、自己调嵌入、逐批送后端，全程打时间日志；`add_texts` 一次性嵌入全部再送后端，另外支持 `duplicate_check`——开了就先 `text_exists(doc_id)` 过滤已存在的。

所以 `create` 是「大量新数据」的路径，`add_texts` 是「少量增量 + 可去重」的路径。两者都没有做幂等保证，靠调用方选对方法。

**空文本过滤在三个入口都做了。** `_filter_empty_text_documents` 丢掉 `page_content.strip()` 为空的文档并打一条带数量的 warning。这条过滤有实际后果：**空文本不进向量库，也就不会有向量**，但它对应的段行可能已经写进数据库。所以「段数」与「向量数」天然可能不等。

**`create_multimodal` 额外要做 base64 与文件查询。** 对每批：批量查 `UploadFile`（避免 N+1）、从存储 `load_once` 读字节、base64 编码、连同 `content_type` 与 `file_id` 组成一个 dict 交给嵌入。**找不到对应 `UploadFile` 的文档会被跳过**——它进不了 `real_batch`，所以那个位置没有向量。这是多模态路径上「静默丢弃」的一处。

**`search_by_file` 是反向的多模态检索**：给一个文件 id，读它的字节、当查询嵌入、拿向量去检索。所以它支持「以图搜图」。

**`delete` 会顺手清 Redis 的集合存在性缓存。** 删完后 `redis_client.delete(f"vector_indexing_{collection_name}")`——这个键是别处用来判断「集合是否已建」的，不删会导致下次以为集合还在而不重建。

**`__getattr__` 让门面能透传后端专有方法**，但有个细节：`self._vector_processor` 在 `__init__` 里可能还没赋值时被访问会无限递归。当前实现里 `__init__` 先赋 `_vector_processor` 再返回，风险窗口很小，但严格说 `__getattr__` 访问 `self._vector_processor` 本身就可能触发 `__getattr__`。

**`_get_embeddings` 与 `_LazyEmbeddings._ensure` 是两段重复代码。** 前者是 `Vector` 的老方法，返回 `CacheEmbedding(model_instance)`；后者是懒代理里的同逻辑实现。前者在构造函数改成懒加载后已经基本不该用了（否则就破坏了懒加载的意义）。

## 关键取舍

**源码事实：** `attributes` 缺省含 `is_summary` 与 `original_chunk_id`，注释说明某些后端把这个列表当返回字段投影，缺了会让摘要检索静默失效。

**源码事实：** 构造 `Vector` 时不构造嵌入模型，用 `_LazyEmbeddings` 代理；只有 `embed_*` 被调用才真正取模型。`embed_query` 还会先查 Redis，命中则不取模型。

**源码事实：** `resolve_vector_type` 优先用数据集已有的 `index_struct_dict["type"]`，其次全局配置，再次白名单覆盖为 `TIDB_ON_QDRANT`。

**源码事实：** 后端通过 `dify.vector_backends` entry point 发现，内置映射表当前为空；找不到时错误消息会列出已安装后端并给安装命令。

**源码事实：** `create` 固定 1000 条一批并逐批嵌入；`add_texts` 一次性嵌入并可选去重；两者都先过滤空文本。

**源码事实：** `create_multimodal` 里找不到 `UploadFile` 的文档被静默跳过，不产向量。

**源码事实：** `delete` 后清 Redis 的 `vector_indexing_<collection>` 键。

**架构推论：** 懒加载嵌入这个设计解决的是一类很具体的故障：**清理路径被无关依赖拖死**。它的代价是「对象构造成功」不再意味着「依赖可用」——错误从构造期推到了第一次 `embed_*`。这类延后是值得的，因为清理是运维动作（要能在计费服务故障时照常工作），而嵌入是功能动作。

**架构推论：** 「`attributes` 在某些后端是投影」暴露了一个更普遍的问题：**统一抽象里混进了各后端语义不等价的参数**。在 Chroma 里它可能是「元数据白名单」，在 Weaviate 里是「返回字段列表」——名字一样、语义不同。这类参数是抽象泄漏的集中点，只能靠注释与 issue 编号维持。

**架构推论：** 「已建库的后端不可切换」这条优先规则是对的：向量库不同，写入的数据格式、索引类型、检索算法都不同。切换意味着重建整个库。把它做成「读数据集上的记录」而非「读配置」，使配置变更只影响新建库、不影响存量——**这是把不可变决策记录在数据里而不是配置里**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `api/core/rag/datasource/vdb/vector_type.py` | `VectorType` | 32 个后端名字的枚举，值即配置取值 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `AbstractVectorFactory` | 后端工厂契约，含 session 归属约定 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `AbstractVectorFactory.gen_index_struct_dict` | 生成数据集上的后端记录结构 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector` | 统一门面：嵌入、分批、后端转发 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.resolve_vector_type` | 三级优先的后端解析 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.create` | 分批嵌入并写入（固定 1000） |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.create_multimodal` | 文件字节转 base64 后嵌入写入 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.add_texts` | 增量写入，可选按 `doc_id` 去重 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.search_by_vector` | 查询嵌入 + 检索（带 trace span） |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.search_by_file` | 以文件为查询的多模态检索 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `Vector.delete` | 删后端数据并清 Redis 集合缓存 |
| `api/core/rag/datasource/vdb/vector_factory.py` | `_LazyEmbeddings` | 延迟构造嵌入模型的代理 |
| `api/core/rag/datasource/vdb/vector_backend_registry.py` | `get_vector_factory_class` | entry point 优先的后端解析与缓存 |

## 参考

- [dify 源码仓库](https://github.com/langgenius/dify)
- [dify 源码：core/rag/datasource/vdb/vector_factory.py](https://github.com/langgenius/dify/blob/main/api/core/rag/datasource/vdb/vector_factory.py)
- [dify 文档：向量数据库配置](https://docs.dify.ai/getting-started/install-self-hosted/environments)
- [Python 文档：importlib.metadata entry points](https://docs.python.org/3/library/importlib.metadata.html#entry-points)

## 与主核 / AI 的关系

「枚举名字 + 工厂契约 + 统一门面」与主核里「存储访问方法（AM）的 `amhandler` 注册 + 统一执行器」是同一种插件化结构。主核用 `pg_am` 系统表登记实现，dify 用 entry point 组——前者可查询、可跨版本，后者零配置、随安装变化。**共同点是：门面之上不知道底下是谁，门面之下必须遵守契约。**

对 RAG 工程最实用的四条。其一：**已建好的知识库不能换向量库后端**，`index_struct_dict` 里的类型优先于配置；换后端等于重建库。其二：**清理路径被刻意设计成不依赖计费接口**，所以删除在计费服务故障时仍能工作——反过来说，若你在别处自己构造嵌入对象做删除，就丢掉了这层保护。其三：**空文本不进向量库但可能已进数据库**，段数与向量数天然可能不等，做一致性核对时不能要求严格相等。其四：**`attributes` 列表要当成公共契约对待**：往里加字段对多数后端无害，但删字段在 Weaviate 这类后端上会让对应功能静默失效。
