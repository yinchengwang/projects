# -*- coding: utf-8 -*-
"""haystack 机制清单（阶段 2C 第三个项目）。

盘点范围：`haystack/` 包本体（299 个 py 文件）。`test/`、`e2e/`、`examples/`、
`docs-website/` 不建页。

haystack 与 2C 前两个项目的根本差别是**组织范式**：
- langchain 的核心是「可运行对象」的表达式组合（`|` 起来的管道）；
- llama-index 的核心是「节点 + 索引」，按 RAG 阶段纵向分层；
- haystack 的核心是**有类型的组件 DAG**：组件声明带类型的输入/输出 socket，管道是
  一张图，运行前做静态校验（socket 类型、必填、可变参数），运行期按 socket 就绪
  状态调度。所以它有 langchain/llama-index 都没有的东西：**管道快照与断点续跑**、
  **反序列化的模块白名单**、**类型兼容与自动转换策略**。

这决定了本文库的组划分：A/B 两组是这套骨架本体（组件契约、管道调度、socket 类型、
类型兼容、序列化与安全），C 组是数据模型，D-I 组是各组件的机制。

盘点方法：`dump_python_defs.py` 建 AST 定义索引，再按职责归并。组件层有 135 个 py
文件（18 个类别），大量是不同后端适配（OpenAI/Azure/Mock），机制重复度高，按类别
归并成机制页而不是按文件建页。

源码锚点：`haystack@979c6fdd685fbf452edba594f7c5d5304b9c668b`。
"""

GROUP_ORDER = [
    "A-组件与管道骨架",
    "B-类型与序列化",
    "C-数据模型",
    "D-文档存储与过滤",
    "E-预处理与转换",
    "F-检索与重排",
    "G-路由",
    "H-Agent 与工具",
    "I-评测与可观测",
]

MECHANISM_SPECS = [
    # ============ A-组件与管道骨架 ============
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "component-contract",
        "title": "组件契约与元类校验",
        "page_file": "a01-component-contract.md",
        "boundary": "负责组件的抽象契约（run/run_async 签名）、元类在类创建期做的校验、以及组件类型注册；"
                    "不负责管道调度（a02）、不负责 socket 类型（a04）。",
        "refs": [
            {"path": "haystack/core/component/component.py",
             "symbols": ["Component", "ComponentMeta", "_Component"]},
            {"path": "haystack/core/component/component.py",
             "symbols": ["_compare_run_methods_signatures", "_hook_component_init"]},
        ],
    },
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "pipeline-run",
        "title": "管道调度与流式运行",
        "page_file": "a02-pipeline-run.md",
        "boundary": "负责管道的图结构与运行循环（就绪判定、调度、优先级、最大运行次数）、以及流式句柄；"
                    "不负责组件契约（a01）、不负责就绪判定细则（a03）。",
        "refs": [
            {"path": "haystack/core/pipeline/base.py",
             "symbols": ["PipelineBase", "ComponentPriority"]},
            {"path": "haystack/core/pipeline/pipeline.py",
             "symbols": ["Pipeline", "PipelineStreamHandle"]},
        ],
    },
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "component-checks",
        "title": "组件就绪判定",
        "page_file": "a03-component-checks.md",
        "boundary": "负责判断「一个组件此刻能否运行」的全部条件（触发、socket ready、可变参数收齐、用户输入）；"
                    "不负责调度循环（a02）、不负责 socket 结构（a04）。",
        "refs": [
            {"path": "haystack/core/pipeline/component_checks.py",
             "symbols": ["can_component_run", "are_all_sockets_ready", "has_any_trigger"]},
            {"path": "haystack/core/pipeline/component_checks.py",
             "symbols": ["has_lazy_variadic_socket_received_all_inputs",
                         "can_not_receive_inputs_from_pipeline", "has_user_input"]},
        ],
    },
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "sockets",
        "title": "输入输出 socket",
        "page_file": "a04-sockets.md",
        "boundary": "负责 socket 的表示、必填/可变参数标记与类型描述符；"
                    "不负责就绪判定（a03）、不负责类型兼容（b01）。",
        "refs": [
            {"path": "haystack/core/component/sockets.py", "symbols": ["Sockets"]},
            {"path": "haystack/core/component/types.py",
             "symbols": ["InputSocket", "OutputSocket", "InputSocketTypeDescriptor"]},
        ],
    },
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "super-component",
        "title": "超级组件与映射包装",
        "page_file": "a05-super-component.md",
        "boundary": "负责把一张子图包装成单个组件、输入输出映射的类型兼容检查与委托默认值；"
                    "不负责管道本体（a02）、不负责类型兼容细则（b01）。",
        "refs": [
            {"path": "haystack/core/super_component/super_component.py",
             "symbols": ["SuperComponent", "super_component", "_SuperComponent"]},
            {"path": "haystack/core/super_component/utils.py",
             "symbols": ["_types_are_compatible", "_is_compatible", "_unwrap_variadics"]},
        ],
    },
    {
        "feature_group": "A-组件与管道骨架",
        "feature": "breakpoints",
        "title": "断点与管道快照",
        "page_file": "a06-breakpoints.md",
        "boundary": "负责在管道中途保存/恢复状态（快照的结构、校验、序列化）；"
                    "不负责调度循环（a02）、不负责序列化机制本身（b02）。",
        "refs": [
            {"path": "haystack/dataclasses/breakpoints.py",
             "symbols": ["Breakpoint", "PipelineState", "PipelineSnapshot"]},
            {"path": "haystack/core/pipeline/breakpoint.py",
             "symbols": ["_create_pipeline_snapshot", "load_pipeline_snapshot"]},
        ],
    },

    # ============ B-类型与序列化 ============
    {
        "feature_group": "B-类型与序列化",
        "feature": "type-compatibility",
        "title": "类型兼容与自动转换",
        "page_file": "b01-type-compatibility.md",
        "boundary": "负责 socket 连接时的类型兼容判定与运行期的自动转换策略；"
                    "不负责组件契约（a01）、不负责序列化（b02）。",
        "refs": [
            {"path": "haystack/core/type_utils.py",
             "symbols": ["ConversionStrategy", "_types_are_compatible",
                         "_strict_types_are_compatible"]},
            {"path": "haystack/core/type_utils.py",
             "symbols": ["_get_conversion_strategy", "_convert_value", "_chat_message_to_str"]},
        ],
    },
    {
        "feature_group": "B-类型与序列化",
        "feature": "serialization",
        "title": "组件序列化",
        "page_file": "b02-serialization.md",
        "boundary": "负责组件与管道到字典的序列化/反序列化、默认实现的字段约定；"
                    "不负责反序列化安全（b03）、不负责 YAML 编组（b04）。",
        "refs": [
            {"path": "haystack/core/serialization.py",
             "symbols": ["component_to_dict", "component_from_dict", "default_to_dict",
                         "default_from_dict"]},
            {"path": "haystack/core/serialization.py",
             "symbols": ["generate_qualified_class_name", "import_class_by_name"]},
        ],
    },
    {
        "feature_group": "B-类型与序列化",
        "feature": "deserialization-security",
        "title": "反序列化的模块白名单",
        "page_file": "b03-deserialization-security.md",
        "boundary": "负责反序列化时允许 import 哪些模块的判定（上下文标记、路径穿越检查、环境变量白名单）；"
                    "不负责序列化字段约定（b02）、不负责 YAML 编组（b04）。",
        "refs": [
            {"path": "haystack/core/serialization_security.py",
             "symbols": ["allow_deserialization_module", "_is_module_allowed",
                         "_check_resolved_module_allowed"]},
            {"path": "haystack/core/serialization_security.py",
             "symbols": ["_is_unsafe_deserialization", "_check_traversable_attribute",
                         "_is_deserialization_internal"]},
        ],
    },
    {
        "feature_group": "B-类型与序列化",
        "feature": "marshal",
        "title": "YAML 编组与可调用对象序列化",
        "page_file": "b04-marshal.md",
        "boundary": "负责管道/组件的 YAML 编组接口、以及可调用对象与类型注解的序列化；"
                    "不负责字典级序列化（b02）、不负责安全策略（b03）。",
        "refs": [
            {"path": "haystack/marshal/protocol.py", "symbols": ["Marshaller"]},
            {"path": "haystack/marshal/yaml.py",
             "symbols": ["YamlMarshaller", "YamlDumper", "YamlLoader"]},
            {"path": "haystack/utils/callable_serialization.py",
             "symbols": ["serialize_callable", "deserialize_callable"]},
        ],
    },

    # ============ C-数据模型 ============
    {
        "feature_group": "C-数据模型",
        "feature": "document",
        "title": "文档模型与字段约束",
        "page_file": "c01-document.md",
        "boundary": "负责 Document 的字段、序列化与遗留字段清理；"
                    "不负责聊天消息（c02）、不负责流式块（c03）。",
        "refs": [
            {"path": "haystack/dataclasses/document.py", "symbols": ["Document", "_RemoveLegacyFields"]},
            {"path": "haystack/dataclasses/file_content.py", "symbols": ["FileContent"]},
            {"path": "haystack/dataclasses/image_content.py", "symbols": ["ImageContent"]},
        ],
    },
    {
        "feature_group": "C-数据模型",
        "feature": "chat-message",
        "title": "聊天消息与内容块",
        "page_file": "c02-chat-message.md",
        "boundary": "负责聊天消息的角色、多模态内容块与 OpenAI 形态的互转；"
                    "不负责文档（c01）、不负责流式增量（c03）。",
        "refs": [
            {"path": "haystack/dataclasses/chat_message.py",
             "symbols": ["ChatMessage", "ChatRole", "TextContent", "ToolCall"]},
            {"path": "haystack/dataclasses/chat_message.py",
             "symbols": ["_haystack_content_part_to_openai_dict",
                         "_openai_dict_to_haystack_content_part", "_parse_openai_base64_data_url"]},
        ],
    },
    {
        "feature_group": "C-数据模型",
        "feature": "streaming-and-answer",
        "title": "流式块与回答构建",
        "page_file": "c03-streaming-and-answer.md",
        "boundary": "负责流式块的结构与流式回调选择、以及把生成结果装配成回答对象；"
                    "不负责消息（c02）、不负责生成器组件本身（H 组）。",
        "refs": [
            {"path": "haystack/dataclasses/streaming_chunk.py",
             "symbols": ["StreamingChunk", "ToolCallDelta", "select_streaming_callback"]},
            {"path": "haystack/dataclasses/answer.py",
             "symbols": ["GeneratedAnswer", "ExtractedAnswer"]},
            {"path": "haystack/components/builders/answer_builder.py", "symbols": ["AnswerBuilder"]},
        ],
    },

    # ============ D-文档存储与过滤 ============ \\
    {
        "feature_group": "D-文档存储与过滤",
        "feature": "document-store-contract",
        "title": "文档存储契约与写入策略",
        "page_file": "d01-document-store-contract.md",
        "boundary": "负责文档存储的协议、重复写入策略与同步/异步两套入口；"
                    "不负责过滤表达式（d02）、不负责内存实现（d03）。",
        "refs": [
            {"path": "haystack/document_stores/types/protocol.py", "symbols": ["DocumentStore"]},
            {"path": "haystack/document_stores/types/policy.py", "symbols": ["DuplicatePolicy"]},
        ],
    },
    {
        "feature_group": "D-文档存储与过滤",
        "feature": "filter-policy",
        "title": "过滤表达式与策略合并",
        "page_file": "d02-filter-policy.md",
        "boundary": "负责元数据过滤表达式的求值、初始化期与运行期策略的合并规则；"
                    "不负责存储契约（d01）、不负责内存实现（d03）。",
        "refs": [
            {"path": "haystack/document_stores/types/filter_policy.py",
             "symbols": ["FilterPolicy", "apply_filter_policy", "is_comparison_filter"]},
            {"path": "haystack/document_stores/types/filter_policy.py",
             "symbols": ["combine_init_comparison_and_runtime_logical_filters",
                         "combine_two_logical_filters"]},
            {"path": "haystack/utils/filters.py", "symbols": ["document_matches_filter"]},
        ],
    },
    {
        "feature_group": "D-文档存储与过滤",
        "feature": "in-memory-store",
        "title": "内存文档存储与 BM25",
        "page_file": "d03-in-memory-store.md",
        "boundary": "负责内存版文档存储的实现（索引结构、BM25 统计、并发保护）；"
                    "不负责存储契约（d01）、不负责过滤策略（d02）。",
        "refs": [
            {"path": "haystack/document_stores/in_memory/document_store.py",
             "symbols": ["InMemoryDocumentStore", "BM25DocumentStats"]},
            {"path": "haystack/document_stores/in_memory/document_store.py", "symbols": ["_synchronized"]},
        ],
    },

    # ============ E-预处理与转换 ============
    {
        "feature_group": "E-预处理与转换",
        "feature": "document-splitter",
        "title": "文档切分器",
        "page_file": "e01-document-splitter.md",
        "boundary": "负责按句/词/段落切分文档并写回元数据；"
                    "不负责结构化切分（e02）、不负责语义切分（e03）。",
        "refs": [
            {"path": "haystack/components/preprocessors/document_splitter.py",
             "symbols": ["DocumentSplitter"]},
            {"path": "haystack/components/preprocessors/document_preprocessor.py",
             "symbols": ["DocumentPreprocessor"]},
        ],
    },
    {
        "feature_group": "E-预处理与转换",
        "feature": "structured-splitters",
        "title": "结构化与层级切分",
        "page_file": "e02-structured-splitters.md",
        "boundary": "负责按 Markdown/MS Word 样式标题切分、递归切分与层级切分；"
                    "不负责普通切分器（e01）、不负责语义切分（e03）。",
        "refs": [
            {"path": "haystack/components/preprocessors/markdown_header_splitter.py",
             "symbols": ["MarkdownHeaderSplitter"]},
            {"path": "haystack/components/preprocessors/recursive_splitter.py",
             "symbols": ["RecursiveDocumentSplitter"]},
            {"path": "haystack/components/preprocessors/hierarchical_document_splitter.py",
             "symbols": ["HierarchicalDocumentSplitter"]},
        ],
    },
    {
        "feature_group": "E-预处理与转换",
        "feature": "semantic-splitter",
        "title": "基于嵌入的语义切分",
        "page_file": "e03-semantic-splitter.md",
        "boundary": "负责用嵌入相似度决定切分边界；"
                    "不负责普通切分（e01）、不负责结构化切分（e02）。",
        "refs": [
            {"path": "haystack/components/preprocessors/embedding_based_document_splitter.py",
             "symbols": ["EmbeddingBasedDocumentSplitter"]},
            {"path": "haystack/components/preprocessors/document_splitter.py",
             "symbols": ["DocumentSplitter"]},
        ],
    },
    {
        "feature_group": "E-预处理与转换",
        "feature": "converters",
        "title": "格式转换器",
        "page_file": "e04-converters.md",
        "boundary": "负责把各来源格式（Markdown/HTML/DOCX/CSV）转成 Document；"
                    "不负责切分（e01-e03）、不负责抓取（e 组之外）。",
        "refs": [
            {"path": "haystack/components/converters/markdown.py",
             "symbols": ["MarkdownToDocument"]},
            {"path": "haystack/components/converters/html.py", "symbols": ["HTMLToDocument"]},
            {"path": "haystack/components/converters/docx.py",
             "symbols": ["DOCXToDocument", "DOCXTableFormat"]},
        ],
    },

    # ============ F-检索与重排 ============
    {
        "feature_group": "F-检索与重排",
        "feature": "embedding-retrievers",
        "title": "嵌入检索器",
        "page_file": "f01-embedding-retrievers.md",
        "boundary": "负责把查询嵌入后到文档存储检索、多查询扩展与多路合并；"
                    "不负责层级检索（f02）、不负责重排（f03）。",
        "refs": [
            {"path": "haystack/components/retrievers/text_embedding_retriever.py",
             "symbols": ["TextEmbeddingRetriever"]},
            {"path": "haystack/components/retrievers/multi_query_embedding_retriever.py",
             "symbols": ["MultiQueryEmbeddingRetriever"]},
            {"path": "haystack/components/retrievers/multi_retriever.py",
             "symbols": ["MultiRetriever"]},
        ],
    },
    {
        "feature_group": "F-检索与重排",
        "feature": "hierarchical-retrievers",
        "title": "自动合并与句子窗口检索",
        "page_file": "f02-hierarchical-retrievers.md",
        "boundary": "负责基于父子关系的两种「小块检索、大块返回」检索器；"
                    "不负责普通嵌入检索（f01）、不负责重排（f03）。",
        "refs": [
            {"path": "haystack/components/retrievers/auto_merging_retriever.py",
             "symbols": ["AutoMergingRetriever"]},
            {"path": "haystack/components/retrievers/sentence_window_retriever.py",
             "symbols": ["SentenceWindowRetriever"]},
        ],
    },
    {
        "feature_group": "F-检索与重排",
        "feature": "rankers",
        "title": "重排器",
        "page_file": "f03-rankers.md",
        "boundary": "负责检索结果的重排（LLM 重排、元数据加权、长上下文位置调整、多向量打分）；"
                    "不负责检索（f01/f02）、不负责合并（f04）。",
        "refs": [
            {"path": "haystack/components/rankers/llm_ranker.py", "symbols": ["LLMRanker"]},
            {"path": "haystack/components/rankers/meta_field.py", "symbols": ["MetaFieldRanker"]},
            {"path": "haystack/components/rankers/lost_in_the_middle.py",
             "symbols": ["LostInTheMiddleRanker"]},
            {"path": "haystack/components/rankers/max_sim.py", "symbols": ["MaxSimRanker"]},
        ],
    },
    {
        "feature_group": "F-检索与重排",
        "feature": "joiners",
        "title": "结果合并与分支汇合",
        "page_file": "f04-joiners.md",
        "boundary": "负责多路文档列表的合并去重排序、答案合并与分支汇合；"
                    "不负责重排（f03）、不负责路由（G 组）。",
        "refs": [
            {"path": "haystack/components/joiners/document_joiner.py",
             "symbols": ["DocumentJoiner", "JoinMode", "_finite_score"]},
            {"path": "haystack/components/joiners/branch.py", "symbols": ["BranchJoiner"]},
            {"path": "haystack/components/joiners/answer_joiner.py", "symbols": ["AnswerJoiner"]},
        ],
    },

    # ============ G-路由 ============
    {
        "feature_group": "G-路由",
        "feature": "conditional-router",
        "title": "条件路由",
        "page_file": "g01-conditional-router.md",
        "boundary": "负责按表达式把输入分派到不同输出、以及路由条件求值的异常语义；"
                    "不负责按元数据/文件类型路由（g02）、不负责 LLM 路由（g03）。",
        "refs": [
            {"path": "haystack/components/routers/conditional_router.py",
             "symbols": ["ConditionalRouter", "Route", "RouteConditionException"]},
            {"path": "haystack/components/routers/conditional_router.py",
             "symbols": ["NoRouteSelectedException"]},
        ],
    },
    {
        "feature_group": "G-路由",
        "feature": "metadata-routers",
        "title": "元数据与文件类型路由",
        "page_file": "g02-metadata-routers.md",
        "boundary": "负责按元数据键值、文件类型、文档长度、文档类型分派；"
                    "不负责条件路由（g01）、不负责 LLM 路由（g03）。",
        "refs": [
            {"path": "haystack/components/routers/metadata_router.py", "symbols": ["MetadataRouter"]},
            {"path": "haystack/components/routers/file_type_router.py", "symbols": ["FileTypeRouter"]},
            {"path": "haystack/components/routers/document_length_router.py",
             "symbols": ["DocumentLengthRouter"]},
            {"path": "haystack/components/routers/document_type_router.py",
             "symbols": ["DocumentTypeRouter"]},
        ],
    },
    {
        "feature_group": "G-路由",
        "feature": "llm-routers",
        "title": "LLM 决策路由",
        "page_file": "g03-llm-routers.md",
        "boundary": "负责用语言模型在多个下游之间做选择（消息路由、OpenAI 决策式文本/图像路由）；"
                    "不负责条件路由（g01）、不负责元数据路由（g02）。",
        "refs": [
            {"path": "haystack/components/routers/llm_messages_router.py",
             "symbols": ["LLMMessagesRouter"]},
            {"path": "haystack/components/routers/openai_decisions_text_router.py",
             "symbols": ["OpenAIDecisionsTextRouter"]},
            {"path": "haystack/components/routers/openai_decisions_image_router.py",
             "symbols": ["OpenAIDecisionsImageRouter"]},
        ],
    },

    # ============ H-Agent 与工具 ============
    {
        "feature_group": "H-Agent 与工具",
        "feature": "agent",
        "title": "智能体组件与状态",
        "page_file": "h01-agent.md",
        "boundary": "负责 Agent 组件的状态结构、退出条件与工具调用循环；"
                    "不负责工具调用的输出处理细节（h02）、不负责工具定义（h03）。",
        "refs": [
            {"path": "haystack/components/agents/agent.py", "symbols": ["Agent"]},
            {"path": "haystack/components/agents/agent.py",
             "symbols": ["_get_model_exit_reason", "_pending_tool_call_messages_from_state",
                         "_consume_continue_run"]},
        ],
    },
    {
        "feature_group": "H-Agent 与工具",
        "feature": "tool-calling",
        "title": "工具调用执行",
        "page_file": "h02-tool-calling.md",
        "boundary": "负责把模型的工具调用请求真正执行、把结果转回消息与流式块、以及跨调用合并状态；"
                    "不负责 Agent 主循环（h01）、不负责工具定义与序列化（h03）。",
        "refs": [
            {"path": "haystack/components/agents/tool_calling.py",
             "symbols": ["ToolNotFoundException", "_validate_and_prepare_tools",
                         "_merge_tool_outputs_into_state"]},
            {"path": "haystack/components/agents/tool_calling.py",
             "symbols": ["_process_tool_output", "_build_tool_result_message",
                         "_create_tool_result_streaming_chunk"]},
        ],
    },
    {
        "feature_group": "H-Agent 与工具",
        "feature": "tools",
        "title": "工具定义与工具集",
        "page_file": "h03-tools.md",
        "boundary": "负责工具的定义、从函数创建、状态序列化与工具集管理；"
                    "不负责调用执行（h02）、不负责 Agent 循环（h01）。",
        "refs": [
            {"path": "haystack/tools/tool.py", "symbols": ["Tool"]},
            {"path": "haystack/tools/from_function.py", "symbols": ["create_tool_from_function"]},
            {"path": "haystack/tools/toolset.py", "symbols": ["Toolset"]},
        ],
    },

    # ============ I-评测与可观测 ============
    {
        "feature_group": "I-评测与可观测",
        "feature": "llm-evaluators",
        "title": "LLM 评测器",
        "page_file": "i01-llm-evaluators.md",
        "boundary": "负责用 LLM 打分的评测器（自定义提示、上下文相关度、忠实度）；"
                    "不负责检索指标（i02）、不负责钩子（i03）。",
        "refs": [
            {"path": "haystack/components/evaluators/llm_evaluator.py",
             "symbols": ["LLMEvaluator"]},
            {"path": "haystack/components/evaluators/context_relevance.py",
             "symbols": ["ContextRelevanceEvaluator"]},
            {"path": "haystack/components/evaluators/faithfulness.py",
             "symbols": ["FaithfulnessEvaluator"]},
        ],
    },
    {
        "feature_group": "I-评测与可观测",
        "feature": "retrieval-metrics",
        "title": "检索指标",
        "page_file": "i02-retrieval-metrics.md",
        "boundary": "负责不依赖 LLM 的检索质量指标（召回、MRR、MAP、精确匹配）；"
                    "不负责 LLM 评测（i01）、不负责评测结果聚合（i05）。",
        "refs": [
            {"path": "haystack/components/evaluators/document_recall.py",
             "symbols": ["DocumentRecallEvaluator", "RecallMode"]},
            {"path": "haystack/components/evaluators/document_mrr.py",
             "symbols": ["DocumentMRREvaluator"]},
            {"path": "haystack/components/evaluators/document_map.py",
             "symbols": ["DocumentMAPEvaluator"]},
            {"path": "haystack/components/evaluators/answer_exact_match.py",
             "symbols": ["AnswerExactMatchEvaluator"]},
        ],
    },
    {
        "feature_group": "I-评测与可观测",
        "feature": "hooks",
        "title": "钩子",
        "page_file": "i03-hooks.md",
        "boundary": "负责钩子的协议、从函数创建、执行时机与生命周期（预热/关闭）；"
                    "不负责追踪（i04）、不负责评测（i01/i02）。",
        "refs": [
            {"path": "haystack/hooks/protocol.py", "symbols": ["Hook"]},
            {"path": "haystack/hooks/from_function.py", "symbols": ["hook", "FunctionHook"]},
            {"path": "haystack/hooks/invocation.py", "symbols": ["_run_hooks", "_create_hook_span"]},
            {"path": "haystack/hooks/utils.py", "symbols": ["warm_up_hooks", "close_hooks"]},
        ],
    },
    {
        "feature_group": "I-评测与可观测",
        "feature": "tracing-telemetry",
        "title": "追踪与遥测",
        "page_file": "i04-tracing-telemetry.md",
        "boundary": "负责追踪器的抽象与启停、默认空实现与日志实现、以及匿名遥测的内容与环境采集；"
                    "不负责钩子（i03）、不负责 token 计数（i05）。",
        "refs": [
            {"path": "haystack/tracing/tracer.py",
             "symbols": ["Tracer", "Span", "enable_tracing", "disable_tracing"]},
            {"path": "haystack/tracing/logging_tracer.py",
             "symbols": ["LoggingTracer", "LoggingSpan"]},
            {"path": "haystack/telemetry/_telemetry.py",
             "symbols": ["Telemetry", "pipeline_running"]},
        ],
    },
    {
        "feature_group": "I-评测与可观测",
        "feature": "token-counters",
        "title": "token 计数与评测结果聚合",
        "page_file": "i05-token-counters.md",
        "boundary": "负责 token 计数的几种实现与消息渲染、以及批量评测结果的聚合形态；"
                    "不负责追踪（i04）、不负责评测器本身（i01/i02）。",
        "refs": [
            {"path": "haystack/token_counters/tiktoken_counter.py", "symbols": ["TiktokenCounter"]},
            {"path": "haystack/token_counters/approximate_counter.py",
             "symbols": ["ApproximateTokenCounter"]},
            {"path": "haystack/token_counters/utils.py",
             "symbols": ["_rendered_conversation", "_rendered_tools"]},
            {"path": "haystack/evaluation/eval_run_result.py",
             "symbols": ["EvaluationRunResult"]},
        ],
    },
]
