---
project: langgraph
feature: channel-base
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/channels/base.py
    symbols: [BaseChannel, ValueType, UpdateType]
---

# BaseChannel 契约与检查点协议

## 一句话职责

BaseChannel 定义了「一个状态槽位」的最小语义：怎么读当前值、怎么接受一轮更新、怎么序列化进检查点。

## 机制是什么

通道是 langgraph 里最小的状态单位。一个通道对应状态 schema 里的一个键，负责三件事：持有当前值、接收本轮写入、把自己变成可持久化的形态。`BaseChannel` 把这套约束固定成抽象基类，具体语义（覆盖、追加、聚合、屏障）由子类实现。

这里有一个必须分清的类型分歧：`ValueType` 是「读出来的东西的类型」，`UpdateType` 是「写进来的东西的类型」。绝大多数通道两者相同，但屏障与增量通道会让它们不同——理解这一点是读懂其余通道页的前提。

## 怎么实现的

**读写方法的边界。** `get()` 是唯一的读入口，空通道必须抛 `EmptyChannelError` 而不是返回 `None`，这样「没有值」与「值是 None」被彻底区分开。`is_available()` 默认实现就是调 `get()` 并捕获异常，但源码注释明确建议子类覆盖成更廉价的判定——因为 `get()` 可能涉及拷贝。

`update(values)` 接收的是一个**顺序不保证**的更新序列，返回「本通道是否真的变了」。这个布尔返回不是装饰：调度器靠它判断是否要推进通道版本。

**两个可选钩子。** `consume()` 在「订阅该通道的任务跑过」之后被调用，默认空实现；`finish()` 在整轮运行即将结束时调用。两者都返回布尔。它们的存在让通道能表达「读完就该失效」这类生命周期——`EphemeralValue` 靠 `consume` 清空自己，`LastValueAfterFinish` 靠 `finish` 才让自己可见。

**序列化协议。** `checkpoint()` 默认调 `get()`，若通道为空则返回内部缺失标记而不是抛异常；`from_checkpoint(value)` 负责反向重建，且必须做深拷贝（docstring 明确要求「复杂结构要复制」），否则同一份检查点被两个线程加载会共享可变对象。`copy()` 默认走「先 checkpoint 再 from_checkpoint」这条较慢但正确性有保证的路径，允许子类覆盖成更快实现。

**相等性决定合并策略。** 子类实现的 `__eq__` 不是给用户用的，而是给构建期用的：`_add_schema` 在同一个键出现两次时比较通道是否相等，不等就报「同一键有不同类型通道」。`LastValue.__eq__` 只判类型不判类型参数，正是为了让「state 用带类型的 LastValue、input 用裸类型」这种写法不被误判为冲突。

## 关键取舍

**源码事实：** 空通道必须抛 `EmptyChannelError`，不允许用 `None` 表示空。这使「键从未写过」与「键被写成 None」在语义上可分，也让下游节点能区分「等不到数据」与「拿到空值」。

**源码事实：** `copy()` 的默认实现依赖 `checkpoint()` / `from_checkpoint()`，是「正确性优先」的兜底；子类可以选择更快的浅拷贝实现，但必须自己保证不共享可变状态。

**源码事实：** `update` 收到的更新序列顺序不保证，因此任何依赖「先写的先合并」的实现都必须自己排序或改用可交换算子。这解释了为什么 `BinaryOperatorAggregate` 要求算子满足结合律。

**架构推论：** `consume` 与 `finish` 做成默认空实现的钩子、而不是必实现方法，使通道的「生命周期管理」成为可选能力。绝大多数通道不需要它，只有表达「临时」「结束时才可见」这类语义的通道才用得上。

**架构推论：** 把相等性放进通道契约（而非外部注册表），意味着「通道类型是否兼容」这件事由通道自己回答。好处是新增通道无需改中心代码；代价是每个子类都必须认真实现 `__eq__`，否则会出现难以察觉的通道复用错误。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/channels/base.py` | `BaseChannel` | 通道抽象基类，定义读写与序列化契约 |
| `libs/langgraph/langgraph/channels/base.py` | `ValueType` | 读出的值类型（抽象属性） |
| `libs/langgraph/langgraph/channels/base.py` | `UpdateType` | 写入的更新类型（抽象属性） |
| `libs/langgraph/langgraph/channels/base.py` | `update` | 接收一轮更新并返回是否变更 |
| `libs/langgraph/langgraph/channels/base.py` | `checkpoint` | 生成可持久化快照，空通道返回缺失标记 |
| `libs/langgraph/langgraph/channels/base.py` | `consume` | 任务消费后的生命周期钩子 |

## 参考

- [LangGraph 源码：channels/base.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/base.py)
- [LangGraph 文档：状态与 reducer](https://langchain-ai.github.io/langgraph/concepts/low_level/#reducers)
- [LangGraph 文档：检查点概念](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

通道的「值类型 / 更新类型分离」与主核里「存储格式 / 逻辑类型」的分离是同一类设计：底层的物理表示与上层的语义视图不必一致。主核用类型转换层把磁盘格式映成逻辑值，这里用两个类型参数把「写进来的东西」与「读出去的东西」分开。

对 AI 是一致的实践意义：agent 状态里最容易被写错的是「累积型字段被当成覆盖型」。把 `UpdateType` 摆在契约层，使这类错误在建图期就以通道类型不匹配的形式暴露，而不是等到运行几轮之后发现消息列表丢了历史。
