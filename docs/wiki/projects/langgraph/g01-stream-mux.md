---
project: langgraph
feature: stream-mux
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/stream/_mux.py
    symbols: [StreamMux, push, close]
---

# StreamMux 多路复用与推流

## 一句话职责

StreamMux 把「一条图事件流」复制成「主事件日志 + 若干具名投影响道」：每来一个事件，先按注册顺序跑完转换器管线，再决定它是否进入主日志。

## 机制是什么

StreamMux 是流式基础设施的中心分发器。它自己持有一条 `StreamChannel[ProtocolEvent]` 作为**主事件日志**（`_events`），同时持有一组 `StreamTransformer`。每个转换器在 `init()` 里返回一个「投影字典」，字典里的 `StreamChannel` 实例被 mux 接管，成为**投影响道**——消费者迭代 `run.values`、`run.messages` 时，实际迭代的就是这些 channel。

一个事件的生命周期只有两条出路：被某个转换器的 `process()` 返回 `False` 抑制掉，或带着一个单调递增的 `seq` 进入主日志。没有第三条。

mux 还负责 scope：根 mux 的 `scope` 是空元组，子 mux 通过 `_make_child` 在更深的命名空间上克隆一整套全新转换器实例，供子图句柄使用。

## 怎么实现的

**注册。** `_register` 一次做五件事：检查该转换器是否要求 async 车道（`transformer_requires_async`），若 mux 是同步模式就直接抛 `RuntimeError`；调用 `init()` 并要求返回 dict，否则 `TypeError`；比对投影键与已有 `extensions` 的重合，重合则报错并指名「哪个键被哪个转换器占了」；把投影里的 `StreamChannel` 逐个 `_bind(is_async)` 并 `_bind_mux(self)`；最后调 `transformer._on_register(self)` 让转换器拿到 mux 引用（`SubgraphTransformer` 靠这一步建子 mux）。

**自动转发。** 有名字的 channel 会被 `_wire` 装上一个转发闭包：`push()` 时除了进本地缓冲，还往主日志注入一条 `ProtocolEvent`。原生转换器（`_native = True`）用 channel 原名当协议方法名，用户自定义的加 `custom:` 前缀。这条注入是**绕过转换器管线**的——否则一个在 `process()` 里往自己 channel 写数据的转换器会无限递归。

**seq 的赋值时机。** 根 mux 在事件**进入主日志之前**才写 `seq`，而不是在管线之前。这样 `process()` 期间由 channel 自动转发出来的事件会拿到更小的序号，主日志里的顺序与因果顺序一致。子 mux 的 `_assign_seq=False`，所以子图转发的事件是共享同一个对象、不改信封的。

**抑制。** `push` 遍历全部转换器，任一返回 `False` 就置 `keep=False`。注意它不短路——后面的转换器仍会看到这个事件，已产生的副作用也保留。抑制只影响「是否进主日志」。

**收尾。** `close()` 先逐个 `finalize()` 并收集**第一个**异常，然后自动关闭所有投影响道和主日志，最后把首个异常重抛。`fail(err)` 则把错误传播给所有转换器与 channel，但吞掉转换器自己抛的异常——失败路径不该被清理代码的二次异常覆盖。

**异步车道。** `apush` 依次 `await` 每个转换器的 `aprocess`，慢的 `aprocess` 会串行化整条管线，这是「让后面的转换器看见前面异步结果」的代价；需要解耦就用 `schedule()` 起独立任务。`aclose()` 先 `gather` 掉所有 `schedule()` 出来的任务再 `afinalize`；`afail()` 反过来先取消这些任务。

**泵与子 mux。** `bind_pump` / `bind_apump` 把「再拉一个事件」的回调灌到主日志、每个投影响道、以及实现了 `_bind_pump` 的转换器上。`_make_child` 要求 mux 是用 `factories=` 构造的——预构建实例无法克隆到新 scope，会直接抛 `RuntimeError`。

## 关键取舍

**源码事实：** 抑制不短路。转换器的 `process` 返回 `False` 时后面的转换器照常执行，已发生的副作用不回滚。这使 `tasks` 类事件可以被折叠进更高层的投影，同时不破坏依赖这些事件的排序簿记。

**源码事实：** 自动转发绕过管线。代价是「由 channel 转发出来的事件」与「主动 push 的事件」在转换器看来不对称：前者没有任何转换器见过。这是为了避免自触发递归而付出的必然代价。

**源码事实：** `seq` 由根 mux 独占赋值，子 mux 不赋。因此一个事件对象在子图路径上是被多个 mux 共享的，谁也不能改它的信封——这是「不复制」的前提。

**架构推论：** 主日志与投影是同一批事件的两套视图，不是两份数据。转换器只做「观察 + 派生」，不负责搬运。因此新增一种消费形态的正确做法是新增转换器，而不是在主日志上开分支。

**架构推论：** 转换器注册顺序即处理顺序，且 `before_builtins` 的转换器被提到最前面。这让「内容改写类」转换器（脱敏、过滤）能在内置转换器把文本快照进投影之前生效——顺序在这里是有语义的，不是实现细节。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux.__init__` | 建立主日志，按车道顺序注册工厂与预构建转换器 |
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux._register` | 校验车道/投影形状，合并 extensions，绑定并接线 channel |
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux.push` | 跑管线、判抑制、赋 seq、入主日志 |
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux.close` | 收尾：finalize 全部转换器并自动关闭投影 |
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux._make_child` | 在更深 scope 上克隆一整套转换器实例 |
| `libs/langgraph/langgraph/stream/_mux.py` | `StreamMux._forward` | 把 channel 的 push 转成主日志里的协议事件 |

## 参考

- [LangGraph 源码：stream/_mux.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/_mux.py)
- [LangGraph 源码：stream/_types.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/_types.py)
- [LangGraph 文档：流式输出](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

「一份数据、多份视图」在主核里对应的是 WAL 与各类后台消费者的关系：日志只写一遍，索引、复制、监控各自按自己的节奏读。差别在于主核的消费者是异步追日志的，这里的转换器是**同步串在写入路径上**的——一个转换器慢，整条推流就慢。这是有意的：只有串行才能保证后面的消费者看见前面改写过的内容。

对 AI 侧而言，这一页解释了 v3 流式协议的基本形状：模型 token、状态快照、子图生命周期都只是主日志上的方法名差异。要新增一种流式输出，正确做法是写一个转换器并声明它需要哪些 stream mode，而不是在图上加开关。
