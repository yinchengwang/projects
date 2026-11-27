---
project: autogen
feature: subscription-manager
title: 订阅登记与接收者解析
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py
    symbols: [SubscriptionManager, add_subscription, get_subscribed_recipients]
---

# 订阅登记与接收者解析

## 一句话职责

维护一份订阅清单，并把「某个主题的收件人是谁」缓存成按主题索引的地址列表——新主题算一次，之后直接查。

## 机制是什么

`SubscriptionManager` 只有三个字段：

| 字段 | 类型 | 作用 |
|---|---|---|
| `_subscriptions` | `List[Subscription]` | 订阅清单，顺序即添加顺序 |
| `_seen_topics` | `Set[TopicId]` | 已经算过收件人的主题集合 |
| `_subscribed_recipients` | `DefaultDict[TopicId, List[AgentId]]` | 主题 → 收件人缓存 |

对外四个入口：`subscriptions`（只读清单）、`add_subscription`、`remove_subscription`、`get_subscribed_recipients`。前两个会变更清单并**重建全部缓存**，第三个只在主题首次出现时增量算一次。

## 怎么实现的

**增删都走全量重建。** `add_subscription` 先做去重检查（`any(sub == subscription for sub in self._subscriptions)`，相等性由订阅自己的 `__eq__` 决定，详见订阅匹配一页），命中就抛 `ValueError("Subscription already exists")`；`remove_subscription` 先确认 id 存在，不存在抛 `ValueError("Subscription does not exist")`。两个方法随后都调用 `_rebuild_subscriptions(self._seen_topics)`：清空缓存，对每个已见主题重新跑一遍 `_build_for_new_topic`。

`_rebuild_subscriptions` 上挂着源码注释 `# TODO: optimize this...`，说明作者清楚这是 O(已见主题 × 订阅数) 的全量重算。之所以能接受，是因为增删订阅发生在装配期，而主题集合在稳态下增长缓慢。

**读取走两级判定。** `get_subscribed_recipients(topic)` 先看 `topic in self._seen_topics`，不在就 `_build_for_new_topic(topic)` 补算，然后返回缓存里的列表。注意它返回的是**缓存里那个列表对象本身**，不是副本——调用方若原地修改会污染缓存。仓内的唯一调用方（`_process_publish`）只遍历读取，所以没出问题，但这是接口里的隐含约定。

**「已见主题」与「有订阅的主题」是两件事。** `_seen_topics` 记录所有被查询过的主题，哪怕一条订阅都没匹配。于是「查询一个没人订阅的主题」会留下一条空列表缓存，之后再查也不会重算。正因为如此，增删订阅时必须重建全部缓存——否则那些「当时空、现在该有」的主题永远不会更新。

**`add_subscription` 是 `async` 但内部全是同步操作。** 签名带 `async` 只是为了让实现方（`AgentRuntime.add_subscription`、grpc 版本的远端调用）保持同形接口，`SubscriptionManager` 本身没有任何 `await`。这是「协议统一优先于最小能力」的典型代价。

**去重逻辑依赖订阅实现。** `Subscription.__eq__` 默认比 id，而两个具体实现（`TypeSubscription`、`TypePrefixSubscription`）放宽成「id 相同或配置相同」。因此管理器的去重强度其实由订阅类型决定：传入自定义订阅若没实现 `__eq__`，同配置两次添加不会被拦住。

## 关键取舍

**源码事实：** 缓存键是 `TopicId`，值是「该主题的全部收件人」。由于 `TopicId` 是 `frozen` dataclass、可作字典键，这条缓存是安全且廉价可查的。

**源码事实：** 只缓存「主题 → 收件人」，不缓存「订阅 → 是否匹配某主题」。所以一次重建要为每个已见主题重新问遍所有订阅的 `is_match`，重复计算在订阅多、主题多时是平方级的。

**源码事实：** 重建时不清空 `_seen_topics`。这是正确性的关键：删掉一条订阅后，之前只是「碰巧没匹配」的主题仍要按新清单重算；若把已见集合也清掉，那些主题就得等到下一次广播才被发现是空的。

**源码事实：** 列表顺序等于订阅添加顺序（`_build_for_new_topic` 按 `_subscriptions` 顺序 append）。广播时 `_process_publish` 也按这个顺序建协程、再 `gather`。所以订阅注册顺序会影响并发发起顺序，虽然最终是并发等待。

**架构推论：** 把「谁订阅了什么」与「某主题该发给谁」分成两层，让广播时既不用遍历订阅，也不用每次重算。代价是订阅变更必须付一次全量重建——这在装配期可接受，在运行期频繁变更订阅的场景下就会退化。

**架构推论：** 管理器不持有运行时引用，也不触发实例化。它只输出地址，实例的创建留给运行时在投递时按需完成。这让「算收件人」和「拉起智能体」两件事解耦，前者是纯计算、后者有副作用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager` | 订阅清单 + 主题收件人缓存 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager.add_subscription` | 去重后追加并全量重建缓存 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager.remove_subscription` | 按 id 删除并全量重建缓存 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager.get_subscribed_recipients` | 首见主题增量计算，之后查缓存 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager._rebuild_subscriptions` | 清缓存后按已见主题全量重算 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `SubscriptionManager._build_for_new_topic` | 单主题：标记已见并逐个订阅匹配追加 |
| `python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py` | `get_impl` | 同文件的地址解析辅助：`lazy` 决定是否拉起实例 |

## 参考

- [AutoGen 源码：_runtime_impl_helpers.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_runtime_impl_helpers.py)
- [AutoGen 源码：_subscription.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_subscription.py)
- [AutoGen 文档：主题与订阅](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/core-concepts/topic-and-subscription.html)

## 与主核 / AI 的关系

这个缓存与主核里「谓词到索引选择的缓存」是同一种策略：把昂贵的匹配结果按「查询条件」缓存起来，条件集合增长慢、结果复用多。主核的缓存失效点在 DDL 与统计信息变更，这里的失效点是订阅增删——都是低频写、高频读。

对 AI 编排有两条硬结论。第一，动态增删订阅会触发全量重建，开销随「已见主题数 × 订阅数」增长；把订阅装配集中在启动期、避免在处理器里临时加订阅，是必要纪律。第二，缓存的是「当时算出来的地址」，而智能体是投递时才按需实例化的，因此「订阅存在」不等于「实例已存在」；判断某智能体是否活着要看实例表，不能看订阅。

还有一条容易忽略的细节：`get_subscribed_recipients` 返回内部列表本身。若上层需要过滤或追加收件人，必须自己复制，否则一次原地 `append` 就会永久污染该主题的缓存。
