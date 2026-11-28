---
project: open-interpreter
feature: mcp-server
title: MCP 服务端与进行中回合
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/mcp-server/src/codex_tool_runner.rs
    symbols: [create_call_tool_result_with_thread_id, prompt_request]
  - path: codex-rs/mcp-server/src/active_turn_registry.rs
    symbols: [ActiveTurnRegistry, ActiveTurnState, ActiveTurn]
---

# MCP 服务端与进行中回合

## 一句话职责

让 MCP 客户端把「调用一个工具」变成「跑一次完整的 agent 回合」：内部先开线程、提交提示、拿到回合 id，然后一路流式转事件，最后把 `tools/call` 的响应连同线程 id 一起还回去——而「这条回合还在进行中」这件事靠一张双向索引表来维持。

## 机制是什么

`ActiveTurnRegistry` 只有一个字段 `state: Mutex<ActiveTurnState>`，而 `ActiveTurnState` 是**两张表**：

```rust
by_request: HashMap<RequestId, ActiveTurn>,
by_thread_and_turn: HashMap<(ThreadId, String), RequestId>,
```

`ActiveTurn` 两字段：`thread_id: ThreadId`、`turn_id: String`。

所以这是一份**双向索引**：从 MCP 请求 id 能找到「它在哪个线程的哪个回合」，从「线程 + 回合」也能找到对应的请求 id。

模块 doc 把为什么要用同一把锁讲透了：「警告与响应入队回调在持有同一把锁时运行。这使得路由移除与响应入队相对警告入队是原子的，所以警告要么排在其工具响应之前、要么被丢弃」。

## 怎么实现的

**为什么需要双向索引。** MCP 的两类流量方向相反：

- **请求 → 内部**：客户端发 `tools/call`，服务端要用 `RequestId` 找到「这次调用对应的线程与回合」。
- **内部 → 通知**：内部产生的警告只有 `(thread_id, turn_id)`，而发 MCP 通知时必须带上原始的 `request_id` 作为 `relatedRequestId`（这样客户端才知道这条进度属于哪个调用）。

单向表只能服务一个方向。若只存 request → turn，那警告就无法路由；若只存 turn → request，那响应就找不到目标。所以两张表是必需的，而且必须**同步维护**——这就是 `register` / `remove` 里成对操作的原因。

**`register` 处理覆盖的方式。** 实现：

```rust
if let Some(previous_turn) = state.by_request.insert(request_id.clone(), active_turn) {
    state.by_thread_and_turn.remove(&(previous_turn.thread_id, previous_turn.turn_id));
}
state.by_thread_and_turn.insert((thread_id, turn_id), request_id);
```

所以「同一个 request_id 被注册第二次」时，先把旧回合从反查表里删掉，再插新的。这防的是「旧的那条反查记录指向一个已不再注册的请求」——那种残留会让针对旧回合的警告被发到一个已经完成的请求上。

反向的「同一个 (thread, turn) 对应两个请求」不会被处理：`by_thread_and_turn.insert` 会覆盖。这在实际中不会发生（一个回合只由一次 `tools/call` 发起），所以不做额外处理。

**`finish` 与 `unregister` 的差别是「原子性」。** 两个方法：

```rust
fn unregister(&self, request_id: &RequestId) { let mut state = self.lock(); Self::remove(&mut state, request_id); }
fn finish(&self, request_id: &RequestId, finish: impl FnOnce()) { let mut state = self.lock(); Self::remove(&mut state, request_id); finish(); }
```

`finish` 多一个回调，且回调**在锁内**执行。这就是模块 doc 里那句话的实现：回调通常是「发送最终响应」，若它不在锁内，那么「移除路由」与「发送响应」之间就有了一个窗口——期间的警告会看到「路由还在」于是尝试发送，但那是往一个已完成的请求发通知。

放在锁内则两种结果都合法：警告先拿到锁 → 它先发出去（于是警告排在响应之前）；响应先拿到锁 → 路由已被移除，警告随后查不到并静默丢弃。

这是一个「在锁内做 I/O 发送」的例子——通常应避免的坏习惯，但这里它换来了一个明确的顺序保证。doc 的措辞是「要么排队在其工具响应之前，要么被丢弃」，恰好是这两种结果。

**`with_request_id` 用回调而不是返回引用。** 实现：

```rust
let state = self.lock();
let Some(request_id) = state.by_thread_and_turn.get(&(thread_id, turn_id.to_string())) else { return false; };
f(request_id);
true
```

它持锁取引用，把引用交给回调，然后返回布尔。所以调用方（`emit_warning`）能在**住持锁的状态下**发送通知——这与 `finish` 的道理相同，但方向相反：这里的目的是让「查表」与「发送」原子。

`return false` 让调用方知道「没找到活跃回合」，于是它记一条 debug 日志并丢弃这条警告。所以「丢弃」是一个显式的、有日志的分支，而不是静默的无操作。

回调式的 API 是这个设计的唯一可行形态：若返回 `Option<&RequestId>`，引用无法跨锁边界存活（`MutexGuard` 会随函数返回释放）。用回调就把「持锁期间要做的事」表述了出来。

**`lock()` 吞掉中毒。** 实现是 `self.state.lock().unwrap_or_else(PoisonError::into_inner)`。所以任何一个线程 panic 导致锁中毒时，后续访问仍然可用（只是数据可能不一致）。对一个「活跃回合索引」来说，这个方向是合理的：宁可带着可能不一致的索引继续跑，也不要让整个 MCP 服务不可用。

**`prompt_request` 是一个三行函数，但注释很关键。** 它构造 `TurnInputRequest::user_input(vec![UserInput::Text { text: prompt, text_elements: Vec::new() }])`，注释写：「MCP 工具提示是纯文本，没有 UI 元素范围」。

那个 `text_elements: Vec::new()` 在 g04 与 i01 里都出现过——每次都是从「外部来源」构造文本输入。这三处的共同点说明：`text_elements` 是给「编辑器/UI 里选中的富文本片段」用的，任何来自协议或合成的内容都没有它。

**`create_call_tool_result_with_thread_id` 把线程 id 放进两处。** 实现：

```rust
let content = vec![ContentBlock::text(content_text.clone())];
let structured_content = json!({ "threadId": thread_id, "content": content_text });
let mut result = CallToolResult::success(content);
result.is_error = is_error;
result.structured_content = Some(structured_content);
```

所以线程 id 同时出现在**给人看的文本内容**与**给程序读的 structured content**里。等等——细看，`content_text` 才是文本内容，`thread_id` 只在 `structured_content` 里。所以「线程 id 进两处」不成立；实际是「文本进两处、线程 id 只进 structured」。

第二个参数 `is_error: Option<bool>` 而被赋给 `result.is_error` ——所以它映射到 MCP 协议的错误标记，且允许 `None`（即「不标记」）。值得注意的是它仍然 `CallToolResult::success(content)`，再单独置 `is_error`——所以「带 text 内容的失败结果」在协议层是「成功调用但标记为错误」。

那个 `json!` 里 `"threadId": thread_id` 直接用 `ThreadId` 值（靠 `Serialize` 实现），而不是 `.to_string()`。测试里断言了 `json!({ "threadId": thread_id, ... })` 与结果相等——所以序列化形态由 `ThreadId` 的 `Serialize` 决定，不是手工拼的。

为什么要把线程 id 还给客户端？因为 MCP 是**无会话**的：客户端下次想「继续这个会话」必须显式带上线程 id。所以每次 `tools/call` 的响应都把这个 id 报回去，让客户端能保存并在下次复用。

**`run_codex_tool_session` 与 `run_codex_tool_session_reply` 是两条入口但共用主体。** 前者「开一个新线程并跑一次」：`thread_manager.start_thread(...)` → 提交初始提示 → `active_turns.register(id, thread_id, turn_id)` → `run_codex_tool_session_inner`。

后者「在已有线程上跑」：`thread.start_or_steer_turn(prompt_request(prompt))` → 根据返回的 `Started { turn_id }` 或 `Steered { turn_id }` 拿回合 id → 注册 → 共用主体。

`start_or_steer` 的这个返回类型是这一层的一个洞察：新提示可能**启动一个回合**，也可能**并入正在跑的回合**（steering）。两种情况都给出 `turn_id`，所以后续逻辑不必区分。若合并了，那么这条 `tools/call` 的响应要等的是那个（被并入的）回合结束——而不是它自己的。

**错误路径的响应也带线程 id。** 三处失败（`start_thread` 失败、提交提示失败、`NotSubmitted`）都会立刻 `outgoing.send_response(id, create_call_tool_result_with_thread_id(...))` 并返回。

`start_thread` 失败那一处的分支用 `CallToolResult::error(vec![ContentBlock::text(...)])`——注意它用的是 `CallToolResult::error` 而不是 `success` + `is_error`，且**没有**线程 id（因为线程根本没建起来）。这是一个细节上的一致性：没有线程时就不能报线程 id。

所以这一层有两种「错误」表达：有线程时用 `success + is_error: Some(true) + threadId`（客户端能继续用这个线程）；没线程时用 `CallToolResult::error`。前者保留了可续性，后者没有。

**`run_codex_tool_session_inner` 是一个「六个事件类别分开处理」的循环。** 它 `loop { thread.next_event().await; ... }`，对 `EventMsg` 的处理分成几类：

1. **转发事件**：`outgoing.send_event_as_notification(&event, Some(OutgoingNotificationMeta { ... }))` —— 一般在循环顶部对多数事件做。
2. **静默继续**：`Warning | AuthRecoveryStarted | AuthRecoveryCompleted | GuardianWarning | ModelVerification | SafetyBuffering | TurnModerationMetadata` → `continue`。这些都是「用户不需要在 MCP 里看到」的元信息。
3. **显式忽略带注释**：`ElicitationRequest` → `continue`（`// TODO: forward elicitation requests to the client?`）；`ThreadGoalUpdated | ThreadQueueChanged` → 「在 MCP 工具运行器里忽略线程级元数据更新」；`McpStartupUpdate | McpStartupComplete` → 「在 MCP 工具运行器里忽略」；`AgentMessage` → 「TODO: 想想我们想怎么在 MCP 里支持这个」。
4. **致命事件并结束**：`Error` → 构造带文本的失败结果、`active_turns.finish(...)`、`break`。
5. **成功结束**：某个终止事件里的 `text` → `create_call_tool_result_with_thread_id(thread_id, text, None)`、`finish`、`break`。
6. **大量「只是记录不处理」的事件**：`AgentReasoningRawContent | TurnStarted | ThreadSettingsApplied | EnvironmentConnected | EnvironmentDisconnected | TokenCount | AgentReasoning | AgentReasoningSectionBreak | McpToolCallBegin | McpToolCallEnd | RealtimeConversationListVoicesResponse` 等。

第 6 类的存在说明了一件重要的事：**`next_event` 给出的事件流比 MCP 客户端需要看到的多得多**。而处理方式是「穷举列出」，不是 `_ => {}` —— 因为 `EventMsg` 是一个大枚举，用一个通配符会让「新增事件类型」无声地落进「忽略」桶。穷举强制每次枚举扩展都来读一遍这个 match。

第 3 类里那几条 TODO 是这一层最诚实的部分：`ElicitationRequest` 需要用户交互，但 MCP 工具运行器没有对应的交互通道，于是只能忽略并留待办。这解释了为什么 `send_event_as_notification` 之后原样继续——MCP 客户端会看到一条它无法响应的事件。

**循环结尾处的错误处理是统一的。** `Err(e)` 分支：

```rust
let result = create_call_tool_result_with_thread_id(thread_id, format!("Codex runtime error: {e}"), Some(true));
active_turns.finish(&request_id, || { outgoing.send_response(request_id.clone(), result); });
break;
```

以及前面 `EventMsg::Error` 的同类处理。两处都用 `finish`（锁内发响应）而不是 `unregister` 后再发——保证了「移除路由」与「响应」的原子性。

## 关键取舍

**源码事实：** `ActiveTurnState` 是 request → turn 与 turn → request 的双向索引，两张表在同一把锁下同步维护。

**源码事实：** `register` 覆盖同一 request_id 时先清掉旧的反映射条目，避免残留指向已不注册的请求。

**源码事实：** `finish` 在锁内执行发送回调，`with_request_id` 在持锁时执行回调——两个方向都用「锁内做发送」换来明确的顺序保证。

**源码事实：** `lock()` 用 `PoisonError::into_inner`，锁中毒不阻断服务。

**源码事实：** `create_call_tool_result_with_thread_id` 总是 `CallToolResult::success(text)` 再单独设 `is_error`，线程 id 只进 `structured_content`。

**源码事实：** 「开线程失败」用 `CallToolResult::error`（无线程 id），「有线程但失败」用 `success + is_error: Some(true)`（带线程 id）。

**源码事实：** `start_or_steer_turn` 的 `Started` 与 `Steered` 都给出 `turn_id`，后续不区分。

**源码事实：** 事件处理是对 `EventMsg` 的穷举 match，大量事件被显式列进「忽略」类，没有通配符。

**架构推论：** 「双向索引 + 原子发送」这个组合，本质是给「异步流 + 请求响应」两套语义搭桥。MCP 是请求/响应式的（一次调用一个响应），而内部是事件流式的（一个回合产生一串事件）。警告这类「属于某个请求的侧信道消息」必须在请求还活着的时候发出去，因此需要「查得到 + 发得原子」。

**架构推论：** 「Steered」这个情形揭示了 MCP 与内部模型的一个语义错位：MCP 客户端以为自己在发起一次新调用，而内部可能把它并入了正在跑的回合。这个错位不会报错，而是表现为「这条调用的响应要等更久」——所以实现选择「不区分」，让两者都走同一个等待路径。

**架构推论：** 用穷举 match 而不是通配符，是在「枚举会增长」的前提下唯一能防止静默漏处理的写法。代价是每次新增事件类型都要回来改这个 match（编译器会提示），收益是没人能偷偷把新事件塞进「忽略」而不被看见。这与 g04 里「压缩摘要伪装成 user 消息」带来的一连串复杂度是同一类问题——结构决定的成本，无法靠纪律绕开。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/mcp-server/src/codex_tool_runner.rs` | `create_call_tool_result_with_thread_id` | 工具结果把线程 id 放进 structured content |
| `codex-rs/mcp-server/src/codex_tool_runner.rs` | `prompt_request` | 纯文本提示输入，无富文本范围 |
| `codex-rs/mcp-server/src/codex_tool_runner.rs` | `run_codex_tool_session` | 开新线程并跑一次调用 |
| `codex-rs/mcp-server/src/codex_tool_runner.rs` | `run_codex_tool_session_reply` | 在已有线程上跑，含 steer 合流 |
| `codex-rs/mcp-server/src/codex_tool_runner.rs` | `run_codex_tool_session_inner` | 事件循环与六类处理，穷举无通配 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurnRegistry` | 活跃回合的唯一入口，单锁 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurnState` | request 与 thread+turn 的双向索引 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurn` | 线程与回合的配对 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurnRegistry.register` | 覆盖时清掉旧的反映射 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurnRegistry.finish` | 锁内移除并发送，保证顺序 |
| `codex-rs/mcp-server/src/active_turn_registry.rs` | `ActiveTurnRegistry.with_request_id` | 持锁回调，未命中返回 false |
| `codex-rs/mcp-server/src/extension_event_sink.rs` | `emit_warning` 的查表 | 用 thread+turn 找 request 再发通知 |

## 参考

- [open-interpreter 源码：mcp-server/src/codex_tool_runner.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/mcp-server/src/codex_tool_runner.rs)
- [open-interpreter 源码：mcp-server/src/active_turn_registry.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/mcp-server/src/active_turn_registry.rs)
- [Model Context Protocol 规范](https://modelcontextprotocol.io/specification/2025-06-18)

## 与主核 / AI 的关系

主核里最接近的是「请求注册表」：一个长任务被受理后，系统要维护「这个客户端句柄还在等哪个任务」的映射，以便任务进展或失败时能推给正确的等待者。这一页的 `ActiveTurnRegistry` 与那个结构同构，连「以请求 id 为键、以任务为值、双向反查」这一套都一样。

与 goose 的 i01（ACP 提供方）对照很有意思：goose 那边用 `HandoffContextClaimGuard` 保证「一次性声明可回滚」，用 `AcpEffortState` 广播能力变化；这边用一张双向索引保证「警告要么在响应前、要么被丢」。两者都在解同一个问题——**异步事件流与请求响应之间要有精确的时序保证**——但手法不同：goose 用声明 + 回滚，这里用锁内发送。

与 g01（会话滚动记录）的关系：这一层的每次 `tools/call` 最终都会产生一批 `RolloutItem` 被写入会话文件。所以「MCP 的一次调用」在内部是「一个回合 + 若干项 + 一个线程 id」——线程 id 既是 MCP 响应的返回字段，也是会话文件的标识。

对 AI 工程最实用的一条是「双向索引必须单锁同步维护」。这类索引最典型的 bug 是「两张表在两条路径上维护，某条路径漏了一个」，症状是「警告偶尔发到错误的请求上」或「偶尔查不到活跃回合」。把两张表包进一个 `Mutex<State>`，并让所有修改都走成对的 `register`/`remove`，就把「遗漏」压缩成「只有两个入口需要检查」。

另一条是「用锁内发送换顺序保证」。这违反「不要持锁做 I/O」的通则，但换来的是一个可陈述的保证：警告要么在响应之前送达，要么被丢弃——不会出现「响应已发完但警告还在路上」。当一个顺序保证比一把锁的争用更重要时（这里每次只影响一个请求），这个取舍是值得的，而关键在于把理由写进类型/模块的 doc 里，否则后来者会以「优化」为名把它挪出锁外。

最后一条是「无会话协议里的标识回传」。MCP 没有连接级会话，所以每次响应都必须携带「下次继续用哪个会话」的标识。`threadId` 放进 `structured_content` 而不是文本内容，是为了让客户端能程序化读取，同时不污染给模型看的文本。任何在无状态协议上做有状态服务的实现，都需要这样一条「把状态标识放在机器可读字段」的规则。
