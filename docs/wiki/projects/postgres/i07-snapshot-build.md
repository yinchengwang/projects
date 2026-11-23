---
project: postgres
feature: snapshot-build
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/snapbuild.c
    symbols: [AllocateSnapshotBuilder, SnapBuildProcessRunningXacts, SnapBuildBuildSnapshot]
---

# 逻辑解码快照构建

## 一句话职责

SnapBuild 依据 WAL 中的 running xacts 记录推算出一个可安全开始解码的一致点，并把该点的「哪些事务已提交」编码成解码期使用的历史快照。

## 机制是什么

逻辑解码要在没有真实 MVCC 环境的情况下判断一行在某个历史时刻是否可见。做法是把「在解码起点时刻已经提交、但可能在 WAL 中还没读到的 catalog 事务 xid」收集成集合，再用 `xl_running_xacts` 记录逐层收窄，直到所有在该点仍在运行的事务都已结束，才认为拿到了完整的历史 catalog 快照。这时刻称为 consistent point，之后才能输出变更。

`SnapBuild` 状态从 `SNAPBUILD_START`（-1）经 `BUILDING_SNAPSHOT`（0）、`FULL_SNAPSHOT`（1）到 `SNAPBUILD_CONSISTENT`（2），每一步都要靠新的 running xacts 记录满足条件才推进。本页负责状态推进、committed/catchange xid 集合维护、快照对象构造与序列化/恢复，不负责变更排序与事务装配（i06 ReorderBuffer），也不负责解码上下文与输出插件回调（i04）。它管理的是「解码起点的可见性判定」，不是执行期普通的 `GetTransactionSnapshot`。

## 怎么实现的

1. `AllocateSnapshotBuilder` 在独立 AllocSet 上下文中分配 `SnapBuild`，把 `state` 置为 `SNAPBUILD_START`，并预分配 `committed.xip`（初值 `xcnt_space` = 128，源码注释标为 arbitrary number），把 `committed.includes_all_transactions` 置真，表示「尚未排除任何事务」。同时记下 `initial_xmin_horizon`、`start_decoding_at`、`in_slot_creation`、`building_full_snapshot`、`two_phase_at`。
2. `SnapBuildProcessRunningXacts` 是状态机驱动器。未达 CONSISTENT 时调 `SnapBuildFindSnapshot` 尝试推进；已达 CONSISTENT 则调 `SnapBuildSerialize` 把快照落盘。之后无论如何更新 `builder->xmin = running->oldestRunningXid`、清理不再需要的事务记录，并通过 `LogicalIncreaseXminForSlot` 推进槽位 xmin 以允许 vacuum 回收。
3. 推进路径按源码注释的 a/b/c 分支：若 `oldestRunningXid == nextXid`（无运行事务），直接把 xmin/xmax 设为 `nextXid`、置 CONSISTENT；或从磁盘恢复旧快照（`SnapBuildRestore`）；`SNAPBUILD_START` → `BUILDING_SNAPSHOT` 时记下 `next_phase_at = running->nextXid`，并 `SnapBuildWaitSnapshot` 等待该批事务结束；此后 `oldestRunningXid >= next_phase_at` 才允许再进一级。
4. 每级推进都要等待的原因是源码注释指出的时间差问题：被标记为 running 的事务可能已经写入了 commit 记录，但还没从 procarray 移除；仅凭 `xl_running_xacts` 的 xids 无法确定。因此 `SnapBuildWaitSnapshot` 反复等待直到 `oldestRunningXid` 越过阈值。
5. `SnapBuildBuildSnapshot` 在 `state >= SNAPBUILD_FULL_SNAPSHOT` 后按 `sizeof(SnapshotData) + committed.xcnt + 1` 一次性分配快照，把 `snapshot_type` 设为 `SNAPSHOT_HISTORIC_MVCC`。它复用了 `SnapshotData` 的 `xip`/`subxip` 字段：`xip` 存放「必须当作已提交」的事务 xid 并对数组做 `qsort` 以便 `bsearch`；`subxip` 初始为空，只有在 catalog 修改事务需要区分自己的子事务时才由 `ReorderBufferCopySnap` 填充。
6. `xmax` 只在 catalog 修改事务提交时前推（见 `SnapBuildCommitTxn`），因此可能出现 `xmax < xmin` 这种「看起来矛盾」的状态；源码注释说明这是刻意为之，因为解码只关心 catalog 事务，可以利用 `heapam_visibility.c` 的快路径。

## 关键取舍

- **源码事实：**`SnapshotData` 的 `xip`/`subxip` 语义被改用作「按已提交处理的事务集合」，源码注释直言 "We misuse the original meaning"。放弃为解码单建一套快照结构，换来可直接复用 MVCC 可见性判断代码；代价是语义要靠注释维持，误用会静默错判可见性。
- **源码事实：**状态推进刻意保守——只信 `oldestRunningXid` 是否越过 `next_phase_at`，不使用 `xl_running_xacts` 里的 xids 来加速。源码注释说明这是因为 running 标记与 commit 记录写入之间存在无法用锁消除的窗口。若激进采用 xids 推断，可能把尚未真正结束的事务当作已完成，从而漏掉其 catalog 变更。
- **源码事实：**一致性快照在 CONSISTENT 后每次遇到 running xacts 记录就 `SnapBuildSerialize` 落盘，而非每次提交都写。源码注释给出的理由是会触发逻辑槽位状态文件的 fsync；降低落盘频率换取重启后仍需回退读取部分 WAL。
- **源码事实：**`xmax` 可低于 `xmin`，只在 catalog 事务提交时前推。若为了「看起来整齐」而在每条 running xacts 上同步抬高 xmax，会让快照区间变宽、命中更多非必要判定，源码注释明确认为当前做法更高效。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/snapbuild.c` | `AllocateSnapshotBuilder` | 建立 builder 内存上下文、初始状态与 committed/catchange 集合初值 |
| `src/backend/replication/logical/snapbuild.c` | `SnapBuildProcessRunningXacts` | 依 running xacts 记录推进状态机、更新 xmin、落盘快照并推进槽位 xmin |
| `src/backend/replication/logical/snapbuild.c` | `SnapBuildBuildSnapshot` | 构造 `SNAPSHOT_HISTORIC_MVCC` 快照，填充已提交 xid 数组并排序供二分查找 |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：逻辑解码概念](https://www.postgresql.org/docs/current/logicaldecoding-explanation.html) —— 支撑一致性点与历史快照在解码中的作用。
- [PostgreSQL 官方源码：snapbuild.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/snapbuild.c) —— 支撑状态机推进、快照构造与序列化。
- [PostgreSQL 官方源码：snapbuild.h](https://github.com/postgres/postgres/blob/master/src/include/replication/snapbuild.h) —— 支撑 `SnapBuildState` 各阶段定义。
