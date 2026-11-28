---
project: open-interpreter
page_type: overview
display_name: Open Interpreter
repo: https://github.com/OpenInterpreter/open-interpreter
license: Apache-2.0
language: Rust
stars: 68950
stars_as_of: 2026-11-23
refs:
  - path: codex-rs/tools/src/harness.rs
    symbols: [Harness, from_config_name]
  - path: codex-rs/core/src/harness/request.rs
    symbols: [build_chat_harness_request]
  - path: codex-rs/execpolicy/src/policy.rs
    symbols: [Policy, Evaluation]
  - path: codex-rs/sandboxing/src/manager.rs
    symbols: [get_platform_sandbox]
  - path: codex-rs/rollout/src/recorder.rs
    symbols: [RolloutRecorder]
  - path: codex-rs/otel/src/trace_context.rs
    symbols: [inject_span_w3c_trace_headers]
---
# Open Interpreter

## 一句话职责

面向低成本模型的编程 agent：它是 OpenAI Codex 的一个 fork，核心主张不是发明新的 agent 循环，而是**复刻各家成熟 agent 工具的请求形状**（称之为 harness），让同一个便宜模型在不同「壳」下表现更好；同时保留一套生产级沙箱、执行策略与会话持久化设施。

## 机制是什么

这个项目的定位需要先讲清，因为它决定了对它的机制该看什么。

**它不改循环，改「模型看到什么」。** smolagents、browser-use、goose 都在设计自己的 agent 循环与工具协议；open-interpreter 认为便宜模型的效果很大程度取决于它所熟悉的请求结构，于是并行维护十几个 harness 实现（`claude_code`、`kimi_cli`、`kimi_code`、`zcode`、`qwen_code`、`swe_agent`、`mini_swe_agent`、`terminus_2`、`opencode`、`pi`、`little_coder`、`deepseek_tui`、`minimal`、`native` 等），用户用 `/harness` 切换。复刻的最小单位是**请求头、版本号常量、系统提示、工具集与消息形态**。

**它是 Codex 兼容的。** README 明确写「speaks the same Codex exec protocol」，用 Codex SDK 只需一行 `codexPathOverride`。它同时也以 ACP agent 身份对外（`interpreter acp`）。

**它的工程主体在安全与持久化。** `codex-rs/` 是 130+ crate、4820 个 rs 文件的 Rust workspace，其中体量最大的一批是：沙箱（macOS Seatbelt、Linux Landlock+bwrap、Windows 受限令牌）、执行策略（规则解析与求值）、会话滚动记录（含压缩与倒序扫描）、上下文压缩、模型目录与提供方认证。

**它强调可迁移。** 仓库的 README 写明产品原则：优先用共享的、工具中立的标准与目录——`AGENTS.md`、`.agents/skills`、MCP、ACP、Codex exec 协议；只有还没有共享标准的配置与运行状态才放在产品私有目录 `~/.openinterpreter`。

本文库按经审阅的机制单元组织：**10 个机制领域、共 41 篇机制页**。130 个 crate 中绝大多数是协议模型、mock 客户端与测试支撑，不建页。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A Harness 与入口 → B 执行策略与审批 → C 沙箱 → D 工具系统 → E 协议条目与动态工具 → F 钩子与技能 → G 会话持久化与上下文 → H 模型与提供方 → I 接入面 → J 工作区与可观测。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`codex-rs/` 下的全部 crate；`codex-cli/`（Node 包装）与 `sdk/`（SDK 示例）只作为兼容面提及。

## 机制导航（41 篇 / 10 组）

### A-Harness 与入口（5 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-harness-model.md`](a01-harness-model.md) | Harness 模型与名称解析 |
| [`a02-codex-compat.md`](a02-codex-compat.md) | Codex 协议与 SDK 兼容 |
| [`a03-cli-entry.md`](a03-cli-entry.md) | 命令行入口与子命令分派 |
| [`a04-harness-emulation.md`](a04-harness-emulation.md) | Harness 请求模拟与路由 |
| [`a05-minimal-harness.md`](a05-minimal-harness.md) | 最小 harness 与消息转换 |

### B-执行策略与审批（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-exec-policy-rules.md`](b01-exec-policy-rules.md) | 执行策略规则与匹配 |
| [`b02-exec-policy-parser.md`](b02-exec-policy-parser.md) | 策略文件解析与校验 |
| [`b03-policy-evaluation.md`](b03-policy-evaluation.md) | 策略求值与规则修订 |
| [`b04-approvals.md`](b04-approvals.md) | 审批档案与风险判定 |

### C-沙箱（4 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-sandbox-manager.md`](c01-sandbox-manager.md) | 沙箱管理器与平台选择 |
| [`c02-macos-seatbelt.md`](c02-macos-seatbelt.md) | macOS Seatbelt 沙箱 |
| [`c03-linux-sandbox.md`](c03-linux-sandbox.md) | Linux Landlock 与 bwrap 沙箱 |
| [`c04-windows-sandbox.md`](c04-windows-sandbox.md) | Windows 受限令牌沙箱 |

### D-工具系统（6 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-tool-call.md`](d01-tool-call.md) | 工具调用与环境抽象 |
| [`d02-json-schema.md`](d02-json-schema.md) | 工具输入 schema 的清洗 |
| [`d03-code-mode.md`](d03-code-mode.md) | 代码模式下的工具定义 |
| [`d04-mcp-tool.md`](d04-mcp-tool.md) | MCP 工具的转换与上限 |
| [`d05-output-schema.md`](d05-output-schema.md) | 工具输出 schema 与存储 |
| [`d06-apply-patch.md`](d06-apply-patch.md) | 补丁解析与应用 |

### E-协议条目与动态工具（2 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-turn-items.md`](e01-turn-items.md) | 回合条目模型 |
| [`e02-dynamic-tools.md`](e02-dynamic-tools.md) | 动态工具规格 |

### F-钩子与技能（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-hooks-registry.md`](f01-hooks-registry.md) | 钩子注册与触发 |
| [`f02-hook-output-spill.md`](f02-hook-output-spill.md) | 钩子输出溢写 |
| [`f03-skills-loading.md`](f03-skills-loading.md) | 技能加载与 frontmatter 解析 |
| [`f04-skill-selection.md`](f04-skill-selection.md) | 技能提及与隐式调用识别 |

### G-会话持久化与上下文（6 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-rollout-recorder.md`](g01-rollout-recorder.md) | 会话滚动记录 |
| [`g02-rollout-compression.md`](g02-rollout-compression.md) | 会话文件压缩 |
| [`g03-session-index.md`](g03-session-index.md) | 会话索引与倒序扫描 |
| [`g04-compaction.md`](g04-compaction.md) | 上下文压缩 |
| [`g05-agents-md.md`](g05-agents-md.md) | AGENTS.md 指令加载 |
| [`g06-context-fragments.md`](g06-context-fragments.md) | 上下文片段与附加信息 |

### H-模型与提供方（4 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-provider-caps.md`](h01-provider-caps.md) | 提供方能力与账号状态 |
| [`h02-provider-auth.md`](h02-provider-auth.md) | 提供方认证与工作区路由 |
| [`h03-models-manager.md`](h03-models-manager.md) | 模型目录与缓存 |
| [`h04-model-info.md`](h04-model-info.md) | 模型信息与基础指令 |

### I-接入面（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-acp-server.md`](i01-acp-server.md) | ACP 服务端与配置项 |
| [`i02-mcp-server.md`](i02-mcp-server.md) | MCP 服务端与进行中回合 |
| [`i03-rmcp-stdio.md`](i03-rmcp-stdio.md) | MCP 客户端 stdio 传输 |

### J-工作区与可观测（3 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-worktree.md`](j01-worktree.md) | 工作树管理与归属记录 |
| [`j02-file-search.md`](j02-file-search.md) | 文件搜索与索引 |
| [`j03-otel-tracing.md`](j03-otel-tracing.md) | 追踪上下文与遥测目标 |

## 关键取舍

**源码事实：** harness 的复刻值是硬编码常量而非可配置项（例如 `CLAUDE_CODE_VERSION`、`CLAUDE_CODE_BILLING_VERSION_SALT`、`CLAUDE_CODE_BILLING_HEADER_PREFIX`）。因为要复刻的是「服务端期待的客户端身份」，配置化会破坏这个目的。

**源码事实：** 各 harness 是独立的大文件实现（`claude_code.rs` 4870 行、`kimi_cli.rs` 2772 行、`zcode.rs` 2593 行），差异不做参数化。

**源码事实：** 沙箱按平台分四套实现（macOS Seatbelt 配置文件、Linux bwrap + Landlock、Windows 受限令牌与提权后端），`get_platform_sandbox` 是选择层。

**源码事实：** 会话历史写成滚动记录（rollout），并单独实现压缩、倒序扫描与索引，说明「历史会变长到需要专门的存储层」被当成一等问题。

**架构推论：** 「复刻请求形状」把兼容性从「可用性问题」升级成「性能手段」。代价是必须持续追上游版本；收益是不依赖模型厂商为你的框架做适配——便宜模型在它熟悉的壳里就能表现更好。

**架构推论：** 沙箱四套实现看似重复，实际是被平台能力逼出来的。macOS 只能用 Seatbelt 配置文件表达策略，Linux 才有 Landlock 与 namespace，Windows 只有令牌与提权。**同一语义在不同平台上没有统一的底层原语**，所以只能各写一份，再用管理器抽象成同一接口。

**架构推论：** 可迁移原则（`AGENTS.md`、`.agents/skills`、MCP、ACP）反映了一个判断：**agent 数据格式的护城河不该建在产品私有目录上**。用户写的技能、指令、配置应当能带走，否则工具迁移成本会反过来抑制采用。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `codex-rs/tools/src/harness.rs` | `Harness` | harness 枚举与名称解析 |
| `codex-rs/core/src/harness/request.rs` | `build_chat_harness_request` | 按 harness 构造请求 |
| `codex-rs/core/src/harness/routing.rs` | `resolve_stream_transport_route` | 按 harness 选传输路由 |
| `codex-rs/execpolicy/src/policy.rs` | `Policy` | 执行策略求值 |
| `codex-rs/execpolicy/src/rule.rs` | `PrefixRule` | 前缀规则与网络规则 |
| `codex-rs/sandboxing/src/manager.rs` | `get_platform_sandbox` | 平台沙箱选择 |
| `codex-rs/linux-sandbox/src/bwrap.rs` | `BwrapArgs` | Linux bwrap 参数构造 |
| `codex-rs/tools/src/json_schema.rs` | `sanitize_json_schema` | 工具输入 schema 清洗 |
| `codex-rs/rollout/src/recorder.rs` | `RolloutRecorder` | 会话滚动记录 |
| `codex-rs/core/src/compact.rs` | `CompactedHistoryMetadata` | 上下文压缩的元数据 |
| `codex-rs/otel/src/trace_context.rs` | `inject_span_w3c_trace_headers` | 追踪上下文注入 |
| `codex-rs/worktree/src/lib.rs` | `WorktreeManager` | 受管工作树 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档：Harness](https://www.openinterpreter.com/docs/terminal/harness)
- [open-interpreter 文档：沙箱与审批](https://www.openinterpreter.com/docs/terminal/sandbox)
- [open-interpreter 仓库：可迁移性说明](https://github.com/openinterpreter/openinterpreter/blob/main/docs/portability.md)

## 与主核 / AI 的关系

open-interpreter 与主核（多模态数据库内核）在**协议兼容的成本结构**上有一条可直接复用的判断准则。

**「兼容」的两种性质。** 主核要兼容 PostgreSQL 的 SQL 方言与客户端协议，做法是「按标准实现 + 少量兼容特例」，因为 SQL 标准相对稳定，兼容性只影响可用性。open-interpreter 选的是「逐细节复刻」，因为 agent 工具的请求形状没有标准，且每个细节都影响模型效果。所以准则可以写成：**当兼容只影响可用性时按标准实现；当兼容直接影响核心指标时只能逐细节复刻，并接受持续追版本的成本。**

**沙箱四套实现 vs 主核的存储适配。** 两者都在处理「同一语义在不同底层没有统一原语」这个现实。主核要为不同存储后端提供同一套表接口，答案也是「各写一份实现 + 一层管理器抽象」。这条经验的价值在于：**抽象层要抽的是语义与调用方式，而不是试图统一底层机制。**

**滚动记录 vs WAL。** 会话历史追加到滚动记录文件、并配压缩与倒序扫描，与主核的 WAL 加归档在目标上同构：顺序追加、可压缩、可从尾部读。差别是 WAL 需要保证崩溃一致性（因此有严格的刷盘顺序与校验），而会话记录允许丢失尾部（因此压缩与追加可以并发处理）。

**可迁移原则值得单独记。** 「把用户数据放在共享标准目录、只有无标准的部分才放私有目录」是一条产品决策，但它有工程后果：私有格式越少，迁移成本越低，工具的采用阻力越小。主核里对应的是「数据文件格式要有版本与自描述，避免出现只有本系统能读的布局」。
