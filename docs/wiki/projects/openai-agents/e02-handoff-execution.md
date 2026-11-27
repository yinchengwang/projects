---
project: openai-agents
feature: handoff-execution
title: 交接在循环中的识别与切换
page_type: feature
schema_version: 1
refs:
  - path: src/agents/items.py
    symbols: [HandoffCallItem, HandoffOutputItem]
  - path: src/agents/run_internal/run_loop.py
    symbols: [_publish_resumed_stream_turn, _complete_stream_interruption]
---

# 交接在循环中的识别与切换

## 一句话职责

在一轮模型响应里认出「哪个函数调用其实是交接」，把它变成两个可追踪的运行项（调用项与输出项），然后把 `current_agent` 换掉、关掉旧 agent 的 span、开新 agent 的 span、发一次切换事件，继续下一轮。

## 机制是什么

交接在运行期只表现为三种东西：

- `HandoffCallItem`：模型发出的那次 `function_call`，标记为「这是交接而非工具」。
- `HandoffOutputItem`：交接结果项，除原始输入项外还携带 `source_agent` 与 `target_agent` 两个弱引用。
- `NextStepHandoff`：单步结果的一种 `next_step`，只带一个 `new_agent` 字段。

识别发生在解析模型响应时：`is_handoff_tool_call(output, handoff_tool_names)` 判断一个 `ResponseFunctionToolCall` 是否指向交接。判定条件有两条：调用名必须是**裸名**（`get_tool_call_qualified_name(output) == output.name`，即没有命名空间前缀），且名字在交接映射里。文档写明「带命名空间的调用永不解析为交接」。

切换发生在 `execute_handoffs`：它接收本轮所有交接调用（`run_handoffs`），只执行第一个，其余的产出一条「Multiple handoffs detected, ignoring this one.」的工具输出。

## 怎么实现的

**为什么交接不是工具。** 解析阶段就把交接从 `function_map` 的那条路里分流出去：命中交接就不查函数工具表、不进 `functions` 列表，而是生成 `HandoffCallItem` 并放进 `run_handoffs`。这样同一批模型响应里，交接与普通工具走两条完全不同的后续处理，不会互相干扰。

**多个交接只执行第一个。** `execute_handoffs` 开头先判定 `len(run_handoffs) > 1`：若是，为第 2 个及之后的每个交接生成一条 `ToolCallOutputItem`，内容是固定的 `"Multiple handoffs detected, ignoring this one."`。这些输出项保持「每个调用都有对应输出」的不变式，同时明确告诉模型哪些没生效。之后 `actual_handoff = run_handoffs[0]`。

**执行交接的三步。** 打开 `handoff_span(from_agent=...)`；若 `_agent_ref` 还能解出目标 agent，就先把它的工具配置注册进运行时（`register_agent_tool_configuration`）；标记这次调用已执行（`_mark_tool_invocation_executed`，`invocation_role="handoff"`，与普通工具的记账区分开）。然后 `await handoff.on_invoke_handoff(ctx, tool_call.arguments)` 拿到新 agent。

**失败即不交接。** 调用抛异常时，若 `failure_error_function` 为 `None` 就原样上抛；否则给 span 挂 `Handoff invocation failed`，用 formatter 生成文案，产出一条 `ToolCallOutputItem` 追加进 `new_step_items`，返回 `NextStepRunAgain()`。注释写明「失败的调用不是一次转移，因此不跑移交钩子与过滤器」。也就是说失败路径下 `current_agent` 不变，模型会在下一轮看到「交接失败」并自行决定怎么办。

**成功后产出切换项。** 构造 `HandoffOutputItem`，其 `raw_item` 是 `ItemHelpers.tool_call_output_item(actual_handoff.tool_call, handoff.get_transfer_message(new_agent))`——注意是 `get_transfer_message` 产出的 `{"assistant": "<name>"}` 而不是交接工具本身的返回值。再追加进 `new_step_items`，若调用方提供了 `handoff_output_committer` 就立刻提交（这是给流式路径用的增量提交钩子）。

**钩子与过滤器都在成功之后。** 先并发跑两个交接钩子：运行级 `hooks.on_handoff` 与旧 agent 自己的 `agent.hooks.on_handoff`，用 `gather_with_cancel` 绑在一起。然后决定过滤器与嵌套策略：`handoff.input_filter` 优先于 `run_config.handoff_input_filter`；`handoff.nest_handoff_history` 优先于 `run_config.nest_handoff_history`（用「是否是 `None`」判断而不是真值判断，因为 `False` 是有效覆盖）。

**服务端会话的硬约束。** `_resolve_server_managed_handoff_behavior`：若服务端在管会话，有 `input_filter` 就 `UserError`（连 `run_config` 级的也算）；`nest_handoff_history` 为真则发 warning 并强制关掉，降级为「仅 delta 输入」。这类约束放在执行期而不是构造期，是因为「是否服务端管理会话」是运行参数。

**构造过滤输入。** 只在「有过滤器或有嵌套策略」时才构造 `HandoffInputData`。`input_history` 若是列表就转成 tuple，`pre_handoff_items` 与 `new_items` 都取 tuple 快照，`run_context` 带上当前上下文。字段类型用 tuple 而非 list，是为了让过滤器无法就地修改原列表——要改必须 `clone`。

**过滤器的两道校验。** 调用前检查 `callable(input_filter)`，不是则给 span 挂 `Invalid input filter` 并抛 `UserError`。调用后检查返回值类型必须是 `HandoffInputData`，否则同理。之后用返回值覆盖 `original_input`、`pre_step_items`、`new_step_items`：`input_history` 是字符串就保留字符串，否则 `list(...)`。

**`input_items` 的分叉。** 若过滤器返回的 `input_items` 不为 `None`，则 `session_step_items = new_items`（完整）、`new_step_items = input_items`（裁剪后）；否则 `session_step_items = None`。这一个分支就是「模型输入」与「会话历史」分离的落地点。嵌套历史路径结构相同：`session_step_items` 拿完整 `nested.new_items`，模型输入拿 `nested.input_items`。

**结果收尾。** 返回 `SingleStepResult`，`next_step=NextStepHandoff(new_agent)`，并带上 `session_step_items`、`nested_history_owned_items`、以及 `handoff_input_filtered=input_filter is not None`。最后这个布尔很重要：它告诉持久化层「本轮的历史被过滤器改写过」，需要重新定位 response 锚点（见 `session_persistence.py` 里对 `NextStepHandoff` 的处理）。

**循环里的切换动作。** 非流式循环拿到 `NextStepHandoff` 后做的事依次是：把本轮的工具护栏结果写回 run state、`current_agent = turn_result.next_step.new_agent`、更新 run state 的当前 agent、持久化本轮项、结束旧 agent 的 span、置 `should_run_agent_start_hooks = True`、发 `AgentUpdatedStreamEvent`。流式路径多一步 `_publish_streamed_result_agent`：先把 `streamed_result.current_agent` 与 `_current_agent_output_schema` 更新掉，再持久化——注释说明这是为了「在取消可能完成流之前先发布 agent 切换」，避免消费者看到旧的 current_agent。

**续跑的发布。** `_publish_resumed_stream_turn` 用于「恢复既有 RunState 后完成一轮」的场景：它把这一轮的结果同步到 `RunResultStreaming` 上，包括 `input`（用 `turn_result.original_input` 覆盖，因为交接可能改写过历史）、`_original_input` 的独立副本、`new_items`（用会话项）、`_model_input_items`（用生成项）、以及 `_replay_from_model_input_items = generated_items != new_items`。最后这个布尔标记「模型输入与会话项不一致」，消费者据此知道不能把会话项直接当作模型输入回放。若本轮带了嵌套历史来源追踪，还要通过 `reconcile_nested_history_owned_session_item_refs` 重新对齐引用。

**中断完成。** `_complete_stream_interruption` 是流式路径上「停下等人」的收尾：清掉 `processed_response.tool_output_guardrail_result_start`、把中断项写进 `streamed_result.interruptions`、记下 `_last_processed_response`、置 `is_complete = True`、往事件队列塞 `QueueCompleteSentinel`。它不区分中断原因是审批还是别的，因为对消费者来说都是「流到此结束」。

## 关键取舍

**源码事实：** 一次响应里出现多个交接时只执行第一个，其余生成「已忽略」的工具输出。这避免了「同一步切到两个 agent」的不可判定状态，但也意味着模型一次想跳两级时只会走一级。

**源码事实：** 带命名空间的函数调用永远不会被识别为交接。命名空间（namespace）用于把工具分组，交接没有这个概念；若一个交接工具被无意套上命名空间，它就会变成「找不到对应函数工具」的错误。

**源码事实：** 交接失败不切换，走 `NextStepRunAgain`。于是「交接失败」对模型来说等价于一次工具失败：它看到错误文本，可以重试或改走别的路。`current_agent` 保持原样，避免把控制权交给一个没被正确调用的目标。

**源码事实：** 服务端管理会话与交接输入过滤器互斥且在运行期检查。同一个团队配置在本地会话下可用、在服务端会话下直接报错，这是配置与运行参数的耦合，不是配置本身的问题。

**源码事实：** `handoff_input_filtered` 会一路传到持久化层。它存在的唯一目的就是让「历史被改写」这件事可被下游感知，因为一旦改写，之前记录的位置锚点（例如某条响应在历史里的偏移）就失效了。

**源码事实：** 流式路径把 agent 切换的发布提到持久化之前。这是一个可取消性设计：如果持久化阶段被取消，消费者至少已经看到「现在由谁接手」，不会留下「已经在用新 agent 跑但 UI 还显示旧 agent」的错位。

**架构推论：** 交接的执行被刻意拆成「识别（解析期）→ 执行（切换期）→ 记账（结果项）」三段，每段都有独立的失败语义。识别失败退化成「工具找不到」，执行失败退化成「工具调用失败」，记账环节则保证 `HandoffCallItem` 与 `HandoffOutputItem` 成对出现。这种分层让「交接」复用了工具系统全部的健壮性，只额外增加「切换 agent」这一件事。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/items.py` | `HandoffCallItem` | 交接调用项，包裹原始的 `function_call` |
| `src/agents/items.py` | `HandoffOutputItem` | 交接输出项，携带源与目标 agent 的弱引用 |
| `src/agents/run_internal/turn_resolution.py` | `is_handoff_tool_call` | 识别条件：裸名 + 在交接映射内 |
| `src/agents/run_internal/turn_resolution.py` | `execute_handoffs` | 切换主体：只执行第一个、失败不切换、过滤器与嵌套策略 |
| `src/agents/run_internal/turn_resolution.py` | `_resolve_server_managed_handoff_behavior` | 服务端会话下禁用过滤器、降级嵌套历史 |
| `src/agents/run_internal/run_steps.py` | `NextStepHandoff` | 单步结果中的切换指令，只带 `new_agent` |
| `src/agents/run_internal/run_steps.py` | `ToolRunHandoff` | 待执行交接：交接对象 + 原始调用 |
| `src/agents/run_internal/run_loop.py` | `_publish_resumed_stream_turn` | 恢复轮次的发布：改写后的输入、会话项与回放标记 |
| `src/agents/run_internal/run_loop.py` | `_complete_stream_interruption` | 流式中断收尾：记录中断、置完成、塞哨兵 |
| `src/agents/run_internal/run_loop.py` | `_publish_streamed_result_agent` | 先发布 agent 切换再持久化，避免取消后错位 |
| `src/agents/run_internal/turn_preparation.py` | `get_handoffs` | 装配期把 agent 参与项转成交接并按 `is_enabled` 过滤 |

## 参考

- [openai-agents-python 源码：turn_resolution.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/turn_resolution.py)
- [openai-agents-python 源码：items.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/items.py)
- [Agents SDK 文档：Handoffs](https://openai.github.io/openai-agents-python/handoffs/)

## 与主核 / AI 的关系

交接的切换过程与主核里「切换执行上下文」有结构对应：都要在边界处完成三件事——结束旧上下文的记账、建立新上下文、把待处理的工作项带过去。主核靠事务与锁保证边界原子性，这里靠「先产出两个配对的运行项，再改 `current_agent`」来保证历史一致。

对 agent 工程最实用的三条推论。其一，**交接与工具在协议上是同一种东西**，所以模型分不清「调用工具」和「换人」的语义差别，只能靠工具描述来区分。描述写得含糊，模型就会在该调工具时换人。其二，**多交接请求是常见现象**：模型一次给出多个 `transfer_to_*` 调用时只有第一个生效，其余被标记忽略。开并行工具调用时尤其容易触发，必要时关掉并行调用或在提示里约束。其三，**交接会改写历史，而改写会让下游的位置计算失效**。任何依赖「历史第 N 项」的缓存或增量同步逻辑，在启用 `input_filter` 后都必须重新定位，SDK 自己就是靠 `handoff_input_filtered` 这个布尔来做这件事的。
