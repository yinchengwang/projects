# -*- coding: utf-8 -*-
"""langchain 机制清单（阶段 2C 第一个项目：RAG / 索引编排框架）。

盘点范围：本仓是 monorepo，`libs/` 下有多个独立分发包。机制集中在两处：

- `libs/core/langchain_core/` —— 表达式语言（Runnable）、消息与内容块、工具契约、
  回调与追踪、文档索引（去重与清理）、向量存储抽象、提示模板、输出解析、以及
  出站请求的安全策略。
- `libs/text-splitters/langchain_text_splitters/` —— 分块算法（字符/递归/标记语言/
  HTML/JSON）。RAG 的 ingestion 侧机制主要在这里。

`libs/langchain`、`libs/langchain_v1`、`libs/partners`（各家模型/向量库的适配）属于
「把 core 接到具体后端」的适配层，机制重复度高，不建页；`libs/standard-tests` 是测试
约定，也不建页。

盘点方法：`dump_python_defs.py` 建 AST 定义索引，再按职责归并成机制簇。篇数是归并
结果，不是按文件数折算（core 下 188 个非测试 py 文件里大量是废弃标记与工具函数）。

源码锚点：`langchain@30c4e38d71c2fdf6af474e0bb21832491aaa983e`。
"""

GROUP_ORDER = [
    "A-表达式语言",
    "B-消息与内容",
    "C-提示与解析",
    "D-工具与函数调用",
    "E-回调与追踪",
    "F-文档分块",
    "G-索引与向量存储",
    "H-模型层",
    "I-序列化与安全",
]

MECHANISM_SPECS = [
    # ============ A-表达式语言 ============
    {
        "feature_group": "A-表达式语言",
        "feature": "runnable-core",
        "title": "Runnable 契约与序列组合",
        "page_file": "a01-runnable-core.md",
        "boundary": "负责 Runnable 的基础契约（invoke/batch/stream 与同步异步双份）与序列组合语义；"
                    "不负责并行（a02）、不负责配置传播（a05）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/base.py",
             "symbols": ["Runnable", "RunnableSerializable", "RunnableSequence"]},
            {"path": "libs/core/langchain_core/runnables/base.py",
             "symbols": ["RunnableLambda", "RunnableGenerator", "_ChunkAccumulator"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "parallel-and-each",
        "title": "并行组合与逐项映射",
        "page_file": "a02-parallel-and-each.md",
        "boundary": "负责并行分支的合流语义与逐项映射（each）的执行模型；"
                    "不负责基础契约（a01）、不负责分支与路由（a03）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/base.py",
             "symbols": ["RunnableParallel", "RunnableEach", "RunnableEachBase"]},
            {"path": "libs/core/langchain_core/runnables/base.py",
             "symbols": ["RunnableBinding", "RunnableBindingBase"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "branch-and-router",
        "title": "分支、路由与回退",
        "page_file": "a03-branch-and-router.md",
        "boundary": "负责条件分支、键路由与回退链的选择语义；"
                    "不负责并行（a02）、不负责重试（a04）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/branch.py", "symbols": ["RunnableBranch"]},
            {"path": "libs/core/langchain_core/runnables/router.py",
             "symbols": ["RouterRunnable", "RouterInput"]},
            {"path": "libs/core/langchain_core/runnables/fallbacks.py",
             "symbols": ["RunnableWithFallbacks"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "retry",
        "title": "重试与退避",
        "page_file": "a04-retry.md",
        "boundary": "负责表达式级别的重试包装与退避参数；"
                    "不负责回退链（a03）、不负责模型层重试（h01）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/retry.py",
             "symbols": ["RunnableRetry", "ExponentialJitterParams"]},
            {"path": "libs/core/langchain_core/runnables/base.py", "symbols": ["RunnableRetry"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "config-and-context",
        "title": "运行配置与上下文传播",
        "page_file": "a05-config-and-context.md",
        "boundary": "负责 RunnableConfig 的字段与在嵌套调用中的传播、以及可配置字段的替换；"
                    "不负责回调（E 组）、不负责分支（a03）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/config.py",
             "symbols": ["RunnableConfig", "ContextThreadPoolExecutor"]},
            {"path": "libs/core/langchain_core/runnables/configurable.py",
             "symbols": ["DynamicRunnable", "RunnableConfigurableFields",
                         "RunnableConfigurableAlternatives"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "passthrough-and-assign",
        "title": "透传、赋值与字段选取",
        "page_file": "a06-passthrough-and-assign.md",
        "boundary": "负责输入透传、在链中途给字典写入字段、以及从字典里挑字段；"
                    "不负责序列组合（a01）、不负责配置（a05）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/passthrough.py",
             "symbols": ["RunnablePassthrough", "RunnableAssign", "RunnablePick"]},
            {"path": "libs/core/langchain_core/runnables/base.py", "symbols": ["_RunnableCallableSync"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "message-history",
        "title": "会话历史绑定",
        "page_file": "a07-message-history.md",
        "boundary": "负责把会话历史读写挂到表达式链上、以及历史键的路由；"
                    "不负责消息结构（b01）、不负责内存存储（g07）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/history.py",
             "symbols": ["RunnableWithMessageHistory"]},
            {"path": "libs/core/langchain_core/messages/base.py",
             "symbols": ["BaseMessage", "BaseMessageChunk"]},
        ],
    },
    {
        "feature_group": "A-表达式语言",
        "feature": "runnable-graph",
        "title": "组合图与可视化",
        "page_file": "a08-runnable-graph.md",
        "boundary": "负责把表达式组合结构导出成图（节点、边、分支）并渲染；"
                    "不负责执行（a01）、不负责追踪（e03）。",
        "refs": [
            {"path": "libs/core/langchain_core/runnables/graph.py",
             "symbols": ["Graph", "Node", "Edge", "Branch"]},
            {"path": "libs/core/langchain_core/runnables/graph_mermaid.py",
             "symbols": ["MermaidDrawMethod"]},
            {"path": "libs/core/langchain_core/runnables/graph_ascii.py", "symbols": ["Graph"]},
        ],
    },

    # ============ B-消息与内容 ============
    {
        "feature_group": "B-消息与内容",
        "feature": "message-model",
        "title": "消息模型与角色",
        "page_file": "b01-message-model.md",
        "boundary": "负责消息基类、角色划分与流式增量消息；"
                    "不负责内容块（b02）、不负责用量元数据（b03）。",
        "refs": [
            {"path": "libs/core/langchain_core/messages/base.py",
             "symbols": ["BaseMessage", "BaseMessageChunk", "TextAccessor"]},
            {"path": "libs/core/langchain_core/messages/ai.py",
             "symbols": ["AIMessage", "AIMessageChunk"]},
            {"path": "libs/core/langchain_core/messages/human.py", "symbols": ["HumanMessage"]},
        ],
    },
    {
        "feature_group": "B-消息与内容",
        "feature": "content-blocks",
        "title": "内容块与工具调用块",
        "page_file": "b02-content-blocks.md",
        "boundary": "负责多模态内容块的类型体系与工具调用/结果块的结构；"
                    "不负责消息基类（b01）、不负责解析（c 组）。",
        "refs": [
            {"path": "libs/core/langchain_core/messages/content.py",
             "symbols": ["TextContentBlock", "ToolCall", "ToolCallChunk"]},
            {"path": "libs/core/langchain_core/messages/content.py",
             "symbols": ["InvalidToolCall", "ReasoningContentBlock", "ServerToolCall"]},
        ],
    },
    {
        "feature_group": "B-消息与内容",
        "feature": "usage-metadata",
        "title": "用量元数据",
        "page_file": "b03-usage-metadata.md",
        "boundary": "负责 token 用量的结构（含缓存与推理拆分）与合并语义；"
                    "不负责消息基类（b01）、不负责回调用量（e02）。",
        "refs": [
            {"path": "libs/core/langchain_core/messages/ai.py",
             "symbols": ["UsageMetadata", "InputTokenDetails", "OutputTokenDetails"]},
            {"path": "libs/core/langchain_core/callbacks/usage.py", "symbols": ["UsageMetadataCallbackHandler"]},
        ],
    },

    # ============ C-提示与解析 ============
    {
        "feature_group": "C-提示与解析",
        "feature": "prompt-template",
        "title": "提示模板与消息占位",
        "page_file": "c01-prompt-template.md",
        "boundary": "负责提示模板的变量填充、消息占位与少样本拼接；"
                    "不负责输出解析（c02）、不负责示例选择（c03）。",
        "refs": [
            {"path": "libs/core/langchain_core/prompts/chat.py",
             "symbols": ["ChatPromptTemplate", "MessagesPlaceholder",
                         "BaseStringMessagePromptTemplate"]},
            {"path": "libs/core/langchain_core/prompts/few_shot.py",
             "symbols": ["FewShotPromptTemplate", "FewShotChatMessagePromptTemplate"]},
        ],
    },
    {
        "feature_group": "C-提示与解析",
        "feature": "output-parsing",
        "title": "输出解析",
        "page_file": "c02-output-parsing.md",
        "boundary": "负责把模型原始输出解析成结构化结果，含流式解析与部分解析；"
                    "不负责提示（c01）、不负责函数调用（d01）。",
        "refs": [
            {"path": "libs/core/langchain_core/output_parsers/base.py",
             "symbols": ["BaseOutputParser", "BaseLLMOutputParser", "BaseGenerationOutputParser"]},
            {"path": "libs/core/langchain_core/output_parsers/pydantic.py", "symbols": ["PydanticOutputParser"]},
            {"path": "libs/core/langchain_core/output_parsers/json.py", "symbols": ["JsonOutputParser"]},
        ],
    },
    {
        "feature_group": "C-提示与解析",
        "feature": "example-selector",
        "title": "示例选择",
        "page_file": "c03-example-selector.md",
        "boundary": "负责少样本示例的挑选策略（长度、语义相似、最大边际相关）；"
                    "不负责模板（c01）、不负责检索（g 组）。",
        "refs": [
            {"path": "libs/core/langchain_core/example_selectors/length_based.py",
             "symbols": ["LengthBasedExampleSelector"]},
            {"path": "libs/core/langchain_core/example_selectors/semantic_similarity.py",
             "symbols": ["SemanticSimilarityExampleSelector", "MaxMarginalRelevanceExampleSelector"]},
        ],
    },

    # ============ D-工具与函数调用 ============
    {
        "feature_group": "D-工具与函数调用",
        "feature": "tool-contract",
        "title": "工具契约与参数注入",
        "page_file": "d01-tool-contract.md",
        "boundary": "负责工具的抽象契约、参数 schema 推导与注入参数标记；"
                    "不负责转换（d02）、不负责结构化工具（d03）。",
        "refs": [
            {"path": "libs/core/langchain_core/tools/base.py",
             "symbols": ["BaseTool", "BaseToolkit", "ToolException"]},
            {"path": "libs/core/langchain_core/tools/base.py",
             "symbols": ["InjectedToolArg", "InjectedToolCallId", "SchemaAnnotationError"]},
        ],
    },
    {
        "feature_group": "D-工具与函数调用",
        "feature": "tool-conversion",
        "title": "函数转工具与 schema 生成",
        "page_file": "d02-tool-conversion.md",
        "boundary": "负责把普通函数/字典转换成工具定义并生成 provider 可消费的 schema；"
                    "不负责契约（d01）、不负责结构化工具（d03）。",
        "refs": [
            {"path": "libs/core/langchain_core/tools/convert.py", "symbols": ["tool", "convert_runnable_to_tool"]},
            {"path": "libs/core/langchain_core/utils/function_calling.py",
             "symbols": ["FunctionDescription", "ToolDescription"]},
        ],
    },
    {
        "feature_group": "D-工具与函数调用",
        "feature": "structured-tool",
        "title": "结构化工具与结果",
        "page_file": "d03-structured-tool.md",
        "boundary": "负责以结构化模型为参数/结果形态的工具，含结果信封与错误归一；"
                    "不负责契约（d01）、不负责检索器工具（d04）。",
        "refs": [
            {"path": "libs/core/langchain_core/tools/structured.py",
             "symbols": ["StructuredTool", "_filter_schema_args"]},
            {"path": "libs/core/langchain_core/tools/simple.py", "symbols": ["Tool"]},
        ],
    },
    {
        "feature_group": "D-工具与函数调用",
        "feature": "retriever-tool",
        "title": "检索器与渲染",
        "page_file": "d04-retriever-tool.md",
        "boundary": "负责把检索器连到表达式链，以及工具描述/参数的渲染；"
                    "不负责工具契约（d01）、不负责文档索引（g01）。",
        "refs": [
            {"path": "libs/core/langchain_core/retrievers.py",
             "symbols": ["BaseRetriever", "RetrieverInput"]},
            {"path": "libs/core/langchain_core/tools/render.py", "symbols": ["render_text_description"]},
        ],
    },

    # ============ E-回调与追踪 ============
    {
        "feature_group": "E-回调与追踪",
        "feature": "callback-manager",
        "title": "回调管理器与运行管理器",
        "page_file": "e01-callback-manager.md",
        "boundary": "负责回调的注册、按事件分发与各阶段运行管理器；"
                    "不负责追踪后端（e03）、不负责用量统计（e02）。",
        "refs": [
            {"path": "libs/core/langchain_core/callbacks/manager.py",
             "symbols": ["CallbackManager", "BaseRunManager", "RunManager",
                         "AsyncCallbackManager"]},
            {"path": "libs/core/langchain_core/callbacks/manager.py",
             "symbols": ["CallbackManagerForLLMRun", "CallbackManagerForToolRun",
                         "ParentRunManager"]},
        ],
    },
    {
        "feature_group": "E-回调与追踪",
        "feature": "callback-handlers",
        "title": "内置处理器与流式输出",
        "page_file": "e02-callback-handlers.md",
        "boundary": "负责内置的若干回调处理器（标准输出、流式输出、文件、用量）；"
                    "不负责管理器（e01）、不负责追踪（e03）。",
        "refs": [
            {"path": "libs/core/langchain_core/callbacks/streaming_stdout.py",
             "symbols": ["StreamingStdOutCallbackHandler"]},
            {"path": "libs/core/langchain_core/callbacks/usage.py",
             "symbols": ["UsageMetadataCallbackHandler"]},
            {"path": "libs/core/langchain_core/callbacks/file.py",
             "symbols": ["FileCallbackHandler"]},
        ],
    },
    {
        "feature_group": "E-回调与追踪",
        "feature": "tracers",
        "title": "追踪器与事件流",
        "page_file": "e03-tracers.md",
        "boundary": "负责追踪器基类、事件流格式与上下文传递；"
                    "不负责回调管理器（e01）、不负责序列化（i01）。",
        "refs": [
            {"path": "libs/core/langchain_core/tracers/base.py",
             "symbols": ["BaseTracer", "AsyncBaseTracer"]},
            {"path": "libs/core/langchain_core/tracers/event_stream.py",
             "symbols": ["RunInfo", "_AstreamEventsCallbackHandler"]},
            {"path": "libs/core/langchain_core/tracers/context.py", "symbols": ["tracing_v2_enabled"]},
        ],
    },

    # ============ F-文档分块 ============
    {
        "feature_group": "F-文档分块",
        "feature": "splitter-base",
        "title": "分块器基类与长度度量",
        "page_file": "f01-splitter-base.md",
        "boundary": "负责分块器的抽象契约、长度度量方式与元数据搬运；"
                    "不负责递归分块（f02）、不负责标记语言分块（f03）。",
        "refs": [
            {"path": "libs/text-splitters/langchain_text_splitters/base.py",
             "symbols": ["TextSplitter", "TokenTextSplitter", "Tokenizer", "Language"]},
            {"path": "libs/core/langchain_core/documents/base.py",
             "symbols": ["Document", "Blob", "BaseMedia"]},
        ],
    },
    {
        "feature_group": "F-文档分块",
        "feature": "character-splitter",
        "title": "字符与递归字符分块",
        "page_file": "f02-character-splitter.md",
        "boundary": "负责按分隔符切分与递归切分的算法细节（重叠、合并、长度约束）；"
                    "不负责基类（f01）、不负责标记语言（f03）。",
        "refs": [
            {"path": "libs/text-splitters/langchain_text_splitters/character.py",
             "symbols": ["CharacterTextSplitter", "RecursiveCharacterTextSplitter"]},
            {"path": "libs/text-splitters/langchain_text_splitters/base.py", "symbols": ["TextSplitter"]},
        ],
    },
    {
        "feature_group": "F-文档分块",
        "feature": "markup-splitter",
        "title": "标记语言与结构化文档分块",
        "page_file": "f03-markup-splitter.md",
        "boundary": "负责按 Markdown 标题、HTML 结构、JSON 层次切分并保留结构元数据；"
                    "不负责字符分块（f02）、不负责语义分块（f04）。",
        "refs": [
            {"path": "libs/text-splitters/langchain_text_splitters/markdown.py",
             "symbols": ["MarkdownHeaderTextSplitter", "MarkdownTextSplitter"]},
            {"path": "libs/text-splitters/langchain_text_splitters/html.py",
             "symbols": ["HTMLHeaderTextSplitter", "HTMLSectionSplitter"]},
            {"path": "libs/text-splitters/langchain_text_splitters/json.py",
             "symbols": ["RecursiveJsonSplitter"]},
        ],
    },
    {
        "feature_group": "F-文档分块",
        "feature": "semantic-splitter",
        "title": "语义与句法分块",
        "page_file": "f04-semantic-splitter.md",
        "boundary": "负责基于语义/句法边界的分块与相关切分器；"
                    "不负责字符递归（f02）、不负责标记结构（f03）。",
        "refs": [
            {"path": "libs/text-splitters/langchain_text_splitters/sentence_transformers.py",
             "symbols": ["SentenceTransformersTokenTextSplitter"]},
            {"path": "libs/text-splitters/langchain_text_splitters/nltk.py",
             "symbols": ["NLTKTextSplitter"]},
            {"path": "libs/text-splitters/langchain_text_splitters/spacy.py",
             "symbols": ["SpacyTextSplitter"]},
        ],
    },

    # ============ G-索引与向量存储 ============
    {
        "feature_group": "G-索引与向量存储",
        "feature": "indexing-api",
        "title": "文档索引与去重清理",
        "page_file": "g01-indexing-api.md",
        "boundary": "负责索引流程如何判定新增/更新/删除，以及清理模式的语义；"
                    "不负责记录管理器（g02）、不负责向量存储（g03）。",
        "refs": [
            {"path": "libs/core/langchain_core/indexing/api.py",
             "symbols": ["index", "IndexingResult", "IndexingException"]},
            {"path": "libs/core/langchain_core/indexing/api.py", "symbols": ["_HashedDocument"]},
        ],
    },
    {
        "feature_group": "G-索引与向量存储",
        "feature": "record-manager",
        "title": "记录管理器",
        "page_file": "g02-record-manager.md",
        "boundary": "负责记录管理器的契约与内存实现，以及写入响应/删除响应的形状；"
                    "不负责索引流程（g01）、不负责向量存储（g03）。",
        "refs": [
            {"path": "libs/core/langchain_core/indexing/base.py",
             "symbols": ["RecordManager", "InMemoryRecordManager", "_Record"]},
            {"path": "libs/core/langchain_core/indexing/base.py",
             "symbols": ["UpsertResponse", "DeleteResponse", "DocumentIndex"]},
        ],
    },
    {
        "feature_group": "G-索引与向量存储",
        "feature": "vectorstore-contract",
        "title": "向量存储契约与检索器",
        "page_file": "g03-vectorstore-contract.md",
        "boundary": "负责向量存储的抽象契约、相似度检索接口与检索器包装；"
                    "不负责内存实现（g04）、不负责索引流程（g01）。",
        "refs": [
            {"path": "libs/core/langchain_core/vectorstores/base.py",
             "symbols": ["VectorStore", "VectorStoreRetriever"]},
            {"path": "libs/core/langchain_core/vectorstores/utils.py",
             "symbols": ["maximal_marginal_relevance"]},
        ],
    },
    {
        "feature_group": "G-索引与向量存储",
        "feature": "in-memory-stores",
        "title": "内存向量存储与检索实现",
        "page_file": "g04-in-memory-stores.md",
        "boundary": "负责内存版向量存储与内存记录管理器的实现细节；"
                    "不负责契约（g03）、不负责索引流程（g01）。",
        "refs": [
            {"path": "libs/core/langchain_core/vectorstores/in_memory.py",
             "symbols": ["InMemoryVectorStore"]},
            {"path": "libs/core/langchain_core/indexing/in_memory.py",
             "symbols": ["InMemoryDocumentIndex"]},
        ],
    },
    {
        "feature_group": "G-索引与向量存储",
        "feature": "document-transformers",
        "title": "文档压缩与嵌入",
        "page_file": "g05-document-transformers.md",
        "boundary": "负责文档压缩器的契约与嵌入模型契约（含假实现）；"
                    "不负责检索器（d04）、不负责分块（f 组）。",
        "refs": [
            {"path": "libs/core/langchain_core/documents/compressor.py",
             "symbols": ["BaseDocumentCompressor"]},
            {"path": "libs/core/langchain_core/embeddings/embeddings.py", "symbols": ["Embeddings"]},
            {"path": "libs/core/langchain_core/embeddings/fake.py",
             "symbols": ["DeterministicFakeEmbedding"]},
        ],
    },
    {
        "feature_group": "G-索引与向量存储",
        "feature": "memory-and-loaders",
        "title": "会话内存与文档加载",
        "page_file": "g06-memory-and-loaders.md",
        "boundary": "负责会话内存的存储契约与文档加载器/Blob 加载器；"
                    "不负责历史绑定（a07）、不负责分块（f 组）。",
        "refs": [
            {"path": "libs/core/langchain_core/document_loaders/base.py",
             "symbols": ["BaseLoader", "BaseBlobParser"]},
            {"path": "libs/core/langchain_core/document_loaders/blob_loaders.py",
             "symbols": ["BlobLoader", "yield_blobs"]},
            {"path": "libs/core/langchain_core/runnables/history.py",
             "symbols": ["BaseChatMessageHistory"]},
        ],
    },

    # ============ H-模型层 ============
    {
        "feature_group": "H-模型层",
        "feature": "chat-model-contract",
        "title": "聊天模型契约",
        "page_file": "h01-chat-model-contract.md",
        "boundary": "负责聊天模型的抽象契约、工具绑定与批处理；"
                    "不负责流式投影（h02）、不负责模型档案（h04）。",
        "refs": [
            {"path": "libs/core/langchain_core/language_models/chat_models.py",
             "symbols": ["BaseChatModel", "SimpleChatModel", "_ChatModelBinding"]},
            {"path": "libs/core/langchain_core/language_models/llms.py",
             "symbols": ["BaseLLM", "LLM"]},
        ],
    },
    {
        "feature_group": "H-模型层",
        "feature": "stream-projection",
        "title": "流式投影与增量合成",
        "page_file": "h02-stream-projection.md",
        "boundary": "负责把流式事件投影成文本/结构化片段的机制与增量合并；"
                    "不负责模型契约（h01）、不负责消息增量（b01）。",
        "refs": [
            {"path": "libs/core/langchain_core/language_models/chat_model_stream.py",
             "symbols": ["ChatModelStream", "AsyncChatModelStream", "SyncTextProjection"]},
            {"path": "libs/core/langchain_core/messages/base.py", "symbols": ["BaseMessageChunk"]},
        ],
    },
    {
        "feature_group": "H-模型层",
        "feature": "functions-and-tools-binding",
        "title": "函数调用格式与工具绑定",
        "page_file": "h03-functions-and-tools-binding.md",
        "boundary": "负责把工具绑定到模型时的格式协商与工具调用结果的归一；"
                    "不负责模型契约（h01）、不负责工具契约（d01）。",
        "refs": [
            {"path": "libs/core/langchain_core/utils/function_calling.py",
             "symbols": ["convert_to_openai_tool", "convert_to_openai_function",
                         "tool_example_to_messages"]},
            {"path": "libs/core/langchain_core/output_parsers/openai_tools.py",
             "symbols": ["JsonOutputToolsParser", "PydanticToolsParser"]},
        ],
    },
    {
        "feature_group": "H-模型层",
        "feature": "model-profile",
        "title": "模型档案与能力声明",
        "page_file": "h04-model-profile.md",
        "boundary": "负责模型档案（能力与限制）的数据结构与兼容桥接；"
                    "不负责模型契约（h01）、不负责流式（h02）。",
        "refs": [
            {"path": "libs/core/langchain_core/language_models/model_profile.py",
             "symbols": ["ModelProfile", "ModelProfileRegistry"]},
            {"path": "libs/core/langchain_core/language_models/_compat_bridge.py",
             "symbols": ["finalize_tool_call_chunk", "_accumulate_usage"]},
        ],
    },

    # ============ I-序列化与安全 ============
    {
        "feature_group": "I-序列化与安全",
        "feature": "serialization",
        "title": "可序列化基类与反序列化",
        "page_file": "i01-serialization.md",
        "boundary": "负责表达式对象的序列化表示与从字典重建（含密钥字段的处理）；"
                    "不负责安全策略（i02）、不负责追踪（e03）。",
        "refs": [
            {"path": "libs/core/langchain_core/load/serializable.py",
             "symbols": ["Serializable", "BaseSerialized", "SerializedConstructor",
                         "SerializedSecret"]},
            {"path": "libs/core/langchain_core/load/load.py", "symbols": ["Reviver"]},
            {"path": "libs/core/langchain_core/load/mapping.py", "symbols": ["SERIALIZABLE_MAPPING"]},
        ],
    },
    {
        "feature_group": "I-序列化与安全",
        "feature": "ssrf-policy",
        "title": "出站请求的 SSRF 防护",
        "page_file": "i02-ssrf-policy.md",
        "boundary": "负责框架内出站 HTTP 请求的策略与传输层防护；"
                    "不负责序列化（i01）、不负责工具契约（d01）。",
        "refs": [
            {"path": "libs/core/langchain_core/_security/_policy.py", "symbols": ["SSRFPolicy"]},
            {"path": "libs/core/langchain_core/_security/_ssrf_protection.py",
             "symbols": ["validate_safe_url", "is_safe_url"]},
            {"path": "libs/core/langchain_core/_security/_transport.py",
             "symbols": ["SSRFSafeTransport", "SSRFSafeSyncTransport"]},
        ],
    },
]
