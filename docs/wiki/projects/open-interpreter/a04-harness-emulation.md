---
project: open-interpreter
feature: harness-emulation
title: Harness 请求模拟与路由
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/core/src/harness/request.rs
    symbols: [ChatHarnessRequest, build_chat_harness_request, apply_chat_harness_postprocess]
  - path: codex-rs/core/src/harness/routing.rs
    symbols: [resolve_stream_transport_route, harness_config_name, ClaudeCodeProfileRoute]
  - path: codex-rs/core/src/harness/guidance.rs
    symbols: [guidance_for_harness]
---

# Harness 请求模拟与路由

## 一句话职责

把「一次普通的对话请求」改写成「某个特定 agent 命令行工具会发出的那个请求」——包括系统提示的写法、工具集的名字与描述、消息的形态、请求头与计费标记——然后按选定的 harness 决定走哪个传输通道。

## 机制是什么

这是 open-interpreter 与 2B 其他项目最不同的一点。smolagents、browser-use、goose 都在设计**自己的** agent 循环；open-interpreter 的核心主张是：**便宜模型的效果很大程度取决于它被喂的提示是哪个「壳」的形状**。

所以它不发明新的循环，而是并行维护十几个 harness 实现（`core/src/harness/` 下有 `claude_code.rs` 4870 行、`kimi_cli.rs` 2772 行、`zcode.rs` 2593 行、`terminus_2.rs` 2032 行等），每个都复刻对应工具的请求外观。用户用 `/harness` 切换，同一个模型在不同壳下表现不同。

这套机制的实际含义：**提示工程的「形状」被当作一等公民**。不是「写一条好的系统提示」，而是「按某个已知有效工具的请求结构复刻一份」，因为那些结构是经过大量真实使用验证的。

## 怎么实现的

**三组常量与三个装配函数。** 以 `claude_code.rs` 为例，文件开头是一大批常量：beta 请求头、标题请求头、启动阶段的 `user-agent`、默认最大 token 数、不同模型变体的 token 上限、版本号、计费版本盐、计费头前缀。这些不是配置项，而是**硬编码的复刻值**——它们必须与真实的 Claude Code 客户端一致，否则服务端的行为会不同。

装配路径由 `request.rs` 提供：

- `ChatHarnessRequest` —— 一次 harness 请求的完整描述。
- `build_chat_harness_request` —— 按 harness 从内部对话模型构造出请求。
- `apply_chat_harness_postprocess` —— 请求构造完成后的后处理（各 harness 的差异点）。

**`guidance_for_harness` 的定位很窄。** 它只有 47 行，按 harness 返回一段额外的提示指引，其中明确写着 `KIMI_CLI_GUIDANCE`。这说明「复刻请求形状」与「追加额外指引」是两件分开的事：前者是主体，后者是少量补丁。源码把它做成一个集中函数而不是散在各 harness 里，是为了让「哪些 harness 需要额外指引」一眼可见。

**路由是独立的一层。** `routing.rs` 定义四种路由类型：`MessagesHarnessRoute`（Anthropic Messages 风格）、`ClaudeCodeProfileRoute`（Claude Code 的档案形态）、`ChatHarnessRoute`（Chat Completions 风格）、`StreamTransportRoute`（流式传输形态）。`resolve_stream_transport_route` 按 harness 选出该走哪条，`harness_config_name` 反向给出配置名。

这层的必要性在于：不同的壳不只用不同的请求体，还走**不同的传输协议**（Messages 与 Chat Completions 是两种完全不同的 JSON 结构，流式事件的形态也不同）。把路由抽出来，使「构造请求体」与「选择通道」解耦。

**`ClaudeCodeProfileRoute` 单列的理由。** Claude Code 的档案形态与其他 harness 有本质差别：它带有 OAuth 档案概念（账号级配置），而不只是请求形状。所以它的路由类型单独一个，而不是复用 `MessagesHarnessRoute`。

## 关键取舍

**源码事实：** 各 harness 实现是**大文件**：`claude_code.rs` 4870 行、`kimi_cli.rs` 2772 行、`zcode.rs` 2593 行。差异不是靠配置参数化，而是每个 harness 一份独立实现。

**源码事实：** 请求头、版本号、计费版本盐、计费头前缀都是硬编码常量（例如 `CLAUDE_CODE_VERSION`、`CLAUDE_CODE_BILLING_VERSION_SALT`、`CLAUDE_CODE_BILLING_HEADER_PREFIX`），不是可配置项。

**源码事实：** `guidance_for_harness` 是 47 行的集中函数，与各 harness 的大实现分处不同文件。

**源码事实：** 路由被切成四种类型，其中 Claude Code 的档案形态单独一类，不复用 Messages 路由。

**架构推论：** 「复刻请求形状」这条路线的代价极高——每个上游工具改一次版本，对应的 harness 就要跟着改，且是硬编码常量级的对齐。收益是：模型看到的是它被训练/调优时熟悉的形状，于是同一个便宜模型的效果显著提升。这是把「兼容性」当成「性能手段」而非「锦上添花」。

**架构推论：** 与 goose、smolagents 的对比能看清两种工程观。它们优化「框架本身的机制质量」（循环、权限、压缩），open-interpreter 优化「模型看到什么的形状」。前者不依赖外部工具的行为，后者必须持续跟踪外部工具的变化。两者都不是错，但维护成本的结构完全不同：前者是「设计一次、慢慢演进」，后者是「持续追版本」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/core/src/harness/request.rs` | `ChatHarnessRequest` | 一次 harness 请求的完整描述 |
| `codex-rs/core/src/harness/request.rs` | `build_chat_harness_request` | 按 harness 构造请求 |
| `codex-rs/core/src/harness/request.rs` | `apply_chat_harness_postprocess` | 请求后处理（各 harness 差异点） |
| `codex-rs/core/src/harness/routing.rs` | `resolve_stream_transport_route` | 按 harness 选出传输路由 |
| `codex-rs/core/src/harness/routing.rs` | `harness_config_name` | 路由到配置名的反向映射 |
| `codex-rs/core/src/harness/routing.rs` | `ClaudeCodeProfileRoute` | Claude Code 档案形态的单列路由 |
| `codex-rs/core/src/harness/guidance.rs` | `guidance_for_harness` | 少量 harness 的额外提示指引 |
| `codex-rs/core/src/harness/claude_code.rs` | `CLAUDE_CODE_VERSION` | 硬编码的上游版本常量 |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [open-interpreter 文档：Harness](https://www.openinterpreter.com/docs/terminal/harness)
- [open-interpreter 文档：模型提供方指南](https://www.openinterpreter.com/docs/terminal/providers)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)

## 与主核 / AI 的关系

这套机制与主核（多模态数据库内核）的交集在**「协议兼容层的成本结构」**上。

主核要兼容 PostgreSQL 的 SQL 方言与客户端协议。做法有两种：一是实现标准后让客户端适配，二是复刻客户端的具体行为细节（包括它对错误码的期待、对字段顺序的依赖）。主核选的是前者加少量兼容特例，因为 SQL 标准是稳定的；open-interpreter 选的是**大规模后一种**，因为 agent 工具的请求形状没有标准，且**每一个细节都影响模型效果**。

这个差别揭示了一条判断准则：**当「兼容」只是可用性问题时，按标准实现；当「兼容」直接影响核心指标（这里是模型效果）时，只能逐细节复刻，并接受持续追版本的维护成本。**

`guidance_for_harness` 那种「集中小补丁」的做法也值得记：当主体机制必须分散时，把「例外清单」集中到一处，至少让「谁需要例外」可查。主核里对应的是「方言差异登记表」——同样是把分散的差异集中成一张表，以便随标准演进逐条删除。
