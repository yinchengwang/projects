---
project: ragflow
feature: syncer
title: 同步器：调度与任务执行
page_type: feature
schema_version: 1
refs:
  - path: internal/syncer/syncer.go
    symbols: [Syncer, NewSyncer]
  - path: internal/syncer/scheduler.go
    symbols: [Scheduler, NewScheduler, TaskEnvelope]
  - path: internal/syncer/job_executor.go
    symbols: [SyncJobExecutor, NewSyncJobExecutor]
---

# 同步器：调度与任务执行

## 一句话职责

`Syncer` 是把「定时从外部数据源拉文件、解析入库」这件事装配起来的根对象：它把调度器、任务队列、任务 worker 与共享批作业执行器接成一条流水线，本身不含任何同步逻辑。

## 机制是什么

一次数据源同步被拆成四层职责，四层之间只通过两个通道交接：

| 层 | 职责 | 交接物 |
|---|---|---|
| `Scheduler` | 发现「该跑了」的任务，投递到队列 | `TaskEnvelope` |
| 任务队列（`chan TaskEnvelope`） | 缓冲待执行任务 | — |
| `TaskWorker` | 从队列取任务、执行一轮同步 | — |
| `SyncJobExecutor` | 把任务内部的批作业公平分派到固定数量的 worker | `syncJob` |

关键点在于**调度与执行不是同一个循环**。`Scheduler` 不执行任何同步，它只做「算延迟 → 定时 → 发布」；`TaskWorker` 不关心任务为什么该跑，它只消费队列。两者之间隔着一条带缓冲的 channel，于是「发现任务的速度」与「消化任务的速度」不必匹配。

`SyncJobExecutor` 又是另一层并发：一个同步任务内部要处理成百上千个文件，这些文件操作是批作业。执行器为每个任务维护一条有界队列，再用一个公平轮转的分派器把这些队列喂给固定数量的 worker。所以**任务间并行**（TaskWorker 数）与**任务内并行**（JobWorker 数）是两个独立的旋钮。

## 怎么实现的

**`Syncer` 的字段就是这四层的句柄。** `queue`、`scheduler`、`worker`、`executor` 四个字段，加上 `cancel`、`workerGroup`、`stopOnce`、`ShutdownCh` 四个生命周期句柄。它没有任何业务状态——没有任务进度、没有游标、没有缓存。

**构造有两条路。** `NewSyncer(taskWorkerCount)` 是「生产用」的便捷构造：它自己创建 `SyncTaskDAO`、`Registry`、`DocumentService`、`SyncPruneService`，然后转调 `New(...)`。`New(config, taskDAO, registry, sink, pruneService)` 是「可测试」的完整构造：所有依赖显式传入。两条路的区别只在依赖从哪来，装配逻辑只有一份。

**构造期有两处「按能力探测替换实现」。** 这是本页最值得注意的设计：

- 检查点存储：默认用 `SyncCheckpointStore(newMemorySyncCheckpointStore())`（进程内存），然后 `engine.GetMessageQueueEngine()` 的类型断言——若消息队列引擎恰好实现了 `SyncCheckpointStore` 接口，就换用它。于是「检查点存在哪」由部署时选的消息队列决定：跑 NATS 就落到消息队列，跑了不支持该接口的引擎就退回内存。
- 调度器：默认 `NewScheduler(queue, taskDAO)`，若消息队列实现了 `SyncTaskBroker` 就换成 `NewNATSScheduler(queue, taskDAO, broker)`。

两处用的是同一个手法：**默认实现 + 接口断言升级**。这使 `internal/syncer` 不硬依赖任何具体中间件，但也意味着「实际用的是哪个实现」只在运行期才能确定。

**调度器是 NATS 单路径的。** `Scheduler.Run` 里只有一句分支：`broker != nil` 就走 `runNATS`，否则返回 `errors.New("syncer scheduler requires a NATS broker")`。`runNATS` 做四件事：初始化流、初始化消费者、订阅任务、然后**扫一遍数据库做启动对账**（`publishStartupTasks`）。最后阻塞在 `<-ctx.Done()`，退出时清掉所有定时器。

`NewScheduler`（非 NATS 版）仍然存在且被 `Syncer.New` 用作默认值，但它的 `Run` 必然返回错误。这是一个**保留但不可用的构造路径**——读代码时容易误以为有两条调度路径。

**启动对账是分页的。** `publishStartupTasks` 先 `taskDAO.RecoverRunning(ctx)`（把上次崩溃时残留的「运行中」任务复位），然后按 `scheduledTaskStartupPageSize`（4096）一页页 `ListScheduledTasks`，用游标推进直到取空或不足一页。逐条 `ScheduleTask`，单条失败只打 warning 并**延后 3 秒重投**（`ScheduleTaskAfter`），不中断整轮扫描。这个容错方向是对的：一条坏任务不该阻塞其余任务的调度。

**延迟计算分三种情况，且失败方向是「不调度」。** `taskDelay(task, now)`：

1. 按任务类型取频率：`TaskTypeSync` 用 `ConnectorRefreshFreq`；`TaskTypePrune` 用 `ConnectorPruneFreq`，但如果连接器配置里 `sync_deleted_files` 为假就**直接返回不调度**——清理任务只在开了「同步已删文件」时才有意义。
2. `UpdateDate == nil`（从未跑过）→ 延迟 0，立即跑。
3. 频率小于等于 0 → 打 warning 并**不调度**。注释写明了理由：非正频率如果照旧发布，会以零间隔重复投递，把同步 worker、NATS 和上游数据源一起打成热循环。这是一个典型的「坏配置宁可不动」选择，代价是该任务从此静默不再运行。

**定时器用「指针身份」防串档。** `ScheduleTaskAfter` 在延迟大于 0 时加锁调 `scheduleTimerLocked`：先 `Stop()` 掉同 taskID 的旧定时器，再 `time.AfterFunc` 注册新的，把 `*time.Timer` 存进 `timers[taskID]`。注意闭包里传的是 `&timer`（**指向指针的指针**），这样回调能拿到「注册时那个定时器」的身份。

回调用 `handleTimerWakeup`：发布成功就 `retireTimer`；发布失败则 `retryTimerIfCurrent`。两个函数都做同一件事——在锁内比较 `s.timers[taskID] != *timer`，不等就直接返回。区别是 `retireTimer` 只删自己那条，`retryTimerIfCurrent` 只在自己仍是当前定时器时才重排 3 秒。注释点明：**过时回调的失败不能覆盖更新的排期**。这是「同一个 key 反复重排」场景下的经典竞态，用身份比较解决。

`stopTimers` 在退出时遍历全部定时器 `Stop()` 并清空 map。

**`TaskEnvelope` 是队里唯一的东西。** 三个字段：`TaskID`（业务键）、`Handle`（消息队列的投递句柄，含 Ack/Nack 能力）、`stopHeartbeat`（停止心跳的函数）。注意 `stopHeartbeat` 是小写字段——它只能在包内被读取，因为「什么时候停心跳」是调度器与 worker 之间的内部约定，不该暴露。

入队路径 `enqueueHandle` 先把 `message.TaskID` 取出：空 ID 直接 `Ack` 丢弃（一条无内容的消息没有处理价值）；否则先启动心跳，然后 `select` 在「ctx 取消」与「写入队列」之间二选一——取消时停心跳并 `Nack`，把消息退回队列交给别人。**心跳的启动早于入队**，这保证了「消息在队列里等的时候也不会被认为超时」。

**`SyncJobExecutor` 的公平轮转。** 它是任务内并行的核心，三组 channel：

- `commands`（无缓冲）：注册/注销任务的命令通道，由分派器串行处理，因此 `tasks` map 与 `order` 切片**不需要加锁**——所有修改都发生在分派器 goroutine 里。
- `jobs`（有缓冲，`JobQueueSize`）：真正的作业通道，被 `WorkerCount` 个 worker 消费。
- `wakeup`（缓冲 1）：从 submit 侧唤醒分派器的信号。缓冲 1 意味着并发提交会合并成一个 token，分派器醒来后会重扫所有任务队列，所以丢 token 无害。

分派器 `dispatch` 的主循环是：`pending == nil` 时 `popReadyJob` 尝试取一个作业；取不到就在 `stop` / `commands` / `wakeup` 三者间阻塞等待；取到了则在 `stop` / `commands` / `jobs <- pending` 三者间等待。这样「投递作业」与「处理注册命令」不会互相饿死。

`popReadyJob` 是公平性的实现：从游标位置开始环形扫描 `order`，对每个任务做**非阻塞取**（`select` + `default`），第一个有货的就返回，并把游标推到这个任务的**下一位**。于是轮转是按「任务」而非按「作业」计数的——一个任务即使有一万个待办，也只占一轮一个名额。

注册与注销都通过 `applyExecutorCommand` 走同一通道。注销时 `unregisterExecutorTask` 要把 taskID 从 `order` 里删掉，并修正游标：空则归零，越界也归零。若不修正，删元素后游标可能指向不存在的下标。

**关闭路径是「把所有排队的作业都判失败」。** `Close` 用 `stopOnce` 保证只执行一次：`close(e.stop)` → 等 `<-e.done`（分派器退出）→ `workerGroup.Wait()`。分派器在收到 stop 时调 `settleQueuedJobs`，把手上那个 `pending` 与每个任务队列里剩余的所有作业都塞一个 `errSyncJobExecutorClosed`。**这些作业的调用方不会永远阻塞**——每个作业都带一个缓冲 1 的 `done` channel，所以塞入不会卡住。

**组件配置的归一。** `SyncJobExecutorConfig.normalize` 把三个尺寸都修正成正值：`WorkerCount <= 0` 归 1，`JobQueueSize <= 0` 归 worker 数，`PerTaskQueueSize <= 0` 归 `JobQueueSize`。注释写的是「防止 channel 在设为 0 时失效」——无缓冲 channel 会让提交与消费严格配对，行为与「有缓冲」不同。

**`Syncer` 的启停。** `StartContext` 起两个 goroutine：调度器 `Run`，worker `Run(runCtx, TaskWorkerCount)`，用 `workerGroup.Add(2)` 计数。调度器返回错误时只在非 `context.Canceled` 时记日志——正常停止不算错误。`Stop` 用 `stopOnce`：取消防抖 → `workerGroup.Wait()` → `executor.Close()` → `close(ShutdownCh)`。顺序有讲究：先停生产者（调度器）与消费者（worker），最后才关执行器，否则 worker 可能还在用执行器提交作业。

## 关键取舍

**源码事实：** `Syncer.New` 用「默认实现 + 接口断言」两处探测消息队列引擎的能力，检查点存储与调度器都可能被替换成 NATS 版本。`internal/syncer` 因此不硬依赖具体中间件。

**源码事实：** `Scheduler.Run` 只有 NATS 一条可用路径，`broker` 为空时直接返回错误。`NewScheduler` 保留但不可用。

**源码事实：** 非正频率的任务被判定为「不调度」并只记 warning，策略是 fail-closed——宁可不再运行，也不以零间隔热循环。

**源码事实：** 定时器回调通过「指向指针的指针」比较身份，过时回调的失败不会覆盖更新的排期；`retireTimer` 只删自己那条。

**源码事实：** 作业执行器的 `tasks`/`order`/`cursor` 只被分派器 goroutine 触碰，因此完全无锁；注册与注销靠 `commands` 通道串行化。

**源码事实：** 公平性按任务轮转而非按作业（环形扫描 + 游标推进 + 非阻塞取），一个任务积压再多也只占一轮一个名额。

**架构推论：** 「调度—队列—worker」三段分离让系统能以不同速率运行：调度器可以因为数据库慢而滞后，worker 可以因为上游 API 限速而变慢，队列吸收两者的差值。代价是队列满时调度器会阻塞（`enqueueHandle` 的 `select` 没有 `default` 分支），进而在 NATS 侧表现为消息堆积而非丢消息——这是正确的方向，但也意味着**队列尺寸是背压的唯一旋钮**。

**架构推论：** 心跳在入队前启动、取消时停止，说明「任务的存活判定」由消息队列侧负责，而不是由 worker 负责。于是「worker 挂了」与「任务还在排队」在消息队列看来是两件不同的事：前者心跳停、消息重投，后者心跳继续。这依赖于 `stopHeartbeat` 一定被调用——它在成功入队后交给 worker，在取消时由调度器自己调。

**架构推论：** 执行器的关闭语义是「排队作业立刻失败」而不是「跑完再退」。这让关闭是快的、可预测的，但调用方必须能正确处理 `errSyncJobExecutorClosed`——否则会把它当业务错误重试。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `internal/syncer/syncer.go` | `Syncer` | 同步服务根对象：四层句柄与生命周期 |
| `internal/syncer/syncer.go` | `NewSyncer` | 生产用构造：自建默认依赖后转调 `New` |
| `internal/syncer/syncer.go` | `New` | 完整构造：显式依赖 + 两处能力探测替换 |
| `internal/syncer/syncer.go` | `Syncer.StartContext` | 起调度器与 worker 两个 goroutine |
| `internal/syncer/syncer.go` | `Syncer.Stop` | 先停生产消费、后关执行器的顺序收尾 |
| `internal/syncer/scheduler.go` | `Scheduler` | 发现到期任务并投递/定时 |
| `internal/syncer/scheduler.go` | `NewScheduler` | 非 NATS 构造（`Run` 会报错） |
| `internal/syncer/scheduler.go` | `TaskEnvelope` | 队列唯一载荷：任务 ID + 投递句柄 + 停心跳 |
| `internal/syncer/scheduler.go` | `Scheduler.taskDelay` | 按类型取频率，非正频率 fail-closed |
| `internal/syncer/scheduler.go` | `Scheduler.retryTimerIfCurrent` | 定时器身份比较，防过时回调覆盖新排期 |
| `internal/syncer/scheduler.go` | `Scheduler.publishStartupTasks` | 崩溃复位 + 分页扫描对账 |
| `internal/syncer/job_executor.go` | `SyncJobExecutor` | 任务内批作业的公平分派 |
| `internal/syncer/job_executor.go` | `NewSyncJobExecutor` | 起分派器与固定数量 worker |
| `internal/syncer/job_executor.go` | `popReadyJob` | 环形扫描 + 游标推进的公平取作业 |
| `internal/syncer/job_executor.go` | `settleQueuedJobs` | 关闭时把所有排队作业判失败 |
| `internal/syncer/job_executor.go` | `SyncJobExecutorConfig.normalize` | 三个尺寸的兜底归一 |

## 参考

- [ragflow 源码仓库](https://github.com/infiniflow/ragflow)
- [ragflow 源码：syncer/scheduler.go](https://github.com/infiniflow/ragflow/blob/main/internal/syncer/scheduler.go)
- [NATS JetStream 文档：消费者与确认](https://docs.nats.io/nats-concepts/jetstream/consumers)
- [Go 文档：time.AfterFunc](https://pkg.go.dev/time#AfterFunc)

## 与主核 / AI 的关系

「调度—队列—worker—作业分派」四层与主核里「连接器—工作队列—执行器」的分层同构：数据库也要把「谁该被服务」与「谁来执行」分开，中间隔着队列吸收速率差。差别在于主核的背压是显式的（连接数上限、队列深度）、拒绝是新连接被拒；这里的背压表现为调度器阻塞，进而让消息队列端的堆积可见。

**fail-closed 的选择值得与主核对照。** 主核遇到非法配置会拒绝启动或拒绝该操作并报错；这里遇到非正频率只是不再调度并记 warning——任务静默失效。这在数据库里是不可接受的（静默不服务等于数据不一致），在同步服务里却可以接受（不跑就是不同步，下游能看到数据没更新）。**同一种「坏配置」在不同系统里的严重性取决于「不做事」是否会被观察到。**

定时器的「身份比较」与主核的 MVCC 版本校验是同一类手法：都假设**同一个 key 上可能有多个并发版本**，于是每次操作都先确认「我改的还是我以为的那份」。主核用版本号，这里用指针身份——后者更轻，但也更依赖「指针在整个生命周期内唯一」这个前提。

对 agent / RAG 工程的实际含义有三条。其一，`TaskWorkerCount` 与 `JobWorkerCount` 是两个独立旋钮：前者控制「同时同步几个数据源」，后者控制「一个数据源内部同时处理几个文件」。上游限流严重时该压后者；数据源很多但每个都不大时该提前者。其二，队列是唯一的背压点，队列太小会让调度器阻塞，队列太大会让内存里堆着大量待办。其三，非正频率配置会让任务永久静默，排查「某个数据源为什么不再同步」时第一个该看的就是频率字段。
