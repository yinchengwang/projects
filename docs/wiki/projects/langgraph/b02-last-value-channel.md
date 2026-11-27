---
project: langgraph
feature: last-value-channel
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/last_value.py
    symbols: [LastValue, LastValueAfterFinish, update]
---

# LastValue 与结束可见语义

## 一句话职责

LastValue 是最常用的通道：它只保存最后一次写入的值，并且强制一轮里最多只能有一个写入者。

## 机制是什么

`LastValue` 是通道的默认形态——状态 schema 里任何一个没有额外注解、没有 reducer 的键，最终都会退化成 `LastValue`。它的语义只有两条：保存最后写入的值；一轮 superstep 内如果有超过一个任务写到同一个键，直接报错。

`LastValueAfterFinish` 是它的变体：写入照收，但**在整轮运行结束之前不可见**。这个差异只体现在可见性时机上，数据本身没有任何不同。

## 怎么实现的

**构造与相等。** 初始化只做两件事：记下类型与键名，把值置成内部缺失标记。`__eq__` 只判 `isinstance(value, LastValue)`，完全不比较类型参数——这不是偷懒，而是构建期合并 schema 时的必要放宽：state schema 用 `TypedDict` 声明（带类型）、input schema 可能用裸类型，两者推到同一个键上会得到两个类型参数不同的 `LastValue`，若比较类型参数就会误报「同键异类通道」。

**写入的严格性。** `update` 在收到空序列时返回 `False`（这一轮没人写，通道没变）；收到多于一个值时抛 `InvalidUpdateError`，错误文案直接指向解法：「用 Annotated 键处理多值」。这是整个通道体系里唯一一处「默认语义就拒绝并发写」的地方，它把「多分支同时写同一键」从静默覆盖变成显式错误。

**读与空判定。** `get()` 在值为缺失标记时抛 `EmptyChannelError`；`is_available()` 则只判断标记，不做任何拷贝——这是基类注释建议的「廉价覆盖」的实例。

**序列化。** `copy()` 走的是直接构造新实例再赋值的快路径，而非基类的「checkpoint 再 from_checkpoint」；`from_checkpoint` 在检查点值不是缺失标记时恢复，否则保持空。这意味着「曾经写过但写的是 None」在检查点里是可区分的，因为 None 不是缺失标记。

**结束才可见的变体。** `LastValueAfterFinish` 覆盖了 `update` 与两个生命周期钩子：写入后记一个「已更新」标记但值要等 `finish()` 才真正对外可用，`consume()` 里再按情况清空。它的用途在源码里能对上：`attach_node` 给 `defer=True` 的节点建下游触发通道时，就选这个变体而不是 `EphemeralValue`——因为被 defer 的节点要等到运行即将结束时才跑，它的触发通道不能让下游提前看到。

## 关键取舍

**源码事实：** `__eq__` 不比较类型参数。这是构建期 schema 合并能容忍「同键不同类型注解」的唯一原因，也是唯一一处刻意的宽松相等。

**源码事实：** 一轮多写直接报错，不回退到「覆盖最后一个也不报」。这让并发写在第一时间暴露，代价是用户必须显式用 reducer 表达「多值合并」。

**源码事实：** `LastValueAfterFinish` 与 `LastValue` 的差别在可见性时机，不在数据结构。因此它不能靠「延迟执行 update」实现，只能在读写两侧加标记。

**架构推论：** 作为默认通道，`LastValue` 的语义决定了整张图的默认行为——「每个状态键默认是覆盖写」。所有累积型需求都必须显式声明，这是有意的默认值选择：宁可让用户多写一个注解，也不让状态在没有提示的情况下无限增长。

**架构推论：** `is_available()` 覆盖成标记判断，使「某通道这轮是否有值」的检查不产生任何数据拷贝。在高频轮转的 agent 循环里，这个差异会累积成可观的节省。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValue` | 默认通道：保存最后值，拒绝一轮多写 |
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValue.update` | 空序列返回 False，多值抛 InvalidUpdateError |
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValue.checkpoint` | 返回当前值，空时返回缺失标记 |
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValue.copy` | 快路径复制，不走 checkpoint 往返 |
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValueAfterFinish` | 写入照收但结束前不可见的变体 |
| `libs/langgraph/langgraph/channels/last_value.py` | `LastValueAfterFinish.consume` | 消费后按标记决定是否清空 |

## 参考

- [LangGraph 源码：channels/last_value.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/last_value.py)
- [LangGraph 文档：状态与 reducer](https://langchain-ai.github.io/langgraph/concepts/low_level/#reducers)
- [LangGraph 文档：defer 节点](https://langchain-ai.github.io/langgraph/concepts/low_level/)

## 与主核 / AI 的关系

「默认覆盖写、多值需显式声明」与主核里「同一行被并发更新必须走行锁或版本校验」是同一个问题的两种解法：主核选择让并发可见并靠事务边界收敛，这里选择在合并期直接拒绝，把并发性问题的暴露点前移到建图期。

对 agent 的直接影响是消息历史：如果把消息列表声明成普通类型，第二轮就会因为多节点同时追加而报错。用法上必须给它一个 reducer 让它退化成 `BinaryOperatorAggregate`，这正是下一篇要讲的通道。
