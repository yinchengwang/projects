# -*- coding: utf-8 -*-
"""graphrag 机制清单（阶段 2C 第五个项目）。

盘点范围：`packages/` 下的 8 个包共 423 个 py 文件。`tests/`、`unified-search-app/`、
`docs/`、`scripts/` 不建页。

graphrag 与 2C 前四个项目的根本差别是**它的中心是「建索引」而不是「编排调用」**：
- langchain / llama-index / haystack 的 RAG 是「检索 + 生成」，图是可选的加分项；
- lightrag 的图是从文档抽出来的实体关系图，查询时沿图扩展；
- graphrag 的图是**离线索引产物**：它先把文档抽成实体关系图、再用 Leiden 算法做层次
  社区划分、再给每个社区生成一份报告，查询时检索的是**社区报告**而不是原始文本块。

这个「社区报告」是 graphrag 的核心贡献：它把「全局性问题」（这个数据集的主要主题是
什么）从「检索若干文本块让模型总结」变成「检索已经总结好的社区报告」。代价是索引
成本高（每个社区一次 LLM 调用）且增量更新困难。

本文库的组划分因此不同：A 配置与数据模型、B 索引工作流、C 图算法与社区、D 查询、
E LLM 层（独立的 graphrag-llm 包）、F 独立子包（cache/storage/vectors/chunking/input）、
G 调用面与回调、H 提示词。

源码锚点：`graphrag@bc7c828d23f50f030f048eaf6d2b3ebe5e537958`。
"""

GROUP_ORDER = [
    "A-配置与数据模型",
    "B-索引工作流",
    "C-图算法与社区",
    "D-查询",
    "E-LLM 层",
    "F-独立子包",
    "G-调用面与回调",
]

MECHANISM_SPECS = [
    # ============ A-配置与数据模型 ============
    {
        "feature_group": "A-配置与数据模型",
        "feature": "graph-rag-config",
        "title": "总配置结构",
        "page_file": "a01-graph-rag-config.md",
        "boundary": "负责顶层配置的字段组织（模型、存储、缓存、各工作流参数）与默认值来源；"
                    "不负责枚举（a02）、不负责数据模型（a03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/config/models/graph_rag_config.py",
             "symbols": ["GraphRagConfig"]},
            {"path": "packages/graphrag/graphrag/config/defaults.py",
             "symbols": ["ChunkingDefaults", "ExtractGraphDefaults", "ClusterGraphDefaults"]},
            {"path": "packages/graphrag/graphrag/config/load_config.py", "symbols": ["load_config"]},
        ],
    },
    {
        "feature_group": "A-配置与数据模型",
        "feature": "config-enums",
        "title": "枚举与索引方法",
        "page_file": "a02-config-enums.md",
        "boundary": "负责索引方法与检索方法的枚举、以及它们的组合约束；"
                    "不负责总配置（a01）、不负责数据模型（a03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/config/enums.py",
             "symbols": ["SearchMethod", "IndexingMethod", "AsyncType", "ModularityMetric"]},
            {"path": "packages/graphrag/graphrag/config/enums.py",
             "symbols": ["NounPhraseExtractorType", "ReportingType"]},
        ],
    },
    {
        "feature_group": "A-配置与数据模型",
        "feature": "data-model",
        "title": "索引产物数据模型",
        "page_file": "a03-data-model.md",
        "boundary": "负责六类索引产物（文档、文本单元、实体、关系、社区、社区报告）的字段与关联；"
                    "不负责 DataFrame 转换（a04）、不负责图算法（C 组）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/data_model/entity.py", "symbols": ["Entity"]},
            {"path": "packages/graphrag/graphrag/data_model/relationship.py",
             "symbols": ["Relationship"]},
            {"path": "packages/graphrag/graphrag/data_model/community_report.py",
             "symbols": ["CommunityReport"]},
            {"path": "packages/graphrag/graphrag/data_model/text_unit.py", "symbols": ["TextUnit"]},
        ],
    },
    {
        "feature_group": "A-配置与数据模型",
        "feature": "row-transformers",
        "title": "DataFrame 行转换与强类型视图",
        "page_file": "a04-row-transformers.md",
        "boundary": "负责把存储里的 DataFrame 行转成强类型模型、以及各产物的类型化访问器；"
                    "不负责模型定义（a03）、不负责存储实现（F 组）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/data_model/row_transformers.py",
             "symbols": ["transform_entity_row", "transform_relationship_row",
                         "transform_community_report_row"]},
            {"path": "packages/graphrag/graphrag/data_model/dfs.py",
             "symbols": ["entities_typed", "relationships_typed", "community_reports_typed"]},
            {"path": "packages/graphrag/graphrag/data_model/dfs.py", "symbols": ["split_list_column"]},
        ],
    },

    # ============ B-索引工作流 ============
    {
        "feature_group": "B-索引工作流",
        "feature": "chunking-workflow",
        "title": "文档分块工作流",
        "page_file": "b01-chunking-workflow.md",
        "boundary": "负责把输入文档切成文本单元（chunk）并写入标准产物；"
                    "不负责载入（b02）、不负责抽取（b03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/workflows/create_base_text_units.py",
             "symbols": ["create_base_text_units", "chunk_document"]},
            {"path": "packages/graphrag/graphrag/index/text_splitting/text_splitting.py",
             "symbols": ["TokenTextSplitter", "split_single_text_on_tokens"]},
            {"path": "packages/graphrag/graphrag/index/workflows/load_input_documents.py",
             "symbols": ["load_input_documents"]},
        ],
    },
    {
        "feature_group": "B-索引工作流",
        "feature": "graph-extraction",
        "title": "实体关系抽取工作流",
        "page_file": "b02-graph-extraction.md",
        "boundary": "负责从文本单元抽实体与关系（LLM 路径与 NLP 路径）、以及描述摘要；"
                    "不负责分块（b01）、不负责合并成图（b03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/workflows/extract_graph.py",
             "symbols": ["extract_graph", "get_summarized_entities_relationships"]},
            {"path": "packages/graphrag/graphrag/index/workflows/extract_graph_nlp.py",
             "symbols": ["extract_graph_nlp"]},
            {"path": "packages/graphrag/graphrag/prompts/index/extract_graph.py",
             "symbols": ["GRAPH_EXTRACTION_PROMPT", "CONTINUE_PROMPT", "LOOP_PROMPT"]},
        ],
    },
    {
        "feature_group": "B-索引工作流",
        "feature": "graph-finalize",
        "title": "图收尾与度数计算",
        "page_file": "b03-graph-finalize.md",
        "boundary": "负责把抽取结果合并成最终图、计算度数与边权、以及写快照；"
                    "不负责抽取（b02）、不负责社区划分（b04）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/workflows/finalize_graph.py",
             "symbols": ["finalize_graph", "_build_degree_map"]},
            {"path": "packages/graphrag/graphrag/index/operations/finalize_entities.py",
             "symbols": ["finalize_entities"]},
            {"path": "packages/graphrag/graphrag/index/operations/compute_edge_combined_degree.py",
             "symbols": ["compute_edge_combined_degree"]},
            {"path": "packages/graphrag/graphrag/graphs/edge_weights.py",
             "symbols": ["calculate_pmi_edge_weights", "calculate_rrf_edge_weights"]},
        ],
    },
    {
        "feature_group": "B-索引工作流",
        "feature": "communities-workflow",
        "title": "社区划分与报告生成",
        "page_file": "b04-communities-workflow.md",
        "boundary": "负责 Leiden 层次社区划分、社区报告生成与收尾；"
                    "不负责图收尾（b03）、不负责图算法细节（C 组）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/workflows/create_communities.py",
             "symbols": ["create_communities"]},
            {"path": "packages/graphrag/graphrag/index/workflows/create_community_reports.py",
             "symbols": ["create_community_reports", "_prep_nodes", "_prep_edges"]},
            {"path": "packages/graphrag/graphrag/index/operations/finalize_community_reports.py",
             "symbols": ["finalize_community_reports"]},
        ],
    },
    {
        "feature_group": "B-索引工作流",
        "feature": "embeddings-and-final",
        "title": "文本嵌入与最终产物",
        "page_file": "b05-embeddings-and-final.md",
        "boundary": "负责给文本单元/实体/社区报告生成嵌入、以及拼装最终产物表；"
                    "不负责社区（b04）、不负责流水线编排（b06）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/workflows/generate_text_embeddings.py",
             "symbols": ["generate_text_embeddings", "EmbeddingFieldConfig"]},
            {"path": "packages/graphrag/graphrag/index/workflows/create_final_text_units.py",
             "symbols": ["create_final_text_units"]},
            {"path": "packages/graphrag/graphrag/index/workflows/create_final_documents.py",
             "symbols": ["create_final_documents"]},
        ],
    },
    {
        "feature_group": "B-索引工作流",
        "feature": "pipeline-run",
        "title": "流水线执行与增量更新",
        "page_file": "b06-pipeline-run.md",
        "boundary": "负责工作流图的执行、统计与增量更新链路；"
                    "不负责单个工作流（b01-b05）、不负责调用面（G 组）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/run/run_pipeline.py",
             "symbols": ["run_pipeline", "_run_pipeline"]},
            {"path": "packages/graphrag/graphrag/index/workflows/factory.py",
             "symbols": ["create_pipeline"]},
            {"path": "packages/graphrag/graphrag/index/workflows/update_entities_relationships.py",
             "symbols": ["update_entities_relationships"]},
            {"path": "packages/graphrag/graphrag/index/workflows/update_text_units.py",
             "symbols": ["update_text_units"]},
        ],
    },

    # ============ C-图算法与社区 ============
    {
        "feature_group": "C-图算法与社区",
        "feature": "hierarchical-leiden",
        "title": "层次 Leiden 社区划分",
        "page_file": "c01-hierarchical-leiden.md",
        "boundary": "负责层次化 Leiden 社区划分的实现与层级产出；"
                    "不负责模块度（c02）、不负责大组件（c03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/graphs/hierarchical_leiden.py",
             "symbols": ["hierarchical_leiden", "first_level_hierarchical_clustering"]},
            {"path": "packages/graphrag/graphrag/graphs/hierarchical_leiden.py",
             "symbols": ["final_level_hierarchical_clustering"]},
        ],
    },
    {
        "feature_group": "C-图算法与社区",
        "feature": "modularity",
        "title": "模块度计算",
        "page_file": "c02-modularity.md",
        "boundary": "负责多层级模块度计算（根/叶/整图/最大连通分量）；"
                    "不负责 Leiden 划分（c01）、不负责图剪枝（c04）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/graphs/modularity.py",
             "symbols": ["modularity", "calculate_graph_modularity", "calculate_root_modularity"]},
            {"path": "packages/graphrag/graphrag/graphs/modularity.py",
             "symbols": ["calculate_leaf_modularity", "calculate_lcc_modularity"]},
        ],
    },
    {
        "feature_group": "C-图算法与社区",
        "feature": "graph-pruning",
        "title": "图剪枝与稳定大组件",
        "page_file": "c03-graph-pruning.md",
        "boundary": "负责按度数阈值剪枝、以及提取稳定的大连通分量；"
                    "不负责社区划分（c01）、不负责模块度（c02）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/index/operations/prune_graph.py",
             "symbols": ["prune_graph", "_get_upper_threshold_by_std"]},
            {"path": "packages/graphrag/graphrag/graphs/stable_lcc.py",
             "symbols": ["stable_lcc"]},
            {"path": "packages/graphrag/graphrag/graphs/connected_components.py",
             "symbols": ["connected_components", "largest_connected_component"]},
        ],
    },

    # ============ D-查询 ============
    {
        "feature_group": "D-查询",
        "feature": "query-base",
        "title": "查询引擎契约与适配",
        "page_file": "d01-query-base.md",
        "boundary": "负责查询引擎的抽象契约、结果结构与索引产物适配层；"
                    "不负责具体检索（d02-d05）、不负责上下文构建（d06）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/structured_search/base.py",
             "symbols": ["BaseSearch", "SearchResult"]},
            {"path": "packages/graphrag/graphrag/query/factory.py",
             "symbols": ["get_local_search_engine", "get_global_search_engine"]},
            {"path": "packages/graphrag/graphrag/query/indexer_adapters.py",
             "symbols": ["read_indexer_entities", "read_indexer_reports",
                         "read_indexer_text_units"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "local-search",
        "title": "局部检索",
        "page_file": "d02-local-search.md",
        "boundary": "负责以实体为中心的局部检索（实体→关系→文本单元→社区报告）；"
                    "不负责全局检索（d03）、不负责朴素检索（d05）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/structured_search/local_search/search.py",
             "symbols": ["LocalSearch"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/local_search/mixed_context.py",
             "symbols": ["LocalSearchMixedContext"]},
            {"path": "packages/graphrag/graphrag/prompts/query/local_search_system_prompt.py",
             "symbols": ["LOCAL_SEARCH_SYSTEM_PROMPT"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "global-search",
        "title": "全局检索与社区报告",
        "page_file": "d03-global-search.md",
        "boundary": "负责以社区报告为单位的 map-reduce 全局检索；"
                    "不负责局部检索（d02）、不负责 DRIFT（d04）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/structured_search/global_search/search.py",
             "symbols": ["GlobalSearch", "GlobalSearchResult"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/global_search/community_context.py",
             "symbols": ["GlobalCommunityContext"]},
            {"path": "packages/graphrag/graphrag/prompts/query/global_search_map_system_prompt.py",
             "symbols": ["MAP_SYSTEM_PROMPT"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "drift-search",
        "title": "DRIFT 检索",
        "page_file": "d04-drift-search.md",
        "boundary": "负责「先由全局视角生成后续问题、再逐轮局部检索」的 DRIFT 检索；"
                    "不负责局部检索（d02）、不负责全局检索（d03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/structured_search/drift_search/search.py",
             "symbols": ["DRIFTSearch"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/drift_search/primer.py",
             "symbols": ["DRIFTPrimer", "PrimerResponse"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/drift_search/state.py",
             "symbols": ["QueryState"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/drift_search/action.py",
             "symbols": ["DriftAction"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "basic-search",
        "title": "朴素向量检索",
        "page_file": "d05-basic-search.md",
        "boundary": "负责不用知识图谱、直接对文本单元做向量检索的基线方案；"
                    "不负责图谱检索（d02-d04）、不负责上下文构建（d06）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/structured_search/basic_search/search.py",
             "symbols": ["BasicSearch"]},
            {"path": "packages/graphrag/graphrag/query/structured_search/basic_search/basic_context.py",
             "symbols": ["BasicSearchContext"]},
            {"path": "packages/graphrag/graphrag/prompts/query/basic_search_system_prompt.py",
             "symbols": ["BASIC_SEARCH_SYSTEM_PROMPT"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "context-building",
        "title": "上下文构建与动态社区选择",
        "page_file": "d06-context-building.md",
        "boundary": "负责把候选记录按 token 预算装进上下文、以及动态挑选相关社区；"
                    "不负责具体检索算法（d02-d05）、不负责对话历史（d07）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/context_builder/dynamic_community_selection.py",
             "symbols": ["DynamicCommunitySelection"]},
            {"path": "packages/graphrag/graphrag/query/context_builder/community_context.py",
             "symbols": ["build_community_context"]},
            {"path": "packages/graphrag/graphrag/query/context_builder/local_context.py",
             "symbols": ["build_entity_context", "build_relationship_context"]},
            {"path": "packages/graphrag/graphrag/query/context_builder/source_context.py",
             "symbols": ["build_text_unit_context"]},
        ],
    },
    {
        "feature_group": "D-查询",
        "feature": "conversation-history",
        "title": "对话历史与问题生成",
        "page_file": "d07-conversation-history.md",
        "boundary": "负责多轮对话历史的裁剪与相关度重排、以及从历史生成完整问题；"
                    "不负责上下文装填（d06）、不负责检索（d02-d05）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/query/context_builder/conversation_history.py",
             "symbols": ["ConversationHistory"]},
            {"path": "packages/graphrag/graphrag/query/context_builder/rate_relevancy.py",
             "symbols": ["rate_relevancy"]},
            {"path": "packages/graphrag/graphrag/query/question_gen/local_gen.py",
             "symbols": ["LocalQuestionGen"]},
        ],
    },

    # ============ E-LLM 层 ============
    {
        "feature_group": "E-LLM 层",
        "feature": "llm-completion",
        "title": "补全抽象与工厂",
        "page_file": "e01-llm-completion.md",
        "boundary": "负责补全与嵌入的抽象契约、注册工厂与消息构建；"
                    "不负责中间件（e02）、不负责限流重试（e03）。",
        "refs": [
            {"path": "packages/graphrag-llm/graphrag_llm/completion/completion.py",
             "symbols": ["LLMCompletion"]},
            {"path": "packages/graphrag-llm/graphrag_llm/completion/completion_factory.py",
             "symbols": ["CompletionFactory", "register_completion", "create_completion"]},
            {"path": "packages/graphrag-llm/graphrag_llm/embedding/embedding.py",
             "symbols": ["LLMEmbedding"]},
            {"path": "packages/graphrag-llm/graphrag_llm/utils/completion_messages_builder.py",
             "symbols": ["CompletionMessagesBuilder"]},
        ],
    },
    {
        "feature_group": "E-LLM 层",
        "feature": "middleware",
        "title": "中间件管道",
        "page_file": "e02-middleware.md",
        "boundary": "负责把缓存、限流、重试、指标、日志等横切关注点组合成中间件管道；"
                    "不负责各中间件内部（e03）、不负责工厂（e01）。",
        "refs": [
            {"path": "packages/graphrag-llm/graphrag_llm/middleware/with_middleware_pipeline.py",
             "symbols": ["with_middleware_pipeline"]},
            {"path": "packages/graphrag-llm/graphrag_llm/middleware/with_cache.py",
             "symbols": ["with_cache"]},
            {"path": "packages/graphrag-llm/graphrag_llm/middleware/with_request_count.py",
             "symbols": ["with_request_count"]},
            {"path": "packages/graphrag-llm/graphrag_llm/middleware/with_metrics.py",
             "symbols": ["with_metrics"]},
        ],
    },
    {
        "feature_group": "E-LLM 层",
        "feature": "rate-limit-and-retry",
        "title": "限流与重试",
        "page_file": "e03-rate-limit-and-retry.md",
        "boundary": "负责滑动窗口限流与指数/立即重试的实现与工厂；"
                    "不负责中间件编排（e02）、不负责缓存键（e04）。",
        "refs": [
            {"path": "packages/graphrag-llm/graphrag_llm/rate_limit/sliding_window_rate_limiter.py",
             "symbols": ["SlidingWindowRateLimiter"]},
            {"path": "packages/graphrag-llm/graphrag_llm/rate_limit/rate_limit_factory.py",
             "symbols": ["RateLimitFactory", "create_rate_limiter"]},
            {"path": "packages/graphrag-llm/graphrag_llm/retry/exponential_retry.py",
             "symbols": ["ExponentialRetry"]},
            {"path": "packages/graphrag-llm/graphrag_llm/retry/retry_factory.py",
             "symbols": ["RetryFactory", "create_retry"]},
        ],
    },
    {
        "feature_group": "E-LLM 层",
        "feature": "cache-key-and-templating",
        "title": "缓存键与模板引擎",
        "page_file": "e04-cache-key-and-templating.md",
        "boundary": "负责 LLM 缓存键的构造与模板引擎/模板管理器；"
                    "不负责限流重试（e03）、不负责线程化（e05）。",
        "refs": [
            {"path": "packages/graphrag-llm/graphrag_llm/cache/create_cache_key.py",
             "symbols": ["create_cache_key"]},
            {"path": "packages/graphrag-llm/graphrag_llm/templating/template_engine.py",
             "symbols": ["TemplateEngine"]},
            {"path": "packages/graphrag-llm/graphrag_llm/templating/file_template_manager.py",
             "symbols": ["FileTemplateManager"]},
        ],
    },
    {
        "feature_group": "E-LLM 层",
        "feature": "threading-and-metrics",
        "title": "线程化执行与指标聚合",
        "page_file": "e05-threading-and-metrics.md",
        "boundary": "负责把同步补全包装成线程池执行的运行器、以及指标采集与聚合；"
                    "不负责中间件（e02）、不负责模板（e04）。",
        "refs": [
            {"path": "packages/graphrag-llm/graphrag_llm/threading/completion_thread_runner.py",
             "symbols": ["completion_thread_runner", "ThreadedLLMCompletionResponseHandler"]},
            {"path": "packages/graphrag-llm/graphrag_llm/metrics/metrics_aggregator.py",
             "symbols": ["MetricsAggregator"]},
            {"path": "packages/graphrag-llm/graphrag_llm/metrics/metrics_processor.py",
             "symbols": ["MetricsProcessor"]},
            {"path": "packages/graphrag-llm/graphrag_llm/model_cost_registry/model_cost_registry.py",
             "symbols": ["ModelCostRegistry", "ModelCosts"]},
        ],
    },

    # ============ F-独立子包 ============
    {
        "feature_group": "F-独立子包",
        "feature": "cache-storage",
        "title": "缓存实现与键",
        "page_file": "f01-cache-storage.md",
        "boundary": "负责缓存抽象的几种实现（内存/JSON/SQLite/空实现）与缓存键类型；"
                    "不负责 LLM 侧缓存中间件（e02）、不负责存储抽象（f02）。",
        "refs": [
            {"path": "packages/graphrag-cache/graphrag_cache/cache.py", "symbols": ["Cache"]},
            {"path": "packages/graphrag-cache/graphrag_cache/cache_key.py",
             "symbols": ["CacheKeyCreator"]},
            {"path": "packages/graphrag-cache/graphrag_cache/sqlite_cache.py",
             "symbols": ["SQLiteCache"]},
            {"path": "packages/graphrag-cache/graphrag_cache/noop_cache.py",
             "symbols": ["NoopCache"]},
        ],
    },
    {
        "feature_group": "F-独立子包",
        "feature": "storage-abstraction",
        "title": "存储抽象与表提供方",
        "page_file": "f02-storage-abstraction.md",
        "boundary": "负责文件/内存/对象存储的抽象与表读写接口；"
                    "不负责缓存（f01）、不负责向量存储（f03）。",
        "refs": [
            {"path": "packages/graphrag-storage/graphrag_storage/storage.py",
             "symbols": ["Storage"]},
            {"path": "packages/graphrag-storage/graphrag_storage/storage.py",
             "symbols": ["get_timestamp_formatted_with_local_tz"]},
        ],
    },
    {
        "feature_group": "F-独立子包",
        "feature": "vector-store",
        "title": "向量存储抽象",
        "page_file": "f03-vector-store.md",
        "boundary": "负责向量存储的文档结构、检索结果与存储契约；"
                    "不负责通用存储（f02）、不负责分块（f04）。",
        "refs": [
            {"path": "packages/graphrag-vectors/graphrag_vectors/vector_store.py",
             "symbols": ["VectorStore", "VectorStoreDocument", "VectorStoreSearchResult"]},
        ],
    },
    {
        "feature_group": "F-独立子包",
        "feature": "input-and-chunking",
        "title": "输入读取与分块器",
        "page_file": "f04-input-and-chunking.md",
        "boundary": "负责输入文件的读取器与 token 级别的分块器契约；"
                    "不负责索引侧工作流（b01）、不负责向量存储（f03）。",
        "refs": [
            {"path": "packages/graphrag-input/graphrag_input/csv.py", "symbols": ["CSVFileReader"]},
            {"path": "packages/graphrag-chunking/graphrag_chunking/chunker.py",
             "symbols": ["Chunker"]},
        ],
    },

    # ============ G-调用面与回调 ============
    {
        "feature_group": "G-调用面与回调",
        "feature": "api-and-cli",
        "title": "API 与命令行调用面",
        "page_file": "g01-api-and-cli.md",
        "boundary": "负责 Python API（建索引/检索）与命令行入口的参数解析与流程衔接；"
                    "不负责回调（g02）、不负责配置（a01）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/api/index.py",
             "symbols": ["build_index", "_get_method"]},
            {"path": "packages/graphrag/graphrag/api/query.py",
             "symbols": ["local_search", "global_search", "drift_search", "basic_search"]},
            {"path": "packages/graphrag/graphrag/cli/index.py",
             "symbols": ["index_cli", "update_cli"]},
            {"path": "packages/graphrag/graphrag/cli/query.py",
             "symbols": ["run_local_search", "run_global_search", "run_drift_search"]},
        ],
    },
    {
        "feature_group": "G-调用面与回调",
        "feature": "callbacks-and-profiling",
        "title": "回调与性能剖析",
        "page_file": "g02-callbacks-and-profiling.md",
        "boundary": "负责工作流与查询的回调协议、控制台实现、以及流水线性能剖析；"
                    "不负责调用面（g01）、不负责数据模型（a03）。",
        "refs": [
            {"path": "packages/graphrag/graphrag/callbacks/workflow_callbacks.py",
             "symbols": ["WorkflowCallbacks"]},
            {"path": "packages/graphrag/graphrag/callbacks/workflow_callbacks_manager.py",
             "symbols": ["WorkflowCallbacksManager"]},
            {"path": "packages/graphrag/graphrag/index/run/profiling.py",
             "symbols": ["WorkflowProfiler"]},
            {"path": "packages/graphrag/graphrag/index/utils/derive_from_rows.py",
             "symbols": ["derive_from_rows", "derive_from_rows_asyncio",
                         "ParallelizationError"]},
        ],
    },
]
