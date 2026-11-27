---
project: langgraph
feature: channel-write
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_write.py
    symbols: [ChannelWrite, do_write, _assemble_writes]
---

# ChannelWrite 写通道节点

## 一句话职责

`ChannelWrite` 把「主体输出该落到哪些通道、怎么切分」编译成一次对 `CONFIG_KEY_SEND` 的调用，是节点输出进入状态系统的唯一出口。

## 机制是什么

节点执行完之后，怎么把返回值拆成通道写入是一个独立的、可组合的步骤——因为同一个节点可能既要写状态键、又要触发分支通道、又要处理 `Command` 与 `Send`，这些规则并不属于业务函数。`ChannelWrite` 就是承载这些规则的 Runnable：它持有一串写入条目，被调用时先按条目把输入映射成 `(通道, 值)` 序列，再一次性交给注入的发送函数。

条目有三种形态：`ChannelWriteEntry`（目标通道 + 可选固定值 / `PASSTHROUGH` + `skip_none` + `mapper`）、`ChannelWriteTupleEntry`（一个 mapper 把输入映射成若干 `(通道, 值)` 元组，可附带供静态分析用的 `static` 声明）、以及 `Send`（动态扇出，写到保留通道 `TASKS`）。

## 怎么实现的

**`_write` 的条目固化。** 执行体不做别的，先把 `PASSTHROUGH` 的条目替换成「值等于当前输入」的新条目——`ChannelWriteEntry` 与 `ChannelWriteTupleEntry` 各有一条分支，其余条目原样带过。替换而不修改原条目，是为了同一 `ChannelWrite` 实例可复用于不同输入（例如并行任务共享同一写入器）。之后交给 `do_write`，最后返回输入本身，使「写入」对其所在序列是透明的。

**`do_write` 的两段。** 先校验：`ChannelWriteEntry` 的目标通道若等于 `TASKS` 直接抛 `InvalidUpdateError`（`TASKS` 承载 `Send` 包，用户手写进去会破坏调度）；再检查 `PASSTHROUGH` 在 `allow_passthrough=False` 时是否残留，残留也抛错。校验完从 config 取发送函数并调用 `write(_assemble_writes(writes))`。

`allow_passthrough` 这个开关的存在，说明有些调用点希望「自己已经算好了值，绝不允许再依赖输入」——这类调用传假即把误用变成显式错误。

**`_assemble_writes` 的展开。** 按条目类型分三路：`Send` 变成 `(TASKS, 该 Send 对象)`；`ChannelWriteTupleEntry` 调 `mapper(value)`，返回假值就整体跳过，否则把返回的元组全部展开；`ChannelWriteEntry` 先过 `mapper`（若有），然后两道跳过检查——值恒等于 `SKIP_WRITE` 哨兵时跳过（mapper 用它表达「这次不写」）、`skip_none` 为真且值为 `None` 时跳过。其余情况追加 `(通道, 值)`。末尾的 `else` 分支对未知条目抛 `ValueError`，属于防御性代码。

注意 `ChannelWriteTupleEntry` 的跳过条件是「mapper 返回假值」，因此 mapper 返回空列表、空元组、`None` 全都等价于「什么都不写」。这条约定让「条件写入」不需要额外的控制结构。

**名字。** `get_name` 拼出 `ChannelWrite<...>`，其中 `ChannelWriteEntry` 展示真实通道名、元组条目展示 `...`、`Send` 展示目标节点名。所以从一个 trace 名字就能看出这条写入是静态的还是动态扇出。

**写入器识别与静态声明。** `is_writer` 除了认 `ChannelWrite` 实例，还认任何被 `register_writer` 打过 `_is_channel_writer` 标记的 runnable——分支包装器就是靠这条被识别成写入器。`register_writer` 用 `object.__setattr__` 写入标记，绕过可能被覆写的 `__setattr__`（Pydantic 模型与 dataclass 都有这个问题）。`get_static_writes` 则从 `ChannelWriteTupleEntry.static` 或那条标记里取出「可能写到哪些通道」，供图渲染与校验使用，不影响执行。

## 关键取舍

**源码事实：** 写入不是「直接操作通道对象」，而是把 `(通道, 值)` 列表交给发送函数。发送函数由任务配置注入（`writes.extend`，即往任务的写入 deque 上追加），于是写入先在任务局部积累，等整个超级步结束才由 `apply_writes` 统一合并。这是「一步之内多任务并发写同一通道」能成立的前提。

**源码事实：** `SKIP_WRITE` 与 `PASSTHROUGH` 是两个不同的 `object()` 哨兵。前者表示 mapper 主动放弃本次写入，后者表示「用输入代替值」。用哨兵而不是 `None` 或布尔标志，避免与合法值域冲突。

**源码事实：** 连续 `ChannelWrite` 可在 `PregelNode.flat_writers` 里被合并成一条，因此「一个节点写几批」不会带来多次序列开销。合并只在尾部连续时成立，因为写入顺序在通道有 reducer 语义时是可见的。

**架构推论：** 校验发生在 `do_write` 而不是构造期。原因是 `TASKS` 与 `PASSTHROUGH` 都是运行期才确定的事实——`PASSTHROUGH` 要到调用时才被替换。代价是错误暴露得晚，收益是写入器可以按需构造、复用。

**架构推论：** 把 `Command`/`Send` 的解析放在上层 mapper（`state.py` 里的控制分支 mapper）而不是 `ChannelWrite` 内部，使 `_write.py` 完全不知道 `Command` 长什么样。这一层隔离让写入器只需理解「条目 → 元组」这一种转换。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_write.py` | `ChannelWrite` | 写入器 Runnable，持条目列表并透传输入 |
| `libs/langgraph/langgraph/pregel/_write.py` | `do_write` | 校验保留通道与 `PASSTHROUGH`，取发送函数并发出写入 |
| `libs/langgraph/langgraph/pregel/_write.py` | `_assemble_writes` | 条目展开成 `(通道, 值)` 列表，含跳过规则 |
| `libs/langgraph/langgraph/pregel/_write.py` | `ChannelWriteEntry` | 单通道写入条目，含 `skip_none` 与 mapper |
| `libs/langgraph/langgraph/pregel/_write.py` | `ChannelWriteTupleEntry` | mapper 驱动的多通道写入条目 |
| `libs/langgraph/langgraph/pregel/_write.py` | `register_writer` | 给非 `ChannelWrite` 的 runnable 打写入器标记 |

## 参考

- [LangGraph 源码：pregel/_write.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_write.py)
- [LangGraph 源码：graph/state.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/graph/state.py)
- [LangGraph 文档：Command 与动态路由](https://langchain-ai.github.io/langgraph/concepts/low_level/#command)

## 与主核 / AI 的关系

「先收集到本地缓冲、在边界处统一应用」与主核把元组修改记进本地缓冲区、提交时再统一落到页面的思路一致：并发单元之间不共享可写状态，冲突留到合并点按确定性顺序解决。这里的合并点是 `apply_writes`，主核是提交。

对 AI 侧而言，最有价值的推论是「写入是延迟生效的」。节点里就算调了写入器，通道值也还是旧的，同一步里其它并行任务看不到这些写入；要在节点内部读到自己刚写的值，只能靠 `fresh` 读取路径的临时视图。另外 `ChannelWriteTupleEntry` 返回空即「不写」这条规则，是给「按条件输出」用的标准手法，比在 mapper 里抛异常要干净。
