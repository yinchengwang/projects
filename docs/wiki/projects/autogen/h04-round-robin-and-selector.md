---
project: autogen
feature: round-robin-and-selector
title: 轮询与模型选人两种策略
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py
    symbols: [RoundRobinGroupChat, RoundRobinGroupChatManager, select_speaker]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py
    symbols: [SelectorGroupChat, _select_speaker, construct_message_history]
---

# 轮询与模型选人两种策略

## 一句话职责

同一套群聊协议下的两种选人实现：轮询用「下一个下标」给出确定性顺序，选择器用一次模型调用在候选名单里挑名字，前者零成本可复现，后者随内容自适应但不可复现。

## 机制是什么

两者都只覆盖管理器基类的三个抽象方法（`validate_group_state`、`select_speaker`、`reset`）以及状态存取，其余装配与轮次推进完全复用。

**轮询**的全部策略状态是一个整数 `_next_speaker_index`。`select_speaker` 取当前下标的名字，把下标推进成 `(i + 1) % len(participants)`，返回单元素列表。没有校验、没有候选过滤，甚至在只有一个参与者时也照样轮转（永远是它自己）。

**选择器**的策略状态是 `_previous_speaker` 与一个独立的 `ChatCompletionContext`（选人专用上下文，与各智能体的上下文无关）。`select_speaker` 的优先级链是：`selector_func` 自定义函数返回非 `None` 就用它；否则 `candidate_func` 过滤候选；否则按 `allow_repeated_speaker` 决定是否剔除上一发言人；候选只有一个就直接选它；候选多于一个才走 `_select_speaker` 的模型调用。

## 怎么实现的

**轮询的下标语义。** `_next_speaker_index` 在 `reset` 与 `load_state` 之外只在 `select_speaker` 里被修改。它记录的是「下一次该谁」，所以 `save_state` 存它而不是「上一次是谁」。`RoundRobinManagerState` 的三个字段正好覆盖恢复所需：消息线索、轮次、下标。

**选择器的上下文双写。** 选择器管理器重写了 `update_message_thread`：除了把消息追加进线索，还把其中的 `BaseChatMessage` 逐条写进选人上下文（`HandoffMessage` 先展开 `context` 再写自身）。所以选人用的历史是「所有聊天消息的模型视角转录」，与参与者各自看到的上下文彼此独立。`load_state` 恢复时会用 `_add_messages_to_context` 重建这个上下文——注意它是重建而非直接恢复，因此**选人上下文不可裁剪**（除非构造时换成有界 `model_context`，但那会在重建时丢掉早期消息）。

**提示语的三个占位符。** `_select_speaker` 用 `selector_prompt.format(roles=..., participants=str(participants), history=...)` 生成提示：`roles` 是每个参与者的 `名字: 描述` 逐行规范化空白后拼接；`participants` 是候选名字列表的 Python 字符串形式；`history` 由 `construct_message_history` 生成——只取 `UserMessage` 与 `AssistantMessage`，格式为 `f"{source}: {content}"` 后加两个换行，最后用换行连接。

**系统消息还是用户消息取决于模型家族。** `ModelFamily.is_openai(model_info["family"])` 为真时提示直接作为 `SystemMessage`；否则作为 `UserMessage(source="user")`。注释说明理由：很多非 OpenAI 模型需要有「用户提问」才能作答。

**重试循环的三种反馈。** `while num_attempts < max_attempts`（默认 3）里，每轮把模型的回答作为 `AssistantMessage(source="selector")` 追加进消息列表（形成多轮对话来纠错），然后 `_mentioned_agents` 统计候选人名字在回答里出现的次数。三种情况各自追加一条 `UserMessage` 反馈：零命中提示「请从候选里选一个」；多于一个命中提示「只选一个」；唯一命中但等于上一发言人且不允许重复，提示「换一个」。唯一命中且合法才返回。

**名字匹配的容错。** `_mentioned_agents` 拼的正则同时接受三种形态：原样名字、下划线换空格、下划线写成 `\_`，左右加 `\W` 边界，并把待搜文本前后各补一个空格以照顾行首行尾。这是为「模型把人名字写成 `story writer` 或 `story\_writer`」兜底。计数用 `re.findall` 的长度。

**流式选人。** `model_client_streaming` 为真时走 `create_stream`，两种块分别处理：字符串块在 `emit_team_events` 为真时塞进输出队列成为 `ModelClientStreamingChunkEvent`；最后一个 `CreateResult` 塞进 `SelectorEvent`。这两个事件都是**直接塞队列**，不走发布通道——所以选人的流式输出不会进入参与者视野。

**`_previous_speaker` 的更新时机。** 它只在 `select_speaker` 的**模型/单候选路径**末尾赋值，即 `self._previous_speaker = agent_name` 那一句。而 `selector_func` 返回非 `None` 时是提前 `return [speaker]`，**绕过**了这次赋值——所以自定义函数选出来的人不会成为下一轮的「上一发言人」，`allow_repeated_speaker=False` 对它无效。这是一个容易踩的不一致。

**候选过滤与模型自纠的差别。** `candidate_func` 生效时候选集完全由函数决定（文档说此时 `allow_repeated_speaker` 被忽略）；但 `_select_speaker` 内部仍然会检查「是否重复上一发言人」——如果候选函数把上一发言人放回名单，模型选了它照样会被打回。另一方面，模型仍可能选到候选之外的名字：`_mentioned_agents` 的统计范围是**全部参与者**而不是候选列表，注释解释了原因：「模型可能仍然选上一发言人，我们要抓到这个情况」。抓到之后走的是「重复发言人不允许」的反馈分支，而不是「不在候选里」的分支。

## 关键取舍

**源码事实：** 轮询策略完全无视内容。参与者说 `TERMINATE` 也照样轮到下一位——终止只看终止条件，与选人无关。

**源码事实：** 选择器的每次选人都是一次**带完整历史的模型调用**，且失败要重试最多 `max_selector_attempts` 次。所以一个 N 轮的对话会产生 N 次（甚至 3N 次）额外的模型调用，成本与轮次线性相关，与参与者数量无关（提示里只放名字与描述）。

**源码事实：** 选人失败后的兜底是「上次的发言人」，再不行「第一个参与者」——只记录 `trace_logger.warning`，不抛错。这意味着选人质量下降是静默的，只能靠日志发现。

**源码事实：** `_select_speaker` 的对话纠错是「追加反馈到消息列表再问一遍」，而不是用结构化输出或函数调用来约束。这是一个刻意的兼容性取舍：字符串 + 名字匹配对所有模型都能用，但把「选人」变成了「文本里找名字」，因此必须写容错正则。

**源码事实：** 选择器要求至少两个参与者（构造期 `ValueError`），轮询没有这个限制——只有一个参与者时轮询就是「每次都是它」，而选择器的模型选人在单候选场景下没有意义，代码里也直接跳过了模型调用。

**源码事实：** `selector_func` 与 `candidate_func` 都不可序列化。`SelectorGroupChatConfig` 里 `selector_func` 被注释掉，`_to_config` 里也留着注释说明它是刻意排除的。所以从配置恢复的选择器团队一定是「纯模型选人」。

**架构推论：** 两种策略展示了「确定性 vs 适应性」的经典权衡。轮询的确定性使整条链路可复现、可测试、零额外成本，适合结构化流程（写手→评审→写手）；选择器的自适应性适合参与者能力互补且顺序不固定的场景。但选择器把「谁下一步」交给一次模型调用，等于把一个可判定的调度问题换成了一个概率性的分类问题——这是它所有成本与不稳定性的根源。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py` | `RoundRobinGroupChatManager` | 轮询管理器：策略状态只有一个下标 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py` | `select_speaker` | 取当前下标、推进取模、返回单元素列表 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py` | `RoundRobinGroupChat` | 团队类：只提供管理器工厂与配置往返 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `SelectorGroupChatManager` | 选择器管理器：独立选人上下文与优先级链 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `select_speaker` | 自定义函数 → 候选过滤 → 单候选短路 → 模型选人 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `_select_speaker` | 重试循环、三类反馈、失败兜底 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `_mentioned_agents` | 三种写法的名字匹配与计数 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `construct_message_history` | 只保留用户/助手消息的转录格式 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py` | `update_message_thread` | 线索与选人上下文双写 |

## 参考

- [AutoGen 源码：_round_robin_group_chat.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_round_robin_group_chat.py)
- [AutoGen 源码：_selector_group_chat.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/teams/_group_chat/_selector_group_chat.py)
- [AutoGen 文档：群聊选人策略](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/selector-group-chat.html)

## 与主核 / AI 的关系

把选人看成一次「查询计划选择」会更清楚：轮询是固定执行计划，选择器是每次执行前重新代价估算并选计划。主核把计划缓存下来以避免重复开销，这里每次都要重新问模型——因为输入（对话内容）确实每轮都变，缓存意义不大，代价只能靠 `max_selector_attempts` 与提示长度控制。

对 AI 工程最实用的推论是**选人成本常常被低估**。一个 10 轮、4 个参与者的选择器团队，除了 10 次参与者推理，还有约 10 次选人推理；若选人提示里塞了完整历史，这 10 次调用的输入规模与参与者调用同级，总成本可能翻倍。可行的缓解手段有三种：用 `candidate_func` 把候选收窄（尤其在能由规则判断的阶段）、把 `selector_prompt` 里的历史裁短（选人未必需要全文）、以及在明确的阶段边界临时切回自定义 `selector_func`。
