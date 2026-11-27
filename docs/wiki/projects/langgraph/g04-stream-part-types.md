---
project: langgraph
feature: stream-part-types
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/types.py
    symbols: [ValuesStreamPart, MessagesStreamPart, DebugStreamPart]
---

# 流事件类型与负载

## 一句话职责

StreamPart 是一组按 `type` 字段判别的 TypedDict，把「图吐出来的每一种流式负载」在类型层面固定下来，让消费者靠 `part["type"]` 就能收窄到具体形状。

## 机制是什么

`StreamPart` 是一个 `TypeAliasType`，联合七种成员：`ValuesStreamPart[OutputT]`、`UpdatesStreamPart`、`MessagesStreamPart`、`CustomStreamPart`、`CheckpointStreamPart[StateT]`、`TasksStreamPart`、`DebugStreamPart[StateT]`、`ToolsStreamPart`。

每个成员都带三个固定字段：`type`（`Literal`，就是判别键）、`ns`（`tuple[str, ...]`，事件所在的命名空间路径）、`data`（负载本体）。`ValuesStreamPart` 多一个 `interrupts` 字段——因为每一步的状态快照要连带这次中断的负载一起交给调用方。

`StreamPart` 自身是泛型的，`StateT` 与 `OutputT` 两个类型参数分别穿到 checkpoints/debug 与 values 上。所以同一份图定义能让类型检查器知道「values 的 data 是 OutputT，checkpoints 的 data 是 CheckpointPayload[StateT]」。

## 怎么实现的

**判别键即 stream mode。** `type` 的字面量取值就是 `StreamMode` 的成员：`values`、`updates`、`checkpoints`、`tasks`、`debug`、`messages`、`custom`、`tools`。这不是巧合——v2 协议里 part 的 `type` 直接被当作方法名写进协议事件（`convert_to_protocol_event` 把 `part["type"]` 塞进 `method`），所以「加一种 stream mode」与「加一个 StreamPart 成员」是同一件事。

**`ns` 而非手工拼名。** `ns` 是元组形式的分段路径，每一段通常是 `node_name:task_id`。转换器靠把它和 `scope`（也是元组）直接做相等比较来判断「这是不是我这层的事件」。用元组而不是拼接字符串，是为了让比较不需要解析。

**负载分层。** 各成员的 `data` 形状差别很大，且都是具名 TypedDict 而不是裸 `Any`：

- `ValuesStreamPart.data` 是 `OutputT`——按输出通道读出来的整份状态。
- `UpdatesStreamPart.data` 是 `dict[str, Any]`，键是节点名；源码注释明确它还可能含 `__interrupt__` 与 `__metadata__` 这两个特殊键。
- `MessagesStreamPart.data` 是 `tuple[AnyMessage, dict[str, Any]]`——消息本体加元数据（`langgraph_step`、`langgraph_node`、`langgraph_triggers` 等）。
- `TasksStreamPart.data` 是 `TaskPayload | TaskResultPayload`，靠 `"result"` 键是否存在来区分开始与结果。
- `DebugStreamPart.data` 是 `DebugPayload[StateT]`，内部再按 `type` 分成 checkpoint / task / task_result 三种（各带 `step` 与 `timestamp`）。
- `ToolsStreamPart.data` 是 `ToolsPayload`，由 `tool-started`、`tool-output-delta`、`tool-finished`、`tool-error` 四种按 `event` 键区分。

**两级判别。** 外层靠 `part["type"]`，内层（tasks / debug / tools）靠负载自己的判别键。这是有意的：外层统一让消费者写一个 switch，内层保留各自最自然的形态，而不强行把 debug 与 tool 压成同一套字段。

**消息的增量形态。** `MessagesStreamPart` 的 `data` 里的消息实际常是 `AIMessageChunk`（增量块），但类型上写的是 `AnyMessage` 并在 docstring 里点名。这是刻意的松：同一位置既可能来增量块也可能来完整的 `BaseMessage`（节点返回最终消息时）。

## 关键取舍

**源码事实：** `StreamPart` 是 TypedDict 联合，不是 dataclass 或带方法的类。因此它在运行期就是普通 dict，序列化零成本，`part["type"] == "values"` 这种判断在任何消费端都能直接做。

**源码事实：** `tasks` 与 `debug` 的负载没有共用一套 schema 而是各带判别键。debug 的条目把 `TaskPayload` 原样包在 `payload` 里再补 `step`/`timestamp`，所以同一份任务信息在两种 mode 下形状不同——不是重复定义，而是两种用途。

**源码事实：** `MessagesStreamPart` 的类型标注比实际取值更宽。好处是「增量与完整消息都能装」；代价是消费者必须自己做类型判断，不能指望类型检查器帮他区分。

**架构推论：** 判别键与 stream mode 同构，使协议层不需要额外映射表：加一个 mode 就要加一个 part 类型、一个转换器、一个 `required_stream_modes` 声明，四处必须同步。这条约束让「悄悄加一种流」变得不可能。

**架构推论：** `ns` 用元组直传而不解析成对象，是把「解析成本」推给真正需要它的少数消费者（比如子图导航），而不是每个事件都付。绝大多数消费者只做一次相等比较。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/types.py` | `StreamMode` | stream mode 的字面量集合，与 part 的判别键同构 |
| `libs/langgraph/langgraph/types.py` | `StreamPart` | 全部流事件类型的判别联合 |
| `libs/langgraph/langgraph/types.py` | `ValuesStreamPart` | values 事件：整份状态加本次中断负载 |
| `libs/langgraph/langgraph/types.py` | `UpdatesStreamPart` | updates 事件：节点名到更新的映射 |
| `libs/langgraph/langgraph/types.py` | `MessagesStreamPart` | messages 事件：消息与元数据的二元组 |
| `libs/langgraph/langgraph/types.py` | `CheckpointStreamPart` | checkpoints 事件：检查点负载 |
| `libs/langgraph/langgraph/types.py` | `TasksStreamPart` | tasks 事件：开始或结果负载 |
| `libs/langgraph/langgraph/types.py` | `DebugStreamPart` | debug 事件：带 step 与时间戳的调试条目 |
| `libs/langgraph/langgraph/types.py` | `ToolsStreamPart` | tools 事件：工具调用四态负载 |
| `libs/langgraph/langgraph/stream/_convert.py` | `convert_to_protocol_event` | 把 v2 part 改写成统一协议事件信封 |

## 参考

- [LangGraph 源码：types.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/types.py)
- [LangGraph 源码：stream/_convert.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/_convert.py)
- [LangGraph 文档：流式输出](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

「用判别键做封闭联合」在主核里对应的是各类记录头部的类型字节：读端先看一个字节决定后面怎么解析，而不是靠上下文猜。StreamPart 是同一套思路在 Python 类型层的实现——判别键既是运行期分支依据，也是静态收窄依据。

对 AI 侧的实际意义在于跨进程边界：这些 part 就是服务端推给客户端的东西，形状一旦固定，SDK 与前端就能各自生成类型而不是手写解析。要注意的是 `MessagesStreamPart.data` 里消息的宽类型——写消费端代码时不能省掉「这是增量还是完整消息」的判断。
