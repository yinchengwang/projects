---
project: open-interpreter
feature: acp-server
title: ACP 服务端与配置项
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/acp-server/src/lib.rs
    symbols: [AppServerAcpAgent, SessionState, AcpSessionMode]
  - path: codex-rs/acp-server/src/lib.rs
    symbols: [CONFIG_ID_MODEL, CONFIG_ID_REASONING_EFFORT, handle_notification]
---

# ACP 服务端与配置项

## 一句话职责

把内部的 app-server 包成 ACP 契约：用一张会话表把 ACP 的 `SessionId` 映射到内部的 `thread_id` 与一套模式、模型、努力度、harness 的状态，并把 app-server 的通知流转译成 ACP 的 `SessionUpdate`——其中「改 harness」不是改字段，而是**换线程**。

## 机制是什么

`AppServerAcpAgent` 四字段：`client`（进程内 app-server 的请求句柄）、`event_rx`（事件流）、`sessions: Mutex<HashMap<SessionId, SessionState>>`（会话表）、`config: Arc<Config>`。

`SessionState` 八字段：`thread_id: String`、`cwd: PathBuf`、`model: String`、`model_provider: String`、`reasoning_effort: Option<ReasoningEffort>`、`harness: Option<String>`、`mode: AcpSessionMode`、`active_turn_id: Option<String>`。

`AcpSessionMode` 三值：`ReadOnly`、`WorkspaceWrite`、`FullAccess`。通过 `from_mode_id` / `mode_id` 在枚举与字符串之间双向换算。

三个配置项的 id 都是编译期常量：`CONFIG_ID_MODEL = "model"`、`CONFIG_ID_REASONING_EFFORT = "reasoning_effort"`、`CONFIG_ID_HARNESS = "harness"`。

## 怎么实现的

**`AcpSessionMode` 是两个策略的联合表达。** 这个枚举最有分量的一点是它同时决定了两个独立的安全策略：

`approval_policy()`：`ReadOnly | WorkspaceWrite` → `AskForApproval::UnlessTrusted`；`FullAccess` → `Never`。

`sandbox_policy(cwd)`：

- `ReadOnly` → `SandboxPolicy::ReadOnly { network_access: false }`
- `WorkspaceWrite` → `SandboxPolicy::WorkspaceWrite { writable_roots: vec![cwd], network_access: false, exclude_tmpdir_env_var: false, exclude_slash_tmp: false }`
- `FullAccess` → `SandboxPolicy::DangerFullAccess`

所以三个模式是**成对**定义的：只读模式既不能写也不能联网且要审批；工作区写模式只能写 cwd 且不能联网且要审批；完全访问模式不审批且无沙箱。

`WorkspaceWrite` 分支里的 `try_into().map_err(|_| Error::invalid_params().data("session cwd must be absolute"))` 是一个必要的转换——`writable_roots` 要的是绝对路径类型，而 `cwd` 是 `PathBuf`。相对路径在这里被拒绝，错误消息直接说明原因。

这个错误用 `invalid_params` 而不 `internal_error` 是对的：cwd 是客户端传来的，路径不合法是客户端的问题。

**两个模式和审批策略的映射是有意的不对称。** `ReadOnly` 用 `UnlessTrusted`——理论上只读不需要审批，但它仍然要。理由是只读模式下的命令执行仍可能读敏感内容（读取任意文件、跑任意命令）。而 `FullAccess` 用 `Never`——既然已经放弃沙箱，再加审批只是徒增交互。

**改 harness 是一次 fork。** `set_session_config_option` 的三个分支里，前两个都是「改字段」：

- `CONFIG_ID_MODEL` → 取出 value id 写进 `session.model`。
- `CONFIG_ID_REASONING_EFFORT` → `parse_reasoning_effort` 后写进 `session.reasoning_effort`。

第三个完全不同：

1. 先校验请求的 harness 存在于 `interpreter_harnesses(&session)` 的列表里（除非请求的是空串）。
2. 发一个 `ClientRequest::ThreadFork`，把 `"harness"` 作为配置项传给新线程。
3. **把 `session.thread_id` 换成 fork 出来的新线程 id**，并把 `session.harness` 更新。

所以「切换 harness」在内部表现为「开一个新线程」，而不是「给现有线程换配置」。这解释了为什么 `SessionState` 里的 `thread_id` 是可变字段而不是不可变标识。

这个设计的必要性可以推断：harness 决定的是提示形状与工具集（见 a04 的 harness 模拟），而这些在内部是**线程级**的配置——线程创建时定下，不能中途改。所以 ACP 的「设置一个配置项」这个语义，内部只能通过「fork 一个带着新配置的线程」来实现。

允许空串是一个刻意的例外：`if selected_harness.is_none() && !requested_harness.is_empty()` —— 所以「请求空 harness」不报错。空串意味着「不指定 harness」，那不需要在列表里存在。

注意 `selected_harness` 的类型是 `Option<Option<String>>`（外层表示「找没找到」，内层表示「harness 自身有没有 id」）。这一层嵌套反映了两件事都要区分：找不到 vs 找到了但 id 为空。随后 `session.harness = selected_harness.flatten()` 把两层压成一层——空 id 与没找到都变成 `None`。而传给 fork 的 `config` 用的是 `serde_json::json!(selected_harness.clone())`，即传的是**外层**（含「找不到」这个信息）。这两处的差别说明「fork 时要知道是否显式指定」而「状态里只关心最终值」。

**三个分支的锁模式不一致，是一个值得注意的细节。** 前两个分支在拿到新值之后才 `self.sessions.lock()` 并 `get_mut`；第三个分支则先读一次（`session_state` 在函数开头已经拿过），做一次网络外的 fork 请求，**再**拿锁更新。

所以第三个分支在 fork 期间不持锁。这是必须的——fork 是一个 `.await`，若持锁等待会让整个会话表冻结。代价是「fork 完成到写入之间」有一个窗口，期间另一个请求可能改了 `session.model` 之类。但因为它们改的是不同字段，且都在同一个 `sessions` 锁下做单次 `get_mut`，所以不会丢更新（只是可能基于稍旧的 thread_id 做了 fork）。

**返回值是「重读后的完整配置列表」。** 三个分支共用的尾部：

```rust
let session = self.session_state(&request.session_id).await?;
Ok(SetSessionConfigOptionResponse::new(self.config_options(&session).await?))
```

所以每次设置配置项都返回**全部**配置项的当前值。这让客户端不必自己维护增量——一次响应就把状态同步完。这也是 ACP 这类协议常见的做法：设置操作的响应往往是完整状态快照。

**`config_options` 每次重新组装三项。** 三项都是 `SessionConfigOption::select`，但内容形态不同：

- Model：选项列表只有**一项**——`SessionConfigSelectOption::new(session.model.clone(), session.model.clone())`。即「当前模型的标签与值都是模型名」。所以 model 是一个「自由输入」的选择器，只有一个当前值可选。
- Reasoning：`reasoning_options()` 给出五档（`default`、`minimal`、`low`、`medium`、`high`），当前值取 `session.reasoning_effort` 的 `to_string()`，`None` 时给 `"default"`。
- Harness：选项来自 `interpreter_harnesses(&session)`，逐项 `SessionConfigSelectOption::new(harness_id, harness.label).description(harness.description)`。

三者的 `category` 也不同：`Model`、`ThoughtLevel`、`Other("_harness".to_string())`。前两个用协议定义的标准分类，第三个用自定义字符串——所以 harness 在 ACP 概念里没有对应的标准分类，只能挂在 `Other` 下。

Model 只给一个选项这一点很值得记：它意味着「模型列表」在 ACP 层不是由这里提供的。客户端要列可选模型得走别的接口（模型目录），这里只报告「当前是什么」。

**`handle_notification` 是通知翻译器，过滤判据都是「三元相等」。** 它接收 `session_id`、`thread_id`、`turn_id`、`notification`、`cx`、`completed_item_ids`，返回 `Result<Option<StopReason>>`——`Some` 表示这一轮结束了。

六个分支的 guard 全是 `payload.thread_id == thread_id && payload.turn_id == turn_id` ——即「这条通知属于我这个线程的这个回合」。所以同一个 app-server 里有多个线程在跑时，各自的事件不会串到别的 ACP 会话。

翻译映射：

- `AgentMessageDelta` → `SessionUpdate::AgentMessageChunk`
- `ReasoningSummaryTextDelta` / `ReasoningTextDelta` → `SessionUpdate::AgentThoughtChunk`（两个来源合成一种更新）
- `TurnPlanUpdated` → `SessionUpdate::Plan`，逐项映射 `TurnPlanStepStatus` 三态
- `ItemStarted` → `SessionUpdate::ToolCall`，且只有 `tool_call_from_item` 认得的项才发
- `ItemCompleted` → `send_completed_item_update_if_new`
- `TurnCompleted` → 先把 `turn.items` 里所有未发过的项补发一遍，再 `return Ok(Some(stop_reason_from_turn(turn)))`

其余通知 `_ => {}` 静默丢弃——所以这是一个「白名单翻译」，未列出的通知被忽略而不是报错。

**`TurnCompleted` 的补发是必要的。** 前面 `ItemCompleted` 分支会实时的发；但 `TurnCompleted` 里那段循环 `for item in &turn.items { send_completed_item_update_if_new(...) }` 是在做「兜底」：若某个项完成事件没被收到（或收到时被过滤掉了），它仍会在回合结束时被发出去。而 `if_new` 保证已发过的不会重复。

`send_completed_item_update_if_new` 的实现就是 `completed_item_ids.insert(item.id().to_string())` 返回真才发。所以那个 `HashSet<String>` 是**去重的账本**，按 item id 键。

这个账本必须跨事件生命周期存在——所以它以 `&mut HashSet` 的形式被传进 `handle_notification`，而不是放在函数内。它的生命周期至少是一个回合。

**`stop_reason_from_turn` 把四态压成两态。** `Interrupted` → `Cancelled`；`Completed | Failed | InProgress` → `EndTurn`。

所以「失败」与「完成」在 ACP 层是同一个 `StopReason::EndTurn`。这个映射的合理性在于 ACP 的 `StopReason` 只表达「回合怎么结束的」，「结束得好不好」由消息内容与错误事件表达。而 `InProgress` 也被映射成 `EndTurn`——这是一个防御性兜底：收到 `TurnCompleted` 通知却说状态是 `InProgress`，那也只能按「结束」处理（否则没有别的返回路径）。

**`tool_call_from_item` 是类型到展示的映射。** 四类项被翻译：

- `CommandExecution { id, command, .. }` → `(id, command, ToolKind::Execute)` —— 标题就用命令本身。
- `FileChange { id, .. }` → `(id, "File change", ToolKind::Edit)` —— 固定文案。
- `McpToolCall { id, server, tool, .. }` → `(id, format!("{server}/{tool}"), ToolKind::Other)` —— 标题是 `server/tool`。
- `WebSearch(item)` → `(item.id, "Web search", ToolKind::Fetch)`。

其余返回 `None`（不发 `ToolCall` 更新）。

所以「哪些内部项该显示成工具调用」是一个显式白名单。标题的取舍也各有理由：命令有唯一文本可用，文件改动没有（只能概括），MCP 调用有服务器与工具名可组合，网页搜索也是固定文案。

**`build_prompt_items` 处理 ACP 的五种内容块。** 它把 `ContentBlock` 转成内部 `UserInput`：

- `Text` → `UserInput::Text { text, text_elements: Vec::new() }`。注意 `text_elements` 恒为空——注释在 compact 那边说过「合成的提示没有 UI 元素范围要保留」，这里同理：ACP 来的文本没有富文本标注。
- `Image` → `UserInput::Image { image: Inline { url: format!("data:{};base64,{}", mime_type, data) } }`。所以图片被转成 data URL 内联。
- `ResourceLink` → 转成文本 `[name](uri)` —— 链接被**降级成 Markdown**，不是当作结构化资源。
- `Resource(EmbeddedResource { TextResourceContents { text, uri } })` → 转成 `<context ref="uri">\n text \n</context>` —— 内嵌文本资源被包成带引用的上下文块。
- `Audio(_)` 与其余 `Resource(_)` → `None`，即丢弃。

所以这个转换是**有损**的：音频被丢掉，非文本的资源被丢掉，链接被降维成文本。这个方向的选择反映的是「内部只支持文本与图片」这个事实——不能表达的就不表达，而不是硬塞成一个可能被误解的形态。

`<context ref="...">` 那个包装很有参考价值：它把「这段文本来自某个 URI」这个元信息嵌进了内容本身，因为内部 `UserInput::Text` 没有「来源」字段。用伪 XML 包起来，模型能看出这是引用而非用户原话。

**`parse_reasoning_effort` 六分支，`default` 与空串都映射成 `None`。** 所以「显式设成 default」与「没设」在内部是同一个状态。其余五个值有明确映射，未知值 `invalid_params`。

`reasoning_options()` 与它必须对齐：选项列表给的五档正是 `parse` 认得的字符串（除了空串不在选项里）。两处若不同步，就会出现「客户端能选一个服务端解析不了的值」——而 `parse` 会报错，表现为选中某个选项后设置失败。

**`default_model` 的兜底是 `"gpt-5.1-codex"`。** `config.model.clone().unwrap_or_else(|| "gpt-5.1-codex".to_string())` —— 配置没设就用这个硬编码名。而 `run_main` 的入口只做三件事：初始化 tracing、`AppServerAcpAgent::start`、`serve(ByteStreams::new(outgoing, incoming))`。`init_tracing` 的 writer 是 `std::io::stderr`——因为 **stdout 是协议通道**，任何日志输出都不能走 stdout。

`next_request_id` 用静态 `AtomicI64` 从 1 开始递增，包装成 `RequestId::Integer`。所以请求 id 是进程内单调的，不需要 UUID。

## 关键取舍

**源码事实：** 三个模式同时决定审批策略与沙箱策略，且 `ReadOnly` 与 `WorkspaceWrite` 都要求审批。

**源码事实：** 改 harness 走 `ThreadFork` 并替换 `session.thread_id`，不是改字段。

**源码事实：** 请求空 harness 不报错，即使它不在列表里。

**源码事实：** `ThreadFork` 的 config 传的是 `Option<Option<String>>`（外层含「找到没」），而状态里存的是 `flatten()` 后的 `Option<String>`。

**源码事实：** 三个配置项设置完后都返回完整的配置列表。

**源码事实：** `handle_notification` 六个分支的 guard 都是线程 id 与回合 id 双相等，未列出的通知静默丢弃。

**源码事实：** `TurnCompleted` 会补发 `turn.items` 里所有未发过的项，靠 `HashSet<String>` 按 item id 去重。

**源码事实：** `Failed` 与 `Completed` 在 ACP 层都映射成 `EndTurn`。

**源码事实：** `build_prompt_items` 丢弃音频与非文本资源，把链接降级成 Markdown，把内嵌文本包成 `<context ref="...">`。

**架构推论：** 「改配置项 = fork 新线程」这个实现揭示了一个更深的东西：**配置的可变性取决于它在哪一层被消费**。harness 在内部是线程创建时的输入，所以它对内不可变；而 ACP 协议把「配置项」定义成会话内可变的。两层语义冲突时，唯一办法是在边界上做一次结构性转换（fork），而不是强行让内层变得可变。这类「协议语义比内部模型更宽」的情况在任何协议适配层都会出现。

**架构推论：** 通知翻译采用白名单 + 双 id 过滤，是因为 app-server 的事件是**多路复用**的：同一个进程里可能同时有多个线程在跑，事件流是共享的。若不过滤，一个会话的消息就会出现在另一个会话里。而双 id（线程与回合）而不是单 id，是因为同一线程里也可能有并发的回合。

**架构推论：** `TurnCompleted` 的补发机制与去重账本，解决的是「流式事件可能丢」这个现实。流式协议的完成事件不保证按序、不保证不丢；把「回合结束时的完整清单」当成权威，并用账本去重，是这类协议的通用兜底。它让「漏了一个中间事件」不至于变成「少显示一个工具调用」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/acp-server/src/lib.rs` | `AppServerAcpAgent` | ACP 契约与内部 app-server 的适配主体 |
| `codex-rs/acp-server/src/lib.rs` | `SessionState` | ACP 会话到内部线程与模式的映射 |
| `codex-rs/acp-server/src/lib.rs` | `AcpSessionMode` | 三模式，同时决定审批与沙箱策略 |
| `codex-rs/acp-server/src/lib.rs` | `AcpSessionMode.sandbox_policy` | 工作区写模式要求 cwd 绝对路径 |
| `codex-rs/acp-server/src/lib.rs` | `CONFIG_ID_MODEL` | `model` 配置项 id 常量 |
| `codex-rs/acp-server/src/lib.rs` | `CONFIG_ID_REASONING_EFFORT` | `reasoning_effort` 配置项 id |
| `codex-rs/acp-server/src/lib.rs` | `CONFIG_ID_HARNESS` | `harness` 配置项 id |
| `codex-rs/acp-server/src/lib.rs` | `AppServerAcpAgent.set_session_config_option` | 两字段改写加一次 fork 三分支 |
| `codex-rs/acp-server/src/lib.rs` | `AppServerAcpAgent.config_options` | 三项配置的当前值与选项列表 |
| `codex-rs/acp-server/src/lib.rs` | `handle_notification` | 白名单通知翻译与回合结束判定 |
| `codex-rs/acp-server/src/lib.rs` | `send_completed_item_update_if_new` | 按 item id 去重的完成项补发 |
| `codex-rs/acp-server/src/lib.rs` | `stop_reason_from_turn` | 四态压成 EndTurn / Cancelled |
| `codex-rs/acp-server/src/lib.rs` | `tool_call_from_item` | 四类内部项到工具调用展示的白名单 |
| `codex-rs/acp-server/src/lib.rs` | `build_prompt_items` | 五类内容块的有损转换 |
| `codex-rs/acp-server/src/lib.rs` | `parse_reasoning_effort` | 六分支，default 与空串归一为 None |
| `codex-rs/acp-server/src/lib.rs` | `run_main` | stdio 服务入口，日志走 stderr |

## 参考

- [open-interpreter 源码：acp-server/src/lib.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/acp-server/src/lib.rs)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)

## 与主核 / AI 的关系

主核里最接近的是「协议适配层」：把外部客户端的标准协议请求翻译成内部执行引擎的调用，再把内部事件转成标准响应。这一页的结构与那个模式同构，连「未识别的请求返回 invalid_params」「未识别的事件静默忽略」这两个方向的不对称都一样——前者要报错，后者不该炸。

与 goose 的 i02（ACP 服务端）对照很有意思：goose 那边是**自己实现 ACP 服务端**（3900 行，含会话类型白名单、meta 校验、工具更新累积），这边是**把已有的 app-server 包一层**（1500 行）。差别就在「谁写协议实现」：goose 直接实现了协议，open-interpreter 复用上游的 app-server 并把 ACP 当成一个薄适配面。所以这边的复杂度集中在**语义映射**（模式→审批+沙箱、配置项→fork、通知→更新），而不是协议细节。

与 g01（会话滚动记录）的联系也值得点：`handle_notification` 翻译的那些项（`CommandExecution`、`FileChange`、`McpToolCall`）正是会被写进 rollout 的条目类型。所以「展示层看到的工具调用」与「持久层记录的条目」来自同一批 `ThreadItem`。

对 AI 工程最实用的一条是「协议语义比内部模型更宽时必须做结构转换」。任何对外暴露「可变配置项」的接口，都要先问一句「这些配置在内部是可变的吗」。若不可变（例如影响提示形状、工具集、模型选择这类在创建时定死的东西），唯一诚实的实现就是「用新配置重建」，然后把新对象的 id 返回给上层。把内层强行改成可变，往往会破坏那些依赖「创建时定死」的不变式。

另一条是「流式事件的完成清单要作为权威 + 去重账本」。流式协议里中间事件可能丢、可能重复、可能乱序；回合结束时的完整列表是最可靠的真相来源。做法就是这里的样子：结束事件到达时遍历完整清单，用一个按 id 的集合跳过已发过的。这样既修复了丢失，也不会重复显示。

最后一条是 `init_tracing` 里那个 `with_writer(std::io::stderr)`。任何用 stdio 作为协议通道的程序，日志都必须显式改道到 stderr——否则一行日志就会污染协议流，导致对端解析失败，而症状是「连接莫名断开」而不是「日志出现在错误的地方」。这个约束必须在初始化时强制，不能靠每个打日志的地方自觉。
