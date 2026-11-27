---
project: pydantic-ai
feature: run-and-result
title: 运行对象与结果契约
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/run.py
    symbols: [AgentRun, AgentRunResult, AgentRunResultEvent]
  - path: pydantic_ai_slim/pydantic_ai/result.py
    symbols: [StreamedRunResult, AgentStream, FinalResult]
---

# 运行对象与结果契约

## 一句话职责

一次运行在内存里有两个形态：**活着的** `AgentRun`（包着图运行器、可逐节点推进、可中途注入消息）与**已结算的** `AgentRunResult`（值对象、可序列化、可落库）；流式场景多一个中间的 `AgentStream`，以及它的对外包装 `StreamedRunResult`。

## 机制是什么

按「生命周期」而不是「类型」来理解这四个类会更清楚：

- `AgentRun` 是**活体**。它持有 `GraphRun`、状态、依赖与取消控制器，靠 `__anext__` / `next(node)` 推进。活体才会长出新消息、累加用量、接受 `enqueue`。
- `AgentRunResult` 是**结算后的快照**。它只保留输出、消息历史、新消息下标、用量、两端 id 与元数据；有 `model_serializer` / `model_validator`，能被 Pydantic 存读。
- `AgentStream` 是**一次模型响应的流**。它把「原始流 + 输出 schema + 校验器 + 工具管理器 + 根能力」绑在一起，对外提供 `stream_text` / `stream_response` / `stream_output` 三种消费方式。
- `StreamedRunResult` 是**流式运行的对外结果**。它有两个初始化路径：要么托着一个 `AgentStream`（读时现取），要么直接托着一个已结算的 `AgentRunResult`（例如 `wrap_run` 短路时没有流可读）。

`FinalResult` 不属于上面任何一个，它是**图终止节点负载**：包着 `output`、产出该输出的工具名与工具调用 id。图以 `End(data=FinalResult(...))` 收尾，`AgentRun.result` 再把 `FinalResult` 连同图状态装配成 `AgentRunResult`。

## 怎么实现的

**`AgentRun` 的结构。** 三个字段：`_graph_run`（`GraphRun`）、`_result_override`（可被能力钩子替换的结果）、`_node_error`（**节点执行的原始异常**，在上下文管理器 `__aexit__` 可能改写异常之前存下来）、`_last_yielded_node`（最近 yield 的节点，下一次迭代要跑的就是它）。

`ctx` 属性是个巧妙的桥：它不去缓存，而是每次 `GraphRunContext(state=self._graph_run.state, deps=self._graph_run.deps)` 现造一个——所以 `ctx.state` 与图的实时状态始终是同一对象，`all_messages()` 直接返回 `self.ctx.state.message_history`。

`next_node` 把图运行器的 `next_task` 转成节点：`_task_to_node` 只在「恰好一个任务且其 `inputs` 是 `BaseNode` 且 `is_agent_node` 为真」时返回那个节点；`EndMarker` 转成 `End(task.value)`；其他形状抛 `AgentRunError`。注意它遇到 `ErrorMarker` 会**直接把错误抛出来**——这是 `next_node` 的属性语义。

`_task_to_node` / `_node_to_task` 是一对互逆映射。`_node_to_task(node)` 造的是 `GraphTaskRequest(NodeStep(type(node)).id, inputs=node, fork_stack=())`——**用节点类的 `NodeStep` id 加上节点实例作为输入**，这就是「把 `BaseNode.run` 塞进 builder 图」的接合方式。

`__anext__` 的推进逻辑分三支，每一支都对应一个真实边界：

1. `previous is None`（第一个节点还没跑）：`node = self.next_node`，把它 yield 出去让调用方**先看到再执行**。
2. `next_node` 与 `previous` 不同：说明循环体自己已经推进了图（例如在刚 yield 的节点上调了 `next()`），此时**不能重跑 `previous`**（会重复执行它与它的钩子），直接返回当前节点。
3. 其余：`node = await self.next(previous)`。

第一支之前还有两道取消检查：`self.ctx.deps.cancellation.bind()`，若 `cancel_requested` 则 `await anyio.lowlevel.checkpoint()`——注释解释了为什么不用别的手段：第一次 yield 发生在节点执行之前，这个边界上没有可等待的步骤来承载挂起的 `task.cancel()`，所以显式让出一次调度器。

`next(node)` 本身只有一行有效逻辑：`await self._run_node_with_hooks(node, self._stream_and_advance)`。注释里留了一句诚实的说明：本来想提供同步迭代接口，但不放在这个类上，否则 IDE 就不会在误用 `for`（而非 `async for`）时发出警告。

**`AgentRun.result` 的装配。** 它先看 `_result_override`（能力钩子可能已给了结果），否则读 `self._graph_run.output`；非 `None` 时构造 `AgentRunResult(output, tool_name, state, deps.new_message_index, traceparent)`。最后一行值得单说：

```
result.__dict__['_workspace'] = self._graph_run.deps.workspace
```

注释给了两个理由：`_workspace` **不是** dataclass 字段，因为 Temporal 会序列化结果的所有 dataclass 字段，而活的工作区无法序列化；并且它**永远取当前 run 的**——`after_run` 钩子可能返回来自另一次 run 的结果。用 `__dict__` 直写既绕过了字段校验也绕过了序列化。

**`AgentRunResult` 的可序列化设计。** 它的公开形状是 `output`、`output_tool_name`、`messages`、`new_message_index`、`usage`、`run_id`、`conversation_id`、`metadata`、`traceparent`，而内部字段全是下划线私有（`_state`、`_new_message_index`、`_traceparent_value`、`_output_tool_name`）。转接靠两个钩子：

- `_validate_serialized`（`model_validator(mode='before')`）：接受**公开形状**与**旧版私有形状**（`_state` 存在时先走 `_from_legacy_shape`），把 `messages` 映射回 `message_history`，把散落的键收进 `state_data`，最后组装成私有字段名——注释说这是为了「让 `messages`/`usage`/`run_id`/`conversation_id`/`metadata` 落回 `_state`」。
- `_serialize`（`model_serializer(mode='wrap')`）：手工拼 `_AgentRunResultData`。它先把 `handler(AgentRunResult(output=self.output))` 交给 dataclass 序列化器，原因在注释里：`output` 的类型由泛型参数决定，只有 `handler` 包着的那份知道实际类型，序列化器自己的返回注解不带参数化。

`__get_pydantic_json_schema__` 也必须存在：不加的话，一个 `AgentRunResult` 出现在 API 请求模型里会把 `_state` 及其引用全部暴露；而 `__get_pydantic_core_schema__` 对参数化泛型 dataclass 不生效，所以只能在这个钩子里改。

`__getstate__` 把 `_workspace` 从 dict 里摘掉，注释说 DBOS 会 pickle 工作流结果，活的工作区可能带密钥（`LocalWorkspace(env=)`）且在另一个进程里毫无意义，ref 已经存在消息的 `workspace_ref` 里。

**`AgentRunResultEvent`。** 一个极薄的 dataclass：`result: AgentRunResult`、`event_kind: Literal['agent_run_result'] = 'agent_run_result'`（**用作判别字段**），并复用了 `dataclasses_no_defaults_repr`。它是 `run_stream_events` 里「事件流的最后一个元素」，让调用方在同一条流上既能收到过程事件、也能收到最终结果。

**`AgentStream` 的三个消费面。**

`stream_response` 的实现揭示了一个细节：调用时若底层响应**已有累积内容但流还没结束**（`state == 'incomplete'` 且 `_model_stream_finished` 为假），会**先 yield 一次**再开始迭代——注释说这是「pre-stream yield 把已有内容先露出来」。迭代本身用 `_utils.group_by_temporal(self._model_response_events(), debounce_by)` 做时间窗聚合，每次 yield 的都是 `self.response` 这个**活的快照对象**，最后再 yield 一次终态（`'complete'` / `'interrupted'` / 中途被切断时仍 `'incomplete'`）。

`stream_text` 有三个前置/后置约束：非 `TextOutputSchema` 时直接 `UserError`；已缓存输出时**直接 yield 缓存并返回**（注释承认：即使用 `delta=True`，后续调用也会拿到完整文本而不是增量）；`delta=False` 时每个聚合后的文本都要过 `_output_validators`，且传入的是 `replace(self._run_ctx, partial_output=True)`——**校验器知道自己在看部分输出**。

`stream_output` 的写法更细：迭代中遇到与上一份响应 `parts` 相同的快照就 `continue`（避免重复校验），`validate_response_output(allow_partial=True)` 抛 `ValidationError` 或 `ModelRetry` 都**静默跳过**；流结束后再做一次 `allow_partial=False` 的最终校验并**无论内容是否与上一次相同都 yield**——注释给了两条理由：输出校验器只在最后一次收到 `partial_output=False`，且用户可以依赖「最后 yield 的一定是完全校验过的输出」。

`cancel()` 只停**当前这次模型响应**，不结束整次 run；`usage` 的计算用 `__post_init__` 里 `_refresh_initial_run_ctx_usage` 拍的快照（`deepcopy(self._run_ctx.usage)` 且 `requests += 1`，因为正在流的这一步要到响应提交后才计入 run 用量）。

**`StreamedRunResult` 的两种初始化与「结算」。** 它有两个 `@overload` 的构造：传 `stream_response` + `on_complete`，或传 `run_result`。核心是 `result` 属性：已有 `_run_result` 或 `_settled` 就直接返回；否则要求 `is_complete` 为真（否则 `UserError`「还在流，没有结算结果」）、且 `_completion_raised` 为假（否则 `UserError`「run 提前停了，没有结算结果」），然后从 `_stream_response._settled_output()` 取输出与工具名，用 `replace(state, message_history=..., usage=..., run_id=..., ...)` 造一个 `AgentRunResult` 并缓存到 `_settled`。

三个字段的原因都写在注释里：`_settled` 让「存到 store 后每次 `result` 都返回同一个对象」；`_settled_from` 记录「流自己的响应结束了整次 run 时」用于重建完整历史的起点；`_traceparent_value` 在流结束时**趁 span 还开着**抓下来，因为结算结果的用途正是「交给活得更久的代码」，那时 span 已关、环境追踪上下文早没了。

`_record_response` 把响应追加进 `_all_messages` 前会**回填 `run_id` 与 `conversation_id`**，保证落库的历史没有缺字段的消息。

`_marked_completed` 是「改成完成」的唯一入口：先查幂等，置 `is_complete`，抓 traceparent，再调 `_on_complete`；`_on_complete` 抛任何 `BaseException` 都会置 `_completion_raised` 并重抛——注释解释了为什么用 `BaseException`：**被取消的 run 同样没有「完成」**。

**`FinalResult` 与 `End` 的关系。** `FinalResult` 有三个字段：`output`、`tool_name`（产出输出的工具名，纯文本输出时为 `None`）、`tool_call_id`（同理）。它被包在 `End` 里作为图的终止负载，这也解释了 `AgentRun._task_to_node` 里 `EndMarker` → `End(task.value)` 的那一步：图运行器的 `EndMarker` 与 `BaseNode` 契约的 `End` 是同一件事的两种表示。

## 关键取舍

**源码事实：** 活体 `AgentRun` 与结算快照 `AgentRunResult` 是分开的两类，且活体**不可序列化**（含图运行器与活的工作区）。这限制了「中途把 run 存下来再恢复」的可能，除非用耐久执行那套机制。

**源码事实：** `_workspace` 用 `__dict__` 直写而不是字段，就为了让 Temporal 的 dataclass 序列化跳过它。这是「一个框架的序列化约定倒过来约束数据结构」的典型。

**源码事实：** `AgentRunResult` 的公开序列化形状与内部字段名不同，且必须额外实现 `__get_pydantic_json_schema__` 才能不把 `_state` 暴露给 API 模型。

**源码事实：** `stream_text` 在已有缓存时直接 yield 完整文本，即使 `delta=True`。增量语义只在首次消费时成立。

**源码事实：** `stream_output` 对中间快照的校验失败**静默跳过**，只在最后做一次不可跳过的校验。这把「流式过程中的不合法中间态」当成常态而非错误。

**源码事实：** `StreamedRunResult.result` 在三种情况下抛 `UserError`：流未结束、run 提前停止、没有流也没有 run_result。也就是说「结算」是有前置条件的，不是随时可读。

**架构推论：** 把「结果」设计成**可序列化的值对象**而不是继续引用运行状态，使结果能跨进程/跨存储传递（`conversation`、DBOS、Temporal 都要这个）。代价是需要一层手工的名字映射（公开形状 ↔ 私有字段），且这层映射必须向后兼容旧形状。

**架构推论：** `FinalResult` 与 `End` 的组合让「终止」既携带数据又携带**产出方式**（哪个工具、哪个调用 id）。这对后续把结果写回历史、或判断 `end_strategy` 的处理结果都是必需的；纯文本输出时两者皆 `None`，这个可空性本身就是「有没有走工具」的判别位。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRun` | 活体运行：逐节点推进、`ctx` 桥接、取消检查、`enqueue` 注入 |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRun.__anext__` | 三分支推进逻辑与「先 yield 首节点再执行」 |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRun._task_to_node` | `GraphTask` ↔ `BaseNode` 的双向映射接合点 |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRun.result` | 把图输出装配成 `AgentRunResult`，`_workspace` 直写 `__dict__` |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRunResult` | 结算快照：私有字段 + 公开序列化形状 |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRunResult._validate_serialized` | 接受公开形状与旧版私有形状，兼容迁移 |
| `pydantic_ai_slim/pydantic_ai/run.py` | `AgentRunResultEvent` | 事件流的末元素，`event_kind` 作判别字段 |
| `pydantic_ai_slim/pydantic_ai/result.py` | `AgentStream` | 单次模型响应的流：三个消费面与缓存/校验策略 |
| `pydantic_ai_slim/pydantic_ai/result.py` | `AgentStream.stream_output` | 中间快照静默跳过、末尾一次强制完整校验 |
| `pydantic_ai_slim/pydantic_ai/result.py` | `StreamedRunResult` | 流式对外结果，两种初始化与延迟结算 |
| `pydantic_ai_slim/pydantic_ai/result.py` | `StreamedRunResult.result` | 结算前置条件与 `_settled` 缓存 |
| `pydantic_ai_slim/pydantic_ai/result.py` | `FinalResult` | 图终止负载：输出 + 工具名 + 工具调用 id |

## 参考

- [pydantic-ai 源码：run.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/run.py)
- [pydantic-ai 源码：result.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/result.py)
- [Pydantic AI 文档：运行与结果](https://ai.pydantic.dev/agent/#running-agents)
- [Pydantic AI 文档：流式输出](https://ai.pydantic.dev/agent/#streaming)

## 与主核 / AI 的关系

「活体对象 vs 结算快照」这条分界，在主核里有直接对应物：游标/事务是活体（持有资源、只能同进程用），结果集摘要或落盘结果是快照（可传、可存、可重建）。主核之所以很少需要「同一份数据两种形态」，是因为它的资源生命周期由连接池统一管理；而这里 run 的活体会被 `async with` 与流式消费拉得很长，中途还有钩子可能提前返回结果，于是必须有一个明确的结算点。

对 AI 工程的实际意义在于：**能否把一次运行的产物存下来，取决于结果里有没有活对象**。`AgentRunResult` 费那么大力气做「私有字段 ↔ 公开形状」的映射、还要实现 `__get_pydantic_json_schema__`、把工作区从 pickle 里摘掉，本质都是在保证「结算结果是一份干净的数据」。反过来，`StreamedRunResult.result` 的三个 `UserError` 条件说明了一个容易忽略的事实：流式运行在没有消费完之前，并不存在一个可存储的结果——想存就必须先消费完，否则只能存到消息历史那一步。
