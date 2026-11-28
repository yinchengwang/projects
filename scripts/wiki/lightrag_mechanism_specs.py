# -*- coding: utf-8 -*-
"""lightrag 机制清单（阶段 2C 第四个项目）。

盘点范围：`lightrag/` 包本体（196 个 py 文件）。`lightrag_webui/`（前端）、`tests/`、
`examples/`、`reproduce/`、`scripts/` 不建页。

lightrag 与 2C 前三个项目的差别在**重心**：
- langchain / llama-index / haystack 都是「通用编排骨架 + 大量组件」；
- lightrag 是**单一产品的完整实现**：它把「从文档到知识图谱再到答案」这一整条链写透，
  包括图谱抽取与合并、实体关系去重、上下文预算裁剪、存储后端迁移、多模态 sidecar、
  配置分片与启动校验。

所以它的体量集中在少数几个巨型文件（`lightrag.py` 9052 行、`pipeline.py` 7777 行、
`utils.py` 7836 行、`operate.py` 7199 行），机制密度远高于组件数量。本文库按
「数据从进来到出去」的顺序切：抽象与存储契约 → 解析 → 分块 → 图谱抽取与合并 →
查询 → 流水线调度 → 存储实现 → 多模态与表格 → 配置与安全 → 评测与重排。

盘点方法：`dump_python_defs.py` 建 AST 定义索引，再按职责归并。存储后端（milvus /
mongo / neo4j / faiss / nano-vectordb / networkx / memgraph）属于同一契约的多个实现，
归并成一到两页而不是每个后端一页。

源码锚点：`lightrag@72c2e9533839036672590988ca1fae04124808a2`。
"""

GROUP_ORDER = [
    "A-抽象与存储契约",
    "B-解析与路由",
    "C-分块",
    "D-图谱抽取与合并",
    "E-查询与上下文",
    "F-流水线调度",
    "G-存储实现",
    "H-多模态与表格",
    "I-配置与安全",
    "J-评测与重排",
]

MECHANISM_SPECS = [
    # ============ A-抽象与存储契约 ============
    {
        "feature_group": "A-抽象与存储契约",
        "feature": "storage-contracts",
        "title": "四类存储契约与命名空间",
        "page_file": "a01-storage-contracts.md",
        "boundary": "负责键值/图/向量/文档状态四类存储的抽象契约与命名空间隔离；"
                    "不负责具体后端（G 组）、不负责文档状态机（a02）。",
        "refs": [
            {"path": "lightrag/base.py",
             "symbols": ["StorageNameSpace", "BaseKVStorage", "BaseGraphStorage",
                         "BaseVectorStorage"]},
            {"path": "lightrag/namespace.py", "symbols": ["NameSpace", "is_namespace"]},
            {"path": "lightrag/kg/factory.py", "symbols": ["get_storage_class"]},
        ],
    },
    {
        "feature_group": "A-抽象与存储契约",
        "feature": "doc-status",
        "title": "文档状态与游标",
        "page_file": "a02-doc-status.md",
        "boundary": "负责文档处理状态机、状态记录结构与分页游标；"
                    "不负责存储契约（a01）、不负责流水线调度（F 组）。",
        "refs": [
            {"path": "lightrag/base.py",
             "symbols": ["DocStatus", "DocProcessingStatus", "DocSchedulingRecord"]},
            {"path": "lightrag/base.py",
             "symbols": ["CursorPosition", "CursorAfter", "_CursorSentinel"]},
        ],
    },
    {
        "feature_group": "A-抽象与存储契约",
        "feature": "query-param",
        "title": "查询参数与基础类型",
        "page_file": "a03-query-param.md",
        "boundary": "负责一次查询的可调参数结构与抽取结果的基础类型；"
                    "不负责查询主流程（E 组）、不负责文档状态（a02）。",
        "refs": [
            {"path": "lightrag/base.py", "symbols": ["QueryParam", "TextChunkSchema"]},
            {"path": "lightrag/types.py",
             "symbols": ["ExtractedEntity", "ExtractedRelationship", "EntityExtractionResult"]},
        ],
    },

    # ============ B-解析与路由 ============
    {
        "feature_group": "B-解析与路由",
        "feature": "parser-base",
        "title": "解析器契约与原生解析基类",
        "page_file": "b01-parser-base.md",
        "boundary": "负责解析器的抽象契约、解析上下文与结果结构、以及原生解析运行时；"
                    "不负责引擎注册（b02）、不负责路由（b03）。",
        "refs": [
            {"path": "lightrag/parser/base.py",
             "symbols": ["BaseParser", "ParseContext", "ParseResult", "ResolvedSource"]},
            {"path": "lightrag/parser/native_base.py",
             "symbols": ["NativeParserBase", "NativeExtractRuntime"]},
        ],
    },
    {
        "feature_group": "B-解析与路由",
        "feature": "parser-registry",
        "title": "解析引擎注册与插件",
        "page_file": "b02-parser-registry.md",
        "boundary": "负责解析引擎的注册表、能力声明与第三方插件加载；"
                    "不负责契约（b01）、不负责路由决策（b03）。",
        "refs": [
            {"path": "lightrag/parser/registry.py",
             "symbols": ["ParserSpec", "register_parser", "get_parser"]},
            {"path": "lightrag/parser/registry.py",
             "symbols": ["parser_specs_snapshot", "supported_parser_engines"]},
            {"path": "lightrag/parser/plugins.py", "symbols": ["load_third_party_parsers"]},
        ],
    },
    {
        "feature_group": "B-解析与路由",
        "feature": "parser-routing",
        "title": "解析路由与选项校验",
        "page_file": "b03-parser-routing.md",
        "boundary": "负责按文件名/后缀/配置选解析引擎、以及解析选项的规范化与校验；"
                    "不负责契约（b01）、不负责注册表（b02）。",
        "refs": [
            {"path": "lightrag/parser/routing.py",
             "symbols": ["normalize_parser_engine", "ProcessOptions", "sanitize_process_options"]},
            {"path": "lightrag/parser/routing.py",
             "symbols": ["chunk_strategy_key", "validate_chunk_token_sizes",
                         "ParserRoutingConfigError"]},
        ],
    },

    # ============ C-分块 ============
    {
        "feature_group": "C-分块",
        "feature": "chunker-registry",
        "title": "分块器注册与上下文传递",
        "page_file": "c01-chunker-registry.md",
        "boundary": "负责分块器的注册表、能力探测与调用约定；"
                    "不负责各算法（c02/c03/c04）、不负责块结构（c05）。",
        "refs": [
            {"path": "lightrag/chunker/registry.py",
             "symbols": ["ChunkerSpec", "register_chunker", "invoke_chunker"]},
            {"path": "lightrag/chunker/registry.py",
             "symbols": ["accepts_chunking_context", "selectable_chunker_names"]},
            {"path": "lightrag/chunker/plugins.py", "symbols": ["load_and_resolve_chunker"]},
        ],
    },
    {
        "feature_group": "C-分块",
        "feature": "recursive-character",
        "title": "递归字符分块与分隔符归一",
        "page_file": "c02-recursive-character.md",
        "boundary": "负责按分隔符递归切分、跨度合并与分隔符写法归一；"
                    "不负责 token 切分（c03）、不负责语义切分（c04）。",
        "refs": [
            {"path": "lightrag/chunker/recursive_character.py",
             "symbols": ["chunking_by_recursive_character", "_split_text_with_spans",
                         "_merge_splits_with_spans"]},
            {"path": "lightrag/chunker/recursive_character.py",
             "symbols": ["normalize_r_separators", "RSeparatorNormalization",
                         "inspect_r_separators"]},
        ],
    },
    {
        "feature_group": "C-分块",
        "feature": "token-window",
        "title": "token 窗口分块",
        "page_file": "c03-token-window.md",
        "boundary": "负责按 token 数与窗口步长切分并记录原文跨度；"
                    "不负责递归字符（c02）、不负责语义（c04）。",
        "refs": [
            {"path": "lightrag/chunker/token_size.py",
             "symbols": ["chunking_by_token_size", "chunking_by_fixed_token"]},
            {"path": "lightrag/chunker/token_size.py",
             "symbols": ["_token_windows", "_window_step", "_make_chunk"]},
        ],
    },
    {
        "feature_group": "C-分块",
        "feature": "semantic-chunking",
        "title": "语义分块",
        "page_file": "c04-semantic-chunking.md",
        "boundary": "负责按嵌入相似度与段落语义切分（含标题与引用尾部处理）；"
                    "不负责递归字符（c02）、不负责 token 窗口（c03）。",
        "refs": [
            {"path": "lightrag/chunker/semantic_vector.py",
             "symbols": ["chunking_by_semantic_vector", "_semantic_groups_with_spans"]},
            {"path": "lightrag/chunker/paragraph_semantic.py",
             "symbols": ["chunking_by_paragraph_semantic", "_block_to_paragraphs"]},
            {"path": "lightrag/chunker/paragraph_semantic.py",
             "symbols": ["_normalize_references_tail_n", "_apply_part_suffixes"]},
        ],
    },
    {
        "feature_group": "C-分块",
        "feature": "chunk-schema",
        "title": "块结构与标题上下文",
        "page_file": "c05-chunk-schema.md",
        "boundary": "负责块的字段结构、标题层级归一与标题上下文拼装；"
                    "不负责切分算法（c02-c04）、不负责多模态标记（H 组）。",
        "refs": [
            {"path": "lightrag/chunk_schema.py",
             "symbols": ["normalize_chunk_heading", "_clean_and_cap_headings",
                         "format_heading_context"]},
            {"path": "lightrag/chunk_schema.py",
             "symbols": ["normalize_chunk_sidecar", "strip_internal_multimodal_markup_for_extraction"]},
        ],
    },

    # ============ D-图谱抽取与合并 ============
    {
        "feature_group": "D-图谱抽取与合并",
        "feature": "entity-extraction",
        "title": "实体关系抽取",
        "page_file": "d01-entity-extraction.md",
        "boundary": "负责从块文本抽取实体与关系（含 gleaning 多轮、JSON 结果解析、类型校验）；"
                    "不负责合并（d02）、不负责摘要（d03）。",
        "refs": [
            {"path": "lightrag/operate.py", "symbols": ["extract_entities"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_handle_single_entity_extraction", "_handle_single_relationship_extraction",
                         "_normalize_and_validate_entity_type"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_process_json_extraction_result", "_is_gleaning_extract_row",
                         "_parse_extract_row"]},
        ],
    },
    {
        "feature_group": "D-图谱抽取与合并",
        "feature": "node-edge-merge",
        "title": "节点与边的合并去重",
        "page_file": "d02-node-edge-merge.md",
        "boundary": "负责同实体/同关系的多来源描述合并、度数与权重更新；"
                    "不负责抽取（d01）、不负责描述摘要（d03）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["_merge_nodes_then_upsert", "_merge_edges_then_upsert",
                         "merge_nodes_and_edges"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_combine_descriptions_dedup", "collect_kg_merge_candidates"]},
        ],
    },
    {
        "feature_group": "D-图谱抽取与合并",
        "feature": "description-summary",
        "title": "描述摘要与合并策略",
        "page_file": "d03-description-summary.md",
        "boundary": "负责实体/关系描述在超长时如何摘要、以及摘要的缓存与降级；"
                    "不负责合并流程（d02）、不负责名称归一（d04）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["_handle_entity_relation_summary", "_summarize_descriptions"]},
            {"path": "lightrag/utils_graph.py",
             "symbols": ["validate_relation_weight", "apply_relation_weight_floor",
                         "relation_evidence_source_ids"]},
        ],
    },
    {
        "feature_group": "D-图谱抽取与合并",
        "feature": "graph-name-rules",
        "title": "图谱名称归一与拒绝规则",
        "page_file": "d04-graph-name-rules.md",
        "boundary": "负责实体名的归一与查找解析、自环与空描述的拒绝、以及手工实体锁；"
                    "不负责合并（d02）、不负责重建（d05）。",
        "refs": [
            {"path": "lightrag/utils_graph.py",
             "symbols": ["resolve_entity_name_for_lookup", "_normalize_manual_entity_name",
                         "_require_stored_entity_name"]},
            {"path": "lightrag/utils_graph.py",
             "symbols": ["_reject_self_loop_relation", "_require_non_empty_description",
                         "_manual_entity_lock_keys"]},
        ],
    },
    {
        "feature_group": "D-图谱抽取与合并",
        "feature": "kg-rebuild",
        "title": "图谱重建与抽取缓存",
        "page_file": "d05-kg-rebuild.md",
        "boundary": "负责从既有块重建知识图谱、抽取结果缓存与单实体/单关系重建；"
                    "不负责抽取（d01）、不负责合并（d02）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["rebuild_knowledge_from_chunks", "_rebuild_from_extraction_result",
                         "_reextract_chunks"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_get_cached_extraction_results", "_rebuild_single_entity",
                         "_rebuild_single_relationship"]},
        ],
    },

    # ============ E-查询与上下文 ============
    {
        "feature_group": "E-查询与上下文",
        "feature": "kg-query",
        "title": "图谱查询主流程",
        "page_file": "e01-kg-query.md",
        "boundary": "负责一次图谱增强查询的完整流程（关键词 → 检索 → 上下文 → 生成）；"
                    "不负责关键词抽取（e02）、不负责上下文预算（e03）。",
        "refs": [
            {"path": "lightrag/operate.py", "symbols": ["kg_query", "naive_query"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_build_query_context", "_build_context_str"]},
        ],
    },
    {
        "feature_group": "E-查询与上下文",
        "feature": "keyword-extraction",
        "title": "查询关键词抽取",
        "page_file": "e02-keyword-extraction.md",
        "boundary": "负责从自然语言问题抽高低层关键词、以及解析模型输出的各种 JSON 形态；"
                    "不负责主流程（e01）、不负责上下文预算（e03）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["get_keywords_from_query", "extract_keywords_only",
                         "_parse_keywords_payload"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_normalize_keyword_list", "_select_keywords_object",
                         "normalize_keyword_extraction_json_mode"]},
        ],
    },
    {
        "feature_group": "E-查询与上下文",
        "feature": "context-budget",
        "title": "上下文预算与截断",
        "page_file": "e03-context-budget.md",
        "boundary": "负责把多来源上下文塞进总 token 预算的裁剪策略与配额分配；"
                    "不负责主流程（e01）、不负责块合并细节（e04）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["_apply_token_truncation", "_fit_kg_context_to_total_budget",
                         "_vector_chunk_quota"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_truncate_entity_identifier", "_truncate_section_context",
                         "_truncate_vdb_content"]},
        ],
    },
    {
        "feature_group": "E-查询与上下文",
        "feature": "context-assembly",
        "title": "上下文组装与块合并",
        "page_file": "e04-context-assembly.md",
        "boundary": "负责从实体/关系扩展到相关文本块、并按标题与顺序合并成最终上下文；"
                    "不负责预算（e03）、不负责关键词（e02）。",
        "refs": [
            {"path": "lightrag/operate.py",
             "symbols": ["_find_related_text_unit_from_entities",
                         "_find_related_text_unit_from_relations", "_merge_all_chunks"]},
            {"path": "lightrag/operate.py",
             "symbols": ["_attach_content_headings", "_get_vector_context",
                         "_resolve_kg_search_queries"]},
        ],
    },
    {
        "feature_group": "E-查询与上下文",
        "feature": "query-validation",
        "title": "查询校验",
        "page_file": "e05-query-validation.md",
        "boundary": "负责查询非空与最小有效长度的校验（含宽字符权重）；"
                    "不负责主流程（e01）、不负责预算（e03）。",
        "refs": [
            {"path": "lightrag/query_validation.py",
             "symbols": ["validate_query_not_empty", "validate_rag_query",
                         "QueryValidationError"]},
            {"path": "lightrag/query_validation.py",
             "symbols": ["rag_query_weight", "meets_min_rag_query_weight", "_is_wide_character"]},
        ],
    },

    # ============ F-流水线调度 ============
    {
        "feature_group": "F-流水线调度",
        "feature": "pipeline-steps",
        "title": "流水线步骤与决策",
        "page_file": "f01-pipeline-steps.md",
        "boundary": "负责扫描/解析/分块/抽取等步骤的推进决策与批处理上下文；"
                    "不负责文档状态机（a02）、不负责插入主流程（f02）。",
        "refs": [
            {"path": "lightrag/pipeline.py",
             "symbols": ["PipelineNextStep", "PipelineNextDecision", "_BatchRunContext"]},
            {"path": "lightrag/pipeline.py",
             "symbols": ["_PipelineMixin", "_resolve_sidecar_image_path", "_rearm_auto_rescan"]},
        ],
    },
    {
        "feature_group": "F-流水线调度",
        "feature": "insert-flow",
        "title": "文档插入流程",
        "page_file": "f02-insert-flow.md",
        "boundary": "负责一次插入的完整编排（重建抽取、写状态、刷盘、失败清理）；"
                    "不负责步骤决策（f01）、不负责删除与清缓存（f03）。",
        "refs": [
            {"path": "lightrag/lightrag.py", "symbols": ["LightRAG", "ainsert"]},
            {"path": "lightrag/lightrag.py",
             "symbols": ["_process_extract_entities", "_flush_storages", "_insert_done_with_cleanup"]},
        ],
    },
    {
        "feature_group": "F-流水线调度",
        "feature": "custom-chunks-kg",
        "title": "自定义块与手工图谱写入",
        "page_file": "f03-custom-chunks-kg.md",
        "boundary": "负责直接写入自定义块、手工知识图谱及其回滚与恢复锚点；"
                    "不负责普通插入（f02）、不负责存储实现（G 组）。",
        "refs": [
            {"path": "lightrag/lightrag.py",
             "symbols": ["ainsert_custom_chunks", "ainsert_custom_kg"]},
            {"path": "lightrag/lightrag.py",
             "symbols": ["arollback_failed_custom_chunk_patches",
                         "_rollback_one_custom_chunk_patch", "_union_doc_recovery_anchors"]},
        ],
    },
    {
        "feature_group": "F-流水线调度",
        "feature": "storage-lifecycle",
        "title": "存储初始化与收尾",
        "page_file": "f04-storage-lifecycle.md",
        "boundary": "负责启动时的存储初始化、嵌入基线核对、以及收尾刷盘与失败释放；"
                    "不负责插入流程（f02）、不负责配置（I 组）。",
        "refs": [
            {"path": "lightrag/lightrag.py",
             "symbols": ["initialize_storages", "finalize_storages", "_establish_embedding_baselines"]},
            {"path": "lightrag/lightrag.py",
             "symbols": ["_release_after_early_failure", "_commit_cache_pair_before_finalize",
                         "_quarantine_cache_before_finalize"]},
        ],
    },

    # ============ G-存储实现 ============
    {
        "feature_group": "G-存储实现",
        "feature": "json-kv-store",
        "title": "JSON 键值存储与文档状态实现",
        "page_file": "g01-json-kv-store.md",
        "boundary": "负责文件型键值与文档状态存储的实现（分片、原子写、清空语义）；"
                    "不负责图存储（g02）、不负责向量存储（g03）。",
        "refs": [
            {"path": "lightrag/kg/json_kv_impl.py", "symbols": ["JsonKVStorage"]},
            {"path": "lightrag/kg/json_doc_status_impl.py", "symbols": ["JsonDocStatusStorage"]},
            {"path": "lightrag/file_atomic.py",
             "symbols": ["atomic_write", "_replace_file", "reap_orphan_tmp_files"]},
        ],
    },
    {
        "feature_group": "G-存储实现",
        "feature": "graph-storage-impl",
        "title": "图存储实现（内存与外部）",
        "page_file": "g02-graph-storage-impl.md",
        "boundary": "负责内存图存储与 Neo4j/Memgraph 实现的差异（事务、索引、并发）；"
                    "不负责键值存储（g01）、不负责向量存储（g03）。",
        "refs": [
            {"path": "lightrag/kg/networkx_impl.py",
             "symbols": ["NetworkXStorage", "_walk_live_view"]},
            {"path": "lightrag/kg/neo4j_impl.py",
             "symbols": ["Neo4JStorage", "WorkspaceIndexCollisionError"]},
            {"path": "lightrag/kg/memgraph_impl.py", "symbols": ["MemgraphStorage"]},
        ],
    },
    {
        "feature_group": "G-存储实现",
        "feature": "vector-storage-impl",
        "title": "向量存储实现",
        "page_file": "g03-vector-storage-impl.md",
        "boundary": "负责 FAISS / nano-vectordb / Milvus 三种向量后端的实现与差异；"
                    "不负责图存储（g02）、不负责嵌入基线核对（g04）。",
        "refs": [
            {"path": "lightrag/kg/faiss_impl.py", "symbols": ["FaissVectorDBStorage"]},
            {"path": "lightrag/kg/nano_vector_db_impl.py",
             "symbols": ["NanoVectorDBStorage", "_sanitize_record_value"]},
            {"path": "lightrag/kg/milvus_impl.py",
             "symbols": ["MilvusVectorDBStorage", "MilvusIndexConfig", "get_collection_gate"]},
        ],
    },
    {
        "feature_group": "G-存储实现",
        "feature": "embedding-baseline",
        "title": "嵌入空间校验与存储迁移",
        "page_file": "g04-embedding-baseline.md",
        "boundary": "负责启动时校验已存向量与当前嵌入模型是否同一空间、以及存储结构迁移；"
                    "不负责向量后端（g03）、不负责配置分片（I 组）。",
        "refs": [
            {"path": "lightrag/vector_space_gate.py",
             "symbols": ["_probe_same_embedding_space", "_cosine", "StartupEvidence"]},
            {"path": "lightrag/vector_space_gate.py",
             "symbols": ["graph_has_nodes", "graph_has_edges", "_vectors_are_expected"]},
            {"path": "lightrag/storage_migrations.py",
             "symbols": ["_StorageMigrationMixin", "_migration_chunk_ids"]},
        ],
    },

    # ============ H-多模态与表格 ============
    {
        "feature_group": "H-多模态与表格",
        "feature": "multimodal-context",
        "title": "多模态上下文与原子化",
        "page_file": "h01-multimodal-context.md",
        "boundary": "负责把图文混排内容切成原子单元、以及按 token 预算裁剪表格行与图片；"
                    "不负责表格标记格式（h02）、不负责 sidecar 写入（h03）。",
        "refs": [
            {"path": "lightrag/multimodal_context.py",
             "symbols": ["_atomize", "_largest_fitting_row_prefix", "_row_trim_table_leading"]},
            {"path": "lightrag/multimodal_context.py",
             "symbols": ["find_target_span", "_split_text_segment", "_char_trim_trailing"]},
        ],
    },
    {
        "feature_group": "H-多模态与表格",
        "feature": "table-markup",
        "title": "表格标记与 HTML 行拆分",
        "page_file": "h02-table-markup.md",
        "boundary": "负责表格标记的检测、序列化与 HTML 行拆分（含 rowspan 切分）；"
                    "不负责上下文裁剪（h01）、不负责 sidecar（h03）。",
        "refs": [
            {"path": "lightrag/table_markup.py",
             "symbols": ["parse_table_tag", "detect_table_format", "serialize_html_rows"]},
            {"path": "lightrag/chunker/_html_rowspan.py",
             "symbols": ["RowspanSlicer", "build_rowspan_slicer", "_header_row_count"]},
        ],
    },
    {
        "feature_group": "H-多模态与表格",
        "feature": "sidecar",
        "title": "sidecar 写入与占位符",
        "page_file": "h03-sidecar.md",
        "boundary": "负责多模态 sidecar 的中间表示、占位符回填与回填工具；"
                    "不负责上下文裁剪（h01）、不负责表格标记（h02）。",
        "refs": [
            {"path": "lightrag/sidecar/ir.py", "symbols": ["IRDoc", "IRBlock", "IRPosition"]},
            {"path": "lightrag/sidecar/placeholders.py", "symbols": ["render_template", "render_table_tag", "table_body_for_rows"]},
            {"path": "lightrag/sidecar/backfill.py", "symbols": ["backfill_chunk_sidecars"]},
        ],
    },

    # ============ I-配置与安全 ============
    {
        "feature_group": "I-配置与安全",
        "feature": "config-store",
        "title": "配置存储与作用域",
        "page_file": "i01-config-store.md",
        "boundary": "负责配置的键规范、作用域与嵌入基线的持久化；"
                    "不负责分片发现（i02）、不负责锚点（i03）。",
        "refs": [
            {"path": "lightrag/config_store.py",
             "symbols": ["ConfigScope", "ConfigKeySpec", "config_key"]},
            {"path": "lightrag/config_store.py",
             "symbols": ["EmbeddingBaseline", "embedding_baseline_key", "make_config_row"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "config-shards",
        "title": "配置分片与工作区发现",
        "page_file": "i02-config-shards.md",
        "boundary": "负责多工作区配置分片的发现、物理身份与归一化去重；"
                    "不负责键规范（i01）、不负责锚点锁（i03）。",
        "refs": [
            {"path": "lightrag/config_shards.py",
             "symbols": ["validate_config_workspace", "validate_deployment_workspace",
                         "DiscoveredShard"]},
            {"path": "lightrag/config_shards.py",
             "symbols": ["probe_snapshot", "_physical_id", "_differ_only_in_normalization"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "storage-anchor",
        "title": "存储锚点与排他锁",
        "page_file": "i03-storage-anchor.md",
        "boundary": "负责存储目录的锚点文件、UUID 归属判定与进程级排他锁；"
                    "不负责分片（i02）、不负责私有文件权限（i04）。",
        "refs": [
            {"path": "lightrag/config_anchor.py",
             "symbols": ["StorageAnchor", "read_anchor", "_write_temp"]},
            {"path": "lightrag/config_anchor.py",
             "symbols": ["canonical_storage_uuid", "new_storage_uuid", "parse_anchor_payload"]},
            {"path": "lightrag/kg/anchor_lock.py", "symbols": ["ExclusiveAnchorLock", "acquire_anchor_lock_exclusive", "anchor_bind_lock"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "private-file",
        "title": "私有文件写入与原子替换",
        "page_file": "i04-private-file.md",
        "boundary": "负责凭据类文件的排他创建与权限校验（含 Windows DACL）以及原子替换；"
                    "不负责锚点（i03）、不负责鉴权（i05）。",
        "refs": [
            {"path": "lightrag/private_file.py",
             "symbols": ["open_private_file", "_windows_verify_dacl", "PrivateFileError"]},
            {"path": "lightrag/private_file.py",
             "symbols": ["_windows_current_user_sid", "windows_create_exclusive"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "api-security",
        "title": "接口鉴权与准入控制",
        "page_file": "i05-api-security.md",
        "boundary": "负责接口令牌鉴权、登录限流、请求体上限与准入票据；"
                    "不负责私有文件（i04）、不负责异常体系（i06）。",
        "refs": [
            {"path": "lightrag/api/auth.py", "symbols": ["AuthHandler", "TokenPayload"]},
            {"path": "lightrag/api/login_rate_limit.py",
             "symbols": ["LoginRateLimiter", "_Entry"]},
            {"path": "lightrag/api/admission.py",
             "symbols": ["AdmissionTicket", "publish_admission_ticket"]},
            {"path": "lightrag/api/body_limit_middleware.py",
             "symbols": ["BodyLimitMiddleware", "resolve_body_limits"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "exceptions-and-roles",
        "title": "异常体系与多角色模型",
        "page_file": "i06-exceptions-and-roles.md",
        "boundary": "负责 API 异常的分类与映射、以及按角色（抽取/查询/摘要）配置不同模型；"
                    "不负责鉴权（i05）、不负责绑定选项（i07）。",
        "refs": [
            {"path": "lightrag/exceptions.py",
             "symbols": ["APIStatusError", "RateLimitError", "EmptyTruncatedResponseError",
                         "ReasoningOnlyResponseError"]},
            {"path": "lightrag/llm_roles.py",
             "symbols": ["RoleSpec", "RoleLLMConfig", "_RoleLLMMixin"]},
        ],
    },
    {
        "feature_group": "I-配置与安全",
        "feature": "llm-bindings",
        "title": "模型绑定选项与环境解析",
        "page_file": "i07-llm-bindings.md",
        "boundary": "负责各家模型提供方的绑定选项结构与环境变量解析；"
                    "不负责角色配置（i06）、不负责重排（J 组）。",
        "refs": [
            {"path": "lightrag/llm/binding_options.py",
             "symbols": ["BindingOptions", "OpenAILLMOptions", "OllamaLLMOptions"]},
            {"path": "lightrag/llm/binding_options.py",
             "symbols": ["_parse_role_json", "_make_bool_or_literal_parser",
                         "_resolve_optional_type"]},
        ],
    },

    # ============ J-评测与重排 ============
    {
        "feature_group": "J-评测与重排",
        "feature": "rerank",
        "title": "重排与分数聚合",
        "page_file": "j01-rerank.md",
        "boundary": "负责块级重排调用、分数聚合与多家重排服务的适配；"
                    "不负责上下文预算（e03）、不负责评测（j02）。",
        "refs": [
            {"path": "lightrag/rerank.py",
             "symbols": ["chunk_documents_for_rerank", "aggregate_chunk_scores", "generic_rerank_api"]},
            {"path": "lightrag/rerank.py",
             "symbols": ["cohere_rerank", "jina_rerank", "ali_rerank"]},
        ],
    },
    {
        "feature_group": "J-评测与重排",
        "feature": "evaluation",
        "title": "RAG 质量评测与离线检索检查",
        "page_file": "j02-evaluation.md",
        "boundary": "负责 RAG 质量评测流程与不依赖 LLM 的离线检索审计；"
                    "不负责重排（j01）、不负责流水线指标（j03）。",
        "refs": [
            {"path": "lightrag/evaluation/eval_rag_quality.py",
             "symbols": ["RAGEvaluator", "_is_nan"]},
            {"path": "lightrag/evaluation/offline_retrieval_check.py",
             "symbols": ["score_query", "inverse_document_frequency", "audit_samples"]},
        ],
    },
    {
        "feature_group": "J-评测与重排",
        "feature": "metrics-and-prompts",
        "title": "指标与提示模板管理",
        "page_file": "j03-metrics-and-prompts.md",
        "boundary": "负责流水线指标采集与提示模板的解析/覆盖机制；"
                    "不负责评测（j02）、不负责重排（j01）。",
        "refs": [
            {"path": "lightrag/pipeline_metrics.py",
             "symbols": ["increment", "observe", "snapshot"]},
            {"path": "lightrag/prompt.py",
             "symbols": ["EntityExtractionPromptProfile", "resolve_entity_extraction_prompt_profile",
                         "resolve_user_prompt_prefix_path"]},
        ],
    },
]
