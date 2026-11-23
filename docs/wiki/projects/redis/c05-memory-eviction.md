---
project: redis
feature: memory-eviction
page_type: feature
schema_version: 1
refs:
  - path: src/evict.c
    symbols: [performEvictions]
---

# 内存淘汰

## 一句话职责

当进程用内存超过 `maxmemory` 时，Redis 按配置的淘汰策略挑出「最该被回收」的键并删掉，回收过程有采样近似、有时间预算、有跨调用的续跑机制，目标是先把内存拉回限制内而不是一次性解决。

## 机制是什么

`src/evict.c` 的 `performEvictions` 是这条链路的唯一入口。函数的文档注释把定位写得很清楚：「Check that memory usage is within the current "maxmemory" limit. If over "maxmemory", attempt to free memory by evicting data (if it's safe to do so).」

注释还点明了它**不是**一次性的修复：「The goal of this function is to improve the memory situation - not to immediately resolve it.」——如果一轮下来还没降到限制内，就启动一个 ae 时间事件继续淘汰，直到达标或无可淘汰。

返回值有三个：`EVICT_OK`（内存没问题，或现在不适合淘汰）、`EVICT_RUNNING`（仍然超限，但淘汰还在进行中）、`EVICT_FAIL`（超限且无可淘汰物）。

策略上，Redis 不维护精确的全局 LRU 链表，而是用**采样近似**：注释说「Redis uses an approximation of the LRU algorithm that runs in constant memory. Every time there is a key to expire, we sample N keys (with N very small, usually in around 5) to populate a pool of best keys to evict of M keys」。候选池大小是常量 `EVPOOL_SIZE`（源码里定义为 16），采样数量是配置项 `maxmemory-samples`（默认 5，可配范围 1 到 64）。

本页只讲键级淘汰流程。客户端级驱逐在 b06，过期扫描在 c04，内存分配器本身在 e07。

## 怎么实现的

**先判断能不能淘汰。** `isSafeToPerformEvictions` 有四个否决条件：正在执行超时的长命令（`isInsideYieldingLongCommand()`）或正在载入数据（`server.loading`）时返回 0；本实例是副本且配置了忽略 maxmemory（`server.repl_slave_ro` 路径上的 `repl_slave_ignore_maxmemory`）时返回 0——注释说「默认副本应该忽略 maxmemory，只是做主节点的精确镜像」；当前客户端带 `CLIENT_MASTER` 与 `CLIENT_ASM_IMPORTING`（槽位迁移的数据导入）时返回 0，注释解释这是为了避免迁移因淘汰失败而出错；`PAUSE_ACTION_EVICT` 动作被暂停时返回 0。注意返回 0 时 `performEvictions` 直接给出 `EVICT_OK`——注释写明「we don't goto update_metrics here because this check skips eviction as if it wasn't triggered. it's a fake EVICT_OK」。

**算还差多少内存。** `getMaxmemoryState` 报的 `mem_reported` 是 `zmalloc_used_memory()`；随后减掉 `freeMemoryGetNotCountedMemory()` 得到 `mem_used`。被减掉的部分是 AOF 缓冲、超过 backlog 部分的复制缓冲、以及槽位迁移客户端的输出缓冲。`freeMemoryGetNotCountedMemory` 的注释解释了为什么不能把它们算进已用内存：「the eviction should use mostly data size, because it can cause feedback-loop when we push DELs into them, putting more and more DELs will make them bigger, if we count them, we need to evict more keys, and then generate more DELs, maybe cause massive eviction loop, even all keys are evicted.」需要释放的量是 `mem_tofree = mem_used - server.maxmemory`。

**策略不匹配就失败。** 策略是 `MAXMEMORY_NO_EVICTION` 时直接给 `EVICT_FAIL`——注释说「We need to free memory, but policy forbids」。

**时间预算。** `evictionTimeLimitUs` 把配置项 `maxmemory-eviction-tenacity`（0 到 100，默认 10）换算成微秒上限：不大于 10 时是线性映射 `50 * tenacity`（注释写「A linear progression from 0..500us」）；大于 10 且小于 100 时是几何映射 `500.0 * pow(1.15, tenacity - 10.0)`，注释说「A 15% geometric progression, resulting in a limit of ~2 min at tenacity==99」；等于 100 时返回 `ULONG_MAX`，即不限制淘汰时长。

**采样与候选池。** 对 LRU/LFU/LRM 策略与 `MAXMEMORY_VOLATILE_TTL` 策略，候选来自候选池 `EvictionPoolLRU`。池是跨调用持久的，注释说明它是「across performEvictions() calls」保存的，用来提升 LRU 近似的质量。池的填充由 `evictionPoolPopulate` 完成，它从随机选出的槽里取 `maxmemory_samples` 个键，按策略算一个「空闲分」并按升序插入池中：

- LRU/LRM 策略用 `estimateObjectIdleTime`——按 LRU 时钟估算对象多久没被访问；`LRU_CLOCK_RESOLUTION` 是 1000 毫秒。
- LFU 策略用 `255 - LFUDecrAndReturn(kv)`，注释解释「we want to evict keys with lower frequency first. So inside the pool we put objects using the inverted frequency subtracting the actual frequency to the maximum frequency of 255」。
- `MAXMEMORY_VOLATILE_TTL` 策略用 `ULLONG_MAX - kvobjGetExpire(kv)`——注释说「the sooner the expire the better」。
- 分数越高越适合淘汰；池按升序排列，所以最右端是最好的候选。
- 带 NO-EVICT 标记的键（`blessIsNoEvict`）被直接跳过，不计入候选。

采样要遍历**所有数据库**，注释给的理由是「We don't want to make local-db choices when expiring keys, so to start populate the eviction pool sampling keys from every DB.」。每库的采样预算有两个停止条件：本库采到的可淘汰键数达到 `maxmemory_samples` 就停；本库键数小于 `maxmemory_samples*10` 也停——注释说「If there are not a lot of keys in the current db, dict/s may be very sparsely populated, exit the loop without meeting the sampling requirement.」

随机策略（`MAXMEMORY_ALLKEYS_RANDOM` 与 `MAXMEMORY_VOLATILE_RANDOM`）不走候选池，直接随机取键；注释说明它用静态变量 `next_db` 轮流访问各个库。

**挑出受害者。** 从池尾（分数最高）往回找：`kvstoreDictFind` 确认这个键还在，命中就是受害者；没命中就是「ghost」（注释：「the pool may contain keys that no longer exist」），摘掉继续往下找。`performEvictions` 里还有一条针对 NO-EVICT 的补救：池是跨调用持久的，一个键可能在入池时还不是 NO-EVICT，之后被标记了——注释写明「The pool persists across performEvictions() calls, so a key pooled while unblessed may since have been marked NO-EVICT. The entry is already removed from the pool above; skip it as a target and try the next one.」

**全是 NO-EVICT 键时的处理。** 如果采样到的键全部带 NO-EVICT 标记，代码不会立刻放弃：它记一个 `blessed_only_rounds` 计数并重试，直到 `EVICTION_MAX_BLESSED_ONLY_ROUNDS`（源码定义为 8）轮才放弃。注释解释了为什么把检查挪到池扫描之后：「so we can tell two cases apart: 1. sampled nothing at all -> no keys to evict, stop here. 2. sampled keys but all were blessed -> retry, don't stop.」放弃时置 `blessed_blocked = 1`。

**删除受害者。** 动作包在 `enterExecutionUnit(1, 0)` 与 `exitExecutionUnit()` 之间：造一个键对象，调 `deleteEvictedKeyAndPropagate(db, keyobj, &key_mem_freed)`——这个调用同时删键、发 `evicted` 通知、传播 DEL/UNLINK、并返回这次释放的内存量——然后 `postExecutionUnitOperations()` 把传播发出去。`mem_freed` 累加，`keys_freed` 加一。

**每 16 个键检查一次状态。** `if (keys_freed % 16 == 0)` 里做三件事：有副本时 `flushSlavesOutputBuffers()`，注释说「we may start spending so much time here that is impossible to deliver data to the replicas fast enough」；若 `lazyfree-lazy-eviction` 打开（对象在别的线程里释放），就重新查一次内存状态，达标就退出——注释解释「the "mem_freed" amount is computed only across the dbAsyncDelete() call, while the thread can release the memory all the time」；最后若 `elapsedUs(evictionTimer) > eviction_time_limit_us` 就调 `startEvictionTimeProc()` 并跳出——注释说「If we suddenly need to free a lot of memory, don't want to spend too much time here.」

**跨调用续跑。** `startEvictionTimeProc` 用 `aeCreateTimeEvent(server.el, 0, evictionTimeProc, NULL, NULL)` 注册一个时间事件；`evictionTimeProc` 反复调 `performEvictions()`，返回 `EVICT_RUNNING` 就返回 0 表示继续，否则返回 `AE_NOMORE` 让时间事件自行删除。续跑时 `performEvictions` 返回的是 `EVICT_RUNNING` 而不是 `EVICT_OK`。

**失败后的等待。** 结果为 `EVICT_FAIL` 时，若 `bioPendingJobsOfType(BIO_LAZY_FREE)` 非空，说明有些对象正在后台线程里释放，代码会在预算内做短暂等待：每次 `usleep` 最多 1000 微秒，等待期间重新查内存状态，达标就把结果改成 `EVICT_OK`。

**NO-EVICT 造成的超限的宽容处理。** 若因为全是 NO-EVICT 键而失败（`blessed_blocked`），代码允许一定程度的超限：`mem_tofree <= server.maxmemory / EVICTION_OVERUSE_DIVISOR`（源码里 `EVICTION_OVERUSE_DIVISOR` 定义为 4，即 1.25 倍）时把结果改成 `EVICT_OK`。注释写明理由：「the blessing is likely why - failing the write with OOM would punish the user for a limit they deliberately raised. Tolerate a bounded overshoot: let the command run (EVICT_OK) while used memory stays within maxmemory*factor; we reclaim on later commands. Past the factor we still OOM (the hard ceiling).」

**统计与延迟样本。** 结束时若结果是 `EVICT_RUNNING` 或 `EVICT_FAIL` 就开始累计 `stat_last_eviction_exceeded_time`；若变成 `EVICT_OK` 就把它并入 `stat_total_eviction_exceeded_time` 并清零。整段淘汰耗时用 `latencyAddSampleIfNeeded("eviction-cycle", latency)` 记录，等待后台释放那段另记为 `"eviction-lazyfree"`。

**真实调用方。** `src/server.c` 的 `processCommand` 在执行命令前调 `performEvictions()`，条件是 `server.maxmemory` 非零且不在超时的长命令里；返回 `EVICT_FAIL` 时置 `out_of_memory`，随后会拒绝那些会增加内存占用的命令。`src/evict.c` 的 `evictionTimeProc` 是第二个调用方，负责续跑。另一个相关入口是 `src/server.c` 的 `dictResizeAllowed` 调 `overMaxmemoryAfterAlloc`——注释说明「we can stop dict to expand provisionally if used memory will be over maxmemory after dict expands, but to guarantee the performance of redis, we still allow dict to expand if dict load factor exceeds HASHTABLE_MAX_LOAD_FACTOR」。

## 关键取舍

**源码事实：** 淘汰是采样近似而非精确 LRU，候选池大小固定为 16，采样数由 `maxmemory-samples` 控制（默认 5）。**源码事实：** 已用内存要扣掉 AOF 缓冲、复制缓冲与迁移客户端输出缓冲，注释给的理由是避免「驱逐循环」。**源码事实：** 淘汰有单次时间预算，由 `maxmemory-eviction-tenacity` 映射而来； tenacity 为 100 时不设上限。**源码事实：** 淘汰删除是否异步由 `lazyfree-lazy-eviction` 决定（默认关闭）。**源码事实：** 每删 16 个键才检查一次时间与内存状态。**源码事实：** 副本默认忽略 maxmemory。**源码事实：** NO-EVICT 键可以造成最多 1.25 倍 maxmemory 的有界超限。**源码事实：** 淘汰结果通过延迟监控的 `eviction-cycle` 与 `eviction-lazyfree` 两个样本暴露。

**架构推论：** 用采样近似换常数内存，是因为精确 LRU 需要在每个键上维护链表指针，对海量小键来说指针开销本身就不可接受；代价是淘汰的对象不一定是理论上最该淘汰的那个，且在访问分布倾斜时会稳定地偏。**架构推论：** 扣掉复制缓冲与 AOF 缓冲是一个「反馈环切断」的设计——淘汰会产生 DEL，DEL 会进入复制流与 AOF，若把这两处内存算进已用量，淘汰量与缓冲增长会互相放大直到把数据全删光。主核若也做内存回收，回收动作本身的日志与通知开销同样需要从配额里排除，否则会出现同样的雪崩。**架构推论：** 「改善而非解决」+ 时间事件续跑，是把一个可能持续数秒的大额回收摊进事件循环，避免单条命令被长时间阻塞；代价是超限状态会持续一段时间，期间写命令可能被拒绝。主核是多写者引擎，可以把回收放到后台线程，但同样需要回答「超限期间读请求怎么办、拒绝策略是什么」这个问题。**架构推论：** NO-EVICT 的 1.25 倍宽容带，说明 Redis 承认「用户主动标记不可回收的键」与「硬性内存上限」存在冲突，选择用有界超限换取用户的标记不被惩罚；主核若提供类似「这张表不参与回收」的能力，必须一并定义超限时的行为，否则标记反而会让写请求被 OOM 拒绝。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/evict.c` | `performEvictions` | 键级淘汰的主体：安全判定、内存缺口计算、采样选候选、删除与传播、时间预算与跨调用续跑 |

## 参考

- [Redis 源码：evict.c](https://github.com/redis/redis/blob/unstable/src/evict.c)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)
- [Redis 文档：配置 maxmemory 与淘汰策略](https://redis.io/docs/latest/develop/reference/eviction/)
- [Redis 命令：CONFIG GET](https://redis.io/docs/latest/commands/config-get/)

## 与主核 / AI 的关系

Redis 这套淘汰是「命令执行前的同步检查 + 有预算的采样回收 + 事件循环续跑」的组合，主核做向量段的内存回收时，有两点值得直接借用：一是把回收动作自身的日志/通知开销从配额里排除（Redis 扣 AOF 与复制缓冲就是切断反馈环），二是回收超限期间要有明确的降级语义而不是让请求无限失败。

但 Redis 的候选池是「键」粒度的，池固定 16 项、采样默认 5 个键；主核的多模态对象是向量段，回收的最小单位是页或段，采样近似的误差会被放大——按段回收时，一次误判就可能释放或占用大量内存，因此主核更适合在段/页粒度上做精确的引用计数与空闲空间管理，而不是把 Redis 的采样近似照搬过来。

AI 侧若用 Redis 承担向量缓存的容量控制，`maxmemory` 淘汰会与向量 TTL 过期同时作用于同一批键，两者的顺序决定了「还没过期但被淘汰」和「已过期但没被回收」这两类偏差各自有多大。淘汰策略若设成 `volatile-ttl`，只有带 TTL 的键参与淘汰——这对纯缓存是合适的，但对混放了业务键与缓存键的实例是一次误伤。
