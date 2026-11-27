---
project: autogen
feature: run-context
title: 运行上下文与停止条件
page_type: feature
schema_version: 1
refs:
  - path: python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py
    symbols: [RunContext, stop_when, stop_when_idle]
---

# 运行上下文与停止条件

## 一句话职责

把「运行循环怎么活、怎么死」从运行时对象里拆成一个小小的 `RunContext`，并提供三种停止口径：立刻停、等空转、按条件轮询。

## 机制是什么

`RunContext` 只持有三样东西：对运行时的反向引用、一个 `asyncio.Task`（跑主循环）、一个 `asyncio.Event`（停止信号）。

```
RunContext.__init__  ->  建 _run_task、建 _stopped 事件
RunContext._run      ->  while True: 若已停则返回；否则 await runtime._process_next()
RunContext.stop      ->  置位 _stopped、队列 shutdown(immediate)、await _run_task
RunContext.stop_when_idle -> await 队列 join()、再执行上面的 stop
RunContext.stop_when ->  起一个轮询协程，条件成立时调 stop
```

两个层次要分清：`RunContext` 管的是「这一轮运行的生命周期」，`SingleThreadedAgentRuntime.stop*` 管的是「把 `_run_context` 置空并换一个新队列」。后者是前者唯一的调用方，也是运行时能被重新 `start()` 的原因。

## 怎么实现的

**启动即建任务。** `start()` 检查 `_run_context` 是否已存在（存在就抛 `RuntimeError("Runtime is already started")`），然后构造 `RunContext`。构造函数里直接 `asyncio.create_task(self._run())`——所以这里必须在事件循环内被调用，否则 `create_task` 会失败。

**三种停止的信号路径不同。** 这是本机制最值得看的部分：

- `stop()`：先置事件、再关队列。关队列用 `immediate=True`，含义是队列里剩余项被丢弃并被逐个 `task_done()`。主循环此刻可能正卡在 `_process_next` 的 `get()` 上，队列关闭会唤醒它并抛 `QueueShutDown`；`_process_next` 捕获后检查有无后台异常，无则直接 `return`，主循环回到 `while` 顶部看到 `_stopped` 已置位，退出。
- `stop_when_idle()`：先 `await queue.join()` 等已入队消息全部完成，再走与 `stop()` 相同的关闭流程。此时队列必然已空，所以 `immediate=True` 实际上不会丢东西。`join()` 能返回的前提是每条路径都调过 `task_done()`——这条链在发送、广播、回复三条处理路径上都要成立。
- `stop_when(condition, check_period=1.0)`：起一个小协程 `check_condition`，每 `check_period` 秒醒一次看条件，成立就 `await self.stop()`。它自己不阻塞等待之外的东西，也不感知队列。

**两种调用面不齐。** `RunContext.stop_when` 接受 `(condition, check_period=1.0)`，而运行时公开的 `stop_when(condition)` 只透传条件、不暴露周期——周期恒为 1 秒。文档还给这个方法挂了 `caution`：明确说不推荐、属于遗留、让调用方自己用后台任务 + `asyncio.Event` 来实现。

**关闭后的重建。** 三个公开停止方法都在 `finally` 里把 `_run_context` 置 `None` 并新建一个 `Queue()`。所以同一个运行时对象可以「start → stop → start」反复用，但队列里残留的东西在新一轮里不存在。`stop_when` 的清理写在 `await self._run_context.stop_when(condition)` 之后、没有 `try/finally` 包裹，与另外两个不一致。

**未启动就直接拒绝。** `stop`/`stop_when_idle`/`stop_when` 都先检查 `_run_context is None` 并抛 `RuntimeError("Runtime is not started")`。这一点让「忘记 start」从静默无操作变成显式错误。

## 关键取舍

**源码事实：** 停止信号是「事件 + 队列关闭」双轨，而不是只靠 `task.cancel()`。取消任务会在任意 `await` 点打断处理器，留下半截状态；关队列则让当前这条消息自然跑完再退出。文档里对 `stop` 的描述与此一致：正在处理的那条会完成，后续的会被丢弃。

**源码事实：** `stop_when_idle` 是推荐的默认停止方式。它把「我已经不再需要运行时」翻译成「队列空了」，而不是「时间到了」或「有人手按了开关」，因此对级联消息（一个处理器里又发了消息）是安全的——`join()` 的未完成计数覆盖新入队的项。

**源码事实：** `stop_when` 的轮询是最弱的一档。周期固定 1 秒的忙循环意味着最短停止延迟是 1 秒，最长也就 1 秒——但条件求值本身若涉及锁或 IO 会放大成本。源码用 `caution` 段明确劝阻，等于把「别用它」写进了 API 文档。

**架构推论：** 把 `_run()` 做成 `while True` + 「检查标志后再取一条消息」，使退出点只有一个。若改成「消息处理完就判断一次」，就会在 `stop` 关队列那一瞬间产生「取到 `QueueShutDown` 却不知道该不该抛」的歧义。

**架构推论：** 状态清理放在公开方法（换队列、置空 context）而不是 `RunContext` 内部，让 `RunContext` 保持纯粹——它只知道怎么让自己的那条任务停下，不知道运行时要不要重开。这个分工是「同一运行时可多轮启停」的实现前提。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext` | 单轮运行的生命周期对象：任务 + 停止事件 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext._run` | 主循环：置位即退出，否则取一条消息处理 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext.stop` | 立即停止：置事件 + 关队列 + 等任务退出 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext.stop_when_idle` | 等队列 `join()` 后再停止，级联消息安全 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `RunContext.stop_when` | 定时轮询条件，成立即停 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `SingleThreadedAgentRuntime.start` | 构造 `RunContext`，重复启动报错 |
| `python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py` | `SingleThreadedAgentRuntime.stop_when_idle` | 转发并重建队列，使运行时可在多轮间复用 |

## 参考

- [AutoGen 源码：_single_threaded_agent_runtime.py](https://github.com/microsoft/autogen/blob/main/python/packages/autogen-core/src/autogen_core/_single_threaded_agent_runtime.py)
- [AutoGen 文档：Agent 与 Agent Runtime](https://microsoft.github.io/autogen/stable/user-guide/core-user-guide/framework/agent-and-agent-runtime.html)
- [Python 文档：asyncio 队列与 join](https://docs.python.org/3/library/asyncio-queue.html)

## 与主核 / AI 的关系

`RunContext` 与主核里「后台写线程的启动与排空」是同构问题：都要回答「还有没有在途工作」以及「关的时候要不要等在途工作收尾」。主核的做法是在关停时先拒新请求、再等引用计数归零；这里用队列的未完成计数做同一件事，`task_done` 相当于引用计数递减。

对 AI 侧有三条可直接用的判据。第一，选停止方式看消息是否级联：智能体互相发消息的场景必须用 `stop_when_idle`，用 `stop` 会截断半条链路。第二，`stop_when` 的 1 秒轮询在生产里就是额外的停止延迟与唤醒开销，条件判断应改写成在处理器里置 `asyncio.Event`。第三，运行时能在多轮启停间复用，但队列会被换掉——任何「发完消息直接 stop 再 start」的写法都会静默丢掉未处理的消息。
