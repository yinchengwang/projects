---
project: pydantic-ai
feature: cancellation
title: 取消传播与截止时间
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/_cancel.py
    symbols: [RunBinding, provide_run_binding, inherited_run_deadline]
  - path: pydantic_ai_slim/pydantic_ai/retries.py
    symbols: [RetryConfig, wait_retry_after]
---

# 取消传播与截止时间

## 一句话职责

把「停下来」分成三件必须分别处理的事：**谁有权取消**（外部令牌 / 调用方任务取消 / 截止时间到期）、**取消怎么送到正在干活的那个任务**（运行会换驱动任务，所以要绑定）、以及**取消到底算谁的**（区分「我们发的」与「别人发的」，前者翻译成 `RunCancelled`，后者继续传播）。

## 机制是什么

`_cancel.py` 里有四层结构：

- `CancellationToken`：线程安全的外部句柄，一次 `cancel()` 取消所有注册到它的运行。
- `RunCancellation`：**单个运行**的取消控制器，每个运行一个实例，在运行的公开句柄与内部之间按引用共享。
- `RunBinding`：把 `AgentRunEvents` 句柄与它将要启动的运行桥接起来的数据类。
- `_InheritedDeadline` + 两个 `ContextVar`：运行截止时间的继承机制。

`retries.py` 提供另一侧的「时间控制」：`RetryConfig` 是 tenacity `retry` 装饰器参数的 `TypedDict`；`wait_retry_after(max_wait=300)` 是一个会尊重 HTTP `Retry-After` 头的等待策略。

## 怎么实现的

**运行会更换驱动任务，所以取消必须「绑定」而不是「记住」。** `RunCancellation` 的类文档写明：驱动运行的任务在每个步骤边界用 `bind()` 绑定自己，于是 `cancel()` 总是取消**当前真正在干活**的那个任务。这解决的是「手动用 `AgentRun.next()` 逐步驱动、而每一步来自不同任务」的情形——如果只在启动时记住任务，后来的步骤就取消不到。

**`bind()` 会补发早到的取消。** 文档说：若在绑定任何任务之前就请求过取消（例如对一个懒启动的运行调 `cancel()`），或取消曾被发给上一个驱动任务，它会**重新投递**给这个新任务。它还提到一个前提：一个捕获并 `uncancel()` 掉控制器自己那次取消的调用方，就接管了记账；发出的计数会在下一边界重新同步。

**`_issued` 用「发出计数」而非布尔。** 因为 `Task.cancel()` 与 `Task.uncancel()` 在 asyncio 里是计数式的，控制器必须记住它发出了几次，才能在 `resolve()` 里只 `uncancel()` 自己那几次。`bind()` 里还有一处 clamp：`self._issued[task] = min(self._issued[task], task.cancelling())`，注释解释这是为了防一个具体 bug——用户 `uncancel` 之后又来一次数量匹配的外部取消，会留下一个陈旧的发出记录，`resolve()` 就会把外部取消误判成自己的。

**`cancel()` 与 `expire()` 是两条不同入口，但只留第一个决定结果。** `cancel()` 是「人要求停」；`expire()` 是「截止时间到了，所以运行该以 `RunTimedOut` 结束」。两者都走 `_request(timed_out=...)`，而 `_request` 在 `self._requested` 已为真时直接返回——**先到的那个决定结局**。这个规则写在 `expire()` 的文档里，是一条容易被忽略但影响语义的优先级约定。

**跨线程/跨 loop 的投递可能被拒绝，且拒绝时不留痕。** `_request` 在「有 owner、有 loop、owner 未完成、且当前不在那个 loop 上」时走 `loop.call_soon_threadsafe(self._deliver)`。若那个 loop 抛 `NotImplementedError`（注释举例：Temporal workflow 的 loop 只允许自己的代码驱动），它会抛 `UserError` 并**不记录取消请求**——这样它也不会在下一个步骤边界被投递。错误文案给了替代方案：在那个 loop 上运行的代码里取消，例如 Temporal 的 `@workflow.signal` 处理器。

**`CancellationToken.cancel()` 是「尽力全员投递」。** 它加锁后取出全部注册项，逐个 `try: cancellation.cancel()`；即使某一个抛 `UserError` 也**继续尝试其余的**（注释：一个不可达的运行不该遮蔽其他运行）。最后若有错误则把 `_cancelled` 改回 `False` 并重新抛出，注释说明理由：**保持令牌未取消，这样从正确位置再触发一次仍能到达那个漏掉的运行**；已经取消的那些忽略重复请求。

**`_register` 处理「令牌已取消之后才注册」的竞态。** 它在锁内判断：若令牌已取消，则不在锁内直接取消（避免持锁调用），而是记下 `should_cancel = True`，出锁后调用。这是一条标准的「先置标志再执行」写法，防止在锁内跑用户可见的回调。

**`resolve()` 是「这次 `CancelledError` 该不该被翻译」的唯一判据。** 它在运行的外缘被调用，只消费控制器发给**当前任务**的那些取消（通过 `Task.uncancel()`），返回 `True` 表示这是第一方取消且没有外部取消仍在挂起（于是翻译成 `RunCancelled`），`False` 表示必须继续以 `CancelledError` 传播。

文档还坦白了两处边界。其一，它**不设基线**：与 `asyncio.timeout()` 用进入作用域时的计数做基线不同，这里运行启动时已挂起的取消计数会让一次第一方取消被解析成外部取消——这是刻意的，因为「外部优先」是保守方向。其二，有一个残留窗口：如果在用户代码捕获第一方取消并自行 `uncancel()` 之后、在下一次 `bind()` 之前恰好来了一次数量匹配的外部取消，`bind()` 的 clamp 会保留陈旧记录，`resolve()` 就会把外部取消吞成第一方。文档明确说修好它需要「发出身份追踪」，并标注了 issue 号。

**`release_issued()` 是清理「没人认领的取消」。** 它把所有仍记在 `_issued` 里、从未被 `resolve()` 消费的取消统一 `uncancel()` 掉，包括「被用户代码吞掉的」与「发给已被替换的驱动任务的」。理由写在文档里：防止污染任务外层的取消记账（例如 `asyncio.timeout()` 与 AnyIO 的取消作用域）。这条存在说明「控制器发出的取消会被外部机制观察到」，所以必须配平。

**`RunBinding` 存在是因为句柄先于运行存在。** 类文档说：`AgentRunEvents` 句柄在它的懒启动后台运行**之前**就存在，所以它拥有取消控制器；`Agent.iter()` 稍后接上活的运行状态，并**保留同一个控制器**。它还有 `started_by_installed_package` 字段，注释说明这个判断必须在调用方的栈里、在后台运行启动之前做出——因为运行本身判断不了（它下面只有驱动事件循环的那些帧）。

**`provide_run_binding` / `take_run_binding` 是一对「设置 + 消费一次」。** `take_run_binding` 取出后会把 contextvar 置 `None`，注释写明目的：**消费掉才能防止嵌套的运行继承外层句柄的绑定**。`provide_run_binding` 则负责在退出时恢复。

**截止时间是继承而不是共享。** `_current_run_deadline` 持有 `_InheritedDeadline(deadline, clock, active)`。`inherited_run_deadline()` 返回「调用者所在上下文里那个活跃运行的截止时间」，于是在这个上下文里启动的子运行可以继承它。关键在 `active` 字段：文档说明一个在运行内部 spawn 出来的任务会复制这个上下文、且可能活得比运行更久，所以运行退出时把它标为**不活跃**——一个已结束的运行的截止时间不再适用于那个任务后来启动的运行。

**`current_run_clock()` 让截止时间可以换钟。** 剩余运行时间按这个上下文里的钟测量，缺省是系统时钟（`_utils.now_utc`）。这对持久执行与测试都必要：一个重放的运行不该用真实时钟去判断「还剩多少时间」。

**`provide_run_deadline` 在退出时先把 `active` 置假再 reset。** 顺序很重要：先失效再恢复，避免恢复出来一个仍然「活跃」的旧截止时间。

**`retries.py` 是同一个问题的另一面：等待多久算合理。** `RetryConfig` 的字段与 tenacity `retry` 装饰器的参数一一对应（`sleep`、`stop`、`wait`、`retry`、`before`、`after`、`before_sleep`、`reraise`、`retry_error_cls`、`retry_error_callback`），全部可选，不填就用 tenacity 默认值。它是 `TypedDict(total=False)` 而不是 dataclass，这样能直接 `@retry(**config)` 展开。

**`wait_retry_after` 解决的是「服务器比你更清楚该等多久」。** 它在 `RetryCallState` 的异常里找 HTTP 状态错误，若有 `retry-after-ms` 头则优先用它（注释：OpenAI 与 Anthropic 会发这个头，他们的 SDK 也优先它），否则读 `retry-after`：先试整数秒，失败再试 HTTP 日期。asctime 格式没有时区，按 UTC 处理。所有结果都 `min(..., max_wait)` 截到上限（默认 300 秒）。无头或解析失败则退回指数退避（`wait_exponential(multiplier=1, max=60)`）。

**重试传输层不能重放不可重放的请求体。** `HTTPX2TenacityTransport` 的文档明确：body 是非可重放流的请求不能被重试——第一次尝试就消耗了流，再试会抛 `httpx2.StreamConsumed`；要重试就用可重放 body（例如 bytes）。它还规定了 `validate_response` 的 body 读取时机：当它为 4xx/5xx 抛错时，传输层会**先读完 body 再关闭响应**，以便 tenacity 的判定与调用方检查 `HTTPStatusError.response` 时能读到它；当它为 2xx/3xx 抛错时则不读 body 直接关闭。这条区分是「错误体对诊断有用，成功体只是浪费」的直接体现。

## 关键取舍

**源码事实：** 取消是按驱动任务绑定并重投递的，而不是记住启动时的任务。运行可以在不同任务上逐步推进。

**源码事实：** 「人取消」与「截止时间到期」只有先到的那个生效，且 `expire()` 让它以 `RunTimedOut` 结束而不是 `RunCancelled`。

**源码事实：** 跨 loop 无法投递时抛 `UserError` 且**不记录**取消请求，于是下次边界也不会补发。错误文案指向「在该 loop 上运行的代码里取消」。

**源码事实：** 令牌取消是尽力全员投递，一个不可达的运行不遮蔽其他运行；有失败时令牌回到「未取消」，以便从正确位置重试。

**源码事实：** 取消归属靠计数然后 `uncancel()` 自己那几次，**不设基线**（外部取消优先），并承认存在一个需要「发出身份追踪」才能修的残留窗口。

**源码事实：** `release_issued()` 必须把没人认领的取消配平掉，否则会污染外层的 `asyncio.timeout()` / AnyIO 取消作用域。

**源码事实：** `take_run_binding` 消费一次就清空，防止嵌套运行继承外层绑定。

**源码事实：** 截止时间通过带 `active` 标志的 contextvar 继承，退出时先失效再恢复；时钟可替换，缺省系统时钟。

**源码事实：** `retry-after-ms` 优先于 `Retry-After`，且所有等待值都被 `max_wait` 截断；无头时退回指数退避。

**源码事实：** 不可重放的请求体不能重试；`validate_response` 只在错误状态时读 body。

**架构推论：** 取消在任何异步框架里都是最难做对的一块，因为 asyncio 的取消是**按任务计的计数器**而不是「一个带身份的取消对象」。于是任何需要「这次取消是不是我发的」的地方，都只能靠记账与配平来近似。这套实现的三条经验值得直接搬走：其一，**运行会换任务，所以要在边界重新绑定**；其二，**自己发出的取消必须自己回收**，否则会破坏调用方的取消作用域；其三，**不确定时让外部优先**——吞掉别人的取消比漏掉自己的取消危险得多。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `CancellationToken` | 线程安全的外部取消句柄，可被多个运行共享 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation` | 单运行的取消控制器，按任务绑定与重投递 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation.bind` | 每步边界绑定驱动任务，补发早到的取消 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation.resolve` | 判定捕获到的取消该翻译还是继续传播 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunCancellation.release_issued` | 回收无人认领的取消，避免污染外层作用域 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `RunBinding` | 把懒启动句柄与运行桥接，含「谁启动的」判定 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `provide_run_binding` / `take_run_binding` | 设置与消费一次绑定，防嵌套继承 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `_InheritedDeadline` | 带 `active` 标志的截止时间继承载体 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `inherited_run_deadline` | 子运行继承外层活跃运行的截止时间 |
| `pydantic_ai_slim/pydantic_ai/_cancel.py` | `current_run_clock` | 剩余时间所依据的钟，可被替换 |
| `pydantic_ai_slim/pydantic_ai/retries.py` | `RetryConfig` | tenacity `retry` 参数的 TypedDict，全字段可选 |
| `pydantic_ai_slim/pydantic_ai/retries.py` | `wait_retry_after` | 尊重 `Retry-After` 的等待策略，含 `max_wait` 截断 |
| `pydantic_ai_slim/pydantic_ai/retries.py` | `HTTPX2TenacityTransport` | 传输层重试，含不可重放 body 的限制与 body 读取时机 |

## 参考

- [pydantic-ai 源码：_cancel.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_cancel.py)
- [pydantic-ai 源码：retries.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/retries.py)
- [Pydantic AI 文档：超时](https://ai.pydantic.dev/timeouts/)
- [Pydantic AI 文档：重试](https://ai.pydantic.dev/retries/)

## 与主核 / AI 的关系

取消与截止时间的处理，与主核里「语句取消 / 查询超时」共享同一组难题：取消是异步的、发出方与接收方不在一个执行上下文、而清理必须仍然完成。数据库通常靠「取消标志 + 定期检查点」处理，这里靠「任务绑定 + 取消计数配平」处理——因为 Python 的 asyncio 把取消做成了按任务的计数器。

对 agent 工程的直接价值有三条。其一，**自己发出的取消要自己回收**。任何在代码里 `Task.cancel()` 之后不 `uncancel()` 的实现，都会破坏调用方的 `asyncio.timeout()` 与 AnyIO 取消作用域——表现是超时不触发或者提前触发。其二，**截止时间要能换钟**。持久执行与回放场景下用真实时钟判断剩余时间会得到不一致的结果，所以时间来源必须可替换。其三，**等待时间要听服务器的**。`Retry-After` 是服务端主动给出的节流信号，硬编码指数退避会在被限流时继续加压；反过来，来自响应头的等待值必须有上限，否则一个恶意的 `Retry-After: 86400` 就能把进程挂住。
