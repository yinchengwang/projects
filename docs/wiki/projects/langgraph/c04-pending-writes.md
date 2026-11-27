---
project: langgraph
feature: pending-writes
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_loop.py
    symbols: [put_writes, _put_pending_writes, accept_push]
---

# 挂起写入与恢复接入

## 一句话职责

「挂起写入」是超步进行中尚未被合并进通道的那批写入：`put_writes` 负责把它同时写进内存日志与检查点器，`_put_pending_writes` 负责在退出时把它们补齐落盘，`accept_push` 负责让任务运行中动态产生的新任务也能拿到这条通路。

## 机制是什么

Pregel 的一步里，任务边跑边写。这些写入不能立刻改进通道（否则超步内的可见性保证就没了），但也不能只留在内存——进程崩了就等于白跑。`checkpoint_pending_writes` 就是这中间的缓冲区，元素是 `(task_id, channel, value)` 三元组。`tick()` 结束前的 `after_tick()` 会清空它，把它合并进通道；在此之前的所有写入都叫挂起写入。

挂起写入有两个消费者，因此有两套写入路径：

- 内存日志 `checkpoint_pending_writes`：给同一超步内的后续代码用（推导任务时的 `_scratchpad` 要从里面找 RESUME 值，`read_task_statuses` 要从里面判任务是否完成）。
- 检查点器的 `put_writes`：给崩溃恢复用。它按 task_id 分槽，恢复时原样读回。

两套路径由 `durability` 决定是否同时生效：`durability != "exit"` 且检查点器提供 `put_writes` 时，`put_writes` 会额外 `submit` 一次后台落盘；`durability == "exit"` 时所有落盘都推到最后，由 `_put_pending_writes` 一次性补齐。

## 怎么实现的

**`put_writes` 的五段处理。**

1. 空写入直接返回。这里不记录 `_written_task_ids`，所以「写了零条」与「没写」在 exit 模式的重放判断里是同一件事。
2. 先登记 task_id 到 `_written_task_ids`——这个集合是 exit 模式「本跑过哪些任务」的唯一依据，必须在任何过滤之前登记，否则被 UntrackedValue 洗掉的写入会让任务看起来没跑过。
3. 特殊通道去重：若**全部**写入都落在 `WRITES_IDX_MAP` 里（`INTERRUPT`/`RESUME`/`ERROR` 这类有固定槽位的控制写入），按通道名做字典去重、后者覆盖前者。注意条件是 `all`，一条普通写入混进来就整体跳过去重。
4. `ensure_message_ids`：在写入被保存或应用之前统一补齐消息 id，保证「同一批写入在内存与落盘两侧持有相同的 id」。
5. 按 `task_id` 分叉。`NULL_TASK_ID` 走「累积」语义：先删掉旧的 NULL 特殊写入，把新的特殊写入排到末尾，再与旧的普通写入拼接。注释解释了为什么要排序——检查点器按写入在列表中的**位置**做键，特殊写入这次被删掉了、下次不会再出现，若把它放在前面，重新保存时后面的普通写入会挪位，恢复时就会应用两遍。其他 task_id 走「替换」语义：先删掉该 task 的全部旧写入，再写入新的。

**UntrackedValue 的两处清洗。** `_has_untracked_channels` 是构造期算好的布尔值（避免每步扫全表）。为真时，落盘列表要做两件事：把 `TASKS` 通道里 `Send` 载荷中属于 UntrackedValue 的键剥掉（`sanitize_untracked_values_in_send`），以及整条丢弃对 UntrackedValue 通道的写入。而**内存日志**用的是未清洗的 `writes`——所以同一份写入在内存里是全的、在盘上是洗过的。这个非对称是有意的：内存要供本超步内的读者使用，盘上不该留不该持久化的值。

**落盘的两个附属 future 列表。** `_delta_write_futs` 收集「含 delta 通道写入」的那次落盘的 future，`_put_checkpoint` 会等它们完成再保存检查点，保证「检查点不会先于产生它的增量写入变得持久」。`_error_handler_write_futs` 收集「含 `ERROR_SOURCE_NODE` 标记」的那次落盘的 future，`schedule_error_handler` 会先等它们完成再启动错误处理器——否则处理器可能在一个「失败记录尚未持久化」的世界里运行，崩溃后重放就找不到失败原因。

**`_put_pending_writes` 的过滤。** exit 模式退出时它遍历内存日志，但只保留 `task_id in self._written_task_ids` 的写入。源码注释给了理由：随检查点加载进来的写入已经存在该检查点上了，再存一次会被检查点器按新位置分槽，于是恢复时应用两遍。这个过滤正是「加载」与「新产生」两类写入的分离点。

**`accept_push` 是运行中的动态接入点。** 任务在跑的时候可以通过 `Schedule` 式的回调产生新任务（即 `Call`），也可以写 `Send` 包。`accept_push` 用 `(PUSH, task.path, write_idx, task.id, call)` 作为任务路径调用 `prepare_single_task`，成功则把新任务发成 `tasks` 流事件、塞进 `self.tasks`，并在需要重放时立刻对新任务做一次写入复原，最后返回给 runner——runner 据此决定是「已经跑过就直接用结果」还是「起一个新任务」。它用的 config 是**父任务的 config**，因此新任务继承父任务的 namespace 与运行时上下文。

## 关键取舍

**源码事实：** 控制写入按固定槽位去重，普通写入按位置保存。两套规则混在同一次调用里，用 `all(...)` 分流。因此「一次 `commit` 里既有 `INTERRUPT` 又有普通输出」时，去重不生效——`INTERRUPT` 会按位置存成多条。恢复侧靠 `read_task_statuses` 把这些值重新解释，而不是靠存储布局。

**源码事实：** NULL_TASK_ID 的写入是「累积 + 特殊排尾」，其他 task 是「整体替换」。这条非对称来自语义：NULL 写入代表「图级输入/更新」，多次调用应当叠加；任务写入代表「某一次执行的产物」，重跑时必须覆盖旧结果。

**源码事实：** 内存日志保留 UntrackedValue 写入，落盘列表不保留。所以「进程内可见、崩溃后消失」这个 UntrackedValue 的语义不是由通道自己保证的，而是由这条清洗路径保证的。

**架构推论：** `_put_pending_writes` 用 `_written_task_ids` 而不是「日志里有没有」来过滤，意味着任何绕过 `put_writes` 直接往 `checkpoint_pending_writes` 追加的代码都不会被落盘。这是有意的封闭性：写入必须经过统一入口才能被持久化，从而保证消息 id 补齐、去重、清洗这些处理一定发生过。

**架构推论：** `accept_push` 让「任务集合」在超步执行期间可以增长，而 `tick()` 推导出的任务集合因此不再是最终形态。代价是 `after_tick()` 必须处理「本步实际跑过的任务比推导时多」的情况——它通过遍历 `self.tasks` 而不是推导结果来实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_loop.py` | `put_writes` | 去重、补 id、清洗 UntrackedValue、落盘并登记 future |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_put_pending_writes` | exit 模式退出时按 `_written_task_ids` 补齐落盘 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `accept_push` | 任务运行中动态接入新任务（`Send` 与 `Call`） |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_reapply_writes_to_succeeded_nodes` | 恢复时把加载的写入还原给任务 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `sanitize_untracked_values_in_send` | 洗掉 `Send` 载荷里的 UntrackedValue 键 |

## 参考

- [LangGraph 源码：pregel/_loop.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_loop.py)
- [LangGraph 源码：pregel/_task_status.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_task_status.py)
- [LangGraph 文档：durability 模式](https://langchain-ai.github.io/langgraph/concepts/persistence/#durability-modes)

## 与主核 / AI 的关系

挂起写入与主核的 WAL 记录是同一类东西：都是「已产生但尚未合并进主数据结构」的变更日志，都要求「先把日志持久化，再让结果对外可见」。差别在于主核的日志由存储引擎在页级别管理并有 LSN 顺序校验，这里的日志由 `put_writes` 按 task_id 分槽管理，顺序性靠「检查点等 future」这条弱保证。

对 AI 侧而言，这一页回答了一个常被问到的问题：`interrupt` 之后进程重启，为什么已经跑完的节点不会重跑。原因是 `commit` 把这些节点的输出写成了挂起写入并落了盘，恢复时 `_reapply_writes_to_succeeded_nodes` 把写入还原回任务，runner 只跑 `writes` 为空的那些。要让某个节点一定重跑，就得让它的输出不落盘或者被判定为未完成——这也是 `INTERRUPT` 与 `ERROR` 被特别对待的原因。

