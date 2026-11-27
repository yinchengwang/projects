---
project: langgraph
feature: delta-snapshot
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_checkpoint.py
    symbols: [delta_channels_to_snapshot, delta_channels_overwritten, delta_channels_with_pending_writes]
---

# 增量通道快照与溢出判定

## 一句话职责

这三个纯谓词决定「哪些增量通道这一轮必须落全量快照」，分别对应节奏到期、被 `Overwrite` 废弃、以及带着别人待取写入起新分支三种触发原因。

## 机制是什么

`DeltaChannel` 只在检查点里存一个哨兵，真实值靠从最近的快照出发、按序重放祖先写入重建。这套设计的成本全在「回溯深度」上：重放永不停止，加载就是 O(线程长度)。于是必须有一条「多久打一次全量」的规则，快照点就是回溯的停车点。

三个判定函数互不重叠，各自回答一种「必须现在打」的情形：

- `delta_channels_to_snapshot`：按节奏到期。两个计数器任一满足即打——该通道自身累计更新数达到 `snapshot_frequency`，或距离上次快照的超级步数达到 `DELTA_MAX_SUPERSTEPS_SINCE_SNAPSHOT`（默认 5000，可由环境变量覆盖）。
- `delta_channels_overwritten`：写入里带了 `Overwrite`。`Overwrite` 的语义是「丢弃此前全部历史」，若不留快照，读路径会从更早的快照起步、把这次 `Overwrite` 扔掉的写入又重放回来。
- `delta_channels_with_pending_writes`：某个检查点上还挂着未消费的写入。那些写入属于「消费了它的那个子检查点」，但检查点本身没记录是哪个子，于是从它起的新分支无法判断该不该跳过。保守做法：把这些通道全部快照，新分支的回溯就不会重放它们。

## 怎么实现的

**节奏判定。** `delta_channels_to_snapshot` 先做三重过滤：不是 `DeltaChannel` 的跳过、当前不可用（`is_available()` 为假，即值仍是 `MISSING`）的跳过、在 `channel_versions` 里没有条目的跳过——最后这条是因为从未写过的通道没有版本，`put` 不会为它存 blob，判它快照也是空转。过滤后取 `counters_since_delta_snapshot` 里的 `(updates, supersteps)`，缺省 `(0, 0)`，两个阈值用逻辑或。

函数是纯谓词：不修改通道、不修改计数器。计数器怎么涨、涨完何时归零，都在调用方——执行循环的 `_put_checkpoint` 每步给每个 delta 通道 `supersteps += 1`，只有在本步 `updated_channels` 里才 `updates += 1`；被判定要快照的通道，计数器重置为 `(0, 0)` 并且全零条目不落 metadata。

`supersteps` 这条上限是专门为「通道还在但不写了」准备的：只有 `updates` 计数的话，一个写过一阵就停的通道永远到不了 `snapshot_frequency`，某次加载就要一路回溯到线程开头。

**Overwrite 判定。** `delta_channels_overwritten` 只做类型与标志检查：通道是 `DeltaChannel` 且 `_get_overwrite(value)[0]` 为真。它不看版本、不看可用性——`Overwrite` 已经是一个确定的事实。返回值被调用方并进强制快照集合，与节奏判定取并集。

**悬挂写入判定。** `delta_channels_with_pending_writes` 遍历 `pending_writes` 的三元组，取第三段通道名，若对应 spec 是 `DeltaChannel` 就收进集合。注意它用的是**声明规格**（`specs`）而不是活的通道对象，因为调用点未必已经水合出通道实例。写入可能来自图输入、`update_state`、`Command` 更新，来源不影响判定。

**参数形态的差异。** 节奏判定要 `counters` 与 `channel_versions` 两个额外输入，因为它必须知道进度；另外两个只看「写入里有什么」，签名因此只有 `specs` 加写入序列。

## 关键取舍

**源码事实：** 快照节奏由「更新数」与「超级步数」两个维度共同约束，而不是单一计数。单维度必然在某一侧失衡：只数更新则静默通道回溯无界，只数步数则高频通道在两步之间就能堆出海量待重放写入。

**源码事实：** 悬挂写入的判定采取「宁可多打快照」的保守策略。源码注释写明检查点不记录 pending writes 属于哪个子节点，而查子节点需要列举整个线程、saver 无法廉价做到。所以用一次全量快照换掉一次不确定的分支语义。

**源码事实：** 三个判定都不做实际快照，只回答「该不该」。真正写 `_DeltaSnapshot` 的是 `create_checkpoint`，而它需要 `get_next_version` 才能 bump 版本。把判定与执行拆开，使 `update_state` 与执行循环能共用同一套判定而各自决定副作用。

**架构推论：** `supersteps` 上限是系统级常量而非每通道可配，意味着「回溯成本上限」被当成全局 SLA 而不是数据类型属性。代价是无法为某个特别贵的通道单独放宽，收益是运维只需一个环境变量就能给全线程设成本天花板。

**架构推论：** 第三个函数的存在暴露了一个设计缺口：待取写入缺少归属信息。当前用快照规避，本质是「用空间换正确性」；若将来写入记录里带上子检查点标识，这个函数可以退化成更精确的判定。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `delta_channels_to_snapshot` | 双计数器到期判定，纯谓词 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `delta_channels_overwritten` | 识别被 `Overwrite` 废弃历史的通道 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `delta_channels_with_pending_writes` | 悬挂写入涉及的通道，保守全量快照 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `create_metadata_for_update_state_api` | `update_state` 侧推进同一套计数器 |
| `libs/langgraph/langgraph/_internal/_config.py` | `DELTA_MAX_SUPERSTEPS_SINCE_SNAPSHOT` | 超级步上限常量，可由环境变量覆盖 |

## 参考

- [LangGraph 源码：pregel/_checkpoint.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_checkpoint.py)
- [LangGraph 源码：channels/delta.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/delta.py)
- [LangGraph 文档：持久化与检查点](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

这组判定的结构与主核的「什么时候必须做全量刷盘 / 什么时候只留增量日志」一致：都以「重放开销必须被有界」为第一约束，都用「计数到阈值」做触发，都接受「某类操作（这里是 `Overwrite`）意味着历史作废，必须立即落全量」这类特殊规则。差别在主核还要保证崩溃恢复点的原子性，这里只需保证读取语义正确。

对 AI 侧而言，两件事从这一页可以直接推出：其一，长线程里 delta 通道的内存延迟不会无限增长，因为 5000 步内必然落一次快照；其二，若某节点返回 `Overwrite` 式更新，那一轮写入会比平时更重，因为它顺带触发一次全量快照。把高频 `Overwrite` 放在热路径上，等于持续自废快照优化。
