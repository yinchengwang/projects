---
project: langgraph
feature: pregel-runner
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_runner.py
    symbols: [PregelRunner, commit, _panic_or_proceed]
---

# PregelRunner 并发执行与提交

## 一句话职责

`PregelRunner` 负责把「这一步的任务集合」并发跑完：它给每个任务提交一个后台 future，边完成边把结果提交为挂起写入（`commit`），在第一个致命失败时取消其余任务，并在收尾时用 `_panic_or_proceed` 决定是抛异常还是正常返回。

## 机制是什么

`tick()` 是一个生成器，不是普通函数。它把「等待」与「产出控制权」交替进行：每次 `yield` 都表示「现在可以把我已产生的输出流出去」，调用方（`Pregel.stream`）在每次 `yield` 时把队列里的事件发出去。这个设计让流式输出不需要额外线程，也不需要把「推送」回调进 runner。

它的输入是任务列表与几个可注入的钩子：`submit`（提交到执行器）、`put_writes`（写入通路）、`schedule_task`（动态派发新任务，即 `loop.accept_push`）、可选的 `get_waiter`（等到流队列有内容的 future）、`timeout`、`reraise`。它不持有检查点、不持有通道表，因此可以在同步与异步两套执行环境里复用同一套控制流（`tick` / `atick` 是两份实现，逻辑逐段对应）。

## 怎么实现的

**三条执行路径，按任务数分流。**

- 零任务：`yield` 一次就返回。这一次 `yield` 让调用方把上一步残留的事件发出去。
- 单任务且无超时、无 waiter：走「快路径」，直接在当前线程 `run_with_retry`，跑完 `commit`。这条路径省掉线程池调度与 future 协调，是最常见的情形（线性 agent 图每步只有一个节点）。
- 其他情况：走「调度路径」，把每个任务 `submit` 成 future 放进 `FuturesDict`。

**快路径的异常处理有四处细节。** 第一，失败时先 `commit(t, exc)` 再判断，保证错误也被记录成写入。第二，若异常是 `GraphBubbleUp`（中断的基类）就不路由错误处理器；否则若该节点配了处理器，登记异常 id 到 `_handled_exception_ids`（这样后续的 stop/panic 检查不会把它当致命错误），再调 `schedule_error_handler` 拿一个处理器任务，把 `tasks` 换成 `(handler_task,)` 继续走调度路径。第三，`reraise` 且 futures 非空时，构造一个已设异常的假 future 塞进 `done`/`failed`，让收尾的 `_panic_or_proceed` 抛出来。第四，`reraise` 且 futures 为空时先 `yield` 一次再抛——源码注释是「让调用方先把这个任务的结果流出去再抛」。

抛出前还有一段「剥栈帧」逻辑：从 traceback 里跳过所有位于框架内部文件（retry、runner、executor、runnable、langchain_core config、concurrent.futures）的尾帧，让用户看到的 traceback 起点是自己的节点函数。

**调度路径的协调靠 `FuturesDict` 的事件。** 每个 future 完成时回调 `on_done`，它调 `commit`，然后递减计数器；计数器归零或 `should_stop(failed)` 为真时置位 event。主循环是 `concurrent.futures.wait(futures, FIRST_COMPLETED)`，逐个处理完成的 future，每个 future 处理完就 `yield` 一次，让流输出有机会发出。

**错误处理器在调度路径里是「换任务」。** 若完成的任务异常、不是 `GraphBubbleUp`、且该节点配了处理器，则登记异常 id、把 future 加进 `SKIP_RERAISE_SET`（执行器退出时不重抛它）与 `handled_futures`，然后 `submit` 一个处理器任务替换它。所以「节点失败 → 处理器接手」在调度层就是「一个 future 换成一个新 future」，不打断整体等待结构。

**`should_stop` 与 `_should_stop_others` 的判据。** 一个 future 的异常要能触发「取消其余任务」，必须同时满足：不是已处理的异常 id、不是 `GraphBubbleUp`、不是 `is_invalid_resume`、不在 `SKIP_RERAISE_SET` 里。把 invalid resume 排除是有明确理由的（注释写明）：它在其他任务跑完之后才抛，这样在同一次 resume 里被一起答复的任务不会被中途掐断又重跑。

**`commit` 是唯一的「任务结果落地」入口。** 它按异常类型分四支：

- `asyncio.CancelledError`：给任务补一条 `(ERROR, exc)` 写作，落盘。理由是「取消也要让 loop 能结束这个超步」——否则被取消的任务在恢复时看不出发生过什么。
- `GraphInterrupt`：**仅当** `exception.args[0]` 非空时，写 `(INTERRUPT, 载荷)`，并把该任务已有的 `RESUME` 写入一起带上。空载荷的中断不写盘（它是「流程控制式中断」，由 loop 侧抛）。
- 其他 `GraphBubbleUp`：什么都不做，注释写明「将在 `_panic_or_proceed` 里抛」。
- 普通异常：补 `(ERROR, exc)`；若该节点配了错误处理器，再补 `(ERROR_SOURCE_NODE, task.name)` 并把异常 id 记为已处理。这两条写入是恢复期「该任务已失败且已路由给处理器」的唯一证据。
- 无异常：先按需调 `node_finished`（`TAG_HIDDEN` 的任务不算「节点完成」）；若该任务**全部**写入都是控制写入，补一条 `(NO_WRITES, None)`——这是「任务跑过但没有产出」的标记，`read_task_statuses` 靠它判定任务已完成。

最后一律 `put_writes(task.id, task.writes)`。

**`_panic_or_proceed` 是收尾裁决。** 它把传入的 future 集合分成 done 与 inflight 两组，逐个检查 done 里的异常：已在 `handled_futures` 或 `handled_exception_ids` 里的跳过；否则取消所有 inflight，然后在 `panic` 为真时按类型处理——`GraphInterrupt` 收集起来、其他异常立刻抛。收集完后若有中断，合成一个新的 `GraphInterrupt` 抛出（把多个任务的中断载荷合并成一个元组）。若没有异常但有 inflight（说明是超时），取消它们并抛 `TimedOut`（异步版抛 `asyncio.TimeoutError`）。

## 关键取舍

**源码事实：** `tick()` 是生成器而不是「执行 + 回调」。这使流式输出与执行在同一个线程里交替推进，不需要把写队列做成线程安全的；代价是调用方必须记得在每次 `yield` 时把队列抽空，否则事件会积压。

**源码事实：** 单任务走快路径，与多任务走完全不同的代码。两条路径的异常语义必须手工保持同步（错误处理器、reraise、剥栈帧在三处重复出现）。这是性能换取的可维护性代价：快路径省掉线程池开销，但每加一条语义都要改三份代码。

**源码事实：** 「处理器接手的异常」靠一个模块级的 `SKIP_RERAISE_SET` 弱集合共享。执行器在退出时据此决定不重抛。这个集合是跨 runner 实例的全局状态，用 `WeakSet` 避免泄漏，但也意味着两棵并发运行的图共享同一份记录——这一点没有额外隔离。

**源码事实：** `_panic_or_proceed` 对中断的处理是「收集后合并」，而不是「遇到第一个就抛」。因此同一超步里多个任务同时中断时，用户拿到的是一个包含全部中断的 `GraphInterrupt`，一次 resume 可以同时答复它们。这直接决定了多中断的响应协议。

**架构推论：** runner 只依赖「提交」「写入」「派发」三个钩子，不认识通道、检查点、图的形状。这使它能被 `Pregel` 之外的调用方复用（源码里 `CONFIG_KEY_RUNNER_SUBMIT` 允许从 config 换掉 submit），也让同步/异步两份实现可以共享同一套控制流逻辑。

**架构推论：** 「任务跑完但没产出」必须显式写 `NO_WRITES`。原因是恢复语义把「有输出写入」等同于「已完成」；若允许空输出，一个无副作用的过滤节点每次恢复都会被判定为未完成而重跑。这条标记把「完成」与「有产出」解耦。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_runner.py` | `PregelRunner` | 并发执行一期任务，边完成边提交 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `PregelRunner.tick` | 同步生成器：快路径 + 调度路径 + 收尾裁决 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `PregelRunner.atick` | 异步对应实现 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `commit` | 按异常类型把任务结果写成挂起写入 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_panic_or_proceed` | 取消 inflight、合并中断、抛超时 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_should_stop_others` | 判定某失败是否应取消其余任务 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_call` / `_acall` | 函数式调用的动态任务派发 |

## 参考

- [LangGraph 源码：pregel/_runner.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_runner.py)
- [LangGraph 源码：pregel/_executor.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_executor.py)
- [LangGraph 文档：并发与 max_concurrency](https://langchain-ai.github.io/langgraph/concepts/low_level/#max-concurrency)

## 与主核 / AI 的关系

「并行执行、成功后统一提交、一个失败就取消其余」与主核的并行 I/O 批次是同一套控制流：都要求「失败不能留下部分可见的结果」，因此都依赖「提交动作与执行动作分离」。差别在于主核的取消受限于内核的 I/O 抽象，取消后必须回滚页锁；这里的取消只是 future 层面的 `cancel()`，已经写下的挂起写入并不会被撤销——所以「取消」在这里不等于「回滚」。

对 AI 侧而言，这一页解释了三个常见现象的来源。其一，同一步里多个工具调用并行跑，任何一个抛非中断异常都会让其余被取消，因此工具应尽量自己吞掉可恢复错误。其二，节点抛异常后若有 `error_handler`，该异常不会再冒泡到调用方，且执行器退出时不会重抛——这正是处理器能「接住」失败的原因。其三，同一超步多个节点同时 `interrupt` 会合并成一次中断事件，调用方需要按 id 逐条答复。

