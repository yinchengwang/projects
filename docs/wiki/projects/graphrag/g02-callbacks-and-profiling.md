---
project: graphrag
feature: callbacks-and-profiling
title: 回调与性能剖析
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag/graphrag/callbacks/workflow_callbacks.py
    symbols: [WorkflowCallbacks]
  - path: packages/graphrag/graphrag/callbacks/workflow_callbacks_manager.py
    symbols: [WorkflowCallbacksManager]
  - path: packages/graphrag/graphrag/index/run/profiling.py
    symbols: [WorkflowProfiler]
  - path: packages/graphrag/graphrag/index/utils/derive_from_rows.py
    symbols: [derive_from_rows, derive_from_rows_asyncio, ParallelizationError]
---

# 回调与性能剖析

## 一句话职责

三件围绕「执行可观测性」的事：用 `Protocol` 声明一组工作流生命周期回调让调用方只实现需要的几个；用一个 `Manager` 把它们扇出给任意多个订阅者；用 `WorkflowProfiler` 在**每个工作流外面**套一层计时与内存追踪；而 `derive_from_rows` 是这三者的消费方——它把每行的进度推给回调，把任一行的失败汇总成一个 `ParallelizationError`。

## 机制是什么

**`WorkflowCallbacks` 是 `Protocol` 而不是抽象基类。** 这是这一页最值得先讲清楚的一点，因为 `NoopWorkflowCallbacks` 的文档字符串把它说反了：

```
class WorkflowCallbacks(Protocol):
    """A collection of callbacks that can be used to monitor the workflow execution.

    This base class is a "noop" implementation so that clients may implement just the callbacks they need.
    """
```

**「这个基类是一个 noop 实现」是错的** —— `Protocol` 没有任何实现，它的六个方法都是 `...`。**真正的 noop 实现是同目录下的 `NoopWorkflowCallbacks`。**

**而这个措辞错误关系到一条重要的机制：`Protocol` 是**结构化**类型，`ABCMeta` 是名义类型。两者的差别在 `WorkflowCallbacksManager` 里被利用：

```
def pipeline_start(self, names: list[str]) -> None:
    for callback in self._callbacks:
        if hasattr(callback, "pipeline_start"):
            callback.pipeline_start(names)
```

**它用 `hasattr` 逐个探方法而不是直接调用。** 若 `WorkflowCallbacks` 是抽象基类，「必须实现全部六个方法」会被 `ABC` 强制（否则无法实例化），于是 `hasattr` 检查永远为真、没有意义。

**而 `Protocol` 允许「部分实现」** —— 一个只定义 `progress` 的普通类在类型层面**不满足** `WorkflowCallbacks`（静态检查会报错），但**运行期完全可用**。`hasattr` 检查正是为这种运行期的宽容兜底。

**于是这一页的机制可以概括成一句话：用 `Protocol` 表达契约，用 `hasattr` 容忍不完整的实现。**

## 怎么实现的

### 一、`WorkflowCallbacks`：六个事件

```
def pipeline_start(self, names: list[str]) -> None
def pipeline_end(self, results: list[PipelineRunResult]) -> None
def workflow_start(self, name: str, instance: object) -> None
def workflow_end(self, name: str, instance: object) -> None
def progress(self, progress: Progress) -> None
def pipeline_error(self, error: BaseException) -> None
```

**六个事件按粒度分成三对：**

| 粒度 | 事件 | 参数 |
|---|---|---|
| 整条管道 | `pipeline_start` / `pipeline_end` | 工作流名字列表 / 所有结果 |
| 单个工作流 | `workflow_start` / `workflow_end` | 工作流名 + 实例对象 |
| 过程 | `progress` | `Progress`（已完成/总数/描述） |
| **异常** | `pipeline_error` | 异常对象本身 |

**`pipeline_start` 的参数是 `names: list[str]`** —— 于是**在开跑之前就能知道要跑哪些工作流**（因为管道是预先构建的，`pipeline.names()` 可以直接问）。

**而 `workflow_start` / `workflow_end` 的 `instance: object` 参数**：`workflow_start` 实际传的是 `None`，而 `workflow_end` 传的是 `PipelineRunResult`。**这个签名用 `object` 笼统地接受了两种不同的东西**，是一个类型上的粗糙处。

**`pipeline_error` 只接收异常本身，不接收「哪个工作流失败了」** —— 而那个信息在 `pipeline_end` 的 `results` 里（每个 `PipelineRunResult` 带 `workflow` 与 `error`）。**于是「实时知道出错」与「事后知道哪个出错」分散在两处。**

### 二、`NoopWorkflowCallbacks`：六个空方法体

**它的全部内容是六个只有 docstring 的空方法。**

**这不是「实现没写」，而是「默认行为就是什么都不做」。** 而它的存在理由与另外几页的 `NoopCache` / `NoopMetricsStore` 完全一致：**让「没配回调」不必判空**。

**`build_index` 里那一行就是它的用法：**

```
workflow_callbacks = create_callback_chain(callbacks) if callbacks else NoopWorkflowCallbacks()
```

**于是后续所有 `workflow_callbacks.pipeline_start(...)` 之类的调用永远有对象可调**（见 g01 那页）。

**而 `ConsoleWorkflowCallbacks` 是它的镜像：每个事件都 `print`。**

```
def pipeline_start(self, names): print("Starting pipeline with workflows:", ", ".join(names))
def pipeline_end(self, results): print("Pipeline complete")
def workflow_start(self, name, instance): print(f"Starting workflow: {name}")
def workflow_end(self, name, instance):
    print("")  # account for potential return on prior progress
    print(f"Workflow complete: {name}")
    if self._verbose: print(instance)
def pipeline_error(self, error): print(f"Pipeline error: {error}")
def progress(self, progress):
    complete = progress.completed_items or 0
    total = progress.total_items or 1
    ...
    print(f"{start:{'.'}<{percent}}", flush=True, end="\r")
```

**四个细节：**

1. **`workflow_end` 先打一个空行**，注释说「为了应对之前进度输出的回车」—— 因为 `progress` 用 `end="\r"` 在同一行上重绘，不换行就叠加。
2. **进度条用 `{start:{'.'}<{percent}}` 填充格式**。`start` 是从字典里取出的当前累计字符串，用 `.` 左对齐填到 `percent` 宽度（即百分比数值本身当下标）。**这是一个把「百分比」同时当长度用的技巧**（`0..100` 恰好可以当 `0..100` 个字符的宽度）。
3. **`flush=True, end="\r"`** —— 立即可见且不换行。
4. **`if self._verbose: print(instance)`** —— 只有 verbose 才打印完整结果（可能是大 DataFrame）。

**`progress` 里 `or 1` 的写法防的是除零**（`total_items` 为 `None` 或 `0`）。

### 三、`WorkflowCallbacksManager`：扇出与 `hasattr` 守卫

**它本身实现了 `WorkflowCallbacks`（六个方法齐全），并把每次调用转发给所有注册者。**

```
def pipeline_start(self, names: list[str]) -> None:
    for callback in self._callbacks:
        if hasattr(callback, "pipeline_start"):
            callback.pipeline_start(names)
```

**六个方法都是这个形状，而三个决定值得说：**

**第一，`hasattr` 守卫让「部分实现」成为合法用法。** 一个只关心进度的回调只需要 `progress` 方法。**如果没有这个守卫，那种回调会在 `pipeline_start` 时抛 `AttributeError`** —— 而 `Protocol` 在运行期不做检查，所以这个错误只会在装配后第一次事件时炸出来。

**第二，没有异常隔离。** 若某个回调抛异常，循环中断、后续回调收不到事件，异常向上传播到 `run_pipeline` 的 `try` 里——**于是「一个回调的 bug 会让整个索引失败」。**

**这是一个可以商量的取舍**：隔离（`try/except` 逐个调）能让索引不受观测代码影响，但也**掩盖了回调的错误**。当前选择是「宁可索引失败也不静默吞掉回调的异常」。

**第三，`register` 只是 `append`，没有去重也没有顺序保证。**

```
def register(self, callbacks: WorkflowCallbacks) -> None:
    self._callbacks.append(callbacks)
```

**顺序即注册顺序**（列表保序），于是「先注册的先生效」。**而重复注册同一个对象会让它收到两次事件** —— 没有检查。

**`create_callback_chain` 是构造这个 Manager 的唯一入口：**

```
def create_callback_chain(callbacks: list[WorkflowCallbacks] | None) -> WorkflowCallbacks:
    manager = WorkflowCallbacksManager()
    for callback in callbacks or []:
        manager.register(callback)
    return manager
```

**返回类型是 `WorkflowCallbacks` 而不是 `WorkflowCallbacksManager`** —— **调用方拿不到 `register`**。这是一个有意的收窄：装配完成后不应再动态增减订阅者。

### 四、`WorkflowProfiler`：上下文管理器 + `tracemalloc`

**它在 `run_pipeline` 里的用法只有三行：**

```
context.callbacks.workflow_start(name, None)

with WorkflowProfiler() as profiler:
    result = await workflow_function(config, context)

context.callbacks.workflow_end(name, result)
```

**注意 `with` 包住的正是「执行工作流」这一段** —— 于是计时与内存统计只覆盖工作流本身，不包括回调与结果序列化。

**`__enter__` / `__exit__` 各做两件事。**

```
def __enter__(self) -> Self:
    tracemalloc.start()
    self._start_time = time.time()
    return self

def __exit__(self, exc_type, exc_val, exc_tb) -> None:
    self._elapsed = time.time() - self._start_time
    self._current_memory, self._peak_memory = tracemalloc.get_traced_memory()
    self._tracemalloc_overhead = tracemalloc.get_tracemalloc_memory()
    tracemalloc.stop()
```

**`__exit__` 里没有返回值 → 返回 `None` → 异常继续传播。** 这是正确的：剖析不该吞异常。

**而 `get_traced_memory()` 返回的是 `(current, peak)`** —— 顺序是「当前, 峰值」。赋值给 `(self._current_memory, self._peak_memory)` 正好对应。

**三个字段的语义值得逐个核对：**

| 字段 | 来源 | 真实含义 |
|---|---|---|
| `overall` | `time.time()` 差值 | **墙钟时间**（含 `await` 等待） |
| `peak_memory_bytes` | `get_traced_memory()[1]` | 该工作流期间的内存峰值 |
| `memory_delta_bytes` | `get_traced_memory()[0]` | **不是「增量」，是「结束时仍存活的对象总量」** |
| `tracemalloc_overhead_bytes` | `get_tracemalloc_memory()` | 追踪器自身的开销 |

**`memory_delta_bytes` 这个名字与它的值不符** —— 它拿的是「当前被追踪的内存总量」，而「delta」应当需要「开始时的量」做减法。**而 `__enter__` 里没有记录起始内存。**

**这是一个真实的命名缺陷**：一个工作流如果创建了大量临时对象并在结束时释放，它的「delta」会接近 0，而峰值可能很高。**要算真正的增量，需要在 `__enter__` 里调一次 `get_traced_memory()[0]`。**

**而 `WorkflowMetrics` 的 dataclass 字段名与之一致：**

```
overall: float                      # Wall-clock time in seconds.
peak_memory_bytes: int
memory_delta_bytes: int             # Net memory change after workflow completion (tracemalloc).
tracemalloc_overhead_bytes: int     # Memory used by tracemalloc itself for tracking allocations.
```

**「Net memory change after workflow completion」这个描述同样不准确** —— 它没有「之前」的参照。

**`tracemalloc` 是进程级全局单例。**

**`tracemalloc.start()` 启动全局追踪，`tracemalloc.stop()` 关闭它。** 于是：

1. **若宿主进程本来开着 `tracemalloc`，这个 `__exit__` 会把它关掉** —— 影响整个进程。
2. **若两个 profiler 嵌套（或并发），内层的 `stop()` 会让外层从此收不到数据。**
3. **每个工作流都 `start`/`stop` 一次** —— 而 `tracemalloc.start()` 的开销不小（要截取分配栈），于是「每个工作流都重新启动追踪」是明显的浪费。

**这三个问题在当前用法下都不触发**（`run_pipeline` 串行地、一次一个地包工作流；graphrag 不假设宿主开了 tracemalloc），**但它们都是「共享全局资源」的典型代价**。

**而 `tracemalloc` 会显著拖慢执行** —— 它的开销通常在 2–5 倍。**把每个工作流都套上它，意味着 graphrag 的索引默认就跑在带追踪的模式下。** 这是这个设计最值得质疑的地方：**剖析本应是可选的，而这里是无条件的。**

### 五、`derive_from_rows`：把并发执行 + 进度 + 错误汇总收成一个函数

**它是这三个机制的消费方，也是这一页唯一涉及「真正的并发」的部分。**

**它的签名说明了三件事：**

```
async def derive_from_rows(
    input: pd.DataFrame,
    transform: Callable[[pd.Series], Awaitable[ItemType]],
    callbacks: WorkflowCallbacks | None = None,
    num_threads: int = 4,
    async_type: AsyncType = AsyncType.AsyncIO,
    progress_msg: str = "",
) -> list[ItemType | None]:
```

1. **`transform` 接受一行、返回 awaitable** —— 于是它是异步的（通常内部是一次 LLM 调用）。
2. **`async_type` 决定「用什么方式并发」** —— 两种：纯 asyncio、或 asyncio + 线程。
3. **返回 `list[ItemType | None]`** —— **与输入等长、失败位置为 `None`**（若没有失败的话）。

**而它的三个分支里 `match` 只处理两个 case。**

```
match async_type:
    case AsyncType.AsyncIO:
        return await derive_from_rows_asyncio(input, transform, callbacks, num_threads, progress_msg)
    case AsyncType.Threaded:
        return await derive_from_rows_asyncio_threads(input, transform, callbacks, num_threads, progress_msg)
    case _:
        msg = f"Unsupported scheduling type {async_type}"
        raise ValueError(msg)
```

**而 refs 里只点了 `derive_from_rows` 与 `derive_from_rows_asyncio`** —— 第三个（`derive_from_rows_asyncio_threads`）是同一模板的另一份。

**两种并发的实现只有 `gather` 参数的差别。**

**`AsyncIO` 版（默认）：**

```
semaphore = asyncio.Semaphore(num_threads or 4)

async def gather(execute):
    async def execute_row_protected(row):
        async with semaphore:
            return await execute(row)
    tasks = [asyncio.create_task(execute_row_protected(row)) for row in input.iterrows()]
    return await asyncio.gather(*tasks)
```

**`Threaded` 版：**

```
semaphore = asyncio.Semaphore(num_threads or 4)

async def gather(execute):
    tasks = [asyncio.to_thread(execute, row) for row in input.iterrows()]

    async def execute_task(task):
        async with semaphore:
            thread = await task
            return await thread

    return await asyncio.gather(*[execute_task(task) for task in tasks])
```

**两个版本的语义差别很关键：**

| | `AsyncIO` | `Threaded` |
|---|---|---|
| 并发单位 | 协程 | 线程 |
| 信号量位置 | **协程内部**（`async with` 包住 `execute`） | **协程外面**（`async with` 包住「等线程 + 等结果」） |
| 实际并行度 | 受 `asyncio` 单线程限制 | 真正的多线程（但受 GIL） |
| 适用 | I/O 密集（LLM 调用） | **同步阻塞的 transform** |

**`Threaded` 版的信号量位置有一处微妙：**

```
tasks = [asyncio.to_thread(execute, row) for row in input.iterrows()]
```

**这一行就把所有行都提交给线程池了**（`asyncio.to_thread` 立即返回一个 future）。**于是信号量限制的不是「有几个线程在跑」，而是「有几个协程在等」。** 线程池本身的大小是默认的（`min(32, cpu+4)`），于是**实际并发线程数是线程池上限而不是 `num_threads`**。

**这是一个真实的语义偏差**：`num_threads` 名字暗示「最多几个线程在跑」，而实际是「最多几个 future 被 await」。**若线程池上限大于 `num_threads`，那 `num_threads` 只限制了结果消费速度。**

**而 `num_threads or 4` 的写法让 `0` 退化为 4** —— 于是 `num_threads=0` 不能表达「不并发」。

**核心逻辑在 `_derive_from_rows_base` 里，它被两个版本共用。**

```
tick = progress_ticker(callbacks.progress, num_total=len(input), description=progress_msg)
errors: list[tuple[BaseException, str]] = []

async def execute(row):
    try:
        result = transform(row[1])
        if inspect.iscoroutine(result):
            result = await result
    except Exception as e:
        errors.append((e, traceback.format_exc()))
        return None
    else:
        return cast("ItemType", result)
    finally:
        tick(1)

result = await gather(execute)

tick.done()

for error, stack in errors:
    logger.error("parallel transformation error", exc_info=error, extra={"stack": stack})

if len(errors) > 0:
    raise ParallelizationError(len(errors), errors[0][1])

return result
```

**六个要点：**

**第一，`transform(row[1])` 里的 `row[1]`** —— `input.iterrows()` 产出 `(index, Series)` 元组，而 `transform` 只要 `Series`。**下标 `[1]` 是「取第二项」**，正确但不易读。

**第二，`inspect.iscoroutine(result)` 让它同时接受同步与异步 transform。**

```
result = transform(row[1])
if inspect.iscoroutine(result):
    result = await result
```

**这放宽了类型声明（声明是 `Callable[..., Awaitable]`）**。**于是签名说「必须异步」而实现接受同步** —— 一个有用的宽容，但不一致。

**第三，`errors` 是一个**列表**，而多个协程并发 append。** CPython 里 `list.append` 是原子的（受 GIL 保护），**于是这里不需要锁**。**而这依赖 CPython 的实现细节** —— 在 free-threaded Python 上需要加锁。

**第四，`finally: tick(1)`** —— 进度无论成功失败都推进一格。**这保证了进度条的终值等于总行数**，不会因为失败而停在中途。

**第五，`tick.done()` 在 `gather` 之后** —— 用于收尾（补一个换行/最终刷新）。

**第六，错误策略是「全部跑完再抛」。**

```
if len(errors) > 0:
    raise ParallelizationError(len(errors), errors[0][1])
```

**`ParallelizationError` 只带「错误数量」与「第一个错误的堆栈字符串」。**

```
class ParallelizationError(ValueError):
    def __init__(self, num_errors: int, example: str | None = None):
        msg = f"{num_errors} Errors occurred while running parallel transformation, could not complete!"
        if example:
            msg += f"\nExample error: {example}"
        super().__init__(msg)
```

**三个决定：**

1. **继承 `ValueError`** —— 与 lightrag 的 `QueryValidationError(ValueError)` 同一个手法：宽泛的 `except ValueError` 会接住它。
2. **只报数量与一个例子** —— 因为异常消息不能无限长。**而全部错误已经逐条写进日志**（`for error, stack in errors: logger.error(...)`），于是「日志里有全部，异常里有一个代表」。
3. **`example` 传的是 `errors[0][1]`（堆栈字符串）而不是异常对象** —— 于是消息里是一段完整堆栈。**这让异常消息变得很长，但也让「只看异常就能定位」成为可能。**

**「全部跑完再抛」是一个重要的语义：**

- 好处：一次运行能看到**所有**失败（而不是第一个就停）。
- 代价：**已经成功的行的工作被浪费了** —— 因为整批最终仍会失败，调用方拿不到 `result`。

**这与 `completion_batch`（e01 那页）的「部分成功」相反**：那里失败的行以 `Exception` 的形式留在结果里，整批仍然返回。**判据是「这一批的产物是否构成一个完整的东西」**：几十次 LLM 调用的结果拼成一张表，缺一行就缺一条实体——那不如整批重来。

## 关键取舍

**源码事实：** `WorkflowCallbacks` 是 `Protocol`（六个方法都是 `...`），而它的 docstring 却写「这个基类是一个 noop 实现」——**该描述错误**，真正的 noop 是同目录的 `NoopWorkflowCallbacks`。

**源码事实：** `WorkflowCallbacksManager` 每个方法都用 `hasattr(callback, "xxx")` 守卫后才调用，于是「只实现部分方法的回调」在运行期合法（尽管静态类型上不满足 `Protocol`）。

**源码事实：** Manager 对回调异常**不做隔离** —— 一个回调抛错会中断后续回调并把异常传播到 `run_pipeline` 的 `try`，使整个索引失败。

**源码事实：** `register` 只是 `append`（无去重、顺序即注册顺序）；`create_callback_chain` 的返回类型收窄为 `WorkflowCallbacks`，于是装配后拿不到 `register`。

**源码事实：** `pipeline_start` 的参数是工作流名字列表（因为管道预先构建，`pipeline.names()` 可直接问）；`pipeline_error` 只接异常不接「哪个工作流失败」，那个信息在 `pipeline_end` 的 results 里。

**源码事实：** `workflow_start` 的 `instance` 实际传 `None`，`workflow_end` 传 `PipelineRunResult` —— 签名用 `object` 笼统接受两种不同的东西。

**源码事实：** `WorkflowProfiler` 用 `tracemalloc`，在 `__enter__` 里 `start()`、`__exit__` 里 `stop()`，且 `__exit__` 返回 `None`（异常继续传播）。

**源码事实：** `memory_delta_bytes` 取的是 `get_traced_memory()[0]`（当前被追踪的内存总量），**不是增量**——因为 `__enter__` 没有记录起始值。dataclass 里「Net memory change」的描述同样不准确。

**源码事实：** `tracemalloc` 是进程级全局追踪器：`stop()` 会关掉别人启动的追踪、嵌套 profiler 会互相破坏、每次 `start()` 有可观开销（截取分配栈）。而 `run_pipeline` **对每个工作流无条件套 profiler** —— 剖析不可选，且拖慢执行。

**源码事实：** profiler 只包住 `await workflow_function(...)` 一行，不含回调与结果序列化；工作流抛异常时 `workflow_end` 不会被调用（异常直接到 `run_pipeline` 的 `except`）。

**源码事实：** `ConsoleWorkflowCallbacks.workflow_end` 先打一个空行以应对 `progress` 的 `end="\r"`；进度条用 `{start:{'.'}<{percent}}` 把百分比同时当长度用；`progress` 里 `or 1` 防除零；`print(instance)` 只在 verbose 时。

**源码事实：** `derive_from_rows` 的 `num_threads or 4` 让 `0` 退化为 4，于是无法表达「不并发」；`Threaded` 版在列出任务时就已把所有行提交给线程池，**信号量限制的是「几个协程在等」而不是「几个线程在跑」**，实际并发受线程池上限约束。

**源码事实：** `_derive_from_rows_base` 用 `inspect.iscoroutine` 兼容同步 transform（尽管类型声明是 `Callable[..., Awaitable]`）；`row[1]` 取 `iterrows()` 的 Series 项；`errors` 并发 `append` 不加锁（依赖 CPython 的 GIL 原子性）；`finally: tick(1)` 保证进度终值等于行数。

**源码事实：** 错误策略是「全部跑完再抛 `ParallelizationError(数量, 第一个堆栈)`」；全部错误逐条写日志、异常里只带一个代表。这与 `completion_batch` 的「部分成功」相反，判据是「这一批产物是否构成完整的东西」。

**架构推论：** `Protocol` + `hasattr` 这个组合，本质上是**「静态契约宽松、运行期容错」**。它让「只实现 `progress` 的回调」成为可能，代价是**拼错方法名不会报错**（`pipeline_star` 会静默不生效，因为 `hasattr` 判假）。**这是一个用「漏调用」换「多实现的自由」的交易** —— 而漏调用在观测代码里极其难发现（你以为在记录，实际什么都没记）。

**架构推论：** profiler 无条件启用这件事，反映了一个常见的资源管理失误：**把「诊断设施」当成「基础设施」**。`tracemalloc` 的 2–5 倍开销对索引这种本来就要跑几十分钟的任务可能可接受，但它是不可关的——而「性能剖析应当可选」是这个领域的常识。**更好的形态是一个 `NullProfiler`（只返回零值），由配置或标志位选择**——`NoopCache` / `NoopMetricsStore` 的存在证明作者知道这个手法，只是这里没用。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag/graphrag/callbacks/workflow_callbacks.py` | `WorkflowCallbacks` | 六事件的 `Protocol`（docstring 有误） |
| `packages/graphrag/graphrag/callbacks/noop_workflow_callbacks.py` | `NoopWorkflowCallbacks` | 六空方法，供「没配回调」用 |
| `packages/graphrag/graphrag/callbacks/console_workflow_callbacks.py` | `ConsoleWorkflowCallbacks` | 打印事件与进度条 |
| `packages/graphrag/graphrag/callbacks/workflow_callbacks_manager.py` | `WorkflowCallbacksManager` | `hasattr` 守卫的扇出，无异常隔离 |
| `packages/graphrag/graphrag/index/run/utils.py` | `create_callback_chain` | 构造 Manager，返回类型收窄 |
| `packages/graphrag/graphrag/index/run/profiling.py` | `WorkflowProfiler` | `tracemalloc` + 计时，`memory_delta_bytes` 名不符实 |
| `packages/graphrag/graphrag/index/typing/stats.py` | `WorkflowMetrics` / `PipelineRunStats` | 四个指标字段与聚合容器 |
| `packages/graphrag/graphrag/index/run/run_pipeline.py` | 工作流循环 | `workflow_start → with WorkflowProfiler() → workflow_end → 存 stats` |
| `packages/graphrag/graphrag/index/utils/derive_from_rows.py` | `derive_from_rows` | 按 `AsyncType` 分派 |
| `packages/graphrag/graphrag/index/utils/derive_from_rows.py` | `derive_from_rows_asyncio` | 协程并发 + 信号量在内部 |
| `packages/graphrag/graphrag/index/utils/derive_from_rows.py` | `_derive_from_rows_base` | 进度、错误收集、`ParallelizationError` |
| `packages/graphrag/graphrag/index/utils/derive_from_rows.py` | `ParallelizationError` | 继承 `ValueError`，带数量与首个堆栈 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [Python 文档：tracemalloc](https://docs.python.org/3/library/tracemalloc.html)
- [Python 文档：typing.Protocol](https://docs.python.org/3/library/typing.html#typing.Protocol)
- [Python 文档：asyncio.Semaphore 与 asyncio.to_thread](https://docs.python.org/3/library/asyncio-sync.html)

## 与主核 / AI 的关系

回调这套与主核的**统计收集器（`pgstat`）** 解决同一个问题：让外部观察者在不侵入执行逻辑的前提下看到进度与错误。而差别在于**「推送」vs「拉取」**。

**`pgstat` 是拉取式的**：后端在共享内存里累加计数器，前端（或一个后台进程）定期来读。**而回调是推送式的**：管道主动调用订阅者的方法。

**推送给了一个 `pgstat` 不会有的问题：订阅者的异常会传到发布者。** 数据库不会因为「统计进程挂了」而让事务失败——因为它们是解耦的（一个写共享内存，一个异步读）。**而这里一个回调抛错会让索引失败。**

**这个差别不是设计失误，而是「同进程」的代价**：回调就在同一个 Python 进程里，没有隔离手段（除非用 `try/except`）。**而 `try/except` 会让「回调的 bug」静默** —— 两难。

**`derive_from_rows` 的「全部跑完再抛」与数据库的**批量操作（如 `COPY`）**同类**：一条坏行会让整个 `COPY` 事务回滚。**判据是「批量是不是一个原子单位」** —— 若这一批的目的是产生一份完整的结果，那么部分成功没有价值。

**而 `ParallelizationError` 只带一个堆栈示例、全部错误进日志，与数据库的「错误消息有 `DETAIL` / `HINT` 但不含全部行」是同一个取舍**：人读的消息要短，机器可查的细节要在别处。

**把剖析无条件套在每个工作流外，与数据库里「永远开着 `EXPLAIN ANALYZE`」是同类错误。** 数据库的工具设计清楚地知道：`EXPLAIN`（不执行）与 `EXPLAIN ANALYZE`（执行并测量）是两个不同的命令，后者明确标注了开销。**诊断设施的默认状态应当是关闭的。**

对 agent 工程的直接价值有三条。其一，**`Protocol` + `hasattr` 让回调可以只实现一部分，但拼错方法名会静默失效**。要缓解这个问题，应当在 `register` 时校验「至少实现了一个已知方法」，否则那个订阅者永远不会被调用。其二，**回调的异常要不要隔离，取决于「观测失败是否该影响主流程」**。选择不隔离的代价是观测代码的 bug 会变成索引失败；选择隔离的代价是那个 bug 永远不被发现。其三是**「诊断设施必须可关」**。`tracemalloc` 有 2–5 倍开销，把它无条件挂在每个工作流外面，等于让所有人的默认路径都变慢——而一个 `NullProfiler` 就能解决这件事，且这个代码库在别处（`NoopCache` / `NoopMetricsStore`）已经用过这个手法。