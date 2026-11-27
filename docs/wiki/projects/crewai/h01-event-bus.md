---
project: crewai
feature: event-bus
title: 事件总线与处理器登记
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/events/event_bus.py
    symbols: [CrewAIEventsBus, on, set_runtime_state]
---

# 事件总线与处理器登记

## 一句话职责

一个进程级单例事件总线：按事件类登记同步与异步处理器、按需起线程池与后台事件循环、支持处理器之间的依赖排序、并把当前运行状态挂到总线上供处理器读取。

## 机制是什么

`CrewAIEventsBus` 是单例（`__new__` 里双检加锁），用一把读写锁保护处理器表。它维护五张表：同步处理器 `_sync_handlers`、异步处理器 `_async_handlers`、处理器依赖 `_handler_dependencies`、执行计划缓存 `_execution_plan_cache`，以及运行时状态。

线程模型是整套实现的核心：**同步处理器跑在一个 10 线程的 `ThreadPoolExecutor`**，**异步处理器跑在一条守护线程上的独立事件循环**里。两者都在首次 `emit` 时惰性初始化（`_ensure_executor_initialized`，双检加锁），理由是「不发射事件就完全没有线程开销」。

`on(event_type, depends_on=...)` 是装饰器形式的登记入口；`register_handler` 是直接调用的形式。处理器签名支持两种形状：`(source, event)` 与 `(source, event, state)`，按参数个数分派。

## 怎么实现的

**事件字段的补齐。** 每次 `emit` 都先调 `_prepare_event`，它有四件事：

1. `_register_source(source)`：若源的 `entity_type` 是 `flow`/`crew`/`agent` 且未登记过，调 `register_entity` 挂进运行时状态。
2. 填 `previous_event_id`（来自 `get_last_event_id()`）、`triggered_by_event_id`（来自 `get_triggering_event_id()`）、`emission_sequence`（来自 `get_next_emission_sequence()`）。
3. 若事件还没有 `parent_event_id`，按事件类型名分派：属于 `SCOPE_ENDING_EVENTS` 就从栈上弹一层、记 `started_event_id`、并校验配对的开始类型；属于 `SCOPE_STARTING_EVENTS` 就取当前栈顶作父、把自己压栈；其余取当前栈顶作父但不压栈。
4. `set_last_event_id(event.event_id)`，然后 `publish_stream_event` 给当前上下文的流式接收器，最后 `_record_event` 记进运行时状态的 `EventRecord`。

这一段是全部「因果信息」的来源，也是 h02 与 j03 两页的地基。

**分派的四条路径。** `emit` 拿到处理器的快照后：

- 有依赖（`event_type in self._handler_dependencies`）→ `asyncio.run_coroutine_threadsafe(self._emit_with_dependencies(...), self._loop)`。
- 只有同步处理器且不是流式块事件 → `contextvars.copy_context()` 后提交到线程池。
- 只有异步处理器 → `run_coroutine_threadsafe(self._acall_handlers(...))`。
- 混合 → 先提交同步的，再提交异步的，返回异步那个 future（同步 future 也被 `_track_future` 记着）。

`LLMStreamChunkEvent` 有特殊处理：**同步处理器在调用线程内直接执行**，不下线程池。原因是流式块必须保序，而线程池的完成顺序不保证。

**future 跟踪。** `_track_future` 把 future 塞进 `_pending_futures` 集合并挂一个 done 回调自动移除。`flush(timeout=30.0)` 就是 `concurrent.futures.wait` 等这批 future，超时返回 `False`，并把已完成 future 的异常逐条打到控制台。`shutdown(wait=True)` 先 flush 再置 `_shutting_down`，之后所有 emit 都被忽略并打警告。

**依赖排序。** `_emit_with_dependencies` 用 `build_execution_plan` 把处理器分成若干「层」，同一层可并行、层间串行。每一层内：同步处理器再整体提交到线程池（流式块事件除外），异步处理器 `gather` 并发跑。计划按事件类型缓存在 `_execution_plan_cache`，任何处理器增删都会 `pop` 掉对应缓存。`validate_dependencies()` 是显式的前置校验，能在发射前就抓出环。

**处理器的错误处理是 fail-open。** `_call_handlers` 逐个调 `is_call_handler_safe`，把返回的错误收集起来再统一打到控制台；`_acall_handlers` 用 `gather(return_exceptions=True)`，同样只打印。**同步与异步处理器的异常都不外抛**——一个坏处理器不会打断业务逻辑。

**运行时状态的 ContextVar 化。** `set_runtime_state(state)` 把状态写进 `_runtime_state_var`（ContextVar），并把 `state.root` 里每个实体的 `id()` 记进 `_registered_entity_ids`。`runtime_state` 属性是公开读口。`reset_runtime_state()` 两者都清空。

`_enter_runtime_scope` / `_exit_runtime_scope` 用 `_runtime_scope_depth` 计数实现嵌套：最外层进入时若状态为空就造一个 `RuntimeState(root=[])`；最外层退出时把状态与登记集合都置 `None`。所以「一次 kickoff 一个运行状态」是靠作用域深度而不是靠显式传对象实现的。

**实体登记的父子去重。** `register_entity` 先用 `id(entity)` 快速判重，再加锁二次检查。有个特殊分支：若实体是 `agent` 且它的 `crew` 已登记，则**只记 id 不追加进 root**——因为 agent 会作为 crew 的一部分被序列化，重复追加会让检查点里出现两份。

**`scoped_handlers` 的保存与还原。** 上下文管理器进入时快照三张表，把所有既有处理器逐个 `off` 掉；退出时把期间新增的处理器也 `off` 掉，再把快照里的重新登记（连同依赖）。所以它是「临时替换全部处理器」而不是「追加」。

**重放通道。** `replay(source, event)` 与 `emit` 的区别很明确：**不调 `_prepare_event`**（因此事件 id、`emission_sequence` 等字段保持原值）、**不重新记录**，并通过 `_replaying` 这个 ContextVar 置真，让处理器能用 `is_replaying()` 判断要不要跳过副作用（写检查点、调外部 API）。

## 关键取舍

**源码事实：** 同步处理器跑在 10 线程的池子里，意味着同步处理器的执行顺序在多处理器时不确定。只有流式块事件被特殊处理成同线程串行。所以「处理器按登记顺序执行」这条直觉在本实现里只对同步块事件成立。

**源码事实：** 处理器异常永不外抛。这让「一个有 bug 的监听器」不会拖垮整个运行，代价是错误只能从控制台输出里看到——除非另有一条链路把它变成事件。

**源码事实：** `emit` 的返回类型是 `Future | None`，四种路径返回的 future 种类不同（线程池 future / asyncio future）。注释里也列了这一点，调用方若想等完成，得用 `flush()` 或者自行 `asyncio.wrap_future`。

**源码事实：** 运行时状态存活在 ContextVar 里，所以在**同一个线程的不同上下文**中，总线看起来可能有不同的运行状态。而处理器跑在线程池或后台循环里，上下文是 `copy_context()` 复制过去的那份——所以处理器能看到发射时那一刻的状态，而不是「全局最新」。这是刻意的快照语义。

**源码事实：** `_prepare_event` 会在发射阶段**改动事件对象的字段**（`previous_event_id`、`parent_event_id`、`emission_sequence` 等）。这也是 `replay` 必须绕开它的原因：重放已记录的事件不能改它的历史字段。

**架构推论：** 「同步处理器进线程池、异步处理器进独立循环」是为同一个问题做的两种妥协。业务代码里同步的监听器常做阻塞 I/O（写文件、发请求），直接在调用线程跑会阻塞 agent；异步监听器需要一条活的循环，而业务通常跑在同步代码里，所以必须有一条常驻循环。两个池子换来的是「随便怎么写都不卡主流程」，代价是执行顺序不可控。

**架构推论：** 依赖机制（`Depends` + 分层计划）本质上是给「多个监听器之间有顺序要求」提供一个显式表达，而不是靠登记顺序碰运气。它在事件系统里属于罕见能力——多数框架的监听器是无序集合。有了它，「先写检查点再发外部通知」这类要求才能被可靠表达。

**架构推论：** 「最外层进入时造状态、最外层退出时清空」用深度计数实现，等于把运行状态的生命周期绑定到**调用栈**而不是某个显式对象。好处是嵌套 kickoff（crew 里套 flow、flow 里套 crew）自然共享同一份状态；坏处是任何忘记退出的路径都会让状态泄漏到后续运行。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus` | 单例总线：处理器表、线程模型、运行时状态 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.on` | 登记装饰器，支持 `depends_on` |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.emit` | 四条分派路径，返回不同类型的 future |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus._prepare_event` | 补齐因果字段、维护作用域栈、记录与流式发布 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus._emit_with_dependencies` | 按分层执行计划调度，计划按事件类型缓存 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.set_runtime_state` | 挂运行状态并登记实体 id |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus._enter_runtime_scope` | 深度计数的运行作用域进入 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.register_entity` | 实体登记，crew 下属 agent 不重复进 root |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.replay` | 重放已记录事件，不改字段不重记录 |
| `lib/crewai/src/crewai/events/event_bus.py` | `is_replaying` | 判定当前是否在重放，供有副作用的处理器跳过 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.flush` | 等待所有 pending future 完成 |
| `lib/crewai/src/crewai/events/event_bus.py` | `CrewAIEventsBus.scoped_handlers` | 临时替换全部处理器的上下文管理器 |

## 参考

- [CrewAI 源码：events/event_bus.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/events/event_bus.py)
- [CrewAI 文档：Event Listeners](https://docs.crewai.com/en/concepts/event-listener)
- [Python 文档：contextvars](https://docs.python.org/3/library/contextvars.html)

## 与主核 / AI 的关系

主核里最接近的是「钩子链 + 后台工作线程」的组合：扩展点按类型登记，执行时按固定顺序调用，重活交给后台。差别在于主核的钩子顺序是编译期固定的，且钩子出错通常必须上报；这里顺序由依赖图决定、错误全吞。这是「业务可观测性」与「内核正确性」的取舍差异——观测挂掉不该影响业务，而内核回调挂掉必须让人知道。

与 AutoGen 相比，AutoGen 的 `event_logger` 只是标准库 logging 的一个命名通道：发事件就是写一条日志，没有同步/异步处理器分流、没有依赖分层、没有重放、也没有把运行状态挂上去。CrewAI 这套总线更像一个内嵌的消息系统：处理器有线程模型、有顺序约束、有事件记录图、还支持重放已记录事件。代价是复杂度和线程数（一条常驻循环线程加一个 10 线程池）。

对 AI 工程最实用的一条是「处理器异常 fail-open」。做 tracing 或 metrics 时，最怕的是观测代码在业务路径上抛出异常，把一次本来成功的运行变成失败。这里的处理方式是全部捕获并打印，业务不受影响。要做同类系统，这条几乎是必需的默认。

另一条是 `replay` 与 `is_replaying()` 的配对。重放历史事件去重建追踪时间线，与「事件触发了副作用」这两件事必须能区分：前者希望处理器照常跑（渲染时间线），后者必须跳过（别把检查点再写一遍、别把请求再发一次）。用一个 ContextVar 标记 + 让处理器自己判断，比在总线上按处理器类型白名单更灵活——因为同一个处理器可能两种情况都要处理。

最后是那个流式块事件的特例。它揭示了一个通用约束：**只要事件有严格的顺序语义，就不能丢进不保序的并发池**。把特例写成「这类事件在调用线程内同步执行」，比让所有处理器都变同步要省得多。
