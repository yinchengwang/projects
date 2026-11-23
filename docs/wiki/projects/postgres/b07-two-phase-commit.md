---
project: postgres
feature: two-phase-commit
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/twophase.c
    symbols: [MarkAsPreparing, EndPrepare, FinishPreparedTransaction]
---

# 两阶段提交

## 一句话职责

两阶段提交把仍需外部协调的事务从普通 backend 生命周期中分离出来，持久保存其待完成动作，供后续 COMMIT PREPARED 或 ROLLBACK PREPARED 安全收尾。

## 机制是什么

普通事务在一次提交中完成 WAL、状态更新与资源释放；两阶段事务先 PREPARE，把 GID、事务 ID、子事务及 commit/abort 所需资源操作记录下来，并保留其锁与“仍在运行”语义；之后由新的会话完成提交或回滚。共享 `GlobalTransactionData`（GXACT）记录 owner、GID、full xid、prepare WAL LSN、dummy PGPROC、是否有效/在盘等状态，受 `TwoPhaseStateLock` 保护。

PREPARE 状态可来自 WAL，也可能在 checkpoint 时迁移到 `pg_twophase` 状态文件；启动恢复能够重建 prepared transaction。该页聚焦 PREPARE 状态保存及完成流程，不重讲一般事务的 XID 分配/提交状态存储（b09）、SLRU 存储细节（b08）或 WAL 通用插入（b01）。

## 怎么实现的

1. `MarkAsPreparing` 检查 GID 长度、功能开关和唯一性，在 `TwoPhaseStateLock` 下从 free list 取得 GXACT，建立 dummy PGPROC 并记录事务/所有者/GID；此时条目尚未 valid，失败退出钩子可回收它。
2. `StartPrepare` 开始构造状态数据：`TwoPhaseFileHeader` 保存 xid、database、prepared_at、owner、GID 长度和子事务、待删文件、统计与 invalidation 数量；可变记录按 MAXALIGN 分块并附加结束 sentinel。子事务摘要也装入 dummy PGPROC 的有限 subxid cache。
3. `EndPrepare` 填好总长度和 replication origin 元数据，将准备数据作为 `XLOG_XACT_PREPARE` 写入 WAL 并 flush；同时保存 prepare start/end LSN。然后先把 GXACT 标 valid 并加入 ProcArray，再清除 checkpoint 延迟标志，使其他事务不会看到 XID 短暂消失；要求同步复制时还要等待 prepare LSN。
4. `FinishPreparedTransaction` 通过 GID 查找并锁定 GXACT，防止同一 prepared transaction 被并发完成；状态若仍在 WAL 则从 `prepare_start_lsn` 读出，否则读取并校验 `pg_twophase` 文件。解析头部后先写 commit/abort WAL，再更新普通事务状态，随后从 ProcArray 移除 dummy PGPROC，执行文件删除、invalidation 与相应 two-phase callbacks，最后清理共享状态和文件。
5. 文件读取校验 magic、长度边界、对齐及 CRC；失败时不把损坏状态当作有效 prepared transaction。WAL 可重建状态文件，故磁盘文件存在与否不等于 prepared transaction 生命周期是否有效。

## 关键取舍

- **PREPARE 数据先由 WAL 持久记录（源码事实）**：`EndPrepare` 在将 GXACT 标有效前插入并 flush prepare WAL，崩溃后能够据此恢复；代价是 WAL record 可能较大且需持有用于恢复的状态。直接只写状态文件会使崩溃恢复边界与日志顺序难以协调。
- **GXACT 在 TwoPhaseStateLock 下以 valid 状态转换（源码事实）**：准备中项可预留 GID 但不让查找者当作完整 prepared transaction；完成入口将 locking backend 标记防止并发 COMMIT/ROLLBACK。若先公开未完成状态，其他会话可能尝试消费不完整文件/记录。
- **状态文件延后于 WAL 使用（源码事实）**：完成时优先由 prepare start LSN 读 WAL 数据，若 checkpoint 已迁移则从磁盘状态文件读；用 WAL 恢复状态可以避免每次 PREPARE 都额外依赖立即创建独立文件，但长时间保留的 prepared transaction 需要 checkpoint 迁移 WAL 状态以免阻止 WAL 回收。
- **完成顺序先写终态 WAL、更新事务状态、再移出 ProcArray（源码事实）**：`FinishPreparedTransaction` 按这个顺序提交/回滚，再执行清理回调；若先移除 ProcArray，旁观者可能误判仍未完成的事务已不在运行。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/twophase.c` | `MarkAsPreparing` | 在共享两阶段表中预留 GID 和 GXACT，建立 preparing 状态 |
| `src/backend/access/transam/twophase.c` | `EndPrepare` | 形成 PREPARE WAL、确保其持久化并公开有效 prepared transaction |
| `src/backend/access/transam/twophase.c` | `FinishPreparedTransaction` | 读取准备数据并完成终态记录、ProcArray 移除及资源回调 |

## 与主核 / AI 的关系

暂无直接对应。外部协调事务的“准备/最终决定”是常见协议概念，但不能据此声称主核已有相同 prepared transaction 状态机或恢复文件格式。

## 参考

- [PostgreSQL 文档：两阶段事务](https://www.postgresql.org/docs/current/two-phase.html) —— 支撑 PREPARE TRANSACTION 与后续提交/回滚语义。
- [PostgreSQL 官方源码：twophase.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/twophase.c) —— 支撑 GXACT 生命周期、WAL 持久化与完成顺序。
- [PostgreSQL 文档：资源消耗参数](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-MAX-PREPARED-TRANSACTIONS) —— 支撑 prepared transaction 数量配置约束。
