---
project: redis
feature: servercron-timers
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [serverCron]
---

# serverCron 定时任务

## 一句话职责

`serverCron` 是挂在事件循环上的定时回调，按 `server.hz` 的节奏集中驱动客户端维护、后台持久化、复制与集群等周期性工作。

## 机制是什么

`serverCron` 是 ae 库里的**时间事件**回调。服务端初始化时用 `aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL)` 注册它（初始延迟 1 毫秒）；回调的返回值决定下一次触发时间，`serverCron` 返回 `1000/server.hz` 毫秒，因此它以 `server.hz` 次/秒的频率运行。

节流分两层。第一层是频率本身：`server.hz` 初始取 `server.config_hz`（`hz` 配置项的默认值是 `CONFIG_DEFAULT_HZ`，即 10），必要时按客户端数上调。第二层是 `run_with_period(ms)` 宏——定义在 `src/server.h`，条件为 `(_ms_ <= 1000/_hz_) || !((_cronloops_) % ((_ms_)/(1000/_hz_)))`，其中计数用的是 `server.cronloops`，频率用的是 `server.hz`。也就是说：请求周期不大于一个 cron tick 时每轮都执行，否则每「请求周期 ÷ tick」轮执行一次。

本页只讲周期性工作的**调度点与节流方式**，不写过期扫描、内存淘汰这些算法本身。

## 怎么实现的

**频率自适应。** 每轮开头先把 `server.hz` 重置为 `server.config_hz`。若 `server.dynamic_hz` 打开（`dynamic-hz` 配置项默认开启），就进入循环：当 `listLength(server.clients) / server.hz` 大于 `MAX_CLIENTS_PER_CLOCK_TICK`（`src/server.h` 里定义为 200）时把 `server.hz` 翻倍；一旦翻到超过 `CONFIG_MAX_HZ`（同文件里定义为 500）就钳到 500 并退出循环。源码注释给的理由是：客户端多时希望 `serverCron` 跑得更频繁。

**调试与看门狗。** `server.pause_cron` 为真时直接返回 `1000/server.hz`，跳过全部实际工作——这个开关只由 `DEBUG pause-cron` 子命令设置（`src/debug.c`），注释写明是「for debug purposes」。若 `server.watchdog_period` 非零（`watchdog-period` 配置项默认 0，即关闭），则先调 `watchdogScheduleSignal` 安排 SIGALRM：若这一轮没能在该周期内返回，信号处理程序就会打断它。

**每轮固定动作**（按源码顺序）：

1. `run_with_period(100)`：`trackInstantaneousMetric` 更新命令数、网络流入流出字节、复制流量、事件循环次数与耗时等瞬时指标；`statsUpdateActiveClients` 清理活跃客户端滑动窗口里的陈旧槽位。
2. `server.lruclock = getLRUClock()` 推进 LRU 时钟。
3. `cronUpdateMemoryStats()` 刷新内存相关指标。
4. **关停处理**：`shouldShutdownAsap()` 且尚未进入关停流程时，按 `shutdown_on_sigint` / `shutdown_on_sigterm` 组装 `shutdownFlags` 调 `prepareForShutdown`，成功则 `exit(0)`；已进入关停流程时按 `server.shutdown_mstime` 到期或 `isReadyToShutdown()` 调 `finishShutdown`。源码注释说明为什么放在这里：不能在信号处理器里做这些事。
5. `run_with_period(5000)`：逐库打印键数与槽位数（仅当 `server.verbosity` 不高于 `LL_VERBOSE`）。
6. `run_with_period(5000)`：打印已连接客户端数（非哨兵模式）。
7. `clientsCron()`：客户端超时等定期维护。
8. `databasesCron()`：后台数据库维护入口，其中包含主节点上的慢速过期扫描（副本走 `expireSlaveKeys`）、`activeDefragCycle`、以及在无后台子进程时的增量扩容与 rehash。
9. `backupCron()`：备份相关维护。
10. 被推迟的 AOF rewrite：满足「无活跃子进程、无备份进行中、未被限流」且 `server.aof_rewrite_scheduled` 时调 `rewriteAppendOnlyFileBackground()`。
11. 有子进程（`hasActiveChildProcess()` 或 `ldbPendingChildren()`）时走 `run_with_period(1000)` 的 `receiveChildInfo()` 与 `checkChildrenDone()`；否则改为检查 `saveparams` 里的变更数与时间阈值以触发 bgsave，并按 AOF 相对基准文件的增长率触发 rewrite。
12. `updateDictResizePolicy()`。
13. AOF 推迟刷盘：`server.aof_flush_postponed_start` 置位时调 `flushAppendOnlyFile(0)`。
14. `run_with_period(1000)`：上次 AOF 写失败时重试 `flushAppendOnlyFile(0)`。
15. `updatePausedActions()` 清理已暂停动作的状态。
16. `replicationCron()`：处于故障转移中时用 `run_with_period(100)`，否则用 `run_with_period(1000)`。
17. `run_with_period(100)`：集群模式下 `clusterCron()` 与 `asmCron()`。
18. 哨兵模式下 `sentinelTimer()`。
19. `run_with_period(1000)`：`migrateCloseTimedoutSockets()` 清理超时的 MIGRATE 缓存连接。
20. `run_with_period(1000)`：`handleExpiredIdmpEntries()` 清理已跟踪流的过期条目。
21. `run_with_period(2000)`：`pendingCommandPoolCron()` 收缩命令复用池。
22. `hashTemplatesCron()` 维护哈希模板注册表。
23. 存在跟踪客户端时 `trackingLimitUsedSlots()` 调整跟踪键表大小。
24. 热点键追踪时长到期时自动停止。
25. `run_with_period(100)`：有模块时 `modulesCron()`。
26. 向模块广播 `REDISMODULE_EVENT_CRON_LOOP` 事件，携带 `RedisModuleCronLoopV1`（含当前 `server.hz`）。
27. `server.cronloops++`——`run_with_period` 的计数基准正是在这里推进。
28. `server.el_cron_duration = getMonotonicUs() - cron_start` 记录本轮 cron 耗时。
29. `return 1000/server.hz`，决定下一次到期时间。

**调度点。** 时间事件在 ae 的一轮里排在**所有文件事件回调之后**执行，因此 `serverCron` 看到的是本轮已经处理完命令后的状态。默认 `hz` 为 10（`CONFIG_DEFAULT_HZ`），tick 约 100 毫秒；按 `run_with_period` 的整数除法换算，`run_with_period(100)` 每轮都跑，`run_with_period(1000)` 每 10 轮跑一次（约 1 秒），`run_with_period(2000)` 每 20 轮跑一次（约 2 秒），`run_with_period(5000)` 每 50 轮跑一次（约 5 秒）。

**退化路径。** 阻塞期间事件循环不会走主循环的常规节奏：`src/networking.c` 的 `processEventsWhileBlocked` 调用 `aeProcessEvents` 时带的标记是 `AE_FILE_EVENTS | AE_DONT_WAIT | AE_CALL_BEFORE_SLEEP | AE_CALL_AFTER_SLEEP`，其中没有 `AE_TIME_EVENTS`，所以时间事件不会被处理，`serverCron` 也就不会运行。此时由 `src/server.c` 的 `whileBlockedCron` 顶替——它的注释写明「在 RDB 或 AOF 载入以及脚本阻塞期间充当 serverCron 的角色」，做法是按 `server.hz` 推进 `server.cronloops`（让 `run_with_period` 继续可用），并在载入期间执行 `defragWhileBlocked` 与 `cronUpdateMemoryStats`。也就是说，阻塞期间大部分周期工作是不执行的——`clientsCron`、`replicationCron` 这些都被跳过，只有少数几项有替代实现。

## 关键取舍

**源码事实：** `serverCron` 是单线程里的协作式定时任务，所有周期工作共享主线程的时间预算。**源码事实：** 频率来自 `server.hz`，`dynamic_hz` 打开时会随客户端数翻倍，上限 `CONFIG_MAX_HZ`。**源码事实：** `run_with_period` 用整数除法取模，节流精度受 tick 长度限制，真实周期只能向上取整到 tick 的倍数。**源码事实：** 阻塞期间 `serverCron` 不运行，替代者 `whileBlockedCron` 只覆盖少数几项工作。

**架构推论：** 用「一个回调 + 取模节流」而不是给每类任务各建一个定时器，可以避开 ae 定时器链表的 O(N) 查找开销——`ae.c` 的注释明确说定时器列表无序、查找最近到期项是 O(N)。**架构推论：** `hz` 随客户端数上调，是为了让超时类维护在连接数大时不被拖慢；代价是空转时 CPU 消耗随 `hz` 线性增长，也意味着所有周期任务的实际周期会随 `hz` 一起缩短。**架构推论：** 把关停流程放进 cron 而不是信号处理器，是为了让「安全落盘再退出」有完整的执行上下文；代价是关停要等下一个 tick。**架构推论：** 若改成独立后台线程跑周期任务，主线程就不再有串行保证，与主核的多写者引擎相比，这种「协作式、定时、限预算」的模式既易实现也更可预测。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `serverCron` | 周期性工作的统一入口：频率自适应、节流宏、看门狗、关停处理与各类维护调度 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c)
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/)
- [Redis 命令：INFO](https://redis.io/docs/latest/commands/info/)

## 与主核 / AI 的关系

「固定频率 + 取模节流」是可直接借鉴的调度思想，但主核是多写者引擎，把周期任务收进单线程回调意味着这些任务与前台事务争抢同一份时间片，主核更适合把它们放到独立后台线程并自带预算与取消。`server.hz` 这类频率旋钮也不能用来推断主核的超时语义——主核的事务超时与租约过期有各自的时钟源。AI 侧若用 Redis 承担向量缓存，`serverCron` 触发的过期与淘汰节奏会直接影响召回结果的时延可预测性，调小 `hz` 会同时抬高 CPU 占用与统计开销。
