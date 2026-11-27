# -*- coding: utf-8 -*-
"""mcp-python-sdk 机制清单（阶段 2B 第二个项目）。

盘点范围：`src/mcp/` 全部子包（client / server / shared / types / cli）。
这是 MCP 协议的官方 Python 实现，与其他 agent 框架不同：它**不是一个 agent 框架**，
而是一套「模型与外部能力之间的协议栈」——被 agent 框架当依赖用（crewai、openai-agents、
smolagents、pydantic-ai 都通过它接 MCP 服务器）。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按目录职责归并成
机制簇。篇数是归并结果，不是按文件数折算。

注意：`server/fastmcp.py` 在 2.x 里是**刻意的空模块**，import 即抛
`ModuleNotFoundError` 并给出迁移指引（FastMCP 已改名 MCPServer）。它没有机制，
不建页。

源码锚点：`mcp-python-sdk@77001c96f9507be0f4d009b83210a384a5e08bda`。
"""

GROUP_ORDER = [
    "A-协议类型与消息",
    "B-分发与请求生命周期",
    "C-服务端骨架",
    "D-工具、资源与提示",
    "E-服务端传输与安全",
    "F-客户端与传输",
    "G-授权与路径安全",
    "H-订阅、诱导与请求状态",
    "I-扩展与技能",
    "J-缓存与可观测",
]

MECHANISM_SPECS = [
    # ============ A 协议类型与消息 ============
    {
        "feature_group": "A-协议类型与消息",
        "feature": "jsonrpc-envelope",
        "title": "JSON-RPC 信封与错误约定",
        "page_file": "a01-jsonrpc-envelope.md",
        "boundary": "负责 JSON-RPC 的请求/响应/通知信封结构与错误码约定；"
                    "不负责分发（b01）、不负责任何业务方法（D 组）。",
        "refs": [
            {"path": "src/mcp-types/mcp_types/jsonrpc.py",
             "symbols": ["JSONRPCRequest", "JSONRPCResponse", "JSONRPCError"]},
            {"path": "src/mcp/shared/message.py",
             "symbols": ["SessionMessage", "ClientMessageMetadata", "ServerMessageMetadata"]},
        ],
    },
    {
        "feature_group": "A-协议类型与消息",
        "feature": "method-registry",
        "title": "方法名注册与能力协商",
        "page_file": "a02-method-registry.md",
        "boundary": "负责协议方法名的登记方式与初识握手时的能力协商范围；"
                    "不负责信封（a01）、不负责分发实现（b01）。",
        "refs": [
            {"path": "src/mcp-types/mcp_types/version.py",
             "symbols": ["SUPPORTED_PROTOCOL_VERSIONS", "LATEST_PROTOCOL_VERSION",
                         "is_version_at_least"]},
            {"path": "src/mcp-types/mcp_types/methods.py",
             "symbols": ["is_input_required", "parse_client_request"]},
        ],
    },
    {
        "feature_group": "A-协议类型与消息",
        "feature": "transport-context",
        "title": "传输上下文与请求元数据",
        "page_file": "a03-transport-context.md",
        "boundary": "负责一次请求在传输层携带的上下文（来源、认证主体、请求 id）如何被构造与读取；"
                    "不负责具体传输（E、F 组）、不负责请求状态编码（h03）。",
        "refs": [
            {"path": "src/mcp/shared/transport_context.py", "symbols": ["TransportContext"]},
            {"path": "src/mcp/shared/metadata_utils.py", "symbols": ["get_display_name"]},
        ],
    },

    # ============ B 分发与请求生命周期 ============
    {
        "feature_group": "B-分发与请求生命周期",
        "feature": "dispatcher-contract",
        "title": "分发器契约与调度选项",
        "page_file": "b01-dispatcher-contract.md",
        "boundary": "负责分发器的抽象契约、调用选项与出站计划的结构；"
                    "不负责 JSON-RPC 具体实现（b02）、不负责进度与取消（b03）。",
        "refs": [
            {"path": "src/mcp/shared/dispatcher.py",
             "symbols": ["Dispatcher", "CallOptions", "DispatchContext"]},
            {"path": "src/mcp/shared/direct_dispatcher.py", "symbols": ["DirectDispatcher"]},
        ],
    },
    {
        "feature_group": "B-分发与请求生命周期",
        "feature": "jsonrpc-dispatcher",
        "title": "JSON-RPC 分发器与在途请求表",
        "page_file": "b02-jsonrpc-dispatcher.md",
        "boundary": "负责把入站消息分派到处理器、维护在途请求与待处理表、以及异常到错误数据的翻译；"
                    "不负责契约（b01）、不负责进度取消（b03）。",
        "refs": [
            {"path": "src/mcp/shared/jsonrpc_dispatcher.py",
             "symbols": ["JSONRPCDispatcher", "_Pending", "_InFlight"]},
            {"path": "src/mcp/shared/jsonrpc_dispatcher.py",
             "symbols": ["handler_exception_to_error_data", "_plan_outbound"]},
        ],
    },
    {
        "feature_group": "B-分发与请求生命周期",
        "feature": "progress-and-cancel",
        "title": "进度通知与取消",
        "page_file": "b03-progress-and-cancel.md",
        "boundary": "负责进度令牌的产生与传递、以及取消请求的识别与传播；"
                    "不负责分发器（b02）、不负责订阅（h01）。",
        "refs": [
            {"path": "src/mcp/shared/jsonrpc_dispatcher.py",
             "symbols": ["progress_token_from_params", "cancelled_request_id_from_params",
                         "_shielded_progress"]},
        ],
    },

    # ============ C 服务端骨架 ============
    {
        "feature_group": "C-服务端骨架",
        "feature": "lowlevel-server",
        "title": "低层服务端与处理器登记",
        "page_file": "c01-lowlevel-server.md",
        "boundary": "负责低层服务端如何按方法名登记处理器、以及通知选项的控制；"
                    "不负责高层门面（c02）、不负责具体能力实现（D 组）。",
        "refs": [
            {"path": "src/mcp/server/lowlevel/server.py",
             "symbols": ["Server", "HandlerEntry", "NotificationOptions"]},
        ],
    },
    {
        "feature_group": "C-服务端骨架",
        "feature": "mcpserver-facade",
        "title": "MCPServer 高层门面",
        "page_file": "c02-mcpserver-facade.md",
        "boundary": "负责 MCPServer 如何把装饰器登记的工具体/资源/提示聚成服务端能力，"
                    "含设置与生命周期；不负责低层登记（c01）、不负责管理器的内部结构（d01）。",
        "refs": [
            {"path": "src/mcp/server/mcpserver/server.py",
             "symbols": ["MCPServer", "Settings", "lifespan_wrapper"]},
        ],
    },
    {
        "feature_group": "C-服务端骨架",
        "feature": "server-session",
        "title": "服务端会话与出站通道",
        "page_file": "c03-server-session.md",
        "boundary": "负责服务端会话的双向通道：发请求、发通知、以及只允许通知的出站形态；"
                    "不负责分发（b02）、不负责传输（e01）。",
        "refs": [
            {"path": "src/mcp/server/session.py", "symbols": ["ServerSession"]},
            {"path": "src/mcp/server/connection.py",
             "symbols": ["Connection", "NotifyOnlyOutbound"]},
        ],
    },
    {
        "feature_group": "C-服务端骨架",
        "feature": "server-runner",
        "title": "服务端运行器与中间件",
        "page_file": "c04-server-runner.md",
        "boundary": "负责把会话与分发器组装成可运行的服务器、以及中间件链的应用方式；"
                    "不负责门面（c02）、不负责传输（e01）。",
        "refs": [
            {"path": "src/mcp/server/runner.py",
             "symbols": ["ServerRunner", "_apply_middleware"]},
            {"path": "src/mcp/server/context.py",
             "symbols": ["ServerMiddleware", "ServerRequestContext"]},
        ],
    },

    # ============ D 工具、资源与提示 ============
    {
        "feature_group": "D-工具、资源与提示",
        "feature": "tool-manager",
        "title": "工具注册与调用参数校验",
        "page_file": "d01-tool-manager.md",
        "boundary": "负责工具的登记、参数模型生成与调用时的校验和异常面；"
                    "不负责资源与提示（d02）、不负责上下文注入（d03）。",
        "refs": [
            {"path": "src/mcp/server/mcpserver/tools/tool_manager.py",
             "symbols": ["ToolManager"]},
            {"path": "src/mcp/server/mcpserver/tools/base.py", "symbols": ["Tool"]},
            {"path": "src/mcp/server/mcpserver/utilities/func_metadata.py",
             "symbols": ["FuncMetadata", "func_metadata"]},
        ],
    },
    {
        "feature_group": "D-工具、资源与提示",
        "feature": "resources-and-prompts",
        "title": "资源、模板与提示",
        "page_file": "d02-resources-and-prompts.md",
        "boundary": "负责资源的登记与 URI 模板匹配、提示的登记与渲染；"
                    "不负责工具（d01）、不负责 URI 模板语法（g03）。",
        "refs": [
            {"path": "src/mcp/server/mcpserver/resources/resource_manager.py",
             "symbols": ["ResourceManager"]},
            {"path": "src/mcp/server/mcpserver/prompts/manager.py", "symbols": ["PromptManager"]},
            {"path": "src/mcp/server/mcpserver/resources/templates.py", "symbols": ["ResourceTemplate"]},
        ],
    },
    {
        "feature_group": "D-工具、资源与提示",
        "feature": "context-injection",
        "title": "上下文参数注入",
        "page_file": "d03-context-injection.md",
        "boundary": "负责工具/资源函数如何声明并拿到上下文对象（含依赖解析）；"
                    "不负责工具登记（d01）、不负责诱导（h02）。",
        "refs": [
            {"path": "src/mcp/server/mcpserver/utilities/context_injection.py",
             "symbols": ["find_context_parameter", "inject_context"]},
            {"path": "src/mcp/server/mcpserver/context.py", "symbols": ["Context"]},
        ],
    },
    {
        "feature_group": "D-工具、资源与提示",
        "feature": "dependency-resolution",
        "title": "请求期依赖解析",
        "page_file": "d04-dependency-resolution.md",
        "boundary": "负责处理器函数声明了「需要客户端提供的数据」时如何发起对客户端的请求并等待结果；"
                    "不负责上下文注入（d03）、不负责诱导的交互细节（h02）。",
        "refs": [
            {"path": "src/mcp/server/mcpserver/resolve.py",
             "symbols": ["Resolve", "Elicit", "Sample", "ListRoots"]},
            {"path": "src/mcp/server/mcpserver/resolve.py",
             "symbols": ["find_resolved_parameters", "returns_input_required"]},
        ],
    },

    # ============ E 服务端传输与安全 ============
    {
        "feature_group": "E-服务端传输与安全",
        "feature": "streamable-http-server",
        "title": "Streamable HTTP 服务端传输",
        "page_file": "e01-streamable-http-server.md",
        "boundary": "负责 Streamable HTTP 的请求响应形态、事件存储与会话管理；"
                    "不负责安全设置（e03）、不负责 stdio（e02）。",
        "refs": [
            {"path": "src/mcp/server/streamable_http.py",
             "symbols": ["StreamableHTTPServerTransport", "EventStore", "EventMessage"]},
            {"path": "src/mcp/server/streamable_http_manager.py",
             "symbols": ["StreamableHTTPSessionManager", "StreamableHTTPASGIApp"]},
        ],
    },
    {
        "feature_group": "E-服务端传输与安全",
        "feature": "stdio-server",
        "title": "stdio 服务端与文件描述符占用",
        "page_file": "e02-stdio-server.md",
        "boundary": "负责基于标准输入输出的传输，含描述符接管与恢复；"
                    "不负责 HTTP（e01）、不负责安全设置（e03）。",
        "refs": [
            {"path": "src/mcp/server/stdio.py",
             "symbols": ["_claim_fd", "_open_stdin_diversion", "_restore_fd"]},
        ],
    },
    {
        "feature_group": "E-服务端传输与安全",
        "feature": "transport-security",
        "title": "传输安全设置与请求体限制",
        "page_file": "e03-transport-security.md",
        "boundary": "负责传输层的来源校验、请求体大小限制与拒绝策略；"
                    "不负责传输实现（e01、e02）、不负责授权（G 组）。",
        "refs": [
            {"path": "src/mcp/server/transport_security.py",
             "symbols": ["TransportSecuritySettings", "TransportSecurityMiddleware",
                         "RequestBodyLimitMiddleware"]},
        ],
    },

    # ============ F 客户端与传输 ============
    {
        "feature_group": "F-客户端与传输",
        "feature": "client-session",
        "title": "客户端会话与握手",
        "page_file": "f01-client-session.md",
        "boundary": "负责客户端会话的初始化握手、请求发送、以及客户端侧回调（采样、诱导、根目录、日志）；"
                    "不负责连接管理（f02）、不负责缓存（j01）。",
        "refs": [
            {"path": "src/mcp/client/session.py",
             "symbols": ["ClientSession", "ClientRequestContext"]},
        ],
    },
    {
        "feature_group": "F-客户端与传输",
        "feature": "client-and-transports",
        "title": "客户端连接与三种传输",
        "page_file": "f02-client-and-transports.md",
        "boundary": "负责客户端如何按参数建立连接（进程内、stdio、SSE、Streamable HTTP）并折叠扩展；"
                    "不负责会话协议（f01）、不负责多会话组（f03）。",
        "refs": [
            {"path": "src/mcp/client/client.py", "symbols": ["Client", "_connect_transport", "_connect_inproc"]},
            {"path": "src/mcp/client/stdio.py", "symbols": ["StdioServerParameters"]},
            {"path": "src/mcp/client/streamable_http.py",
             "symbols": ["StreamableHTTPTransport", "ResumptionError"]},
        ],
    },
    {
        "feature_group": "F-客户端与传输",
        "feature": "session-group",
        "title": "多会话组与命名空间",
        "page_file": "f03-session-group.md",
        "boundary": "负责把多个服务器的会话聚成一个带命名空间的工具集合；"
                    "不负责单连接（f02）、不负责缓存（j01）。",
        "refs": [
            {"path": "src/mcp/client/session_group.py",
             "symbols": ["ClientSessionGroup", "ClientSessionParameters",
                         "StreamableHttpParameters"]},
        ],
    },

    # ============ G 授权与路径安全 ============
    {
        "feature_group": "G-授权与路径安全",
        "feature": "oauth-models",
        "title": "OAuth 授权模型",
        "page_file": "g01-oauth-models.md",
        "boundary": "负责 OAuth 相关数据结构与作用域/回调校验；"
                    "不负责授权流程的服务端实现（属服务端 auth 子包）、不负责路径安全（g02）。",
        "refs": [
            {"path": "src/mcp/shared/auth.py",
             "symbols": ["OAuthToken", "OAuthClientMetadata", "InvalidScopeError"]},
            {"path": "src/mcp/shared/auth_utils.py",
             "symbols": ["check_resource_allowed", "calculate_token_expiry"]},
        ],
    },
    {
        "feature_group": "G-授权与路径安全",
        "feature": "path-security",
        "title": "路径穿越防护",
        "page_file": "g02-path-security.md",
        "boundary": "负责判断路径是否逃逸、以及安全拼接；"
                    "不负责授权（g01）、不负责 URI 模板（g03）。",
        "refs": [
            {"path": "src/mcp/shared/path_security.py",
             "symbols": ["contains_path_traversal", "safe_join", "PathEscapeError"]},
        ],
    },
    {
        "feature_group": "G-授权与路径安全",
        "feature": "uri-template",
        "title": "URI 模板展开与匹配",
        "page_file": "g03-uri-template.md",
        "boundary": "负责 RFC 6570 风格 URI 模板的解析、展开与贪婪匹配；"
                    "不负责资源登记（d02）、不负责路径安全（g02）。",
        "refs": [
            {"path": "src/mcp/shared/uri_template.py",
             "symbols": ["UriTemplate", "_Expression", "InvalidUriTemplate"]},
        ],
    },

    # ============ H 订阅、诱导与请求状态 ============
    {
        "feature_group": "H-订阅、诱导与请求状态",
        "feature": "subscriptions",
        "title": "订阅总线与监听",
        "page_file": "h01-subscriptions.md",
        "boundary": "负责资源/提示变更订阅的登记、广播与清理；"
                    "不负责进度通知（b03）、不负责事件缓存（j01）。",
        "refs": [
            {"path": "src/mcp/server/subscriptions.py",
             "symbols": ["SubscriptionBus", "InMemorySubscriptionBus", "ListenHandler"]},
            {"path": "src/mcp/shared/subscriptions.py",
             "symbols": ["event_to_notification", "event_from_wire", "ResourceUpdated"]},
        ],
    },
    {
        "feature_group": "H-订阅、诱导与请求状态",
        "feature": "elicitation",
        "title": "诱导请求与结果形态",
        "page_file": "h02-elicitation.md",
        "boundary": "负责向客户端请求用户输入的诱导流程与四种结果形态、以及 schema 渲染校验；"
                    "不负责依赖解析（d04）、不负责请求状态（h03）。",
        "refs": [
            {"path": "src/mcp/server/elicitation.py",
             "symbols": ["AcceptedElicitation", "DeclinedElicitation", "render_elicitation_schema"]},
            {"path": "src/mcp/server/elicitation.py", "symbols": ["CancelledElicitation",
                                                                  "AcceptedUrlElicitation"]},
        ],
    },
    {
        "feature_group": "H-订阅、诱导与请求状态",
        "feature": "request-state",
        "title": "请求状态编码与边界校验",
        "page_file": "h03-request-state.md",
        "boundary": "负责把服务端状态编码进令牌、解密校验与身份绑定；"
                    "不负责诱导（h02）、不负责授权（g01）。",
        "refs": [
            {"path": "src/mcp/server/request_state.py",
             "symbols": ["RequestStateCodec", "AESGCMRequestStateCodec",
                         "RequestStateSecurity"]},
            {"path": "src/mcp/server/request_state.py", "symbols": ["RequestStateBoundary",
                                                                    "authenticated_principal"]},
        ],
    },

    # ============ I 扩展与技能 ============
    {
        "feature_group": "I-扩展与技能",
        "feature": "extension-framework",
        "title": "扩展点与绑定",
        "page_file": "i01-extension-framework.md",
        "boundary": "负责扩展标识、能力声明与工具/资源/方法的绑定机制；"
                    "不负责技能（i02）、不负责会话（f01）。",
        "refs": [
            {"path": "src/mcp/server/extension.py",
             "symbols": ["Extension", "ToolBinding", "MethodBinding"]},
            {"path": "src/mcp/client/extension.py",
             "symbols": ["ClientExtension", "ClaimContext", "ResultClaim"]},
        ],
    },
    {
        "feature_group": "I-扩展与技能",
        "feature": "skills",
        "title": "技能包与资源校验",
        "page_file": "i02-skills.md",
        "boundary": "负责技能的清单、目录结构约定与资源摘要校验；"
                    "不负责扩展（i01）、不负责路径安全（g02）。",
        "refs": [
            {"path": "src/mcp/shared/skills.py",
             "symbols": ["Skill", "SkillResource", "validate_directory_result"]},
            {"path": "src/mcp/server/skills.py", "symbols": ["Skills"]},
        ],
    },

    # ============ J 缓存与可观测 ============
    {
        "feature_group": "J-缓存与可观测",
        "feature": "client-response-cache",
        "title": "客户端响应缓存",
        "page_file": "j01-client-response-cache.md",
        "boundary": "负责把可缓存的响应按键存起来、以及 TTL 与失效；"
                    "不负责服务端缓存声明、不负责可观测（j02）。",
        "refs": [
            {"path": "src/mcp/client/caching.py",
             "symbols": ["ClientResponseCache", "CacheConfig", "CacheKey"]},
            {"path": "src/mcp/client/caching.py",
             "symbols": ["InMemoryResponseCacheStore", "ResponseCacheStore"]},
        ],
    },
    {
        "feature_group": "J-缓存与可观测",
        "feature": "observability",
        "title": "链路追踪与日志级别",
        "page_file": "j02-observability.md",
        "boundary": "负责 OpenTelemetry 跨度与日志级别的传递控制；"
                    "不负责缓存（j01）、不负责客户端回调（f01）。",
        "refs": [
            {"path": "src/mcp/server/_otel.py", "symbols": ["_otel"]},
            {"path": "src/mcp/server/connection.py", "symbols": ["allowed_log_levels"]},
        ],
    },
]
