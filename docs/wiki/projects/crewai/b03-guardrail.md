---
project: crewai
feature: guardrail
title: 护栏与重试上限
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/task.py
    symbols: [validate_guardrail_function, ensure_guardrail_is_callable]
---

# 护栏与重试上限

## 一句话职责

护栏是「任务产出之后、交付之前」的一道可编程或可由模型执行的检查：不通过就把错误反馈塞回给智能体重跑一次任务，直到通过或用完 `guardrail_max_retries` 次配额；`validate_guardrail_function` 与 `ensure_guardrail_is_callable` 分别在构造期把护栏验成「签名正确」与「真的可调用」。

## 机制是什么

护栏有两种形态，共用同一个接口 `(TaskOutput) -> tuple[bool, Any]`：

- **可调用对象**：用户写的 Python 函数。返回 `(True, 结果)` 表示通过、`(False, 错误说明)` 表示不通过。
- **字符串描述**：由框架包成 `LLMGuardrail`，把「判定标准」写成提示词，交给一个专职智能体（角色是 `Guardrail Agent`）用结构化输出给出 `valid` 与 `feedback`。

两种形态最终都收敛到 `GuardrailCallable`，因此执行侧只看到一种东西。

字段层面有三个入口：`guardrail`（单条）、`guardrails`（列表）、以及已废弃的 `max_retries`（会被映射到 `guardrail_max_retries`）。`guardrail_max_retries` 默认 3，实际尝试次数是 `guardrail_max_retries + 1`——**首次尝试不算重试**。

## 怎么实现的

**构造期的两道校验。**

`validate_guardrail_function` 是 `@field_validator("guardrail")`，只在值是 callable 时检查：

- 用 `inspect.signature` 取参数，筛出**没有默认值**的参数（`param.default is inspect.Parameter.empty`），要求恰好一个。注意它统计的是「必填参数」而不是全部参数——所以 `def g(output, *, verbose=False)` 也能通过。
- 若函数带返回注解，则必须能解构为「`tuple` 泛型 + 两个类型参数 + 第一个是 `bool` + 第二个是 `Any` / `str` / `TaskOutput` / `str | TaskOutput`」；否则抛「If return type is annotated, it must be Tuple[bool, Any]」。
- 没有返回注解时直接放行（`return_annotation == inspect.Signature.empty` 时跳过检查）。

这段校验的动机写在 docstring 里：类型注解在运行时可被忽略，而护栏签名错误要到任务跑完才暴露，代价太高，所以提前检查。注意它**只检查、不转换**，原值原样返回。

`ensure_guardrail_is_callable` 是 `@model_validator(mode="after")`，负责把字符串变成可调用对象：`callable(self.guardrail)` 为真就把引用存到私有属性 `_guardrail`；是 `str` 则先检查 `self.agent` 非空（否则抛「Agent is required to use LLMGuardrail」）、再检查 `self.agent.llm` 是 `BaseLLM` 实例（否则抛「Agent must have a BaseLLM instance to use LLMGuardrail」），然后 `LLMGuardrail(description=self.guardrail, llm=self.agent.llm)` 存进 `_guardrail`。

两个前置条件都不是形式主义：`LLMGuardrail` 需要一个模型来当裁判，所以既要有智能体（用来取模型），模型又必须是框架自己的 `BaseLLM`（因为要走 `agent.kickoff(response_format=LLMGuardrailResult)` 这条结构化输出链路）。

**列表字段的自洽规则。** `ensure_guardrails_is_list_of_callables` 处理 `guardrails` 的四种输入形态：列表 / 元组、单个 callable、单个字符串、其他（抛「Guardrail must be a callable or a string」）。列表里逐项归类，字符串项要走与上面相同的 LLM 包装路径。最后一步是关键：

```
self._guardrails = guardrails
if self._guardrails:
    self.guardrail = None
    self._guardrail = None
```

也就是说 **`guardrails` 一旦非空就清空单条的 `guardrail`**。这避免了「同一个护栏既在列表里又在单条字段里」导致重复执行两遍。

**执行期的重试循环。** `_invoke_guardrail_function`（同步）与 `_ainvoke_guardrail_function`（异步）逻辑一致：

- 计数器来源分两种：传了 `guardrail_index` 就用 `self._guardrail_retry_counts[guardrail_index]`（每条护栏各自计数），否则用全局的 `self.retry_count`。
- `max_attempts = self.guardrail_max_retries + 1`。
- `accumulated_failures = list(task_output.tool_failures)`——注释说明每次重试会重置智能体的失败列表，所以要在这里累积，否则被拦下的那几次的工具失败会从最终输出上消失。
- 循环 `for attempt in range(max_attempts)`：调 `process_guardrail(...)`；成功则先检查 `guardrail_result.result is not None`（为 `None` 直接抛异常，理由是「护栏通过就必须给出可用的结果」），再 `_apply_guardrail_result` 套用结果，若需要补导出就 `_export_output` 并写回 `pydantic` / `json_dict`；失败且 `attempt >= self.guardrail_max_retries` 就抛「Task failed guardrail validation after N retries. Last error: ...」。
- 未耗尽时递增计数（分索引与全局两条路），用国际化文案 `validation_error` 把「错误说明 + 当前输出」拼成新的 `context`；verbose 时打印「blocked (attempt x/y), retrying due to: ...」。
- 然后**重跑整个任务**：`with tool_failure_collector() as retry_failures: result = agent.execute_task(task=self, context=context, tools=tools)`。重跑结果按与主流程相同的规则规范成 `raw` / `pydantic_output` / `json_output`，造一个新的 `TaskOutput`，并 `tool_failures=merge_tool_failures(accumulated_failures, retry_failures)`，再把这份合并结果作为下一轮的累积起点。

注意重试分支里的规范化**没有**「有护栏就跳过导出」这条判断（那是主流程 `_execute_core` 里的），因为这里本身就是护栏环节，导出是必要的。

**`process_guardrail` 是护栏调用的统一入口。** 它做三件事：类型检查（输出必须是 `TaskOutput` 或 `LiteAgentOutput`，否则 `TypeError`；护栏非空，否则 `ValueError`）；发 `LLMGuardrailStartedEvent`；调 `guardrail(output)` 并用 `GuardrailResult.from_tuple` 把元组转成结构体；最后发 `LLMGuardrailCompletedEvent`。

异常处理值得单独说：`HookAborted`（钩子拒绝这次判定）与普通 `Exception` 都被单独捕获，**各自补发一个 `success=False` 的完成事件后重新抛出**。注释解释了原因——没有终止事件，「evaluate guardrail」这个追踪 span 会活到执行结束才被当成孤儿关掉，真实的错误信息就丢了。

`GuardrailResult` 有一个 `after` 校验器强制 `result` 与 `error` 互斥：`success=True` 时不能同时带 error，`success=False` 时不能同时带 result。它的 docstring 还解释了一个技术细节——为什么不能在字段级校验：因为字段级校验器看不到在其之后声明的字段，`success=True` 那种情况永远观察不到非空 `error`。

**`_apply_guardrail_result` 的五种返回形态。** 这是护栏能「改写输出」的关键：

- 返回整个 `TaskOutput`：直接替换。这里要做两件补偿——把累积的工具失败合并进新输出（否则「前面的尝试白费了」），以及检查结构化字段是否缺失（`output_pydantic` 已配但 `result.pydantic is None`，或 `output_json` 已配但 `json_dict is None`），缺了就把 `result.raw` 作为待导出值返回。注释给出的理由正是「护栏运行期间输出未导出，一个直接透传的输出可能还没有配置的结构化字段」。
- 返回 `BaseModel`：`task_output.raw = result.model_dump_json()`，并把该模型作为待导出值。
- 返回 `dict`：`raw = json.dumps(result, default=str)`，把 `raw` 作为待导出值。
- 返回 `str`：`raw = result`，`str` 作为待导出值。
- 其他（含 `None`）：不改 `raw`，不导出。

也就是说：**返回值类型决定「改写什么」**——`TaskOutput` 是整体替换，`BaseModel` / `dict` / `str` 只改 `raw` 并触发重新导出。

**`LLMGuardrail` 的裁判实现。** 它内部 `_validate_output` 每次都**现造一个 `Agent`**（角色 `Guardrail Agent`，目标 `Validate the output of the task`，原样复用任务的 `llm`），拼一段提示词把「任务结果」与「护栏标准」并列，要求「只指出问题、不要提出修正」，然后 `agent.kickoff(query, response_format=LLMGuardrailResult)`。返回的 pydantic 若不是 `LLMGuardrailResult` 就抛「The guardrail result is not a valid pydantic model」。`__call__` 里：`valid` 为真返回 `(True, task_output.raw)`；为假返回 `(False, feedback)`；任何异常（除 `HookAborted` 透传）都被转成 `(False, "Error while validating the task output: ...")`。

这个「异常即不通过」的选择很重要：裁判本身挂了（模型超时、返回格式坏掉）不算通过，会占用一次重试配额。而如果配额耗尽，任务就以失败告终。

**两个刻意的隔离。** `_validate_output` 用 `with no_spreadsheet_urls():` 包住裁判调用，注释写明「query 里有任务的输出，那是模型文本，所以里面的 URL 永不抓取」。这是一道针对「模型输出诱导框架去读文件」的防护。`kickoff` 可能返回 coroutine（当前已存在事件循环时），`_run_coroutine_sync` 用「复制 contextvars + 单线程池 + `asyncio.run`」来跑，避免在运行中的 loop 上嵌套调用。

**重试计数是持久的。** `retry_count` 与 `_guardrail_retry_counts` 都存在任务对象上，不会在两次调用之间重置。同一任务在同一进程里被第二次执行时，计数会接着上次的值——这意味着重试配额是**任务级累计**而不是**单次执行**的。

## 关键取舍

**源码事实：** 重试的粒度是「重跑整个任务」而不是「重跑模型调用」。护栏因此可以依赖工具调用产生的真实副作用，但那一次被拦下的模型调用与工具调用的成本都无法回收。

**源码事实：** 单条护栏与列表护栏共存时，列表优先（`guardrails` 非空会把 `guardrail` 与 `_guardrail` 都置 `None`）。这条规则防止同一条判定跑两遍。

**源码事实：** 每条列表护栏各自维护重试计数（`_guardrail_retry_counts[index]`），单条护栏用全局 `retry_count`。所以「三条护栏各重试 3 次」最多可能重跑 9 次任务，而不是共享一份配额。

**源码事实：** 护栏返回 `None` 被视为错误（「Task guardrail returned None as result. This is not allowed.」）。通过就必须给出可用结果，不允许「过了但不告诉你过的是什么」。

**源码事实：** `LLMGuardrail` 的裁判异常被转成「不通过」而不是抛出。模型层的抖动会被消耗成重试次数，最终可能表现为「任务失败于护栏」而不是「任务失败于模型超时」。

**源码事实：** 构造期只校验「必填参数恰好一个」而不校验参数类型，也不强制要求返回注解。这降低了接入成本，但也意味着 `def g(x)` 里 x 实际收到的 `TaskOutput` 类型只能靠约定。

**架构推论：** 护栏被建模成「返回元组」而不是「抛异常」，使「不通过」成为一种正常控制流，能与重试配额自然配合。代价是失败原因只能通过返回值字符串传递，粒度有限。

**架构推论：** 把 LLM 裁判包装成普通 `GuardrailCallable`，让「可编程校验」与「模型校验」在调度侧完全同构。这一抽象层次的选择，使得同一个 `guardrails` 列表里可以混排两种形态。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/task.py` | `validate_guardrail_function` | 构造期校验护栏签名：必填参数恰好一个、返回注解须为 `tuple[bool, ...]` |
| `lib/crewai/src/crewai/task.py` | `ensure_guardrail_is_callable` | 把字符串护栏包成 `LLMGuardrail`，要求有智能体且其 llm 为 `BaseLLM` |
| `lib/crewai/src/crewai/task.py` | `ensure_guardrails_is_list_of_callables` | 归一化列表/单值/字符串/元组四种输入，非空则清空单条护栏 |
| `lib/crewai/src/crewai/task.py` | `_invoke_guardrail_function` | 同步重试循环：判定、套用结果、耗尽抛错、否则重跑任务 |
| `lib/crewai/src/crewai/task.py` | `_apply_guardrail_result` | 按返回类型改写输出（整体替换 / 只改 raw 并补导出） |
| `lib/crewai/src/crewai/utilities/guardrail.py` | `process_guardrail` | 统一调用入口：类型检查、双事件配对、元组转 `GuardrailResult` |
| `lib/crewai/src/crewai/utilities/guardrail.py` | `GuardrailResult` | 结果结构体，强制 `result` 与 `error` 互斥 |
| `lib/crewai/src/crewai/tasks/llm_guardrail.py` | `LLMGuardrail` | 用模型做裁判：现造 `Guardrail Agent`、结构化输出 `valid`/`feedback` |

## 参考

- [CrewAI 源码：task.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/task.py)
- [CrewAI 源码：guardrail.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/utilities/guardrail.py)
- [CrewAI 文档：任务护栏](https://docs.crewai.com/en/concepts/tasks#task-guardrails)

## 与主核 / AI 的关系

护栏的形态与主核里的「约束校验 / 触发器检查」同构：都在写入之前设一道检查，不通过就拒绝。但两者的失败语义截然不同——主核里检查失败即拒绝，事务中止，用户看到错误码；这里检查失败会**触发一次重试**，把失败原因作为新的输入喂回系统，指望下一次产出合格结果。这是从「确定性系统」跨到「概率性系统」时必然要发生的语义变化：既然模型可能这次做对下次做错，正确的策略不是拒绝而是重试。

对 AI 工程的实际意义在于：重试配额的设计决定了系统的成本与可靠性边界。CrewAI 把配额做成「每条护栏独立 + 次数为 N+1」，并且把护栏异常（包括裁判模型自己挂掉）都算作不通过，这使得最坏情况的成本是「任务执行次数 × 护栏条数 × (N+1)」，对长流水线来说这是一个需要认真估算的量。同时它刻意让护栏只做「判定与改写」，不做「自动修复」——修复动作就是重跑任务本身，因此护栏的提示词质量直接决定重试是否能收敛。
