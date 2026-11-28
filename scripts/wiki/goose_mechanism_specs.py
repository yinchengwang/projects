# -*- coding: utf-8 -*-
"""goose 机制清单（阶段 2B 第五个项目）。

盘点范围：`crates/goose/src/` —— Rust 写成的 agent 运行时。goose 的定位与其他 2B 项目不同：
它把「智能体循环」和「权限/安全/调度」放在同一个 crate 里，且横切机制（权限判定、注入扫描、
出口审查、上下文压缩、钩子）密度明显高于其他框架。

盘点方法：Rust 也没有现成的定义索引器，因此用「正则抽取 `pub struct/enum/trait/fn` +
人工核对」建符号清单，再用 `gen_manifest.py --verify` 的 refs 校验兜底。篇数是归并结果，
不是按文件数折算（crates 下共 642 个 rs 文件，绝大多数是 provider 适配与测试）。

源码锚点：`goose@c546eaa1cd02696e4cd628e203df51429e158175`。
"""

GROUP_ORDER = [
    "A-会话与运行时",
    "B-智能体循环与提示",
    "C-权限",
    "D-安全审查",
    "E-扩展与工具",
    "F-配方与技能",
    "G-提供方与模型",
    "H-配置与迁移",
    "I-ACP 接入",
    "J-上下文、调度与网关",
]

MECHANISM_SPECS = [
    # ============ A 会话与运行时 ============
    {
        "feature_group": "A-会话与运行时",
        "feature": "session-manager",
        "title": "会话管理与用量聚合",
        "page_file": "a01-session-manager.md",
        "boundary": "负责会话的创建、查询、更新与用量汇总；"
                    "不负责运行时上下文（a02）、不负责上下文压缩（j01）。",
        "refs": [
            {"path": "crates/goose/src/session/session_manager.rs",
             "symbols": ["SessionManager", "Session", "SessionType"]},
            {"path": "crates/goose/src/session/session_manager.rs",
             "symbols": ["SessionUpdateBuilder", "SessionUsageTotals", "SessionInsights"]},
        ],
    },
    {
        "feature_group": "A-会话与运行时",
        "feature": "session-runtime",
        "title": "会话运行时与附件",
        "page_file": "a02-session-runtime.md",
        "boundary": "负责一次会话运行期间持有的运行时对象与附件处理；"
                    "不负责会话持久化（a01）、不负责提供方选择（g01）。",
        "refs": [
            {"path": "crates/goose/src/agents/session_runtime.rs",
             "symbols": ["SessionRuntime", "Attachment"]},
            {"path": "crates/goose/src/agents/provider_manager.rs",
             "symbols": ["ProviderManager", "SharedProviders", "SessionSlot"]},
        ],
    },
    {
        "feature_group": "A-会话与运行时",
        "feature": "session-hooks",
        "title": "会话钩子文件与规则选择",
        "page_file": "a03-session-hooks.md",
        "boundary": "负责钩子文件的格式、规则的匹配与动作选择，含失败时的行为开关；"
                    "不负责权限（C 组）、不负责安全审查（D 组）。",
        "refs": [
            {"path": "crates/goose/src/hooks/mod.rs",
             "symbols": ["HookEvent", "HooksFile", "RawHookRule", "LoadedRule"]},
            {"path": "crates/goose/src/hooks/mod.rs",
             "symbols": ["select_action", "HookDecision", "OnFailure", "ActionSelection"]},
        ],
    },

    # ============ B 智能体循环与提示 ============
    {
        "feature_group": "B-智能体循环与提示",
        "feature": "state-machine",
        "title": "状态机式智能体循环",
        "page_file": "b01-state-machine.md",
        "boundary": "负责把一次运行的推进表达成状态机，以及提供方的类型安全包装；"
                    "不负责提示组装（b02）、不负责重试（b03）。",
        "refs": [
            {"path": "crates/goose/src/agents/state_machine_builder.rs",
             "symbols": ["typesafe_provider"]},
            {"path": "crates/goose/src/agents/active_run.rs", "symbols": ["ActiveRun", "SessionRunState"]},
        ],
    },
    {
        "feature_group": "B-智能体循环与提示",
        "feature": "prompt-assembly",
        "title": "系统提示组装",
        "page_file": "b02-prompt-assembly.md",
        "boundary": "负责系统提示的部件化组装与上下文注入；"
                    "不负责模板渲染（b04）、不负责权限（C 组）。",
        "refs": [
            {"path": "crates/goose/src/agents/prompt_manager.rs",
             "symbols": ["PromptManager", "SystemPromptBuilder", "SystemPromptParts"]},
            {"path": "crates/goose/src/agents/prompt_manager.rs",
             "symbols": ["SystemPromptContext"]},
        ],
    },
    {
        "feature_group": "B-智能体循环与提示",
        "feature": "failure-hooks",
        "title": "失败后命令与超时执行",
        "page_file": "b03-failure-hooks.md",
        "boundary": "负责成功校验命令与失败后命令的执行方式（超时、stderr 尾部截断、错误归类）；"
                    "不负责钩子规则匹配（a03）、不负责提供方重试（g05）。",
        "refs": [
            {"path": "crates/goose/src/agents/retry.rs",
             "symbols": ["execute_success_checks_with_timeout",
                         "execute_on_failure_command_with_timeout"]},
            {"path": "crates/goose/src/agents/retry.rs",
             "symbols": ["execute_shell_command", "capture_tail", "MAX_COMMAND_STDERR_BYTES"]},
        ],
    },
    {
        "feature_group": "B-智能体循环与提示",
        "feature": "prompt-template",
        "title": "提示模板与转义",
        "page_file": "b04-prompt-template.md",
        "boundary": "负责模板渲染与不可信文本的转义/包裹，使模型能区分数据与指令；"
                    "不负责提示组装（b02）、不负责注入扫描（d02）。",
        "refs": [
            {"path": "crates/goose/src/prompt_template.rs", "symbols": ["render_template"]},
            {"path": "crates/goose/src/agents/moim.rs",
             "symbols": ["compose_moim", "escape_xml_text", "turn_budget_part"]},
        ],
    },

    # ============ C 权限 ============
    {
        "feature_group": "C-权限",
        "feature": "permission-judge",
        "title": "权限判定与批量只读跳过",
        "page_file": "c01-permission-judge.md",
        "boundary": "负责判定一次工具调用是否需要用户批准，以及把整批只读调用一次性放行的捷径；"
                    "不负责判定结果缓存（c02）、不负责安全审查（D 组）。",
        "refs": [
            {"path": "crates/goose/src/permission/permission_judge.rs",
             "symbols": ["PermissionJudgeContext", "PermissionCheckResult"]},
            {"path": "crates/goose/src/permission/permission_judge.rs",
             "symbols": ["create_check_messages", "extract_read_only_request_ids",
                         "create_read_only_tool"]},
        ],
    },
    {
        "feature_group": "C-权限",
        "feature": "permission-store",
        "title": "权限记录与判定缓存",
        "page_file": "c02-permission-store.md",
        "boundary": "负责把用户的批准/拒绝持久化、以及非只读判定的缓存；"
                    "不负责判定逻辑（c01）、不负责审查（D 组）。",
        "refs": [
            {"path": "crates/goose/src/permission/permission_store.rs",
             "symbols": ["ToolPermissionStore", "ToolPermissionRecord"]},
            {"path": "crates/goose/src/permission/permission_inspector.rs",
             "symbols": ["PermissionInspector", "cache_non_readonly_decision"]},
        ],
    },

    # ============ D 安全审查 ============
    {
        "feature_group": "D-安全审查",
        "feature": "security-inspector",
        "title": "安全审查统一入口",
        "page_file": "d01-security-inspector.md",
        "boundary": "负责把多种审查器串成一道检查、以及判定结果的合并；"
                    "不负责注入扫描（d02）、不负责出口审查（d03）。",
        "refs": [
            {"path": "crates/goose/src/security/security_inspector.rs",
             "symbols": ["SecurityInspector"]},
            {"path": "crates/goose/src/security/mod.rs", "symbols": ["SecurityManager", "SecurityResult"]},
        ],
    },
    {
        "feature_group": "D-安全审查",
        "feature": "injection-scanner",
        "title": "提示注入扫描",
        "page_file": "d02-injection-scanner.md",
        "boundary": "负责对不可信文本做注入特征扫描与分类器判定；"
                    "不负责统一入口（d01）、不负责出口（d03）。",
        "refs": [
            {"path": "crates/goose/src/security/scanner.rs",
             "symbols": ["PromptInjectionScanner", "ScanResult", "ClassifierType"]},
            {"path": "crates/goose/src/security/scanner.rs",
             "symbols": ["is_shell_tool_name", "DetailedScanResult"]},
        ],
    },
    {
        "feature_group": "D-安全审查",
        "feature": "egress-inspector",
        "title": "出口方向审查",
        "page_file": "d03-egress-inspector.md",
        "boundary": "负责判断一次工具调用的数据流向（进还是出）与目标地址，并对其内容做审查；"
                    "不负责注入扫描（d02）、不负责对手配置（d04）。",
        "refs": [
            {"path": "crates/goose/src/security/egress_inspector.rs",
             "symbols": ["EgressInspector", "EgressDirection", "EgressDestination"]},
            {"path": "crates/goose/src/security/egress_inspector.rs",
             "symbols": ["detect_direction", "extract_destinations", "extract_text_for_inspection"]},
        ],
    },
    {
        "feature_group": "D-安全审查",
        "feature": "adversary-inspector",
        "title": "对手模拟与配置",
        "page_file": "d04-adversary-inspector.md",
        "boundary": "负责用对手模型复核可疑调用，含配置与判定；"
                    "不负责正则类扫描（d02）、不负责出口（d03）。",
        "refs": [
            {"path": "crates/goose/src/security/adversary_inspector.rs",
             "symbols": ["AdversaryInspector", "AdversaryConfig"]},
            {"path": "crates/goose/src/security/patterns.rs", "symbols": ["ThreatPattern", "PatternMatcher"]},
        ],
    },

    # ============ E 扩展与工具 ============
    {
        "feature_group": "E-扩展与工具",
        "feature": "extension-runtime",
        "title": "扩展运行时与工具可见性",
        "page_file": "e01-extension-runtime.md",
        "boundary": "负责扩展的加载、工具信息登记与对模型的可见性；"
                    "不负责插件发现（e02）、不负责大响应处理（e03）。",
        "refs": [
            {"path": "crates/goose/src/agents/extension.rs", "symbols": ["ToolInfo"]},
            {"path": "crates/goose/src/agents/extension_config.rs",
             "symbols": ["ExtensionConfig", "CLIENT_SECRET_PREFIX"]},
        ],
    },
    {
        "feature_group": "E-扩展与工具",
        "feature": "plugin-discovery",
        "title": "插件发现与作用域",
        "page_file": "e02-plugin-discovery.md",
        "boundary": "负责从磁盘发现插件、区分作用域与合并配置；"
                    "不负责扩展运行时（e01）、不负责配方（F 组）。",
        "refs": [
            {"path": "crates/goose/src/plugins/discovery.rs",
             "symbols": ["DiscoveredPlugin", "PluginScope", "PluginSettings"]},
            {"path": "crates/goose/src/plugins/discovery.rs",
             "symbols": ["discover_enabled_plugins", "plugin_scope_rank", "filter_by_config"]},
        ],
    },
    {
        "feature_group": "E-扩展与工具",
        "feature": "large-response",
        "title": "大响应的落盘处理",
        "page_file": "e03-large-response.md",
        "boundary": "负责工具返回过大时如何落盘并只回给模型一个引用；"
                    "不负责上下文压缩（j01）、不负责扩展登记（e01）。",
        "refs": [
            {"path": "crates/goose/src/agents/large_response_handler.rs",
             "symbols": ["process_tool_response", "write_large_text_to_file"]},
        ],
    },

    # ============ F-配方与技能 ============
    {
        "feature_group": "F-配方与技能",
        "feature": "recipe-model",
        "title": "配方模型与参数",
        "page_file": "f01-recipe-model.md",
        "boundary": "负责配方（recipe）的字段构成、参数声明与序列化；"
                    "不负责文件发现（f02）、不负责校验（f03）。",
        "refs": [
            {"path": "crates/goose/src/recipe/mod.rs",
             "symbols": ["Recipe", "RecipeParameter", "RecipeBuilder"]},
            {"path": "crates/goose/src/recipe/mod.rs",
             "symbols": ["SubRecipe", "RecipeParameterRequirement", "RecipeParameterInputType"]},
        ],
    },
    {
        "feature_group": "F-配方与技能",
        "feature": "recipe-manifest",
        "title": "配方文件清单与加载",
        "page_file": "f02-recipe-manifest.md",
        "boundary": "负责列出磁盘上的配方文件、按 id 加载与解析子配方路径；"
                    "不负责配方结构（f01）、不负责校验（f03）。",
        "refs": [
            {"path": "crates/goose/src/recipe/manifest.rs",
             "symbols": ["RecipeFileManifest", "list_recipe_file_manifests",
                         "load_recipe_by_id"]},
            {"path": "crates/goose/src/recipe/manifest.rs",
             "symbols": ["resolve_recipe_sub_recipe_paths", "short_id_from_path"]},
        ],
    },
    {
        "feature_group": "F-配方与技能",
        "feature": "recipe-validation",
        "title": "配方校验与斜杠命令",
        "page_file": "f03-recipe-validation.md",
        "boundary": "负责配方校验与把配方/技能暴露成斜杠命令；"
                    "不负责配方结构（f01）、不负责技能实现（f04）。",
        "refs": [
            {"path": "crates/goose/src/recipe/validate_recipe.rs",
             "symbols": ["convert_recipe_from_value", "RecipeFileFormat"]},
            {"path": "crates/goose/src/slash_commands/recipe_slash_command.rs",
             "symbols": ["SlashCommandMapping", "set_recipe_slash_command"]},
        ],
    },
    {
        "feature_group": "F-配方与技能",
        "feature": "skills",
        "title": "技能与支撑文件",
        "page_file": "f04-skills.md",
        "boundary": "负责技能的登记、支撑文件处理与暴露方式；"
                    "不负责配方（f01）、不负责插件（e02）。",
        "refs": [
            {"path": "crates/goose/src/skills/builtin.rs", "symbols": ["get_all"]},
            {"path": "crates/goose/src/skills/supporting_files.rs",
             "symbols": ["read_supporting_file_with_limit", "ReadLimit"]},
        ],
    },

    # ============ G-提供方与模型 ============
    {
        "feature_group": "G-提供方与模型",
        "feature": "provider-def",
        "title": "提供方定义与两枚编译期常量",
        "page_file": "g01-provider-def.md",
        "boundary": "负责提供方的抽象契约与类型划分；"
                    "不负责成本估算（g03）、不负责自定义提供方（g04）。",
        "refs": [
            {"path": "crates/goose/src/providers/base.rs",
             "symbols": ["ProviderDef", "ProviderType"]},
        ],
    },
    {
        "feature_group": "G-提供方与模型",
        "feature": "provider-retry",
        "title": "重试判定层次与退避曲线",
        "page_file": "g05-provider-retry.md",
        "boundary": "负责一次模型请求失败后的重试判定、退避曲线与「永久失败」识别；"
                    "不负责回退（g02）、不负责失败后命令（b03）。",
        "refs": [
            {"path": "crates/oi-kit-provider-types/src/retry.rs",
             "symbols": ["RetryConfig", "ProviderRetry", "should_retry"]},
            {"path": "crates/oi-kit-provider-types/src/retry.rs",
             "symbols": ["is_permanent_request_failure", "DEFAULT_MAX_RETRIES",
                         "DEFAULT_BACKOFF_MULTIPLIER"]},
        ],
    },
    {
        "feature_group": "G-提供方与模型",
        "feature": "model-fallback",
        "title": "模型配置补全与目录回落",
        "page_file": "g02-model-fallback.md",
        "boundary": "负责在多个模型间按规则选择与回退；"
                    "不负责定义（g01）、不负责成本（g03）。",
        "refs": [
            {"path": "crates/goose/src/catalog/model_catalog.rs" if False else "crates/goose/src/model_catalog.rs",
             "symbols": ["initialize", "CATALOG_URL"]},
            {"path": "crates/goose/src/model_config.rs", "symbols": ["ModelConfig"]},
        ],
    },
    {
        "feature_group": "G-提供方与模型",
        "feature": "canonical-cost",
        "title": "代价解析与来源优先级",
        "page_file": "g03-canonical-cost.md",
        "boundary": "负责按模型定价估算一次调用的成本、以及多来源定价的合并；"
                    "不负责选型（g02）、不负责用量统计（a01）。",
        "refs": [
            {"path": "crates/goose/src/providers/canonical_cost.rs",
             "symbols": ["estimate_model_cost", "merge_pricing", "pricing_from_model_info"]},
            {"path": "crates/goose/src/providers/canonical_cost.rs",
             "symbols": ["is_price_gap", "full_pricing_model", "bundled_model_info"]},
        ],
    },
    {
        "feature_group": "G-提供方与模型",
        "feature": "declarative-providers",
        "title": "声明式提供方与自定义接入",
        "page_file": "g04-declarative-providers.md",
        "boundary": "负责用声明文件定义提供方、环境变量展开与增删改；"
                    "不负责内置提供方（g01）、不负责迁移（h02）。",
        "refs": [
            {"path": "crates/goose/src/config/declarative_providers.rs",
             "symbols": ["LoadedProvider", "expand_env_vars", "persist_custom_provider"]},
            {"path": "crates/goose/src/config/declarative_providers.rs",
             "symbols": ["create_custom_provider", "update_custom_provider",
                         "remove_custom_provider"]},
        ],
    },

    # ============ H-配置与迁移 ============
    {
        "feature_group": "H-配置与迁移",
        "feature": "config-experiments",
        "title": "实验开关与目录布局",
        "page_file": "h01-config-experiments.md",
        "boundary": "负责配置的读取与实验开关的管理；"
                    "不负责迁移（h02）、不负责权限配置（c02）。",
        "refs": [
            {"path": "crates/goose/src/config/experiments.rs", "symbols": ["ExperimentManager"]},
            {"path": "crates/goose/src/config/paths.rs", "symbols": ["Paths", "DirType"]},
        ],
    },
    {
        "feature_group": "H-配置与迁移",
        "feature": "config-migrations",
        "title": "配置迁移与破坏性边界",
        "page_file": "h02-config-migrations.md",
        "boundary": "负责旧配置到新结构的迁移与清理；"
                    "不负责实验开关（h01）、不负责提供方定义（g04）。",
        "refs": [
            {"path": "crates/goose/src/config/migrations.rs",
             "symbols": ["run_migrations", "migrate_provider_config",
                         "cleanup_legacy_provider_keys"]},
            {"path": "crates/goose/src/config/migrations.rs",
             "symbols": ["migrate_platform_extensions", "read_enabled_field"]},
        ],
    },
    {
        "feature_group": "H-配置与迁移",
        "feature": "oauth-persist",
        "title": "OAuth 凭据持久化与形状兼容",
        "page_file": "h03-oauth-persist.md",
        "boundary": "负责 OAuth 凭据的保存与读取；"
                    "不负责配置迁移（h02）、不负责权限（C 组）。",
        "refs": [
            {"path": "crates/goose/src/oauth/persist.rs",
             "symbols": ["PersistedCredentials", "GooseCredentialStore"]},
        ],
    },

    # ============ I-ACP 接入 ============
    {
        "feature_group": "I-ACP 接入",
        "feature": "acp-provider",
        "title": "ACP 提供方与一次性上下文声明",
        "page_file": "i01-acp-provider.md",
        "boundary": "负责把外部 ACP 智能体接成提供方、以及会话与工具调用累积；"
                    "不负责服务端（i02）、不负责交接裁剪（i03）。",
        "refs": [
            {"path": "crates/goose/src/acp/provider.rs",
             "symbols": ["AcpProviderConfig", "AcpSession", "AccumulatedToolCall"]},
            {"path": "crates/goose/src/acp/provider.rs",
             "symbols": ["provider_error_from_acp", "AcpEffortState", "HandoffContextClaim"]},
        ],
    },
    {
        "feature_group": "I-ACP 接入",
        "feature": "acp-server",
        "title": "ACP 服务端与连接工厂",
        "page_file": "i02-acp-server.md",
        "boundary": "负责以 ACP 服务端身份对外服务、连接工厂与可见会话类型；"
                    "不负责提供方侧（i01）、不负责交接（i03）。",
        "refs": [
            {"path": "crates/goose/src/acp/server.rs",
             "symbols": ["AcpProviderFactory", "AcpConnection", "AcpConnectionOptions"]},
            {"path": "crates/goose/src/acp/server.rs",
             "symbols": ["is_acp_visible_session_type", "AcpBuiltinSelection"]},
        ],
    },
    {
        "feature_group": "I-ACP 接入",
        "feature": "acp-handoff",
        "title": "交接备忘录的预算装箱",
        "page_file": "i03-acp-handoff.md",
        "boundary": "负责把历史裁剪到交接预算内，含工具响应脱敏与按单元装箱；"
                    "不负责会话（i01）、不负责压缩（j01）。",
        "refs": [
            {"path": "crates/goose/src/acp/handoff.rs",
             "symbols": ["fit_unit", "elide_to_budget", "unit_cost"]},
            {"path": "crates/goose/src/acp/handoff.rs",
             "symbols": ["redact_tool_responses", "selection_units", "FittedUnit"]},
        ],
    },

    # ============ J-上下文、调度与网关 ============
    {
        "feature_group": "J-上下文、调度与网关",
        "feature": "context-management",
        "title": "上下文压缩",
        "page_file": "j01-context-management.md",
        "boundary": "负责上下文超限时的压缩阈值、工具调用截断与摘要；"
                    "不负责交接裁剪（i03）、不负责大响应（e03）。",
        "refs": [
            {"path": "crates/goose/src/context_mgmt/mod.rs",
             "symbols": ["auto_compact_threshold", "capped_compaction_threshold",
                         "is_compaction_summary"]},
            {"path": "crates/goose/src/context_mgmt/mod.rs",
             "symbols": ["compute_tool_call_cutoff", "tool_ids_to_summarize"]},
        ],
    },
    {
        "feature_group": "J-上下文、调度与网关",
        "feature": "scheduler",
        "title": "调度器与定时配方",
        "page_file": "j02-scheduler.md",
        "boundary": "负责定时任务的登记、运行与状态清理；"
                    "不负责配方结构（f01）、不负责网关（j03）。",
        "refs": [
            {"path": "crates/goose/src/scheduler/full.rs",
             "symbols": ["Scheduler", "RunningTasksMap", "JobsMap"]},
            {"path": "crates/goose/src/scheduler/full.rs",
             "symbols": ["read_validated_schedule_recipe", "clear_running_state"]},
        ],
    },
    {
        "feature_group": "J-上下文、调度与网关",
        "feature": "gateway-pairing",
        "title": "网关配对与白名单",
        "page_file": "j03-gateway-pairing.md",
        "boundary": "负责远程网关的配对流程、密钥与允许用户列表；"
                    "不负责调度（j02）、不负责凭据存储（h03）。",
        "refs": [
            {"path": "crates/goose/src/gateway/pairing.rs",
             "symbols": ["PairingStore", "StoredPairing", "StoredPendingCodes"]},
            {"path": "crates/goose/src/gateway/manager.rs",
             "symbols": ["GatewayManager", "secret_key_for", "allowed_user_ids_from_platform_config"]},
        ],
    },
    {
        "feature_group": "J-上下文、调度与网关",
        "feature": "observability",
        "title": "追踪上下文与 OTLP 三层",
        "page_file": "j04-observability.md",
        "boundary": "负责追踪上下文的注入与提取、以及遥测上报；"
                    "不负责成本（g03）、不负责记录（a01）。",
        "refs": [
            {"path": "crates/goose/src/otel/trace_context.rs",
             "symbols": ["extract_from_meta", "inject_current"]},
            {"path": "crates/goose/src/otel/otlp.rs", "symbols": ["OtlpTracingLayer"]},
        ],
    },
]
