---
project: redis
feature: beforesleep-cycle
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [beforeSleep, afterSleep]
---

# 每轮事件循环前后的固定动作

## 一句话职责

`beforeSleep` 与 `afterSleep` 是事件循环每轮等待前后的两个固定钩子，把「睡前必须做完的收尾」与「刚睡醒的记账」固化在每个循环周期里执行。

## 机制是什么

ae 层只提供两个回调槽位（`aeSetBeforeSleepProc` / `aeSetAfterSleepProc`），由 `aeProcessEvents` 在阻塞等待之前、之后各调一次；带 `AE_CALL_BEFORE_SLEEP` / `AE_CALL_AFTER_SLEEP` 标记才会触发。Redis 服务端在初始化时把这两个槽位注册成 `src/server.c` 里的 `beforeSleep` 与 `afterSleep`。

`beforeSleep` 负责的是「不做完就不能睡」的固定动作：把待发回复写出去、唤醒阻塞客户端、刷新持久化缓冲、把手上的客户端交接给 I/O 线程。`afterSleep` 负责的是「刚睡醒」的记账：取回模块 GIL、记录本轮起点时间、刷新缓存时间。本页不涉及定时任务本身（那是 `serverCron` 的事），也不涉及回复的协议编码细节。

## 怎么实现的

**触发点。** `beforeSleep` 在 `aeProcessEvents` 计算阻塞超时**之前**被调用，`afterSleep` 在多路复用调用返回后、文件事件回调**之前**被调用。源码注释写明这个函数「目前」只被两处调用：`aeMain`（服务端主循环）与 `processEventsWhileBlocked`（`src/networking.c`，RDB/AOF 载入与阻塞脚本期间处理客户端）。

**`beforeSleep` 的重入分支。** 函数开头判断 `ProcessingEventsWhileBlocked`：若为真，只执行一个低风险子集——`connTypeProcessPendingData`、`clientCompressionProcessPendingData`、AOF 打开时的 `flushAppendOnlyFile(0)`、`handleClientsWithPendingWrites`、`freeClientsInAsyncFreeQueue`、`processClientsOfAllIOThreads`、`sendPendingClientsToIOThreads`——然后立刻返回。注释解释：重入时不能做全部动作（例如不希望在载入期间做键过期），但至少要做上面这些。

**`beforeSleep` 的完整路径。** 源码注释给出的顺序约束比动作本身更重要：

1. `updatePeakMemory` 更新内存峰值。
2. `connTypeProcessPendingData` 与 `clientCompressionProcessPendingData`——注释写明「必须在 `flushAppendOnlyFile` 之前」处理连接层（典型是 TLS）与压缩层的待处理数据。
3. 计算 `dont_sleep`：连接层或压缩层还有待读数据时置位。
4. 集群模式下 `clusterBeforeSleep` 与 `clusterCommonBeforeSleep`——注释说明它可能改变集群状态（正常与失败之间切换），所以要在服务被解除阻塞的客户端之前做。
5. `blockedBeforeSleep`——注释写明必须在 `flushAppendOnlyFile` 之前，因为被解除阻塞的客户端可能写数据（`appendfsync=always` 时尤其重要）。它内部处理阻塞客户端超时、被 `WAIT`/`WAITAOF` 阻塞的客户端、按键阻塞的客户端与模块解除阻塞的客户端。
6. 快速过期循环：`server.active_expire_enabled` 且 `iAmMaster()` 时调用 `activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST)`，注释说明被调用方不需要时会立刻返回；`ACTIVE_EXPIRE_CYCLE_FAST_DURATION` 在 `src/expire.c` 里定义为 1000 微秒。
7. 模块事件 `REDISMODULE_EVENT_EVENTLOOP` / `REDISMODULE_SUBEVENT_EVENTLOOP_BEFORE_SLEEP`。
8. `sendGetackToReplicas`——上一轮有客户端因等待复制确认而被阻塞时，向副本索取 ACK。
9. `updateFailoverStatus`，并断言客户端侧缓存的待失效键列表与待推送消息列表都为空。
10. `trackingBroadcastInvalidationMessages` 广播失效消息。
11. `flushAppendOnlyFile(0)`——注释写明必须在 `handleClientsWithPendingWrites` 与 `sendPendingClientsToIOThreads` 之前执行，因为 `appendfsync=always` 时会同步落盘。
12. 更新已 fsync 的复制偏移；若偏移发生变化且有 `WAITAOF` 等待者，则把 `dont_sleep` 置位。
13. I/O 线程交接：`processClientsOfAllIOThreads` 返回非零说明线程正忙，`dont_sleep` 置位；否则先把 `server.running` 原子置 0（表示要睡了），再补调一次以免漏掉刚转移过来的客户端。
14. 统计本轮是否发生过客户端输入缓冲处理。
15. `handleClientsWithPendingWrites`——注释给的理由是：希望直接把回复写进客户端输出缓冲，不必先用一次系统调用去装可写事件、等它被触发再写。
16. `putReplicasInPendingClientsToIOThreads` 与 `sendPendingClientsToIOThreads`。
17. `freeClientsInAsyncFreeQueue` 释放标记为待关闭的客户端。
18. `incrementalTrimReplicationBacklog(10*REPL_BACKLOG_TRIM_BLOCKS_PER_CALL)` 增量裁剪复制积压缓冲，`REPL_BACKLOG_TRIM_BLOCKS_PER_CALL` 在 `src/server.h` 里定义为 64。
19. `evictClients` 驱逐占用内存过多的客户端。
20. 事件循环延迟采样：`durationAddSample` 记 `EL_DURATION_TYPE_EL` 与 `EL_DURATION_TYPE_CRON`，并调用 `offCpuCheckEventLoopCycle`——注释说明 AOF 写与 fsync 的耗时会从 off-CPU 判定里扣掉，免得磁盘等待被算成事件循环延迟，但它仍然计入 `EL_DURATION_TYPE_EL` 采样。
21. `aeSetDontWait(server.el, dont_sleep)`——把「下一轮要不要立刻返回不睡」的判断交回事件循环。
22. `moduleReleaseGIL` 释放模块全局锁；源码在此处有一段醒目警告注释：`moduleReleaseGIL` 之后不允许再添加任何代码。

**`afterSleep`。** 非重入时先取回模块 GIL（`moduleAcquireGIL`，并用延迟监控记 `module-acquire-GIL`），随后做两件事：记录事件循环起点 `server.el_start = getMonotonicUs()`，记录本轮命令计数起点 `server.el_cmd_cnt_start = server.stat_numcommands`——这两个正是 `beforeSleep` 计算事件循环耗时与本轮命令数的基准。I/O 线程模式下把 `server.running` 原子置 1。最后 `updateCachedTime(1)` 刷新 `server.ustime`/`server.mstime`/`server.unixtime` 缓存并更新夏令时标志（注释说明缓存时间是为了避免每次对象访问都调 `time()`），并把 `server.cmd_time_snapshot` 设为当前 `mstime`。

## 关键取舍

**源码事实：** `beforeSleep` 挂在「阻塞等待之前」而不是「事件回调之后」，因此这些收尾动作每轮只做一次，且一定发生在真正睡着之前。**源码事实：** `beforeSleep` 有一段重入分支，载入或阻塞期间只执行子集并提前返回。**源码事实：** `dont_sleep` 是贯穿整段的布尔量，连接层待处理数据、I/O 线程交接、`fsynced_reploff` 变化三处都会把它置位，最终由 `aeSetDontWait` 交给事件循环。**源码事实：** `moduleReleaseGIL` 之后有明确的禁止继续添加代码的警告注释。

**架构推论：** 把「回复写回」放进 `beforeSleep` 而不是等可写事件，可以省掉一次系统调用与一轮事件循环延迟；代价是 `beforeSleep` 的执行时间直接计入命令延迟。**架构推论：** 把模块 GIL 的释放与获取分别绑在「睡」与「醒」两个点上，主线程睡着期间模块线程才能访问数据集，省掉了额外的同步原语；代价是 `beforeSleep` 结尾成为禁区。**架构推论：** 源码注释里反复出现的顺序约束（连接层数据在 AOF 之前、阻塞客户端在 AOF 之前、AOF 在写回之前）本质上是同一条不变式——**持久化必须早于对客户端可见**；这条不变式一旦被重排就会破坏 `appendfsync=always` 与 `WAITAOF` 的语义。**架构推论：** 若把这些收尾动作改为事件回调内即时触发，每个命令都要重复做一遍，摊销成本反而更高。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `beforeSleep` | 睡前固定动作：唤醒阻塞客户端、刷 AOF、写回回复、交接 I/O 线程 |
| `src/server.c` | `afterSleep` | 睡后记账：取回模块 GIL、记录事件循环起点、刷新缓存时间 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c)
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/)
- [Redis 命令：INFO](https://redis.io/docs/latest/commands/info/)

## 与主核 / AI 的关系

「睡前刷、醒后记账」的结构可作为主核后台刷盘与事务可见性维护的参照，但 Redis 的顺序约束是为 AOF 落盘与回复时效服务的，主核必须按自己的事务边界重排，不能照抄次序。主核是多写者引擎，「每轮只做一次收尾」的前提在并发写入下不成立。AI 侧的回调通常远长于一条命令，主核需要更强的超时与取消机制，不能指望事件循环的轮次来兜底。
