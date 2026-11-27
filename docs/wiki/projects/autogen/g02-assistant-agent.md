---
project: autogen
feature: assistant-agent
title: AssistantAgent 的模型调用与工具循环
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py
    symbols: [AssistantAgent, _call_llm, _reflect_on_tool_use_flow]
---

# AssistantAgent 的模型调用与工具循环

## 一句话职责

把「调用模型 → 执行工具 → 决定是否反思 → 产出最终消息」这条循环固定成一种可复用的智能体实现，使工具、工作台、记忆、交接、结构化输出都挂在这条循环上而不是散落在各个智能体里。

## 机制是什么

`AssistantAgent` 继承 `BaseChatAgent`，核心状态是六个可注入的部件：`ChatCompletionClient`（模型客户端）、工具集合（要么是 `tools` 列表，要么是 `workbench` 序列，二者互斥）、`handoffs`（交接配置）、`ChatCompletionContext`（模型上下文）、`Memory` 序列、以及一组策略开关（`reflect_on_tool_use`、`max_tool_iterations`、`model_client_stream`、`output_content_type`）。

一次 `on_messages_stream` 的固定骨架是五步：把新消息写进模型上下文、把记忆内容并入上下文并产出 `MemoryQueryEvent`、生成一个 `message_id`、跑第一次推理、把结果交给 `_process_model_result` 处理并产出后续事件。

`_process_model_result` 内部是一个 `for loop_iteration in range(max_tool_iterations)` 循环，循环体按模型返回的内容类型分两支：内容为 `str` 就是终局，直接产出 `Response` 并 `return`；内容为 `FunctionCall` 列表就是工具轮，先产出 `ToolCallRequestEvent`，再并发执行全部调用，然后产出 `ToolCallExecutionEvent`，接着检查交接，最后看是否还有迭代额度。

## 怎么实现的

**上下文的写入顺序。** `_add_messages_to_context` 对每条输入消息先判断是否为 `HandoffMessage`：若是，先把它的 `context` 字段里的 `LLMMessage` 逐条写进上下文，再把消息本身转成 `UserMessage` 追加。顺序很关键——交接带来的工具调用记录必须排在交接消息之前，否则模型看到的是「先被通知接手、后看到要接手的内容」。

**工具清单的组装。** `_call_llm` 里写的是 `[tool for wb in workbench for tool in await wb.list_tools()] + handoff_tools`：每轮推理都重新向工作台索取工具清单（因此工作台的可用工具可以动态变化），交接工具则直接拼在末尾。没有 `workbench` 参数时构造器会造一个 `StaticStreamWorkbench(self._tools)` 占位，于是「普通工具」与「工作台」在调用路径上被统一成同一种东西。

**并发的工具执行与有序的事件流。** 多个工具调用用 `asyncio.gather` 并发跑，但事件必须按顺序跳出生成器，所以实现里开了一个 `asyncio.Queue`：每个工具执行协程把中途产生的流式事件 `put` 进队列，全部完成后 `put_nowait(None)` 作为哨兵；主流程一边从队列取事件一边 `yield`，取到 `None` 才 `await task` 收结果。这是「并发执行、串行上报」的典型写法。

**单次工具调用的四道分支。** `_execute_tool_call` 先 `json.loads` 参数，失败就直接返回一个 `is_error=True` 的结果而不是抛异常——参数错误被降级成工具结果，让模型有机会自我修正。之后按名字匹配交接工具；再遍历工作台，用 `list_tools()` 的名字集合判断归属；命中的是 `StaticStreamWorkbench` 就走 `call_tool_stream`，否则走 `call_tool`；所有工作台都没命中则返回「tool not found」的错误结果。

**交接的优先级。** `_check_and_handle_handoff` 在工具执行完之后运行，扫的是**模型这一轮的原始 `FunctionCall` 列表**（不是执行结果），凡是名字落在 `handoffs` 字典里的都算交接请求。多于一个时只取第一个并发出 `warnings.warn`，提示去关掉模型客户端的 `parallel_tool_calls`。交接消息的 `context` 是拼出来的：非交接的工具调用及其结果打包成 `AssistantMessage` + `FunctionExecutionResultMessage`；如果没有普通工具调用但有 `thought`，则把 thought 作为 `AssistantMessage` 内容；交接工具自身的返回值覆盖 `HandoffMessage.content`。

**反思流的工具禁用。** `reflect_on_tool_use` 为真时走 `_reflect_on_tool_use_flow`，它虽然签名里仍接收 `workbench` 与 `handoff_tools`，但实际调模型时只传 `json_output` 与 `tool_choice="none"`，完全不传工具清单——反思只能产出文本，不能再触发工具。若反思返回的不是字符串（比如模型仍给了函数调用），直接 `RuntimeError`。相反，`reflect_on_tool_use` 为假时走 `_summarize_tool_use`，把每个非交接工具调用的结果按 `tool_call_summary_format`（默认 `{result}`）逐条格式化后用换行拼接成 `ToolCallSummaryMessage`。

**默认值的联动。** 构造器里 `reflect_on_tool_use` 的三分支很有意思：`output_content_type` 已设置且未显式传入 → 默认 `True`（因为结构化输出需要一次额外的模型调用来产出 JSON）；未设置且未传入 → 默认 `False`；显式传入则照用。同一段逻辑还说明：`max_tool_iterations` 小于 1 直接 `ValueError`。

**思考事件的 id 重生成。** 第一次推理若返回 `thought`，会先产出 `ThoughtEvent`，然后**重新生成** `message_id`，再继续。注释写明原因是「让流式碎片与最终消息保持对应」。反思流同理，另起一个 `reflection_message_id`——因为反思是一次独立的模型调用，其流式碎片不能挂在上一轮的消息 id 上。

**上下文的兼容性裁剪。** 所有发给模型的消息都会过 `_get_compatible_context`：`model_client.model_info["vision"]` 为假时调用 `remove_images` 剥掉图片。这是能力探测式的降级，而不是让调用方自己判断。

**状态只有上下文。** `save_state` 返回 `AssistantAgentState(llm_context=model_context.save_state())`，`load_state` 反向回灌；`on_reset` 只 `model_context.clear()`。工具、交接、记忆、系统消息都不进状态——它们是构造期配置，不是运行期可变状态。

## 关键取舍

**源码事实：** 工具结果默认直接当回复（`reflect_on_tool_use` 默认 `False`），所以默认行为下模型不会「看到」自己工具的产出再解释一遍。源码文档特意提醒：另一个智能体若期待特定 schema，就要注意工具返回值的格式。这是把「省一次模型调用」放在「回复自然度」之前的取舍。

**源码事实：** 交接优先于最终回复。只要本轮出现交接调用，`_check_and_handle_handoff` 就产出 `HandoffMessage` 并 `return`，即使这一轮还有普通工具调用——普通调用的结果被打包进交接上下文，交由接手方看到。

**源码事实：** 工具名与交接名必须全局唯一，校验做了三层：工具名去重、交接工具名去重、以及「交接目标名不得与工具名相同」。最后一条是字符串级别的前置检查，因为 `handoffs=["Bob"]` 这种简写会在构造期才展开成 `transfer_to_bob` 工具。

**源码事实：** `_to_config` 里 `tools=None`，注释说明自 v0.5.5 起工具不再单独序列化，因为工具已是工作台的一部分。`tool_call_summary_formatter`（可调用对象）同样不可序列化，从配置加载时会被忽略。

**源码事实：** `produced_message_types` 固定返回 `TextMessage`、`ToolCallSummaryMessage`、`HandoffMessage`，仅在配置了结构化输出时追加 `StructuredMessage`。也就是说「这个智能体可能产出什么」是静态声明，即便某些分支实际不会触发。

**架构推论：** 把工具、工作台、记忆、交接全部揉进一个智能体，换来的是「绝大多数单智能体场景不需要写自定义类」；代价是这个类的分支密度很高（单文件接近 1700 行），任何新能力都必须嵌入同一条五步骨架里，否则就会绕开 `Response` 契约。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `AssistantAgent` | 智能体本体：部件装配与五步骨架 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `on_messages_stream` | 上下文写入 → 记忆 → 首轮推理 → 结果处理 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_call_llm` | 组装系统消息、上下文与工具清单后调用模型 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_process_model_result` | 工具循环主体：文本即终局，函数调用即一轮 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_execute_tool_call` | 单次工具调用：参数解析、交接匹配、工作台分派 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_reflect_on_tool_use_flow` | 反思流，`tool_choice="none"` 禁止再触发工具 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_summarize_tool_use` | 无反思时的摘要，按格式串拼接工具结果 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_check_and_handle_handoff` | 交接检测：取第一个、打包上下文 |

## 参考

- [AutoGen 源码：_assistant_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py)
- [AutoGen 文档：AssistantAgent](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/agents.html#assistant-agent)
- [AutoGen 文档：工具与交接](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/human-in-the-loop.html)

## 与主核 / AI 的关系

这条循环的结构与主核里的「请求—执行—提交」三段式有可比性：模型推理像一次读事务，工具执行像副作用写，反思像提交前的校验。差别在于这里没有回滚——工具已经执行的副作用不会因为反思失败而撤销，所以工具本身必须是幂等或可接受的。

对 AI 工程最重要的推论是**工具循环次数是成本开关**。`max_tool_iterations=1` 时只执行一轮，`reflect_on_tool_use=False` 时还省掉反思那次调用，整个交互只有两次模型调用（首轮 + 无）。一旦打开多轮迭代或反思，轮次上限就成了主要的费用与延迟控制点；同时，因为每轮都会把 `AssistantMessage` 与 `FunctionExecutionResultMessage` 追加进上下文，上下文长度随轮次线性增长，长循环必须配合有界 `model_context` 才有意义。
