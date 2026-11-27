---
project: autogen
feature: termination-conditions
title: 终止条件与布尔组合
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py
    symbols: [TerminationCondition, AndTerminationCondition, OrTerminationCondition]
  - path: python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py
    symbols: [StopMessageTermination, MaxMessageTermination, TextMentionTermination]
---

# 终止条件与布尔组合

## 一句话职责

把「这一轮该停了」做成一个有状态的可调用对象：每次只接收「自上次调用以来的新消息」，返回 `StopMessage` 或 `None`，并允许用 `&` 与 `|` 组合成表达式树。

## 机制是什么

`TerminationCondition` 是一个 ABC，只有三个抽象成员：`terminated` 属性（是否已达终止）、`async __call__(messages)`（判定）、`async reset()`（清状态）。`component_type` 固定为 `"termination"`，因此所有条件都能进组件配置。

两个组合器把它变成表达式树：`__and__` 返回 `AndTerminationCondition(self, other)`，`__or__` 返回 `OrTerminationCondition(self, other)`。两者都是 `TerminationCondition` 的子类，因此组合可以嵌套——`(a | b) & c` 是合法的。

仓库内置的具体条件分四类：按消息类型（`StopMessageTermination`、`TextMessageTermination`）、按计数（`MaxMessageTermination`、`TokenUsageTermination`）、按内容（`TextMentionTermination`、`FunctionCallTermination`）、按外部事件（`HandoffTermination`、`SourceMatchTermination`、`TimeoutTermination`、`ExternalTermination`、`FunctionalTermination`）。

## 怎么实现的

**增量语义是核心不变式。** 所有具体条件的 `__call__` 都只遍历**传入的那批消息**，不保存历史。`MaxMessageTermination` 累加计数，`TokenUsageTermination` 累加 token，其余都是「在这批里找特征」。团队管理器每次只把「本轮 delta」传进来（启动消息一批、每个响应一批），因此条件看到的是增量流而不是全量历史。这条不变式决定了：条件必须自己维护跨调用状态（计数、标记位），不能依赖「每次拿到全量再重算」。

**终止后再次调用是错误。** 每个具体条件开头都有同一句：`if self.terminated: raise TerminatedException(...)`。`TerminatedException` 继承自 `BaseException` 而不是 `Exception`——刻意避开 `except Exception` 的普遍捕获。团队管理器在判定终止后会立刻 `await termination_condition.reset()`，所以正常流程不会撞上这条异常；撞上说明调用方在未重置的情况下重复使用。

**`MaxMessageTermination` 的计数口径。** 默认只数 `BaseChatMessage`；`include_agent_event=True` 时把 `BaseAgentEvent` 也算进去。计数在 `__call__` 里先加后比，所以阈值语义是「达到或超过」即停。注意它数的是**消息条数**，不是轮次，也不是模型调用次数。

**`TextMentionTermination` 用 `to_text()`。** 判定是 `self._termination_text in message.to_text()`，即「渲染给人看的文本」而不是「发给模型的文本」。对 `TextMessage` 两者相同；对 `MultiModalMessage`，`to_text()` 会把图片渲染成 `<image>` 而不是 base64，所以无法通过图片内容触发。可选的 `sources` 参数先按来源过滤，不在名单里的消息直接 `continue`。

**`sources` 会在配置往返中丢失。** `TextMentionTerminationConfig` 只有 `text` 一个字段，`_to_config` 也只写 `text`，`_from_config` 构造时 `sources` 缺席即取默认 `None`。所以一个带来源白名单的文本终止条件，`dump_component` 之后再 `load_component` 回来就变成「任何来源都算」——过滤范围被静默放宽，这是本组条件里唯一会改变行为的序列化损失。

**`TokenUsageTermination` 的三个阈值是或关系。** `terminated` 属性把三个上限任一命中即为真；`__call__` 遍历消息累加 `models_usage` 的 `prompt_tokens` 与 `completion_tokens`（只处理非 `None` 的使用量），累加后再判。构造期要求三者至少给一个，否则 `ValueError`。这意味着**没有 usage 信息的消息不计费**——自定义消息若不带 `models_usage`，这个条件永远不会触发。

**`AndTerminationCondition` 的两个细节。** 一是 `__call__` 只对「尚未终止」的子条件并发求值（`asyncio.gather`，过滤掉 `condition.terminated` 为真的），已经终止的不再调用——避免触发 `TerminatedException`。二是它把各次拿到的 `StopMessage` 累积进 `self._stop_messages`，最终拼成一条：`content` 用 `", "` 连接所有累积内容，`source` 用 `", "` 连接所有来源。累积而不是只用本轮，是因为「与」语义下不同子条件在不同轮次达成，最终消息需要把全过程的原因都带上。

**`AndTerminationCondition` 的短路条件。** 只要本轮求值结果里有任何一个 `None`，就返回 `None`（未终止）；全部非 `None` 才终止。注意 `terminated` 属性是 `all(condition.terminated ...)`——即所有子条件都必须已达终止。这两个视角在正常流程下一致，因为「本轮全部返回非 None」之后所有子条件的 `terminated` 都会变真。

**`OrTerminationCondition` 更直接。** 每轮对**全部**子条件并发求值（不跳过已终止的），过滤掉 `None` 后若有剩余就拼成一条 `StopMessage` 返回。它没有 `_stop_messages` 累积。`terminated` 是 `any(...)`。终止后重复调用它抛的是 `RuntimeError` 而不是 `TerminatedException`——与 `AndTerminationCondition` 不一致，是源码现实。

**组合器的可序列化性。** 两者都实现 `_to_config` / `_from_config`，把子条件逐个 `dump_component()` 成 `AndTerminationConditionConfig(conditions=[...])`，恢复时逐个 `TerminationCondition.load_component`。所以组合树可以完整往返 YAML/JSON——前提是每个叶子条件本身可序列化。

**`ExternalTermination` 的双标志位。** 它有 `_terminated` 与 `_setted` 两个布尔：`set()` 只置 `_setted`，下次 `__call__` 时才把它转成 `_terminated` 并返回 `StopMessage`。这个间接层让「外部请求停止」这个动作可以在任意线程/协程里安全发出，真正的终止仍在判定点发生。`reset` 两个都清。

**`TimeoutTermination` 用单调时钟。** `_start_time` 在构造与 `reset` 时取 `time.monotonic()`，判定是比较 `time.monotonic() - _start_time >= timeout_seconds`。注意它**不看消息**，只要有调用就可能超时——因此超时的检查粒度等于轮次粒度，不会在某一轮中途打断。

## 关键取舍

**源码事实：** 条件是有状态的且一次性。`terminated` 一旦为真就必须 `reset` 才能复用。这解释了团队管理器为什么在每次终止后都立刻重置条件与轮次——为了让同一个团队对象能被反复 `run`。

**源码事实：** 「与」组合会累积历史 `StopMessage` 而「或」不会。结果是：`a & b` 的停止原因包含两个子条件的说明，`a | b` 只包含本轮命中的那些。语义上「与」需要多轮才能成立，累积是必要的；「或」一击即中，无需累积。

**源码事实：** `TerminatedException` 继承 `BaseException`，而 `OrTerminationCondition` 在已终止时抛 `RuntimeError`。同一个抽象契约下两种异常类型，调用方若要统一处理必须同时捕获——这是内部实现不一致留下的坑。

**源码事实：** 所有内置条件都不做去重。`StopMessageTermination` 只要本批里有一条 `StopMessage` 就终止；`SourceMatchTermination` 只要来源匹配就终止。若同一批里有重复特征，语义等价，只影响性能。

**源码事实：** `FunctionalTermination` 是唯一的逃逸口，接受同步或异步函数（用 `asyncio.iscoroutinefunction` 判定），但**不可序列化**（没有 `_to_config` / `_from_config`，只继承 `TerminationCondition` 而非 `Component`）。所以配置驱动的团队里不能用函数式条件。

**架构推论：** 把终止做成「增量判定的有状态对象」而不是「对全量历史跑一次纯函数」，是为了让判定成本与轮次成正比而不是与历史长度成正比。代价是条件成为团队状态的一部分：保存/恢复团队时，若条件状态没被正确保存或重置，恢复后的行为会与预期不同——这也是基类管理器在终止路径上无条件重置条件的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py` | `TerminationCondition` | 抽象契约：`terminated`、`__call__`、`reset` 与两种布尔组合运算 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py` | `TerminatedException` | 终止后重复调用的错误，继承 `BaseException` |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py` | `AndTerminationCondition` | 「与」组合：跳过已终止子条件、累积停止消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py` | `OrTerminationCondition` | 「或」组合：全量并发求值、本轮即拼装停止消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `StopMessageTermination` | 收到 `StopMessage` 即停 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `MaxMessageTermination` | 按消息条数计数，可选含事件 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `TextMentionTermination` | 按 `to_text()` 的文本包含判定，可按来源过滤 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `TokenUsageTermination` | 三个 token 阈值，或关系，只统计带 usage 的消息 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `ExternalTermination` | 外部 `set()` 触发，双标志位间接生效 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `HandoffTermination` | 指定目标的交接消息即停，人在回路的钩子 |
| `python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py` | `FunctionalTermination` | 函数式逃逸口，不可序列化 |

## 参考

- [AutoGen 源码：_termination.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/base/_termination.py)
- [AutoGen 源码：_terminations.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-agentchat/src/autogen_agentchat/conditions/_terminations.py)
- [AutoGen 文档：终止条件](https://microsoft.github.io/autogen/stable/user-guide/agentchat-user-guide/tutorial/termination.html)

## 与主核 / AI 的关系

这套设计对应主核里「有状态的游标 / 流式聚合器」：不重放历史，只消费增量，内部维护足以判定的最小状态。主核的聚合在数据库内部完成，这里的聚合在应用层完成，抽象结构一致——也因此共享同一个失效模式：状态与输入流一旦错位（漏喂一批、重喂一批），结论就会错，而且很难从结果上看出来。

对 AI 工程最实用的三条推论。其一，**必须至少配一个与内容无关的兜底条件**。仅靠 `TextMentionTermination("TERMINATE")` 依赖模型听话，一旦模型忘了说，团队会一直跑到 `max_turns`，而 `max_turns` 默认是 `None`——无上限。`MaxMessageTermination` 或 `TimeoutTermination` 应当默认开启。其二，**`TokenUsageTermination` 的成本信号依赖消息带 `models_usage`**，而 `AssistantAgent` 产出的 `TextMessage` 会带上 usage，事件类则不保证，所以用它做预算控制时要知道口径是「至少」。其三，**组合表达式的成本不是相加而是相乘**：`a & b` 每轮都要对所有未终止子条件求值，嵌套组合树的求值点会随深度增加；在长对话里应优先用单层 `|` 拼接多个兜底条件。
