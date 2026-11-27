---
project: metagpt
feature: retriever-factory
title: 检索器工厂与混合检索
page_type: feature
schema_version: 1
refs:
  - path: metagpt/rag/factories/retriever.py
    symbols: [RetrieverFactory]
  - path: metagpt/rag/retrievers/hybrid_retriever.py
    symbols: [SimpleHybridRetriever]
---

# 检索器工厂与混合检索

## 一句话职责

`RetrieverFactory` 把「一份检索器配置」翻译成一个具体检索器实例，并在给了多份配置时自动把它们包成 `SimpleHybridRetriever`；后者并行问所有子检索器、按节点 id 去重合并结果。

## 机制是什么

工厂的核心是一张「配置类型 → 创建函数」的字典。分发不看配置里的字符串字段，而是看**配置对象的 Python 类型**：`FAISSRetrieverConfig` 走 FAISS 创建函数，`ChromaRetrieverConfig` 走 Chroma，依此类推。这套分发规则由父类 `ConfigBasedFactory` 提供——它取的键是 `type(key)`，而不是 `key` 本身。

第二层结构是「索引与检索器的分工」。`RetrieverFactory` 不自己建向量库，它调用索引工厂（`get_index`、以及内部直接构造 `VectorStoreIndex`）拿到索引，再把索引塞进配置对象的 `index` 字段，最后用 `**config.model_dump()` 把整份配置摊平去构造检索器。因此**配置对象既是输入也是检索器构造参数的来源**，中间被工厂就地改写。

## 怎么实现的

**分发与实例化的两段式。** `get_retriever(configs=None, **kwargs)` 是唯一公开入口。无配置时走 `_create_default`：从 kwargs 或配置里取已有索引，取不到就用 `_build_default_index` 现建一个，最后返回 `index.as_retriever()`——即 llama-index 的默认检索器，不做任何本仓定制。有配置时调父类 `get_instances` 逐个分发，结果只有一个就直接返回它，多于一个就 `SimpleHybridRetriever(*retrievers)`。

**`get_or_build_index` 装饰器。** 三个「建索引」的函数（FAISS / Chroma / ES 各一个）都套了这个装饰器。它的逻辑是：先试 `_extract_index(config, **kwargs)`，取到就**直接返回、完全跳过构造**；取不到才执行被装饰函数去建。这避免了「已经加载过索引却又新建一个向量库」的浪费，也让 `from_index` 与 `from_nodes` 两条路径共用同一组创建函数。

`_extract_index` 与 `_extract_nodes`、`_extract_embed_model` 都转调 `_val_from_config_or_kwargs`，后者的优先级是「配置对象上该字段非 None 就用它，否则看 kwargs，都没有返回 None」。因此调用方既可以「在配置里带索引」，也可以「在 kwargs 里带索引」，后者覆盖前者（因为配置为 None 时才回落到 kwargs）。

**FAISS 的创建链。** `_build_faiss_index` 建 `FaissVectorStore(faiss_index=faiss.IndexFlatL2(config.dimensions))`，再 `_build_index_from_vector_store` 组 `StorageContext` 与 `VectorStoreIndex`。维度取自配置而不是硬编码——这一点与 `document_store/faiss_store.py` 里硬编码 1536 的做法不同，两套代码各自演进。

注意 `_build_faiss_index` 用 `IndexFlatL2` 建**全新的空索引**，而 `from_index` 路径下 FAISS 的加载是走索引工厂的 `_create_faiss`（从 `persist_path` 加载）。两条路径分别覆盖「新建」与「加载」，靠装饰器在运行时选择。

**Chroma 的创建链。** `_build_chroma_index` 用 `chromadb.PersistentClient(path=persist_path)` 加 `get_or_create_collection(collection_name, metadata=config.metadata)`，再把集合包成 `ChromaVectorStore`。`get_or_create` 意味着「同名集合已存在就复用」，因此重复调用不会覆盖已有数据。

**ES 的创建链。** 只需 `ElasticsearchStore(**config.store_config.model_dump())`，连接参数全部收在 `store_config` 这个子配置里。

**BM25 是三家里最特殊的一条。** `_create_bm25_retriever` 不建向量库，它要的是「节点列表」：优先从已有索引的 `docstore.docs.values()` 取，取不到就用 `_extract_nodes(config, **kwargs)`。然后按三种情形决定 `config.index`：已有索引就填进去；没有索引但 `create_index` 为真就用 `VectorStoreIndex(nodes, embed_model=MockEmbedding(embed_dim=1))` 现造一个（假 embedding，因为 BM25 不需要向量）；两者都没有就留空。最后用 `DynamicBM25Retriever(nodes=nodes, **config.model_dump())` 构造。

`DynamicBM25Retriever` 的「Dynamic」正体现在这里：BM25 的语料可以随节点变化重建，而不像向量索引那样必须一次性定型。

**混合检索器的合并逻辑。** `SimpleHybridRetriever.__init__(*retrievers)` 把子检索器存成列表，然后调 `super().__init__()`——继承自 `RAGRetriever`（也就继承自 llama-index 的 `BaseRetriever`）。真正的检索发生在 `_aretrieve`：对每个子检索器 `copy.deepcopy(query)` 一份再调 `aretrieve`。

深拷贝这一步的注释是「Prevent retriever changing query」——防止某个子检索器就地改写查询对象影响其它子检索器。合并时按 `n.node.node_id` 去重，保留**先出现**的那条（即按子检索器顺序优先），后面的同 id 节点被丢弃。

`add_nodes` 与 `persist` 都只是把调用转发给每个子检索器。没有实现 `query_total_count` / `clear`，所以混合检索器不满足「可查询」「可清空」两个能力协议——这与 `SimpleEngine` 的断言失败是同一件事的两面。

**同步路径是空的。** `RAGRetriever._retrieve` 只写了 docstring，没有实现体，因此会返回 `None`。整个链路只支持异步检索。`SimpleHybridRetriever` 同样只实现 `_aretrieve`。

## 关键取舍

**源码事实：** 工厂按配置对象的**类型**分发，而不是按配置里的枚举字段。新增一种检索器必须新增一个配置类，并在工厂的 creators 字典里注册；改一个字符串字段不会切换后端。

**源码事实：** 创建函数会就地改写传入的配置对象（`config.index = ...`）。同一个配置实例被复用两次时，第二次会带着上一次的索引进入，`get_or_build_index` 会因此跳过重建。这是「配置即参数」写法的副作用。

**源码事实：** `_val_from_config_or_kwargs` 的优先级是「配置优先、kwargs 兜底」，但只有在配置字段为 `None` 时才落到 kwargs。因此调用方无法用 kwargs 覆盖一个已设置好的配置字段。

**源码事实：** 混合检索的去重是「先到先得」，不比较 score。两路召回同一个节点时，最终分数取先遍历到的那个子检索器的结果，不是最优分数。

**架构推论：** 把「建索引」和「建检索器」拆成两个工厂，使同一份索引能被不同检索器复用（例如 FAISS 索引 + 自定义检索器），也让 `from_index` 与 `from_nodes` 两条装配路径共享创建逻辑。代价是「索引从哪来」这个决策分散在装饰器、配置字段、kwargs 三处。

**架构推论：** 混合检索只实现 `_aretrieve` 与转发 `add_nodes`/`persist`，没有实现计数与清空。因此「混合」是一个只读组合——它能查、能追加、能落盘，但不能统计总量、不能整体清空。`SimpleEngine` 的能力断言在这种组合下必然失败，两个模块的行为是自洽的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `metagpt/rag/factories/retriever.py` | `RetrieverFactory` | 配置类型到检索器创建函数的注册表 |
| `metagpt/rag/factories/retriever.py` | `RetrieverFactory.get_retriever` | 单配置直取、多配置自动包混合检索器 |
| `metagpt/rag/factories/retriever.py` | `get_or_build_index` | 已有索引则跳过构建的装饰器 |
| `metagpt/rag/factories/retriever.py` | `_create_bm25_retriever` | 从索引或节点取语料，必要时造假 embedding 索引 |
| `metagpt/rag/factories/retriever.py` | `_build_faiss_index` | 按配置维度建 FAISS 空索引 |
| `metagpt/rag/factories/retriever.py` | `_build_index_from_vector_store` | 向量库到索引的公共拼装 |
| `metagpt/rag/retrievers/hybrid_retriever.py` | `SimpleHybridRetriever` | 多检索器组合与节点去重 |
| `metagpt/rag/retrievers/hybrid_retriever.py` | `SimpleHybridRetriever._aretrieve` | 逐个子检索（深拷贝查询）、按 node_id 去重 |
| `metagpt/rag/factories/base.py` | `ConfigBasedFactory` | 按配置类型分发的基类 |

## 参考

- [MetaGPT 源码：rag/factories/retriever.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/factories/retriever.py)
- [MetaGPT 源码：rag/retrievers/hybrid_retriever.py](https://github.com/geekan/MetaGPT/blob/main/metagpt/rag/retrievers/hybrid_retriever.py)
- [LlamaIndex 文档：Retriever 组合与混合检索](https://docs.llamaindex.ai/en/stable/module_guides/querying/retriever/)

## 与主核 / AI 的关系

「按类型注册创建函数、再由统一入口分发」与主核里按存储引擎类型注册打开函数的注册表是同一种模式：都要求新增后端时只增一个注册项，而不是在分发处加分支。差别在于主核的注册发生在进程启动、且注册表内容决定能否打开既有数据；这里的注册发生在模块导入、且只影响新建实例。

对 agent 工程的实际含义有两层。其一，混合检索是「读侧组合」而非「写侧统一」：它能同时用关键词与向量召回，但新增文档时是分发给每个子检索器各写一份，若某个后端写失败不会回滚其它后端——因此混合库的一致性靠「同一批节点重复写入」的幂等性，而不是事务。其二，去重取先到先得而非最高分，意味着子检索器的顺序会实质影响结果排序；调整 `configs` 列表顺序不是无关紧要的格式化改动。

