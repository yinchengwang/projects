---
project: langgraph
feature: prepare-next-tasks
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_algo.py
    symbols: [prepare_next_tasks, _TaskIDFn]
---

# 下一批任务的推导

## 一句话职责

`prepare_next_tasks` 在超步边界上回答一个问题：「这一步该跑哪些任务，每个任务拿什么输入」——它把通道版本、待处理写入、触发关系三者合一，产出一个以任务 id 为键的任务字典。

## 机制是什么

任务是 Pregel 的执行单元，但「有哪些任务」不是图结构静态决定的，而是每一步现场算出来的。`prepare_next_tasks` 做两件事：

1. **PUSH 任务**：从 `TASKS` 通道（`Topic(Send)`）里取出待执行的 `Send` 包，每个包变成一个任务。这类任务由上游节点动态发起，名字由包里的 `node` 决定。
2. **PULL 任务**：检查每个节点注册的触发通道，若其中任何一个的版本比该节点「已见」的版本更新，就为它生成一个任务。这类任务由边与触发关系决定。

两类任务统一进一个 `dict[str, PregelTask]`，键是任务 id。任务 id 由检查点 id、命名空间、超步号、节点名、任务种类（`PULL`/`PUSH`）和一个区分性子串共同做哈希得到，因此**同一个超步里同一个节点的任务 id 是可重现的**——这是恢复时能把落盘写入对回到具体任务上的前提。

返回类型由 `for_execution` 决定：`False` 时返回 `dict[str, PregelTask]`（只有 id、名字、路径，用于读状态与渲染），`True` 时返回 `dict[str, PregelExecutableTask]`（带 config、通道写入队列、读函数、缓存键、超时策略，可直接交给 runner）。

## 怎么实现的

**入口的两处预处理。** `input_cache` 是一个跨任务共享的字典，键是「mapper + 通道名元组」，值是该节点算好的输入。多个节点若订阅完全相同的通道组合与 mapper，输入只算一次。`checkpoint_id_bytes` 把检查点 id 从十六进制转成字节，供 id 哈希使用；`null_version` 从现有版本里取一个「同类型的零值」，用作版本比较的缺省下界。

**候选节点的三种来源，按成本从低到高。** 先是 PUSH；然后若调用方同时给了 `updated_channels` 与 `trigger_to_nodes`，就只把这些更新通道能触发的节点当候选（`sorted` 保证确定性）；否则若 `channel_versions` 为空（全新图、第一次运行）就一个候选都没有；最后退化成扫全部节点。这条降级链是性能优化，它的正确性依赖一个前提：`trigger_to_nodes` 必须准确反映「哪些通道能触发哪些节点」。若两者不一致，快速路径会漏任务。

**`prepare_single_task` 的三路分派。** 它按任务路径的第一段分派：`(PUSH, path, idx, task_id, Call)` 走函数式 API 的任务准备；`(PUSH, idx)` 走 `Send` 任务准备；`(PULL, name)` 走节点触发任务准备。三条路最后都构造同样的 `PregelExecutableTask`，但输入、任务 id 的组成部分、缓存键的命名空间各不相同。

**PULL 任务的五步。**

1. 节点名不在 `processes` 里 → 返回 `None`（静默跳过，容错来自「节点表可能被动态构造」）。
2. `checkpoint_null_version is None` → 返回 `None`。这意味着检查点里一个版本都没有，`_triggers` 无从比较。
3. `_triggers` 判定是否该触发：该节点某个触发通道可用，且其版本严格大于 `versions_seen[node][chan]`。若该节点从未出现在 `versions_seen` 里（`seen is None`），退化成「只要触发通道可用就触发」。
4. 生成任务 id 与任务命名空间。命名空间形如 `父命名空间:节点名:任务id`，它同时用于检查点分区与事件流路由。
5. 造 scratchpad 与输入。`_proc_input` 按节点 `channels` 字段的类型分叉：列表则收集所有可用通道的值成字典，字符串则取该通道的值（不可用就返回 `MISSING`，任务被丢弃），既非列表也非字符串则抛 `RuntimeError`。`MISSING` 与「值为 `None`」是两回事：前者表示「这个任务不该跑」，后者是合法输入。

**`for_execution` 分叉。** 不执行时直接返回三字段的 `PregelTask`。要执行时还要做六件事：合并节点自身 metadata 进任务 metadata；按 `proc.cache_policy or cache_policy` 算缓存键（键含 `CACHE_NS_WRITES`、节点标识、节点名与参数摘要）；用 `runtime.override` 造一个带 `execution_info` 的任务级 `Runtime`；`patch_config` 把任务 id、`CONFIG_KEY_SEND`（即 `writes.extend`）、`CONFIG_KEY_READ`（`local_read` 的偏应用）、`CONFIG_KEY_CHECKPOINT_NS`、`CONFIG_KEY_SCRATCHPAD` 等注入 config；最后把 `proc.flat_writers`、`proc.subgraphs`、`proc.timeout` 一并挂上。

`CONFIG_KEY_SEND` 绑的是 `writes.extend`——一个 `deque` 的 `extend` 方法。源码注释点明这是刻意的：`deque.extend` 是线程安全的，因此多个执行线程往同一任务写不需要额外加锁。这是整个并发模型里最省事的一处设计。

**任务 id 的算法切换。** 检查点 `v > 1` 用 `_xxhash_str`（XXH3-128），否则用 `_uuid5_str`（SHA-1）。两者都把「命名空间字节 + 各路径段」拼成摘要，再格式化成 UUID 形态。切换是为了兼容旧检查点：旧版本线程里的任务 id 是按 SHA-1 算的，恢复时必须用同一算法才能对上。`_TaskIDFn` 就是这个可替换算法的协议形态——它只声明 `(namespace, *parts) -> str`，让调用方不必关心用的是哪种哈希。

## 关键取舍

**源码事实：** 任务 id 是内容的函数，不是自增序号。好处是同一个超步重算任务会得到同样的 id，落盘写入能对上；坏处是「同一节点同一超步要跑两次」必须靠不同的路径段区分，所以 `Send` 任务用 `idx`、`Call` 任务用父路径与计数器——这些路径分量不是装饰，而是 id 的唯一性来源。

**源码事实：** `_triggers` 用「严格大于」比较版本，而所有被更新通道在同一超步共用同一个版本号。因此「某通道这一步被写了」恰好会唤醒一次订阅它的节点，不会因为同版本重复触发。

**源码事实：** `versions_seen` 里没有该节点条目时退化到「通道可用即触发」。这保证了第一次运行能启动，但也意味着对全新线程而言，「谁先跑」只由 PULL 候选的排序决定，与版本无关。

**源码事实：** 缓存键包含节点标识而非任务 id。因此同一节点在不同超步、不同 thread 上算出的缓存键可能相同——缓存是跨运行的，粒度为「节点 + 参数摘要」。这是性能特性，也是正确性风险点：任何依赖隐藏外部状态的节点都不该开缓存。

**架构推论：** 因为 `prepare_next_tasks` 同时被 `PregelLoop._prepare_tasks_and_emit_checkpoint`、`update_state`、`get_state` 复用，它必须在「不执行」也能算准。这是 `for_execution` 布尔参数存在的原因；代价是同一条代码路径里有大量 `if for_execution:` 分支，两半的演化容易不同步。

**架构推论：** PUSH 任务从 `TASKS` 通道读，而 `TASKS` 是 `Topic(accumulate=False)`。所以 `Send` 包只活一个超步：被读走就清空，下一步不会重放。要「重放」只能靠检查点里的待处理写入——这正是恢复路径把 `pending_writes` 传进来的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_algo.py` | `prepare_next_tasks` | PUSH + PULL 两类任务的统一推导与 id 索引 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_TaskIDFn` | 任务 id 生成算法的协议，兼容新旧检查点 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `prepare_single_task` | 按任务路径三段分派，构造可执行任务 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_triggers` | 版本比较判定节点是否该唤醒 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_proc_input` | 按节点 channels 形态组装输入，含 `MISSING` 语义 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_xxhash_str` / `_uuid5_str` | 两代任务 id 哈希实现 |

## 参考

- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 文档：Send 与 map-reduce](https://langchain-ai.github.io/langgraph/concepts/low_level/#send)
- [LangGraph 文档：Pregel 超步中的任务选择](https://langchain-ai.github.io/langgraph/concepts/low_level/#pregel-algorithm)

## 与主核 / AI 的关系

「用内容哈希当主键，从而让重算与落盘对得上」与主核的确定性页标识是同一种思路：主核用表空间 + 页号定位，页号由逻辑结构决定而非分配顺序，因此重做日志能精确对位。这里把「内容哈希」的范围扩大到「检查点 id + 命名空间 + 超步 + 节点 + 种类」，效果相同：任务不需要持久化身份，身份可以从上下文重算。

对 AI 侧而言，这一页解释了 agent 里 `Send` 分发的成本模型。每个 `Send` 都会在下一步变成独立任务、独立 config、独立 runtime，且被 `Topic(accumulate=False)` 保证只活一步。因此「一个 planner 分发一百个子任务」等于下一步起一百个任务；若还想让它们的结果在再下一步才聚合，就必须显式写入一个聚合通道，而不能指望任务本身携带上下文。

