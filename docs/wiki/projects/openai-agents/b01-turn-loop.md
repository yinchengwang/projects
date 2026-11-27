---
project: openai-agents
feature: turn-loop
title: 单轮循环与最大轮次
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_internal/run_loop.py
    symbols: [_prepare_turn_input_items, _publish_streamed_result_agent]
  - path: src/agents/run_internal/agent_runner_helpers.py
    symbols: [snapshot_usage, usage_delta]
---

# 单轮循环与最大轮次

## 一句话职责

把「模型调用 + 工具/交接执行」压成一个只产出四种下一步结论的单轮函数，再由一个 `while True` 循环按结论决定继续、换人、结束或停下等审批，并用「自增后判断」的方式执行轮次上限。

## 机制是什么

循环体每轮只做一件事：调用一次单轮函数，拿到一个 `SingleStepResult`，然后按其中的 `next_step` 分派。`next_step` 只有四种类型：

| `next_step` | 含义 | 循环动作 |
|---|---|---|
| `NextStepRunAgain` | 工具跑完了，结果要回喂模型 | `continue`，轮次 +1 |
| `NextStepHandoff` | 交接被调用 | 换 `current_agent`，标 `should_run_agent_start_hooks`，`continue` |
| `NextStepFinalOutput` | 得到最终输出 | 跑输出护栏、落盘、构造 `RunResult` 返回 |
| `NextStepInterruption` | 有工具调用等审批 | 构造带 `interruptions` 的 `RunResult` 返回 |

单轮函数本身（非流式的 `run_single_turn`、流式的 `run_single_turn_streamed`）固定按这个顺序推进：取当前轮输入项 → 跑 agent start 钩子 → 收集工具 → 取系统提示与 prompt → 收集交接并解决重名 → 解析模型与设置 → 组装模型输入并过输入过滤器 → 调模型 → 处理响应（工具/交接/最终输出）。

**循环不接受自定义。** 没有「用户可以插入一个自定义 step」的挂载点，也没有图或 DAG。可扩展性全部通过 `Agent` 的四个字段（工具、交接、护栏、钩子）与 `RunConfig` 的过滤器表达。这是这个 SDK 与 langgraph（画图）、autogen（组运行时）、crewai（定义 SOP）最硬的分野。

## 怎么实现的

**轮次计数是「先自增、后判断」，且判断放在本轮的 hooks 与持久化之后。** 非流式路径的顺序是：本轮结束、步骤返回 `RunAgain` 或其它 → `continue` 回到循环顶 → 处理 pending input → `current_turn += 1` → 若 `current_turn > max_turns` 则抛 `MaxTurnsExceeded`。也就是说 `max_turns=3` 允许模型被调用 3 次，第 4 次之前被拦住。仓内测试正是这么断言的：脚本化模型准备 5 轮，`max_turns=3` 期望抛错；`max_turns=None` 时设 `result.max_turns is None` 并且跑完全部。

**上限检查有两处，加一处独立的前置守卫。** 非流式在 `run.py` 的循环里，流式在 `run_loop.py` 的循环里，两处都是同一个形态：`current_turn += 1` 之后紧接着 `if max_turns is not None and current_turn > max_turns`。第三处不是循环检查而是 `RunState.add_input` 的准入守卫，用的是 `>=` 而不是 `>`——因为 `add_input` 判断的是「还剩不剩预算」，而不是「这一轮能不能开始」。两处循环检查都做同一件事：往当前 span 挂 `SpanError(message="Max turns exceeded", data={"max_turns": ...})`，构造 `MaxTurnsExceeded(f"Max turns ({n}) exceeded")`，然后**先问错误处理器**。

**超过上限不是立刻抛，而是先给 `error_handlers["max_turns"]` 一次机会。** `resolve_run_error_handler_result` 支持返回三种东西：`RunErrorHandlerResult` 实例、带 `final_output` 的字典、或任意值（被包成 `final_output=任意值`）。字典只接受 `final_output` 与 `include_in_history` 两个键，多一个就抛 `UserError`。处理器返回 `None` 或没注册处理器时，异常原样抛出。走 handler 分支时还会额外做三步：把 `result._current_turn` 显式设成 `max_turns`；若 `include_in_history` 且输出还没记录，用 `_terminal_unrecoverable` 包住那次 append（注释说明这是为了让「已接受的结果」在写入期间对续跑关闭）；然后返回带 `final_output` 的 `RunResult` 而不是抛错。

**每轮的输入项由两段拼接而成，且只对第二段做修剪。** `_prepare_turn_input_items` 做三件事：把调用方给的 `caller_input` 转成项列表、把 `generated_items` 用 `run_items_to_input_items(..., reasoning_item_id_policy)` 转成项列表、再交给 `prepare_model_input_items` 合并。合并函数的行为差异是刻意的：调用方项只做归一化（`normalize_input_items_for_api`），**生成项**还要额外 `drop_orphan_function_calls`——把「有 tool_call 却找不到对应 tool_call_output」的悬空项去掉。注释写明理由：只修剪 runner 自己生成的历史。

`reasoning_item_id_policy` 在这里生效：`"omit"` 会剥离推理项的 id，`"preserve"`（或 `None`）保留。这个策略既可以来自 `RunConfig`，也可以从 `RunState` 恢复，优先级是「RunConfig 有就用 RunConfig」。

**服务端会话追踪会接管输入组装。** 若存在 `server_conversation_tracker`（由 `conversation_id` / `previous_response_id` / `auto_previous_response_id` 触发），就改为 `tracker.prepare_input(原始输入, 生成项)`，跳过 `_prepare_turn_input_items`。两条路径互斥，且追踪路径还要在过滤后做 `validate_pending_input_filter` 与 `mark_input_as_sent`。

**输入过滤器在「组装完、钩子前」执行一次。** `maybe_filter_model_input` 拿 `ModelInputData`（`input` 与 `instructions` 两份），交给 `RunConfig.call_model_input_filter`，要求返回值必须也是 `ModelInputData`，否则 `UserError`。失败时向当前 span 挂错误（受 `trace_include_sensitive_data` 控制是否带原文）。之后若输入是列表，还会 `deduplicate_input_items_preferring_latest`。

**钩子成对并发跑，运行级与智能体级并列。** 每个钩子点都用 `gather_with_cancel(hooks.on_X(...), agent.hooks.on_X(...) or noop)`：`RunHooks` 与 `AgentHooks` 的两个回调**同时**发起，任一方抛错都会取消另一方并排空。这个辅助函数用 `asyncio.ensure_future` + `asyncio.gather` + done 回调组合，注释说明「兄弟失败时给已由调用方接管的子任务一个宽限期再取消，父级取消则立即取消全部」。所以钩子里做重活会直接拖慢该阶段，且没有「哪个先」的保证。

**流式路径多了三个循环特有的动作。** 一是每轮开头 `_publish_streamed_result_agent(streamed_result, current_agent)` 把「当前智能体」与「当前输出 schema」写进结果对象——docstring 写明目的是「让智能体切换在取消完成流之前就可见」。二是每轮结束调 `_wait_for_streamed_turn_events_and_stop_if_cancelled`：等消费者把本轮事件处理完，然后若 `_cancel_mode == "after_turn"` 就主动完成流并退出。三是取消分两档：`"immediate"` 立刻停，`"after_turn"` 等本轮收尾。

**用量按「任务级快照 + 轮级快照」两级做差。** `snapshot_usage(usage)` 造一份只含聚合字段的浅副本（故意不含 `request_usage_entries`）；循环开始前取一次 `task_usage_start`，每轮开始前取一次 `turn_usage_start`；span 结束时分别算 `usage_delta(task_usage_start, 当前)` 与 `usage_delta(turn_usage_start, 当前)` 挂到 task span 与 turn span 上。`usage_delta` 对每个字段（含 `cached_tokens`、`cache_write_tokens`、`reasoning_tokens`）做减法，`None` 当 0。因为 `Usage.add` 是原地累加，这个差值就是「这一轮/这一次运行新花掉的」。

**流式循环的退出条件比非流式多。** 除了返回与抛错，还有：`streamed_result._stored_exception` 非空（后台错误已记录）、`is_complete` 已为真、`_cancel_mode == "after_turn"` 且本轮已等消费者处理完。`NextStepRunAgain` 分支会在 `continue` 前先持久化本轮项并设 `run_state._current_step = NextStepRunAgain()`，这样「取消发生在两轮之间」时恢复能知道下一步是再跑一轮。

## 关键取舍

**源码事实：** 轮次是「模型调用次数」，docstring 明写「a turn is defined as one AI invocation (including any tool calls that might occur)」。所以一轮里可以有多次工具调用（并行或串行），但只有一次模型请求。这个定义直接决定了 `max_turns` 的直觉：它限的是模型往返次数，不是工具次数。

**源码事实：** 上限检查放在自增之后、本轮执行之前，于是「第 n 轮已执行完但第 n+1 轮不该开始」这个时点被精确拦住。副作用是：`Result._current_turn` 在超限且被 handler 收下时被强制设成 `max_turns`（而不是实际的 `max_turns + 1`），使结果对象上的轮次号与预算一致。

**源码事实：** `max_turns=None` 是显式关闭，不是「传个很大的数」。三处检查都写成 `if max_turns is not None and current_turn > max_turns`，`RunState._max_turns` 也允许 `None` 并在序列化里以 `null` 保存（schema 1.10 起）。

**源码事实：** 超限可不抛错。`RunErrorHandlers` 目前支持三类错误（`max_turns`、模型拒绝、模型行为错误），处理器的返回值被当成最终输出。这使「达到预算就返回已知最好结果」这种产品化行为不需要用户在循环外自己兜。

**源码事实：** 每一轮都重新收集工具与交接（`get_all_tools`、`get_handoffs`），不是运行开始时算一次。因此工具与交接的启用判定（`is_enabled` 回调）每轮都会重新求值——这是「按上下文动态启停工具」得以成立的前提，也意味着这些回调会被高频调用。

**架构推论：** 固定循环 + 四种下一步，使「行为可枚举」：任何一次运行的历史都能被还原成一串 step 类型。对比 langgraph 的任意图，这里的可观测性更强（span 树结构固定），但表达力更弱（无法做条件循环之外的编排）。

**架构推论：** 钩子成对并发而不是串联，意味着 `RunHooks` 不能用作「在 `AgentHooks` 之前完成某事」的序化机制。需要顺序时只能放在同一个对象里，或改用工具/护栏这些有明确阶段的挂载点。

**架构推论：** 两级用量快照让「这一次运行花了多少」与「这一轮花了多少」都不依赖外部计数器。代价是快照函数必须知道哪些字段参与做差——`snapshot_usage` 里逐字段抄了一遍，新增字段若忘了加，差值就会缺这项且不会报错。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run.py` | `AgentRunner.run` 主循环 | 非流式的 `while True`、四种 next_step 分派与轮次上限 |
| `src/agents/run_internal/run_loop.py` | `run_single_turn` | 非流式单轮：钩子、工具、提示、模型、响应处理 |
| `src/agents/run_internal/run_loop.py` | `run_single_turn_streamed` | 流式单轮，多出事件发布与消费者等待 |
| `src/agents/run_internal/run_loop.py` | `_prepare_turn_input_items` | 调用方项 + 生成项拼接，仅修剪生成项 |
| `src/agents/run_internal/run_loop.py` | `_publish_streamed_result_agent` | 在取消可能完成流之前发布智能体切换 |
| `src/agents/run_internal/run_loop.py` | `_wait_for_streamed_turn_events_and_stop_if_cancelled` | 等本轮事件消费完，按取消模式决定是否收尾 |
| `src/agents/run_internal/run_loop.py` | `_complete_stream_interruption` | 中断时写入 interruptions 并投递完成哨兵 |
| `src/agents/run_internal/run_loop.py` | `finalize_max_turns_handler_output` | 超限处理器输出的校验、护栏与终结钩子 |
| `src/agents/run_internal/items.py` | `prepare_model_input_items` | 归一化调用方项、修剪生成项的悬空调用 |
| `src/agents/run_internal/agent_runner_helpers.py` | `snapshot_usage` | 只含聚合字段的用量快照 |
| `src/agents/run_internal/agent_runner_helpers.py` | `usage_delta` | 两个快照的逐字段差值，`None` 归零 |
| `src/agents/run_internal/agent_runner_helpers.py` | `attach_usage_to_span` | 把差值写进 span 导出元数据 |
| `src/agents/run_internal/turn_preparation.py` | `maybe_filter_model_input` | 输入过滤器的唯一执行点与返回值校验 |
| `src/agents/run_internal/error_handlers.py` | `resolve_run_error_handler_result` | 错误处理器的三种返回形态与严格键校验 |
| `src/agents/util/_asyncio_tasks.py` | `gather_with_cancel` | 钩子并发：兄弟失败取消兄弟，父级取消全停 |

## 参考

- [openai-agents-python 源码：run_loop.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/run_loop.py)
- [openai-agents-python 源码：agent_runner_helpers.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/agent_runner_helpers.py)
- [Agents SDK 文档：运行智能体](https://openai.github.io/openai-agents-python/running_agents/)
- [Agents SDK 文档：结果对象](https://openai.github.io/openai-agents-python/results/)

## 与主核 / AI 的关系

这个循环与主核的查询执行流水线同构：固定阶段、每阶段产出确定的中间结果、由阶段结论决定下一步。主核的阶段是解析 → 计划 → 执行 → 返回，这里的阶段是「取输入 → 调模型 → 处理工具/交接 → 决定下一步」。两者都用「阶段不再可插入」换取「执行可预测、可追踪」。

对 agent 工程有三条可直接用的结论。第一，**轮次定义要先对齐**：如果团队以为 `max_turns` 限的是工具调用次数，就会把预算设错一个数量级；这个 SDK 限的是模型往返。第二，**上限要能被接管**：生产系统里「跑超预算」的合理行为往往是返回当前最好的结果，而不是抛异常给用户；`error_handlers` 让这件事不必在循环外重写。第三，**输入修剪要区分来源**：只修剪框架生成的悬空调用、不动用户输入的完整性，是避免「用户传的历史被悄悄改写」的关键；反之若统一修剪，用户手写的 tool_call 会被吃掉。

用量做差这套做法也值得借用：比起在每个调用点累加计数，用「进入阶段前快照、离开阶段后做差」更不容易漏记，代价是快照函数必须与 `Usage` 的字段同步演进。
