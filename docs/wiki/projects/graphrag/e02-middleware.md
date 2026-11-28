---
project: graphrag
feature: middleware
title: 中间件管道
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-llm/graphrag_llm/middleware/with_middleware_pipeline.py
    symbols: [with_middleware_pipeline]
  - path: packages/graphrag-llm/graphrag_llm/middleware/with_cache.py
    symbols: [with_cache]
  - path: packages/graphrag-llm/graphrag_llm/middleware/with_request_count.py
    symbols: [with_request_count]
  - path: packages/graphrag-llm/graphrag_llm/middleware/with_metrics.py
    symbols: [with_metrics]
---

# 中间件管道

## 一句话职责

用**七层闭包包装**把限流、重试、缓存、指标、计数、日志叠加在一次模型调用外面——没有中间件类、没有责任链对象，只有一串「返回 `(sync_fn, async_fn)` 元组的高阶函数」，而**代码里的书写顺序决定运行时的内外次序**。

## 机制是什么

**中间件的类型签名只有一种：**

```
middleware(*, sync_middleware: LLMFunction, async_middleware: AsyncLLMFunction, ...额外配置)
    -> tuple[LLMFunction, AsyncLLMFunction]
```

**产物与输入类型相同** —— 于是它可被无限次嵌套，而每次嵌套都把新层放在**外层**：新包装函数先执行自己的逻辑，再调 `sync_middleware(**kwargs)` 往里走。

**而 `with_middleware_pipeline` 的书写顺序与文档列出的顺序是相反的。** 这一点必须先讲清楚，否则会读错整页：

代码里从先到后的包装顺序是：

```
with_errors_for_testing → with_metrics → with_rate_limiting → with_retries → with_cache → with_request_count → with_logging
```

而**最后包的是最外层**，所以运行时的调用顺序是：

```
with_logging → with_request_count → with_cache → with_retries → with_rate_limiting → with_metrics → with_errors_for_testing → 真正的模型调用
```

**请求从 `with_logging` 进入、向内逐层穿透；响应从内核向外逐层返回。** 所以：

- **`with_logging` 在最外层** —— 它看到的是最终成败（含重试耗尽后的失败）。
- **`with_request_count` 第二层** —— 它统计的是「一次逻辑请求」，而不是「每次物理尝试」。
- **`with_cache` 第三层** —— 缓存命中会**跳过**重试、限流、指标三层（因为根本不需要调模型）。
- **`with_retries` 在缓存之内、限流之外** —— 于是每次重试都重新走一遍限流。
- **`with_rate_limiting` 在重试之内** —— 这是文档特别强调的顺序。

## 怎么实现的

### 一、为什么重试必须在限流之外

**文档把这条规则写在 `with_middleware_pipeline` 的注释里，而它是整个管道的核心决策：**

> `with_retries`：重试失败的请求。**因为重试中间件出现在限流之前，所有重试都会重新排队等限流。** 这是为了避免线程快速反打端点，从而增加所需的冷却时间。

**注意「prior to rate limiting」在这里指的是「在管道文档列表里排在限流之前」= 更外层**（因为文档列表是按外层到内层写的，与代码的包装顺序相反——这正是容易读错的地方，注释本身也是按运行时顺序写的）。

**这个顺序的必要性：**

| 若限流在外、重试在内 | 若重试在外、限流在内 |
|---|---|
| 第一次请求通过限流，然后失败 | 第一次请求通过限流，失败 |
| 重试发生在**限流内部**，不重新排队 | 重试需要**重新过限流** |
| 快速重试可能瞬间打满端点的真实配额 | 每次重试都被本地限流器节流 |
| **结果是端点侧被迫提高冷却时间** | 结果是本地就压住了速率 |

**这不是「哪个更礼貌」的偏好，而是「本地限流器的存在意义」**：若重试不重新排队，本地限流器就只限制首轮请求，而重试流量完全绕过它——**那么限流器估算的速率与实际发出的速率不符**，本地限流形同虚设。

### 二、`with_request_count`：最外层里最内的一层业务计数

**它只做三件事：初始化三个计数、成功时置位、失败时置位。**

```
def _request_count_middleware(**kwargs: Any):
    metrics: Metrics | None = kwargs.get("metrics")
    if metrics is not None:
        metrics["attempted_request_count"] = 1
        metrics["successful_response_count"] = 0
        metrics["failed_response_count"] = 0
    try:
        result = sync_middleware(**kwargs)
        if metrics is not None:
            metrics["successful_response_count"] = 1
        return result  # noqa: TRY300
    except Exception:
        if metrics is not None:
            metrics["failed_response_count"] = 1
        raise
```

**三个计数器全部是「每请求的 0/1 标记」而不是累加。** 而在聚合阶段（`MetricsAggregator`，见 e05 那页）它们被 `sum` 起来才是总数。

**为什么不直接累加？** 因为 `metrics` 是一个**每次请求新建的字典**，最后被送进 `MetricsStore.update_metrics` 累加。**若这里就累加，同一个字典被复用时会重复计数。**

**`noqa: TRY300` 那条压制的是 "consider moving this statement to an else block"。** 也就是 lint 建议把 `return result` 移进 `else:`——但那样要在 `try` 外面再判断一次，而当前写法更直白。

**而 `attempted_request_count = 1` 恒为 1 这一行同时充当「初始化」** —— 若 `metrics` 是一个空 dict，这三行建立了后面所有处理器依赖的字段。**于是下游读取时可以直接 `.get(k, 0)` 而不必担心键不存在。**

### 三、`with_metrics`：指标只在有 `metrics` 字典时才收集

**核心是一个条件式的后置处理。**

```
def _metrics_middleware(**kwargs: Any):
    metrics: Metrics | None = kwargs.get("metrics")
    start_time = time.time()
    response = sync_middleware(**kwargs)
    end_time = time.time()

    if metrics is not None:
        metrics_processor.process_metrics(
            model_config=model_config, metrics=metrics, input_args=kwargs, response=response,
        )
        if kwargs.get("stream"):
            metrics["compute_duration_seconds"] = 0
            metrics["streaming_responses"] = 1
        else:
            metrics["compute_duration_seconds"] = end_time - start_time
            metrics["streaming_responses"] = 0
    return response
```

**四个要点：**

1. **`start_time` / `end_time` 即使在 `metrics` 为 `None` 时也会取** —— 这是一个可优化点（若 `metrics is None` 就完全不必取时间），但保留它让代码结构对称。**代价是每次调用两次 `time.time()`**。
2. **`process_metrics` 接收 `input_args=kwargs`** —— 完整的请求参数。于是处理器能算「这次请求用了多少 prompt token」。
3. **流式响应的 `compute_duration_seconds` 被硬置 0**，而 `streaming_responses` 置 1。**因为流式的「完成时刻」不是这次调用返回的时刻**（迭代器返回了但内容还没生成完）。
4. **`streaming_responses` 这个标记让聚合阶段能算「非流式的平均耗时」** —— 见 e05 那页的 `_compute_duration_per_response` 会先减掉流式响应数。

**流式情况下置 0 而不是「不设」是一个有意的选择**：置 0 让「字段存在但无效」与「字段不存在」区分开，而聚合时的减法依赖 `streaming_responses` 的存在。

**同步与异步两份实现的唯一差别是 `await`。** 这是贯穿全层的模式——而它带来一个必须接受的成本：**任何一处逻辑改动都要改两遍**。

### 四、`with_cache`：同步版本要自己造事件循环

**这是全层最不寻常的一段。缓存接口是异步的（`async def get`），而同步中间件必须调它。**

```
def _cache_middleware(**kwargs: Any):
    is_streaming = kwargs.get("stream") or False
    is_mocked = kwargs.get("mock_response") or False
    metrics: Metrics | None = kwargs.get("metrics")

    if is_streaming or is_mocked:
        return sync_middleware(**kwargs)

    cache_key = cache_key_creator(kwargs)

    event_loop = asyncio.new_event_loop()
    try:
        cached_response = event_loop.run_until_complete(cache.get(cache_key))
        ...
        response = sync_middleware(**kwargs)
        cache_value = {"response": response.model_dump(), "metrics": metrics if metrics is not None else {}}
        event_loop.run_until_complete(cache.set(cache_key, cache_value))
        return response
    finally:
        event_loop.close()
```

**四个决定：**

**第一，流式与模拟响应直接跳过缓存。**

```
if is_streaming or is_mocked:
    # don't cache streaming or mocked responses
    return sync_middleware(**kwargs)
```

- **流式不能缓存**：缓存下来的是一个迭代器，而迭代器只能消费一次。再次命中会得到一个已耗尽的生成器。
- **`mock_response` 不能缓存**：它是测试注入的假响应，缓存它会让后续的真实调用命中假数据。

**第二条注释用 `don't cache streaming or mocked responses` 一句话说明了两个理由的共性：这两者都不可重复消费/不代表真实结果。**

**第二，`asyncio.new_event_loop()` 每次调用新建一个循环。**

**这在一个本身运行在事件循环里的进程中是危险的** —— 而这段代码是**同步路径**，所以它必然不在事件循环中运行（在事件循环里调用同步版本的 `completion` 会阻塞整个循环）。**新建循环是这里唯一可行的做法**（`asyncio.run` 也新建，但不便于复用同一个循环做 `get` 与 `set`）。

**第三，`cache_key_creator(kwargs)` 接收的是完整 kwargs。**

**于是缓存键自动包含了所有调用参数** —— 包括 `messages`、`temperature`、`max_completion_tokens` 等。而 `create_cache_key` 会剔除一些字段（见 e04 那页）。

**第四，缓存的读取校验有多层。**

```
if (cached_response is not None
    and isinstance(cached_response, dict)
    and "response" in cached_response
    and cached_response["response"] is not None
    and isinstance(cached_response["response"], dict)):
    try:
        ...
        if request_type == "chat":
            return LLMCompletionResponse(**cached_response["response"])
        return LLMEmbeddingResponse(**cached_response["response"])
    except Exception:  # noqa: BLE001
        # Try to retrieve value from cache but if it fails, continue to make the request.
        ...
```

**五层条件 + 一个裸 `except`。** 而 `except` 里的注释说明取向：「尝试从缓存取，若失败就继续发请求」。**缓存绝不能成为失败的来源**——它只是加速，任何解析失败都退化为「未命中」。

**而 `metrics` 也被缓存了。**

```
if (metrics is not None and "metrics" in cached_response
    and cached_response["metrics"] is not None
    and isinstance(cached_response["metrics"], dict)):
    metrics.update(cached_response["metrics"])
    metrics["cached_responses"] = 1
```

**命中时把当时的指标复制进当前这次请求的指标**，并标 `cached_responses = 1`。这解释了 `_CACHE_VERSION` 从 3 升到 4 的一个原因（见 e04 那页的注释）：「graphrag-llm 现在支持指标，也缓存不再被缓存的指标」。

**这意味着缓存里的指标是「第一次调用时」的耗时与费用** —— 而聚合时会把它算进总分。**这是一个微妙的统计问题**：缓存命中的请求会计入「总花费」但实际没花钱。而 `cached_responses` 标记让 `_cache_hit_rate` 能算出命中率，从而把成本分析拆开。

**`request_type` 参数决定反序列化成哪种响应。** 这是「补全与嵌入共用同一套中间件」的直接体现：管道本身不知道自己在包什么，所以要把类型作为参数传进来。

### 五、其余四层：极薄

**`with_rate_limiting` 估算 token 数并包一个 `with`。**

```
token_count = int(kwargs.get("max_tokens") or kwargs.get("max_completion_tokens") or 0)
messages = kwargs.get("messages")  # completion call
input: list[str] | None = kwargs.get("input")  # embedding call
if messages:
    token_count += tokenizer.num_prompt_tokens(messages=messages)
elif input:
    token_count += sum(tokenizer.num_tokens(text) for text in input)

with rate_limiter.acquire(token_count):
    return sync_middleware(**kwargs)
```

**它用 `messages` 与 `input` 两个键名区分补全与嵌入** —— 因为这是两种调用各自的必填参数。

**而 token 估算 = 输出上限 + 输入实际量。** `max_completion_tokens` 是**预留**（最坏情况），而输入是**真实计数**。**这是一个保守估算**：实际输出通常小于上限，所以限流器按「最坏情况」记账。

**`with_retries` 只有两行。**

```
def _retry_middleware(**kwargs):
    return retrier.retry(func=sync_middleware, input_args=kwargs)
```

**它把整个「怎么重试」交给 `retrier` 对象**（见 e03 那页）。中间件只负责「把内层函数交给重试器调用」。

**`with_logging` 只在内层抛异常时记日志。**

```
try:
    return sync_middleware(**kwargs)
except Exception as e:
    retries = metrics.get("retries", None) if metrics else None
    retry_str = f" after {retries} retries" if retries else ""
    logger.exception(f"Request failed{retry_str} with exception={e}")
    raise
```

**它读 `metrics["retries"]` 来丰富消息** —— 这暴露了中间件之间通过 `metrics` 字典通信的事实（`with_retries` 写在里面，`with_logging` 读它）。**而由于 `with_logging` 在最外层，此时 `retries` 已经是最终值。**

**`with_errors_for_testing` 是唯一默认不启用的层。**

```
extra_config = model_config.model_extra or {}
failure_rate_for_testing = extra_config.get("failure_rate_for_testing", 0.0)

if failure_rate_for_testing > 0.0:
    model_fn, async_model_fn = with_errors_for_testing(...)
```

**它从 `model_extra` 读而不是从 `ModelConfig` 的具名字段读** —— 而文档明确写「`failure_rate_for_testing` 不是暴露的配置选项，只用于内部测试」。**用 `model_extra` 这个「杂项字典」承载它，是在类型层面表达「这不是正式配置」。**

**而它在最内层（除模型调用外）** —— 所以抛出的错误会被重试、限流、缓存、指标、计数、日志全部看到。**这正是测试这些层次所需要的注入点。**

### 六、`model_fn` 与 `async_model_fn` 始终成对

**每个中间件都返回元组，每个中间件都接受两个函数。** 于是管道里没有任何「同步用这个、异步用那个」的条件分支——**两条链是彻底平行的**。

**这个对称性带来一个重要后果：`with_cache` 的同步版本与异步版本可以有不同的实现策略**（一个造事件循环、一个直接 `await`），而对外接口完全一致。**若用「一个函数内部判断是否 async」的写法，这种差异化实现就不可能。**

## 关键取舍

**源码事实：** 中间件的类型签名统一为 `(*, sync_middleware, async_middleware, ...) -> tuple[LLMFunction, AsyncLLMFunction]`，产物与输入同型，可无限嵌套且新层在外。

**源码事实：** 代码里的包装顺序与运行时顺序相反。运行时（外→内）为 `with_logging → with_request_count → with_cache → with_retries → with_rate_limiting → with_metrics → with_errors_for_testing → 模型调用`。

**源码事实：** 重试在限流之外（更外层）是刻意的，注释理由：「所有重试都会重新排队等限流，这是为了避免线程快速反打端点，从而增加所需的冷却时间」。

**源码事实：** `with_cache` 在重试之外，于是**缓存命中会跳过重试、限流、指标三层**——根本不需要调模型。

**源码事实：** `with_request_count` 在最外层业务层，于是它统计的是「一次逻辑请求」而非「每次物理尝试」；三个计数器是每请求的 0/1 标记，聚合时才求和。

**源码事实：** `with_metrics` 的 `start_time` 在 `metrics is None` 时也会取（可优化但保留对称）；流式响应的 `compute_duration_seconds` 硬置 0 且 `streaming_responses` 置 1，因为流式的完成时刻不是调用返回的时刻。

**源码事实：** `with_cache` 跳过流式（缓存下来的是只能消费一次的迭代器）与 `mock_response`（测试注入的假响应被缓存会让后续真实调用命中假数据）。

**源码事实：** 同步缓存中间件每次调用 `asyncio.new_event_loop()`；这是可行的因为同步路径必然不在事件循环中运行。

**源码事实：** 缓存读取有五层条件校验 + 裸 `except`，注释明写「若失败就继续发请求」——缓存绝不能成为失败的来源。

**源码事实：** 缓存里同时存了 `{"response": ..., "metrics": ...}`，命中时把当时的指标复制进当前请求并标 `cached_responses = 1`。于是缓存命中会计入总花费统计（虽然实际没花钱），靠 `cached_responses` 拆开。

**源码事实：** `with_rate_limiting` 的 token 估算 = `max_completion_tokens`（预留的最坏情况）+ 输入的真实计数；用 `messages` 与 `input` 两个键名区分补全与嵌入。

**源码事实：** `with_logging` 读 `metrics["retries"]` 来丰富错误消息——中间件之间通过 `metrics` 字典通信，而它在最外层所以读到的是最终值。

**源码事实：** `with_errors_for_testing` 从 `model_config.model_extra` 取 `failure_rate_for_testing`（而非具名字段），因为在类型层面表达「这不是正式配置」；它在最内层，于是注入的错误会被所有外层看到。

**源码事实：** 每个中间件都同时返回同步与异步两个函数，管道里没有「按是否 async 分支」的代码——两条链彻底平行，于是允许两者有不同实现策略（如缓存那份）。

**架构推论：** 用**闭包组合**而不是责任链对象，换来的是「零样板代码」：新增一层只需写一个返回元组的函数，不需要类、注册表、`next` 指针。代价是**层序隐藏在代码的书写顺序里**——没有一处显式的数组声明「这些层的顺序是什么」。文档注释承担了这个职责，而它是这个设计里最脆弱的部分：**调换两个 `if` 块的顺序就能改变全部语义，而类型检查不会提示。**

**架构推论：** 中间件之间通过 `kwargs["metrics"]` 这个共享字典通信，是一个**隐式的、无类型的通道**。`with_retries` 往里写 `retries`、`with_request_count` 写三个计数、`with_cache` 写 `cached_responses`、`with_metrics` 写 token 与耗时、`with_logging` 读 `retries`。**这条通道让层与层耦合但不需要互相引用** —— 代价是「谁写了什么」只能靠读全部代码才知道，且字典键没有拼写检查。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-llm/graphrag_llm/middleware/with_middleware_pipeline.py` | `with_middleware_pipeline` | 七层装配与顺序文档 |
| `packages/graphrag-llm/graphrag_llm/middleware/with_cache.py` | `with_cache` | 缓存层，同步版自造事件循环 |
| `packages/graphrag-llm/graphrag_llm/middleware/with_metrics.py` | `with_metrics` | 计时与指标处理，流式置 0 |
| `packages/graphrag-llm/graphrag_llm/middleware/with_rate_limiting.py` | `with_rate_limiting` | token 估算 + `with acquire()` |
| `packages/graphrag-llm/graphrag_llm/middleware/with_retries.py` | `with_retries` | 把内层交给 `retrier.retry` |
| `packages/graphrag-llm/graphrag_llm/middleware/with_request_count.py` | `with_request_count` | 三个 0/1 计数标记（非累加） |
| `packages/graphrag-llm/graphrag_llm/middleware/with_logging.py` | `with_logging` | 最外层，读 `metrics["retries"]` 丰富消息 |
| `packages/graphrag-llm/graphrag_llm/middleware/with_errors_for_testing.py` | `with_errors_for_testing` | 最内层故障注入，从 `model_extra` 取配置 |
| `packages/graphrag-llm/graphrag_llm/config/metrics_config.py` | `MetricsConfig` | 决定 `with_metrics` 是否启用 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [graphrag 源码：middleware 目录](https://github.com/microsoft/graphrag/tree/main/packages/graphrag-llm/graphrag_llm/middleware)
- [LiteLLM 文档：重试与限流](https://docs.litellm.ai/docs/completion/reliable_completions)
- [Python 文档：contextlib 与闭包](https://docs.python.org/3/library/contextlib.html)

## 与主核 / AI 的关系

这套中间件与主核的**WAL 写路径**在结构上相反、在研究的问题上相同：「一次操作外面该包几层、以什么顺序包」。

**数据库不允许用中间件包装写操作**，因为每一层（缓冲、日志、锁）都必须在引擎内部紧密耦合——日志必须先落盘再改页，锁必须按固定顺序获取。**这些不是「可叠加的增强」而是「不可分割的协议」。**

而限流、重试、缓存是**可分离的增强**，判据是「包装后语义不变」：缓存命中返回等价结果、重试返回某次成功的结果、限流只延迟。**一旦某个关注点改变语义（如超时：超时后不能保证操作没发生），它就不能中间件化，只能进核心。**

**「重试必须在限流之外」这个问题在数据库里有精确对应：连接池的排队位置。** 一个重试查询是否要重新排队等连接？答案与这里一样——必须。否则重试绕过了连接池的上限，实际并发超过配置值。**这是「资源计量点必须覆盖全部流量」的通用要求**。

**层序靠代码顺序表达这个弱点，在数据库的日志与锁顺序里也同样存在。** 数据库用「锁层级编号」把顺序变成可校验的（加锁时检查 `lock_id` 是否递增）；这里没有等价机制。**能校验的顺序是可靠的顺序，只能靠注释维护的顺序迟早会被改错。**

对 agent 工程的直接价值有三条。其一，**缓存要放在重试与限流之外**。命中缓存时不该付限流的代价——它没打端点。其二，**重试必须在限流之内**（即在限流包着的范围外面），否则重试流量绕过本地的速率计量，本地限流器就失去了意义。其三，**中间件之间用共享字典通信是一把双刃剑**。它让层与层解耦，但没有类型检查、没有文档、没有一个地方能查出「有哪些指标键」——如果层数继续增加，这个通道会变成最难维护的部分。