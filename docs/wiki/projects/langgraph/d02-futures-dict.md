---
project: langgraph
feature: futures-dict
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_runner.py
    symbols: [FuturesDict, on_done]
---

# 完成回调与结果表

## 一句话职责

`FuturesDict` 是「future → 任务」的字典，外加一套完成回调机制：它在每个 future 注册时清零一个事件、在完成时把结果交给 `commit`，并在「全部完成」或「该停下来」时置位该事件，让 runner 的主等待循环得以推进。

## 机制是什么

并发执行需要一个「谁跑完了」的通知机制。`concurrent.futures.as_completed` 与 `asyncio.as_completed` 都能做到，但它们都不提供这里需要的两件事：一是「每完成一个就立刻提交结果并让调用方有机会流输出」，二是「任一失败就让等待提前结束」。`FuturesDict` 自己实现了这套协调。

它继承 `dict`，键是 future，值是 `PregelExecutableTask` 或 `None`。值为 `None` 的条目是「waiter」——一个不执行任务、只用来在流队列有新内容时唤醒等待的 future。这个区分不是靠额外标记，而是靠值本身：`__setitem__` 只在值非 `None` 时才登记计数与回调。

三个协调量：`counter`（在跑的 future 数）、`done`（已完成的集合，累积）、`failed`（已完成且带异常的集合，累积）。`done` 与 `failed` 都不清除，因为收尾的裁决需要看到全部历史。

## 怎么实现的

**构造期的四个依赖。** `event` 由调用方给（同步用 `threading.Event`，异步用 `asyncio.Event`）；`callback` 是 `weakref.WeakMethod(self.commit)`，即对 runner 提交方法的弱引用；`should_stop` 由 runner 注入，实际是 `_should_stop_others` 的偏应用，带上了 runner 自己的 `_handled_exception_ids`；`future_type` 参数只用于泛型标注，源码注释直说了「供泛型类型用，新版 Python 支持 `FutureDict[...](...)` 才不需要」。

把 `should_stop` 做成注入而不是写死，源码注释给了理由：让 runner 能把「已被错误处理器接住的异常」当作非致命，从而不因为一个已被处理的失败而提前结束等待。

**`__setitem__` 的三步。** 先写进字典本体；若值非 `None`，在锁内 `event.clear()` 并 `counter += 1`，再 `key.add_done_callback(partial(self.on_done, value))`。

`event.clear()` 这一句是整个机制的关键。runner 在每批任务提交完之后调 `futures.event.wait()`；如果 event 在上一批已经置位、且没有清零，这次 `wait()` 会立刻返回，导致「以为全跑完了」。所以在每次登记新 future 时清零，语义变成「event 代表『本批已全部完成』」而不是「曾经完成过」。

**`on_done` 的两段结构。** 第一段在 `try` 里：取弱引用拿到 `commit` 方法，若有就调用 `cb(task, _exception(fut))`。`_exception` 是一个辅助函数，它把「已取消」的 future 翻译成对应类型的 `CancelledError` 实例（`asyncio.Future` 给 `asyncio.CancelledError`，其他给 `concurrent.futures.CancelledError`）——直接调 `fut.exception()` 在取消状态下会抛 `CancelledError`，而这个回调里不能抛。所以取消也被当成一种「结果」交给 `commit`。

第二段在 `finally` 里，因此即使 `commit` 自身抛异常，计数也会递减、event 也可能被置位——否则整个等待会永久挂住。锁内做三件事：`done.add(fut)`；`counter -= 1`；若 future 未被取消且 `fut.exception()` 非 `None` 就加进 `failed`。最后判断 `counter == 0 or self.should_stop(self.failed)`，命中则 `event.set()`。

判断条件里有一处性能取舍，源码注释写明：只有抛了异常的 future 才可能满足停止条件，所以只对这些 future 调 `should_stop`，而不是对每个完成的 future 调——否则宽超步（几十上百个并行任务）下这个判断是平方级。

**waiter 条目的特殊性。** `futures[get_waiter()] = None` 只进字典、不计数、不注册回调。因此 waiter 完成时 `on_done` 根本不会被调用，`counter` 不受影响。runner 的主循环条件写成 `while len(futures) > (1 if get_waiter is not None else 0)`，正是为了把这一个 waiter 条目从「还有多少任务没完成」的计数里排除掉。处理完成集合时若 `task is None`，说明完成的是 waiter，于是「若有 inflight 就再挂一个新的 waiter」——这是流式等待的续接逻辑。

**与 runner 主循环的配合。** runner 侧的顺序是：`wait(futures, FIRST_COMPLETED, timeout=...)` → 对每个完成的 future 从字典 `pop` 掉 → 处理（错误处理器换任务、或计入 `done_for_stop`）→ 判断该不该停 → `yield`。字典被 `pop` 之后 `len` 减少，但 `counter` 由 `on_done` 独立维护——两者不是同一个量：`pop` 发生在 `wait` 返回之后，`on_done` 发生在 future 完成那一刻，所以 `counter` 通常更早归零，`event.wait()` 可能在主循环还没处理完所有完成项时就已经返回。这不是缺陷：`event.wait()` 只用来「等 done 回调都跑完」，主循环的推进靠 `wait()`。

**`done` 与 `failed` 的最终用途。** 收尾时 runner 调 `_panic_or_proceed(futures.done.union(f for f, t in futures.items() if t is not None), ...)`——把「已完成的历史」与「仍在字典里的任务条目」合并，跳过 waiter。这样「失败的 future 被 `pop` 掉了」不会导致裁决时看不到它；反过来「仍在跑的 future」也参与判定，从而能识别超时。

## 关键取舍

**源码事实：** `callback` 是弱引用。若 runner 被回收，`self.callback()` 返回 `None`，`commit` 不会执行，但计数与事件照常推进。因此「结果没被记录」这件事不会表现为挂起，而表现为静默丢写。这是弱引用换来的代价。

**源码事实：** `event.clear()` 在每次登记时执行，而不是在 `wait()` 返回后执行。因此「一批登记后清零、全部完成时置位」的配对由 `FuturesDict` 自己维持，调用方无需关心。反过来说，若有人绕过 `__setitem__` 直接 `dict.__setitem__`，这套配对就断了。

**源码事实：** `on_done` 的 `finally` 里调用 `should_stop`，而 `should_stop` 可能读 `failed` 集合。这意味着「停止判定」在 `commit` 之后、在同一把锁内发生。若 `commit` 里的 `put_writes` 阻塞（它可能提交到后台执行器），`on_done` 会被拉长，进而推迟 `event.set()`。这里是刻意接受的：`put_writes` 在正常路径上只是入队。

**架构推论：** 用「事件 + 计数」而不是「收集所有结果再返回」，使 runner 能在任务陆续完成时多次 `yield`，从而边跑边流。代价是「一次超步的结果」不是一个返回值，而是分散在 `done`/`failed`/字典三处的累积状态，读代码时必须同时看这三处。

**架构推论：** waiter 复用同一个字典但绕过全部记账逻辑，说明这套结构本质上是「future 注册表 + 一个完成信号」，而不是「任务结果集合」。任务结果由 `commit` 直接写进挂起写入，字典只负责「何时可以继续」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_runner.py` | `FuturesDict` | future 注册表、计数、完成与失败集合 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `FuturesDict.__setitem__` | 清零事件、递增计数、注册完成回调 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `on_done` | 调 `commit`、记账、按条件置位事件 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_exception` | 把已取消的 future 翻译成异常实例而不抛出 |
| `libs/langgraph/langgraph/pregel/_runner.py` | `_should_stop_others` | 注入的停止条件：排除中断、已处理与无效 resume |

## 参考

- [LangGraph 源码：pregel/_runner.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_runner.py)
- [LangGraph 源码：pregel/_executor.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_executor.py)
- [Python 文档：concurrent.futures.Future 的回调](https://docs.python.org/3/library/concurrent.futures.html#concurrent.futures.Future.add_done_callback)

## 与主核 / AI 的关系

「注册回调 + 计数归零 + 一个事件置位」与主核里等待一批异步 I/O 完成的机制同构：都是不轮询、由完成路径主动推进，且都要处理「完成顺序与提交顺序不同」的问题。主核靠 I/O 完成队列与 LSN 排序恢复顺序；这里靠「谁先完成谁先 `commit`」，顺序性由 `apply_writes` 的路径排序在合并阶段重新建立。

对 AI 侧而言，这一页解释了流式输出的时序：任务一完成就 `commit` 并 `yield`，所以「边跑边出 token」不是靠单独的生产者线程，而是靠 runner 的生成器结构。也解释了为什么「一个节点失败会让同批其他节点的输出也可能已经流出」——它们的完成回调在我们知道有失败之前就已经跑过了。

