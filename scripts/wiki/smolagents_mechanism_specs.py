# -*- coding: utf-8 -*-
"""smolagents 机制清单（阶段 2B 第一个 AI / Agent 项目：工具使用类）。

盘点范围：`src/smolagents/` 单包（仅 18 个模块，是一个刻意做小的框架）。
它与其他 agent 框架最大的差别是 **以代码为动作**：CodeAgent 不用 JSON 表达工具调用，
而是让模型生成 Python 代码、在受限解释器里执行。这个选择把「工具调用协议」
换成了「代码执行沙箱」，也把安全边界从「参数校验」变成了「解释器能力裁剪」。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按模块职责归并
成机制簇。篇数是归并结果，不是按文件数折算。

源码锚点：`smolagents@474bb3314bb6b6e18d8a743f5f6582c06dd3b6e2`。
"""

GROUP_ORDER = [
    "A-智能体骨架",
    "B-代码执行沙箱",
    "C-工具定义",
    "D-工具启用与校验",
    "E-模型层",
    "F-记忆与步骤",
    "G-远程执行",
    "H-MCP 与序列化",
    "I-错误与可观测",
]

MECHANISM_SPECS = [
    # ============ A 智能体骨架 ============
    {
        "feature_group": "A-智能体骨架",
        "feature": "multi-step-agent",
        "title": "多步智能体主循环",
        "page_file": "a01-multi-step-agent.md",
        "boundary": "负责一步的构成（规划、行动、观察、收尾）、最大步数与中断；"
                    "不负责代码生成（a03）、不负责记忆存储（f01）。",
        "refs": [
            {"path": "src/smolagents/agents.py",
             "symbols": ["MultiStepAgent", "_step_stream", "_handle_max_steps_reached"]},
            {"path": "src/smolagents/agents.py",
             "symbols": ["run", "_run_stream", "interrupt"]},
        ],
    },
    {
        "feature_group": "A-智能体骨架",
        "feature": "tool-calling-agent",
        "title": "工具调用式智能体",
        "page_file": "a02-tool-calling-agent.md",
        "boundary": "负责 ToolCallingAgent 如何把工具描述交给模型并按 JSON 工具调用执行；"
                    "不负责代码式智能体（a03）、不负责工具定义（c01）。",
        "refs": [
            {"path": "src/smolagents/agents.py",
             "symbols": ["ToolCallingAgent", "extract_action"]},
        ],
    },
    {
        "feature_group": "A-智能体骨架",
        "feature": "code-agent",
        "title": "代码式智能体",
        "page_file": "a03-code-agent.md",
        "boundary": "负责 CodeAgent 如何让模型生成代码作为动作、如何抽出代码块并执行；"
                    "不负责解释器实现（b01）、不负责工具调用式路径（a02）。",
        "refs": [
            {"path": "src/smolagents/agents.py", "symbols": ["CodeAgent"]},
            {"path": "src/smolagents/utils.py",
             "symbols": ["parse_code_blobs", "extract_code_from_text"]},
        ],
    },
    {
        "feature_group": "A-智能体骨架",
        "feature": "managed-agents",
        "title": "被管理的子智能体",
        "page_file": "a04-managed-agents.md",
        "boundary": "负责把一个智能体包装成可被上级调用的工具，含命名与校验；"
                    "不负责主循环（a01）、不负责工具集（c03）。",
        "refs": [
            {"path": "src/smolagents/agents.py",
             "symbols": ["_setup_managed_agents", "_validate_tools_and_managed_agents"]},
            {"path": "src/smolagents/agents.py", "symbols": ["ManagedAgentPromptTemplate"]},
        ],
    },

    # ============ B 代码执行沙箱 ============
    {
        "feature_group": "B-代码执行沙箱",
        "feature": "local-python-executor",
        "title": "本地 Python 解释器与能力裁剪",
        "page_file": "b01-local-python-executor.md",
        "boundary": "负责本地解释器如何遍历 AST 求值、如何裁剪可用内置与导入、以及超时；"
                    "不负责远程执行（g01）、不负责代码抽取（a03）。",
        "refs": [
            {"path": "src/smolagents/local_python_executor.py",
             "symbols": ["LocalPythonExecutor", "evaluate_attribute", "check_import_authorized"]},
            {"path": "src/smolagents/local_python_executor.py",
             "symbols": ["InterpreterError", "ExecutionTimeoutError", "timeout"]},
        ],
    },
    {
        "feature_group": "B-代码执行沙箱",
        "feature": "executor-operations",
        "title": "解释器支持的语句与表达式",
        "page_file": "b02-executor-operations.md",
        "boundary": "负责解释器逐个实现的语句与表达式种类（赋值、循环、函数定义、类定义、"
                    "lambda、属性访问等）以及各自的限制；不负责导入裁剪（b01）、不负责远程（g01）。",
        "refs": [
            {"path": "src/smolagents/local_python_executor.py",
             "symbols": ["evaluate_while", "evaluate_lambda", "evaluate_class_def"]},
            {"path": "src/smolagents/local_python_executor.py",
             "symbols": ["evaluate_augassign", "create_function", "evaluate_boolop"]},
        ],
    },
    {
        "feature_group": "B-代码执行沙箱",
        "feature": "final-answer-mechanism",
        "title": "最终答案的表达与提取",
        "page_file": "b03-final-answer-mechanism.md",
        "boundary": "负责「如何在代码里表达最终答案」这一协议及其修复与校验；"
                    "不负责解释器求值（b01）、不负责智能体收尾（a01）。",
        "refs": [
            {"path": "src/smolagents/local_python_executor.py",
             "symbols": ["FinalAnswerException", "fix_final_answer_code", "CodeOutput"]},
            {"path": "src/smolagents/agents.py",
             "symbols": ["_validate_final_answer", "provide_final_answer"]},
        ],
    },

    # ============ C 工具定义 ============
    {
        "feature_group": "C-工具定义",
        "feature": "base-tool",
        "title": "工具基类与两种提示形态",
        "page_file": "c01-base-tool.md",
        "boundary": "负责工具基类的契约与它对外提供的两种描述形态（代码提示 / 工具调用提示）；"
                    "不负责工具集合（c03）、不负责参数校验（d01）。",
        "refs": [
            {"path": "src/smolagents/tools.py",
             "symbols": ["BaseTool", "Tool", "to_code_prompt", "to_tool_calling_prompt"]},
        ],
    },
    {
        "feature_group": "C-工具定义",
        "feature": "tool-source-handling",
        "title": "工具源码与序列化往返",
        "page_file": "c02-tool-source-handling.md",
        "boundary": "负责工具的源码形态、保存与从源码/Hub 重建，以及与第三方工具包装的互操作；"
                    "不负责工具基类契约（c01）、不负责校验（d02）。",
        "refs": [
            {"path": "src/smolagents/tools.py",
             "symbols": ["to_dict", "from_dict", "from_code"]},
            {"path": "src/smolagents/tools.py",
             "symbols": ["from_hub", "from_langchain"]},
        ],
    },
    {
        "feature_group": "C-工具定义",
        "feature": "tool-collection",
        "title": "工具集合与流水线工具",
        "page_file": "c03-tool-collection.md",
        "boundary": "负责把多个工具聚成集合、以及把工具串成流水线；"
                    "不负责单个工具（c01）、不负责默认工具实现（c04）。",
        "refs": [
            {"path": "src/smolagents/tools.py",
             "symbols": ["ToolCollection", "PipelineTool"]},
        ],
    },
    {
        "feature_group": "C-工具定义",
        "feature": "default-tools",
        "title": "内置工具清单",
        "page_file": "c04-default-tools.md",
        "boundary": "负责内置工具（搜索、网页访问、Python 解释器、用户输入、最终答案）各自做什么；"
                    "不负责工具基类（c01）、不负责校验（d02）。",
        "refs": [
            {"path": "src/smolagents/default_tools.py",
             "symbols": ["PythonInterpreterTool", "FinalAnswerTool", "WebSearchTool"]},
            {"path": "src/smolagents/default_tools.py",
             "symbols": ["VisitWebpageTool", "UserInputTool", "ApiWebSearchTool"]},
        ],
    },

    # ============ D 工具启用与校验 ============
    {
        "feature_group": "D-工具启用与校验",
        "feature": "argument-validation",
        "title": "工具参数校验",
        "page_file": "d01-argument-validation.md",
        "boundary": "负责调用前的参数校验（必填、类型、未知键）与失败时的错误面；"
                    "不负责 schema 生成（d03）、不负责工具定义（c01）。",
        "refs": [
            {"path": "src/smolagents/tools.py",
             "symbols": ["validate_tool_arguments", "validate_arguments"]},
        ],
    },
    {
        "feature_group": "D-工具启用与校验",
        "feature": "tool-code-validation",
        "title": "工具源码的静态校验",
        "page_file": "d02-tool-code-validation.md",
        "boundary": "负责对工具源码做静态检查（禁止的语句、属性、导入）与初始化后校验；"
                    "不负责参数校验（d01）、不负责解释器（b01）。",
        "refs": [
            {"path": "src/smolagents/tool_validation.py",
             "symbols": ["MethodChecker", "validate_tool_attributes"]},
            {"path": "src/smolagents/tools.py", "symbols": ["validate_after_init"]},
        ],
    },
    {
        "feature_group": "D-工具启用与校验",
        "feature": "json-schema-from-signature",
        "title": "从签名与文档生成 JSON schema",
        "page_file": "d03-json-schema-from-signature.md",
        "boundary": "负责把类型注解与文档字符串转成工具 JSON schema（含联合类型与类型解析）；"
                    "不负责参数校验（d01）、不负责提示渲染（c01）。",
        "refs": [
            {"path": "src/smolagents/_function_type_hints_utils.py",
             "symbols": ["get_json_schema", "_parse_type_hint", "_parse_google_format_docstring"]},
            {"path": "src/smolagents/_function_type_hints_utils.py",
             "symbols": ["_convert_type_hints_to_json_schema", "_parse_union_type"]},
        ],
    },
    {
        "feature_group": "D-工具启用与校验",
        "feature": "tool-visibility",
        "title": "工具暴露与描述注入",
        "page_file": "d04-tool-visibility.md",
        "boundary": "负责工具如何被暴露给模型（描述渲染、schema 注入、工具集代码生成）；"
                    "不负责 schema 生成（d03）、不负责主循环（a01）。",
        "refs": [
            {"path": "src/smolagents/tools.py",
             "symbols": ["get_tools_definition_code", "add_description"]},
            {"path": "src/smolagents/models.py", "symbols": ["get_tool_json_schema"]},
        ],
    },

    # ============ E 模型层 ============
    {
        "feature_group": "E-模型层",
        "feature": "model-base",
        "title": "模型基类与消息协议",
        "page_file": "e01-model-base.md",
        "boundary": "负责模型基类的调用契约、消息结构、停止序列处理与工具调用解析；"
                    "不负责具体实现（e02、e03）、不负责流式聚合（e04）。",
        "refs": [
            {"path": "src/smolagents/models.py",
             "symbols": ["Model", "ChatMessage", "get_tool_call_from_text"]},
            {"path": "src/smolagents/models.py",
             "symbols": ["remove_content_after_stop_sequences", "get_clean_message_list"]},
        ],
    },
    {
        "feature_group": "E-模型层",
        "feature": "api-models",
        "title": "API 模型实现",
        "page_file": "e02-api-models.md",
        "boundary": "负责通过 API 访问的模型实现（推理提供方、兼容端点）的请求装配与响应解析；"
                    "不负责本地推理（e03）、不负责基类（e01）。",
        "refs": [
            {"path": "src/smolagents/models.py",
             "symbols": ["_prepare_completion_kwargs", "generate"]},
        ],
    },
    {
        "feature_group": "E-模型层",
        "feature": "local-inference",
        "title": "本地推理模型",
        "page_file": "e03-local-inference.md",
        "boundary": "负责本地推理后端（vLLM、MLX 等）的加载、生成与清理；"
                    "不负责 API 模型（e02）、不负责基类（e01）。",
        "refs": [
            {"path": "src/smolagents/models.py", "symbols": ["VLLMModel", "MLXModel"]},
        ],
    },
    {
        "feature_group": "E-模型层",
        "feature": "streaming-aggregation",
        "title": "流式增量与聚合",
        "page_file": "e04-streaming-aggregation.md",
        "boundary": "负责流式增量的结构、聚合规则与工具调用分片的拼接；"
                    "不负责模型实现（e02）、不负责主循环（a01）。",
        "refs": [
            {"path": "src/smolagents/models.py",
             "symbols": ["ChatMessageStreamDelta", "agglomerate_stream_deltas",
                         "ChatMessageToolCallStreamDelta"]},
        ],
    },

    # ============ F 记忆与步骤 ============
    {
        "feature_group": "F-记忆与步骤",
        "feature": "memory-steps",
        "title": "记忆步骤类型与消息转换",
        "page_file": "f01-memory-steps.md",
        "boundary": "负责五类步骤的数据结构与如何转成模型消息；"
                    "不负责记忆容器（f02）、不负责主循环（a01）。",
        "refs": [
            {"path": "src/smolagents/memory.py",
             "symbols": ["MemoryStep", "ActionStep", "PlanningStep"]},
            {"path": "src/smolagents/memory.py",
             "symbols": ["TaskStep", "SystemPromptStep", "FinalAnswerStep"]},
        ],
    },
    {
        "feature_group": "F-记忆与步骤",
        "feature": "agent-memory",
        "title": "记忆容器与重放",
        "page_file": "f02-agent-memory.md",
        "boundary": "负责记忆的存储、摘要视图、重放与完整代码重建；"
                    "不负责步骤结构（f01）、不负责序列化（h02）。",
        "refs": [
            {"path": "src/smolagents/memory.py",
             "symbols": ["AgentMemory", "get_succinct_steps", "replay", "return_full_code"]},
        ],
    },
    {
        "feature_group": "F-记忆与步骤",
        "feature": "callbacks",
        "title": "步骤回调注册",
        "page_file": "f03-callbacks.md",
        "boundary": "负责按步骤类型注册与触发回调；"
                    "不负责记忆（f01）、不负责监控指标（i03）。",
        "refs": [
            {"path": "src/smolagents/memory.py",
             "symbols": ["CallbackRegistry", "register", "callback"]},
            {"path": "src/smolagents/agents.py", "symbols": ["_setup_step_callbacks"]},
        ],
    },

    # ============ G 远程执行 ============
    {
        "feature_group": "G-远程执行",
        "feature": "remote-executor-contract",
        "title": "远程执行器契约",
        "page_file": "g01-remote-executor-contract.md",
        "boundary": "负责远程执行器的统一契约：代码执行、错误传递、最终答案回传与清理；"
                    "不负责具体后端（g02）、不负责任务本地沙箱（b01）。",
        "refs": [
            {"path": "src/smolagents/remote_executors.py",
             "symbols": ["RemotePythonExecutor", "run_code_raise_errors",
                         "_patch_final_answer_with_exception"]},
        ],
    },
    {
        "feature_group": "G-远程执行",
        "feature": "executor-backends",
        "title": "远程执行后端实现",
        "page_file": "g02-executor-backends.md",
        "boundary": "负责各远程后端（E2B、Docker、Modal 等）的启动、执行与清理差异；"
                    "不负责统一契约（g01）、不负责工具传递机制（g03）。",
        "refs": [
            {"path": "src/smolagents/remote_executors.py",
             "symbols": ["E2BExecutor", "DockerExecutor", "ModalExecutor"]},
            {"path": "src/smolagents/remote_executors.py",
             "symbols": ["BlaxelExecutor", "_wait_for_server"]},
        ],
    },
    {
        "feature_group": "G-远程执行",
        "feature": "tool-and-variable-transfer",
        "title": "工具与变量跨进程传递",
        "page_file": "g03-tool-and-variable-transfer.md",
        "boundary": "负责把工具与变量送到远程执行环境、以及结果如何序列化回来；"
                    "不负责后端实现（g02）、不负责安全序列化（h02）。",
        "refs": [
            {"path": "src/smolagents/remote_executors.py",
             "symbols": ["send_tools", "send_variables", "install_packages"]},
        ],
    },

    # ============ H-MCP 与序列化 ============
    {
        "feature_group": "H-MCP 与序列化",
        "feature": "mcp-client",
        "title": "MCP 客户端与工具接入",
        "page_file": "h01-mcp-client.md",
        "boundary": "负责 MCP 服务器连接与把其工具接成 smolagents 工具；"
                    "不负责工具基类（c01）、不负责序列化（h02）。",
        "refs": [
            {"path": "src/smolagents/mcp_client.py", "symbols": ["MCPClient"]},
        ],
    },
    {
        "feature_group": "H-MCP 与序列化",
        "feature": "safe-serialization",
        "title": "安全序列化",
        "page_file": "h02-safe-serialization.md",
        "boundary": "负责跨边界传输时的安全序列化与反序列化（拒绝可执行载荷）；"
                    "不负责远程传递流程（g03）、不负责 MCP（h01）。",
        "refs": [
            {"path": "src/smolagents/serialization.py",
             "symbols": ["SafeSerializer", "SerializationError"]},
            {"path": "src/smolagents/utils.py", "symbols": ["make_json_serializable"]},
        ],
    },

    # ============ I-错误与可观测 ============
    {
        "feature_group": "I-错误与可观测",
        "feature": "error-taxonomy",
        "title": "错误分类与重试",
        "page_file": "i01-error-taxonomy.md",
        "boundary": "负责错误类型的层级划分、格式化与限流重试；"
                    "不负责解析错误处理（i02）、不负责日志（i03）。",
        "refs": [
            {"path": "src/smolagents/utils.py",
             "symbols": ["AgentError", "AgentParsingError", "AgentMaxStepsError"]},
            {"path": "src/smolagents/utils.py",
             "symbols": ["RateLimiter", "Retrying"]},
        ],
    },
    {
        "feature_group": "I-错误与可观测",
        "feature": "parsing-and-truncation",
        "title": "解析工具与内容截断",
        "page_file": "i02-parsing-and-truncation.md",
        "boundary": "负责 JSON 解析的容错、代码块抽取的容错与长内容截断；"
                    "不负责错误分类（i01）、不负责代码执行（b01）。",
        "refs": [
            {"path": "src/smolagents/utils.py",
             "symbols": ["parse_json_blob", "truncate_content", "extract_code_from_text"]},
        ],
    },
    {
        "feature_group": "I-错误与可观测",
        "feature": "monitoring",
        "title": "监控、用量与日志",
        "page_file": "i03-monitoring.md",
        "boundary": "负责 token 用量与耗时统计、日志级别与输出形态；"
                    "不负责错误分类（i01）、不负责回调（f03）。",
        "refs": [
            {"path": "src/smolagents/monitoring.py",
             "symbols": ["TokenUsage", "Timing", "Monitor"]},
            {"path": "src/smolagents/monitoring.py", "symbols": ["AgentLogger", "LogLevel"]},
        ],
    },
]
