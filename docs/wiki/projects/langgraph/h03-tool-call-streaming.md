---
project: langgraph
feature: tool-call-streaming
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_tools.py
    symbols: [StreamToolCallHandler, on_tool_start, tap_output_iter]
---

# 工具调用流处理

## 一句话职责

StreamToolCallHandler 把 LangChain 的工具回调翻译成 `tools` 流模式下的四态事件，同时用一个 ContextVar 让工具实体能主动吐增量而不必改自己的签名。

## 机制是什么

工具执行的细节只有回调层看得见。这个 handler 挂在回调上，把每次工具调用的开始、增量、结束、失败转成 `(ns, "tools", payload)` 三元组，交给上游的 stream 回调入队。四种 payload 靠 `event` 键区分：`tool-started`（带 id、name、input）、`tool-output-delta`（带 id 与 delta）、`tool-finished`（带 id 与 output）、`tool-error`（带 id 与 message）。

同时它在 `on_tool_start` 里往一个 `ContextVar` 塞一个闭包，闭包绑定住当前调用的命名空间与 `tool_call_id`。工具实现只要调 `ToolRuntime.emit_output_delta(delta)`，就会读到这个 ContextVar 并发出增量事件——调用方不需要把 writer 一层层传进去。

## 怎么实现的

**命名空间的解析。** `_ns_for_emit` 复刻了消息流 handler 的判断：metadata 缺失就跳过（说明这个上下文里没有 Pregel 路由信息）；标签里有 `TAG_NOSTREAM` 就跳过（调用方显式要求不流）；工具跑在子图里（`len(ns) > 0`）且 `subgraphs=False` 且这个命名空间不等于 `parent_ns` 就跳过。最后一条是逃生口：节点自己显式用 `stream_mode="tools"` 流某个子图时，该子图里的工具仍然要能发事件。

`ns` 本身由 `graph_namespace(metadata)` 算出，它会剥掉任务自己那一段，以及每一层 `@task` 调用嵌套的段。

**以 run_id 做关联键。** `on_tool_end` 的回调参数里没有 `tool_call_id`，所以必须在 `on_tool_start` 时按 `run_id` 记下 `(ns, tool_call_id, ContextVar token)`，结束时再按同一个 `run_id` 取回。`tool_call_id` 优先取 `kwargs` 里的值，取不到就退回 `str(run_id)`——保证每个事件都有关联键，即使调用方没给 id。

**ContextVar 的复位。** `_reset_writer` 用 token 复位，并且吞掉 `ValueError`。注释解释了原因：`on_tool_end` 可能在另一个上下文里跑（比如 langchain 把工作交给线程 worker 而没复制上下文），此时 token 已失效。不复位也不会泄漏——ContextVar 的生命周期本来就被外层任务框住。

**中断不是失败。** `_error` 在把错误转成 `tool-error` 之前先判 `isinstance(error, (GraphInterrupt, GraphDrained))` 并直接返回。理由写在注释里：被中断或 drain 的工具是**暂停**而不是失败，恢复时它会重新执行并重新报开始与结束。若这里发一条 `tool-error`，前端会在等待人类回答时看到一条假错误。

**空实现的 tap。** `tap_output_iter` / `tap_output_aiter` 是纯透传。它们是 `_StreamingCallbackHandler` 协议要求的方法，但工具输出的流式化不走这里——这里只负责旁路发事件，不劫持输出流本身。

**内联执行。** `run_inline = True`。若走线程池派发回调，事件的相对顺序会与真实执行顺序脱节。

**挂载条件。** `Pregel` 只在 `stream_modes` 里含 `"tools"` 时才把这个 handler 加进回调列表，同步与异步两条路径各一处。所以不请求 `tools` 模式时，工具调用完全没有这层开销。

## 关键取舍

**源码事实：** `tool_call_id` 缺失时退化成 `run_id`。这是一个「宁可有个能关联的键，也不发无主事件」的决定，代价是前端必须容忍两种 id 空间混在同一个字段里。

**源码事实：** handler 不劫持工具的输出流。`tap_output_*` 全部透传，意味着工具的输出只有被显式 `emit_output_delta` 或作为最终结果时才进流。一个边跑边 print 的工具不会自动产生增量事件。

**源码事实：** 中断/暂停被排除在错误之外。这要求前端理解「开始了但没有结束」是合法中间态，而不是协议违例。

**架构推论：** 用 ContextVar 做注入而不是给工具加参数，使「流式输出」成为工具可选的能力：不调就没开销，调了也不必改函数签名。代价是隐式依赖上下文——脱离图执行的工具（比如单测里直接调）读不到 writer，`emit_output_delta` 变成空操作。`prebuilt` 里的实现正是这么做的：注释说可以放心留着调用。

**架构推论：** 事件粒度是「一次工具调用」，不是「一次工具内部的进度」。因此四态模型天然支持并发多工具——每个 id 一条独立轨迹，不需要额外的会话概念。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_tools.py` | `StreamToolCallHandler` | 把工具回调翻译成 `tools` 流事件 |
| `libs/langgraph/langgraph/pregel/_tools.py` | `on_tool_start` | 解析命名空间、装 writer 闭包、发 `tool-started` |
| `libs/langgraph/langgraph/pregel/_tools.py` | `tap_output_iter` | 同步透传，满足流式回调协议但不动输出 |
| `libs/langgraph/langgraph/pregel/_tools.py` | `_ns_for_emit` | 决定这次调用该在哪层发、或干脆不发 |
| `libs/langgraph/langgraph/pregel/_tools.py` | `_tool_call_writer` | ContextVar，承载当前调用绑定的增量写入闭包 |
| `libs/langgraph/langgraph/pregel/_messages.py` | `graph_namespace` | 从元数据算出任务所属图的命名空间 |
| `libs/langgraph/langgraph/pregel/main.py` | `stream` | 仅当 `stream_modes` 含 `tools` 时挂载该 handler |

## 参考

- [LangGraph 源码：pregel/_tools.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_tools.py)
- [LangGraph 源码：pregel/_messages.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_messages.py)
- [LangGraph 文档：流式输出](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

「回调旁路发事件 + ContextVar 注入写入口」与主核里「在关键路径埋观测点、但不改变主路径返回值」是同一个路数：观测是附加的，不能因为有人看日志就改变执行语义。主核用 tracepoint，这里用 LangChain 回调。

对 AI 侧的实际影响是前端体验：`tool-started` 立刻能让界面显示「正在调用某工具」，`tool-output-delta` 让长任务有进度，`tool-finished` 带上结果。要做到最后一条，工具实现必须显式调 `emit_output_delta`——这也是最容易漏的一步，因为不调也不会报错，只是界面一直安静到结束。
