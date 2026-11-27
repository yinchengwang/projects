---
project: pydantic-ai
feature: tool-validation
title: 工具参数校验与重试
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/exceptions.py
    symbols: [ModelRetry, ToolFailed, SkipToolValidation]
  - path: pydantic_ai_slim/pydantic_ai/_tool_execution.py
    symbols: [_ToolCallProcessor, tool_bound_retry_part, build_tool_return_part]
---

# 工具参数校验与重试

## 一句话职责

校验失败与执行失败被刻意分成两条路：前者变成「可重试的模型反馈」，由重试预算限量；后者要么同样可重试、要么变成终态失败——而这两种异常的选择，就是工具作者对「模型该再试一次还是该换个做法」的表态。

## 机制是什么

工具调用的失败被归成三类异常，语义各不相同：

| 异常 | 模型看到 | 消耗重试预算 | 带重试指令 |
|---|---|---|---|
| `ModelRetry` | 一条工具返回 | 是 | 是 |
| `ToolFailed` | 一条失败的工具返回 | 否 | 否 |
| `ValidationError`（schema 校验失败） | 同上，按 `ModelRetry` 处理 | 是 | 是 |

`ModelRetry` 的 docstring 说得很直接：可以从工具函数、输出校验器、以及能力钩子（`after_model_request`、`after_tool_execute` 等）里抛出，要求模型改正再试。`ToolFailed` 的对照描述是「调用已经结束并且失败了——缺资源、不支持的操作、确定的上游错误」，模型应该看见并适应，而不是重复同一个调用；它不预置修正指令，也不消耗预算，所以重复失败的边界应当用运行级的 `UsageLimits` 而不是重试次数来控制。

`SkipToolValidation` 是第三条路：在校验前/包裹钩子里抛出它，就跳过校验，直接用它携带的 `validated_args` 当作已校验参数。

## 怎么实现的

**校验的两段结构。** 参数先过 Pydantic 的 schema 校验（`validator.validate_json` 或 `validate_python`，取决于原始参数是字符串还是 dict；流式部分响应时 `allow_partial` 映射成 `'trailing-strings'`），再跑工具自定义的 `args_validator_func`。两者都可能失败，但前者产生 `ValidationError`，后者按约定抛 `ModelRetry` 或 `ToolFailed`。

**错误如何变成「模型能读的历史」。** 这是 `_tool_execution` 的核心职责之一。`ToolRetryError` 与 `ToolFailedError` 都是携带 `ToolReturnPart` 的包装异常：

- `ToolRetryError` 携带的是**重试型**返回（`build_retried_tool_return` 构造，`outcome='retried'`，带修正指令）；
- `ToolFailedError` 携带的是**失败型**返回（`outcome='failed'`，只有消息，无指令）。

`build_tool_return_part` 负责把「工具正常返回的值」翻译成规范的返回部件，并顺带做三件校验：工具返回 `ToolDenied` 时转成 `outcome='denied'` 的部件；返回列表里嵌着 `ToolReturn` 时报错（提示「应当直接用 `ToolReturn`」）；`ToolReturn.tools` 不是字符串列表时报错。这些检查放在这里而不是工具实现里，是因为**所有工具返回路径都经过它**。

`tool_bound_retry_part` 是个小断言工具：它断言错误携带的一定是 `ToolReturnPart` 而不是无主的 `RetryFeedbackPart`。注释解释了为什么这是不变量：处理一次调用时产生的每个重试都是从那次调用构造的，所以不可能无主。这个断言的意义在于，一旦某条路径构造了无主的重试，它会在派发处立刻炸掉而不是静默传给模型。

**重试预算的三处记账。** 预算按工具名计数，存在 `ctx.retries`，上限来自 `ToolsetTool.max_retries`（其回退链是：工具自己的 → 工具集的 → `ctx.max_retries`）。

1. **检查**：`_check_max_retries` 用 `retries >= max_retries` 判断，超过就抛 `UnexpectedModelBehavior`，文案带一个指向重试文档的链接；注释说明为什么用 `>=` 而非 `==`——负预算也必须立刻抛，否则永远等不到。
2. **打标**：`_record_attempt` 把「这次是第几次重试、上限多少」写回重试部件（`part.retry`），让历史里能看出进度。
3. **结转**：`ToolManager.for_run_step` 跨步时把「本步失败过的工具」计数 +1、把成功过的清零。

**输出工具的重试预算是另一套。** 输出工具走 `max_output_retries`，且预算的累加被推迟到批次结束（`output_retries_increment` 在并行批次里累加，最后一次性写回 `ctx.state.output_retries_used`）。注释说明了为什么要推迟：并行的输出任务若各自直接写计数器就会竞态。

还有一个专门的判断：输出工具校验失败时，先调 `check_incomplete_tool_call(call)`。若参数被截断（模型撞了输出 token 上限），直接抛 `IncompleteToolCall` 而不是重试——注释写明理由：「用同样的 token 上限再提示一次，只会让参数再被截断一次」。

**retry-wins：函数工具的失败可以推翻已成立的输出。** 这是这套校验机制里最反直觉的一条规则。若本条模型响应里，一个**真正的函数工具**（`kind='function'`）产生了重试结果，那么即使某个输出工具已经成功、已确定最终结果，那个输出也要被作废——注释说这是为了让模型下一轮先去处理这个重试。

`_is_retry_wins_trigger` 把判据集中在一个谓词里，供「按顺序执行的三条策略」与「并行穷举策略」共用：

- 只有 `kind == 'function'` 且部件是 `outcome == 'retried'` 的返回才算触发。
- `unknown`（模型幻觉出的工具名）的重试**不**触发——注释说它们「不是必须完成才能让输出有效的工作」。
- 通过 `RunContext.submit_output` 提交的输出被拒绝时不算触发，因为它是一次失败的输出而不是失败的函数调用。

`_apply_retry_wins` 的实现很务实：它记着「获胜输出的状态部件」的直接引用（`winning_output_part`），于是替换时只需 `output_parts.index(...)` 一次定位，用内容字符串扫描会更脆。替换的内容是 `RETRY_WINS` 这条固定文案，同时把 `final_result` 置空，让下一步继续对话。若最终结果是在外部设定好的（`run_stream`），则不适用——流式输出已经交给消费者了。

**三种执行策略与校验的关系。** `_ToolCallProcessor` 是三种 `end_strategy` 的共同基类，校验与重试的处理在基类里，策略只决定「跑哪些、什么顺序」：

- `early`：先跑全部输出工具；只有全部输出都失败后才跑函数工具。
- `graceful`：按模型发出的顺序走，遇到输出工具前先把挂起的函数调用批次跑掉。
- `exhaustive`：函数与输出一起并行，只被 `sequential=True` 的屏障分段。

三者的校验都是**先整体校验本批函数调用再执行**（`_validate_function_calls` 逐个发 `FunctionToolCallEvent(args_valid=...)`，把结果收进 `validated_calls`），只有 `exhaustive` 额外要求按索引回填结果。

`_duplicate_tool_call_ids` 在这里承担一道失败关闭（fail closed）的防线：函数调用与它的结果/执行是按 `tool_call_id` 匹配回来的，重复 id 会让绑定有歧义，所以在**任何调用开始执行前**就抛 `UnexpectedModelBehavior`。注释还说明「批次之间独立绑定」，所以 `graceful` 在输出调用处切开的两批各自可以重复 id。

**恢复路径（resume）的特殊处理。** 当带着 `tool_call_results` 恢复时，`executable_function_kinds` 从 `('function', 'unknown')` 扩成四种（多出 `external` 与 `unapproved`），因为它们的调用结果已经由调用方提供，走常规流水线即可。同时有两道校验：重复 id 直接拒绝；结果集合必须满足 `eligible_call_ids <= result_tool_call_ids <= response_tool_call_ids`，即「每个可执行的调用都要有结果，但不能多出响应里根本没有的 id」。`'skip'` 这个哨兵表示「上一步已处理过」。

恢复时若某结果不是 `ToolApproved`（例如 `ToolDenied`、`ModelRetry`），`_call_tool` 会在校验之前就短路：`ToolDenied` 直接当结果返回，`ModelRetry` 重新抛成 `ToolRetryError`，`ToolFailed` 抛成 `ToolFailedError`。这解释了为什么 `_validate_function_calls` 对这种调用只发一个不带 `args_valid` 的事件——校验不需要发生。

**`SkipToolValidation` 与 `SkipToolExecution` 是两个不同阶段的逃逸通道。** 前者在校验阶段接受调用方给的 `validated_args`；后者在执行阶段直接用调用方给的 `result`，并**撤销**该调用此前提交的输出（`submitter.discard_submission`），注释写明了理由：「只有真正跑过的调用，才能让它提交的输出成为最终结果」。

## 关键取舍

**源码事实：** `ModelRetry` 与 `ToolFailed` 的差异不止文案，还包括「是否消耗预算」。这是把「模型该再试还是该改道」的判断权交给工具作者，而不是由框架统一策略。

**源码事实：** 重试计数用 `>=` 判断，负预算立即失败。这是一个防御性设计，防止配置错误导致死循环。

**源码事实：** 函数工具的重试可以推翻已成功的输出（retry-wins），但 `unknown` 工具的重试不行，被拒绝的 `submit_output` 也不行。三条边界各对应一类「这次失败不代表还有工作要做」的情形。

**源码事实：** 重复 `tool_call_id` 是拒绝执行的硬错误，而不是尽力而为的绑定。因为按 id 回填结果无法消歧。

**源码事实：** 截断的参数不重试。撞上输出 token 上限的参数，再提示一次只会再截断一次。

**架构推论：** 把「校验」与「执行」拆成两次返回值（`ValidatedToolCall` 与最终结果），使事件流能在**执行之前**就报告 `args_valid`。这对 UI 很重要：用户需要在工具真正跑起来之前就看到「参数是否合法」的状态。

**架构推论：** 重试部件承载的修正指令让「失败」成为可学习信号。这也是为什么 `ToolFailed` 特意不预置指令——它的语义是「这条路走不通」，若还附上「请改正参数」会把模型引向错误的方向。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/exceptions.py` | `ModelRetry` | 可重试反馈：消耗预算、带修正指令 |
| `pydantic_ai_slim/pydantic_ai/exceptions.py` | `ToolFailed` | 终态失败：不消耗预算、无修正指令 |
| `pydantic_ai_slim/pydantic_ai/exceptions.py` | `SkipToolValidation` | 校验阶段逃逸：用调用方给的参数 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_ToolCallProcessor` | 三种策略的基类：校验、执行、重试记账、延迟收集 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `tool_bound_retry_part` | 断言重试部件一定绑在具体调用上 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `build_tool_return_part` | 工具返回值 → 规范历史部件，并校验 `ToolReturn` 形态 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_duplicate_tool_call_ids` | 重复 id 检测，执行前的失败关闭 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_is_retry_wins_trigger` | 判定哪类重试能推翻已成立的输出 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_run_output_tool_call` | 输出工具校验/执行，含截断判定与延迟预算累加 |

## 参考

- [pydantic-ai 源码：_tool_execution.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_tool_execution.py)
- [pydantic-ai 源码：exceptions.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/exceptions.py)
- [Pydantic AI 文档：工具重试](https://ai.pydantic.dev/tools-toolsets/tools-advanced/#tool-retries)
- [Pydantic AI 文档：工具](https://ai.pydantic.dev/tools/)

## 与主核 / AI 的关系

这套「可重试错误 vs 终态错误」的二分，与主核里的事务回滚 vs 硬错误是同一种判断：前者是「这次尝试失败，重来一次可能成功」，后者是「状态已经不可用，继续没有意义」。两边都必须显式区分，否则要么把可恢复的错误当致命错误（用户看到无意义的失败），要么把致命错误当可重试（陷入死循环）。

主核里对应「重试预算」的东西是重试次数与退避策略，且预算通常属于连接或语句级；这里预算属于**工具名**，并在步之间结转。粒度选择反映了一个事实：模型对同一个工具连续尝试是常见模式，而跨步的连续失败才是真正需要限量的东西。

最值得对照的是 retry-wins 这条规则。它相当于说「一个已经提交但另一个分支失败的结果不算数」——在主核里这类似「并行执行的分支里有一个失败，整个语句必须重来」。区别是主核的原子性由事务保证，这里的「作废」只是把历史里的状态部件改成一句固定文案。这个做法很轻，但它必须精确：所以源码才用「记住部件对象的引用」而不是字符串匹配来定位。

对 AI 工程的实际意义在于：工具重试是 agent 里最容易做成「无限循环」的地方。这套实现给出的三条防线很有参考价值——用 `>=` 判预算以防配置错误、用 `ToolFailed` 给作者一条「明确失败」的出口、用 `IncompleteToolCall` 把「不可能成功」的重试直接掐掉而不是让它空转一次。
