---
project: metagpt
feature: document-store
title: 文档存储后端抽象
page_type: feature
schema_version: 1
refs:
  - path: metagpt/document_store/base_store.py
    symbols: [BaseStore, LocalStore, search]
---

# 文档存储后端抽象

## 一句话职责

`document_store` 这一层想把「一个可检索的文档库」收敛成三个动作——`search`、`write`、`add`；`BaseStore` 声明这三个动作，`LocalStore` 在此之上补出「本地文件 → 索引 → 缓存目录」的公共骨架，各后端只实现「怎么读」与「怎么写」。

## 机制是什么

这一层有两级抽象，各自解决的问题不同。

**`BaseStore` 是能力声明。** 它是一个 `ABC`，只声明三个 `@abstractmethod`：`search`、`write`、`add`。三个方法签名全写成 `*args, **kwargs`，也就是**不约束参数**。这使任何后端都能落进这个接口，但反过来——接口不提供任何关于「search 返回什么、add 返回什么」的信息。类的 docstring 自己写着 `FIXME: consider add_index, set_index and think about granularity.`，等于承认索引粒度这件事还没想清楚。

**`LocalStore` 是本地文件后端的骨架。** 它的构造流程是一条固定的四步：校验 `raw_data_path` 非空（否则抛 `FileNotFoundError`，注意抛的是这个而不是参数错误）→ 把 `raw_data_path.stem` 记为 `fname`、默认把缓存目录定在原始文件同目录 → 调抽象方法 `_load()` 尝试加载已存在的索引 → **加载结果为空时调 `_write` 族方法重建**。

这里有一个容易忽略的自调用陷阱：父类构造里写的是 `self.store = self.write()`，而 `write()` 在子类里是无参的公开方法（`FaissStore.write` 就是无参）；真正负责建索引的抽象方法是 `_write(docs, metadatas)`——两者名字不同、参数不同。因此 `LocalStore` 的构造并不直接调用 `_write`，而是调用子类的 `write()`，由子类自己决定「怎么从原始文件读出 docs 与 metadatas 再交给 `_write`」。

**`_get_index_and_store_fname` 定义了缓存目录的期望布局。** 它把索引文件定为 `cache_dir/default__vector_store/.json`、文档库定为 `cache_dir/docstore/.json`，两个默认扩展名都是 `.json`。这个布局来自 llama-index 的 `StorageContext.persist` 约定——人类可读的目录结构，而不是单个打包文件。`LocalStore` 自己不解释这两个路径，只提供计算；怎么用由子类的 `_load` 决定。

## 怎么实现的

**`search` 的实现落在子类，且返回形态不统一。** `FaissStore.search` 的签名是 `(query, expand_cols=False, sep="\n", *args, k=5, **kwargs)`，行为是：用 `self.store.as_retriever(similarity_top_k=k)` 构造检索器、把 query 与它的 embedding 一起包成 `QueryBundle` 交给 `retrieve`，然后把结果拼成**一个字符串**（`expand_cols` 为真时每行带上 metadata）。而 `ChromaStore.search` 直接返回 chromadb 的原始 `results` 字典，调用方要从 `["ids"][0]` 里取。同一个接口名下，一个是格式化好的文本，一个是后端原始结构。

**`FaissStore` 的三个生命周期方法。** `_load` 先算两个缓存文件路径，任一不存在就返回 `None`（打印一条 info 日志），存在则用 `FaissVectorStore.from_persist_dir` 加 `StorageContext.from_defaults` 加 `load_index_from_storage` 复原索引。`_write` 校验 docs 与 metadatas 长度相等，把每个 doc 与对应 metadata 组 `Document`，再建 `FaissVectorStore(faiss_index=faiss.IndexFlatL2(1536))`——**维度硬编码 1536**，对应 OpenAI 系 embedding 的输出维度。`persist` 直接调 `storage_context.persist(self.cache_dir)`。

`write()` 是「首次构建」的入口：确认原始文件存在，用 `IndexableDocument.from_path` 按内容列与元数据列把文件解析成 `docs` 与 `metadatas`，再 `_write` 建索引、`persist` 落盘、返回索引。

**`add` 的已知缺陷。** `FaissStore.add` 把文本批量算 embedding、组 `TextNode`、`insert_nodes` 进内存索引，然后**返回空列表**，方法上方的注释直接写着 `FIXME: Currently, the store is not updated after adding.`——即新增节点没有触持久化，也没有更新 `self.store` 的对外视图。`delete` 则直接 `raise NotImplementedError`，注释说明 faiss 没有删除接口。

**`ChromaStore` 是刻意的例外。** 它**没有**继承 `BaseStore`，源码注释给了理由：「如果继承 BaseStore 或从 metagpt 导入其他模块，会出现奇怪的 Python 异常」。所以它单独实现了同名的一组方法：`__init__` 用 `chromadb.Client()` 建内存客户端并 `create_collection`；`search` 把三个可选过滤参数（`n_results`、`metadata_filter`、`document_filter`）映射到 chroma 的 `query`；`add` 与 `write` 分别对应单条与批量写入；`delete` 按 id 删；`persist` 直接 `raise NotImplementedError`，注释写「chroma 推荐用 server 模式，不本地持久化」。

**服务型后端用连接对象解耦。** `MilvusStore` 与 `QdrantStore` 都先在模块里定义一个 `@dataclass` 的 `MilvusConnection`/`QdrantConnection` 描述连接参数（uri、token、host、port、memory、api_key 等），再让 store 的构造只吃这一个对象。两个 store 都在真正需要时才 import 后端 SDK（`from pymilvus import MilvusClient` 写在 `__init__` 里），缺依赖时抛带「先安装」提示的异常。`MilvusStore.create_collection` 显式声明主键 id（VARCHAR，36 长度，容纳 uuid 文本）、向量字段与 `AUTOINDEX`/`COSINE` 索引参数；`QdrantStore` 同样提供 `create_collection`、`has_collection`、`delete_collection`。

服务型后端的 `write` 普遍是 `raise NotImplementedError` 或空实现——它们的写入走 `add`（`MilvusStore.add` 收 ids/vector/metadata 三个平行列表，`QdrantStore.add` 收 `PointStruct` 列表）。也就是说 `BaseStore` 声明的三个动作里，`write` 在服务型后端上是没有对应物的。

## 关键取舍

**源码事实：** `BaseStore` 的三个方法签名都是 `*args, **kwargs`，返回值无约定。因此「search 返回字符串」与「search 返回字典」并存，任何跨后端的调用代码都必须按具体后端分支。这层抽象只统一了方法名，没有统一语义。

**源码事实：** `LocalStore` 的构造里复用 `self.write()` 而不是抽象方法 `_write`，意味着「首次构建」这一步不在骨架里，而是每个本地子类自己实现。骨架真正固化的只有「路径校验 + 尝试加载 + 加载失败则构建」这个顺序。

**源码事实：** `ChromaStore` 主动不继承 `BaseStore`，原因写在注释里（导入引发的异常），这是一处被绕开的抽象，而不是被满足的抽象。

**源码事实：** `FaissStore` 的向量维度硬编码为 1536，`add` 不落盘、`delete` 不支持。三项都指向同一件事：这个后端被当作「一次性建索引、之后只读」的用法，而不是可增量维护的库。

**架构推论：** 这一层的价值不在统一语义，而在统一**入口形状**：所有后端都能被同一个构造点创建、都能被叫 `search`。它也解释了为什么 `rag/` 目录后来另起了一套 `SimpleEngine` + 检索器协议——`document_store` 的抽象太薄，无法承载「可增删、可持久化、可查询总量」这些能力断言，而这些断言在 RAG 链路里是必需的。

**架构推论：** 把连接参数抽成 dataclass 是个正确的小设计：它让「配置」成为可序列化、可比较、可默认的值对象，store 构造只依赖它。代价是每个后端都要自带一个这样的类，配置面无法集中校验。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/document_store/base_store.py` | `BaseStore` | 三个动作的能力声明，签名为 `*args/**kwargs` |
| `metagpt/document_store/base_store.py` | `LocalStore` | 本地后端骨架：路径校验、加载、失败即构建 |
| `metagpt/document_store/base_store.py` | `LocalStore._get_index_and_store_fname` | 索引与文档库的默认缓存布局 |
| `metagpt/document_store/faiss_store.py` | `FaissStore.search` | 检索并拼成以分隔符连接的文本 |
| `metagpt/document_store/faiss_store.py` | `FaissStore._write` | 硬编码 1536 维 L2 索引的构建 |
| `metagpt/document_store/faiss_store.py` | `FaissStore.add` | 仅插入内存索引，不持久化（已知缺陷） |
| `metagpt/document_store/chromadb_store.py` | `ChromaStore` | 刻意不继承 BaseStore 的内存 chroma 实现 |
| `metagpt/document_store/milvus_store.py` | `MilvusConnection` / `MilvusStore` | 连接对象解耦 + 显式集合与索引声明 |
| `metagpt/document_store/qdrant_store.py` | `QdrantConnection` / `QdrantStore` | 同上，含 memory 模式 |

## 参考

- [MetaGPT 源码：document_store/base_store.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/document_store/base_store.py)
- [MetaGPT 源码：document_store/faiss_store.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/document_store/faiss_store.py)
- [LlamaIndex 文档：StorageContext 与持久化布局](https://docs.llamaindex.ai/en/stable/module_guides/storing/stores/)

## 与主核 / AI 的关系

用三个 `*args, **kwargs` 方法去抽象五种完全不同的存储，与主核里「用统一的 I/O 回调屏蔽块设备与内存盘」表面像、实质不同：主核的回调签名固定（读哪一页、读多少、写到哪），语义由参数约定；这里的签名是空的，语义靠各自实现约定。前者是接口抽象，后者只是命名约定。这也解释了为什么它没能拦住 `search` 的两种返回形态。

对 agent 工程的直接结论：`document_store` 这一层今天不是可靠的基础设施。要接一个可增量写入的检索库，应走 `rag/` 那套（`SimpleEngine` 加能力协议），它才有「可修改 / 可持久化 / 可计数 / 可清空」的显式断言；`document_store` 更适合「一次性把一批文件建成索引、之后只读」的脚本场景，且要知道 `FaissStore.add` 之后不落盘。

