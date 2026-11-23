---
project: postgres
feature: origin-tracking
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/origin.c
    symbols: [replorigin_create, replorigin_session_setup, replorigin_get_progress]
---

# 复制来源追踪

## 一句话职责

复制来源（replication origin）为「变更来自哪个远端节点」和「该远端进度追到哪」提供持久标识，使逻辑复制在崩溃重启后能从正确位置续传，并让输出插件识别并过滤自己产生过的变更以避免环路。

## 机制是什么

origin 由两部分组成：`pg_replication_origin` 里的持久记录（外部名 `roname` 与内部 2 字节 `roident`），以及共享内存 `ReplicationState[]` 数组中的运行时进度（`remote_lsn` 远端提交位置、`local_lsn` 本地对应位置）。文件名与槽位都要在 WAL 与共享内存中使用紧凑的内部 ID，因此需要这个「短 ID + 长名」的拆分；源码注释说明 2 字节来源假设单套复制拓扑中不会超过 65k 个节点。

每个后端最多绑定一个会话级 origin（`session_replication_state`），绑定后可用更廉价的 `replorigin_session_advance` 推进；`replorigin_xact_state` 记录当前事务的来源 origin、远端 LSN 与时间戳，提交记录会把它们写进 WAL，从而让恢复后也能重建精确进度。本页负责 origin 的创建、会话绑定、进度读写与 checkpoint 持久化，不负责逻辑解码的可见性判定（i07）、变更装配（i06）或订阅端消息应用（i05）。循环检测本身（origin 过滤）发生在解码侧调用 `filter_by_origin`，本页只提供被判定的来源信息。

## 怎么实现的

1. `replorigin_create` 必须在事务中调用，名字长度上限 `MAX_RONAME_LEN`（源码为 512 字节）。由于内部 ID 只有 2 字节，不能依赖常规 OID 分配：它对 `pg_replication_origin` 加 `ExclusiveLock`，用 `SnapshotDirty` 允许读到其他未提交会话刚插入的行，然后从 1 开始线性扫描 `roident` 找第一个未被占用的值，插入新行并 `CommandCounterIncrement` 让自身可见。
2. 之所以能无快照访问该 catalog，是因为它被刻意设计成没有 TOAST 表（源码以 `Assert(!OidIsValid(rel->rd_rel->reltoastrelid))` 守住这一点），而 `roname` 靠 512 字节上限避免离行存储。若给它加 TOAST 表，就必须在每处访问前准备快照。
3. `replorigin_session_setup` 在 `ReplicationOriginLock` 排他锁下，在 `replication_states[]` 中找该 `roident` 的槽位或一个空槽；`acquired_by == 0` 表示本进程首次获取，要求槽位空闲；传入非零 `acquired_by`（首个进程 PID）表示多进程共用同一 origin（如 parallel apply），仅做一致性断言。成功后 `refcount++`，并把指针缓存到 `session_replication_state` 以避开后续每次提交都线性查找。
4. 进程退出时 `ReplicationOriginExitCleanup` 调 `replorigin_session_reset_internal`：只有 `acquired_by` 恰好是自己时才清 PID，`refcount--`，然后 `ConditionVariableBroadcast` 唤醒可能在等该 origin 释放的 drop 流程。
5. `replorigin_get_progress` 以 `ReplicationOriginLock` 共享锁遍历数组，找到后在其 per-slot `LWLock` 下读 `remote_lsn`/`local_lsn`；`flush` 为真则对 `local_lsn` 调 `XLogFlush`，保证返回的远端位置对应的本地记录已经落盘——否则崩溃后可能丢失本地未持久化部分却已宣告进度。
6. 进度推进有两条路径：非会话路径 `replorigin_advance` 在两张锁下更新，并按 `wal_log` 决定是否写 `xl_replorigin_set` 记录（`force` 字段即 `go_backward`），WAL 重放时不再写日志；会话路径 `replorigin_session_advance` 只单调取较大值，不做锁与日志。
7. checkpoint 时把 `ReplicationState` 的 `remote_lsn` 写入 `pg_logical/replorigin_checkpoint`（临时文件 + rename，magic `REPLICATION_STATE_MAGIC` 0x1257DADE）；写入前对 `local_lsn` 做 `XLogFlush`，保证 checkpoint 中引用的本地事务确实在盘上。

## 关键取舍

- **源码事实：**内部 ID 只有 2 字节且按线性扫描分配，源码注释自承 "That's not particularly efficient, but this should be a fairly infrequent operation"。放弃通用 OID 分配器的扩展性与速度，换来 WAL 与共享内存中的紧凑表示；若改用 4 字节 ID，每条带 origin 的 WAL 记录与每个槽位都要变大。
- **源码事实：**`replorigin_create` 依赖「`pg_replication_origin` 没有 TOAST 表」这一隐含约束，并用 512 字节名字上限维持它。放弃无长度限制的名字，换来可在无快照下安全读写该 catalog；若允许更长名字或加 TOAST，则必须补齐快照准备，否则会读到不可见的离行数据。
- **源码事实：**`replorigin_get_progress` 的 `flush` 参数决定是否 `XLogFlush(local_lsn)`。调用方若为省 I/O 传 false，可能在崩溃后回报了本地尚未持久化的远端进度，导致重复应用或位置错乱；源码把这条代价显式交给调用方选择。
- **源码事实：**`replorigin_advance` 默认要求调用者持有 `pg_replication_origin` 的 `RowExclusiveLock`（恢复期除外），而 `wal_log` 决定是否产生 WAL。源码注释说明强制设置应记 WAL 以便备库也能获得该信息；若一律不记日志，则强制回退的进度无法传播到备库。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/origin.c` | `replorigin_create` | 在排他锁与 dirty snapshot 下扫描分配 2 字节内部 ID 并写入持久 origin 记录 |
| `src/backend/replication/logical/origin.c` | `replorigin_session_setup` | 为当前后端绑定共享内存 origin 槽位、维护 `acquired_by`/`refcount` 与查找缓存 |
| `src/backend/replication/logical/origin.c` | `replorigin_get_progress` | 在共享锁下读取该 origin 的远端/本地进度，并按需 flush 本地 WAL |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：逻辑复制的冲突与环路处理](https://www.postgresql.org/docs/current/logical-replication-conflicts.html) —— 支撑来源追踪用于避免环路的背景。
- [PostgreSQL 官方源码：origin.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/origin.c) —— 支撑 origin 创建、会话绑定、进度读写与 checkpoint 持久化。
- [PostgreSQL 文档：system administration functions 中的复制来源函数](https://www.postgresql.org/docs/current/functions-admin.html) —— 支撑 origin 进度的可观测接口。
