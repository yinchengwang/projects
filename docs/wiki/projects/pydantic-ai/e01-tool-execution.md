---
project: pydantic-ai
feature: tool-execution
title: 工具执行与并发控制
page_type: feature
schema_version: 1
refs:
  - path: pydantic_ai_slim/pydantic_ai/concurrency.py
    symbols: [ConcurrencyLimiter, ConcurrencyLimit, get_concurrency_context]
  - path: pydantic_ai_slim/pydantic_ai/_tool_execution.py
    symbols: [_segment_by_barriers, _duplicate_tool_call_ids]
---

# 工具执行与并发控制

## 一句话职责

一条模型响应里的多个工具调用按「分段并发」执行：`sequential=True` 的工具是屏障，它自己独占一段，前后各自成段；段内并发、段间串行，事件按段内完成顺序流出——并发上限则交给一套独立的信号量限流器。

## 机制是什么

执行层的三件事：

1. **分段**。`_segment_by_barriers` 把「本步要执行的调用下标」切成一串段：每个屏障下标自成一节，连续的普通下标合成一节。段按顺序跑，所以「屏障之前的工具全部完成」之后屏障才开始，「屏障之后的工具」要等屏障结束。
2. **策略**。`end_strategy` 决定跑哪些、什么顺序：`early`（先跑输出工具，全失败才跑函数工具）、`graceful`（按模型发出顺序，遇到输出工具前先跑掉挂起的函数批次）、`exhaustive`（全部并行，只被屏障分段）。
3. **限流**。`ConcurrencyLimiter` 是一个带可观测性的信号量封装，用于限制并发的外部操作（模型请求、agent 运行），与工具分段是两套独立机制。

## 怎么实现的

**分段的判据有两个来源。** `exhaustive` 策略里 `is_barrier` 是 `global_sequential or self.tool_manager.is_sequential(...)`：

- `global_sequential` 来自 `ToolManager.parallel_execution_mode('sequential')`——一个运行级开关，把所有调用都当屏障，于是每段只剩一个元素，整体变成串行。
- `is_sequential(call)` 查该调用的 `tool_def.sequential`。注释写明了它与运行级开关的关系：「单个屏障工具不再强制整批串行（v1 的行为）；要把整个运行投入串行请用 `parallel_execution_mode('sequential')`」。这是 v2 的一处行为修正。

**并发靠 `asyncio.Task` + 完成即产出。** `_run_strategy` 对每一段 `asyncio.create_task(run_one(i), name=...)`（任务名直接取工具名，便于观测与调试），然后 `_iter_completed_or_buffered(tasks, event_stream_buffer)` 逐个拿完成的任务。三个分支决定结果落在哪：

- `_OutputCallResult` → `output_results[index]`；
- `CallDeferred` / `ApprovalRequired` → 记进延迟桶（按 kind 分 `external` / `question` / `unapproved`）；
- 其他（函数工具返回）→ `function_parts[index]`，若带用户内容再记 `function_user_parts[index]`，并立即判一次 `_is_retry_wins_trigger`。

`is_retry_wins_trigger` 在并行路径里**当场**判定并把 `retry_wins_triggered` 置真，而在顺序策略里是在整批 `_call_tools` 之后扫一遍新追加的部件再判。两处都汇入同一个谓词，注释特意点明这一点。

**结果装配是「先并发、后按顺序」。** 即便执行是并发的，历史部件的追加严格按 `executable_indices`（即模型发出顺序）进行：先按序处理输出工具（发事件、记状态部件），再按序追加函数工具部件，最后按序追加用户内容部件。这是为了让「同一份响应两次运行得到同一份历史」。

`parallel_ordered_events` 模式额外把函数工具的结果事件也缓存起来（`function_events[index]`），到装配阶段才按序发出；默认模式下它们是任务完成时立刻发出的。这个区别写在注释里：该模式只推迟函数工具的结果事件，其他运行事件照旧实时流出。

**中途中断要保住已完成的结果。** 这是整段里最严谨的部分。`finally` 块会把「已完成但尚未装配」的函数返回追加进 `output_parts`，但只补那些正常循环**还没追加过**的下标子集（`appended_function_indices` / `appended_user_indices`）。注释解释了两条原因：正常循环里那些 `yield` 是挂起点，一个在那里被送达的取消会让 `finally` 重复追加；而 `_prune_duplicate_tool_reveals` **不是幂等的**（第二次跑会把所有名字都当成「已发现」而丢掉刚保留的增量），所以用 `pruned` 布尔守住。

这里还有一个更细的设计：为什么用「每个下标一组集合」而不是一个单一标志？注释直接讲了——追加循环在多次 `extend` 之间会 `yield` 事件，消费者在任何挂起点关闭或抛出，单一标志都无法区分「哪些下标已经追加过」。

取消路径单独处理：捕获 `asyncio.CancelledError` 时先 `cancel_and_drain(*tasks, msg=...)` 再 `_record_completed_results(...)` 再把异常重新抛出。另一条 `except BaseException` 分支则 `cancel_and_drain(*tasks)` 无消息。两条都把已完成任务的结果记下来，注释说明理由：在某个兄弟任务抛错或运行被取消之前就已经完成的工具，其返回值要保留在被打断的那次请求里，这样从该历史恢复时不会重跑它。

**重复 id 在并发前挡住。** `_duplicate_tool_call_ids` 遍历调用，记录「第一次作为重复出现」的 id（去重后按顺序返回）。两处使用：

- `_validate_function_calls` 里在**本批任何调用执行之前**抛 `UnexpectedModelBehavior`，理由是并发执行下 id 重复会让结果回填有歧义（「last write wins」）：在并发下尤其危险，因为谁后写取决于调度。
- 恢复路径（`_ToolCallProcessor.__post_init__`）里用 `UserError`，因为在那种场景下「一个提供的结果会绑到多个调用」是参数错误而不是模型行为异常。

注释还说明批次之间独立绑定：`graceful` 在输出调用处切开的两批各自可以重复 id，各批用自己的 `validated_calls`。

**输出工具的胜负判定（`exhaustive` 特有）。** `_pick_winner` 按发出顺序遍历，第一个「有 `final_result` 的输出」或「通过 `RunContext.submit_output` 提交过且成功的函数调用」获胜，随后立即 `return`——后面的输出即使也成功，只记成 `OUTPUT_NOT_FINAL_RESULT` 状态部件。`exhaustive` 的注释写明「其余输出仍会执行」——这不是浪费，因为执行本身可能带副作用，且事件流需要完整。

有一条专门的例外：若 `final_result` 已由外部设定（流式传入），对应那个 `tool_call_id` 的输出不再重跑（`output_results[i]` 直接预填），并从分段里排除（`task_indices` 过滤掉这些下标），因为「预定的输出没有任务要启动」。

**输出重试的累加被推迟。** `output_retries_increment` 在 `_run_output_tool_call` 里只做 `+= 1`，真正写回 `ctx.state.output_retries_used` 是在策略结束后（`early` / `graceful` 在 `_run_strategy` 末尾，`exhaustive` 在决定抛错之前）。注释说得很清楚：并行的输出任务若各自直接写计数器就会竞态。

**`IncompleteToolCall` 优先于重试。** 输出校验失败时先 `check_incomplete_tool_call(call)`，若判定参数被截断就直接抛，不进重试。理由写在旁边：用同样的 token 上限再提示一次只会再截断一次。

**限流器：为什么用信号量而不是容量限流器。** `ConcurrencyLimiter` 用 `anyio.Semaphore` 而不是 `anyio.CapacityLimiter`，注释的理由很具体：`CapacityLimiter` 把每个槽绑定到取得它的任务，而这里 `release()` 必须能从**任意**任务调用——一个流式模型响应被哪个任务迭代与关闭并不固定。这条约束来自真实场景，不是理论洁癖。

限流器的三个可观测量：`running_count = max_running - semaphore.value`、`available_count = semaphore.value`、`waiting_count`（自己维护的计数）。等待路径的并发安全靠一把 `anyio.Lock`：先尝试 `acquire_nowait()`，失败才进临界区，在锁内检查 `max_queued` 并**先登记**等待计数再释放锁，然后才真正 `await acquire()`。注释解释了为什么必须先登记：否则多个任务可能在任何一方真正开始等待之前都通过了检查。释放计数在 `finally` 里做，保证取消也会减。

等待期间会开一个 OTel span（名字是 `waiting for <display_name> concurrency`，属性带 `source`、`waiting_count`、`max_running`、可选 `limiter_name` 与 `max_queued`）。这是「可观测性是内建而非附加」的一处体现：连排队都在追踪里可见。

`AnyConcurrencyLimit` 是三态联合：`int`（简单上限、队列无限）、`ConcurrencyLimit`（带 `max_queued`）、已建好的 `AbstractConcurrencyLimiter`（跨模型/跨 agent 共享）、或 `None`（不限）。`normalize_to_limiter` 做归一，`get_concurrency_context` 在不限时返回一个空上下文（`_null_context`），使调用点不必写分支。

## 关键取舍

**源码事实：** 屏障的粒度是「单个调用」而不是「整批」。一个 `sequential=True` 的工具只让自己独占一段，其他工具仍可并行；要把整个运行串行必须显式用运行级模式。这是 v2 有意改变 v1 行为的地方。

**源码事实：** 历史装配严格按发出顺序，即使执行是并发的。这让同一响应的历史可复现，代价是装配阶段要等所有任务结束。

**源码事实：** 重复 `tool_call_id` 在执行前就被挡住，因为并发下依赖「谁后写」是不可接受的。

**源码事实：** 输出重试计数推迟到批次结束再写，避免并行竞态。

**源码事实：** 限流器只能用信号量，因为 `release()` 可能来自与 `acquire()` 不同的任务。

**架构推论：** 「执行并发、装配有序」这个分工是这套实现的主轴。它把「什么时候跑」与「按什么顺序记录」解耦，使并发不牺牲可复现性。代价是中断路径的清理逻辑必须非常小心——源码为此引入了两组已追加下标集合与一个已剪枝布尔，且注释里承认有些分支用测试难以触发。

**架构推论：** 屏障用「分段」实现而不是「锁」：不需要任何显式同步原语，只要把段按序跑、段内并发即可。这比用锁简单得多，也更容易解释给用户。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_segment_by_barriers` | 按屏障切分执行段，段内并发段间串行 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_duplicate_tool_call_ids` | 收集重复的调用 id，按首次作为重复出现的顺序 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_ExhaustiveProcessor` | 全并行策略：分段、胜负判定、按序装配、中断保底 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_EarlyProcessor` / `_GracefulProcessor` | 另两种顺序策略及其与输出的关系 |
| `pydantic_ai_slim/pydantic_ai/_tool_execution.py` | `_prune_duplicate_tool_reveals` | 装配期按序去重工具揭示，非幂等 |
| `pydantic_ai_slim/pydantic_ai/concurrency.py` | `ConcurrencyLimiter` | 信号量限流 + 队列上限 + 等待期 span |
| `pydantic_ai_slim/pydantic_ai/concurrency.py` | `ConcurrencyLimit` | 配置：并发上限与队列上限的校验 |
| `pydantic_ai_slim/pydantic_ai/concurrency.py` | `get_concurrency_context` | 归一为上下文管理器，不限时为空上下文 |

## 参考

- [pydantic-ai 源码：_tool_execution.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/_tool_execution.py)
- [pydantic-ai 源码：concurrency.py](https://github.com/pydantic/pydantic-ai/blob/main/pydantic_ai_slim/pydantic_ai/concurrency.py)
- [Pydantic AI 文档：工具集与并发](https://ai.pydantic.dev/toolsets/)
- [anyio 文档：信号量](https://anyio.readthedocs.io/en/stable/synchronization.html#semaphores)

## 与主核 / AI 的关系

「分段 + 段内并发」这套结构，与主核里的并行查询执行是同一类设计：都存在「某些操作必须独占」的语义（主核里是一次 DDL 或者需要全序的聚合），都要在「允许并行」与「必须串行」之间切分执行序列。区别在主核的切分由规划器静态产生并可代价比较，这里的切分由工具的声明字段直接给出，无需估算。

中断处理那一段与主核里的「部分完成的排序归并」值得对照。主核在排序中途被中断时，已写下的临时文件会被清理；这里的原则相反——已完成的结果要**保留**进历史，理由是让恢复时不重跑。两边的目标不同：数据库要保证存储一致（多余的中间产物是有害的），agent 要保证工作不重复（多余的结果是有益的）。这条差异解释了为什么这里的清理逻辑那么绕：它要保留一切，却只能保留一次。

限流器那段有一条特别值得记住的工程判断：用 `Semaphore` 而不是 `CapacityLimiter`，因为 `release` 必须能从任意任务调用。这类「API 语义与真实生命周期不匹配」的坑，在把库用在流式场景时非常常见。

对 AI 工程的实际意义在于：工具并发是 agent 性能与正确性的直接来源。这套实现给出的三条经验很实用——用声明式的屏障而不是锁、并发执行但按发出顺序装配历史、以及在中断时保住已完成的结果。第三条尤其容易被忽略，而它恰恰决定了「崩一次要重做多少」。
