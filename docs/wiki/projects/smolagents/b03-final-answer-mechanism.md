---
project: smolagents
feature: final-answer-mechanism
title: 最终答案的表达与提取
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/local_python_executor.py
    symbols: [FinalAnswerException, fix_final_answer_code, CodeOutput]
  - path: src/smolagents/agents.py
    symbols: [_validate_final_answer, provide_final_answer]
---

# 最终答案的表达与提取

## 一句话职责

在代码式智能体里，「任务完成」不是一个返回值而是一个**协议**：模型必须调用 `final_answer(...)`，这个调用在解释器里被换成「抛一个继承自 `BaseException` 的异常」来短路执行，异常携带的值就是答案，被 `CodeOutput` 带回主循环。

## 机制是什么

为什么不能只靠「最后一行代码的值」当答案？因为模型写的是多语句代码，最后一句可能是 `print`、可能是赋值、可能是循环。需要一个**显式的、不可被忽略的**终止信号。

三条路本可以选：

| 方案 | 问题 |
|---|---|
| 返回值约定（最后一条语句的值即答案） | 模型无法表达「我算完了」，只能靠位置猜 |
| 特殊返回值对象 | 模型写的 `try/except` 或正常流程容易把它吃掉/覆盖 |
| **抛异常短路** | 无法被普通 `except Exception` 捕获，控制流确定 |

实现选了第三种。协议由三个部件组成：`final_answer` 工具（回答什么）、`FinalAnswerException`（怎么短路）、`fix_final_answer_code`（模型写错时怎么救）。

## 怎么实现的

**包装发生在执行器入口，不在工具定义里。** `evaluate_python_code` 在开始求值前检查 `if "final_answer" in static_tools:`，把原来的可调用对象存成 `previous_final_answer`，然后替换成：

```
def final_answer(*args, **kwargs):
    raise FinalAnswerException(previous_final_answer(*args, **kwargs))
```

注意参数是 `*args, **kwargs`——注释写的是「Allow arbitrary arguments to be passed」。这样模型多传、少传、用关键字传都不会因为签名不匹配而失败。**先执行原函数拿值，再抛异常**：所以 `FinalAnswerTool.forward`（实现就是 `return answer`）仍会被调用，返回什么就被包进异常。

这个替换只作用于 `static_tools` 的**副本**（`static_tools = static_tools.copy()`），因此不会污染执行器实例上的工具表。

**异常继承 `BaseException` 是这套协议的支点。** `FinalAnswerException(BaseException)` 的 docstring 直写理由：「继承 `BaseException` 而不是 `Exception`，以防被智能体生成代码里的通用 `except Exception` 子句捕获」。这带来两个可见后果：

- 模型写的 `try: final_answer(x) except Exception: ...` 拦不住它，答案不会丢；
- 解释器的 `evaluate_try` 只捕 `Exception`，所以它也不会被当作普通异常处理——这一点在实现上是**必要配合**，否则协议失效。

**短路后仍会收尾打印日志。** `_execute_code` 的结构是三个出口都先做同一件事：

```
state["_print_outputs"].value = truncate_content(str(state["_print_outputs"]), max_length=max_print_outputs_length)
```

正常结束的出口返回 `(result, False)`；`FinalAnswerException` 的出口返回 `(e.value, True)`；其它 `Exception` 的出口先收尾打印再抛 `InterpreterError`，消息里带上**出错那一行的源码**（`ast.get_source_segment(code, node)` 与异常类型）。所以在最终答案之前的 `print` 内容不会因为短路而丢失——它是 Observation 的一部分。

**`CodeOutput` 是这三元组的定型。** dataclass 三个字段：`output`（答案或最后一个语句的值）、`logs`（截断后的打印文本）、`is_final_answer`（是否由 `final_answer` 触发）。`LocalPythonExecutor.__call__` 把 `evaluate_python_code` 的二元组与 `str(self.state["_print_outputs"])` 拼成它返回。主循环只看 `is_final_answer` 与 `output`，`logs` 进 Observation。

**`fix_final_answer_code` 处理一种具体的模型错误：把工具名当变量赋值。** 模型常写出 `final_answer = "结果"`（以为在设置答案）或 `final_answer = compute()`，这会把 `static_tools` 里这个名字……其实不会——`set_value` 会直接抛「Cannot assign to name 'final_answer': doing this would erase the existing tool!」。为了让这种写法不至于整段失败，执行前先做一次文本改写。

改写有严格的前置条件：`if "final_answer(" not in code or not re.search(assignment_pattern, code): return code`。也就是说**只有当代码里既有 `final_answer(` 调用、又有 `final_answer =` 赋值形态时才动手**。源码注释解释了为什么：若代码里根本没调用这个工具，做替换反而会误导模型对下一步的记忆——「Let's not modify the code and leave the subsequent assignment error happen」。

两轮替换：

1. `(?<!\.)(?<!\w)(\bfinal_answer)(\s*=)` → `final_answer_variable\2`（只在赋值左侧改名，负向后行断言挡住 `obj.final_answer =` 这种属性赋值）；
2. `(?<!\.)(?<!\w)(\bfinal_answer\b)(?!\s*\()` → `final_answer_variable`（把其余**非调用**的用法也改名，负向前瞻挡住 `final_answer(`）。

先替赋值、后替引用，顺序不能反：若先替引用，`final_answer = ...` 那行的左侧会被上一轮的 `(?!\s*\()` 判断为「非调用」而改名成 `final_answer_variable = ...`，第二轮就没东西可切了——实际结果是两种顺序都能到达同一终点，但分两步写可以让两条正则各自简单、各自可读。

副作用要说清：这个函数是**纯文本改**，不做 AST 分析。所以字符串字面量里出现的 `final_answer` 也会被改（比如 `print("final_answer = 结果")`）。这是一个已知的粗糙处，代价换来了「不需要先解析成功才能救」。

**`_validate_final_answer` 让答案校验失败可继续。** 主循环拿到 `is_final_answer` 为真后，若 `self.final_answer_checks` 非空就逐个调用：每个函数签名是 `(final_answer, memory, agent=self)`，返回假值则抛 `AgentError(f"Check {name} failed with error: {e}")`。按主循环的错误分类，`AgentError` 会被写进 `action_step.error` 然后**继续下一轮**——所以校验失败不终止运行，而是让智能体看到「你的答案没通过」再试。

这里有一处必须点出的实现细节：框架判断校验结果用的是 **`assert` 语句**——`assert check_function(final_answer, self.memory, agent=self)`。而 `assert` 在 `python -O` / `PYTHONOPTIMIZE` 下会被整体移除，于是**校验函数根本不会被调用**，校验静默失效。实测确认：同一段带 `assert` 的校验逻辑在默认解释器下返回失败、在 `-O` 下直接放行。生产构建里用优化模式跑这个框架，「答案校验」这一层等于不存在。

**`provide_final_answer` 是另一条独立的「取答案」路径，与协议无关。** 它在**超过步数上限**时被调用（`_handle_max_steps_reached`），做法是拼三条消息发给模型：

1. `SYSTEM` 角色，内容为 `final_answer.pre_messages`；
2. `write_memory_to_messages()[1:]`——**跳过第一条**，即丢掉原本的系统提示，只保留对话历史；
3. `USER` 角色，内容为 `final_answer.post_messages` 渲染（带 `task` 变量）。

然后用 `self.model.generate(messages)` 拿回答。注意这次调用**不带 `stop_sequences`、不带 `tools_to_call_from`**——不给工具，也不做格式约束，是一次纯粹的「根据已有记录总结出答案」。异常处理很软：任何异常都被吞掉，返回一条内容为 `"Error in generating final LLM output: {e}"` 的 `ChatMessage`，于是「超步数兜底」自身失败也不会再抛。

**这条路径的答案不经过 `FinalAnswerException`。** `_handle_max_steps_reached` 直接把 `final_answer.content` 塞进 `action_step.action_output`，再返回它。也就是说超步数生成的答案走的是「文本兜底」通道，`is_final_answer` 保持 `False`——主循环是靠「步数耗尽」而非「拿到最终答案」退出的，而 `RunResult.state` 因此是 `"max_steps_error"`。

**答案的类型化发生在最后一步。** `_run_stream` 末尾 `FinalAnswerStep(handle_agent_output_types(final_answer))`：若智能体声明了 `output_type`，按类型映射解码；否则按值得类型猜——`str` → `AgentText`、`PIL.Image` → `AgentImage`、`torch.Tensor` → `AgentAudio`（`torch` 未装则跳过该分支），其余原样返回。所以同一个 `final_answer("42")` 在不同 `output_type` 配置下会被包成不同的 Agent 类型。

**两个智能体的协议实现不同，但收口一致。** `ToolCallingAgent` 里 `is_final_answer = tool_name == "final_answer"`（纯名字比较，答案就是工具返回值，可被 state 变量名替换）；`CodeAgent` 里 `is_final_answer` 来自 `CodeOutput.is_final_answer`（异常触发）。两条路最终都产出 `ActionOutput(output, is_final_answer)` 交给同一个主循环判定。

## 关键取舍

**源码事实：** `FinalAnswerException` 继承 `BaseException` 而非 `Exception`，docstring 写明目的是不被用户代码的 `except Exception` 捕获。配套前提是解释器的 `evaluate_try` 也只捕 `Exception`——两个决定必须成对存在。

**源码事实：** 替换 `final_answer` 时用 `*args, **kwargs` 接收任意参数，且**先调原函数再抛异常**。因此 `FinalAnswerTool` 的 `forward` 一定被调用，返回值得以进入异常。

**源码事实：** `fix_final_answer_code` 只在「既调用了 `final_answer(`、又有 `final_answer =` 赋值」时生效，否则原样返回。源码注释给了理由：没有调用时改动反而会污染模型对下一步的记忆。

**源码事实：** `fix_final_answer_code` 是纯正则文本改写，不做 AST 分析，所以字符串字面量里的 `final_answer` 也会被改名。它也有负向断言挡属性赋值（`obj.final_answer =`）与方法调用。

**源码事实：** `_validate_final_answer` 抛的是 `AgentError`，按主循环分类会被吞进 `action_step.error` 并继续运行。所以「答案校验」在这套实现里的语义是**让模型重试**，不是拒绝交付。

**源码事实：** `provide_final_answer` 在超步数路径上不给工具、不加停止序列、吞掉所有异常。它产出的答案 `is_final_answer` 为 `False`，`RunResult.state` 为 `"max_steps_error"`。

**源码事实：** 短路前的 `print` 输出会被收尾进 `logs`（截断到 `max_print_outputs_length`），三个出口都做这件事。

**架构推论：** 「答案 = 抛异常」把终止条件变成了控制流，好处是**任何位置**都能立刻结束（深层循环、嵌套函数里都行），坏处是「答案」与「错误」共享同一条传播路径，必须靠继承层级（`BaseException` vs `Exception`）来区分。这是这套协议唯一真正脆弱的地方：任何用 `except BaseException` 的用户代码或包装层都会破坏它。

**架构推论：** 两条取答案路径（协议触发 / 超步数兜底）产出的东西质量不同：前者受提示词格式约束、有类型化、可校验；后者是「再问一次」的自由文本、不校验、状态标为错误。用法上必须区分 `result.state`，否则「兜底答案」会被当成正常结果。

**架构推论：** `fix_final_answer_code` 存在本身就说明一件事——「让模型用代码表达答案」的失败模式里，最常见的是**模型把工具名当变量用**。任何自定义 DSL/协议的 agent 系统都会遇到同类问题，专门的修复函数比反复改提示词更有效。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/local_python_executor.py` | `FinalAnswerException` | 继承 `BaseException` 的答案载体 |
| `src/smolagents/local_python_executor.py` | `evaluate_python_code` | 包装 `final_answer`、三出口收尾、拼出答案二元组 |
| `src/smolagents/local_python_executor.py` | `fix_final_answer_code` | 把对 `final_answer` 的赋值改名为 `final_answer_variable` |
| `src/smolagents/local_python_executor.py` | `CodeOutput` | 执行结果定型：输出值、打印日志、是否最终答案 |
| `src/smolagents/local_python_executor.py` | `LocalPythonExecutor.__call__` | 调求值函数并组装 `CodeOutput` |
| `src/smolagents/local_python_executor.py` | `evaluate_try` | 只捕 `Exception`，因此不会截获答案异常 |
| `src/smolagents/local_python_executor.py` | `set_value` | 赋值到工具名时抛错，是 `fix_final_answer_code` 存在的背景 |
| `src/smolagents/agents.py` | `_validate_final_answer` | 运行用户答案校验函数，失败抛 `AgentError` 让模型重试 |
| `src/smolagents/agents.py` | `provide_final_answer` | 超步数兜底：不带工具再问一次，吞掉所有异常 |
| `src/smolagents/agents.py` | `_handle_max_steps_reached` | 把兜底回答写成带 `AgentMaxStepsError` 的 `ActionStep` |
| `src/smolagents/agents.py` | `_run_stream` 的终止判定 | 见 `ActionOutput.is_final_answer` 即跑校验并停止 |
| `src/smolagents/agents.py` | `CodeAgent._step_stream` | 用 `code_output.is_final_answer` 产出 `ActionOutput` |
| `src/smolagents/agents.py` | `ToolCallingAgent.process_tool_calls` | 用工具名比较判定最终答案，支持 state 变量名替换 |
| `src/smolagents/agent_types.py` | `handle_agent_output_types` | 终结答案的类型化包装 |

## 参考

- [smolagents 源码：local_python_executor.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/local_python_executor.py)
- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 文档：智能体参考](https://huggingface.co/docs/smolagents/reference/agents)
- [smolagents 文档：代码执行安全](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)

## 与主核 / AI 的关系

「用异常短路来表示成功完成」在主核里也有对应物：查询执行器里「结果集已满」「限流触发」这类终止条件，常常也靠一个内部控制流（长跳转、状态码、或专门的异常）从深层算子直接回到顶层，而不是层层返回布尔。两者的共同判断是：**深层嵌套里最可靠的终止方式，是让控制流自己往上跑**。

三条对 agent 工程有价值的结论。第一，**区分「完成」与「出错」要靠类型层级**，不能靠消息文本；用 `BaseException` 是这一步的关键，但它同时也要求整条链路上没人粗暴捕 `BaseException`。第二，**答案协议要有「修复层」**：模型把工具名当变量用是高频错误，`fix_final_answer_code` 这种针对性正则是很划算的投入，但必须写清「只做文本改写、可能误伤字符串字面量」。第三，**兜底路径必须与正常路径在结果对象上可区分**：这里用 `state="max_steps_error"` 与 `is_final_answer=False` 双标记，任何只判「有没有输出」的调用方都会把兜底当成功。

还有一条容易被忽略的工程教训：`_validate_final_answer` 抛 `AgentError` 会被主循环吞掉并继续，所以「答案校验」这个功能名听起来像拦截，实际行为是**重试提示**。给机制命名时若与实际控制流不符，使用者一定会误判其强度。同样地，用 `assert` 写校验函数在 `python -O` 下会静默失效——这类「校验器用断言实现」的写法在任何生产系统里都是隐患。
