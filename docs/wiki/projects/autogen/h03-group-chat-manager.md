---
project: autogen
feature: group-chat-manager
title: 群聊管理器与轮次推进
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py
    symbols: [BaseGroupChatManager, _transition_to_next_speakers, select_speaker]
---

# 群聊管理器与轮次推进

## 一句话职责

团队里唯一知道「现在轮到谁、这一轮结束没有、什么时候停」的组件：它维护公共消息线索与轮次计数，把参与者响应汇聚成一轮，再按子类给定的策略选出下一批发言人。

## 机制是什么

`BaseGroupChatManager` 是抽象类，构造参数里除了参与者元信息（话题类型、名字、描述）之外，还有三样驱动状态的东西：`termination_condition`（可为 `None`）、`max_turns`（可为 `None`）、以及一条由团队持有的 `output_message_queue`。它自身维护：

- `_message_thread`：公共线索，累积所有参与者的事件与消息。
- `_current_turn`：轮次计数。
- `_active_speakers`：已被点名、尚未回应的发言人列表。
- `_participant_name_to_topic_type`：名字到话题的映射。

三个抽象方法划清了子类的责任边界：`validate_group_state`（收到 `GroupChatStart` 时校验输入是否合法）、`select_speaker`（返回下一批发言人名字）、`reset`（清空自身状态）。

## 怎么实现的

**构造期的五条硬校验。** `max_turns` 若非 `None` 必须大于 0；参与者话题类型数量必须与描述数量一致；话题类型必须唯一；群话题类型不得出现在参与者话题里；`reset` 之外的初始化把 `_active_speakers` 置空。前两条防的是配置错误，第四条防的是「管理器把群话题当成某个参与者」这种路由歧义。

**启动分支的三段。** `handle_start` 先检查「上一轮是否已经终止」：若 `termination_condition.terminated` 为真，直接构造 `StopMessage("The group chat has already terminated.")` 并 `_signal_termination` 返回。这是一个**不重置就重跑**的保护：终止条件已达成就不能再无任务继续。之后 `validate_group_state`，再由子类（例如 Swarm）判断输入里的交接消息是否合法。

若有启动消息，按顺序做四件事：发 `GroupChatStart` 到输出话题（供观察者看到任务本身）；按 `output_task_messages` 开关把这些消息塞进输出队列；发 `GroupChatStart` 到**群话题**（让所有参与者把它们进缓冲）；追加进 `_message_thread`；然后用这批消息跑一次 `_apply_termination_condition`——**启动消息本身就可以直接满足终止条件**（例如任务里就写了 `TERMINATE`）。最后无论前面是否提前终止，都走 `_transition_to_next_speakers` 选第一批人。

**响应汇聚与「一轮」的判定。** `handle_agent_response`（同时接 `GroupChatAgentResponse` 与 `GroupChatTeamResponse`）先算 delta：单智能体是 `inner_messages + chat_message`，嵌套团队是整个 `result.messages`。delta 追加进线索，然后从 `_active_speakers` 里移除该发言人。若还剩活跃发言人就直接返回——**这就是「并行发言人」的实现**：一批人被同时点名，每人的响应各自触发一次本方法，只有最后一个响应到达时才推进轮次。移除后若队列空了，才跑 `_apply_termination_condition(delta, increment_turn_count=True)` 并选下一批。

注意这里有两个隐含约束：`_active_speakers.remove(message.name)` 若名字不在列表里会抛 `ValueError`（由外层 `except Exception` 捕获后转为终止事件）；且「一轮」的定义是「一批发言人全部响应完毕」，不是「一条消息」。

**终止判定的两段优先级。** `_apply_termination_condition` 先问终止条件：`await self._termination_condition(delta)` 返回非 `None` 就是终止，此时**重置终止条件与轮次计数**，`_signal_termination` 发出停止消息并返回 `True`。再处理轮次：`increment_turn_count` 为真则 `_current_turn += 1`，检查 `>= max_turns` 则同样构造 `StopMessage("Maximum number of turns N reached.")`、重置、发出、返回 `True`。

顺序很重要：终止条件优先于轮次上限，且**只要终止就清零轮次**。这意味着 `max_turns` 是「本次运行内」的上限而非「团队生命周期内」的上限——每次终止后重跑都从 0 计。

**选人与点名。** `_transition_to_next_speakers` 把 `select_speaker(self._message_thread)` 包成 `asyncio.ensure_future` 并 `cancellation_token.link_future`（这样取消令牌能中断一次可能正在进行的模型调用）。返回值允许单个字符串或字符串列表，统一成列表后逐个校验名字在参与者映射里存在，不合法直接 `RuntimeError`。然后 `_log_speaker_selection` 记录选人结果，最后对每个名字发一条 `GroupChatRequestPublish` 到该参与者话题，并把名字压进 `_active_speakers`。

**`_log_speaker_selection` 的开关。** 它产出 `SelectSpeakerEvent`，但只在 `emit_team_events` 为真时才既发到输出话题又进输出队列；否则什么都不做。所以「谁被选中」默认不可见。

**错误与终止的三条通道。** `_signal_termination` 把 `GroupChatTermination` 同时发到输出话题和塞进输出队列；`_signal_termination_with_error` 额外带上 `SerializableException`；`handle_group_chat_error` 收到参与者侧发来的 `GroupChatError` 时也调后者。`handle_agent_response` 里任何异常都会被转成 `_signal_termination_with_error` 后**再 `raise`**——保证错误既能让 `run_stream` 侧收到终止事件，也让运行时能看到异常。

**事件型处理器与 RPC 的分工。** `handle_start`、`handle_agent_response`、`handle_group_chat_message`、`handle_group_chat_error` 是 `@event`（单向，不返回）；`handle_reset`、`handle_pause`、`handle_resume` 是 `@rpc`（调用方等待）。`handle_group_chat_message` 只做一件事：把消息塞进输出队列——这是团队输出的唯一入口。

**顺序消息类型的选择。** 管理器把 `GroupChatStart`、`GroupChatAgentResponse`、`GroupChatTeamResponse`、`GroupChatMessage`、`GroupChatReset` 声明为需要 FIFO 锁的类型。注意 `GroupChatTermination` 不在其中——终止事件只单向发出，不会被自己收到。

## 关键取舍

**源码事实：** 轮次语义是「一批发言人齐活算一轮」。因此配了并行发言时，`max_turns` 数的是批次数而不是消息数；想按消息数限制应改用 `MaxMessageTermination`。

**源码事实：** 终止后立刻重置终止条件与轮次。这让「一个团队对象可以被反复 `run`」成为可能，但也意味着**用户自己实现的终止条件若不在 `reset` 里正确清状态，第二次运行就会从脏状态开始**。

**源码事实：** 启动消息会先发到输出话题再发到群话题。同一条消息因此出现两次（一次给观察者、一次给参与者），`output_task_messages=False` 只关掉「进输出队列」这一条，不影响广播。

**源码事实：** `GroupChatRequestPublish` 是空消息，管理器**不告诉参与者「你收到的请求属于第几轮」**。参与者的缓冲里攒的是自上次清空以来的所有广播，它无法区分「这是新一轮」还是「同一轮的追加」。轮次概念只存在于管理器内部。

**源码事实：** 选人调用被包进 future 并挂取消令牌，而发 `GroupChatRequestPublish` 时也带 `cancellation_token`。这使「选人时的一次慢模型调用」和「投递请求」都可被取消；但 `_active_speakers` 的清理只在响应到达时发生，取消路径不会主动回滚它——取消后团队处于不一致状态，源码文档在 `run`/`run_stream` 处明确提示过这点。

**架构推论：** 管理器的设计把「策略」与「协议」彻底分开：谁来选人（轮询、模型、交接、图）由子类决定，选完人怎么点名、怎么攒轮、怎么停由基类决定。这正是新增策略成本极低的原因，也是四种群聊变体能在同一套装配代码里工作的前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `BaseGroupChatManager` | 管理器基类：线索、轮次、活跃发言人 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `handle_start` | 启动分支：已终止保护、广播、首轮终止检查、选首批 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `handle_agent_response` | 汇总 delta、减活跃发言人、满一批后判停并选下一批 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `_transition_to_next_speakers` | 选人（可取消）、校验名字、逐人发请求并登记活跃 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `_apply_termination_condition` | 终止条件优先，其次轮次上限；两者命中都重置 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `_log_speaker_selection` | 受 `emit_team_events` 控制的 `SelectSpeakerEvent` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `_signal_termination` | 终止双通道：输出话题 + 输出队列 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py` | `handle_group_chat_message` | 团队输出的唯一入口，塞队列 |

## 参考

- [AutoGen 源码：_base_group_chat_manager.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_base_group_chat_manager.py)
- [AutoGen 文档：Teams 与轮次](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/teams.html)
- [AutoGen 文档：终止条件](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/termination.html)

## 与主核 / AI 的关系

管理器是这套编排里唯一的「串行化点」。所有参与者都能并发地向群话题广播，但「谁接着说」与「何时停」的判断完全收在它一条消息队列上（运行时是单线程 actor 队列，再加上容器层的 FIFO 锁）。这与主核把并发收敛到锁的顺序化思路一致：并发只发生在副作用侧，决策侧保持串行。

对 AI 工程最实用的三条推论：其一，**轮次上限与消息上限是两种不同语义**，多发言人场景下它们相差数倍；其二，**终止条件是有状态的**，`terminated` 为真后再次调用会抛 `TerminatedException`，所以任何自定义终止条件必须在 `reset` 里清干净状态，否则重跑会直接崩；其三，**管理器是唯一能看到完整线索的地方**，`_message_thread` 包含所有参与者的过程消息（`inner_messages`），因此基于线索的模型选人（SelectorGroupChat）实际读到的信息量远大于单个参与者能看到的。
