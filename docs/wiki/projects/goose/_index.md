---
project: goose
page_type: overview
display_name: goose
repo: https://github.com/aaif-goose/goose
license: Apache-2.0
language: Rust
stars: 56864
stars_as_of: 2026-11-22
refs:
  - path: crates/goose/src/agents/state_machine_builder.rs
    symbols: [typesafe_provider]
  - path: crates/goose/src/security/security_inspector.rs
    symbols: [SecurityInspector]
  - path: crates/goose/src/permission/permission_judge.rs
    symbols: [PermissionJudgeContext]
  - path: crates/goose/src/session/session_manager.rs
    symbols: [SessionManager]
  - path: crates/goose/src/recipe/mod.rs
    symbols: [Recipe]
  - path: crates/oi-kit-provider-types/src/retry.rs
    symbols: [RetryConfig]
---
# goose

## 一句话职责

Rust 写成的 AI agent 框架：一个 `goose` crate 里同时容纳智能体循环、权限判定、安全审查、配方（recipe）、调度器与 ACP 接入，另有 CLI 与 Electron 桌面两端。

## 机制是什么

goose 的机制密度在 2B 波次里最高。它不像 smolagents 或 browser-use 那样围绕单一主张（代码即动作）组织，而是围绕一个**生产级 agent 运行时**该有的全部横切能力：

- **会话与运行时**：会话持久化、用量聚合、运行期上下文、钩子文件。
- **智能体循环与提示**：状态机式的有序推进、提示部件化组装、模板转义、失败后命令。
- **权限**：判定一次工具调用是否需要用户批准，以及「整批只读调用一次性放行」。
- **安全审查**：审查器是**可插拔契约**（`ToolInspector`），安全只是其中一个实现，另有注入扫描、出口方向审查、对手模型复核。
- **扩展与工具**：扩展运行时、插件发现、大响应落盘。
- **配方与技能**：可复用的任务定义、文件清单、校验、技能与支撑文件。
- **提供方与模型**：定义、选型、成本估算、声明式提供方、重试。
- **配置与迁移**：实验开关、配置迁移、凭据持久化。
- **ACP 接入**：既能当 ACP 提供方（调外部智能体），也能当 ACP 服务端（对外服务），还包括交接时的上下文裁剪。
- **上下文、调度与网关**：压缩、定时任务、远程网关配对、追踪遥测。

其中最有辨识度的是**权限与安全的分层**：权限回答「要不要问用户」，安全回答「这次调用是否可疑」，两者都压进同一个 `InspectionAction` 词汇表，由检查链管理器合并。

本文库按经审阅的机制单元组织：**10 个机制领域、共 35 篇机制页**。crates 下共 642 个 rs 文件，绝大多数是提供方适配与测试，不建页。

## 怎么实现的

下表的 10 个领域按下面的顺序排列：A 会话与运行时 → B 智能体循环与提示 → C 权限 → D 安全审查 → E 扩展与工具 → F 配方与技能 → G 提供方与模型 → H 配置与迁移 → I ACP 接入 → J 上下文、调度与网关。每一篇的具体边界、实现路径与取舍以该页正文为准，本页只做导航。

**盘点范围**：`crates/goose/src/`（运行时主体）与 `crates/oi-kit-provider-types/src/`（提供方类型的独立发布 crate）。

## 机制导航（35 篇 / 10 组）

### A-会话与运行时（3 篇）

| 页文件 | 标题 |
|---|---|
| [`a01-session-manager.md`](a01-session-manager.md) | 会话管理与用量聚合 |
| [`a02-session-runtime.md`](a02-session-runtime.md) | 会话运行时与附件 |
| [`a03-session-hooks.md`](a03-session-hooks.md) | 会话钩子文件与规则选择 |

### B-智能体循环与提示（4 篇）

| 页文件 | 标题 |
|---|---|
| [`b01-state-machine.md`](b01-state-machine.md) | 状态机式智能体循环 |
| [`b02-prompt-assembly.md`](b02-prompt-assembly.md) | 系统提示组装 |
| [`b03-failure-hooks.md`](b03-failure-hooks.md) | 失败后命令与超时执行 |
| [`b04-prompt-template.md`](b04-prompt-template.md) | 提示模板与转义 |

### C-权限（2 篇）

| 页文件 | 标题 |
|---|---|
| [`c01-permission-judge.md`](c01-permission-judge.md) | 权限判定与批量只读跳过 |
| [`c02-permission-store.md`](c02-permission-store.md) | 权限记录与判定缓存 |

### D-安全审查（4 篇）

| 页文件 | 标题 |
|---|---|
| [`d01-security-inspector.md`](d01-security-inspector.md) | 安全审查统一入口 |
| [`d02-injection-scanner.md`](d02-injection-scanner.md) | 提示注入扫描 |
| [`d03-egress-inspector.md`](d03-egress-inspector.md) | 出口方向审查 |
| [`d04-adversary-inspector.md`](d04-adversary-inspector.md) | 对手模拟与配置 |

### E-扩展与工具（3 篇）

| 页文件 | 标题 |
|---|---|
| [`e01-extension-runtime.md`](e01-extension-runtime.md) | 扩展运行时与工具可见性 |
| [`e02-plugin-discovery.md`](e02-plugin-discovery.md) | 插件发现与作用域 |
| [`e03-large-response.md`](e03-large-response.md) | 大响应的落盘处理 |

### F-配方与技能（4 篇）

| 页文件 | 标题 |
|---|---|
| [`f01-recipe-model.md`](f01-recipe-model.md) | 配方模型与参数 |
| [`f02-recipe-manifest.md`](f02-recipe-manifest.md) | 配方文件清单与加载 |
| [`f03-recipe-validation.md`](f03-recipe-validation.md) | 配方校验与斜杠命令 |
| [`f04-skills.md`](f04-skills.md) | 技能与支撑文件 |

### G-提供方与模型（5 篇）

| 页文件 | 标题 |
|---|---|
| [`g01-provider-def.md`](g01-provider-def.md) | 提供方定义与类型 |
| [`g05-provider-retry.md`](g05-provider-retry.md) | 提供方重试与永久失败判定 |
| [`g02-model-fallback.md`](g02-model-fallback.md) | 模型回退与选型 |
| [`g03-canonical-cost.md`](g03-canonical-cost.md) | 成本估算与定价合并 |
| [`g04-declarative-providers.md`](g04-declarative-providers.md) | 声明式提供方 |

### H-配置与迁移（3 篇）

| 页文件 | 标题 |
|---|---|
| [`h01-config-experiments.md`](h01-config-experiments.md) | 配置与实验开关 |
| [`h02-config-migrations.md`](h02-config-migrations.md) | 配置迁移 |
| [`h03-oauth-persist.md`](h03-oauth-persist.md) | 凭据持久化 |

### I-ACP 接入（3 篇）

| 页文件 | 标题 |
|---|---|
| [`i01-acp-provider.md`](i01-acp-provider.md) | ACP 提供方与会话 |
| [`i02-acp-server.md`](i02-acp-server.md) | ACP 服务端与连接工厂 |
| [`i03-acp-handoff.md`](i03-acp-handoff.md) | 交接上下文裁剪 |

### J-上下文、调度与网关（4 篇）

| 页文件 | 标题 |
|---|---|
| [`j01-context-management.md`](j01-context-management.md) | 上下文压缩 |
| [`j02-scheduler.md`](j02-scheduler.md) | 调度器与定时配方 |
| [`j03-gateway-pairing.md`](j03-gateway-pairing.md) | 网关配对与白名单 |
| [`j04-observability.md`](j04-observability.md) | 追踪与遥测 |

## 关键取舍

**源码事实：** 审查器是可插拔契约（`ToolInspector` trait），安全审查器只是其中一个实现。多个审查器独立判定，结果由管理器合并成一个 `InspectionAction`。

**源码事实：** 安全审查器只在 `is_malicious && should_ask_user` 两条同时成立时要求用户批准。「判定」与「处置」是两个独立字段，不是单一布尔。

**源码事实：** 每条审查结果都带 `finding_id`，被拦时用户看到的提示里包含这个 id，使「用户看到的警告」与「日志里的记录」能对上。

**源码事实：** 失败后命令的执行有超时与 stderr 尾部截断（`MAX_COMMAND_STDERR_BYTES` = 8 KiB），且 `capture_tail` 保证小型诊断信息不被截断丢掉。

**源码事实：** 提供方重试的判定在独立的 `oi-kit-provider-types` crate 里（`should_retry` / `is_permanent_request_failure` / `RetryConfig`），与 agent 循环解耦。

**架构推论：** 这个项目最值得学的是「横切能力的分层密度」。同样要拦一次危险的工具调用，其他框架可能只有一道检查；goose 有权限、注入扫描、出口方向、对手复核四类独立检查，各管一个维度。代价是每类检查都要压成同一个动作词汇表，细节只能塞进 `reason`。

**架构推论：** 「配方（recipe）」是这个项目把 agent 任务从「对话」升级成「可复用产物」的机制。它把提示、工具、参数、子配方写成一个可校验的文件，于是同一任务可以重复执行、可以带参数、可以被调度器定时跑。这是 agent 从「临时会话」走向「工程资产」的关键一步。

## 源码落点

| 路径 | 符号 | 对应职责 |
|---|---|---|
| `crates/goose/src/agents/state_machine_builder.rs` | `typesafe_provider` | 智能体循环的有序操作 |
| `crates/goose/src/security/security_inspector.rs` | `SecurityInspector` | 安全审查器（检查链中的一个） |
| `crates/goose/src/tool_inspection.rs` | `ToolInspector` | 审查器的通用契约 |
| `crates/goose/src/permission/permission_judge.rs` | `PermissionJudgeContext` | 权限判定与只读批量放行 |
| `crates/goose/src/session/session_manager.rs` | `SessionManager` | 会话持久化与用量聚合 |
| `crates/goose/src/recipe/mod.rs` | `Recipe` | 配方模型与参数 |
| `crates/goose/src/context_mgmt/mod.rs` | `auto_compact_threshold` | 上下文压缩阈值 |
| `crates/goose/src/acp/handoff.rs` | `fit_unit` | 交接上下文按单元装箱 |
| `crates/oi-kit-provider-types/src/retry.rs` | `RetryConfig` | 提供方重试判定与退避 |
| `crates/goose/src/gateway/pairing.rs` | `PairingStore` | 远程网关配对 |

## 参考

- [goose 源码仓库](https://github.com/aaif-goose/goose)
- [goose 文档](https://block.github.io/goose/)
- [goose 文档：配方（Recipes）](https://block.github.io/goose/docs/guides/recipes/)
- [Agent Client Protocol](https://agentclientprotocol.com/)
- [goose 源码：tool_inspection.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/tool_inspection.rs)

## 与主核 / AI 的关系

goose 与主核（多模态数据库内核）在「**横切能力的组织方式**」上可以直接对照：

**审查器 vs 规划期校验。** 主核在查询执行前要过一系列检查（权限、限额、合法性），每个独立判定后合并成一个「执行或拒绝」。goose 把这套结构显式化成 trait + 结果类型 + 管理器，并且多了一个 `RequireApproval` 动作——这是主核里没有的第三种结局。它的价值在于承认了「有些操作不是非法，但需要人确认」。

**「判定」与「处置」分离。** 主核里对应的问题是：一个查询可能合法但昂贵（需限额确认），也可能非法（必须拒绝）。把这两件事混成一个布尔，就只有两种结局。goose 用两个字段表达，再在转换点合取。

**配方 vs 预编译语句。** 主核的预编译语句把一次查询的「形状」固化下来，参数与数据分离；goose 的配方把一次 agent 任务固化下来，参数与执行分离。两者都是「把一次性操作变成可复用资产」的同一种思路，只是固化对象不同。

**上下文压缩 vs 缓冲池淘汰。** 两者都在「有限容量里保住最重要的东西」。主核按访问频率与代价淘汰页面，goose 按工具调用边界成组压缩——后者必须成组，因为一条工具调用与其响应拆开会让模型看到无响应的调用。

对 2B 波次的整体判断：goose 是这批项目里最接近「生产运行时」的一个。它的机制清单像一份检查表：任何要上生产的 agent 系统，迟早都要有权限、审计、压缩、重试、调度、凭据管理这些东西。读写这个项目的方式建议是按需查阅——遇到具体运维问题时按机制领域翻，而不是通读。
