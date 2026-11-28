---
project: open-interpreter
feature: compaction
title: 上下文压缩
page_type: feature
schema_version: 1
refs:
  - path: codex-rs/core/src/compact.rs
    symbols: [COMPACT_USER_MESSAGE_MAX_TOKENS, CompactedHistoryMetadata, InitialContextInjection]
  - path: codex-rs/core/src/compact.rs
    symbols: [CompactionAnalyticsAttempt, compaction_status_from_result, CompactedUserMessage]
---

# 上下文压缩

## 一句话职责

把一长串历史换成「一份模型写的摘要 + 若干条用户原话」，并在替换时精确决定**初始上下文该插在哪个位置**——因为摘要被编码成一条 user 消息，位置放错就会让模型把摘要当成最新提问。

## 机制是什么

压缩的产物不是「摘要」一项，而是一份**新的历史**：`build_compacted_history(initial_context, user_messages, summary_text)` 拼出三段——注入的初始上下文、被保留的用户消息、最后一条压缩摘要。

`InitialContextInjection` 把这个决定做成显式枚举，两个变体：

- `DoNotInject`——「pre-turn / manual 压缩变体用它：它们把历史换成摘要并清掉 `reference_context_item`，于是下一次常规回合会在压缩之后完整重注初始上下文」。
- `BeforeLastUserMessage { world_state, step_context }`——「**回合中**压缩必须用它，因为模型被训练成在回合中压缩之后看到压缩摘要是历史的最后一项；所以我们把初始上下文注入到替换历史里、正好在最后一条真用户消息之上」。

所以「要不要在这一步注入初始上下文」不是一个实现细节，而是一个**取决于压缩发生在哪一刻**的语义决定。枚举上那段 doc comment 是这一页最重要的文字。

`CompactedHistoryMetadata` 是新检查点的元信息：`message`（摘要全文）、`window_number` 与 `window_ids`（自动压缩窗口的编号与 id 集）、`compaction_response_id`、`compaction_model_hash`、`reviewer_compaction_hash`。doc comment 说明它与「替换历史」刻意分开：「`Session::replace_compacted_history` 在构造持久化 `CompactedItem` 之前会补上缺失的 item id，从而保证活跃历史与持久化历史完全一致」。

`COMPACT_USER_MESSAGE_MAX_TOKENS = 20_000`——被保留的用户原话总量上限。

## 怎么实现的

**压缩是一次真正的模型请求。** `run_compact_task_inner_impl` 的做法是：把「压缩提示」当作一条 user 输入，加上当前历史，作为一个完整的 prompt 发给模型，等它回一段摘要。

所以它复用了完整的一次推理路径：`sess.clone_history()` → 记录压缩提示 → `Prompt { input, base_instructions, .. }` → `drain_to_completed`。注释说明这里刻意**复用同一个 client session**：「让回合作用域内的状态（粘性路由、websocket 增量请求跟踪）在这次压缩回合的重试之间存活」。

这一点很实际：如果每次重试都新建 session，粘性路由会重新选一遍后端，前一次请求的增量缓存也就废了。

**压缩自身的重试有三类，处理方式完全不同。** `loop` 里的 `match attempt_result`：

1. `Interrupted` / `TurnAborted` → 立即返回错误，不重试。用户中断了，重试没有意义。
2. `SessionBudgetExceeded` → 立即返回，不重试。预算超了，重试只会更超。
3. `ContextWindowExceeded` → 若 `turn_input_len > 1`，`history.remove_first_item()` 并 `retries = 0; continue`。注释：「从开头裁掉以保留缓存（前缀式）且保持最近消息完整」。
4. 其余错误 → 若 `retries < max_retries`，`retries += 1`、`backoff(retries)`、通知流错误、sleep、continue；否则返回。

第 3 分支是这里最巧妙的一处：压缩本身也可能因为输入太长而超窗。此时唯一可行的动作是「丢掉最老的一条历史再试」——而且必须从**开头**丢，因为 prompt 缓存是前缀匹配的，从中间或尾部改动会让整个缓存失效。`retries = 0` 重置是因为这是「换了一个输入」，不是「同一个请求的又一次失败」，把它算进重试配额会过早耗尽。

**历史的最后形态。** 拿到摘要后，最后几件装配动作按顺序是：

1. `history_snapshot = sess.clone_history()`、`history_items = annotated_items()`——取压缩前的历史（**不是**压缩请求里那份裁剪过的）。
2. `summary_text = format!("{SUMMARY_PREFIX}\n{summary_suffix}")`——前缀是必需的，因为 `is_summary_message` 就是靠 `starts_with(SUMMARY_PREFIX + "\n")` 判定的。
3. `collect_annotated_user_messages(history_items, identity)` 收集所有真用户消息（排除摘要，见下）。
4. `build_compacted_history(Vec::new(), &user_messages, &summary_text)`——注意第一个参数是**空**的初始上下文，因为初始上下文稍后才按位置注入。
5. `summary_item.set_turn_id_if_missing(&turn_context.sub_id)`——注释解释：「这份替换历史跳过了 `record_conversation_items`；只有被追加的摘要属于这个压缩回合」。所以只有摘要需要打上本回合 id。
6. `advance_auto_compact_window()` 得到新的窗口号与窗口 id 集。
7. `build_compaction_initial_context` 造初始上下文；非空时 `insert_initial_context_before_last_real_user_or_summary` 插入。
8. `replace_compacted_history(new_history, reference_context_item, world_state_baseline, metadata)`。
9. `recompute_token_usage`——因为历史变了，token 账必须重算。

第 4 与第 7 步的分离值得注意：不在 `build_compacted_history` 里就把初始上下文放进去，是因为**插入位置取决于历史已经拼好之后的内容**（最后一条真用户消息在哪）。所以顺序必须是「先拼出不带初始上下文的历史，再按位置 splice 进去」。

**`collect_annotated_user_messages` 的三件事。** 它逐项过滤：

- 只保留能解析成 `TurnItem::UserMessage` 的项。
- **丢弃摘要消息**：`if is_summary_message(&user.message()) { return None }`。所以多次压缩之后，旧的摘要不会被当成用户原话保留——它已经被新的摘要取代。
- 按 `identity` 决定是否清掉 `id`：`CompactedMessageIdentity::Regenerate` 时 `message.id = None`；`Preserve` 时保留。

`identity` 的来源是一行判断：`if sess.guardian_context_mode == GuardianContextMode::ThreadOwned { Preserve } else { Regenerate }`。`CompactedUserMessage` 的字段注释解释了为什么要保留 id：「即使压缩缩短了文本，也保留来源身份，这样回滚能把重建的消息与线程自有的保留证据对上」。

所以 id 的存在是为了**回滚时能重新关联**——若每次压缩都重新生成 id，回滚就无法确认「这条压缩后的消息原本对应哪条原始消息」。

**`insert_initial_context_before_last_real_user_or_summary` 的四层位置规则。** doc comment 列得很清楚：

1. 优先插在**最后一条真用户或 agent 消息**之前。
2. 若没有真用户消息，插在摘要之前（让摘要保持在最后）。
3. 若连用户消息都没有，插在最后的压缩项之前（「远端压缩可能只返回压缩项」）。
4. 若什么都没有，直接追加。

实现上它从后往前遍历，同时记录两个下标：`last_real_user_index`（真用户，首选）与 `last_user_or_summary_index`（用户消息样目，兜底）。摘要被编码成 user 消息，所以两者在「只看类型」时无法区分——`is_summary_message` 才是分辨手段。注释明确：「压缩摘要是以 user 消息的形式编码的，所以两者都要跟踪」。

还有一个特殊处理：`ResponseItem::AgentMessage` 里若 `content.first()` 是 `InputText` 且以 `"Message Type: FINAL_ANSWER\n"` 开头，就被当作真用户消息的等价物。所以「最终答案」这类 agent 消息也算一个合法的插入边界。

最后一个表达式是三层 `or`：`last_real_user_index.or(last_user_or_summary_index).or(last_compaction_index)`。若三者皆 `None` 则 `extend`。这个 fallback 链正是那四条规则的实现。

**用户消息的保留是「从最新往回装到预算」。** `build_compacted_history_with_limit` 的核心循环：

```rust
for message in user_messages.iter().rev() {
    if remaining == 0 { break; }
    let tokens = approx_token_count(&message.message);
    if tokens <= remaining { push; remaining -= tokens; }
    else { push(truncate_text(...)); break; }
}
selected_messages.reverse();
```

所以是「从最新往旧装，装不下就截断当前这条并停止，最后翻转成时间正序」。截断仍保留 `id` 与 metadata——继续满足回滚关联的需求。

`max_tokens > 0` 的守卫让测试可以传 0 造出「没有任何用户消息」的极端历史。

**summary 为空时的兜底。** `if summary_text.is_empty() { "(no summary available)" }`——一个空字符串会被拼成 `SUMMARY_PREFIX + "\n" + ""`，那仍然能被 `is_summary_message` 识别，但内容为空。填一个可读占位让模型不至于看到一句莫明其妙的话。

**`CompactionAnalyticsAttempt` 是「配对采样」。** 它的形状很值得学：`begin` 时记下 `active_context_tokens_before`（压缩前 token 用量）、`started_at`（unix 秒）、`start_instant`（用于测时长）；`track` 时读 `active_context_tokens_after`，然后发一个事件。

`track` 接收 `details: CompactionAnalyticsDetails`，五个字段全是 `Option`：`active_context_tokens_before`、`retained_image_count`、`compaction_summary_tokens`、`cached_input_tokens`、`cache_write_input_tokens`。

`active_context_tokens_before` 在 details 里也有一个同名字段——`track` 里写 `details.active_context_tokens_before.unwrap_or(self.active_context_tokens_before)`。所以「details 能给更精确的值（例如压缩请求实际发送前那一刻），否则用 begin 时的粗值」。这是一个「后补精度」的设计。

`track` 是**消费 self** 的（`pub(crate) async fn track(self, ...)`），所以一个 attempt 只能上报一次。这比「可多次上报」更安全——指标重复上报会让统计失真，而类型系统直接禁掉了。

`CompactionStrategy::Memento` 是硬编码的单一策略值。它暗示了未来的扩展位：这里将来会有多策略（例如本地压缩 vs 远端压缩），而现在只有一种。

**状态映射是一个纯函数。** `compaction_status_from_result` 把 `CodexResult<T>` 映射成三态：`Ok` → `Completed`；错误且是 `Interrupted` / `TurnAborted` → `Interrupted`；其余错误 → `Failed`。

所以「用户中断」不算失败。这个区分对指标很关键：中断率与失败率是完全不同的信号，混在一起会让「压缩可靠性」无法评估。

**hook 的介入点有两个，且都会改状态。** `run_pre_compact_hooks` 在真正干活前跑，返回 `Stopped` 就以 `Interrupted` 状态上报并返回 `TurnAborted`。`run_post_compact_hooks` 在成功之后跑，返回 `Stopped` 也以当前状态上报并返回 `TurnAborted`。

注意后者的顺序：它用 `status`（此时必是 `Completed`，因为进入这个分支的前提是 `result.is_ok()`）上报，然后才返回错误。所以「压缩成功了但后置 hook 决定中止」这个状态被记成 `Completed`——因为压缩本身确实完成了。

最后那段错误上报有两条排除：`phase` 是 `PostTurn` 时不报（后置回合的错误不该再弹一次）；错误是 `Interrupted` / `TurnAborted` 时不报（用户自己中断的，不该弹错误）。且 `PreTurn` 阶段也不发事件，注释说「前置失败在保留来输入提示之后才报告」——即在真正开始回合之前就失败，不该当成回合内错误弹出来。

**一句警示被无条件发出。** 无论压缩多少次、成功与否，末尾都会发一条 `WarningEvent`：「长线程与多次压缩会让模型准确度下降。尽量开新线程保持小而聚焦」。

这条提示放在压缩路径里或许看起来是产品话术，但它标注了一个真实的机制事实：每压缩一次，历史就多了一层「摘要的摘要」，信息损失是累积的。把这句话硬编码在压缩成功之后（而不是等用户抱怨），是唯一能确保它出现在恰当时刻的位置。

## 关键取舍

**源码事实：** `InitialContextInjection` 的两个变体对应两种压缩时机；`DoNotInject` 用于 pre-turn 与 manual，`BeforeLastUserMessage` 用于回合中压缩。

**源码事实：** 摘要用 `SUMMARY_PREFIX` 前缀编码成 user 消息，`is_summary_message` 靠前缀判定。

**源码事实：** 用户消息按「从最新往旧」装填，超出预算时截断当前这条并停止。总量上限 20k token。

**源码事实：** 压缩自身遇到 `ContextWindowExceeded` 时从**开头**删历史条目，并把 `retries` 归零。

**源码事实：** `Interrupted` / `TurnAborted` 被映射成 `Interrupted` 状态，与 `Failed` 分开统计。

**源码事实：** `id` 在 `GuardianContextMode::ThreadOwned` 时保留、否则重新生成。

**源码事实：** `CompactionAnalyticsAttempt::track` 消费 `self`，所以一次尝试只能上报一次。

**架构推论：** 「摘要编码成 user 消息」这个选择带来了一整串连锁设计——必须有前缀来区分、必须有 `is_summary_message` 过滤、必须有四层插入位置规则、必须有「上次的摘要不要被当成用户原话保留」。这一切的根源是「模型的训练方式假定压缩摘要是一条 user 消息」。这提醒一件事：**当一个数据形态被训练方式绑定时，它带来的复杂度会扩散到所有相关逻辑**。

**架构推论：** 四层插入位置规则是「兼容多种历史形态」的产物。正常路径下历史里必然有真用户消息；但远端压缩可能只返回压缩项（第 3 层），而某些路径可能连压缩项都没有（第 4 层）。每一层都是一个真实存在的输入形态，而不是防御性冗余。

**架构推论：** 「压缩时超窗就从开头删」这个策略与 prompt 缓存的前缀特性直接相关。若从中间或尾部删，缓存命中率会掉到接近零，压缩请求本身反而更贵。选择「从开头删」是唯一保住前缀缓存的选项——代价是丢掉最老的上下文，而那正是压缩想要丢掉的东西。这是一个方向上的巧合，但正是它让这个策略可行。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `codex-rs/core/src/compact.rs` | `COMPACT_USER_MESSAGE_MAX_TOKENS` | 保留用户原话的 20k token 上限 |
| `codex-rs/core/src/compact.rs` | `CompactedHistoryMetadata` | 新检查点的元信息，与替换历史分开 |
| `codex-rs/core/src/compact.rs` | `InitialContextInjection` | 是否注入初始上下文的时机枚举 |
| `codex-rs/core/src/compact.rs` | `CompactionAnalyticsAttempt` | begin/track 配对的采样对象，track 消费 self |
| `codex-rs/core/src/compact.rs` | `compaction_status_from_result` | 三态映射，中断不算失败 |
| `codex-rs/core/src/compact.rs` | `CompactedUserMessage` | 保留原话及其来源 id 与元数据 |
| `codex-rs/core/src/compact.rs` | `CompactedMessageIdentity` | 保留或重生成 id 的策略 |
| `codex-rs/core/src/compact.rs` | `run_compact_task_inner_impl` | 压缩请求、重试分类、历史替换全流程 |
| `codex-rs/core/src/compact.rs` | `build_compacted_history` | 拼出「原话 + 摘要」的新历史 |
| `codex-rs/core/src/compact.rs` | `build_compacted_history_with_limit` | 从最新往旧装填并在超限时截断 |
| `codex-rs/core/src/compact.rs` | `insert_initial_context_before_last_real_user_or_summary` | 四层插入位置规则 |
| `codex-rs/core/src/compact.rs` | `is_summary_message` | 靠前缀区分摘要与普通用户消息 |
| `codex-rs/core/src/compact.rs` | `build_compaction_initial_context` | 按注入模式造初始上下文 |
| `codex-rs/core/src/compact.rs` | `collect_annotated_user_messages` | 收集原话并排除旧摘要 |

## 参考

- [open-interpreter 源码：core/compact.rs](https://github.com/OpenInterpreter/open-interpreter/blob/main/codex-rs/core/src/compact.rs)
- [OpenAI Codex 仓库（本项目 fork 的上游）](https://github.com/openai/codex)
- [open-interpreter 文档](https://www.openinterpreter.com/docs)

## 与主核 / AI 的关系

主核里最接近的是「日志检查点（checkpoint）与恢复」：为了不让日志无限增长，定期做一次检查点，把之前的历史压成一份快照，后续从快照续上。这一页的 `CompactedHistoryMetadata` 与检查点元数据几乎同构——都记「这份快照对应哪个窗口、用什么生成、生成的标识是什么」。差别在于主核的快照是可逆的（数据完整保留在别处），而这里的摘要是**有损**的，所以它才需要那句「多次压缩会降低准确度」的警示。

与 goose 的 j01（上下文压缩）对比很有意思：那边有两层阈值（`auto_compact_threshold` / `capped_compaction_threshold`）与工具调用配对的边界计算；这边没有阈值体系，压缩是显式触发的（`CompactionTrigger::Auto` / `Manual`），且核心难点全在「摘要的位置」而不是「压缩量的边界」。差别来自触发方式：goose 是按比例自动压，open-interpreter 是按时机压（回合前 / 回合中 / 回合后 / 手动）。

与 smolagents 的记忆相比，那边是「把整个历史序列化成一个对象」，没有摘要、没有压缩——因为它的会话通常是单轮的。这里要支撑「跑几个小时的长会话」，压缩就成了核心机制。

对 AI 工程最实用的一条是「摘要的位置比摘要的内容更容易出错」。这份实现花了四层规则来定位插入点，原因就是摘要伪装成了 user 消息。如果系统当初把摘要编码成一条独立的 role（例如 `system` 或专门的 `summary` 类型），位置规则会简单得多。做上下文注入时，如果必须复用现有 role，一定要立刻补一个可靠的判别手段（这里是前缀常量），并在所有「按类型判断」的地方换成「按内容判断」。

另一条是「压缩时的超窗处理要从开头裁」。这个策略依赖 prompt 缓存的前缀性质，所以它不只是「删最少的东西」，而是「保住缓存」。任何在流式/长对话里做裁剪的地方，都应该先问一句「这次裁剪会不会让前缀缓存失效」——若会，裁剪的收益可能被重新处理整个前缀的成本吃掉。

最后一条是那条无条件警示。技术上有损的机制，应该在**每次执行**时都提醒使用者它的代价，而不是写在文档里等人去查。这条 `WarningEvent` 的出现位置（压缩成功之后、紧接着历史被替换）保证它落在「用户刚经历了一次压缩」这个上下文里，此时这句话才有意义。
