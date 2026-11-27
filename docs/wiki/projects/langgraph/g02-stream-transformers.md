---
project: langgraph
feature: stream-transformers
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/stream/transformers.py
    symbols: [ValuesTransformer, UpdatesTransformer, MessagesTransformer]
---

# 内置流式转换器

## 一句话职责

内置转换器把「主日志上同构的协议事件」分成几类可迭代的投影：值快照、节点增量、聊天模型流，各自只认自己的方法名与自己的命名空间。

## 机制是什么

所有内置转换器共享一个骨架：`init()` 返回 `{投影名: StreamChannel}`；`process(event)` 先按 `event["method"]` 过滤，再按 `params["namespace"]` 过滤，通过就往自己的 channel 里 push。

`_native = True` 让投影键成为 run stream 上的直接属性（`run.values`、`run.messages`），而不只是 `extensions` 里的一项。`required_stream_modes` 声明这个转换器需要图发出哪些 stream mode——`Pregel` 把所有已注册转换器的声明取并集，作为内部 `stream()` 调用的 `stream_mode`，没有硬编码的默认集合。

`scope` 决定了「自己的层级」：根调用的 scope 是空元组，嵌套调用携带外层图的检查点命名空间。每个转换器都缓存了一份 `list(scope)` 用于和事件的 `namespace` 列表做廉价相等比较。

## 怎么实现的

**ValuesTransformer。** 除投影外还维护 `_latest`、`_interrupted`、`_interrupts` 三份旁路状态：`values` 事件带 `interrupts` 字段时累加。`error` 属性直接读底层 channel 的 `_error`，由 mux 在失败时自动写入，转换器自己不管。

**UpdatesTransformer。** 最薄的一个：只把 `updates` 事件里的 `data` 往 channel 里塞。一个 item 是「节点名 → 该节点返回的更新」的映射。

**MessagesTransformer。** 唯一真正有状态的转换器。它处理两种输入形状：

1. 协议事件字典（带 `"event"` 键）。按 `metadata["run_id"]` 在 `_by_run` 里索引到具体流：`message-start` 建新流并入投影；后续事件 `dispatch` 给该流；`message-finish` 关闭并从表里删掉。role 为 `tool` 的起始事件被记进 `_ignored_runs`——工具结果属于 tools 投影和状态快照，不属于聊天 token 投影——且要等到它的 `message-finish` 才从忽略集合里移除。
2. 完整 `BaseMessage`（来自节点返回的最终消息）。用 `message_to_events` 合成一套协议事件生命周期回放进去，再把已完成的流 push 进投影。`AIMessageChunk` 与 `ToolMessage` 被显式排除。

**泵的绑定。** `MessagesTransformer` 实现了 `_bind_pump` / `_bind_apump`，因为每个 `ChatModelStream` 的游标需要能推动整张图前进。`_make_stream` 优先用异步泵——这样在 `AsyncGraphRunStream` 下嵌套迭代不会另起后台任务；两个泵都没绑时退化成未接线的流（单测手工派发事件时走这条路）。

**生命周期清理。** `finalize()` 只清路由表——流靠自己的 `message-finish` 关闭。`fail(err)` 则主动把错误推给所有仍然开着的流，否则消费者会永远等一个不会到达的结束事件。

**任务驱动的生命周期推断。** `LifecycleTransformer` 与 `SubgraphTransformer` 共同继承 `_TasksLifecycleBase`：都靠观察 `tasks` 事件发现子图，`started` 在某个命名空间首次出现时触发，终止状态在父任务的 `TaskResultPayload` 到达时判定。基类把「发现—打开集合—终止转换」的簿记集中起来，子类只实现三个模板方法。`process` 对 `tasks` 事件返回 `False`，把它折叠进更高层投影，避免消费者在原始流里重复看到同一信息两次。

## 关键取舍

**源码事实：** 转换器只认自己 scope 上的事件，更深的子图事件留在主日志里但不进投影。若消费者需要子图 token，要么迭代原始事件流，要么自己注册转换器。这是「投影是单层视图」的明确边界。

**源码事实：** 旧式的 `AIMessageChunk` 元组（来自 `on_llm_new_token`）不进 `messages` 投影。想用内容块流式填充 `run.messages`，必须走 `stream_events(version="v3")`。这是故意断掉一条历史路径，而不是兼容两种。

**源码事实：** `tasks` 事件被抑制出主日志。好处是消费者不会在两个抽象层看到同一件事；代价是想要任务级原始细节的人必须显式注册 `TasksTransformer`。

**架构推论：** `required_stream_modes` 让「注册什么转换器」决定「图实际推什么流」。这比固定集合更省——不注册 messages 转换器，图就不会为 token 流做额外工作；但也意味着漏声明会导致投影永远空着而无报错。

**架构推论：** `before_builtins` 与注册顺序耦合，说明内置转换器之间的顺序本身承载语义：`MessagesTransformer` 会把文本字段快照进投影，所以改写内容的转换器必须排在它前面。这是一条隐式的、只能靠文档与约定维持的依赖。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/stream/transformers.py` | `ValuesTransformer` | 抓本层 values 快照，附带 interrupt 旁路状态 |
| `libs/langgraph/langgraph/stream/transformers.py` | `UpdatesTransformer` | 抓本层 updates，最薄的透传投影 |
| `libs/langgraph/langgraph/stream/transformers.py` | `MessagesTransformer` | 按 run_id 把协议事件路由到各自的聊天模型流 |
| `libs/langgraph/langgraph/stream/transformers.py` | `CustomTransformer` | 抓节点经 `get_stream_writer()` 写出的自定义数据 |
| `libs/langgraph/langgraph/stream/transformers.py` | `_TasksLifecycleBase` | 子图生命周期的共同簿记与模板方法 |
| `libs/langgraph/langgraph/stream/transformers.py` | `SubgraphTransformer` | 为每个直接子图建 mini-mux 与导航句柄 |
| `libs/langgraph/langgraph/stream/_types.py` | `StreamTransformer` | 转换器契约：车道判定、`schedule`、生命周期钩子 |

## 参考

- [LangGraph 源码：stream/transformers.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/transformers.py)
- [LangGraph 源码：stream/_types.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/_types.py)
- [LangGraph 文档：流式模式](https://langchain-ai.github.io/langgraph/concepts/streaming/#streaming-modes)

## 与主核 / AI 的关系

这些转换器就是主核里「同一份日志喂多个物化视图」的做法：每个视图只读自己关心的行、只维护自己的游标，谁也不替别人算。区别在于主核的视图可以落后于日志（最终一致），这里的投影必须**等长**——`values` 事件少抓一个，消费者就永久少了那一步的状态，没有补读机制。

对 AI 侧而言，`MessagesTransformer` 的 `_ignored_runs` 是一个值得记住的细节：工具调用的输出不走聊天 token 投影。做 agent 前端时若发现「工具结果在 `run.messages` 里看不到」，那不是 bug，而是投影边界的自觉选择。
