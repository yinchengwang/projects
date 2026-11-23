---
project: postgres
feature: standby-recovery-conflict
page_type: feature
schema_version: 1
refs:
  - path: src/backend/storage/ipc/standby.c
    symbols: [InitRecoveryTransactionEnvironment, ResolveRecoveryConflictWithSnapshot]
---

# 热备恢复冲突

## 一句话职责

热备冲突处理协调 startup 的 WAL 恢复推进与 standby 查询所持事务快照、对象锁等资源之间的不兼容状态。

## 机制是什么

恢复记录有时要求回收某一快照之前的版本、删除数据库/表空间，或取得与查询冲突的锁；primary 上这些动作可以等待或按事务语义继续，但 standby 上恢复必须追赶 WAL。冲突管理维护恢复进程的事务跟踪环境，并按冲突类型查找阻塞查询，等待到策略期限后通知取消相应 backend、记录冲突等待。

本页仅负责 standby 冲突判定后的等待、取消与日志，不负责 WAL 记录读取、恢复 redo 本身（b03/b04），也不负责 replication slot 的生命周期管理（见 i02）。`ResolveRecoveryConflictWithSnapshot` 根据 WAL 中的 snapshot conflict horizon 找到仍可能看到被清理 XID 的 backend；这一 horizon 由恢复记录调用方基于实际变更提供，不是全库无条件取消所有读事务。

## 怎么实现的

1. `InitRecoveryTransactionEnvironment` 为 startup 建立 RecoveryLockHash 与按 XID 串接锁项的 RecoveryLockXidHash，初始化 shared invalidation，并给 startup 注册虚拟事务 ID。恢复事务可持有锁/发出 invalidation，故需要让锁管理器能识别它。
2. redo 决定冲突类型和范围后，快照冲突入口将 `snapshotConflictHorizon` 与数据库 OID 交给 `GetConflictingVirtualXIDs`，再调用 `ResolveRecoveryConflictWithVirtualXIDs` 逐个等待冲突虚拟事务退出。无效 horizon 表示无需冲突，直接返回。
3. 等待时间由 WAL receipt 时间与来源决定：streaming 与 archive 使用不同配置的最大延迟；负值表示无限等待。等待以逐步增加的睡眠避免忙轮询，若到达限制则通过 recovery-conflict 信号通知冲突 backend 取消，并继续等待其退出。
4. 可选日志在等待超过 `DeadlockTimeout` 后记录冲突类型和 backend PID；后台显示可标注 waiting。数据库删除冲突走另一条路径，因单纯等活跃事务无法处理 idle session，所以持有删除锁后直接反复发信号直至 backend 退出。
5. 冲突解决完成后清除等待显示；关闭恢复事务环境时将已跟踪事务标为结束、释放 standby 跟踪的锁，并销毁 hash 表与虚拟事务入口。

## 关键取舍

- **按 WAL 冲突 horizon 精确找快照，而非见恢复记录就取消查询（源码事实）**：仅可能受删除/清理影响且快照仍覆盖 horizon 的事务进入候选；无效 horizon 不做处理。粗暴取消所有查询会降低 standby 可用性且扩大恢复影响面。
- **允许延迟恢复推进，超过时间再取消读者（源码事实）**：按 archive/stream 的 WAL 接收时点计算等待上限，避免正常查询与恢复争用时立即中断查询；代价是在延迟期内 redo 暂停，standby replay lag 增加。设无限等待则保留查询但恢复可能永不推进。
- **数据库级冲突不用虚拟事务等待队列（源码事实）**：`ResolveRecoveryConflictWithDatabase` 指出 idle session 也会阻塞数据库删除，因此直接强制结束该数据库连接。若只等待活动事务完成，空闲连接可能令恢复永久卡住；过于激进则会立即断开所有相关连接。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/storage/ipc/standby.c` | `InitRecoveryTransactionEnvironment` | 初始化 startup 恢复事务跟踪、锁哈希和虚拟事务身份 |
| `src/backend/storage/ipc/standby.c` | `ResolveRecoveryConflictWithSnapshot` | 按 XID horizon 找出可能阻碍 redo 的 standby 查询并等待/取消 |

## 与主核 / AI 的关系

暂无直接对应。该机制是只读热备节点的恢复与查询协调，不应直接映射为主核常规事务锁等待或 replication slot 管理。

## 参考

- [PostgreSQL 文档：热备冲突处理](https://www.postgresql.org/docs/current/hot-standby.html#HOT-STANDBY-CONFLICT) —— 说明恢复冲突类别、延迟和取消行为。
- [PostgreSQL 官方源码：standby.c](https://github.com/postgres/postgres/blob/master/src/backend/storage/ipc/standby.c) —— 支撑冲突虚拟事务等待、时间限制与日志实现。
- [PostgreSQL 文档：热备参数](https://www.postgresql.org/docs/current/runtime-config-replication.html#RUNTIME-CONFIG-REPLICATION-STANDBY) —— 支撑 archive/streaming 最大 standby delay 参数语义。
