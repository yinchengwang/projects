---
project: openai-agents
feature: tracing-scope
title: 追踪作用域与父子挂载
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tracing/scope.py
    symbols: [Scope]
  - path: src/agents/tracing/create.py
    symbols: [agent_span, trace]
---

# 追踪作用域与父子挂载

## 一句话职责

用两个 ContextVar 记住「当前 trace 与当前 span」，再让所有 `create_*` 帮助函数在创建时按「显式 parent 优先、否则取当前作用域」推导父关系——于是嵌套结构由调用栈自动决定，而不是靠每个调用点手工传 id。

## 机制是什么

`Scope` 只有六个类方法和两个模块级 ContextVar：`_current_span` 与 `_current_trace`，默认都是 `None`。三组方法是 `get_current_*`、`set_current_*`（返回 `contextvars.Token`）、`reset_current_*`（按 token 还原）。`set_current_trace` 与 `reset_current_trace` 各打一条 debug 日志。

ContextVar 的选择是关键：它随 asyncio 任务与线程复制，所以并发的两个 trace 各自看到自己的作用域，不需要加锁也不需要显式传参。

`create.py` 里那十几个帮助函数（`trace`、`agent_span`、`task_span`、`turn_span`、`function_span`、`generation_span`、`response_span`、`handoff_span`、`custom_span`、`guardrail_span`、`transcription_span`、`speech_span`、`speech_group_span`、`mcp_tools_span`）以及 `get_current_trace` / `get_current_span` 两个读取入口，全部只是**转发到 provider**：`get_trace_provider().create_span(span_data=..., span_id=..., parent=..., disabled=...)`。所以真正的父子推导在 provider 里，而不在这些函数里。

## 怎么实现的

**trace 创建前的冲突警告。** `trace()` 先 `get_trace_provider().get_current_trace()`，若已有活动 trace 就打一条 warning：「已存在 trace，正在创建新的，但这大概是错误」。然后转给 provider 的 `create_trace`。所以嵌套 trace 是被允许的（内层会覆盖 current），但有一条提示。

**parent 参数的三态。** 每个创建函数都有 `parent: Trace | Span | None = None`。`None` 表示「自动」——用当前作用域；传 `Trace` 表示「挂在某个 trace 的顶层」；传 `Span` 表示「挂在这个 span 下面」。三种形态的存在让调用方可以跳出当前嵌套，把 span 挂到别处（例如把异步任务的 span 挂回它的发起者）。

**provider 侧的推导顺序。** `DefaultTraceProvider.create_span` 里的判断链是这一页的核心，共五步：

1. 先 `_refresh_disabled_flag()`，若全局关闭或本次 `disabled=True`，返回 `NoOpSpan(span_data)`——**注意仍然返回一个可用对象**，只是不记录。
2. 若传入的 `span_id` 本身就是 `"no-op"`，也返回 `NoOpSpan`。
3. `parent is None` 时读当前作用域：没有当前 trace 就打一条「请先用 `trace()` 启动」的提示并返回 `NoOpSpan`；当前 trace 或当前 span 是 no-op 时同样返回 `NoOpSpan`。都不是，则 `parent_id = current_span.span_id`（当前 span 可能为 `None`），`trace_id` 与 `tracing_api_key` 从当前 trace 取，`trace_metadata` 从 `getattr(current_trace, "metadata", None)` 取。
4. `parent` 是 `Trace` 时：no-op 判断同上；否则 `trace_id = parent.trace_id`、`parent_id = None`（挂在 trace 顶层）、key 与 metadata 从 parent 取，metadata 走 `getattr(parent, "metadata", None)`。
5. `parent` 是 `Span` 时：no-op 判断同上；否则 `parent_id = parent.span_id`、`trace_id = parent.trace_id`、key 从 parent、metadata 走 `parent.trace_metadata`。

第 3、4 步取 metadata 都用 `getattr(..., None)` 而不是直接属性访问，注释说明了原因：「Trace 是接口，自定义实现可能不带 metadata」。这是对「抽象基类被第三方实现」的实际让步。

**no-op 的传染性判断有三个来源。** `_is_noop_span` 既检查 `isinstance(span, NoOpSpan)`，也检查 `span_id` 或 `trace_id` 等于 `"no-op"`；`_is_noop_trace` 同理只看 `trace_id`。为什么不能只靠 isinstance？因为可能有别的实现也用 `"no-op"` 作为占位 id——用值判断能覆盖这种情况。所以「关闭追踪」这个状态在父子链上会**向下传染**：父是 no-op，子也必须是 no-op，否则会出现「有 trace 记录但父不存在」的悬空 span。

**trace_metadata 的向下传递。** 从 span 创建子 span 时取的是 `parent.trace_metadata`（父 span 转存的 trace 元数据），而不是去查当前 trace。这样即使 `parent` 是显式传入的、与当前作用域无关，metadata 也跟着 parent 走而不是跟着上下文走。

**no-op 判定的日志分级。** 多处写成「若 `DONT_LOG_MODEL_DATA` 或 `DONT_LOG_TOOL_DATA` 为真就打不带 span_data 的日志，否则打带 span_data 的」。所以敏感数据开关也管到了追踪日志这一层——避免在调试日志里泄漏模型或工具数据。

**关闭状态的两级来源。** `_refresh_disabled_flag` 合并两个输入：环境变量 `OPENAI_AGENTS_DISABLE_TRACING`（接受 `"true"` 与 `"1"`，大小写不敏感）与手动 `set_disabled`。规则写在注释里：环境变量**只在首次使用时读一次**并缓存（`_env_disabled`），避免运行中途变化带来意外；而一旦手动设置过（`_manual_disabled` 非 `None`），就**永远优先于环境值**。

**多处理器的顺序与超时分摊。** `SynchronousMultiTracingProcessor` 用元组保存处理器列表（注释：「用元组避免迭代时的竞态」），加锁追加或整体替换。四个回调都按注册顺序逐个转发，每个回调用 `try/except Exception` 包住并记日志——一个处理器出错不影响其他处理器。

`shutdown(timeout=None)` 做得更细：先算出总 deadline，然后每轮用 `_remaining_timeout(deadline)` 算剩余时间；若已耗尽就打一条 `[non-fatal]` 警告并**直接 return**（放弃剩余处理器）；若该处理器接受 `timeout` 参数（由 `_supports_shutdown_timeout` 用 `inspect.signature` 判断，`**kwargs` 也算），就把剩余时间传进去，否则无参调用。所以超时是**总预算**而不是每个处理器各一份。

**关闭期的日志容错。** `_safe_debug` 先检查 logger 链上是否有 handler 的 stream 已关闭（遍历 handler 与 parent，遇到 `propagate=False` 停止），有就完全跳过输出。整个函数包在 `try/except Exception` 里。注释说这是「容忍关闭期间已关闭的流」。这是解释器退出阶段的实际问题——大量「I/O operation on closed file」噪音来源于此。

**provider 抽象里有一条默认的保守值。** `TraceProvider.is_disabled()` 的默认实现直接 `return True`。注释解释了原因：「旧的自定义 provider 在没有覆盖这个查询方法前，服务端追踪会保持关闭」——也就是说新增这个方法时不能假设第三方实现会同步更新，默认必须取最安全的值（当作关闭）。

## 关键取舍

**源码事实：** 没有当前 trace 时创建 span 会返回 `NoOpSpan` 并只打一条提示，不抛异常。所以「忘了开 trace」不会炸，只是没有数据。

**源码事实：** 关闭状态会向下传染：父是 no-op 时子必然也是 no-op。这避免了悬空 span，代价是「想在关闭状态下单独记录一个 span」做不到。

**源码事实：** 环境变量只读一次并缓存，之后不再响应变化。所以运行期改环境变量不会生效——这是为「同一进程行为稳定」付的代价。

**源码事实：** `Scope` 的 set 返回 token、reset 需要 token，所以「还原成上一层」严格依赖调用顺序。若两次 set 的 token 被以错误顺序 reset，ContextVar 会抛 `ValueError`（token 只能按后进先出顺序重置）。

**架构推论：** 「作用域在 ContextVar 里 + 创建时读作用域推导父」这套组合，让嵌套结构完全由调用栈决定。任何 `with agent_span(...)` 内部的创建都自动挂在它下面，不需要把 id 一层层传下去。代价是**跨任务的挂载会丢**：`asyncio.create_task` 复制上下文，但若任务在新上下文里跑完再回来，父链的推导仍发生在任务自己的上下文里，因此需要显式传 `parent`。

**架构推论：** 三种 parent 形态（None / Trace / Span）中，「显式传 Span」是唯一能跨越上下文的做法。这也解释了为什么每个创建函数都保留这个参数——它不是冗余的便利功能，而是异步场景下的必要出口。

**架构推论：** no-op 传播与「关闭时不创建对象」是两件事。这里选了「创建占位对象」，因为调用方总会写 `with agent_span(...) as span:`，若返回 `None` 就会在 `with` 里炸。返回一个 `__enter__` 返回自身、`export()` 返回 `None` 的哑对象，是让关闭路径与开启路径共用同一份业务代码的唯一办法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tracing/scope.py` | `Scope` | 两个 ContextVar 的读、设、按 token 还原 |
| `src/agents/tracing/scope.py` | `Scope.set_current_span` | 设当前 span 并返回还原 token |
| `src/agents/tracing/scope.py` | `Scope.set_current_trace` | 设当前 trace 并返回还原 token |
| `src/agents/tracing/create.py` | `trace` | 创建 trace，已有活动 trace 时警告 |
| `src/agents/tracing/create.py` | `agent_span` | 创建 agent span，转发 parent 与 disabled |
| `src/agents/tracing/create.py` | `generation_span` | 创建生成 span，带模型、配置、工具与用量 |
| `src/agents/tracing/create.py` | `get_current_trace` / `get_current_span` | 读取当前作用域的公共入口 |
| `src/agents/tracing/provider.py` | `DefaultTraceProvider.create_span` | 五步父子推导与 no-op 传染 |
| `src/agents/tracing/provider.py` | `DefaultTraceProvider._refresh_disabled_flag` | 环境变量缓存与手动覆盖优先 |
| `src/agents/tracing/provider.py` | `SynchronousMultiTracingProcessor.shutdown` | 总超时预算按剩余时间分摊 |
| `src/agents/tracing/provider.py` | `_supports_shutdown_timeout` | 探测处理器是否接受 timeout 参数 |
| `src/agents/tracing/provider.py` | `_safe_debug` | 关闭期跳过已关闭 stream 的日志 |

## 参考

- [openai-agents-python 源码：tracing/scope.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/scope.py)
- [openai-agents-python 源码：tracing/create.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/create.py)
- [openai-agents-python 源码：tracing/provider.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/provider.py)

## 与主核 / AI 的关系

主核里也有「当前上下文」这类隐式状态，但它必须显式保存与恢复（因为 C 里没有 ContextVar 那种自动随执行流复制的能力），且跨线程时必须手工传递。这里用 ContextVar 省掉了全部样板，代价是「跨任务自动隔离」同时意味着「跨任务不会自动继承父关系」——所以显式 `parent` 参数仍然必要。

与 CrewAI 的事件上下文栈相比，两边都是「用一个栈/变量记住当前作用域」，但目的不同：CrewAI 的栈用来给**已发生的事件**补父关系，事后可重建图；这里的变量用来给**正在创建的 span** 推导父 id，是创建期的一次性决策。前者是历史事实的记录，后者是当下状态的读取。

对 AI 工程最实用的一条是「no-op 占位对象」的必要性。追踪关闭时返回 `None` 看似更干净，但会让所有 `with span(...)` 的调用点都需要判空——于是业务代码里布满防御分支，且一旦漏判就在关闭追踪时崩掉。返回一个哑实现让两条路径共用同一份代码，这个做法在所有「可选的可观测性」场景里都适用。

另一条是环境变量只读一次并缓存的取舍。热更新看起来更灵活，但在追踪这种横切设施上，运行中期行为变化会让「为什么这段时间没有数据」变得极难解释。把配置固定为「首次使用时的快照」，用可解释性换灵活性，通常是更划算的交易。
