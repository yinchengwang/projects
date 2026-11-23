---
project: postgres
feature: reorder-buffer
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/reorderbuffer.c
    symbols: [ReorderBufferAllocate, ReorderBufferQueueChange, ReorderBufferCommit]
---

# ReorderBuffer

## 一句话职责

ReorderBuffer 按 WAL 出现顺序收集 WAL 中零散的解码变更，把它们重新装配成完整事务，并在读到该事务的提记录后一次性交给输出插件。

## 机制是什么

逻辑解码从 WAL 中逐条读到的是行级变更、快照、命令 ID 等碎片，而输出插件需要的是「一个已提交事务的完整变更序列」。ReorderBuffer 就是这两者之间的装配层：它按 XID 建立 `ReorderBufferTXN`，把 `ReorderBufferChange` 链到所属事务，再按 LSN 顺序把子事务的变更流合并进顶层事务。事务在提交（或 prepare、abort）之前对外不可见，因此解码结果的事务边界与源库一致。

本页负责变更累积、子事务合并、事务边界处理与内存/落盘策略，不负责构建 MVCC 快照（由 snapbuild.c 提供，见 i07），也不负责把变更翻译成发布端可发给订阅方的 JSON 格式（那是输出插件与 pgoutput 的职责）。ReorderBuffer 也不决定重启点位置，`current_restart_decoding_lsn` 只是随事务记录下来的可供回退的起点提示。

## 怎么实现的

1. `ReorderBufferAllocate` 在独立 AllocSet 上下文中建立缓冲区，并用 SlabContext 存放定长的 `ReorderBufferChange` 与 `ReorderBufferTXN`，用 GenerationContext 存放解码元组；建立 `by_txn` 哈希（键为 XID）、单条查找缓存 `by_txn_last_xid`/`by_txn_last_txn`、按事务大小的 `txn_heap`，以及按顶层事务 LSN 排序的 `toplevel_by_lsn` 链表。分配时还清理该槽位上次残留的磁盘溢写目录，避免同一槽位复用后重复处理旧事务。
2. 解码器每个变更调用 `ReorderBufferQueueChange`：先以 `ReorderBufferTXNByXid` 查或建事务（`create_as_top` 时同时挂入 `toplevel_by_lsn`），若事务已被标记 abort 则直接丢弃该变更（不计内存），否则写入 `lsn`、`txn` 并 `dlist_push_tail` 到 `txn->changes`，同时累加 `nentries`/`nentries_mem`。
3. 累积之后立刻做两件事：`ReorderBufferProcessPartialChange` 标记流式边界（toast 与 speculative 变更未完成时不得流式），`ReorderBufferCheckMemoryLimit` 在 `rb->size` 超过 `logical_decoding_work_mem` 时按事务大小从 `txn_heap` 挑最大者，能流式就流式输出，否则溢写磁盘。
4. 读到 commit 记录时 `ReorderBufferCommit` 按 XID 找到事务（找不到说明该事务没有产生变更，直接返回），再走 `ReorderBufferReplay`：填入 `final_lsn`/`end_lsn`/`commit_time`/origin 信息；已流式的事务转为 `stream_commit`，没有 `base_snapshot` 的事务说明未改数据库，仅清理结构。
5. 普通路径由 `ReorderBufferProcessTXN` 在内部子事务或独立事务中遍历变更：构建 `tuplecid_hash` 以便按 catalog 元组定位命令 ID，`SetupHistoricSnapshot` 安装历史快照；然后按 `ReorderBufferIterTXNNext` 从各子事务变更流（或磁盘分块）中按最小 LSN 依次取出，断言 LSN 单调不减（`MULTI_INSERT` 允许同 LSN），逐条调输出插件的变更回调。
6. 结束阶段按事务类型与调用点发 `commit`/`prepare`/`abort` 回调，随后 `ReorderBufferCleanupTXN` 释放变更与磁盘溢写文件；变更处理的 PG_TRY 失败路径重置资源所有者与历史快照，保证错误后不留下已部分输出的悬浮状态。

## 关键取舍

- **源码事实：**磁盘溢写只针对解码后的变更，事务结构体本身不参与内存上限统计（源码注释明确说明事务记录很小）。这样能保住「事务索引仍在内存」，代价是子事务极多的单个事务仍可能占用可观内存。
- **源码事实：**淘汰策略只按事务大小选最大者，源码注释直言「current eviction algorithm is very simple - the transaction is picked merely by size」，并承认考虑 LSN 年龄可能更利于 Generational 内存真正释放。放弃的是更精细的淘汰收益，换来无需在热路径维护额外年龄索引。
- **源码事实：**事务在被 abort 标记后，后续变更直接释放不再收集（`ReorderBufferQueueChange` 的早退分支）。这依赖上游能及时判定 abort；若判定滞后，已收集的变更要等到 abort 记录才被整体清理。
- **源码事实：**流式输出必须等「完整变更」——toast 插入与 speculative 插入在确认前被标为不完整（`RBTXN_HAS_PARTIAL_CHANGE` 一类标志），此时即使超过内存阈值也不流式，宁可等待。放弃的是低延迟，换来下游不会收到半条逻辑行。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/reorderbuffer.c` | `ReorderBufferAllocate` | 建立内存上下文、XID 哈希、大小堆与 LSN 有序顶层事务表，并清理槽位残留溢写 |
| `src/backend/replication/logical/reorderbuffer.c` | `ReorderBufferQueueChange` | 把单条解码变更按 XID 归入事务链表，推进内存记账并触发流式/溢写检查 |
| `src/backend/replication/logical/reorderbuffer.c` | `ReorderBufferCommit` | 在 commit 记录到达时定位事务并交由重放路径输出完整事务 |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：逻辑解码](https://www.postgresql.org/docs/current/logicaldecoding.html) —— 支撑逻辑解码按事务边界向输出插件交付变更的模型。
- [PostgreSQL 官方源码：reorderbuffer.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/reorderbuffer.c) —— 支撑变更累积、子事务合并、内存阈值溢写与提交输出流程。
- [PostgreSQL 官方源码：reorderbuffer.h](https://github.com/postgres/postgres/blob/master/src/include/replication/reorderbuffer.h) —— 支撑 `ReorderBufferTXN`/`ReorderBufferChange` 结构与变更类型定义。
