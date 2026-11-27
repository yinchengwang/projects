---
project: autogen
feature: team-and-agent-state
title: 状态对象与保存恢复
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py
    symbols: [BaseState, TeamState, AssistantAgentState]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py
    symbols: [AssistantAgent, save_state, load_state]
---

# 状态对象与保存恢复

## 一句话职责

把「智能体记住什么」与「团队推进到哪一步」显式建模成一组带 `type` 与 `version` 标记的 pydantic 模型，使一次运行可以在中途被序列化、跨进程迁移、并在恢复后从原处继续。

## 机制是什么

`BaseState` 是所有状态模型的根，只有两个字段：`type`（默认 `"BaseState"`）与 `version`（默认 `"1.0.0"`）。八个子类各自覆盖 `type`，把默认值改成自己的类名：

- `AssistantAgentState`：`llm_context`（模型中上下文的保存结果）。
- `TeamState`：`agent_states`（参与者名字 → 各自状态的映射）。
- `BaseGroupChatManagerState`：`message_thread`（消息 dump 列表）、`current_turn`。
- `ChatAgentContainerState`：`agent_state`（内层智能体状态）、`message_buffer`（未处理的缓冲消息）。
- `RoundRobinManagerState`：基态 + `next_speaker_index`。
- `SelectorManagerState`：基态 + `previous_speaker`。
- `SwarmManagerState`：基态 + `current_speaker`。
- `GraphFlowManagerState`：基态 + `active_nodes`（当前实现中未被使用）。
- `MagenticOneOrchestratorState`：基态 + `task`、`facts`、`plan`、`n_rounds`、`n_stalls`。
- `SocietyOfMindAgentState`：`inner_team_state`。

保存接口是每个对象自己的 `save_state()` / `load_state()`：智能体侧由 `BaseChatAgent` 提供默认实现（返回空 `BaseState`），团队侧由 `BaseGroupChat` 遍历参与者与管理器。

## 怎么实现的

**三层嵌套的保存调用。** 团队层的 `save_state` 不是自己拼装，而是把「保存」这件事交给运行时：对每个参与者构造 `AgentId(type=参与者话题类型, key=team_id)`，调 `runtime.agent_save_state(agent_id)`；再对管理器做同样的事；最后把结果装进 `TeamState(agent_states=...)` 返回。源码注释说明了为什么要绕经运行时而不是直接调实例方法：**为了支持远端智能体**——运行时的接口约定与位置无关，直连实例就只能同进程。

运行时的 `agent_save_state` 实现极简：`await (await self._get_agent(agent)).save_state()`——先按 id 取（或惰性实例化）智能体，再转发调用。所以「保存状态」这一步**可能会触发实例化**。

**键是名字，不是 id。** `agent_states` 的 key 用的是**参与者名字**与管理器名字，而不是 `AgentId`。源码注释写明理由：让状态与运行时身份解耦，从而可以跨团队、跨运行时迁移。代价是恢复时必须靠名字重新对应——`load_state` 会逐个检查名字是否存在，缺一个就 `ValueError`。

**容器层保存两样。** `ChatAgentContainer.save_state` 返回 `ChatAgentContainerState(agent_state=await self._agent.save_state(), message_buffer=[m.dump() for m in self._message_buffer])`。`load_state` 反向：清空缓冲、用消息工厂逐条重建（非 `BaseChatMessage` 直接 `ValueError`）、再把 `agent_state` 注回内层智能体。

注意这里的层级：`TeamState.agent_states["alice"]` 的值就是一个 `ChatAgentContainerState` 的 dump，里面的 `agent_state` 才是 `AssistantAgentState`。所以恢复时是三层 `model_validate`：团队 → 容器 → 智能体。

**`AssistantAgent` 的状态只有一个字段。** `save_state` 是 `AssistantAgentState(llm_context=await self._model_context.save_state()).model_dump()`，`load_state` 是 `model_validate` 后把 `llm_context` 注回 `model_context`。而 `model_context` 的保存结果是 `{"messages": [...]}` 形式（`UnboundedChatCompletionContext` 的默认）。所以**智能体状态 = 它的模型上下文**：工具、交接、记忆、系统消息都是构造期配置，不进状态。

**`on_reset` 与 `load_state` 的分工。** `AssistantAgent.on_reset` 只做 `await self._model_context.clear()`，不重置任何配置；`load_state` 也不 clear 而是直接灌入。因此「重置到初始状态」与「恢复到某个历史状态」是两条独立路径，前者清空、后者覆盖。若先 `load_state` 再 `run`，新消息会追加在恢复的历史之后。

**管理器状态的四个变体。** 轮询存 `next_speaker_index`（下次该谁，而不是上次是谁）；选择器存 `previous_speaker`（上次谁说的，用于避免重复）；Swarm 存 `current_speaker`（当前的持棒者）；图存 `remaining`、`enqueued_any`、`ready`（手写字典，不走 `GraphFlowManagerState`）。共同点是都存**基态**（消息线索 + 轮次），因为线索是选人判定的输入，必须完整恢复。

**选择器恢复时的上下文重建。** `SelectorGroupChatManager.load_state` 除了恢复线索与两个标量，还调 `_add_messages_to_context` 把线索里的 `BaseChatMessage` 逐条写进选人上下文。这意味着**选人上下文是从线索重建的，而不是从自己的 `save_state` 恢复的**——它是派生状态，不需要单独保存。对应地，`SelectorManagerState` 里也确实没有选人上下文的字段。

**图管理器的状态序列化绕开了自己的状态模型。** `GraphFlowManager.save_state` 返回裸 `dict`：`message_thread`、`current_turn`、`remaining`（`Counter` 转 `dict`）、`enqueued_any`、`ready`（`deque` 转 `list`）。`load_state` 反向。尽管定义了 `GraphFlowManagerState`（带 `active_nodes`），实际未使用——所以图的「当前执行到哪」完全由这三张表表达，没有额外的活跃节点列表。

**`SocietyOfMindAgent` 只保存内层。** 它返回 `SocietyOfMindAgentState(inner_team_state=await self._team.save_state())`，`load_state` 反向注入。注意它**没有保存自己的 `model_context`**（那里面存着跨调用累积的前情），所以恢复后的社心智能体会丢掉前情摘要，但内层团队的状态是完整的。

**版本与类型的实际用途。** `BaseState` 的 `type` 与 `version` 字段在保存/恢复路径上都没有被读取——`load_state` 一律用具体的状态类做 `model_validate`，不检查 dump 里的 `type` 是否匹配。它们目前是**标记性字段**：给人工排查和未来的迁移逻辑留的钩子。团队层对旧格式的兼容是通过捕获 `ValidationError` 转成「自 v0.4.9 起格式已变」的错误提示实现的，而不是靠版本号分支。

## 关键取舍

**源码事实：** 状态里**不包含订阅**。运行时层的 `save_state` 注释明确写了「当前不保存订阅状态，后续会加」。对团队而言这没有实际影响（订阅在 `_init` 里按配置重建），但对手工使用运行时的场景是个真实限制。

**源码事实：** 团队状态以名字为键，而名字在团队内必须唯一、跨团队可以重复。所以状态可以在团队间迁移，前提是目标团队的参与者名字一一对应。若名字对不上，`load_state` 抛 `ValueError("Agent state for X not found in the saved state.")`。

**源码事实：** `TeamState` 的 `agent_states` 类型是 `Mapping[str, Any]`，不做结构校验——深层的容器状态与智能体状态在各自的 `load_state` 里才被 `model_validate`。所以一个结构错误的状态可能通过团队层校验、在容器层或智能体层才失败。

**源码事实：** `BaseChatAgent` 的默认 `save_state` 返回 `BaseState().model_dump()`，即 `{"type": "BaseState", "version": "1.0.0"}`；默认 `load_state` 只做 `BaseState.model_validate(state)` 校验、不做任何应用。于是「无状态智能体」天然满足契约，且它的状态是空壳。`CodeExecutorAgent` 与 `UserProxyAgent` 都属于这一类。

**源码事实：** 状态保存不保证一致性。团队层文档明确提示：运行中调用 `save_state` 可能得到不一致的状态，应在停止后调用。原因是保存是逐个 `await` 的，参与者与管理员之间、参与者彼此之间没有快照点。

**架构推论：** 这套设计把「状态」定义成「每个对象自己声明它的可变部分」，由父层负责聚合。好处是组件的状态边界与它的实现细节同处一文件，新增智能体不必改框架；代价是聚合层无法知道总共有多少状态、也无法做统一的一致性切面——只能信任每个 `save_state` 返回的东西。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `BaseState` | 状态根：`type` 与 `version` 标记 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `TeamState` | 团队状态：参与者名字 → 状态 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `AssistantAgentState` | 智能体状态：只有模型上下文 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `ChatAgentContainerState` | 容器状态：内层状态 + 未处理缓冲 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `BaseGroupChatManagerState` | 管理器基态：消息线索 + 轮次 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `RoundRobinManagerState` | 轮询：多加一个下次下标 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `SelectorManagerState` | 选择器：多加一个上一发言人 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py` | `SwarmManagerState` | 交接：多加一个当前持棒者 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `AssistantAgent.save_state` | 把模型上下文包成 `AssistantAgentState` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `AssistantAgent.load_state` | 校验后回灌模型上下文 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat.py` | `BaseGroupChat.save_state` | 经运行时逐个取状态，以名字为键聚合 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_chat_agent_container.py` | `ChatAgentContainer.save_state` | 容器状态：内层状态 + 缓冲 dump |

## 参考

- [AutoGen 源码：_states.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/state/_states.py)
- [AutoGen 文档：状态管理](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/state.html)
- [AutoGen 文档：暂停与恢复团队](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/teams.html)

## 与主核 / AI 的关系

这套状态模型与主核的「检查点 + 恢复」是同构的，但严格程度差一个量级：主核的检查点要求所有页的 LSN 对齐，这里只要求「逐个对象把它的可变部分 dump 出来」。差别源于一致性要求的来源——主核要保证崩溃后数据可恢复，这里只需要「一次长对话能被切开」。

对 AI 工程最实用的三条推论。其一，**状态就是上下文**。`AssistantAgentState` 只有一个 `llm_context` 字段，意味着「把对话存到哪里」等价于「把模型看到的历史存到哪里」；换一个 `model_context` 实现（如 `BufferedChatCompletionContext`），保存下来的内容就自动变成「只保留最近 N 条」的版本——有界上下文会**丢失早期状态**，且这个丢失在保存时就已经发生，事后无法找回。

其二，**以名字为键使状态可移植但脆弱**。团队可以换运行时、换进程恢复，但参与者改名就等于状态失配。生产上应当把参与者名字当作稳定标识对待，而不是随手改的展示字段。

其三，**`memory` 不进状态**。`AssistantAgent` 的 `save_state` 只存模型上下文，`Memory` 存储自身若有状态需要另行保存与恢复。这意味着「长期记忆」与「对话状态」是两套独立的持久化，恢复对话不会恢复记忆——这个分工必须在使用时显式处理，否则会以为恢复后一切照旧。
