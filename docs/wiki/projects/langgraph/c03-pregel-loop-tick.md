---
project: langgraph
feature: pregel-loop-tick
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_loop.py
    symbols: [PregelLoop, tick, after_tick]
---

# PregelLoop 单轮步进

## 一句话职责

`PregelLoop` 是一张图「一次运行」的全部可变状态的家：它按 BSP 语义把运行切成超步，`tick()` 负责「决定这一步跑什么」，`after_tick()` 负责「把这一步的结果落地成新检查点」。

## 机制是什么

一次运行从进入 `SyncPregelLoop`（或异步版）的上下文开始，到退出为止。`__enter__` 做六件事：按 config 决定加载哪个检查点（显式 `checkpoint_id`、父图的 replay 状态、或该 thread 的最新检查点）、归一 `checkpoint_config`、把 `pending_writes` 从落盘形态拷进内存、用 `BackgroundExecutor` 起后台执行器、`channels_from_checkpoint` 从检查点恢复通道与托管值、然后调 `_first` 处理本次输入。之后主循环是 `while loop.tick(): runner.tick(...); loop.after_tick()`，三步一循环，直到 `tick()` 返回 `False`。

`status` 字段是这条流水线的状态机，取值 `input`/`pending`/`done`/`draining`/`interrupt_before`/`interrupt_after`/`out_of_steps`。`tick()` 与 `after_tick()` 各自负责把 status 从 `pending` 推到某个终态或留在 `pending`；调用方只看 `tick()` 的布尔返回值，退出后再查 status 决定是抛递归上限还是抛 drain。

超步计数有两个字段：`step` 是当前超步号，`stop = step + recursion_limit + 1`。`tick()` 的第一条判断是 `step > stop`，命中就置 `out_of_steps`、把最后保存的那个检查点也作为输出发出去、返回 `False`。`step` 的自增不在 `tick()` 里，而在 `_put_checkpoint` 末尾（且只在非退出调用时自增）。

## 怎么实现的

**`tick()` 的七段判断，顺序本身就是语义。**

1. 超步上限。命中 `out_of_steps` 时仍然调用 `_prepare_tasks_and_emit_checkpoint()`，源码注释写明「把上一步保存的检查点也流出去，像一个真的跑了的 tick 那样」。这让 `debug`/`checkpoints` 两种流模式的输出条数与实际超步数一致。
2. 推导任务：`_prepare_tasks_and_emit_checkpoint()` 调 `prepare_next_tasks`，把结果赋给 `self.tasks`，然后在有检查点器时发一次 `checkpoints` 流事件。注意这一步在「判断有没有任务」之前，所以「空步」也是能看到 debug 输出的。
3. 空则结束：`self.tasks` 为空 → `done`，返回 `False`。
4. 重放已完成的写入：`_reapplies_pending_writes` 为真（即 `not is_replaying`）且有待处理写入时，`_reapply_writes_to_succeeded_nodes` 把加载进来的写入塞回对应任务，`_resume_error_handlers_if_applicable` 处理「上次失败、这次该走错误处理器」的任务。这一步是恢复语义的核心：把已完成的输出复原到内存任务上，runner 只跑 `writes` 为空的那些。
5. 进之前的断点：`interrupt_before` 命中时置 `interrupt_before` 并直接 `raise GraphInterrupt()`。真正的抑制发生在 `__exit__` 里注册的 `_suppress_interrupt`。
6. drain 检查放在断点检查之后。源码注释给了理由：恢复运行要越过下一步的断点，如果先查 drain 就会把断点跳过。
7. 最后把 `tasks` 发成 debug 流，并把「已经被复原写入的任务」的输出按 `cached=True` 补发一次——这些任务这一步不会真跑，但它们的输出必须让流消费者看到。

**`after_tick()` 是把内存态落成检查点的唯一入口。**

顺序是：先扫一遍本步所有任务写入，把带 `Overwrite` 的 delta 通道标记为「必须快照」；再把「加载进来但没被复原回去」的写入（属于被丢弃或重跑的 `Send`、重新执行的错误处理器）也标记为必须快照；然后调 `apply_writes` 得到本步真正更新的通道集合 `updated_channels`；若更新集合与 `output_keys` 有交集就发一次 `values`；再为 exit 模式的增量写入累加器记录本步内容；清空 `checkpoint_pending_writes`；把 `is_replaying` 置 `False`（只有第一轮重放已完成任务）；调 `_put_checkpoint({"source": "loop"})`；最后检查 `interrupt_after`，命中则先 `_prepare_tasks_and_emit_checkpoint()` 再抛 `GraphInterrupt`——注释点明「下一轮本来要发这个检查点但那一轮已经不会跑了」，所以在这里补发。

**`_put_checkpoint` 承担的不只是保存。** 它还负责 delta 通道的快照节奏计数（每个 delta 通道维护 `(更新次数, 超步数)` 二元组）、决定哪些 delta 通道这一轮要快照、把 `TASKS` 通道里的 `UntrackedValue` 洗掉再存、以及用 `submit` 把保存排到后台并「等上一个保存完成」以保证检查点按序落盘。它用 `metadata is self.checkpoint_metadata` 这个对象身份判断区分「退出时的保存」与「超步间的保存」，退出那次不自增 `step`、也不重算计数——源码注释承认这个写法别扭并留了 TODO，说明这是待偿的技术债。

**流式输出的统一出口是 `_emit`。** 它做两件事：按 `self.stream.modes` 过滤；把 `checkpoints` 与 `tasks` 两种模式在 `debug` 模式开启时额外重映射成带 `step`/`timestamp`/`type` 包装的 `debug` 事件。所以「开 `debug` 就能看到检查点与任务」不是两套代码，而是同一份 payload 的两种投影。

## 关键取舍

**源码事实：** `tick()` 与 `after_tick()` 严格分离，`tick()` 不改检查点、`after_tick()` 不跑任务。这条界线让「本步的所有通道更新对其他任务不可见」成为结构上的保证，而不是靠约定——任务只在 `tick()` 里被推导和读取输入，通道只在 `after_tick()` 里被批量更新。

**源码事实：** `tick()` 在「没有任务」与「超出步数」两种终止上都先做了发流动作。代价是终止路径也要访问检查点元数据；收益是流消费者看到的超步序列不会因为终止方式不同而缺一条。

**源码事实：** `step > stop` 用的是 `>`，而 `stop = step + recursion_limit + 1`。多出来的两步是给「输入写入步」与「最后一步之后的那次空 tick」留的位置；把边界写成 `>` 而不是 `>=` 是刻意的，因为第一轮 tick 只推导任务、不产生新超步。

**架构推论：** 因为 `after_tick()` 里先 `apply_writes` 再 `_put_checkpoint`，检查点描述的是「本步写入已生效之后」的状态。若在两者之间抛异常（例如 delta 通道快照计算失败），内存通道已前进但落盘没有，这一轮的状态就只存在于内存——退出处理会再存一次最终检查点来收尾。

**架构推论：** `is_replaying` 只在 `after_tick()` 里被清掉，意味着「重放已完成任务」这件事只可能发生在第一轮。这直接支撑了恢复语义：第一轮把上次已完成的输出复原，后续轮次一律按正常流程跑。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_loop.py` | `PregelLoop` | 单次运行的全部可变状态与状态机 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `tick` | 超步上限、任务推导、写入复原、断点与 drain 判断 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `after_tick` | 写入合并、输出发射、delta 累加、清空待写入、保存检查点、后置断点 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_prepare_tasks_and_emit_checkpoint` | 任务推导与 checkpoints/debug 流 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_put_checkpoint` | 检查点创建、计数推进、后台保存与顺序保证 |
| `libs/langgraph/langgraph/pregel/_loop.py` | `_emit` | 流模式过滤与 debug 重映射 |

## 参考

- [LangGraph 源码：pregel/_loop.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_loop.py)
- [LangGraph 文档：Pregel 算法与超步](https://langchain-ai.github.io/langgraph/concepts/low_level/#pregel-algorithm)
- [LangGraph 文档：持久化模式（durability）](https://langchain-ai.github.io/langgraph/concepts/persistence/#durability-modes)

## 与主核 / AI 的关系

「先读后写、中间不许看」这条纪律与主核的写事务边界同构：主核在一个事务里累积改动，提交时才把页刷出去，期间其他读只能看到旧版本。`tick`/`after_tick` 用同一套手法把「通道在超步内不可变」变成免费性质，代价是任何需要「本步内看到本步写入」的需求都得走 `local_read` 这类显式旁路。

对 AI 侧而言，这一页解释了 agent 循环的可见性：一个节点写下的状态，同一步里的其他并行节点看不到。若 agent 的多个工具调用需要互相看到中间结果，就必须拆成两个超步——这不是实现细节，而是 `tick` 的结构决定的。

