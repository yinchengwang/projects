---
project: smolagents
feature: code-agent
title: 代码式智能体
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/agents.py
    symbols: [CodeAgent]
  - path: src/smolagents/utils.py
    symbols: [parse_code_blobs, extract_code_from_text]
---

# 代码式智能体

## 一句话职责

`CodeAgent` 把「动作」从 JSON 调用换成一段 Python 代码：模型被要求按代码块格式输出，智能体用正则抠出代码交给解释器执行，执行结果与打印日志合并成 Observation——于是**中间结果留在解释器变量里，不必每轮往返进提示词**。

## 机制是什么

这是这个框架最有辨识度的设计。一次动作的形态对比如下：

| 环节 | `ToolCallingAgent` | `CodeAgent` |
|---|---|---|
| 模型输出 | 结构化 tool_calls | 一段 `<code>...</code>` 或 markdown 代码块 |
| 解析 | provider + `parse_tool_calls` | `parse_code_blobs` 正则 |
| 执行单位 | 每个工具调用一次 | 整段代码一次 |
| 工具怎么用 | 按名字调用 | 工具是解释器里的变量/函数，代码里直接调 |
| 中间结果 | 必须写进 Observation 才能传递 | 存在 `executor.state`，下一步仍是变量 |
| 组合能力 | 无（只能一次调几个） | 有（循环、条件、切片、pandas 链式调用） |

`CodeAgent` 重写了 `initialize_system_prompt`、`_step_stream`、`create_python_executor`、`cleanup`、`to_dict`/`from_dict`，但主循环（`run`/`_run_stream`）、memory、计划步、步数上限、`RunResult` 全部原样继承。它额外拥有的东西是：一个 `python_executor`、一组 `authorized_imports`、代码块的起止标记、以及是否用结构化输出的开关。

## 怎么实现的

**执行器是可插拔的，且「本地」与「远程」在构造期就分叉。** `create_python_executor` 按 `executor_type` 选择：`"local"` 造 `LocalPythonExecutor`；`"blaxel"/"e2b"/"docker"/"modal"` 分别映射到对应的远程执行器。非法值直接 `ValueError`。关键限制：**托管智能体与远程执行不兼容**——`if self.managed_agents: raise Exception("Managed agents are not yet supported with remote code execution.")`。原因是远程执行器跑在另一台机器上，而托管智能体是本地 Python 对象，没法作为可调用名字注入进去。

执行器实例挂在 `self.python_executor`，跨步复用。变量的持久化因此来自两处：执行器自己的 `state`，以及 `run` 开始时 `send_variables(self.state)` 的同步。`LocalPythonExecutor.__init__` 里 `state` 初始值只有 `{"__name__": "__main__"}`，所以「跨 `run` 保留变量」靠的是 agent 的 `state` 字段回灌。

**授权导入是「基础模块 ∪ 用户追加」的并集。** 构造时 `self.authorized_imports = sorted(set(BASE_BUILTIN_MODULES) | set(additional_authorized_imports))`。传 `"*"` 会有一个显式警告日志：注释写的是「你给了全部导入授权，意味着智能体可以自行决定导入任何包，若环境没装就会出问题」。执行器初始化时还会用 `find_spec` 预检每个授权模块是否已安装。

**代码块标记有三种配法。** `code_block_tags` 可以是元组（自定义正则）、字符串 `"markdown"`（变成 `` ("```python", "```") ``）、或 `None`（默认 `("<code>", "</code>")`）。传其它字符串直接 `ValueError`，报错文案写明「字符串参数只支持 'markdown'」。

**停止序列是动态拼的，且有一处防御。** 基础是 `["Observation:", "Calling tools:"]`；若闭合标记不包含在开启标记里，才把闭合标记也加进去。注释说明理由：若闭合标签是开启标签的子串，把它加进停止序列会把代码生成提前截断——这是为「用户自定义了奇怪的标签对」准备的保护。

**生成后有一处「补闭合标签」的写入。** 非结构化输出模式下，若模型输出没有以闭合标签结尾，代码会把闭合标签**追加到 `output_text` 并写回 `memory_step.model_output_message.content`**。注释说明这是为了「引导后续 LLM 调用也以这个标记结束，从而更早停止生成」。注意它是写回消息对象的，不是只改本地变量——所以这条被污染的文本会进 memory 与下一轮的提示词。

**解析有两条路，取决于是否用结构化输出。** `use_structured_outputs_internally=True` 时，模型被要求按 `CODEAGENT_RESPONSE_FORMAT` 返回 JSON，取 `json.loads(output_text)["code"]`，再用 `extract_code_from_text` 尝试剥一层代码块（`or code_action` 兜底保留原文）。否则走 `parse_code_blobs(output_text, self.code_block_tags)`。

两条路之后都过一遍 `fix_final_answer_code(code_action)`，然后写进 `memory_step.code_action`。解析阶段的任何异常都包成 `AgentParsingError`，文案是「Error in code parsing... Make sure to provide correct code blobs.」——同样是给模型看的。

**`parse_code_blobs` 的三级回退是这一页的核心协议。** 顺序是：

1. 用配置的 `code_block_tags` 做正则抠块（`extract_code_from_text`，`re.DOTALL`，多处匹配用两个换行拼起来）；
2. 抠不到就退到 markdown 模式 `` ("```(?:python|py)", "\n```") ``；
3. 还抠不到，就**试着把整段文本直接 `ast.parse`**——能解析就说明模型裸给了代码，直接用；
4. 都失败则抛 `ValueError`。文案分两种：若文本里同时出现 `final` 与 `answer`，错误提示会给出「你应该这样写」的示例（`final_answer("YOUR FINAL ANSWER HERE")`）；否则给出通用的「要按 Thoughts + 代码块格式输出」的示例。

第 3 步很关键：它让「不按格式输出但确实是代码」的模型仍能跑通，不必因为少了个标记就整轮失败。

**执行结果与打印日志被拼成两段 Observation。** 执行成功时 `observation = "Execution logs:\n" + code_output.logs`，再追加 `"Last output from code snippet:\n" + truncate_content(str(code_output.output))`。也就是说模型看到的既有 `print` 出来的内容，也有最后一条语句的值（`CodeOutput.output` 是 `evaluate_python_code` 返回的最后一个语句的求值结果）。`truncate_content` 默认上限 20000 字符，超了保留头尾并插一句「内容已被截断」。

**执行失败时仍会抢救打印日志。** `except` 分支先检查 `python_executor.state` 里有没有 `_print_outputs`，有就把已有日志写进 `memory_step.observations` 并输出到终端，然后才抛 `AgentExecutionError(error_msg)`。所以失败步的 memory 里也保留了「失败前打印了什么」，这对调试模型行为是必要的。

**授权失败的提示是给用户的，不是给模型的。** `except` 里如果错误文本同时含 `"Import of "` 与 `" is not allowed"`，会额外打一条 `Warning to user: ... Consider passing said import under additional_authorized_imports`。这条走的是 logger 的 INFO 级别终端输出，不改变抛出的异常内容——即模型看到的是原始解释器错误，用户看到的是「你漏配了授权」。

**每一步都产出一个假的 `ToolCall`。** 代码执行被包装成 `ToolCall(name="python_interpreter", arguments=code_action, id=f"call_{len(self.memory.steps)}")` 并 `yield` 出去、写进 `memory_step.tool_calls`。这样「代码动作」在事件流与 memory 结构上与「JSON 工具调用」同形，下游的渲染、回放、序列化不需要分叉。

**结构化输出与非结构化输出的差别不止在解析。** 结构化模式用专门的 `structured_code_agent.yaml` 模板，且**不做**「补闭合标签」那段（因为 `if not self._use_structured_outputs_internally` 把它挡住了）。也就是说这个开关同时改了三件事：提示词模板、模型返回格式、以及是否写回污染文本。

**`__enter__`/`__exit__` 支持资源清理。** `cleanup` 检查执行器有没有 `cleanup` 方法并调用——远程执行器需要关掉远端会话，本地执行器没有这个方法所以什么也不做。

**序列化多存四个字段。** `to_dict` 在基类之上加了 `authorized_imports`、`executor_type`、`executor_kwargs`、`max_print_outputs_length`；`from_dict` 把 `authorized_imports` 反向映射回 `additional_authorized_imports`，并过滤掉 `None` 值。注意 `BaseAgent.save` 那条路径还会把 `managed_agents` 逐个递归存成子目录。

## 关键取舍

**源码事实：** 托管智能体与远程执行器互斥，源码里是硬 `raise` 且文案是 "not yet supported"。这不是配置校验，是能力缺口——远程沙箱里没有父进程的 Python 对象可注入。

**源码事实：** 补闭合标签的写了回 memory 消息对象（`memory_step.model_output_message.content = output_text`），不是只改局部变量。所以这个「引导后续生成」的手法会永久改变历史文本。

**源码事实：** `parse_code_blobs` 第三步允许「裸代码」，即整段输出能被 `ast.parse` 就当代码用。这牺牲了格式严格性换取鲁棒性，但也意味着模型随口说的一段合法 Python 表达式会被当成动作执行。

**源码事实：** 代码动作被包装成 `ToolCall(name="python_interpreter", ...)`。这是一个「对齐形状」的做法：执行机制完全不同，但事件与 memory 里的表示与 JSON 路径一致。

**源码事实：** 授权导入采用并集，且 `*` 只打警告不禁止。源码把选择权留给用户，并用一条日志把风险讲明白，而不是替用户拦下。

**架构推论：** 「以代码为动作」的三个真实收益是：**中间结果不进提示词**（省 token）、**一次动作可含任意控制流**（组合多步）、**工具是普通可调用对象**（模型不需要学 schema，直接按 Python 习惯用）。三个代价是：**必须自带解释器**（约 1800 行的 AST 求值器）、**错误更难约束**（一段代码里任意位置失败）、**结构化输出能力依赖模型**。

**架构推论：** 因为工具在解释器里以变量形式存在，`CodeAgent` 天然支持「把工具的输出直接喂给下一个工具」这种链式写法，而这在 JSON 路径下必须绕一圈——先返回给模型、再让模型把结果填进下一次调用。这解释了为什么在多步数据处理任务上代码路径的往返次数显著更低。

**架构推论：** 两条路径共享主循环，说明这个框架把「动作表示」当成可替换策略而不是可继承骨架。这是一个清晰的分层判断：循环语义（步、终止、记账）属于框架，动作语法属于子类。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/agents.py` | `CodeAgent` | 代码动作子类：执行器、授权、代码块标记、结构化输出开关 |
| `src/smolagents/agents.py` | `CodeAgent.create_python_executor` | 按 `executor_type` 造本地或远程执行器，拒绝托管智能体 + 远程 |
| `src/smolagents/agents.py` | `CodeAgent.initialize_system_prompt` | 用 code_agent 模板渲染，注入授权导入与代码块标记 |
| `src/smolagents/agents.py` | `CodeAgent._step_stream` | 单步：生成、补闭合标签、解析、执行、拼 Observation |
| `src/smolagents/agents.py` | `CodeAgent.to_dict` / `from_dict` | 额外序列化授权、执行器类型与参数、输出长度上限 |
| `src/smolagents/agents.py` | `CodeAgent.cleanup` | 远程执行器的资源释放 |
| `src/smolagents/utils.py` | `parse_code_blobs` | 三级回退的代码块提取协议 |
| `src/smolagents/utils.py` | `extract_code_from_text` | 按标记对做 DOTALL 正则提取，多块以空行拼接 |
| `src/smolagents/utils.py` | `truncate_content` | 观察文本截断，保留头尾 |
| `src/smolagents/local_python_executor.py` | `CodeOutput` | 一次执行的产出：输出值、打印日志、是否最终答案 |
| `src/smolagents/local_python_executor.py` | `PythonExecutor` | 执行器抽象：`send_tools`、`send_variables`、`__call__` |
| `src/smolagents/local_python_executor.py` | `LocalPythonExecutor.send_tools` | 工具、内置函数、额外函数合并成 `static_tools` |

## 参考

- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 源码：utils.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/utils.py)
- [smolagents 文档：Code agents 概念](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)
- [smolagents 文档：代码执行安全](https://huggingface.co/docs/smolagents/tutorials/secure_code_execution)

## 与主核 / AI 的关系

「以代码为动作」在系统设计上对应的是一个熟悉的选择：**给用户一个通用语言还是给一套专用 API**。主核选择 SQL（声明式、可优化、可移植），SQL 之外的复杂逻辑靠 UDF 补；这里选择 Python（通用、任意控制流、不可优化但表达力强）。两者的共同前提都是「必须有一个受限执行环境」，区别在裁剪的目标：主核裁剪是为了可优化，这里裁剪是为了限制模型能造成的破坏。

对 agent 工程四条结论。第一，**格式容错要分层**：`parse_code_blobs` 的三级回退（配置标记 → markdown → 裸代码）让「模型不完全守格式」不至于整轮失败，比单一正则稳得多。第二，**失败也要保留现场**：执行失败时把已有打印日志写进 memory，是让模型下一轮能自我定位的前置条件。第三，**别悄悄改历史文本**：补闭合标签那段写回了消息对象，虽然目的是引导停止，但会让「模型实际输出」与「记忆里的输出」不一致；复现问题时这是第一个坑。第四，**能力缺口要硬报错**：托管智能体 + 远程执行是 `raise`，不是降级，避免用户在无提示的情况下拿到错误行为。

最后一条关于选型：判断一个框架的「代码式智能体」是否真的可用，看三件事——解释器是不是自己实现的（还是直接 `exec`）、格式解析有没有多级回退、执行失败时会不会丢现场。三者缺一，「模型写的代码」就会频繁把整次运行打断。
