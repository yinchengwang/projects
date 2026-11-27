---
project: smolagents
feature: multi-step-agent
title: 多步智能体主循环
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/agents.py
    symbols: [MultiStepAgent, _step_stream, _handle_max_steps_reached]
  - path: src/smolagents/agents.py
    symbols: [run, _run_stream, interrupt]
---

# 多步智能体主循环

## 一句话职责

`MultiStepAgent` 把「一次运行」定义为若干「步骤」的串行序列：循环只负责推进步号、记录步骤、判定终止，而「一步里做什么」被抽成抽象方法 `_step_stream`，由两个子类分别用代码或 JSON 实现。

## 机制是什么

循环骨架是 ReAct 的老套路，但这套实现把它写成了一个**生成器流水线**：所有中间事件都从 `_run_stream` 里 `yield` 出来，非流式调用只是把生成器抽干后取最后一项。

一次运行由三样东西描述清楚：

| 概念 | 载体 | 说明 |
|---|---|---|
| 步 | `ActionStep`（另有 `PlanningStep`、`TaskStep`、`SystemPromptStep`、`FinalAnswerStep`） | 每一步是一个数据对象，不是回调 |
| 步的产出 | `_step_stream` 生成器 yield 的 `ChatMessageStreamDelta` / `ToolCall` / `ToolOutput` / `ActionOutput` | `ActionOutput.is_final_answer` 为真表示任务完成 |
| 步的记录 | `AgentMemory.steps` 列表 | 循环在 `finally` 里把每步写入，因此**抛错的步也会被记录** |

主循环的四件事：**排计划**（按 `planning_interval`）、**跑一步**、**判终止**、**记账**。它不负责「怎么想」「怎么调工具」——那在子类里。

## 怎么实现的

**`run` 只是 `_run_stream` 的两行包装。** 非流式路径是 `steps = list(self._run_stream(...))`，然后断言 `steps[-1]` 是 `FinalAnswerStep` 并取 `output`。流式路径直接把生成器交出去。所以两条路径的循环逻辑只有一份，`run(stream=True)` 不会走另一套代码。

`run` 在进入循环前做了六件事：把 `task` 存到实例上、把 `interrupt_switch` 归零、把 `additional_args` 塞进 `state` 并把它们的名字拼进任务描述（这样模型才知道能直接用这些变量名）、重建 `SystemPromptStep`、可选地重置 memory 与 monitor、追加 `TaskStep`。最后一句是 `CodeAgent` 专有的：若存在 `python_executor`，就把 `state` 通过 `send_variables` 与 `send_tools({**self.tools, **self.managed_agents})` 同步进去——托管子智能体与普通工具在这一步被同等对待，都变成解释器里的一个可调用名字。

**步号从 1 开始，上限检查在循环条件里而不是循环体里。** `_run_stream` 的结构是 `while not returned_final_answer and self.step_number <= max_steps`。因此 `max_steps=3` 允许 3 次 `_step_stream`，第 4 次进入前就退出。退出后有一个兜底分支：若没拿到最终答案且步号正好等于 `max_steps + 1`，调 `_handle_max_steps_reached`。

**计划步是「插在动作步之前」的独立步骤。** 触发条件是 `self.step_number == 1` 或 `(self.step_number - 1) % planning_interval == 0`，即在第 1 步以及之后每隔 `planning_interval` 步。首次计划用 `is_first_step=len(self.memory.steps) == 1` 判断，注释特意说明「不要用 `step_number` 属性，因为跨多次 `run` 时会残留旧步」。计划步有自己的 `Timing`，也要过 `_finalize_step` 并写入 memory。

**错误分成两类，处置完全不同。** 循环体 `try` 里捕获后分派：

- `AgentGenerationError` —— 注释写明「不是模型的问题，是实现问题」，所以**原样抛出并退出**；
- 其他 `AgentError`（解析失败、执行失败、工具参数错等）—— 归因给模型，**写进 `action_step.error` 然后继续下一轮**。

这是「让模型看到自己的错误再改」的实现基础：失败不是中断，而是下一轮提示词里的一段 Observation。

**`finally` 里做三件事，顺序固定。** `_finalize_step(action_step)`（补 `timing.end_time` 并触发 step callbacks）、`self.memory.steps.append(action_step)`、`yield action_step`，最后 `step_number += 1`。因为 `finally` 在 `yield` 之后仍会执行，所以即使调用方中途 `close()` 生成器，步也会被记账。

**超步数的兜底是「再问一次，不给工具」，而且默认调用方看不到它是兜底。** `_handle_max_steps_reached` 调 `provide_final_answer(task)`——一次不带工具的模型调用，提示词由 `final_answer.pre_messages` + 当前 memory + `final_answer.post_messages` 组成；然后把结果包成一个 `ActionStep`，其 `error` 是 `AgentMaxStepsError`、`action_output` 是这次回答的内容、`token_usage` 也来自这次调用，写入 memory 后返回文本。于是「超步数」在结果上依赖 `RunResult.state == "max_steps_error"` 来表达，而非抛异常。

注意 `state` 这个字段只在 `return_full_result=True` 时才存在：`run` 默认 `return_full_result=False`，此时它直接 `return output`——一个裸的答案值。所以默认用法下**超步数与正常完成在返回值上完全同形**，要区分必须显式打开 `return_full_result` 并读 `state`。这是这一页最容易踩的一处。

另外这段兜底代码里 `yield action_step` yield 的是循环中最后一次赋值的那个局部变量（也就是刚被追加过的那一步），而不是新构造的兜底步——兜底步只进了 memory，没被单独 `yield`。

**`interrupt` 是协作式的中断，只在步边界生效。** `interrupt_switch` 置真后，循环在**下一轮开头**检查并抛 `AgentError("Agent interrupted.")`。所以中断不会打断正在进行的模型调用或工具执行；若要立即停，只能从外部取消任务。

**`step` 是非流式的单步门面。** 它写成了 `list(self._step_stream(memory_step))[-1]`——抽干生成器只取最后一个元素。约定是「最后 yield 的必须是 `ActionOutput`」，于是非流式与流式共享同一份实现，代价是单步模式下所有中间事件都被构造出来又丢掉。

**终止判定只看 `ActionOutput.is_final_answer`。** 循环里对每个 `yield` 出来的 `ActionOutput` 检查该字段，为真则跑 `final_answer_checks`（每个检查函数接收 `(final_answer, memory, agent=self)`，返回假值即抛 `AgentError`），置 `returned_final_answer = True`。校验失败抛出的是 `AgentError`，会被外层当作模型错误吞进 `action_step.error`——也就是说**答案校验不通过不会终止运行，而是让智能体继续尝试**。

**`RunResult` 的用量是事后从 memory 里扫出来的。** `run` 在返回前遍历 `memory.steps`，把所有 `ActionStep` 与 `PlanningStep` 的 `token_usage` 相加；只要有一个是 `None`，整次运行的 `token_usage` 就是 `None`（而不是部分求和）。`step_callbacks` 里给 `ActionStep` 与 `PlanningStep` 注册了 `monitor.update_metrics`，注释说明这是为了「让 monitor 的总量包含计划步的 token，与 `RunResult.token_usage` 一致」。

**提示词模板是 TypedDict 而不是字符串常量。** `PromptTemplates` 分四块：`system_prompt`、`planning`（三个子键）、`managed_agent`（`task`/`report`）、`final_answer`（`pre_messages`/`post_messages`）。构造时若传了自定义模板，`__init__` 会用 `assert` 逐键校验缺失（含嵌套子键），缺了就报错；渲染走 `populate_template`，底层是 Jinja2 且 `undefined=StrictUndefined`——模板里写错变量名会在渲染时炸，而不是静默变空串。

**工具与托管智能体的名字必须全局唯一。** `_validate_tools_and_managed_agents` 把「工具名 + 托管智能体名 + 自身 name」拼成一个列表查重。这一步是必要的，因为 `CodeAgent` 会把它们合并进同一个 `send_tools` 字典，重名会在解释器里直接撞车。

## 关键取舍

**源码事实：** 每一步都是一个 dataclass（`ActionStep` 有 `step_number`/`timing`/`model_input_messages`/`tool_calls`/`error`/`model_output`/`code_action`/`observations`/`action_output`/`token_usage`/`is_final_answer` 共十来个字段），而不是日志行。这使「重放、可视化、序列化导出」都可以直接遍历 memory，也让 step callbacks 能按步骤类型注册。

**源码事实：** 错误分类是「生成错误 vs 其他智能体错误」。前者被视作实现缺陷直接抛，后者被视作模型可自纠的失败写进步骤。这个二分法决定了「哪些失败会被模型看见」，是整套容错的核心。

**源码事实：** 超步数走的是「再问一次」而不是报错，且**默认配置下返回值与正常完成同形**。区分只能靠 `return_full_result=True` 时的 `RunResult.state`。任何只判「有没有输出」的调用方会把兜底答案当成功。

**源码事实：** 计划步与动作步共享同一个步号计数器。`planning_interval=2` 时序列是「计划(1) 动作(1) 动作(2) 计划(3) 动作(3)…」——计划不额外占步号，所以「步数预算」里计划是免费的。

**源码事实：** `final_answer` 工具总是被注入。`_setup_tools` 用 `setdefault("final_answer", FinalAnswerTool())`，即使用户的 `tools` 里已经有同名工具也不会被覆盖，但用户没传时一定有。这个工具的实现就是 `return answer`——它不做任何事，真正的作用是在 `CodeAgent` 里被解释器包一层抛 `FinalAnswerException`（见最终答案一页）。

**架构推论：** 把主循环写成生成器、把单步写成抽象生成器，换来的是「流式与非流式共用一份控制流」。代价是所有状态机细节（什么时候记步、什么时候判终止）都压在生成器的 `yield` 顺序上，读代码时必须顺着 `yield` 走一遍才能确定执行序。

**架构推论：** 因为 agent 自己持有 `memory` 与 `state`，这个框架里的智能体是**有状态且可执行的**——`run`/`__call__` 都在智能体上。这与「执行入口唯一」那一类设计（智能体只是数据、循环在运行器里）是对立的两端：这里编排自由度更高（子类可以改单步内部的一切），但「一次运行」与「一个智能体实例」被绑死，跨运行复用与并发共享需要额外小心。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/agents.py` | `MultiStepAgent` | 主循环、memory、state、工具与模板的持有者 |
| `src/smolagents/agents.py` | `run` | 运行门面：准备状态、流式与否的分派、用量汇总与 `RunResult` 组装 |
| `src/smolagents/agents.py` | `_run_stream` | 真正的循环：排计划、跑一步、判终止、记账 |
| `src/smolagents/agents.py` | `_step_stream` | 抽象单步生成器，两个子类各自实现 |
| `src/smolagents/agents.py` | `step` | 非流式单步门面，抽干生成器取最后一项 |
| `src/smolagents/agents.py` | `_handle_max_steps_reached` | 超步数兜底：不带工具再问一次并写成 `ActionStep` |
| `src/smolagents/agents.py` | `_generate_planning_step` | 计划步生成，首步与周期触发 |
| `src/smolagents/agents.py` | `interrupt` | 置中断标志，仅在步边界生效 |
| `src/smolagents/agents.py` | `_finalize_step` | 补计时并触发 step callbacks |
| `src/smolagents/agents.py` | `_validate_final_answer` | 运行用户提供的答案检查函数 |
| `src/smolagents/agents.py` | `provide_final_answer` | 超步数时的一次额外模型调用 |
| `src/smolagents/agents.py` | `write_memory_to_messages` | memory → 模型消息序列 |
| `src/smolagents/agents.py` | `_setup_tools` | 工具字典化、基础工具注入、`final_answer` 兜底 |
| `src/smolagents/agents.py` | `_validate_tools_and_managed_agents` | 工具名与托管智能体名唯一性校验 |
| `src/smolagents/agents.py` | `RunResult` | 运行结果：输出、状态、步骤、用量、计时 |
| `src/smolagents/agents.py` | `populate_template` | Jinja2 严格未定义渲染 |
| `src/smolagents/memory.py` | `ActionStep` | 一步的完整记录载体 |
| `src/smolagents/memory.py` | `AgentMemory` | 步骤列表与系统提示 |

## 参考

- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 文档：智能体参考](https://huggingface.co/docs/smolagents/reference/agents)
- [smolagents 文档：智能体概念导引](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)
- [smolagents 文档：ReAct 概念](https://huggingface.co/docs/smolagents/conceptual_guides/react)

## 与主核 / AI 的关系

这个主循环与主核的「逐算子执行 + 每个算子返回状态码」在结构上同源：都是一个固定顺序的阶段序列，每阶段产出一个可记录的结果对象，由阶段结果决定继续还是结束。差别在**错误语义**：主核里算子失败意味着整个执行失败（除非有显式的重试或降级），这里失败是循环的输入——把错误写进 Observation，让模型下一轮自己修。

「超步数不抛异常而是返回带状态的正常结果」这一条，与主核里「达到 work_mem 上限改为落盘而不是报错」是同一种产品判断：把预算耗尽当成一种**正常结局**而不是错误，调用方必须显式读状态字段。任何限流/限额系统都要在「抛错」与「返回降级结果」之间选一个，混着做会让上层无法区分「坏」与「没做完」。

对 agent 工程的三条可用结论。第一，**步的定义决定了预算的含义**：这里一步 = 一次模型往返（可能含多次工具调用），而计划步不占步号，所以 `max_steps` 不等于模型调用次数。第二，**错误分类比错误处理更重要**：把「实现 bug」与「模型犯错」分开，才敢让后者自动重试；混在一起的框架只能整段重试，成本与噪声都高。第三，**中间事件流与最终结果是同一份数据**——循环 `yield` 的东西最后又被写进 memory 供下一轮提示词使用，因此「流式给用户看的内容」与「给模型看的内容」天然一致，不需要维护两套格式化逻辑。

选型判断：看一个框架的智能体是否自带 memory 与 `run`。带的，单步可深度定制但实例有状态、共享需谨慎；不带的（智能体是纯数据、循环在运行器里），编排自由度低但实例无状态、天然可并发。
