# -*- coding: utf-8 -*-
"""openhands 机制清单（阶段 2B 第四个项目）。

盘点范围：`src/` 的**控制面层** —— `api/`（各后端服务客户端）、`stores/`（状态容器）、
`hooks/` 中的状态派生部分。这是 OpenHands 的 Agent Canvas：一个自托管的开发者控制中心，
本身**不是** agent 运行时，而是「把多个 agent 后端（OpenHands / Claude Code / Codex /
Gemini / Pi / OpenCode）统一到一套控制界面」的前端。

React 组件（`components/`，755 个文件）与样式不属于机制，不建页。`electron/`、`tests/`、
`docs/` 亦然。

盘点方法：本项目是 TypeScript，没有现成的 AST 定义索引器（`dump_defs.py` 只认 C 风格
函数定义、`dump_python_defs.py` 只认 Python），因此改用「正则抽取顶层导出 + 人工核对」
的方式建符号清单，再用 `gen_manifest.py --verify` 的 refs 校验兜底。

源码锚点：`OpenHands@4adc3d6d00ab22844edf54131de81070724267bb`。
"""

GROUP_ORDER = [
    "A-后端注册表与选择",
    "B-后端健康与存储",
    "C-ACP 智能体接入",
    "D-会话与事件",
    "E-MCP 集成",
    "F-自动化与工具目录",
    "G-工作区与 Git",
    "H-云隧道与桥接",
    "I-状态容器与路由",
]

MECHANISM_SPECS = [
    # ============ A 后端注册表与选择 ============
    {
        "feature_group": "A-后端注册表与选择",
        "feature": "backend-model",
        "title": "后端类型模型",
        "page_file": "a01-backend-model.md",
        "boundary": "负责「后端」这个概念的数据结构与种类划分；"
                    "不负责选择逻辑（a02）、不负责持久化（b02）。",
        "refs": [
            {"path": "src/api/backend-registry/types.ts",
             "symbols": ["BackendKind", "BackendAuthMode", "Backend", "BackendSelection"]},
            {"path": "src/api/backend-registry/types.ts",
             "symbols": ["ResolvedActiveBackend"]},
        ],
    },
    {
        "feature_group": "A-后端注册表与选择",
        "feature": "active-backend",
        "title": "当前后端的选定与回退",
        "page_file": "a02-active-backend.md",
        "boundary": "负责当前活动后端的计算、订阅与回退规则；"
                    "不负责类型定义（a01）、不负责持久化（b02）。",
        "refs": [
            {"path": "src/api/backend-registry/active-store.ts",
             "symbols": ["getActiveBackend", "getEffectiveLocalBackend",
                         "getRegisteredBackends", "getSnapshot"]},
            {"path": "src/api/backend-registry/active-store.ts",
             "symbols": ["computeSnapshot", "pickFallbackBackend", "readInitialSelection"]},
        ],
    },
    {
        "feature_group": "A-后端注册表与选择",
        "feature": "default-backend",
        "title": "默认后端与锁定后端",
        "page_file": "a03-default-backend.md",
        "boundary": "负责首次运行时的默认本地后端、以及被锁定的云后端如何构造；"
                    "不负责选择（a02）、不负责 URL 参数（i02）。",
        "refs": [
            {"path": "src/api/backend-registry/default-backend.ts",
             "symbols": ["SEEDED_DEFAULT_BACKEND_ID", "makeDefaultLocalBackend"]},
            {"path": "src/api/backend-registry/default-backend.ts",
             "symbols": ["LOCKED_CLOUD_BACKEND_ID", "makeLockedCloudBackend"]},
        ],
    },

    # ============ B 后端健康与存储 ============
    {
        "feature_group": "B-后端健康与存储",
        "feature": "backend-health",
        "title": "后端健康状态跟踪",
        "page_file": "b01-backend-health.md",
        "boundary": "负责把后端的成功与失败记录成健康状态并对外订阅；"
                    "不负责持久化（b02）、不负责会话状态（i01）。",
        "refs": [
            {"path": "src/api/backend-registry/health-store.ts",
             "symbols": ["getHealthSnapshot", "getBackendHealthEntry", "subscribeBackendHealth"]},
            {"path": "src/api/backend-registry/health-store.ts",
             "symbols": ["recordBackendFailure", "recordBackendSuccess", "dropBackendHealth"]},
        ],
    },
    {
        "feature_group": "B-后端健康与存储",
        "feature": "backend-storage",
        "title": "后端清单的本地持久化",
        "page_file": "b02-backend-storage.md",
        "boundary": "负责后端清单与当前选择的读写、以及读到脏数据时的校验与丢弃；"
                    "不负责健康（b01）、不负责 URL 参数（i02）。",
        "refs": [
            {"path": "src/api/backend-registry/storage.ts",
             "symbols": ["BACKENDS_STORAGE_KEY", "readStoredBackends", "writeStoredBackends"]},
            {"path": "src/api/backend-registry/storage.ts",
             "symbols": ["isValidBackend", "isLoopbackUrl", "parseBackendSelection"]},
        ],
    },

    # ============ C-ACP 智能体接入 ============
    {
        "feature_group": "C-ACP 智能体接入",
        "feature": "acp-auth-probe",
        "title": "ACP 智能体认证探测",
        "page_file": "c01-acp-auth-probe.md",
        "boundary": "负责判断各 ACP 智能体（Claude / Codex 等）在本机是否已登录、以及探测失败的归类；"
                    "不负责会话（d01）、不负责后端注册（a01）。",
        "refs": [
            {"path": "src/api/acp-service/acp-service.api.ts",
             "symbols": ["AcpAuthStatus", "AcpAuthProbe", "PROBE_TIMEOUT_SECONDS"]},
            {"path": "src/api/acp-service/acp-service.api.ts",
             "symbols": ["classifyClaude", "classifyCodex", "ACP_AUTH_PROBES", "AcpService"]},
        ],
    },
    {
        "feature_group": "C-ACP 智能体接入",
        "feature": "agent-profiles",
        "title": "智能体档案与当前档案",
        "page_file": "c02-agent-profiles.md",
        "boundary": "负责智能体档案（模型、努力程度等）如何在界面上被选择与派生；"
                    "不负责认证（c01）、不负责会话创建（d01）。",
        "refs": [
            {"path": "src/hooks/use-active-agent-profile.ts",
             "symbols": ["useActiveAgentProfile", "useActiveAgentKind"]},
            {"path": "src/api/conversation-service/agent-server-conversation-service.api.ts",
             "symbols": ["normalizeAcpModels", "normalizeAcpEfforts"]},
        ],
    },

    # ============ D-会话与事件 ============
    {
        "feature_group": "D-会话与事件",
        "feature": "conversation-service",
        "title": "会话服务与响应归一化",
        "page_file": "d01-conversation-service.md",
        "boundary": "负责会话的创建、查询与响应归一化（含路径安全校验）；"
                    "不负责事件流（d02）、不负责前端状态容器（i01）。",
        "refs": [
            {"path": "src/api/conversation-service/agent-server-conversation-service.api.ts",
             "symbols": ["normalizeWorkspace", "normalizeRuntimeInfo", "normalizeAgent"]},
            {"path": "src/api/conversation-service/agent-server-conversation-service.api.ts",
             "symbols": ["normalizeTokenUsage", "requirePathInsideDirectory"]},
        ],
    },
    {
        "feature_group": "D-会话与事件",
        "feature": "conversation-types",
        "title": "会话类型与启动任务",
        "page_file": "d02-conversation-types.md",
        "boundary": "负责会话相关的数据结构：沙箱状态、消息内容、启动任务状态；"
                    "不负责服务实现（d01）、不负责事件（d03）。",
        "refs": [
            {"path": "src/api/conversation-service/agent-server-conversation-service.types.ts",
             "symbols": ["SandboxStatus", "AppConversationStartTaskStatus",
                         "AppConversationStartRequest"]},
            {"path": "src/api/conversation-service/agent-server-conversation-service.types.ts",
             "symbols": ["MessageContent", "TokenUsage", "MetricsSnapshot"]},
        ],
    },
    {
        "feature_group": "D-会话与事件",
        "feature": "event-service",
        "title": "事件检索与确认响应",
        "page_file": "d03-event-service.md",
        "boundary": "负责事件的搜索、排序与分页，以及需要用户确认的事件的应答；"
                    "不负责会话（d01）、不负责 MCP 审批（e03）。",
        "refs": [
            {"path": "src/api/event-service/event-service.api.ts", "symbols": ["EventService"]},
            {"path": "src/api/event-service/event-service.types.ts",
             "symbols": ["EventSortOrder", "EventSearchOptions", "ConfirmationResponseRequest"]},
        ],
    },

    # ============ E-MCP 集成 ============
    {
        "feature_group": "E-MCP 集成",
        "feature": "mcp-service",
        "title": "MCP 服务器配置与测试",
        "page_file": "e01-mcp-service.md",
        "boundary": "负责 MCP 服务器的增删改查、连接测试与响应脱敏；"
                    "不负责凭据替换（e02）、不负责健康探测（e03）。",
        "refs": [
            {"path": "src/api/mcp-service/mcp-service.api.ts",
             "symbols": ["McpService", "toMcpServer", "buildMcpTestRequest"]},
            {"path": "src/api/mcp-service/mcp-service.api.ts",
             "symbols": ["redactMcpTestResponse", "finalizeMcpTestResponse", "getMcpTestTimeout"]},
        ],
    },
    {
        "feature_group": "E-MCP 集成",
        "feature": "mcp-redacted-credentials",
        "title": "MCP 凭据脱敏与服务端替换",
        "page_file": "e02-mcp-redacted-credentials.md",
        "boundary": "负责把已存储的 MCP 凭据换成占位符、以及提交时再换回真实值；"
                    "不负责服务接口（e01）、不负责探测（e03）。",
        "refs": [
            {"path": "src/api/mcp-service/mcp-redacted-credentials.ts",
             "symbols": ["hasRedactedValue", "substituteRedactedLeaves",
                         "substituteRedactedMcpCredentials"]},
            {"path": "src/api/mcp-service/mcp-redacted-credentials.ts",
             "symbols": ["fetchEncryptedStoredServer", "findStoredServer"]},
        ],
    },
    {
        "feature_group": "E-MCP 集成",
        "feature": "mcp-health",
        "title": "MCP 健康探测与重授权",
        "page_file": "e03-mcp-health.md",
        "boundary": "负责对 MCP 服务器发起健康探测、结果归类与重授权；"
                    "不负责配置（e01）、不负责凭据替换（e02）。",
        "refs": [
            {"path": "src/api/mcp-health/probe-mcp-server-health.ts",
             "symbols": ["probeMcpServerHealth", "interpretMcpTestResponse",
                         "reauthorizeMcpServerHealth"]},
            {"path": "src/api/mcp-health/mcp-health-store.ts",
             "symbols": ["getMcpHealthSnapshot", "beginMcpHealthCheck",
                         "resolveMcpHealthCheck"]},
        ],
    },

    # ============ F-自动化与工具目录 ============
    {
        "feature_group": "F-自动化与工具目录",
        "feature": "automation-service",
        "title": "自动化与自定义 Webhook",
        "page_file": "f01-automation-service.md",
        "boundary": "负责自动化的创建、健康检查与自定义 Webhook 注册；"
                    "不负责工具目录（f02）、不负责会话（d01）。",
        "refs": [
            {"path": "src/api/automation-service/automation-service.api.ts",
             "symbols": ["AUTOMATION_BASE_PATH", "AUTOMATION_KV_PATH",
                         "automationCreateEndpointForTarget"]},
            {"path": "src/api/automation-service/automation-service.api.ts",
             "symbols": ["CustomWebhook", "AutomationHealthResponse",
                         "normalizeAutomationSdkVersion"]},
        ],
    },
    {
        "feature_group": "F-自动化与工具目录",
        "feature": "tool-catalog",
        "title": "工具目录校验",
        "page_file": "f02-tool-catalog.md",
        "boundary": "负责工具目录条目的结构校验与加载；"
                    "不负责自动化（f01）、不负责 MCP 工具（e01）。",
        "refs": [
            {"path": "src/api/tool-catalog-service/tool-catalog-service.api.ts",
             "symbols": ["isToolCatalogEntry", "assertValidCatalog", "ToolCatalogService"]},
        ],
    },

    # ============ G-工作区与 Git ============
    {
        "feature_group": "G-工作区与 Git",
        "feature": "git-service",
        "title": "Git 服务与云/本地分支",
        "page_file": "g01-git-service.md",
        "boundary": "负责仓库、分支与安装列表的查询与归一化，以及云后端与本地后端的分流；"
                    "不负责运行时路径（g02）、不负责工作区文件（g03）。",
        "refs": [
            {"path": "src/api/git-service/git-service.api.ts",
             "symbols": ["safeProvider", "isCloudActive", "normalizeRepositoryPage"]},
            {"path": "src/api/git-service/git-service.api.ts",
             "symbols": ["EMPTY_REPOSITORY_PAGE", "searchAgentServerRepositories", "GitService"]},
        ],
    },
    {
        "feature_group": "G-工作区与 Git",
        "feature": "agent-server-git",
        "title": "运行时侧 Git 与路径换算",
        "page_file": "g02-agent-server-git.md",
        "boundary": "负责把改动与提交从运行时取回、以及相对路径到绝对运行时路径的换算；"
                    "不负责仓库查询（g01）、不负责 bash（g03）。",
        "refs": [
            {"path": "src/api/git-service/agent-server-git-service.api.ts",
             "symbols": ["AgentServerGitChange", "AgentServerGitCommit", "toAbsoluteRuntimePath"]},
            {"path": "src/api/git-service/agent-server-git-service.api.ts",
             "symbols": ["getFromRuntime", "isEndpointMissingError", "AgentServerGitService"]},
        ],
    },
    {
        "feature_group": "G-工作区与 Git",
        "feature": "bash-service",
        "title": "Bash 输出分页与检索",
        "page_file": "g03-bash-service.md",
        "boundary": "负责命令输出页的取回、上限与搜索；"
                    "不负责 Git（g02）、不负责事件（d03）。",
        "refs": [
            {"path": "src/api/bash-service/bash-service.api.ts",
             "symbols": ["isBashOutput", "MAX_OUTPUT_PAGES", "BashService"]},
            {"path": "src/api/bash-service/bash-service.api.ts", "symbols": ["SearchOptions"]},
        ],
    },

    # ============ H-云隧道与桥接 ============
    {
        "feature_group": "H-云隧道与桥接",
        "feature": "mars-tunnel",
        "title": "Mars 隧道与沙箱代理",
        "page_file": "h01-mars-tunnel.md",
        "boundary": "负责 Mars 隧道的连接、凭据与沙箱代理配置；"
                    "不负责 Web 桥接（h02）、不负责后端注册（a01）。",
        "refs": [
            {"path": "src/api/mars/mars-tunnel-backend.ts",
             "symbols": ["MarsConnection", "MarsAuthState", "MarsSession"]},
            {"path": "src/api/mars/mars-tunnel-backend.ts",
             "symbols": ["MarsTransport", "isValidAgentName", "MarsAgentConfig"]},
        ],
    },
    {
        "feature_group": "H-云隧道与桥接",
        "feature": "mars-web-bridge",
        "title": "Mars Web 桥接与 RPC",
        "page_file": "h02-mars-web-bridge.md",
        "boundary": "负责浏览器侧到隧道的 RPC 桥接、请求头与健康检查；"
                    "不负责隧道本身（h01）、不负责会话（d01）。",
        "refs": [
            {"path": "src/api/mars/mars-web-bridge.ts",
             "symbols": ["RpcEnvelope", "rpc", "createMarsWebBridge"]},
            {"path": "src/api/mars/mars-web-bridge.ts",
             "symbols": ["MarsWebBridgeError", "getMarsBridgeHeaders", "probeMarsWebBridge"]},
        ],
    },

    # ============ I-状态容器与路由 ============
    {
        "feature_group": "I-状态容器与路由",
        "feature": "conversation-store",
        "title": "会话状态容器",
        "page_file": "i01-conversation-store.md",
        "boundary": "负责前端会话状态（标签页、模式、待发送消息）的容器与从 URL 初始化；"
                    "不负责后端会话服务（d01）、不负责后端选择（a02）。",
        "refs": [
            {"path": "src/stores/conversation-store.ts",
             "symbols": ["ConversationTab", "ConversationState", "ConversationActions"]},
            {"path": "src/stores/conversation-store.ts",
             "symbols": ["getConversationIdFromLocation", "getInitialConversationMode"]},
        ],
    },
    {
        "feature_group": "I-状态容器与路由",
        "feature": "url-selection",
        "title": "后端选择的 URL 参数",
        "page_file": "i02-url-selection.md",
        "boundary": "负责通过 URL 参数指定后端与云组织、以及写回 URL；"
                    "不负责默认后端（a03）、不负责本地存储（b02）。",
        "refs": [
            {"path": "src/api/backend-registry/url-selection.ts",
             "symbols": ["BACKEND_QUERY_PARAM", "withBackendSelectionParams",
                         "readBackendSelectionFromUrl"]},
            {"path": "src/api/backend-registry/url-selection.ts",
             "symbols": ["ORG_QUERY_PARAM", "readLockedCloudOrgFromUrl"]},
        ],
    },
    {
        "feature_group": "I-状态容器与路由",
        "feature": "agent-state",
        "title": "智能体执行状态的派生",
        "page_file": "i03-agent-state.md",
        "boundary": "负责把运行时执行状态映射成界面用的智能体状态，以及规划态的派生；"
                    "不负责档案选择（c02）、不负责事件（d03）。",
        "refs": [
            {"path": "src/hooks/use-agent-state.ts",
             "symbols": ["mapExecutionStatusToAgentState", "useAgentState"]},
            {"path": "src/stores/agent-store.ts", "symbols": ["AgentStateData", "useAgentStore"]},
        ],
    },
]
