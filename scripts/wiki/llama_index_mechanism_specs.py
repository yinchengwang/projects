# -*- coding: utf-8 -*-
"""llama-index 机制清单（阶段 2C 第二个项目）。

盘点范围：`llama-index-core/llama_index/core/` —— 这是整个 llama-index 生态的
抽象层。`llama-index-integrations/` 下有 2969 个 py 文件（各家向量库、读取器、
模型的适配），属于适配层，机制重复度高，不建页；`llama-index-instrumentation`
是独立的轻量事件包，不建页。

llama-index 与 langchain 的关键差别（决定了两者的机制簇不同）：
- langchain 的核心抽象是 `Runnable`（表达式语言），一切靠组合；
- llama-index 的核心抽象是 `Node`（带关系与元数据的节点）与 `Index`（索引结构），
  它把问题定义成「文档 → 节点 → 索引 → 检索 → 合成回答」，每一段都有独立的机制。

所以本文库的组划分沿 RAG 管道的实际阶段走：分块 → 节点 → 索引 → 检索 → 重排 →
合成 → 评测 → 存储 → 基础设施。

盘点方法：`dump_python_defs.py` 建 AST 定义索引，再按职责归并。core 下 737 个 py
文件里大量是 `__init__.py` 与加载器胶水，实际机制单元远少于文件数。

源码锚点：`llama-index@40d4f79eb9d8f68430692d0cc156483ca2d7f4f6`。
"""

GROUP_ORDER = [
    "A-分块与节点解析",
    "B-节点模型",
    "C-索引结构",
    "D-检索器",
    "E-后处理与重排",
    "F-查询引擎与回答合成",
    "G-摄取流水线与评测",
    "H-存储与索引持久化",
    "I-模型与服务上下文",
]

MECHANISM_SPECS = [
    # ============ A-分块与节点解析 ============
    {
        "feature_group": "A-分块与节点解析",
        "feature": "node-parser-base",
        "title": "节点解析器契约与 id 推导",
        "page_file": "a01-node-parser-base.md",
        "boundary": "负责解析器的抽象契约、节点 id 的生成规则与从切分片段构造节点的过程；"
                    "不负责具体切分算法（a02/a03/a04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/interface.py",
             "symbols": ["NodeParser", "TextSplitter", "MetadataAwareTextSplitter"]},
            {"path": "llama-index-core/llama_index/core/node_parser/node_utils.py",
             "symbols": ["default_id_func", "build_nodes_from_splits", "IdFuncCallable"]},
        ],
    },
    {
        "feature_group": "A-分块与节点解析",
        "feature": "sentence-splitter",
        "title": "句子切分与窗口",
        "page_file": "a02-sentence-splitter.md",
        "boundary": "负责按句子/段落优先的切分与句子窗口扩展；"
                    "不负责契约（a01）、不负责按 token 切分（a03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/text/sentence.py",
             "symbols": ["SentenceSplitter", "_Split"]},
            {"path": "llama-index-core/llama_index/core/node_parser/text/sentence_window.py",
             "symbols": ["SentenceWindowNodeParser"]},
        ],
    },
    {
        "feature_group": "A-分块与节点解析",
        "feature": "token-splitter",
        "title": "按 token 切分与代码切分",
        "page_file": "a03-token-splitter.md",
        "boundary": "负责按 token 单位切分（含分词器抽象）与按代码语言结构切分；"
                    "不负责句子切分（a02）、不负责结构感知（a04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/text/token.py",
             "symbols": ["TokenTextSplitter"]},
            {"path": "llama-index-core/llama_index/core/node_parser/text/code.py",
             "symbols": ["CodeSplitter"]},
        ],
    },
    {
        "feature_group": "A-分块与节点解析",
        "feature": "structured-parser",
        "title": "结构化文档解析",
        "page_file": "a04-structured-parser.md",
        "boundary": "负责按 Markdown/HTML/JSON 结构与表格元素解析出带父子的节点；"
                    "不负责纯文本切分（a02/a03）、不负责语义切分（a05）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/file/markdown.py",
             "symbols": ["MarkdownNodeParser"]},
            {"path": "llama-index-core/llama_index/core/node_parser/file/html.py",
             "symbols": ["HTMLNodeParser"]},
            {"path": "llama-index-core/llama_index/core/node_parser/relational/base_element.py",
             "symbols": ["BaseElementNodeParser", "Element", "TableOutput"]},
        ],
    },
    {
        "feature_group": "A-分块与节点解析",
        "feature": "semantic-splitter",
        "title": "语义切分",
        "page_file": "a05-semantic-splitter.md",
        "boundary": "负责按相邻句子的语义相似度决定切分边界；"
                    "不负责句子切分（a02）、不负责层级切分（a06）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/text/semantic_splitter.py",
             "symbols": ["SemanticSplitterNodeParser", "SentenceCombination"]},
            {"path": "llama-index-core/llama_index/core/node_parser/text/semantic_double_merging_splitter.py",
             "symbols": ["SemanticDoubleMergingSplitterNodeParser"]},
        ],
    },
    {
        "feature_group": "A-分块与节点解析",
        "feature": "hierarchical-parser",
        "title": "层级切分与父子关系",
        "page_file": "a06-hierarchical-parser.md",
        "boundary": "负责把文档同时切成多层大小并建立父子关系，以及相关的节点遍历工具；"
                    "不负责语义切分（a05）、不负责节点关系结构本身（b01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/node_parser/relational/hierarchical.py",
             "symbols": ["HierarchicalNodeParser", "get_leaf_nodes", "get_root_nodes"]},
            {"path": "llama-index-core/llama_index/core/node_parser/relational/hierarchical.py",
             "symbols": ["get_child_nodes", "get_deeper_nodes"]},
        ],
    },

    # ============ B-节点模型 ============
    {
        "feature_group": "B-节点模型",
        "feature": "node-schema",
        "title": "节点类型与关系",
        "page_file": "b01-node-schema.md",
        "boundary": "负责节点/文档的类型体系、关系枚举与元数据可见性模式；"
                    "不负责索引结构（C 组）、不负责 id 推导（a01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/schema.py",
             "symbols": ["BaseNode", "Node", "TextNode", "Document"]},
            {"path": "llama-index-core/llama_index/core/schema.py",
             "symbols": ["NodeRelationship", "RelatedNodeInfo", "MetadataMode"]},
        ],
    },
    {
        "feature_group": "B-节点模型",
        "feature": "node-multimodal",
        "title": "多模态节点与媒体资源",
        "page_file": "b02-node-multimodal.md",
        "boundary": "负责图像/索引节点的表示与媒体资源、模态枚举；"
                    "不负责文本节点（b01）、不负责多模态检索（d 组）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/schema.py",
             "symbols": ["ImageNode", "IndexNode", "MediaResource", "Modality"]},
            {"path": "llama-index-core/llama_index/core/schema.py", "symbols": ["NodeWithScore"]},
        ],
    },

    # ============ C-索引结构 ============
    {
        "feature_group": "C-索引结构",
        "feature": "index-base",
        "title": "索引基类与构建流程",
        "page_file": "c01-index-base.md",
        "boundary": "负责索引的抽象基类、插入/删除节点的通用流程与索引结构持久化入口；"
                    "不负责向量索引（c02）、不负责其它索引类型（c03/c04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/base.py",
             "symbols": ["BaseIndex"]},
            {"path": "llama-index-core/llama_index/core/indices/common/struct_store/base.py",
             "symbols": ["SQLDocumentContextBuilder", "BaseStructDatapointExtractor"]},
        ],
    },
    {
        "feature_group": "C-索引结构",
        "feature": "vector-index",
        "title": "向量索引",
        "page_file": "c02-vector-index.md",
        "boundary": "负责向量索引的构建、插入与相似度检索入口；"
                    "不负责索引基类（c01）、不负责检索器抽象（d01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/vector_store/base.py",
             "symbols": ["VectorStoreIndex"]},
            {"path": "llama-index-core/llama_index/core/indices/vector_store/base.py",
             "symbols": ["VectorStoreIndex"]},
        ],
    },
    {
        "feature_group": "C-索引结构",
        "feature": "list-and-tree-index",
        "title": "列表索引与树索引",
        "page_file": "c03-list-and-tree-index.md",
        "boundary": "负责摘要式列表索引与自底向上构建的树索引；"
                    "不负责向量索引（c02）、不负责关键词索引（c04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/list/base.py",
             "symbols": ["SummaryIndex", "ListRetrieverMode"]},
            {"path": "llama-index-core/llama_index/core/indices/tree/base.py",
             "symbols": ["TreeIndex", "TreeRetrieverMode"]},
        ],
    },
    {
        "feature_group": "C-索引结构",
        "feature": "keyword-and-docsummary-index",
        "title": "关键词索引与文档摘要索引",
        "page_file": "c04-keyword-and-docsummary-index.md",
        "boundary": "负责从节点抽关键词建表检索、以及为每篇文档生成摘要并据摘要选文档；"
                    "不负责列表/树索引（c03）、不负责属性图索引（c05）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/keyword_table/base.py",
             "symbols": ["KeywordTableIndex", "BaseKeywordTableIndex", "KeywordTableRetrieverMode"]},
            {"path": "llama-index-core/llama_index/core/indices/document_summary/base.py",
             "symbols": ["DocumentSummaryIndex", "DocumentSummaryRetrieverMode"]},
        ],
    },
    {
        "feature_group": "C-索引结构",
        "feature": "property-graph-index",
        "title": "属性图索引",
        "page_file": "c05-property-graph-index.md",
        "boundary": "负责从文档抽实体与关系构成属性图并支持多种检索路径；"
                    "不负责图存储契约（h03）、不负责关键词索引（c04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/property_graph/base.py",
             "symbols": ["PropertyGraphIndex"]},
            {"path": "llama-index-core/llama_index/core/graph_stores/types.py",
             "symbols": ["PropertyGraphStore", "LabelledPropertyGraph", "EntityNode"]},
        ],
    },

    # ============ D-检索器 ============
    {
        "feature_group": "D-检索器",
        "feature": "retriever-base",
        "title": "检索器契约",
        "page_file": "d01-retriever-base.md",
        "boundary": "负责检索器的抽象契约与同步/异步检索的默认降级；"
                    "不负责各类索引的检索实现（d02）、不负责自动检索（d03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/base/base_retriever.py",
             "symbols": ["BaseRetriever"]},
            {"path": "llama-index-core/llama_index/core/schema.py", "symbols": ["NodeWithScore"]},
        ],
    },
    {
        "feature_group": "D-检索器",
        "feature": "index-retrievers",
        "title": "各类索引的检索实现",
        "page_file": "d02-index-retrievers.md",
        "boundary": "负责列表/树/关键词/文档摘要四种索引各自的检索器与检索模式；"
                    "不负责检索器契约（d01）、不负责向量检索的相似度计算（c02）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/indices/list/retrievers.py",
             "symbols": ["SummaryIndexRetriever", "SummaryIndexEmbeddingRetriever"]},
            {"path": "llama-index-core/llama_index/core/indices/keyword_table/retrievers.py",
             "symbols": ["KeywordTableGPTRetriever", "KeywordTableRAKERetriever"]},
            {"path": "llama-index-core/llama_index/core/indices/document_summary/retrievers.py",
             "symbols": ["DocumentSummaryIndexLLMRetriever", "DocumentSummaryIndexEmbeddingRetriever"]},
        ],
    },
    {
        "feature_group": "D-检索器",
        "feature": "auto-retriever",
        "title": "自动检索与选择器",
        "page_file": "d03-auto-retriever.md",
        "boundary": "负责按自然语言描述在多个检索器/工具之间选择；"
                    "不负责单个检索器（d02）、不负责路由查询引擎（f03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/base/base_auto_retriever.py",
             "symbols": ["BaseAutoRetriever"]},
            {"path": "llama-index-core/llama_index/core/base/base_selector.py",
             "symbols": ["BaseSelector", "SingleSelection", "MultiSelection"]},
        ],
    },

    # ============ E-后处理与重排 ============
    {
        "feature_group": "E-后处理与重排",
        "feature": "node-postprocessor",
        "title": "节点后处理与相似度裁剪",
        "page_file": "e01-node-postprocessor.md",
        "boundary": "负责检索结果的后处理契约与基础过滤器（关键词、相似度、前后邻接扩展、长上下文重排）；"
                    "不负责 LLM 重排（e02）、不负责时序权重（e03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/postprocessor/node.py",
             "symbols": ["KeywordNodePostprocessor", "SimilarityPostprocessor", "LongContextReorder"]},
            {"path": "llama-index-core/llama_index/core/postprocessor/node.py",
             "symbols": ["PrevNextNodePostprocessor", "AutoPrevNextNodePostprocessor"]},
        ],
    },
    {
        "feature_group": "E-后处理与重排",
        "feature": "llm-rerank",
        "title": "LLM 重排",
        "page_file": "e02-llm-rerank.md",
        "boundary": "负责用 LLM 对候选节点打分/排序的几种实现；"
                    "不负责基础过滤（e01）、不负责向量模型重排（e03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/postprocessor/llm_rerank.py",
             "symbols": ["LLMRerank"]},
            {"path": "llama-index-core/llama_index/core/postprocessor/structured_llm_rerank.py",
             "symbols": ["StructuredLLMRerank", "DocumentWithRelevance",
                         "default_parse_structured_choice_select_answer"]},
            {"path": "llama-index-core/llama_index/core/postprocessor/rankGPT_rerank.py",
             "symbols": ["RankGPTRerank", "get_default_llm"]},
        ],
    },
    {
        "feature_group": "E-后处理与重排",
        "feature": "temporal-and-embedding-rerank",
        "title": "时序权重与向量模型重排",
        "page_file": "e03-temporal-and-embedding-rerank.md",
        "boundary": "负责按时间衰减的权重调整与基于交叉编码器的重排，以及句级优化器；"
                    "不负责 LLM 重排（e02）、不负责基础过滤（e01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/postprocessor/node_recency.py",
             "symbols": ["TimeWeightedPostprocessor", "FixedRecencyPostprocessor",
                         "EmbeddingRecencyPostprocessor"]},
            {"path": "llama-index-core/llama_index/core/postprocessor/sbert_rerank.py",
             "symbols": ["SentenceTransformerRerank"]},
            {"path": "llama-index-core/llama_index/core/postprocessor/optimizer.py",
             "symbols": ["SentenceEmbeddingOptimizer"]},
        ],
    },

    # ============ F-查询引擎与回答合成 ============
    {
        "feature_group": "F-查询引擎与回答合成",
        "feature": "query-engine-base",
        "title": "查询引擎契约",
        "page_file": "f01-query-engine-base.md",
        "boundary": "负责查询引擎的抽象契约与从检索器组装查询引擎；"
                    "不负责合成器（f02）、不负责路由与子问题（f03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/base/base_query_engine.py",
             "symbols": ["BaseQueryEngine"]},
            {"path": "llama-index-core/llama_index/core/query_engine/retriever_query_engine.py",
             "symbols": ["RetrieverQueryEngine"]},
        ],
    },
    {
        "feature_group": "F-查询引擎与回答合成",
        "feature": "response-synthesizers",
        "title": "回答合成策略",
        "page_file": "f02-response-synthesizers.md",
        "boundary": "负责把多个节点的片段合成一段回答的几种策略（逐条精炼、紧凑精炼、树状归约）；"
                    "不负责查询引擎（f01）、不负责引用追踪（f04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/response_synthesizers/base.py",
             "symbols": ["BaseSynthesizer"]},
            {"path": "llama-index-core/llama_index/core/response_synthesizers/refine.py",
             "symbols": ["Refine", "DefaultRefineProgram", "StructuredRefineResponse"]},
            {"path": "llama-index-core/llama_index/core/response_synthesizers/tree_summarize.py",
             "symbols": ["TreeSummarize"]},
        ],
    },
    {
        "feature_group": "F-查询引擎与回答合成",
        "feature": "router-and-subquestion",
        "title": "路由查询与子问题分解",
        "page_file": "f03-router-and-subquestion.md",
        "boundary": "负责在多个查询引擎/检索器间路由、以及把复杂问题拆成子问题并合并答案；"
                    "不负责查询引擎契约（f01）、不负责合成策略（f02）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/query_engine/router_query_engine.py",
             "symbols": ["RouterQueryEngine", "RetrieverRouterQueryEngine",
                         "ToolRetrieverRouterQueryEngine"]},
            {"path": "llama-index-core/llama_index/core/query_engine/sub_question_query_engine.py",
             "symbols": ["SubQuestionQueryEngine", "SubQuestionAnswerPair"]},
        ],
    },
    {
        "feature_group": "F-查询引擎与回答合成",
        "feature": "citation-and-retry",
        "title": "引用追踪与重试",
        "page_file": "f04-citation-and-retry.md",
        "boundary": "负责给回答标注来源片段、以及在结果不满意时重试/变换查询；"
                    "不负责合成策略（f02）、不负责路由（f03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/query_engine/citation_query_engine.py",
             "symbols": ["CitationQueryEngine"]},
            {"path": "llama-index-core/llama_index/core/query_engine/retry_query_engine.py",
             "symbols": ["RetryQueryEngine"]},
            {"path": "llama-index-core/llama_index/core/query_engine/transform_query_engine.py",
             "symbols": ["TransformQueryEngine"]},
        ],
    },

    # ============ G-摄取流水线与评测 ============
    {
        "feature_group": "G-摄取流水线与评测",
        "feature": "ingestion-pipeline",
        "title": "摄取流水线与文档去重",
        "page_file": "g01-ingestion-pipeline.md",
        "boundary": "负责把一串变换（切分/抽元数据/嵌入）按流水线跑完并处理文档级去重；"
                    "不负责分块算法（A 组）、不负责存储契约（h01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/ingestion/pipeline.py",
             "symbols": ["IngestionPipeline", "DocstoreStrategy", "run_transformations"]},
            {"path": "llama-index-core/llama_index/core/ingestion/pipeline.py",
             "symbols": ["get_transformation_hash", "remove_unstable_values"]},
        ],
    },
    {
        "feature_group": "G-摄取流水线与评测",
        "feature": "evaluation",
        "title": "检索与回答的评测",
        "page_file": "g02-evaluation.md",
        "boundary": "负责评测器的抽象契约与几类典型指标（忠实度、相关度、正确性、语义相似）、以及成对比较；"
                    "不负责摄取（g01）、不负责数据集生成（g03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/evaluation/base.py",
             "symbols": ["BaseEvaluator", "EvaluationResult"]},
            {"path": "llama-index-core/llama_index/core/evaluation/faithfulness.py",
             "symbols": ["FaithfulnessEvaluator"]},
            {"path": "llama-index-core/llama_index/core/evaluation/pairwise.py",
             "symbols": ["PairwiseComparisonEvaluator", "EvaluationSource"]},
        ],
    },
    {
        "feature_group": "G-摄取流水线与评测",
        "feature": "batch-eval-and-dataset",
        "title": "批量评测与数据集生成",
        "page_file": "g03-batch-eval-and-dataset.md",
        "boundary": "负责批量跑评测的并发组织与从文档自动生成问答数据集；"
                    "不负责单个评测器（g02）、不负责流水线（g01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/evaluation/batch_runner.py",
             "symbols": ["BatchEvalRunner", "eval_response_worker", "eval_worker"]},
            {"path": "llama-index-core/llama_index/core/evaluation/dataset_generation.py",
             "symbols": ["DatasetGenerator", "QueryResponseDataset"]},
        ],
    },

    # ============ H-存储与索引持久化 ============
    {
        "feature_group": "H-存储与索引持久化",
        "feature": "storage-context",
        "title": "存储上下文与三类存储契约",
        "page_file": "h01-storage-context.md",
        "boundary": "负责存储上下文的组装、以及键值存储/文档存储/索引存储三者的契约；"
                    "不负责文档存储的去重语义（h02）、不负责图存储（h03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/storage/storage_context.py",
             "symbols": ["StorageContext"]},
            {"path": "llama-index-core/llama_index/core/storage/kvstore/types.py",
             "symbols": ["BaseKVStore", "BaseInMemoryKVStore", "MutableMappingKVStore"]},
        ],
    },
    {
        "feature_group": "H-存储与索引持久化",
        "feature": "docstore",
        "title": "文档存储与引用信息",
        "page_file": "h02-docstore.md",
        "boundary": "负责文档存储的契约、按文档引用清理节点、以及键值存储上的文档存储实现；"
                    "不负责索引存储（h01）、不负责去重策略（g01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/storage/docstore/types.py",
             "symbols": ["BaseDocumentStore", "RefDocInfo"]},
            {"path": "llama-index-core/llama_index/core/storage/docstore/keyval_docstore.py",
             "symbols": ["KVDocumentStore"]},
        ],
    },
    {
        "feature_group": "H-存储与索引持久化",
        "feature": "graph-and-vector-store",
        "title": "图存储与向量存储契约",
        "page_file": "h03-graph-and-vector-store.md",
        "boundary": "负责属性图存储与向量存储的抽象契约、查询模式与元数据过滤表达式；"
                    "不负责索引结构（C 组）、不负责文档存储（h02）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/graph_stores/types.py",
             "symbols": ["GraphStore", "PropertyGraphStore", "LabelledNode", "Relation"]},
            {"path": "llama-index-core/llama_index/core/vector_stores/types.py",
             "symbols": ["VectorStore", "BasePydanticVectorStore", "VectorStoreQuery"]},
            {"path": "llama-index-core/llama_index/core/vector_stores/types.py",
             "symbols": ["MetadataFilters", "MetadataFilter", "VectorStoreQueryMode"]},
        ],
    },

    # ============ I-模型与服务上下文 ============
    {
        "feature_group": "I-模型与服务上下文",
        "feature": "llm-and-embedding-base",
        "title": "模型与嵌入的抽象",
        "page_file": "i01-llm-and-embedding-base.md",
        "boundary": "负责 LLM 与嵌入模型的抽象契约、消息到提示的转换与补全/聊天两种接口的互转；"
                    "不负责全局设置（i02）、不负责多模态嵌入（i03）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/base/llms/base.py",
             "symbols": ["BaseLLM", "ChatResponse", "CompletionResponse"]},
            {"path": "llama-index-core/llama_index/core/base/llms/generic_utils.py",
             "symbols": ["messages_to_prompt", "prompt_to_messages",
                         "chat_response_to_completion_response"]},
            {"path": "llama-index-core/llama_index/core/base/embeddings/base.py",
             "symbols": ["BaseEmbedding", "similarity", "SimilarityMode"]},
        ],
    },
    {
        "feature_group": "I-模型与服务上下文",
        "feature": "settings",
        "title": "全局设置与解析注册",
        "page_file": "i02-settings.md",
        "boundary": "负责全局设置容器（模型、嵌入、切分器）与节点解析器注册表；"
                    "不负责模型契约（i01）、不负责速率限制（i04）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/settings.py", "symbols": ["_Settings"]},
            {"path": "llama-index-core/llama_index/core/node_parser/loading.py",
             "symbols": ["load_parser"]},
        ],
    },
    {
        "feature_group": "I-模型与服务上下文",
        "feature": "multimodal-embedding",
        "title": "多模态嵌入与稀疏嵌入",
        "page_file": "i03-multimodal-embedding.md",
        "boundary": "负责文本/图像混合嵌入与稀疏向量嵌入的契约；"
                    "不负责基础嵌入（i01）、不负责多模态节点（b02）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/embeddings/multi_modal_base.py",
             "symbols": ["MultiModalEmbedding"]},
            {"path": "llama-index-core/llama_index/core/base/embeddings/base_sparse.py",
             "symbols": ["BaseSparseEmbedding", "sparse_similarity"]},
            {"path": "llama-index-core/llama_index/core/embeddings/pooling.py",
             "symbols": ["Pooling"]},
        ],
    },
    {
        "feature_group": "I-模型与服务上下文",
        "feature": "rate-limiter-and-async",
        "title": "速率限制与异步工具",
        "page_file": "i04-rate-limiter-and-async.md",
        "boundary": "负责出站请求的速率限制实现与批量并发/重试工具；"
                    "不负责全局设置（i02）、不负责模型契约（i01）。",
        "refs": [
            {"path": "llama-index-core/llama_index/core/rate_limiter.py",
             "symbols": ["BaseRateLimiter", "TokenBucketRateLimiter", "SlidingWindowRateLimiter"]},
            {"path": "llama-index-core/llama_index/core/async_utils.py",
             "symbols": ["run_async_tasks", "batch_gather", "run_jobs"]},
            {"path": "llama-index-core/llama_index/core/utils.py",
             "symbols": ["retry_on_exceptions_with_backoff", "get_tokenizer", "truncate_text"]},
        ],
    },
]
