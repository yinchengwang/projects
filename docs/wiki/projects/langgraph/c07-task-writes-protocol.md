---
project: langgraph
feature: task-writes-protocol
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_algo.py
    symbols: [WritesProtocol, PregelTaskWrites, Call]
---

# 任务写入协议对象

## 一句话职责

`WritesProtocol` 把「一批要合并进通道的写入」抽象成四个属性（路径、名字、写入、触发），让 `apply_writes`、`local_read`、`should_interrupt` 这些算法不必区分「写入来自正在跑的任务」还是「写入来自图输入、状态更新、中断应答」。

## 机制是什么

合并算法关心的是「这批写入是谁写的、在图的哪个位置写的、它因为什么被触发」。这三个问题与「写入是在任务里实时产生的、还是从检查点里读回来的、还是调用方通过 `update_state` 塞进来的」无关。`WritesProtocol` 就声明这四个属性，任何满足它的对象都能进 `apply_writes`。

源码里有两个实现：

- `PregelExecutableTask`（一个 dataclass）：真正的运行期任务，`writes` 是一个 `deque`，会被任务执行过程持续追加。
- `PregelTaskWrites`（一个 `NamedTuple`）：为「不属于任何 runnable 任务」的写入准备的轻量实现——图输入、`update_state`、中断应答、`get_state` 里算「若这些写入生效」的临时视图。

`Call` 是第三个被 refs 点到的对象，它不是 `WritesProtocol` 的实现，而是 PUSH 任务里「函数式调用」的载荷：`func` + `input`（`(args, kwargs)` 二元组）+ `retry_policy` + `cache_policy` + `callbacks` + `timeout`。它出现在任务路径的末尾，使 `prepare_single_task` 能把「函数式 API 的一次调用」与「`Send` 分发」区分开。

## 怎么实现的

**协议只要求四个只读属性。** `path` 是 `tuple[str | int | tuple, ...]`，编码任务在嵌套结构中的位置；`name` 是写入者名字，`apply_writes` 用它做 `versions_seen` 的键；`writes` 是 `(channel, value)` 序列；`triggers` 是触发通道名序列，`apply_writes` 用它推进 `versions_seen` 与判定 `bump_step`。协议声明成 `@property`，所以实现方可以返回动态值——`PregelExecutableTask` 的 `writes` 就是同一个 `deque` 对象，运行期持续增长。

**`PregelTaskWrites` 的四段构造。** 它是 `NamedTuple`，因此不可变、可哈希、可比较。源码里出现的调用形态有五类，各不相同：

- 图输入：`PregelTaskWrites((), INPUT, input_writes, [])`。路径为空元组、名字为 `INPUT`、无触发。空路径让排序键把它排在最前，无触发让 `bump_step` 不受它影响。
- NULL 写入：`PregelTaskWrites((), INPUT, null_writes, [])`，与上面同构，用于检查点里加载的 `NULL_TASK_ID` 写入。
- 状态更新/中断应答：`PregelTaskWrites((INTERRUPT, i), as_node, writes, [INTERRUPT])`。路径带 `(INTERRUPT, i)` 序号以避免同一次更新里多条写入的顺序歧义，触发集合为 `[INTERRUPT]`——这让 `apply_writes` 认为这是真超步（`bump_step` 为真），从而会走「通知未更新通道」与「可能 finish」的分支。
- 任务内部的读视图：`PregelTaskWrites(task_path[:3], name, writes, triggers)`。注意 `writes` 传的是任务自己的 `deque` 引用，所以条件边读到的始终是最新写入。截取 `path[:3]` 是因为更深的路径只用于嵌套任务的 id 计算。
- `Send` 任务：`PregelTaskWrites(translated_task_path, packet.node, writes, triggers)`，其中 `translated_task_path` 在原始路径末尾追加了一个布尔值，用来标记「这是否是一次函数式调用」。

**末尾布尔值的用途。** 两处赋值方向相反：`Send` 任务追加 `False`，`Call` 任务追加 `True`。`output_writes` 在遇到 `INTERRUPT` 写入时会检查 `task.path[0] == PUSH and task.path[-1] is True`，为真则不发射中断事件——因为函数式调用的中断由父任务负责上报，子任务再报一次会重复。所以这个布尔位不是 id 的填充，而是一条实打实的语义标记。

**`Call` 的字段选择。** 它带 `retry_policy`、`cache_policy`、`callbacks`、`timeout` 而不带 config。原因是它需要在父任务的执行线程里被构造（`_call` 与 `_acall` 里 `schedule_task(...)` 的第三参数），而 config 由 `prepare_push_task_functional` 从父任务 config 派生。`input` 的类型标注是 `tuple[tuple[Any, ...], dict[str, Any]]`，即预先拆好的 args 与 kwargs——因为缓存键计算需要 `cache_policy.key_func(*args, **kwargs)`，提前拆好省一次解包分派。

**`Call` 与 `Send` 在任务路径上的区别。** `Send` 任务路径是 `(PUSH, idx)`，`idx` 是在 `TASKS` 通道里的下标；`Call` 任务路径是 `(PUSH, 父任务路径, 父任务第几次 call, 父任务 id, Call 对象)`。后者更长，因为它必须编码「哪个任务的第几次调用」才能保证 id 唯一；前者只需「通道里第几个包」。这个差异直接落进 `task_id_func` 的入参。

## 关键取舍

**源码事实：** `WritesProtocol` 用 `Protocol` 而不是基类。因此 `PregelExecutableTask` 与 `PregelTaskWrites` 之间没有任何继承关系，`apply_writes` 只按结构消费。好处是运行期任务不必为「能被合并」付继承代价；代价是任何字段改名都会在不相关的文件里静默破坏兼容，类型检查器之外没有防线。

**源码事实：** `PregelTaskWrites` 是 `NamedTuple` 而不是 dataclass。这让它可以被排序、被当作字典键、被直接比较；`apply_writes` 恰好按 `task_path_str(t.path)` 排序，不可变对象在这里比可变对象安全得多。

**源码事实：** 同一个 `deque` 既是任务的写入缓冲，又被包进 `PregelTaskWrites` 作为读视图。因此「读到自己刚写的值」不需要任何同步机制——读的就是同一块内存。代价是这个视图的有效期与任务绑定：任务结束后 `deque` 可能被清空或丢弃，此时读到的就是空序列。

**架构推论：** 把「写入来源」抽象成四个属性，使得 `update_state` 与图输入可以复用同一条合并路径。这是「外部注入状态」与「节点产生状态」在语义上完全等价的原因；也意味着任何为「真实任务」加的合并特例都会自动作用于外部更新。

**架构推论：** `Call` 不进 `WritesProtocol`，说明函数式调用在合并层是不可见的。它的产物最终以普通写入的形式进入父任务的 `deque`，因此从 `apply_writes` 的视角看，「函数式调用」和「节点内普通计算」没有区别。这把调度的复杂度完全关在 `prepare_*` 与 runner 里。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_algo.py` | `WritesProtocol` | 四属性写入源协议：path/name/writes/triggers |
| `libs/langgraph/langgraph/pregel/_algo.py` | `PregelTaskWrites` | 非任务来源写入的轻量不可变实现 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `Call` | 函数式调用的载荷：func/input/策略/callbacks/timeout |
| `libs/langgraph/langgraph/pregel/_algo.py` | `local_read` | 接收 `WritesProtocol` 作为「当前任务的写入视图」 |
| `libs/langgraph/langgraph/types.py` | `PregelExecutableTask` | 运行期任务，`writes` 为 `deque` |

## 参考

- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 源码：types.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/types.py)
- [LangGraph 文档：update_state 与 as_node](https://langchain-ai.github.io/langgraph/concepts/persistence/#update-state)

## 与主核 / AI 的关系

用结构协议而非继承来统一「写入来源」，与主核用回调式的 I/O 向量而不是基类来统一「存储后端」是同一种解耦：主核不要求后端继承某个 class，只要求它提供 read/write 两个行为，于是内存后端与磁盘后端可以互换。这里不要求写入源继承什么，只要求它给出四个属性，于是任务、输入、外部更新可以互换。

对 AI 侧而言，这一页解释了 `update_state` 为什么能改变「下一步跑什么」：它的写入被包装成 `PregelTaskWrites`，触发集合写成 `[INTERRUPT]`，于是 `apply_writes` 会把它当成一个真超步、推进版本、并让订阅这些通道的节点在下一步被唤醒。反过来，如果只是想让状态变而不触发任何节点，就需要避免让写入进入这条路径——这正是 `as_node` 参数存在的原因：它决定 `versions_seen` 记在谁名下，从而影响谁能被唤醒。

