---
project: autogen
feature: routed-agent
title: 按消息类型分发的 RoutedAgent
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_routed_agent.py
    symbols: [RoutedAgent, message_handler, event]
---

# 按消息类型分发的 RoutedAgent

## 一句话职责

用装饰器把「这个方法处理哪种消息、能返回什么、还要满足什么条件」编码成方法上的元数据，再在构造期扫成一张「消息类型 → 处理器列表」的表，使 `on_message` 退化成一次查表加一次调用。

## 机制是什么

三个装饰器共享同一套机制，差别只在路由谓词：

| 装饰器 | 处理的消息 | `router` 的判定 | 返回值检查 |
|---|---|---|---|
| `message_handler` | 事件与 RPC 都行 | `match(...)` 或恒 `True` | 必须在返回类型集合内 |
| `event` | 仅广播（`is_rpc=False`） | `not ctx.is_rpc and match(...)` | 必须为 `None` |
| `rpc` | 仅点对点（`is_rpc=True`） | `ctx.is_rpc and match(...)` | 必须在返回类型集合内 |

每个被装饰的方法都会拿到四个属性：`target_types`（从 `message` 参数的类型注解解析出的类型元组）、`produces_types`（从返回注解解析）、`is_message_handler`（标记位，供扫描时识别）、`router`（接收消息与上下文、返回布尔的谓词）。

`RoutedAgent.__init__` 把扫描结果摊平成 `DefaultDict[type, List[MessageHandler]]`。分发时按 `type(message)` 精确取列表，逐项问 `router`，**第一个返回真**的处理器被调用并立即返回其结果。全部不命中则落到 `on_unhandled_message`，默认实现只记一条 info 日志。

## 怎么实现的

**类型解析靠 `get_types`。** `get_type_hints(func)` 拿到注解后交给 `_type_helpers.get_types`，它处理四种形态：`Union` 拆成参数元组、`Optional` 补 `NoneType`、裸 `Any` 换成内部哨兵 `AnyType`、普通类型包成单元素元组。这解释了为什么处理器可以写联合类型来一次接多种消息，也解释了 `Any` 为什么要换成哨兵——`Any` 无法参与 `type(x) in ...` 的真值比较，只能用一个可识别的替身。

**返回值的校验是运行期检查，不是静态检查。** `wrapper` 里真做两件事：先 `type(message) not in target_types` 就按 `strict` 抛错或告警；再 `type(return_value) not in return_types` 做同样的判断。`AnyType` 会让返回检查直接放过。也就是说注解在运行期被当成了一份可执行的契约——`strict=True`（默认）时，注解写错会在第一次投递就炸，而不是静默。

**`event` 的返回值被强行归零。** 它不只检查「返回是不是 `None`」，`strict=False` 时即使返回了非 `None` 也照样 `return None`。这是语义上的强制：广播没有等待方，返回一个值出去无人接收，与其让它被静默丢弃，不如在严格模式下直接报错。

**`is_rpc` 参与路由，而不是参与类型判断。** `event` 与 `rpc` 把「是广播还是请求」塞进 `router` 谓词里，因此同一个消息类型可以同时有一个事件处理器和一个 RPC 处理器而互不干扰。仓内测试正对应这条：同一份 `MessageType` 广播时只触发 broadcast 计数、点对点发送时只触发 rpc 计数。

**处理器发现走 `dir(cls)`。** `_discover_handlers` 遍历类属性名、取可调用对象、看有没有 `is_message_handler` 标记。用 `dir` 而非 `vars` 意味着继承链上的处理器也会被发现；用类属性而非实例属性意味着处理器必须定义在类上。`@wraps(func)` 保证装饰后方法名仍是原名，这既是 `dir` 能找到它的前提，也是同一类里多个同名处理器会被后者覆盖的原因。

**`_handles_types` 顺手做了序列化器推导。** 它遍历每个处理器的 `target_types`，用 `try_get_known_serializers_for_type` 找出该类型可用的序列化器，找不到就抛 `ValueError`，最后返回 `(类型, 序列化器列表)` 列表供 `BaseAgent.register` 登记到运行时。这让「能处理某类型」与「该类型可被序列化」在注册期就被绑定。

## 关键取舍

**源码事实：** 分发只认精确类型。`_handlers` 用 `type(message)` 作键，源码注释也明写「works on concrete types and not inheritance」。因此处理器声明接收基类、实际投递子类时**不会命中**。这是刻意的：要接多个相关类型就写 `Union`，要按条件细分就用 `match`，而不是靠 MRO 推断。

**源码事实：** 同一类型的多个处理器按 `dir(cls)` 的字母序依次询问 `router`，第一个为真者胜出，其余被跳过——文档里明确写了「按处理器名字的字母序」。所以「两个处理器都匹配」不是错误，而是静默地只跑一个。这条规则使行为可预测，但也意味着把处理器改名可能改变实际执行的逻辑。

**源码事实：** `strict` 是双重开关：既控制类型不匹配时抛错还是告警，也控制返回值不匹配时抛错还是告警。没有「消息类型严格、返回类型宽松」这种中间档。

**源码事实：** `Any` 被换成 `AnyType` 哨兵，只在返回值检查处被特殊放过；消息类型侧若写 `Any`，`type(message) in (AnyType,)` 永远为假，于是会走进「不匹配」分支。所以「接收任意消息」在 `RoutedAgent` 里只有 `on_unhandled_message` 一条路，不是注解 `Any`。

**架构推论：** 把路由表在构造期一次建好、把谓词检查放在运行期，使分发热路径只做一次字典查找加若干次谓词调用，不做类型反射。代价是「处理器集合在实例生命周期内不可改」，动态增删处理器必须重建实例。

**架构推论：** 三个装饰器共用一套 `wrapper` 与四个属性，让 `event` 与 `rpc` 只是「预置了 `is_rpc` 谓词的 `message_handler`」。这个统一使运行时只需认一个接口，而语义差异全部收敛在谓词里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `RoutedAgent` | 查表分发的基类，构造期建路由表 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `RoutedAgent.on_message_impl` | 精确类型查表 + 首个命中即返回 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `RoutedAgent._discover_handlers` | 用 `dir` 扫类属性，收集带标记的处理器 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `RoutedAgent.on_unhandled_message` | 无命中时的兜底，默认仅记日志 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `message_handler` | 通用装饰器，运行期校验入参与返回类型 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `event` | 仅广播，返回值强制为 `None` |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `rpc` | 仅点对点，返回值参与类型校验 |
| `python/packages/autogen-core/src/autogen_core/_routed_agent.py` | `MessageHandler` | 处理器协议：类型元组 + 谓词 + 调用签名 |
| `python/packages/autogen-core/src/autogen_core/_type_helpers.py` | `get_types` | 把注解翻译成类型元组，`Any` 换成哨兵 |

## 参考

- [AutoGen 源码：_routed_agent.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_routed_agent.py)
- [AutoGen 文档：消息与通信](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/message-and-communication.html)
- [AutoGen 源码：_type_helpers.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_type_helpers.py)

## 与主核 / AI 的关系

这套「声明式注册 + 运行期精确匹配」与主核里「按操作码查跳转表」是同一形态：注册期把名字映射成入口，执行期只做一次索引加一次调用。主核的 op 表用整数下标，这里的键是 Python 类型对象；主核的分支在编译期定死，这里的分支可以在构造期按注解重建。

对 AI 工程有三条实际判据。第一，路由不认继承，这是最容易踩的坑：写一个接收基类消息的处理器，投递子类消息时会静默走 `on_unhandled_message`，而默认兜底只记 info 日志、不抛异常。第二，相同类型的多个处理器靠字母序决定谁赢，一个方法改名就可能改掉实际执行路径，因此细分条件必须写进 `match` 而不是拆成多个同名类型处理器。第三，「能处理某类型」与「该类型可序列化」在注册期绑定，加一种新消息类型时若忘了让它是 dataclass/pydantic 模型，报错会出现在注册阶段而不是处理阶段。
