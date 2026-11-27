---
project: pydantic-ai
feature: history-processing
title: 历史处理与重写
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_run_conversation.py
    symbols: [_HistoryMerge, conversation_from_state, full_history]
  - path: pydantic_ai_slim/pydantic_ai/_enqueue.py
    symbols: [PendingMessageQueue, PendingMessage]
---

# 历史处理与重写

## 一句话职责

一次运行结束后，把「它收到的历史」「它中途丢弃或改写的历史」「它新加的消息」重新合成一份完整历史，并算出「这份历史里哪些是存储端还没有的」；同时在运行过程中维护一个可随时注入消息的待投队列。

## 机制是什么

两条彼此独立的机制，共同决定「模型看到的历史」与「会话存下的历史」：

- `_HistoryMerge` 负责**事后合成**：把运行记录下来的消息与运行真正使用的历史对齐，识别出「运行丢掉了哪些」「改写了哪些」，从而在不丢消息的前提下重新构造完整历史。`conversation_from_state` 与 `full_history` 是它的两个入口。
- `PendingMessageQueue` 负责**运行中注入**：`RunContext.enqueue` 把消息排进队，`deliver_asap_messages` 在下次请求前把它们塞进请求里，同时塞进持久历史。

`PendingMessage` 是队列元素，带 `priority`（`'asap'` / `'when_idle'`）、`placement`（`'append'` / `'before_prompt'`）、`enqueue_id`，以及一个内部字段 `_tool_call`（`(tool_call_id, run_step)`）。

## 怎么实现的

**`rewritten` 的判定规则精确到「算什么算改写」。** 它先算出「运行会为一条请求补哪些字段」：`STAMPED_FIELDS` 里那些原始消息为 `None` 而现在有值的字段。然后一条特例：如果两边都是 `ModelRequest` 且 `_keeps_in_order(message.parts, original.parts)` 成立（原有部件都还在、顺序不变、只是周围多了别的部件），就把 `parts` 也算作「补」而非「改」。最后用 `dataclasses.replace(original, **filled) != message` 判定。

这条规则的实际含义很重要：**运行在请求周围添加部件（重新注入的系统提示、设置变更记录）不算改写**；**改动或丢弃请求自身的某个部件才算改写**。`_keeps_in_order` 的实现是「逐个在剩余迭代器里找」，也就是子序列判定——顺序不变但可穿插。

**`unsaved_start` 的双分支。** 若 `state.conversation_saved` 为真，直接返回 `len(messages)`（全部已存）。否则取 `conversation_unsaved`（运行收到但存储端还没有的那批），算出它们在当前历史里的起点 `new_message_index - len(unsaved)`，再逐条用 `rewritten` 比对「当前历史里的对应位置」与「当初收到的那批」——只要有一条被改写，就返回 `None` 表示「无法局部对齐，让存储端整份比较」。这条设计避免了一个典型问题：运行改了历史之后，用长度差做增量会让存储端与运行端永久错位。

**`_split_merged_tail` 处理「合并保存的请求」这一特殊形态。** 运行可能把多条未保存请求合并成一条发出（省一次往返），而存储端需要看到它们各自独立。这个函数把合并后的那条请求拆回：先按部件计数校验「合并请求的部件 = tail 部件 + 各追加请求部件」（用逐个 `remove`，任一缺失即判定被就地编辑过、放弃拆分）；再逐条重建追加请求，补上运行填的字段（`timestamp` 有个特例：不采用合并请求的时间戳，因为合并请求带的是最新的那个）；最后把追加请求的最后一条补上「合并请求里多出来的部件」。返回新历史与调整后的 `new_message_index`。

**`full_history` 的两条来源。** 运行结束时用 `keep_full_history` 保下来的那份；运行抛异常时从运行记录里算出来（不改动记录，因为这个运行可能还在继续——嵌套运行的错误会带着外层运行的会话）。`keep_full_history` 的注释说明了时机：必须在最后一个节点与其钩子跑完之后调用，这样「结果里的会话既不会再看历史、也不持有记录」，于是运行结束后对历史的编辑仍能到达会话——除非有哪个能力在运行中丢弃或改写了消息。

**`conversation_from_state` 的三处拷贝。** `usage` 用 `copy()`、`deferred_tool_requests` 用 `_copy_requests`（连 `_kinds` 都拷一份）。文档给出理由：会话要能被携带与修改，而不碰到运行自己的记账与它收到的那个会话。这是一条「交出所有权」的边界。

**`failed_run_conversation` 的一条细则。** 若运行在添加任何消息之前就失败了（比如还没应用「会话正在等待的工具调用」的结果），那这些调用应当**继续等待**；因此当 `len(state.message_history) > deps.new_message_index` 为假时，把原来的 `conversation_deferred_tool_requests` 原样带上，而不是置空。这条区分让「失败的轮次」不会误吞待决状态。

**队列的三个优先级与两个落点。** `priority='asap'` 在最近的机会投递（下一请求，或本该终止时的重定向）；`'when_idle'` 只在本该终止时投递，且排在所有 `asap` 之后。`placement='append'` 落在历史已有内容之后；`'before_prompt'` 落在即将发送的那条请求的用户提示之前。两者可组合，因此同一批消息在 `asap` 下被降级为 `when_idle` 时，会作为独立请求投递（不再紧挨工具结果）——`_tool_call` 的文档明确这一点。

**`_tool_call` 为什么要带 run_step。** 因为它约定「这条内容紧挨着那个工具调用的结果之后」。只带 `tool_call_id` 不够：模型可能在后续轮次**复用**同一个 ID，而一个早先调用晚些时候（比如它启动的任务里）入队的内容，不该跑到后来那次返回的旁边。带上步号就能区分。

**投递同时落到两处。** `deliver_asap_messages` 把内容放进 `request_context.messages`（这一步模型就能看到）与 `ctx.messages`（持久历史里也留一份）。被写进的那条请求会被替换成副本，`PendingMessageQueue.request_in_history` 能找到它。这条双写是「enqueue 既是提示也是一条真实消息」的实现基础。

**事件与顺序保证。** 每次被 drain 的 `enqueue` 调用发出一条 `EnqueuedMessagesEvent`，按内容落进历史的顺序发出，且**精确描述实际投递的内容**。文档说明这样做的目的：UI 适配器按这个顺序流给客户端，客户端再按这个顺序装回历史，于是客户端渲染的历史与服务器一致。这是一个端到端的顺序契约，而不是通知。

**同一落点的多次投递保序。** 文档明说：同一位置的多次投递按入队顺序排列，无论中间到了什么其它内容。这是靠 `_last_delivered_for_call` 与 `_last_delivered_before_prompt` 两个记录实现的：每次投递都接在上次同一位置的投递之后。

**队列关闭后不可入队。** `append` 在 `_closed` 为真时抛 `UserError('enqueue is not available because the agent run has ended.')`。`dequeue` 的文档补了一句：一个被提前离开的运行留下的队列已经关闭，里面的消息也取不回来了。这是「运行结束后不再接受外部干预」的显式边界。

**`_capability_loads` 与消息分开存。** 队列里除了 `PendingMessage` 还单独存 `CapabilityLoadRecord`，注释给出理由：能力加载会搭下一次请求的便车，但**绝不能导致运行继续**，因此任何「检查是否有待处理消息」（例如判断运行会不会继续）的代码都不能看到它。

**帧相似性检查发生在入队前。** `_enqueue.py` 引入了 `resembles_frame` 与 `operator_voice_lookalike`，也就是说框架会检查入队内容是否「看起来像框架自己的帧/语气标记」，并据此处置。这与 `framing` 那一层的策略一致：内容不转义，但会给相似内容加注记。

## 关键取舍

**源码事实：** 「改写」与「补充」被精确区分。运行在请求周围添加部件（重新注入的系统提示、设置变更、能力部件）不算改写；改动或丢弃请求自身部件才算。这条区分让增量保存能在常见情况下工作，只在真实改写时退回整份比较。

**源码事实：** 一旦无法局部对齐（某条被收到过的消息被改写），增量保存直接放弃并交给存储端整份比较。不做模糊匹配，也不猜。

**源码事实：** 合并保存的请求会被拆回多条再交给存储端，且拆之前会按部件计数校验没被就地编辑过。校验失败就放弃拆分而不是尽力而为。

**源码事实：** `enqueue` 的内容同时进「本次请求」与「持久历史」。它不是纯粹的临时插话，会作为一条真实消息留下。

**源码事实：** `_tool_call` 带 run step 是因为 `tool_call_id` 会被复用。这条细节说明「按 ID 关联」在这种历史结构里不总成立。

**源码事实：** 能力加载记录与待投消息分开存，因为它「搭便车但不会导致运行继续」，不能被「是否有待处理消息」的判定看到。

**源码事实：** 队列在运行结束后关闭，且关闭后取不回来消息。这是显式的生命周期终点。

**架构推论：** 这一层解决的是「多方都能改历史」带来的对账问题：运行会补字段、加部件、合并请求、丢弃消息，能力也会改写。`_HistoryMerge` 的价值在于**它不假设历史没被动过**，而是逐条判定「这条还是当初那条吗」。代价是这套判定依赖对象身份与顺序（`_keeps_in_order` 用成员关系），因此「就地编辑一条消息的部件」这种操作会破坏对齐——文档对此给出的建议是构造替代副本而不是就地改。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `rewritten` | 区分「运行补充的字段」与「真正的改写」 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `_keeps_in_order` | 子序列判定：原部件都在且顺序不变 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `unsaved_start` | 未保存消息的起点；无法对齐时返回 `None` |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `conversation_from_state` | 从运行状态构造会话，usage 与待决请求都取副本 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `full_history` | 完整历史的两条来源：结束时保留的、异常时推算的 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `keep_full_history` | 在末节点与钩子跑完后保留完整历史 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `_split_merged_tail` | 把合并发出的请求拆回多条，含部件计数校验 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `failed_run_conversation` | 失败轮次的会话，保留待决请求 |
| `pydantic_ai_slim/pydantic_ai/_run_conversation.py` | `_HistoryMerge` | 把运行记录的历史合并进它收到的与新增的消息 |
| `pydantic_ai_slim/pydantic_ai/_enqueue.py` | `PendingMessage` | 待投消息：优先级、落点、一步一号的工具关联 |
| `pydantic_ai_slim/pydantic_ai/_enqueue.py` | `PendingMessageQueue` | 线程安全的队列入与 drain，含能力加载独立存储 |
| `pydantic_ai_slim/pydantic_ai/_enqueue.py` | `deliver_asap_messages` | 把 asap 内容投进请求与持久历史的双写入口 |
| `pydantic_ai_slim/pydantic_ai/_enqueue.py` | `run_queue` | 从运行上下文中取当前队列 |

## 参考

- [pydantic-ai 源码：_run_conversation.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_run_conversation.py)
- [pydantic-ai 源码：_enqueue.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_enqueue.py)
- [Pydantic AI 文档：消息历史](https://ai.pydantic.dev/message-history/)

## 与主核 / AI 的关系

`_HistoryMerge` 的工作与主核的「日志与快照对账」同类：一方有一份记录，另一方有当前状态，要判定「哪些已经持久化了、哪些是新的、哪些被改过」。主核对账靠 LSN 与页版本，这里的对账靠对象身份、字段补齐与子序列判定——因为没有单调序号可用。

对 agent 工程的直接价值有三条。其一，**别就地改历史里的消息部件**。删一个部件、就地替换一个部件的值，都会让增量保存失效或产生错位；构造替代副本才是安全做法。其二，**`enqueue` 不是旁白**，它的内容会成为历史中的真实消息，并且会触发一个按落点顺序的事件流。要「只说一次」的提示，注意它会被持久化。其三，**运行结束时待投队列会关闭**，之后任何入队都报错；任何从工具启动的后台任务若想回灌消息，必须在运行生命周期内完成，否则消息会被静默丢弃（队列关闭时取不回来）。
