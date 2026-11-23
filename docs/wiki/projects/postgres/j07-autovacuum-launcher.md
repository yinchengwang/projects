---
project: postgres
feature: autovacuum-launcher
page_type: feature
schema_version: 1
refs:
  - path: src/backend/postmaster/autovacuum.c
    symbols: [AutoVacLauncherMain, do_start_worker, AutoVacWorkerMain, do_autovacuum]
---

# autovacuum 调度

## 一句话职责

autovacuum launcher 周期性挑选数据库并请求 postmaster 启动 worker；worker 在单个数据库内挑出需要维护的关系，避免维护需求积累到 XID/MultiXact 环回风险。

## 机制是什么

launcher 与 worker 分工：`AutoVacLauncherMain` 常驻但不常驻某个数据库，它维护按下次可运行时间排序的 `DatabaseList`，基于 `autovacuum_naptime`、空闲 worker 槽数与启动 worker 的互斥状态决定何时请求派发。真正 fork 仍由 postmaster 执行；launcher 调 `SendPostmasterSignal(PMSIGNAL_START_AUTOVAC_WORKER)`，postmaster 的启动失败会通过共享 `av_signal[AutoVacForkFailed]` 回报，launcher 等待后重发。

worker 注册时由 `do_start_worker` 从数据库列表中选择一个当前有维护需求或最接近 freeze 截止的数据库，依据统计与配置形成候选；启动时在共享 `AutovacuumLock` 下从 `av_freeWorkers` 移一个 `WorkerInfo` 至唯一 `av_startingWorker`，postmaster 收到请求后按一个 worker child slot 起进程。worker 进入 `AutoVacWorkerMain` 后必须先在共享锁下把自身从 starting 改登记到 `av_runningWorkers`、清空 `av_startingWorker`，再连接数据库；这样 launcher 能在本 worker 启动阶段结束后继续安排其他数据库。

该机制负责“什么时候启动谁、启动失败如何重试、worker 在库内收集候选并串行/并行维护”；不负责单张表的 VACUUM 算法、死 tuple 回收、冻结规则细节，也不负责 checkpointer。特别是 `do_autovacuum` 只是基于 reloptions/pgstat 对候选做筛选、排序、协调 worker 重叠并发出 VACUUM/ANALYZE，具体 heap 维护过程见 C 组。

## 怎么实现的

1. `AutoVacLauncherMain` 作为 auxiliary process 建立 `PGPROC`、`InitPostgres` 后装入独立 memory context，用 `sigsetjmp` 提供 launcher 自己的错误恢复；发生 ERROR 时输出错误、abort transaction、释放 LWLock/buffer/file 等遗留资源、重置 context，并至少 sleep 后再继续，避免错误循环刷日志。它把 `search_path` 设空、禁用危险 damaged-page 选项及维护会被阻碍的 session timeouts，并强制 READ COMMITTED 与新鲜统计读取。
2. launcher 首先 `rebuild_database_list(InvalidOid)`，列表按 `adl_next_worker` 排序；每轮先 `launcher_determine_sleep` 得睡眠间隔，再 `WaitLatch(WL_LATCH_SET | WL_TIMEOUT | WL_EXIT_ON_PM_DEATH)`。唤醒后处理配置重载、关停、屏障和信号。SIGUSR2 表示 worker 退出/失败：可能要在排他 `AutovacuumLock` 下重平衡 cost limit，fork 失败时 sleep 后重发 postmaster signal。
3. 启动前在共享 `AutovacuumLock` 下调用 `av_worker_available()`：必须有 free worker slot、且没有 `av_startingWorker` 长时间未完成；若超时则清理 starting entry 回收至 free list 并记录 warning。若没有 worker slot 或另一个 worker 仍在连接期，直接睡眠，不再派发。
4. `DatabaseList` 为空时按 naptime 限制立即尝试一个数据库；否则因列表从最远时间到最近时间排序，检查尾元素 `adl_next_worker` 到期才 `launch_worker(current_time)`。一次启动后把该 DB 下次时间设为当前时间加 `autovacuum_naptime`，移到队头；数据库列表定期重建，以反映新增/删除 DB 与统计改变。
5. `do_start_worker` 在启动选择前先快速检查 worker slot，再以临时 memory context 获取数据库列表；读取当前 XID/MultiXact 及 freeze force limit，优先选可能 wraparound 数据库（XID 风险优先于 MultiXact），否则从 pgstat 中找最久未做 autovacuum 的数据库。若当前 worker 配置与 slot 数受限，按可用容量/权重进行派发和 cost limit 平衡，选中 DB 后在 `AutovacuumLock` 下把 worker info 登记到 `av_startingWorker` 并通过 postmaster signal 请求 fork。
6. `AutoVacWorkerMain` 安装信号：SIGINT 只取消当前表的 vacuum，SIGTERM 正常清理退出，SIGQUIT 放弃进程；`InitProcess` 后把 search_path 清空、关闭会阻碍 maintenance 的 timeout、必要时把 synchronous_commit 降到 local。它在 `AutovacuumLock` 排他锁下读取 `av_startingWorker`、填 `wi_proc` 并移到 `av_runningWorkers`，清空 starting 指针、注册 `FreeWorkerInfo` 为 shmem-exit callback，再唤醒 launcher；无 worker entry 则 warning 后退出。
7. worker 在 `InitPostgres` 之前调用 `pgstat_report_autovac(dbid)` 记录本次启动时间，避免连接失败的数据库被反复挑中；连接到目标库后读 XID/MultiXact，进入 `do_autovacuum`。若该 worker 遇到 ERROR，不像 launcher 一样继续循环：记录错误后 `proc_exit(0)`，由 launcher 稍后重启替代。
8. `do_autovacuum` 建 worker 长生命周期 memory context，启动事务，读取 `pg_database` 决定 freeze 参数；扫描 `pg_class` 两遍，第一遍收集普通表/物化视图的 `reloptions`、TOAST 对应关系并运行 `relation_needs_vacanalyze`，记录待处理 OID 与 score，同时记下 orphan temp table；第二遍独立处理 TOAST 表，并把未显式设置的 TOAST options 从主表合并。其他 backend 的 temp relation 不可安全处理，活跃 temp namespace 跳过，确认 idle 的 orphan relation 后才另取 AccessExclusiveLock 与 namespace AccessShareLock 并重新核验，再单独 transaction DROP。
9. 候选表按 vacuum、analyze、freeze urgency 等 score 排序（全零权重时跳过排序），复用 `BAS_VACUUM` buffer access strategy 限制共享缓存污染。逐表前检查 `ConfigReloadPending` 与 interrupts，拿 `AutovacuumScheduleLock` 排他锁及 `AutovacuumLock` 共享锁扫描 `av_runningWorkers`，避开同库/共享表上已被别的 worker 处理的关系；获准后登记当前 `wi_tableoid`，release schedule lock 再执行 VACUUM/ANALYZE，完成后清理该 worker 的表占用标记。

## 关键取舍

- 源码事实：launcher 不直接 fork，而是用 `PMSIGNAL_START_AUTOVAC_WORKER` 请求 postmaster；全局只允许一个 `av_startingWorker`，worker 在完成数据库连接准备前仍占该入口，超时后 launcher 回收并记录 warning。架构推论：把 OS 进程创建集中在 postmaster，并在“启动中”与“运行中”区分状态，避免 worker fork 瞬间把空闲 slot 当成可并发启动；若 launcher 自己 fork，postmaster 的 child-slot 管理与关停/崩溃重启会出现双重所有者。
- 源码事实：`do_start_worker` 在 worker slot 不足时立即返回 InvalidOid；launcher 在 worker slot/starting lock 检查后不再启动，配置同时检查 `autovacuum_max_workers` 与 `autovacuum_worker_slots`。架构推论：以维护延迟换取资源上限与可控并发；若超出可用 slots 仍并发启动，shared worker info 与 proc slots 会越界或压迫正常 backend。
- 源码事实：launcher 的 `AutoVacForkFailed` 分支 sleep 1 秒后重发 postmaster signal；worker ERROR 则清理并退出，launcher 后续再派生替代者。架构推论：短暂 fork 资源故障采用限速重试，避免 busy loop；worker 自身状态复杂时不试图在部分失败状态恢复，而是用进程重启隔离故障，代价是该次收集候选工作丢失。
- 源码事实：`do_autovacuum` 遍历候选表前通过 `AutovacuumScheduleLock` + `AutovacuumLock` 检查其它 `av_runningWorkers` 的 `wi_tableoid`，跳过同时维护的关系；temp table 先检查 owner namespace 状态并在删 orphan 前重新加锁、复核。架构推论：用调度级锁而非每张表一个全局队列，维护成本更低，但每个 worker 的扫描仍可能重复读共享状态；删除 orphan 则宁愿漏过本轮也不冒并发删活表风险。
- 源码事实：launcher 强制维护进程的 search_path 为空、关掉若干用户可设 timeout，并令 stats fetch consistency 为 none；worker 也把 synchronous_commit 降至 local（若原来要求更高）。架构推论：维护任务不应被会话用户设置的路径注入、timeout 或同步备库等待拖住；代价是该后台会话故意不继承普通用户会话的一部分运行配置。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/postmaster/autovacuum.c` | `AutoVacLauncherMain` | 常驻调度循环：建 DB 时间表、处理信号/重载、检查 slot 与 starting 状态后请求 worker，并在 launcher 自身 ERROR 后回收上下文。 |
| `src/backend/postmaster/autovacuum.c` | `do_start_worker` | 对 DB 列表做 wraparound 与统计优先级选择，预留共享 WorkerInfo 并通知 postmaster 派发。 |
| `src/backend/postmaster/autovacuum.c` | `AutoVacWorkerMain` | worker 启动时把 starting entry 变成 running entry、连接选定 DB，并在 ERROR 时清理退出。 |
| `src/backend/postmaster/autovacuum.c` | `do_autovacuum` | 扫 pg_class 收集需维护 relation、过滤孤儿/并发项，排序候选并执行 VACUUM/ANALYZE 命令。 |

## 与主核 / AI 的关系

主核没有 PostgreSQL launcher/worker 组成的自动维护调度，也没有按 database/table stat 周期安排 vacuum/analyze 的对应机制。可借鉴把“发现/调度”与“单对象维护”拆成后台 worker 的模式，以及对 XID 风险设置比普通统计更高的优先级；不要将它描述为主核已有的对象回收器。

## 参考

- [PostgreSQL 文档：自动清理](https://www.postgresql.org/docs/current/routine-vacuuming.html) —— 说明 autovacuum 的维护目的、冻结与统计收集背景。
- [PostgreSQL 文档：自动清理配置](https://www.postgresql.org/docs/current/runtime-config-autovacuum.html) —— 说明 launcher/worker 的启用、worker 上限与调度配置。
- [PostgreSQL 官方源码：autovacuum launcher 与 worker](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/autovacuum.c) —— 支撑数据库选择、worker slot 登记与候选表处理路径。
