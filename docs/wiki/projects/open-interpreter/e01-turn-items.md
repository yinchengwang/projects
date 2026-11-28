---
project: open-interpreter
feature: turn-items
title: 回合条目模型
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/protocol/src/items.rs
    symbols: [TurnItem, UserMessageItem, FunctionCallOutputItem]
  - path: codex-rs/protocol/src/items.rs
    symbols: [HookPromptItem, AgentMessageItem, AsyncUserInputQuestion]
---

# 回合条目模型

## 一句话职责

把「一个回合里发生过的每一件事」写成一种**带类型标签的统一条目**（`TurnItem` 十九个变体），于是界面、协议与持久化层都只需要认一个枚举，而不是各自维护「什么算一条消息」的口径。

## 机制是什么

`items.rs` 869 行，核心是一个枚举加它每个变体的载荷结构：

| 层 | 内容 |
|---|---|
| 判别 | `TurnItem` 用 `#[serde(tag = "type")]`——线格式里靠 `type` 字段区分变体 |
| 载荷 | 每个变体一个结构：`UserMessageItem`、`FunctionCallOutputItem`、`HookPromptItem`、`AgentMessageItem`、`PlanItem`、`ReasoningItem`、`CommandExecutionItem`、`DynamicToolCallItem`、`CollabAgentToolCallItem`、`SubAgentActivityItem`、`WebSearchItem`、`ImageViewItem`、`ExtensionItem`、`ImageGenerationItem`、`EnteredReviewModeItem`、`ExitedReviewModeItem`、`FileChangeItem`、`McpToolCallItem`、`ContextCompactionItem` |
| 统一出口 | `TurnItem::id()`：一个穷尽 match，每个变体交出自己的 id |
| 特殊形态 | `HookPromptItem` 有一套「条目 ↔ 模型消息」的 XML 编解码 |

双标签体系（`TS` 与 `JsonSchema`）说明这个枚举同时被三个消费者读：Rust 内部、生成的 TypeScript 类型、以及 JSON Schema 校验。

## 怎么实现的

**`#[allow(clippy::large_enum_variant)]` 在枚举上是刻意的。** 十九个变体的载荷大小不一，clippy 建议装箱最大的那个。这里的取舍与前一个项目的 `e01` 同源：**装箱会让每处读载荷的地方多一次解引用与 match**。这句话在 `TurnItem::id()` 上体现得最直接——那个函数有十九个分支，每个分支只做一件事：取 `id`。

**`id()` 的形状暴露了一个真实的类型差异。** 除 `Extension` 之外的十八个分支都是 `item.id.clone()`，而 `Extension(item) => item.id().to_string()`。差别在于 `ExtensionItem` 是**另一个 crate 的类型**（`codex_extension_items`），它的 `id()` 返回 `&str` 而不是持有 `String`。

也就是说 `TurnItem::id()` 存在的意义是把「id 可能以 String 或 &str 形式存在」这件事收敛到一处。调用方拿到的是 `String`，不必关心某变体的载荷是自持的还是借用的。这是一个很小的适配，但它让上层可以对所有条目一视同仁。

**`Extension` 与 `ImageGeneration` / `WebSearch` 的三方分工写在了文档注释里。** 三个变体都在处理「web search」与「image generation」，而注释区分得极细：

- `WebSearch`：**由 core 直接处理的 Hosted Responses API 网页搜索条目**；而**独立的 web search 走 `Extension`**，因为「它的展示 schema 由 web-search 扩展拥有」。
- `Extension`：**schema 与生命周期细节由某个扩展拥有的条目**——独立的图像生成、sleep、web search 都走这条。
- `ImageGeneration`：**由 core 直接处理的 Hosted Responses API 图像生成条目**，且明确「它仍与 `Extension` 分开，因为 core 还拥有 hosted 图像持久化与 legacy 事件分发」。

这三条注释合起来说明一件事：**「谁拥有 schema」与「谁拥有持久化」是两个独立的归属维度**。同一个语义动作（比如「生成一张图」）在 hosted 路径下由 core 拥有持久化，在独立扩展路径下由扩展拥有 schema，于是需要两个不同变体——而不是一个变体加一个 `source` 字段。

这是一个值得记的判断：当两个维度正交时，用「具体变体」比用「变体 + 标志位」更容易保持不变量，代价是枚举会长。

**`TurnItem` 的变体集合实际上是一份「回合里会发生什么」的清单**，而这份清单的广度是这层的价值。它覆盖了：用户消息、函数调用输出、钩子注入的提示、助手消息、计划、推理、命令执行、动态工具调用、协作 agent 调用、子 agent 活动、网页搜索、图像查看、扩展条目、图像生成、进入/退出评审模式、文件变更、MCP 工具调用、上下文压缩。

界面不需要知道「这些东西各自怎么产生」，只需要知道「它们都是一条条目」。这就是统一模型的作用。

### 钩子提示条目：条目与模型消息之间的双向编解码

**`HookPromptItem` 是这套模型里唯一带自定义线格式的变体。** 它装的是一个 `Vec<HookPromptFragment>`，每个片段有 `text` 与 `hook_run_id`。而在线格式里，这两个字段用 `camelCase` 命名（`hookRunId`）。

而**它到模型消息的转换是另一个形态**：`build_hook_prompt_message(fragments)` 把片段序列化成 XML 文本，塞进一个 `role: "user"` 的 `ResponseItem::Message`；`parse_hook_prompt_message` 做反向。

中间的 XML 结构 `HookPromptXml` 只有两个字段：

```
#[serde(rename = "@hook_run_id")] hook_run_id: String,   // XML 属性
#[serde(rename = "$text")] text: String,                 // 元素文本
```

也就是说模型看到的是一段形如「带 `hook_run_id` 属性的 `hook_prompt` 元素」的文本。**为什么要绕这一圈，而不是直接用 JSON？** 因为这段东西要塞进模型的对话消息里——而对话消息的形态是纯文本块（`ContentItem::InputText`）。用 XML 是为了让「这段文本是机器可辨识的结构」与「它在消息里是一个文本块」同时成立。

于是 `parse_hook_prompt_message` 能从对话历史里把这类消息**还原**成结构化条目。这解释了整套编解码的必要性：**条目模型是结构化的，而模型看到的是文本，所以两个方向各需要一次转换**。

**两个方向都要求 `hook_run_id` 非空，这保证了往返一致。** `build_hook_prompt_message` 先 `.filter(|f| !f.hook_run_id.trim().is_empty())`，`serialize_hook_prompt_fragment` 再判一次；`parse_hook_prompt_fragment` 解析后 `if hook_run_id.trim().is_empty() { return None }`。

三条守卫的效果是：**没有 run id 的片段根本不会出现在消息里，也就不会被解析回来**。而 run id 是「这段提示来自哪次钩子执行」的追溯键——一个空的 run id 会让这段文本变成一段无来源、也无法归因的注入。

**`build_hook_prompt_message` 在全空时返回 `None` 而不是空消息。** `if content.is_empty() { return None }`——于是「一次没有任何有效片段的钩子提示」不会在对话里产生一条空消息。这个方向是对的：空消息会污染上下文并可能触发模型的奇怪反应。

**`parse_hook_prompt_message` 用 `collect::<Option<Vec<_>>>()?` 实现全或无。** 那行是：

```
content.iter().map(...).collect::<Option<Vec<_>>>()?
```

`Option` 的 `FromIterator` 语义是「任何一个 `None` 就让整体为 `None`」。于是**只要有一条内容块不是可解析的 `hook_prompt` XML，整条消息就不算钩子提示消息**。

这个严格是必要的：一条消息要么整体是钩子注入的，要么不是。若允许部分解析，就会产出一条「半个钩子提示」的条目，而它的 `text` 语义与 run id 归属都不完整。

### 助手消息：三个可选字段各解决一个界面问题

**`AgentMessageItem` 的字段比其它条目多，且每个可选字段的来源都写在注释里。**

| 字段 | 注释里的用途 |
|---|---|
| `phase: Option<MessagePhase>` | **被 TUI 渲染用来区分回合中途的评论与最终答案，避免状态指示器抖动** |
| `memory_citation: Option<MemoryCitation>` | 引自 `ResponseItem::Message` 的引用信息 |
| `delivery: Option<AgentMessageDelivery>` | 投递方式，枚举目前只有 `Async` |
| `questions: Option<Vec<AsyncUserInputQuestion>>` | 异步提问 |

`phase` 那条注释是本页最实用的一处：**「回合中途的评论」与「最终答案」在文本形态上完全一样**，而界面要用不同的展示（前者是一条旁注，后者是结论）。没有 phase 时，界面只能靠「这是不是最后一条」来推断，而这个推断会在答案之后又出现新消息时抖动——状态指示器一闪一闪。

所以这里把 `phase` 从模型的 `ResponseItem` 一路带到条目层，并明确写了「不是所有 provider/模型都会发它，消费者应当在有它时使用、没有时保留 legacy 完成语义」。

**这一句是整段的关键：`Option` 意味着「上游可能不提供」，而处理方式是「退回旧语义」而不是「报错」。** 这就是跨 provider 的协议字段应有的写法。

**`AsyncUserInputQuestion` 带 `deny_unknown_fields`。** 这个结构只有 `title` 与 `options`（后者是 `Option<Vec<String>>`），而 `deny_unknown_fields` 让「上游多发了一个字段」变成解析失败而不是静默忽略。

这与 `AgentMessageItem` 的宽容方向相反，而差值是有理由的：`AgentMessageItem` 是**长期演进的消息载体**（要容忍新旧字段），`AsyncUserInputQuestion` 是**给用户看的问答结构**（多出来的字段没有地方展示，忽略它会让界面与实际数据不一致）。

**`AgentMessageDelivery` 只有一个变体 `Async`。** 一个单变体枚举通常意味着「将来会有同步/流式之类的第二个值」。它的存在让 `delivery: Option<...>` 的语义现在就能表达「这是一条异步投递的消息」，而不必用一个布尔。

## 关键取舍

**源码事实：** `TurnItem` 有十九个变体、单一 `tag = "type"` 判别、`#[allow(clippy::large_enum_variant)]` 抑制装箱。统一出口是穷尽 match 的 `id()`。

**源码事实：** `Extension` 分支写 `item.id().to_string()` 而其余分支写 `item.id.clone()`——因为 `ExtensionItem` 来自另一个 crate 且 `id()` 返回 `&str`。`TurnItem::id()` 收敛了这个类型差异。

**源码事实：** web search 与 image generation 各有两个变体（hosted 走 core、独立走 Extension），理由是「谁拥有 schema」与「谁拥有持久化」是两个正交维度。

**源码事实：** 钩子提示条目在模型侧是 XML 文本（属性 `hook_run_id` + 元素文本 `text`），因为它必须塞进纯文本内容块，而条目层要保持结构化。

**源码事实：** 空 `hook_run_id` 在构造、序列化、解析三处都被拦下，于是没有来源的注入片段压根不进入消息，也不会被解析回来。全空时 `build_hook_prompt_message` 返回 `None` 而非空消息。

**源码事实：** `parse_hook_prompt_message` 用 `collect::<Option<Vec<_>>>()` 实现全或无——一条消息要么整条是钩子提示，要么不是。

**源码事实：** `AgentMessageItem.phase` 的可选性带明确说明：不是所有 provider 都发，消费者应在有它时用、没有时退回 legacy 完成语义。注释给出的动机是 TUI 要区分中途评论与最终答案以避免状态指示器抖动。

**源码事实：** `AsyncUserInputQuestion` 用 `deny_unknown_fields`，与 `AgentMessageItem` 的宽容方向相反。

**架构推论：** 条目模型的价值在于**把「发生了一件事」与「这件事怎么显示」分开**。十九个变体各自只是一个数据形状，界面拿到 `type` 就知道该渲染成什么；而 `id()` 是唯一被所有变体共同承担的行为。

这也解释了为什么这层不负责「顺序」「分组」「生命周期」——它只是形状。回合的顺序在别处（流式事件序列），生命周期在别处（状态机）。一个纯粹的判别联合是最省的中间层。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/protocol/src/items.rs` | `TurnItem` | 十九个变体的回合条目判别联合 |
| `codex-rs/protocol/src/items.rs` | `TurnItem::id` | 穷尽 match 的统一 id 出口，收敛 String/&str 差异 |
| `codex-rs/protocol/src/items.rs` | `UserMessageItem` | 用户消息：id、可选 client_id、用户输入内容 |
| `codex-rs/protocol/src/items.rs` | `FunctionCallOutputItem` | 函数调用输出：id、名字、可选命名空间、输出体 |
| `codex-rs/protocol/src/items.rs` | `HookPromptItem` | 钩子注入的提示条目，装片段列表 |
| `codex-rs/protocol/src/items.rs` | `HookPromptFragment` | 单个片段：文本 + `hookRunId`（camelCase 线格式） |
| `codex-rs/protocol/src/items.rs` | `build_hook_prompt_message` | 片段 → XML 文本消息；全空返回 `None` |
| `codex-rs/protocol/src/items.rs` | `parse_hook_prompt_message` | 消息 → 条目；全或无（`collect::<Option<_>>`） |
| `codex-rs/protocol/src/items.rs` | `AgentMessageItem` | 助手消息，含 `phase` / `delivery` / `questions` 等可选字段 |
| `codex-rs/protocol/src/items.rs` | `AgentMessageDelivery` | 单变体 `Async`，为将来的投递方式留位 |
| `codex-rs/protocol/src/items.rs` | `AsyncUserInputQuestion` | 提问结构，`deny_unknown_fields` |

## 参考

- [open-interpreter 源码仓库](https://github.com/OpenInterpreter/open-interpreter)
- [serde 文档：枚举表示（内部标签）](https://serde.rs/enum-representations.html)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档：终端](https://www.openinterpreter.com/docs/terminal)

## 与主核 / AI 的关系

这套「带判别标签的统一条目」与主核里「统一的行格式描述符」是同一个抽象：数据库的元组描述符把「这一列是什么类型」压成一种可枚举的标签，于是执行器只需按标签分派。两者的共同收益是**下游不必为每个来源写一套处理逻辑**。

对 agent 工程的直接价值有三条。其一，**「谁拥有 schema」与「谁拥有持久化」是两个维度，不要压进一个标志位**。这里用两个变体（hosted / Extension）表达，而不是一个变体加 `source` 枚举——后者会让「这个 source 能不能带这个字段」变成运行期检查。其二，**模型看到文本、程序需要结构，两个方向都要显式编解码**。钩子提示的 XML 编解码是这类问题的标准形态，而两边都校验 run id 才保证往返一致。其三，**可选字段要写明「缺失时怎么办」**。`phase` 的注释说「不是所有 provider 都发，没有时退回 legacy 语义」，这句话让调用方不必猜；而一个只写「optional」的字段，下一个人一定会写出「缺失就当成某一种」的隐式假设。
