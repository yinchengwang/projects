---
project: postgres
feature: tablesync
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/tablesync.c
    symbols: [start_table_sync, copy_table, TableSyncWorkerMain]
---

# 表同步

## 一句话职责

表同步 worker 负责把一张发布端表的既有数据完整复制到订阅端，并把该表从「初始同步」推进到「已就绪」状态，使后续增量变更可以由 apply worker 接管。

## 机制是什么

逻辑复制只传送变更，不传送历史数据。订阅创建或 `refresh` 之后，每张需要同步的表要经历一次全量拷贝，拷贝期间产生的新变更则通过一个临时的 tablesync 复制槽暂存，等拷贝完成后由同一 worker 继续消费并追平到 apply worker 的位置。`pg_subscription_rel` 的 `srsubstate` 记录这个推进过程：`SUBREL_STATE_INIT`（'i'，初始化）、`DATASYNC`（'d'，数据同步中）、`FINISHEDCOPY`（'f'，COPY 阶段完成）、`SYNCWAIT`（'w'，等 apply 放行）、`CATCHUP`（'c'，追平中）、`SYNCDONE`（'s'，追平完成）、`READY`（'r'，就绪），另有 `UNKNOWN`（'\0'）。

本页只负责「单张表的初始同步与状态推进」。不负责 apply worker 如何应用已就绪表的行变更（i05），不负责快照构建（i07），不负责 origin 进度存储机制本身（i09；本页只是调用方）。表同步也不做冲突检测与用户订阅的 DDL 校验。

## 怎么实现的

1. `TableSyncWorkerMain` 是入口：`SetupApplyOrSyncWorker` 完成共享槽位 attach、信号与 libpqwalreceiver 初始化，然后调 `run_tablesync_worker`，最后 `FinishSyncWorker` 退出。`run_tablesync_worker` 先 `start_table_sync` 拿 `origin_startpos` 与 tablesync 槽位名，再按该位置设置流选项并 `start_apply`。
2. `start_table_sync` 用 PG_TRY 包住 `LogicalRepSyncTableStart`：失败时若订阅设了 `disableonerr` 就 `DisableSubscriptionAndExit`，否则 `AbortOutOfAnyTransaction` 后上报订阅错误并重抛。返回的槽位名被复制到长生命周期的 `ApplyContext`。
3. `LogicalRepSyncTableStart` 先读 `pg_subscription_rel` 的当前状态并写入 `MyLogicalRepWorker->relstate/relstate_lsn`。若状态已是 `SYNCDONE`/`READY`/`UNKNOWN`，直接 `FinishSyncWorker` 不再干活；然后按 `pg_%u_sync_%u_<system_identifier>` 生成槽位名并连接发布端，以槽位名（而非订阅名）作为 `application_name`，使同步复制能区分两个会话。
4. 续跑分支按状态处理：`DATASYNC` 说明上次在拷贝阶段失败，槽位可能还残留，先 `ReplicationSlotDropAtPubNode` 删掉再重来；`FINISHEDCOPY` 说明 COPY 已完成但后续崩溃，则跳过拷贝直接复用已存在的 origin，从 `FINISHEDCOPY` 之后继续。
5. 无论哪条分支，都会在拷贝之前先做 `replorigin_advance(..., go backward = true, WAL log = true)` 并 `replorigin_session_setup`，把 origin 定位到开始同步的位置；源码注释明确这样是为了「避免因为推进 origin 或建立 origin 跟踪出错而重做整次拷贝」。
6. `copy_table` 先 `fetch_remote_table_info` 取发布端表结构与行过滤条件、`logicalrep_relmap_update` 登记关系、`logicalrep_rel_open` 取本地映射；然后按情形拼 `COPY table TO STDOUT` 或 `COPY (SELECT ... FROM ...) TO STDOUT`（有行过滤、发布生成列、或分区表且发布端版本不足时需要后者），必要时加 `WITH (FORMAT binary)`，最后用本地 `BeginCopyFrom` + `CopyFrom` 把数据落库。
7. 拷贝结束后 `CommandCounterIncrement` 让数据可见，`UpdateSubscriptionRelState(..., SUBREL_STATE_FINISHEDCOPY, ...)` 落盘并把共享内存状态切到 `SYNCWAIT`、`relstate_lsn` 设为 `origin_startpos`，随后 `wait_for_worker_state_change(SUBREL_STATE_CATCHUP)` 等 apply worker 放行，再返回进入追平阶段。
8. 两个协作函数分别在不同进程里推进状态：`ProcessSyncingTablesForSync` 在 tablesync 侧判断 `CATCHUP` 且 `current_lsn >= relstate_lsn` 时置 `SYNCDONE`、`walrcv_endstreaming` 并删除 tablesync 槽位；`ProcessSyncingTablesForApply` 在 apply 侧判断 `SYNCDONE` 且已追到该 LSN 时，先删 origin 再置 `READY`。

## 关键取舍

- **源码事实：**tablesync 使用独立槽位而非订阅主槽位，槽位名基于 `suboid + relid + system_identifier`。源码注释给出的理由是这批槽位由 tablesync 自己负责清理，若沿用订阅槽位名，一旦订阅槽位改名就无法反推要清理哪个槽。代价是每个同步表都要在发布端占一个槽位，受发布端 `max_replication_slots` 限制。
- **源码事实：**`DATASYNC` 失败重试时选择「删槽重拷」而非「断点续拷」。源码注释直言另一个方案（保留并尝试从中断处继续）需要清理 COPY 中间状态，且网络中断下也未必成功，故不值得。代价是重复传输整表数据。
- **源码事实：**origin 推进与 session 建立放在拷贝之前，即使拷贝失败也已完成。放弃的是「拷贝成功后才建 origin」的整洁顺序，换来不会因 origin 相关错误重跑一次全量拷贝。
- **源码事实：**有行过滤或存活生成列时改用 `COPY (SELECT ...)`，并在普通表上显式 `ONLY` 避免把子表数据重复拷进来。若一律用 `COPY table`，行过滤会无法表达；若不加 `ONLY`，继承子表会被父表与自身各拷一次。
- **源码事实：**`ProcessSyncingTablesForSync` 先把状态写成 `SYNCDONE` 再删槽位、删 origin，且 `ReplicationSlotDropAtPubNode` 传 `missing_ok = false`。源码注释说明若允许静默成功，槽位会残留到订阅被删除为止；顺序反过来则出错时无法回滚已删槽位。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/tablesync.c` | `start_table_sync` | 以错误处理包裹初始同步启动，并在长生命周期上下文中保留槽位名 |
| `src/backend/replication/logical/tablesync.c` | `copy_table` | 构造发布端 COPY 命令并驱动本地 COPY FROM 完成一张表的全量拷贝 |
| `src/backend/replication/logical/tablesync.c` | `TableSyncWorkerMain` | 表同步 worker 入口，串联 attach、同步与退出清理 |

## 与主核 / AI 的关系

暂无直接对应。

## 参考

- [PostgreSQL 文档：逻辑复制的初始同步](https://www.postgresql.org/docs/current/logical-replication-architecture.html) —— 支撑表的初始拷贝与状态推进语义。
- [PostgreSQL 官方源码：tablesync.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/tablesync.c) —— 支撑拷贝实现、状态机与协作更新。
- [PostgreSQL 官方源码：pg_subscription_rel.h](https://github.com/postgres/postgres/blob/master/src/include/catalog/pg_subscription_rel.h) —— 支撑 `SUBREL_STATE_*` 状态常量定义。
