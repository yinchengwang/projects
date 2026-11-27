---
project: langgraph
feature: run-stream-driver
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/stream/run_stream.py
    symbols: [GraphRunStream, _pump_next, _drive_until_done]
---

# 流式驱动与投影

## 一句话职责

GraphRunStream 把「调用方的一次迭代」当成发动机：谁消费投影，谁就在推动图往前跑一格，没有后台线程也没有预取。

## 机制是什么

`Pregel.stream_events(version="v3")` 返回的不是迭代器，而是一个 run stream 对象。它在构造时拿到两个东西：图的**拉式迭代器**（内部 `stream(..., version="v2")` 的迭代器）和一个已注册好转换器的 `StreamMux`。

驱动模型是「调用方驱动泵」（caller-driven pump）。`StreamChannel` 的游标在缓冲空时调 `_request_more`，而这个回调最终指向 run stream 的 `_pump_next`。所以 `for msg in run.messages` 里的每一次推进，实际上都在向图要下一个事件。没有消费者时，图就静止——这是背压的唯一来源。

投影是**单消费者**的。第二次迭代同一个投影会抛错，需要扇出就用 `projection.tee(n)`。

## 怎么实现的

**单步拉取。** `_pump_next` 做四件事：从 `graph_iter` 取一个 v2 stream part；`convert_to_protocol_event` 把它改写成统一信封（`type`/`method`/`params`，`params` 里是 `namespace`、`timestamp`、`data`，values 事件额外带 `interrupts`）；`_observe_event` 顺手记下「最新状态、是否中断、中断负载」；`mux.push` 派发。`StopIteration` 走 `mux.close()` 并标记耗尽，其它异常走 `mux.fail(e)`——两条路径都返回 `False`，于是泵自然停下。

**阻塞式结果。** `output` / `interrupted` / `interrupts` 是属性，实现都是先 `_drive_until_done(self._pump_next)` 把图跑到头，再检查 `mux._events._error`，有错就抛，没错才返回值。也就是说「读最终状态」这个动作本身会消费掉整条流，之后投影里已经空了。

**接线。** `_wire_request_more` 只调一句 `mux.bind_pump(self._pump_next)`，而不是自己遍历投影去逐个赋值。这样做的好处是子 mux（`_make_child` 建的）能继承同一个泵，于是子图句柄上的游标也能推动根图。

**多投影交错。** `interleave(*names)` 按到达顺序合并若干个投影。它依赖 `StreamChannel.push` 给每个条目盖的单调 stamp（来自 mux 的 `_next_push_seq`），每轮取 stamp 最小的那条出队；都没数据时调一次泵。进入前会逐个校验：名字必须在 `extensions` 里、必须是 `StreamChannel`、必须已绑定且是同步车道、必须还没有订阅者，任何一条不满足就直接抛错（而不是静默跳过）。

**异步单飞泵。** `_apump_next` 用 `asyncio.Condition` 实现「取号」语义：同一时刻只允许一个任务调 `graph_aiter.__anext__()`（asyncio 迭代器不能并发推进），其它调用方在 Condition 上等待，由正在泵的任务每完成一步 `notify_all`。这样「被动消费者」（它的缓冲正被别人填）能在自己数据落地的瞬间醒来，而不是排一次队、晚一个事件才看到。

**把拉取做成子任务。** 真正的一次 `__anext__` 被包成 `asyncio.ensure_future` 的子任务。这是为了让 `abort()` 能在拉取**进行中**取消它——直接 `aclose()` 一个正在运行的异步生成器会撞上「asynchronous generator is already running」。取消时的分支靠 `_aborting` 标志区分：abort 触发的取消安静收尾，外部取消则继续上抛。

**提前终止。** `abort()` 幂等：标记耗尽、摘掉图的迭代器引用、调它的 `close()`（同步车道靠这个把 `GeneratorExit` 传进去，取消在跑的节点与子图）、再关 mux。`__exit__` 直接调它，所以 `with` 块掉出去就一定收尾。

**缺失投影的报错。** `__getattr__` 只在原生投影都已安装（`__init__` 里按 `mux.native_keys` 逐个 `setattr`）之后才会被调用，所以走到它必然是名字不存在。它报错时想列出已注册投影名，但必须从 `run.__dict__` 直接读 `_mux`——访问 `self._mux` 会再次触发 `__getattr__` 并无限递归。

**子图句柄。** `SubgraphRunStream` 继承同一个类但传 `graph_iter=None` 与 `wire_pump=False`：它自己不向上游拉，而是在 `_pump_next` 里委托给构造时捕获的父泵。父泵一推，事件经由 `SubgraphTransformer` 转发进子 mux，子图投影就被填上了。

## 关键取舍

**源码事实：** 同步车道用属性、异步车道用方法（`await run.output()`）。这不是风格不一致，而是刻意的：异步下写成属性会让「忘了 await」静默拿到一个 coroutine 对象，只有写成方法才能在类型检查与运行期都立刻暴露。

**源码事实：** 读取 `output` 会消费整条流。因此「先看流、后拿结果」在同一份 run 上做不到——想要两样就必须在迭代体里自己攒，或者先 `tee`。

**源码事实：** 单消费者是硬约束。开销小（一个布尔标记），但把「多点消费」的责任明确推给调用方，避免内核为不确定数量的消费者维护回放缓冲。

**架构推论：** 没有后台任务意味着「图的推进速度 = 最慢消费者的速度」。这在服务端是优点（内存有界、取消及时），在需要并行消费多个投影时会变成串行等待——这也正是 `interleave` 存在的理由：一个循环同时推进多个投影。

**架构推论：** 子图句柄不自持泵，而是委托父泵。这让子图导航不需要额外的调度路径，代价是句柄脱离父流就没有意义——它只能存活在父 run 的迭代体内。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/stream/run_stream.py` | `GraphRunStream.__init__` | 接图迭代器与 mux，安装原生投影为属性 |
| `libs/langgraph/langgraph/stream/run_stream.py` | `GraphRunStream._pump_next` | 拉一个事件、转协议事件、观察、派发；异常转 close/fail |
| `libs/langgraph/langgraph/stream/run_stream.py` | `_drive_until_done` | 同步车道「跑到耗尽」的循环 |
| `libs/langgraph/langgraph/stream/run_stream.py` | `GraphRunStream._wire_request_more` | 经 mux 绑定泵，使子 mux 继承同一回调 |
| `libs/langgraph/langgraph/stream/run_stream.py` | `GraphRunStream.interleave` | 按 stamp 合并多个投影的到达顺序 |
| `libs/langgraph/langgraph/stream/run_stream.py` | `AsyncGraphRunStream._apump_next` | 取号式单飞泵，Condition 唤醒被动消费者 |
| `libs/langgraph/langgraph/stream/run_stream.py` | `AsyncGraphRunStream.abort` | 取消在途拉取后关闭图迭代器与 mux |
| `libs/langgraph/langgraph/stream/run_stream.py` | `SubgraphRunStream` | 委托父泵的子图句柄 |
| `libs/langgraph/langgraph/pregel/main.py` | `_resolve_parent_ns` | 从调用方配置解出根 scope，供 mux 限定投影层级 |

## 参考

- [LangGraph 源码：stream/run_stream.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/run_stream.py)
- [LangGraph 源码：stream/stream_channel.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/stream/stream_channel.py)
- [LangGraph 文档：流式输出与 backpressure](https://langchain-ai.github.io/langgraph/concepts/streaming/)

## 与主核 / AI 的关系

调用方驱动泵与主核里「迭代器式执行器」是同一个思路：执行由消费者拉取触发，而不是生产者推。主核用它来避免算子之间堆积无界队列，这里用它来让「流式输出」天然带上背压——LLM 产得再快，读得慢就停在那儿。

对 AI 侧的实际影响是取消语义：`abort()` 会把 `GeneratorExit` 送进图生成器，进而取消在跑的节点与嵌套子图。做 agent 服务时，客户端断连就应该走这条路径，而不是等图自己跑完——否则一个挂住的工具调用会一直占着执行槽。
