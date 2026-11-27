# -*- coding: utf-8 -*-
"""browser-use 机制清单（阶段 2B 第三个项目：浏览器使用类）。

盘点范围：`browser_use/` 包全部子目录（agent / browser / llm / mcp / repl / skills /
telemetry）。这是 1.0 版本，与 0.x 有根本差别：**模型不再每步从 DOM 快照里挑一个动作，
而是在一个常驻 REPL 里写 Python**，REPL 进程里既有 browser-harness 辅助函数也有裸 CDP。
0.x 的公开 API（Agent / Browser / Tools / run() / final_result()）保留在顶层。

这个「代码即动作」的取向与 smolagents 同源，但差别很大：smolagents 用 AST 解释器裁剪能力，
browser-use 用一个**独立子进程 + 环境变量白名单 + 敏感值脱敏**来约束，不裁剪语言本身。

盘点方法：先用 `dump_python_defs.py` 建定义索引（`ast` 解析），再按目录职责归并成
机制簇。篇数是归并结果，不是按文件数折算。

源码锚点：`browser-use@456ff150afb8fd09fc32db1a0d4d64bd85c505cd`。
"""

GROUP_ORDER = [
    "A-Agent 入口与生命周期",
    "B-循环、投递与预算",
    "C-REPL 会话与工作进程",
    "D-动作、观测与结果",
    "E-浏览器后端",
    "F-域名与网络策略",
    "G-MCP 与 CLI 接入",
    "H-遥测与录制",
    "I-兼容、配置与自评",
]

MECHANISM_SPECS = [
    # ============ A-Agent 入口与生命周期 ============
    {
        "feature_group": "A-Agent 入口与生命周期",
        "feature": "agent-entry",
        "title": "Agent 门面与运行生命周期",
        "page_file": "a01-agent-entry.md",
        "boundary": "负责 Agent 如何组装模型与浏览器、启动与收尾、以及运行结果如何包装；"
                    "不负责循环内部（b01）、不负责浏览器后端细节（e01）。",
        "refs": [
            {"path": "browser_use/agent/service.py",
             "symbols": ["Agent", "run", "build"]},
            {"path": "browser_use/agent/service.py",
             "symbols": ["close", "__aenter__", "_shutdown"]},
        ],
    },
    {
        "feature_group": "A-Agent 入口与生命周期",
        "feature": "agent-construction",
        "title": "Agent 构造期校验与参数透传",
        "page_file": "a02-agent-construction.md",
        "boundary": "负责构造期的参数检查、任务文本校验与未知属性转发；"
                    "不负责运行（a01）、不负责 0.x 兼容错误（i01）。",
        "refs": [
            {"path": "browser_use/agent/service.py",
             "symbols": ["_check_task_arg", "_check_task", "__getattr__"]},
            {"path": "browser_use/agent/service.py",
             "symbols": ["workspace", "add_new_task"]},
        ],
    },

    # ============ B-循环、投递与预算 ============
    {
        "feature_group": "B-循环、投递与预算",
        "feature": "agent-loop",
        "title": "循环骨架与 python 工具",
        "page_file": "b01-agent-loop.md",
        "boundary": "负责一次循环的构成：模型请求、python 工具调用、结束信号；"
                    "不负责投递校验（b02）、不负责压缩（b03）。",
        "refs": [
            {"path": "browser_use/agent/loop.py",
             "symbols": ["BrowserLoop", "python", "run"]},
            {"path": "browser_use/agent/loop.py",
             "symbols": ["build_agent", "model_kind"]},
        ],
    },
    {
        "feature_group": "B-循环、投递与预算",
        "feature": "delivery-guards",
        "title": "投递校验与预算",
        "page_file": "b02-delivery-guards.md",
        "boundary": "负责结束投递的校验、时间预算与部分结果的固定；"
                    "不负责循环骨架（b01）、不负责图像裁剪（b03）。",
        "refs": [
            {"path": "browser_use/agent/loop.py",
             "symbols": ["Session", "check_delivery", "check_split_delivery"]},
            {"path": "browser_use/agent/loop.py",
             "symbols": ["delivery_tool_choice", "_sendable", "last_text"]},
        ],
    },
    {
        "feature_group": "B-循环、投递与预算",
        "feature": "compaction-and-images",
        "title": "上下文压缩与图像裁剪",
        "page_file": "b03-compaction-and-images.md",
        "boundary": "负责历史压缩的时机与惰性触发、以及图像部件的裁剪；"
                    "不负责投递（b02）、不负责录制（b04）。",
        "refs": [
            {"path": "browser_use/agent/loop.py",
             "symbols": ["Compaction", "LazyCompaction", "_compact"]},
            {"path": "browser_use/agent/loop.py",
             "symbols": ["_prune_images", "_estimate", "redact_part"]},
        ],
    },
    {
        "feature_group": "B-循环、投递与预算",
        "feature": "streaming-recording",
        "title": "响应流改造与请求录制",
        "page_file": "b04-streaming-recording.md",
        "boundary": "负责在不改模型实现的前提下包一层响应流与请求录制；"
                    "不负责循环（b01）、不负责产品遥测（h01）。",
        "refs": [
            {"path": "browser_use/agent/loop.py",
             "symbols": ["StreamResponses", "StreamedRequest", "_cut_after_response"]},
            {"path": "browser_use/agent/loop.py",
             "symbols": ["Recording", "thinking_and_text", "usage_and_cost"]},
        ],
    },

    # ============ C-REPL 会话与工作进程 ============
    {
        "feature_group": "C-REPL 会话与工作进程",
        "feature": "repl-session",
        "title": "REPL 父进程侧与会话管理",
        "page_file": "c01-repl-session.md",
        "boundary": "负责父进程如何起停工作进程、外发调用与清理；"
                    "不负责工作进程内部（c02）、不负责脱敏（c03）。",
        "refs": [
            {"path": "browser_use/repl/session.py",
             "symbols": ["Repl", "start", "call"]},
            {"path": "browser_use/repl/session.py",
             "symbols": ["check_mode", "check_loop", "_kill_tree", "kill_workers"]},
        ],
    },
    {
        "feature_group": "C-REPL 会话与工作进程",
        "feature": "repl-worker",
        "title": "工作进程与单元执行",
        "page_file": "c02-repl-worker.md",
        "boundary": "负责工作进程如何编译执行单元、暴露辅助函数、上报统计与错误；"
                    "不负责父进程（c01）、不负责脱敏（c03）。",
        "refs": [
            {"path": "browser_use/repl/worker.py",
             "symbols": ["CellStats", "_compile", "_run"]},
            {"path": "browser_use/repl/worker.py",
             "symbols": ["_watch_parent", "_redact_between_cells", "main"]},
        ],
    },
    {
        "feature_group": "C-REPL 会话与工作进程",
        "feature": "secret-redaction",
        "title": "敏感值脱敏与环境白名单",
        "page_file": "c03-secret-redaction.md",
        "boundary": "负责敏感值在哪些通道被替换、以及工作进程拿到的环境变量范围；"
                    "不负责工作进程执行（c02）、不负责域名策略（f01）。",
        "refs": [
            {"path": "browser_use/repl/session.py",
             "symbols": ["scoped_secrets", "redact", "redact_forms", "secret_forms"]},
            {"path": "browser_use/repl/session.py", "symbols": ["worker_env", "clip"]},
        ],
    },

    # ============ D-动作、观测与结果 ============
    {
        "feature_group": "D-动作、观测与结果",
        "feature": "tools-and-actions",
        "title": "工具注册与动作结果",
        "page_file": "d01-tools-and-actions.md",
        "boundary": "负责工具如何登记成可调用集合、动作结果的字段与校验；"
                    "不负责无障碍树（d02）、不负责历史记录（d03）。",
        "refs": [
            {"path": "browser_use/tools.py",
             "symbols": ["Tools", "action", "ActionResult"]},
            {"path": "browser_use/tools.py",
             "symbols": ["_check_done_results", "_without_0x_defaults"]},
        ],
    },
    {
        "feature_group": "D-动作、观测与结果",
        "feature": "accessibility-tree",
        "title": "无障碍树观测与交互原语",
        "page_file": "d02-accessibility-tree.md",
        "boundary": "负责从 CDP 取无障碍树、稳定等待、可见文本提取与基于坐标的点击输入；"
                    "不负责工具注册（d01）、不负责工作进程（c02）。",
        "refs": [
            {"path": "browser_use/ax.py",
             "symbols": ["AxHelpers", "state", "settle", "drain_events"]},
            {"path": "browser_use/ax.py",
             "symbols": ["click", "type", "_describe", "_capture"]},
        ],
    },
    {
        "feature_group": "D-动作、观测与结果",
        "feature": "history-and-result",
        "title": "历史列表与结果提取",
        "page_file": "d03-history-and-result.md",
        "boundary": "负责运行历史的字段构成与结果提取入口（最终文本、结构化输出、成功判定）；"
                    "不负责工具结果（d01）、不负责自评（i03）。",
        "refs": [
            {"path": "browser_use/agent/views.py",
             "symbols": ["AgentHistoryList", "final_result", "is_successful"]},
            {"path": "browser_use/agent/views.py",
             "symbols": ["Usage", "load_from_file", "structured_output"]},
        ],
    },

    # ============ E-浏览器后端 ============
    {
        "feature_group": "E-浏览器后端",
        "feature": "browser-backends",
        "title": "浏览器后端选择与连接",
        "page_file": "e01-browser-backends.md",
        "boundary": "负责 Browser 如何按参数选择云端、系统 Chrome、内置 Chromium 或已有 CDP 地址，"
                    "以及启动与重连；不负责本地进程管理（e02）、不负责域名策略（f01）。",
        "refs": [
            {"path": "browser_use/browser/browser.py",
             "symbols": ["Browser", "start", "reattach"]},
            {"path": "browser_use/browser/browser.py",
             "symbols": ["is_cloud_cdp", "_kind", "_ensure_daemon"]},
        ],
    },
    {
        "feature_group": "E-浏览器后端",
        "feature": "local-browser",
        "title": "本地浏览器进程与用户目录",
        "page_file": "e02-local-browser.md",
        "boundary": "负责本地 Chromium/Chrome 的可执行查找、用户数据目录选择与调试端口约束；"
                    "不负责后端选择（e01）、不负责下载与代理（e03）。",
        "refs": [
            {"path": "browser_use/browser/local.py",
             "symbols": ["LocalBrowser", "start", "find_binary"]},
            {"path": "browser_use/browser/local.py",
             "symbols": ["resolve_profile", "require_debug_port", "chrome_args"]},
        ],
    },
    {
        "feature_group": "E-浏览器后端",
        "feature": "downloads-and-proxy",
        "title": "下载与代理轮换",
        "page_file": "e03-downloads-and-proxy.md",
        "boundary": "负责下载记录的收取与落盘、以及代理轮换的触发与凭据来源；"
                    "不负责后端选择（e01）、不负责域名策略（f01）。",
        "refs": [
            {"path": "browser_use/browser/browser.py",
             "symbols": ["list_downloads", "download_files", "download_records"]},
            {"path": "browser_use/browser/browser.py",
             "symbols": ["rotate_proxy", "can_rotate_proxy"]},
        ],
    },

    # ============ F-域名与网络策略 ============
    {
        "feature_group": "F-域名与网络策略",
        "feature": "domain-rules",
        "title": "域名规则与守卫进程",
        "page_file": "f01-domain-rules.md",
        "boundary": "负责域名允许/阻断规则的匹配方式、以及独立守卫进程如何拦截跳转；"
                    "不负责 TLS（f02）、不负责脱敏（c03）。",
        "refs": [
            {"path": "browser_use/domains.py",
             "symbols": ["DomainRules", "of", "blocked"]},
            {"path": "browser_use/domains.py",
             "symbols": ["Guard", "pattern_matches", "sweep"]},
        ],
    },
    {
        "feature_group": "F-域名与网络策略",
        "feature": "network-and-url",
        "title": "URL 规范化与 TLS 请求",
        "page_file": "f02-network-and-url.md",
        "boundary": "负责 URL 解析与主机规范化、以及带 TLS 配置的请求构造；"
                    "不负责域名规则（f01）、不负责搜索（f03）。",
        "refs": [
            {"path": "browser_use/domains.py",
             "symbols": ["Url", "canonical_host", "parse_url"]},
            {"path": "browser_use/utils.py",
             "symbols": ["tls_context", "install_tls_opener", "urlopen_keyed"]},
        ],
    },
    {
        "feature_group": "F-域名与网络策略",
        "feature": "web-search",
        "title": "网页搜索接入",
        "page_file": "f03-web-search.md",
        "boundary": "负责把搜索能力包装成可调用的网络搜索入口与失败面；"
                    "不负责 URL 规范化（f02）、不负责工具注册（d01）。",
        "refs": [
            {"path": "browser_use/search.py", "symbols": ["WebSearch", "SearchError"]},
            {"path": "browser_use/agent/service.py", "symbols": ["_web_search"]},
        ],
    },

    # ============ G-MCP 与 CLI 接入 ============
    {
        "feature_group": "G-MCP 与 CLI 接入",
        "feature": "cli-mcp-server",
        "title": "CLI MCP 服务端",
        "page_file": "g01-cli-mcp-server.md",
        "boundary": "负责把浏览器能力以 MCP 工具形式暴露、以及单元执行与登录流程；"
                    "不负责技能安装（g02）、不负责 REPL 内部（c02）。",
        "refs": [
            {"path": "browser_use/mcp/cli_mcp.py",
             "symbols": ["CLIMCPServer", "run", "_tool_definitions"]},
            {"path": "browser_use/mcp/cli_mcp.py",
             "symbols": ["_Cell", "_Login", "_run_cell"]},
        ],
    },
    {
        "feature_group": "G-MCP 与 CLI 接入",
        "feature": "skills-packaging",
        "title": "技能包与安装",
        "page_file": "g02-skills-packaging.md",
        "boundary": "负责随包发布的 agent 技能文本与安装流程；"
                    "不负责 MCP 服务端（g01）、不负责兼容层（i01）。",
        "refs": [
            {"path": "browser_use/skills/browser_use.py",
             "symbols": ["as_browser_use_skill", "skill_text"]},
            {"path": "browser_use/skills/install.py", "symbols": ["handle", "_build_parser"]},
        ],
    },

    # ============ H-遥测与录制 ============
    {
        "feature_group": "H-遥测与录制",
        "feature": "product-telemetry",
        "title": "产品遥测与离线投递",
        "page_file": "h01-product-telemetry.md",
        "boundary": "负责遥测事件的采集、禁用开关、设备标识与离线补投；"
                    "不负责运行录制（h02）、不负责反馈（h03）。",
        "refs": [
            {"path": "browser_use/telemetry/service.py",
             "symbols": ["ProductTelemetry", "capture", "telemetry"]},
            {"path": "browser_use/telemetry/service.py",
             "symbols": ["get_or_create_device_id", "_send_outbox"]},
        ],
    },
    {
        "feature_group": "H-遥测与录制",
        "feature": "run-recorder",
        "title": "运行录制与步骤归集",
        "page_file": "h02-run-recorder.md",
        "boundary": "负责把一次运行的阶段、单元、投递与耗时归集成可上报的结构；"
                    "不负责传输（h01）、不负责历史结果（d03）。",
        "refs": [
            {"path": "browser_use/telemetry/recorder.py",
             "symbols": ["RunRecorder", "build", "Step"]},
            {"path": "browser_use/telemetry/recorder.py",
             "symbols": ["_timings", "_hosts", "_struggle"]},
        ],
    },
    {
        "feature_group": "H-遥测与录制",
        "feature": "feedback-and-auth-events",
        "title": "反馈与登录事件",
        "page_file": "h03-feedback-and-auth-events.md",
        "boundary": "负责反馈提交入口与登录相关事件的记录；"
                    "不负责遥测传输（h01）、不负责录制（h02）。",
        "refs": [
            {"path": "browser_use/telemetry/feedback.py", "symbols": ["feedback", "RunReports"]},
            {"path": "browser_use/telemetry/auth_events.py", "symbols": ["capture", "keyed_success"]},
        ],
    },

    # ============ I-兼容、配置与自评 ============
    {
        "feature_group": "I-兼容、配置与自评",
        "feature": "legacy-compat",
        "title": "0.x 兼容层与已移除名称",
        "page_file": "i01-legacy-compat.md",
        "boundary": "负责 0.x 名称被访问时如何给出指向 1.0 答案的错误、以及已移除字段的拦截；"
                    "不负责构造期校验（a02）、不负责配置（i02）。",
        "refs": [
            {"path": "browser_use/legacy.py",
             "symbols": ["removed_name", "removed_module", "install"]},
            {"path": "browser_use/tools.py", "symbols": ["RemovedField", "_no_removed_fields"]},
        ],
    },
    {
        "feature_group": "I-兼容、配置与自评",
        "feature": "config-and-errors",
        "title": "配置加载与错误类型",
        "page_file": "i02-config-and-errors.md",
        "boundary": "负责环境配置的读取、运行环境探测与错误类型的层级；"
                    "不负责兼容层（i01）、不负责自评（i03）。",
        "refs": [
            {"path": "browser_use/config.py", "symbols": ["Config", "load_env"]},
            {"path": "browser_use/exceptions.py",
             "symbols": ["MissingAPIKeyError", "BrowserConfigError", "BrowserConnectionLost"]},
        ],
    },
    {
        "feature_group": "I-兼容、配置与自评",
        "feature": "self-judging",
        "title": "运行后自评",
        "page_file": "i03-self-judging.md",
        "boundary": "负责运行结束后由模型（或缓存结果）判定任务是否达成的机制；"
                    "不负责结果提取（d03）、不负责循环（b01）。",
        "refs": [
            {"path": "browser_use/agent/judge.py",
             "symbols": ["Judgement", "judge_run", "judge_cached"]},
            {"path": "browser_use/agent/service.py", "symbols": ["_judge"]},
        ],
    },
]
