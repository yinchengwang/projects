---
project: haystack
feature: pipeline-run
title: 管道调度与流式运行
page_type: feature
schema_version: 1
refs:
  - path: haystack/core/pipeline/base.py
    symbols: [PipelineBase, ComponentPriority]
  - path: haystack/core/pipeline/pipeline.py
    symbols: [Pipeline, PipelineStreamHandle]
---

# 管道调度与流式运行

## 一句话职责

`PipelineBase` 持有组件图并负责「算优先级、选下一个能跑的组件、把输出写回下游 socket」；`Pipeline` 在上面实现三条执行路径——同步循环 `run`、异步并发 `run_async_generator`（带 `concurrency_limit` 的 `Semaphore`）、以及流式 `stream`（返回 `PipelineStreamHandle`，把组件的 `StreamingChunk` 逐块吐给消费者）。调度的核心是一个 `ComponentPriority` 四级优先队列，而不是拓扑序列表。

## 机制是什么

haystack 的执行模型有一个前提与别的框架不同：**它不按拓扑序遍历组件，而是每一轮问「现在哪个组件最该跑」**。

这与 a03 的就绪判定直接相连——判定函数给出「能不能跑」，而这里要回答的是「能跑的里面先跑哪个」。答案是一个四级枚举：

| 优先级 | 含义 | 语义 |
|---|---|---|
| `HIGHEST` | 有贪婪 socket 就绪 **且** 全部 socket 就绪 | 可以独占运行（sync 路径直接跑，async 路径进隔离模式） |
| `READY` | 所有前置组件都已执行 | 正常并发调度 |
| `DEFER` | 能跑但在等可选输入 | 让位给 READY |
| `BLOCKED` | 根本不能跑 | 终止或等任务 |

**为什么必需四级而不是两级？** 因为「能跑」与「该跑」之间存在一个真实的中间态：一个组件的必填输入到了、但它还有一个**接了上游的可选 socket** 尚未填满（a03 里 `are_all_sockets_ready` 的第二种口径就是为此存在）。这时它确实能跑，但立刻跑就会拿到半份输入。`DEFER` 就是为这个状态设的：**让它排队但不优先**，等真正 READY 的都跑完，它的可选输入自然也就齐了。

`HIGHEST` 的语义更特别：它要求「贪婪 socket 就绪」——贪婪 socket 的语义是「来一个就跑」，于是这种组件**不该等**。而它可能与其他 READY 组件并发冲突（它会消费共享输入），所以源码给它「必须独占运行」的待遇。

## 怎么实现的

**优先级计算只有四行，但每行的成立条件都依赖 a03 的判定函数。**

```
if not can_component_run(comp, comp_inputs):
    return ComponentPriority.BLOCKED
if is_any_greedy_socket_ready(comp, comp_inputs) and are_all_sockets_ready(comp, comp_inputs):
    return ComponentPriority.HIGHEST
if all_predecessors_executed(comp, comp_inputs):
    return ComponentPriority.READY
return ComponentPriority.DEFER
```

**注意第一行上方的注释**：

> Even if a component can run, it doesn't mean it's ready to run since it could be waiting for optional inputs. This is why it's only used to determine if a component is BLOCKED or not.

也就是说 `can_component_run` 只用来区分「绝对不能跑」与「可能能跑」，`DEFER` 与 `READY` 的区别交给 `all_predecessors_executed`。这个分工很关键：**「能不能跑」与「该不该现在跑」被彻底分离**，于是两个判定函数各自可以独立演化。

`HIGHEST` 的两个条件是**与**关系：既要有贪婪 socket 就绪，又要全部 socket 就绪。第二项看似多余（贪婪都就绪了，为什么还要全部就绪？），实际必要——如果还有别的必填 socket 空着，那这个组件仍然跑不了，`can_component_run` 已经返回假了。这行代码读起来像冗余，实际是「显式写出前提，让读者不必回头查 `can_component_run` 的实现」。

而源码注释还标出了这个枚举值的两处使用点：

> This priority is explicitly used in the async run path + implicitly in `_is_queue_stale` (Implicit b/c it checks via ">" operator if there is a component with HIGHEST priority)

**`_is_queue_stale` 用「队首优先级 > READY」来判断队列该不该重算。**

```
return len(priority_queue) == 0 or priority_queue.peek()[0] > ComponentPriority.READY
```

因为 `ComponentPriority` 是 `IntEnum`（`HIGHEST=1`、`READY=2`、`DEFER=3`、`BLOCKED=4`），所以「队首 > READY」精确地表达「队首是 DEFER 或 BLOCKED」，也就是「已经没有能立刻跑的了」。文档注释把这个比较逐例展开解释了一遍——这种把「为什么这个不等式成立」写进 docstring 的做法很值得记，因为**依赖枚举数值大小关系的代码是最容易在读时误解的**。

sync 路径的循环里，这个检查只在「跑完一个组件之后」调用：

```
component_pipeline_outputs = self._write_component_outputs(...)
if component_pipeline_outputs or component_name in include_outputs_from:
    pipeline_outputs[component_name] = component_pipeline_outputs
if self._is_queue_stale(priority_queue):
    priority_queue = self._fill_queue(ordered_component_names, inputs, component_visits)
```

**「只在必要时重算」是 sync 路径的性能点**：一次 `_fill_queue` 要为所有组件算一次优先级（每个都要跑 a03 那套判定），而组件输出通常只影响下游少数几个。队列还新鲜就继续用旧的。

**反过来，async 路径每一轮都重建队列，而且注释明确解释了为什么不能省。**

```
# We rebuild the priority queue every iteration: each iteration waits for one or more concurrent
# tasks to finish, which mutates `inputs` and can change many components' priorities at once, so
# we rebuild to give every scheduling decision an up-to-date view.
priority_queue = self._fill_queue(ordered_component_names, inputs, component_visits)
```

**这是并发带来的必然代价**：一次 `wait_for_tasks` 可能同时结束多个任务、批量填充多个组件的 socket，于是「哪些组件变得可跑」不再能增量推断。sync 路径一次只跑一个组件，输出影响范围小，增量判断成立；async 路径一次可能完成 N 个，只能全量重算。**两条路径的差异不是偷懒，而是正确性要求不同。**

**sync 路径的终止条件是「队首是 BLOCKED 就 break」。**

```
if priority == ComponentPriority.BLOCKED:
    if self._is_pipeline_possibly_blocked(current_pipeline_outputs=pipeline_outputs):
        self._find_components_blocking_pipeline(priority_queue=..., component_visits=..., inputs=inputs)
    break
```

注意它先判断「管道是否**可能**被卡住」，若是则调 `_find_components_blocking_pipeline` 做一次深度诊断（打警告、列出阻塞者）。**「可能」这个词很重要**：BLOCKED 也可能只是「剩下的组件都跑完了」（输入被消费光，于是它们全都 BLOCKED）。所以要再判断一次「是不是真的卡住了」——判据是「还有没跑过的组件」这类信息。这个二次判断避免了「正常结束时打一堆阻塞警告」。

`DEFER` 的处理是 `_tiebreak_waiting_components`：

```
if len(priority_queue) > 0 and priority == ComponentPriority.DEFER:
    component_name, topological_sort = self._tiebreak_waiting_components(
        component_name=component_name, priority=priority, priority_queue=priority_queue,
        topological_sort=cached_topological_sort)
```

**平手如何决断？用拓扑序。** `_topological_sort()` 的 docstring 说明它对有环图用**强连通分量的凝聚图**：同一环内的组件共享同一优先级、再按名字字典序打破；环之间按拓扑序。这是一个「同一优先级内部仍要确定性」的设计——**调度必须是确定的**，否则同一个管道跑两次可能产生不同的组件执行顺序，调试与复现都会失效。

`cached_topological_sort` 的缓存逻辑也值得一提：它只在 `DEFER` 分支里被赋值与使用，是「惰性计算 + 本次运行内复用」的形态（拓扑序不随运行变化）。

**`max_runs_per_component` 的检查放在「取下一个组件」里，而不是跑完之后。**

```
item = priority_queue.get()
if item is None:
    return None
component_name = item[1]
comp = self._get_component_with_graph_metadata_and_visits(component_name, component_visits[component_name])
# Only raise the max run count error if the component is not blocked, since if it's blocked it means it
# can't run anyway.
if item[0] < ComponentPriority.BLOCKED and comp["visits"] >= self._max_runs_per_component:
    msg = f"Maximum run count {self._max_runs_per_component} reached for component '{component_name}'"
    raise PipelineMaxComponentRuns(msg)
return ComponentPriority(item[0]), component_name, comp
```

三点：

1. **默认 100**（`__init__` 的 `max_runs_per_component: int = 100`）。它是**循环的唯一终止保障**——管道图里可以有环，没有这个上限，一个「永远喂不满懒惰可变 socket」的环会无限跑。
2. **`item[0] < BLOCKED` 这个前置条件**：BLOCKED 的组件本来就跑不了，不该因为它 visits 计数高而报错。少了这个条件，一个「跑满 100 次后彻底阻塞」的组件会让整条管道在收尾时报错——而正常收尾不该报错。
3. **`visits` 不存进图里，而是每次访问时注入。**

```
comp_dict = self.graph.nodes[component_name]
# We inject the visits into the component dict here to avoid storing it in the graph, which would prevent
# thread-safe execution
return {**comp_dict, "visits": visits}
```

这条注释是**并发正确性的核心**：`graph.nodes` 是共享可变结构，若把 `visits` 写进去，两个并发运行同一管道的线程会互相污染计数。改成「调用时临时拼一个 dict」，图就退化成只读，而 visits 是每次运行的局部变量。a03 里那句「visits 存在组件元数据里」在这里得到了补充——**它是「看起来在元数据里，实际是临时拼进去的」**。

**组件执行的统一外壳做四件事：断点检查、tracing span、异常包装、输出校验。**

```
if (isinstance(break_point, Breakpoint)
    and break_point.component_name == component_name
    and break_point.visit_count == component_visits[component_name]):
    raise BreakpointException.from_triggered_breakpoint(break_point=break_point)

instance: Component = component["instance"]
with PipelineBase._create_component_span(component_name=..., instance=instance, inputs=inputs, parent_span=parent_span) as span:
    try:
        outputs = instance.run(**inputs_copy)
    except BreakpointException as error:
        raise error
    except PipelineRuntimeError as runtime_error:
        raise runtime_error
    except Exception as error:
        raise PipelineRuntimeError.from_exception(component_name, instance.__class__, error) from error

    component_visits[component_name] += 1
    if not isinstance(outputs, Mapping):
        raise PipelineRuntimeError.from_invalid_output(component_name, instance.__class__, outputs)
    _validate_component_output_keys(component_name, component, outputs)
    ...
return outputs
```

五处细节：

1. **断点检查在组件运行之前**，且要求 `visit_count` 也匹配——于是同一个组件在循环中的第 3 次执行可以被单独断住。a06 会展开这一点。
2. **两类异常原样重抛**：`BreakpointException`（断点要冒泡到顶层，不能被包成通用运行错误）与 `PipelineRuntimeError`（组件内部可能已经构造了带上下文的错误，例如 Agent 的快照，再包一层会丢信息）。这两个 `except` 分支存在的理由在注释里写得很清楚——**它们是「不包装」的显式声明**。
3. **其余异常包成 `PipelineRuntimeError.from_exception(...)`，并且 `from error` 保留因果链**，于是堆栈里能同时看到「管道说哪个组件炸了」与「组件内部的原始异常」。
4. **`component_visits[component_name] += 1` 在输出校验之前**。也就是说一个「输出校验失败」的组件仍然消耗了一次 visit 计数。
5. **输出必须是 `Mapping`**，且键要与声明的输出 socket 一致（`_validate_component_output_keys`）。这是 a01 契约的运行期对偶：输入 socket 从签名反推，输出键在运行后校验。

**async 路径的核心是「信号量 + 在飞任务表 + 已排程集合」三件套。**

```
ready_sem = asyncio.Semaphore(concurrency_limit)
running_tasks: dict[asyncio.Task, str] = {}
scheduled_components: set[str] = set()
```

三者的分工：

- **`ready_sem`**：控制并发度。`concurrency_limit` 默认 4，且 `if concurrency_limit < 1: raise ValueError`。
- **`running_tasks`**：`Task → 组件名` 的映射。用 `Task` 作键是为了能在 `wait` 返回后知道「完成的是哪个组件」。
- **`scheduled_components`**：已排程未完成的组件名集合。**这个集合解决一个真实问题**：`_consume_component_inputs` 在清理输入时**会刻意保留「来自管道外」的输入**（pruned 逻辑里 `sock["sender"] is None` 的那些），于是该组件的就绪判定在消费后依然可能为真——若没有这道闸，同一个组件会在下一次循环里被再次排程，与自己在飞的那份执行抢同一批输入。`_schedule_component` 开头就是这个检查（`if component_name in scheduled_components: return`），而调度循环里也有一支专门「已在集合里就等任务」。

`_schedule_component` 里有一处顺序安排值得单独记：**输入是在创建 task 之前同步消费的。**

```
scheduled_components.add(component_name)
component = self._get_component_with_graph_metadata_and_visits(...)
component_inputs = self._consume_component_inputs(component_name, component, inputs)
component_inputs = self._add_missing_input_defaults(component_inputs, component["input_sockets"])
async def _runner() -> Mapping[str, Any]:
    async with ready_sem:
        ...
task = asyncio.create_task(_runner())
```

docstring 写明理由：「输入在这里同步消费，是为了让同一轮调度里后续被排程的组件看到更新后的输入状态，不至于为同一批输入相互竞争」。若把消费放进 `_runner` 里，两个在同一轮被排程的组件可能都认为自己拿到了同一个上游的输出。

**调度循环的骨架是「重建队列 → 取候选 → 按四种情形分派」。**

四种情形与 sync 路径的对应关系：

| 情形 | 处理 | 与 sync 的差异 |
|---|---|---|
| 无候选或有候选但 BLOCKED **且** 有在飞任务 | `wait_for_tasks(FIRST_COMPLETED)` 后 `continue` | sync 没有并发，直接 break |
| 无候选且无在飞任务 | `break` | 相同 |
| 候选的 BLOCKED **且** 无在飞任务 | 诊断后 `break` | 相同 |
| 组件已在 `scheduled_components` 里 | `wait_for_tasks(FIRST_COMPLETED)` 后 `continue` | sync 无此情形 |
| `HIGHEST` | `_run_component_in_isolation(...)` | 独占执行 |
| `READY` | `_schedule_component(...)`，然后**继续偷看队列**把更多 READY 一起排上 | sync 一次只跑一个 |
| `DEFER` 且无在飞任务 | 平手决断后 `_schedule_component(...)` | 相同 |

**那个「继续偷看队列」的内层循环是 async 路径的并发来源。**

```
while len(priority_queue) > 0 and not ready_sem.locked():
    peek_priority, peek_name = priority_queue.peek()
    if peek_priority != ComponentPriority.READY:
        # We stop scheduling: the next component is BLOCKED (can't run), HIGHEST (must run
        # alone), or DEFER (waiting for more inputs - we only schedule it once it becomes READY).
        break
    priority_queue.pop()
    self._schedule_component(...)
```

三个终止条件各有理由：**队列空**（没得排了）、**信号量已满**（并发度用完）、**队首不是 READY**（注释逐项列出三种：BLOCKED 跑不了、HIGHEST 必须独占、DEFER 要等它变成 READY）。也就是说：**一次只有一个 READY 组件被显式调度，其余同类靠这个 peek 循环批量抓取**，且严格尊重并发上限。

而 `DEFER` 的调度条件是 `not running_tasks`——**只有在没有任何任务在飞时才跑 DEFER 组件**。这与 sync 路径的「DEFER 让位于 READY」语义一致，只是并发版本里「让位」表现为「等所有在飞任务结束」。

**`_wait_for_tasks` 是两条路径共用的「收割器」。**

```
async def _wait_for_tasks(running_tasks, scheduled_components, *, return_when):
    ...
```

它按 `return_when`（`FIRST_COMPLETED` 或 `ALL_COMPLETED`）等任务，然后逐个收结果、写输出、从两个集合里移除，并把产生的 pipeline 输出 yield 出去。**`_wait_for_tasks` 是 async 生成器（`AsyncIterator`），因为它要把「某个组件完成后的部分输出」逐个交给调用方**——在 `run_async_generator` 里这些被 `yield` 给上层消费者，在 `stream` 里最终进了队列。

末尾那段「安全网」的注释很值得记：

```
# Safety net: drain any leftover tasks once the scheduling loop has finished. With the current loop
# both `break` paths require `running_tasks` to be empty, so this is a no-op. We keep it so that a
# future change adding a `break` that leaves tasks in flight doesn't lose outputs.
```

**它当前是死代码，但被有意保留**，理由是「未来若有人加一条会留下在飞任务的 `break`，不该丢输出」。这是一个诚实标注的死代码——比悄悄删掉或悄悄留着都好。

`finally` 块的清理同样有注释豁免说明：

```
finally:
    # If iteration is abandoned early ... cancel any tasks still in flight so they don't leak.
    # This is a no-op on normal completion and on a component error, since no tasks are left running by then
    await self._cancel_in_flight_tasks(running_tasks, scheduled_components)
```

**`stream` 返回的 `PipelineStreamHandle` 是一个「队列 + 任务 + 迭代即清理」的三件套。**

```
class PipelineStreamHandle:
    _END_OF_STREAM: ClassVar[_EndOfStream] = _EndOfStream()
    _CLEANUP_TIMEOUT_SECONDS: ClassVar[float] = 1.0

    def __init__(self, queue, task, cancel_on_abandon: bool = True):
        self._queue = queue
        self._task = task
        self._cancel_on_abandon = cancel_on_abandon
```

四个设计点：

**第一，`stream()` 不返回异步迭代器，而返回一个 `PipelineStreamHandle`。** 这个 indirection 换来了两样东西：一个 `.result` 属性（拿到最终输出）与一个 `aclose()`（显式清理）。若直接返回 async generator，这两样就只能靠 `StopAsyncIteration` 之后的副作用来拿，而且**无法显式取消**。

**第二，`_END_OF_STREAM` 是一个类型化哨兵，不是 `None`。**

```
class _EndOfStream:
    """Sentinel type indicating no more chunks will arrive on the stream."""
```

理由与 a03 的 `_NoOutputProduced` 相同：`StreamingChunk` 可能为 `None` 吗？可能不为 None，但用哨兵能表达**类型上的区分**（`asyncio.Queue["StreamingChunk | _EndOfStream"]`）。于是队列里出现的任一元素都能无歧义判定。

**第三，`__aiter__` 是异步生成器函数，`try/finally` 保证放弃迭代时也被清理。**

```
async def __aiter__(self):
    try:
        while True:
            item = await self._queue.get()
            if item is self._END_OF_STREAM:
                await self._task  # called to make exceptions surface
                return
            yield cast(StreamingChunk, item)
    finally:
        if self._cancel_on_abandon:
            await self.aclose()
```

**`await self._task` 那一行是异常传播的关键**：流水线任务若抛异常，异常存的是 `Task` 内部，而不是队列里。发现流结束时 `await` 它，异常就会在消费者这一侧抛出——而不是被静默吞掉。**若没有这一行，一个失败的流会以「正常结束」的形态呈现给消费者。**

docstring 里还解释了为什么 `__aiter__` 要写成异步生成器函数：**「每次 `async for` 得到一个新的生成器，`try/finally` 在退出时执行」**。若写成返回一个 `AsyncIterator` 对象的普通方法，`finally` 就不会在 `async for` 结束时自动执行。

**第四，`result` 属性把三种状态变成三种异常。**

```
if not self._task.done():
    raise RuntimeError("Pipeline has not finished; iterate the handle first.")
if self._task.cancelled():
    raise RuntimeError("Pipeline was cancelled; no result available.")
exc = self._task.exception()
if exc is not None:
    raise exc
return self._task.result()
```

**「没跑完」「被取消」「失败了」三种情况都不返回 `None` 或空字典，而是分别抛不同的错。** 这让「取结果」这个动作不可能静默失败——尤其 `raise exc` 把组件的原始异常重抛，于是消费者拿到的错误与 `run()` 完全一致。

**第五，`aclose()` 有一个超时上界。**

```
if not self._task.done():
    self._task.cancel()
    with contextlib.suppress(BaseException):
        await asyncio.wait_for(self._task, timeout=self._CLEANUP_TIMEOUT_SECONDS)
```

**`_CLEANUP_TIMEOUT_SECONDS = 1.0` 是一个「组件不能无限期阻塞清理」的硬保证。** 注释写明理由。而 `contextlib.suppress(BaseException)` 吞掉一切（包括 `CancelledError` 与超时的 `TimeoutError`）——清理阶段不该再抛异常打扰调用方。源码注释也提到，对「只能同步执行的组件」，async 路径会 `asyncio.to_thread` 派发，而**运行中的线程无法被中断**，因此取消只是「放弃等待」而不是「真正停下」。

**`stream` 的 chunk 是怎么进入队列的？靠给组件注入 `streaming_callback`。**

docstring 说明：

> For every async-capable component that exposes a `streaming_callback` input socket, a forwarder is injected at runtime that pushes chunks onto the handle's queue. If a `streaming_callback` is provided at component init or at runtime (inside `data`, e.g. `data={"llm": {"streaming_callback": cb}}`), it is also invoked for each chunk. Async callbacks are preferred; a sync callback is accepted but will run synchronously on the event loop and may block it.

三个要点：

1. **注入点是 `streaming_callback` 这个输入 socket**——也就是说「这个组件能不能流式」不是靠类型声明，而是靠**有没有这个 socket**。
2. **用户自己传的 callback 与注入的 forwarder 会同时生效**，不是一个覆盖另一个。
3. **同步 callback 会在事件循环上同步执行**，可能阻塞——这是个明确的性能警告，而不是错误。

`streaming_components` 参数则是「显式指定哪些组件要走流式」，docstring 里给出的示例还带了 `concurrency_limit` 与 `cancel_on_abandon` 的组合用法。

## 关键取舍

**源码事实：** 调度用四级 `IntEnum`（`HIGHEST=1` / `READY=2` / `DEFER=3` / `BLOCKED=4`），而不是拓扑序列表；`_calculate_priority` 只把 `can_component_run` 用于区分「绝对跑不了」，`READY` 与 `DEFER` 的区分交给 `all_predecessors_executed`。

**源码事实：** `DEFER` 是为「必填齐了但接了上游的可选 socket 还没填满」这个中间态设的：能跑但跑早了会拿半份输入，于是让它排队但不优先。

**源码事实：** `_is_queue_stale` 用 `len(priority_queue) == 0 or priority_queue.peek()[0] > ComponentPriority.READY` 判定，依赖 `IntEnum` 数值大小；docstring 把这个不等式的两种情形逐例展开。

**源码事实：** sync 路径只在「跑完一个组件后」检查队列是否过时、必要时才 `_fill_queue`；async 路径**每一轮都全量重建**，注释说明理由是「一次 `wait_for_tasks` 可能同时完成多个任务、批量改优先级，无法增量推断」。

**源码事实：** `HIGHEST` 必须独占运行（async 走 `_run_component_in_isolation`）；`READY` 一次显式调度后可继续 `peek` 队列批量抓取，但严格受 `ready_sem`（`concurrency_limit`，默认 4）与「队首必须是 READY」两条限制。

**源码事实：** `DEFER` 只在 `not running_tasks` 时被调度。

**源码事实：** `max_runs_per_component` 默认 100，检查在「取下一个组件」时做且带 `item[0] < ComponentPriority.BLOCKED` 前置条件；它是含环管道的唯一终止保障。

**源码事实：** `visits` **不**存进 `graph.nodes`，而是每次访问时临时拼进组件 dict，注释说明理由是「存进图里会破坏线程安全 / 重入」。

**源码事实：** 组件执行外壳先检查断点（组件名 **且** visit_count 都匹配），然后对 `BreakpointException` 与 `PipelineRuntimeError` **原样重抛**（注释说明是为了不丢失内层上下文），其余异常包成 `PipelineRuntimeError.from_exception(...) from error`；`visits` 自增发生在输出校验之前；输出必须是 `Mapping` 且键要与声明的输出 socket 一致。

**源码事实：** `PipelineStreamHandle` 用类型化哨兵 `_EndOfStream` 标记流结束；`__aiter__` 是异步生成器函数以便 `try/finally` 在放弃迭代时也执行清理；`await self._task` 是为了让流水线任务的异常浮出来（否则失败的流会看起来正常结束）；`result` 对「未完成 / 已取消 / 失败」三种状态分别抛错并重抛原始异常；`aclose()` 有 1 秒超时上界并用 `suppress(BaseException)` 吞掉清理期异常。

**源码事实：** 对只能同步执行的组件，async 路径通过 `asyncio.to_thread` 派发（会复制 `contextvars` 上下文以保住 tracing span），而**运行中的线程无法被中断**，故取消只是放弃等待。

**源码事实：** `stream` 的 chunk 通过给带 `streaming_callback` 输入 socket 的组件注入 forwarder 进入队列；用户自带的 callback 会同时被调用；同步 callback 会在事件循环上同步执行。

**源码事实：** async 路径末尾有一段「drain 剩余任务」的代码被注释标为当前是 no-op，保留理由是「未来若新增会留下在飞任务的 `break`，不该丢输出」。

**架构推论：** 「优先级调度 + 就绪判定分离」把调度策略变成可替换的层。判定函数只回答「能不能跑」（a03），优先级函数只回答「先跑谁」，两者互不知情。代价是**每个组件每一轮都要重算一遍判定**——这就解释了为什么 sync 路径要缓存队列新鲜度、而 async 路径干脆承认无法增量。

**架构推论：** `DEFER` 这个优先级的存在说明「数据流就绪」不是二值的。绝大多数框架只有「能跑/不能跑」两态，于是要么提前跑（拿半份输入）要么永远等（如果某个可选上游永远不产出就死锁）。四态设计让「可以跑但不急」成为一个可表达的状态——这是这类系统里少见的细致之处。

**架构推论：** `result` 属性对三种状态分别抛错，是「把不确定状态显式化」的正确做法。返回 `None` 会让调用方在 `if result:` 上静默走错分支；抛错会强制调用方处理。这也是「流式 API 的返回值语义」里最容易被做错的一处。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `haystack/core/pipeline/base.py` | `PipelineBase` | 持有 `networkx.MultiDiGraph`，实现接线、优先级计算、队列刷新、输出写回 |
| `haystack/core/pipeline/base.py` | `ComponentPriority` | 四级调度优先级枚举（`HIGHEST` / `READY` / `DEFER` / `BLOCKED`） |
| `haystack/core/pipeline/base.py` | `_calculate_priority` | 从就绪判定推出优先级，四行分支 |
| `haystack/core/pipeline/base.py` | `_get_next_runnable_component` | 取队首、检查 `max_runs_per_component`（带 BLOCKED 豁免） |
| `haystack/core/pipeline/base.py` | `_get_component_with_graph_metadata_and_visits` | 每次访问临时拼 `visits`，避免写进共享图 |
| `haystack/core/pipeline/base.py` | `_is_queue_stale` | 用 `IntEnum` 数值比较判断队列是否需重算 |
| `haystack/core/pipeline/base.py` | `_topological_sort` | 平手决断用拓扑序；有环时用强连通分量凝聚图 |
| `haystack/core/pipeline/pipeline.py` | `Pipeline` | 同步 `run`、异步生成器 `run_async_generator`、`run_async`、`stream` 四条入口 |
| `haystack/core/pipeline/pipeline.py` | `PipelineStreamHandle` | 队列 + 任务句柄：异步迭代、`result` 三态抛错、带超时的 `aclose` |
| `haystack/core/pipeline/pipeline.py` | `_run_component` / `_run_component_async` | 组件执行外壳：断点检查、span、异常包装策略、输出校验 |
| `haystack/core/pipeline/pipeline.py` | `_wait_for_tasks` / `_cancel_in_flight_tasks` | 收割在飞任务并 yield 部分输出；取消清理 |

## 参考

- [haystack 源码：core/pipeline/base.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/pipeline/base.py)
- [haystack 源码：core/pipeline/pipeline.py](https://github.com/deepset-ai/haystack/blob/main/haystack/core/pipeline/pipeline.py)
- [haystack 文档：管道](https://docs.haystack.deepset.ai/docs/pipelines)
- [Python 文档：`asyncio.Semaphore`](https://docs.python.org/3/library/asyncio-sync.html#asyncio.Semaphore)

## 与主核 / AI 的关系

管道调度与主核的**算子调度器**是同一类问题，但两边的取舍恰好相反，值得逐条对照。

**第一，按「就绪优先级」调度 vs 按拓扑序推进。** 主核（以及大多数数据库执行器）沿着计划树推进，算子顺序在设计期就定死了；haystack 在运行期每一轮重新问「谁最该跑」。后者能处理「一个组件可跑多次」这类动态拓扑（循环、条件分支），代价是每轮都要重算优先级。**判断标准：如果图在执行期会变（次数不定、路径不定），就必须动态调度；如果图是固定的，静态顺序更便宜也更可预测。**

**第二，`DEFER` 这个中间态值得借鉴。** 主核做「并行扫描 + 聚合」时也有同类问题：聚合算子的部分输入已到，此时跑会浪费（半份数据要再扫一遍），不跑又怕死锁。haystack 用「能跑但降级优先级」表达它，本质是**给调度器一个「可以但不必现在」的选项**。这在两态调度器里无法表达。

**第三，共享状态必须显式排除。** `visits` 不写进 `graph.nodes` 的理由是「会破坏线程安全 / 重入」——这条经验直接对应主核里的执行期计数器：**任何随每次执行变化的状态都不能挂在共享的计划结构上**，否则两个并发查询会互相污染。要么放进每次调用的局部状态（haystack 的做法），要么放进显式的执行上下文对象。

**第四，流式 API 的返回值必须拒绝「不确定」**。`result` 对未完成/已取消/失败三种状态各抛一个错，而不是返回空值。主核里若做「增量结果流」（例如 `EXPLAIN ANALYZE` 的逐算子输出），同样应当在「还没跑完就取最终统计」时明确报错，而不是返回一份看起来合理的中途快照。

**第五，清理必须有超时上界。** `aclose()` 的 1 秒上界与「运行中的线程无法被中断」的注释，揭示了一个通用事实：**取消是协作式的，你不能保证它真的停下**。主核里若做「查询取消」，同样不能假定算子会立刻响应——必须给清理路径一个上界，否则一个卡在磁盘 I/O 的算子会让整个关闭流程挂住。
