# -*- coding: utf-8 -*-
"""autogen 机制清单（阶段 2A 第二个 AI / Agent 项目）。

盘点范围：`python/packages/` 下的三个核心包 —— `autogen-core`（actor 运行时）、
`autogen-agentchat`（消息协议与团队编排）、`autogen-ext` 只取与本项目机制强相关
的少数扩展点（code executor 基类与 web surfer 交接），因为 `autogen-ext` 主体是
第三方集成，按机制归类属于「外部适配层」，留给后续增量。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按包内目录骨架
归并成机制簇。篇数是归并结果，不是按文件数折算。

源码锚点：`autogen@8390985108cb1e3881f5f7297765758d8ad2540c`。
"""

GROUP_ORDER = [
    "A-actor 运行时",
    "B-消息路由与订阅",
    "C-智能体基类与消息处理",
    "D-组件与序列化",
    "E-模型上下文与记忆",
    "F-工具与代码执行",
    "G-单智能体实现",
    "H-团队编排",
    "I-终止条件和状态恢复",
]

MECHANISM_SPECS = [
    # ============ A actor 运行时 ============
    {
        "feature_group": "A-actor 运行时",
        "feature": "single-threaded-runtime",
        "title": "单线程 actor 运行时与消息队列",
        "page_file": "a01-single-threaded-runtime.md",
        "boundary": "负责运行时主循环：消息入队、按序处理 send/publish、以及「何时算空闲」的判定；"
                    "不负责路由匹配（B 组）、不负责智能体内部逻辑（C 组）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py",
             "symbols": ["SingleThreadedAgentRuntime", "_process_send", "_process_publish"]},
            {"path": "python/packages/autogen-core/src/autogen_core/_queue.py",
             "symbols": ["Queue", "QueueShutDown"]},
        ],
    },
    {
        "feature_group": "A-actor 运行时",
        "feature": "runtime-interface",
        "title": "运行时抽象与智能体注册",
        "page_file": "a02-runtime-interface.md",
        "boundary": "负责定义运行时的抽象能力：注册工厂、注册实例、按 id 取实例、存取状态；"
                    "不负责具体实现的消息循环（a01）、不负责订阅维护（b03）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_agent_runtime.py",
             "symbols": ["AgentRuntime", "register_factory", "register_agent_instance"]},
        ],
    },
    {
        "feature_group": "A-actor 运行时",
        "feature": "run-context",
        "title": "运行上下文与停止条件",
        "page_file": "a03-run-context.md",
        "boundary": "负责把一次运行包成可 await 的上下文：空闲即停、条件轮询、显式停止；"
                    "不负责消息处理（a01）、不负责智能体生命周期（c01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py",
             "symbols": ["RunContext", "stop_when", "stop_when_idle"]},
        ],
    },
    {
        "feature_group": "A-actor 运行时",
        "feature": "agent-id-and-type",
        "title": "智能体标识与类型约束",
        "page_file": "a04-agent-id-and-type.md",
        "boundary": "负责智能体 id 的构成（类型 + 键）、合法性校验与字符串往返；"
                    "不负责订阅（b02）、不负责实例化（a02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_agent_id.py",
             "symbols": ["AgentId", "is_valid_agent_type"]},
            {"path": "python/packages/autogen-core/src/autogen_core/_topic.py",
             "symbols": ["TopicId", "is_valid_topic_type"]},
        ],
    },

    # ============ B 消息路由与订阅 ============
    {
        "feature_group": "B-消息路由与订阅",
        "feature": "routed-agent",
        "title": "按消息类型分发的 RoutedAgent",
        "page_file": "b01-routed-agent.md",
        "boundary": "负责 RoutedAgent 如何按入站消息的运行时类型挑选处理函数，以及同步/异步、"
                    "rpc/event 三种处理形态的注册；不负责运行时分发（a01）、不负责订阅匹配（b02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_routed_agent.py",
             "symbols": ["RoutedAgent", "message_handler", "event"]},
        ],
    },
    {
        "feature_group": "B-消息路由与订阅",
        "feature": "subscription-matching",
        "title": "订阅模型与主题匹配",
        "page_file": "b02-subscription-matching.md",
        "boundary": "负责订阅的定义与匹配：按类型或主题订阅、通配、映射到目标智能体；"
                    "不负责订阅管理器的增删（b03）、不负责消息投递（a01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_subscription.py",
             "symbols": ["Subscription", "is_match", "map_to_agent"]},
            {"path": "python/packages/autogen-core/src/autogen_core/_default_subscription.py",
             "symbols": ["DefaultSubscription", "type_subscription"]},
        ],
    },
    {
        "feature_group": "B-消息路由与订阅",
        "feature": "subscription-manager",
        "title": "订阅登记与接收者解析",
        "page_file": "b03-subscription-manager.md",
        "boundary": "负责维护「订阅 → 接收者」映射，并在发布消息时给出接收者集合；"
                    "不负责匹配语义（b02）、不负责把消息投给智能体（a01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py",
             "symbols": ["SubscriptionManager", "add_subscription", "get_subscribed_recipients"]},
        ],
    },
    {
        "feature_group": "B-消息路由与订阅",
        "feature": "closure-agent",
        "title": "闭包智能体与轻量注册",
        "page_file": "b04-closure-agent.md",
        "boundary": "负责用闭包而非类定义智能体：注册时推断可处理消息类型、把上下文与运行时"
                    "注入闭包；不负责类式智能体（c01）、不负责订阅（b02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_closure_agent.py",
             "symbols": ["ClosureAgent", "register_closure", "ClosureContext"]},
        ],
    },

    # ============ C 智能体基类与消息处理 ============
    {
        "feature_group": "C-智能体基类与消息处理",
        "feature": "base-agent-contract",
        "title": "BaseAgent 契约与消息入口",
        "page_file": "c01-base-agent-contract.md",
        "boundary": "负责智能体基类定义：id 与运行时的绑定、消息入口、状态存取、关闭钩子；"
                    "不负责具体分类实现（b01）、不负责注册流程（a02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_base_agent.py",
             "symbols": ["BaseAgent", "bind_id_and_runtime", "on_message"]},
            {"path": "python/packages/autogen-core/src/autogen_core/_agent.py",
             "symbols": ["Agent", "on_message", "save_state"]},
        ],
    },
    {
        "feature_group": "C-智能体基类与消息处理",
        "feature": "agent-instantiation-context",
        "title": "实例化上下文与订阅延迟绑定",
        "page_file": "c02-agent-instantiation-context.md",
        "boundary": "负责在智能体构造期间提供可用的运行时与订阅注册能力，使构造体内能立即登记订阅；"
                    "不负责订阅匹配（b02）、不负责工厂注册（a02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_agent_instantiation.py",
             "symbols": ["AgentInstantiationContext"]},
            {"path": "python/packages/autogen-core/src/autogen_core/_subscription_context.py",
             "symbols": ["SubscriptionInstantiationContext"]},
        ],
    },
    {
        "feature_group": "C-智能体基类与消息处理",
        "feature": "agent-proxy",
        "title": "代理与跨智能体调用",
        "page_file": "c03-agent-proxy.md",
        "boundary": "负责给调用方一个「可 await 的智能体句柄」：发消息、读写远端状态；"
                    "不负责运行时的消息处理（a01）、不负责订阅（b02）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_agent_proxy.py",
             "symbols": ["AgentProxy", "send_message", "save_state"]},
        ],
    },
    {
        "feature_group": "C-智能体基类与消息处理",
        "feature": "intervention-handler",
        "title": "消息拦截与改写",
        "page_file": "c04-intervention-handler.md",
        "boundary": "负责在消息发出与响应返回两侧插入可改写的钩子，以及「丢弃消息」这种特殊返回；"
                    "不负责路由（b02）、不负责智能体内部逻辑（g01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_intervention.py",
             "symbols": ["InterventionHandler", "DropMessage", "on_publish"]},
        ],
    },

    # ============ D 组件与序列化 ============
    {
        "feature_group": "D-组件与序列化",
        "feature": "message-serialization",
        "title": "消息序列化注册表",
        "page_file": "d01-message-serialization.md",
        "boundary": "负责按类型选择序列化器、dataclass / pydantic / protobuf 三种实现与未知载荷处理；"
                    "不负责组件声明（d02）、不负责状态恢复（i03）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_serialization.py",
             "symbols": ["SerializationRegistry", "MessageSerializer", "UnknownPayload"]},
        ],
    },
    {
        "feature_group": "D-组件与序列化",
        "feature": "component-config",
        "title": "组件声明与配置往返",
        "page_file": "d02-component-config.md",
        "boundary": "负责组件化对象如何声明自身配置、如何由配置重建、以及可信命名空间校验；"
                    "不负责消息序列化（d01）、不负责具体组件实现（E/F/G 组）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_component_config.py",
             "symbols": ["Component", "ComponentModel", "ComponentLoader"]},
        ],
    },
    {
        "feature_group": "D-组件与序列化",
        "feature": "function-introspection",
        "title": "函数签名到工具 schema",
        "page_file": "d03-function-introspection.md",
        "boundary": "负责把 Python 函数签名反射成 JSON schema（参数、必填、默认值、注解归一）；"
                    "不负责工具调用的执行（f02）、不负责模型侧协议（e01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_function_utils.py",
             "symbols": ["get_function_schema", "get_typed_signature", "normalize_annotated_type"]},
        ],
    },
    {
        "feature_group": "D-组件与序列化",
        "feature": "cache-store",
        "title": "缓存存储抽象与内存实现",
        "page_file": "d04-cache-store.md",
        "boundary": "负责键值缓存的最小契约与内存实现，供需要跨调用的短期结果复用；"
                    "不负责任何 LLM 语义缓存，也不负责状态持久化（i03）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/_cache_store.py",
             "symbols": ["CacheStore", "InMemoryStore", "InMemoryStoreConfig"]},
        ],
    },

    # ============ E 模型上下文与记忆 ============
    {
        "feature_group": "E-模型上下文与记忆",
        "feature": "chat-completion-context",
        "title": "模型上下文基类与状态存取",
        "page_file": "e01-chat-completion-context.md",
        "boundary": "负责「送给模型的对话历史」这一抽象：追加消息、取消息、清空与状态往返；"
                    "不负责具体裁剪策略（e02）、不负责记忆检索（e04）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/model_context/_chat_completion_context.py",
             "symbols": ["ChatCompletionContext", "add_message", "get_messages"]},
        ],
    },
    {
        "feature_group": "E-模型上下文与记忆",
        "feature": "context-window-strategies",
        "title": "上下文窗口的四种裁剪策略",
        "page_file": "e02-context-window-strategies.md",
        "boundary": "负责无界、缓冲、头尾保留、按 token 上限四种裁剪实现各自的取舍；"
                    "不负责上下文基类契约（e01）、不负责记忆（e04）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/model_context/_buffered_chat_completion_context.py",
             "symbols": ["BufferedChatCompletionContext", "get_messages"]},
            {"path": "python/packages/autogen-core/src/autogen_core/model_context/_head_and_tail_chat_completion_context.py",
             "symbols": ["HeadAndTailChatCompletionContext"]},
            {"path": "python/packages/autogen-core/src/autogen_core/model_context/_token_limited_chat_completion_context.py",
             "symbols": ["TokenLimitedChatCompletionContext"]},
        ],
    },
    {
        "feature_group": "E-模型上下文与记忆",
        "feature": "model-client-contract",
        "title": "模型客户端与能力声明",
        "page_file": "e03-model-client-contract.md",
        "boundary": "负责模型客户端的统一契约：创建、流式创建、token 计数、用量统计与能力声明；"
                    "不负责上下文裁剪（e02）、不负责具体供应商实现（属 autogen-ext）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/models/_model_client.py",
             "symbols": ["ChatCompletionClient", "ModelFamily", "ModelInfo"]},
        ],
    },
    {
        "feature_group": "E-模型上下文与记忆",
        "feature": "memory-abstraction",
        "title": "记忆抽象与列表实现",
        "page_file": "e04-memory-abstraction.md",
        "boundary": "负责记忆的四个动作（更新上下文、查询、添加、清空）与内存列表实现；"
                    "不负责上下文窗口（e02）、不负责外部向量库（属 autogen-ext）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/memory/_base_memory.py",
             "symbols": ["Memory", "MemoryContent", "update_context"]},
            {"path": "python/packages/autogen-core/src/autogen_core/memory/_list_memory.py",
             "symbols": ["ListMemory", "query", "add"]},
        ],
    },

    # ============ F 工具与代码执行 ============
    {
        "feature_group": "F-工具与代码执行",
        "feature": "tool-schema",
        "title": "工具 schema 与状态化调用",
        "page_file": "f01-tool-schema.md",
        "boundary": "负责工具的统一描述（名字、描述、参数 schema、返回类型）与状态存取接口；"
                    "不负责函数反射（d03）、不负责工作台调度（f03）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/tools/_base.py",
             "symbols": ["Tool", "ToolSchema", "BaseTool"]},
        ],
    },
    {
        "feature_group": "F-工具与代码执行",
        "feature": "function-tool",
        "title": "函数工具与参数校验",
        "page_file": "f02-function-tool.md",
        "boundary": "负责把普通函数包装成工具：按 schema 校验传入 JSON、把异常转成可读结果；"
                    "不负责 schema 生成（d03）、不负责工作台（f03）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/tools/_function_tool.py",
             "symbols": ["FunctionTool", "run"]},
        ],
    },
    {
        "feature_group": "F-工具与代码执行",
        "feature": "workbench",
        "title": "工作台与工具集合调度",
        "page_file": "f03-workbench.md",
        "boundary": "负责工具集合的生命周期与调用入口：列举、调用、启动/停止、状态存取；"
                    "不负责单个工具实现（f02）、不负责代码执行沙箱（f05）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/tools/_workbench.py",
             "symbols": ["Workbench", "ToolResult", "call_tool"]},
            {"path": "python/packages/autogen-core/src/autogen_core/tools/_static_workbench.py",
             "symbols": ["StaticWorkbench", "call_tool"]},
        ],
    },
    {
        "feature_group": "F-工具与代码执行",
        "feature": "tool-agent",
        "title": "工具智能体与调用循环",
        "page_file": "f04-tool-agent.md",
        "boundary": "负责把工作台暴露给模型：处理工具调用请求、异常分类、以及模型与工具之间的调用循环；"
                    "不负责工作台内部（f03）、不负责单智能体的编排（g01）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py",
             "symbols": ["ToolAgent", "ToolNotFoundException", "handle_function_call"]},
            {"path": "python/packages/autogen-core/src/autogen_core/tool_agent/_caller_loop.py",
             "symbols": ["tool_agent_caller_loop"]},
        ],
    },
    {
        "feature_group": "F-工具与代码执行",
        "feature": "code-executor",
        "title": "代码执行器与依赖声明",
        "page_file": "f05-code-executor.md",
        "boundary": "负责代码块执行契约：块结构、结果、启动停止重启，以及「带依赖的函数」如何被序列化"
                    "成可执行代码；不负责哪个语言或哪个沙箱后端（属 autogen-ext）。",
        "refs": [
            {"path": "python/packages/autogen-core/src/autogen_core/code_executor/_base.py",
             "symbols": ["CodeExecutor", "CodeBlock", "CodeResult"]},
            {"path": "python/packages/autogen-core/src/autogen_core/code_executor/_func_with_reqs.py",
             "symbols": ["FunctionWithRequirements", "with_requirements", "build_python_functions_file"]},
        ],
    },

    # ============ G 单智能体实现 ============
    {
        "feature_group": "G-单智能体实现",
        "feature": "chat-agent-contract",
        "title": "ChatAgent 契约与响应对象",
        "page_file": "g01-chat-agent-contract.md",
        "boundary": "负责单智能体对外契约：消息响应、流式响应、暂停/恢复、状态存取；"
                    "不负责具体智能体实现（g02）、不负责团队（h01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py",
             "symbols": ["ChatAgent", "Response", "on_messages_stream"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/base/_task.py",
             "symbols": ["TaskRunner", "TaskResult"]},
        ],
    },
    {
        "feature_group": "G-单智能体实现",
        "feature": "assistant-agent",
        "title": "AssistantAgent 的模型调用与工具循环",
        "page_file": "g02-assistant-agent.md",
        "boundary": "负责把模型调用、工具执行、反思总结串成一个完整回合，并处理交接消息；"
                    "不负责上下文裁剪（e02）、不负责团队选人（h04）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py",
             "symbols": ["AssistantAgent", "_call_llm", "_reflect_on_tool_use_flow"]},
        ],
    },
    {
        "feature_group": "G-单智能体实现",
        "feature": "code-executor-agent",
        "title": "代码执行智能体与审批",
        "page_file": "g03-code-executor-agent.md",
        "boundary": "负责从消息里抽代码块、执行、以及可选的审批与重试流程；"
                    "不负责执行器实现（f05）、不负责模型调用（g02）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py",
             "symbols": ["CodeExecutorAgent", "extract_code_blocks_from_messages", "execute_code_block"]},
        ],
    },
    {
        "feature_group": "G-单智能体实现",
        "feature": "user-proxy-and-filters",
        "title": "用户代理与消息过滤智能体",
        "page_file": "g04-user-proxy-and-filters.md",
        "boundary": "负责人工介入的两个辅助智能体：请求用户输入、按来源过滤消息；"
                    "不负责主循环（g02）、不负责团队（h01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_user_proxy_agent.py",
             "symbols": ["UserProxyAgent", "on_messages"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_message_filter_agent.py",
             "symbols": ["MessageFilterAgent", "_apply_filter"]},
        ],
    },
    {
        "feature_group": "G-单智能体实现",
        "feature": "society-of-mind",
        "title": "心智社会智能体（团队作为单体）",
        "page_file": "g05-society-of-mind.md",
        "boundary": "负责把整个团队包成一个单智能体对外呈现，并把团队内部过程折叠成一条回复；"
                    "不负责团队编排（h01）、不负责单智能体循环（g02）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py",
             "symbols": ["SocietyOfMindAgent", "on_messages_stream"]},
        ],
    },

    # ============ H 团队编排 ============
    {
        "feature_group": "H-团队编排",
        "feature": "group-chat-base",
        "title": "群聊团队基类与参与者装配",
        "page_file": "h01-group-chat-base.md",
        "boundary": "负责把参与智能体与选人管理器装配成一次运行，处理启动、重置、暂停与流式输出；"
                    "不负责具体选人策略（h03–h05）、不负责终止判定（i01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat.py",
             "symbols": ["BaseGroupChat", "_create_group_chat_manager_factory", "run_stream"]},
        ],
    },
    {
        "feature_group": "H-团队编排",
        "feature": "chat-agent-container",
        "title": "参与者容器与事件桥接",
        "page_file": "h02-chat-agent-container.md",
        "boundary": "负责把每个 ChatAgent 包成运行时智能体：转译事件、缓冲消息、接收发布请求；"
                    "不负责选人（h03）、不负责管理器的编排逻辑（h01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_chat_agent_container.py",
             "symbols": ["ChatAgentContainer", "handle_team_response", "_buffer_message"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_events.py",
             "symbols": ["GroupChatStart", "GroupChatRequestPublish", "GroupChatTermination"]},
        ],
    },
    {
        "feature_group": "H-团队编排",
        "feature": "group-chat-manager",
        "title": "群聊管理器与轮次推进",
        "page_file": "h03-group-chat-manager.md",
        "boundary": "负责管理器的通用流程：起手选人、收到回复后推进到下一批说话者、应用终止条件、"
                    "记录消息线程；不负责具体策略（h04、h05）、不负责容器（h02）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py",
             "symbols": ["BaseGroupChatManager", "_transition_to_next_speakers", "select_speaker"]},
        ],
    },
    {
        "feature_group": "H-团队编排",
        "feature": "round-robin-and-selector",
        "title": "轮询与模型选人两种策略",
        "page_file": "h04-round-robin-and-selector.md",
        "boundary": "负责两种选人器：按固定顺序轮流，以及用模型从候选里选下一个说话者；"
                    "不负责管理器通用流程（h03）、不负责交接型策略（h05）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py",
             "symbols": ["RoundRobinGroupChat", "RoundRobinGroupChatManager", "select_speaker"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py",
             "symbols": ["SelectorGroupChat", "_select_speaker", "construct_message_history"]},
        ],
    },
    {
        "feature_group": "H-团队编排",
        "feature": "swarm-handoff",
        "title": "Swarm 交接式选人",
        "page_file": "h05-swarm-handoff.md",
        "boundary": "负责按交接消息决定下一个说话者，并维护允许的交接边；"
                    "不负责模型选人（h04）、不负责图式编排（h06）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py",
             "symbols": ["Swarm", "SwarmGroupChatManager", "select_speaker"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/base/_handoff.py",
             "symbols": ["Handoff", "handoff_tool"]},
        ],
    },
    {
        "feature_group": "H-团队编排",
        "feature": "graph-flow",
        "title": "图式编排与激活条件",
        "page_file": "h06-graph-flow.md",
        "boundary": "负责有向图团队的节点边模型：激活条件、父子关系、环检测与剩余映射；"
                    "不负责群聊管理器（h03）、不负责终止条件（i01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_digraph_group_chat.py",
             "symbols": ["DiGraph", "GraphFlowManager", "has_cycles_with_exit"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_graph/_graph_builder.py",
             "symbols": ["DiGraphBuilder"]},
        ],
    },

    # ============ I 终止条件和状态恢复 ============
    {
        "feature_group": "I-终止条件和状态恢复",
        "feature": "termination-conditions",
        "title": "终止条件与布尔组合",
        "page_file": "i01-termination-conditions.md",
        "boundary": "负责终止条件的求值与组合（与、或），以及内置的若干判定实现；"
                    "不负责管理器何时调用它们（h03）、不负责状态恢复（i03）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py",
             "symbols": ["TerminationCondition", "AndTerminationCondition", "OrTerminationCondition"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py",
             "symbols": ["StopMessageTermination", "MaxMessageTermination", "TextMentionTermination"]},
        ],
    },
    {
        "feature_group": "I-终止条件和状态恢复",
        "feature": "message-model",
        "title": "消息类型与事件模型",
        "page_file": "i02-message-model.md",
        "boundary": "负责消息与事件的类型体系：聊天消息、智能体事件、结构化消息、多模态消息，"
                    "以及与模型消息的双向转换；不负责序列化注册（d01）、不负责状态（i03）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/messages.py",
             "symbols": ["BaseChatMessage", "TextMessage", "to_model_message"]},
        ],
    },
    {
        "feature_group": "I-终止条件和状态恢复",
        "feature": "team-and-agent-state",
        "title": "状态对象与保存恢复",
        "page_file": "i03-team-and-agent-state.md",
        "boundary": "负责智能体与团队的状态对象构成，以及 save/load 如何把内部上下文与计数器往返；"
                    "不负责消息序列化（d01）、不负责运行时状态（a01）。",
        "refs": [
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py",
             "symbols": ["BaseState", "TeamState", "AssistantAgentState"]},
            {"path": "python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py",
             "symbols": ["AssistantAgent", "save_state", "load_state"]},
        ],
    },
]
