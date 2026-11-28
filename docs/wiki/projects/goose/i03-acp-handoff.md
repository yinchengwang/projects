---
project: goose
feature: acp-handoff
title: 交接备忘录的预算装箱
page_type: feature
schema_version: 1
refs:
  - path: crates/goose/src/acp/handoff.rs
    symbols: [fit_unit, elide_to_budget, unit_cost]
  - path: crates/goose/src/acp/handoff.rs
    symbols: [redact_tool_responses, selection_units, FittedUnit]
---

# 交接备忘录的预算装箱

## 一句话职责

把 goose 侧的对话历史压成一段发给 ACP 对端的文本：先按「不可分的单元」分组，再从最近往过去装进 token 预算，装不下的单元只能降级或丢弃——绝不允许留下「半截工具调用」。

## 机制是什么

模块文档说得明白：当对话被交接给一个**没有原生会话可恢复**的 ACP agent 时，goose 侧的历史要作为**一整块文本**重放给它。这段重放必须和对方自己的系统提示、工具 schema 一起挤进同一个上下文窗口，所以在这里做预算、脱敏与截断，而不是整段发过去。

五条预算与保留规则：

- `CONTEXT_LIMIT_RATIO = 0.30`——备忘录最多占上下文窗口的 30%。
- `MAX_MEMO_TOKENS = 64_000`——绝对上限。
- `IMAGE_TOKEN_ESTIMATE = 1_600`——每张图按「满尺寸图片能达到的上限」计费，因为真实成本取决于需要解码才知道的尺寸，宁可高估。
- `PROTECTED_TOOL_EXCHANGES = 5`——最近 5 次工具交换的响应保持原样，更早的被脱敏。
- `MIN_ELIDED_TOKENS = 32`——截断后如果只剩不到 32 token，就没意义，直接丢弃。

## 怎么实现的

**为什么按「单元」而不按「消息」装箱。** 这是这一页的核心。`selection_units` 把消息切成「要么整组保留、要么整组丢弃」的连续区间，理由是注释里那句：「受保护的工具响应要跟着产生它的请求一起走，这样紧张的预算永远不会让一次交换的某一半变成孤儿」。

具体做法是一个「左边界传播」算法：

1. 初始化 `earliest[i] = i`（每条消息最早可以是自己）。
2. 遍历消息，记录每个 `ToolRequest` 的 id 与其所在下标。
3. 遇到**受保护**的 `ToolResponse` 时，查它的请求在哪条消息，把 `earliest[response_index]` 更新成 `min(原值, 请求下标)`。
4. 然后从末尾往前切：每次从 `start = end - 1` 出发，反复取 `earliest[start..end]` 的最小值；若最小值比 `start` 小，就把 `start` 往前挪，直到不再变化。这一段 `start..end` 就是一个单元。

第 4 步的循环是必要的：前移之后可能又暴露出新的依赖（例如请求本身又依赖更早的某条受保护响应），所以要反复直到不动点。这就是「传递闭包」的手写版本。

结果是：一条受保护的响应会把它对应的请求一起拉进同一个单元。所以一条工具响应的单元等于「请求 + 响应 + 夹在两者之间的所有消息」。

**从最近往过去装。** 主循环 `for unit in units.iter().rev()`——倒序遍历单元（最新的先装）。对每个单元 `fit_unit`，装得下就 `kept.extend(...)`，装不下就 `break` 整轮。

注意 `break` 而不是 `continue`：一旦某个单元装不下，更早的全部放弃。这符合「备忘录的价值随时间衰减」——越近的上下文越重要，中间的断点不该由「跳过一个大单元继续装更早的」造成（那会留下一段不连续的历史，比直接截断更难理解）。

**`FittedUnit.cost` 的三态。** 它是一个 `Option<usize>`：

- `Some(cost)`——单元原样装下，`cost` 是实际开销。
- `None`——单元**被降级**才装下，且这个 `None` 同时表示「到此为止，不再继续装更早的」。

第二重语义藏在主循环里：`match fitted.cost { Some(cost) => remaining -= cost, None => remaining = 0 }`。所以降级过的单元会把剩余预算清零，循环下一轮就 `if remaining == 0 { break; }`。这是一个「降级即终止」的隐式信号——用一个 `Option` 承载了「成本」与「终止标志」两种信息。

**三级降级。** `fit_unit` 依次尝试三种形态：

1. **原样**：`unit_cost(members) <= budget` 就直接用，`cost: Some(...)`。
2. **脱敏降级**：若单元里**含有工具响应**，就把**全部**响应替换成占位文本（`redact_tool_responses(&redacted[index], |_| false)`——那个恒假谓词意味着「一个都不保留」）。注释解释了为什么必须整批替换而不是截断某条：「把携带受保护响应的消息截断，会把单个调用从批次中间切出来。改成按陈旧交换的方式降级——请求保持完整、响应整条替换——这样没有任何东西处于半报告状态」。
3. **截断降级**：到这一步单元里已不含受保护响应了（第 2 步已把响应全部脱敏），所以可以放心 `elide_unit_to_budget`。注释说「这里已经没有携带受保护响应的消息了，所以过大的工具请求会被截断，而不是把整个备忘录拖垮」。

三级的顺序不能变：脱敏必须先于截断，因为截断一条带响应的消息会破坏交换的完整性，而脱敏保持了「有这个响应，但内容被隐去」这个结构。

**`elide_unit_to_budget` 的削峰策略。** 它是循环：算出每条成员的成本（`count_tokens + 1`，那个 `+1` 是消息之间的换行符），若总和不超预算就返回；否则找**最大**的那条，算出「除了它以外的部分占多少」，把剩余空间全给它，`elide_to_budget` 把它截成那个大小。

然后循环最多跑 `members.len()` 次——每轮至少削掉一条消息的一部分，所以有界。

`let (index, largest) = ...max_by_key(...)` 用 `?` 处理「空列表」的情况（虽然实际上不会空）。`budget.checked_sub(total - largest + 1)?` 这个 `?` 处理「连其余部分都超预算」——此时返回 `None`，整个单元被放弃。

注意那一行算的是 `total - largest + 1`：先减掉最大的那条的成本，再加 1（因为 `costs` 里每条都含 `+1`，所以减掉时多减了 1，要补回来）。这是一个容易被误读的细节。

**`elide_to_budget` 的二分式收敛。** 它要「中间截断、保留首尾」，且要用 token 计数而不是字节数。做法：

1. `budget < MIN_ELIDED_TOKENS` 直接 `None`（截了也没意义）。
2. 算 `ratio = budget / total`（预计需要保留的比例）。
3. 最多迭代 6 次：
   - `keep = (text.len() * ratio * 0.9)`——0.9 是安全系数，因为「字节比例」与「token 比例」不成正比。
   - `keep < 2 * MIN_ELIDED_TOKENS` 就 `None`（首尾各留至少要够 `MIN`）。
   - `head_end = floor_char_boundary(keep / 2)`、`tail_start = ceil_char_boundary(len - (keep - keep/2))`——**首尾各一半，且都对齐到字符边界**。
   - `tail_start <= head_end` 则 `None`（中间没有可删的部分了）。
   - 拼成「头 + `ELISION_MARKER` + 尾」再数 token，够小就返回。
   - 不够就把 `ratio *= 0.7` 重试。

第 3 步那个 `floor_char_boundary` / `ceil_char_boundary` 是 Rust 字符串的必要处理：切片下标必须落在 UTF-8 字符边界上，否则 `text.get(..)` 返回 `None`（代码里用 `?` 正是依赖这个安全行为）。两个辅助函数分别向前/向后找最近的有效边界。

`text.get(..head_end)?` 与 `text.get(tail_start..)?` 用 `Option` 而不是 `unwrap`，所以即使边界计算有误也只是返回 `None` 而不是 panic。

**脱敏的实现很节省。** `redact_tool_responses` 先判有没有要脱敏的（`if !message.content.iter().any(should_redact) { return message.clone(); }`），没有就**直接 clone 返回**，不做任何重建。有的话才逐条 map 替换。

注意它是**内容级**而非消息级：一条消息里可能同时有文本与多个工具响应，只有响应被替换成 `MessageContent::text(REDACTED_TOOL_RESPONSE)`，其余内容原样保留。这比「整条消息脱敏」保住了更多上下文。

**受保护集合按响应 id 而非位置。** `recent_tool_call_ids` 收集所有 `ToolResponse` 的 id，然后 `.rev().take(PROTECTED_TOOL_EXCHANGES)`——取最后 5 个。注释说明了这一点：「按响应为键，这样并行与批量调用会被**逐个**保护，而不是按消息位置」。

这个选择很实际：一轮里模型可能并行发起 5 个调用，它们的响应可能全在一条 user 消息里。若按「最后 5 条消息」保护，那条消息里的 5 个响应会全保留或全脱敏；按 id 保护则能精确到「最近 5 个响应」。

**预算的计算顺序。** `memo_token_budget(context_limit, current_prompt_tokens)` 先取 `min(context_limit * 0.3, 64000)` 作上限，再 `saturating_sub(current_prompt_tokens)`。

而 `current_prompt_tokens` 由 `prompt_token_cost(blocks, counter)` 算——当前轮的文本块按实际 token 数、图片按 1600、其余按 0。注释说不这么算的话「图片密集的一轮会把窗口花两次」（图片既被转给对端，又被算进备忘录预算，等于占用两份）。

`saturating_sub` 而不是普通减法，是因为当前提示可能已经超过上限——那时预算直接归零，备忘率为空（后面 `kept.is_empty()` 就返回 `None`）。

**头部与尾部的开销先扣。** `overhead` 先算上 `MEMO_HEADER` + `MEMO_FOOTER` + `OMISSION_MARKER_TOKENS`（16，给「N earlier messages omitted」那行预留）。所以真正用于消息内容的预算比名义预算小。

**`OMISSION_MARKER_TOKENS` 是预留而非实测。** 注释写「允许『earlier messages omitted』那一行的开销，它是在选择之后才写的」。所以先固定扣 16，然后不管实际那行多长。这是一个可接受的近似——那行最多几十个字符，16 token 的预留基本够。

**首尾固定文案承担了两件事。** `MEMO_HEADER` 声明「以下是 goose 在 ACP 会话创建之前的对话上下文」；`MEMO_FOOTER` 则是一句行为指令：「当前用户请求随后就到。仅用上面的上下文继续既有对话，不要把它当成新任务，也不要提及这次交接，除非相关」。

尾部那句是这一页最容易被忽略但最实用的部分：没有它，对端 agent 很可能把「一整块历史」当成「新的输入」重新开始，或者反过来在回答里说「根据你之前给我的上下文」。两句都是提示工程层面的必要护栏。

**`visible` 的三步过滤。** 在装箱之前，历史先过三道：

1. `Conversation::new_unvalidated(...).agent_visible_messages()`——只取 agent 可见的（排除仅供 UI 的消息）。
2. `.filter(|m| !m.is_turn_context())`——排除回合上下文这种元数据消息。
3. `.map(|m| m.agent_visible_content())`——只保留 agent 可见的内容部分。

所以发给对端的是「对端该看到的那部分」，不是「goose 内部存储的全部」。这三步的每一道都对应一种「内部有用但不必外传」的数据。

## 关键取舍

**源码事实：** 装箱单位是「连续消息区间」而非单条消息，且受保护的响应会把它对应的请求拉进同一单元（通过 `earliest` 的左边界传播算传递闭包）。

**源码事实：** 装不下就 `break`，不跳过继续装更早的。所以备忘录是历史的**连续后缀**，不是「挑重要的拼起来」。

**源码事实：** `FittedUnit.cost` 的 `None` 同时表示「被降级」与「到此为止」。降级即终止。

**源码事实：** 含受保护响应的单元在降级时把**全部**响应替换成占位文本，而不是截断某一条。之后才允许截断。

**源码事实：** 受保护集合按响应 id 取最近 5 个，不是按消息位置。

**源码事实：** 图片一律按 1600 token 计费，且当前轮提示里的图片也计入扣减。

**源码事实：** `elide_to_budget` 最多迭代 6 次，每次把 `ratio` 降为 0.7 倍；`MIN_ELIDED_TOKENS = 32` 是「值得截断」的下限。

**架构推论：** 「按单元装箱」解决的是一个**结构完整性**问题，而不是效率问题。按消息装箱能让预算填得更满（大消息可以被跳过），但会产生「请求在、响应不在」或「响应在、请求不在」的历史。给对端 agent 看一段残缺的工具交换，比看一段较短但完整的历史更糟——因为它会基于错误的因果推断继续工作。

**架构推论：** 「降级即终止」这个设计把「备忘录的边界」与「最后一个完整保留的单元」绑定在一起。若无此约束，可能出现「中间某几个单元被降级脱敏、更早的几个原样保留」——那种备忘录里新旧信息密度不一致，对端很难判断哪些是真实、哪些是被抹掉的。终止在第一次降级处，让整段备忘录只有一种「保真度」。

**架构推论：** 「先脱敏后截断」的顺序是被结构约束逼出来的。截断一条带工具响应的消息，会让那个调用的参数只出现一半——对端可能据此认为「这个调用传了不完整的参数」。而脱敏保留了「调用发生过、输出被隐去」这个可解释的结构。所以脱敏不是「省 token 的手段」，而是「在不能保留原文时的正确降级形态」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `crates/goose/src/acp/handoff.rs` | `build_handoff_context_memo` | 过滤、脱敏、装箱、拼接备忘录全文 |
| `crates/goose/src/acp/handoff.rs` | `memo_token_budget` | 30% 与 64k 双上限，扣除当前提示开销 |
| `crates/goose/src/acp/handoff.rs` | `prompt_token_cost` | 当前轮文本按实数、图片按固定估值计费 |
| `crates/goose/src/acp/handoff.rs` | `selection_units` | 左边界传播算传递闭包，切出不可分单元 |
| `crates/goose/src/acp/handoff.rs` | `recent_tool_call_ids` | 按响应 id 取最近 5 次交换 |
| `crates/goose/src/acp/handoff.rs` | `fit_unit` | 原样/脱敏/截断三级降级 |
| `crates/goose/src/acp/handoff.rs` | `elide_unit_to_budget` | 反复削最大成员直到整单元装下 |
| `crates/goose/src/acp/handoff.rs` | `elide_to_budget` | 首尾各半的中间截断，最多 6 轮收敛 |
| `crates/goose/src/acp/handoff.rs` | `unit_cost` | 逐条 token 数加换行符 |
| `crates/goose/src/acp/handoff.rs` | `redact_tool_responses` | 内容级替换响应为占位文本 |
| `crates/goose/src/acp/handoff.rs` | `FittedUnit` | 承载「消息 + 可选成本」的双重语义 |
| `crates/goose/src/acp/handoff.rs` | `floor_char_boundary` | UTF-8 安全的下取整切片点 |

## 参考

- [goose 源码：acp/handoff.rs](https://github.com/aaif-goose/goose/blob/main/crates/goose/src/acp/handoff.rs)
- [Agent Client Protocol 规范](https://agentclientprotocol.com/)
- [goose 文档：ACP 代理](https://block.github.io/goose/docs/guides/acp-providers/)

## 与主核 / AI 的关系

主核里最接近的是「WAL 段的截断与保留」：日志段可以因为空间不足被归档或删除，但不能留下「半个事务的记录」——一旦某条记录被截断，恢复时看到的事务就不完整。所以截断的单位是「段」或「事务」而不是「字节」，且必须先在完整边界上切分。这里的「单元」正是那个边界。

与 smolagents 的 `truncate_content` 对比很有意思：那个是「首尾各留一半、中间插一句说明」，单元是单条字符串。goose 这里的 `elide_to_budget` 在算法上几乎是同一个函数（首尾各半、中间插标记、用 0.9 安全系数与多轮 ratio 收敛），但**作用单位不同**：smolagents 用它截断单个工具输出，goose 用它截断一个单元里最大的那条消息，且在此之前必须先做脱敏降级。所以同一套算法在两个「层」上各有职责。

对 AI 工程最实用的一条是「按不可分的结构单元装箱」。做上下文预算时，最容易的做法是把消息当成等价的块、从新到旧装填。这在纯对话历史里没问题，但只要涉及工具调用，就一定会有「配对结构」——请求与响应必须同进同退。识别这些配对并算出它们的传递闭包，是让压缩结果仍然「可理解」的关键。这个算法本身（先标记依赖、再从后往前切区间）可以直接复用到任何带配对结构的历史压缩场景。

另一条是首尾那句固定文案的作用。给一个 agent 塞另一段 agent 的历史时，**必须显式声明这段文字的地位**：它是「之前对话的延续」而不是「新任务」，也不该被提起。没有这两句，压缩做得再精确也会导致对端行为错位——它可能把整段历史当成新的输入重新分析，或者在回答里说「根据你之前提供的上下文」。这类「元信息提示」的成本极低（几十 token），但决定了交接是否成功，值得在任何跨 agent 的上下文传递里固定带上。
