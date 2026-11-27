---
project: openai-agents
feature: streaming-events
title: 流式事件与去重发送
page_type: feature
schema_version: 1
refs:
  - path: src/agents/stream_events.py
    symbols: [RawResponsesStreamEvent, RunItemStreamEvent, AgentUpdatedStreamEvent]
  - path: src/agents/run_internal/run_loop.py
    symbols: [_stream_event_item_occurrence_key, _ensure_stream_event_item_occurrence_key]
---

# 流式事件与去重发送

## 一句话职责

把流式运行输出分成三层事件（模型原始增量、语义项完成、agent 切换），并给每个语义项打一个「发生次序号」，使同一个项不会因为被两条路径各发一次而让消费者看到两次。

## 机制是什么

`StreamEvent` 是三个 dataclass 的联合类型，每个都有固定的 `type` 字面量：

- `RawResponsesStreamEvent`：`data` 是 SDK 的 `TResponseStreamEvent`，`type = "raw_response_event"`。注释强调「直接从模型透传」。
- `RunItemStreamEvent`：`item` 是 `RunItem`，`name` 是十一个语义名之一，`type = "run_item_stream_event"`。
- `AgentUpdatedStreamEvent`：`new_agent`，`type = "agent_updated_stream_event"`，表示「有新 agent 在跑」。

十一个语义名覆盖了模型响应被解析出的所有项类型：`message_output_created`、`handoff_requested`、`handoff_occured`、`tool_called`、`tool_search_called`、`tool_search_output_created`、`tool_output`、`reasoning_item_created`、`mcp_approval_requested`、`mcp_approval_response`、`mcp_list_tools`。

其中 `handoff_occured` 有一条注释：「这里是拼错的，但不能改，那会是破坏性变更」。一个拼写错误被保留为契约，说明这些字符串是公开 API 的一部分。

## 怎么实现的

**三层事件的粒度分工。** 原始事件是「模型说了什么」——token 增量、内容块开始/结束这类协议级事件。语义事件是「运行产生了什么」——一条消息、一次工具调用、一个工具结果。agent 事件是「控制权转移」——交接之后新 agent 开始运行。三者不是同一件事的不同包装，而是三个观察层次：想渲染逐字输出用第一层，想渲染对话气泡与工具卡片用第二层，想更新「当前是谁在回答」用第三层。

**项到语义名的映射。** `stream_step_items_to_queue` 是一串 `isinstance` 分支，把 `RunItem` 子类逐个映射成 `RunItemStreamEvent` 并 `put_nowait` 入队。两个子类被显式排除：`ToolApprovalItem`（注释：「审批代表中断，不是流式项」）与 `CompactionItem`（注释：「压缩项是会话记账，不是流式项」）。落在 `else` 的类型打一条 warning 并丢弃。

所以「哪些项会作为流事件出现」是白名单式决定的：新增一个 `RunItem` 子类必须同时改这里，否则调用方永远看不到它。

**两条发送路径与去重键。** 这是这一页最需要说清的部分。`run_loop` 里有两处会发送流事件：一处在「响应被接受后」把本轮的 `new_items` 立刻发出去（为的是尽早让消费者看到），另一处在单步结果算完之后再发一次完整结果。若两处都发，同一个消息或工具调用就会出现两次。

解法是给每个 `RunItem` 挂一个隐藏属性。`_STREAM_EVENT_ITEM_OCCURRENCE_KEY` 是字符串 `"_agents_stream_event_item_occurrence_key"`；`_stream_event_item_occurrence_key(item)` 读它并校验是非空字符串；`_ensure_stream_event_item_occurrence_key(item)` 没有就 `uuid4().hex` 生成并 `setattr` 上去。

于是流程变成：第一处发送前，对每个待发项调 `_ensure_...` 取得键并记进 `emitted_model_item_occurrence_keys` 集合，然后入队；第二处发送前，先按同一函数读键，**键已在集合里的项过滤掉**，剩下的才入队。

**为什么用属性而不是字典键。** `RunItem` 是可变 dataclass 且未做 hash 处理，直接当字典键不可靠；而把键存成对象属性，则「同一个对象被两条路径看到时读到同一个键」自动成立——不需要按 id 或按内容匹配。这也解释了为什么键名要带 `_agents_` 前缀：它在对象 `__dict__` 里，必须避免与 dataclass 字段或第三方代码冲突。

**键的生命周期。** 键一旦生成就一直留在对象上，没有清理逻辑。所以「同一项被第三次发送」也会被同一集合拦住（只要集合还活着）。集合是每次流式运行内部的局部变量，运行结束即释放。

**流消费者的注册与取消。** `RunResultStreaming.stream_events` 是一段状态机。它维护三个闭包变量：`consumer_registered`、`registered_consumer_task`、`item_acknowledgement_pending`。四个内部函数分工明确：`acknowledge_item` 对队列调 `task_done`（保证每个 `get` 恰好对应一次 `task_done`）；`unregister_consumer` 递减 `_active_stream_consumers` 并在归零时置 `_stream_consumers_stopped`；`consumer_task_done` 在消费任务被取消或异常退出时触发注销；`register_current_consumer` 把当前 task 记成消费者（若换了 task 就先摘掉旧回调）。

`register_current_consumer` 里有一个细节：`if current_task is None or registered_consumer_task is current_task: return`——所以同一个 task 重复进入不会重复计数。

**主循环的四个退出条件。** 每轮先 `_check_errors()`，再算 `should_drain_queued_events`：只有 `MaxTurnsExceeded` 以及 `_should_drain_stream_events_before_raising` 判为真的异常才「先排空队列再抛」。然后：

1. 有存储的异常，且不需要排空（或队列已空）→ 标记完成并 break。
2. 已完成且队列空 → break。
3. 从队列 `get` 到 `QueueCompleteSentinel` → 先 `await self._await_task_safely(self._input_guardrails_task)`（注释：「等输入护栏跑完，以便捕获迟到的异常」），`task_done`，再 `_check_errors()` 一次，break。
4. `get` 被取消 → 置 `cancelled = True`、调 `self.cancel()`、重抛。

正常取到项时先置 `item_acknowledgement_pending = True`，然后 `yield item`，在 `finally` 里 `acknowledge_item()`。所以**即使消费方在 yield 之后抛异常**，队列的未完成计数也会被正确递减。

**排空判定的意义。** `should_drain_queued_events` 这段逻辑解决的是「已经产生的输出该不该在异常抛出前交给消费者」。对 `MaxTurnsExceeded`，答案是「该」——用户想看到的是「跑了很多轮最后超限」，之前那些消息是有价值的。对其他异常，由 `_should_drain_stream_events_before_raising` 逐类判定。

**取消的双向传播。** 消费方取消（`asyncio.CancelledError` 在 `get` 处抛出）会调 `self.cancel()`——把取消传播给后台运行任务，然后重抛。所以在 `stream_events` 上取消不只是「不消费了」，而是「停止这次运行」。docstring 里也写明「取消后应继续消费 `stream_events()`」，这样后台任务的收尾事件（包括最终结果）仍能被观察到。

## 关键取舍

**源码事实：** `handoff_occured` 的拼写错误保留下来了，注释明说改了会是破坏性变更。语义名是公开契约，不是内部标识。

**源码事实：** `ToolApprovalItem` 与 `CompactionItem` 被显式排除在流事件之外。所以「审批请求」不是通过 `RunItemStreamEvent` 通知的，而是通过 `mcp_approval_requested` 这类专门的语义名；`RunResultStreaming.interruptions` 才是审批的读取入口。

**源码事实：** 去重键挂在对象属性上，用 `uuid4` 生成，一旦生成永不清除。所以同一项跨多次 `stream_events` 调用（若消费者换了 task）仍保持同一身份。

**源码事实：** `stream_events` 允许 AsyncIterator 被中途 `break`，靠 `finally` 里的 `unregister_consumer` 递减计数并置 `_stream_consumers_stopped`。这使「不加限制地反复创建消费者然后丢弃」不会让运行永久挂在 `await self._event_queue.join()` 上。

**源码事实：** 队列元素有两种类型：`StreamEvent` 与 `QueueCompleteSentinel`。哨兵不 yield 给消费者，只用来 signalling 结束。

**架构推论：** 「先发一次、再过滤一次」的两路径设计，本质上是**用延迟换及时性**的折中。只在单步结果算完后发，消费者要在解析完所有工具调用之后才看到消息；只在响应接受后发，则有些项（在后处理里才生成的）根本发不出来。两条都发加上去重键，兼顾两者。

**架构推论：** 去重键选「对象上的随机 id」而不是「内容哈希」，是因为内容可能相同而实例不同（模型两次生成同样的文本是合法的两条消息），而这里要去重的是「同一个对象被发两次」。用实例身份是唯一正确的判据，而 Python 的 `id()` 会在对象释放后被重用——所以不能只存 `id()` 整数，必须让键跟着对象走。

**架构推论：** 消费任务注册这套机制解决的是一个隐形的资源泄漏：后台任务在等 `queue.join()` 时，若消费者已经放弃迭代且没通知，`join()` 永远不返回。把「消费者是谁」记在结果对象上，并在 task 结束时回调注销，是把生命周期绑定到 task 而不是绑定到迭代器对象。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/stream_events.py` | `RawResponsesStreamEvent` | 模型原始增量事件，直接透传 |
| `src/agents/stream_events.py` | `RunItemStreamEvent` | 语义项完成事件，带十一个语义名之一 |
| `src/agents/stream_events.py` | `AgentUpdatedStreamEvent` | 交接后新 agent 开始运行的通知 |
| `src/agents/stream_events.py` | `StreamEvent` | 三层事件的联合类型别名 |
| `src/agents/run_internal/streaming.py` | `stream_step_items_to_queue` | 项到语义事件的映射与入队，含排除项 |
| `src/agents/run_internal/streaming.py` | `stream_step_result_to_queue` | 把单步结果里的新项转发入队 |
| `src/agents/run_internal/run_loop.py` | `_stream_event_item_occurrence_key` | 读取对象上的去重键并校验 |
| `src/agents/run_internal/run_loop.py` | `_ensure_stream_event_item_occurrence_key` | 缺失时生成 uuid 去重键并写回对象 |
| `src/agents/result.py` | `RunResultStreaming.stream_events` | 消费者注册、错误检查、哨兵与取消处理 |
| `src/agents/result.py` | `RunResultStreaming.cancel` | 停下后台运行并入队结束哨兵 |
| `src/agents/result.py` | `RunResultStreaming._drain_event_queue` | 丢弃队列残留并把未完成计数补齐 |

## 参考

- [openai-agents-python 源码：stream_events.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/stream_events.py)
- [openai-agents-python 源码：result.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/result.py)
- [Agents SDK 文档：Streaming](https://openai.github.io/openai-agents-python/streaming/)

## 与主核 / AI 的关系

主核里与「三层事件」最接近的是执行器的观测层次：底层是元组级的执行细节（每个算子的输入输出），中层是算子完成事件，上层是事务/语句的边界。调优看底层，业务集成看中层，控制流看上层。这里的三层对应关系几乎一一对齐，差别在于模型流的「底层」是 token 级的，粒度细得多，因此中间必须有一层把增量聚合成「项」。

与 CrewAI 的事件总线相比，这里的流事件不经过总线：它们直接进 `asyncio.Queue`，由结果对象持有，由消费者拉取。这个选择让流式路径没有事件系统那些线程模型与处理器依赖排序的开销——但也意味着流事件只能有一个消费视角（谁迭代谁看到），不能像总线那样广播给多个监听器。

对 AI 工程最实用的一条是那个「用对象属性做去重键」的技巧。任何「同一实体可能从多条路径被观察到」的系统都需要这个机制，而通常的做法是按业务 id 或内容哈希去重——两者都会误杀合法重复。把一次性的随机键挂在对象上，去重的是「这一份数据有没有被发过」，语义精确且不依赖内容。

另一条是 `should_drain_queued_events` 那类判定。异常发生时要决定「已产生但未交付的输出怎么办」，这不是细节而是产品行为：用户看到 `MaxTurnsExceeded` 时，前面那些消息应该已经在流里了。把「哪类异常需要先排空」做成显式判断（而不是统一先抛或统一先排空），是让终止行为可解释的关键。
