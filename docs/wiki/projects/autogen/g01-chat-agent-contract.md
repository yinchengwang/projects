---
project: autogen
feature: chat-agent-contract
title: ChatAgent 契约与响应对象
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py
    symbols: [ChatAgent, Response, on_messages_stream]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/base/_task.py
    symbols: [TaskRunner, TaskResult]
---

# ChatAgent 契约与响应对象

## 一句话职责

用一组抽象方法把「能对话的智能体」固化成团队、工具、状态系统都能统一调用的接口，并把「最终回复」和「过程消息」拆成 `Response` 的两个字段，使上层能分别对待。

## 机制是什么

`ChatAgent` 是一个 ABC，抽象成员覆盖四类能力：身份（`name`、`description`）、能力声明（`produced_message_types`）、对话（`on_messages` 与 `on_messages_stream`）、生命周期（`on_reset`、`on_pause`、`on_resume`、`close`、`save_state`、`load_state`）。它的 `component_type` 固定为 `"agent"`，因此任何实现都能被组件配置系统序列化并在团队配置里引用。

`Response` 是一个 `kw_only` 的 dataclass，只有两个字段：`chat_message`（本轮的回复消息）与 `inner_messages`（过程中产生的 `BaseAgentEvent` 或 `BaseChatMessage` 列表，可为 `None`）。

任务层由 `TaskRunner` Protocol 定义：`run` 与 `run_stream` 接收 `task`（字符串、单条消息、消息序列或 `None`）、`cancellation_token` 与 `output_task_messages`，返回或流式产出 `TaskResult`；`TaskResult` 只含 `messages` 与可选的 `stop_reason`。`ChatAgent` 继承 `TaskRunner`，所以「一个智能体」与「一个团队」在调用形式上完全一致——这也是团队可以互相嵌套的类型基础。

## 怎么实现的

**单向的数据流不变式。** 契约规定 `on_messages_stream` 是主实现，`on_messages` 是它的适配器：具体实现里 `on_messages` 一律写成「async for 遍历 stream，遇到 `Response` 就返回」，取不到就抛 `AssertionError`（`AssistantAgent`、`CodeExecutorAgent`、`UserProxyAgent` 都是这个写法）。反过来 `BaseChatAgent` 提供的默认 `on_messages_stream` 则是「await `on_messages`，先吐 `inner_messages` 再吐 `Response`」。两者构成兜底对：子类只要实现其中一个就能工作，但基类默认方向是「stream 由 messages 合成」，与具体类的方向相反。

**名字即校验。** `BaseChatAgent.__init__` 在赋值前检查 `name.isidentifier()`，不合法直接 `ValueError`。这条限制不是风格洁癖：智能体名字后面会被当作 agent 类型、话题类型、组件引用名使用，必须是合法标识符才能不破坏这些下游用途。

**任务的归一化。** `BaseChatAgent.run` 与 `run_stream` 把各种 `task` 形态归一成 `input_messages`：字符串转成 `TextMessage(content=task, source="user")`；单条 `BaseChatMessage` 直接使用；序列逐条校验类型，空序列抛 `ValueError("Task list cannot be empty.")`；`None` 表示「不带新输入，接着上次继续」。`output_task_messages` 控制这些任务消息是否同时回吐到输出（默认 `True`，为向后兼容）。

**流式输出的收尾。** `run_stream` 在遍历 `on_messages_stream` 时做了一次分流：遇到 `Response` 就产出 `chat_message`、把它加入 `output_messages`，然后立刻产出 `TaskResult` 并结束；遇到其他消息就原样产出，其中 `ModelClientStreamingChunkEvent` 只产出但不进 `output_messages`——即「流式碎片给眼睛看，不进结果集」。注意 `run` 版本没有这个例外处理，而是直接把 `inner_messages` 全量拼进结果。

**默认生命周期的三档语义。** `BaseChatAgent` 给 `on_pause`、`on_resume`、`close` 都留了空实现；`save_state` 返回 `BaseState().model_dump()`（即 `{"type": "BaseState", "version": "1.0.0"}`），`load_state` 只做 `BaseState.model_validate` 校验。这意味着「无状态智能体」不需要写任何状态代码就能满足契约，`on_reset` 则被声明为抽象方法——重置语义必须由每个实现明确表态。

## 关键取舍

**源码事实：** `Response` 把过程消息与最终回复分开，而不是塞进一条消息的内容里。这让「过程」可以走与「回复」不同的传播路径：团队容器只把 `chat_message` 放进参与者的消息缓冲（见参与者容器一篇），而团队管理器会把 `inner_messages` 一并追加进公共消息线索（见群聊管理器一篇）。于是过程消息进入团队可见的历史，却不进入其他成员下一轮的模型上下文。

**源码事实：** `BaseChatAgent.run` 返回的 `TaskResult` 不填 `stop_reason`；该字段只由团队层在终止时写入。单智能体跑完没有「停止原因」，因为它没有终止条件这个概念。

**源码事实：** 契约要求调用方「只传新消息」。`on_messages` 的文档字符串明确写了智能体是有状态的、历史应存在智能体内部，`AssistantAgent` 的做法是把消息累积进自己的 `model_context`。这与其他「把全量历史当参数传」的对话框架方向相反。

**源码事实：** 团队路径里只有 reset / pause / resume 三条 RPC（容器上对应 `handle_reset`、`handle_pause`、`handle_resume`），没有对应的 `close` 调用点。也就是说 `close` 属于使用者自己负责的资源清理钩子，不参与团队编排的生命周期。

**架构推论：** 把 `produced_message_types` 放进契约，是为了让团队在构造期就能知道每个参与者可能产出什么消息类型，从而预先检查消息注册表（团队的 `MessageFactory`）。这是一种「能力声明前置」的设计：运行期不再做类型发现，一类错误在装配阶段就暴露。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py` | `ChatAgent` | 智能体抽象协议：身份、能力、对话、生命周期 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py` | `Response` | 双通道响应：最终回复 + 过程消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py` | `on_messages_stream` | 主实现入口，最后一项必须是 `Response` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_task.py` | `TaskRunner` | `run` / `run_stream` 协议，智能体与团队共用 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_task.py` | `TaskResult` | 结果集：消息列表 + 停止原因 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_base_chat_agent.py` | `BaseChatAgent` | 契约的默认实现：名字校验、stream 兜底、任务归一化 |

## 参考

- [AutoGen 源码：_chat_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/base/_chat_agent.py)
- [AutoGen 源码：_base_chat_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_base_chat_agent.py)
- [AutoGen 文档：AgentChat Agents](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/agents.html)

## 与主核 / AI 的关系

从接口设计看，这份契约与主核里「对外 API 与内部执行分离」是同一路数：抽象层只声明「一次调用能得到什么」，把「怎么算出来的」留给实现，这样上层编排（团队）不必知道下层（模型调用、工具执行、人在回路）的细节。

对 AI 工程最实际的推论有两条。其一，`inner_messages` 这条侧信道决定了「可观测性」的粒度：思考、工具调用、代码生成这些中间态可以只给观察者看，不必污染其他智能体的上下文——这相当于把日志与消息面切开了。其二，契约只要求「新消息」，意味着上下文管理与裁剪必须由智能体自己负责（`AssistantAgent` 用 `model_context` 实现）；如果照搬「每次传全量历史」的写法，同一份历史会被重复追加，模型看到的是叠加后的重复内容。
