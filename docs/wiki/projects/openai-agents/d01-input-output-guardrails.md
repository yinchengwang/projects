---
project: openai-agents
feature: input-output-guardrails
title: 输入输出护栏与触发语义
page_type: feature
schema_version: 1
refs:
  - path: src/agents/guardrail.py
    symbols: [InputGuardrail, OutputGuardrail, GuardrailFunctionOutput]
---

# 输入输出护栏与触发语义

## 一句话职责

在 agent 层面的两个固定切点插检查：输入护栏在首轮处理用户输入时跑，输出护栏在产出最终答案时跑；两侧都用同一个「触发线」语义——返回 `tripwire_triggered=True` 就抛专用异常，让整次运行失败。

## 机制是什么

`GuardrailFunctionOutput` 只有两个字段：`output_info`（任意检查细节，只给应用看）与 `tripwire_triggered`。没有「拦截」「改写」「降级」这些中间档——只有「放行」与「触发」两种结局。

`InputGuardrail` 与 `OutputGuardrail` 都是 dataclass，各含三样：`guardrail_function`、`name`（缺省取函数名，用于追踪）、以及输入侧独有的 `run_in_parallel`（默认 `True`）。

两者的函数签名不同：输入护栏是 `(run_context, agent, input)`，其中 `input` 是 `str` 或输入项列表；输出护栏是 `(run_context, agent, agent_output)`，拿到的是 agent 的最终输出。

两边的结果对象也不同：`InputGuardrailResult` 只有护栏与输出；`OutputGuardrailResult` 额外带上 `agent` 与 `agent_output`——因为「被检查的那份输出」是判定上下文的一部分，应用排查时需要它。

护栏来自两处并**拼接**：`agent.input_guardrails + run_config.input_guardrails`。也就是说 agent 级与运行级护栏是累加关系，不是覆盖关系。

## 怎么实现的

**调用侧的同步/异步统一。** 两个 `run` 方法开头都检查 `callable(self.guardrail_function)`，不是就 `UserError`。然后直接调用，用 `inspect.isawaitable` 判断是否需要 `await`。这个「先调后判」的写法让同步函数与协程函数都能用，但有个细节：同步函数若返回 awaitable 也能被识别，因此不需要在装饰器阶段就分叉。

**追踪条目的写入时机。** `run_single_input_guardrail` 与 `run_single_output_guardrail` 都包在 `guardrail_span(name)` 里，并在**跑完之后**写 `span_data.triggered = result.output.tripwire_triggered`。所以追踪里记的是真实结论，而不是「跑过了」。

**并发执行 + 边完成边记录。** 两个批量函数都为每个护栏建一个 `asyncio.Task`，然后用 `asyncio.as_completed` 逐个收取，把结果追加进本地列表与调用方的 `results_sink`。关键在触发分支的次序：拿到触发结果后**先** `record(triggering)`，**再**补齐兄弟结果，**最后**才取消 task 并抛异常。这样即使异常抛出，调用方看到的列表里也一定包含触发项与所有已完成的兄弟项，不会缺项。

**触发时的收集补齐。** 一旦某个结果 `tripwire_triggered` 为真，函数不会立刻跳出——先调 `_unreported_guardrail_results` 扫一遍所有 task，把「已经完成、没被取消、没有异常、但 `as_completed` 还没轮到」的兄弟结果补进列表。源码注释解释了动机：`as_completed` 的产出顺序不等于完成顺序，可能有已经跑完的护栏结论还没被 yield 出来；不补，这些结论就会在异常路径上丢失，调用方看到的结果集就会缺项。

补齐之后才取消所有 task 并 `asyncio.gather(..., return_exceptions=True)` 等它们收尾。两个批量函数的外层还有一层 `except BaseException`：任何错误（包括护栏自己抛异常、调用方被取消）都会取消并等待兄弟任务，注释写明目的是「不让它们泄漏到这个函数返回之后」。

**错误挂到 span 上。** 输入侧在非流式路径里用 `_error_tracing.attach_error_to_current_span`；流式路径由调用方传入 `parent_span`，有则挂到它，没有则退回当前 span——注释说明早于 agent span 建立的流式首轮护栏可能还没有 parent。输出侧一律挂当前 span。

**异常类型。** `InputGuardrailTripwireTriggered` 与 `OutputGuardrailTripwireTriggered` 都只携带结果对象，消息是 `Guardrail <类名> triggered tripwire`。注意消息里用的是**护栏类名**而不是 `get_name()` 的返回值——若应用给护栏起了自定义名字，异常消息里看不到它，但追踪里能看到。

**输入护栏只在首轮跑。** 非流式路径的判定是 `current_turn <= 1`，流式路径是 `current_turn == 1`。这不是「每轮都检查」：输入护栏的职责是审查用户输入，而后续轮次的模型输入里混的是工具结果与自己的历史，不属于「新输入」。例外是「待决输入」路径：`run_state.pending_input` 非空时会用 `current_agent` 的护栏再跑一次——因为那份输入是在中断之后才被接纳的，等于一次新的用户输入。

**`run_in_parallel` 的两种时序。** 为假的护栏在模型调用**之前**串行等待完（`await run_input_guardrails(...)`），也就是说它们可以阻止一次昂贵的模型调用。为真的护栏与模型调用并发：非流式路径建 `guardrail_task` 与 `model_task` 两个 task 再 `asyncio.gather`；流式路径把护栏任务挂到 `streamed_result._input_guardrails_task` 上，让模型流先跑。

**输出护栏的位置与专属限制。** 输出护栏在 `NextStepFinalOutput` 分支里跑，即已经有最终输出之后。它们与服务器托管会话（`conversation_id`/`previous_response_id`/`auto_previous_response_id`）**不兼容**：`validate_output_guardrails_with_server_managed_conversation` 在存在这些标识且有输出护栏时抛 `UserError`，理由是「被拒绝的输出无法从服务端管理的会话历史里删除」。例外是 `OpenAIChatCompletionsModel`，注释说明它自己已有既定行为（警告忽略或严格拒绝），由它自行处理。

**一处特例：终局工具输出可被持平。** 输出护栏抛触发异常时，若当前响应是「工具直接产出终局」（`_is_terminal_tool_output_response`），代码不直接 `raise`，而是做三件事：把本轮的护栏结果做数据脱敏重建（`_sanitize_blocked_output_guardrail_results`，保留行为类型但把 `output_info` 换成固定的阻断文案）、把会话项回退到本轮之前的快照（`_blocked_output_failure_items`）、把这个结论折成 `OUTPUT_GUARDRAIL_BLOCKED_TOOL_OUTPUT`。也就是「有东西被拦下了，但运行不炸」。

## 关键取舍

**源码事实：** 护栏只有二元结论。没有「拦截并替换内容」「要求重试」「降级放行」这些中间档。应用的常见需求（例如「输出不合规就换一句话回给用户」）必须自己在护栏之上再包一层，因为 SDK 只会抛异常。

**源码事实：** 结果集在异常路径上仍然完整（含触发项与已完成的兄弟项）。这使应用可以在捕获异常后报告「所有跑过的检查结论」，而不是只拿到触发的那一个。

**源码事实：** 输入护栏与模型调用是**真并发**。这意味着护栏的判定可能晚于模型开始生成——一次很慢的护栏不会阻止模型调用发生，只能事后让运行失败。真正需要「先审后跑」的场景必须把 `run_in_parallel` 设为假。

**源码事实：** 护栏并发但**不限制并发度**。十个输入护栏就是十个并发任务，没有批量大小或队列。护栏里若调用模型，就是十条并发的模型请求。

**源码事实：** 输出护栏与服务器托管会话互斥，且这是**运行前的配置期硬拒绝**，不是运行期降级。这条约束的根源是「拒绝必须能在本地历史里生效」，而服务端历史删不掉。

**源码事实：** 触发异常消息用护栏的**类名**而非 `get_name()`。自定义名字只进追踪，不进异常文案。

**架构推论：** 输入护栏挂在「首轮」而不是「每次调用」上，等于把护栏定位成「对外边界检查」而不是「每步检查」。这条边界的选择会连带影响所有下游设计：想在多轮任务里持续检查，得用工具护栏（在每次工具调用前后）而不是输入护栏。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/guardrail.py` | `GuardrailFunctionOutput` | 二元结论：检查细节 + 是否触发 |
| `src/agents/guardrail.py` | `InputGuardrail` | 输入护栏：`run_in_parallel` 决定与模型并发还是先行 |
| `src/agents/guardrail.py` | `OutputGuardrail` | 输出护栏，绑定在最终输出之后 |
| `src/agents/guardrail.py` | `InputGuardrailResult` / `OutputGuardrailResult` | 结果对象；输出侧额外带被测输出与 agent |
| `src/agents/guardrail.py` | `input_guardrail` / `output_guardrail` | 装饰器，同时支持直接调用与带参数调用 |
| `src/agents/run_internal/guardrails.py` | `run_single_input_guardrail` | 单条护栏执行 + 追踪条目写入真实触发值 |
| `src/agents/run_internal/guardrails.py` | `run_input_guardrails` | 非流式批量执行：边完成边记录、触发时补齐兄弟项后抛错 |
| `src/agents/run_internal/guardrails.py` | `run_output_guardrails` | 输出侧批量执行，结构与非流式输入侧对称 |
| `src/agents/run_internal/guardrails.py` | `_unreported_guardrail_results` | 收集 `as_completed` 尚未 yield 的已完成结果 |
| `src/agents/exceptions.py` | `InputGuardrailTripwireTriggered` | 携带 `InputGuardrailResult` 的触发异常 |
| `src/agents/exceptions.py` | `OutputGuardrailTripwireTriggered` | 携带 `OutputGuardrailResult` 的触发异常 |
| `src/agents/run_internal/agent_runner_helpers.py` | `validate_output_guardrails_with_server_managed_conversation` | 输出护栏与托管会话的配置期互斥校验 |

## 参考

- [openai-agents-python 源码：guardrail.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/guardrail.py)
- [openai-agents-python 源码：run_internal/guardrails.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/guardrails.py)
- [Agents SDK 文档：Guardrails](https://openai.github.io/openai-agents-python/guardrails/)

## 与主核 / AI 的关系

这类「检查点 + 触发线」的设计，与主核里「断言失败即中止事务」同构：检查本身不修改数据，结论只有通过或不通过，不通过就整体失败。差别是主核的断言崩溃后可以重试新事务，这里的触发意味着这次运行作废——已经花掉的模型调用与工具副作用都不会回滚。

对 agent 工程最实用的三条推论。其一，**护栏的判定成本要计入总成本**。并发跑的护栏不阻塞主流程，但会与主流程一起消耗 token 与配额；把大模型审查塞进输入护栏，等于每次运行都多一次完整推理。其二，**「并行护栏不能阻止模型调用」这件事必须显式处理**：如果想省钱，就该用 `run_in_parallel=False` 让便宜的规则检查先跑；便宜的通过之后，再把贵的那条放进并行。其三，**在异常里读结果集，而不是读异常本身**。异常只带触发的那一条，`RunResult.input_guardrail_results` 之类的结果字段才有完整清单。
