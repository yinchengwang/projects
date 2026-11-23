---
project: postgres
feature: replication-slots
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/slot.c
    symbols: [ReplicationSlotCreate, ReplicationSlotSave, ReplicationSlotsComputeRequiredLSN]
  - path: src/backend/replication/slotfuncs.c
    symbols: [pg_replication_slot_advance]
---

# 复制槽

## 一句话职责

复制槽在主库上为一个消费方锚定进度点，使它对应的 WAL 段与旧行版本在消费方确认之前不被回收。

## 机制是什么

复制槽把「谁还没消费完」从 WAL 回收与 vacuum 的隐式假设变成显式的、可持久化的对象。每个槽是共享内存数组 `ReplicationSlotCtl->replication_slots` 里的一项，由两部分组成：跨重启保留的 `ReplicationSlotPersistentData`（名字、所属数据库 `database`、`persistency`、`xmin`/`catalog_xmin`、`restart_lsn`、`confirmed_flush`、`two_phase`/`two_phase_at`、`plugin`、`failover`、`synced`）与只在共享内存里存在的运行态字段（`dirty`/`just_dirtied`、`effective_xmin`、`effective_catalog_xmin`、候选推进位置 `candidate_xmin_lsn`/`candidate_restart_lsn`、以及 `last_saved_confirmed_flush`/`last_saved_restart_lsn`）。持久态落在 `$PGDATA/pg_replslot/<槽名>/state`，由 `StartupReplicationSlots` 在崩溃恢复之前读回共享内存。

槽按 `data.database` 是否有效分成物理槽与逻辑槽（`SlotIsPhysical`/`SlotIsLogical` 宏）：物理槽只记 WAL 位置，逻辑槽还要额外钉住 catalog 的 xmin 地平线，因为解码需要看到一致时刻的 catalog 快照。槽本身不搬运任何字节：它只声明「从这里开始的 WAL 与这些行版本还不能删」，把数据发给客户端属于物理流复制侧，把 WAL 变成事务变更属于逻辑解码侧。

保留策略最终落到两条全局下界上。`ReplicationSlotsComputeRequiredXmin` 汇总所有有效槽中最老的 `effective_xmin` 与 `effective_catalog_xmin`，交给 procarray 作为 replication slot xmin，阻止 vacuum 提前删掉仍可能被解码看到的行版本；`ReplicationSlotsComputeRequiredLSN` 汇总所有有效槽中最老的 `restart_lsn`，交给 xlog 的 `XLogSetReplicationSlotMinimumLSN`，阻止对应 WAL 段被回收。本页不负责同步复制的确认与 quorum 策略（`syncrep.c`）、不负责逻辑解码本身（`logical.c`、`decode.c`）、也不负责事务变更的累积与排序（`reorderbuffer.c`）。

槽的失效是这条保留链的兜底：当 `max_slot_wal_keep_size` 等上限已经越过槽需要的资源时，`InvalidateObsoleteReplicationSlots` 会用 `RS_INVAL_WAL_REMOVED`/`RS_INVAL_HORIZON`/`RS_INVAL_WAL_LEVEL`/`RS_INVAL_IDLE_TIMEOUT` 标记槽失效，失效的槽不再参与下界计算，也不能再被取用。

## 怎么实现的

1. **创建与命名。** `ReplicationSlotCreate` 先用 `ReplicationSlotValidateName` 校验名字：只允许小写字母、数字与下划线，长度必须小于 `NAMEDATALEN`；`pg_conflict_detection` 是保留名，仅逻辑 launcher 可用。带 `failover` 的槽不允许建在备库上，也不允许与 `RS_TEMPORARY` 组合（临时槽不会同步到备库）。
2. **在共享数组里占位。** `ReplicationSlotCreate` 先取 `ReplicationSlotAllocationLock` 独占（防止并发分配同一个槽名，也防止与正在清理的旧槽目录打架），再取 `ReplicationSlotControlLock` 共享扫全数组查重名并挑一个空闲槽。普通槽取 `max_replication_slots` 段，REPACK 专用槽取 `max_repack_replication_slots` 段；源码里这两个 GUC 的默认值分别是 10 与 5。全部被占用时报 `CONFIGURATION_LIMIT_EXCEEDED` 并提示调大对应 GUC。
3. **初始化状态并落盘。** 填充 `data`（`database` 按 `db_specific` 取 `MyDatabaseId` 或 `InvalidOid`，`two_phase_at` 置 `InvalidXLogRecPtr`）并复位运行态字段；随后 `CreateSlotOnDisk` 建 `pg_replslot/<名字>.tmp` 目录、置 `dirty = true` 让 `SaveSlotToPath` 写出 `state`，再把目录 `rename` 成正式名字，并在临界区里 fsync 槽目录与 `pg_replslot` 目录。
4. **置为 in_use 并归属本进程。** 再取 `ReplicationSlotControlLock` 独占，置 `slot->in_use = true`，在 `slot->mutex` 自旋锁下把 `active_proc` 设为 `MyProcNumber`，并把 `MyReplicationSlot` 指向它；逻辑槽还要建 pgstat 统计项。之后释放 `ReplicationSlotAllocationLock`，广播 `slot->active_cv`。
5. **占用与释放。** `ReplicationSlotAcquire` 在 `ReplicationSlotControlLock` 共享下按名字找槽；若已被别的进程持有，`nowait` 为假时用条件变量 `active_cv` 睡眠等待，为真时直接报 `OBJECT_IN_USE`。拿到槽后复查 `invalidated`，已失效的槽不再可用。`ReplicationSlotRelease` 对 `RS_EPHEMERAL` 槽直接 drop，对 `RS_PERSISTENT` 槽只清 `active_proc`、记 `inactive_since` 并广播，槽本身保留。
6. **预留 WAL。** `ReplicationSlotReserveWal` 在 `ReplicationSlotAllocationLock` 独占下写 `data.restart_lsn`：物理槽取 `GetRedoRecPtr()`，备库上的逻辑槽取 `GetXLogReplayRecPtr(NULL)`，主库逻辑槽取 `GetXLogInsertRecPtr()`（随后由 `LogStandbySnapshot` 打出构建快照所需的记录并 `XLogFlush`）。写完立刻重算下界，并检查 `XLogGetLastRemovedSegno()`，发现需要的 WAL 已被并发回收就直接报错。
7. **推进位置。** `pg_replication_slot_advance` 先做权限校验，把目标 LSN 夹到本机已 flush 的位置（恢复中则夹到 replay 位置），再以 `nowait = true` 取槽。若槽的 `restart_lsn` 从未有效，报「槽从未预留过 WAL 或已被失效」。逻辑槽的下限是 `confirmed_flush`（更早的数据已不可用），物理槽的下限是 `restart_lsn`；目标小于下限时直接报错。通过后按槽类型分派：物理槽在 `restart_lsn` 前移时置脏并唤醒持有 failover 逻辑槽的 walsender，逻辑槽交给 `LogicalSlotAdvanceAndCheckSnapState`。函数末尾重算 xmin 与 LSN 下界、释放槽，返回（槽名，实际到达位置）。
8. **持久化。** `ReplicationSlotSave` 把当前持有的槽写到 `pg_replslot/<槽名>`。真正干活的是 `SaveSlotToPath`：只有 `dirty` 为真才动手，先取 `slot->io_in_progress_lock` 独占，写 `state.tmp`（`O_CREAT|O_EXCL|O_WRONLY`），填入 `magic`（`SLOT_MAGIC`）、`version`（`SLOT_VERSION`）、`length`，在 `slot->mutex` 下拷出 `slot->data`，对校验和覆盖区算 CRC32C，然后 `pg_fsync`、`close`、`rename` 成 `state`，最后在临界区里 fsync 文件、槽目录与 `pg_replslot` 目录。成功后清 `dirty`（若期间又被置 `just_dirtied` 则保留），并把 `last_saved_confirmed_flush`、`last_saved_restart_lsn` 记成刚写下去的值。

## 关键取舍

**槽状态放文件系统而不是系统目录。** `slot.c` 顶部 NOTES 直接给出理由：槽必须在备库上也能用（支持级联），这条要求排除了存系统目录的做法。放弃的是 SQL 层可见性与事务性（不能靠 `CREATE TABLE` 之类保证原子创建），换来的是崩溃安全与备库可创建；代价是槽必须在崩溃恢复之前由 `StartupReplicationSlots` 手工从目录读回共享内存，且目录操作要自己处理 fsync 与 rename 的原子性。

**用 `dirty` 位做延迟落盘，而不是每次位置前进都 fsync。** `confirmed_flush` 每次前进都落盘会产生大量小 IO。放弃的是「崩溃后位置绝不回退」，换来吞吐；代价是崩溃后 `confirmed_flush` 可能退回，源码用两处补偿：`CheckPointReplicationSlots` 在关库时若逻辑槽的 `confirmed_flush` 已超过 `last_saved_confirmed_flush` 就强制置脏补写一次，`ReplicationSlotsComputeRequiredLSN` 对持久槽改用 `last_saved_restart_lsn` 作为 WAL 下界。真实退化路径的 caller 就是 `CheckPointReplicationSlots`（`src/backend/replication/slot.c`）。

**持久槽的 WAL 下界取 `min(restart_lsn, last_saved_restart_lsn)`，而不是只取 `restart_lsn`。** `ReplicationSlotsComputeRequiredLSN` 对 `RS_PERSISTENT` 槽额外检查：若 `last_saved_restart_lsn` 有效且小于 `restart_lsn`，就用更早的那个。放弃的是 WAL 保留量（多留一段），换来崩溃后重启时不会发现需要的段已被回收；替代方案（只用 `restart_lsn`）会让持久槽在崩溃后指向一段并不存在的 WAL。非持久槽不做这个修正，因为它们本来就活不过崩溃。

**槽位耗尽时硬失败，而不是自动驱逐最老的槽。** `ReplicationSlotCreate` 在数组全满时报错并提示调大 `max_replication_slots`。放弃的是「自动回收闲置槽」的便利，换来不会在管理员不知情时丢掉某个消费方的进度锚点；替代方案需要额外的引用计数与淘汰策略，而槽没有可靠的使用计数可用。

**推进接口对逻辑槽和物理槽采用不同的下限语义。** `pg_replication_slot_advance` 里逻辑槽用 `confirmed_flush`、物理槽用 `restart_lsn` 作下限。放弃的是「两者统一用同一个 LSN」的简洁，换来符合两者的实际数据可用性边界：物理槽更早的位置会被检查点直接删掉，逻辑槽更早的位置已经被解码消费掉。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/slot.c` | `ReplicationSlotCreate` | 校验槽名与 failover 组合，在共享内存数组中查重名并占位，初始化持久态与运行态字段，通过 `CreateSlotOnDisk` 落盘后置 `in_use`/`active_proc` 并把槽归属当前 backend |
| `src/backend/replication/slot.c` | `ReplicationSlotSave` | 把当前持有的槽序列化到 `pg_replslot/<槽名>`，是崩溃安全的入口；实际写入细节由 `SaveSlotToPath` 完成 |
| `src/backend/replication/slot.c` | `ReplicationSlotsComputeRequiredLSN` | 汇总所有有效槽最老的 `restart_lsn`（持久槽再与 `last_saved_restart_lsn` 取较早者），交给 `XLogSetReplicationSlotMinimumLSN` 作为 WAL 回收下界 |
| `src/backend/replication/slotfuncs.c` | `pg_replication_slot_advance` | SQL 侧的推进入口：夹紧目标 LSN、校验槽已预留 WAL 且目标不低于下限，按物理/逻辑分派实际推进，最后重算 xmin 与 LSN 下界 |

## 参考

- [PostgreSQL 文档：流复制槽](https://www.postgresql.org/docs/current/warm-standby.html#STREAMING-REPLICATION-SLOTS) —— 槽对 WAL 保留与 xmin 地平线的用户可见语义
- [PostgreSQL 文档：复制槽管理函数](https://www.postgresql.org/docs/current/functions-admin.html#FUNCTIONS-REPLICATION-SLOT-FUNCS) —— `pg_replication_slot_advance` 等 SQL 入口的调用契约
- [PostgreSQL 官方源码：slot.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/slot.c) —— 槽的共享内存布局、锁顺序与落盘实现

## 与主核 / AI 的关系

主核 `engine/db` 目前没有复制或消费方锚点概念：它的 `wal_truncate` 只按单一 `checkpoint_lsn` 截断日志，`engine/db` 中的 “slot” 指的是 slotted page 的页内槽位，与复制槽无关。但两者要解决的是同一类问题——「日志/旧版本能删到哪」。主核若将来出现多读者（只读副本、多模态索引副本、外部同步），复制槽的 `restart_lsn` + `confirmed_flush` 双锚点与「按读者取最小下界」的汇总方式，可以直接作为设计参照。
