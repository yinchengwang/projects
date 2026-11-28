# -*- coding: utf-8 -*-
"""dify 机制清单（阶段 2C 第八个项目）。

盘点范围：`api/` 下 4969 个 py 文件、135 万行（其中 `tests/` 78 万行不建页），
加上 `dify-agent/` 这个独立的 Agent 运行时包。`web/`（Next.js 前端）、`sdks/`、
`e2e/`、`docker/`、`docs/` 不建页。

dify 与 2C 前七个项目的差别是**它同时是框架和产品，而且产品的形状决定了框架的形状**：

- langchain / llama-index / haystack 是「给别人搭 RAG 的库」；
- lightrag / graphrag / ragas 是「解决单一问题的工具」；
- ragflow 是「Go 写的完整服务端」，规模大一档但形态是传统的分层服务；
- **dify 是「LLM 应用平台」**：它把工作流编排、Agent、RAG 摄取、插件、模型接入、
  MCP、人工介入、评测全部做进一个产品，而这些能力之间**通过「应用」这个概念耦合**。

最值得单独读的一条弧线是：**dify 的图执行内核不在这个仓里**。
`pyproject.toml` 依赖 `graphon`（`langgenius/graphon`，pin 到具体 commit），
所以 `core/workflow/` 只负责「node factory + runtime 适配 + 入口」，
真正的超步执行、通道、检查点都在外部包。这与 ragflow 把 langgraph 移植进
`internal/harness/` 是**相反的架构决策**：一个内联，一个外置。

组划分按「用户能看到什么 → 它底下靠什么」来切：
A 应用形态 → B 工作流图与节点 → C 工作流生成器 → D RAG 摄取与索引 →
E RAG 检索与后处理 → F 插件与工具 → G Agent 与 MCP → H 服务与仓储 →
I 运行时支撑与部署。

源码锚点：`dify@ad860bffc118393003128d8ed60a0a4e208589cf`。
"""

GROUP_ORDER = [
    "A-应用形态与编排",
    "B-工作流图与节点",
    "C-工作流生成器",
    "D-RAG 摄取与索引",
    "E-RAG 检索与后处理",
    "F-插件与工具",
    "G-Agent 与 MCP",
    "H-服务与仓储",
    "I-运行时支撑与部署",
]

MECHANISM_SPECS = [
    # ============ A-应用形态与编排 ============
    {
        "feature_group": "A-应用形态与编排",
        "feature": "app-runner",
        "title": "应用执行器骨架",
        "page_file": "a01-app-runner.md",
        "boundary": "负责所有应用形态共用的执行骨架（任务启停、队列、取消、审计）；"
                    "不负责各形态的具体流程（a03-a06）、不负责响应转换（a02）。",
        "refs": [
            {"path": "api/core/app/apps/base_app_runner.py", "symbols": ["AppRunner"]},
            {"path": "api/core/app/apps/execution_coordinator.py",
             "symbols": ["AppExecutionCoordinator", "AppExecutionState"]},
            {"path": "api/core/app/apps/base_app_queue_manager.py",
             "symbols": ["AppQueueManager"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "app-generator",
        "title": "应用响应生成与流式转换",
        "page_file": "a02-app-generator.md",
        "boundary": "负责把执行产物转成客户端可消费的 SSE 流（含审核拦截与错误映射）；"
                    "不负责执行骨架（a01）、不负责工作流响应（a07）。",
        "refs": [
            {"path": "api/core/app/apps/base_app_generator.py",
             "symbols": ["BaseAppGenerator"]},
            {"path": "api/core/app/apps/base_app_generate_response_converter.py",
             "symbols": ["AppGenerateResponseConverter"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "workflow-app",
        "title": "工作流应用形态",
        "page_file": "a03-workflow-app.md",
        "boundary": "负责「工作流」这一应用形态的配置管理与单次执行；"
                    "不负责图执行（B 组）、不负责对话形态（a04）。",
        "refs": [
            {"path": "api/core/app/apps/workflow/app_runner.py",
             "symbols": ["WorkflowAppRunner"]},
            {"path": "api/core/app/apps/workflow/app_generator.py",
             "symbols": ["WorkflowAppGenerator"]},
            {"path": "api/core/app/apps/workflow/active_workflow_tasks.py",
             "symbols": ["active_workflow_task"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "chat-apps",
        "title": "对话与补全形态",
        "page_file": "a04-chat-apps.md",
        "boundary": "负责对话、补全、高级对话三种形态的差异（历史管理、变量、会话状态）；"
                    "不负责工作流形态（a03）、不负责 Agent 形态（a05）。",
        "refs": [
            {"path": "api/core/app/apps/chat/app_runner.py", "symbols": ["ChatAppRunner"]},
            {"path": "api/core/app/apps/completion/app_runner.py",
             "symbols": ["CompletionAppRunner"]},
            {"path": "api/core/app/apps/advanced_chat/generate_task_pipeline.py",
             "symbols": ["AdvancedChatAppGenerateTaskPipeline", "WorkflowSnapshot"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "agent-app",
        "title": "Agent 应用形态",
        "page_file": "a05-agent-app.md",
        "boundary": "负责把外部 Agent 运行时接进应用层（文本增量合并、会话快照、失败映射）；"
                    "不负责 Agent 循环本体（G 组）、不负责工作流形态（a03）。",
        "refs": [
            {"path": "api/core/app/apps/agent_app/app_runner.py",
             "symbols": ["AgentAppRunner", "_TextDeltaDebouncer"]},
            {"path": "api/core/app/apps/agent_app/app_generator.py",
             "symbols": ["AgentAppGenerator"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "app-config",
        "title": "应用配置与模型参数",
        "page_file": "a06-app-config.md",
        "boundary": "负责应用级配置实体（模型参数、提示模板、变量映射）；"
                    "不负责运行时执行（a01）、不负责持久化模型（h05）。",
        "refs": [
            {"path": "api/core/app/app_config/entities.py", "symbols": ["ModelConfigEntity"]},
            {"path": "api/models/model.py", "symbols": ["EnabledConfig", "EmbeddingModelInfo"]},
            {"path": "api/models/model.py", "symbols": ["AnnotationReplyDisabledConfig"]},
        ],
    },
    {
        "feature_group": "A-应用形态与编排",
        "feature": "workflow-response",
        "title": "工作流响应转换",
        "page_file": "a07-workflow-response.md",
        "boundary": "负责把工作流事件流翻译成对外响应（含人工介入暂停的表示）；"
                    "不负责图执行（B 组）、不负责基础流式（a02）。",
        "refs": [
            {"path": "api/core/app/apps/common/workflow_response_converter.py",
             "symbols": ["WorkflowResponseConverter"]},
        ],
    },

    # ============ B-工作流图与节点 ============
    {
        "feature_group": "B-工作流图与节点",
        "feature": "workflow-entry",
        "title": "工作流入口与图引擎边界",
        "page_file": "b01-workflow-entry.md",
        "boundary": "负责把工作流 DSL 交给外部图引擎并适配事件；"
                    "不负责节点构造（b02）、不负责生成器（C 组）。",
        "refs": [
            {"path": "api/core/workflow/workflow_entry.py",
             "symbols": ["WorkflowEntry", "iter_dify_graph_engine_events"]},
            {"path": "api/core/workflow/graph_topology.py", "symbols": ["WorkflowGraphTopology"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "node-factory",
        "title": "节点工厂与类型注册",
        "page_file": "b02-node-factory.md",
        "boundary": "负责节点类型的惰性导入、注册与按类型构造；"
                    "不负责节点实现（b03-b07）、不负责运行时装配（b08）。",
        "refs": [
            {"path": "api/core/workflow/node_factory.py",
             "symbols": ["DifyNodeFactory", "resolve_workflow_node_class", "register_nodes"]},
            {"path": "api/core/workflow/node_factory.py",
             "symbols": ["_LazyNodeTypeClassesMapping", "is_start_node_type"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "llm-node",
        "title": "LLM 节点与模型环境变量",
        "page_file": "b03-llm-node.md",
        "boundary": "负责 LLM 节点的模型选择、环境变量覆盖与提示渲染；"
                    "不负责其他节点（b04）、不负责模型提供方（H 组）。",
        "refs": [
            {"path": "api/core/workflow/llm_node.py", "symbols": ["DifyLLMNode"]},
            {"path": "api/core/workflow/llm_environment_variable.py",
             "symbols": ["LLMEnvironmentVariable", "LLMModelSelection"]},
            {"path": "api/core/workflow/llm_environment_variable.py",
             "symbols": ["resolve_llm_model_config"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "agent-nodes",
        "title": "两代 Agent 节点",
        "page_file": "b04-agent-nodes.md",
        "boundary": "负责工作流内的 Agent 节点两代实现与工具构建；"
                    "不负责 Agent 循环本体（G 组）、不负责 LLM 节点（b03）。",
        "refs": [
            {"path": "api/core/workflow/nodes/agent/agent_node.py", "symbols": ["AgentNode"]},
            {"path": "api/core/workflow/nodes/agent_v2/agent_node.py",
             "symbols": ["DifyAgentNode"]},
            {"path": "api/core/workflow/nodes/agent_v2/dify_tools_builder.py",
             "symbols": ["ProviderToolsLister", "MCPProviderIDResolver"]},
            {"path": "api/core/workflow/nodes/agent_v2/output_adapter.py",
             "symbols": ["WorkflowAgentOutputAdapter", "ToolFileRebacker"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "knowledge-nodes",
        "title": "知识检索与知识索引节点",
        "page_file": "b05-knowledge-nodes.md",
        "boundary": "负责工作流里的检索节点与索引节点（含元数据过滤归一）；"
                    "不负责 RAG 底层（D/E 组）、不负责 Agent 节点（b04）。",
        "refs": [
            {"path": "api/core/workflow/nodes/knowledge_retrieval/knowledge_retrieval_node.py",
             "symbols": ["KnowledgeRetrievalNode"]},
            {"path": "api/core/workflow/nodes/knowledge_retrieval/retrieval.py",
             "symbols": ["KnowledgeRetrievalRequest", "RAGRetrievalProtocol"]},
            {"path": "api/core/workflow/nodes/knowledge_index/knowledge_index_node.py",
             "symbols": ["KnowledgeIndexNode"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "human-input",
        "title": "人工介入：两代实现与表单编译",
        "page_file": "b06-human-input.md",
        "boundary": "负责工作流暂停等人填表的机制（表单定义、默认值、回调、超时）；"
                    "不负责投递渠道（i05）、不负责触发器（b07）。",
        "refs": [
            {"path": "api/core/workflow/nodes/human_input/callback.py",
             "symbols": ["DifyHITLCallback"]},
            {"path": "api/core/workflow/nodes/human_input/entities.py",
             "symbols": ["ParagraphInputConfig", "StringSource"]},
            {"path": "api/core/workflow/nodes/human_input/enums.py",
             "symbols": ["HumanInputFormStatus", "FormInputType"]},
            {"path": "api/core/workflow/nodes/human_input_v2/node.py",
             "symbols": ["HumanInputNode"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "trigger-nodes",
        "title": "触发器节点",
        "page_file": "b07-trigger-nodes.md",
        "boundary": "负责 Webhook / 定时 / 插件事件三类触发器节点；"
                    "不负责人工介入（b06）、不负责数据源节点（b08）。",
        "refs": [
            {"path": "api/core/workflow/nodes/trigger_webhook/node.py",
             "symbols": ["TriggerWebhookNode"]},
            {"path": "api/core/workflow/nodes/trigger_webhook/entities.py",
             "symbols": ["WebhookParameter", "WebhookBodyParameter"]},
            {"path": "api/core/workflow/nodes/trigger_schedule/trigger_schedule_node.py",
             "symbols": ["TriggerScheduleNode"]},
            {"path": "api/core/workflow/nodes/trigger_plugin/trigger_event_node.py",
             "symbols": ["TriggerEventNode"]},
        ],
    },
    {
        "feature_group": "B-工作流图与节点",
        "feature": "node-runtime",
        "title": "节点运行时装配与变量池",
        "page_file": "b08-node-runtime.md",
        "boundary": "负责把运行时依赖（LLM、检索器、文件保存、系统变量）装配进节点；"
                    "不负责节点工厂（b02）、不负责各节点实现（b03-b07）。",
        "refs": [
            {"path": "api/core/workflow/node_runtime.py",
             "symbols": ["DifyPreparedLLM", "DifyRetrieverAttachmentLoader"]},
            {"path": "api/core/workflow/system_variables.py",
             "symbols": ["SystemVariableKey", "build_system_variables"]},
            {"path": "api/core/workflow/file_reference.py",
             "symbols": ["FileReference", "build_file_reference"]},
            {"path": "api/core/workflow/nodes/datasource/datasource_node.py",
             "symbols": ["DatasourceNode"]},
        ],
    },

    # ============ C-工作流生成器 ============
    {
        "feature_group": "C-工作流生成器",
        "feature": "workflow-generator",
        "title": "工作流自动生成器",
        "page_file": "c01-workflow-generator.md",
        "boundary": "负责用 LLM 从自然语言生成工作流 DSL（分阶段生成、错误码、重试）；"
                    "不负责手工 DSL 导入（h04）、不负责工具候选（c02）。",
        "refs": [
            {"path": "api/core/workflow/generator/runner.py", "symbols": ["WorkflowGenerator"]},
            {"path": "api/core/workflow/generator/types.py",
             "symbols": ["WorkflowGenerateErrorCode", "PlannerNodeDict"]},
        ],
    },
    {
        "feature_group": "C-工作流生成器",
        "feature": "tool-catalogue",
        "title": "工具候选与能力查询",
        "page_file": "c02-tool-catalogue.md",
        "boundary": "负责生成器可用的工具目录与能力查询（候选筛选、回退策略）；"
                    "不负责生成主流程（c01）、不负责工具运行时（F 组）。",
        "refs": [
            {"path": "api/core/workflow/generator/tool_catalogue.py",
             "symbols": ["ToolCatalogueEntry", "ToolCapabilityQuery", "ToolCandidateSelection"]},
            {"path": "api/core/workflow/generator/tool_catalogue.py",
             "symbols": ["LegacyToolFallbackSelection"]},
        ],
    },
    {
        "feature_group": "C-工作流生成器",
        "feature": "app-pipeline",
        "title": "流水线应用与 DSL 校验",
        "page_file": "c03-app-pipeline.md",
        "boundary": "负责流水线形态应用的生成与 DSL 结构校验/差异；"
                    "不负责工作流生成（c01）、不负责 RAG 流水线服务（h03）。",
        "refs": [
            {"path": "api/core/app/apps/pipeline/pipeline_generator.py",
             "symbols": ["PipelineGenerator", "PipelineFileAdmission"]},
            {"path": "api/services/workflow/graph_check.py",
             "symbols": ["GraphIssue", "IssueCode", "IssueSeverity"]},
            {"path": "api/services/workflow/graph_diff.py",
             "symbols": ["WorkflowDiff", "NodeChange"]},
        ],
    },

    # ============ D-RAG 摄取与索引 ============
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "extract-processor",
        "title": "文档抽取分派",
        "page_file": "d01-extract-processor.md",
        "boundary": "负责按扩展名分派到各格式抽取器并统一产出文本；"
                    "不负责切分（d02）、不负责索引（d03-d05）。",
        "refs": [
            {"path": "api/core/rag/extractor/extract_processor.py",
             "symbols": ["ExtractProcessor"]},
        ],
    },
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "text-splitter",
        "title": "递归字符切分与增强",
        "page_file": "d02-text-splitter.md",
        "boundary": "负责递归字符切分器及其增强版（分隔符优先级、chunk 重叠）；"
                    "不负责抽取（d01）、不负责索引处理（d03）。",
        "refs": [
            {"path": "api/core/rag/splitter/fixed_text_splitter.py",
             "symbols": ["FixedRecursiveCharacterTextSplitter",
                         "EnhanceRecursiveCharacterTextSplitter"]},
        ],
    },
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "index-processor",
        "title": "索引处理器工厂与基类",
        "page_file": "d03-index-processor.md",
        "boundary": "负责索引处理器的工厂分派、基类契约与内置字段约定；"
                    "不负责三种具体处理器（d04-d06）、不负责检索（E 组）。",
        "refs": [
            {"path": "api/core/rag/index_processor/index_processor.py",
             "symbols": ["IndexProcessorFactory", "IndexProcessor"]},
            {"path": "api/core/rag/index_processor/index_processor_base.py",
             "symbols": ["BaseIndexProcessor"]},
        ],
    },
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "paragraph-index",
        "title": "段落索引处理器",
        "page_file": "d04-paragraph-index.md",
        "boundary": "负责「段落/经济」索引模式的写入与预览；"
                    "不负责父子索引（d05）、不负责问答索引（d06）。",
        "refs": [
            {"path": "api/core/rag/index_processor/processor/paragraph_index_processor.py",
             "symbols": ["ParagraphIndexProcessor"]},
        ],
    },
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "parent-child-index",
        "title": "父子分块索引处理器",
        "page_file": "d05-parent-child-index.md",
        "boundary": "负责父子分块索引（子块检索、父块回填）的写入与预览；"
                    "不负责段落索引（d04）、不负责检索侧拼装（e03）。",
        "refs": [
            {"path": "api/core/rag/index_processor/processor/parent_child_index_processor.py",
             "symbols": ["ParentChildIndexProcessor"]},
        ],
    },
    {
        "feature_group": "D-RAG 摄取与索引",
        "feature": "qa-index",
        "title": "问答对索引处理器",
        "page_file": "d06-qa-index.md",
        "boundary": "负责问答对模式的索引（问题独立向量化）；"
                    "不负责段落索引（d04）、不负责父子索引（d05）。",
        "refs": [
            {"path": "api/core/rag/index_processor/processor/qa_index_processor.py",
             "symbols": ["QAIndexProcessor"]},
        ],
    },

    # ============ E-RAG 检索与后处理 ============
    {
        "feature_group": "E-RAG 检索与后处理",
        "feature": "vector-store",
        "title": "向量库工厂与懒加载",
        "page_file": "e01-vector-store.md",
        "boundary": "负责按配置构造向量库客户端与嵌入的懒加载；"
                    "不负责检索编排（e03）、不负责各后端实现（providers）。",
        "refs": [
            {"path": "api/core/rag/datasource/vdb/vector_factory.py",
             "symbols": ["AbstractVectorFactory", "Vector"]},
            {"path": "api/core/rag/datasource/vdb/vector_type.py", "symbols": ["VectorType"]},
        ],
    },
    {
        "feature_group": "E-RAG 检索与后处理",
        "feature": "embedding-cache",
        "title": "嵌入缓存",
        "page_file": "e02-embedding-cache.md",
        "boundary": "负责嵌入调用的缓存层（批量、去重、缓存缺失回填）；"
                    "不负责向量库（e01）、不负责检索（e03）。",
        "refs": [
            {"path": "api/core/rag/embedding/cached_embedding.py", "symbols": ["CacheEmbedding"]},
        ],
    },
    {
        "feature_group": "E-RAG 检索与后处理",
        "feature": "dataset-retrieval",
        "title": "知识库检索编排",
        "page_file": "e03-dataset-retrieval.md",
        "boundary": "负责一次多知识库检索的编排（路由、并行、分数归一、重试）；"
                    "不负责单库查询（e04）、不负责重排（e05）。",
        "refs": [
            {"path": "api/core/rag/retrieval/dataset_retrieval.py",
             "symbols": ["DatasetRetrieval"]},
            {"path": "api/core/rag/retrieval/retrieval_methods.py",
             "symbols": ["RetrievalMethod"]},
        ],
    },
    {
        "feature_group": "E-RAG 检索与后处理",
        "feature": "retrieval-service",
        "title": "单库检索服务",
        "page_file": "e04-retrieval-service.md",
        "boundary": "负责单知识库的查询执行（向量/全文/混合、元数据过滤、父块回填）；"
                    "不负责多库编排（e03）、不负责重排（e05）。",
        "refs": [
            {"path": "api/core/rag/datasource/retrieval_service.py",
             "symbols": ["SegmentRecord", "DefaultRetrievalModelDict"]},
            {"path": "api/core/rag/docstore/dataset_docstore.py",
             "symbols": ["DatasetDocumentStore"]},
        ],
    },
    {
        "feature_group": "E-RAG 检索与后处理",
        "feature": "rerank-and-post",
        "title": "重排与后处理",
        "page_file": "e05-rerank-and-post.md",
        "boundary": "负责重排模型调用与结果后处理（关键词加权、去重、摘要索引）；"
                    "不负责检索（e03/e04）、不负责索引写入（D 组）。",
        "refs": [
            {"path": "api/core/rag/rerank/rerank_model.py", "symbols": ["RerankModelRunner"]},
            {"path": "api/core/rag/summary_index/summary_index.py", "symbols": ["SummaryIndex"]},
        ],
    },

    # ============ F-插件与工具 ============
    {
        "feature_group": "F-插件与工具",
        "feature": "plugin-service",
        "title": "插件服务与守护进程通信",
        "page_file": "f01-plugin-service.md",
        "boundary": "负责插件生命周期（安装、注册、锁、身份）与到插件守护进程的请求；"
                    "不负责各能力实现（f02）、不负责工具管理（f04）。",
        "refs": [
            {"path": "api/core/plugin/plugin_service.py",
             "symbols": ["PluginService", "_RedisLock"]},
            {"path": "api/core/plugin/impl/base.py", "symbols": ["BasePluginClient"]},
        ],
    },
    {
        "feature_group": "F-插件与工具",
        "feature": "plugin-capabilities",
        "title": "插件提供的四类能力",
        "page_file": "f02-plugin-capabilities.md",
        "boundary": "负责插件暴露的模型、工具、数据源、Agent 端点四类能力客户端；"
                    "不负责插件生命周期（f01）、不负责反向调用（f03）。",
        "refs": [
            {"path": "api/core/plugin/impl/model.py", "symbols": ["PluginModelClient"]},
            {"path": "api/core/plugin/impl/tool.py", "symbols": ["PluginToolManager"]},
            {"path": "api/core/plugin/impl/datasource.py",
             "symbols": ["PluginDatasourceManager"]},
            {"path": "api/core/plugin/impl/agent.py", "symbols": ["PluginAgentClient"]},
        ],
    },
    {
        "feature_group": "F-插件与工具",
        "feature": "plugin-backwards",
        "title": "反向调用",
        "page_file": "f03-plugin-backwards.md",
        "boundary": "负责插件回调宿主（反向调用模型与工具）的机制；"
                    "不负责插件正向能力（f02）、不负责工具运行时（f05）。",
        "refs": [
            {"path": "api/core/plugin/backwards_invocation/model.py",
             "symbols": ["PluginModelBackwardsInvocation"]},
            {"path": "api/core/plugin/backwards_invocation/tool.py",
             "symbols": ["PluginToolBackwardsInvocation"]},
        ],
    },
    {
        "feature_group": "F-插件与工具",
        "feature": "tool-manager",
        "title": "工具管理器与凭据解析",
        "page_file": "f04-tool-manager.md",
        "boundary": "负责工具的加载、凭据解析与运行时规格；"
                    "不负责插件通信（f01/f02）、不负责工作流即工具（f05）。",
        "refs": [
            {"path": "api/core/tools/tool_manager.py",
             "symbols": ["ToolManager", "ToolCredentialResolver", "WorkflowToolRuntimeSpec"]},
            {"path": "api/core/tools/entities/tool_entities.py",
             "symbols": ["ToolProviderType", "ApiProviderSchemaType"]},
        ],
    },
    {
        "feature_group": "F-插件与工具",
        "feature": "workflow-as-tool",
        "title": "工作流即工具",
        "page_file": "f05-workflow-as-tool.md",
        "boundary": "负责把工作流/工作流工具节点包装成可被 Agent 调用的工具；"
                    "不负责工具管理（f04）、不负责 Agent 节点（b04）。",
        "refs": [
            {"path": "api/core/tools/workflow_as_tool/tool.py", "symbols": ["WorkflowTool"]},
            {"path": "api/core/workflow/workflow_tool_node.py",
             "symbols": ["DifyWorkflowToolNode"]},
            {"path": "api/core/workflow/workflow_tool_container_handler.py",
             "symbols": ["WorkflowToolContainerHandler"]},
        ],
    },

    # ============ G-Agent 与 MCP ============
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "agent-runners",
        "title": "Agent 执行器族",
        "page_file": "g01-agent-runners.md",
        "boundary": "负责旧式 Agent 执行器（CoT 与函数调用两种策略）；"
                    "不负责应用层接入（a05）、不负责 MCP（g05）。",
        "refs": [
            {"path": "api/core/agent/base_agent_runner.py", "symbols": ["BaseAgentRunner"]},
            {"path": "api/core/agent/fc_agent_runner.py",
             "symbols": ["FunctionCallAgentRunner"]},
            {"path": "api/core/agent/cot_agent_runner.py", "symbols": ["CotAgentRunner"]},
        ],
    },
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "dify-agent-runtime",
        "title": "外部 Agent 运行时",
        "page_file": "g02-dify-agent-runtime.md",
        "boundary": "负责 dify-agent 这个独立包的运行循环（校验、事件、用量、取消）；"
                    "不负责宿主侧接入（a05）、不负责分层中间件（g03）。",
        "refs": [
            {"path": "dify-agent/src/dify_agent/runtime/runner.py",
             "symbols": ["AgentRunRunner", "AgentRunValidationError"]},
            {"path": "dify-agent/src/dify_agent/protocol/__init__.py", "symbols": ["__all__"]},
        ],
    },
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "agent-layers",
        "title": "Agent 能力分层",
        "page_file": "g03-agent-layers.md",
        "boundary": "负责 dify-agent 的分层能力（提示、历史、知识、插件、读媒体、shell、输出）；"
                    "不负责运行循环（g02）、不负责 MCP（g05）。",
        "refs": [
            {"path": "dify-agent/src/dify_agent/layers/__init__.py", "symbols": ["__all__"]},
            {"path": "dify-agent/src/dify_agent/runtime/modules.py",
             "symbols": ["load_modules", "create_modules", "REGISTRY"]},
        ],
    },
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "mcp-client",
        "title": "MCP 客户端与传输",
        "page_file": "g04-mcp-client.md",
        "boundary": "负责 MCP 客户端（SSE / Streamable HTTP 传输、会话、重连）；"
                    "不负责 MCP 服务端（g06）、不负责工具管理（f04）。",
        "refs": [
            {"path": "api/core/mcp/mcp_client.py", "symbols": ["MCPClient"]},
            {"path": "api/core/mcp/client/streamable_client.py",
             "symbols": ["StreamableHTTPTransport", "StreamableHTTPError"]},
            {"path": "api/core/mcp/session/client_session.py", "symbols": ["ClientSession"]},
        ],
    },
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "mcp-auth",
        "title": "MCP 授权流程",
        "page_file": "g05-mcp-auth.md",
        "boundary": "负责 MCP 的 OAuth 发现、PKCE 与状态保持；"
                    "不负责传输（g04）、不负责服务端（g06）。",
        "refs": [
            {"path": "api/core/mcp/auth/auth_flow.py",
             "symbols": ["generate_pkce_challenge", "discover_protected_resource_metadata"]},
            {"path": "api/core/mcp/auth/auth_flow.py",
             "symbols": ["discover_oauth_authorization_server_metadata"]},
        ],
    },
    {
        "feature_group": "G-Agent 与 MCP",
        "feature": "mcp-server",
        "title": "MCP 服务端",
        "page_file": "g06-mcp-server.md",
        "boundary": "负责把应用暴露成 MCP 服务端（协议协商、工具列表、调用）；"
                    "不负责客户端（g04）、不负责授权（g05）。",
        "refs": [
            {"path": "api/core/mcp/server/streamable_http.py",
             "symbols": ["handle_mcp_request", "handle_call_tool", "build_parameter_schema"]},
            {"path": "api/core/mcp/types.py", "symbols": ["Request", "Notification", "Result"]},
        ],
    },

    # ============ H-服务与仓储 ============
    {
        "feature_group": "H-服务与仓储",
        "feature": "knowledge-service",
        "title": "知识库与文档服务",
        "page_file": "h01-knowledge-service.md",
        "boundary": "负责知识库/文档的领域服务（分段、索引任务、状态机）；"
                    "不负责索引写入（D 组）、不负责仓储（h05）。",
        "refs": [
            {"path": "api/services/knowledge/dataset_service.py",
             "symbols": ["DatasetService", "DocumentService"]},
        ],
    },
    {
        "feature_group": "H-服务与仓储",
        "feature": "knowledge-fs",
        "title": "知识文件系统与后台引擎",
        "page_file": "h02-knowledge-fs.md",
        "boundary": "负责把知识库暴露成文件系统语义的绑定与后台执行；"
                    "不负责知识库服务（h01）、不负责 RAG 流水线（h03）。",
        "refs": [
            {"path": "api/services/knowledge_fs/background_engine.py",
             "symbols": ["KnowledgeFSBackgroundEngine"]},
        ],
    },
    {
        "feature_group": "H-服务与仓储",
        "feature": "rag-pipeline-service",
        "title": "RAG 流水线服务",
        "page_file": "h03-rag-pipeline-service.md",
        "boundary": "负责 RAG 流水线应用的编排服务（DSL、任务、变量池）；"
                    "不负责流水线生成（c03）、不负责知识库服务（h01）。",
        "refs": [
            {"path": "api/services/rag_pipeline/rag_pipeline.py",
             "symbols": ["RagPipelineService"]},
            {"path": "api/services/rag_pipeline/rag_pipeline_dsl_service.py",
             "symbols": ["RagPipelineDslService", "CURRENT_DSL_VERSION"]},
        ],
    },
    {
        "feature_group": "H-服务与仓储",
        "feature": "rbac-and-tools-service",
        "title": "权限与工具管理服务",
        "page_file": "h04-rbac-and-tools-service.md",
        "boundary": "负责工作区 RBAC 内置角色与工具管理服务（内置/MCP/API 三类）；"
                    "不负责工具运行时（f04）、不负责知识库服务（h01）。",
        "refs": [
            {"path": "api/services/rbac/builtin.py",
             "symbols": ["builtin_member_roles", "builtin_permissions"]},
            {"path": "api/services/rbac/contracts.py", "symbols": ["Paginated", "MembersInRole"]},
            {"path": "api/services/tools/builtin_tools_manage_service.py",
             "symbols": ["BuiltinToolManageService"]},
            {"path": "api/services/tools/mcp_tools_manage_service.py",
             "symbols": ["ReconnectResult", "OAuthDataType"]},
        ],
    },
    {
        "feature_group": "H-服务与仓储",
        "feature": "repositories",
        "title": "仓储工厂与持久化模型",
        "page_file": "h05-repositories.md",
        "boundary": "负责仓储工厂与 SQLAlchemy 模型的组织方式；"
                    "不负责领域服务（h01）、不负责配置（i02）。",
        "refs": [
            {"path": "api/repositories/factory.py",
             "symbols": ["DifyAPIRepositoryFactory"]},
            {"path": "api/models/dataset.py",
             "symbols": ["ProcessRuleDict", "build_built_in_field_values"]},
        ],
    },
    {
        "feature_group": "H-服务与仓储",
        "feature": "tracing-and-evaluation",
        "title": "追踪与评测",
        "page_file": "h06-tracing-and-evaluation.md",
        "boundary": "负责追踪规范模型与评测框架接入（ragas/deepeval）；"
                    "不负责观测后端（i06）、不负责 Agent 运行时（g02）。",
        "refs": [
            {"path": "api/services/tracing/canonical.py",
             "symbols": ["CanonicalTraceBuilder"]},
            {"path": "api/core/evaluation/evaluation_manager.py",
             "symbols": ["EvaluationManager"]},
            {"path": "api/core/evaluation/frameworks/ragas/ragas_evaluator.py",
             "symbols": ["RagasEvaluator"]},
            {"path": "api/core/evaluation/frameworks/deepeval/deepeval_evaluator.py",
             "symbols": ["DeepEvalEvaluator"]},
        ],
    },

    # ============ I-运行时支撑与部署 ============
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "model-provider",
        "title": "模型提供方服务",
        "page_file": "i01-model-provider.md",
        "boundary": "负责模型提供方与模型的注册、校验与凭据；"
                    "不负责 LLM 节点（b03）、不负责插件模型（f02）。",
        "refs": [
            {"path": "api/services/model_provider/service.py",
             "symbols": ["ModelProviderService"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "configs",
        "title": "配置装配",
        "page_file": "i02-configs.md",
        "boundary": "负责中间件/特性开关等配置的读取与校验；"
                    "不负责部署形态（i07）、不负责模型提供方（i01）。",
        "refs": [
            {"path": "api/configs/middleware/__init__.py",
             "symbols": ["StorageConfig", "VectorStoreConfig", "DatabaseConfig"]},
            {"path": "api/configs/feature/__init__.py",
             "symbols": ["AppExecutionConfig", "PluginConfig"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "async-tasks",
        "title": "异步任务与工作流执行任务",
        "page_file": "i03-async-tasks.md",
        "boundary": "负责 Celery 异步任务（工作流执行、索引、清理）的入口与恢复；"
                    "不负责 Celery 装配（i04）、不负责应用执行骨架（a01）。",
        "refs": [
            {"path": "api/tasks/app_generate/workflow_execute_task.py",
             "symbols": ["_publish_failed_workflow_terminal_events", "_resolve_user_for_run"]},
            {"path": "api/tasks/async_workflow_tasks.py",
             "symbols": ["execute_workflow_professional", "resume_workflow_execution"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "celery-and-blueprints",
        "title": "Celery 与蓝图装配",
        "page_file": "i04-celery-and-blueprints.md",
        "boundary": "负责 Celery 配置（哨兵、beat 调度）与 Flask 蓝图注册；"
                    "不负责任务实现（i03）、不负责存储后端（i05）。",
        "refs": [
            {"path": "api/extensions/ext_celery.py", "symbols": ["CelerySSLOptionsDict"]},
            {"path": "api/extensions/ext_blueprints.py", "symbols": ["init_app"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "storage-and-human-input-delivery",
        "title": "存储后端与人工介入投递",
        "page_file": "i05-storage-and-human-input-delivery.md",
        "boundary": "负责对象存储后端抽象与人工介入的多渠道投递绑定；"
                    "不负责 Celery（i04）、不负责人工介入节点（b06）。",
        "refs": [
            {"path": "api/extensions/storage/base_storage.py", "symbols": ["BaseStorage"]},
            {"path": "api/core/human_input_v2/im_integration/__init__.py",
             "symbols": ["IMSyncRepository", "OpaqueProviderPayload"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "observability",
        "title": "日志存储与可观测性",
        "page_file": "i06-observability.md",
        "boundary": "负责日志存储适配（云日志/SQL 双实现）与 OTel 接入；"
                    "不负责追踪规范（h06）、不负责 Celery（i04）。",
        "refs": [
            {"path": "api/extensions/logstore/aliyun_logstore.py",
             "symbols": ["AliyunLogStore"]},
            {"path": "api/extensions/logstore/sql_escape.py",
             "symbols": ["escape_logstore_query_value", "escape_identifier"]},
        ],
    },
    {
        "feature_group": "I-运行时支撑与部署",
        "feature": "controllers",
        "title": "控制器分层与鉴权包装",
        "page_file": "i07-controllers.md",
        "boundary": "负责四类控制器（console/openapi/service_api/inner_api）的分层与鉴权装饰器；"
                    "不负责服务实现（H 组）、不负责配置（i02）。",
        "refs": [
            {"path": "api/controllers/console/__init__.py",
             "symbols": ["_register_controllers"]},
            {"path": "api/controllers/openapi/__init__.py", "symbols": ["_OpenApi"]},
            {"path": "api/controllers/service_api/wraps.py",
             "symbols": ["validate_app_token", "FetchUserArg"]},
        ],
    },
]
