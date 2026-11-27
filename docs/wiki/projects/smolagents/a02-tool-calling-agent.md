---
project: smolagents
feature: tool-calling-agent
title: 工具调用式智能体
page_type: feature
schema_version: 1
refs:
  - path: src/smolagents/agents.py
    symbols: [ToolCallingAgent, extract_action]
---

# 工具调用式智能体

## 一句话职责

`ToolCallingAgent` 走「模型原生工具调用」这条路：它把工具定义交给 provider 的 function calling 接口，拿回结构化的 tool calls 再逐个执行，因此它的「动作」是 JSON 参数而不是代码。

## 机制是什么

同一套主循环，两种「动作表示」：

| | `ToolCallingAgent` | `CodeAgent` |
|---|---|---|
| 动作形态 | JSON 工具调用列表 | 一段 Python 源码 |
| 谁解析 | provider 的 function calling + 模型类的 `parse_tool_calls` | 正则抠代码块 + 本地解释器 |
| 并行 | 真的并行（线程池） | 一次解释器调用内自然并行 |
| 中间结果 | 每轮都进提示词（Observation 文本） | 留在解释器变量里，不进提示词 |
| 一次能做几步 | 一轮一批调用 | 一段代码里任意多条语句 |

`ToolCallingAgent` 是**只换 `_step_stream` 实现**的子类：主循环、memory、计划步、步数上限、`RunResult` 全部继承，未做修改。

## 怎么实现的

**单步五段：取记忆、生成、规整调用、执行、出结论。** `_step_stream` 里的顺序是：`write_memory_to_messages()` 拿消息 → 调模型（带 `tools_to_call_from=self.tools_and_managed_agents`，停止序列是 `["Observation:", "Calling tools:"]`）→ 保证 `chat_message.tool_calls` 已规整 → `process_tool_calls` 逐个执行 → `yield ActionOutput(output=final_answer, is_final_answer=got_final_answer)`。

`tools_and_managed_agents` 是一个属性，返回「工具值 + 托管智能体值」的合并列表。也就是说**托管智能体在模型眼里就是一个工具**，没有任何区分标记——它的 `inputs`/`output_type` 在 `_setup_managed_agents` 里被强制改写成 `{task, additional_args}` 与 `"string"`，正好符合一次子任务调用的形状。

**工具调用的规整有两条路。** 若模型返回的 `ChatMessage` 已经带 `tool_calls`，就只对每个调用的 `function.arguments` 做 `parse_json_if_needed`（兼容 provider 直接给字符串的情形）；若没有，就调 `self.model.parse_tool_calls(chat_message)` 让它从文本里解析——这是给不支持原生 function calling 的模型留的后路。两条路之后 `tool_calls` 都是统一的 `ChatToolCall` 形状。

**并行执行是有条件的，且用线程池。** `process_tool_calls` 先把所有调用逐个 `yield` 出来（让流式消费者看到「要调这些」），再按数量分叉：

- 只有 1 个调用时直接同步执行；
- 多个调用时 `ThreadPoolExecutor(self.max_tool_threads)`，每个提交前套一次 `copy_context()`，用 `as_completed` 收结果。

`copy_context()` 是必要的：执行器跑在别的线程里，不显式带上 contextvar 快照，工具内部读到的追踪/上下文作用域就是错的。

**「最终答案」不是特殊返回，而是一个叫 `final_answer` 的工具。** `process_single_tool_call` 里 `is_final_answer = tool_name == "final_answer"` ——只比名字。于是「给出答案」与「调用工具」在机制上完全同构，差别只是这个名字触发了 `ToolOutput.is_final_answer=True`。

**多调用与最终答案互斥。** 一个响应里既有 `final_answer` 又有别的工具时，抛 `AgentExecutionError("If you want to return an answer, please do not perform any other tool calls than the final answer tool call!")`；同一响应里出现两个 `final_answer` 抛 `AgentToolExecutionError`。这两条都是**面向模型**的报错文案，会被写进下一步的 Observation 让它自己纠正。

**答案支持「变量名引用」。** 若 `final_answer` 的返回值是字符串且正好是 `self.state` 的键，会被替换成该变量的值（`if isinstance(final_answer, str) and final_answer in self.state.keys()`）。这是为「工具产生了大对象（图、音频）时，模型只回报变量名」这一场景准备的。

**非文本结果落进 state 而不是塞进提示词。** `process_single_tool_call` 判断结果类型：`AgentImage` 存成 `state["image.png"]`、`AgentAudio` 存成 `state["audio.mp3"]`，Observation 变成一句「Stored 'image.png' in memory.」。这样二进制内容不会变成提示词里的乱码，后续步骤用变量名引用即可。

**Observation 的拼接顺序被刻意固定。** 所有 `ToolOutput` 先按 id 排序（`sorted(outputs.keys())`）再拼进 `memory_step.observations`，`memory_step.tool_calls` 也按 id 排序。理由是多线程完成顺序不确定，若按完成顺序拼，同一份对话在两次运行里会产生不同的提示词——不可复现。排序把并行执行的不确定性挡在了提示词之外。

**参数里的字符串会被当成 state 变量名解析。** `_substitute_state_variables` 对每个字符串参数做 `self.state.get(value, value)`：若这个字符串恰好是 state 的键，就替换成对应对象。这使模型可以传 `"df"` 而不是把整个 DataFrame 序列化进 JSON。

**执行入口统一收在 `execute_tool_call`。** 它先查名字（未知工具直接报错并列出可选名）、再替换 state 变量、再 `validate_tool_arguments`（失败归为 `AgentToolCallError`），最后调用。工具与托管智能体的调用形态被一个布尔分开：

- 托管智能体：`tool(**arguments)` —— 因为 `__call__` 是「加提示词 → `run` → 包报告」；
- 普通工具：`tool(**arguments, sanitize_inputs_outputs=True)` —— 多传一个参数，让工具侧做输入输出的类型清洗。

两者的错误文案也不同：托管智能体的失败提示是「please try again or request to another team member」，普通工具是「please try again or use another tool」。文案差异是有意的，因为下一步要做的事不同。

**`extract_action` 是基类留给「文本式动作」的通用切分器，但本仓内没有调用点。** 实现在 `MultiStepAgent` 上：按 `split_token` 切分后取**倒数第二段**与**最后一段**（注释说明「从末尾取索引，解决输出里出现多个分隔符的情况」），分别是 `rationale` 与 `action`，各自 `strip()`；找不到分隔符则抛 `AgentParsingError`，错误文案会附带原始输出并提示必须包含该前缀。它在当前锚点的 `src/` 与 `tests/` 里都只有定义、没有调用——`ToolCallingAgent` 用 provider 的结构化调用、`CodeAgent` 用代码块正则，两条路都不需要它。所以它是「给自定义子类或旧版文本协议准备的公开工具」，不是任何内置路径的一环。

**流式开关有硬前置条件。** `stream_outputs=True` 时构造期就检查 `hasattr(self.model, "generate_stream")`，没有就 `ValueError`。开关打开后走 `generate_stream`，一边累积 delta 一边用 `Live` 逐块刷新终端渲染；关闭时走 `generate` 一次性拿完整消息。两条路最终都产出同一个 `ChatMessage` 并写进 `memory_step.model_output_message`。

**模型调用失败被统一包成 `AgentGenerationError`。** 任何异常（含网络、限流）在这里都变成生成错误。按主循环的分类，它会被原样抛出并终止运行——这一点值得注意：`ToolCallingAgent` 里模型调用失败**不会**重试，也不会让模型自纠。

## 关键取舍

**源码事实：** 最终答案与普通工具共用一条通路，靠名字 `final_answer` 判定。好处是模型不需要学两种格式；代价是「`final_answer` 这个名字被占用」，用户不能再定义同名工具。

**源码事实：** 并行执行结果必须排序后才能拼 Observation。`memory_step.tool_calls` 与 `outputs` 都按 id 排序，这是一条为可复现性付的显式成本。

**源码事实：** 字符串参数会被当作 state 变量名解析。这让「传大数据对象」成为可能，但也意味着**一个恰好与 state 键同名的普通字符串参数会被意外替换**。这是这套设计里最反直觉的一处：参数是 `"df"` 还是 `"\"df\""` 语义不同。

**源码事实：** `extract_action` 在本仓内没有调用点。它是基类提供的文本切分工具，属于公开 API 面的余量，不是 `ToolCallingAgent` 的实现细节——读代码时容易误以为 JSON 路径也走它。

**源码事实：** `max_tool_threads` 直接传给 `ThreadPoolExecutor` 的 `max_workers`。源码注释写的是「更高并发更高资源占用」，没有默认上限保护——即默认就是 `ThreadPoolExecutor` 自己的默认值。

**架构推论：** 「工具与托管智能体在模型眼里无差别」是这套多智能体方案的关键简化：不需要专门的「委派」语法，子智能体就是一个返回字符串的工具。代价是父智能体无法感知子智能体的内部状态，也无法在子任务进行中干预——它只能拿到最终报告文本。

**架构推论：** 一轮里并行执行多个工具，把「步」的粒度从「一次工具调用」提升到「一次模型往返」。这与 `CodeAgent` 的做法收敛到同一个结论：减少模型往返次数才是省 token 与省延迟的关键，两条路只是实现手段不同（一个靠并行调用，一个靠一段代码）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/smolagents/agents.py` | `ToolCallingAgent` | JSON 工具调用式子类，只重写系统提示与 `_step_stream` |
| `src/smolagents/agents.py` | `ToolCallingAgent._step_stream` | 单步：生成 → 规整调用 → 执行 → 出结论 |
| `src/smolagents/agents.py` | `ToolCallingAgent.process_tool_calls` | 调用分组、单/多线程执行、Observation 排序拼接 |
| `src/smolagents/agents.py` | `ToolCallingAgent.execute_tool_call` | 唯一执行入口，区分工具与托管智能体 |
| `src/smolagents/agents.py` | `ToolCallingAgent._substitute_state_variables` | 字符串参数按 state 键替换 |
| `src/smolagents/agents.py` | `ToolCallingAgent.tools_and_managed_agents` | 交给模型的可调用集合 |
| `src/smolagents/agents.py` | `ToolCallingAgent.initialize_system_prompt` | 用 toolcalling 模板渲染系统提示 |
| `src/smolagents/agents.py` | `extract_action` | 基类的文本式动作切分器，本仓无调用点 |
| `src/smolagents/agents.py` | `ToolOutput` | 单次工具调用的结果载体，带 `is_final_answer` |
| `src/smolagents/agents.py` | `ActionOutput` | 单步结论，主循环据此判终止 |

## 参考

- [smolagents 源码：agents.py](https://github.com/huggingface/smolagents/blob/main/src/smolagents/agents.py)
- [smolagents 文档：智能体参考](https://huggingface.co/docs/smolagents/reference/agents)
- [smolagents 文档：智能体概念导引](https://huggingface.co/docs/smolagents/conceptual_guides/intro_agents)

## 与主核 / AI 的关系

这条路对应主核里的「结构化 API 调用」：参数是类型化的、由校验器把关、失败可精确定位到某个参数。`validate_tool_arguments` 失败被归为 `AgentToolCallError` 而执行失败归为 `AgentToolExecutionError`，这个区分与数据库里「语法/语义错误」和「运行期错误」的分层一致——前者是调用方写错，后者是环境出问题，处置不同。

对 agent 工程四条结论。第一，**并行会污染可复现性**：线程完成顺序不同就会产生不同的 Observation 拼接顺序，必须显式排序才能让两次运行产生同样的提示词。第二，**「答案即工具」是低成本的统一**：不用为终止条件发明新语法，但也把某个名字永久占掉。第三，**参数里的字符串是危险的隐式协议**：把 `"df"` 解析成变量引用，省了序列化，代价是「恰好同名的字面量」会被误替换；做类似设计时至少要有一个转义约定。第四，**模型调用失败不重试**：`AgentGenerationError` 直抛，意味着这个类里「模型抖一下」就是整次运行失败，生产上必须在外层加重试。

一条更容易被忽略的判断：`extract_action` 存在但无人调用，说明这个仓里「文本式动作」协议已经从主路径退出，只留兼容面。读代码时若顺着名字去追调用链，会走进死路——这类「有定义无调用」的公开方法在成熟框架里很常见，靠它推断当前实现路径会得出错误结论。
