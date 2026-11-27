---
project: autogen
feature: society-of-mind
title: 心智社会智能体（团队作为单体）
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py
    symbols: [SocietyOfMindAgent, on_messages_stream]
---

# 心智社会智能体（团队作为单体）

## 一句话职责

把一个完整的团队包成一个 `ChatAgent`：内部跑一轮多智能体讨论，再由一个独立的模型调用把整段讨论压缩成一条对外的、不提中间过程的回复。

## 机制是什么

`SocietyOfMindAgent` 持有四样东西：`team`（内层团队，任意 `Team` 实现）、`model_client`（用来做「提炼」的模型客户端，与内层团队的模型无关）、`instruction` 与 `response_prompt`（两条固定提示语，分别承担「告知这是前情」与「要求产出独立回复」的角色），以及一个可选的 `model_context`（保存跨调用的前情摘要）。

它对外的 `produced_message_types` 只有 `TextMessage`，状态模型是 `SocietyOfMindAgentState`（唯一字段 `inner_team_state`）。

关键结构是：内层团队的消息**不进入**对外 `Response`。`on_messages_stream` 会把内层团队产生的消息逐条 `yield` 出去（供观察者看），但构造最终 `Response` 时 `inner_messages=[]`——注释写得很直白：「Response 的 inner_messages 应为空，因为这是对外的回复」。

## 怎么实现的

**前情的注入方式。** 方法开头先读 `model_context.get_messages()`，若非空，就把它包成一条 `HandoffMessage(content="relevant previous messages", source=self.name, target="", context=prev_content)` 并**前置**到本次任务消息之前。这里用 `HandoffMessage` 而不是普通消息是有讲究的：`AssistantAgent` 的 `_add_messages_to_context` 与团队容器的缓冲逻辑都会把 `HandoffMessage.context` 里的 `LLMMessage` 展开写进上下文，于是前情自然地被内层团队看到，而 `target=""` 让它不触发任何交接行为。

**任务的零值处理。** 若拼完 `task_messages` 为空，就把 `task` 设为 `None`——传给团队的 `run_stream` 表示「不带新输入，接着团队自己的状态往下跑」。

**内层团队调用。** 用 `self._team.run_stream(task=task, cancellation_token=..., output_task_messages=False)`。`output_task_messages=False` 是关键参数：任务消息本身不回流，避免把「刚传进去的输入」再当成「团队的产出」收集一遍。遍历流时：`TaskResult` 存下来不当输出；其它消息一律 `yield` 给外部并累积进 `inner_messages`；`ModelClientStreamingChunkEvent` 只 `yield` 不累积。

**空结果与正常结果两支。** `inner_messages` 为空则回 `TextMessage(source=self.name, content="No response.")`，且 `inner_messages=[]`。否则构造提炼请求：第一条是 `instruction`——若模型客户端 `model_info["multiple_system_messages"]` 为真就作为 `SystemMessage`，否则降级成 `UserMessage(source="user")`；接着把 `inner_messages` 里所有 `BaseChatMessage` 逐条 `to_model_message()` 追加进去；最后再按同样规则追加 `response_prompt`。然后一次 `model_client.create(...)`，把结果包成 `TextMessage(source=self.name, content=completion.content, models_usage=completion.usage)`，`inner_messages` 仍为空。

**收尾的两步。** 先 `_add_messages_to_context` 把本次收到的输入写进自己的 `model_context`（`HandoffMessage` 先展开 `context` 再写自身），再 `await self._team.reset()`——**每次调用结束都重置内层团队**。这是「团队作为单体」的核心语义：外部看不到团队的状态在累积，每次都是一次干净的内层会话。

**状态是两层的。** `save_state` 返回 `SocietyOfMindAgentState(inner_team_state=await self._team.save_state())`，`load_state` 反向注回内层团队。注意 `model_context` 的内容**不在** `save_state` 里——`model_context` 自身有 `save_state`，但这里没有调用它。所以保存/恢复只覆盖内层团队，不覆盖前情摘要。

**构造期的一致性。** `model_client` 与内层团队的模型客户端可以完全不同；`description` 默认是「用内层团队生成回复的智能体」；`model_context` 缺省时是 `UnboundedChatCompletionContext`。

## 关键取舍

**源码事实：** 内层团队每次调用后都被 `reset`。这使 `SocietyOfMindAgent` 在外部看起来是无状态的（除了 `model_context` 里的前情），代价是**跨调用的团队连续性被主动丢弃**——上一轮内层团队累积的消息线索不会被下一轮复用。

**源码事实：** 提炼是把内层团队**所有** `BaseChatMessage` 塞进一次模型调用。没有过滤、没有截断、没有分层摘要。内层团队越啰嗦，这一次调用的上下文越长，成本与溢出风险都在这里。

**源码事实：** 提供 `output_task_messages=False` 的理由在源码注释里点明：避免「基于计数的脆弱逻辑」。也就是说，早先的实现靠「数前 N 条是任务消息」来剔除输入，后来改成显式参数。这类改动方向值得注意——把隐式约定（位置/数量）换成显式声明。

**源码事实：** 交给外部观察者的流里**包含**内层团队的全部过程消息，但最终 `Response.inner_messages` 恒为空。同一条数据在这两条通道上的待遇相反：可观测但不入结果。

**源码事实：** 「多条系统消息」的能力差异用一条 `model_info` 探测来处理，而不是让调用方配置。不支持时降级为 `UserMessage(source="user")`，语义上稍弱但能跑通。

**架构推论：** 这是「层级编排」的通用解：把团队当成单个智能体后，外层团队不需要理解内层的选人策略、终止条件、消息线索。反之也成立——内层团队可以随意更换（RoundRobin、Selector、GraphFlow 都行），外层完全无感。代价是每层都引入至少一次额外的模型调用与一次上下文转录。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py` | `SocietyOfMindAgent` | 包装器本体：跑团队、提炼、重置 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py` | `on_messages_stream` | 前情注入 → 团队运行 → 提炼 → 写上下文与重置 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py` | `DEFAULT_INSTRUCTION` | 把内层转录定位为「前情」的系统提示 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py` | `DEFAULT_RESPONSE_PROMPT` | 要求产出独立回复、不提中间讨论 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py` | `save_state` / `load_state` | 只保存内层团队状态 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `SocietyOfMindAgentState` | 状态载体：唯一字段是内层团队状态 |

## 参考

- [AutoGen 源码：_society_of_mind_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/agents/_society_of_mind_agent.py)
- [AutoGen 文档：Society of Mind](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/teams.html#society-of-mind-agent)
- [AutoGen 文档：团队与嵌套](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/teams.html)

## 与主核 / AI 的关系

这个智能体的形态与主核里的「存储引擎做成一层门面」类似：内部是一整套复杂机制（团队、选人、终止、消息线索），对外却只暴露一个与单智能体完全相同的接口。分层编排之所以可行，前提是接口足够抽象——`Team` 与 `ChatAgent` 都继承 `TaskRunner`，才有这种替换空间。

对 AI 工程最重要的推论是**「多智能体讨论」的产物需要一次提炼才适合外传**。内层团队的转录里充满试探、返工、互相修正，直接交给下游会把噪声当成结论。但提炼这一步的输入规模没有约束，一旦内层团队跑长（比如十几轮工具循环），单次提炼的上下文就可能超出模型窗口。实用做法是给内层团队配一个有界 `model_context`、或收紧内层的 `max_turns`，而不是指望提炼步骤自己扛住。
