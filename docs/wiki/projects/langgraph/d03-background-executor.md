---
project: langgraph
feature: background-executor
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_executor.py
    symbols: [BackgroundExecutor, AsyncBackgroundExecutor, gated]
---

# 后台执行器与异步门控

## 一句话职责

`BackgroundExecutor` 与 `AsyncBackgroundExecutor` 是运行期统一的异步执行底座：它们把「提交一个可调用对象」变成 future，在退出时负责取消、等待、重抛，其中 `gated` 是给异步版加并发上限的包装。

## 机制是什么

Pregel 的运行期到处都在「先开跑、回头再收结果」：任务执行、检查点保存、写入落盘、缓存写入，全部走 `submit`。这些调用共享同一个执行底座，底座提供三样东西：

1. 一个 `submit(fn, *args, __cancel_on_exit__, __reraise_on_exit__, __next_tick__, **kwargs) -> Future`。
2. 一个上下文管理器语义：进入时建立执行资源，退出时统一收尾。
3. 一个账本：记录每个 future 的两个布尔策略，决定退出时「取消它」与「抛它的异常」。

`PregelLoop.__enter__` 里 `self.submit = self.stack.enter_context(BackgroundExecutor(self.config))`，同一行代码同时把 `submit` 暴露给 loop 与把执行器挂进 `ExitStack`。所以整个 loop 生命周期内的所有后台提交都归一个执行器管，退出顺序由 `ExitStack` 保证。

## 怎么实现的

**同步版的资源来源。** `BackgroundExecutor.__init__` 用 `get_executor_for_config(config)` 拿线程池——这是 LangChain 的约定：同一个 config 里的并发上限、是否复用全局池都由它决定。它被 `stack.enter_context` 进入，因此线程池的关闭也由同一个 `ExitStack` 负责，不需要执行器自己关心。

**上下文传播。** `submit` 里 `ctx = copy_context()`，然后 `self.executor.submit(ctx.run, fn, *args, **kwargs)`。用 `ctx.run` 包一层，保证任务运行在提交点的 contextvars 快照里，而不是线程池线程的默认上下文。这一条支撑了「节点里 `get_config()` 能拿到自己的 config」——config 在 contextvar 里。

**`__next_tick__` 的语义。** 为真时提交的是 `next_tick(ctx.run, fn, ...)`，而 `next_tick` 第一句是 `time.sleep(0)`——源码注释写的是「在运行另一个函数之前把控制权让给其他线程」。这不是为了性能而是为了**顺序**：`_runner._call` 在提交子任务时用 `__next_tick__=True`，注释明确解释「在下一个 tick 启动新任务，确保这一 tick 的更新先被提交/流出去」。也就是说动态派生的任务必须比父任务的提交动作晚一步开始，否则它可能先于父任务的结果被流出去。

**账本与 `done` 回调。** `self.tasks[task] = (__cancel_on_exit__, __reraise_on_exit__)`，然后注册 `task.add_done_callback(self.done)`。`done` 的第一动作是 `task.result()`——这是「取走异常」的标准做法。分支是：

- `GraphBubbleUp`：`pop` 掉。注释写明「这是中断信号不是错误，所以退出时不该重抛」。
- 其他 `BaseException`：`pass`，即**留在字典里**。这就是「退出时重抛」的实现方式：字典里剩下的带异常 future 就是待重抛的清单。
- 无异常：`pop` 掉。

**`__exit__` 的四步收尾。** 先 `self.tasks.copy()`——注释说明 `done` 回调可能改字典，遍历必须基于快照。然后对每个 `cancel` 标记为真的 future 调 `cancel()`，同步版 `cancel()` 只能取消尚未开始的任务（源码注释也写了这一点）。再 `concurrent.futures.wait(pending)` 等所有未完成的任务落定。然后 `self.stack.__exit__(...)` 关掉线程池。最后：若 `exc_type is None`（不是「因为已有异常而退出」），遍历快照，跳过 `reraise` 为假的和在 `SKIP_RERAISE_SET` 里的，其余 `task.result()`——第一个抛出的异常就此冒泡，循环因此中断，所以实际效果是「重抛第一个遇到的成功提交顺序的失败」。`CancelledError` 被显式吞掉，因为取消是预期行为。

`if exc_type is None` 这个守卫很重要：若已经有异常正在传播（例如节点抛错导致 loop 退出），收尾不再重抛另一个，避免覆盖原始错误。

**异步版的差异。** 它不建线程池，直接用当前事件循环：`self.loop = asyncio.get_running_loop()`。提交走 `run_coroutine_threadsafe(coro, self.loop, name=..., context=copy_context(), lazy=__next_tick__)`；`CONTEXT_NOT_SUPPORTED`（Python 3.11 以下）时不传 `context`。`__next_tick__` 在这里不靠 `time.sleep`，而是靠 `lazy` 参数——源码注释写「noop in async (always True)」，即异步版总是延迟到下一次循环迭代才启动。

`__aexit__` 的取消用 `task.cancel(self.sentinel)`，把一个哨兵对象当取消消息传进去，便于在 `CancelledError.args` 里辨认取消来源。等待用 `await asyncio.wait(tasks)`（注意传的是字典，`asyncio.wait` 接受可迭代的 future），没有线程池可关。重抛逻辑与同步版同构。

**`gated` 是并发上限的唯一实现。** `AsyncBackgroundExecutor.__init__` 读 `config["max_concurrency"]` 建一个 `asyncio.Semaphore`（无值则为 `None`）。`submit` 里有信号量时把协程包成 `gated(self.semaphore, coro)`：

```
async with semaphore:
    return await coro
```

协程对象在 `submit` 里已经被 `fn(*args, **kwargs)` 创建，但协程体不会跑；`gated` 让「进入协程体」这件事被信号量门控。因此 `max_concurrency` 限制的是同时在跑的节点数，而不是同时在排队的任务数。

## 关键取舍

**源码事实：** 并发上限只在异步执行器上生效。同步版直接依赖 `get_executor_for_config` 的线程池大小，没有自己的信号量。因此「同一份代码在 `invoke` 与 `ainvoke` 下并发行为不同」——异步版有显式的 `gated`，同步版靠池容量。

**源码事实：** 失败的任务留在字典里，靠「退出时统一重抛」而不是「完成时立刻抛」。这使多个任务的异常不会同时炸开：只有第一个被抛出，其余被丢弃。代价是排在前面的成功任务若在后面又失败，用户只能看到最先提交的那个错误。

**源码事实：** `GraphBubbleUp` 在 `done` 里被 pop 掉，即「中断不算失败」。所以一个任务抛出中断后，执行器退出时不会重抛它——中断要靠 loop 侧的处理路径传播，而不是靠执行器。

**源码事实：** `SKIP_RERAISE_SET` 是一个跨实例的模块级 `WeakSet`。执行器与 runner 共享它：runner 把「错误已交给处理器」「异常会冒泡到父任务」的 future 放进去，执行器据此不重抛。这是两套组件之间唯一的隐式耦合点，用弱集合避免泄漏。

**架构推论：** 所有后台工作共用一个 `ExitStack` 与一个执行器，意味着「loop 退出」与「执行器退出」是同一个事件。这换来了确定性的收尾顺序（先取消、再等、再关池、最后重抛），代价是任何需要「比 loop 活得更久」的后台工作都无法用它——例如异步的检查点保存必须显式 `await`，不能指望执行器替它保活。

**架构推论：** `__next_tick__` 用一个「让出 CPU」的技巧解决顺序问题，而不是引入显式的队列或屏障。这依赖一个前提：线程池线程数大于 1，否则 `time.sleep(0)` 之后仍是同一个线程立刻继续。在 `max_concurrency=1` 的同步场景下，这个技巧的排序效果就消失了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_executor.py` | `BackgroundExecutor` | 同步执行器：线程池、上下文复制、退出取消/等待/重抛 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `BackgroundExecutor.submit` | 提交并登记策略，`__next_tick__` 延后启动 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `BackgroundExecutor.done` | 用 `task.result()` 取走异常并据类型决定是否留在重抛清单 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `AsyncBackgroundExecutor` | 异步执行器：事件循环、`max_concurrency` 信号量 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `AsyncBackgroundExecutor.__aexit__` | 取消（带哨兵消息）、等待、重抛 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `gated` | 用信号量门控协程进入执行 |
| `libs/langgraph/langgraph/pregel/_executor.py` | `next_tick` | 同步版的「让出线程」实现 |

## 参考

- [LangGraph 源码：pregel/_executor.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_executor.py)
- [LangGraph 源码：_internal/_future.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/_internal/_future.py)
- [Python 文档：contextvars 与 Context.run](https://docs.python.org/3/library/contextvars.html#contextvars.Context.run)

## 与主核 / AI 的关系

「提交时记策略、退出时统一收尾」与主核的 I/O 生命周期管理是同一种结构：主核在发起异步 I/O 时登记回调与取消方式，在关闭表空间时统一取消、等待、报错。两者都必须回答同一个问题：退出时遇到多个错误怎么办。这里选择「抛第一个、丢其余」，主核在多数路径上也选择「报第一个、其余记录」。

对 AI 侧而言，这一页解释了两件实际影响行为的事。其一，`max_concurrency` 只对异步路径生效，所以把一个同步 agent 改成 `ainvoke` 之后并发上限才真正起作用。其二，节点内用 `schedule` / `call` 派生的子任务在同步路径上靠「让出一个 tick」保证顺序，在 `max_concurrency=1` 或单线程池下这个保证会退化——若子任务的输出顺序重要，应显式写进状态而不是依赖启动顺序。

