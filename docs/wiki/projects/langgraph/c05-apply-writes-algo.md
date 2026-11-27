---
project: langgraph
feature: apply-writes-algo
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_algo.py
    symbols: [apply_writes, increment, local_read]
---

# 写入合并算法

## 一句话职责

`apply_writes` 是超步边界上唯一把「一批写入」变成「通道新状态」的地方：它按确定性顺序合并写入、推进通道版本、并回报本步真正被更新的通道集合；`local_read` 是它的只读旁路，让条件边能看见「若这批写入生效，状态会是什么样」。

## 机制是什么

超步结束时，每个任务手里都有一串 `(channel, value)` 写入。`apply_writes` 接收四样东西：当前 `checkpoint`、通道表 `channels`、写入来源 `tasks`（统一抽象成 `WritesProtocol`）、可选的 `get_next_version`，以及 `trigger_to_nodes` 映射。它返回 `set[str]`——本步被更新且当前可用的通道名集合。这个返回值随即被当作下一步任务推导的 `updated_channels`，是「触发传播」的唯一信号源。

版本是这套算法的骨架。每个通道在 `checkpoint["channel_versions"]` 里有一个版本号，`versions_seen[node][channel]` 记录「该节点上次看到的是哪个版本」。`apply_writes` 同时更新两者：前者推进为本步的统一新版本，后者推进为「该任务在本步开始时看到的版本」。

## 怎么实现的

**第一段：确定性排序。** `tasks` 先按 `task_path_str(t.path)` 的写入排序键排序。排序理由是注释里写明的「保证更新应用的顺序确定」——同一批写入若顺序不定，聚合型通道的 reducer 会得到不同结果，两次相同输入的重放就可能不一致。

**第二段：是否推进超步。** `bump_step = any(t.triggers for t in tasks)`。只要有一个任务带触发通道，这一步就算「真超步」；若全是空触发（例如 NULL 任务单独写输入），就只更新被写到的通道，不推进其他通道、不调 `finish()`。这个判断把「图的输入写入」与「节点的执行写入」分开。

**第三段：推进 `versions_seen`。** 对每个任务，把它所有触发通道的**当前**版本记进它自己的 `versions_seen` 条目。注意用的是 `checkpoint["channel_versions"][chan]`，即本步的版本，而不是新版本——它表达的是「我这个任务是在看到这个版本时被唤醒的」。若某个触发通道不在 `channel_versions` 里（例如从未被写过的通道），直接跳过而不是写一个空值。

**第四段：算新版本。** 有 `get_next_version` 时，取 `channel_versions` 的**最大值**作为输入算下一个版本；表为空则传 `None`。所以同一超步里所有被更新的通道共用同一个版本号，这也是为什么版本号能当「超步序号」用。

**第五段：消费读过的通道。** 对每个任务触发过的、非保留、且存在的通道调 `consume()`；返回 `True` 就把该通道版本推进到新版本。「消费」是 `Topic` 这类通道的语义（取走后清空），把它也计入版本推进，是因为「被消费」本身是一次可见的状态变化。

**第六段：按通道分组写入。** 保留通道（`NO_WRITES`、`PUSH`、`RESUME`、`INTERRUPT`、`RETURN`、`ERROR`、`ERROR_SOURCE_NODE`）被显式跳过——它们已经由 runner 直接写进挂起写入，不参与通道合并。写入未知通道时记一条 warning 并忽略，不抛异常。这条宽容是必要的：`ChannelWrite` 里可能带着为未来节点准备的名字。

**第七段：合并。** 对每个通道调 `channels[chan].update(vals)`，返回真则推进版本；再判 `is_available()`，可用才加入 `updated_channels`。不可用的通道（例如屏障还没到齐、`LastValueAfterFinish` 还没 finish）即使收到写入也不该触发下游——注释明确写了这一点。

**第八段：通知未更新的通道。** `bump_step` 为真时，对每个「可用且本步没被更新」的通道 `update(EMPTY_SEQ)`。这是给 `NamedBarrierValue` 这类「等齐」通道的一次机会：没人写它，但它可能该被重置。同理只在 `is_available()` 之后才计入 `updated_channels`。

**第九段：收尾。** `bump_step` 为真且 `updated_channels` 与 `trigger_to_nodes` 完全不相交时，调每个通道的 `finish()`。这就是「这一个超步没人被触发，所以整张图要停了」的判定：让 `LastValueAfterFinish` 这类通道把「结束可见」的值暴露出来，最后再收一次 `updated_channels`。

**`local_read` 的两条路。** 它被注入到任务 config 的 `CONFIG_KEY_READ` 上，供条件边与节点内部读状态。参数里带 `task: WritesProtocol`——正是「当前任务」。它把 `task.writes` 里命中 `select` 的值按通道聚起来。`fresh=True` 时走「应用写入」路径：对**每个**通道做 `copy()`，把该通道命中 select 的写入更新进去，构造一份本地通道表，再 `read_channels`。所以 `fresh` 的效果是「看见我自己刚写的值」，而且只应用当前任务自己的写入，不是全批任务。

`select` 是列表时，托管值键被单独摘出来（`managed_keys`），因为它们只能通过 `managed[k].get(scratchpad)` 取，不能从通道表读。`select` 是字符串时不做这个区分——这是一个刻意的窄化，用字符串形式读托管值拿不到值。

**`increment` 是缺省版本函数。** 没有检查点器时它就是 `get_next_version`：`current + 1 if current is not None else 1`。所以「无检查点」的运行也有一致的版本推进，只是版本不落盘。有了检查点器就换成 `checkpointer.get_next_version`，让后端可以决定版本形态（例如时间戳）。

## 关键取舍

**源码事实：** 排序键是任务路径的字符串表示，不是任务 id。路径编码了「在哪个父任务下第几个 push」，所以同一结构的两轮运行会得到同样的顺序；任务 id 里含 `step`，用它排序在跨轮重放时反而不稳定。

**源码事实：** 一个超步内所有更新共用同一个版本号。这让版本比较退化成「是否大于等于上一超步」，代价是无法在一个超步内区分两个通道的先后。需要精细顺序的语义只能靠写入在列表里的位置表达（如特殊通道的槽位）。

**源码事实：** 写入未知通道只 warning 不报错，而写入不可用通道则静默丢弃（不进 `updated_channels`）。前者是对未来的宽容，后者是屏障/结束语义的必要条件；两者都让「写入丢失」在运行期难以察觉。

**源码事实：** `finish()` 的触发条件是 `updated_channels.isdisjoint(trigger_to_nodes)`，而不是「没有任务」。区别在于：本步确实更新了通道，但更新的这些通道没有触发任何节点——这才是「图要停了」。用「无任务」判定会把「本步被中断」误判成结束。

**架构推论：** `apply_writes` 是纯函数式的：它只修改传入的 `checkpoint` 与 `channels`，不读全局、不发事件、不落盘。因此同一批写入可以被重放多次而无副作用，这也是恢复路径敢在 `__exit__` 里再调一次它的原因。

**架构推论：** `local_read` 的 `fresh` 只应用**当前任务的**写入，而不是全批任务的写入。这保证了条件边看到的是「我自己造成的状态变化」，不会因为并行的另一个任务写了同名字段而改变路由判断——路由的可预测性优先于「看到最新全局状态」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_algo.py` | `apply_writes` | 排序写入、推进版本与 seen、合并、通知与 finish |
| `libs/langgraph/langgraph/pregel/_algo.py` | `local_read` | 注入 `CONFIG_KEY_READ` 的读函数，支持只看本任务写入 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `increment` | 无检查点器时的缺省版本推进 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `should_interrupt` | 用 `versions_seen[INTERRUPT]` 判定自上次中断是否有更新 |
| `libs/langgraph/langgraph/pregel/_write.py` | `_assemble_writes` | 把写条目摊平成 `(通道, 值)` 供本算法消费 |

## 参考

- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 源码：channels/base.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/base.py)
- [LangGraph 文档：状态与 reducer](https://langchain-ai.github.io/langgraph/concepts/low_level/#reducers)

## 与主核 / AI 的关系

「一批变更按确定顺序合并、共用同一个版本号、只有可用的槽位才对外可见」与主核的批量提交同构：主核在提交时按确定顺序应用日志、推进页面版本，并把「脏页可用」与「数据可见」分开判定。差异在于主核的合并是覆盖写成页，这里的合并必须经过通道的 reducer，所以顺序敏感度更高——主核可以靠页面重做保证幂等，这里只能靠排序保证确定性。

对 AI 侧而言，这一页解释了 reducer 为什么必须满足结合律之外还要满足「顺序无关或顺序固定」。消息历史用 `add_messages` 这类 reducer 时，同一超步里两个并行节点各追加一条消息，最终顺序由任务路径排序决定——这既保证了可重现，也意味着**不能依赖节点的实际完成时序**来决定消息顺序。

