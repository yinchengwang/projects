---
project: goose
feature: security-inspector
title: 安全审查统一入口
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/security/security_inspector.rs
    symbols: [SecurityInspector]
  - path: crates/goose/src/security/mod.rs
    symbols: [SecurityManager, SecurityResult]
---

# 安全审查统一入口

## 一句话职责

把安全检查从「智能体循环里的某一段」提升成「一类可插拔的审查器」：`SecurityInspector` 实现通用的 `ToolInspector` 契约，把安全模块的判定结果翻译成循环能消费的 `InspectionAction`。

## 机制是什么

goose 的工具有一条「检查链」：模型请求执行某个工具后，在真正执行前要经过若干审查器（inspector）。审查器各自独立判定，结果被汇总成一个动作：放行、要求用户批准、或拒绝。

`SecurityInspector` 是其中一个审查器，但它不是安全机制本身——它是一层**适配器**：把 `SecurityManager` 的判定（`SecurityResult`）翻译成检查链的通用结果（`InspectionResult`）。

这个分层的价值在于：安全检查的内部逻辑（正则匹配、分类器、对手模型复核）可以自由演进，而检查链只认识一个稳定的抽象。于是安全模块能被替换或补充，而不需要改智能体循环。

## 怎么实现的

**契约在 `tool_inspection.rs`。** 那里定义了三样东西：

- `ToolInspector` —— 审查器 trait，只有一个核心方法 `inspect`，入参是「会话 id、待检查的工具请求、消息历史、goose 模式」，出参是一组 `InspectionResult`。
- `InspectionResult` —— 每个结果绑定一个 `tool_request_id`，带动作、理由、置信度、审查器名与可选的 `finding_id`。
- `InspectionAction` —— 动作枚举，包含 `Allow` 与 `RequireApproval(Option<String>)`。

还有 `ToolInspectionManager`（把多个审查器串起来跑）与 `apply_inspection_results_to_permissions`（把审查结果落到权限状态里）。

**`SecurityInspector` 只做三件事。** 它的 `inspect` 调 `security_manager.analyze_tool_requests(tool_requests, messages)`，拿到一组 `SecurityResult`，然后逐个转换。转换逻辑很短但两个判据值得注意：

```
is_malicious && should_ask_user  ->  RequireApproval(提示文案)
否则                              ->  Allow
```

也就是说**只有同时满足「判定为恶意」与「需要问用户」两条才拦**。这是两个独立判断的合取，不是单一「是否恶意」：某些被判定为恶意的调用可能因为策略配置不需要打断用户（例如在某个模式下已预先授权），而某些需要问用户的调用可能并不恶意（例如只是高风险操作）。源码把这两件事分开建模，转换点才需要显式合取。

**提示文案带 finding id。** 被拦时给出的文案包含 `Security Alert`、解释文本与 `Finding ID: {finding_id}`。finding id 的作用是让用户能把「我看到的这条警告」与「遥测/日志里的那条记录」对上——否则用户只能描述现象，无法定位具体判定。

**`InspectionResult` 携带审查器名。** `inspector_name: self.name()` 返回 `"security"`。当多个审查器同时给出结果时，界面与日志能区分是谁拦的。

## 关键取舍

**源码事实：** `SecurityInspector` 的 `inspect` 忽略 `_session_id` 与 `_goose_mode` 两个入参（参数名前的下划线就是标记）。安全判定目前不依赖会话上下文与运行模式；模式相关的策略在更上层（权限层）处理。

**源码事实：** 拦截条件是 `is_malicious && should_ask_user` 的合取，不是单一布尔。这把「判定」与「处置」分开了：同一个判定在不同策略下可能有不同处置。

**源码事实：** 每个 `InspectionResult` 都带 `finding_id`（`Option<String>`），安全审查器总是填上。这让审查结果可以被外部系统引用。

**架构推论：** 「审查器是可插拔的」这个设计比「安全模块是一段代码」强得多。它允许多个独立检查并存（安全、权限、出口、对手）而不需要在循环里排优先级——每个检查只管自己给出结果，合并由管理器负责。代价是每个检查都必须把结论压成统一的 `InspectionAction` 词汇表，细节只能放在 `reason` 里。

**架构推论：** 用 `tool_request_id` 而不是「工具名 + 参数」作为结果的主键，说明这套检查的语义是「一次具体的请求」，不是「某种请求」。这对并发/批量工具调用很关键：同一轮里可能有两个完全相同的调用，只有 id 能区分。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/security/security_inspector.rs` | `SecurityInspector` | 把安全判定翻译成检查链结果 |
| `crates/goose/src/security/mod.rs` | `SecurityManager` | 安全判定的实际执行者 |
| `crates/goose/src/security/mod.rs` | `SecurityResult` | 判定结果的结构与 `should_ask_user` 判据 |
| `crates/goose/src/tool_inspection.rs` | `ToolInspector` | 审查器的通用契约 |
| `crates/goose/src/tool_inspection.rs` | `InspectionAction` | 放行 / 要求批准的动作词汇表 |
| `crates/goose/src/tool_inspection.rs` | `ToolInspectionManager` | 多个审查器的串接与结果合并 |

## 参考

- [goose 源码：security/security_inspector.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/security/security_inspector.rs)
- [goose 文档：安全](https://block.github.io/goose/docs/guides/security/)
- [goose 源码：tool_inspection.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/tool_inspection.rs)
- [OWASP：LLM 应用中的过度代理](https://genai.owasp.org/llmrisk/llm062024-excessive-agency/)

## 与主核 / AI 的关系

审查器模式与主核（多模态数据库内核）里的「规划期校验钩子」是同一种结构：主核在查询执行前要过一系列检查（权限、资源限额、语法合法性），每个检查独立判定，最终合并成一个「执行或拒绝」的决定。goose 把这套结构显式化成一个 trait + 结果类型 + 管理器。

**「判定」与「处置」分离**这条最值得借鉴。主核里对应的问题是：一个查询可能合法但昂贵（需要限额确认），也可能非法（必须拒绝）。如果把这两件事混成一个布尔，就只有「执行」与「报错」两种结局，无法表达「需要用户确认」。goose 用 `is_malicious` 与 `should_ask_user` 两个字段表达，再在转换点合取，恰好覆盖这两种结局。

**finding id 的用途**可以对照主核的审计日志：主核给每次拒绝/告警生成可检索的标识，用户报障时能直接对上。这是「让用户能描述问题」的基础设施，容易被忽略但价值很高。
