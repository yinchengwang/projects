# -*- coding: utf-8 -*-
"""open-interpreter 机制清单（阶段 2B 第六个项目，也是 2B 最后一个）。

盘点范围：`codex-rs/` —— 这是从 OpenAI Codex fork 出来的 Rust workspace（130+ crate、
4820 个 rs 文件）。与 2B 前几个项目的关键差别：它**不是自己发明一套 agent 循环**，
而是把「让便宜模型发挥最大效果」这件事落成**多种外部队列（harness）的运行时模拟**，
再加上一整套生产级沙箱、执行策略、会话持久化设施。

盘点方法：Rust 无现成定义索引器，用「正则抽取 `pub struct/enum/trait/fn` + 人工核对」
建符号清单，再用 `gen_manifest.py --verify` 的 refs 校验兜底。篇数是按机制归并的结果，
不是按 crate 数折算（130 个 crate 里绝大多数是协议模型、mock 客户端与测试支撑）。

源码锚点：`open-interpreter@412f37d08908db1d1d2b72eb0259ae97aea78f2b`。
"""

GROUP_ORDER = [
    "A-Harness 与入口",
    "B-执行策略与审批",
    "C-沙箱",
    "D-工具系统",
    "E-协议条目与动态工具",
    "F-钩子与技能",
    "G-会话持久化与上下文",
    "H-模型与提供方",
    "I-接入面",
    "J-工作区与可观测",
]

MECHANISM_SPECS = [
    # ============ A-Harness 与入口 ============
    {
        "feature_group": "A-Harness 与入口",
        "feature": "harness-model",
        "title": "Harness 模型与名称解析",
        "page_file": "a01-harness-model.md",
        "boundary": "负责 harness 的枚举、配置名到变体的解析、以及各类判定谓词；"
                    "不负责 Codex 协议兼容（a02）、不负责命令行入口（a03）。",
        "refs": [
            {"path": "codex-rs/tools/src/harness.rs",
             "symbols": ["Harness", "from_config_name"]},
            {"path": "codex-rs/tools/src/harness.rs",
             "symbols": ["is_claude_code", "is_kimi_code", "is_minimal", "is_swe_agent"]},
        ],
    },
    {
        "feature_group": "A-Harness 与入口",
        "feature": "codex-compat",
        "title": "Codex 协议与 SDK 兼容",
        "page_file": "a02-codex-compat.md",
        "boundary": "负责与 Codex exec 协议/SDK 的兼容面；"
                    "不负责 harness 枚举（a01）、不负责 ACP 服务端（i01）。",
        "refs": [
            {"path": "codex-rs/chat-wire-compat/src/lib.rs",
             "symbols": ["ChatCompletionsCompatClient"]},
            {"path": "codex-rs/chat-wire-compat/src/request.rs",
             "symbols": ["ToolKinds", "ToolOutputKind"]},
            {"path": "codex-rs/acp-server/src/lib.rs",
             "symbols": ["AppServerAcpAgent", "CONFIG_ID_HARNESS"]},
        ],
    },
    {
        "feature_group": "A-Harness 与入口",
        "feature": "cli-entry",
        "title": "命令行入口与子命令分派",
        "page_file": "a03-cli-entry.md",
        "boundary": "负责 CLI 的参数结构与子命令分派、以及各入口模式的选择；"
                    "不负责 harness 解析（a01）、不负责 app-server（i01）。",
        "refs": [
            {"path": "codex-rs/cli/src/main.rs",
             "symbols": ["MultitoolCli", "Subcommand", "DebugSubcommand"]},
            {"path": "codex-rs/cli/src/main.rs",
             "symbols": ["ALLOCATOR", "CompletionCommand"]},
        ],
    },

    {
        "feature_group": "A-Harness 与入口",
        "feature": "harness-emulation",
        "title": "Harness 请求模拟与路由",
        "page_file": "a04-harness-emulation.md",
        "boundary": "负责把一次对话构造成某个特定 agent 命令行工具会发出的请求形状（系统提示、工具集、消息形态、请求头），以及按 harness 选择传输路由；"
                    "不负责枚举解析（a01）、不负责 Codex 协议兼容（a02）。",
        "refs": [
            {"path": "codex-rs/core/src/harness/request.rs",
             "symbols": ["ChatHarnessRequest", "build_chat_harness_request",
                         "apply_chat_harness_postprocess"]},
            {"path": "codex-rs/core/src/harness/routing.rs",
             "symbols": ["resolve_stream_transport_route", "harness_config_name",
                         "ClaudeCodeProfileRoute"]},
            {"path": "codex-rs/core/src/harness/guidance.rs",
             "symbols": ["guidance_for_harness"]},
        ],
    },
    {
        "feature_group": "A-Harness 与入口",
        "feature": "minimal-harness",
        "title": "最小 harness 与消息转换",
        "page_file": "a05-minimal-harness.md",
        "boundary": "负责 `minimal` 这一个 harness 的请求构造细节：系统提示、工具集、消息内容转换与工具调用收尾；"
                    "不负责通用请求装配（a04）、不负责其它 harness（a01）。",
        "refs": [
            {"path": "codex-rs/core/src/harness/minimal.rs",
             "symbols": ["MINIMAL_SYSTEM_PROMPT", "build_request", "build_tools"]},
            {"path": "codex-rs/core/src/harness/minimal.rs",
             "symbols": ["flush_pending_tool_calls", "convert_message_content",
                         "minimal_tool_output_content"]},
        ],
    },

    # ============ B-执行策略与审批 ============
    {
        "feature_group": "B-执行策略与审批",
        "feature": "exec-policy-rules",
        "title": "执行策略规则与匹配",
        "page_file": "b01-exec-policy-rules.md",
        "boundary": "负责策略规则的数据结构（前缀规则、网络规则）与匹配语义；"
                    "不负责解析（b02）、不负责求值与修订（b03）。",
        "refs": [
            {"path": "codex-rs/execpolicy/src/rule.rs",
             "symbols": ["PrefixRule", "NetworkRule", "Decision"]},
            {"path": "codex-rs/execpolicy/src/rule.rs",
             "symbols": ["PrefixPattern", "PatternToken", "normalize_network_rule_host"]},
        ],
    },
    {
        "feature_group": "B-执行策略与审批",
        "feature": "exec-policy-parser",
        "title": "策略文件解析与校验",
        "page_file": "b02-exec-policy-parser.md",
        "boundary": "负责策略文件（Starlark 风格）的解析、词法与示例校验；"
                    "不负责规则语义（b01）、不负责求值（b03）。",
        "refs": [
            {"path": "codex-rs/execpolicy/src/parser.rs",
             "symbols": ["PolicyParser", "PolicyBuilder", "PendingExampleValidation"]},
            {"path": "codex-rs/execpolicy/src/parser.rs",
             "symbols": ["parse_pattern", "parse_pattern_token", "parse_network_rule_decision"]},
        ],
    },
    {
        "feature_group": "B-执行策略与审批",
        "feature": "policy-evaluation",
        "title": "策略求值与规则修订",
        "page_file": "b03-policy-evaluation.md",
        "boundary": "负责给定一次命令如何得到决策、启发式兜底、以及运行中追加规则的落盘；"
                    "不负责规则结构（b01）、不负责审批流程（b04）。",
        "refs": [
            {"path": "codex-rs/execpolicy/src/policy.rs",
             "symbols": ["Policy", "Evaluation", "MatchOptions", "HeuristicsFallback"]},
            {"path": "codex-rs/execpolicy/src/amend.rs",
             "symbols": ["blocking_append_allow_prefix_rule", "blocking_append_network_rule"]},
        ],
    },
    {
        "feature_group": "B-执行策略与审批",
        "feature": "approvals",
        "title": "审批档案与风险判定",
        "page_file": "b04-approvals.md",
        "boundary": "负责权限档案、升级授权、网络审批上下文与风险等级；"
                    "不负责策略求值（b03）、不负责沙箱（C 组）。",
        "refs": [
            {"path": "codex-rs/protocol/src/approvals.rs",
             "symbols": ["ResolvedPermissionProfile", "EscalationPermissions",
                         "ExecPolicyAmendment"]},
            {"path": "codex-rs/protocol/src/approvals.rs",
             "symbols": ["GuardianRiskLevel", "NetworkApprovalContext", "NetworkPolicyRuleAction"]},
        ],
    },

    # ============ C-沙箱 ============
    {
        "feature_group": "C-沙箱",
        "feature": "sandbox-manager",
        "title": "沙箱管理器与平台选择",
        "page_file": "c01-sandbox-manager.md",
        "boundary": "负责按平台与偏好选择沙箱实现、以及把命令包装成沙箱命令；"
                    "不负责各平台实现细节（c02/c03/c04）。",
        "refs": [
            {"path": "codex-rs/sandboxing/src/manager.rs",
             "symbols": ["SandboxablePreference", "get_platform_sandbox", "SandboxCommand"]},
            {"path": "codex-rs/sandboxing/src/manager.rs",
             "symbols": ["SandboxExecRequest", "SandboxTransformError",
                         "with_managed_mitm_ca_readable_root"]},
        ],
    },
    {
        "feature_group": "C-沙箱",
        "feature": "macos-seatbelt",
        "title": "macOS Seatbelt 沙箱",
        "page_file": "c02-macos-seatbelt.md",
        "boundary": "负责 macOS 上 Seatbelt 配置文件的构造与策略选择；"
                    "不负责管理器（c01）、不负责 Linux（c03）。",
        "refs": [
            {"path": "codex-rs/sandboxing/src/seatbelt.rs",
             "symbols": ["MacosSeatbeltProfile", "MACOS_SEATBELT_BASE_POLICY",
                         "MACOS_SEATBELT_NETWORK_POLICY"]},
            {"path": "codex-rs/sandboxing/src/seatbelt.rs",
             "symbols": ["is_loopback_host", "proxy_loopback_ports_from_env",
                         "SeatbeltPreparationError"]},
        ],
    },
    {
        "feature_group": "C-沙箱",
        "feature": "linux-sandbox",
        "title": "Linux Landlock 与 bwrap 沙箱",
        "page_file": "c03-linux-sandbox.md",
        "boundary": "负责 Linux 上的沙箱实现：bwrap 参数构造、Landlock 规则与挂载处理；"
                    "不负责管理器（c01）、不负责 Windows（c04）。",
        "refs": [
            {"path": "codex-rs/linux-sandbox/src/bwrap.rs",
             "symbols": ["BwrapOptions", "BwrapArgs", "BwrapNetworkMode"]},
            {"path": "codex-rs/linux-sandbox/src/bwrap.rs",
             "symbols": ["LINUX_PLATFORM_DEFAULT_READ_ROOTS", "SyntheticMountTarget",
                         "FileIdentity"]},
        ],
    },
    {
        "feature_group": "C-沙箱",
        "feature": "windows-sandbox",
        "title": "Windows 受限令牌沙箱",
        "page_file": "c04-windows-sandbox.md",
        "boundary": "负责 Windows 上基于受限令牌与提权后端的沙箱，以及文件系统覆盖解析；"
                    "不负责管理器（c01）、不负责 Linux（c03）。",
        "refs": [
            {"path": "codex-rs/sandboxing/src/windows.rs",
             "symbols": ["WindowsSandboxFilesystemOverrides",
                         "windows_sandbox_uses_elevated_backend",
                         "resolve_windows_restricted_token_filesystem_overrides"]},
            {"path": "codex-rs/sandboxing/src/windows.rs",
             "symbols": ["permission_profile_supports_windows_restricted_token_sandbox",
                         "has_reopened_writable_descendant"]},
        ],
    },

    # ============ D-工具系统 ============
    {
        "feature_group": "D-工具系统",
        "feature": "tool-call",
        "title": "工具调用与环境抽象",
        "page_file": "d01-tool-call.md",
        "boundary": "负责一次工具调用的表示、环境能力与历史加载抽象；"
                    "不负责 schema（d02）、不负责具体工具（d03/d04/d06）。",
        "refs": [
            {"path": "codex-rs/tools/src/tool_call.rs",
             "symbols": ["ToolCall", "ToolCallSource", "ToolEnvironment"]},
            {"path": "codex-rs/tools/src/tool_call.rs",
             "symbols": ["TurnItemEmitter", "ExtensionTurnItem", "HistoryLoader"]},
        ],
    },
    {
        "feature_group": "D-工具系统",
        "feature": "json-schema",
        "title": "工具输入 schema 的清洗",
        "page_file": "d02-json-schema.md",
        "boundary": "负责把模型或 MCP 给的 JSON Schema 清洗成可下发的形状；"
                    "不负责调用（d01）、不负责输出 schema（d05）。",
        "refs": [
            {"path": "codex-rs/tools/src/json_schema.rs",
             "symbols": ["parse_tool_input_schema", "sanitize_json_schema",
                         "prepare_tool_input_schema"]},
            {"path": "codex-rs/tools/src/json_schema.rs",
             "symbols": ["DEFINITION_TABLE_KEYS", "has_composition_keyword", "sanitize_schema_table"]},
        ],
    },
    {
        "feature_group": "D-工具系统",
        "feature": "code-mode",
        "title": "代码模式下的工具定义",
        "page_file": "d03-code-mode.md",
        "boundary": "负责把工具规格转换成代码模式下的定义与描述增强；"
                    "不负责调用（d01）、不负责 MCP 工具（d04）。",
        "refs": [
            {"path": "codex-rs/tools/src/code_mode.rs",
             "symbols": ["augment_tool_spec_for_code_mode",
                         "collect_code_mode_tool_definitions",
                         "code_mode_name_for_tool_name"]},
            {"path": "codex-rs/tools/src/code_mode.rs",
             "symbols": ["tool_spec_to_code_mode_tool_definition",
                         "augmented_description_for_spec"]},
        ],
    },
    {
        "feature_group": "D-工具系统",
        "feature": "mcp-tool",
        "title": "MCP 工具的转换与上限",
        "page_file": "d04-mcp-tool.md",
        "boundary": "负责把 MCP 工具描述转换成内部工具，含描述长度上限与输出 schema；"
                    "不负责传输（i03）、不负责输入 schema 清洗（d02）。",
        "refs": [
            {"path": "codex-rs/tools/src/mcp_tool.rs",
             "symbols": ["MAX_MCP_TOOL_DESCRIPTION_BYTES", "parse_mcp_tool",
                         "parse_mcp_tool_with_description_limit"]},
            {"path": "codex-rs/tools/src/mcp_tool.rs",
             "symbols": ["parse_agent_plugin_mcp_tool", "mcp_call_tool_result_output_schema"]},
        ],
    },
    {
        "feature_group": "D-工具系统",
        "feature": "output-schema",
        "title": "工具输出 schema 与存储",
        "page_file": "d05-output-schema.md",
        "boundary": "负责工具输出 schema 的表示与存放；"
                    "不负责输入 schema（d02）、不负责调用（d01）。",
        "refs": [
            {"path": "codex-rs/tools/src/output_schema.rs",
             "symbols": ["ToolOutputSchema", "OutputSchemaStorage"]},
            {"path": "codex-rs/tools/src/tool_definition.rs", "symbols": ["ToolDefinition"]},
        ],
    },
    {
        "feature_group": "D-工具系统",
        "feature": "apply-patch",
        "title": "补丁解析与应用",
        "page_file": "d06-apply-patch.md",
        "boundary": "负责解析补丁文本并安全地应用到文件系统；"
                    "不负责沙箱（C 组）、不负责 Git 工作树（j01）。",
        "refs": [
            {"path": "codex-rs/apply-patch/src/lib.rs",
             "symbols": ["ApplyPatchArgs", "ApplyPatchFileChange", "apply_patch_file_update_mode_from_env"]},
            {"path": "codex-rs/apply-patch/src/lib.rs",
             "symbols": ["ApplyPatchOptions", "ApplyPatchError", "CODEX_APPLY_PATCH_PRESERVE_LINE_ENDINGS_ENV_VAR"]},
        ],
    },

    # ============ E-协议条目与动态工具 ============
    {
        "feature_group": "E-协议条目与动态工具",
        "feature": "turn-items",
        "title": "回合条目模型",
        "page_file": "e01-turn-items.md",
        "boundary": "负责回合中各类条目的数据结构（用户消息、函数输出、钩子片段等）；"
                    "不负责动态工具（e02）、不负责审批档案（b04）。",
        "refs": [
            {"path": "codex-rs/protocol/src/items.rs",
             "symbols": ["TurnItem", "UserMessageItem", "FunctionCallOutputItem"]},
            {"path": "codex-rs/protocol/src/items.rs",
             "symbols": ["HookPromptItem", "AgentMessageItem", "AsyncUserInputQuestion"]},
        ],
    },
    {
        "feature_group": "E-协议条目与动态工具",
        "feature": "dynamic-tools",
        "title": "动态工具规格",
        "page_file": "e02-dynamic-tools.md",
        "boundary": "负责运行时下发的工具规格（含命名空间形态）与调用请求/响应；"
                    "不负责 MCP 工具（d04）、不负责回合条目（e01）。",
        "refs": [
            {"path": "codex-rs/protocol/src/dynamic_tools.rs",
             "symbols": ["DynamicToolSpec", "DynamicToolNamespaceSpec", "normalize_dynamic_tool_specs"]},
            {"path": "codex-rs/protocol/src/dynamic_tools.rs",
             "symbols": ["DynamicToolCallRequest", "DynamicToolResponse", "LegacyDynamicToolSpec"]},
        ],
    },

    # ============ F-钩子与技能 ============
    {
        "feature_group": "F-钩子与技能",
        "feature": "hooks-registry",
        "title": "钩子注册与触发",
        "page_file": "f01-hooks-registry.md",
        "boundary": "负责钩子的发现、规则配置与触发后的结果处理；"
                    "不负责输出溢写（f02）、不负责技能（f03）。",
        "refs": [
            {"path": "codex-rs/hooks/src/registry.rs",
             "symbols": ["Hooks", "HooksConfig", "list_hooks"]},
            {"path": "codex-rs/hooks/src/types.rs",
             "symbols": ["Hook", "HookPayload", "HookEvent", "HookResult"]},
        ],
    },
    {
        "feature_group": "F-钩子与技能",
        "feature": "hook-output-spill",
        "title": "钩子输出溢写",
        "page_file": "f02-hook-output-spill.md",
        "boundary": "负责钩子输出超限时落盘并只回预览；"
                    "不负责注册（f01）、不负责上下文片段（g06）。",
        "refs": [
            {"path": "codex-rs/hooks/src/output_spill.rs",
             "symbols": ["HookOutputSpiller", "DEFAULT_HOOK_OUTPUT_TOKEN_LIMIT",
                         "spilled_hook_output_preview"]},
            {"path": "codex-rs/hooks/src/output_spill.rs",
             "symbols": ["AdditionalContextLimit", "AdditionalContext", "HOOK_OUTPUTS_DIR"]},
        ],
    },
    {
        "feature_group": "F-钩子与技能",
        "feature": "skills-loading",
        "title": "技能加载与 frontmatter 解析",
        "page_file": "f03-skills-loading.md",
        "boundary": "负责从目录加载技能、解析 frontmatter 并维持快照缓存；"
                    "不负责提及识别（f04）、不负责钩子（f01）。",
        "refs": [
            {"path": "codex-rs/skills/src/loading.rs",
             "symbols": ["SkillRootLoader", "LoadedSkills", "SkillRootSnapshotCache"]},
            {"path": "codex-rs/skills/src/parser.rs",
             "symbols": ["parse_skill_frontmatter_metadata", "MAX_NAME_LEN", "SkillParseError"]},
        ],
    },
    {
        "feature_group": "F-钩子与技能",
        "feature": "skill-selection",
        "title": "技能提及与隐式调用识别",
        "page_file": "f04-skill-selection.md",
        "boundary": "负责从命令与文本中识别技能提及、选定技能、以及隐式访问检测；"
                    "不负责加载（f03）、不负责工具提及的其它类别（d04）。",
        "refs": [
            {"path": "codex-rs/skills/src/mentions.rs",
             "symbols": ["ToolMentions", "tool_kind_for_path", "SKILL_PATH_PREFIX"]},
            {"path": "codex-rs/skills/src/invocation.rs",
             "symbols": ["detect_implicit_skill_invocation_for_command",
                         "tokenize_command", "detect_skill_script_run"]},
            {"path": "codex-rs/skills/src/selection.rs",
             "symbols": ["collect_explicit_skill_mentions", "select_skills_from_mentions"]},
        ],
    },

    # ============ G-会话持久化与上下文 ============
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "rollout-recorder",
        "title": "会话滚动记录",
        "page_file": "g01-rollout-recorder.md",
        "boundary": "负责会话历史的追加记录、命令协议与写入任务；"
                    "不负责压缩（g02）、不负责索引（g03）。",
        "refs": [
            {"path": "codex-rs/rollout/src/recorder.rs",
             "symbols": ["RolloutRecorder", "RolloutRecorderParams", "RolloutWriterTask"]},
            {"path": "codex-rs/rollout/src/recorder.rs",
             "symbols": ["RolloutCmd", "ThreadListArchiveFilter", "ThreadListRepairMode"]},
        ],
    },
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "rollout-compression",
        "title": "会话文件压缩",
        "page_file": "g02-rollout-compression.md",
        "boundary": "负责会话记录文件的压缩触发、原子替换与压缩后读取；"
                    "不负责记录（g01）、不负责搜索（g03）。",
        "refs": [
            {"path": "codex-rs/rollout/src/compression.rs",
             "symbols": ["RolloutCompressionTrigger", "spawn_rollout_compression_worker",
                         "materialize_rollout_for_append_blocking"]},
            {"path": "codex-rs/rollout/src/compression.rs",
             "symbols": ["compressed_rollout_path", "COMPRESSED_SUFFIX", "MAX_NOT_FOUND_RETRIES"]},
        ],
    },
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "session-index",
        "title": "会话索引与倒序扫描",
        "page_file": "g03-session-index.md",
        "boundary": "负责会话索引的维护与从文件尾部倒序扫描查找；"
                    "不负责记录（g01）、不负责压缩（g02）。",
        "refs": [
            {"path": "codex-rs/rollout/src/session_index.rs",
             "symbols": ["SessionIndexEntry", "scan_index_from_end", "SESSION_INDEX_FILE"]},
            {"path": "codex-rs/rollout/src/reverse_jsonl_scanner.rs",
             "symbols": ["ReverseJsonlScanner", "ScanOutcome", "READ_CHUNK_SIZE"]},
        ],
    },
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "compaction",
        "title": "上下文压缩",
        "page_file": "g04-compaction.md",
        "boundary": "负责历史压缩的触发、提示构造与压缩后元数据；"
                    "不负责会话文件压缩（g02）、不负责上下文片段（g06）。",
        "refs": [
            {"path": "codex-rs/core/src/compact.rs",
             "symbols": ["COMPACT_USER_MESSAGE_MAX_TOKENS", "CompactedHistoryMetadata",
                         "InitialContextInjection"]},
            {"path": "codex-rs/core/src/compact.rs",
             "symbols": ["CompactionAnalyticsAttempt", "compaction_status_from_result",
                         "CompactedUserMessage"]},
        ],
    },
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "agents-md",
        "title": "AGENTS.md 指令加载",
        "page_file": "g05-agents-md.md",
        "boundary": "负责从目录层级加载 AGENTS.md 指令、来源标注与大小限制；"
                    "不负责压缩（g04）、不负责技能（f03）。",
        "refs": [
            {"path": "codex-rs/core/src/agents_md.rs",
             "symbols": ["DEFAULT_AGENTS_MD_FILENAME", "LoadedAgentsMd", "InstructionProvenance"]},
            {"path": "codex-rs/core/src/agents_md_manager.rs",
             "symbols": ["AgentsMdManager", "MAX_THREAD_INSTRUCTIONS_TOKENS",
                         "validate_thread_instruction_size"]},
        ],
    },
    {
        "feature_group": "G-会话持久化与上下文",
        "feature": "context-fragments",
        "title": "上下文片段与附加信息",
        "page_file": "g06-context-fragments.md",
        "boundary": "负责附加上下文片段的渲染、标记与 token 上限；"
                    "不负责压缩（g04）、不负责钩子溢写（f02）。",
        "refs": [
            {"path": "codex-rs/context-fragments/src/fragment.rs",
             "symbols": ["RenderedFragment", "ContextualUserFragment", "matches_marked_text"]},
            {"path": "codex-rs/context-fragments/src/additional_context.rs",
             "symbols": ["MAX_ADDITIONAL_CONTEXT_VALUE_TOKENS", "AdditionalContextUserFragment",
                         "additional_context_body"]},
        ],
    },

    # ============ H-模型与提供方 ============
    {
        "feature_group": "H-模型与提供方",
        "feature": "provider-caps",
        "title": "提供方能力与账号状态",
        "page_file": "h01-provider-caps.md",
        "boundary": "负责提供方能力声明、账号状态与未授权恢复；"
                    "不负责认证实现（h02）、不负责模型目录（h03）。",
        "refs": [
            {"path": "codex-rs/model-provider/src/provider.rs",
             "symbols": ["ProviderCapabilities", "ProviderAccountState",
                         "ProviderUnauthorizedRecovery"]},
            {"path": "codex-rs/model-provider/src/provider.rs",
             "symbols": ["RemoteCompactionSupport", "enforce_managed_residency",
                         "ProviderAuthRecoveryMessages"]},
        ],
    },
    {
        "feature_group": "H-模型与提供方",
        "feature": "provider-auth",
        "title": "提供方认证与工作区路由",
        "page_file": "h02-provider-auth.md",
        "boundary": "负责提供方的多种认证提供者、工作区路由头与连接键；"
                    "不负责能力声明（h01）、不负责模型目录（h03）。",
        "refs": [
            {"path": "codex-rs/model-provider/src/auth.rs",
             "symbols": ["ResolvedProviderAuth", "ProviderAuthScope", "AuthManagerAuthProvider"]},
            {"path": "codex-rs/model-provider/src/workspace_routing.rs",
             "symbols": ["WorkspaceRoutingContext", "ACCOUNT_ROUTING_HEADER",
                         "apply_workspace_routing"]},
        ],
    },
    {
        "feature_group": "H-模型与提供方",
        "feature": "models-manager",
        "title": "模型目录与缓存",
        "page_file": "h03-models-manager.md",
        "boundary": "负责模型列表的拉取、缓存与刷新策略；"
                    "不负责提供方认证（h02）、不负责工作树（j01）。",
        "refs": [
            {"path": "codex-rs/models-manager/src/manager.rs",
             "symbols": ["ModelsManager", "RefreshStrategy", "DEFAULT_MODEL_CACHE_TTL"]},
            {"path": "codex-rs/models-manager/src/cache.rs",
             "symbols": ["ModelsCache", "FileModelsCache", "ModelsCacheEntry"]},
        ],
    },
    {
        "feature_group": "H-模型与提供方",
        "feature": "model-info",
        "title": "模型信息与基础指令",
        "page_file": "h04-model-info.md",
        "boundary": "负责模型元信息推导与基础指令拼装；"
                    "不负责目录缓存（h03）、不负责 AGENTS.md（g05）。",
        "refs": [
            {"path": "codex-rs/models-manager/src/model_info.rs",
             "symbols": ["BASE_INSTRUCTIONS", "model_info_from_slug",
                         "strip_personality_section"]},
            {"path": "codex-rs/models-manager/src/model_info.rs",
             "symbols": ["with_config_overrides", "PERSONALITY_SECTION_HEADER"]},
        ],
    },

    # ============ I-接入面 ============
    {
        "feature_group": "I-接入面",
        "feature": "acp-server",
        "title": "ACP 服务端与配置项",
        "page_file": "i01-acp-server.md",
        "boundary": "负责以 ACP 服务端身份对外服务、配置项映射与会话状态；"
                    "不负责 MCP 服务端（i02）、不负责 Codex 兼容（a02）。",
        "refs": [
            {"path": "codex-rs/acp-server/src/lib.rs",
             "symbols": ["AppServerAcpAgent", "SessionState", "AcpSessionMode"]},
            {"path": "codex-rs/acp-server/src/lib.rs",
             "symbols": ["CONFIG_ID_MODEL", "CONFIG_ID_REASONING_EFFORT", "handle_notification"]},
        ],
    },
    {
        "feature_group": "I-接入面",
        "feature": "mcp-server",
        "title": "MCP 服务端与进行中回合",
        "page_file": "i02-mcp-server.md",
        "boundary": "负责把自身暴露为 MCP 服务端、工具运行与进行中回合登记；"
                    "不负责 ACP（i01）、不负责 MCP 客户端传输（i03）。",
        "refs": [
            {"path": "codex-rs/mcp-server/src/codex_tool_runner.rs",
             "symbols": ["create_call_tool_result_with_thread_id", "prompt_request"]},
            {"path": "codex-rs/mcp-server/src/active_turn_registry.rs",
             "symbols": ["ActiveTurnRegistry", "ActiveTurnState", "ActiveTurn"]},
        ],
    },
    {
        "feature_group": "I-接入面",
        "feature": "rmcp-stdio",
        "title": "MCP 客户端 stdio 传输",
        "page_file": "i03-rmcp-stdio.md",
        "boundary": "负责 MCP 客户端的本地 stdio 传输与行长上限；"
                    "不负责服务端（i02）、不负责工具转换（d04）。",
        "refs": [
            {"path": "codex-rs/rmcp-client/src/local_stdio_transport.rs",
             "symbols": ["LocalStdioTransport", "StdioTransport"]},
            {"path": "codex-rs/rmcp-client/src/bounded_stdio_transport.rs",
             "symbols": ["BoundedStdioTransport", "MAX_MCP_STDIO_LINE_BYTES"]},
        ],
    },

    # ============ J-工作区与可观测 ============
    {
        "feature_group": "J-工作区与可观测",
        "feature": "worktree",
        "title": "工作树管理与归属记录",
        "page_file": "j01-worktree.md",
        "boundary": "负责创建/删除受管工作树、Git 命令执行与归属元数据；"
                    "不负责补丁应用（d06）、不负责沙箱（C 组）。",
        "refs": [
            {"path": "codex-rs/worktree/src/lib.rs",
             "symbols": ["WorktreeManager", "CreateWorktree", "is_safe_worktree_cwd"]},
            {"path": "codex-rs/worktree/src/metadata.rs",
             "symbols": ["OwnerRecord", "OWNER_FILENAME", "bind_thread"]},
        ],
    },
    {
        "feature_group": "J-工作区与可观测",
        "feature": "file-search",
        "title": "文件搜索与索引",
        "page_file": "j02-file-search.md",
        "boundary": "负责文件名匹配、索引与搜索会话；"
                    "不负责工作树（j01）、不负责会话搜索（g03）。",
        "refs": [
            {"path": "codex-rs/file-search/src/lib.rs",
             "symbols": ["FileSearchSession", "FileSearchResults", "FileMatch"]},
            {"path": "codex-rs/file-search/src/lib.rs",
             "symbols": ["FileSearchOptions", "IndexedEntry", "MatchType"]},
        ],
    },
    {
        "feature_group": "J-工作区与可观测",
        "feature": "otel-tracing",
        "title": "追踪上下文与遥测目标",
        "page_file": "j03-otel-tracing.md",
        "boundary": "负责 W3C 追踪上下文的注入与提取、遥测目标分类与提供方生命周期；"
                    "不负责会话记录（g01）、不负责模型目录（h03）。",
        "refs": [
            {"path": "codex-rs/otel/src/trace_context.rs",
             "symbols": ["inject_span_w3c_trace_headers", "current_span_w3c_trace_context",
                         "TRACEPARENT_ENV_VAR"]},
            {"path": "codex-rs/otel/src/targets.rs",
             "symbols": ["is_trace_safe_target", "OTEL_TARGET_PREFIX"]},
            {"path": "codex-rs/otel/src/provider.rs",
             "symbols": ["OtelProvider", "resource_attributes", "ResourceKind"]},
        ],
    },
]
