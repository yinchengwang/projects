---
project: autogen
feature: subscription-matching
title: 订阅模型与主题匹配
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_subscription.py
    symbols: [Subscription, is_match, map_to_agent]
  - path: python/packages/autogen-core/src/autogen_core/_default_subscription.py
    symbols: [DefaultSubscription, type_subscription]
---

# 订阅模型与主题匹配

## 一句话职责

把「某个主题该由谁处理」拆成两个可组合的问题——`is_match` 判断是否命中、`map_to_agent` 推出具体地址——于是订阅可以用一条规则覆盖无限多个主题，而不必逐条登记收件人。

## 机制是什么

`Subscription` 是一个 `Protocol`，只要求三样东西：

| 成员 | 语义 |
|---|---|
| `id` | 唯一标识，用于删除与去重 |
| `is_match(topic_id)` | 这个主题归我管吗 |
| `map_to_agent(topic_id)` | 归我管的话，交给哪个 `AgentId` |

协议自带两段实现：`__eq__` 默认比较 `id`，`is_match` 与 `map_to_agent` 只有签名。仓内有两个实现，差别在匹配方式：

- `TypeSubscription(topic_type, agent_type)`：`is_match` 是 `topic_id.type == topic_type` 的精确相等；`map_to_agent` 返回 `AgentId(agent_type, key=topic_id.source)`。
- `TypePrefixSubscription(topic_type_prefix, agent_type)`：`is_match` 换成 `topic_id.type.startswith(prefix)`；`map_to_agent` 同样用 source 当 key。

两条规则共用同一个映射：**主题的 source 变成智能体的 key**。这一条决定了多租户行为——每来一个新的 source，就自动对应一个同类型不同 key 的新实例，不需要任何注册动作。

## 怎么实现的

**映射带守卫。** 两个 `map_to_agent` 实现都先复查 `is_match`，不匹配就抛 `CantHandleException`，而不是返回一个错地址。这是把「调用者必须先 `is_match`」这条前置条件变成运行期断言：写错顺序会立刻失败，不会静默投递到不存在的智能体。

**`DefaultSubscription` 是类型订阅的特例。** 它继承 `TypeSubscription`，默认 `topic_type="default"`，而 `agent_type` 在缺省时从 `SubscriptionInstantiationContext.agent_type()` 取。取不到就抛 `CantHandleException`，报错文案直接指向原因：`DefaultSubscription` 必须在注册的订阅回调里创建。这条约束的来源是——订阅回调被运行时代 `SubscriptionInstantiationContext.populate_context(agent_type)` 包裹，而 agent type 只有在那个上下文里才存在。

**两个装饰器的差别只是 topic type。** `default_subscription` 与 `type_subscription(topic_type)` 都返回 `subscription_factory(lambda: [...])` 的结果，后者把 `DefaultSubscription(topic_type=...)` 交给同一个工厂。`subscription_factory` 做的事极简：把 lambda 追加进类的 `internal_unbound_subscriptions_list`。注意 lambda 而非实例——订阅在注册时才被实例化，这样 `agent_type` 可以在正确的上下文里被解析出来。

**去重靠 `__eq__`，不是靠 `id`。** 两个实现的 `__eq__` 都比默认实现宽：`id` 相同**或**配置相同即视为相等。因此两次构造 `TypeSubscription("default","MyAgent")` 虽然拿到不同 UUID，仍被判重。仓内测试正是这么断言的：第二次添加抛 `ValueError("Subscription already exists")`，`DefaultSubscription(agent_type="MyAgent")` 也与上面那个 `TypeSubscription("default","MyAgent")` 撞车。

**广播的默认 source 由上下文决定。** `DefaultTopicId()` 在消息处理上下文里会把 source 设成当前智能体的 key，否则设成 `"default"`。配合「source → agent key」的映射，这意味着「在某处理器里发布到默认主题」会把消息送给同 key 的那批智能体，而不是全部。

**前缀订阅只被框架内部使用。** `TypePrefixSubscription` 在仓内唯一的构造点是 `BaseAgent.register` 与 `register_instance`，前缀固定为 `agent_type + ":"`，注释写明「前缀必须包含 `:` 以避免与其他智能体冲突」。它是「直连消息」这条能力的实现方式：把一个带冒号的字符串当主题类型，就能定向到一个具体 agent type。注册时若该订阅已存在会吞掉 `ValueError`。至于「谁按这个约定去拼主题类型」，仓内没有对应的构造代码，说明这是一条留给上层（或跨语言实现）遵循的命名约定，而不是框架自己生成的地址。

## 关键取舍

**源码事实：** 订阅是「规则 + 推导」，不是「主题 → 地址」的显式表。因此新主题第一次出现时无需登记，`map_to_agent` 现推地址，运行时再按需实例化那个 key 的智能体。

**源码事实：** `map_to_agent` 用 topic source 当 agent key，使「多租户」与「多会话」成为零成本特性——但也意味着主题 source 的取值范围直接决定了会产生多少个智能体实例。一个把用户输入当 source 的写法会导致实例数随之膨胀。

**源码事实：** `__eq__` 比 `id` 宽。好处是同配置订阅天然幂等，重复注册被拦；代价是「两个 id 不同但配置相同的订阅」在删除时必须用 id，用对象比较会删错——`remove_subscription` 收的是字符串 id，正是为了避开这一点。

**源码事实：** 协议用 `runtime_checkable` 的 `Protocol` 而非 ABC，且 `Subscription` 同时被当作基类使用（`class TypeSubscription(Subscription)`）。所以它既是结构契约，也被当成可继承的父类；协议里没有抽象方法声明，子类漏实现不会在类定义时报错，只会在调用时以属性缺失暴露。

**架构推论：** 把 `is_match` 与 `map_to_agent` 分开而不是合成一个「返回收件人列表」的方法，使匹配与映射可以分别优化：`SubscriptionManager` 只在主题第一次出现时对全部订阅跑一遍 `is_match`，之后直接查缓存。

**架构推论：** 订阅只输出「一个主题 → 一个地址」，广播的多个收件人是「多条订阅各自输出一个」的并集。这个「一条订阅对应一个收件人」的粒度，让订阅的增删语义非常清晰：删掉一条订阅就少一个收件人，不会牵连别的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `Subscription` | 订阅协议：`id` / `is_match` / `map_to_agent` |
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `Subscription.__eq__` | 默认按 `id` 比较，实现可放宽 |
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `Subscription.is_match` | 主题是否命中该订阅 |
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `Subscription.map_to_agent` | 主题映射为具体地址，不命中抛 `CantHandleException` |
| `python/packages/autogen-core/src/autogen_core/_subscription.py` | `UnboundSubscription` | 注册期才求值的订阅工厂别名 |
| `python/packages/autogen-core/src/autogen_core/_type_subscription.py` | `TypeSubscription` | 按 topic type 精确匹配，source 作 agent key |
| `python/packages/autogen-core/src/autogen_core/_type_prefix_subscription.py` | `TypePrefixSubscription` | 按 topic type 前缀匹配，直连消息用它实现 |
| `python/packages/autogen-core/src/autogen_core/_default_subscription.py` | `DefaultSubscription` | 类型订阅特例，agent type 从实例化上下文推断 |
| `python/packages/autogen-core/src/autogen_core/_default_subscription.py` | `type_subscription` | 装饰器：登记一条带指定 topic type 的订阅 |
| `python/packages/autogen-core/src/autogen_core/_base_agent.py` | `subscription_factory` | 把订阅工厂挂到类上，注册期才求值 |

## 参考

- [AutoGen 源码：_subscription.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_subscription.py)
- [AutoGen 源码：_default_subscription.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_default_subscription.py)
- [AutoGen 文档：主题与订阅](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/topic-and-subscription.html)
- [AutoGen 文档：主题与订阅场景](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/cookbook/topic-subscription-scenarios.html)

## 与主核 / AI 的关系

订阅的 `is_match` / `map_to_agent` 两段式，与主核里「谓词下推 + 代价估算」的分工类似：先用最廉价的可判定条件筛出候选，再对候选做具体定位。主核在索引层做同样的取舍——范围谓词用来筛块，块内再逐行判定。

对 AI 编排有三条直接结论。第一，「source → agent key」这条隐式映射是多租户友好的关键，也是实例数失控的根源；把用户输入直接当 source，就相当于给每个输入开一个新智能体。第二，广播的默认 source 取自当前处理器所在智能体，所以「处理器里发布到默认主题」不是全局广播，而是同 key 广播——这是最常见的误判点。第三，订阅去重按配置而非 id，重复注册被拦是有意的保护；但在需要「同一配置挂两条」的场景里，框架没有提供开关，只能靠不同 topic type 绕开。
