---
project: graphrag
feature: threading-and-metrics
title: 线程化执行与指标聚合
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-llm/graphrag_llm/threading/completion_thread_runner.py
    symbols: [completion_thread_runner, ThreadedLLMCompletionResponseHandler]
  - path: packages/graphrag-llm/graphrag_llm/metrics/metrics_aggregator.py
    symbols: [MetricsAggregator]
  - path: packages/graphrag-llm/graphrag_llm/metrics/metrics_processor.py
    symbols: [MetricsProcessor]
  - path: packages/graphrag-llm/graphrag_llm/model_cost_registry/model_cost_registry.py
    symbols: [ModelCostRegistry, ModelCosts]
---

# 线程化执行与指标聚合

## 一句话职责

用两套机制把「一次同步补全」变成「可以并发的一批」：线程侧用**输入队列 + N 个工作线程 + 输出队列 + 一个回调线程**的四件套跑批；指标侧用**「每次请求一个字典，累加进存储，读取时算派生量」**的三段式把原始计数变成费率与均价。

## 机制是什么

两半的动机不同但都源于同一个事实：**底层 SDK 是同步阻塞的**。

| | 问题 | 解法 |
|---|---|---|
| 线程化 | 同步调用让并发只能靠多线程 | 队列 + 线程池 + 回调线程 |
| 指标 | 每个请求分散在很多线程里，无法直接统计 | 每请求一个字典，最后累加 |

**而指标那套的关键设计是「派生量不在写入时算，在读取时算」。** 原始计数（请求数、重试次数、token 数、花费）是**可加的**，所以能直接累加；而费率与均价（失败率、缓存命中率、每响应耗时）是**不可加的**，只能在拿到总量后计算。

## 怎么实现的

### 一、`completion_thread_runner`：四件套与两个终止哨兵

**这个上下文管理器创建四个东西：**

```
quit_process_event = threading.Event()
threads, input_queue, output_queue = _start_completion_thread_pool(...)
handle_response_thread = threading.Thread(target=_process_output, args=(quit_process_event, output_queue, response_handler))
handle_response_thread.start()
```

| 组件 | 类型 | 职责 |
|---|---|---|
| `input_queue` | `Queue(queue_limit)` | 调用方投递 `(request_id, kwargs)` |
| `threads` | N 个 `CompletionThread` | 消费输入队列、调补全、产出到输出队列 |
| `output_queue` | `Queue()`（无上限） | 承载 `(request_id, response_or_exception)` |
| `handle_response_thread` | 一个独立线程 | 消费输出队列、调用户回调 |

**`queue_limit` 只作用于输入队列，输出队列无上限。** 这是一个有意的非对称：

- **输入有上限 = 对调用方形成背压**（`input_queue.put` 阻塞），于是提交得比处理快时调用方被拖慢，内存不涨。
- **输出无上限**：若输出队列也限长，工作线程会在 `put` 上阻塞，而唯一的消费者是回调线程——**一旦回调慢，所有工作线程停摆**，吞吐退化到回调速度。

**`None` 是终止信号，而它们走两个队列。**

```
# CompletionThread.run()
input_data = self._input_queue.get(timeout=1)
if input_data is None:
    break          # 该线程退出

# _process_output()
data = output_queue.get(timeout=1)
if data is None:
    break          # 回调线程退出
```

**`_cleanup()` 的顺序是两个终止信号的正确用法：**

```
def _cleanup():
    for _ in threads:
        input_queue.put(None)          # 1. 每个工作线程一个 None
    for thread in threads:
        while thread.is_alive():
            thread.join(timeout=1)      # 2. 等工作线程全部退出
    output_queue.put(None)              # 3. 再给回调线程一个 None
    while handle_response_thread.is_alive():
        handle_response_thread.join(timeout=1)
```

**为什么必须先等工作线程全部退出，才给回调线程发 `None`？**

因为工作线程还在往输出队列写。若先让回调线程退出，剩下的响应就**永远不会被回调处理**——而 `yield` 之后紧接着就是 `_cleanup()`，也就是说**调用方在上下文管理器退出时期望「所有已提交的请求都被处理完」**。

**这个顺序把「优雅关闭」表达清楚了：先停止生产、等生产完成、再停止消费。**

**而每个工作线程收到的 `None` 是逐个投递的**（`for _ in threads`）——若只投一个，只有一个线程收到。

**`get(timeout=1)` 与 `while is_alive(): join(timeout=1)` 让关闭可被打断。** 这两个 1 秒超时不是性能调优，而是**让 `quit_process_event` 有机会被检查**——线程在 `get` 超时后回到 `while` 条件，看到事件已设置就退出。

**`quit_process_event` 只用于 `KeyboardInterrupt`。**

```
try:
    yield _process_input
    _cleanup()
except KeyboardInterrupt:
    quit_process_event.set()
    sys.exit(1)
```

**`_cleanup()` 在 `try` 里而不是 `finally` 里。** 若用户回调在自己的线程里抛异常，那个线程会死掉而主线程不知道——`_cleanup` 里的 `while handle_response_thread.is_alive()` 会立即通过，于是剩余响应留在输出队列里无人消费。**泄漏的是「未处理的响应」，不是线程**（工作线程仍会被 `None` 正常终止）。

**而 `sys.exit(1)` 在 `KeyboardInterrupt` 分支里是重手**——它直接终止进程，而不是让上下文管理器正常退出。对 CLI 合理（Ctrl-C 就该退出），对库调用则是意外的。

**`finally` 里只做一件事：记录整批的运行时长。**

```
finally:
    end_time = time.time()
    runtime = end_time - start_time
    if metrics_store:
        metrics_store.update_metrics(metrics={"runtime_duration_seconds": runtime})
```

**这是 `metrics_store` 被单独传进 runner 而不是从补全实例里取的原因**（见 e01 那页）：它量化的是**这一批**的执行时长，而不是单次调用。

**`_process_output` 支持异步回调。**

```
response = callback(request_id, response)
if asyncio.iscoroutine(response):
    response = asyncio.run(response)
```

**`ThreadedLLMCompletionResponseHandler` 协议声明返回 `Awaitable[None] | None`**——于是回调可以是 `async def`。而这里用 `asyncio.run` 在一个**独立线程**里跑它，那个线程没有正在运行的事件循环，所以合法。

**代价是每个异步回调都新建并销毁一个事件循环。** 若回调里要发起异步 I/O（比如写数据库），每次都是新循环意味着连接池无法跨回调复用。

**`_process_input` 校验 `request_id` 非空。**

```
def _process_input(request_id: str, **kwargs):
    if not request_id:
        msg = "request_id needs to be passed as a keyword argument"
        raise ValueError(msg)
    input_queue.put((request_id, kwargs))
```

**为什么是检查「非空」而不是「是否提供」**：`request_id` 在协议里是关键字参数，不传会得到 `None`（而 `Protocol` 声明为 `str`）。**用真值判断同时覆盖了 `None` 与 `""`。**

**这行也是 `completion_batch` 那套「`request_id` 即数组下标」机制的前提**（见 e01 那页）：`request_id` 必须存在且唯一。

### 二、`CompletionThread`：一个消费者循环

```
def run(self):
    while True and not self._quit_process_event.is_set():
        try:
            input_data = self._input_queue.get(timeout=1)
        except Empty:
            continue
        if input_data is None:
            break
        request_id, data = input_data
        try:
            response = self._completion(**data)
            self._output_queue.put((request_id, response))
        except Exception as e:  # noqa: BLE001
            self._output_queue.put((request_id, e))
```

**最关键的一行是 `except Exception` 里把异常当结果放进输出队列。** 于是**单个请求失败不会杀死工作线程**，也不会让整批失败——异常作为一个「响应」被回调收到（这就是 `completion_batch` 返回 `list[Response | Iterator | Exception]` 的来源）。

**`while True and not ...` 里的 `True and` 是冗余的**（等价于 `while not ...`），看起来是「无限循环 + 条件」的思维残留。无害，但值得记为一个可清理点。

**`Empty` 时 `continue` 会让线程每秒醒一次**——空闲期（比如请求都处理完了、上下文管理器还没退出）所有 N 个工作线程都在 1Hz 空转。**这是用轮询换「能响应 `quit_event`」的代价**，因为 `Queue.get` 无法同时等待事件。

### 三、`MetricsProcessor`：把响应折算成计数

**抽象只有一个方法 `process_metrics`，契约是「原地修改 `metrics` 字典」。**

| 参数 | 用途 |
|---|---|
| `model_config` | 查模型身份（拼 `provider/model`）以查价 |
| `metrics` | 被原地累加的字典 |
| `input_args` | 完整的请求参数 |
| `response` | 补全或嵌入响应，四种类别 |

**`response` 的类型联合有四支**：`LLMCompletionResponse`、`Iterator[LLMCompletionChunk]`、`AsyncIterator[LLMCompletionChunk]`、`LLMEmbeddingResponse`。

**而 `DefaultMetricsProcessor` 只处理其中两支。**

```
if isinstance(response, LLMCompletionResponse):
    self._process_lm_chat_completion(...)
elif isinstance(response, LLMEmbeddingResponse):
    self._process_lm_embedding_response(...)
```

**流式的两个分支被完全跳过**——因为流式的 token 数要等流消费完才知道，而 `with_metrics` 在调用返回时就处理完了（见 e02 那页：流式的 `compute_duration_seconds` 被硬置 0）。**于是流式请求的 token 与费用完全不计入指标。**

**这是一个真实的统计盲区**：若大量使用流式，`total_tokens` 与 `total_cost` 会严重低估，而没有任何机制用 `streaming_responses` 去校正它。

**补全侧的折算：**

```
prompt_tokens = response.usage.prompt_tokens if response.usage else 0
completion_tokens = response.usage.completion_tokens if response.usage else 0
total_tokens = prompt_tokens + completion_tokens

if total_tokens > 0:
    metrics["responses_with_tokens"] = 1
    metrics["prompt_tokens"] = prompt_tokens
    metrics["completion_tokens"] = completion_tokens
    metrics["total_tokens"] = total_tokens

    model_id = f"{model_config.model_provider}/{model_config.model}"
    model_costs = model_cost_registry.get_model_costs(model_id)
    if not model_costs:
        return
    input_cost = prompt_tokens * model_costs["input_cost_per_token"]
    output_cost = completion_tokens * model_costs["output_cost_per_token"]
    total_cost = input_cost + output_cost
    metrics["responses_with_cost"] = 1
    metrics["input_cost"] = input_cost
    metrics["output_cost"] = output_cost
    metrics["total_cost"] = total_cost
```

**五个决定：**

1. **`response.usage` 可能为 `None`**——某些 provider 不返回用量。此时所有计数为 0，而 `if total_tokens > 0` 让整块跳过。
2. **`responses_with_tokens = 1`** 是「这一响应带 token 信息」的标记，不是 token 数本身。累加后它表示「有多少响应提供了用量」——于是分母正确。
3. **查不到价格时 `return`（而不是报错）**——只记 token，不记费用。**这解释了 `responses_with_tokens` 与 `responses_with_cost` 为什么是两个独立计数器：它们的值可能不同。**
4. **`model_id` 是「provider/model」拼的**——与工厂里一致（e01 那页），于是换 provider 或换模型都会查不同价格。
5. **嵌入侧没有 `completion_tokens`**——嵌入只有输入侧，于是 embedding 的 `total_cost = input_cost` 且不写 `completion_tokens` 键。

**嵌入侧少写一个键，而累加器的「有则加、无则赋初值」容忍这一点。** 这就是累加逻辑必须判键存在的原因：**不同类型的请求贡献不同的键集合。**

### 四、`MetricsAggregator`：单例 + 注册的派生函数

**它是单例，用 `ClassVar` 缓存实例。**

```
_instance: ClassVar["Self | None"] = None

def __new__(cls, *args, **kwargs):
    if cls._instance is None:
        cls._instance = super().__new__(cls, *args, **kwargs)
    return cls._instance

def __init__(self):
    if not hasattr(self, "_initialized"):
        self._initialized = True
        self._aggregate_functions = {}
```

**`__init__` 里的 `hasattr` 守卫是必需的**——因为 `__new__` 返回同一实例，而 Python 每次构造都会调 `__init__`。没有守卫，第二次 `MetricsAggregator()` 会清空已注册的函数。

**而 `_initialized` 是一个不在类声明里的动态属性**，于是 `hasattr` 判定依赖运行顺序。这是单例的经典形态，也是它的经典脆弱点。

**六个派生函数在模块底部就被注册了。**

```
metrics_aggregator = MetricsAggregator()
metrics_aggregator.register("failure_rate", _failure_rate)
metrics_aggregator.register("retry_rate", _retry_rate)
metrics_aggregator.register("tokens_per_response", _tokens_per_response)
metrics_aggregator.register("cost_per_response", _cost_per_response)
metrics_aggregator.register("compute_duration_per_response", _compute_duration_per_response)
metrics_aggregator.register("cache_hit_rate", _cache_hit_rate)
```

**「导入这个模块」就等于「注册了这些派生量」**——模块级副作用。而注册表的键（`failure_rate` 等）**就是最终指标字典里的键名**，于是「有哪个派生量」由这个注册表决定。

**六个函数统一形态是 `if 分母 > 0: 算 else: 0.0`。**

```
def _failure_rate(metrics):
    attempted = metrics.get("attempted_request_count", 0)
    failed = metrics.get("failed_response_count", 0)
    if attempted > 0:
        metrics["failure_rate"] = failed / attempted
    else:
        metrics["failure_rate"] = 0.0
```

**于是派生键总是存在，读取方不必判空。** 这与 `NoopMetricsStore` 是同一个思路（用「有意义的零」代替「缺失」）。

**三个公式值得逐个核对：**

**`_retry_rate`：**

```
if attempted > 0 and "retries" in metrics:
    metrics["retry_rate"] = retries / (retries + attempted)
elif "retries" in metrics:
    metrics["retry_rate"] = 0.0
```

**分母是 `retries + attempted`**——即「总物理尝试次数」。于是它的含义是「**所有尝试中有多少比例是重试**」，而不是「多少比例的请求重试过」（那是 `requests_with_retries / attempted`）。

**而 `"retries" in metrics` 这个额外条件是六者中的异类**：若整个批次没有任何重试记录（`with_retries` 未启用），则**不写** `retry_rate` 键，而其余五个函数在分母为 0 时都写 `0.0`。**读取方对这一个键必须判存在性。**

**`_compute_duration_per_response` 的分母有减法：**

```
responses = metrics.get("successful_response_count", 0)
streaming_responses = metrics.get("streaming_responses", 0)
responses = responses - streaming_responses
compute_duration = metrics.get("compute_duration_seconds", 0)
if responses > 0:
    metrics["compute_duration_per_response_seconds"] = compute_duration / responses
```

**分母减掉流式响应数**——因为流式的 `compute_duration_seconds` 被硬置 0（见 e02 那页），若把流式计入分母，平均耗时会被人为压低。**这一步是流式置 0 的配套修正。**

**`_cache_hit_rate` 的分母是 `successful_response_count`。** 语义正确的前提是「缓存命中的请求也被计为一次成功请求」——而这成立，因为 `with_request_count` 在 `with_cache` **外层**（见 e02 那页的顺序），命中的请求同样经过它。**这也印证了 `attempted_request_count` 统计的是「逻辑请求」而非「物理尝试」。**

### 五、`MemoryMetricsStore`：累加、聚合、排序

**`update_metrics` 是纯累加。**

```
def update_metrics(self, *, metrics: "Metrics") -> None:
    with self._thread_lock:
        for name, value in metrics.items():
            if name in self._metrics:
                self._metrics[name] += value
            else:
                self._metrics[name] = value
```

**「首次赋值、之后累加」容忍了不同请求贡献不同键集合**（嵌入没有 `completion_tokens`）。

**而 `get_metrics` 先聚合、再排序。**

```
def get_metrics(self) -> "Metrics":
    metrics_aggregator.aggregate(self._metrics)
    return self._sort_metrics()
```

**聚合发生在读取时且是原地修改。** 反复调用不会重复累积派生量——因为每个聚合函数都是「用原始计数重新算并覆盖写派生键」，而不是「累加派生量」。**派生量是原始量的幂等纯函数，这是整个三段式成立的前提。**

**而排序用一份 23 项固定顺序。**

```
_default_sort_order = [
    "attempted_request_count", "successful_response_count", "failed_response_count",
    "failure_rate", "requests_with_retries", "retries", "retry_rate",
    "compute_duration_seconds", "compute_duration_per_response_seconds",
    "runtime_duration_seconds", "cached_responses", "cache_hit_rate",
    "streaming_responses", "responses_with_tokens", "prompt_tokens",
    "completion_tokens", "total_tokens", "tokens_per_response",
    "responses_with_cost", "input_cost", "output_cost", "total_cost", "cost_per_response",
]
```

**分组是「计数 → 比率 → 耗时 → 缓存 → token → 费用」，且每组内「原始量在派生量之前」。**

**排序函数有 fallback：**

```
sorted_metrics: Metrics = {}
for key in self._sort_order:
    if key in self._metrics:
        sorted_metrics[key] = self._metrics[key]
for key in self._metrics:
    if key not in sorted_metrics:
        sorted_metrics[key] = self._metrics[key]
```

**第一遍按序取已知键，第二遍把未知键追加在后。** 于是第三方注册的派生量不会丢失，只是排在末尾。**排序只影响展示，不影响正确性。**

**而落盘靠 `atexit`。**

```
if metrics_writer:
    self._metrics_writer = metrics_writer
    atexit.register(self._on_exit_)

def _on_exit_(self):
    if self._metrics_writer:
        self._metrics_writer.write_metrics(id=self._id, metrics=self.get_metrics())
```

**「进程退出时写一次」粗糙但有效**：保证长跑批处理的指标最终落盘，代价是崩溃（非正常退出）时全丢。**这与 lightrag 那套「启动校验 + 显式 finalize」形成对照：那边有明确的收尾阶段，这里只有尽力而为的钩子。**

**而 `_thread_lock` 只在 `update_metrics` 里用，`get_metrics` 不加锁**——于是「读取时有别的线程在写」会产生不一致快照。**在 `atexit` 场景下无害（没有别的线程了），在「周期性读指标」的用法下则是数据竞争。**

### 六、`ModelCostRegistry`：把 LiteLLM 的价格表当初始数据

**它把 `litellm.model_cost` 直接赋给自己的字典。**

```
def __init__(self):
    if not hasattr(self, "_initialized"):
        self._model_costs = model_cost
        self._initialized = True
```

**这是引用赋值而不是拷贝**，于是：

1. `register_model_costs` 会**直接修改 LiteLLM 的全局价格表**。
2. 反过来，LiteLLM 自己的更新也会反映进来。

**副作用外溢**：一个「注册自己的模型价格」的操作会改到整个进程里所有读 `litellm.model_cost` 的代码。由于 `model_cost` 本身就是模块级共享，一致性上没有新增问题，但作用域远大于「这个注册表」。

**`ModelCosts` 只有两个必需字段。**

```
class ModelCosts(TypedDict):
    input_cost_per_token: float
    output_cost_per_token: float
```

**而 LiteLLM 的价格条目有更多字段**（`max_tokens`、`litellm_provider` 等）。**`TypedDict` 在运行期不校验，多出的字段被容忍**——于是「用 LiteLLM 原始条目当 `ModelCosts`」类型上不严格、运行上可行。

**`get_model_costs` 返回 `ModelCosts | None`**，而对 `None` 的处理是「记 token、不记费用」。**这是正确的降级**：价格表里没有的模型（自部署、新模型）不该让整个指标失真，只是缺费用那一维。

## 关键取舍

**源码事实：** 线程池是「输入队列（可限长，形成背压）+ N 工作线程 + 输出队列（无上限）+ 一个回调线程」四件套。输出队列不限长是因为限长会让工作线程阻塞在 `put` 上，而唯一消费者是回调线程——吞吐会退化到回调速度。

**源码事实：** `_cleanup` 的顺序是「给每个工作线程投 `None` → join 全部 → 给回调线程投 `None` → join」。必须先等生产者退出，否则回调线程先死会让剩余响应无人处理。

**源码事实：** `get(timeout=1)` 与 `while is_alive(): join(timeout=1)` 的 1 秒超时是为了让 `quit_process_event` 有机会被检查，不是性能调优。

**源码事实：** `_cleanup()` 在 `try` 里而非 `finally` 里；`KeyboardInterrupt` 分支直接 `sys.exit(1)`（对 CLI 合理、对库调用意外）。

**源码事实：** 工作线程把异常当「响应」放进输出队列，于是单请求失败不杀线程、不影响整批；这也是 `completion_batch` 返回类型含 `Exception` 的来源。

**源码事实：** `_process_input` 用真值判断 `request_id`（同时覆盖 `None` 与 `""`），这是 `completion_batch` 用它当数组下标的前提。

**源码事实：** 异步回调在回调线程里用 `asyncio.run` 执行——每次回调新建并销毁一个事件循环，于是回调内无法跨调用复用异步连接池。

**源码事实：** `DefaultMetricsProcessor` 只处理非流式的两支；流式的两支被跳过，于是流式请求的 token 与费用完全不计入指标，且没有代码用 `streaming_responses` 去校正这个偏差。

**源码事实：** `responses_with_tokens` 与 `responses_with_cost` 是独立计数器，因为查不到价格时只记 token 不记费用。

**源码事实：** 嵌入侧无 `completion_tokens`，`total_cost = input_cost`；累加器「有则加、无则赋」容忍不同请求贡献不同键集合。

**源码事实：** `MetricsAggregator` 是单例（`ClassVar` + `__new__` + `__init__` 的 `hasattr` 守卫），六个聚合函数在模块导入时注册——「有哪个派生量」由模块级副作用决定，注册表的键就是输出字典的键。

**源码事实：** 五个聚合函数在分母为 0 时写 `0.0`（派生键总存在），但 `_retry_rate` 在 `"retries" not in metrics` 时**不写**该键——六者不一致，读取方对这一个必须判存在性。

**源码事实：** `_retry_rate` 的分母是 `retries + attempted`（总物理尝试数），语义是「所有尝试中重试的占比」而非「多少比例的请求重试过」。

**源码事实：** `_compute_duration_per_response` 从分母里减掉 `streaming_responses`，因为流式的 `compute_duration_seconds` 被硬置 0，不减会压低平均值。

**源码事实：** `MemoryMetricsStore.update_metrics` 在锁内累加；`get_metrics` 先原地聚合再排序且**不加锁**（周期性读取时会与写入竞争）。`_sort_metrics` 用 23 项固定顺序，未知键追加在后不丢失。

**源码事实：** 指标落盘靠 `atexit`——保证长跑批处理最终写出，但崩溃时全丢。

**源码事实：** `ModelCostRegistry.__init__` 是 `self._model_costs = model_cost` 的引用赋值，于是 `register_model_costs` 会直接修改 LiteLLM 的全局价格表。

**架构推论：** 线程这一半与指标这一半的耦合点只有一处——`runtime_duration_seconds` 由 runner 写入。**其余全部解耦**：工作线程只管调补全、回调只管交接、处理器只管折算、存储只管累加。**「每个环节只做一次格式转换」让每层都能单独替换**，代价是要读完整条链才知道 `metrics` 字典里最终有什么键。

**架构推论：** 「原始量累加、派生量读取时算」是所有指标系统的标准形态，正确性依赖一个前提：**派生量必须是原始量的纯函数**。六个函数都满足（幂等重算）。一旦某个派生量需要「历史状态」（环比、移动平均），这个结构就必须改——因为累加器里没有时间序列。**这也是 `_default_sort_order` 里没有「每秒请求数」这类指标的原因。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-llm/graphrag_llm/threading/completion_thread_runner.py` | `completion_thread_runner` | 四件套装配、`_cleanup` 顺序、运行时长指标 |
| `packages/graphrag-llm/graphrag_llm/threading/completion_thread_runner.py` | `ThreadedLLMCompletionResponseHandler` | 回调协议（可为 async） |
| `packages/graphrag-llm/graphrag_llm/threading/completion_thread.py` | `CompletionThread` | 工作线程，异常当响应入队 |
| `packages/graphrag-llm/graphrag_llm/threading/completion_thread.py` | `LLMCompletionRequestQueue` / `LLMCompletionResponseQueue` | 两个队列的类型别名与 `None` 哨兵说明 |
| `packages/graphrag-llm/graphrag_llm/metrics/metrics_processor.py` | `MetricsProcessor` | 抽象：原地改 `metrics` 的单方法 |
| `packages/graphrag-llm/graphrag_llm/metrics/default_metrics_processor.py` | `DefaultMetricsProcessor` | 非流式的 token/费用折算，查不到价则只记 token |
| `packages/graphrag-llm/graphrag_llm/metrics/metrics_aggregator.py` | `MetricsAggregator` | 单例注册表 + 六个派生量 |
| `packages/graphrag-llm/graphrag_llm/metrics/memory_metrics_store.py` | `MemoryMetricsStore` | 锁内累加、读取时聚合、23 项排序、`atexit` 落盘 |
| `packages/graphrag-llm/graphrag_llm/model_cost_registry/model_cost_registry.py` | `ModelCostRegistry` / `ModelCosts` | 直接引用 `litellm.model_cost` 的价格表 |
| `packages/graphrag-llm/graphrag_llm/config/metrics_config.py` | `MetricsConfig` | 决定指标是否启用与写到哪 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：metrics 目录](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-llm/graphrag_llm/metrics)
- [Python 文档：queue 与 threading](https://docs.python.org/3/library/queue.html)
- [LiteLLM 文档：token 用量与价格](https://docs.litellm.ai/docs/completion/token_usage)

## 与主核 / AI 的关系

线程池这半与主核的**后台 I/O 工作队列**（checkpointer、WAL writer）结构相同：生产队列 + 固定数量的消费者 + 独立的完成通知通道。而关键差别在「完成通知」这一环。

**数据库的工作线程把结果写回共享内存的完成队列，由等待者轮询或由 latch 唤醒。** 这里是**一个专门线程消费输出队列并调用户回调**——把「完成通知」抽成独立组件，好处是回调可以任意慢（不阻塞工作线程），坏处是引入了一个必须被正确关闭的线程。

**「先停生产者、等生产者退出、再停消费者」这个顺序在数据库里是关停协议的标准形态**：先让新请求停止进入，再等活跃事务提交或回滚，最后才关闭后台进程。**顺序反了就会丢工作。** 这里的 `_cleanup` 是同一协议的极简版本。

**「原始量累加、派生量读取时算」与数据库的 `pg_stat_*` 视图完全同构。** `pg_stat_database` 里的 `blks_read` 是累加计数器，而 `blk_read_time / blks_read` 这样的平均量**是在查询视图时现算的**。数据库不预存平均值，因为平均值不可加——**两组计数器相加得到的平均值必须重新除。**

**而 `_retry_rate` 的分母是 `retries + attempted` 这个细节，与数据库的「命中率」定义是同一类问题。** `blks_hit / (blks_hit + blks_read)` 的分母是「总访问次数」而不是「读次数」。**分母选错会让指标失去意义**，而这两个例子都选了「总尝试数」。

对 agent 工程的直接价值有三条。其一，**输出队列不能限长**。它的唯一消费者是回调，限长会让工作线程阻塞在 `put` 上，吞吐塌到回调速度。同理，输入队列**应该**限长——那里阻塞的是调用方，正是背压想要的效果。其二，**派生指标必须能被幂等重算**。累加器只存原始计数，每次读取重算派生量，这样「聚合被调用几次」不影响结果；一旦派生量需要历史状态，这个结构就得换成保留时间序列。其三，**关闭顺序是不可省的正确性**。先让生产者停止、等它们真的退出、再停消费者——顺序反了，队列里尚未被消费的响应就永久丢失。