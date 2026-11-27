---
project: autogen
feature: code-executor-agent
title: 代码执行智能体与审批
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py
    symbols: [CodeExecutorAgent, extract_code_blocks_from_messages, execute_code_block]
---

# 代码执行智能体与审批

## 一句话职责

把「从聊天里抽出代码块、执行、把结果写回对话」这件事做成一个独立智能体，并在执行前插入一个可插拔的审批回调，让人或模型在代码真正运行之前有一次否决机会。

## 机制是什么

`CodeExecutorAgent` 有两种形态，由构造器是否传入 `model_client` 来决定：

- **纯终端形态**（无 `model_client`）：只扫输入消息里的 Markdown 代码块，执行，把 `CodeResult.output` 作为回复。这是向后兼容的默认行为。
- **生成 + 执行形态**（有 `model_client`）：先让模型生成代码，再执行，再把执行结果喂回模型做一次反射，最终回复是反射文本。

两种形态的产出消息类型都只声明 `TextMessage`（`produced_message_types` 返回 `(TextMessage,)`）；代码生成与执行这两个阶段通过 `CodeGenerationEvent`、`CodeExecutionEvent` 两个**事件**广播出去，不进入 `Response.chat_message`。

## 怎么实现的

**代码块的抽取。** `extract_code_blocks_from_messages` 遍历输入消息，先按 `sources` 过滤（`sources` 为 `None` 时接受全部来源），只处理 `TextMessage`，用 `_extract_markdown_code_blocks` 抽出代码块。抽取用的正则把语言名限定在 `supported_languages` 上：构造器先把语言列表 `re.escape` 后拼成 `python|sh` 这样的分支，再嵌进 ``` ```(?:\s*(...))\n([\s\S]*?)``` ``` 中并忽略大小写。所以**语言未列在 `supported_languages` 里的代码块会被静默忽略**，不报错也不执行。

**纯终端形态的短路。** 没有代码块就直接回 `NO_CODE_BLOCKS_FOUND_MESSAGE`（提示用户用 ```python 或 ```sh 标注），有代码块则执行后把输出作为回复。这条路径完全没有模型参与，也不做审批之外的任何校验。

**生成 + 执行形态的循环。** 外层是 `for nth_try in range(max_retries_on_error + 1)`，即「默认一次执行」加「最多 N 次重试」。每轮固定顺序：写上下文 → 调模型（`tools=[]`，不提供任何工具）→ 有 `thought` 就产出 `ThoughtEvent` → 把 `AssistantMessage` 写回上下文 → 抽代码块 → 无代码块则把模型原文当回复并结束 → 有代码块则产出 `CodeGenerationEvent`（带 `retry_attempt=nth_try`）→ 执行 → 把执行输出作为 `UserMessage` 写回上下文 → 产出 `CodeExecutionEvent`。

**重试判定是一次独立的模型调用。** 执行成功（`exit_code == 0`）或已到最后一轮就退出循环；否则再调一次模型，用 `json_output=RetryDecision` 要求结构化输出，`RetryDecision` 只有 `reason` 与 `retry` 两个字段。`retry` 为假就退出；为真则产出一条 `code_blocks` 为空、内容为「尝试轮次 + 修正建议」的 `CodeGenerationEvent`，然后进入下一轮。这条事件的作用是让观察者看到「模型决定重试的理由」，同时又不会被当成可执行代码。

**审批回调的执行时机与形态。** `execute_code_block` 在真正调用执行器之前先做审批：把本轮全部代码块用 ``` 语言 ``` 包裹后以双换行拼接成**一个**字符串（而不是逐个审批），连同当前模型上下文的 `get_messages()` 一起塞进 `ApprovalRequest(code=..., context=...)`。同步与异步回调在构造期就已经判定（`iscoroutinefunction`），执行时据此走 `await async_func(...)` 或 `sync_func(...)`。未获批时代码不执行，返回 `CodeResult(exit_code=1, output="Code execution was not approved. Reason: ...")`——注意这是**普通结果而不是异常**，因此后续的重试判定与反射流程仍会照常进行。

**执行结果的三段式加工。** 执行器返回后，`output` 会被改写：空白输出被替换成「脚本运行但无输出，POSIX 退出码是 N，若期待输出请修改脚本」；非零退出码则在输出前面加上「脚本运行后出错（POSIX 退出码 N）」的前缀。这两种改写都是为了让模型下一轮能读到可判定的自然语言描述，而不只是原始 stdout。

**构造期的三条硬约束。** `max_retries_on_error > 0` 要求同时具备 `model_client` 与 `model_info`，并且 `model_info["structured_output"]` 必须为真（否则重试判定无法用 `json_output` 约束出 `RetryDecision`）；`description` 未提供时按有无 `model_client` 在两种默认文案间切换；没有 `approval_func` 时发出 `UserWarning`，明说「代码将被自动执行，没有人工审核」。

**状态与序列化。** `on_reset` 是空实现，注释写明「代码执行智能体没有可变状态」——它确实不维护消息历史，上下文只在 `model_context` 里；`_to_config` 在 `approval_func` 非空时直接抛 `ValueError`（回调不可序列化），`_from_config` 则无条件把 `approval_func` 置为 `None`。也就是说**从配置恢复出来的执行智能体一定是无审批的**，这个安全属性的损失必须在部署侧另行补偿。

## 关键取舍

**源码事实：** `produced_message_types` 只报 `TextMessage`。代码生成与执行阶段虽然是这个智能体最重要的行为，却只以事件形式存在。这带来的直接后果是：在群聊里，其他参与者看到的只是「执行完之后的文本」，而不是「生成了哪些代码」。

**源码事实：** 审批是「一轮全部代码块一次性过审」，粒度是轮次而不是单个代码块。实现里明文把多个块拼成一整段文本再交给回调，所以回调无法只批准其中一部分。

**源码事实：** 未获批的结果被伪装成 `exit_code=1` 的 `CodeResult`，而不是抛出异常。这使拒绝路径与执行失败路径在下游看起来一样，都走「非零退出 → 询问是否重试 → 反射」的同一套流程。

**源码事实：** 生成形态每轮调用模型都显式传 `tools=[]`，且反射调用完全不传工具。这个智能体在设计上只能产出文本形式的代码，不能通过函数调用触发工具，边界很清晰。

**源码事实：** 语言过滤既是特性也是坑。`supported_languages` 默认只有 `python` 与 `sh`，其它语言的代码块（例如 ```js）既不会被执行，也不会产生任何提示——消息会被当作「没有代码块」处理，纯终端形态下直接返回 `NO_CODE_BLOCKS_FOUND_MESSAGE`。

**架构推论：** 把「代码执行」做成独立智能体而不是 `AssistantAgent` 的一个工具，换来的是审批点天然落在「代码文本 → 执行器」这一步之间，且模型与执行器可以各用各的客户端。代价是多了一次模型往返（生成、执行、反射至少三次调用），且代码必须经由聊天文本传递，无法传递结构化参数。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `CodeExecutorAgent` | 智能体本体，两种形态由 `model_client` 是否存在决定 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `on_messages_stream` | 纯终端短路路径与生成执行循环 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `extract_code_blocks_from_messages` | 按 `sources` 过滤后从 `TextMessage` 抽代码块 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `_extract_markdown_code_blocks` | 语言白名单正则；不在名单内的代码块被忽略 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `execute_code_block` | 审批 → 执行 → 输出改写三段式 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `ApprovalRequest` / `ApprovalResponse` | 审批契约：代码文本 + 模型上下文，回应批准与理由 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `RetryDecision` | 重试判定的结构化输出模型 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py` | `_reflect_on_code_block_results_flow` | 循环结束后的强制反射，产出最终回复 |

## 参考

- [AutoGen 源码：_code_executor_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_code_executor_agent.py)
- [AutoGen 文档：Code Executor Agent](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/agents.html#code-executor-agent)
- [AutoGen 文档：代码执行器](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/code-executors.html)

## 与主核 / AI 的关系

审批回调的位置值得与主核的事务边界对照：主核把「提交前校验」放在写事务之内，这里把「执行前校验」放在执行器调用的入口，两者都把不可逆动作挡在最后一道门之前。差别是这里没有强制门——不传回调时只发一条警告，代码照样执行。

对 AI 工程最实际的推论：**这个智能体的安全性完全由外部注入的回调承担**。默认路径既没有沙箱也没有白名单，`supported_languages` 只决定「哪些代码能被认出来并送进执行器」，不是安全策略。要部署到生产，必须显式提供 `approval_func`（或换成自带隔离的执行器），并且清楚「从 YAML/JSON 配置恢复时审批回调一定为空」这条序列化事实——配置驱动的流水线里，人工把关点必须在恢复之后重新注入。
