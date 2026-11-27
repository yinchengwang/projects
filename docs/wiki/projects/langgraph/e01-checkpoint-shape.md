---
project: langgraph
feature: checkpoint-shape
page_type: feature
schema_version: 1
refs:
  - path: libs/langgraph/langgraph/pregel/_checkpoint.py
    symbols: [empty_checkpoint, create_checkpoint]
---

# 检查点结构与快照创建

## 一句话职责

`empty_checkpoint` 造一份空检查点骨架，`create_checkpoint` 在一次超级步结束时把「上一份检查点 + 当前活通道状态」拍成一份新的、可序列化的快照。

## 机制是什么

检查点是一个 `Checkpoint` TypedDict，六个字段各管一件事：`v` 是格式版本（本地常量为 `LATEST_VERSION`，值为 4）、`id` 与 `ts` 是身份与时间、`channel_values` 是「通道名 → 存盘值」、`channel_versions` 是「通道名 → 单调递增版本」、`versions_seen` 是「节点 → 通道 → 该节点看过的版本」、`updated_channels` 是本次被真正更新的通道名（可为 None，且 None 与空列表语义不同）。

两个构造函数分工明确：`empty_checkpoint` 只服务「这个线程还没有任何检查点」的场景，三份映射全空，`id` 用 `uuid6(clock_seq=-2)` 生成，取负的时钟序列是为了让空检查点稳定排在所有真实检查点之前；`create_checkpoint` 才是每步在用的那个，它不合并写入（那是 `apply_writes` 的职责），只把已经算好的通道状态翻译成快照。

## 怎么实现的

**两套入口。** 当 `channels` 传 `None` 时，函数完全不触碰通道：`channel_values` 与 `channel_versions` 直接从旧检查点透传，只重算 `ts`、`id` 与 `versions_seen`。这条路径用于「结构要出新检查点、但状态没有新值」的场合，例如只推进 metadata 或格式迁移。传了通道映射时，`values` 从空 dict 开始，`channel_versions` 浅拷贝旧值，再逐通道决定写什么。

**逐通道三分支。** 对每个通道名 `k`：若 `k` 还不在 `channel_versions` 里，说明它在本分支上从未被写过；此时只有「它被强制纳入本次快照集合」才动作——用 `get_next_version(None, None)` 造一个首版本，把 `(None, 新版本)` 记进 bump 表，往 `values` 塞一个 `_DeltaSnapshot(ch.get())`，然后 `continue`。之所以必须 bump，是因为 saver 的 `put` 只为「版本动过」的通道存 blob，不动版本就落不了盘，祖先回溯也就停不下来。

若 `k` 在 `channels_to_snapshot` 里，就写 `_DeltaSnapshot(ch.get())`；同时检查「本次步没写它、版本与 saver 上次存的一致」这一条件（靠 `stored_versions` 与 `get_next_version`），成立则也 bump 一次。这是 exit 模式与分叉首个检查点的需要：命中快照节奏，但本步恰好没写过该通道时，没有版本推进就没有 blob。

其余通道走常规路径：`ch.checkpoint()`，返回 `MISSING` 的通道不落 `values`。这条分支正好解释了 `DeltaChannel` 的 `checkpoint()` 为什么恒返回 `MISSING`——快照决策不在通道内部，因为通道拿不到版本号和计数器，只有 `create_checkpoint` 有。

**身份与版本。** `id` 可由调用方指定（exit 模式复用最后一份 id），否则用 `uuid6(clock_seq=step)`。`uuid6` 内部维护全局时间戳单调，所以同一 step 反复构造也不会产出乱序 id。`updated_channels` 写盘前排序，保证同一状态产出同一字节。`versions_seen` 交给 `_mark_bumps_seen` 收尾，把为落盘而做的 bump 从「节点看过」的语义里摘出去。

## 关键取舍

**源码事实：** `Checkpoint` 是 TypedDict 而不是 dataclass。好处是 saver 可以按普通 dict 序列化、`_migrate_checkpoint` 可以就地修补字段；代价是没有字段级校验，`v` 与实际字段形状是否匹配全靠迁移函数手动兜底。

**源码事实：** 增量通道的快照是「哨兵 + 祖先重放」而非「全量值」。`channel_values` 里对未到快照点的 delta 通道什么都不写，读时靠 saver 回溯；换来的是每步写入量恒定，代价是加载路径必须有回溯能力。

**源码事实：** 版本 bump 是「为了能落盘」而产生的副作用，会污染 `versions_seen` 的真实语义。因此快照创建必须与 `_mark_bumps_seen` 配对使用，单独调用 `create_checkpoint` 会得到一份「看起来状态变了」的检查点。

**架构推论：** 整个函数只做读与构造，从不修改传入的旧检查点或通道对象。这是异步保存的前提：执行循环可以把构造好的检查点 `copy_checkpoint` 后丢给后台线程提交，而前台继续用旧对象推进。

**架构推论：** 快照内容完全由调用方决定的 `channels_to_snapshot` 与 `get_next_version` 驱动，函数自身没有「该不该快照」的判断。把策略留在执行循环与 `update_state` 两条调用路径里，是这个文件能同时服务二者而不引入分支的原因。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `empty_checkpoint` | 生成空检查点骨架，`id` 取负时钟序列 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `create_checkpoint` | 逐通道生成 `channel_values`、推进 `channel_versions`、收尾 `versions_seen` |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `LATEST_VERSION` | 当前检查点格式版本常量 |
| `libs/langgraph/langgraph/pregel/_checkpoint.py` | `copy_checkpoint` | 深拷贝需变动的映射，供后台保存 |
| `libs/checkpoint/langgraph/checkpoint/base/__init__.py` | `Checkpoint` | 检查点字段契约 |

## 参考

- [LangGraph 源码：pregel/_checkpoint.py](https://github.com/langchain-ai/langgraph/blob/main/libs/langgraph/langgraph/pregel/_checkpoint.py)
- [LangGraph 源码：checkpoint/base](https://github.com/langchain-ai/langgraph/tree/main/libs/checkpoint/langgraph/checkpoint/base)
- [LangGraph 文档：持久化与检查点](https://langchain-ai.github.io/langgraph/concepts/persistence/)

## 与主核 / AI 的关系

这一页的取舍与主核把内存页刷成落盘页是同一个问题：写放大与加载成本不能同时最优。这里选择「常态只记增量、按节奏打全量」，与预写日志搭配周期性检查点（checkpoint）的结构同构；差别在于主核的检查点要处理脏页位图与恢复一致性，这里处理的是「哪个通道的哪一版」这套更粗的粒度。

对 AI 侧而言，理解 `channel_values` 只存「到点该存的值」很关键：调试时看到某个通道在检查点里消失，并不意味着值被清空，而是它还没到快照节奏、要靠祖先写重放。误判这一点会让人以为状态丢失，从而去改 `snapshot_frequency`，而真正该看的是回溯路径。
