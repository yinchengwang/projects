---
project: crewai
feature: agent-executor-loop
title: 执行器循环与工具调用
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/utilities/agent_utils.py
    symbols: [get_llm_response, process_llm_response, handle_agent_action_core]
  - path: lib/crewai/src/crewai/utilities/agent_utils.py
    symbols: [has_reached_max_iterations, handle_max_iterations_exceeded, format_answer]
---

# 执行器循环与工具调用

## 一句话职责

执行器循环是「调模型 → 解析成动作或终答 → 若动作则执行工具 → 把观察结果拼回上下文 → 再来一轮」的 ReAct 骨架；`agent_utils` 提供这条骨架上四段可复用的零件：调模型（`get_llm_response`）、解析（`process_llm_response` / `format_answer`）、把工具结果接回（`handle_agent_action_core`）、以及迭代耗尽时的强制收尾（`has_reached_max_iterations` / `handle_max_iterations_exceeded`）。

## 机制是什么

循环的终止条件有两个，而且都没有「自然结束」之外的兜底：

一是**模型自己给出 `AgentFinish`**——解析出的对象类型决定走向，`AgentAction` 意味着「还要用工具」，`AgentFinish` 意味着「我有答案了」。

二是**迭代数用尽**。`has_reached_max_iterations(iterations, max_iter)` 就是 `iterations >= max_iterations` 一行，`max_iter` 默认 25。用尽后不直接报错，而是走 `handle_max_iterations_exceeded`：**再做一次模型调用**，但要先往消息尾部追加一条「请给最终答案」的用户消息，把返回文本包成 `AgentFinish`。

两个终止条件之间有一个中间态值得指出：工具结果可以被声明为「就是最终答案」（`tool_result.result_as_answer`），此时不等模型再表态，循环立刻以该结果收尾。

## 怎么实现的

**`get_llm_response` 是调用模型前的统一闸门。** 它的结构是一个上下文管理器包住一次调用：

```
with _prepare_llm_call(executor_context, messages, printer, verbose=verbose) as prepared_messages:
    if getattr(executor_context, "respect_context_window", False):
        _proactively_summarize_context(...)
    answer = _call_llm_retrying_empty(llm, lambda: llm.call(prepared_messages, tools=..., callbacks=..., available_functions=..., from_task=..., from_agent=..., response_model=...), ...)
return _setup_after_llm_call_hooks(executor_context, answer, printer, verbose=verbose)
```

三段各有职责：`_prepare_llm_call` 跑 before 钩子并把消息解析成「实际要发的那份」（有执行器上下文时用 `executor_context.messages` 而不是传入的 `messages`），同时在 `model_call_hooks_dispatched()` 里 yield，让模型层知道钩子已经跑过、不要重复派发；`_proactively_summarize_context` 是上下文预检（见 c04）；`_setup_after_llm_call_hooks` 跑 after 钩子。

`_prepare_llm_call` 的 `executor_context is None` 分支值得一提：没有执行器上下文时直接 `yield messages` 并返回，一个钩子都不跑。也就是说钩子机制是绑在执行器上的，裸调用不参与。

`respect_context_window` 的默认值在两侧不一致：`Agent` 上默认 `True`，`AgentExecutor` 上同名字段默认 `False`。实际生效的是执行器上那份，由 `Agent.create_agent_executor` 在构造执行器时、以及 `_prepare_kickoff` 在跑任务前，从智能体赋值过去（`_update_executor_parameters` 这条「只更新参数不重建」的路径不同步这个字段）。所以在 `get_llm_response` 里用 `getattr(executor_context, "respect_context_window", False)` 读到的是智能体的配置，但这依赖执行器确实被赋值过。

**`process_llm_response` 与 `format_answer` 是两层解析。** `format_answer` 只有 try/except：`parse(answer)` 成功则返回 `AgentAction` 或 `AgentFinish`，任何异常都兜底成 `AgentFinish(thought="Failed to parse LLM response", output=answer, text=answer)`——**解析失败一律当成终答**，把原始文本原样交出去。

`process_llm_response(answer, use_stop_words)` 在 `use_stop_words` 为假时多一步：调一次 `format_answer(answer)`，若抛 `OutputParserError` 且错误里含 `FINAL_ANSWER_AND_PARSABLE_ACTION_ERROR_MESSAGE`，就把 `answer` 截到 `"Observation:"` 之前再解析。这段处理的是「模型同时吐了 Final Answer 和一段可解析的 Action」这种畸形输出——取 Observation 之前的部分，即剥掉模型自己编造的工具观察结果。

为什么只在 `use_stop_words` 为假时才做这件事？因为开了 stop words 时模型会被 stop 序列提前截断，本来就不该产出「带 Observation 的完整文本」。这是一个「按调用配置决定后处理强度」的例子。

**`handle_agent_action_core` 把工具结果接回消息流。** 它做四件事：

1. 有 `step_callback` 就调它（同步返回用 `asyncio.run` 跑 coroutine），传的是 `tool_result` 而不是动作。
2. `formatted_answer.text += f"\nObservation: {tool_result.result}"`——**把观察结果拼进动作的文本**，这是 ReAct 格式的核心：下一轮把整段文本作为助手消息发回模型，模型看到的就是「我的动作 + 它的结果」。
3. `formatted_answer.result = tool_result.result`。
4. 若 `tool_result.result_as_answer` 为真，立刻返回 `AgentFinish(thought="", output=tool_result.result, text=formatted_answer.text)`——**某些工具可以宣告「我的输出就是答案」**，跳过后续模型调用。

否则调 `show_logs`（若提供）并返回原动作对象，循环继续。

注意 `handle_agent_action_core` 里有一句 `# TODO: Remove messages parameter and its usage.`，而 `messages` 参数在函数体里确实没被使用——这是源码里遗留的签名。

**迭代耗尽后的收尾。** `handle_max_iterations_exceeded` 的步骤：

```
if verbose: printer.print("Maximum iterations reached. Requesting final answer.", color="yellow")
messages.append(format_message_for_llm(I18N_DEFAULT.errors("force_final_answer"), role="user"))
enforce_rpm_limit(request_within_rpm_limit)
answer = _call_llm_retrying_empty(llm, lambda: llm.call(messages, callbacks=callbacks), printer, verbose=verbose, request_within_rpm_limit=request_within_rpm_limit)
formatted = format_answer(answer=answer)
if isinstance(formatted, AgentFinish): return formatted
return AgentFinish(thought=formatted.thought, output=formatted.text, text=formatted.text)
```

三个细节值得指出：

- 追加的是 **user 角色**消息，注释解释了原因：尾部若是 assistant 轮次，等价于 prefill 请求，当前 Claude 模型会以 400 拒绝；而所有供应商都接受尾部 user 轮次。这是一条被真实故障逼出来的兼容处理。
- 这次调用**不带 tools**（`llm.call(messages, callbacks=callbacks)`，没有 `tools` 参数），从协议上剥夺了模型再喊工具的能力，只能给文本。
- 返回值兜底：若模型仍然吐出一个可解析的 `AgentAction`（说明它不听话），就把它强行转成 `AgentFinish`，用 `formatted.text` 当输出。加上前面 `format_answer` 的「解析失败即终答」兜底，这里有两道保险。

**执行器侧的循环接线。** `AgentExecutor` 继承 `Flow[AgentExecutorState]`，把 ReAct 循环写成一组 `@start` / `@router` / `@listen` 方法而非 `while`：

- `@start() generate_plan` 是入口，规划未启用时直接返回，启用则生成计划并 `_create_todos_from_plan`。
- `initialize_reasoning` 在 `iterations == 0` 时判定是否使用原生函数调用（`_check_native_tool_support()`），是则 `_setup_native_tools()` 把工具转成 OpenAI schema。
- `check_max_iterations` 是分叉点：`has_reached_max_iterations(self.state.iterations, self.max_iter)` **或** `repeated_tool_call_limit_reached(self._tool_call_counts)` 为真就返回 `force_final_answer`；否则按 `use_native_tools` 走 `continue_reasoning_native` 或 `continue_reasoning`。
- `ensure_force_final_answer` 调 `handle_max_iterations_exceeded`，并把 `state.is_finished` 置真。它还带一处防重复：若已 finished 就直接返回（注释说 Flow 框架可能在同一次监听中多次路由到这里）。
- `call_llm_and_parse` 走文本解析路径：`get_llm_response`（`response_model` 在**没有** `original_tools` 时才传，因为结构化输出与工具调用不能同时用）→ 若是 `BaseModel` 直接包成 `AgentFinish`；否则 `process_llm_response` → `_route_finish_with_todos`。
- `call_llm_native_tools` 走原生路径：先 `state.pending_tool_calls.clear()`，`get_llm_response(tools=self._openai_tools, response_model=None)`，返回若是工具调用列表就存进 `pending_tool_calls`。
- `route_by_answer_type` 按 `isinstance(self.state.current_answer, AgentAction)` 决定 `execute_tool` 还是 `agent_finished`。
- `execute_tool_action` 执行工具：调 `execute_tool_and_check_finality(...)`，异常被细分成三类处理——`HookAborted` 与 `ToolExecutionFailedError` **原样抛出**（注释说这类「故意的停止」若被通用处理器接住，会被当成可恢复的观察喂回模型），其他异常则 `increment_tools_errors()`、把错误拼成观察、追加一条 `post_tool_reasoning` 提示，返回 `tool_completed` 让模型看到错误并自行调整。

之后 `check_repeated_tool_call(...)` 检测重复调用并可能追加格式提醒，`_handle_agent_action(action, tool_result)`（内部即 `handle_agent_action_core`）返回新状态，`_invoke_step_callback(result)`，`_append_message_to_state(result.text)`，若已成 `AgentFinish` 就置 `is_finished` 并返回 `tool_result_is_final`，否则追加 `post_tool_reasoning` 用户消息并返回 `tool_completed`。

**每个 `AgentAction` 的文本都通过 `_append_message_to_state` 落进 `state.messages`**，这是「Observation 拼进 text 再作为助手消息回传」的落地点。观察结果不是单独的 tool 角色消息（原生工具调用路径除外），而是文本的一部分——这是文本 ReAct 与原生 function calling 的根本差别。

**原生路径的并行判定。** `execute_native_tool` 会调 `_should_parallelize_native_tool_calls(tool_calls)` 决定是否并发执行多个工具调用，`_execute_single_native_tool_call` 负责单个调用与结果整理。具体阈值与条件属于另一页的范畴，这里只需知道：**批量工具调用只存在于原生路径**，文本路径一轮只执行一个工具。

## 关键取舍

**源码事实：** 解析失败不抛异常而是包装成终答（`thought="Failed to parse LLM response"`）。这让畸形输出成为可交付的结果，而不是流程中断——代价是「模型胡说」与「模型正常回答」在返回值上无法区分。

**源码事实：** 迭代耗尽后仍追加一次模型调用，且这次调用不带工具。这把「预算用尽」变成「强制交付」而不是「报错退出」。

**源码事实：** 强制收尾的消息用 user 角色而非 assistant 角色，理由是 Claude 系列会拒绝尾部 assistant 轮次（被当成 prefill）。兼容性考虑直接写进了消息构造。

**源码事实：** `handle_agent_action_core` 的 `messages` 参数未被使用，源码里留着 `# TODO`。这是重构未清理的遗留。

**源码事实：** `HookAborted` 与 `ToolExecutionFailedError` 在工具执行处被单独 `raise`，不走通用的「把异常变成观察」分支。理由是「故意的停止」与「工具报错」在语义上不同：前者必须终止，后者应当让模型看到并重试。

**源码事实：** 文本路径把 Observation 拼进 `AgentAction.text` 再作为助手消息回传；原生路径用独立的 tool 角色消息。两条路径的消息结构不同，因此上下文压缩面对的形状也不同。

**架构推论：** 把循环写成 Flow 的路由图而不是 `while` + 状态变量，使得「检查迭代上限」「按答案类型分叉」「工具执行后判断是否终止」这些判定点都成为可单独测试与替换的节点。代价是阅读时必须沿着装饰器的标签在方法之间跳转，控制流不再线性可读。

**架构推论：** 终止判定分散在三处（`route_by_answer_type` 的 `agent_finished`、`handle_agent_action_core` 的 `result_as_answer`、`ensure_force_final_answer` 的迭代耗尽），任何一处遗漏都会导致循环空转，因此 `max_iter` 是最后的安全网。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `get_llm_response` | 调用统一闸门：钩子、上下文预检、模型调用、after 钩子 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `process_llm_response` | 按是否用 stop words 决定是否先剥掉自造 Observation 再解析 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `format_answer` | 解析兜底：失败即包装成 `AgentFinish` |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `handle_agent_action_core` | 拼 Observation、回填 `result`、`result_as_answer` 即终止 |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `has_reached_max_iterations` | 迭代终止判据：`iterations >= max_iterations` |
| `lib/crewai/src/crewai/utilities/agent_utils.py` | `handle_max_iterations_exceeded` | 迭代耗尽后的强制收尾：追加 user 消息、无工具调用、结果强制成终答 |
| `lib/crewai/src/crewai/agents/agent_executor.py` | `AgentExecutor` | 用 Flow 的 start/router/listen 表达的 ReAct 循环 |
| `lib/crewai/src/crewai/agents/agent_executor.py` | `check_max_iterations` | 分叉点：迭代或重复调用超限 → 强制收尾，否则按原生/文本继续 |
| `lib/crewai/src/crewai/agents/agent_executor.py` | `execute_tool_action` | 工具执行与异常分类：故意停止抛出，普通错误转为观察 |
| `lib/crewai/src/crewai/agents/agent_executor.py` | `call_llm_and_parse` | 文本路径：结构化输出与工具互斥的处理，解析后路由 |
| `lib/crewai/src/crewai/agents/agent_executor.py` | `AgentExecutorState` | 循环状态：消息、迭代数、当前答案、是否结束、待决工具调用 |

## 参考

- [CrewAI 源码：agent_utils.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/agent_utils.py)
- [CrewAI 源码：agent_executor.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/agents/agent_executor.py)
- [CrewAI 文档：智能体执行与迭代上限](https://docs.crewai.com/en/concepts/agents#direct-code-execution)

## 与主核 / AI 的关系

这个循环与主核里的「重试驱动的事件循环」在骨架上同构：状态机 + 每轮一次外部调用 + 结果决定下一步 + 迭代上限保底。主核里常见的是「读页失败重试、锁冲突重试」，这里换成「模型产出动作、执行工具、再问模型」。差别在于主核的重试由错误码驱动，是**封闭集合**；这里的循环由模型的自然语言输出驱动，是**开放集合**——模型可以产出任何东西，包括无法解析的文本、伪造的 Observation、本不存在的工具名。

对 AI 工程的实际意义在于：这类循环的可靠性不来自「让模型不出错」，而来自**三层兜底**。第一层是解析兜底（解析失败就当终答，至少交出原文）；第二层是迭代上限（无论如何不会无限跑）；第三层是强制收尾（用一次无工具调用逼出文本答案）。三层都不试图修正模型，只保证循环一定会终止且一定产出某种结果。理解了这一点，就能理解为什么 `format_answer` 要把异常吞成 `AgentFinish`——在这样一个开放输入的系统里，「优雅地交出一个可能不对的答案」比「抛异常让整条流水线挂掉」更符合整体设计取向。
