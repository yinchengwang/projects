---
project: graphrag
feature: rate-limit-and-retry
title: 限流与重试
page_type: feature
schema_version: 1
refs:
  - path: packages/graphrag-llm/graphrag_llm/rate_limit/sliding_window_rate_limiter.py
    symbols: [SlidingWindowRateLimiter]
  - path: packages/graphrag-llm/graphrag_llm/rate_limit/rate_limit_factory.py
    symbols: [RateLimitFactory, create_rate_limiter]
  - path: packages/graphrag-llm/graphrag_llm/retry/exponential_retry.py
    symbols: [ExponentialRetry]
  - path: packages/graphrag-llm/graphrag_llm/retry/retry_factory.py
    symbols: [RetryFactory, create_retry]
---

# 限流与重试

## 一句话职责

用一对可替换的策略对象控制「打端点的节奏」：限流器按**双滑动窗口**（请求数 + token 数）决定何时可以发，重试器按**异常白名单**决定哪些失败值得重发、按**指数退避 + 抖动**决定等多久再发。

## 机制是什么

两者的职责边界必须分清：

| | 限流 | 重试 |
|---|---|---|
| 管的是 | **速率**（单位时间内允许多少） | **失败**（哪些失败可再来一次） |
| 触发条件 | 每次请求进入前 | 每次请求失败后 |
| 状态 | 窗口队列（跨请求累积） | 只在本请求内（计数器） |
| 不做的 | 判断请求是否合法 | 判断现在是否该慢下来 |

**关键在最后一行：重试器不知道当前的速率，限流器不知道刚才失败了几次。** 它们通过中间件的层序协作（重试在外、限流在内，见 e02 那页），而不是通过共享状态。

**两个抽象都极薄**（`RateLimiter` 38 行、`Retry` 32 行），因为实现全在子类里，而工厂按配置的类型名挑子类。

## 怎么实现的

### 一、`SlidingWindowRateLimiter`：两个队列、一个锁、四道闸

**它的状态只有五个字段，而其中两个队列必须严格同长。**

```
_rpp: int | None       # requests per period
_tpp: int | None       # tokens per period
_lock: threading.Lock
_rate_queue: deque[float]   # 每次请求的时间戳
_token_queue: deque[int]    # 每次请求的 token 数
_period_in_seconds: int
_last_time: float | None
_stagger: float
```

**两个队列的同步是硬约束**：弹出时同时 `popleft()`，压入时同时 `append()`。**若某次弹出只做了队列之一，两个窗口就从同一条请求开始错位。**

**`acquire` 是一个 `@contextmanager`，而它的主体是一个 `while True` 循环 —— 一个「拿不到就重试」的自旋。**

```
@contextmanager
def acquire(self, token_count: int) -> Generator[None, None, None]:
    while True:
        with self._lock:
            current_time = time.time()
            while (len(self._rate_queue) > 0 and self._rate_queue[0] < current_time - self._period_in_seconds):
                self._rate_queue.popleft()
                self._token_queue.popleft()
            ...
```

**第一步永远是「丢弃滑出窗口的历史」。** 判据是 `< current_time - period`，即「超过一个周期之前的时间戳」。

**然后四道闸，全部用 `continue` 回到循环开头（而不是 `sleep`）。**

**闸一：请求数超限。**

```
if self._rpp is not None and self._rpp > 0 and len(self._rate_queue) >= self._rpp:
    continue
```

**`>=` 而不是 `>`** —— 窗口里已满 `rpp` 条时不能再进。注意这里**没有 `sleep`**，靠 `while True` 立刻重试。**这个自旋看起来激进，但 `continue` 之前 `with self._lock` 已经退出**（`with` 块随 `continue` 结束），于是其他线程能拿到锁检查自己的窗口。**注释明确说了这一点：**

> 等待需要重新获取锁，让其他线程能看自己的请求是否落在限流窗口内。**这对 token 限制比请求限制更合理。**

**闸二：token 窗口已满。**

```
if self._tpp is not None and self._tpp > 0 and sum(self._token_queue) >= self._tpp:
    continue
```

**注意它 `sum()` 整个队列** —— O(n) 每次循环。而 `n` 的上界是 `rpp`（几百量级），所以可接受；但这也是为什么两个限制通常只配一个。

**闸三：本次请求会让 token 窗口溢出 —— 但有条件。**

```
if (self._tpp is not None and self._tpp > 0
    and token_count <= self._tpp
    and sum(self._token_queue) + token_count > self._tpp):
    continue
```

**`token_count <= self._tpp` 这个前提是整段最关键的语义，注释给了解释：**

> 这**不**计入当前请求的 token。这是**有意**的，因为我们希望当前请求能被处理——**若它比 tpm 更大但比上下文窗口小**。tpm 是一个**速率/软限制**，不是上下文窗口的硬限制。

**展开这条规则：**

| 历史用量 | 本次 token | 结果 |
|---|---|---|
| 窗口未满 | 小 | 允许 |
| 窗口未满 | **单个请求就超过 `tpp`** | **允许（放行）** |
| 窗口 + 本次 > `tpp`，但本次 ≤ `tpp` | — | 等待 |

**第二行是「软限制」的体现**：一个超过每分钟 token 配额的单次请求**不会被永久阻塞**（否则它会永远等不到「历史用量归零到足够小」的时刻，因为它的需求量本身大于配额）。**用 `continue` 自旋会变成死循环** —— 而这正是这个条件在防止的。

**闸四：错峰（stagger）。**

```
if (self._stagger > 0 and (self._last_time and current_time - self._last_time < self._stagger)):
    time.sleep(self._stagger - (current_time - self._last_time))
    current_time = time.time()
```

**`_stagger = period / rpp`** —— 即「如果均匀发请求，每个之间该隔多久」。

**这是唯一会 `sleep` 的闸，而它 sleep 时仍持有锁。** 这是一个有意（且值得注意）的选择：持有锁 sleep 会阻塞其他线程。理由大概是「错峰就是要求大家排队」——但代价是**所有并发线程被串行化成 `period/rpp` 的间隔**。

**而 `time.sleep` 之后立刻更新 `current_time`** —— 因为后面要用它记时间戳与 `_last_time`。

**四道闸都过了才记账。**

```
self._rate_queue.append(current_time)
self._token_queue.append(token_count)
self._last_time = current_time
break
```

**`_last_time` 是「上一次放行的时间」，与队尾不同**（队尾是最后一次记账，而两者其实相同）。它的用途只在错峰判断里。

**而 `yield` 在循环之外：**

```
yield
```

**所以 `with rate_limiter.acquire(n):` 的语义是「先等到能发，进入时已记账，退出时什么都不做」。** 记账发生在**进入**而不是退出——因为要限制的是「发出请求」这个动作。

**`period_in_seconds / requests_per_period` 的校验在 `__init__` 里只做了「> 0 才设 stagger」，没有抛错** —— 尽管文档字符串声明了 `Raises ValueError`。**这是一个文档与实现的不一致**：`period_in_seconds` 为 0 或负数时不会抛，而是让窗口判据 `_rate_queue[0] < current_time - period` 永远为假（历史永不过期），最终全部线程卡在闸一。**代码里没有对应的校验**，值得注意。

### 二、`ExponentialRetry`：延迟序列的构造

**延迟的递推是三行：**

```
retries: int = 0
delay = 1.0
...
delay *= self._base_delay
sleep_delay = min(self._max_delay, delay + (self._jitter * random.uniform(0, 1)))
time.sleep(sleep_delay)
```

**展开 `base_delay=2.0`（默认）、`jitter=True`、`max_delay=inf` 的情况：**

| 第几次重试 | `delay` | `sleep_delay` 区间 |
|---|---|---|
| 1 | 2.0 | 2.0 – 3.0 |
| 2 | 4.0 | 4.0 – 5.0 |
| 3 | 8.0 | 8.0 – 9.0 |
| … | … | … |
| 7 | 128.0 | 128.0 – 129.0 |

**注意 `delay` 初值是 `1.0` 而不是 `base_delay`** —— 于是**第一次**重试前 `delay *= 2` 得到 2.0。若初值写成 `base_delay`，第一次就要等 4 秒。**这个 off-by-one 是刻意的**，而默认注释也印证了它：「`max_retries=7` 时默认设置下最大延迟 2^7 = 128 秒」。

**抖动是加性的 `+ uniform(0, 1)` 而不是乘性的** —— 于是**高延迟时抖动占比可忽略**（128 秒上 ±1 秒），**低延迟时抖动占比很大**（2 秒上最多 +50%）。这个分布是保守的：**它只会让等待变长，不会变短**。

**为什么需要抖动**：多个线程同时失败会同时重试，形成「惊群」。加性抖动把它们的重试时刻打散，代价是最坏延迟增加 1 秒（与 `max_delay` 的截断是两件事——`min` 在加法之后，所以抖动可能被截断掉）。

**异常白名单用类名匹配，不是 `isinstance`。**

```
except Exception as e:
    if e.__class__.__name__ in self._exceptions_to_skip:
        raise
```

**`e.__class__.__name__` 得到的是**字符串**，与 `_default_exceptions_to_skip` 里的字符串比较。**

**这个选择有明确的动机：不导入那些异常类。** 白名单里有 `ContextWindowExceededError`、`ContentPolicyViolationError`、`ImageFetchError`、`BudgetExceededError` 这些 **LiteLLM 特有的异常**——若用 `isinstance`，`graphrag-llm` 就必须硬依赖 `litellm` 的异常模块（而 `graphrag_llm` 的定位之一是「provider 无关」）。

**代价是它无法区分同名异常**：两个不同库各自定义一个 `APIError`，白名单会同时命中（或误中）。**这是一个用精确性换解耦的交易。**

**而白名单本身的构成值得读：**

```
_default_exceptions_to_skip = [
    "invalid_request_error",
    "UnsupportedParamsError",
    "ContextWindowExceededError",
    "ContentPolicyViolationError",
    "ImageFetchError",
    "InvalidRequestError",
    "AuthenticationError",
    "PermissionDeniedError",
    "NotFoundError",
    "UnprocessableEntityError",
    "APIConnectionError",
    "APIError",
    "ServiceUnavailableError",
    "APIResponseValidationError",
    "BudgetExceededError",
]
```

**分三类，而分类揭示了重试的完整判据：**

| 类别 | 例子 | 为什么立即失败 |
|---|---|---|
| **请求本身不合法** | `invalid_request_error`、`UnsupportedParamsError`、`InvalidRequestError`、`UnprocessableEntityError` | 重发同样的请求必然同样失败 |
| **权限与资源** | `AuthenticationError`、`PermissionDeniedError`、`NotFoundError`、`BudgetExceededError` | 重试不会改变凭证或余额 |
| **语义性拒绝** | `ContextWindowExceededError`、`ContentPolicyViolationError`、`ImageFetchError` | 输入超限/违规/资源不可取，重发一样 |

**注意一个反直觉的项：`APIConnectionError`、`APIError`、`ServiceUnavailableError` 都在跳过名单里。**

**这三个看起来最该重试**（连接失败、服务暂时不可用）。而它们被列为「跳过」意味着**默认不重试这些**——这与常识相反。

**可能的解释是「让 LiteLLM 自己处理这些」**：LiteLLM 内部有对连接错误与 5xx 的重试机制，而这里的重试器只处理「LiteLLM 放弃之后仍然冒出来的失败」。**若两层都重试，实际尝试次数会相乘**（7 × LiteLLM 的 N 次）。

**这是一个值得注意的设计**：它把重试职责**分层**了，而不是接管全部。而代价是「为什么这些经典可重试的错误反而不重试」需要读白名单才明白——**注释里没有解释这一点**。

**`metrics` 在 `finally` 里更新。**

```
finally:
    if metrics is not None:
        metrics["retries"] = retries
        metrics["requests_with_retries"] = 1 if retries > 0 else 0
```

**`finally` 而不是「成功后」** —— 于是**失败路径也写入重试次数**。这很关键：一次耗尽 7 次重试的失败，其 `retries=7` 必须被记录，否则指标会低估失败的成本。

**而 `retry_async` 与同步版逐行同构**，唯一差别是 `asyncio.sleep` 替代 `time.sleep`、`await func(...)` 替代 `func(...)`。**连 `delay = 1.0` 的初值与 `finally` 里的指标写法都相同。**

**这意味着两份代码必须手动保持同步。** 而 `ImmediateRetry` 是第三个副本（去掉延迟计算）。**同一个循环逻辑存在六份拷贝**（Exponential/Immediate × sync/async，加上两处 `finally`）。

### 三、`ImmediateRetry`：同一循环去掉退避

**它保留了一切，只删了延迟。**

```
def retry(self, *, func, input_args):
    retries: int = 0
    metrics = input_args.get("metrics")
    while True:
        try:
            return func(**input_args)
        except Exception as e:
            if e.__class__.__name__ in self._exceptions_to_skip: raise
            if retries >= self._max_retries: raise
            retries += 1
        finally:
            if metrics is not None:
                metrics["retries"] = retries
                metrics["requests_with_retries"] = 1 if retries > 0 else 0
```

**`delay`、`time.sleep`、`_base_delay`、`_jitter`、`_max_delay` 全部消失，构造签名也少了三个参数。**

**它的存在说明「退避」不是重试的定义，而是一个可选策略** —— 而「立即重试」在某些场景下是对的（比如对本地资源竞争的失败，退避反而让别的请求抢走资源）。

**但它也暴露了一个问题：两者的公共结构没有被抽出来。** 一个 `_retry_loop(func, sleeper)` 的辅助函数能消掉大部分重复——而代码选择了复制。理由可能是「重试循环的控制流对读者太重要，不该被抽象藏起来」。

### 四、两个工厂：完全同构

**`create_rate_limiter` 与 `create_retry` 的骨架逐行相同。**

```
strategy = config.type
init_args = config.model_dump()

if strategy not in xxx_factory:
    match strategy:
        case Type.A: xxx_factory.register(...)
        case _: raise ValueError(f"... not registered in the XxxFactory. Registered strategies: {', '.join(xxx_factory.keys())}")

return xxx_factory.create(strategy=strategy, init_args=init_args)
```

**三个共同细节：**

1. **`init_args = config.model_dump()`** —— 把 pydantic 配置对象整个摊平当构造参数。于是配置项的名字必须与构造参数名**逐字一致**（`period_in_seconds`、`requests_per_period`、`tokens_per_period`；`max_retries`、`base_delay`、`jitter`、`max_delay`、`exceptions_to_skip`）。**这比显式挑字段省事，但也意味着改一个构造参数名会静默地让配置失效**（多出的 `**kwargs` 会吞掉它）。
2. **懒注册 + `match` 分派** —— 与 `create_completion` 同构（见 e01 那页），`import` 在 `case` 分支内。
3. **兜底分支列出已注册的 strategy** —— 让拼写错误可见。

**注意 `create_rate_limiter` 是唯一不需要额外判断的**：它没有「可选组件」的概念（限流器要么有要么没有，而「没有」在装配层用 `if model_config.rate_limit` 表达）。

**而 `RateLimitType` 只有 `SlidingWindow` 一个成员，`RetryType` 只有 `ExponentialBackoff` 与 `Immediate` 两个。** 于是这些 `match` 目前都是「一个 case + 兜底」——**但结构已经为扩展准备好了**（注册表是开放的，第三方可 `register_rate_limiter`）。

## 关键取舍

**源码事实：** 限流器用两个严格同长的 `deque`（时间戳 + token 数）实现双滑动窗口，弹出时同时 `popleft`；每轮先丢弃滑出一个周期之前的历史。

**源码事实：** `acquire` 是一个 `while True` 自旋，四道闸全部用 `continue`（不是 `sleep`）回环，且 `continue` 时已释放锁——注释说明这是为了让其他线程能检查自己的窗口。

**源码事实：** 闸三带 `token_count <= self._tpp` 前提：**单个请求的 token 数超过 `tpp` 时会被放行**。注释理由：「tpm 是速率/软限制，不是上下文窗口的硬限制」；若不设这个前提，超大请求会因「永远等不到窗口空出足够空间」而死循环。

**源码事实：** 错峰闸 `_stagger = period / rpp` 是唯一 `time.sleep` 的地方，且 **sleep 时仍持有锁**；sleep 后立刻刷新 `current_time`。

**源码事实：** `acquire` 的记账发生在**进入**（`yield` 之前），因为要限制的是「发出请求」这个动作。

**源码事实：** `__init__` 只做了「`rpp > 0` 时设 stagger」，**没有实现文档字符串声明的 `ValueError`**；`period_in_seconds` 为 0 或负数时不会报错，而会让历史永不过期、所有线程卡在闸一。

**源码事实：** `ExponentialRetry` 的 `delay` 初值是 `1.0` 而非 `base_delay`，于是第一次重试等 `base_delay` 秒（默认 2 秒）而非 `base_delay²`。默认 `max_retries=7` 对应注释里的「2^7 = 128 秒最大延迟」。

**源码事实：** 抖动是加性 `uniform(0, 1)` 而非乘性，于是低延迟时占比大、高延迟时占比小，且只会让等待变长（`min` 在加法之后，抖动可能被 `max_delay` 截断）。

**源码事实：** 异常白名单按 `e.__class__.__name__` 字符串匹配而非 `isinstance`，为的是不导入 LiteLLM 私有异常类（保持 provider 无关）。代价是无法区分不同库的同名异常。

**源码事实：** 白名单把 `APIConnectionError` / `APIError` / `ServiceUnavailableError` 也列为「不重试」——与「这三个最该重试」的常识相反，推测是让 LiteLLM 内部的重试负责它们以避免两层重试相乘。**这一点代码里没有注释说明。**

**源码事实：** 重试次数在 `finally` 里写入指标，于是失败路径（耗尽重试）也记录 `retries=7`，不会低估失败成本。

**源码事实：** `ImmediateRetry` 与 `ExponentialRetry` 的循环结构完全重复（去掉延迟后其余逐行相同），同步与异步各一份，共六份近似拷贝；公共结构未被抽出。

**源码事实：** 两个工厂都 `init_args = config.model_dump()`，于是配置字段名必须与构造参数名逐字一致，多出的键会被 `**kwargs` 静默吞掉。

**架构推论：** 限流器的核心决策是**「用一个软限制而不是硬限制」**。tpm 超限时它不会拒绝请求，只会让它等；而单个请求超过配额时它选择放行而不是死锁。**这个选择把「配额」从「约束」降级成「节流目标」** —— 因为真正不可越过的边界是上下文窗口（会报错），而每分钟 token 数只是计费与公平性的口径。

**架构推论：** 限流与重试完全不知道对方的存在，这是**通过中间件层序实现的协作**（e02 那页）。好处是两个对象各自可独立测试、独立替换；坏处是「重试流量会被限流器看到」这个关键性质**不在任何一个类的代码里**——它在装配那几行 `if` 的顺序中。**跨组件的语义保证，如果只存在于装配顺序里，就没有任何单元测试能覆盖它。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `packages/graphrag-llm/graphrag_llm/rate_limit/sliding_window_rate_limiter.py` | `SlidingWindowRateLimiter` | 双滑动窗口 + 四道闸 + 错峰 |
| `packages/graphrag-llm/graphrag_llm/rate_limit/rate_limiter.py` | `RateLimiter` | 抽象基类，唯一方法 `acquire` 上下文管理器 |
| `packages/graphrag-llm/graphrag_llm/rate_limit/rate_limit_factory.py` | `RateLimitFactory` / `create_rate_limiter` | 按 `RateLimitType` 装配 |
| `packages/graphrag-llm/graphrag_llm/retry/exponential_retry.py` | `ExponentialRetry` | 指数退避 + 加性抖动 |
| `packages/graphrag-llm/graphrag_llm/retry/immediate_retry.py` | `ImmediateRetry` | 同循环去掉退避 |
| `packages/graphrag-llm/graphrag_llm/retry/exceptions_to_skip.py` | `_default_exceptions_to_skip` | 15 个按类名匹配的跳过名单 |
| `packages/graphrag-llm/graphrag_llm/retry/retry.py` | `Retry` | 抽象基类，`retry` / `retry_async` |
| `packages/graphrag-llm/graphrag_llm/retry/retry_factory.py` | `RetryFactory` / `create_retry` | 按 `RetryType` 装配 |
| `packages/graphrag-llm/graphrag_llm/config/rate_limit_config.py` | `RateLimitConfig` | 限流配置模型 |
| `packages/graphrag-llm/graphrag_llm/config/retry_config.py` | `RetryConfig` | 重试配置模型 |

## 参考

- [graphrag 源码仓库](https://github.com/microsoft/graphrag)
- [AWS 架构博客：指数退避与抖动](https://aws.amazon.com/blogs/architecture/exponential-backoff-and-jitter/)
- [LiteLLM 文档：可靠补全](https://docs.litellm.ai/docs/completion/reliable_completions)
- [graphrag 源码：sliding_window_rate_limiter.py](https://github.com/microsoft/graphrag/blob/main/packages/graphrag-llm/graphrag_llm/rate_limit/sliding_window_rate_limiter.py)

## 与主核 / AI 的关系

限流器与主核的**准入控制**（admission control / 并发闸）面对同一个问题：在资源有限时决定「现在放谁进去」。而两者的判据截然不同。

**数据库的准入看的是「可预见的资源消耗」** —— 连接数、内存、锁冲突。它有精确的内部视图，能算出「再加一个事务会不会超出 buffer pool」。**而限流器只有外部信号**：它不知道端点的真实负载，只能按约定的分钟配额自己计数。**这是「闭系统」与「开系统」的差别**：闭系统可以精确控制，开系统只能保守节流。

**「软限制」这个定位与主核的 `work_mem` 同构**：PostgreSQL 的 `work_mem` 不是硬上限（一个查询用超了不会被杀，只是可能溢出到磁盘），而 `statement_timeout` 才是硬的。**软限制用性能换确定性，硬限制用失败换确定性** —— 而限流器选前者。

**「重试白名单按类名字符串匹配」在数据库里有精确对应：错误码分级的重试策略。** 数据库客户端也把错误分成「可重试（序列化失败、死锁）」「不可重试（语法错误、约束违反）」，而判据同样是错误码而非异常类型——因为跨语言的客户端无法用类型系统。**代价也一样：错误码是字符串，拼错就静默失效。**

**「重试在限流之外」与数据库连接池的重试排队是同一个要求**：重试必须重新计入资源计量，否则实际并发超过配置值。这一条在两边都是「不写在任何单个类里」的跨组件约定。

对 agent 工程的直接价值有三条。其一，**软限制必须允许「单个请求超配额」通过**。否则那个请求会永远等不到足够空间，从调节变成死锁——这是配额类设计里最容易犯的错。其二，**退避初值不要写成 `base_delay`**。递推在循环体内先乘再睡，初值应为 1，否则每次延迟都多一档。其三，**重试层与 provider 内置重试不能叠加**。要么关掉一层的重试，要么把「经典可重试错误」排除在应用层白名单外，否则实际尝试次数是乘积。