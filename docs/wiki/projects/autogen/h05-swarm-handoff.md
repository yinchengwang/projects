---
project: autogen
feature: swarm-handoff
title: Swarm 交接式选人
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py
    symbols: [Swarm, SwarmGroupChatManager, select_speaker]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/base/_handoff.py
    symbols: [Handoff, handoff_tool]
---

# Swarm 交接式选人

## 一句话职责

让「下一个发言人」由当前发言人的消息内容自己决定：只有当发言人产出 `HandoffMessage` 时控制权才转移，否则同一个人继续说——选人策略被下沉成了对话内容的一部分。

## 机制是什么

`SwarmGroupChatManager` 的全部策略状态是一个字符串 `_current_speaker`，初值是 `participants[0]`。`select_speaker` 的行为是：线索为空就返回当前发言人；否则**从后往前**扫线索找最近的 `HandoffMessage`，找到就把 `_current_speaker` 更新为它的 `target` 并返回；扫不到就继续返回当前发言人。

与轮询/选择器相比，这里没有「候选集」、没有「上一发言人」、也没有额外模型调用。选人退化成一次线扫描，代价 O(线索长度) 的纯内存操作。

`Handoff` 是交接的配置对象：`target`（必填）、`description`、`name`、`message` 三个可选字段各有默认值生成规则——`description` 缺省为 `Handoff to {target}.`；`name` 缺省为 `transfer_to_{target}` 转小写，显式给出时必须是合法 Python 标识符；`message` 缺省为 `Transferred to {target}, adopting the role of {target} immediately.`。

## 怎么实现的

**交接工具是怎么造出来的。** `Handoff.handoff_tool` 每次访问都新建一个 `FunctionTool`，包住一个无参函数 `_handoff_tool()`，它唯一的作用是返回 `self.message`。构造时带上 `name=self.name`、`description=self.description`、`strict=True`。于是「交接」对模型来说就是一个无参函数调用——模型想转移控制权，就调用这个工具；工具返回的字符串会被 `AssistantAgent` 用作 `HandoffMessage.content`。

**启动校验的两段扫描。** `validate_group_state` 先看本次传入的启动消息：只要其中出现 `HandoffMessage`，就校验它的 `target` 是否在参与者名单内，合法则立即 `return`（不再看历史）。若启动消息里没有交接消息，则**反向**扫已有线索找最近的 `HandoffMessage`：若它的 `target` 非法就 `ValueError`；合法则 `return`。注释解释了一处细节：「不要越过最近的交接消息继续往前看」——更早的交接消息已经失效，用它们判断会误报。

这两段校验合起来覆盖了两种恢复场景：用新的 `HandoffMessage` 作为任务恢复（例如用户接手后再交还），以及不带新交接消息直接续跑。

**`select_speaker` 的两处差异。** 第一处是空线索返回 `[self._current_speaker]`（列表），而扫不到交接消息时返回 `self._current_speaker`（裸字符串）。基类对两种形态都能接受（`isinstance(str)` 就包成列表），所以功能无差别，但类型不一致是源码现实。第二处是找到交接消息后有一句 `assert self._current_speaker in self._participant_names`——这是**断言而不是异常**，依赖 `validate_group_state` 已经校验过；若校验被绕过（例如手工 `load_state` 注入了非法 target），断言会在 `-O` 优化下被剥掉。

**首发是「参与者列表第一个」。** 构造期就把 `_current_speaker` 设为 `participants[0]`，且 `reset` 也回到 `participants[0]`。顺序因此是语义的一部分：把谁放在第一位决定了对话由谁开场。

**首参与者必须能交接。** `Swarm.__init__` 校验 `HandoffMessage in participants[0].produced_message_types`，否则 `ValueError("The first participant must be able to produce a handoff messages.")`。这条约束的逻辑是：如果第一个人无法交接，控制权永远无法转移，整个团队退化成单智能体。

**不支持嵌套团队。** `Swarm.__init__` 逐个检查 `isinstance(participant, ChatAgent)`，不是就 `TypeError`。这与轮询/选择器相反——那两者允许把 `Team` 当参与者。原因是 Swarm 的选人靠交接消息的 `target` 匹配参与者名字，而嵌套团队产出的是 `GroupChatTeamResponse`，其内部的 `HandoffMessage` 语义与「交接给团队」不一致。

**交接在智能体侧的执行路径。** `AssistantAgent` 执行工具调用时先匹配交接工具（在遍历工作台之前）；匹配到就跑 `handoff_tool.run_json` 并用 `return_value_as_string` 得到字符串结果。随后 `_check_and_handle_handoff` 扫本轮的 `FunctionCall` 列表，命中就构造 `HandoffMessage(content=交接工具的返回值, target=..., source=..., context=...)` 并提前 `return`。

**交接上下文的三支。** 若本轮既有普通工具调用又有交接，`context` 是 `[AssistantMessage(普通调用列表), FunctionExecutionResultMessage(结果)]` 两条；若只有交接但有 `thought`，`context` 是一条内容为 thought 的 `AssistantMessage`；否则 `context` 为空列表。接手方的 `_add_messages_to_context` 会先展开 `context` 再写交接消息本体，于是接手方能读到「前任做了什么」。

**状态三个字段。** `SwarmManagerState` 继承管理器基态（消息线索、轮次），加上 `current_speaker`。`save_state` 把线索逐条 `dump`、`load_state` 用消息工厂重建——因此线索里的消息类型必须在团队的消息工厂里注册过。

## 关键取舍

**源码事实：** 选人完全由内容驱动，没有兜底策略。若当前发言人既没有交接也没有产出任何消息（例如异常），控制权不会转移；若它反复产出普通消息，就是自己在自言自语——团队不会自动换人。这与轮询的「总会轮到下一个」形成鲜明对比。

**源码事实：** `_current_speaker` 的更新只发生在 `select_speaker` 里。因此 `save_state` 保存的 `current_speaker` 是「上次选人结果」，恢复后即使线索里已有更新的交接消息，也要等下一次 `select_speaker` 才会同步。不过 `select_speaker` 每次都从线索尾部重扫，所以结果仍然正确——这个字段主要是给「线索为空」的冷启动兜底。

**源码事实：** 第一次启动时若任务消息里就带 `HandoffMessage`，它会被直接当成启动校验依据，但**首发选人仍走 `select_speaker`**：由于此时线索已包含这条交接消息（`handle_start` 会把它追加进线索），扫描会命中它并选中其 `target`。所以「用交接消息指定开局人选」是可行的，只要它进得了线索。

**源码事实：** `Handoff.name` 校验了 `isidentifier()`，但没校验与工具名冲突——这个冲突检查在 `AssistantAgent` 构造期做（交接工具名与工具名取交集，非空即 `ValueError`）。所以同一个名字在两处被检查，责任分属两层。

**源码事实：** `Handoff.handoff_tool` 是 property，每次访问都构造新的 `FunctionTool`。对 `AssistantAgent` 无影响（构造期取一次存进 `_handoff_tools`），但如果调用方在多处反复访问，会得到多个等价但不同实例的工具。

**架构推论：** Swarm 把「控制流」编码进「数据」——下一位发言人不是由外部调度器决定，而是由消息内容里的 `target` 声明。这让流程可以被模型自由改写（模型可以跳到任意合法参与者），也让流程无法被外部约束（除非限制每个智能体的 `handoffs` 列表）。灵活性与可控性是同一枚硬币的两面。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py` | `SwarmGroupChatManager` | 交接式管理器：状态只有一个当前发言人 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py` | `select_speaker` | 反向扫线索找最近交接，找不到保持原发言人 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py` | `validate_group_state` | 启动消息与线索两段校验交接目标合法性 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py` | `Swarm` | 团队类：首参与者必能交接、参与者必须是 `ChatAgent` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_handoff.py` | `Handoff` | 交接配置：目标、工具名、工具描述、返回消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_handoff.py` | `handoff_tool` | 无参函数工具，返回值即交接消息内容 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/agents/_assistant_agent.py` | `_check_and_handle_handoff` | 智能体侧：从函数调用识别交接并打包上下文 |

## 参考

- [AutoGen 源码：_swarm_group_chat.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_swarm_group_chat.py)
- [AutoGen 源码：_handoff.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/base/_handoff.py)
- [AutoGen 文档：Swarm](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/swarm.html)

## 与主核 / AI 的关系

把「控制权转移」实现成一次工具调用，是从主核的「权限移交」里能借到的思路：不是外部调度器改配置，而是持有者主动把令牌交出去。差别是主核的移交有严格的所有权校验，而这里的校验只在两处——`AssistantAgent` 构造期确认交接目标名合法、管理器在启动与选人时确认 target 在参与者名单内；除此之外模型可以自由指定任何已注册的交接目标。

对 AI 工程最实用的三条推论。其一，**Swarm 的行为完全取决于交接工具的 `description`**：模型是看着描述决定何时移交的，描述写得模糊就会乱移交或不移交。其二，**「不支持嵌套团队」是重要的能力边界**：Swarm 只能扁平地组织智能体，需要层级时得在外层再套一个轮询/选择器团队。其三，**`HandoffTermination` 是 Swarm 的人类接管钩子**：把 `target="user"` 的交接与 `HandoffTermination(target="user")` 配合，可以让团队在「需要人类」时干净地停下来并把状态交出去，等用户回应后再用一条 `HandoffMessage` 作为新任务恢复——这比阻塞式的人在回路更适合真实部署。
