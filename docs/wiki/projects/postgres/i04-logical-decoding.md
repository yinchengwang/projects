---
project: postgres
feature: logical-decoding
page_type: feature
schema_version: 1
refs:
  - path: src/backend/replication/logical/logical.c
    symbols: [CreateDecodingContext, CreateInitDecodingContext, StartupDecodingContext]
  - path: src/backend/replication/logical/decode.c
    symbols: [LogicalDecodingProcessRecord]
---

# 逻辑解码

## 一句话职责

逻辑解码把 WAL 记录翻译成带事务边界的数据变更流，经输出插件回调交给消费者，同时维护一份与变更流相匹配的 catalog 快照。

## 机制是什么

`logical.c` 的 NOTES 说明了这个模块的定位：它不实现解码本身，而是协调各模块的交互，主要手段是提供 `LogicalDecodingContext`——目的是把内部复杂度封装起来，让消费者用很少的代码就能创建并消费一条变更流。消费者只需提供三个回调：一个读 WAL、一个准备写数据、一个真正写出（`prepare_write` / `write` / `update_progress`）。内置消费者是 walsender 与 SQL 的 SRF 接口，但源码明确指出也可以在不改核心代码的前提下增加别的消费者，例如在 bgworker 里消费变更。

上下文本身是 `LogicalDecodingContext`（`src/include/replication/logical.h`）：持有专属内存上下文 `context`、关联的复制槽 `slot`、WAL 读取器 `reader`、变更累积器 `reorder`、快照构建器 `snapshot_builder`，加上输出插件回调表 `callbacks`、插件选项 `options`、输出缓冲 `out`、以及 `streaming`/`twophase` 两个能力标志与 `accept_writes`、`write_location` 等输出状态。

`decode.c` 承担另一半职责：它把 xlogreader 读出的低层记录转成可用的形式，分发给 `reorderbuffer`（事务变更本体）与 `snapbuild`（catalog 快照）。该文件头部注释说得很清楚——这里「处理所有低层 xlog 细节」，除了把记录转成更可用的格式之外，不该包含太多关于记录内容本身的智能。

本页只讲**解码的编排层**：上下文如何建立、插件如何被加载与准入、WAL 记录如何被分发到各 rmgr 的解码入口。它不负责变更的累积、事务边界维护与顺序输出（`reorderbuffer.c`），不负责快照的构建算法与 running xacts 追踪（`snapbuild.c`），不负责复制槽对 WAL 的保留（`slot.c`），也不负责在订阅端把变更应用到目标表（`worker.c`）。

## 怎么实现的

1. **前置条件与插件准入。** `CheckLogicalDecodingRequirements` 校验槽要求与 `wal_level >= WAL_LEVEL_REPLICA`、必须连着一个具体数据库（`MyDatabaseId` 有效），并要求备库上解码时主库的 `effective_wal_level` 至少是 logical。插件加载在 `StartupDecodingContext` 里进行，但在 `LoadOutputPlugin` 之前先做准入：`pgrepack` 是特例，只允许 REPACK CONCURRENTLY 场景使用；其它插件名必须在 `output_plugin_libraries` 这个 GUC 的白名单里，否则以 `INSUFFICIENT_PRIVILEGE` 拒绝，错误详情里还会带上当前 GUC 值。fast_forward 模式完全不加载插件。
2. **搭骨架。** `StartupDecodingContext` 新建一个名为 `Logical decoding context` 的内存上下文，在其中 `palloc0_object(LogicalDecodingContext)`。若当前不在事务或事务块里（`!IsTransactionOrTransactionBlock()`），它取 `ProcArrayLock` 独占并置上 `PROC_IN_LOGICAL_DECODING`，源码注释解释：这样 xmin 计算就不必单独检查这个解码后端，因为它的 xmin 由复制槽保证；但在事务内不能这么做，否则已建立的快照/xid 会被忽略。
3. **装配解码基础设施。** `ctx->reader = XLogReaderAllocate(wal_segment_size, NULL, xl_routine, ctx)`；`ctx->reorder = ReorderBufferAllocate()`；`ctx->snapshot_builder = AllocateSnapshotBuilder(ctx->reorder, xmin_horizon, start_lsn, need_full_snapshot, in_create, slot->data.two_phase_at)`；并把 `ctx->reorder->private_data = ctx` 让变更累积器能反查上下文。
4. **用 wrapper 替换插件回调。** `ctx->reorder->begin`、`apply_change`、`apply_truncate`、`commit`、`message` 以及全部 streaming、two-phase 回调，都被替换成 `*_cb_wrapper`。wrapper 的作用是补上错误上下文（`output_plugin_error_callback`）并统一处理插件缺回调的情况。`ctx->streaming` 与 `ctx->twophase` 的判定方式是「插件是否提供了其中任意一个回调」，源码注释说这里只做决定、真正的检查留到 wrapper 里；缺失的可选回调（如 `stream_message`、`stream_truncate`）会被安一个空实现而不是报错。
5. **新槽路径：`CreateInitDecodingContext`。** 它要求已持有槽、槽是逻辑槽、槽属于当前数据库、且不能在一个已经写过数据的事务里创建；把插件名写进 `slot->data.plugin`；若调用者给的 `restart_lsn` 无效就调 `ReplicationSlotReserveWal()` 预留 WAL，否则直接写进槽。随后按固定顺序取 `ReplicationSlotControlLock` 独占、`ProcArrayLock` 独占，用 `GetOldestSafeDecodingTransactionId(!need_full_snapshot)` 算出安全的 xmin 地平线，写入 `slot->effective_catalog_xmin` 与 `slot->data.catalog_xmin`；需要全量快照时连 `slot->effective_xmin` 一起钉住（源码注释解释这是临时的，导出初始快照之后就不需要了）。再 `ReplicationSlotsComputeRequiredXmin(true)`、放锁、`ReplicationSlotMarkDirty()` + `ReplicationSlotSave()` 落盘，最后建上下文并以 `is_init = true` 调用插件的 `startup_cb`。两阶段能力最终被 `ctx->twophase &= slot->data.two_phase` 收窄。
6. **已有槽路径：`CreateDecodingContext`。** 同样要求逻辑槽；非 fast_forward 时要求槽属于当前库；恢复期间若槽是从主库同步来的（`slot->data.synced`）则拒绝使用（源码注释说这类槽是留给 failover 之后的），除非正在做槽同步。`start_lsn` 无效就从 `slot->data.confirmed_flush` 继续；若它小于 `confirmed_flush`，源码选择记一条 LOG 并把起点前移到 `confirmed_flush`，而不是报错——注释解释说客户端经常确认一个自己并不持久保存的 LSN，重启后可能发回更旧的确认值。随后建上下文并调用 `startup_cb`；若两阶段能力是这次才启用的，还要回写 `slot->data.two_phase = true` 与 `two_phase_at = start_lsn` 并落盘，同时 `SnapBuildSetTwoPhaseAt` 通知快照构建器。
7. **逐条记录分发：`LogicalDecodingProcessRecord`。** 它先把记录包装成 `XLogRecordBuffer`（`origptr`、`endptr`、`record`），取 `XLogRecGetTopXid`；若顶层 xid 有效，先 `ReorderBufferAssignChild` 把子事务归到顶层事务下——源码注释强调这一步对所有记录都要做，所以放在按 rmgr 分发之前。之后按 rmid 取 `RmgrData`：若该 rmgr 实现了 `rm_decode` 就调用它（`heap_decode`、`heap2_decode`、`xact_decode`、`standby_decode` 等），否则只调 `ReorderBufferProcessXid` 处理 xid 就结束。函数头注释特别点出：每条记录的 xid 都必须经过 reorderbuffer 处理，即使记录本身与逻辑解码无关；而默认不对每个 record type 都调 `ReorderBufferProcessXid`，是因为在没有先前状态时空事务可以更高效地处理。
8. **各 rmgr 的处理要点。** `heap_decode`/`heap2_decode` 在 `SnapBuildCurrentState(builder) < SNAPBUILD_FULL_SNAPSHOT` 时直接返回——快照没建好之前不解码数据变更；但 fast_forward 模式仍要通过 `SnapBuildProcessChange` 建基础快照，因为需要快照的 xmin 来决定复制槽的候选 catalog_xmin。INSERT/UPDATE/DELETE/TRUNCATE/CONFIRM 分别交给 `DecodeInsert`、`DecodeUpdate`、`DecodeDelete`、`DecodeTruncate`、`DecodeSpecConfirm`；HOT update 按普通 update 处理（注释说本地做 HOT 的事实没有有用信息，且 WAL 布局兼容）；`XLOG_HEAP_INPLACE` 被忽略，因为原地更新只发生在 catalog 元组上、且按定义不改变可见性。`xact_decode` 同样在快照未达 `SNAPBUILD_FULL_SNAPSHOT` 时 bail out；COMMIT 走 `DecodeCommit`：解析 commit 记录、用 `DecodeTXNNeedSkip` 判断是否要跳过这个事务（跳过时用 `ReorderBufferForget` 丢弃内容而不是 `ReorderBufferAbort`，因为还需要执行该事务的 invalidation）、提交子事务、最后 `ReorderBufferCommit` 或 `ReorderBufferFinishPrepared`。`standby_decode` 处理 `XLOG_RUNNING_XACTS`：调 `SnapBuildProcessRunningXacts` 更新解码器对运行中事务的认识并判断是否达到一致状态，再用 `ReorderBufferAbortOld` 掐掉比 `oldestRunningXid` 更老的事务。

## 关键取舍

**用上下文对象封装内部复杂度，让消费者只提供三个回调。** `logical.c` 的 NOTES 把这条写成了设计目标，并明确说新增消费者（例如 bgworker）不必改核心代码。放弃的是让消费者直接操纵 reorderbuffer 与 snapbuild 的自由度，换来插件与消费者两侧的接口稳定。

**插件必须先过白名单才允许加载。** `StartupDecodingContext` 在 `LoadOutputPlugin` 之前检查 `output_plugin_libraries`，注释称之为「blessed for logical decoding」，拒绝时给的是 `INSUFFICIENT_PRIVILEGE` 而不是普通错误。放弃的是「任何插件都能用」的便利，换来 REPLICATION 角色的用户不能借解码之名加载任意动态库；替代方案（放开限制）会把插件目录变成代码执行面。

**请求的起点早于已确认位置时前移而不是报错。** `CreateDecodingContext` 里 `start_lsn < confirmed_flush` 会记 LOG 并前移，注释给出的理由是客户端常确认一个并不持久保存的 LSN，重启后可能发回旧值，源码同时提醒客户端可以自行核对 `confirmed_flush_lsn`。放弃的是「严格按请求起点解码」的语义，换来对不严格客户端的容错；代价是客户端无法通过错误发现自己的起点被改写。

**快照未建好之前不解码数据变更，但 fast-forward 模式仍要建基础快照。** `heap_decode`/`heap2_decode` 用 `SNAPBUILD_FULL_SNAPSHOT` 作门槛，而 fast_forward 分支的注释说明此时仍必须调 `SnapBuildProcessChange`，因为要用快照的 xmin 决定复制槽的候选 catalog_xmin。放弃的是「边建快照边输出变更」，换来变更所引用的 catalog 元组一定可见；代价是解码起点之前的数据变更被整体丢弃。

**缺回调的判定按「有没有提供其中一个」来推断能力开关。** `ctx->streaming` 与 `ctx->twophase` 都是看插件是否实现了对应的一组回调中的任意一个，源码注释说决定在这里做、检查在 wrapper 里做；可选回调缺失时装一个空实现而不是报错。放弃的是「每个能力都要插件显式声明」的严格性，换来插件作者只实现自己支持的那部分即可。

**每条 WAL 记录的 xid 都要过 reorderbuffer，即使记录本身无关。** `LogicalDecodingProcessRecord` 的头注释专门解释了这一点，并说明默认不按 record type 分别调用 `ReorderBufferProcessXid` 是为了空事务在没有先前状态时能更高效地处理。放弃的是「按记录类型精细裁剪」的少量性能空间，换来 xid 记账不会漏。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/replication/logical/logical.c` | `StartupDecodingContext` | 两种解码路径共用的内部实现：新建内存上下文、做插件白名单准入、置 `PROC_IN_LOGICAL_DECODING`、分配 WAL 读取器与 reorder buffer 与快照构建器、用 wrapper 装配插件回调 |
| `src/backend/replication/logical/logical.c` | `CreateInitDecodingContext` | 为新建逻辑槽建立解码上下文：校验槽类型与所属库、登记插件名、预留或写入 `restart_lsn`、在双重锁下算出并钉住 xmin 地平线、落盘后调用插件 `startup_cb` |
| `src/backend/replication/logical/logical.c` | `CreateDecodingContext` | 为已有逻辑槽建立解码上下文：校验槽可用性、决定起点 LSN（含对过旧确认值的前移处理）、按需现场开启两阶段解码并回写槽状态 |
| `src/backend/replication/logical/decode.c` | `LogicalDecodingProcessRecord` | 单条 WAL 记录的解码入口：包装记录、归置子事务到顶层事务、按 rmgr 分发到各自的 `rm_decode`，无解码器的 rmgr 则只做 xid 记账 |

## 参考

- [PostgreSQL 文档：逻辑解码](https://www.postgresql.org/docs/current/logicaldecoding.html) —— 解码槽、变更流与快照的整体语义
- [PostgreSQL 文档：逻辑解码输出插件](https://www.postgresql.org/docs/current/logicaldecoding-output-plugin.html) —— `startup_cb`、`begin_cb`、`change_cb`、`commit_cb` 等回调契约
- [PostgreSQL 官方源码：logical.c](https://github.com/postgres/postgres/blob/master/src/backend/replication/logical/logical.c) —— 解码上下文的装配与插件准入实现

## 与主核 / AI 的关系

主核 `engine/db` 没有逻辑解码层，它的 WAL 只有重放语义、没有面向订阅者的变更流。不过主核的多模态设计里存在结构同源的问题：把变更流从一个节点送到另一个节点，需要同样的三件套——「变更的顺序与事务边界」（主核的 WAL 记录有 LSN 与事务边界，但没有跨节点累积器）、「变更所依赖的元数据快照」（主核有 catalog 层，但没有版本化快照）、「消费者的进度锚点与重放起点」（主核只有单机 `checkpoint_lsn`）。`LogicalDecodingContext` 把这三者显式挂在一个对象上的做法，可以作为主核将来做跨节点同步时的结构参照。
