# -*- coding: utf-8 -*-
"""openai-agents 机制清单（阶段 2A 第五个 AI / Agent 项目）。

盘点范围：`src/agents/` 主包。这是 OpenAI 官方的 Agents SDK：以 Runner 为唯一
执行入口，围绕「一轮模型调用 + 工具/交接执行」的循环，配护栏、会话、MCP、
追踪四套横切能力。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按包内目录
骨架归并成机制簇。篇数是归并结果，不是按文件数折算。

`realtime/`（实时语音）、`voice/`（语音流水线）、`sandbox/`（远程沙箱）三块
各自是独立子系统且体量大，本轮不纳入，留作后续增量。

源码锚点：`openai-agents-python@6afdd47b4fab14d61e0e6f7c002e6ec9b36fcd71`。
"""

GROUP_ORDER = [
    "A-Agent 定义与运行入口",
    "B-单轮执行循环",
    "C-工具系统",
    "D-护栏",
    "E-交接",
    "F-模型层",
    "G-会话与记忆",
    "H-MCP 集成",
    "I-追踪与可观测",
    "J-流式与结果",
]

MECHANISM_SPECS = [
    # ============ A Agent 定义与运行入口 ============
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "agent-definition",
        "title": "Agent 定义与配置合并",
        "page_file": "a01-agent-definition.md",
        "boundary": "负责 Agent 由哪些字段构成、工具与 MCP 与服务端配置如何归一、"
                    "以及 AgentBase 与 Agent 的继承关系；不负责执行循环（B 组）、不负责工具包装（c02）。",
        "refs": [
            {"path": "src/agents/agent.py",
             "symbols": ["AgentBase", "Agent", "ToolsToFinalOutputResult"]},
            {"path": "src/agents/agent.py",
             "symbols": ["StopAtTools", "MCPConfig", "_initial_model_settings_for_model"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "runner-entry",
        "title": "Runner 唯一入口与默认运行器",
        "page_file": "a02-runner-entry.md",
        "boundary": "负责 Runner 的三个入口（同步 / 异步 / 流式）、默认运行器的可替换性与参数归一；"
                    "不负责循环内部（b01）、不负责结果对象（j02）。",
        "refs": [
            {"path": "src/agents/run.py",
             "symbols": ["Runner", "AgentRunner", "set_default_agent_runner"]},
            {"path": "src/agents/run.py",
             "symbols": ["get_default_agent_runner"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "run-context",
        "title": "运行上下文与用途约束",
        "page_file": "a03-run-context.md",
        "boundary": "负责 RunContextWrapper 携带的上下文对象、用量统计与「仅工具可用」的约束；"
                    "不负责工具调用（c03）、不负责会话（g01）。",
        "refs": [
            {"path": "src/agents/run_context.py",
             "symbols": ["RunContextWrapper", "AgentHookContext"]},
            {"path": "src/agents/tool_context.py", "symbols": ["ToolContext"]},
        ],
    },
    {
        "feature_group": "A-Agent 定义与运行入口",
        "feature": "run-state",
        "title": "运行状态与序列化恢复",
        "page_file": "a04-run-state.md",
        "boundary": "负责 RunState 如何承载一次运行的完整可续跑状态、如何序列化与反序列化；"
                    "不负责结果对象（j02）、不负责会话存储（g01）。",
        "refs": [
            {"path": "src/agents/run_state.py",
             "symbols": ["RunState", "_HeldCurrentResponse", "_PendingSessionWrite"]},
        ],
    },

    # ============ B 单轮执行循环 ============
    {
        "feature_group": "B-单轮执行循环",
        "feature": "turn-loop",
        "title": "单轮循环与最大轮次",
        "page_file": "b01-turn-loop.md",
        "boundary": "负责一轮「调用模型 → 处理输出项 → 决定是否继续」的推进方式，"
                    "含结束条件与最大轮次；不负责工具执行细节（c01）、不负责流式投影（j01）。",
        "refs": [
            {"path": "src/agents/run_internal/run_loop.py",
             "symbols": ["_prepare_turn_input_items", "_publish_streamed_result_agent"]},
            {"path": "src/agents/run_internal/agent_runner_helpers.py",
             "symbols": ["snapshot_usage", "usage_delta"]},
        ],
    },
    {
        "feature_group": "B-单轮执行循环",
        "feature": "run-config",
        "title": "运行配置与敏感数据开关",
        "page_file": "b02-run-config.md",
        "boundary": "负责 RunConfig 承载的运行级配置：模型、模型设置、追踪开关、"
                    "工具执行策略与沙箱限制；不负责 Agent 级配置（a01）、不负责追踪实现（i01）。",
        "refs": [
            {"path": "src/agents/run_config.py",
             "symbols": ["ModelInputData", "CallModelData", "ToolExecutionConfig"]},
            {"path": "src/agents/run_config.py",
             "symbols": ["_default_trace_include_sensitive_data", "_coerce_run_config"]},
        ],
    },
    {
        "feature_group": "B-单轮执行循环",
        "feature": "model-retry",
        "title": "模型调用重试与裁决合并",
        "page_file": "b03-model-retry.md",
        "boundary": "负责模型调用失败后的错误归一、重试裁决、退避与用量补偿记账；"
                    "不负责模型实现（f01）、不负责超时语义（b02）。",
        "refs": [
            {"path": "src/agents/retry.py",
             "symbols": ["ModelRetrySettings", "RetryDecision", "ModelRetryAdvice"]},
            {"path": "src/agents/retry.py",
             "symbols": ["_merge_positive_retry_decisions", "RetryPolicyContext"]},
            {"path": "src/agents/run_internal/model_retry.py",
             "symbols": ["_normalize_retry_error", "apply_retry_attempt_usage"]},
        ],
    },
    {
        "feature_group": "B-单轮执行循环",
        "feature": "lifecycle-hooks",
        "title": "生命周期钩子",
        "page_file": "b04-lifecycle-hooks.md",
        "boundary": "负责运行级与智能体级两组钩子的触发点与顺序；"
                    "不负责追踪事件（i02）、不负责流式事件（j01）。",
        "refs": [
            {"path": "src/agents/lifecycle.py",
             "symbols": ["RunHooksBase", "AgentHooksBase"]},
        ],
    },

    # ============ C 工具系统 ============
    {
        "feature_group": "C-工具系统",
        "feature": "tool-types",
        "title": "工具类型体系与输出形态",
        "page_file": "c01-tool-types.md",
        "boundary": "负责工具的抽象分类（函数 / 自定义 / 计算机 / 补丁）与结构化输出类型；"
                    "不负责函数工具的包装（c02）、不负责调用编排（c03）。",
        "refs": [
            {"path": "src/agents/tool.py",
             "symbols": ["ToolOutputText", "ToolOutputImage", "FunctionToolCustomDataContext"]},
            {"path": "src/agents/tool.py",
             "symbols": ["CustomToolCustomDataContext", "ComputerToolCustomDataContext",
                         "ApplyPatchToolCustomDataContext"]},
        ],
    },
    {
        "feature_group": "C-工具系统",
        "feature": "function-tool-schema",
        "title": "函数工具与 schema 生成",
        "page_file": "c02-function-tool-schema.md",
        "boundary": "负责把普通函数包装成工具：文档字符串风格识别、参数 schema 生成、"
                    "严格 schema 变换；不负责工具调用（c03）、不负责审批（c04）。",
        "refs": [
            {"path": "src/agents/function_schema.py",
             "symbols": ["function_schema", "generate_func_documentation", "FuncSchema"]},
            {"path": "src/agents/strict_schema.py",
             "symbols": ["ensure_strict_json_schema", "_ensure_strict_root"]},
        ],
    },
    {
        "feature_group": "C-工具系统",
        "feature": "tool-invocation",
        "title": "工具调用与参数解析",
        "page_file": "c03-tool-invocation.md",
        "boundary": "负责一次工具调用的完整路径：参数 JSON 解析、并发执行、错误归入结果；"
                    "不负责 schema 生成（c02）、不负责审批（c04）。",
        "refs": [
            {"path": "src/agents/tool.py",
             "symbols": ["with_function_tool_failure_error_handler", "get_function_tool_origin"]},
            {"path": "src/agents/util/_approvals.py",
             "symbols": ["parse_function_tool_arguments"]},
        ],
    },
    {
        "feature_group": "C-工具系统",
        "feature": "tool-approvals",
        "title": "工具审批与拒绝路径",
        "page_file": "c04-tool-approvals.md",
        "boundary": "负责需要人工批准的工具调用如何挂起、批准或拒绝后如何继续，"
                    "以及拒绝时产生什么输出；不负责调用执行（c03）、不负责护栏（d01）。",
        "refs": [
            {"path": "src/agents/run_internal/approvals.py",
             "symbols": ["filter_tool_approvals", "approvals_from_step",
                         "append_approval_error_output"]},
        ],
    },

    # ============ D 护栏 ============
    {
        "feature_group": "D-护栏",
        "feature": "input-output-guardrails",
        "title": "输入输出护栏与触发语义",
        "page_file": "d01-input-output-guardrails.md",
        "boundary": "负责输入护栏与输出护栏的定义、执行时机与触发后的行为差异；"
                    "不负责工具级护栏（d02）、不负责拒绝后的工具取消（d03）。",
        "refs": [
            {"path": "src/agents/guardrail.py",
             "symbols": ["InputGuardrail", "OutputGuardrail", "GuardrailFunctionOutput"]},
        ],
    },
    {
        "feature_group": "D-护栏",
        "feature": "tool-guardrails",
        "title": "工具级护栏与行为策略",
        "page_file": "d02-tool-guardrails.md",
        "boundary": "负责工具输入/输出护栏的数据结构与三种行为（拒绝内容、抛异常、放行）；"
                    "不负责输入输出护栏（d01）、不负责审批（c04）。",
        "refs": [
            {"path": "src/agents/tool_guardrails.py",
             "symbols": ["ToolInputGuardrailData", "ToolGuardrailFunctionOutput",
                         "RejectContentBehavior", "RaiseExceptionBehavior"]},
        ],
    },
    {
        "feature_group": "D-护栏",
        "feature": "guardrail-gating",
        "title": "护栏门控与并行任务取消",
        "page_file": "d03-guardrail-gating.md",
        "boundary": "负责输入护栏与工具并行执行的互斥门控，以及护栏触发时如何取消并发的模型或工具任务；"
                    "不负责护栏定义（d01）、不负责重试（b03）。",
        "refs": [
            {"path": "src/agents/run_internal/guardrails.py",
             "symbols": ["_InputGuardrailToolGate", "input_guardrail_tool_gate_scope",
                         "cancel_tools_on_input_guardrail_rejection"]},
        ],
    },

    # ============ E 交接 ============
    {
        "feature_group": "E-交接",
        "feature": "handoff-model",
        "title": "交接对象与输入改造",
        "page_file": "e01-handoff-model.md",
        "boundary": "负责 Handoff 的构成、交接时输入数据的整形与可选工具化；"
                    "不负责交接在循环里怎么被识别（e02）、不负责过滤扩展（e03）。",
        "refs": [
            {"path": "src/agents/handoffs/__init__.py",
             "symbols": ["Handoff", "HandoffInputData", "handoff"]},
        ],
    },
    {
        "feature_group": "E-交接",
        "feature": "handoff-execution",
        "title": "交接在循环中的识别与切换",
        "page_file": "e02-handoff-execution.md",
        "boundary": "负责循环如何识别交接调用、如何切换当前智能体并继续下一轮；"
                    "不负责交接对象构造（e01）、不负责结果对象（j02）。",
        "refs": [
            {"path": "src/agents/items.py",
             "symbols": ["HandoffCallItem", "HandoffOutputItem"]},
            {"path": "src/agents/run_internal/run_loop.py",
             "symbols": ["_publish_resumed_stream_turn", "_complete_stream_interruption"]},
        ],
    },
    {
        "feature_group": "E-交接",
        "feature": "handoff-filters",
        "title": "交接过滤器与提示扩展",
        "page_file": "e03-handoff-filters.md",
        "boundary": "负责交接时对历史项的裁剪与提示注入两类扩展；"
                    "不负责交接对象（e01）、不负责循环切换（e02）。",
        "refs": [
            {"path": "src/agents/extensions/handoff_filters.py",
             "symbols": ["remove_all_tools", "_remove_tool_types_from_input"]},
            {"path": "src/agents/extensions/handoff_prompt.py",
             "symbols": ["RECOMMENDED_PROMPT_PREFIX", "prompt_with_handoff_instructions"]},
        ],
    },
    {
        "feature_group": "E-交接",
        "feature": "agent-as-tool",
        "title": "智能体作为工具",
        "page_file": "e04-agent-as-tool.md",
        "boundary": "负责把一个智能体包装成工具供另一个智能体调用，含输入结构与运行结果登记；"
                    "不负责交接（e01）、不负责工具调用（c03）。",
        "refs": [
            {"path": "src/agents/agent_tool_input.py",
             "symbols": ["AgentAsToolInput", "build_structured_input_schema_info",
                         "default_tool_input_builder"]},
            {"path": "src/agents/agent_tool_state.py",
             "symbols": ["record_agent_tool_run_result", "get_agent_tool_state_scope"]},
        ],
    },

    # ============ F 模型层 ============
    {
        "feature_group": "F-模型层",
        "feature": "model-interface",
        "title": "模型接口与追踪模式",
        "page_file": "f01-model-interface.md",
        "boundary": "负责模型抽象的三要素：请求、流式响应、追踪开关；"
                    "不负责具体实现（f02、f03）、不负责提供方映射（f04）。",
        "refs": [
            {"path": "src/agents/models/interface.py",
             "symbols": ["Model", "ModelProvider", "ModelTracing"]},
        ],
    },
    {
        "feature_group": "F-模型层",
        "feature": "responses-model",
        "title": "Responses 模型实现",
        "page_file": "f02-responses-model.md",
        "boundary": "负责 Responses API 的模型实现：工具参数物化、流事件构造、用量提取与 WebSocket 选项；"
                    "不负责 Chat Completions（f03）、不负责接口契约（f01）。",
        "refs": [
            {"path": "src/agents/models/openai_responses.py",
             "symbols": ["OpenAIResponsesModel", "_materialize_responses_tool_params",
                         "_usage_from_response"]},
            {"path": "src/agents/models/openai_responses.py",
             "symbols": ["OpenAIResponsesWebSocketOptions", "_construct_response_stream_event_from_payload"]},
        ],
    },
    {
        "feature_group": "F-模型层",
        "feature": "chatcmpl-model",
        "title": "Chat Completions 实现与转换",
        "page_file": "f03-chatcmpl-model.md",
        "boundary": "负责 Chat Completions 的模型实现与到统一项模型的转换、流式增量拼接；"
                    "不负责 Responses（f02）、不负责接口（f01）。",
        "refs": [
            {"path": "src/agents/models/openai_chatcompletions.py",
             "symbols": ["OpenAIChatCompletionsModel"]},
            {"path": "src/agents/models/chatcmpl_converter.py",
             "symbols": ["Converter", "items_to_messages"]},
        ],
    },
    {
        "feature_group": "F-模型层",
        "feature": "multi-provider",
        "title": "多提供方映射",
        "page_file": "f04-multi-provider.md",
        "boundary": "负责按前缀或规则把模型名路由到不同提供方；"
                    "不负责模型实现（f02、f03）、不负责配置（b02）。",
        "refs": [
            {"path": "src/agents/models/multi_provider.py",
             "symbols": ["MultiProvider", "MultiProviderMap"]},
        ],
    },

    # ============ G 会话与记忆 ============
    {
        "feature_group": "G-会话与记忆",
        "feature": "session-interface",
        "title": "会话接口与压缩感知",
        "page_file": "g01-session-interface.md",
        "boundary": "负责会话的最小契约、压缩感知扩展接口与方法兼容性探测；"
                    "不负责具体后端（g02）、不负责运行状态的序列化（a04）。",
        "refs": [
            {"path": "src/agents/memory/session.py",
             "symbols": ["Session", "SessionABC", "is_openai_responses_compaction_aware_session"]},
        ],
    },
    {
        "feature_group": "G-会话与记忆",
        "feature": "sqlite-session",
        "title": "SQLite 会话实现",
        "page_file": "g02-sqlite-session.md",
        "boundary": "负责 SQLite 后端的表结构、追加与读取、并发与锁；"
                    "不负责会话契约（g01）、不负责服务端会话（g03）。",
        "refs": [
            {"path": "src/agents/memory/sqlite_session.py", "symbols": ["SQLiteSession"]},
        ],
    },
    {
        "feature_group": "G-会话与记忆",
        "feature": "server-conversation-tracking",
        "title": "服务端会话追踪",
        "page_file": "g03-server-conversation-tracking.md",
        "boundary": "负责跟踪服务端已持有的会话项，避免重复投递与错序确认；"
                    "不负责本地会话（g02）、不负责运行循环（b01）。",
        "refs": [
            {"path": "src/agents/run_internal/oai_conversation.py",
             "symbols": ["OpenAIServerConversationTracker", "_fingerprint_for_tracker"]},
        ],
    },

    # ============ H MCP 集成 ============
    {
        "feature_group": "H-MCP 集成",
        "feature": "mcp-server-connection",
        "title": "MCP 服务器连接与工具快照",
        "page_file": "h01-mcp-server-connection.md",
        "boundary": "负责 MCP 服务器连接的建立、工具快照、审批声明与传输错误处理；"
                    "不负责多服务器生命周期（h02）、不负责重试裁决（b03）。",
        "refs": [
            {"path": "src/agents/mcp/server.py",
             "symbols": ["RequireApprovalToolList", "_snapshot_tools"]},
            {"path": "src/agents/mcp/server.py",
             "symbols": ["_first_unsafe_transport_error", "_transport_error_urls_are_safe"]},
        ],
    },
    {
        "feature_group": "H-MCP 集成",
        "feature": "mcp-manager",
        "title": "多服务器管理与工作进程",
        "page_file": "h02-mcp-manager.md",
        "boundary": "负责多个 MCP 服务器的启动、健康检查与关闭顺序；"
                    "不负责单个连接（h01）、不负责工具调用（c03）。",
        "refs": [
            {"path": "src/agents/mcp/manager.py",
             "symbols": ["MCPServerManager", "_ServerWorker", "_ServerCommand"]},
        ],
    },

    # ============ I 追踪与可观测 ============
    {
        "feature_group": "I-追踪与可观测",
        "feature": "tracing-spans",
        "title": "追踪跨度与错误记录",
        "page_file": "i01-tracing-spans.md",
        "boundary": "负责跨度的生命周期、数据载荷与错误附着；"
                    "不负责处理器（i02）、不负责作用域（i03）。",
        "refs": [
            {"path": "src/agents/tracing/spans.py",
             "symbols": ["Span", "SpanImpl", "NoOpSpan", "SpanError"]},
        ],
    },
    {
        "feature_group": "I-追踪与可观测",
        "feature": "tracing-processors",
        "title": "追踪处理器与导出",
        "page_file": "i02-tracing-processors.md",
        "boundary": "负责追踪处理器的注册、批量与导出接口；"
                    "不负责跨度结构（i01）、不负责作用域传播（i03）。",
        "refs": [
            {"path": "src/agents/tracing/processor_interface.py",
             "symbols": ["TracingProcessor", "TracingExporter"]},
        ],
    },
    {
        "feature_group": "I-追踪与可观测",
        "feature": "tracing-scope",
        "title": "追踪作用域与上下文传播",
        "page_file": "i03-tracing-scope.md",
        "boundary": "负责当前跨度在调用链中的传播与作用域嵌套；"
                    "不负责跨度实现（i01）、不负责导出（i02）。",
        "refs": [
            {"path": "src/agents/tracing/scope.py", "symbols": ["Scope"]},
            {"path": "src/agents/tracing/create.py", "symbols": ["agent_span", "trace"]},
        ],
    },

    # ============ J 流式与结果 ============
    {
        "feature_group": "J-流式与结果",
        "feature": "streaming-events",
        "title": "流式事件与三层事件源",
        "page_file": "j01-streaming-events.md",
        "boundary": "负责三种流式事件的区分（原始模型事件、运行项事件、智能体切换）与投影顺序；"
                    "不负责结果对象（j02）、不负责追踪（i01）。",
        "refs": [
            {"path": "src/agents/stream_events.py",
             "symbols": ["RawResponsesStreamEvent", "RunItemStreamEvent", "AgentUpdatedStreamEvent"]},
            {"path": "src/agents/run_internal/run_loop.py",
             "symbols": ["_stream_event_item_occurrence_key", "_ensure_stream_event_item_occurrence_key"]},
        ],
    },
    {
        "feature_group": "J-流式与结果",
        "feature": "result-objects",
        "title": "结果对象与项模型",
        "page_file": "j02-result-objects.md",
        "boundary": "负责 RunResult 与 RunResultStreaming 的字段构成、输入项与输出项的转换；"
                    "不负责流式事件（j01）、不负责运行状态（a04）。",
        "refs": [
            {"path": "src/agents/result.py",
             "symbols": ["RunResult", "RunResultStreaming", "RunResultBase"]},
            {"path": "src/agents/items.py",
             "symbols": ["RunItemBase", "MessageOutputItem", "ToolCallItem"]},
        ],
    },
    {
        "feature_group": "J-流式与结果",
        "feature": "structured-output",
        "title": "结构化输出与校验",
        "page_file": "j03-structured-output.md",
        "boundary": "负责智能体输出 schema 的构造、严格模式校验与解析失败的处理；"
                    "不负责工具 schema（c02）、不负责结果对象（j02）。",
        "refs": [
            {"path": "src/agents/agent_output.py",
             "symbols": ["AgentOutputSchema", "AgentOutputSchemaBase"]},
        ],
    },
]
