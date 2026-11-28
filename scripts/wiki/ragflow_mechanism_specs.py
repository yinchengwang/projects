# -*- coding: utf-8 -*-
"""ragflow 机制清单（阶段 2C 第六个项目）。

盘点范围：`internal/` 下 1581 个非测试 `.go` 文件、54.7 万行。`web/`（React 前端）、
`test/`、`docs/`、`sdk/`（多语言客户端）、`tools/`、`cmd/` 不建页。

ragflow 与 2C 前五个项目的差别是**量纲上的**，不是类型上的：
- langchain / llama-index / haystack 是「可替换组件的编排框架」；
- lightrag 是「单一产品的 Python 实现」；
- graphrag 是「离线建索引的 monorepo」；
- **ragflow 是一个完整的 Go 服务端产品**：它同时有文档解析（含 PDF 版面分析的
  CGO 原生实现）、摄取流水线、文档引擎（ES / OpenSearch / Infinity 三后端）、
  检索引擎、Agent 平台（含一个 langgraph 风格的图执行内核 `harness/`）、
  MCP 接入、同步器、渠道接入（飞书 / Discord / Slack 等）、权限模型、CLI。

因此它的规模不能按「框架」的密度估算：`internal/service` 8.1 万行、`internal/ingestion`
6.2 万行、`internal/agent` 5.8 万行、`internal/entity` 5.2 万行、`internal/harness` 4.1 万行。

组划分按「一次请求会穿过哪些层」来切：A 实体与服务层 → B 摄取流水线 → C 文档解析 →
D 文档引擎与存储 → E 图执行内核 → F Agent 平台 → G Agentic RAG → H 同步接入与运维面。

源码锚点：`ragflow@70ed4c95d7eb4af7d893da7d340a4a02550a3b50`。
"""

GROUP_ORDER = [
    "A-实体与服务层",
    "B-摄取流水线",
    "C-文档解析",
    "D-文档引擎与存储",
    "E-图执行内核",
    "F-Agent 平台",
    "G-Agentic RAG",
    "H-同步接入与运维面",
]

MECHANISM_SPECS = [
    # ============ A-实体与服务层 ============
    {
        "feature_group": "A-实体与服务层",
        "feature": "entity-model",
        "title": "实体模型与状态枚举",
        "page_file": "a01-entity-model.md",
        "boundary": "负责知识库/文档/会话/任务四类核心实体的字段与状态枚举；"
                    "不负责服务层流程（a02-a05）、不负责摄取流水线（B 组）。",
        "refs": [
            {"path": "internal/entity/dataset.go",
             "symbols": ["Knowledgebase", "ParserType", "TaskStatus", "PipelineTaskType"]},
            {"path": "internal/entity/document.go",
             "symbols": ["Document", "DocumentListItem", "IngestionStatusWith"]},
            {"path": "internal/entity/chat.go", "symbols": ["Chat", "ChatSession"]},
        ],
    },
    {
        "feature_group": "A-实体与服务层",
        "feature": "chat-pipeline",
        "title": "对话编排流水线",
        "page_file": "a02-chat-pipeline.md",
        "boundary": "负责一次对话请求的完整编排（模型选择、知识检索、提示拼装、token 预算、流式输出）；"
                    "不负责模型提供方注册（a03）、不负责检索本身（G 组）。",
        "refs": [
            {"path": "internal/service/chat_pipeline.go",
             "symbols": ["ChatPipelineService", "AsyncChat", "AsyncChatResult", "ThinkEvent"]},
            {"path": "internal/service/chat_pipeline.go", "symbols": ["TurnUsage", "kbPrompt"]},
            {"path": "internal/service/chat_pipeline.go", "symbols": ["messageFitIn"]},
        ],
    },
    {
        "feature_group": "A-实体与服务层",
        "feature": "model-provider",
        "title": "模型提供方与模型工厂",
        "page_file": "a03-model-provider.md",
        "boundary": "负责模型提供方的注册、连通性检查与模型实例构造（对话/嵌入）；"
                    "不负责对话编排（a02）、不负责图执行内核的模型链（E 组）。",
        "refs": [
            {"path": "internal/service/model_service.go",
             "symbols": ["ModelProviderService", "CheckConnectionModelInfo"]},
            {"path": "internal/service/model_factory.go",
             "symbols": ["ModelFactory", "ResolvedModelTarget", "NewChatModel"]},
            {"path": "internal/service/model_factory.go", "symbols": ["NewEmbeddingModel"]},
        ],
    },
    {
        "feature_group": "A-实体与服务层",
        "feature": "metadata-filter",
        "title": "元数据过滤条件生成",
        "page_file": "a04-metadata-filter.md",
        "boundary": "负责把自然语言问题变成可执行的元数据过滤条件、并据此收窄检索范围；"
                    "不负责向量检索（G 组）、不负责实体模型（a01）。",
        "refs": [
            {"path": "internal/service/metadata_filter.go",
             "symbols": ["ApplyMetaFilter", "GenMetaFilter", "MetaFilterResult"]},
            {"path": "internal/service/metadata_filter.go",
             "symbols": ["MetadataConditionToDocIDs"]},
        ],
    },
    {
        "feature_group": "A-实体与服务层",
        "feature": "dataset-artifact",
        "title": "知识库产物与技能检索",
        "page_file": "a05-dataset-artifact.md",
        "boundary": "负责知识库派生产物（wiki 页面、结构图）与技能检索服务；"
                    "不负责对话编排（a02）、不负责知识编译（b06）。",
        "refs": [
            {"path": "internal/service/dataset_artifact_service.go",
             "symbols": ["DatasetArtifactService", "HasWiki", "WikiPageItem"]},
            {"path": "internal/service/skill_search.go",
             "symbols": ["SkillSearchService", "SearchRequest", "SearchResponse"]},
            {"path": "internal/service/dataset_structure_graph.go",
             "symbols": ["StructureGraphNode", "DocumentStructureGraphTemplate"]},
        ],
    },

    # ============ B-摄取流水线 ============
    {
        "feature_group": "B-摄取流水线",
        "feature": "pipeline-executor",
        "title": "摄取任务执行器",
        "page_file": "b01-pipeline-executor.md",
        "boundary": "负责一次文档摄取任务的执行编排（分派、进度、块写入、失败锚点）；"
                    "不负责各组件内部（b02-b05）、不负责编译产物（b06）。",
        "refs": [
            {"path": "internal/ingestion/task/pipeline_executor.go",
             "symbols": ["PipelineExecutor", "PipelineResult", "NewPipelineExecutor"]},
            {"path": "internal/ingestion/task/pipeline_executor.go",
             "symbols": ["insertChunksForIngestion", "parseBaselineAnchor"]},
        ],
    },
    {
        "feature_group": "B-摄取流水线",
        "feature": "parser-component",
        "title": "解析组件与文件族分派",
        "page_file": "b02-parser-component.md",
        "boundary": "负责摄取侧的解析组件、文件族判定与解析器装配；"
                    "不负责解析器实现本身（C 组）、不负责抽取（b03）。",
        "refs": [
            {"path": "internal/ingestion/component/parser.go", "symbols": ["ParserComponent"]},
            {"path": "internal/ingestion/component/parser_dispatch.go",
             "symbols": ["ParserFileFamily", "parserSetupConfigurer"]},
        ],
    },
    {
        "feature_group": "B-摄取流水线",
        "feature": "extractor",
        "title": "实体关系抽取组件",
        "page_file": "b03-extractor.md",
        "boundary": "负责从块文本抽取实体关系与标签（含并发、模型选择、恢复钩子）；"
                    "不负责解析（b02）、不负责分块（b04）。",
        "refs": [
            {"path": "internal/ingestion/component/extractor.go",
             "symbols": ["ExtractorComponent", "extractorJob"]},
            {"path": "internal/ingestion/component/extractor_tag.go",
             "symbols": ["OrderedTagWeights"]},
        ],
    },
    {
        "feature_group": "B-摄取流水线",
        "feature": "chunker-components",
        "title": "四种分块组件",
        "page_file": "b04-chunker-components.md",
        "boundary": "负责摄取阶段的 token / 标题 / 通用 / 分组四种分块组件；"
                    "不负责解析层分块算子（c03）、不负责章节规划（c04）。",
        "refs": [
            {"path": "internal/ingestion/component/chunker/token.go",
             "symbols": ["TokenChunkerComponent", "tokenChunkerParam"]},
            {"path": "internal/ingestion/component/chunker/title.go",
             "symbols": ["TitleChunkerComponent", "outlineEntry"]},
            {"path": "internal/ingestion/component/chunker/general.go",
             "symbols": ["GeneralChunkerComponent"]},
            {"path": "internal/ingestion/component/chunker/group.go",
             "symbols": ["GroupTitleChunkerComponent"]},
        ],
    },
    {
        "feature_group": "B-摄取流水线",
        "feature": "tokenizer-embedder",
        "title": "分词与嵌入组件",
        "page_file": "b05-tokenizer-embedder.md",
        "boundary": "负责分词的批处理与嵌入计算（含截断与模型解析）；"
                    "不负责抽取（b03）、不负责向量库写入（D 组）。",
        "refs": [
            {"path": "internal/ingestion/component/tokenizer.go",
             "symbols": ["TokenizerComponent", "Embedder", "EmbeddingResult"]},
            {"path": "internal/ingestion/component/tokenizer.go",
             "symbols": ["NewTokenizerComponent"]},
        ],
    },
    {
        "feature_group": "B-摄取流水线",
        "feature": "knowledge-compile",
        "title": "知识编译：排产、消费与落盘",
        "page_file": "b06-knowledge-compile.md",
        "boundary": "负责知识编译产物（wiki 页面 / 结构树 / claims）的排产、消费与落盘；"
                    "不负责摄取主链（b01）、不负责抽取（b03）。",
        "refs": [
            {"path": "internal/ingestion/knowledge_compile/writer.go",
             "symbols": ["Writer", "StructureBucket", "pageCommitter"]},
            {"path": "internal/ingestion/knowledge_compile/scheduler.go",
             "symbols": ["Claimer", "Publisher", "ClaimResult"]},
            {"path": "internal/ingestion/knowledge_compile/consumer.go", "symbols": ["Consumer"]},
            {"path": "internal/ingestion/component/knowledge_compiler/tree/claims.go",
             "symbols": ["Claim", "EvidenceRef", "ExtractClaimsForChunks"]},
        ],
    },

    # ============ C-文档解析 ============
    {
        "feature_group": "C-文档解析",
        "feature": "doc-item",
        "title": "文档条目与解析结果",
        "page_file": "c01-doc-item.md",
        "boundary": "负责解析产物的条目模型（文本/表格/图片/原始块）与位置信息；"
                    "不负责解析器注册（c02）、不负责分块算子（c03）。",
        "refs": [
            {"path": "internal/parser/parser/doc_item.go",
             "symbols": ["DocItem", "TextItem", "TableItem", "ImageItem"]},
            {"path": "internal/parser/parser/doc_item.go", "symbols": ["PositionKind", "RawItem"]},
            {"path": "internal/parser/parser/parse_result.go",
             "symbols": ["ParseResult", "NewTextJSONItem"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "parser-registry",
        "title": "解析器注册与按类型分派",
        "page_file": "c02-parser-registry.md",
        "boundary": "负责解析器的注册表、按扩展名/方法分派与文本类解析器实现；"
                    "不负责条目模型（c01）、不负责摄取侧分派（b02）。",
        "refs": [
            {"path": "internal/parser/parser/parser_type.go",
             "symbols": ["TryParser", "GetParser", "RegisterParserFactory", "HasParseMethod"]},
            {"path": "internal/parser/parser/text_parser.go",
             "symbols": ["TextParser", "NewTextParser"]},
            {"path": "internal/parser/parser/doc_parser.go", "symbols": ["DOCParser"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "chunk-operator",
        "title": "分块算子链",
        "page_file": "c03-chunk-operator.md",
        "boundary": "负责解析层的分块算子（预处理 / 切分 / 后处理）与算子上下文；"
                    "不负责摄取层分块组件（b04）、不负责章节规划（c04）。",
        "refs": [
            {"path": "internal/parser/chunk/chunk_type.go",
             "symbols": ["Operator", "ChunkData", "ChunkContext"]},
            {"path": "internal/parser/chunk/split.go",
             "symbols": ["SplitOperator", "NewSplitOperator"]},
            {"path": "internal/parser/chunk/postprocess.go", "symbols": ["PostprocessOperator"]},
            {"path": "internal/parser/chunk/options.go", "symbols": ["ChunkOptions"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "chapter-plan",
        "title": "章节规划与 PDF 切片",
        "page_file": "c04-chapter-plan.md",
        "boundary": "负责把大纲变成自适应章节计划、以及按计划切片 PDF；"
                    "不负责版面合并（c05）、不负责分块算子（c03）。",
        "refs": [
            {"path": "internal/parser/splitter/plan.go",
             "symbols": ["PlanAdaptiveChapters", "sanitizeTitle", "newSectionCoalescer"]},
            {"path": "internal/parser/splitter/slice.go", "symbols": ["SlicePDF"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "pdf-layout",
        "title": "PDF 版面重建",
        "page_file": "c05-pdf-layout.md",
        "boundary": "负责 PDF 文本框的阅读顺序重建、去连字符、分栏归并；"
                    "不负责表格构造（c06）、不负责章节规划（c04）。",
        "refs": [
            {"path": "internal/deepdoc/parser/pdf/layout/layout.go",
             "symbols": ["TextMerge", "FinalReadingOrderMerge", "FilterWatermarkBoxes"]},
            {"path": "internal/deepdoc/parser/pdf/layout/boxes_sections.go",
             "symbols": ["BoxesToSections", "SectionsToJSON"]},
            {"path": "internal/deepdoc/parser/pdf/layout/dehyphen.go", "symbols": ["Dehyphenate"]},
            {"path": "internal/deepdoc/parser/pdf/layout/combined_column.go",
             "symbols": ["AssignColumn"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "pdf-table",
        "title": "PDF 表格重建",
        "page_file": "c06-pdf-table.md",
        "boundary": "负责从版面框构造表格结构、以及表格的 HTML 双向转换；"
                    "不负责版面顺序（c05）、不负责媒体预算（c07）。",
        "refs": [
            {"path": "internal/deepdoc/parser/pdf/table/table_construct.go",
             "symbols": ["ConstructTable", "StripCaptionFromCells"]},
            {"path": "internal/parser/tableutil/table_html.go",
             "symbols": ["ParseTableHTML", "RenderTableHTML", "BuildTableData"]},
        ],
    },
    {
        "feature_group": "C-文档解析",
        "feature": "media-budget",
        "title": "媒体预算与落盘",
        "page_file": "c07-media-budget.md",
        "boundary": "负责解析阶段图片等媒体的体量预算、准入与落盘；"
                    "不负责表格（c06）、不负责视觉增强（b 组）。",
        "refs": [
            {"path": "internal/parser/media/budget.go",
             "symbols": ["Budget", "Reservation", "NewBudget"]},
            {"path": "internal/parser/media/sink.go", "symbols": ["Sink", "NewSink"]},
            {"path": "internal/parser/media/limits.go", "symbols": ["Limits", "LoadLimits"]},
        ],
    },

    # ============ D-文档引擎与存储 ============
    {
        "feature_group": "D-文档引擎与存储",
        "feature": "doc-engine",
        "title": "文档引擎抽象",
        "page_file": "d01-doc-engine.md",
        "boundary": "负责块存储引擎的抽象契约、引擎类型枚举与消息队列；"
                    "不负责各后端实现（d02/d03）、不负责 blob 存储（d04）。",
        "refs": [
            {"path": "internal/engine/engine.go",
             "symbols": ["EngineType", "DocEngine", "MessageQueue"]},
        ],
    },
    {
        "feature_group": "D-文档引擎与存储",
        "feature": "chunk-store-es",
        "title": "ES 与 OpenSearch 块存储",
        "page_file": "d02-chunk-store-es.md",
        "boundary": "负责 Elasticsearch 与 OpenSearch 两个后端的块存储与检索实现差异；"
                    "不负责引擎抽象（d01）、不负责 Infinity（d03）。",
        "refs": [
            {"path": "internal/engine/elasticsearch/chunk.go", "symbols": ["SearchResponse"]},
            {"path": "internal/engine/opensearch/chunk.go", "symbols": ["SearchResponse"]},
        ],
    },
    {
        "feature_group": "D-文档引擎与存储",
        "feature": "infinity-engine",
        "title": "Infinity 引擎与块分页排序",
        "page_file": "d03-infinity-engine.md",
        "boundary": "负责 Infinity 后端的块存储、分页排序与检索；"
                    "不负责 ES/OpenSearch（d02）、不负责 blob 存储（d04）。",
        "refs": [
            {"path": "internal/engine/infinity/chunk.go",
             "symbols": ["CreateChunkStore", "InsertChunks", "Search", "DeleteChunks"]},
        ],
    },
    {
        "feature_group": "D-文档引擎与存储",
        "feature": "blob-storage",
        "title": "对象存储后端",
        "page_file": "d04-blob-storage.md",
        "boundary": "负责内存/MinIO/OSS/GCS 四种对象存储实现与统一删除；"
                    "不负责块引擎（d01-d03）、不负责媒体预算（c07）。",
        "refs": [
            {"path": "internal/storage/memory.go",
             "symbols": ["MemoryStorage", "NewMemoryStorage"]},
            {"path": "internal/storage/minio.go",
             "symbols": ["MinioStorage", "NewMinioStorage"]},
            {"path": "internal/storage/oss.go", "symbols": ["OSSStorage", "NewOSSStorage"]},
            {"path": "internal/storage/remove.go", "symbols": ["RemoveBlob"]},
        ],
    },

    # ============ E-图执行内核 ============
    {
        "feature_group": "E-图执行内核",
        "feature": "graph-api",
        "title": "状态图装配接口",
        "page_file": "e01-graph-api.md",
        "boundary": "负责状态图的构建入口、检查点/中断选项与字段映射；"
                    "不负责通道（e02）、不负责执行引擎（e04）。",
        "refs": [
            {"path": "internal/harness/graph/graph/graph.go",
             "symbols": ["NewStateGraph", "WithCheckpointer", "WithInterrupts", "MapFields"]},
            {"path": "internal/harness/harness.go",
             "symbols": ["NewStateGraph", "NewMemorySaver", "DefaultRetryPolicy"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "graph-channels",
        "title": "通道与屏障汇合",
        "page_file": "e02-graph-channels.md",
        "boundary": "负责状态通道的契约、归并语义与多节点汇合屏障；"
                    "不负责图装配（e01）、不负责执行循环（e04）。",
        "refs": [
            {"path": "internal/harness/graph/channels/base.go", "symbols": ["Channel", "BaseChannel"]},
            {"path": "internal/harness/graph/channels/barrier.go",
             "symbols": ["NamedBarrierValue", "LastValueAfterFinish"]},
            {"path": "internal/harness/graph/channels/reducer.go",
             "symbols": ["ReducerChannel", "NewReducerChannel"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "checkpoint",
        "title": "检查点与待写记录",
        "page_file": "e03-checkpoint.md",
        "boundary": "负责检查点结构、待写记录与检查点存储契约；"
                    "不负责中断恢复（e06）、不负责执行引擎（e04）。",
        "refs": [
            {"path": "internal/harness/graph/checkpoint/checkpoint.go",
             "symbols": ["BaseCheckpointer", "Checkpoint", "CheckpointTuple", "PendingWrite"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "pregel-engine",
        "title": "Pregel 执行引擎",
        "page_file": "e04-pregel-engine.md",
        "boundary": "负责超步执行循环、任务调度与优先级队列；"
                    "不负责流式输出（e05）、不负责通道（e02）。",
        "refs": [
            {"path": "internal/harness/graph/pregel/engine.go",
             "symbols": ["Engine", "NewEngine", "ExecuteResult"]},
            {"path": "internal/harness/graph/pregel/optimized.go",
             "symbols": ["PregelOptimizedEngine", "PriorityTaskQueue"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "pregel-stream",
        "title": "流式事件与消息聚合",
        "page_file": "e05-pregel-stream.md",
        "boundary": "负责执行过程的流式事件、消息聚合与增量输出；"
                    "不负责执行循环（e04）、不负责中断（e06）。",
        "refs": [
            {"path": "internal/harness/graph/pregel/stream.go",
             "symbols": ["StreamManager", "NewStreamManager", "StreamEvent"]},
            {"path": "internal/harness/graph/pregel/messages.go",
             "symbols": ["StreamMessagesHandler", "MessageAggregator"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "interrupt",
        "title": "中断与恢复",
        "page_file": "e06-interrupt.md",
        "boundary": "负责人工介入式中断、恢复值收集与节点触发模式；"
                    "不负责检查点结构（e03）、不负责工具节点（e08）。",
        "refs": [
            {"path": "internal/harness/graph/interrupt/interrupt.go",
             "symbols": ["WithInterruptContext", "GetResumeValues", "Interrupt"]},
            {"path": "internal/harness/graph/types/types.go",
             "symbols": ["NodeTriggerMode", "StreamMode", "Command", "Send"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "react-agent",
        "title": "ReAct Agent 与图形态",
        "page_file": "e07-react-agent.md",
        "boundary": "负责 ReAct 循环的配置、Agent 对象与图形态实现；"
                    "不负责工具节点（e08）、不负责中间件（e09）。",
        "refs": [
            {"path": "internal/harness/core/react_agent.go",
             "symbols": ["ReActAgent", "ReActConfig", "NewReActAgent"]},
            {"path": "internal/harness/core/react_graph.go",
             "symbols": ["ReActGraph", "NewReActGraph", "ReActGraphState"]},
            {"path": "internal/harness/core/agent_loop.go", "symbols": ["AgentLoop", "NewAgentLoop"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "tools-node",
        "title": "工具节点与工具注册表",
        "page_file": "e08-tools-node.md",
        "boundary": "负责工具执行节点、循环守卫与工具注册表；"
                    "不负责 Agent 循环（e07）、不负责平台工具实现（f04）。",
        "refs": [
            {"path": "internal/harness/core/tools_node.go",
             "symbols": ["ToolsNode", "ToolsNodeConfig", "LoopGuard"]},
            {"path": "internal/harness/core/tool_registry.go",
             "symbols": ["ToolRegistry", "NewToolRegistry"]},
            {"path": "internal/harness/core/contracts.go",
             "symbols": ["Tool", "Model", "ToolCapability", "InvokableToolEndpoint"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "middlewares",
        "title": "中间件体系",
        "page_file": "e09-middlewares.md",
        "boundary": "负责摘要压缩、文件系统、任务清单与子 Agent 四类中间件的挂载协议；"
                    "不负责预制 Agent（e10）、不负责工具节点（e08）。",
        "refs": [
            {"path": "internal/harness/core/middlewares/summarization/summarization.go",
             "symbols": ["NewTyped", "BeforeModelRewrite"]},
            {"path": "internal/harness/core/middlewares/filesystem/filesystem.go",
             "symbols": ["ContributeTools", "BeforeAgent"]},
            {"path": "internal/harness/core/middlewares/plantask/plantask.go",
             "symbols": ["Manager", "TaskState"]},
            {"path": "internal/harness/core/middlewares/subagent/subagent.go",
             "symbols": ["SubAgentSpec", "SubAgentMiddleware"]},
        ],
    },
    {
        "feature_group": "E-图执行内核",
        "feature": "prebuilt-agents",
        "title": "预制 Agent 形态",
        "page_file": "e10-prebuilt-agents.md",
        "boundary": "负责开箱可用的 Agent 形态（深度任务/Supervisor）及其配置；"
                    "不负责中间件（e09）、不负责 ReAct 循环（e07）。",
        "refs": [
            {"path": "internal/harness/core/prebuilt/deep/deep.go",
             "symbols": ["TaskManager", "NewWithSubAgents"]},
            {"path": "internal/harness/core/prebuilt/supervisor/supervisor.go",
             "symbols": ["AgentSpec"]},
        ],
    },

    # ============ F-Agent 平台 ============
    {
        "feature_group": "F-Agent 平台",
        "feature": "canvas",
        "title": "画布与组件工厂",
        "page_file": "f01-canvas.md",
        "boundary": "负责 Agent 画布的状态、组件模型与组件工厂；"
                    "不负责 DSL 解析（f02）、不负责运行时上下文（f03）。",
        "refs": [
            {"path": "internal/agent/canvas/canvas.go",
             "symbols": ["Canvas", "CanvasState", "CanvasComponentObj", "ComponentFactory"]},
        ],
    },
    {
        "feature_group": "F-Agent 平台",
        "feature": "dsl",
        "title": "DSL 抽取与归一",
        "page_file": "f02-dsl.md",
        "boundary": "负责把画布 JSON 抽成组件参数、并归一成可运行/可保存两种形态；"
                    "不负责画布模型（f01）、不负责运行时（f03）。",
        "refs": [
            {"path": "internal/agent/dsl/extract.go",
             "symbols": ["ExtractComponentParams", "ExtractMode", "ExtractPrologue"]},
            {"path": "internal/agent/dsl/normalize.go",
             "symbols": ["NormalizeForCanvas", "NormalizeForRun"]},
        ],
    },
    {
        "feature_group": "F-Agent 平台",
        "feature": "agent-runtime",
        "title": "Agent 运行时",
        "page_file": "f03-agent-runtime.md",
        "boundary": "负责组件执行上下文、消息发射与延迟流；"
                    "不负责 DSL（f02）、不负责工具注册表（f04）。",
        "refs": [
            {"path": "internal/agent/runtime/context.go",
             "symbols": ["AgentMessageEmitter", "DeferredStream", "WithDeferredNodeRegistry"]},
            {"path": "internal/agent/runtime/component.go",
             "symbols": ["Component", "ComponentFactory", "UserFacingError"]},
            {"path": "internal/agent/component/base.go",
             "symbols": ["Component", "ParamBase", "ParamError"]},
        ],
    },
    {
        "feature_group": "F-Agent 平台",
        "feature": "tool-registry",
        "title": "平台工具注册与检索工具",
        "page_file": "f04-tool-registry.md",
        "boundary": "负责平台侧工具的注册与按名构建、以及知识库检索工具；"
                    "不负责图执行内核的工具注册表（e08）、不负责沙箱（f05）。",
        "refs": [
            {"path": "internal/agent/tool/registry.go", "symbols": ["Factory", "BuildByName"]},
            {"path": "internal/agent/tool/retrieval.go",
             "symbols": ["RetrievalTool", "NewRetrievalTool"]},
        ],
    },
    {
        "feature_group": "F-Agent 平台",
        "feature": "sandbox",
        "title": "代码执行沙箱",
        "page_file": "f05-sandbox.md",
        "boundary": "负责沙箱提供方的目录、配置校验与本地沙箱实现；"
                    "不负责工具注册（f04）、不负责工作流（f06）。",
        "refs": [
            {"path": "internal/agent/sandbox/catalog.go",
             "symbols": ["ConfigSchema", "ListProviders", "ValidateConfig"]},
            {"path": "internal/agent/sandbox/local.go",
             "symbols": ["LocalProvider", "newLocalProviderFromEnv"]},
        ],
    },
    {
        "feature_group": "F-Agent 平台",
        "feature": "workflowx",
        "title": "循环与并行结构",
        "page_file": "f06-workflowx.md",
        "boundary": "负责 Agent 工作流的循环与并行两种控制结构的选项与检查点；"
                    "不负责画布（f01）、不负责图执行内核（E 组）。",
        "refs": [
            {"path": "internal/agent/workflowx/loop.go",
             "symbols": ["WithLoopMaxIterations", "WithLoopLifecycleHooks"]},
            {"path": "internal/agent/workflowx/parallel.go",
             "symbols": ["WithParallelMaxConcurrency"]},
        ],
    },

    # ============ G-Agentic RAG ============
    {
        "feature_group": "G-Agentic RAG",
        "feature": "agentic-run",
        "title": "Agentic 检索主循环",
        "page_file": "g01-agentic-run.md",
        "boundary": "负责 agentic 检索的主循环与三类台账（文档 / 块读取 / 已服务）；"
                    "不负责知识图谱服务（g02）、不负责 grep（g03）。",
        "refs": [
            {"path": "internal/agentic_rag/agent.go",
             "symbols": ["Run", "NewDocIDLedger", "NewServedLedger", "NewChunkReadLedger"]},
        ],
    },
    {
        "feature_group": "G-Agentic RAG",
        "feature": "kg-service",
        "title": "知识图谱服务与探索",
        "page_file": "g02-kg-service.md",
        "boundary": "负责实体关系图的查询与多跳探索服务；"
                    "不负责主循环（g01）、不负责 grep（g03）。",
        "refs": [
            {"path": "internal/agentic_rag/kg_service.go",
             "symbols": ["KgService", "GetKgService", "KgExploreRequest", "KgEntity"]},
            {"path": "internal/agentic_rag/kg_service.go", "symbols": ["KgAdapter"]},
        ],
    },
    {
        "feature_group": "G-Agentic RAG",
        "feature": "grep-service",
        "title": "精确串检索服务",
        "page_file": "g03-grep-service.md",
        "boundary": "负责字面串检索通道的适配；"
                    "不负责图探索（g02）、不负责结构导航（g04）。",
        "refs": [
            {"path": "internal/agentic_rag/grep_service.go",
             "symbols": ["GrepAdapter", "NewGrepAdapter"]},
        ],
    },
    {
        "feature_group": "G-Agentic RAG",
        "feature": "structure-and-search-tools",
        "title": "结构导航与 agentic 检索工具",
        "page_file": "g04-structure-and-search-tools.md",
        "boundary": "负责结构树导航工具与 agentic 检索工具的对外形态；"
                    "不负责主循环（g01）、不负责平台工具注册（f04）。",
        "refs": [
            {"path": "internal/agentic_rag/tool_navigate_structure.go",
             "symbols": ["NavigateStructureTool"]},
            {"path": "internal/agent/tool/agentic_search.go",
             "symbols": ["AgenticSearchTool", "NewAgenticSearchTool"]},
        ],
    },

    # ============ H-同步接入与运维面 ============
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "syncer",
        "title": "同步器：调度与任务执行",
        "page_file": "h01-syncer.md",
        "boundary": "负责外部数据源的同步调度、任务队列与执行；"
                    "不负责锁与检查点（h02）、不负责渠道（h03）。",
        "refs": [
            {"path": "internal/syncer/syncer.go", "symbols": ["Syncer", "NewSyncer"]},
            {"path": "internal/syncer/scheduler.go",
             "symbols": ["Scheduler", "NewScheduler", "TaskEnvelope"]},
            {"path": "internal/syncer/job_executor.go",
             "symbols": ["SyncJobExecutor", "NewSyncJobExecutor"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "syncer-lock",
        "title": "同步锁与检查点",
        "page_file": "h02-syncer-lock.md",
        "boundary": "负责连接器级互斥锁与同步进度检查点；"
                    "不负责调度（h01）、不负责渠道（h03）。",
        "refs": [
            {"path": "internal/syncer/connector_lock.go",
             "symbols": ["ConnectorLock", "NewConnectorLock", "TryLock"]},
            {"path": "internal/syncer/checkpoint_store.go",
             "symbols": ["SyncCheckpointStore", "LoadSyncCheckpoint"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "chat-channels",
        "title": "渠道接入与出站",
        "page_file": "h03-chat-channels.md",
        "boundary": "负责把 Agent 接入即时通讯渠道（飞书等）及出站转发；"
                    "不负责同步器（h01）、不负责 MCP（h04）。",
        "refs": [
            {"path": "internal/channels/bootstrap.go", "symbols": ["Runtime", "NewRuntime"]},
            {"path": "internal/channels/feishu.go",
             "symbols": ["SetMessageHandler", "Start", "Stop"]},
            {"path": "internal/channels/egress.go", "symbols": ["RoundTrip"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "mcp",
        "title": "MCP 服务端与连接器",
        "page_file": "h04-mcp.md",
        "boundary": "负责对外的 MCP 服务端与内部服务连接器；"
                    "不负责渠道（h03）、不负责 CLI（h05）。",
        "refs": [
            {"path": "internal/mcp/connector.go",
             "symbols": ["ServiceConnector", "NewServiceConnector", "Retrieval"]},
            {"path": "internal/mcp/http.go", "symbols": ["Handler", "NewHandler"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "cli",
        "title": "命令行与管理命令",
        "page_file": "h05-cli.md",
        "boundary": "负责命令行入口的参数解析与运行、以及管理与用户命令集；"
                    "不负责 MCP（h04）、不负责权限（h06）。",
        "refs": [
            {"path": "internal/cli/cli.go",
             "symbols": ["CLI", "ParseArgs", "Run", "CommandLineConfig"]},
            {"path": "internal/cli/admin_command.go",
             "symbols": ["AdminPingStoreCommand", "AdminListProvidersCommand"]},
            {"path": "internal/cli/user_command.go",
             "symbols": ["RegisterUser", "APIListDatasetsCommand"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "permission",
        "title": "资源权限模型",
        "page_file": "h06-permission.md",
        "boundary": "负责资源引用、特性开关与按租户的权限判定；"
                    "不负责 CLI（h05）、不负责运行时公共结构（h07）。",
        "refs": [
            {"path": "internal/permission/policy.go",
             "symbols": ["CheckFeature", "CheckResource", "ResolveAccess"]},
            {"path": "internal/permission/types.go",
             "symbols": ["ResourceKind", "ResourceRef", "NewChecker"]},
        ],
    },
    {
        "feature_group": "H-同步接入与运维面",
        "feature": "runtime-common",
        "title": "公共常量、Agent 事件与分词器",
        "page_file": "h07-runtime-common.md",
        "boundary": "负责跨包共享的状态判定函数、Agent 事件结构与分词器入口；"
                    "不负责权限（h06）、不负责摄取侧分词组件（b05）。",
        "refs": [
            {"path": "internal/common/constants.go",
             "symbols": ["DialogStatus", "IsTerminalTaskStatus"]},
            {"path": "internal/common/agent_event.go",
             "symbols": ["AgentEvent", "AgentEventSnapshot"]},
            {"path": "internal/tokenizer/tokenizer.go", "symbols": ["Tokenizer", "New", "Tokenize"]},
        ],
    },
]
