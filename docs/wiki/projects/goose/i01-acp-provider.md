---
project: goose
feature: acp-provider
title: ACP 提供方与一次性上下文声明
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/acp/provider.rs
    symbols: [AcpProviderConfig, AcpSession, AccumulatedToolCall]
  - path: crates/goose/src/acp/provider.rs
    symbols: [provider_error_from_acp, AcpEffortState, HandoffContextClaim]
---

# ACP 提供方与一次性上下文声明

## 一句话职责

把一个外部 ACP agent 子进程包装成 goose 的普通 provider：用独立 OS 线程承载它的运行时，用请求枚举串行化所有会话操作，并把「这次是不是第一次发提示」做成一个原子声明——只在那一次附带历史备忘录。

## 机制是什么

`AcpProviderConfig` 描述怎么起这个子进程：`command`、`args`、`env`、`env_remove`、`work_dir`、`mcp_servers`、`session_mode_id`、`session_config_options`、`model_config_option_id`、`mode_mapping`、以及一个可选的 `notification_callback`。

`AcpProvider` 本身持有：`name`、goose 侧模式（`Arc<Mutex<GooseMode>>`）、模式映射、当前会话（`Mutex<AcpSession>`）、待确认权限、以及那个关键的 `handoff_context_sent: Arc<AtomicBool>`。

通信不走普通函数调用，而走一个 `ClientRequest` 枚举：

- `NewSession` / `LoadSession` / `CloseSession`
- `SetMode` / `SetConfigOption`
- `Prompt`

每个变体都带一个 `oneshot::Sender` 或 `mpsc::Sender` 作为回程通道。所以「所有 ACP 操作」都被规约成「发一条命令给那个线程，等它回结果」。`AcpUpdate` 则是 prompt 期间从子进程流上来的事件：文本、思考、工具调用开始/完成、权限请求、结束（带停止原因与用量）、错误。

## 怎么实现的

**子进程必须在自己的线程里起。** `ClientLoopFn` 的类型别名上有一句关键注释：「tokio I/O 句柄无法在运行时之间移动，所以子进程必须在 OS 线程里 spawn。这个闭包让 `start()` 能共享其余逻辑」。

于是架构是：一个专用 OS 线程跑自己的一套 tokio 运行时，里面 `spawn_acp_process` 起子进程并跑客户端循环；主运行时通过 channel 与它对话。

这不是「为了性能」，而是「为了正确性」——子进程的 stdio 句柄绑在创建它的运行时上。若在运行时 A 创建、在运行时 B 使用，会直接失败。

**`ClientLoopGuard` 的析构顺序。** 它持三样：`tx`（命令发送端）、`cancel_tx`（取消信号）、`thread`（线程句柄）。`Drop` 里按序做三件事：发取消信号、丢掉 tx、**再起一个新线程来 join 旧线程**。

第三步是这一页最不寻常的地方：`std::thread::spawn(move || { let _ = thread.join(); })`。为什么要另起线程 join？

因为 Drop 可能在异步上下文里执行，而 `join` 是阻塞调用。若直接在 Drop 里 join，就会阻塞当前异步任务所在的执行器线程。把 join 挪到一次性线程里，让 Drop 立即返回，代价是线程资源晚一点回收。

`AcpProvider::drop` 则**没有**这样做——它直接 `h.join()` 并记 debug 日志。所以两处析构策略不一致：guard 走「非阻塞委托」，provider 走「原地阻塞」。

**原子声明只有一次。** `claim_handoff_context` 是这个 provider 的语义核心：

```rust
let first_prompt = !self.handoff_context_sent.swap(true, Ordering::AcqRel);
```

`swap(true)` 返回旧值：旧值为 `false` 时取反得 `true`——这是第一次；旧值为 `true` 时取反得 `false`——之后都是。所以 `first_prompt` 精确地表示「我是不是第一个」。

然后 `include_context = first_prompt && has_handoff_context(messages)`——两个条件都要。所以「第一次发提示但历史为空」时不带备忘录（没什么可带的）。

`AcqRel` 是必要的：多个并发 prompt 可能同时到达，只有 `swap` 能保证恰好一个看到「第一次」。

**失败必须能回滚。** `HandoffContextClaimGuard` 就是为此存在：它持 `handoff_context_sent` 与一个 `pending` 标志。`commit()` 把 `pending` 置假（表示「这次真的用上了」）；`rollback()` 与 `Drop` 都会在 `pending` 为真时把原子标志写回 `false`。

所以「声明 → 尝试 → 成功则 commit、失败则 rollback」是一个显式的事务语义。为什么需要？注释在 `retry_without_memo_could_help` 里给了完整理由：若 agent 拒绝了**第一份**更新，我们能知道的信息只是「它不喜欢这个提示」，而备忘录是我们唯一额外加的东西——所以值得放掉备忘录重试一次。但「账户没钱」或「凭据缺失」与提示内容无关，重试只会浪费掉这次唯一的回落机会，还消耗掉一个 agent 根本没拒绝过的备忘录。

因此 `retry_without_memo_could_help` 的判定是：`AuthRequired` 一律不放；错误 `data.reason` 等于 `CREDITS_EXHAUSTED_REASON` 也不放；其余才认为「去掉备忘录可能有用」。而因为要重试，第一次那声 `swap(true)` 必须能被撤销——这就是 guard 的存在理由。

**`AccumulatedToolCall` 是跨非终态更新累加。** ACP 的工具调用会分多条更新推送：先报开始（带 name、kind、raw_input），中间推 `content` 与 `raw_output` 片段，最后一条终态更新才宣告完成。

所以需要一个 per-id 缓冲：`HashMap<String, AccumulatedToolCall>`，而结构里只放两样——`raw_output` 与 `content` 累积。终态更新到达时把缓冲区 drain 掉，组合成完整的 `ToolCallComplete`。

注意结构里**没有** `name` 与 `kind`：那两样在开始时就有，直接进 `AcpUpdate::ToolCallStart`，不需要跨更新累积。所以缓冲只装「可能分片到达」的字段。

**错误映射只有两档。** `provider_error_from_acp` 极短：`AuthRequired` 映射成 `ProviderError::Authentication`，其余一律 `RequestFailed`。

这与 g05 的三层重试判定直接衔接：`Authentication` 不重试（`should_retry` 的兜底分支返回 false），而 `RequestFailed` 默认重试（除非 `transient_only`）。所以「ACP 把什么都报成 RequestFailed」意味着**大部分 ACP 错误都会被重试 3 次**。

**努力度（effort）的双通道状态。** `AcpEffortState` 有两个字段：

- `capability: Arc<Mutex<Option<ThinkingEffortCapability>>>`——本会话是否支持努力度、支持哪些档。
- `updates: watch::Sender<ThinkingEffortSupport>`——一个 watch channel，让上层可以 `subscribe_thinking_effort_support()` 被动获知变化。

初始值是 `Unsupported`。所以「还不知道支不支持」时对外声明的是「不支持」——保守方向。等首次查询到会话能力后再更新，watch 会把新值推给所有订阅者。

这个双通道设计对应 `Provider` trait 的两个方法：`thinking_effort_support()`（立即问一次）与 `subscribe_thinking_effort_support()`（订阅变化）。前者适合「现在就要决定」，后者适合「UI 要实时反映」。

**会话状态的包装。** `AcpSessionState` 持 `active_id: Arc<Mutex<Option<SessionId>>>` 与 `effort: AcpEffortState`。所以「哪个会话是活动的」是一个可在多处共享读写的选项，而不是 provider 的普通字段——因为客户端循环线程也要读它。

**`AcpSession` 带整份 `NewSessionResponse`。** 它不只有 `id`，还有完整的响应对象。因为 ACP 的 `newSession` 响应里带模型清单、模式状态、配置选项——这些东西后续都要用（例如 `fetch_supported_models` 就是从 `session.response` 里解析）。存整份响应省去了「再发一次查询」。

**`fresh_text_run` 造的是「运行标识」。** 它返回 `(uuid, 时间戳)` 一对——一个随机的运行 id 与它的起始时间。这类双字段在 trace 与日志里很常见：uuid 保证唯一，时间戳给人类看顺序。

## 关键取舍

**源码事实：** 子进程只能在专用 OS 线程上起，因为 tokio I/O 句柄不能跨运行时移动。注释把它写成「所以子进程必须在 OS 线程里 spawn」。

**源码事实：** `ClientLoopGuard::drop` 另起线程 join 旧线程，而 `AcpProvider::drop` 原地 join。两处策略不一致。

**源码事实：** `claim_handoff_context` 用 `swap(true, AcqRel)`，所以并发下恰好一个调用者得到 `first_prompt = true`。

**源码事实：** 备忘录的「声明」是可回滚的：`HandoffContextClaimGuard` 在 `pending` 时会被 `Drop` 写回 `false`。所以「第一次尝试失败」之后，真正的第一次带备忘录的成功尝试仍然可能发生。

**源码事实：** `provider_error_from_acp` 只区分 `AuthRequired` 与「其他」，其余全部归为 `RequestFailed`。

**源码事实：** `AccumulatedToolCall` 只累加 `raw_output` 与 `content`，不累加 `name`/`kind`（那两个在开始时已有）。

**架构推论：** 那个「原子声明 + 可回滚 guard」的组合解决的是一类特殊问题：**恰好一次**的资源使用，且这次使用可能失败需要重试。普通的「只做一次」用 `Once` 就够了，但这里要求「失败可以撤销这次使用」，所以必须是可回滚的声明。这个模式在任何「一次性的昂贵准备工作」（拼接上下文、预计算、建立隧道）上都有价值。

**架构推论：** 把整个 ACP 客户端循环隔离在一个 OS 线程里，代价是所有操作都要经过 channel 序列化（多一次往返、不能直接借用），收益是「运行时边界清晰」。这同时也是这个 provider 为什么声明 `SESSION_BOUND = true`（见 g01）的根源——实例里钉着一个线程与一个子进程，天然不能跨会话共享。

**架构推论：** 工具调用的「跨更新累积」是流式协议的通用形态：开始事件带元信息、中间事件带碎片、终态事件宣告完成。为此必须维护一个以 id 为键的缓冲，并且只在终态 drain。若不做累积而在每条更新上单独产出事件，上层拿到的会是「半个工具调用」十几次。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/acp/provider.rs` | `AcpProviderConfig` | 子进程启动参数与模式映射 |
| `crates/goose/src/acp/provider.rs` | `AcpSession` | 会话 id 与整份 `NewSessionResponse` |
| `crates/goose/src/acp/provider.rs` | `AccumulatedToolCall` | 工具调用跨更新的输出与内容缓冲 |
| `crates/goose/src/acp/provider.rs` | `provider_error_from_acp` | 只区分认证与其余两档的错误映射 |
| `crates/goose/src/acp/provider.rs` | `AcpEffortState` | 努力度能力缓存加 watch 广播 |
| `crates/goose/src/acp/provider.rs` | `HandoffContextClaim` | 「是否第一次发提示且需带上下文」的声明结果 |
| `crates/goose/src/acp/provider.rs` | `HandoffContextClaimGuard` | 声明的提交与回滚，Drop 时撤销 |
| `crates/goose/src/acp/provider.rs` | `claim_handoff_context` | 原子 swap 保证恰好一次 |
| `crates/goose/src/acp/provider.rs` | `retry_without_memo_could_help` | 判断「去掉备忘录重试」是否值得 |
| `crates/goose/src/acp/provider.rs` | `bounded_handoff_memo` | 装配 memo 预算并截断到最后一个用户消息之前 |
| `crates/goose/src/acp/provider.rs` | `ClientRequest` | 所有会话操作的请求枚举 |
| `crates/goose/src/acp/provider.rs` | `ClientLoopGuard` | 取消与「另起线程 join」的非阻塞析构 |

## 参考

- [goose 源码：acp/provider.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/acp/provider.rs)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [goose 文档：ACP 提供方](https://block.github.io/goose/docs/guides/acp-providers/)

## 与主核 / AI 的关系

主核里最接近的是「外部进程式扩展」：某些功能以独立进程运行，主进程通过管道或 socket 与它通信，且必须由主进程保证子进程的启动与回收。`AcpProvider` 与这个模式同构，连「句柄绑在创建它的运行时上」这类约束都类似——主核里对应的是「文件描述符不能跨事件循环使用」。

与 smolagents 的 MCP 客户端相比，两者都把外部能力包装成本地抽象，但包装的深度不同。smolagents 的 `MCPClient` 只有 170 行，因为它把协议实现推给了 `mcpadapt`；goose 这个 provider 有 4600 多行，因为它**自己实现 ACP 客户端**：起子进程、跑客户端循环、处理权限请求、累积工具更新、管理 effort 能力、还要做一次性上下文声明。

对 AI 工程最实用的一条是那个「可回滚的一次性声明」。任何「只该发生一次」的准备工作都会遇到同一个问题：如果第一次尝试失败并被重试，那这次准备有没有被消费掉？如果被消费掉了，重试就会缺少它；如果没被消费，重试就会重复它。`HandoffContextClaimGuard` 给出了明确答案：声明可撤销，且撤销绑在 `Drop` 上——只要没 `commit`，无论正常返回还是 panic 展开，撤销都会发生。这比手工管理布尔位可靠得多。

另一条是「去掉某个附加物再重试」这个降级策略的判据。`retry_without_memo_could_help` 的判定逻辑很有参考价值：它不看错误类型笼统判断，而是问「这个失败与我要去掉的东西有关系吗」。鉴权失败、余额耗尽与提示内容无关，所以去掉备忘录重试无意义且浪费机会。反过来，若错误是「提示被拒」，那唯一可疑的附加物就是备忘录，值得一试。这个「归因」思路比「一刀切重试」精确得多。
