# -*- coding: utf-8 -*-
"""crewai 机制清单（阶段 2A 第三个 AI / Agent 项目）。

盘点范围：`lib/crewai/src/crewai/` 主包。同仓的 `crewai-core`、`crewai-tools`、
`crewai-files`、`cli` 各有独立包边界：`crewai-tools` 是上百个第三方工具适配
（按机制归类属外部集成层），`crewai-files` 是文件上传/格式化，`cli` 是脚手架与
部署，`crewai-core` 只承载认证提供方。它们不在本清单内，留作后续增量。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按包内目录骨架
归并成机制簇。篇数是归并结果，不是按文件数折算。

源码锚点：`crewai@77237aecacb204684a80e334dcd8eefa90d7ee60`。
"""

GROUP_ORDER = [
    "A-团队与流程",
    "B-任务与条件执行",
    "C-智能体执行循环",
    "D-Flow 编排",
    "E-记忆系统",
    "F-工具系统",
    "G-MCP 与技能",
    "H-事件与钩子",
    "I-模型层与安全",
    "J-运行时状态与检查点",
]

MECHANISM_SPECS = [
    # ============ A 团队与流程 ============
    {
        "feature_group": "A-团队与流程",
        "feature": "crew-object",
        "title": "Crew 对象与配置校验",
        "page_file": "a01-crew-object.md",
        "boundary": "负责 Crew 的构成与建期校验：智能体与任务的装配、流程类型的约束、"
                    "以及各类互斥规则；不负责执行推进（a02）、不负责任务内部逻辑（b01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["Crew", "check_config", "validate_tasks"]},
        ],
    },
    {
        "feature_group": "A-团队与流程",
        "feature": "sequential-and-hierarchical",
        "title": "顺序与层级两种流程",
        "page_file": "a02-sequential-and-hierarchical.md",
        "boundary": "负责 kickoff 如何按流程类型分派，以及层级流程如何创建管理智能体并注入委派工具；"
                    "不负责任务执行细节（b01）、不负责异步任务排队（a03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["kickoff", "_arun_sequential_process", "_arun_hierarchical_process"]},
            {"path": "lib/crewai/src/crewai/process.py", "symbols": ["Process"]},
        ],
    },
    {
        "feature_group": "A-团队与流程",
        "feature": "task-loop-and-async",
        "title": "任务循环与异步任务排队",
        "page_file": "a03-task-loop-and-async.md",
        "boundary": "负责遍历任务的执行循环：跳过的判定、异步任务的入队与汇合时机、上下文累积；"
                    "不负责单个任务怎么跑（b01）、不负责条件任务（b02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["_execute_tasks", "_process_async_tasks", "_get_context"]},
        ],
    },
    {
        "feature_group": "A-团队与流程",
        "feature": "replay-and-checkpoint",
        "title": "重放与执行起点定位",
        "page_file": "a04-replay-and-checkpoint.md",
        "boundary": "负责从已有输出推断重跑起点、重放时的日志标记与跳过逻辑；"
                    "不负责运行时状态快照（j01）、不负责任务重试（b03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["replay", "_get_execution_start_index", "_validate_replay_tasks"]},
        ],
    },

    # ============ B 任务与条件执行 ============
    {
        "feature_group": "B-任务与条件执行",
        "feature": "task-execution-core",
        "title": "Task 执行内核",
        "page_file": "b01-task-execution-core.md",
        "boundary": "负责一个任务从输入插值到产出结果的执行路径，含同步与异步两条；"
                    "不负责团队循环（a03）、不负责条件跳过（b02）、不负责护栏（b04）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/task.py",
             "symbols": ["Task", "execute_sync", "_execute_core"]},
            {"path": "lib/crewai/src/crewai/task.py",
             "symbols": ["execute_async", "aexecute_sync", "_aexecute_core"]},
        ],
    },
    {
        "feature_group": "B-任务与条件执行",
        "feature": "conditional-task",
        "title": "条件任务的跳过判定",
        "page_file": "b02-conditional-task.md",
        "boundary": "负责条件任务如何依据前序输出决定跳过，以及跳过时产生什么样的占位输出；"
                    "不负责普通任务执行（b01）、不负责重试（b03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["_handle_conditional_task", "_ahandle_conditional_task"]},
        ],
    },
    {
        "feature_group": "B-任务与条件执行",
        "feature": "guardrail",
        "title": "护栏与重试上限",
        "page_file": "b03-guardrail.md",
        "boundary": "负责任务护栏的校验时机、失败后的重试与最终放弃，以及护栏的可调用性约束；"
                    "不负责工具失败策略（f05）、不负责条件跳过（b02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/task.py",
             "symbols": ["validate_guardrail_function", "ensure_guardrail_is_callable"]},
        ],
    },
    {
        "feature_group": "B-任务与条件执行",
        "feature": "task-output-handling",
        "title": "任务输出与结构化导出",
        "page_file": "b04-task-output-handling.md",
        "boundary": "负责把任务结果归一成统一输出对象，含 JSON/Pydantic 结构化、文件落盘与"
                    "结果存储；不负责团队级聚合（a03）、不负责执行内核（b01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/task.py",
             "symbols": ["check_output", "_validate_output_file_path", "artifact_kind_validation"]},
            {"path": "lib/crewai/src/crewai/utilities/task_output_storage_handler.py",
             "symbols": ["TaskOutputStorageHandler", "update", "load"]},
        ],
    },

    # ============ C 智能体执行循环 ============
    {
        "feature_group": "C-智能体执行循环",
        "feature": "agent-definition",
        "title": "Agent 定义与属性校验",
        "page_file": "c01-agent-definition.md",
        "boundary": "负责智能体的构造期装配：角色化字段、工具与 MCP 校验、记忆解析、"
                    "执行器准备；不负责执行循环（c03）、不负责委派（c02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/agent/core.py",
             "symbols": ["Agent", "validate_from_repository", "post_init_setup"]},
            {"path": "lib/crewai/src/crewai/agents/agent_builder/base_agent.py",
             "symbols": ["BaseAgent", "execute_task"]},
        ],
    },
    {
        "feature_group": "C-智能体执行循环",
        "feature": "delegation",
        "title": "委派工具注入与层级协作",
        "page_file": "c02-delegation.md",
        "boundary": "负责按委派开关注入委派工具、层级流程下管理智能体的工具集，"
                    "以及工具合并规则；不负责执行循环（c03）、不负责 MCP 工具（g01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["_prepare_tools", "_add_delegation_tools", "_update_manager_tools"]},
            {"path": "lib/crewai/src/crewai/crew.py",
             "symbols": ["_create_manager_agent", "_get_agent_to_use"]},
        ],
    },
    {
        "feature_group": "C-智能体执行循环",
        "feature": "agent-executor-loop",
        "title": "执行器循环与工具调用",
        "page_file": "c03-agent-executor-loop.md",
        "boundary": "负责智能体的「思考—调用工具—观察」循环：最大迭代、重复调用检测、"
                    "结果格式化与最终答案收敛；不负责工具实现（f01）、不负责任务层（b01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/utilities/agent_utils.py",
             "symbols": ["get_llm_response", "process_llm_response", "handle_agent_action_core"]},
            {"path": "lib/crewai/src/crewai/utilities/agent_utils.py",
             "symbols": ["has_reached_max_iterations", "handle_max_iterations_exceeded", "format_answer"]},
        ],
    },
    {
        "feature_group": "C-智能体执行循环",
        "feature": "context-summarization",
        "title": "上下文压缩与空回复重试",
        "page_file": "c04-context-summarization.md",
        "boundary": "负责在长上下文中主动摘要、以及模型返回空内容时的重试策略；"
                    "不负责主循环（c03）、不负责记忆检索（e03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/utilities/agent_utils.py",
             "symbols": ["_summarize_context_before_llm_call", "_proactively_summarize_context",
                         "_call_llm_retrying_empty"]},
        ],
    },

    # ============ D-Flow 编排 ============
    {
        "feature_group": "D-Flow 编排",
        "feature": "flow-decorators",
        "title": "Flow 装饰器与触发条件",
        "page_file": "d01-flow-decorators.md",
        "boundary": "负责 @start / @listen / @router 如何把方法登记成带条件与触发事件的编排单元，"
                    "以及 or_ / and_ 条件组合；不负责执行引擎（d03）、不负责表达式求值（d04）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/dsl/_start.py", "symbols": ["start"]},
            {"path": "lib/crewai/src/crewai/flow/dsl/_listen.py", "symbols": ["listen"]},
            {"path": "lib/crewai/src/crewai/flow/dsl/_router.py", "symbols": ["router"]},
            {"path": "lib/crewai/src/crewai/flow/dsl/_conditions.py", "symbols": ["or_", "and_"]},
        ],
    },
    {
        "feature_group": "D-Flow 编排",
        "feature": "flow-definition",
        "title": "Flow 定义契约与状态模型",
        "page_file": "d02-flow-definition.md",
        "boundary": "负责可序列化的 Flow 定义结构：方法定义、状态定义、配置与持久化声明；"
                    "不负责运行期执行（d03）、不负责装饰器登记（d01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/flow_definition.py",
             "symbols": ["FlowDefinition", "FlowMethodDefinition", "FlowConfigDefinition"]},
        ],
    },
    {
        "feature_group": "D-Flow 编排",
        "feature": "flow-runtime",
        "title": "Flow 运行时与监听器执行",
        "page_file": "d03-flow-runtime.md",
        "boundary": "负责 Flow 的 kickoff 与事件驱动推进：起点方法、监听器触发、竞态分组、"
                    "以及状态在方法间的传递；不负责定义结构（d02）、不负责人工反馈（d05）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/runtime/__init__.py",
             "symbols": ["Flow", "kickoff", "_execute_listeners"]},
            {"path": "lib/crewai/src/crewai/flow/runtime/__init__.py",
             "symbols": ["_execute_start_method", "_find_triggered_methods", "_execute_racing_listeners"]},
        ],
    },
    {
        "feature_group": "D-Flow 编排",
        "feature": "flow-expressions",
        "title": "Flow 表达式与模板渲染",
        "page_file": "d04-flow-expressions.md",
        "boundary": "负责 Flow 内的表达式求值与模板插值：CEL 片段、变量解析与安全约束；"
                    "不负责触发条件（d01）、不负责执行引擎（d03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/expressions.py",
             "symbols": ["Expression", "evaluate", "render_template"]},
        ],
    },
    {
        "feature_group": "D-Flow 编排",
        "feature": "human-feedback",
        "title": "人工反馈与恢复",
        "page_file": "d05-human-feedback.md",
        "boundary": "负责 Flow 的人工介入：请求反馈、暂停等待、带反馈恢复，以及反馈结果折叠；"
                    "不负责持久化后端（d06）、不负责定义（d02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/human_feedback.py",
             "symbols": ["human_feedback", "HumanFeedbackConfig", "HumanFeedbackResult"]},
            {"path": "lib/crewai/src/crewai/flow/runtime/__init__.py",
             "symbols": ["resume", "pending_feedback", "from_pending"]},
        ],
    },
    {
        "feature_group": "D-Flow 编排",
        "feature": "flow-persistence",
        "title": "Flow 持久化与恢复",
        "page_file": "d06-flow-persistence.md",
        "boundary": "负责 Flow 状态的持久化后端与恢复入口：注册、写入时机与执行数据重建；"
                    "不负责运行时推进（d03）、不负责检查点配置（j02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/flow/persistence/decorators.py", "symbols": ["persist"]},
            {"path": "lib/crewai/src/crewai/flow/persistence/sqlite.py",
             "symbols": ["SQLiteFlowPersistence"]},
            {"path": "lib/crewai/src/crewai/flow/runtime/__init__.py",
             "symbols": ["install_persistence", "reload", "_persist_method_completion"]},
        ],
    },

    # ============ E 记忆系统 ============
    {
        "feature_group": "E-记忆系统",
        "feature": "unified-memory",
        "title": "统一记忆入口",
        "page_file": "e01-unified-memory.md",
        "boundary": "负责记忆门面的四个动作（记住、回忆、忘记、更新）与后台写入排队；"
                    "不负责作用域隔离（e02）、不负责编码流程（e04）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/memory/unified_memory.py",
             "symbols": ["Memory", "remember", "recall"]},
            {"path": "lib/crewai/src/crewai/memory/unified_memory.py",
             "symbols": ["_submit_save", "drain_writes", "forget"]},
        ],
    },
    {
        "feature_group": "E-记忆系统",
        "feature": "memory-scope",
        "title": "记忆作用域与隔离",
        "page_file": "e02-memory-scope.md",
        "boundary": "负责按作用域路径隔离记忆、只读视图与子作用域派生；"
                    "不负责记忆门面（e01）、不负责检索流程（e03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/memory/memory_scope.py",
             "symbols": ["MemoryScope", "MemorySlice", "subscope"]},
        ],
    },
    {
        "feature_group": "E-记忆系统",
        "feature": "recall-flow",
        "title": "记忆检索流程",
        "page_file": "e03-recall-flow.md",
        "boundary": "负责检索的递归流程：查询分析、深度判定、分块搜索与再检索收敛；"
                    "不负责写入编码（e04）、不负责作用域（e02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/memory/recall_flow.py",
             "symbols": ["RecallFlow", "analyze_query_step", "recursive_exploration"]},
        ],
    },
    {
        "feature_group": "E-记忆系统",
        "feature": "encoding-flow",
        "title": "记忆编码与去重",
        "page_file": "e04-encoding-flow.md",
        "boundary": "负责把原始内容编码成可存储记忆：批量嵌入、批内去重、相似查找与计划执行；"
                    "不负责检索（e03）、不负责知识库（e05）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/memory/encoding_flow.py",
             "symbols": ["EncodingFlow", "batch_embed", "intra_batch_dedup"]},
        ],
    },
    {
        "feature_group": "E-记忆系统",
        "feature": "knowledge",
        "title": "知识库与来源管理",
        "page_file": "e05-knowledge.md",
        "boundary": "负责团队级知识库：来源解析、添加与查询、重置；"
                    "不负责个体记忆（e01）、不负责 RAG 工厂（e06）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/knowledge/knowledge.py",
             "symbols": ["Knowledge", "query", "add_sources"]},
        ],
    },
    {
        "feature_group": "E-记忆系统",
        "feature": "rag-factory",
        "title": "RAG 存储工厂",
        "page_file": "e06-rag-factory.md",
        "boundary": "负责按配置创建向量存储与检索器，以及支持的后端类型；"
                    "不负责知识库门面（e05）、不负责文档加载（属 crewai-tools）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/rag/factory.py",
             "symbols": ["create_client", "register_rag_client_factory"]},
        ],
    },

    # ============ F 工具系统 ============
    {
        "feature_group": "F-工具系统",
        "feature": "base-tool",
        "title": "工具基类与参数模式",
        "page_file": "f01-base-tool.md",
        "boundary": "负责工具的统一契约：参数与结果模式、序列化、使用次数配额；"
                    "不负责结构化包装（f02）、不负责调用编排（f03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/base_tool.py",
             "symbols": ["BaseTool", "Tool", "_serialize_args_schema"]},
        ],
    },
    {
        "feature_group": "F-工具系统",
        "feature": "structured-tool",
        "title": "结构化工具与描述生成",
        "page_file": "f02-structured-tool.md",
        "boundary": "负责把函数包装成带模式描述的工具：签名校验、描述格式化、输出规整；"
                    "不负责基类契约（f01）、不负责调用循环（f03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/structured_tool.py",
             "symbols": ["CrewStructuredTool", "from_function", "format_description_for_llm"]},
        ],
    },
    {
        "feature_group": "F-工具系统",
        "feature": "tool-usage",
        "title": "工具调用编排与重复检测",
        "page_file": "f03-tool-usage.md",
        "boundary": "负责一次工具调用的完整路径：解析模型给出的调用、选择工具、执行、"
                    "格式化结果并检测重复使用；不负责工具实现（f01）、不负责失败策略（f05）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/tool_usage.py",
             "symbols": ["ToolUsage", "use", "_check_tool_repeated_usage"]},
        ],
    },
    {
        "feature_group": "F-工具系统",
        "feature": "tool-calling-schema",
        "title": "函数调用式工具协议",
        "page_file": "f04-tool-calling-schema.md",
        "boundary": "负责把工具集转成模型可用的函数调用协议（含参数 schema 与调用载荷）；"
                    "不负责执行编排（f03）、不负责 MCP 工具（g01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/tool_calling.py",
             "symbols": ["ToolCalling", "InstructorToolCalling"]},
            {"path": "lib/crewai/src/crewai/utilities/agent_utils.py",
             "symbols": ["convert_tools_to_openai_schema", "render_text_description_and_args"]},
        ],
    },
    {
        "feature_group": "F-工具系统",
        "feature": "tool-failure",
        "title": "工具失败分类与策略",
        "page_file": "f05-tool-failure.md",
        "boundary": "负责工具失败的分类、策略裁决与失败记录聚合；"
                    "不负责调用编排（f03）、不负责护栏（b03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/tool_failure.py",
             "symbols": ["ToolFailureReason", "detect_tool_failure", "resolve_tool_failure_policy"]},
        ],
    },

    # ============ G MCP 与技能 ============
    {
        "feature_group": "G-MCP 与技能",
        "feature": "mcp-client",
        "title": "MCP 客户端与连接生命周期",
        "page_file": "g01-mcp-client.md",
        "boundary": "负责 MCP 服务器的连接建立、工具列举与调用、断连清理；"
                    "不负责把 MCP 工具暴露成智能体工具（g02）、不负责工具基类（f01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/mcp/client.py",
             "symbols": ["MCPClient", "connect", "call_tool"]},
        ],
    },
    {
        "feature_group": "G-MCP 与技能",
        "feature": "mcp-native-tool",
        "title": "MCP 工具的原生暴露",
        "page_file": "g02-mcp-native-tool.md",
        "boundary": "负责把 MCP 工具包装成智能体可直接调用的原生工具，含参数清洗与结果映射；"
                    "不负责连接管理（g01）、不负责团队注入（a02）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/tools/mcp_native_tool.py", "symbols": ["MCPNativeTool"]},
            {"path": "lib/crewai/src/crewai/crew.py", "symbols": ["_add_mcp_tools", "_inject_mcp_tools"]},
        ],
    },
    {
        "feature_group": "G-MCP 与技能",
        "feature": "skills",
        "title": "技能发现与加载",
        "page_file": "g03-skills.md",
        "boundary": "负责技能的发现、注册引用与按需加载成上下文；"
                    "不负责 MCP（g01）、不负责工具执行（f03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/skills/loader.py",
             "symbols": ["discover_skills", "load_skill", "format_skill_context"]},
            {"path": "lib/crewai/src/crewai/skills/tool.py",
             "symbols": ["LoadSkillTool", "create_skill_loader_tool"]},
        ],
    },

    # ============ H 事件与钩子 ============
    {
        "feature_group": "H-事件与钩子",
        "feature": "event-bus",
        "title": "事件总线与处理器登记",
        "page_file": "h01-event-bus.md",
        "boundary": "负责事件总线的单例与运行时作用域、处理器登记与分发线程模型；"
                    "不负责事件上下文（h02）、不负责钩子（h03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/events/event_bus.py",
             "symbols": ["CrewAIEventsBus", "on", "set_runtime_state"]},
        ],
    },
    {
        "feature_group": "H-事件与钩子",
        "feature": "event-context",
        "title": "事件上下文与因果链",
        "page_file": "h02-event-context.md",
        "boundary": "负责事件的父子关系、触发来源与作用域栈，使事件链可追溯；"
                    "不负责总线与分发（h01）、不负责事件记录（j03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/events/event_context.py",
             "symbols": ["push_event_scope", "get_triggering_event_id", "triggered_by_scope"]},
        ],
    },
    {
        "feature_group": "H-事件与钩子",
        "feature": "hooks",
        "title": "工具与模型调用钩子",
        "page_file": "h03-hooks.md",
        "boundary": "负责工具调用与模型调用前后的钩子注册、执行与中止语义；"
                    "不负责事件总线（h01）、不负责工具实现（f01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/hooks/dispatch.py",
             "symbols": ["register", "InterceptionPoint", "HookAborted"]},
            {"path": "lib/crewai/src/crewai/hooks/tool_hooks.py",
             "symbols": ["run_before_tool_call_hooks", "register_before_tool_call_hook"]},
            {"path": "lib/crewai/src/crewai/hooks/llm_hooks.py",
             "symbols": ["before_llm_call_reducer", "LLMCallHookContext", "register_before_llm_call_hook"]},
        ],
    },

    # ============ I 模型层与安全 ============
    {
        "feature_group": "I-模型层与安全",
        "feature": "base-llm",
        "title": "模型基类与调用包装",
        "page_file": "i01-base-llm.md",
        "boundary": "负责模型基类：调用包装与上下文窗口校验、流式开关、配置序列化；"
                    "不负责模型工厂（i02）、不负责安全指纹（i03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/llms/base_llm.py",
             "symbols": ["BaseLLM", "_wrap_call_method", "to_config_dict"]},
        ],
    },
    {
        "feature_group": "I-模型层与安全",
        "feature": "llm-factory",
        "title": "模型创建与环境回退",
        "page_file": "i02-llm-factory.md",
        "boundary": "负责按名称或配置创建模型实例、从已有实例派生同型实例、"
                    "以及环境变量回退；不负责模型基类契约（i01）、不负责安全（i03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/utilities/llm_utils.py",
             "symbols": ["create_llm", "create_llm_like", "_llm_via_environment_or_fallback"]},
        ],
    },
    {
        "feature_group": "I-模型层与安全",
        "feature": "security-fingerprint",
        "title": "安全配置与指纹",
        "page_file": "i03-security-fingerprint.md",
        "boundary": "负责安全配置项与代码指纹的生成与校验；"
                    "不负责模型创建（i02）、不负责执行上下文（j04）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/security/security_config.py", "symbols": ["SecurityConfig"]},
            {"path": "lib/crewai/src/crewai/security/fingerprint.py", "symbols": ["Fingerprint"]},
        ],
    },

    # ============ J 运行时状态与检查点 ============
    {
        "feature_group": "J-运行时状态与检查点",
        "feature": "runtime-state",
        "title": "运行时状态与序列化",
        "page_file": "j01-runtime-state.md",
        "boundary": "负责运行时状态的字段构成、序列化/反序列化与谱系链接；"
                    "不负责触发时机（j02）、不负责事件记录（j03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/state/runtime.py",
             "symbols": ["RuntimeState", "_serialize", "_deserialize"]},
        ],
    },
    {
        "feature_group": "J-运行时状态与检查点",
        "feature": "checkpoint-triggers",
        "title": "检查点配置与触发",
        "page_file": "j02-checkpoint-triggers.md",
        "boundary": "负责检查点的触发配置、监听哪些事件、以及写入流程的编排；"
                    "不负责状态结构（j01）、不负责事件记录图（j03）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/state/checkpoint_config.py",
             "symbols": ["CheckpointConfig", "trigger_events", "apply_checkpoint"]},
            {"path": "lib/crewai/src/crewai/state/checkpoint_listener.py",
             "symbols": ["_on_any_event", "_should_checkpoint", "_do_checkpoint"]},
        ],
    },
    {
        "feature_group": "J-运行时状态与检查点",
        "feature": "event-record-graph",
        "title": "事件记录图与因果重建",
        "page_file": "j03-event-record-graph.md",
        "boundary": "负责把事件按父子关系组织成可查询的图，支持根集合、后代遍历与重建；"
                    "不负责事件总线（h01）、不负责状态对象（j01）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/state/event_record.py",
             "symbols": ["EventRecord", "EventNode", "descendants"]},
        ],
    },
    {
        "feature_group": "J-运行时状态与检查点",
        "feature": "execution-context",
        "title": "执行上下文与追踪标识",
        "page_file": "j04-execution-context.md",
        "boundary": "负责执行 UUID、平台令牌与当前任务 id 的传播，以及上下文的捕获与施加；"
                    "不负责事件作用域（h02）、不负责追踪上报细节（属 telemetry）。",
        "refs": [
            {"path": "lib/crewai/src/crewai/context.py",
             "symbols": ["ExecutionContext", "capture_execution_context", "apply_execution_context"]},
            {"path": "lib/crewai/src/crewai/execution.py",
             "symbols": ["ExecutionTrace", "begin_execution", "end_execution"]},
        ],
    },
]
