---
project: langgraph
feature: node-builder
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/main.py
    symbols: [NodeBuilder, subscribe_to, write_to, build]
---

# NodeBuilder 的订阅与写入声明

## 一句话职责

`NodeBuilder` 是直接使用 Pregel 时对 `PregelNode` 的流式构造器：它把「监听哪些通道」「收到什么输入」「往哪些通道写」「挂哪些策略」分别攒在独立的字段里，直到 `build()` 才一次性拼成一个 `PregelNode`。

## 机制是什么

`PregelNode` 有十来个构造参数（`channels`、`triggers`、`mapper`、`writers`、`tags`、`metadata`、`bound`、`retry_policy`、`cache_policy`、`timeout`、`subgraphs`、错误处理器相关字段），手写很啰嗦。`NodeBuilder` 用 `__slots__` 固定九个字段，每个字段对应一小组构造参数，再用链式方法逐个填充：`subscribe_to`/`subscribe_only`/`read_from` 管读，`do` 管主体，`write_to` 管写，`meta`/`add_retry_policies`/`add_cache_policy`/`set_timeout` 管策略。所有方法都返回 `Self`，所以典型写法是一条链。

它不持有任何运行时信息。整个对象的生命周期止于 `build()`：`Pregel.__init__` 见到 `NodeBuilder` 就叫 `.build()`，之后图里只认 `PregelNode`。

## 怎么实现的

**读通道与触发通道是两份。** 这是本页最要紧的一点：`_channels` 是「喂给节点的输入」，`_triggers` 是「什么情况下叫醒节点」。`subscribe_to` 默认 `read=True`，于是同一个通道名既进 `_channels` 又进 `_triggers`；`read=False` 时只进 `_triggers`，节点会被它唤起但输入里没有它。`read_from` 只往 `_channels` 里加，完全不进 `_triggers`——用于「我要看到这个值，但它的变化不该触发我」。三者组合出的语义差异完全由这两个列表的成员关系表达，`PregelNode` 侧不需要任何额外标记。

**单通道模式的互斥。** `subscribe_only` 是特例：它把 `_channels` 直接设成字符串而不是列表，表示节点收到的是该通道的值本身（`PregelNode` 里 `channels` 为 `str` 时按值处理，为 `list` 时按字典处理）。两条断言把两种模式隔开：已经用了 `subscribe_to`（`_channels` 是 list 且非空）再调 `subscribe_only` 会 `ValueError`；已经 `subscribe_only` 过再调 `subscribe_to` 或 `read_from` 同样报错。`read_from` 用的是 `assert` 而不是抛异常，所以在 `-O` 下这层保护会消失。

注意 `subscribe_only` 的判据是 `if not self._channels`。空列表为假，所以「先 `subscribe_to()` 不传任何通道」之后仍能进入单通道模式，此时 `_channels` 被换成字符串，之前调 `subscribe_to` 记进 `_triggers` 的东西仍然留着。

**`do` 的串联。** `do` 不是覆盖而是追加：`_bound` 初始是 `DEFAULT_BOUND`（一个恒等 `RunnableCallable`），第一次 `do` 直接替换，之后每次都用 `RunnableSeq(旧, 新的)` 串起来。这让「一个节点里串多个步骤」不必手工包 `RunnableSequence`，但每次串联都新建一层 `RunnableSeq`，多次调用会形成左深链。

**`write_to` 的两种形态。** 位置参数 `*channels` 接受通道名或现成的 `ChannelWriteEntry`；关键字参数 `**kwargs` 是「通道名 → 值映射」，值可调用时被当成 mapper、否则当成固定值。两种都归一成 `ChannelWriteEntry` 追加进 `_writes`。真正落地写在 `build()` 里：`writers=[ChannelWrite(self._writes)]`——即使 `_writes` 为空也照样挂一个空的 `ChannelWrite`，因为 `PregelNode.flat_writers` 的合并优化要靠 `ChannelWrite` 实例来识别「这是写者」。

**`build()` 的一次性转译。** 它把九个字段一一对应成 `PregelNode` 的构造参数，并显式传 `mapper=None`、`subgraphs` 不传（让 `PregelNode` 自己去 `find_subgraph_pregel` 探测 `bound` 里有没有子图）。`timeout` 在 `set_timeout` 阶段就已经过 `coerce_timeout_policy` 归一，`build` 不再处理。

## 关键取舍

**源码事实：** 读通道与触发通道分离，且用「是否 `read`」而不是单独的 `trigger_only` 标志来表达。好处是 `subscribe_to` 一个方法覆盖两种常见意图；坏处是 `read_from`（只读不触发）与 `subscribe_to(read=False)`（只触发不读）都不在 `_channels` / `_triggers` 的对称位置上，读代码时容易误判某个通道到底会不会触发节点。

**源码事实：** 单通道模式与多通道模式只能二选一，且 `read_from` 的守卫是 `assert`。这意味着在优化模式下两者混用不会报错，而是静默产生一个 `channels` 为字符串、却塞了多个通道的非法状态，最终由 `_proc_input` 的 `RuntimeError` 兜底。

**源码事实：** `write_to` 即使没声明任何写，也会构造一个空的 `ChannelWrite`。这不是浪费：`PregelNode.node` 用 `bound is DEFAULT_BOUND and not writers` 判断「这个节点没有主体只有写者」，空 `ChannelWrite` 让它走进「单写者即 node」的分支，从而支持纯路由节点（只写不执行）。

**架构推论：** `NodeBuilder` 只做语法糖，不引入新的执行语义。凡是它表达不出来的东西（`mapper`、显式 `subgraphs`、`is_error_handler`、`error_handler_node`），都必须直接用 `PregelNode`。这条界线让「Pregel 的公开构造面」与「图 API 的编译产物」共用同一个终态类型。

**架构推论：** 因为 `build()` 是纯函数式转译、不改动 builder 自身，同一个 `NodeBuilder` 可以被 `build` 多次，得到互不共享 `_writes`/`_triggers` 副本的多个 `PregelNode`（`ChannelWrite` 持有的是新列表，`_triggers` 被 `PregelNode.__init__` 复制成新 list）。这使「用同一个模板造两个节点」成为可行写法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/main.py` | `NodeBuilder` | 九字段的链式节点构造器 |
| `libs/langgraph/langgraph/pregel/main.py` | `subscribe_to` | 同时登记读通道与触发通道，`read` 决定是否进读集合 |
| `libs/langgraph/langgraph/pregel/main.py` | `NodeBuilder.subscribe_only` | 单通道模式，`_channels` 退化为字符串 |
| `libs/langgraph/langgraph/pregel/main.py` | `NodeBuilder.read_from` | 只读不触发的通道追加 |
| `libs/langgraph/langgraph/pregel/main.py` | `write_to` | 归一位置参数与关键字映射为 `ChannelWriteEntry` |
| `libs/langgraph/langgraph/pregel/main.py` | `build` | 一次性转译成 `PregelNode`，含空写者的保留 |

## 参考

- [LangGraph 源码：pregel/main.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/main.py)
- [LangGraph 源码：pregel/_read.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_read.py)
- [LangGraph 文档：直接使用 Pregel](https://langchain-ai.github.io/langgraph/concepts/low_level/#pregel)

## 与主核 / AI 的关系

「读集合」与「触发集合」分离，与主核里「索引键」和「可见性判定」分离是同一类设计：主核的某个字段可以参与索引、决定行的可见性，也可以只作为投影列读出而不影响扫描范围。两者都在同一份数据上维护两个成员关系，用「是否属于某一侧」替代独立的布尔标记。

对 AI 侧而言，`read=False` 与 `read_from` 的区别直接决定 agent 的唤醒语义。用 `read=False` 订阅一个「消息到来」的通道，能实现「来了新消息就重跑，但不把消息本身塞进 prompt」；用 `read_from` 则相反，能在不因某字段变化而重跑的前提下把它放进上下文。选错会导致要么无谓重跑、要么状态永远看不到某个值。

