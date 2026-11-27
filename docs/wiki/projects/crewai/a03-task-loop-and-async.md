---
project: crewai
feature: task-loop-and-async
title: 任务循环与异步任务排队
page_type: feature
schema_version: 1
refs:
  - path: lib/crewai/src/crewai/crew.py
    symbols: [_execute_tasks, _process_async_tasks, _get_context]
---

# 任务循环与异步任务排队

## 一句话职责

`_execute_tasks` 是整个框架唯一真正跑任务的地方：它按顺序遍历任务列表，把标了 `async_execution` 的任务丢进后台线程池并暂存 future，遇到下一个同步任务时再把它们全部收回来——顺序语义与并行语义由此在同一个循环里共存。

## 机制是什么

这里的设计核心是「**同步屏障**」。任务列表的遍历顺序从不改变，但一个异步任务并不会立刻产出结果：它被提交到线程池后只留下一个 `Future` 句柄，与它自己的 `task_index` 一起压进 `futures` 列表。只有当下一个**同步**任务即将执行、或者整个列表走完、或者遇到条件任务需要判定时，这些 future 才会被逐个 `result()` 收集，输出按原顺序追加进 `task_outputs`。

因此这里的并行不是「乱序执行」而是「**先排队、后回收**」：异步任务的实际执行可以与紧随其后的其他异步任务重叠，但它们的结果在语义上仍占据原来的位置。这也解释了为什么团队级校验要限制「尾部连续异步段最多一个」并禁止「异步任务的上下文里隔着零个同步任务的另一个异步任务」——那是这套屏障模型的技术边界。

另一个关键点是 `_get_context`：每个任务拿到的上下文字符串是**从已有的 `task_outputs` 里现拼的**，不是预计算的。同一份 `task_outputs` 既被用来构造后序任务的上下文，也被最后拿去合成 `CrewOutput`。

## 怎么实现的

**循环体的四段。** `_execute_tasks` 的每次迭代依次做：先 `prepare_task_execution` 准备（返回 `TaskExecutionData`，其中含实际要用的智能体与工具列表）；若 `should_skip` 为真则 `continue`；条件任务走单独分支（见 b02）；否则按 `async_execution` 分成提交与执行两条路。

**`start_index` 在循环之前就被改写。** 方法开头先调 `_get_execution_start_index(tasks)`：若团队带着 `checkpoint_kickoff_event_id`（说明这次是从检查点续跑），返回第一个 `output is None` 的任务下标；全部都有输出则返回 `len(tasks)`。非空返回值会覆盖调用方传进来的 `start_index`。`start_index` 的实际作用不在这个循环里，而在 `prepare_task_execution` 里：任务下标小于它就跳过执行，但仍把它的 `task.output` 追加进 `task_outputs`，并在该任务非异步时更新 `last_sync_output`。这就是「重放时已完成的任务也要参与上下文构建」的实现方式。

**异步路径。** 提交分支做两件准备工作：`context = self._get_context(task, [last_sync_output] if last_sync_output else [])`，然后 `task.execute_async(...)`。

上下文的参数值得单拎：异步任务拿到的是**只有最后一个同步任务的输出**，而不是全部 `task_outputs`。原因很实际——`task_outputs` 里此刻可能还有尚未 `result()` 的 future 对应输出（虽然本分支只在提交时使用，但语义上维护「异步任务只看同步屏障」这条规则），更重要的是，异步任务与其同批的其他异步任务本就该互相隔离，只共享最近一个已确定的同步结论。这也正是团队校验里那条「异步任务不能隔着零个同步任务引用另一个异步任务」的动机。

`Task.execute_async` 的实现是「复制 contextvars + 起守护线程 + 塞 `Future`」：先 `contextvars.copy_context()`，再用 `threading.Thread(daemon=True, target=ctx.run, args=(self._execute_task_async, agent, context, tools, future))` 启动，返回 `future`。复制上下文这一步是必要的：事件总线、运行作用域、`current_task_id` 这些都存在 contextvar 里，不复制就会丢父上下文。守护线程选择意味着解释器退出时不等这些任务收尾。

**回收路径。** 同步分支执行前先判 `if futures:`，非空就 `_process_async_tasks(futures, was_replayed)` 并把结果 `extend` 进 `task_outputs`，随后 `futures.clear()`。这一步就是同步屏障。`_process_async_tasks` 本身很薄：对每个 `(future_task, future, task_index)` 依次 `future.result()`（阻塞直到完成，异常原样向上抛），追加输出，再调 `_process_task_result`（写 `output_log_file`）与 `_store_execution_log`（把输出写进 `TaskOutputStorageHandler`，带 `task_index` 与 `was_replayed`）。循环结束后若 `futures` 仍非空，再收一次。最后 `_create_crew_output(task_outputs)`。

**`_get_context` 的三分支。** 它是 `@staticmethod`，只依赖任务与输出列表：

- `not task.context` 为真（含 `None` 与空列表）时返回空串。
- `task.context is NOT_SPECIFIED` 时用 `aggregate_raw_outputs_from_task_outputs(task_outputs)`，即把**当前已经产出的全部输出**拼起来。
- 否则用 `aggregate_raw_outputs_from_tasks(task.context)`，即按任务声明的 `context` 列表取各任务的 `task.output`。

这里依赖 `NOT_SPECIFIED` 这个哨兵而不是 `None`，就是为了区分「用户显式写了 `context=[]`（不想要上下文）」与「用户根本没写（默认拿全部前序输出）」。哨兵自身重写了 `__repr__` 并提供了 Pydantic 序列化 schema，使它既能被比较也能被序列化。

拼接分隔符是 `DIVIDERS = "\n\n----------\n\n"`，纯文本拼接、没有结构化标签。这意味着下游模型看到的是「一堆用横线隔开的原文」，谁是谁得靠内容自己判断——这是刻意的简化，也把上下文的可解释性交给了模型的阅读能力。

**条件任务也插在这里。** 条件任务分支在两种流程下各有一份（同步 `_handle_conditional_task` 与异步 `_ahandle_conditional_task`），行为一致：**先回收所有待决 future**，再调 `check_conditional_skip` 判定。为什么要先回收？因为条件判定的输入是「上一个任务的输出」，而那个输出可能还在 future 里。若不先收，`task_outputs[-1]` 会是更早的结论。

## 关键取舍

**源码事实：** 并行度来自线程而不是协程。`Task.execute_async` 用 `threading.Thread` + `Future`，因此异步任务共享 GIL，真正的收益在「等 IO/等模型返回」而不是 CPU 并行。这是为了兼容同步执行链路（`BaseAgent.execute_task` 是同步签名）而做的一致性选择。

**源码事实：** 异步任务的结果顺序是**提交顺序**而不是完成顺序。回收时按 `futures` 的插入顺序逐个 `result()`，先提交的即使后完成也会先被追加。这使得 `task_outputs` 始终与 `tasks` 的下标对齐，`_store_execution_log(task_index)`、`_find_task_index`、以及重放的 `start_index` 才有意义。

**源码事实：** 同步屏障是「遇到同步任务才收」，不是「定时收」。所以**连续多个异步任务之间没有任何回收动作**，它们的失败也要等到屏障处才暴露。这换来的是更长的重叠窗口，代价是错误发生的时刻被推迟。

**源码事实：** 异步任务只看到 `last_sync_output`（最近一个同步任务的输出），同步任务看到全部 `task_outputs`。这一处不对称是这套模型里最容易踩坑的地方，团队级校验的那几条规则正是在替它兜底。

**源码事实：** `_get_context` 返回的是纯文本拼接，不做长度控制。上下文变长没有任何上限保护，超出模型窗口的处理被推给了模型层的上下文压缩机制（见 c04）。

**架构推论：** 把「顺序语义」与「并发执行」解耦成「遍历顺序 + 屏障回收」，使得新增流程（例如源码 TODO 里的共识流程）不需要改这个循环，只需要改循环前的准备与任务的分派逻辑。

**架构推论：** 由于上下文从 `task_outputs` 现拼，任务之间的依赖是**隐式且全量**的（默认情况），而不是显式的图。这让配置简单，但也就失去了「只传必要上下文」的裁剪能力，长流水线的上下文膨胀是必然的。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `lib/crewai/src/crewai/crew.py` | `_execute_tasks` | 唯一任务循环：跳过判定、条件分支、异步提交、同步屏障、输出合成 |
| `lib/crewai/src/crewai/crew.py` | `_process_async_tasks` | 回收 future：按提交顺序取结果、写日志、落库 |
| `lib/crewai/src/crewai/crew.py` | `_get_context` | 三分支上下文构造，依赖 `NOT_SPECIFIED` 哨兵区分「没写」与「空列表」 |
| `lib/crewai/src/crewai/crew.py` | `_aexecute_tasks` | 同一循环的 `asyncio` 版本，用 `create_task` 而非线程 |
| `lib/crewai/src/crewai/crew.py` | `_store_execution_log` | 把任务输出连同 `task_index`、`inputs`、`was_replayed` 交给存储处理器 |
| `lib/crewai/src/crewai/crews/utils.py` | `prepare_task_execution` | 循环每轮的准备：重放跳过、智能体选取、工具准备、开始日志 |
| `lib/crewai/src/crewai/utilities/formatter.py` | `aggregate_raw_outputs_from_task_outputs` | 以固定分隔符拼接原始输出 |

## 参考

- [CrewAI 源码：crew.py](https://github.com/crewAIInc/crewAI/blob/main/lib/crewai/src/crewai/crew.py)
- [CrewAI 文档：异步任务执行](https://docs.crewai.com/en/concepts/tasks#asynchronous-execution)
- [CrewAI 文档：任务上下文](https://docs.crewai.com/en/concepts/tasks#task-context)

## 与主核 / AI 的关系

这个循环与主核的「批量请求处理」高度相似：都是先按序排队，再把可并行的部分丢进后台，最后在需要一致性的位置做一次屏障回收。主核里这类结构常见于「预读 + 顺序应用」——预读可以乱序完成，但日志应用必须按序。差别在于主核的屏障是事务级的（有明确的提交点与回滚），这里的屏障只是「等 future 返回」，失败就直接向上抛，没有部分提交的概念。

对 AI 工程的实际意义在于：任务循环是「确定性调度」与「不确定性执行」的接缝。调度侧（顺序、屏障、跳过）完全可预测、可重放；执行侧（模型与工具）完全不可预测。CrewAI 把两者用 `task_index` 与 `task_outputs` 这两个稳定锚点串起来，使得重放能精准定位「从哪继续」、日志能精确对上「哪一步产出什么」。理解了这一点，就能理解为什么异步任务只允许出现在尾部连续段、为什么它们的上下文被刻意收窄——凡是要保住这条锚点链的地方，都不允许无序的并发介入。
