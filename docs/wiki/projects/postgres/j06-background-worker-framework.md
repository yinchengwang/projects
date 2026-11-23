---
project: postgres
feature: background-worker-framework
schema_version: 1
page_type: feature
refs:
  - path: src/backend/postmaster/bgworker.c
    symbols: [RegisterBackgroundWorker, RegisterDynamicBackgroundWorker, BackgroundWorkerMain, BackgroundWorkerStateChange]
---

# 后台 worker 框架

## 一句话职责

后台 worker 框架提供一套受 postmaster 监管的常驻子进程机制：worker 把自己注册进共享内存槽位，postmaster 认领并 fork 它们，子进程再按需连接数据库并进入用户代码。

## 机制是什么

这条框架服务的是「不是用户会话、但需要在实例内长期跑」的工作：并行查询的 worker、逻辑复制的 apply worker、autovacuum worker、分区同步 worker、校验和 worker。它们和普通后端的区别在于：普通后端由客户端连接驱动，生命周期与连接绑定；worker 由 postmaster 拉起，生命周期由注册记录和重启策略决定。

框架把「注册」拆成两条路径。**静态注册** `RegisterBackgroundWorker` 只能发生在 postmaster 进程里、必须在 `shared_preload_libraries` 加载期间、必须在共享内存初始化之前；它对应的是随实例启动就存在的 worker。**动态注册** `RegisterDynamicBackgroundWorker` 由普通后端在运行时发起，占用共享内存里的一个槽位；并行查询、逻辑复制 apply worker、autovacuum worker走的都是这条路径。

框架**不负责**：worker 内部做什么（那是 autovacuum、逻辑复制等各自的机制）、worker 的重启策略语义如何解释为具体行为、以及 postmaster 自身的 fork/信号框架（那是 j01 的范围）。它负责的是「注册—认领—fork—初始化—退出通知」这条生命周期的骨架，以及共享内存里槽位的分配、回收与并发安全。

## 怎么实现的

1. 共享内存里放的是 `BackgroundWorkerArray`：`total_slots` 等于 `max_worker_processes`，`slot[]` 是变长数组，每个元素是 `BackgroundWorkerSlot`，字段包括 `in_use`（槽位是否被占用）、`terminate`（是否要求终止）、`pid`（`InvalidPid` 表示还没启动，0 表示已死）、`generation`（槽位回收时自增，用来识别槽位是否被复用）以及内嵌的 `BackgroundWorker` 描述符。`BackgroundWorkerShmemInit` 把 postmaster 私有链表里的 worker 逐条拷进共享数组，保证私有链表与共享数组槽位一一对应；剩余槽位标记为未使用。
2. `BackgroundWorker` 描述符（`src/include/postmaster/bgworker.h`）的内容包括 `bgw_name`、`bgw_type`、`bgw_flags`、`bgw_start_time`（允许启动的最早 postmaster 状态）、`bgw_restart_time`（崩溃后重启等待秒数，`BGW_NEVER_RESTART` 表示不重启）、`bgw_library_name`、`bgw_function_name`、`bgw_main_arg`（一个 `Datum` 传参）、`bgw_extra`（固定长度扩展字段）和 `bgw_notify_pid`（状态变化时向谁发 SIGUSR1）。标志位里 `BGWORKER_SHMEM_ACCESS` 表示要连共享内存，`BGWORKER_BACKEND_DATABASE_CONNECTION` 表示需要数据库连接，`BGWORKER_CLASS_PARALLEL` 表示属于并行 worker 配额。
3. `RegisterBackgroundWorker`（静态）的前置检查很硬：`IsUnderPostmaster` 为假且 `IsPostmasterEnvironment` 为真时，说明处在 EXEC_BACKEND 或单用户模式，若正在加载 `shared_preload_libraries` 就直接静默返回，否则记 LOG 级错误说明「必须在 shared_preload_libraries 中注册」；`BackgroundWorkerData != NULL` 说明共享内存已初始化，此时 `elog(ERROR)` 拒绝。静态 worker 不允许设 `bgw_notify_pid`（只有动态 worker 能请求通知）。注册数受 `max_worker_processes` 限制，源码明确承认这个限制「过于严格」——不连共享内存的 worker 本来不必计入 `MAX_BACKENDS`。
4. `RegisterDynamicBackgroundWorker`（动态）先判断 `IsUnderPostmaster`，为假直接返回 false（postmaster 里不能再注册，单后端环境也没有第二个进程可起）。随后取 `BackgroundWorkerLock` 排他锁：若 worker 带 `BGWORKER_CLASS_PARALLEL` 且 `parallel_register_count - parallel_terminate_count` 已达 `max_parallel_workers`，释放锁并返回 false。然后扫描空槽位，`memcpy` 整个描述符、置 `slot->pid = InvalidPid`、`slot->generation++`、清 `terminate`；并行类还要 `parallel_register_count++`。关键次序是 `pg_write_barrier()` 之后才置 `slot->in_use = true`，保证 postmaster 不会先看到「已占用」再看到内容。
5. 释放锁后若成功，`SendPostmasterSignal(PMSIGNAL_BACKGROUND_WORKER_CHANGE)` 叫醒 postmaster；若调用方给了 `BackgroundWorkerHandle`，则把 `slot` 与 `generation` 填进去——`generation` 是用来识别「我等的那个 worker 的槽位是不是已经被别人复用了」的判据。
6. postmaster 收到信号后调 `BackgroundWorkerStateChange(allow_new_workers)`。源码开头就点明这里运行在 postmaster 里，「必须非常小心不假设共享内存内容是合理的，否则一个恶意后端就能搞垮 postmaster」。它先核对 `max_worker_processes` 与 `total_slots` 是否一致，不一致就记 LOG 并直接返回；随后逐槽 `pg_read_barrier()` 再读内容。
7. `BackgroundWorkerStateChange` 对已知 worker 只处理 `terminate` 标志：置位后如果进程已启动就 `kill(pid, SIGTERM)`，如果还没启动则调 `ReportBackgroundWorkerPID` 把它当成已死的上报。对未知 worker，若 `allow_new_workers` 为假就立刻 `slot->terminate = true`——源码解释这样是为了「确保任何等待该 worker 的进程都会被唤醒，尽管它永远不会真正运行」。已标终止的槽位直接回收：并行类 `parallel_terminate_count++`、`slot->pid = 0`、`pg_memory_barrier()` 后清 `in_use`，再给 `bgw_notify_pid` 发 SIGUSR1。
8. 新 worker 的登记同样谨慎：字符串用 `ascii_safe_strlcpy` 防御性拷贝，源码说明「如果共享内存被破坏，源数据可能连 NUL 结尾都没有」；`bgw_flags`、`bgw_start_time`、`bgw_restart_time` 直接赋值，源码注明这些即使被破坏也不会造成大碍，其余字段只被子进程看，崩了也连累不到 postmaster。`bgw_notify_pid` 要经 `PostmasterMarkPIDForWorkerNotify` 校验是活着的后端才保留。
9. postmaster 的启动调度在 `maybe_start_bgworkers`（`src/backend/postmaster/postmaster.c`）：跳过已在运行的；已标终止的直接注销；崩溃过的 worker 若 `bgw_restart_time == BGW_NEVER_RESTART` 就注销，否则要等距上次崩溃超过 `bgw_restart_time` 秒才重启。真正启动前用 `bgworker_should_start_now` 判断 postmaster 当前状态是否满足 `bgw_start_time`（`BgWorkerStart_PostmasterStart`、`BgWorkerStart_ConsistentState`、`BgWorkerStart_RecoveryFinished` 分别对应不同阶段）。单次调用最多启动 `MAX_BGWORKERS_TO_LAUNCH` 个，超出就置 `StartWorkerNeeded`，让 `ServerLoop` 立刻再调一次。
10. `StartBackgroundWorker` 先 `AssignPostmasterChildSlot(B_BG_WORKER)` 预留子进程槽位，失败时按「worker 崩溃」处理并记 `rw_crashed_at`，源码解释这样稍后重试不会立刻再撞上同样的资源耗尽；随后 `postmaster_child_launch` fork 子进程，把 `BackgroundWorker` 结构作为启动数据传过去，成功后把 PID 写回 `rw->rw_pid` 并调 `ReportBackgroundWorkerPID` 写进共享槽位、通知 `bgw_notify_pid`。
11. 子进程侧 `BackgroundWorkerMain` 把启动数据拷进自己的 `BackgroundWorker`（分配在 `TopMemoryContext`），删掉继承来的 `PostmasterContext`，设置 `MyBgworkerEntry` 与 `init_ps_display`。信号处理按标志分岔：带 `BGWORKER_BACKEND_DATABASE_CONNECTION` 的 worker 装上 SIGINT（取消当前动作）、SIGUSR1（procsignal）、SIGFPE（浮点异常）处理器，不带的把这三个信号全部忽略；SIGTERM 用 `die`，SIGHUP/SIGPIPE/SIGUSR2 忽略，`InitializeTimeouts()` 建立 SIGALRM 处理器。
12. `BackgroundWorkerMain` 用 `sigsetjmp` 而不是 `PG_TRY` 建立错误处理点：出错时手工清 `error_context_stack`、`HOLD_INTERRUPTS`、`BackgroundWorkerUnblockSignals`、调 `EmitErrorReport` 上报，然后 `proc_exit(1)`。正常路径上先 `InitProcess()` 建立共享内存里的 `PGPROC`（在此之前不能用 LWLocks 和共享内存），再 `BaseInit()`，然后用 `LookupBackgroundWorkerFunction` 找入口：库名是 `postgres` 就在 `InternalBGWorkers[]` 表里查（并行 worker、apply worker、表同步 worker 等），否则用 `load_external_function` 从外部扩展动态加载。
13. 关键设计：worker **不**在启动时调 `InitPostgres`——源码说「对 worker 而言我们还不知道要连哪个数据库，所以要等用户代码通过 `BackgroundWorkerInitializeConnection` 来做」。该函数会检查 `bgw_flags` 里必须有 `BGWORKER_BACKEND_DATABASE_CONNECTION`，否则以 FATAL 结束；连上之后 `SetProcessingMode(NormalProcessing)` 才算进入正常处理模式。

## 关键取舍

- 源码事实：静态注册被限制在 postmaster 进程、共享内存初始化之前，源码承认在 EXEC_BACKEND/单用户模式下「处理有点随意，但出于历史原因我们容忍」。架构推论：把静态 worker 的生命周期锚定在实例启动期，换取 postmaster 私有链表与共享内存槽位的一一对应（`BackgroundWorkerShmemInit` 的注释明确写了这个对应关系）；替代方案（允许运行时追加静态 worker）会让两份列表失去对应，postmaster 认领槽位时无从判断某个槽位对应谁。
- 源码事实：并行 worker 的配额用两个无锁计数器实现——`parallel_register_count` 由后端在 `BackgroundWorkerLock` 下修改，`parallel_terminate_count` 只由 postmaster 修改；源码明说「postmaster 不能取锁，内存屏障也保证不了任何有用的事」，因此计数视图可能略陈旧。架构推论：接受计数的近似性，换取 postmaster 永不阻塞；替代方案（postmaster 取 `BackgroundWorkerLock`）会让 postmaster 有被拖死的风险。
- 源码事实：postmaster 侧对共享内存内容做防御性处理——字符串用 `ascii_safe_strlcpy`，固定字段直接赋值并注明「即使损坏也不会有大碍」。架构推论：放弃「共享内存必然可信」的假设，换取一个后端的恶意写入不能搞垮 postmaster；代价是每轮认领都要额外拷贝字符串。
- 源码事实：动态注册失败返回 false 而不是报错。真实 caller 是并行查询的 worker 启动路径（`src/backend/access/transam/parallel.c` 中调用 `RegisterDynamicBackgroundWorker`），该处的注释写明「调用方必须能容忍启动的 worker 少于预期，所以注册失败不需要报错；反正注册成功也不保证 worker 真能启动并初始化成功」。架构推论：把「配额不足」降级为「少几个 worker」而不是失败，换取并行查询在资源紧张时仍能跑；一旦某次注册失败，后续注册也大概率失败，代码置 `any_registrations_failed` 直接跳过剩余注册，同时必须把已分配的 error queue 拆掉，否则会等一个永远不会启动的 worker。
- 源码事实：`maybe_start_bgworkers` 单次最多启动 `MAX_BGWORKERS_TO_LAUNCH` 个，源码注释说明是为了「避免消耗 postmaster 太多注意力」，超出就置 `StartWorkerNeeded` 让 `ServerLoop` 马上再调。架构推论：放弃一次拉起全部待启动 worker，换取 postmaster 主循环对信号与子进程退出的及时响应；代价是需要多转一圈。
- 源码事实：崩溃过的 worker 要等 `bgw_restart_time` 秒才重启，`BGW_NEVER_RESTART` 则直接注销。架构推论：放弃崩溃后立刻重启，换取「崩溃—重启」循环不会打爆机器；替代方案（立即重启）在 worker 有系统性缺陷时会把 CPU 和日志刷满。
- 源码事实：静态 worker 的注册数受 `max_worker_processes` 限制，源码自评这个限制「过于严格」，理由是不连共享内存的 worker 不该计入 `MAX_BACKENDS` 限制。架构推论：用统一的槽位上限简化配额管理，接受少量浪费；替代方案（按是否连共享内存分设两套配额）会让槽位分配与并行 worker 计数两套逻辑交织。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/postmaster/bgworker.c` | `RegisterBackgroundWorker` | 静态注册入口：校验只能在 postmaster 的 `shared_preload_libraries` 阶段注册、拒绝 shmem 初始化后注册、拒绝静态 worker 设 `bgw_notify_pid`、受 `max_worker_processes` 限制，最后把记录压入 postmaster 私有链表。 |
| `src/backend/postmaster/bgworker.c` | `RegisterDynamicBackgroundWorker` | 动态注册入口：拒绝在 postmaster 中调用、在 `BackgroundWorkerLock` 排他锁下检查并行 worker 配额、扫描空槽位填入描述符并自增 `generation`、用写屏障保证 postmaster 看到一致内容，随后发 `PMSIGNAL_BACKGROUND_WORKER_CHANGE` 通知 postmaster。 |
| `src/backend/postmaster/bgworker.c` | `BackgroundWorkerMain` | worker 子进程入口：接管启动数据、按标志配置信号处理、用 `sigsetjmp` 建立错误上报点、`InitProcess`/`BaseInit` 初始化、查表或动态加载找到入口函数并调用；不自行连库，等用户代码调 `BackgroundWorkerInitializeConnection`。 |
| `src/backend/postmaster/bgworker.c` | `BackgroundWorkerStateChange` | postmaster 侧认领逻辑：核对槽位总数一致性、按读屏障逐槽扫描、只对已知 worker 处理终止标志、对未知 worker 按 `allow_new_workers` 决定拒绝或登记、用防御性字符串拷贝防止共享内存损坏危及 postmaster，并管理 `notify_pid` 的 SIGUSR1 通知。 |

## 与主核 / AI 的关系

主核 `engine/db` 是单进程多线程结构，没有 postmaster、没有 fork 出来的常驻子进程，也没有「注册—认领—fork—通知」这条链路，因此本机制在主核里没有直接对应物。可借鉴的是两点：一是把「谁在运行、谁已死、槽位是否被复用」编码进共享内存槽位并用 `generation` 识别复用；二是 worker 子进程不预知要连的库，把连接时机交给 worker 自己的代码决定。若主核将来需要常驻后台任务（校验和、物化视图刷新），更接近的对应物是线程池的常驻线程加任务队列，而不是本套 fork 机制；不要把 `BackgroundWorker` 描述符的字段布局、`bgw_start_time` 的状态机或 SIGUSR1 通知协议当作主核已有的抽象。

## 参考

- [PostgreSQL 文档：后台工作者](https://www.postgresql.org/docs/current/bgworker.html) —— 说明后台工作者的注册方式、标志位语义与重启参数，对应本页的静态与动态两条注册路径。
- [PostgreSQL 文档：配置参数 max_worker_processes](https://www.postgresql.org/docs/current/runtime-config-resource.html#GUC-MAX-WORKER-PROCESSES) —— 说明 worker 槽位总数与并行 worker 配额的配置来源。
- [PostgreSQL 官方源码：后台 worker 框架实现](https://github.com/postgres/postgres/blob/master/src/backend/postmaster/bgworker.c) —— 支撑共享内存槽位结构、注册前置条件与 postmaster 认领逻辑的事实。
