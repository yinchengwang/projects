---
project: llama-index
page_type: overview
display_name: LlamaIndex
repo: https://github.com/run-llama/llama_index
license: MIT
language: Python
stars: 52990
stars_as_of: 2026-11-23
refs:
  - path: llama-index-core/llama_index/core/schema.py
    symbols: [BaseNode, TextNode, NodeRelationship]
  - path: llama-index-core/llama_index/core/indices/vector_store/base.py
    symbols: [VectorStoreIndex]
  - path: llama-index-core/llama_index/core/base/base_retriever.py
    symbols: [BaseRetriever]
  - path: llama-index-core/llama_index/core/response_synthesizers/base.py
    symbols: [BaseSynthesizer]
  - path: llama-index-core/llama_index/core/storage/docstore/types.py
    symbols: [BaseDocumentStore]
---
# LlamaIndex

## 一句话职责

面向 RAG 的数据框架：它把「文档 → 节点 → 索引 → 检索 → 重排 → 合成回答」这条链上的每一步都做成可替换的组件，并用一层 `Node` 数据模型把文本、元数据、关系与模态统一起来。

## 机制是什么

这个项目与 langchain 的区别值得先说清，因为它决定了两者的机制簇不同。

**langchain 的核心抽象是「可运行对象」**（`Runnable`），一切靠组合：把它的一切都变成能 `|` 起来的管道。
**llama-index 的核心抽象是「节点」与「索引」**，它把问题按 RAG 管道的实际阶段切分，每一段都有独立的机制：分块器、节点解析器、索引结构、检索器、后处理器、合成器、评测器。管道是**按阶段的纵向分层**，而不是一个统一的表达式类型。

这带来两个系统性后果：

1. **索引是一等公民**。它有独立的存储契约（索引存储 + 文档存储 + 键值存储三层）、独立的构建流程、独立的自定义入口。所以「换一种索引结构」是框架级操作，不是换一个组件。
2. **节点携带关系**。`NodeRelationship` 让节点之间能表达「父/子/前/后/源文档」，这是层级检索、窗口检索、邻接扩展能够存在的前提。langchain 的 `Document` 只有元数据，没有关系类型。

本文库按 RAG 管道的实际阶段组织：**9 个机制领域、共 33 篇机制页**。

## 怎么实现的

下表的 9 个领域按下面的顺序排列：A 分块与节点解析 → B 节点模型 → C 索引结构 → D 检索器 → E 后处理与重排 → F 查询引擎与回答合成 → G 摄取流水线与评测 → H 存储与索引持久化 → I 模型与服务上下文。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`llama-index-core/llama_index/core/`。本仓是 monorepo，`llama-index-integrations/` 下有近 3000 个 py 文件（各家向量库、读取器、模型的适配），机制重复度高，不建页；`llama-index-instrumentation` 是独立的轻量事件包，不建页。

## 机制导航（33 篇 / 9 组）

### A-分块与节点解析（6 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-node-parser-base.md`](a01-node-parser-base.md) | 节点解析器契约与 id 推导 |
| [`a02-sentence-splitter.md`](a02-sentence-splitter.md) | 句子切分与窗口 |
| [`a03-token-splitter.md`](a03-token-splitter.md) | 按 token 切分与代码切分 |
| [`a04-structured-parser.md`](a04-structured-parser.md) | 结构化文档解析 |
| [`a05-semantic-splitter.md`](a05-semantic-splitter.md) | 语义切分 |
| [`a06-hierarchical-parser.md`](a06-hierarchical-parser.md) | 层级切分与父子关系 |

### B-节点模型（2 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-node-schema.md`](b01-node-schema.md) | 节点类型与关系 |
| [`b02-node-multimodal.md`](b02-node-multimodal.md) | 多模态节点与媒体资源 |

### C-索引结构（5 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-index-base.md`](c01-index-base.md) | 索引基类与构建流程 |
| [`c02-vector-index.md`](c02-vector-index.md) | 向量索引 |
| [`c03-list-and-tree-index.md`](c03-list-and-tree-index.md) | 列表索引与树索引 |
| [`c04-keyword-and-docsummary-index.md`](c04-keyword-and-docsummary-index.md) | 关键词索引与文档摘要索引 |
| [`c05-property-graph-index.md`](c05-property-graph-index.md) | 属性图索引 |

### D-检索器（3 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-retriever-base.md`](d01-retriever-base.md) | 检索器契约 |
| [`d02-index-retrievers.md`](d02-index-retrievers.md) | 各类索引的检索实现 |
| [`d03-auto-retriever.md`](d03-auto-retriever.md) | 自动检索与选择器 |

### E-后处理与重排（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-node-postprocessor.md`](e01-node-postprocessor.md) | 节点后处理与相似度裁剪 |
| [`e02-llm-rerank.md`](e02-llm-rerank.md) | LLM 重排 |
| [`e03-temporal-and-embedding-rerank.md`](e03-temporal-and-embedding-rerank.md) | 时序权重与向量模型重排 |

### F-查询引擎与回答合成（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-query-engine-base.md`](f01-query-engine-base.md) | 查询引擎契约 |
| [`f02-response-synthesizers.md`](f02-response-synthesizers.md) | 回答合成策略 |
| [`f03-router-and-subquestion.md`](f03-router-and-subquestion.md) | 路由查询与子问题分解 |
| [`f04-citation-and-retry.md`](f04-citation-and-retry.md) | 引用追踪与重试 |

### G-摄取流水线与评测（3 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-ingestion-pipeline.md`](g01-ingestion-pipeline.md) | 摄取流水线与文档去重 |
| [`g02-evaluation.md`](g02-evaluation.md) | 检索与回答的评测 |
| [`g03-batch-eval-and-dataset.md`](g03-batch-eval-and-dataset.md) | 批量评测与数据集生成 |

### H-存储与索引持久化（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-storage-context.md`](h01-storage-context.md) | 存储上下文与三类存储契约 |
| [`h02-docstore.md`](h02-docstore.md) | 文档存储与引用信息 |
| [`h03-graph-and-vector-store.md`](h03-graph-and-vector-store.md) | 图存储与向量存储契约 |

### I-模型与服务上下文（4 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-llm-and-embedding-base.md`](i01-llm-and-embedding-base.md) | 模型与嵌入的抽象 |
| [`i02-settings.md`](i02-settings.md) | 全局设置与解析注册 |
| [`i03-multimodal-embedding.md`](i03-multimodal-embedding.md) | 多模态嵌入与稀疏嵌入 |
| [`i04-rate-limiter-and-async.md`](i04-rate-limiter-and-async.md) | 速率限制与异步工具 |

## 关键取舍

**源码事实：** 核心抽象是 `Node` 而非「可运行对象」。`NodeRelationship` 提供父/子/前/后/源文档的显式关系，`RelatedNodeInfo` 是关系的载体。

**源码事实：** 索引有独立的三层存储契约：索引存储（存索引结构）、文档存储（存节点）、键值存储（前两者的底层）。`RefDocInfo` 是「文档 → 它产出的节点 id」的反向索引——删除文档时靠它精确清理。

**源码事实：** 切分采用「双阶段」：先按语义边界降级切碎，再按 token 预算合并并允许重叠。遇到不可再分的超长片段（CJK/emoji 单字符场景）保留并抛错，而不是递归（注释明确写递归会 `RecursionError`）。

**源码事实：** 检索结果可以经过多级后处理：基础过滤（关键词/相似度/邻接扩展）、LLM 重排（三种策略）、时序权重、交叉编码器重排。这些是**独立可叠加的组件**而非一个综合打分器。

**架构推论：** 「索引是一等公民」意味着换索引结构是框架级操作。代价是每种索引都要维护自己的一整套（构建器 + 检索器 + 检索模式），收益是不同索引的语义差异能被真实表达（向量、关键词、摘要树、属性图的检索语义本就不同）。

**架构推论：** 「节点携带关系」是很多 RAG 高级技巧的地基。句子窗口（检索用小节点、生成用大窗口）、层级检索（先选大块再细化）、邻接扩展（前后文补充）全都依赖关系能表达。没有关系类型，这些技巧只能靠重复文本或外部映射表。

**架构推论：** 后处理设计成「可叠加的多级」而非「一个打分器」，反映的是 RAG 调优的现实：不同数据集需要不同的过滤与重排组合，把它们拆成组件才能逐项实验。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `llama-index-core/llama_index/core/schema.py` | `BaseNode` | 节点数据模型与关系 |
| `llama-index-core/llama_index/core/schema.py` | `NodeRelationship` | 父子/前后/源文档关系枚举 |
| `llama-index-core/llama_index/core/node_parser/text/sentence.py` | `SentenceSplitter` | 句子优先的双阶段切分 |
| `llama-index-core/llama_index/core/node_parser/relational/hierarchical.py` | `HierarchicalNodeParser` | 多层切分与父子关系 |
| `llama-index-core/llama_index/core/indices/vector_store/base.py` | `VectorStoreIndex` | 向量索引 |
| `llama-index-core/llama_index/core/indices/document_summary/base.py` | `DocumentSummaryIndex` | 文档摘要索引 |
| `llama-index-core/llama_index/core/base/base_retriever.py` | `BaseRetriever` | 检索器契约 |
| `llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py` | `StructuredLLMRerank` | LLM 重排 |
| `llama-index-core/llama_index/core/response_synthesizers/refine.py` | `Refine` | 逐条精炼合成 |
| `llama-index-core/llama_index/core/ingestion/pipeline.py` | `IngestionPipeline` | 摄取流水线与变换缓存 |
| `llama-index-core/llama_index/core/storage/storage_context.py` | `StorageContext` | 三类存储的组装 |
| `llama-index-core/llama_index/core/vector_stores/types.py` | `MetadataFilters` | 元数据过滤表达式 |

## 参考

- [llama-index 源码仓库](https://github.com/run-llama/llama_index)
- [llama-index 文档](https://docs.llamaindex.ai/)
- [llama-index 文档：索引与检索](https://docs.llamaindex.ai/en/stable/module_guides/indexing/)
- [llama-index 文档：节点与切分器](https://docs.llamaindex.ai/en/stable/module_guides/loading/node_parsers/)
- [llama-index 文档：查询引擎](https://docs.llamaindex.ai/en/stable/module_guides/deploying/query_engine/)

## 与主核 / AI 的关系

llama-index 与主核（多模态数据库内核）的交集比 langchain 更深，因为它显式建模了**索引与存储的分层**，这正好是数据库内核的核心领地。

**三層存储契约 vs 数据库的物理/逻辑分层。** llama-index 把索引结构、节点、键值存储分成三层独立契约，与主核的「表 → 索引 → 页」分层结构同构。差别在**谁保证一致性**：主核靠事务，llama-index 靠调用方顺序。`RefDocInfo` 这个「文档 → 节点 id」的反向索引在主核里对应的是「二级索引与基表的同步」——数据库用 MVCC 与索引维护保证，RAG 框架只能靠删除时手工按记录清理。这是同一问题在两种一致性模型下的两种答案。

**节点关系 vs 数据库的外键与层级。** `NodeRelationship` 让节点能表达父子/前后/源文档，与数据库的外键、B-tree 的兄弟指针同构。但 RAG 的关系是**弱约束**：没有级联删除、没有引用完整性检查。这样做的收益是灵活（切分器可以随意造关系），代价是关系可以不一致（父节点被删了但子节点仍指向它）。

**句子窗口与层级检索 vs 主核的页内记录与索引条目。** 「检索用小单位、读取用大单位」这条分离在主核里是「索引条目 ≠ 数据页」。llama-index 把它显式做成两种节点 + 一个后处理器，主核把它隐含在存储布局里。显式化让它可以被逐项调优，隐含化让数据库可以对上层透明。

**摄取流水线的变换缓存**值得单独比：`get_transformation_hash` + `remove_unstable_values` 在算缓存 key 时**主动排除不稳定字段**。主核里对应的是「查询计划的规范化」——同样要排除不影响结果的差异（如表达式顺序），才能正确复用。这条经验是通用的：**任何基于哈希的缓存都必须显式定义「什么算相同」。**
