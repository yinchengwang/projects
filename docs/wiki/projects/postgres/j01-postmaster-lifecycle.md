---
project: postgres
feature: postmaster-lifecycle
page_type: feature
schema_version: 1
refs:
  - path: src/backend/postmaster/postmaster.c
    symbols: [PostmasterMain, ServerLoop, StartChildProcess, PostmasterStateMachine]
---

# postmaster 与子进程管理

## 一句话职责

postmaster 是服务端唯一长期存活的父进程：它完成进程级别的启动、监听客户端连接、按需 fork 各类子进程，并在崩溃或关停时把「哪些子进程应存在」重新调度回一致状态。

## 机制是什么

`PostmasterMain` 建立 clonable 的进程环境（数据目录锁、监听 socket、共享内存、信号处理），随后把控制权交给 `ServerLoop` 的事件循环。它自己**不执行任何 SQL、不读任何事务数据**：所有实际工作由子进程承担，postmaster 只维护一张 `ActiveChildList`（每项一个 `PMChild`，记录 child slot、PID 与 `BackendType`）并统计数据目录锁文件状态。

postmaster 的两条核心职责是分离的：`ServerLoop` 负责**事件驱动**（监听 socket 可读、子进程退出、pmsignal、关停/重载请求），`PostmasterStateMachine` 负责**状态驱动**（在 `pmState` 之间推进，按当前状态决定该启动谁、该向谁发什么信号、何时退出或重新初始化）。二者在每次循环里交替执行：任何可能改变子进程集合或关停进度的事件都只置标志，再由状态机统一推进，避免在信号处理上下文里做 fork 或复杂判定。

本页只讲进程级生命周期与调度，不讲单个会话如何解析启动包与执行查询（后端协议页）、不讲 bgworker 的注册/重启策略细节（bgworker 页）、也不讲 checkpoint 与恢复算法（各自页面）。子进程自身退出时的资源清理语义属于子进程侧，postmaster 只依据退出状态做决策。

## 怎么实现的

1. `PostmasterMain` 先设置最严格的 umask、建立 `PostmasterContext`（可被子进程复用的分配区）、安装 postmaster 专用信号处理（SIGHUP/SIGINT/SIGQUIT/SIGTERM/SIGUSR1/SIGCHLD 等）并解除信号阻塞，然后解析 `-D`、`-B`、`-c name=value` 等命令行选项、`SelectConfigFiles` 读配置。`-C guc` 在合适的时机打印配置值后直接退出。
2. 启动序列按依赖顺序推进：`process_shared_preload_libraries` 让扩展先注册 GUC 与 shmem 需求，`InitializeMaxBackends` + `InitPostmasterChildSlots` 计算并分配 child slot 数组，`InitializeFastPathLocks`，再 `process_shmem_requests`、`ShmemCallRequestCallbacks`、`InitializeShmemGUCs`、`InitializeWalConsistencyChecking`，最后 `CreateSharedMemoryAndSemaphores`；接着 `set_max_safe_fds`、`InitPostmasterDeathWatchHandle`，并建立监听 socket。
3. 进入服务前先 `UpdatePMState(PM_STARTUP)`、`maybe_start_io_workers()`，然后并行启动 checkpointer 与 bgwriter（它们能在恢复期协助），再启动 `B_STARTUP`（启动/恢复进程）并置 `StartupStatus = STARTUP_RUNNING`；`maybe_start_bgworkers()` 处理此刻已到启动时刻的 worker，之后进入 `ServerLoop`。若 `ServerLoop` 意外返回则 `ExitPostmaster`。
4. `ServerLoop` 用 `WaitEventSetWait` 等 latch 与监听 socket，`DetermineSleepTime` 决定超时。每次唤醒后：无条件处理 `pending_pm_shutdown_request`、`pending_pm_reload_request`、`pending_pm_child_exit`、`pending_pm_pmsignal`（即使没看到 latch 也处理，保证关停/重载不被事件顺序拖延）；对 `WL_SOCKET_ACCEPT` 调 `AcceptConnection` 与 `BackendStartup`，并在父进程中关闭已交给子进程的 socket。
5. 事件处理之后调用 `LaunchMissingBackgroundProcesses`：按 `pmState` 补齐当前状态应存在的进程——syslogger、io workers、PM_RUN/PM_RECOVERY/PM_HOT_STANDBY/PM_STARTUP 下的 checkpointer 与 bgwriter、PM_RUN 下的 walwriter、autovac launcher（受 `AutoVacuumingActive`/二进制升级抑制）、archiver（受 `XLogArchivingActive`/`XLogArchivingAlways` 与 `PgArchCanRestart` 约束），必要时才 fork，避免每次都创建。
6. `StartChildProcess` 是所有辅助进程的统一 fork 入口：先 `AssignPostmasterChildSlot(type)` 抢占 child slot（失败时按类型区分日志，autovac worker 属正常资源上限），再 `postmaster_child_launch` 实际 fork/exec；fork 失败要释放 slot 并记日志，且**仅当类型是 `B_STARTUP` 时视为致命**（`ExitPostmaster(1)`），其它辅助进程失败只是返回 NULL 让状态机后续重试。
7. `BackendStartup` 是客户端连接入口：用 `canAcceptConnections(B_BACKEND)` 判定当前是否接受连接；能接受就分配 `B_BACKEND` slot，slot 耗尽则降级为 `CAC_TOOMANY` 并 `AllocDeadEndChild`（该子进程只负责向客户端报错后退出，不进入正常后端）；fork 失败时以 V2 协议错误包尽力通知客户端。成功则把 PID 记入 `ActiveChildList`。
8. 子进程异常退出由 `process_pm_child_exit` 收集，`HandleChildCrash` 在非 `FatalError`/非立即关停时记日志并转 `HandleFatalError(PMQUIT_FOR_CRASH, …)`：置 `FatalError`、向其余子进程发信号。`PostmasterStateMachine` 遂按崩溃路径推进。
9. 状态机按 `pmState` 分段推进：`PM_RUN`/`PM_HOT_STANDBY` 且 `!connsAllowed`（智能关停）在普通后端归零后转 `PM_STOP_BACKENDS`；`PM_STOP_BACKENDS` 先 `ForgetUnstartedBackgroundWorkers()`，再对目标掩码发 SIGTERM（崩溃时改走 `HandleFatalError(PMQUIT_FOR_STOP)`）并转 `PM_WAIT_BACKENDS`；目标进程归零后，立即关停或崩溃直接转 `PM_WAIT_DEAD_END` 并 SIGQUIT dead-end 子进程，正常关停则确保 checkpointer 存在并发 SIGINT 让它写关停 checkpoint，转 `PM_WAIT_XLOG_SHUTDOWN`。
10. 后续阶段依次是 `PM_WAIT_XLOG_ARCHIVAL`（等 walsender/archiver 退出，转 `PM_WAIT_IO_WORKERS` 并发 SIGUSR2）、`PM_WAIT_IO_WORKERS`（`io_worker_count == 0` 后转 `PM_WAIT_CHECKPOINTER` 并向 checkpointer 发 SIGUSR2）、`PM_WAIT_DEAD_END`（只剩 logger 时转 `PM_NO_CHILDREN`）；其中 XLOG_SHUTDOWN → XLOG_ARCHIVAL 的跳转发生在 `process_pm_pmsignal` 收到 `PMSIGNAL_XLOG_IS_SHUTDOWN` 时，不在状态机内。
11. `PM_NO_CHILDREN` 是决策收敛点：`Shutdown > NoShutdown` 时按 `FatalError` 决定 `ExitPostmaster(1)`（记 "abnormal database system shutdown"）或 `ExitPostmaster(0)`；`StartupStatus == STARTUP_CRASHED` 或 `restart_after_crash` 关闭时直接退出；否则若 `FatalError`，则按需清理临时文件、`ResetBackgroundWorkerCrashTimes()`、`shmem_exit(1)`、重读控制文件、`ResetShmemAllocator` + 重新建共享内存、回到 `PM_STARTUP` 并重启 `B_STARTUP`，再把监听 socket 加回 waitset——即崩溃自动重启。
12. `ServerLoop` 的周期性兜底独立于状态机：对 `Shutdown >= ImmediateShutdown || FatalError` 且已发过优雅关停超过 `SIGKILL_CHILDREN_AFTER_SECS` 的子进程补发 SIGKILL/SIGABRT；每分钟校验 `postmaster.pid` 是否被删除或篡改，失效则自杀式立即关停；每 58 分钟 touch Unix socket 与锁文件，避免被 /tmp 清理器误删。

## 关键取舍

- 源码事实：所有状态推进集中在 `PostmasterStateMachine`，信号处理器只置 `pending_*` 标志，`ServerLoop` 在主循环里统一处理。架构推论：把 fork、信号群发、退出判定放在非信号上下文，避免在异步信号处理中触碰锁/内存分配这些不安全操作；若在处理器内直接 fork 或判定状态，重入与半完成状态会让崩溃恢复路径不可预测。
- 源码事实：只有 `B_STARTUP` fork 失败被视为致命并 `ExitPostmaster(1)`，其它辅助进程 fork 失败只记日志并返回 NULL。架构推论：启动/恢复进程缺位就无法进入可用状态，而 checkpointer/bgwriter/walwriter 等可在后续 `LaunchMissingBackgroundProcesses` 重试，接受短暂能力降级换取 postmaster 不在资源瞬时不足时整体退出。
- 源码事实：`BackendStartup` 在 slot 耗尽时降级为 dead-end 子进程（`CAC_TOOMANY`），而不是拒绝连接或无限等待；子进程只在关停末段被 SIGQUIT。架构推论：让超限客户端收到明确的服务器错误而非连接超时，代价是仍需 fork 一个轻量进程；若直接 close socket，客户端只能看到连接被重置，错误信息不可解释。
- 源码事实：崩溃恢复不重建 postmaster 自身，而是 `shmem_exit(1)` 后重读控制文件、`ResetShmemAllocator`、重新 `CreateSharedMemoryAndSemaphores`，再重启 `B_STARTUP`；`RegisterBuiltinShmemCallbacks` 不再重复调用，保留旧注册。架构推论：复用父进程可以省掉重新初始化 GUC/预加载库的成本，代价是共享内存布局必须可重入地重建，且扩展无法在崩溃路径重新注册 shmem 钩子。
- 源码事实：`restart_after_crash` 为 off 或 `StartupStatus == STARTUP_CRASHED` 时，即使仍是 `PM_NO_CHILDREN` 也直接退出而不自动重启。架构推论：把"恢复进程本身失败"与"普通子进程崩溃"分开，避免对必然失败的启动循环做无意义重启风暴。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/postmaster/postmaster.c` | `PostmasterMain` | 进程级启动编排：锁数据目录、算 `MaxBackends` 与 child slots、建共享内存与监听 socket，然后启动首批子进程并进入 `ServerLoop`。 |
| `src/backend/postmaster/postmaster.c` | `ServerLoop` | 事件循环：等 latch/socket，消费 pending 标志、接受连接、补齐缺失后台进程，并做 SIGKILL 超时、锁文件复核与 socket touch 的周期兜底。 |
| `src/backend/postmaster/postmaster.c` | `StartChildProcess` | 辅助进程统一 fork 入口：先占 child slot 再 fork，失败时释放 slot，只有 `B_STARTUP` 失败升级为致命。 |
| `src/backend/postmaster/postmaster.c` | `PostmasterStateMachine` | 依据 `pmState` 与存活子进程集合推进关停/崩溃/重启：发信号、等各阶段清零、在 `PM_NO_CHILDREN` 决定退出或重新初始化。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL 这种由单一 postmaster 通过 fork 与信号状态机管理全部子进程生命周期的结构。可借鉴的是把「事件收集」与「状态推进」分成两层、把 child slot 抢占放在 fork 之前以便干净处理失败；不要把 postmaster 的 `pmState` 或 SIGQUIT/SIGKILL 阶梯当成主核已有的进程管理语义。

## 参考

- [PostgreSQL 文档：服务器进程与启动](https://www.postgresql.org/docs/current/runtime-config-connection.html) —— 说明监听地址、端口与连接相关配置入口。
- [PostgreSQL 文档：pg_ctl](https://www.postgresql.org/docs/current/app-pgctl.html) —— 说明智能/快速/立即三种关停方式及其信号语义。
- [PostgreSQL 官方源码：postmaster 实现](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/postmaster.c) —— 支撑启动序列、事件循环与状态机事实。
