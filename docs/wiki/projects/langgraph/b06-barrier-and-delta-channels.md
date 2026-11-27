---
project: langgraph
feature: barrier-and-delta-channels
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/named_barrier_value.py
    symbols: [NamedBarrierValue, NamedBarrierValueAfterFinish]
  - path: libs/langgraph/langgraph/channels/delta.py
    symbols: [DeltaChannel, replay_writes]
---

# 命名屏障与增量通道

## 一句话职责

命名屏障表达「等齐若干上游才放行」，增量通道表达「检查点里只存哨兵、靠重放祖先写入重建状态」。

## 机制是什么

这两个通道解决的是两个不同层面的问题，放在一页是因为它们都偏离了「存值—取值」的朴素模型。

**命名屏障**关心的不是数据，而是**是否到齐**。它的值恒为 `None`，真正的状态是「已见集合」与「应有集合」是否相等。多起点边的会合语义就落在这里。

**增量通道**关心的是**检查点体积**。聚合型状态每轮都会增长，若每轮都存全量快照，一个长会话的检查点会迅速膨胀。增量通道改成只存一个哨兵，恢复时把祖先轮次的写入按 reducer 重新折叠出当前值。

## 怎么实现的

**屏障的到齐判定。** `NamedBarrierValue` 内有两个集合：`names` 是应到的名字集合（构建期固定），`seen` 是已到集合。`update` 逐个检查写入值：在 `names` 里且未见过就加入并标记变化；不在 `names` 里直接抛 `InvalidUpdateError`——这条严格性保证了「写错上游」不会被静默忽略。`is_available()` 就是 `seen == names`，`get()` 在不相等时抛 `EmptyChannelError`。

`consume()` 是它的关键：一旦到齐，就清空 `seen` 并返回 `True`。这意味着屏障是一次性的——下游消费一次之后，屏障重新变成「未到齐」，为下一轮会合做准备。检查点里存的就是 `seen` 集合，恢复时若已到齐也能立刻可用。

**结束才放行的变体。** `NamedBarrierValueAfterFinish` 多一个 `finished` 标记：到齐还不够，必须等整轮 `finish()` 之后才对外可见。它的检查点是 `(seen, finished)` 二元组而非单个集合，`consume()` 里同时清掉两个标记。这对应 `attach_edge` 给 `defer=True` 的节点建会合通道时的选择。

**增量通道的哨兵化。** `DeltaChannel` 的 `checkpoint()` 不返回状态本身，而是一个哨兵；恢复由 `from_checkpoint` 配合重放完成。文档约定的三种 blob 形态分别是：缺失或 `None` 表示从头开始由调用方重放、`_DeltaSnapshot` 表示直接取快照值、普通值表示从旧形态迁移而来。三种都保留兼容，是为了让「字段从聚合通道改成增量通道」的历史线程仍可读。

**批量折叠与覆盖基准点。** `replay_writes` 把一串祖先写入一次性交给 reducer，而不是逐条折叠。这要求 reducer 满足批量不变性：分两批折叠与合成一批折叠必须等价。源码在类文档里写出了这条等式，并明确它存在的理由——只有这样才能把「原始逐轮产生的写入」用更大的批次重放。

覆盖写入在重放里有特殊地位：序列里最后一个覆盖被视为「重置基准点」，它之前的值全部作废，基准变成覆盖值，只有它之后的写入才进 reducer。`update` 侧的规则一致但更严格：一轮里出现第二个覆盖直接报错。

**快照节奏。** 什么时候存全量快照由两个计数器决定：本通道的更新次数到达 `snapshot_frequency`，或者距上次快照的 superstep 数到达全局上限。两个条件是「或」的关系，因此即使一个通道不再收到写入，重放深度也不会无限增长。`snapshot_frequency` 在构造期就校验必须为正数。

## 关键取舍

**源码事实：** 屏障的值恒为 `None`，它的 `get()` 返回的是 `None` 而不是某个具体值。这说明屏障只表达「可以走了」，不携带数据。

**源码事实：** 屏障在到齐后靠 `consume()` 自行重置。会合不是一次性的终点，而是可重复的周期动作。

**源码事实：** 增量通道的 reducer 必须满足批量不变性，这条约束写在类文档里并被重放实现依赖。违反它的 reducer 会得到「重放结果与原运行不一致」的错误状态，而且很难察觉。

**源码事实：** 覆盖在重放中被当作重置基准点，在 `update` 中则限制一轮只能一次。两侧规则不一致是有意的：重放面对的是历史序列，允许中间有一次重置；实时更新面对的是同一轮的并发写，多次重置必然丢失一方。

**架构推论：** 增量通道用「重放祖先写入」换检查点体积，本质是把存储成本换成计算成本。快照频率就是这两者之间的旋钮：频率越低，检查点越小但恢复越慢。

**架构推论：** 屏障的「到齐」由集合相等判定，因此上游名字集合必须在建图期完全确定。这排除了「运行时才知道有几个上游」的用法——那种场景只能退化成计数或外部协调。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/named_barrier_value.py` | `NamedBarrierValue` | 等齐名字集合才放行，消费后重置 |
| `libs/langgraph/langgraph/channels/named_barrier_value.py` | `NamedBarrierValue.update` | 收集已见名字，未知名字报错 |
| `libs/langgraph/langgraph/channels/named_barrier_value.py` | `NamedBarrierValueAfterFinish` | 到齐且结束之后才可见的变体 |
| `libs/langgraph/langgraph/channels/delta.py` | `DeltaChannel` | 检查点只存哨兵、靠重放重建的通道 |
| `libs/langgraph/langgraph/channels/delta.py` | `replay_writes` | 批量折叠祖先写入，覆盖作为重置基准 |
| `libs/langgraph/langgraph/channels/delta.py` | `DeltaChannel.update` | 一轮至多一次覆盖的实时更新 |

## 参考

- [LangGraph 源码：channels/named_barrier_value.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/named_barrier_value.py)
- [LangGraph 源码：channels/delta.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/delta.py)
- [LangGraph 文档：多起点边](https://langchain-ai.github.io/langgraph/concepts/low_level/#edges)

## 与主核 / AI 的关系

增量通道的「哨兵 + 重放」与主核的 WAL 重做是同一种思想：不存结果，存能推出结果的输入。差别在于主核的重做由存储引擎在恢复期执行、有严格的页面版本校验，这里的重放则由 reducer 的代数性质保证正确性，没有额外校验。

对长会话 agent，这一页解释了一个现实的容量问题：消息历史若用普通聚合通道，每轮检查点都会写全量列表，长会话会变得很慢。改成增量通道后检查点只存哨兵，代价是恢复时要重放历史写入，因此 reducer 必须是可批量折叠的纯函数。选哪一种取决于会话长度与恢复时延哪个更敏感。
