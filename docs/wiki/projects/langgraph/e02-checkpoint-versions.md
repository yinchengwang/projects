---
project: langgraph
feature: checkpoint-versions
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_checkpoint.py
    symbols: [_mark_bumps_seen, _bump_keys, versions_seen_without_bumps]
---

# 版本推进与已见标记

## 一句话职责

维护 `versions_seen` 这份「节点看过哪个通道的哪一版」账本，并把「仅为落盘而做的版本 bump」标注出来，使触发判定不会把它误读成「状态变了」。

## 机制是什么

节点要不要在下一超级步被调度，判断依据是版本比较：`_algo` 里的 `_triggers` 拿通道的当前 `channel_versions` 与 `versions_seen[节点名]` 比较，当前版本更大才算有新写。所以 `versions_seen` 不是日志，而是调度器的唯一输入。

问题出在上一页留下的尾巴：快照时为了让 saver 的 `put` 愿意存 blob，会对「本步没写过」的通道强行 bump 一个新版本。这个 bump 是记账动作，不是状态变化。若放任不管，它会让订阅者看到「版本变新了」而重新触发——最典型的症状是 `interrupt_before` 在没有任何新写的情况下再次拦停，以及节点被无意义地重跑。

## 怎么实现的

**正向：`_mark_bumps_seen`。** 入参是旧账本、bump 表（通道 → `(旧版本, 新版本)`）与 `trigger_to_nodes`。bump 表为空时原样返回旧对象，不做拷贝——绝大多数超级步不会快照，这条快路径省掉了整份账本的复制。

对 `旧版本 is None` 的 bump（即该通道从未被写过、被强制快照），函数给 `trigger_to_nodes` 里每个订阅者 `setdefault` 一个空 entry。理由是「没有 entry」等于该节点从没跑过，`_triggers` 会按「通道可用即触发」处理，于是正好会在这次 bump 上启动；给个空 entry 就等于宣布局部结算过。

随后对每个节点自己的 `seen` 逐个比对：若 `seen.get(k)` 恰好等于 bump 的旧版本，说明该节点看到的就是 bump 前的那一版，于是把它的 `k` 直接改成新版本，并在 `SNAPSHOT_BUMPS` 这个哨兵键下记两条：`node<分隔符>channel → 新版本`（bump 后的）和 `node<END分隔符>channel → 旧版本`（节点真正读过的）。已有记录不覆盖，避免连续两次 bump 时把真实读值冲掉。

**键空间：`_bump_keys`。** 两个拼出来的键分别是「节点名 + `NS_SEP` + 通道名」与「节点名 + `NS_END` + 通道名」。用两个不同分隔符而不是一个标记位，是为了在同一次调用里同时表达「advanced」与「read」两种身份；节点名不允许包含这两种分隔符，所以键不会碰撞。

**反向：`versions_seen_without_bumps`。** 没有 `SNAPSHOT_BUMPS` 条目时直接返回原账本。有则重建一份：逐节点逐通道检查，若 `SNAPSHOT_BUMPS` 记的 bump 后版本等于当前值，说明这个值来自 bump，于是替换为记录的「真实读值」；没有真实读值记录就整条丢弃。这样调用方拿到的就是「节点视角的账本」。

**调用面。** `main.py` 的 `_infer_as_node` 是这套还原的典型消费者：它要从账本里推断「谁最后更新了状态」，若把 bump 算进去就会指向一个什么都没写的节点。反过来，`apply_writes` 侧更新账本时直接写真实版本，不需要还原——写路径只认实际写入。

## 关键取舍

**源码事实：** bump 元数据塞进 `versions_seen` 内部的哨兵键，而不是给 `Checkpoint` 加新字段。好处是检查点格式不变、老 saver 仍能读写；代价是 `versions_seen` 不再是纯粹的「节点 → 通道 → 版本」映射，任何遍历者都必须显式跳过 `SNAPSHOT_BUMPS`——两个函数里都有这行 skip，漏写就会把哨兵当成节点名。

**源码事实：** 还原发生在读时而非写时。真实读值不回写检查点，只在需要时按需计算。这保持检查点字节干净，代价是「读 `versions_seen` 必须先过 `versions_seen_without_bumps`」成为一条隐式纪律，新代码很容易违反。

**源码事实：** 全新通道的 bump 走「给订阅者补空 entry」这条路，而不是跳过。因为空 entry 与「从没跑过」在触发判定里是两个不同结果，而这里需要的是后者失效、前者生效。

**架构推论：** 版本号本身是 saver 提供的单调函数（内存实现是「主版本号 32 位补零 + 随机十六进制尾巴」），框架只要求「新版本 > 旧版本」。所以 bump 与真实写用的是同一个版本源，谁先谁后只影响数值大小，不影响比较结果——这也是「先 bump 后写」不会打架的原因。

**架构推论：** 把 bump 与真实写区分开的唯一手段是记录映射，没有更便宜的方案：两者在 `channel_versions` 里长得完全一样。这一层的存在本身说明「版本推进」与「状态变更」在这套设计里被有意解耦了。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `_mark_bumps_seen` | 把被 bump 的通道从账本里推到新版本，并记录真实读值 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `_bump_keys` | 生成 advanced / read 两套哨兵键 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `versions_seen_without_bumps` | 按记录还原节点视角的账本 |
| `libs/langgraph/langgraph/_internal/_constants.py` | `SNAPSHOT_BUMPS` | 哨兵键名常量 |
| `libs/langgraph/langgraph/pregel/_algo.py` | `_triggers` | 版本比较的消费者 |

## 参考

- [LangGraph 源码：pregel/_checkpoint.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_checkpoint.py)
- [LangGraph 源码：pregel/_algo.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_algo.py)
- [LangGraph 文档：低层图 API 与超级步](https://langchain-ai.github.io/langgraph/concepts/low_level/)

## 与主核 / AI 的关系

这里的问题与主核区分「数据真的改了」和「事务号推进了」完全同构：都是把单调递增的版本当作事实来源，又必须让「纯记账的推进」对可见性判断隐形。主核靠事务快照与提交号解决，这里靠一张显式的 bump 映射 + 读时还原，手段更土但不需要版本存储格式配合。

对 AI 侧来说，这一页解释了 interrupt 为什么有时会「莫名」不再触发，以及节点为什么不重复跑：判定依据不是「状态内容是否相同」，而是「版本是否更大」，而版本可能被快照逻辑单独推过。手工构造检查点或篡改 `channel_versions` 会直接破坏这套约定，恢复循环会因此做出完全错误的调度决定。
