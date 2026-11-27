---
project: openai-agents
feature: tracing-processors
title: 追踪处理器与批量导出
page_type: feature
schema_version: 1
refs:
  - path: src/agents/tracing/processor_interface.py
    symbols: [TracingProcessor, TracingExporter]
---

# 追踪处理器与批量导出

## 一句话职责

把「追踪数据怎么离开进程」拆成两级抽象：processor 收到 trace/span 的生命周期回调，exporter 只负责把一批对象送出去；中间那层批处理器用队列加后台线程，让导出不阻塞 agent 执行。

## 机制是什么

`TracingProcessor` 有六个抽象方法：`on_trace_start`、`on_trace_end`、`on_span_start`、`on_span_end`、`shutdown`、`force_flush`。前四个是生命周期通知，后两个是资源与缓冲控制。

`TracingExporter` 只有一个抽象方法：`export(items: list[Trace | Span])`——一次收一批，不关心单条。这个签名的选择决定了整个下游结构：exporter 天然面向批次，于是批处理与重试都可以放在 exporter 内部，也可以放在 processor 与 exporter 之间。

两者的关系是「多对一」：多个 processor 可以并存（各自决定关注什么），每个 processor 通常配一个 exporter。这个分层让「收集」与「传输」解耦：想换成写到本地文件，只换 exporter；想改成按采样过滤，只换 processor。

抽象类的 docstring 里有三条约束，写在类的 Notes 里：所有方法都应线程安全、方法不应长时间阻塞、应自行处理错误以免干扰 agent 执行。这三条不是建议，而是对实现的硬要求——因为回调发生在业务路径上。

## 怎么实现的

**批处理器的工作模型。** `BatchTraceProcessor` 是实现这一层的主力。构造参数五个：`exporter`、`max_queue_size`（默认 8192）、`max_batch_size`（默认 128）、`schedule_delay`（默认 5 秒）、`export_trigger_ratio`（默认 0.7）。

内部维护一个 `queue.Queue`（线程安全）、一个 `threading.Event` 作为关闭信号、一个由 `max_queue_size * export_trigger_ratio` 算出的即时导出阈值（`max(1, int(...))`）、一个「下次必须导出」的时间点，以及两个锁：`_thread_start_lock`（防止重复起线程）与 `_export_lock`（串行化导出）。

**按需起线程。** 后台线程**不在构造时启动**，而是第一次有东西入队时由 `_ensure_thread_started` 起。它是双检锁：先无锁快查 `_worker_thread and .is_alive()`，再在锁内复查后创建 daemon 线程。所以「用了追踪才有线程开销」。

**回调只做入队。** `on_trace_start` 与 `on_span_end` 是唯一两个真正入队的回调；`on_trace_end` 与 `on_span_start` 是 `pass`，注释分别说明「trace 通过 on_trace_start 发送」「span 通过 on_span_end 发送」。所以这一层只导出**已开始**的 trace 与**已结束**的 span——因为结束的 span 才有完整的时间与错误，而 trace 的开始就足够标识一次 workflow。

入队用 `put_nowait`，`queue.Full` 时打一条 warning 并丢弃。这是一个明确的丢弃策略：追踪数据丢得起，业务阻塞丢不起。

**导出循环。** `_run` 是个 while 循环：每轮看当前时间与队列长度，满足「到了计划导出时间」或「队列长度达到触发阈值」就 `_export_batches()` 并把下次导出时间推后；否则 `time.sleep(0.2)`。退出条件由 `_shutdown_event` 控制，退出后再跑一次 `_export_batches(deadline=...)` 做最终排空。

注意这个循环是**轮询而非阻塞等待**——没有 `queue.get(timeout=...)`，只有固定 0.2 秒的睡眠。这让实现简单（不用处理 get 超时与 Event 的竞态），代价是空转时每 0.2 秒醒一次。

**分批排空。** `_export_batches(deadline=None)` 在 `_export_lock` 内循环：每次最多取 `max_batch_size` 个（用 `get_nowait`，`queue.Empty` 就跳出内层），取不到就结束外层。所以一次调用会把队列**排空到底**，而不是只导出一批。

两处保护：有 deadline 且已过期时打 warning 并 break（关闭时的超时保护）；exporter 抛任何 `Exception` 都被捕获并记 `[non-fatal]` 级别的错误，注释说「一个不听话的 exporter 不能杀死后台线程，否则后续所有 span 都会静默滞留」。

还有一个细节：若 exporter 是 `BackendSpanExporter` 且其 `export` 方法**没有被应用覆盖**（用 `__func__` 与类方法比较），则调它的内部 `_export_with_deadline` 以保留 deadline；否则调公开的 `export`。注释说这是「保留 deadline 给继承的后端导出行为，但绝不绕过应用自己的公开 export 覆盖」。所以这是一个有条件的内部方法调用，条件就是「确实是原版实现」。

**关闭的两条路径。** `shutdown(timeout=None)`：置关闭信号；若有 timeout 且 exporter 提供 `_request_shutdown` 就调它；算出 deadline；**若线程存在且活着就 join**，超时再打 warning；否则（线程从未启动）直接同步跑一次 `_export_batches(deadline=deadline)`。

这个 else 分支很重要：从没起过线程的处理器（比如只用来手动 flush）不能因为「等一个不存在的线程」而永远阻塞。

**force_flush 是同步的。** 直接 `_export_batches()` 无 deadline，在当前线程里把队列排空。所以调用方会在这段时间里被阻塞——这正是 `force_flush` 的语义：「在处理完成前返回」不可接受。

**默认实例的惰性创建。** `default_exporter()` 与 `default_processor()` 都是双检锁下的惰性单例，注释解释了原因：「避免在模块导入时创建网络客户端或线程原语（对 fork 型进程模型很重要）」。所以 `import agents` 不会建立 HTTP 客户端。

**后端导出器的几个常量。** `BackendSpanExporter` 定义了 ingest 端点、单字段 100KB 上限、截断后缀、允许的 usage 键（只有 `input_tokens` 与 `output_tokens`）、允许带 usage 的 span 类型（只有 `generation`），以及可重试的 4xx 状态码集合 `{408, 409, 429}`——注释说这是「镜像 OpenAI 客户端自己的重试策略：请求超时、冲突、限流」。

它持有一个**长生命周期** `httpx2.Client`，注释说「跨多次导出复用连接池」，超时配置是总 60 秒、连接 5 秒。

**api_key 的三态读取。** `api_key` 属性同时是 getter 与 setter。getter 的逻辑值得看：取 `self._api_key`，空则读环境变量 `OPENAI_API_KEY`，**读到才写回** `self._api_key`。注释用一句话解释了为什么不写回缺失值：「失败查找不写任何东西，所以它不会丢弃在查找期间由 `set_api_key()` 存入的 key」。同时「找到一个就记住」让后续导出不再反复读环境。

## 关键取舍

**源码事实：** 队列满时**丢弃**而非阻塞。追踪是观测设施，不能让被观测者等它。这条与「处理器异常被吞」是同一取向。

**源码事实：** 只导出已开始的 trace 与已结束的 span。所以一个从未结束的 span 永远不会被导出——超时或进程崩溃时会缺这条记录。

**源码事实：** `shutdown` 在从未起线程时同步导出而不是直接返回。这让「只用 force_flush 的轻量用法」也能正确排空。

**源码事实：** `default_exporter` 与 `default_processor` 的惰性创建是为了支持 fork 型进程模型——在 import 期建 httpx 客户端会在 fork 时把连接状态复制给子进程，导致连接错乱。

**源码事实：** `force_flush()` 无 deadline，会一直排空到队列空。若 exporter 卡住，`force_flush` 就卡住。

**架构推论：** 「processor 收四条回调，exporter 收一批对象」的两级结构，把「什么时候导出」与「怎么导出」彻底分开。批处理器要做的是决策（阈值、周期、丢弃策略），exporter 要做的是传输（序列化、重试、鉴权）。混在一起会导致每换一个后端就重写一遍队列逻辑。

**架构推论：** 后台线程采用「轮询 + 固定睡眠」而不是阻塞在队列上，代价是空转唤醒，收益是关闭逻辑简单——只需一个 Event 与一次 join，不必处理「get 超时与 shutdown 同时发生」。对一个每秒最多醒五次的监听线程，这个交易是合理的。

**架构推论：** 那个「exporter 的 export 是否被覆盖」的判断揭示了 Python 里一个真实的困境：既想让子类能替换行为，又想在原版实现上用更高效的路径。用 `getattr(self._exporter.export, "__func__", None) is BackendSpanExporter.export` 做身份比较，是「只在确实是原版时才走快路径」的精确写法。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/agents/tracing/processor_interface.py` | `TracingProcessor` | 四条生命周期回调加 shutdown/force_flush |
| `src/agents/tracing/processor_interface.py` | `TracingExporter` | 单方法批量导出接口 |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor` | 队列加后台线程的批处理器 |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor._ensure_thread_started` | 双检锁的按需起线程 |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor.on_span_end` | 只在此处入队 span |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor._run` | 轮询式导出循环与最终排空 |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor._export_batches` | 分批排空队列，含 deadline 与异常隔离 |
| `src/agents/tracing/processors.py` | `BatchTraceProcessor.shutdown` | join 线程或同步排空 |
| `src/agents/tracing/processors.py` | `BackendSpanExporter` | 后端导出器：端点、字段上限、重试常量 |
| `src/agents/tracing/processors.py` | `BackendSpanExporter.api_key` | 环境变量兜底且只在命中时缓存 |
| `src/agents/tracing/processors.py` | `default_exporter` | 惰性单例，避免 import 期建网络客户端 |
| `src/agents/tracing/processors.py` | `default_processor` | 惰性单例批处理器 |

## 参考

- [openai-agents-python 源码：tracing/processors.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/processors.py)
- [openai-agents-python 源码：tracing/processor_interface.py](https://github.com/openai/openai-agents-python/blob/main/src/agents/tracing/processor_interface.py)
- [OpenTelemetry 文档：Batch Span Processor](https://opentelemetry.io/docs/specs/otel/trace/sdk/#batching-processor)

## 与主核 / AI 的关系

主核的性能计数器与统计视图也是「采集与输出分离」：采集点只递增计数器，输出由一个独立的后台进程周期性读取。差别在于主核的采集是无锁原子操作且永不排队，这里因为要保留完整事件结构而必须排队——于是不得不引入丢弃策略。这揭示了观测设施的共同权衡：**保留多少细节，就承担多少排队与丢弃风险**。

与 CrewAI 的事件总线相比，这里没有处理器之间的依赖排序、没有重放、也没有同步/异步分流；它只有一个队列和一个线程。CrewAI 的总线更通用（可以承载业务事件），这一层更专注（只承载追踪）。专注的收益是行为可预测，代价是它无法被复用为业务事件通道。

对 AI 工程最实用的一条是「队列满丢弃 + 处理器异常吞掉」这两条策略必须成对出现。只做其一都不够：若队列满了阻塞，观测设施就会拖慢业务；若异常不吞，一个写错的 exporter 会让整个运行崩掉。两者同时具备，追踪才真正是「可选的、不影响的」。

另一条是默认实例的惰性创建。任何在模块导入期建立网络连接或线程的库，都会在 fork、pytest 并行、无网络环境下出问题。把「建客户端」推迟到首次使用，并用双检锁保证只建一次，是这类模块的标准做法——注释里点明的 fork 场景正是最容易被忽略的那个。
