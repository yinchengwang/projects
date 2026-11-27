---
project: openai-agents
feature: guardrail-gating
title: 护栏门控与并行任务取消
page_type: feature
schema_version: 1
refs:
  - path: src/agents/run_internal/guardrails.py
    symbols: [_InputGuardrailToolGate, input_guardrail_tool_gate_scope, cancel_tools_on_input_guardrail_rejection]
---

# 护栏门控与并行任务取消

## 一句话职责

把「输入护栏比模型调用慢，但模型已经开始并行跑」这类竞态收束为一次 run-local 的门控：一旦输入判定为拒绝，阻止新的工具任务进入、取消仍可取消的工具任务，并让调用端收尾后再传播拒绝异常。

## 机制是什么

门控状态是 `_InputGuardrailToolGate`，每次 Runner 调用都在 `input_guardrail_tool_gate_scope` 里新建一个对象，放进 `ContextVar`，退出时用 token 复原外层值。字段只有三项：

- `should_cancel`：调用端给的「是否要取消并行模型任务」谓词。
- `rejection`：首个触发拒绝的 `InputGuardrailResult`。
- `cancel_tools`：当前工具批次注册的取消回调集合。

三个操作围成闭环：输入护栏触发时 `_reject_input_guardrail_tools` 写入首个拒绝并调用已注册的取消回调；工具调度器调用 `raise_if_input_guardrail_rejected()` 检查门控、拒绝已存在就抛 `InputGuardrailTripwireTriggered`；工具批次进入时通过 `cancel_tools_on_input_guardrail_rejection` 注册取消回调，退出时注销。

注意作用域本身是**一次 Runner 调用共享**而不是全局单例。非流式路径会传入 `should_cancel_parallel_model_task_on_input_guardrail_trip`；流式路径当前用无参数作用域，因此没有这层可配置谓词。

## 怎么实现的

**为什么需要门控。** `run_in_parallel=True` 的输入护栏与模型调用并发。模型调用解析出多个函数工具后，又由 `_FunctionToolBatchExecutor` 同时调度这些工具。若此时输入护栏晚到并触发拒绝，仅仅在最外层抛异常不够：工具任务可能已经运行，甚至已经产生副作用。门控的目的不只是「报告拒绝」，还要尽早让模型任务停止补充工具任务、取消当前批次里尚未进入后置收尾阶段的任务。

**门控只接收第一个拒绝。** `_reject_input_guardrail_tools` 先查 `gate.rejection is None`，若已有人写入就不覆盖。其后调用 `should_cancel()`；若谓词为假则直接返回，不记录门控拒绝也不取消工具。这是 Temporal workflow 的确定性兼容点：`should_cancel_parallel_model_task_on_input_guardrail_trip` 在 workflow 外恒真，在 workflow 内由 `workflow.patched(...)` 决定，旧历史可以继续走旧语义而不因新增取消动作破坏 replay。

**门控隔离在 ContextVar。** `_input_guardrail_tool_gate` 是 `ContextVar[_InputGuardrailToolGate | None]`，不是模块级可变对象。Runner 进入 scope 时 `set` 并持有 token，退出时 `reset(token)`。`asyncio.create_task` 会复制当前 context，所以在 run 的上下文里创建的模型/工具子任务读到的是当前 run 的门控；并发的另一条 Runner 调用拥有不同 gate，不会互相取消。

**工具批次登记取消器。** `_FunctionToolBatchExecutor.execute()` 外围包 `cancel_tools_on_input_guardrail_rejection(self._cancel_tools_for_input_rejection)`。进入时先 `raise_if_input_guardrail_rejected()`：如果 gate 已经拒绝，就不注册、立即抛，防止迟到的工具批次继续入场。gate 存在时把取消回调放入 set；离开时无论成功还是失败，都从 set 移除。set 避免重复登记同一个回调。

**真正取消的是可取消子集。** `_cancel_tools_for_input_rejection` 调 `_partition_pending_tasks()`，按 `task_state.in_post_invoke_phase` 切两组：尚未进入后置阶段的任务加入 `cancellable_tasks`，已进入后置阶段的任务归入另一组。前一组由 `_cancel_tasks_once` 逐个 `.cancel()`，并登记在 `teardown_cancelled_tasks` 防止重叠收尾路径重复取消；后一组不取消，因为它可能正在跑输出护栏、写输出项或完成其他必须收尾的动作。

这个分类意味着取消不是「所有事情立即停住」。如果工具已执行完，正在跑输出护栏，输入拒绝不会中断它——SDK 要保住 post-invoke 清理与结果收尾；如果还在调用工具或等它返回，则请求取消。

**调度器在两个位置检查拒绝。** `_fill_tool_task_slots` 开头先 `raise_if_input_guardrail_rejected()`，然后才创建新工具 task；`_drain_pending_tasks` 每次有一组任务完成后，先记录成功输出与失败，再检查拒绝，之后才重新填满并发槽。两处结合，既不允许拒绝后启动新任务，也会在当前任务完成/取消后的第一个调度边界停止继续投放。

**工具体内还有入口检查。** `_execute_single_tool_body` 在创建 invoke task 之前检查一次 gate（`raise_if_input_guardrail_rejected`），`_invoke_tool_and_run_post_invoke` 在协程开头、真正调用工具之前再检查一次。这样即使任务刚创建就遇到输入护栏拒绝，工具函数体也不会被调用。取消传播还有一个语义区分：如果取消来自外层 teardown，就原样传播；工具自身内部取消则可以按 `failure_error_function` 转成模型可见的工具错误。

**非流式的最终收场由模型任务与护栏任务共同决定。** 非流式 Runner 并发创建 `model_task` 与 `guardrail_task`，再 await `asyncio.gather`。当拿到 `InputGuardrailTripwireTriggered` 时，先等护栏任务自己把兄弟护栏取消并 drain 完。若工具错误已经先被选中，则先完成该错误的有界清理；然后按 `should_cancel_parallel_model_task_on_input_guardrail_trip()` 决定是否取消模型 task（Temporal 旧 replay 可选择保留旧行为）。最终持久化这次输入，并重新抛护栏异常。其它任意异常路径会在 `finally` 中取消并 gather 两个子任务，保留原始异常。

**流式路径的门控时序不一样。** `run_loop` 建立并行输入护栏任务后，模型流继续推进，护栏结果通过 `_input_guardrail_queue` 发给消费者。一旦触发：`_reject_input_guardrail_tools` 先写门控并调用工具批次取消回调；结果进队列，兄弟 guardrail task 被取消并 gather；随后外层 `run_loop_task` 被取消，事件队列收到 `QueueCompleteSentinel`。`input_guardrail_tripwire_triggered_for_stream` 让消费者等待该任务结束并检查 `input_guardrail_results`，确保异常不会在异步清理尚未完成时就越界。

## 关键取舍

**源码事实：** 输入护栏门控是「软中断」：它在调度边界阻止新工具任务、对可取消子集发取消，但不能回滚已经完成的工具副作用，也不会取消已进入 `in_post_invoke_phase` 的任务。

**源码事实：** 取消发生在工具 batch 的层面。并行工具任务共享一个 gate；一个输入护栏触发会调用该 batch 注册的取消回调，把同一批中仍可取消的任务一起取消，而不是只取消某个具体函数。

**源码事实：** 输入护栏拒绝只保留第一个 `rejection`。多个并发护栏同时触发时，结果列表会收集已完成的护栏结果，但门控的取消来源是第一个写入者；不存在「合并多个拒绝原因」的行为。

**源码事实：** `should_cancel` 为假时 `_reject_input_guardrail_tools` 不记录 rejection。这个判断不意味着「护栏不触发」——护栏任务仍然会把 `InputGuardrailTripwireTriggered` 送到 Runner；它只改变是否通过门控取消并行模型/工具工作。

**源码事实：** 流式与非流式路径不完全对称。非流式 Runner 为 gate 传入 Temporal replay 兼容的取消谓词；流式 `run_loop` 建立默认 gate。因而非流式的 Temporal workflow 能按 patch 历史选择兼容取消行为，而流式路径没有这个 predicate 分支。

**架构推论：** 这里的关键不是「异步取消能不能用」，而是「并发任务走到哪个阶段后可以安全取消」。单纯 `task.cancel()` 会把处于输出收尾阶段的工具一起打断，造成可见输出或持久化状态不完整；`in_post_invoke_phase` 就是运行时给这条安全边界加的阶段标记。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/run_internal/guardrails.py` | `_InputGuardrailToolGate` | 单次 Runner 的 rejection 与取消回调登记表 |
| `src/agents/run_internal/guardrails.py` | `input_guardrail_tool_gate_scope` | 用 ContextVar 建立与恢复 run-local gate |
| `src/agents/run_internal/guardrails.py` | `_reject_input_guardrail_tools` | 首个拒绝写 gate、按策略调用取消回调 |
| `src/agents/run_internal/guardrails.py` | `cancel_tools_on_input_guardrail_rejection` | 工具批次注册与注销取消器 |
| `src/agents/run_internal/guardrails.py` | `raise_if_input_guardrail_rejected` | 调度边界发现拒绝后抛专用异常 |
| `src/agents/run_internal/guardrails.py` | `run_input_guardrails_with_queue` | 流式护栏结果入队、触发时取消 siblings 与 run loop |
| `src/agents/run_internal/tool_execution.py` | `_FunctionToolBatchExecutor.execute` | 批次级安装门控取消回调 |
| `src/agents/run_internal/tool_execution.py` | `_partition_pending_tasks` | 将 invoke 与 post-invoke 阶段任务分开 |
| `src/agents/run_internal/tool_execution.py` | `_cancel_tools_for_input_rejection` | 只取消仍处于可取消阶段的任务 |
| `src/agents/run_internal/tool_execution.py` | `_fill_tool_task_slots` / `_drain_pending_tasks` | 投放新工具前、完成一批后检查 gate |
| `src/agents/run_internal/agent_runner_helpers.py` | `should_cancel_parallel_model_task_on_input_guardrail_trip` | Temporal workflow 旧 replay 兼容谓词 |
| `src/agents/run.py` | `input_guardrail_tool_gate_scope` | 非流式 Runner 的 gate 作用域与取消谓词接线 |

## 参考

- [openai-agents-python 源码：run_internal/guardrails.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/guardrails.py)
- [openai-agents-python 源码：run_internal/tool_execution.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/run_internal/tool_execution.py)
- [Agents SDK 文档：Guardrails](https://openai.github.io/openai-agents-python/guardrails/)

## 与主核 / AI 的关系

这层与主核的取消/回滚边界相似：上层请求停止并不代表所有下层动作都能立即撤销，系统必须区分「尚未提交的工作」与「已经进入提交/收尾阶段的工作」。SDK 没有数据库事务可以回滚工具副作用，所以它用 `in_post_invoke_phase` 做的是尽可能保住 SDK 自己的状态收尾，而不是保证外部副作用可逆。

对 AI 工程最实用的三条推论。其一，**输入护栏并行并不等于零成本**：慢护栏期间模型与工具可能已经跑过一段，触发后会取消尚可取消的工作，但已执行的外部操作不会消失。对不可逆工具，真正的安全控制应放在工具级输入护栏或审批门上。其二，**取消边界必须按阶段设计**：`in_post_invoke_phase` 的任务不被 gate 取消，因为输出护栏、结果项提交与 hook 收尾需要完成；自定义工具也应把不可逆副作用与后置日志/清理分开。其三，**流式消费者可能先看到部分事件再收到 tripwire**：调用方不能把「流里出现了工具开始事件」误当作「护栏最终放行」，最终结论要看 `RunResultStreaming` 的完成状态与异常。
