---
project: postgres
feature: checkpoint-and-wal-flush
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xlog.c
    symbols: [CreateCheckPoint, CheckPointGuts, XLogFlush]
---

# Checkpoint 与持久化屏障

## 一句话职责

Checkpoint 将一组已确定 REDO 起点的事务与数据页状态推进到可安全重启的磁盘边界，并持久保存该边界。

## 机制是什么

Checkpoint 并非单纯的 WAL flush：它在检查点记录中保存恢复所需的事务分配、timeline、MultiXact、校验状态等摘要，并使共享内存中的脏数据页和 SLRU 页写出、同步后，再发布完成记录和控制文件指针。崩溃恢复从该 REDO 位置向前重放，避免从 WAL 起始处扫描。

本页只负责 checkpoint 编排及持久化屏障，不重复讲 record 的构造与插入细节（见 b01），也不负责启动 redo 的记录读取和执行（见 b03）。在线 checkpoint 期间允许新的 WAL 插入，因此它通过先确定 REDO 起点、释放插入锁、刷数据、完成 checkpoint 的顺序建立一致性；关机 checkpoint 则可禁止并发 WAL 插入。

## 怎么实现的

1. `CreateCheckPoint` 先运行 `SyncPreCheckpoint`，然后在关键区检查是否需要跳过空闲系统的重复 checkpoint；获取必要事务、时间线和 WAL 状态。`CheckPoint` 结构保存 redo、当前/前序 timeline、nextXid、nextMulti、oldestXid 等重启元数据。
2. 在线场景先锁住 WAL 插入状态，插入专用 `XLOG_CHECKPOINT_REDO` 记录以确定 redo LSN，再释放插入锁让正常事务继续；关机场景没有并发插入，redo 位置可直接对应将写入的 checkpoint 记录。线上双记录设计使长时间刷盘不必冻结所有 WAL 写者。
3. 准备好的状态离开关键区后，检查点等待处于提交关键阶段且可能造成记录与状态页跨 REDO 边界的事务退出；随后 `CheckPointGuts` 依序刷 relation map、replication origin、CLOG、commit timestamp、SUBTRANS、MultiXact、predicate 锁与共享 buffer，并执行排队的 sync 请求。后续再处理 replication slots、逻辑快照/重写及 2PC 状态。
4. 回到关键区插入在线完成记录或关机记录，`XLogFlush(recptr)` 确保该 WAL 记录落盘；然后在 `ControlFileLock` 下更新 `checkPoint`、`checkPointCopy`、校验状态和恢复点并更新控制文件。旧 WAL 清理、预分配和统计是在此后整理。

## 关键取舍

- **在线 checkpoint 用 redo 起点与完成记录分开表示（源码事实）**：先写 REDO 标记，允许 checkpoint 扫描/刷页期间继续写 WAL，最后写完成记录；代价是必须保证屏障两侧提交状态与相关页同步正确。若从开始到结束禁止插入，长时间刷盘会令写事务停顿。
- **将可能失败的 I/O 放在关键区外（源码事实）**：`CreateCheckPoint` 明确在刷共享页前结束关键区，I/O 失败可中止 checkpoint 而非无条件系统 PANIC；但完成记录和控制文件发布仍需遵守严格先后关系，未完成 checkpoint 不得被当作新重启点。
- **为跨页事务状态更新等待事务关键段（源码事实）**：`CreateCheckPoint` 调用 `GetVirtualXIDsDelayingChkpt` 等待 `DELAY_CHKPT_START/COMPLETE`，避免 commit WAL 已在 REDO 点之前、而其 CLOG 状态未包含在本轮刷盘中的窗口。真实相关 caller 包含 `RecordTransactionCommit`（`xact.c`），它保护 commit WAL 与状态更新阶段；漏等会导致恢复时丢失状态而数据页可能已引用该事务结果。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xlog.c` | `CreateCheckPoint` | 采样状态、选择 REDO 边界、调度刷页并发布 checkpoint 记录/控制文件 |
| `src/backend/access/transam/xlog.c` | `CheckPointGuts` | 写 SLRU 与 buffer 脏页并执行文件同步，再处理 checkpoint 相关辅助持久状态 |
| `src/backend/access/transam/xlog.c` | `XLogFlush` | 在发布完成记录前建立 WAL 持久化屏障；本页不展开其基本写出实现 |

## 与主核 / AI 的关系

暂无直接对应。将其理解为恢复边界推进可作类比，但不能从检查点概念推导主核采用同样的 WAL/control-file 协议。

## 参考

- [PostgreSQL 文档：WAL 配置](https://www.postgresql.org/docs/current/runtime-config-wal.html) —— 说明 checkpoint、WAL 与持久化相关配置语义。
- [PostgreSQL 官方源码：checkpoint 编排](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlog.c) —— 支撑在线与关机 checkpoint 的 REDO、刷页和发布顺序。
