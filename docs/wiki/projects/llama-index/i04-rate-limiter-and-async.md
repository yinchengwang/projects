---
project: llama-index
feature: rate-limiter-and-async
title: 速率限制与异步工具
page_type: feature
schema_version: 1
refs:
  - path: llama-index-core/llama_index/core/rate_limiter.py
    symbols: [BaseRateLimiter, TokenBucketRateLimiter, SlidingWindowRateLimiter]
  - path: llama-index-core/llama_index/core/async_utils.py
    symbols: [run_async_tasks, batch_gather, run_jobs]
  - path: llama-index-core/llama_index/core/utils.py
    symbols: [retry_on_exceptions_with_backoff, get_tokenizer, truncate_text]
---

# 速率限制与异步工具

## 一句话职责

给「打后端 API」这件事准备三件基础设施：一个**能允许多少突发**的限流器（令牌桶与滑动窗口两种策略，前者允许突发、后者严格均摊），一套**能处理已在事件循环中调用**的异步执行工具（含并发上限与分批），以及一组重试与文本工具的细节——其中 `concat_dirs` 和 `get_tokenizer` 各藏着一个跨平台/跨版本的坑。

## 机制是什么

`BaseRateLimiter(ABC)` 只有两个抽象方法：`acquire(num_tokens=0)` 与 `async_acquire(num_tokens=0)`。docstring 说明它的存在理由是「可以在不改 `BaseLLM` / `BaseEmbedding` 调用代码的情况下换入替代策略（例如通过 Redis 的分布式限流）」。

`TokenBucketRateLimiter(BaseRateLimiter, BaseModel)` 两个公开字段 `requests_per_minute` 与 `tokens_per_minute`（都 `Optional`，都 `gt=0`），加**八个** `PrivateAttr`：三个请求侧（`_request_tokens` / `_request_max_tokens` / `_request_refill_rate`）、三个令牌侧（`_token_tokens` / `_token_max_tokens` / `_token_refill_rate`）、`_last_refill_time`、`_lock: threading.Lock`。

`SlidingWindowRateLimiter` 有 `_request_timestamps: Deque[float]` 与 `_token_usage: Deque[Tuple[float, int]]` 两条时间队列，窗口常量 `_SLIDING_WINDOW_SECONDS = 60.0`。

`async_utils.py` 提供 `run_async_tasks`（列表协程 → 结果列表）、`batch_gather`（分批 gather）、`run_jobs`（带信号量并发上限）。另有 `asyncio_run` ——那个「怎么在已有事件循环里跑协程」的适配器。

`utils.py` 里三个：`retry_on_exceptions_with_backoff`（同步重试，指数退避）、`get_tokenizer`（tiktoken 封装加缓存目录处理）、`truncate_text`（带省略号的长度截断）。

## 怎么实现的

**令牌桶与滑动窗口的差别，一句话说完：桶允许突发，窗口严格均摊。**

令牌桶的模型是「容量 + 补充速率」：`_request_tokens` 是**当前存量**，`_request_max_tokens` 是**容量上限**，`_request_refill_rate` 是**每秒补充量**。初始化时 `_request_tokens = requests_per_minute`（**装满**），所以一启动就能连发 `requests_per_minute` 个请求——这就是「初始突发」。

补充分两步（`acquire` 里）：先 `_refill_request_tokens(now)` 按经过时间加回，再判「够不够扣一次」。所以一个闲置 10 分钟的限流器会先补满到容量，然后你可以瞬间用掉全部——**补充不超过容量**是令牌桶与「累计额度」的关键区别。

滑动窗口的模型是「60 秒内最多 N 次」：`_prune_request_timestamps(now)` 把窗口外的旧时间戳弹掉，然后判 `len(self._request_timestamps) < self.requests_per_minute`。所以它**不允许突发**——不管闲置多久，任何 60 秒窗口内的请求数都严格受限。

两者的实际差别可以这样描述：连续调用 10 次（每分钟限 5 次）时，令牌桶在**最初**就让你过一次 5 个，之后每 12 秒一个；滑动窗口则是**立即通过 5 个**，第 6 个要等满 60 秒（因为前 5 个都还在窗口里）。所以令牌桶的等待更均匀，滑动窗口更「攒批」。

而这两者在这个实现里还有一个更细的差别：**滑动窗口对 token 用量也按时间蓄积**（`_token_usage: Deque[Tuple[float, int]]`），所以「这一分钟用了多少 token」是精确的滚动和；令牌桶则是「扣存量 + 按时间补充」，token 用量是**被平滑掉的**。

**令牌桶的两把「锁」是分开的，且同步与异步共用一个。** 实现在 `acquire` 里：

```python
while True:
    now = time.monotonic()
    with self._lock:
        self._refill_request_tokens(now)
        self._refill_token_tokens(now)
        wait = self._wait_for_capacity(now, num_tokens)
        if wait <= 0:
            self._consume_request(num_tokens)
            return
    time.sleep(wait)
```

`self._lock` 是 `threading.Lock`（**不是** `asyncio.Lock`），所以同一份实例可以同时被同步线程与异步协程使用。而 `time.sleep(wait)` 在同步版里阻塞，`await asyncio.sleep(wait)` 在异步版里让出——两版唯一的差别就是这一行与 `with` 那一块。

`_lock` 用 `PrivateAttr(default_factory=threading.Lock)` ——注意 `default_factory` 而不是 `default=threading.Lock()`。后者会让所有实例共享同一把锁（在 Pydantic 里 `default` 是求值一次）。这是一个容易写错的地方。

`time.monotonic()` 而不是 `time.time()` ——**时钟回拨不会导致额度倒退**。这是限流器的正确性前提：若用挂钟，一次 NTP 校正就可能让「已经用掉的时间」变负。

**`SlidingWindowRateLimiter` 的 `_wait_time` 是「算到最早一个重新可用的时刻」。** 它需要回答「还要等多久」，而这需要看队列头部——最老的那个请求出窗口之后就有配额了。所以等待时长 = `queue[0] + 60 - now`。这是滑动窗口与令牌桶在实现上的根本差异：前者要持有**每个请求的时间点**，后者只需要一个存量数字。

所以滑动窗口的**内存随请求数增长**，而令牌桶是 O(1)。对一个限流器来说这不是大问题（60 秒窗口内的请求数有限），但它是两者在资源上的真实差别。

**`RateLimiter = TokenBucketRateLimiter` 是兼容别名。** 文件末尾一行，注释「向后兼容的别名」。所以旧的 `from ... import RateLimiter` 仍然拿到令牌桶实现。

**`run_jobs` 的并发上限靠信号量，且 worker 被单独包了追踪。** 实现：

```python
semaphore = asyncio.Semaphore(workers)

@dispatcher.span
async def worker(job: Coroutine) -> Any:
    async with semaphore:
        return await job

pool_jobs = [worker(job) for job in jobs]

if show_progress:
    from tqdm.asyncio import tqdm_asyncio
    results = await tqdm_asyncio.gather(*pool_jobs, desc=desc)
else:
    results = await asyncio.gather(*pool_jobs)
return results
```

三点值得记：

1. **信号量包裹的是「等待 + 执行」整段**，所以「同时进行的 job 数」被真正限制在 `workers`。若只包裹 `await job` 的启动部分（例如先创建所有 task 再限流），并发数会瞬间冲高。
2. **`pool_jobs` 是一次性把全部 `worker(job)` 建出来的** —— 所以内存里会有 N 个协程对象（N 是任务总数），只是**同时执行**的受限于 `workers`。对「几万个任务」的场景，这个模式不会撑爆内存（协程很轻），但会一次性持有全部。
3. **`@dispatcher.span` 同时挂在 `run_jobs` 与内部的 `worker`** ——所以追踪里能看到「一次批量任务」以及其中「每个单独任务」两层 span。

`DEFAULT_NUM_WORKERS = 4` ——一个保守的默认值。

**`run_async_tasks` 有一个「进度条路径」直接绕过 `asyncio_run`。** 有 `show_progress` 时：

```python
import nest_asyncio
from tqdm.asyncio import tqdm
nest_asyncio.apply()
loop = asyncio.get_event_loop()

async def _tqdm_gather() -> List[Any]:
    return await tqdm.gather(*tasks_to_execute, desc=progress_bar_desc)

tqdm_outputs = loop.run_until_complete(_tqdm_gather())
return tqdm_outputs
```

而这段整个包在 `try/except Exception: pass` 里——**任何异常都静默降级到无进度路径**。注释解释：「在碰到致命错误时以无 tqdm 的方式跑；在某些 tqdm.asyncio 不受支持的环境里可能发生」。

`nest_asyncio.apply()` 是全局副作用（它 patch 掉事件循环以允许重入）。所以「开进度条」这件事在这一层顺手改了整个进程的 asyncio 行为——这是一个值得注意的副作用范围。

**`asyncio_run` 是这一页最实用的一个函数，它解决「已经在事件循环里怎么跑协程」。** 三个分支：

1. 有事件循环且**在运行** → 在**新线程**里新建一个循环跑。
2. 有事件循环但**没在跑** → `loop.run_until_complete(coro)`。
3. `RuntimeError`（拿不到循环，例如不在主线程）→ `asyncio.run(coro)`，再失败就抛一条指引性错误。

第 1 分支的实现里有一句关键的注释与一行代码：

```python
# Snapshot the current context so we can propagate contextvars
ctx = contextvars.copy_context()

def run_coro_in_thread() -> Any:
    new_loop = asyncio.new_event_loop()
    asyncio.set_event_loop(new_loop)
    try:
        return ctx.run(new_loop.run_until_complete, coro)
    finally:
        new_loop.close()

with concurrent.futures.ThreadPoolExecutor(max_workers=1) as executor:
    future = executor.submit(run_coro_in_thread)
    return future.result()
```

`contextvars.copy_context()` + `ctx.run(...)` 是必需的：`ThreadPoolExecutor` 的线程**不继承**调用方的 contextvars，所以「当前的回调 manager、追踪 id、日志上下文」会全部丢失。用 `copy_context` 抓一份再在新线程里 `run`，把这些上下文带过去。这是一个很容易漏掉的正确性细节。

第 3 分支的错误消息是本页最该记的一条：「Detected nested async. Please use nest_asyncio.apply() to allow nested event loops. Or, use async entry methods like `aquery()`, `aretriever`, `achat`, etc.」

它给出了两条具体出路：装 `nest_asyncio`，或者改用 `a*` 异步入口。后者是更正确的做法（避免嵌套），而前者是打补丁。

**`batch_gather` 用 `zip_longest` 造出定长批次。** `chunks` 的实现：

```python
args = [iter(iterable)] * size
return zip_longest(*args, fillvalue=None)
```

这是「同一迭代器重复 N 次」的经典技巧：`zip` 每次从每个参数取一个，而所有参数都是同一迭代器，所以一次取 N 个。`zip_longest` + `fillvalue=None` 让最后一批不满时补 `None`。

所以 `batch_gather` 里必须过滤掉那些 `None`：

```python
for task_chunk in chunks(tasks, batch_size):
    task_chunk = (task for task in task_chunk if task is not None)
    output_chunk = await asyncio.gather(*task_chunk)
```

**这些 `None` 是一个真实的风险**：若调用方传的 `tasks` 里本身含 `None`（而不是「类型上都是协程」），它会被当成填充值过滤掉。所以这个接口的隐含契约是「任务列表里不能有 `None`」。

`verbose` 参数用 `print` 而不是日志——`print(f"Completed {len(output)} out of {len(tasks)} tasks")`。这是一处小瑕疵（库代码用 print），但它是显式的进度输出，行为可见。

**`retry_on_exceptions_with_backoff` 的错误匹配方式有一个瑕疵。** 实现：

```python
error_checks = {error_to_retry.exception_cls: error_to_retry.check_fn for error_to_retry in errors_to_retry}
exception_class_tuples = tuple(error_checks.keys())

while True:
    try:
        return lambda_fn()
    except exception_class_tuples as e:
        traceback.print_exc()
        tries += 1
        if tries >= max_tries: raise
        check_fn = error_checks.get(e.__class__)
        if check_fn and not check_fn(e): raise
        time.sleep(backoff_secs)
        backoff_secs = min(backoff_secs * 2, max_backoff_secs)
```

三处值得看：

1. **`except exception_class_tuples`** ——把一张字典的键当异常元组用。所以「会重试哪类异常」由字典键决定。
2. **`error_checks.get(e.__class__)`** ——按**精确类**查找，不做 MRO 遍历。所以若你注册 `Exception` 而实际抛出 `ValueError`，`except exception_class_tuples` 会**捕获到**（因为 `ValueError` 是 `Exception` 子类），但 `error_checks.get(ValueError)` 拿到 `None`，于是 **`check_fn` 被跳过**，直接重试。这是一个**匹配与查找不对称**的 bug：捕获用继承关系，回调查找用精确类。
3. **`traceback.print_exc()` 在每次重试前打印** ——所以「重试成功的请求」也会在日志里留下完整堆栈。对「会重试几十次」的限流场景，这会把日志刷满。

`tries >= max_tries` 的语义是「包括第一次」（docstring 明写「Maximum number of tries, including the first」）。所以 `max_tries=10` 意味着「1 次初始 + 9 次重试」。

退避是 `min(backoff_secs * 2, max_backoff_secs)` ——纯指数加封顶，**没有随机抖动**。缺少抖动意味着多个进程同时被限流时会**同步重试**（惊群），这在生产里是真实的放大效应。

**`get_tokenizer` 里有一段「临时改环境变量再撤销」的巧思。**

```python
should_revert = False
if "TIKTOKEN_CACHE_DIR" not in os.environ:
    should_revert = True
    os.environ["TIKTOKEN_CACHE_DIR"] = os.path.join(os.path.dirname(os.path.abspath(__file__)), "_static/tiktoken_cache")

enc = tiktoken.encoding_for_model(model_name)
tokenizer = partial(enc.encode, allowed_special="all")
set_global_tokenizer(tokenizer)

if should_revert:
    del os.environ["TIKTOKEN_CACHE_DIR"]
```

所以它只在「用户没设过」时设一个内置缓存目录，用完就删——**尊重用户已有的配置**。若不这样做，用户设的缓存路径会被覆盖，或者若不设，`tiktoken` 会尝试下载模型文件到网络位置（在离线环境里会失败）。

注意 `if should_revert` 的判定是在**设之前**取的，所以「用户本来设了空字符串」这个边缘情况会被当成「已设」（`"X" in os.environ` 对空值也返回真）。

`allowed_special="all"` 让 tiktoken 不把 `<|endoftext|>` 这类特殊 token 当错误抛出——它们出现在文本里时应该被正常编码。

三段式：先查全局缓存 → 没有就导入 tiktoken（失败时报「请 pip install tiktoken」）→ 用它建编码器 → 写进全局 → 返回 `llama_index.core.global_tokenizer` 且带 `assert ... is not None`。

那个 `assert` 的作用是类型缩窄（让后面的返回类型确定非 `None`），而不是运行时校验——它前面刚 `set_global_tokenizer` 过。

**`truncate_text` 有一个负预算分支。** 实现：

```python
if len(text) <= max_length: return text
if max_length - 3 < 0: return text[:max_length]
return text[: max_length - 3] + "..."
```

所以 `max_length < 3` 时不做省略号（因为放不下），直接硬截。`max_length=2` 给 `text[:2]`，`max_length=0` 给空串。

注意最后一行用的是 `text[: max_length - 3] + "..."` ——**总长度恰为 `max_length`**。所以这个函数保证「要么原样返回，要么长度等于 `max_length`」，不会出现「截短了但长度不等于目标」。

**`concat_dirs` 是那个跨平台坑的补丁。** 实现与 docstring：

```python
dirname += "/" if dirname[-1] != "/" else ""
return os.path.join(dirname, basename)
```

docstring 说「把 basename 追加到 dirname，避免在 Windows 上出现反斜杠。`os.path.join(dirname, basename)` 会在 basename 不以斜杠结尾时在 dirname 前加一个反斜杠，所以我们先确保它有」。

原文的措辞有点绕，但意图是：`os.path.join` 在 Windows 上产生 `\`，而 fsspec 的远端路径需要 `/`。先在 `dirname` 末尾保证有 `/`，`os.path.join` 就不会插 `\`。

这是 h01（存储上下文）里 `persist` 那条「`fs is not None` 时用 `concat_dirs`」的实现细节——所以这一页与 H 组的那条分支有直接关系。

## 关键取舍

**源码事实：** 令牌桶初始化时**装满**（`_request_tokens = requests_per_minute`），所以允许初始突发；滑动窗口不允许突发。

**源码事实：** 令牌桶状态是 O(1)（三个数字），滑动窗口持有每个请求的时间戳（内存随请求数增长）。

**源码事实：** `_lock` 用 `threading.Lock` 的 `default_factory`，同步版 `time.sleep` 与异步版 `asyncio.sleep` 是两版唯一差别。

**源码事实：** 全部用 `time.monotonic()`，挂钟回拨不影响额度。

**源码事实：** `run_jobs` 用信号量包裹「等待 + 执行」整段，但一次性建出全部协程；`run_jobs` 与 `worker` 双层 `@dispatcher.span`。

**源码事实：** `run_async_tasks` 的进度条路径调 `nest_asyncio.apply()`（全局 patch）且整段包在裸 `except Exception: pass`。

**源码事实：** `asyncio_run` 的三分支里，第一支用 `contextvars.copy_context()` 把上下文带进新线程。

**源码事实：** `batch_gather` 用 `zip_longest` + `fillvalue=None` 造批次，所以任务列表里若有 `None` 会被误过滤。

**源码事实：** `retry_on_exceptions_with_backoff` 捕获用异常元组（走 MRO），但 `check_fn` 查找用 `error_checks.get(e.__class__)`（精确类），两者不对称。

**源码事实：** 每次重试前都 `traceback.print_exc()`；退避无随机抖动。

**源码事实：** `get_tokenizer` 只在环境变量未设时临时设内置缓存目录，用完撤销；`allowed_special="all"`。

**源码事实：** `truncate_text` 在 `max_length - 3 < 0` 时硬截不加省略号；正常路径总长度恰为 `max_length`。

**源码事实：** `concat_dirs` 先补 `/` 再用 `os.path.join`，避免 Windows 反斜杠污染 fsspec 路径。

**架构推论：** 「两种限流策略并存」不是冗余，而是对应两类不同的上游条款。多数 LLM provider 的限流是「每分钟请求数 + 每分钟 token 数」，且允许短时突发（它们是令牌桶）；但有些场景（例如按秒计费或写入配额）严格要求「任意窗口内不超过 N」——那只能用滑动窗口。所以选择哪一个取决于**上游的条款形状**，而不是实现者的偏好。

**架构推论：** `asyncio_run` 这个函数的存在本身说明一个普遍困境：「同步 API 与异步 API 同时对外提供」会让调用方落在各种事件循环状态里（无循环、有循环未运行、有循环在运行）。第三种的唯一正解是「换线程」，而换线程又必须显式搬运 contextvars。这条链上的每个决定都不是可选的——漏掉任一步都会有「回调丢失」或「死锁」这类难查的症状。

**架构推论：** `retry` 里那个「捕获走 MRO、查找走精确类」的不对称，是一个**类型匹配与字典查找语义不同**的典型 bug。它不会报错，只会在「注册了父类异常、抛出了子类」这种常见场景下让 `check_fn` 静默失效——于是本该「检查后放弃」的错误被无脑重试。判据很实用：**任何「用异常类型做键」的字典，取值时都要想清是精确匹配还是要走 MRO**。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `llama-index-core/llama_index/core/rate_limiter.py` | `BaseRateLimiter` | 两个抽象方法，允许替换分布式实现 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `TokenBucketRateLimiter` | 存量加补充速率，允许初始满载突发 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `TokenBucketRateLimiter.__init__` | 把两个公开字段换算成三个私有状态并装满 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `TokenBucketRateLimiter.acquire` | 锁内查容量，锁外 sleep，同步异步只差这一行 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `SlidingWindowRateLimiter` | 时间队列，严格均摊且精确统计 token 用量 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `_SLIDING_WINDOW_SECONDS` | 60 秒窗口常量 |
| `llama-index-core/llama_index/core/rate_limiter.py` | `RateLimiter` | 令牌桶的向后兼容别名 |
| `llama-index-core/llama_index/core/async_utils.py` | `asyncio_run` | 三分支处理事件循环状态，含 contextvars 搬运 |
| `llama-index-core/llama_index/core/async_utils.py` | `run_async_tasks` | 进度条路径全局 patch nest_asyncio，异常静默降级 |
| `llama-index-core/llama_index/core/async_utils.py` | `batch_gather` | 分批 gather，过滤 `None` 填充值 |
| `llama-index-core/llama_index/core/async_utils.py` | `chunks` | `zip_longest` 造定长批次 |
| `llama-index-core/llama_index/core/async_utils.py` | `run_jobs` | 信号量限并发，双层 span |
| `llama-index-core/llama_index/core/async_utils.py` | `DEFAULT_NUM_WORKERS` | 保守默认 4 |
| `llama-index-core/llama_index/core/utils.py` | `retry_on_exceptions_with_backoff` | 捕获走 MRO、check_fn 走精确类的不对称 |
| `llama-index-core/llama_index/core/utils.py` | `ErrorToRetry` | 异常类加可选检查函数 |
| `llama-index-core/llama_index/core/utils.py` | `get_tokenizer` | 临时设 tiktoken 缓存目录后撤销 |
| `llama-index-core/llama_index/core/utils.py` | `truncate_text` | 负预算硬截，正常路径长度恰为目标 |
| `llama-index-core/llama_index/core/utils.py` | `concat_dirs` | 先补 `/` 再 join，避免 Windows 反斜杠 |

## 参考

- [llama-index 源码：rate_limiter.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/rate_limiter.py)
- [llama-index 源码：async_utils.py](https://github.com/run-llama/llama_index/blob/main/llama-index-core/llama_index/core/async_utils.py)
- [Python 文档：contextvars 与线程隔离](https://docs.python.org/3/library/contextvars.html)

## 与主核 / AI 的关系

主核里最接近的是「准入控制」：连接数上限、每秒查询数配额、写入速率限制。那一层的典型实现也是令牌桶（因为数据库的准入向来允许突发——一个闲置的连接突然发一批查询是正常的）。

差别在于**限流的作用域**。主核的限流是**实例级、由服务端强制**的，客户端无法绕过；这里的限流器是**客户端自愿遵守**的——它只是让你的代码在本地等待，任何一个绕过它的调用路径（例如直接发 HTTP）都不会被限制。所以它是「礼貌」而非「强制」，这与主核的准入控制在性质上不同。

与 i01（模型与嵌入抽象）的连接：`BaseLLM.rate_limiter` 与 `BaseEmbedding.rate_limiter` 都是 `Optional[Any]`（用 `Any` 规避导入环），而 `_aget_text_embeddings_rate_limited` 是实际接入点——一批只 acquire 一次。

与 h01（存储上下文）的 `concat_dirs` 关系直接：`StorageContext.persist` 在 `fs is not None` 时用的就是这一页的函数。

对 AI 工程最实用的一条是「限流策略要跟着上游条款选」。如果 provider 的文档说「每分钟 N 次」，令牌桶是对的（它模仿了这个语义且允许突发）；如果条款说「任意 60 秒窗口内不超过 N」，那就只能滑动窗口。而两者在客户端感受到的差异很明显：令牌桶会在闲置后立刻放行一批，滑动窗口会让「刚用满」的那段时间寸步难行。选错的症状是「偶尔 429 但平均速率没超」。

另一条是 `asyncio_run` 里那个 contextvars 搬运。任何「同步接口内部起线程跑异步」的实现都要做这件事，否则「当前请求的追踪 id、日志上下文、回调管理器」在子线程里全部变成默认值。症状很隐蔽：日志里看不到关联、追踪里出现断开的两段、回调打到了错误的地方。做法是 `contextvars.copy_context()` 加 `ctx.run(...)`。

最后一条是 `retry` 那个不对称。写重试逻辑时，最容易忽略的是「异常类型的匹配策略」：用 `except (A, B)` 捕获时 Python 会走继承（子类也命中），而用字典按类型做查表时是精确匹配。两者混用就会让「按父类注册的检查函数」永远不生效。修法很直接：查表时沿 MRO 找第一个命中的键，而不是只查精确类。
