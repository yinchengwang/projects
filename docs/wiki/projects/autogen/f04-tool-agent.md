---
project: autogen
feature: tool-agent
title: 工具智能体与调用循环
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py
    symbols: [ToolAgent, ToolNotFoundException, handle_function_call]
  - path: python/packages/autogen-core/src/autogen_core/tool_agent/_caller_loop.py
    symbols: [tool_agent_caller_loop]
---

# 工具智能体与调用循环

## 一句话职责

把「执行工具」这件事做成一个独立智能体：它只接收 `FunctionCall`、只回 `FunctionExecutionResult`，再配一个驱动器在模型与它之间来回转发，直到模型不再要求调用工具。

## 机制是什么

为什么要多一个智能体？因为工具执行是**可以出错、可能需要隔离、可能想单独部署**的动作。把它做成 actor，工具就跑在自己的消息处理里，失败被包成消息回给对方，而不是把调用方一起拖崩；同时它天然可被换成远端实例，而不改调用方代码。

`ToolAgent` 是个极窄的 `RoutedAgent`：只有一个 `@message_handler` 处理 `FunctionCall`，返回 `FunctionExecutionResult`。它持有 `List[Tool]`，按名字线性查找。异常体系是三个 `BaseException` 的 dataclass 子类：`ToolException(call_id, content, name)` 为基，派生 `ToolNotFoundException`、`InvalidToolArgumentsException`、`ToolExecutionException`。注意它们继承的是 `BaseException` 而不是 `Exception`——这使得 `except Exception` 的兜底捕获不到它们，必须显式处理。

`tool_agent_caller_loop` 是配套的驱动器：给定调用方（`BaseAgent` 或 `AgentRuntime`）、工具智能体的 `AgentId`、模型客户端、输入消息与工具清单，它交替调模型与工具智能体，返回这次循环里生成的所有消息。

## 怎么实现的

**`handle_function_call` 的三层异常。** 先按 `message.name` 线性找工具，找不到 `ToolNotFoundException`。找到后 `json.loads(message.arguments)`——`json.JSONDecodeError` 被单独捕获成 `InvalidToolArgumentsException`，说明「参数串本身不是 JSON」被当成一类独立故障。其余任何 `Exception` 包成 `ToolExecutionException`。成功路径返回 `FunctionExecutionResult(content=tool.return_value_as_string(result), call_id=message.id, is_error=False, name=message.name)`。

`ctx.cancellation_token` 被直接传给 `tool.run_json`，`call_id=message.id` 也一并传下去用于追踪。所以一条工具调用在追踪系统里是同一个 id 贯穿的。

**`tool_agent_caller_loop` 的节奏。** 先调一次模型，把响应用 `AssistantMessage(content=response.content, source=caller_source)`（默认 `"assistant"`）追加进 `generated_messages`。然后进入 `while`：条件是 `isinstance(response.content, list) and all(isinstance(item, FunctionCall) for item in response.content)`——即「模型这次只给了函数调用，没给文本」。

循环体四步：
1. 对 `response.content` 里每个 `FunctionCall` 并发 `caller.send_message(message=call, recipient=tool_agent_id, cancellation_token=...)`，用 `asyncio.gather(..., return_exceptions=True)` 收集。
2. 逐个处理结果：`FunctionExecutionResult` 原样收；`ToolException` 转成 `is_error=True` 的 `FunctionExecutionResult`，内容写 `f"Error: {result}"`；其他 `BaseException` **直接 `raise`**。
3. 把这一批结果装成一条 `FunctionExecutionResultMessage` 追加。
4. 用 `input_messages + generated_messages` 再调模型，把新响应追加。

循环退出后返回 `generated_messages`（**不含** `input_messages`）。

**并发而非串行。** 同一轮里的多个 `FunctionCall` 是并发发出的，`gather` 一次性等齐。这符合模型一次给出多个独立调用的语义，但也意味着其中一个慢调用会拖住整轮。

**异常的两级分流。** 这是整页最精细的地方：`ToolException` 家族的异常被**转成数据**（`is_error=True` 的结果消息），而其他 `BaseException` 被**重新抛出**、终止循环。因为在 actor 运行时里，点对点请求的异常是通过 future 传回调用方的（见 a01 页），所以 `gather(return_exceptions=True)` 拿到的 `BaseException` 可能是运行时自身的故障，不该被当成「工具失败」吞掉。

**错误文案的嵌套。** `ToolException` 的 `content` 里已经带了 `"Error: "` 前缀（`ToolNotFoundException` 写 `f"Error: Tool not found: {name}"`），而 `caller_loop` 又写 `f"Error: {result}"`。而 `result` 是 dataclass 的 repr，形如 `ToolNotFoundException(call_id='1', content='Error: Tool not found: x', name='x')`。所以最终塞给模型的内容是**一整个 dataclass 的 repr**，包含 call_id、重复的 name 与两层 Error 前缀。

## 关键取舍

**源码事实：** `ToolException` 继承 `BaseException`。这是个强约束：常规的 `except Exception:` 兜底捕不到工具异常，一定漏到最外层。好处是工具故障不会被误当成普通异常吞掉；坏处是任何不显式处理的中间层都会直接崩。

**源码事实：** `caller_loop` 交给模型的错误文本是 dataclass 的 `repr`，而不是 `result.content`。这看起来像是实现疏漏，但也意味着模型能看到 `call_id`——对需要「针对某次调用重试」的场景反而有用。

**源码事实：** `caller_loop` 的 `while` 条件用 `all(...)`，所以「部分内容是文本、部分是函数调用」的混合响应会让循环**退出**。此时那条混合内容的 `AssistantMessage` 已经在 `generated_messages` 里，函数调用却没有被执行——模型要求的调用被静默丢弃。

**源码事实：** 返回值不含 `input_messages`。调用方要把它们拼起来才得到完整上下文，否则会丢掉首轮的 user 消息。

**源码事实：** 循环没有迭代上限。若模型每轮都要求调用工具，就会一直转下去。终止责任完全在调用方（例如在外层套一个终止条件，或让模型客户端带上预算）。

**架构推论：** 把工具执行放在独立 actor 里，直接的收益是**故障隔离**：工具内部抛异常只污染这一次消息处理，运行循环继续推进（点对点请求的异常经 future 回传给 caller，不终止运行时）。换成函数内联调用，一个未捕获异常可能让整个处理链断掉。

**架构推论：** `ToolAgent` 与 `Workbench` 走了两条不同的路。`ToolAgent` 用 `List[Tool]` 加 `run_json`，结果是字符串；`Workbench` 用 `list_tools`/`call_tool`，结果是富类型 `ToolResult`。前者适合「一组固定的本地函数 + 消息化隔离」，后者适合「动态工具源 + 富结果」。两者不互相委托，选哪条由结果形态与工具来源的稳定性决定。

**架构推论：** `caller_loop` 接收 `tool_schema: List[ToolSchema] | List[Tool]`，模型客户端两种都能收（`create` 的 `tools` 参数类型就是这么声明的）。这意味着工具**清单**送给模型与工具**执行**给智能体是两份可能不一致的数据——若传进去的是 `ToolSchema`，`ToolAgent` 那边的工具列表必须另行保证同名同参。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `ToolAgent` | 只处理 `FunctionCall` 的专用智能体 |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `ToolAgent.handle_function_call` | 名字查找、参数解析、执行、三层异常分类 |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `ToolException` | 工具异常基类，继承 `BaseException`，带 call_id |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `ToolNotFoundException` | 名字找不到 |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `InvalidToolArgumentsException` | 参数串不是合法 JSON |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py` | `ToolExecutionException` | 工具内部执行失败 |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_caller_loop.py` | `tool_agent_caller_loop` | 模型与工具智能体之间的交替驱动循环 |
| `python/packages/autogen-core/src/autogen_core/tool_agent/_caller_loop.py` | `generated_messages` | 循环产出的消息，不含输入消息 |

## 参考

- [AutoGen 源码：tool_agent/_tool_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/tool_agent/_tool_agent.py)
- [AutoGen 源码：tool_agent/_caller_loop.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/tool_agent/_caller_loop.py)
- [AutoGen 文档：Tools](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/components/tools.html)

## 与主核 / AI 的关系

这套结构在主核里对应的是「后台工作进程 + 前端协调」的分工：前端只负责把请求发出去、把结果收回来，真正的执行在另一个进程里，失败以消息形式回传而不是让前端崩掉。主核靠的是进程边界与共享内存，这里靠的是 actor 边界与消息队列——隔离的强度不同，但「把可能失败的动作推远一层」这个思路完全一致。

对 AI 工程来说，`caller_loop` 的结构其实就是一个最小的 agent loop 教科书样本，四步值得记住：调模型 → 若只要工具就并发执行 → 结果打包成一条消息 → 带着新消息再调模型。任何「让模型自己决定用什么工具」的系统都是这个循环的变体。理解它之后，看 AgentChat 里那些更复杂的 agent 实现，会发现它们只是在这四步上加了流式、终止条件、状态保存与干预钩子。

要特别留意两处容易踩的边界。第一，`ToolException` 继承 `BaseException`，所以写 `except Exception` 兜底是捕不到的——接入自定义工具时若想统一处理，必须显式 `except ToolException`。第二，循环没有迭代上限，且混合内容响应会让循环提前退出并把未执行的调用丢掉；把它当生产组件用之前，外层必须自己加轮次上限与「响应里既有文本又有调用」的检查。
