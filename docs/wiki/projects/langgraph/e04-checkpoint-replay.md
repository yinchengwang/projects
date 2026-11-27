---
project: langgraph
feature: checkpoint-replay
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_checkpoint.py
    symbols: [_replay, _delta_channels_to_replay, checkpoint_superseded]
---

# 写入重放与超时判定

## 一句话职责

`_delta_channels_to_replay` 挑出「本地检查点里没有值、必须回祖先找」的通道，`_replay` 把找到的历史折叠成一个活通道实例，`checkpoint_superseded` 判定被寻址的检查点是否已被线程抛在身后。

## 机制是什么

前两页建立了「大部分步骤不写全量快照」的前提，加载路径就必须补上逆向过程。检查点的 `channel_values` 对未到快照点的 delta 通道是空的，`from_checkpoint` 只能得到空值；真实状态要沿父链向上找到最近的存值点（一个 `_DeltaSnapshot` 或迁移遗留下来的普通值），把它当种子，再把从种子之后到目标检查点之间、这条链上的写入按序折叠进去。

这段逻辑被压成两步：一个「谁需要重放」的筛选函数，一个「怎么重放一条历史」的折叠函数。二者之间隔着一个 saver 调用——所有需要重放的通道被打包成一次 `get_delta_channel_history` 请求，避免逐通道回溯。

`checkpoint_superseded` 是另一件事，但同属恢复语义：它回答「我要恢复的那个检查点，还是不是这个线程的头」。

## 怎么实现的

**筛选条件。** `_delta_channels_to_replay` 要求同时满足三条：spec 是 `DeltaChannel` 或 `BinaryOperatorAggregate`；通道名在 `checkpoint["channel_versions"]` 里；`channel_values` 里没有这个名字（用 `MISSING` 判）。三个条件的用意各不相同。

第二条排掉「从未被写过」的通道：没有版本就是空值，语义上正确，而若让它去回溯，链上不会出现任何快照停车点，每次加载线程都要把全部祖先读一遍。

第三条排掉「本检查点已经直接有值」的通道：`_DeltaSnapshot` 与迁移遗留的普通值都能被 `from_checkpoint` 直接消费，不需要重放。

把 `BinaryOperatorAggregate` 也算进来是个刻意的补丁：它每个版本都存值，因此「它有版本但没存值」只可能发生在「注解从聚合通道改成 delta 通道」这一迁移场景，这类线程需要按 delta 语义重建。

**重放。** `_replay` 先 `spec.from_checkpoint(history.get("seed", MISSING))` 拿种子——没有种子就是空值起步（链上没找到任何存值点）。然后分两条路：

若重建出来的仍是 `DeltaChannel`，直接调 `replay_writes(history["writes"])`，把整段写入一次性交给 reducer。这依赖 reducer 满足批处理不变性（结合律），`DeltaChannel` 的文档字符串明确把这条写成了契约的一部分，正是为了让重放能比原始产生时批得更粗。

若不是 `DeltaChannel`（即迁移来的聚合通道），则逐条 `update([value])`。注释写得很直白：这些写入跨越多个超级步，一次性灌进去会撞上「一个超级步只能有一个 `Overwrite`」的检查，第二个 `Overwrite` 就抛错，后面的写入全丢。逐条喂相当于把每个写入还原成各自独立的一步。

`replay_writes` 自己还要处理 `Overwrite`：扫描整段写入，最后一个 `Overwrite` 决定重置点——它之前的一切作废，从它的值（或空容器）重新起步，只有它之后的写入才进 reducer。

**批量与前置校验。** `channels_from_checkpoint` 收集全部待重放通道后，先过 `_require_saver_for_history`：有待重放通道却没有 saver 或 config 时直接抛 `ValueError`，而不是静默返回空通道。这是「宁可不给也不给错的」取向——返回空值会让上层算出一个看似合法但错误的状态。

**被抛在身后的检查点。** `checkpoint_superseded` 先看配置里有没有 `checkpoint_id`；没有就返回假，因为「寻址」是这整个判定的前提。有则把 `checkpoint_id` 抹掉后再查一次 saver，得到线程当前的头，比较两者的 `checkpoint["id"]`。相等即未被超越。

源码注释承认了它的两个已知偏差：一是「有子节点的检查点一定不是最新提交」，所以漏判不存在；二是被抛弃分支的叶子也会被当成「未超越」，要区分必须列举线程找子节点，saver 做不到这个便宜查询。所以这个函数偏向「少判」，把「确实被超越」的识别留给后续的写入落点判断。

## 关键取舍

**源码事实：** 重放依赖 reducer 的结合律，这不是可选优化而是正确性前提。若用户的 reducer 对分批敏感（例如取平均、取最大且不幂等），重建值与原始值会不一致。这条约束由文档声明而非运行期检查，框架无法验证。

**源码事实：** 迁移场景（聚合通道 → delta 通道）走逐条 `update` 而不是批量 `replay_writes`，代价是慢，收益是不触发单步单 `Overwrite` 限制。这是「正确性优先于性能」在代码里留下的可见痕迹。

**源码事实：** 找不到 saver 时抛异常而不是降级。因为降级后得到的通道值会是空容器，而空容器能顺利通过下游所有类型检查，错误会在很远的地方才暴露。

**架构推论：** 筛选与重放分成两个函数、中间夹一次批量 saver 调用，是为了把 N 次祖先遍历压成一次范围查询。`get_delta_channel_history` 的签名因此接受通道列表、返回按通道切分的历史映射。

**架构推论：** `checkpoint_superseded` 的判断被放在「写入要不要落到被寻址检查点」这个决策点上，而不是阻止加载。也就是说，被超越的检查点照样可以加载、可以恢复，差别只在写入落点：落到它身上会污染它其它子分支的读取，所以改用快照。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `_delta_channels_to_replay` | 筛选需要祖先回溯的通道，含迁移场景兼容 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `_replay` | 从种子起折叠历史，按通道类型分两条路 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `checkpoint_superseded` | 比较被寻址检查点与线程当前头 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `channels_from_checkpoint` | 批量取历史并水合全部通道 |
| `libs/langgraph/langgraph/channels/delta.py` | `replay_writes` | 定位最后一个 `Overwrite` 并折叠其余写入 |

## 参考

- [LangGraph 源码：pregel/_checkpoint.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_checkpoint.py)
- [LangGraph 源码：channels/delta.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/channels/delta.py)
- [LangGraph 文档：时间旅行与状态重放](https://langchain-ai.github.io/langgraph/concepts/time-travel/)

## 与主核 / AI 的关系

「从最近的物化点出发、把之后的日志按序应用」是主核崩溃恢复的标准动作，这一页与它同构：物化点对应这里的 `_DeltaSnapshot` 种子，日志对应祖先链上的写入，应用顺序由 saver 的 `writes_sort_key` 保证与在线执行一致。差别在于主核对重放失败零容忍、需要前滚/回滚，这里只要求 reducer 满足结合律。

对 AI 侧而言，最实用的推论是：给状态字段挂 delta 通道时，reducer 必须是确定且可分批的。写一个「取最近 N 条」「取平均值」这类依赖批次边界的实现，在线状态看起来正确，一旦走到祖先重放就会与在线值分叉，且这种分叉只在跨快照点加载时才显现，极难定位。
