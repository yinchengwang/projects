---
project: autogen
feature: closure-agent
title: 闭包智能体与轻量注册
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_closure_agent.py
    symbols: [ClosureAgent, register_closure, ClosureContext]
---

# 闭包智能体与轻量注册

## 一句话职责

让「一个异步函数」直接充当智能体：函数签名里写明它处理什么类型，`register_closure` 一次性把类型、订阅、序列化器都登记好，省掉定义类与工厂的样板。

## 机制是什么

`ClosureAgent` 继承 `BaseAgent`，但不要求子类化——用户只提供一个函数：

```python
async def closure(ctx: ClosureContext, message: T, ctx: MessageContext) -> Any
```

`ClosureContext` 是给函数的窄接口，只有三样东西：`id`（当前智能体的 `AgentId`）、`send_message`、`publish_message`。它**没有** `metadata`、没有状态方法——闭包智能体被明确设计成无状态。

`register_closure` 是唯一入口，做五件事：造一个返回 `ClosureAgent` 的工厂；调 `ClosureAgent.register` 并强制 `skip_class_subscriptions=True`；在有 `SubscriptionInstantiationContext` 的上下文里求值用户传入的 `subscriptions` 回调并逐条添加；为闭包能处理的消息类型登记序列化器；返回 `AgentType`。

## 怎么实现的

**实例化必须在运行时的工厂调用里。** `ClosureAgent.__init__` 直接调 `AgentInstantiationContext.current_runtime()` 与 `current_agent_id()`，任一失败就抛 `RuntimeError`，文案写明「不能在运行时上下文外直接实例化」。因此在测试里想手工构造一个 `ClosureAgent` 是不可能的，必须借运行时的工厂路径。

**处理类型靠函数签名反射。** `get_handled_types_from_closure` 用 `inspect.getfullargspec` 取参数名，要求正好 3 个（不含 `self`，因为闭包是自由函数），取第 2 个参数的名字作为消息参数名，再用 `get_type_hints` 取该参数的注解，交给 `get_types` 解析。返回注解也必须存在，否则断言失败——即使闭包返回 `None`，也必须显式标注。注意这里断言文案写的是「Closure must have 4 arguments」，与实际检查的 3 个不一致，属于文案笔误。

**类型不符的三种处置，行为不等价。** `on_message_impl` 先判 `type(message) not in self._expected_types`，然后：

| `unknown_type_policy` | 行为 |
|---|---|
| `"warn"`（默认） | 发一条 `UserWarning`，然后 `return None`，闭包**不执行** |
| `"error"` | 抛 `CantHandleException` |
| `"ignore"` | 不进任何分支，继续调用闭包 |

也就是说 `"ignore"` 不是「忽略这条消息」，而是「忽略类型不符这件事，照样交给闭包」。这在订阅把不该来的消息送过来时最危险：`"warn"` 至少还返回 `None`，`"ignore"` 会把意外类型的消息直接喂进业务函数。

**无状态是硬约定。** `save_state` 返回 `{}`，`load_state` 什么都不做，docstring 直接写「闭包智能体没有状态」。因此它在整运行时 `save_state` 里只会贡献一个空字典。

**类订阅被断言清零。** `register_closure` 里有 `assert len(cls._unbound_subscriptions()) == 0`，并把 `skip_class_subscriptions=True` 传给 `register`。含义是：闭包智能体的订阅只能通过 `subscriptions=` 参数给，用 `@default_subscription` 之类的装饰器挂在 `ClosureAgent` 子类上不会生效。

**订阅回调的求值时机。** `subscriptions` 是 `Callable[[], list[Subscription] | Awaitable[list[Subscription]]]`，不是列表。它被 `SubscriptionInstantiationContext.populate_context(agent_type)` 包着调用，所以闭包里可以安全地写 `DefaultSubscription()`——后者正是靠这个上下文推断 agent type。同时兼容同步与异步回调（`inspect.isawaitable` 分叉）。

**直连订阅默认开着。** `register_closure` 把 `skip_direct_message_subscription` 透传给 `register`，默认 `False`，于是每个闭包智能体除了用户给的订阅，还会自动获得一条 `TypePrefixSubscription`（前缀为 `agent_type + ":"`）。

## 关键取舍

**源码事实：** 闭包智能体彻底放弃状态。换来的是「提取结果」这类只读消费者的最简写法：把消息 `put` 进外部 `asyncio.Queue`，状态由外部持有。代价是它无法参与 `save_state`/`load_state` 的断点恢复，重启后什么都没留下。

**源码事实：** 三个 `unknown_type_policy` 值的语义不对称：一个跳过、一个抛错、一个照常执行。默认选 `warn`（跳过）是最保守的一档——消息不会进业务代码，也不会让运行时崩。文档字符串对 `"ignore"` 的描述是「抑制这条告警」，容易被读成「忽略消息」，实际是「忽略不匹配」。

**源码事实：** 处理类型在**注册期**就从函数签名解析一次，之后不再反射。`_expected_types` 是构造时算好的元组，`on_message_impl` 只做一次 `in` 判断。这让分发热路径极轻，但也意味着运行期动态改注解没有意义。

**源码事实：** `ClosureContext` 只暴露 `id` / `send_message` / `publish_message`，故意不给 `metadata`、`runtime`、`save_state`。这是一条接口收窄的取舍：闭包函数只需要「我是谁、我能发消息」这两件事，给得越多越容易依赖实现细节。

**架构推论：** 把「订阅回调求值」放在 `register` 之后、用返回的 `agent_type` 作为上下文，使 `DefaultSubscription()` 不必显式传 agent type。这个顺序不可换：若先求值订阅再注册，上下文里就没有类型可推断。

**架构推论：** 闭包智能体是「智能体」这条抽象的最小可行实例——一个处理函数、一个地址、无状态。它证明了框架的抽象边界确实落在 `on_message_impl` 上，而不是落在类继承上。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureAgent` | 用闭包实现的智能体，无状态 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureAgent.__init__` | 从实例化上下文取 runtime 与 id，否则报错 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureAgent.on_message_impl` | 按 `unknown_type_policy` 处置类型不符，再调闭包 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureAgent.register_closure` | 注册工厂、订阅、序列化器的统一入口 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureAgent.save_state` | 恒返回空字典，无状态约定 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `ClosureContext` | 闭包可见的窄接口：id + 发消息 |
| `python/packages/autogen-core/src/autogen_core/_closure_agent.py` | `get_handled_types_from_closure` | 从函数签名解析可处理的消息类型 |

## 参考

- [AutoGen 源码：_closure_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_closure_agent.py)
- [AutoGen 文档：用智能体提取结果](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/cookbook/extracting-results-with-an-agent.html)
- [AutoGen 文档：Agent 与 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/agent-and-agent-runtime.html)

## 与主核 / AI 的关系

闭包智能体与主核里「回调式迭代器 / 访问者」的位置相近：把一段外部逻辑塞进内核，内核只负责在正确时机调用它，并把「它是谁、它能做什么」收窄成最小接口。主核的访问者拿到的是元组与上下文，这里拿到的是 `ClosureContext` 与 `MessageContext`。

对 AI 工程有两条直接可用的结论。第一，这类「只收不发结论」的智能体是多智能体系统里最实用的观测点：把消息转发进外部队列，交给测试或 UI 消费，完全不触碰运行时状态。第二，`unknown_type_policy` 默认 `warn` 会**静默丢弃**类型不符的消息（只留一条警告），因此订阅配置错误在闭包智能体上表现为「没有任何反应」，而不是「报错」；排查时要把 `"error"` 临时打开来定位。第三，无状态意味着它不参与断点恢复，任何「重启后还要继续」的消费者不能建成闭包智能体。
