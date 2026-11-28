---
project: openai-agents
feature: tool-guardrails
title: 工具级护栏与行为策略
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tool_guardrails.py
    symbols: [ToolInputGuardrailData, ToolGuardrailFunctionOutput, RejectContentBehavior, RaiseExceptionBehavior]
---

# 工具级护栏与行为策略

## 一句话职责

把每次工具执行切成两个独立的检查点：参数还没进函数体前做输入检查，函数刚返回后做输出检查；每个检查点都能选择「放行」「拒绝但继续」或「拒绝并让运行失败」。

## 机制是什么

工具级护栏只挂在 `FunctionTool` 上，与 agent 级的 `InputGuardrail` / `OutputGuardrail` 是不同体系。参数类型也专门分开：

- `ToolInputGuardrailData`：`context: ToolContext` + `agent`。
- `ToolOutputGuardrailData`：继承前者再加 `output`。
- `ToolGuardrailFunctionOutput`：`output_info`（任意应用数据）+ `behavior`（一个 TypedDict 判别联合）。

`behavior` 只有三种值：

| type | 工具执行 | 返回模型的结果 | 运行是否继续 |
|---|---|---|---|
| `allow` | 按正常路径 | 真正的工具结果 | 继续 |
| `reject_content` | 输入侧不执行；输出侧执行已发生 | 指定拒绝文本替代结果 | 继续 |
| `raise_exception` | 立即中止 | 无正常工具输出 | 运行失败，抛 tripwire 异常 |

`ToolGuardrailFunctionOutput.allow()`、`.reject_content(message)`、`.raise_exception()` 是三种语义的快捷构造器。输入、输出护栏既可同步也可异步；包装类的 `run()` 统一用 `inspect.isawaitable` 处理。

## 怎么实现的

**输入护栏的精确拦截点。** `_execute_single_tool_body` 先排除「nested continuation」（被嵌套 agent 中断后恢复的工具调用），然后标记调用已开始、跑输入护栏，之后才调 `on_tool_start` hooks，再真正创建 `_invoke_tool_and_run_post_invoke` 任务。输入护栏位于函数调用之前，所以拒绝时函数体完全不执行：`reject_content` 让该方法直接返回那条拒绝消息，并把该次工具运行登记进 `schema_bypassed_tool_runs`；后续构造结果项时（`_build_function_tool_results`）看到这个标记，就用 `function_tool_error_output` 把拒绝消息包装成符合工具输出契约的形状，并跳过正常返回值的类型校验与输出 schema 适配。

输入护栏按配置列表顺序串行遍历。每个结果先追加进 `tool_input_guardrail_results`，然后看 `behavior["type"]`：`raise_exception` 抛 `ToolInputGuardrailTripwireTriggered(guardrail, output)`；`reject_content` 返回消息并停止后续护栏；全部 `allow` 才返回 `None` 表示继续执行。也就是说**第一个拒绝就短路**，后面的输入护栏不再跑。

**输出护栏收到的是原始工具结果。** `_invoke_tool_and_run_post_invoke` 在函数执行成功、或错误已按失败策略变成文本后，先检查嵌套 agent 是否仍有待决中断；若没有，就设置 `in_post_invoke_phase=True`，把 `real_result` 交给 `_execute_tool_output_guardrails`。每个输出护栏的 `ToolOutputGuardrailData.output` 都是 `real_result`，不是上一护栏处理后的结果；这是因为输出护栏只做判定，不负责级联变换。

执行侧保留 `final_result`，但 `reject_content` 分支会立即返回拒绝文案并标记 `is_rejection=True`；`raise_exception` 抛 `ToolOutputGuardrailTripwireTriggered`。若所有护栏都放行，最终输出才还是原始 `real_result`。

**拒绝输出如何被写回。** 输出护栏的 `reject_content` 已经在运行了工具之后才发生，因此副作用不可撤销；SDK 做的是把正常输出替换成护栏消息。若工具声明了 `output_json_schema`，该拒绝消息走 `function_tool_error_output(...)` 形状，避免向模型写入一个违反工具协议的裸字符串。该 rejection 也会进入 `schema_bypassed_tool_runs`，后续跳过正常返回值的输出类型校验。

**行为结果也进报告。** 每次运行产生的输入/输出护栏结果会分别收集成 `ToolInputGuardrailResult` 与 `ToolOutputGuardrailResult`，内容是「哪个 guardrail + 它返回的完整 `ToolGuardrailFunctionOutput`」。抛异常时，异常自身带触发的 guardrail 与输出；未触发的 sibling 护栏结论仍通过结果列表留给 `RunResult` / `RunState`。

**护栏能短路审批，但不默认先于审批。** 默认情况下 `needs_approval` 先于工具输入护栏处理：如果调用需要人审批且状态未决，先产出 `ToolApprovalItem` 并中断，输入护栏不会在审批之前运行。`ToolExecutionConfig.pre_approval_tool_input_guardrails=True` 是一个显式选项，会先跑输入护栏；它仍会在批准后、执行前**再跑一次**相同护栏。配置注释强调这点，所以用输入护栏做计费型外部校验时要知道它可能执行两次。

**工具输出护栏不受 input guardrail gate 控制。** 输入护栏的拒绝只负责阻止当前工具批次和模型轮次；输出护栏是工具返回后的判定，属于另一个阶段。它们的异常会交给工具执行器/运行层收尾，不会被解释为「未调用工具」。

**应用回调的同步与异步都合法。** `ToolInputGuardrail.run`、`ToolOutputGuardrail.run` 先检查 callable，再直接调用，若结果 `inspect.isawaitable` 就 `await`，否则直接返回。装饰器既可 `@tool_input_guardrail` 裸用，也可 `@tool_input_guardrail(name="...")` 带参数；未显式传名称时由函数的 `__name__` 命名。

## 关键取舍

**源码事实：** `reject_content` 在输入侧表示「不执行工具」，在输出侧表示「工具已经执行，但不把真实结果给模型」。同一个行为名、两个完全不同的副作用边界；输出侧拒绝不能回滚工具函数造成的状态变化。

**源码事实：** 同一方向的 guardrail 按列表顺序串行执行，不并行。与 agent 级护栏的并行执行不同，这里允许前一个护栏拒绝后立即停止后续检查，避免拒绝后仍花钱跑剩下的护栏。

**源码事实：** 工具输入护栏的 `raise_exception` 与 `reject_content` 是两条不同错误路径：前者异常向上冒泡、工具批次失败；后者生成普通的 function-call output 继续下一轮模型推理。它们不是「异常 vs 更温和的异常」，而是「失败终止 vs 对话控制流」。

**源码事实：** 输出护栏一律看到原始 `real_result`。它们不串联输出变换，因此多个输出护栏并不是「前一个过滤结果交给后一个再过滤」，而是每个都对同一个原结果单独作判断。

**源码事实：** 输出 `reject_content` 只改模型可见输出、不撤销副作用；如工具发送邮件、写数据库后才被输出护栏拦住，外部动作已经发生。这是调用方必须承担的业务语义。

**源码事实：** 审批前输入护栏默认关闭，且显式开启后批准前与执行前各跑一次。这是把「审批前预检」与「执行前安全检查」分离：一次的结论不能替代执行时的再检查，但会使纯函数以外的护栏重复产生副作用。

**架构推论：** 工具级护栏的双切点类似数据库里的「写前约束」与「提交后结果验证」。写前拒绝能保证动作未发生；写后拒绝只能保护下游消费者，不能保证世界没变。判断护栏应该放在哪边，先看要保护的是「动作本身」还是「模型看到的内容」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tool_guardrails.py` | `ToolInputGuardrailData` | 输入检查上下文：工具调用上下文 + agent |
| `src/agents/tool_guardrails.py` | `ToolOutputGuardrailData` | 输出检查上下文：再加工具输出 |
| `src/agents/tool_guardrails.py` | `ToolGuardrailFunctionOutput` | 三策略结果对象与快捷构造器 |
| `src/agents/tool_guardrails.py` | `RejectContentBehavior` | 拒绝内容但继续运行的判别项 |
| `src/agents/tool_guardrails.py` | `RaiseExceptionBehavior` | 抛专用异常中止的判别项 |
| `src/agents/tool_guardrails.py` | `ToolInputGuardrail` / `ToolOutputGuardrail` | 同步/异步回调的统一包装 |
| `src/agents/run_internal/tool_execution.py` | `_execute_tool_input_guardrails` | 执行前检查：按顺序运行、拒绝短路或抛异常 |
| `src/agents/run_internal/tool_execution.py` | `_execute_tool_output_guardrails` | 执行后检查：原结果传递、拒绝替换或抛异常 |
| `src/agents/run_internal/tool_execution.py` | `_execute_single_tool_body` | 执行前护栏相对 hooks 与函数体的精确次序 |
| `src/agents/run_internal/tool_execution.py` | `_invoke_tool_and_run_post_invoke` | 工具函数返回后调用输出护栏并产出结果项 |
| `src/agents/run_config.py` | `ToolExecutionConfig.pre_approval_tool_input_guardrails` | 审批前预检开关，开启后输入护栏会在执行前再跑一次 |
| `src/agents/exceptions.py` | `ToolInputGuardrailTripwireTriggered` / `ToolOutputGuardrailTripwireTriggered` | 两个阶段各自的 tripwire 异常 |

## 参考

- [openai-agents-python 源码：tool_guardrails.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tool_guardrails.py)
- [openai-agents-python 源码：run_internal/tool_execution.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/tool_execution.py)
- [Agents SDK 文档：Tool Guardrails](https://openai.github.io/openai-agents-python/guardrails/)

## 与主核 / AI 的关系

工具级护栏与主核的「约束检查必须位于副作用边界之前」同构。输入护栏在工具函数之前，有机会阻止副作用；输出护栏在之后，只能把结果从后续上下文中隔离。主核里写前约束失败会中止事务，写后检查失败可以回滚；这里则没有通用回滚，所以输出护栏的拒绝只能隐藏结果，不能撤销副作用。

对 agent 工程最实用的三条推论。其一，**安全的危险动作优先用输入护栏**，把「能不能执行」放在函数体之前；输出护栏用于发现敏感结果、格式不合规结果等内容问题，而不要拿它当副作用的安全阀。其二，**避免护栏函数本身有副作用**，特别是 `pre_approval_tool_input_guardrails=True` 时同一函数可能在审批前和执行前运行两遍。其三，**要区分 reject 与 tripwire**：reject 是给模型一条输出、让模型重新决策；tripwire 是让整个执行链失败。对「拒绝后让模型换方案」的场景用前者，对「绝不允许继续」的场景用后者。
