---
project: langchain
feature: stream-projection
title: 流式投影与增量合成
page_type: feature
schema_version: 1
refs:
  - path: libs/core/langchain_core/language_models/chat_model_stream.py
    symbols: [ChatModelStream, AsyncChatModelStream, SyncTextProjection]
  - path: libs/core/langchain_core/messages/base.py
    symbols: [BaseMessageChunk]
---

# 流式投影与增量合成

## 一句话职责

把一条消息的流式过程做成一个**可分叉的观察对象**：原始协议事件按到达顺序进重放缓冲，同时被三套独立累积器吸收，于是调用方既能按事件看「模型正在发生什么」，也能按字段看「文本涨到哪了」，最后还能拿到一份拼好的完整消息。

## 机制是什么

模块 doc 把两类消费者写得很清楚：

「`ChatModelStream` 是 `BaseChatModel.stream_events(version="v3")` 返回的同步变体。`AsyncChatModelStream` 是 `astream_events(version="v3")` 返回的异步变体。两者都暴露有类型的投影属性（`.text`、`.reasoning`、`.tool_calls`、`.usage`、`.output`），它们随着协议事件到达而累积。**投影可以被迭代取增量，也可以被 drain 取最终累积值**。原始协议事件也通过对流对象直接迭代而可用（**重放缓冲语义 —— 支持多个独立消费者**）」。

所以一个流对象上有**两个正交的视图**：

| 视图 | 取什么 | 语义 |
|---|---|---|
| 直接迭代流对象 | 原始协议事件（`MessagesData`） | 重放缓冲，多个迭代器各自从 0 开始 |
| 迭代 / drain 投影 | 某一类增量（文本、推理、工具调用） | 每类一个独立累积器 |

`ChatModelStream` 文档字符串还专门补了一条对输出形状的硬保证：「`.output.content` 永远是 v1 协议块的列表（text、reasoning、tool_call、image……），**与底层模型的 `output_version` 设置无关**。那个属性只控制 `stream()` / `astream()` / `invoke()` 路径；`ChatModelStream` 建在内容块协议之上，**按构造就产出 v1 形状**」。

`BaseMessageChunk` 在这一层是背景概念：`stream()` / `astream()` 那条路径产出的是消息**块**（`BaseMessageChunk`，靠 `+` 累加成完整消息）；而 v3 这条路径不产出块，它产出事件、由流对象自己组装成 `AIMessage`。两条路径的合成方式完全不同——一条是「块相加」，一条是「事件累积」。

## 怎么实现的

**投影不是过滤，而是第三个累积器。** 这是理解这一层的关键。`_ChatModelStreamBase.__init__` 里能数出**三套**并行累积：

1. **类型化累加器**：`_text_acc`、`_reasoning_acc`、`_tool_calls_acc`、`_invalid_tool_calls_acc` —— 服务公开投影。
2. **按块索引的存储**：`_text_per_block: dict[int, str]`、`_reasoning_per_block`、`_tool_call_chunks`、`_server_tool_call_chunks` —— 服务「每个块的 finalized 载荷」。
3. **块快照**：`_blocks: dict[int, FinalizedContentBlock]` —— 服务 `.output.content`。

第 3 套的注释写明了它是唯一真相源：「每个已 finalize 的块的有序快照，按事件索引键。`_output.content` 的唯一真相源。类型化累加器（text/reasoning/tool_calls/invalid_tool_calls）继续服务公开投影」。

而第 1 与第 2 套必须并存的原因，注释也说得很具体：「用于填充 finalized 块载荷，而不会把同一消息里同类型的其他块串味。**没有按块存储的话，消息级的累加器会把更早块的文本渗进后来的 finalized 块**」。

所以一条消息里有多个 text 块时，`_text_acc` 是全部拼接（那是 `.text` 投影想要的），而 `_text_per_block[idx]` 是「第 idx 块自己的文本」（那是 finalized 块载荷想要的）。两者用途不同，都必须维护。

**`dispatch` 是唯一的入口，且先记录后分派。** 实现是 `_record_event(event)` 然后按 `event.get("event")` 的字符串分派到六个内部处理器（`message-start` / `content-block-start` / `content-block-delta` / `content-block-finish` / `message-finish` / `error`）。

`_record_event` 的实现只有一行 `self._events.append(...)`。所以**每个事件都先落进重放缓冲，再被吸收**——因此「重放」与「投影」看到的是同一批事件，不可能出现某个事件只进了投影却没进缓冲的缺口。

`error` 事件走 `self.fail(RuntimeError(event.get("message", "Unknown error")))` —— 所以协议层的错误变成一个 `RuntimeError` 存在流的终态里，后续所有消费者都会在取到它时抛出来。

**重放缓冲的迭代是「游标 + 拉式补齐」。** `ChatModelStream.__iter__` 的循环四分支：

```python
if cursor < len(self._events): yield self._events[cursor]; cursor += 1
elif self._error is not None: raise self._error
elif self._done: return
elif self._request_more is not None:
    while cursor >= len(self._events) and not self._done:
        if not self._request_more(): break
    if cursor >= len(self._events):
        if self._error is not None: raise self._error
        return
else: return
```

`cursor` 是**每次调用 `__iter__` 都从 0 开始**的局部变量。所以「多个独立消费者」是靠「每次迭代新建游标」实现的，而不是靠复制缓冲。同一个流对象可以被 `for` 两遍，每遍都从头看到全部事件。

补齐那段 `while cursor >= len(...) and not self._done` 是**拉式反压**：缓冲空了但没结束，就调一次 `_request_more()` 让生产者再推一批。`_request_more()` 返回 `False` 表示「暂时没有更多」，此时 `break` 然后检查是否仍空——若仍空就 `return`（结束迭代）。

这个循环有个微妙处：`_request_more()` 返回 `False` 时，一次迭代就结束了，而不是挂起等待。所以这条路径**不是真正的阻塞等待**，而是「有多少拿多少」。真正的挂起只发生在 `_drain` / `.output` 那条「一直拉直到 done」的路径上（同步版会在那里真的自旋）。

`SyncProjection.__iter__` 的逻辑与它同构（`cursor` 局部、`_request_more` 补齐、`done` 结束、`error` 抛），只是读的是 `_deltas` 而不是 `_events`。所以「投影迭代」与「原始事件迭代」用的是**同一套拉取协议**——这也是为什么 `set_request_more` 会同时把回调装到流与其三个投影上：

```python
def set_request_more(self, cb: Callable[[], bool]) -> None:
    self._request_more = cb
    self._text_proj.set_request_more(cb)
    self._reasoning_proj.set_request_more(cb)
    self._tool_calls_proj.set_request_more(cb)
```

一个回调，四个消费者。所以「拉一次源」这个动作无论由谁发起，都会让所有视图向前推进——它们是同一份源的四个投影。

**`set_start` 是懒启动。** 它同样装到四处，而 `__iter__` / `get` / `_drain` 一进来就 `if self._ensure_started is not None: self._ensure_started()`。所以「拿到流对象」与「真正开始请求模型」是分离的：只有第一次消费才启动生产者。对同步路径这尤其重要——它让「先拿流、再决定怎么读」成为可能。

**`SyncTextProjection` 覆写了五个方法，全为「类型收窄 + 便捷语义」。** 它在 `_ProjectionBase` 之上做五件事：

- `push(delta: str)` / `complete(final_value: str)` 都先 `isinstance` 检查，非字符串抛 `TypeError`。`cast("Any", delta)` 是给类型检查器看的（参数已标注 `str`，但仍做运行时检查）。
- `__iter__` 对每条 delta 再验一次（`super().__iter__()` 是 `Iterator[Any]`），非字符串抛 `TypeError`。
- `get()` 加了一条 `if value is None: return ""` —— 所以「没完成就取终值」得到空串而不是 `None`。这让 `str(proj)` 在流未完成时安全。
- `__str__` 直接 `return self.get()` —— 所以 `str(stream.text)` 就是「拉完并给出全文」。
- `__bool__` 返回 `len(self._deltas) > 0` —— 注意它判的是「有没有推过增量」，不是「有没有最终值」。所以一个已完成但零增量的文本投影是 falsy。
- `__repr__` 两分支：`_final_set` 为真就 `repr(self._final_value)`，否则 `repr("".join(self._deltas))` —— 所以调试时看到的 repr 是「当前已累积的文本」，而不是对象地址。

这一串覆写的目的是让 `.text` 用起来像字符串：`str(...)`、真值判断、`print` 时的可读输出。而它仍然是可迭代的增量流。

**`_ProjectionBase` 用 `__slots__` 且把生产侧与消费侧分开。** 它的 docstring 明确分工：「`push` / `complete` / `fail` 是**生产侧 API** —— 由流在事件到达时调用。子类加上消费协议（同步迭代或异步迭代 + await）。`done` 与 `error` 是对终态的**安全只读视图**，给迭代器与其他兄弟对象用，使它们观察生命周期而不必伸手进底层字段」。

`__slots__ = ("_deltas", "_done", "_error", "_final_set", "_final_value")` —— 五个字段，`_final_set` 与 `_final_value` 分开存是为了区分「已设置终值」与「终值是 `None`」（`SyncTextProjection.get` 的 `if value is None` 就靠它之前的 `_final_set` 才能安全）。

`fail` 只做两件事：存 error、置 done。所以失败是「终态的一种」，与成功共用同一个 `_done` 标志——消费侧靠「先查 `_error` 再查 `_done`」区分这两种终态。

**异步版的差别只在消费侧。** `AsyncProjection` 与 `SyncProjection` 共享 `_ProjectionBase`，但消费协议完全不同：`__aiter__` 返回 `_AsyncProjectionIterator(self)`、`__await__` 返回 `_await_impl().__await__()`、`set_arequest_more` 收的是 `Awaitable[bool]` 而不是 `bool`。

`AsyncChatModelStream.output` 返回的是 `AsyncProjection`（`self._output_proj`），而同步版 `.output` 是个**阻塞 property**（直接返回 `AIMessage`）。同一个名字在两条路径上是两种形态——这是同步/异步 Python 双实现的固有代价，也是这一层为什么用两个类而不是一个类加 `is_async` 标志的原因。

`AsyncChatModelStream.__await__` 的实现是 `self._await_full().__await__()`，而 `_await_full` 三行：

```python
if self._ensure_started is not None: await self._ensure_started()
message = await self._output_proj
if self._producer_task is not None: await self._producer_task
return message
```

第二与第三行的顺序有个明确理由，注释写着：「**生产者任务在输出投影解析之后被 await，这样截至调用方 `await` 返回时，流后工作（尤其是 `on_llm_end` 回调）已经跑完**」。

所以 `await stream` 保证的不只是「消息拼好了」，还有「追踪回调已触发」。若反过来先 await 任务，那任务可能在投影完成前就被等完了（或反之导致回调被丢）。

**`_assemble_message` 的三处硬决定值得逐条读。**

第一处：块为空时退回裸字符串：

```python
if not self._blocks:
    content = self._text_acc
else:
    ordered_blocks = [self._blocks[idx] for idx in sorted(self._blocks)]
    content = [dict(b) for b in ordered_blocks]
```

`dict(b)` 是**浅拷贝**——所以 `AIMessage.content` 里的块不会与内部快照共享同一批字典对象。

第二处：**单个 text 块也不折叠成裸字符串**。注释解释了这条：

「`ChatModelStream` 是 v1 内容块表面：只要有任何块到达，content 就永远是协议块列表。**不要把单个 text 块压成裸字符串**——那会丢掉下游序列化器在后续回合里往返消息所需的块级字段（`id`、`index`、annotations、extras）」。

所以「看起来冗余的单元素列表」是为了保住块级元信息。这个决定与 `model_info` 那一类的「元数据不可丢」是同一个考虑：一旦压平成字符串，`id` 与 `index` 就再也回不来了。

第三处：`output_version` 最后钉成 `"v1"`：

```python
response_metadata.update(self._finish_reason_fields)
response_metadata["output_version"] = "v1"
```

注释：「**最后**钉 `output_version`：`stream_events(version="v3")` 永远把 content 组装成 v1 协议块，与 provider 配置的输出格式无关。若 finish 元数据里带 provider 提供的 `output_version`（例如 `ChatOpenAI(use_responses_api=True, ...)` 给的 `"responses/v1"`），那会让 `AIMessage.content_blocks` 对**已经是 v1 的内容**再跑一遍错误的翻译器」。

这句是这一层最有价值的注释之一：它点出了一个**双向一致性的陷阱**。`output_version` 是「内容是哪种形状」的标记；既然这里按构造就产出 v1，那这个标记必须是 v1，不能从上游照抄。否则下游看到「responses/v1」就会用对应的翻译器再翻一次——把 v1 内容当成 responses 形状读，得到错误的结果而不是报错。

**`response_metadata` 的合并顺序也是有意的三层**：先 `_start_metadata` 映射成 `model_provider` / `model_name`，再 `update(_finish_metadata)`，再 `update(_finish_reason_fields)`，最后钉 `output_version`。所以「越晚的越权威」：起始元数据被结束元数据覆盖，结束元数据被 finish-reason 字段覆盖（注释说「事件里的字段带着工具调用与拒答的修正」），`output_version` 无条件。

**`output_message` 与 `.output` 是两个不同的读法。** `output_message` 的 docstring 写得极准：

「如果流已结束则是组装好的消息，否则 `None`。**与 `ChatModelStream.output`（它会阻塞直到流结束）不同，这个方法从不拉取、不阻塞、也不抛异常**。供流驱动（`stream_events(version="v3")` 及其异步等价）在触发 `on_llm_end` 回调之前检查流是否产出了消息」。

所以同一个内部字段有两个访问器，一个是「给我结果，必要时推到底」，一个是「现在有就给，没有就 `None`」。后者存在的原因是驱动器需要一个**非阻塞探针**——它在回调时机上做决定，不能因为探一下就卡住。

`ChatModelStream.output` 的失败路径也明确：`_drain()` → 有 error 就 raise → 消息仍为 `None` 就 `RuntimeError("Stream finished without producing a message")`。所以「流结束了却没有消息」是一个显式错误，不是一个静默的 `None`。

**`aclose` 是这一层最复杂的一段，而它的复杂度全在「取消语义」上。** docstring 四句：

1. 「若消费者在流中途取消或决定提前停止迭代，**生产者任务会继续把 provider HTTP 调用泵到完成**，因为 `asyncio.Task` 对其等待者没有隐式链接。调用本方法来显式取消生产者；流会过渡到带 `CancelledError` 的错误态」。
2. 「若流已经成功产出了消息（例如在 `await stream.output` 之后），生产者可能仍在跑流后工作比如 `on_llm_end` 回调。这种情况下 `aclose()` **等待**任务而不是取消它 —— 把一次成功的运行变成被取消的会丢掉结束回调并**污染追踪**」。
3. 「幂等：安全地多次调用，包括在流正常结束之后。也被异步上下文管理器协议在 `__aexit__` 时调用」。
4. 后面还有两段注释解释实现细节。

第 1 句揭示了一个常被误解的事实：**`asyncio.Task` 不会因为没人 await 就停止**。所以「消费者提前 `break`」不会让 HTTP 请求停下来——它会继续跑完并把事件塞进缓冲。`aclose()` 的存在就是为了能主动停。

第 2 句是一个重要区分：`we_cancelled = not (self._output_message is not None and self._error is None)` ——**只有「没成功产出消息」时才算「我们取消了」**。若已成功，则不取消，只等待。理由写得很直接：取消会把 `on_llm_end` 丢掉，而丢掉结束回调等于追踪链断掉。

**那个 `done_future` 技巧是这一层最巧的一处。** 注释原文：

「通过一个链接的 `Future` 等待任务，而不是直接 await 任务。**直接 await 任务会在两种不可区分的情况下抛 `CancelledError`：（1）我们刚取消的那个任务完成了，（2）我们的调用方取消了 我们**。`asyncio.Task.cancelling()` 能在 3.11+ 上区分，但 3.10 上不存在。`done_future` 在任务结束（任何原因）时以 `None` 解析。它自身不是 `Task`，所以它的 `await` 只会在我们的调用方被取消时抛出 —— 给我们一个可移植、无歧义的信号来传播」。

所以这是为一个「API 版本差异」而做的兼容设计：要区分「我自己取消的」与「外面取消我的」，3.10 上没有官方手段，于是用一个普通的 `Future` 做中介。`Future` 的 `await` 只在被取消者取消时才抛，所以「抛了就说明是外面的取消，该传播」。

`add_done_callback(_link)` 与 `finally: task.remove_done_callback(_link)` 的配对也必要：回调必须在等待结束后摘掉，否则任务完成时会调用一个已失效的 `done_future`（虽然 `if not done_future.done()` 已经防住了，但摘掉能避免回调累积）。

最后那段处理「任务在 `_produce` 跑起来之前就被取消」的边界：此时流的 `CancelledError` 处理器从未执行，所以投影仍挂起、结束回调也没触发。代码显式补上两件事——`self.fail(cancel_exc)` 让 `await stream.output` 不会永久挂住；`self._on_aclose_fail` 的 `with contextlib.suppress(Exception)` 调用让追踪看到一个配对的结束事件。

**`bind_pump` 与 `set_request_more` 是两个名字同一件事。** `bind_pump(pump_one)` 的实现是 `self.set_request_more(pump_one)`，docstring 说「绑定一个泵用于独立流式。委托给 `set_request_more`。由 `BaseChatModel.stream_events(version="v3")` 使用」。而 `set_request_more` 的 docstring 说「由 langgraph 的 `GraphRunStream._wire_request_more` 使用，以连接共享的图级泵」。

所以两个命名对应两个调用方：独立的模型流式（单流一个泵）与图内的流式（多个流共用一个图级泵）。前者是「我自己推自己」，后者是「图推我们所有人」。同一个回调类型，两种调度来源。

## 关键取舍

**源码事实：** 一个流对象同时提供原始事件重放迭代与三套类型化投影；每次 `__iter__` 从游标 0 开始，所以多消费者只是「各建游标」。

**源码事实：** 每个事件先落进 `_events` 再做类型分派，所以重放与投影看到同一批事件。

**源码事实：** `_blocks` 是 `.output.content` 的唯一真相源，与类型化累加器并行维护。

**源码事实：** 按块索引的 storage 存在的原因是「消息级累加器会把更早块的文本渗进后来的 finalized 块」。

**源码事实：** 单个 text 块也不折叠成裸字符串，因为会丢掉 `id` / `index` / annotations / extras。

**源码事实：** `output_version` 在 `response_metadata` 里被**最后**钉成 `"v1"`，防止 provider 的值让 `content_blocks` 误跑翻译器。

**源码事实：** `output_message` 是非阻塞探针，从不拉取、不阻塞、不抛。

**源码事实：** `aclose` 只在「未成功产出消息」时才取消任务；已成功则只等待，避免丢 `on_llm_end`。

**源码事实：** `aclose` 用链接 `Future` 而不是直接 await 任务，为在 3.10 上区分两种 `CancelledError`。

**源码事实：** `SyncTextProjection.get()` 在无终值时返回空串，`__bool__` 判增量数而非终值。

**架构推论：** 「投影」这个词在这里是精确的：把一维的事件序列**投影**到几个不同的可观察量上（文本长度、推理长度、工具调用列表、最终消息）。投影不是「过滤事件」，因为它保留全部信息（时间维度还在：你可以迭代看增量）。这正是它与「把事件攒成一份结果」的区别——后者会丢掉过程。让两种视图都成立，靠的是「累积器 + 重放缓冲」双份存储。

**架构推论：** 三套累积器（消息级、块级、块快照）看似冗余，实际是三种不同「粒度」的需求：`.text` 要全文、块载荷要单块、`.output` 要原始块序列。把它们合成一个会在「多条 text 块」的情形下出错——而那恰恰是多模态/多段落回复的常态。这类「同一批事件，多个粒度」的结构在流处理里普遍存在，判据是：**不同消费者需要的聚合边界不同**，就别硬塞进一个累加器。

**架构推论：** `aclose` 那段关于 `CancelledError` 的兼容处理，反映的是异步编程里一个长期存在的语义模糊——「取消」没有主体。同一个异常可能来自上游、也可能来自自己的动作，而在某些 Python 版本上无法区分。用中介 `Future` 把「任务完成」与「我等的人被取消」分成两个不同的等待点，是把模糊语义拆成可判别语义的标准做法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `ChatModelStream` | 同步流对象，`.output` 是阻塞 property |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `AsyncChatModelStream` | 异步流对象，`.output` 可 await |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `SyncTextProjection` | 文本投影，`str()` 取全文、可迭代取增量 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `_ProjectionBase` | 生产侧 push/complete/fail 与只读终态 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `SyncProjection` | 拉式反压迭代，游标从 0 重放 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `_ChatModelStreamBase` | 三套累积器与事件分派 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `_ChatModelStreamBase.dispatch` | 唯一入口，先记录后分派 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `_ChatModelStreamBase.output_message` | 非阻塞探针，供驱动器判回调时机 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `_assemble_message` | 拼 AIMessage，钉 `output_version` 为 v1 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `ChatModelStream.set_request_more` | 一个回调装到流与三个投影 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `ChatModelStream.bind_pump` | 独立流式的泵绑定，委托给 `set_request_more` |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `AsyncChatModelStream.aclose` | 未成功则取消、已成功则等待，用 Future 消歧义 |
| `libs/core/langchain_core/language_models/chat_model_stream.py` | `AsyncChatModelStream.__await__` | 先等输出投影，再等生产者任务 |
| `libs/core/langchain_core/messages/base.py` | `BaseMessageChunk` | `stream()`/`astream()` 路径的块累加概念 |

## 参考

- [langchain 源码：language_models/chat_model_stream.py](https://github.com/langchain-ai/langchain/blob/master/libs/core/langchain_core/language_models/chat_model_stream.py)
- [langchain 文档：流式输出](https://python.langchain.com/docs/concepts/streaming/)
- [Python asyncio 文档：Task 与取消](https://docs.python.org/3/library/asyncio-task.html#task-cancellation)

## 与主核 / AI 的关系

主核里最接近的是「变更订阅（CDC）同时服务多个消费者」：一份变更流既可以被逐条消费（看过程），也可以被聚合成一份快照（看结果），而两者不能只保留一个——前者用于增量同步，后者用于一次性复制。这一页的重放缓冲 + 投影就是这个结构的对应物，连「多个消费者各自从起点开始」这个语义都一致。

差别在于这里还多了一层**协议版本**。主核的变更流只有一种形状；这里的事件流有 v1 与 v2 两代，而 `ChatModelStream` 承诺「按构造产出 v1」（见 h01 的 `_should_use_protocol_streaming`，它决定走不走 v2）。所以这一层同时是「多消费者」与「多版本」的适配面，复杂度来自两个方向的叠加。

与 h01（聊天模型契约）的分工：那一页的 `_should_stream` / `_should_use_protocol_streaming` 决定**要不要走流式、走哪个协议**；这一页定义**走流式之后拿到什么**。判定的产物就是这页的 `ChatModelStream` 实例。

与 g06（内存与加载）的一处直接呼应：两处的异步桥接都用了「哨兵对象代替 `StopIteration`」（那里是 `done = object()` + `next(iterator, done)`，这里是 `done = object()` 的同一手法出现在 `_astream`）。这是这一批代码里反复出现的同一个技术点，说明它在该代码库已被视为标准做法。

与 g04（内存存储）的 `load(..., allowed_objects=[Document])` 对照：两处都在「从外部数据构造对象」时做了限制。这里限制的是内容形状（`output_version` 钉 v1），那里限制的是类型白名单。共同点是**都对输入的自我描述保持不信任**——上游说它是 responses 形状，不代表你手上的内容真按那个形状组织的。

对 AI 工程最实用的一条是「流式对象要同时提供过程视图与结果视图」。很多实现只给其一：只给增量回调的函数，或者只给最终结果。结果是「想显示打字效果就得自己拼」「想拿完整消息就得自己攒」。让一个对象同时能被迭代（看增量）与被 drain（取终值），并且这两个视图共享同一份源，就把两种需求都一次性解决，且不会有「两条路径攒出的结果不一致」的隐患。

另一条是「单个块不要压平成字符串」。这在流式组装里是个真实的陷阱：看起来「只有一个 text 块，返回字符串更简洁」，但块上的 `id` / `index` / annotations 就此丢失，而下游在下一轮把这条消息发回去时需要它们。判据可以写成一句话：**若这个字段在后续回合会被需要，它就不能在组装时被丢弃**——哪怕当前看起来冗余。

最后一条是 `aclose` 里那个「成功就不取消」的区分。任何「包装了后台任务」的 API 都会遇到「调用方提前退出」这种情况，而「提前退出」有两种完全不同的意图：中途放弃（该取消）与已经读到结果（该只等待收尾）。把两者混为一谈的后果是「一次成功的调用被记成取消」——追踪里出现一个假的取消事件，而排查时会误以为是超时或用户中断。用「有没有成功产出」这个事实来分岔，是一个简单而可靠的判据。
