# -*- coding: utf-8 -*-
"""pydantic-ai 机制清单（阶段 2A 第六个 AI / Agent 项目，收尾 2A 波次）。

盘点范围：两个包 —— `pydantic_ai_slim/pydantic_ai/`（智能体 SDK 主体）与
`pydantic_graph/pydantic_graph/`（独立图执行引擎，SDK 的 agent 循环本身是一张图）。
`pydantic_evals`（评估框架）是独立包、面向的是测试与数据集而非运行时机制，
本轮不纳入。

pydantic-ai 与其他 agent 框架最大的差别是 **能力系统（Capability）**：不是靠继承、
也不是靠钩子回调，而是把「可组合的执行期增强」做成一等公民，通过一层组合器把
多个能力编织成一次请求/一次工具调用/一次输出校验的包裹链。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按包内目录骨架
归并成机制簇。篇数是归并结果，不是按文件数折算。

源码锚点：`pydantic-ai@0fb8436a011e8938d98acf60bd6bf342c32f7ef0`。
"""

GROUP_ORDER = [
    "A-Agent 定义与运行入口",
    "B-图执行引擎",
    "C-能力系统",
    "D-工具与工具集",
    "E-工具执行与延迟审批",
    "F-输出与结构化",
    "G-模型层",
    "H-消息与历史",
    "I-MCP 与工作区",
    "J-持久执行与可观测",
]

MECHANISM_SPECS = [
    # ============ A Agent 定义与运行入口 ============
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "agent-definition",
        "title": "Agent 定义与抽象基类",
        "page_file": "a01-agent-definition.md",
        "boundary": "负责 Agent 由哪些字段构成、与 AbstractAgent 的关系、以及 agent 循环如何被"
                    "编织成一张图；不负责图引擎本身（B 组）、不负责运行结果（a03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/agent/__init__.py",
             "symbols": ["Agent"]},
            {"path": "pydantic_ai_slim/pydantic_ai/agent/abstract.py",
             "symbols": ["AbstractAgent", "AgentRetries"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "agent-spec",
        "title": "Agent 规格与可序列化定义",
        "page_file": "a02-agent-spec.md",
        "boundary": "负责把 agent 定义外化成可序列化的规格（含能力的声明与加载），"
                    "使定义能脱离 Python 构造；不负责运行（a03）、不负责能力语义（C 组）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/agent/spec.py",
             "symbols": ["AgentSpec", "CapabilitySpecContext"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_spec.py",
             "symbols": ["NamedSpec", "CapabilitySpec", "build_registry"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "run-and-result",
        "title": "运行对象与结果契约",
        "page_file": "a03-run-and-result.md",
        "boundary": "负责一次运行的句柄（可迭代、可取消、可流式）与运行结果的字段构成；"
                    "不负责循环内部（a04）、不负责流式结果（f 组之外的 result 页面）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/run.py",
             "symbols": ["AgentRun", "AgentRunResult", "AgentRunResultEvent"]},
            {"path": "pydantic_ai_slim/pydantic_ai/result.py",
             "symbols": ["StreamedRunResult", "AgentStream", "FinalResult"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "run-context",
        "title": "运行上下文与依赖注入",
        "page_file": "a04-run-context.md",
        "boundary": "负责 RunContext 携带的依赖、模型与用量、输出提交入口，以及上下文在"
                    "工具与能力之间的传播；不负责结果对象（a03）、不负责用量归属（j03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_run_context.py",
             "symbols": ["RunContext", "OutputSubmitter", "EventStreamBuffer"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_cancel.py",
             "symbols": ["CancellationToken", "RunCancellation"]},
        ],
    },

    # ============ B 图执行引擎 ============
    {
        "feature_group": "B-图执行引擎",
        "feature": "graph-builder",
        "title": "图构建与运行入口",
        "page_file": "b01-graph-builder.md",
        "boundary": "负责把节点与边组装成可执行的图、以及图运行时的任务调度与迭代；"
                    "不负责节点契约（b02）、不负责分支与合并（b03、b04）。",
        "refs": [
            {"path": "pydantic_graph/pydantic_graph/graph_builder.py",
             "symbols": ["Graph", "GraphRun", "GraphTask"]},
        ],
    },
    {
        "feature_group": "B-图执行引擎",
        "feature": "node-contract",
        "title": "节点契约与运行上下文",
        "page_file": "b02-node-contract.md",
        "boundary": "负责节点的抽象契约：返回下一个节点或结束、读写图状态；"
                    "不负责图构建（b01）、不负责步（b05）。",
        "refs": [
            {"path": "pydantic_graph/pydantic_graph/basenode.py",
             "symbols": ["BaseNode", "GraphRunContext", "End", "Edge"]},
            {"path": "pydantic_graph/pydantic_graph/node.py",
             "symbols": ["StartNode", "EndNode", "Fork"]},
        ],
    },
    {
        "feature_group": "B-图执行引擎",
        "feature": "decision-routing",
        "title": "决策节点与分支路由",
        "page_file": "b03-decision-routing.md",
        "boundary": "负责按条件在多个后续节点间选择，以及分支的声明方式；"
                    "不负责并行路径（b04）、不负责合并（b04）。",
        "refs": [
            {"path": "pydantic_graph/pydantic_graph/decision.py",
             "symbols": ["Decision", "DecisionBranch", "DecisionBranchBuilder"]},
        ],
    },
    {
        "feature_group": "B-图执行引擎",
        "feature": "fork-join",
        "title": "并行分叉与汇合",
        "page_file": "b04-fork-join.md",
        "boundary": "负责并行路径的展开、路径标记与结果汇合（含归约器）；"
                    "不负责分支决策（b03）、不负责步（b05）。",
        "refs": [
            {"path": "pydantic_graph/pydantic_graph/paths.py",
             "symbols": ["Path", "PathBuilder", "BroadcastMarker"]},
            {"path": "pydantic_graph/pydantic_graph/join.py",
             "symbols": ["Join", "JoinState", "ReducerContext"]},
        ],
    },
    {
        "feature_group": "B-图执行引擎",
        "feature": "step-and-stream",
        "title": "步节点与流式节点",
        "page_file": "b05-step-and-stream.md",
        "boundary": "负责把函数包装成节点（含流式函数）以及步上下文的构造；"
                    "不负责节点契约（b02）、不负责图构建（b01）。",
        "refs": [
            {"path": "pydantic_graph/pydantic_graph/step.py",
             "symbols": ["Step", "StepNode", "StepContext", "NodeStep"]},
        ],
    },

    # ============ C 能力系统 ============
    {
        "feature_group": "C-能力系统",
        "feature": "capability-contract",
        "title": "能力契约与生命周期挂载点",
        "page_file": "c01-capability-contract.md",
        "boundary": "负责能力能挂到哪些执行点上、每类挂载点的语义与顺序；"
                    "不负责组合（c02）、不负责具体能力（c03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/capability.py",
             "symbols": ["Capability"]},
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/abstract.py",
             "symbols": ["AbstractCapability", "CapabilityOrdering"]},
        ],
    },
    {
        "feature_group": "C-能力系统",
        "feature": "capability-composition",
        "title": "能力组合与排序",
        "page_file": "c02-capability-composition.md",
        "boundary": "负责多个能力如何被编织成一条包裹链、顺序如何决定、以及重复能力如何合并；"
                    "不负责契约（c01）、不负责具体能力（c03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/combined.py",
             "symbols": ["CombinedCapability"]},
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/abstract.py",
             "symbols": ["leaf_capabilities", "_combine_duplicate_capabilities"]},
        ],
    },
    {
        "feature_group": "C-能力系统",
        "feature": "builtin-capabilities",
        "title": "内置能力清单",
        "page_file": "c03-builtin-capabilities.md",
        "boundary": "负责若干代表性内置能力（缓存、思考、模型回退、工具集挂载、工具预处理）"
                    "各自改写什么；不负责组合机制（c02）、不负责钩子（c04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/caching.py", "symbols": ["Caching"]},
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/thinking.py", "symbols": ["Thinking"]},
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/model_fallback.py",
             "symbols": ["ModelFallback"]},
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/prepare_tools.py",
             "symbols": ["PrepareTools", "PrepareOutputTools"]},
        ],
    },
    {
        "feature_group": "C-能力系统",
        "feature": "capability-hooks",
        "title": "能力钩子与超时",
        "page_file": "c04-capability-hooks.md",
        "boundary": "负责钩子的注册形态、事件匹配与工具级过滤，以及钩子超时；"
                    "不负责组合（c02）、不负责具体能力（c03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/capabilities/hooks.py",
             "symbols": ["HookTimeoutError", "_HookEntry", "_filter_tool_entries"]},
        ],
    },

    # ============ D 工具与工具集 ============
    {
        "feature_group": "D-工具与工具集",
        "feature": "tool-definition",
        "title": "工具定义与 schema",
        "page_file": "d01-tool-definition.md",
        "boundary": "负责工具定义的字段、类型种类与 JSON schema 生成；"
                    "不负责工具集（d02）、不负责参数校验（d04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/tools.py",
             "symbols": ["Tool", "ToolDefinition", "GenerateToolJsonSchema"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_function_schema.py",
             "symbols": ["function_schema", "FunctionSchema", "takes_ctx"]},
        ],
    },
    {
        "feature_group": "D-工具与工具集",
        "feature": "toolsets",
        "title": "工具集抽象与装饰型工具集",
        "page_file": "d02-toolsets.md",
        "boundary": "负责工具集的抽象契约与几种装饰型实现（组合、过滤、加前缀、重命名、延迟加载）；"
                    "不负责单个工具（d01）、不负责调用校验（d04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/toolsets/abstract.py",
             "symbols": ["AbstractToolset", "ToolsetTool"]},
            {"path": "pydantic_ai_slim/pydantic_ai/toolsets/function.py",
             "symbols": ["FunctionToolset"]},
            {"path": "pydantic_ai_slim/pydantic_ai/toolsets/filtered.py",
             "symbols": ["FilteredToolset"]},
            {"path": "pydantic_ai_slim/pydantic_ai/toolsets/prefixed.py",
             "symbols": ["PrefixedToolset"]},
        ],
    },
    {
        "feature_group": "D-工具与工具集",
        "feature": "tool-manager",
        "title": "工具管理器与工具裁决",
        "page_file": "d03-tool-manager.md",
        "boundary": "负责在一次请求前把多个工具集裁成一个可用集合：同名工具谁留下、"
                    "不可用工具如何标记、循环覆盖如何破解；不负责工具集实现（d02）、不负责执行（e01）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/tool_manager.py",
             "symbols": ["ToolManager", "ValidatedToolCall"]},
            {"path": "pydantic_ai_slim/pydantic_ai/tool_manager.py",
             "symbols": ["_decide_which_tools_stay", "_first_on_a_cycle"]},
        ],
    },
    {
        "feature_group": "D-工具与工具集",
        "feature": "tool-validation",
        "title": "工具参数校验与重试",
        "page_file": "d04-tool-validation.md",
        "boundary": "负责模型给出的工具参数如何校验、校验失败如何以「重试提示」回给模型；"
                    "不负责执行（e01）、不负责审批（e02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/exceptions.py",
             "symbols": ["ModelRetry", "ToolFailed", "SkipToolValidation"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_tool_execution.py",
             "symbols": ["_ToolCallProcessor", "tool_bound_retry_part", "build_tool_return_part"]},
        ],
    },

    # ============ E 工具执行与延迟审批 ============
    {
        "feature_group": "E-工具执行与延迟审批",
        "feature": "tool-execution",
        "title": "工具执行与并发控制",
        "page_file": "e01-tool-execution.md",
        "boundary": "负责工具调用的实际执行路径：并发限流、失败归类、结果回填；"
                    "不负责校验（d04）、不负责延迟工具（e02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/concurrency.py",
             "symbols": ["ConcurrencyLimiter", "ConcurrencyLimit", "get_concurrency_context"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_tool_execution.py",
             "symbols": ["_segment_by_barriers", "_duplicate_tool_call_ids"]},
        ],
    },
    {
        "feature_group": "E-工具执行与延迟审批",
        "feature": "deferred-tools",
        "title": "延迟工具调用与审批",
        "page_file": "e02-deferred-tools.md",
        "boundary": "负责把工具调用挂起成「待批准/待外部结果」、以及带结果恢复运行；"
                    "不负责执行（e01）、不负责工具搜索（e03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_deferred.py",
             "symbols": ["DeferredToolRequests", "DeferredToolResults", "ToolApproved", "ToolDenied"]},
            {"path": "pydantic_ai_slim/pydantic_ai/exceptions.py",
             "symbols": ["ApprovalRequired", "CallDeferred"]},
        ],
    },
    {
        "feature_group": "E-工具执行与延迟审批",
        "feature": "tool-search",
        "title": "工具搜索与延迟发现",
        "page_file": "e03-tool-search.md",
        "boundary": "负责「工具太多时按需发现」的机制：搜索调用、返回内容与本地/原生两种形态；"
                    "不负责工具集（d02）、不负责审批（e02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_tool_search.py",
             "symbols": ["ToolSearchCallPart", "ToolSearchReturnPart", "ToolSearchArgs"]},
            {"path": "pydantic_ai_slim/pydantic_ai/toolsets/deferred_loading.py",
             "symbols": ["DeferredLoadingToolset"]},
        ],
    },

    # ============ F 输出与结构化 ============
    {
        "feature_group": "F-输出与结构化",
        "feature": "output-modes",
        "title": "四种输出模式",
        "page_file": "f01-output-modes.md",
        "boundary": "负责文本、工具、原生、提示词四种输出模式的差别与选择；"
                    "不负责 schema 构造（f02）、不负责校验失败处理（f03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/output.py",
             "symbols": ["TextOutput", "ToolOutput", "NativeOutput", "PromptedOutput"]},
        ],
    },
    {
        "feature_group": "F-输出与结构化",
        "feature": "output-schema",
        "title": "输出 schema 与自动推断",
        "page_file": "f02-output-schema.md",
        "boundary": "负责把输出类型转成 schema、自动模式下如何选择与退让、以及 schema 的组织；"
                    "不负责模式语义（f01）、不负责校验（f03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_output.py",
             "symbols": ["OutputSchema", "AutoOutputSchema", "TextOutputSchema"]},
            {"path": "pydantic_ai_slim/pydantic_ai/output.py",
             "symbols": ["OutputObjectDefinition", "OutputContext"]},
        ],
    },
    {
        "feature_group": "F-输出与结构化",
        "feature": "output-validation",
        "title": "输出校验与不完整输出处理",
        "page_file": "f03-output-validation.md",
        "boundary": "负责输出校验失败的重试、被截断的 JSON 与不完整工具调用的识别与修复提示；"
                    "不负责 schema（f02）、不负责流式（a03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_output.py",
             "symbols": ["OutputValidator", "is_incomplete_json", "truncated_output_message"]},
        ],
    },

    # ============ G 模型层 ============
    {
        "feature_group": "G-模型层",
        "feature": "model-abstract",
        "title": "模型抽象与请求参数",
        "page_file": "g01-model-abstract.md",
        "boundary": "负责模型接口的最小契约与一次请求的参数结构；"
                    "不负责具体实现（g03）、不负责回退（g02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/models/_abstract.py",
             "symbols": ["AbstractModel"]},
            {"path": "pydantic_ai_slim/pydantic_ai/models/__init__.py",
             "symbols": ["ModelRequestParameters", "ModelRequestContext"]},
        ],
    },
    {
        "feature_group": "G-模型层",
        "feature": "model-fallback",
        "title": "模型回退链",
        "page_file": "g02-model-fallback.md",
        "boundary": "负责按条件在多个模型间回退：拒绝判定、尝试记录、消息回退与续接；"
                    "不负责抽象（g01）、不负责并发限流（g04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/models/fallback.py",
             "symbols": ["FallbackModel"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_fallback.py",
             "symbols": ["FallbackChain", "ResponseRejected", "FallbackPredicates"]},
        ],
    },
    {
        "feature_group": "G-模型层",
        "feature": "model-providers",
        "title": "提供方与模型档案",
        "page_file": "g03-model-providers.md",
        "boundary": "负责提供方注册与推断、模型档案（能力与限制）如何影响请求；"
                    "不负责抽象（g01）、不负责回退（g02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/providers/__init__.py",
             "symbols": ["Provider", "infer_provider_class", "infer_provider"]},
            {"path": "pydantic_ai_slim/pydantic_ai/profiles/__init__.py",
             "symbols": ["ModelProfile", "merge_profile"]},
        ],
    },
    {
        "feature_group": "G-模型层",
        "feature": "model-wrappers",
        "title": "模型包装：插桩与并发限流",
        "page_file": "g04-model-wrappers.md",
        "boundary": "负责在不改模型实现的前提下加一层（可观测插桩、并发上限、批量请求）；"
                    "不负责档案（g03）、不负责追踪细节（j02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/models/instrumented.py",
             "symbols": ["InstrumentedModel", "InstrumentationSettings", "instrument_model"]},
            {"path": "pydantic_ai_slim/pydantic_ai/models/concurrency.py",
             "symbols": ["ConcurrencyLimitedModel", "limit_model_concurrency"]},
            {"path": "pydantic_ai_slim/pydantic_ai/models/wrapper.py",
             "symbols": ["WrapperModel"]},
        ],
    },

    # ============ H 消息与历史 ============
    {
        "feature_group": "H-消息与历史",
        "feature": "message-model",
        "title": "消息部件与多模态内容",
        "page_file": "h01-message-model.md",
        "boundary": "负责消息的部件化结构、多模态内容类型与序列化；"
                    "不负责历史改写（h02）、不负责帧包裹（h03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/messages.py",
             "symbols": ["SystemPromptPart", "TextContent", "ImageUrl"]},
        ],
    },
    {
        "feature_group": "H-消息与历史",
        "feature": "history-processing",
        "title": "历史处理与重写",
        "page_file": "h02-history-processing.md",
        "boundary": "负责请求前对消息历史的改写（裁剪、重写、合并尾部），以及消息入队；"
                    "不负责消息结构（h01）、不负责会话存储（h04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_run_conversation.py",
             "symbols": ["_HistoryMerge", "conversation_from_state", "full_history"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_enqueue.py",
             "symbols": ["PendingMessageQueue", "PendingMessage"]},
        ],
    },
    {
        "feature_group": "H-消息与历史",
        "feature": "framing",
        "title": "内容帧包裹与提示注入防护",
        "page_file": "h03-framing.md",
        "boundary": "负责给不可信内容加帧标记，使模型能区分「数据」与「指令」；"
                    "不负责消息结构（h01）、不负责历史改写（h02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/framing.py",
             "symbols": ["wrap", "open_tag", "close_tag"]},
        ],
    },
    {
        "feature_group": "H-消息与历史",
        "feature": "conversation-store",
        "title": "会话存储与冲突检测",
        "page_file": "h04-conversation-store.md",
        "boundary": "负责把对话持久化成可恢复的记录、并发写入的冲突检测与作用域；"
                    "不负责历史改写（h02）、不负责运行状态编码（j01）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/conversation_store.py",
             "symbols": ["ConversationStore", "InMemoryConversationStore", "ConversationConflict"]},
            {"path": "pydantic_ai_slim/pydantic_ai/conversation.py", "symbols": ["Conversation"]},
        ],
    },

    # ============ I-MCP 与工作区 ============
    {
        "feature_group": "I-MCP 与工作区",
        "feature": "mcp-integration",
        "title": "MCP 接入与能力映射",
        "page_file": "i01-mcp-integration.md",
        "boundary": "负责把 MCP 服务器的工具/资源/采样接进 SDK，以及双向消息映射；"
                    "不负责工作区（i02）、不负责工具搜索（e03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/mcp.py",
             "symbols": ["MCPError", "Resource", "ResourceTemplate"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_mcp.py",
             "symbols": ["map_from_mcp_params", "map_from_pai_messages"]},
        ],
    },
    {
        "feature_group": "I-MCP 与工作区",
        "feature": "workspaces",
        "title": "工作区后端与权限分层",
        "page_file": "i02-workspaces.md",
        "boundary": "负责文件读写后端的抽象与几种形态（本地、只读、带授权）；"
                    "不负责 MCP（i01）、不负责网络抓取防护（i03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/workspaces/workspace.py",
             "symbols": ["Workspace", "WorkspacePaths"]},
            {"path": "pydantic_ai_slim/pydantic_ai/workspaces/local.py",
             "symbols": ["LocalWorkspaceBackend"]},
            {"path": "pydantic_ai_slim/pydantic_ai/workspaces/authority.py",
             "symbols": ["AuthorityWorkspace"]},
        ],
    },
    {
        "feature_group": "I-MCP 与工作区",
        "feature": "ssrf-guard",
        "title": "网络抓取的地址校验",
        "page_file": "i03-ssrf-guard.md",
        "boundary": "负责抓取外部 URL 前的地址解析与内网地址拦截、以及下载体积上限；"
                    "不负责工作区（i02）、不负责工具定义（d01）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_ssrf.py",
             "symbols": ["ResolvedUrl", "DownloadTooLargeError"]},
            {"path": "pydantic_ai_slim/pydantic_ai/common_tools/web_fetch.py",
             "symbols": ["WebFetchLocalTool", "WebFetchResult"]},
        ],
    },

    # ============ J-持久执行与可观测 ============
    {
        "feature_group": "J-持久执行与可观测",
        "feature": "durable-execution",
        "title": "持久执行与可重放单元",
        "page_file": "j01-durable-execution.md",
        "boundary": "负责把一次运行拆成可持久化的操作单元、重启后如何重放、以及缓存身份；"
                    "不负责会话存储（h04）、不负责取消（j04）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/durable_exec/_base.py",
             "symbols": ["_RestrictedRunContext", "_ModelRequestCacheIdentity",
                         "conversation_id_from_run_id"]},
            {"path": "pydantic_ai_slim/pydantic_ai/durable_exec/_spec.py",
             "symbols": ["DurabilityEngineSpec"]},
        ],
    },
    {
        "feature_group": "J-持久执行与可观测",
        "feature": "instrumentation",
        "title": "插桩与敏感内容策略",
        "page_file": "j02-instrumentation.md",
        "boundary": "负责追踪跨度里的内容捕获策略、请求上下文与命名；"
                    "不负责模型包装（g04）、不负责用量（j03）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_instrumentation.py",
             "symbols": ["ContentPolicy", "InstrumentationNames", "span_include_content"]},
        ],
    },
    {
        "feature_group": "J-持久执行与可观测",
        "feature": "usage-accounting",
        "title": "用量归属与成本",
        "page_file": "j03-usage-accounting.md",
        "boundary": "负责 token 与成本的累计、按请求/工具调用的归属、以及限额判定；"
                    "不负责上下文长度估算（f02）、不负责插桩（j02）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/usage.py",
             "symbols": ["RunUsage", "RequestUsage", "UsageLimits"]},
            {"path": "pydantic_ai_slim/pydantic_ai/_usage_attribution.py",
             "symbols": ["accumulate", "record_request", "record_tool_call"]},
        ],
    },
    {
        "feature_group": "J-持久执行与可观测",
        "feature": "cancellation",
        "title": "取消传播与截止时间",
        "page_file": "j04-cancellation.md",
        "boundary": "负责取消信号如何在运行、工具、子运行之间传播，以及运行级截止时间；"
                    "不负责重试（d04）、不负责持久执行（j01）。",
        "refs": [
            {"path": "pydantic_ai_slim/pydantic_ai/_cancel.py",
             "symbols": ["RunBinding", "provide_run_binding", "inherited_run_deadline"]},
            {"path": "pydantic_ai_slim/pydantic_ai/retries.py",
             "symbols": ["RetryConfig", "wait_retry_after"]},
        ],
    },
]
