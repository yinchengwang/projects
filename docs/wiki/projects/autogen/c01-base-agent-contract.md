---
project: autogen
feature: base-agent-contract
title: BaseAgent 契约与消息入口
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_base_agent.py
    symbols: [BaseAgent, bind_id_and_runtime, on_message]
  - path: python/packages/autogen-core/src/autogen_core/_agent.py
    symbols: [Agent, on_message, save_state]
---

# BaseAgent 契约与消息入口

## 一句话职责

`Agent` 协议规定「一个智能体对外长什么样」，`BaseAgent` 把它落到实现上：绑定身份与运行时、把 `on_message` 变成不可覆盖的转发层、把注册与订阅打包成一对类方法。

## 机制是什么

两层契约，粒度不同：

| 层 | 形态 | 要求 |
|---|---|---|
| `Agent` | `@runtime_checkable` 的 `Protocol` | `metadata`、`id`、`bind_id_and_runtime`、`on_message`、`save_state`、`load_state`、`close` |
| `BaseAgent` | `ABC` + `Agent` | 额外要求 `on_message_impl`（抽象方法），并给出注册、订阅、发消息的实现 |

`BaseAgent` 的关键构造是把**入口与实现分开**：`on_message` 标了 `@final`，实现只有一行 `return await self.on_message_impl(message, ctx)`；子类要扩展的是 `on_message_impl`。子类再往下（`RoutedAgent`、`ClosureAgent`）都覆写的是后者。

`BaseAgent` 还声明了两个 `ClassVar` 容器：`internal_unbound_subscriptions_list`（订阅工厂）与 `internal_extra_handles_types`（消息类型 + 序列化器）。`__init_subclass__` 给每个子类重新赋一份空容器，避免 class-var 在父子类间共享。

## 怎么实现的

**身份两段式绑定。** 构造时若处在工厂调用上下文（`AgentInstantiationContext.is_in_factory_call()`），`__init__` 立刻把 `_runtime` 与 `_id` 填好；否则两个属性都不存在。之后 `bind_id_and_runtime` 才补上，并且是**幂等但不可改写**的：若已存在且与新值不同，分别抛「已绑定到不同 ID」「已绑定到不同运行时」。这让「工厂里写 `_ = self.id`」这种用法可行（值已就位），也让「同一实例被塞进两个运行时」在运行期被拦住。

**`metadata` 依赖 `_id`。** 属性里有 `assert self._id is not None`，取 `key`/`type`/`description` 三项。这意味着在绑定完成前读 `metadata` 会 `AssertionError`，而不是返回半成品——用断言而不是默认值，是在明示「此刻还没就绪」。

**`description` 必须是字符串。** `__init__` 里 `if not isinstance(description, str): raise ValueError`。这个看似琐碎的检查其实是对的：`BaseAgent.__init__(description)` 的实参在 `RoutedAgent` 里是位置参数，很容易被写成别的类型而不自知。

**`save_state`/`load_state` 是警告占位。** 两者都 `warnings.warn("save_state not implemented")` 然后返回 `{}` / 无操作。所以默认实现的智能体在整运行时 `save_state` 里贡献一个空字典，恢复时什么也不做——基类不强迫你实现状态，但会用警告提醒你正在失去状态。

**`send_message` 强制填 `sender`。** `BaseAgent.send_message` 调的是 `runtime.send_message(..., sender=self.id, ...)`。对比 `AgentRuntime.send_message` 允许 `sender=None`（表示「来自运行时外部」），基类这里填死 `self.id`，把「智能体发的消息」与「外部注入的消息」在协议层区分开。

**注册是三层打包。** `register(cls, runtime, type, factory, skip_class_subscriptions, skip_direct_message_subscription)` 依次做：

1. `runtime.register_factory(type=agent_type, agent_factory=factory, expected_class=cls)`——顺带把类型守卫也传进去；
2. 除非跳过，在 `SubscriptionInstantiationContext.populate_context(agent_type)` 里求值每个 `_unbound_subscriptions()` 工厂，兼容同步与异步，逐条 `add_subscription`；
3. 除非跳过，加一条前缀为 `type + ":"` 的 `TypePrefixSubscription`（注释写明前缀必须含 `:`）；
4. 对 `_handles_types()` 里的每个 `(类型, 序列化器列表)` 调 `runtime.add_message_serializer`。

`register_instance` 是它的实例版，第 1 步换成 `runtime.register_agent_instance`，并额外加一条「topic type 等于 agent key」的 `TypeSubscription`。两处都有 `# TODO: deduplication` 注释，说明序列化器重复登记是已知未处理项（运行时侧注册表按 `(type_name, content_type)` 覆盖写，所以重复无害）。

**装饰器的实现极简。** `subscription_factory(lambda)` 只把 lambda 追加进 `internal_unbound_subscriptions_list`；`handles(type, serializer)` 则立即解析序列化器（没给就 `try_get_known_serializers_for_type`），空列表直接 `ValueError`，然后把 `(type, serializers)` 追加进 `internal_extra_handles_types`。注意两个装饰器对时序的处理不同：订阅**延迟**到注册时求值，类型+序列化器**立即**求值。

## 关键取舍

**源码事实：** `on_message` 是 `@final`。这条约束把「运行时只调一个方法」写死，子类不可能通过覆盖入口改变投递语义；要改变行为只能改 `on_message_impl`。这让运行时的调用面稳定在一个点上。

**源码事实：** `bind_id_and_runtime` 的幂等是「同值可以重复调、异值直接报错」，而不是「后者覆盖前者」。这个取向使工厂路径（构造时已绑定）与实例注册路径（注册时绑定）能复用同一个方法，同时把「一个实例两个身份」从静默错误变成显式失败。

**源码事实：** 默认识别 `save_state` 无实现时只警告、不报错。取舍是宽容：不写状态的智能体（多数闭包式、只读消费者）不必实现任何东西；代价是「忘了实现」与「确实无状态」在日志上无法区分。

**源码事实：** 注册时自动挂的直连订阅用「类型 + 冒号」当主题类型前缀，并注释强调这个冒号是为了避免与其他智能体冲突。这意味着 `TypePrefixSubscription` 是一条框架自用的约定，用户不该手写同形前缀。

**架构推论：** 「构造期绑定 or 注册期绑定」两条路径合流到同一对属性，使智能体既可以由工厂创建（`register`），也可以是外部已构造的实例（`register_instance`）。两种形态在运行时的实例表里没有区别，差别只在谁负责构造。

**架构推论：** 把订阅与序列化器都挂在**类**上而不是实例上，使同一类型的多个实例共享一套订阅规则。代价是「按实例定制订阅」不被支持——要那样做只能绕开 `register`，直接调 `runtime.add_subscription`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_agent.py` | `Agent` | 智能体结构契约，七个成员 |
| `python/packages/autogen-core/src/autogen_core/_agent.py` | `Agent.on_message` | 唯一消息入口，文档写明只应由运行时调用 |
| `python/packages/autogen-core/src/autogen_core/_agent.py` | `Agent.save_state` | 状态导出契约，要求结果可 JSON 序列化 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent` | 契约落地的抽象基类 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.__init_subclass__` | 为每个子类重建类级容器，避免父子共享 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.__init__` | 上下文可用时即时绑定身份，并校验 description |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.bind_id_and_runtime` | 幂等绑定，异值报错 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.on_message` | `@final` 转发层，只调 `on_message_impl` |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.send_message` | 强制携带 `sender=self.id` |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.register` | 注册 + 类订阅 + 直连订阅 + 序列化器 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `BaseAgent.register_instance` | 已构造实例的注册版本 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `subscription_factory` | 订阅装饰器的共用实现，延迟求值 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `handles` | 声明消息类型与序列化器，立即求值 |

## 参考

- [AutoGen 源码：_base_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_base_agent.py)
- [AutoGen 源码：_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_agent.py)
- [AutoGen 文档：Agent 与 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/agent-and-agent-runtime.html)

## 与主核 / AI 的关系

`on_message` 的 `@final` 转发与主核里「统一入口函数 + 内部分派」是同一手法：对外收成一个入口，把可变的实现部分放在入口之后。主核的入口往往要顺手做参数校验与错误码映射，这里把校验权下放给各实现（`RoutedAgent` 查表、`ClosureAgent` 查类型）。

对 AI 工程有三条判断依据。第一，默认 `save_state` 只警告不报错，这意味着「断点恢复能恢复什么」完全取决于每个智能体自己实现到什么程度；不做审计就假定状态已存，是最常见的运行期事故来源。第二，身份的两种绑定路径决定了测试写法：想手工造实例就必须先 `populate_context`，否则 `__init__` 里读不到 runtime。第三，`register` 同时管订阅与序列化器，所以「注册了智能体但它收不到消息」通常不是注册失败，而是 `skip_class_subscriptions=True` 或订阅上下文缺失。
