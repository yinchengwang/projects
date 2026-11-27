---
project: autogen
feature: runtime-interface
title: 运行时抽象与智能体注册
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_agent_runtime.py
    symbols: [AgentRuntime, register_factory, register_agent_instance]
---

# 运行时抽象与智能体注册

## 一句话职责

用一个 `Protocol` 把「能收发消息、能注册智能体、能存取状态」固定成运行时契约，使单机实现与分布式实现可以互相替换，而上层智能体代码一行不改。

## 机制是什么

`AgentRuntime` 是 `@runtime_checkable` 的 `Protocol`，不是抽象基类。它声明的能力可以归成五组：

- 消息：`send_message`（点对点、有返回值）、`publish_message`（广播、无返回值）；
- 注册：`register_factory`（按类型登记工厂）、`register_agent_instance`（登记已存在的实例）；
- 解析：`get`（从类型/key 或 AgentId 得到 AgentId，`lazy` 控制是否顺手实例化）、`try_get_underlying_agent_instance`（取实体，分布式下不可用）；
- 状态：`save_state`/`load_state`（整运行时）、`agent_save_state`/`agent_load_state`（单个智能体）、`agent_metadata`；
- 装配：`add_subscription`/`remove_subscription`/`add_message_serializer`。

协议里**没有** `start`、`stop`、`close`。这不是遗漏——生命周期被有意留在实现侧，因为单机运行时是同步 `start()`，而 grpc worker 运行时的 `start()` 是 `async` 且需要先建立连接。

## 怎么实现的

**Protocol 而非 ABC。** 结构化类型判定意味着任何类只要有同名成员就算实现，仓内两个实现（`SingleThreadedAgentRuntime` 与 autogen-ext 里的 grpc worker 运行时）不需要共享基类，也就不会被迫共享一套生命周期 API。代价是「实现是否完整」主要靠类型检查器，运行期的 `isinstance` 只查属性是否存在。

**`register_factory` 的三件事实。** 一是 type 唯一：同 type 二次注册直接 `ValueError`。二是返回 `AgentType` 包装值而不是裸字符串，让调用方在类型层面区分「agent 类型」与普通字符串。三是 `expected_class` 的校验被搬进了 `factory_wrapper` 闭包，真正执行 `issubclass` 的时刻是**工厂被调用时**，不是注册时。因此「注册了一个返回错误类的工厂」不会当场报错，最早要等到第一条消息投递、实例被拉起时才炸。仓内那个期待 `ValueError` 的注册用例其实是被「type 已存在」拦住的，不是被类型校验拦住的。

**`register_agent_instance` 的相反取向。** 实例已经存在，绑定立刻发生（`bind_id_and_runtime`）。同一个 type 下先到者把类记进 `_agent_instance_types`，后续实例必须同类，否则 `ValueError`。它还给这个 type 塞了一个「故意抛错的桩工厂」，靠比较函数的 `__code__` 身份来区分「这个 type 是实例注册的」还是「工厂注册的」：两者混用直接拒绝。桩工厂的报错文案本身就是文档——它提示你多半是把一个未注册实例的 type 订阅到了某个主题。

**`sender` 的语义。** 协议文档写明：`sender` 只有在「消息来自运行时外部、而非某个智能体」时才可为 `None`。所以 `BaseAgent.send_message` 永远填 `sender=self.id`。这个字段不是装饰：它决定广播时是否跳过发送者自己，也决定接收方看到的 `MessageContext.sender`。

**`lazy` 是唯一的行为开关。** `get(..., lazy=True)` 只解析出地址，不拉起实例；`lazy=False` 才触发实例化。这是把「我要一个地址」与「我要把实例启动起来」分开的仅有 API。

**同步的那个方法。** `add_message_serializer` 是整份协议里唯一非 `async` 的成员——序列化注册不涉及 IO，没必要占一个事件循环 tick。

## 关键取舍

**源码事实：** 用 `Protocol` 换来了「形状共享、代码不共享」。分布式实现可以把注册转成 RPC、把订阅交给 host，而不必继承单机实现的任何内部状态。

**源码事实：** `expected_class` 延迟到实例化时校验。好处是工厂可以返回可接受类型的子类、并且异步工厂也能被同一段代码覆盖（`inspect.isawaitable` 判断）；坏处是配置错误推迟暴露。

**源码事实：** 注册与订阅彻底分离——`register_factory` 的文档明确写「本 API 不添加任何订阅」。把「注册 + 类订阅 + 直连前缀订阅 + 序列化器」打包起来的是 `BaseAgent.register`，属于上层便利。

**源码事实：** 协议没有 unregister，也没有实例淘汰。智能体一旦被实例化就留在 `_instantiated_agents` 里直到运行时销毁，对应文档里那句「paging in/out 尚未实现」。

**架构推论：** 把「注册工厂」与「订阅主题」做成两个正交动作，使同一份智能体代码可以按不同部署形态装配——单机时本地加订阅，分布式时把订阅集中到 host。

**架构推论：** `try_get_underlying_agent_instance` 的长名字与「generally discouraged」的措辞是刻意的防滥用设计。协议允许它存在（调试、测试、断言），但用命名把「跨边界取实体」标成坏味道；分布式实现里它只能抛 `NotAccessibleError`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime` | 运行时结构契约，含五组能力声明 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.register_factory` | 按类型登记工厂，类型守卫在实例化时才生效 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.register_agent_instance` | 登记已存在实例，立即完成 id 绑定 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.get` | 地址解析，`lazy` 控制是否拉起实例 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.try_get_underlying_agent_instance` | 取实体实例，分布式下抛不可访问 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.add_subscription` | 订阅入口，实现转交订阅管理器 |
| `python/packages/autogen-core/src/autogen_core/_agent_runtime.py` | `AgentRuntime.add_message_serializer` | 唯一同步成员，登记消息序列化器 |

## 参考

- [AutoGen 源码：_agent_runtime.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_agent_runtime.py)
- [AutoGen 文档：Agent 与 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/agent-and-agent-runtime.html)
- [AutoGen 文档：分布式 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/distributed-agent-runtime.html)

## 与主核 / AI 的关系

这份协议与主核里「存储引擎接口」是同一类设计：先定一组不带实现的函数签名，再让不同后端去满足它。区别在于主核的接口通常还承诺错误码与事务语义，这里只承诺形状——因为消息投递的语义已经由运行时实现自己定义了。

对 AI 工程的实际意义在于三条判据。第一，判断一个框架是否真的支持多后端，看它的抽象层里有没有混入生命周期方法；混了，就说明抽象是为单机提取的。第二，`sender=None` 与 `sender` 有值必须是两种语境，否则「谁发起」这条溯源链在广播场景里会失真，而这正是多智能体调试最常丢的信息。第三，注册期的类型守卫若被推迟到实例化，配置错误会表现为「第一条消息失败」而不是「启动失败」，排障时要先翻注册代码而不是业务代码。
