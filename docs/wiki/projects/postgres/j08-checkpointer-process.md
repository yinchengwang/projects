---
project: postgres
feature: checkpointer-process
page_type: feature
schema_version: 1
refs:
  - path: src/backend/postmaster/checkpointer.c
    symbols: [CheckpointerMain, ExecCheckpoint, RequestCheckpoint]
---

# checkpointer 进程

## 一句话职责

checkpointer 是常驻后台进程，决定“何时该做 checkpoint 或 restartpoint”，在后台把 checkpoint 请求统一排队执行并做写速率节流，同时承担若干不适合分散给普通后端的维护动作。

## 机制是什么

普通后端、WAL 写入路径与用户在 SQL 里执行 `CHECKPOINT` 都**不自己做 checkpoint**：它们只把请求标志写进共享内存 `CheckpointerShmem->ckpt_flags` 并唤醒 checkpointer 的 latch。checkpointer 是唯一执行者，因此 checkpoint 的触发源可以统一排队、合并与限速，且崩溃后进度可以用共享计数器观测。

checkpointer 的一轮循环由四类事件驱动：共享标志里的显式请求（`CHECKPOINT_REQUESTED`/`CHECKPOINT_FAST`/`CHECKPOINT_FORCE` 等）、`CheckPointTimeout` 到期的时间驱动、恢复期需要做 restartpoint、以及 `XLogArchiveTimeout` 到期需要强制切换 WAL 段。前两类做完才更新 `last_checkpoint_time`（源码特意记开始时间而非结束时间，使时间驱动间隔可预测）；restartpoint 未能真正执行时把下次尝试提前到 15 秒后。

本页只讲进程侧的职责边界：触发判定、请求排队与等待、写延迟节流、fsync 请求队列托管、shared memory 配置发布和关停阶段的收尾顺序。**checkpoint 内部的 WAL 屏障与刷盘语义（redo 点、`CreateCheckPoint` 的写盘顺序、`max_wal_size` 如何触发）属于 WAL/checkpoint 机制页**，本页不解释其算法，也不讨论 bgwriter 的脏页写出策略。

## 怎么实现的

1. `CheckpointerMain` 先 `AuxiliaryProcessMainCommon()` 并把 `checkpointer_pid` 写进 `CheckpointerShmem`。信号安装有明确取舍：SIGHUP 走配置重载、SIGINT 是 `ReqShutdownXLOG`（置 `ShutdownXLOGPending` 并 `SetLatch`）、**SIGTERM 被显式忽略**（系统关机时 init 会同时 SIGTERM 所有进程，这里要等 postmaster 用 SIGUSR2 才真正退出）、SIGUSR1 走 `procsignal_sigusr1_handler`、SIGUSR2 走 `SignalHandlerForShutdownRequest`。
2. 它注册 `before_shmem_exit(pgstat_before_server_shutdown, 0)`，因为 checkpointer 是所有进程里最后退出的一个：等 shutdown checkpoint 写完、且所有可能上报统计的进程（含 walsender）都退出后，才轮到它做统计落盘。
3. 建立私有 `checkpointer_context` 供每轮工作使用，并用 `sigsetjmp` 作为异常恢复的落点。错误路径只做 `AbortTransaction()` 的最小等价集：`LWLockReleaseAll()`、`ConditionVariableCancelSleep()`、`pgstat_report_wait_end()`、`pgaio_error_cleanup()`、`UnlockBuffers()`、`ReleaseAuxProcessResources`、`AtEOXact_Buffers/SMgr/Files/HashTables`；若 `ckpt_active` 仍为真，则在 `ckpt_lck` 下递增 `ckpt_failed` 并把 `ckpt_done` 对齐到 `ckpt_started`，再 `ConditionVariableBroadcast(&done_cv)`，让所有等待中的后端立刻收到“本次 checkpoint 失败”，而不是一直挂住。
4. 进入主循环前 `UpdateSharedMemoryConfig()`：由于 checkpointer 最后退出，源码把“把 SIGHUP 改动后的配置同步到共享内存主副本”也交给它做；`ProcessCheckpointerInterrupts` 在每次配置重载后重复这一步。
5. 主循环每轮先 `ResetLatch`、`AbsorbSyncRequests()`、`ProcessCheckpointerInterrupts()`；若 `ShutdownXLOGPending || ShutdownRequestPending` 则跳出。随后检查 `ckpt_flags` 是否非零（读该字不需要 `ckpt_lck`），再比 `time(NULL) - last_checkpoint_time >= CheckPointTimeout` 是否到期；时间到期即使没有显式请求也做，并置 `CHECKPOINT_CAUSE_TIME`。
6. 真正开始前在 `ckpt_lck` 下把 `ckpt_flags` 原子取走并清零、`ckpt_started++`，然后 `ConditionVariableBroadcast(&start_cv)` —— 顺序上先“宣告开始”再执行，等待者据此区分“我这一请求已被看到”。`RecoveryInProgress()` 决定走 restartpoint，`CHECKPOINT_END_OF_RECOVERY` 例外为真正的 checkpoint。
7. 统计与告警也在这里分流：时间触发与显式请求分别计入 `PendingCheckpointerStats` 的 timed/requested、restartpoints_timed/requested；若本次由 WAL 填充触发（`CHECKPOINT_CAUSE_XLOG`）且距上次不足 `CheckPointWarning`，记 log 并提示增大 `max_wal_size`。随后置 `ckpt_active = true`、记录 `ckpt_start_recptr`（正常取 `GetInsertRecPtr()`，恢复取 `GetXLogReplayRecPtr()`）与 `ckpt_start_time`，再 `CreateCheckPoint(flags)` 或 `CreateRestartPoint(flags)`。
8. 无论成功失败，执行完都 `smgrdestroyall()`：checkpointer 不处理共享失效消息、也不调用 `AtEOXact_SMgr()`，不主动销毁 smgr 对象则被删关系的 smgr 永不释放。完成后再在 `ckpt_lck` 下 `ckpt_done = ckpt_started` 并广播 `done_cv`，等待方据此结束。
9. 每轮末尾还做三件与 checkpoint 无直接关系但“必须有人定时做”的事：`DisableLogicalDecodingIfNecessary()`（避免逻辑解码长期占用资源）、`CheckArchiveTimeout()`（`XLogArchiveTimeout` 到期时用 `RequestXLogSwitch(true)` 切段，但只在 `GetLastImportantRecPtr()` 超过上次切段点时才切，避免把仅含不重要记录的空段归档）、以及 `pgstat_report_checkpointer()` / `pgstat_report_wal(true)` 上报统计。若 `ckpt_flags` 又有新请求则立刻重跑一轮不睡。
10. 睡眠时长不是固定值：取 `CheckPointTimeout` 剩余与 `XLogArchiveTimeout` 剩余（仅在非恢复期且该参数大于零）的较小者，再用 `WaitLatch(WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH)`；两者都已到期则不睡直接 continue。
11. 写延迟节流由 `CheckpointWriteDelay` 完成，它被 `BufferSync` 每写完一页调用一次：若 `!(flags & CHECKPOINT_FAST)`、没有关停/快速请求、且 `IsCheckpointOnSchedule(progress)` 为真，就短暂 `WaitLatch(..., 100, WAIT_EVENT_CHECKPOINT_WRITE_DELAY)` 顺带吸收 fsync 请求；`IsCheckpointOnSchedule` 把 `progress` 乘以 `CheckPointCompletionTarget`，分别与“已写 WAL 段占 `CheckPointSegments` 的比例”和“已耗时间占 `CheckPointTimeout` 的比例”比较，并缓存上次算出的 elapsed 以避免重复开销。落后时不等，而是把每 `WRITES_PER_ABSORB` 次写盘做一次 `AbsorbSyncRequests()`，防止 fsync 请求队列溢出。
12. 请求侧：`RequestCheckpoint(flags)` 在独立后端模式下直接 `CreateCheckPoint(flags | CHECKPOINT_FAST)` 并 `smgrdestroyall()`（standalone 没有别的后端可被打扰，不需要慢速节流）。在 postmaster 环境下，它在 `ckpt_lck` 下取 `ckpt_failed`/`ckpt_started` 快照并 `ckpt_flags |= (flags | CHECKPOINT_REQUESTED)`——用 OR 而非赋值，避免覆盖别的后端设过的“更强”请求。然后最多 `MAX_SIGNAL_TRIES`（600 次、每次 100ms）尝试读到 `checkpointerProc` 并 `SetLatch`；未设 `CHECKPOINT_WAIT` 时通知失败只记 LOG。
13. 设了 `CHECKPOINT_WAIT` 时用两个条件变量等完：先 `start_cv` 等到 `ckpt_started != old_started`（确认自己那次已被看见），再 `done_cv` 等到 `ckpt_done - ckpt_started >= 0`（模意义下的完成）；若 `ckpt_failed != old_failed` 则 `ERROR` “checkpoint request failed” 并提示查日志。
14. 用户入口 `ExecCheckpoint` 只做参数解析与权限校验后转调：`mode` 支持 `fast`/`spread`（`spread` 即不带 `CHECKPOINT_FAST`，按 `checkpoint_completion_target` 摊开写）、`flush_unlogged` 映射 `CHECKPOINT_FLUSH_UNLOGGED`；必须有 `pg_checkpoint` 角色权限；最后拼出 `CHECKPOINT_WAIT` + 可选 FAST/FLUSH_UNLOGGED + 非恢复期时的 `CHECKPOINT_FORCE`（保证无 WAL 活动也真的做）。
15. 关停阶段：跳出主循环后 `ExitOnAnyError = true`，此后 `elog(ERROR)` 直接退出而不回到 sigsetjmp。若 `ShutdownXLOGPending` 则计入一次 requested、`ShutdownXLOG` 写关停 checkpoint、上报统计、`SendPostmasterSignal(PMSIGNAL_XLOG_IS_SHUTDOWN)`。之后进入第二个循环只等 `ShutdownRequestPending` —— 把“写关停 checkpoint”和“进程退出”刻意分开，好让 checkpointer 继续做越晚越好的统计落盘。

## 关键取舍

- 源码事实：checkpointer 显式 `pqsignal(SIGTERM, PG_SIG_IGN)`，退出只认 SIGUSR2；且关停被拆成“先写 shutdown checkpoint 并发 `PMSIGNAL_XLOG_IS_SHUTDOWN`，再等 SIGUSR2 才 `proc_exit`”。架构推论：系统关机时 init 会朝所有进程一起发 SIGTERM，若 checkpointer 跟随退出就没人写关停 checkpoint；拆成两段则能在最晚点做统计落盘。代价是 postmaster 的关停状态机多一个必须等的阶段（`PM_WAIT_XLOG_SHUTDOWN` 转到 `PM_WAIT_IO_WORKERS` 之后才向 checkpointer 发 SIGUSR2）。
- 源码事实：`RequestCheckpoint` 用 `ckpt_flags |= ...` 而不是赋值，并在 latch 未送达时重试 600 次×100ms；仅当调用方要求 `CHECKPOINT_WAIT` 时把“通知不到”升级为 ERROR。架构推论：多个后端与 WAL 触发点会并发请求同一轮 checkpoint，OR 语义让“快速/强制/恢复结束”等更强需求不被后来的弱需求抹掉；而“发起即返回”的调用方（如 WAL 填充触发）不应因为 checkpointer 尚未就绪就让写入路径失败。
- 源码事实：`CheckpointerMain` 在错误路径里若 `ckpt_active` 为真就递增 `ckpt_failed` 并把 `ckpt_done` 对齐 `ckpt_started` 再广播 `done_cv`。架构推论：等待方靠 `ckpt_done >= ckpt_started` 判定结束，若不主动“宣告失败完成”，所有 `CHECKPOINT_WAIT` 调用者会永久阻塞；代价是错误恢复代码必须知道共享计数器的语义，而非只做本地清理。
- 源码事实：`CheckpointWriteDelay` 只在“不在 `CHECKPOINT_FAST`、无关停/快速请求、且 `IsCheckpointOnSchedule` 为真”时才睡 100ms，落后就走每 `WRITES_PER_ABSORB` 次写盘吸收一次 fsync 请求的分支。架构推论：把节流做成“能跟上就摊平、跟不上就全速”的自适应行为，避免慢速 checkpoint 在写高峰反而拖长总时间；代价是节流效果依赖进度估算，`IsCheckpointOnSchedule` 用 WAL 段与时间两种估算并缓存结果。
- 源码事实：`CheckpointWriteDelay` 里对 `ConfigReloadPending` 的处理是重读配置并 `UpdateSharedMemoryConfig()`，`ForwardSyncRequest` 在队列过半时就 `SetLatch` 唤醒 checkpointer。架构推论：checkpointer 结束得太晚，很多“必须由某个常驻进程周期性做”的事情被并到它的循环里，减少额外进程；代价是这些职责（配置发布、archive_timeout、fsync 队列）与 checkpoint 的调度互相影响，错误处理里必须一并清理。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/postmaster/checkpointer.c` | `CheckpointerMain` | 进程主体：信号语义、异常恢复与失败广播、按请求/超时/恢复判定是否做 checkpoint 或 restartpoint、关停两阶段收尾，并顺带做配置发布与 archive_timeout。 |
| `src/backend/postmaster/checkpointer.c` | `ExecCheckpoint` | SQL `CHECKPOINT` 的入口：解析 `mode`/`flush_unlogged`、校验 `pg_checkpoint` 权限，然后把带 `CHECKPOINT_WAIT` 的请求交给 `RequestCheckpoint`。 |
| `src/backend/postmaster/checkpointer.c` | `RequestCheckpoint` | 后端/各触发点的请求协议：OR 进 `ckpt_flags`、按 `checkpointerProc` 唤醒 latch，按需用 `start_cv`/`done_cv` 等待起止并把失败转成错误。 |

## 与主核 / AI 的关系

主核没有独立的 checkpointer 进程，也没有“后端只置共享标志、由唯一后台进程执行并限速”的 checkpoint 请求协议。可借鉴的是把开销大的周期性维护集中到单一执行者并用共享计数器向等待方暴露起止状态；不要把 `ckpt_flags` 或 `CHECKPOINT_WAIT` 的等待语义当作主核已有能力。

## 参考

- [PostgreSQL 文档：checkpoint 相关配置](https://www.postgresql.org/docs/current/runtime-config-wal.html) —— 说明 `checkpoint_timeout`、`checkpoint_completion_target`、`max_wal_size` 等参数含义。
- [PostgreSQL 文档：CHECKPOINT 命令](https://www.postgresql.org/docs/current/sql-checkpoint.html) —— 说明手工 checkpoint 命令及 `pg_checkpoint` 权限要求。
- [PostgreSQL 官方源码：checkpointer 实现](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/checkpointer.c) —— 支撑请求排队、写延迟节流与关停顺序的事实。
