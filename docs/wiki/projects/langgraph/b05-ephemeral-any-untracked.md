---
project: langgraph
feature: ephemeral-any-untracked
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/ephemeral_value.py
    symbols: [EphemeralValue]
  - path: libs/langgraph/langgraph/channels/any_value.py
    symbols: [AnyValue]
  - path: libs/langgraph/langgraph/channels/untracked_value.py
    symbols: [UntrackedValue]
---

# 临时值、任意值与不追踪通道

## 一句话职责

这三种通道都用来存放「不该长期保留」的状态，区别只在「什么时候消失」和「要不要进检查点」。

## 机制是什么

它们共同构成一组「生命周期受限」的通道：数据只是一轮或几轮的中转，不承担状态累积。三者的差异可以放在两条轴上判断：

| 通道 | 空写时的行为 | 是否进检查点 |
|---|---|---|
| `EphemeralValue` | 收到空更新序列时主动清空自己 | 是（但通常已被清空） |
| `AnyValue` | 收到空更新序列时不清空 | 是 |
| `UntrackedValue` | 收到空更新序列时不清空 | 否，永远返回缺失标记 |

其中 `EphemeralValue` 与 `UntrackedValue` 还各有一个 `guard` 开关，控制「一轮多写」是报错还是任意取一个。

## 怎么实现的

**`EphemeralValue` 的自清空。** 它的 `update` 在收到空序列时做了一件别处没有的事：如果当前有值就置空并返回 `True`，表明「通道发生了变化」。这意味着「这一轮没人写」对它而言不是无事发生，而是一次清除动作。这正是「临时」的实现方式——不需要专门的生命周期钩子，靠每轮的空更新自然收敛。

`guard=True` 时一轮多写抛 `InvalidUpdateError`，错误文案直接给出解法（改用 `guard=False` 即可存其中任意一个）。`guard=False` 时取序列最后一个值。相等性同时比较类型与 `guard`，因此同一键上 `guard` 不同的两个通道会被判为冲突。

**`AnyValue` 的语义。** 它与 `EphemeralValue` 结构几乎一致，唯一差别是空更新时不清空。也就是说它保存的「上一轮写进来的值」会一直留着，直到下一次有人写。这使它适合承载「当前配置」这类跨轮有效但不累积的值。

**`UntrackedValue` 的不追踪。** 它的关键覆盖在 `checkpoint()`：永远返回缺失标记；`from_checkpoint` 则忽略传入值、直接构造空实例。于是无论检查点里有没有这个键，恢复后它都是空的。这从序列化协议层面实现了「不进检查点」，而不是靠调用方记得跳过它。

**空判定的统一。** 三者都用同一个缺失标记表示「没有值」，`get()` 抛 `EmptyChannelError`，`is_available()` 只判标记。这与其他通道保持一致，使下游节点不需要为不同通道写不同的空判定。

## 关键取舍

**源码事实：** `EphemeralValue` 把「空更新」当成一次清除并返回 `True`。这个返回值会影响通道版本推进，因此「没人写」在这一种通道上确实会带来状态变化。

**源码事实：** `UntrackedValue` 通过让 `checkpoint()` 恒定返回缺失标记来实现不追踪，而不是在检查点构建侧做过滤。好处是通道自身就完整表达了语义，任何使用方都不会漏判。

**源码事实：** `guard` 参与相等性判定。同一键上用不同 `guard` 会被判成异类通道，防止「建图时以为有并发保护、实际没有」。

**架构推论：** 三者都不做深拷贝，`copy()` 是直接赋引用。因为它们承载的是短生命周期值，跨线程共享的风险由「用完即清」的设计来回避，而不是靠复制。

**架构推论：** 「进不进检查点」被实现为通道的能力差异，而非外部策略。这意味着恢复语义由通道类型决定，用户换通道就等于换持久化行为，不需要额外配置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/ephemeral_value.py` | `EphemeralValue` | 临时值通道，空更新时自清空 |
| `libs/langgraph/langgraph/channels/ephemeral_value.py` | `EphemeralValue.update` | 空序列清空并返回 True，guard 控制多写 |
| `libs/langgraph/langgraph/channels/any_value.py` | `AnyValue` | 任意值通道，空更新不清空 |
| `libs/langgraph/langgraph/channels/untracked_value.py` | `UntrackedValue` | 不追踪通道，检查点恒为缺失 |
| `libs/langgraph/langgraph/channels/untracked_value.py` | `UntrackedValue.checkpoint` | 恒定返回缺失标记，实现不进检查点 |

## 参考

- [LangGraph 源码：channels/ephemeral_value.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/ephemeral_value.py)
- [LangGraph 源码：channels/untracked_value.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/untracked_value.py)
- [LangGraph 文档：检查点与状态持久化](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

「不进检查点」在主核里对应的是非持久化临时表与内存上下文：生命周期短、崩溃后不保证存在。差别在于主核用表属性声明，这里用通道类型声明。

对 agent 最实际的是 `UntrackedValue`：把大对象（例如一次抓取的网页全文）放进主状态会让每轮检查点都变大，恢复也变慢。这类数据应该走不追踪通道或用外部 store，只把引用留在状态里。这一页的机制就是那条边界的形式化表达。
