---
project: redis
feature: active-expire
page_type: feature
schema_version: 1
refs:
  - path: src/expire.c
    symbols: [activeExpireCycle]
---

# 主动过期扫描

## 一句话职责

Redis 用一套「按时间配额分批采样」的算法主动回收已过期的键：每次只处理有限个键、只花有限微秒，并把工作分摊到多个数据库上轮转推进，避免过期键堆积占内存。

## 机制是什么

`src/expire.c` 顶部注释写明了动机的起点：「When keys are accessed they are expired on-access. However we need a mechanism in order to ensure keys are eventually removed when expired even if no access is performed on them.」——读时过期（c01）只能保证「被访问到的过期键」被清掉，那些再也没人访问的键必须靠主动扫描回收。

算法是自适应的，注释的说法是「will use few CPU cycles if there are few expiring keys, otherwise it will get more aggressive」。它有两种形态：

- **快速周期**（`ACTIVE_EXPIRE_CYCLE_FAST`）：在每轮事件循环的睡前钩子里调用，预算是一个固定的微秒数，并且不会在同一个时间窗内重复。
- **慢速周期**（`ACTIVE_EXPIRE_CYCLE_SLOW`）：以 `server.hz` 的频率调用，预算是「一个 hz 周期内可用 CPU 时间的百分比」换算出的微秒数。这是回收过期键的主要途径。

慢速周期可能因为超时提前退出，正因如此才需要快速周期来兜底——注释写得很直接：「the function is also invoked to perform a fast cycle at every event loop cycle, in the beforeSleep() function. The fast cycle will try to perform less work, but will do it much more often.」

采样对象是过期字典 `db->expires`（只装带 TTL 的键），由游标 `db->expires_cursor` 驱动扫描。本页只讲这个扫描算法本身；过期时间怎么写进去在 c03，内存淘汰在 c05。

## 怎么实现的

**参数按 effort 缩放。** 函数开头把配置项 `active-expire-effort`（取值 1 到 10，默认 1）减一重标定到 0 到 9 的 `effort`，然后派生四个基线参数：

| 派生量 | 公式 | effort=0 时的值 |
|---|---|---|
| `config_keys_per_loop` | `ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP + ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP/4*effort` | 20 |
| `config_cycle_fast_duration` | `ACTIVE_EXPIRE_CYCLE_FAST_DURATION + ACTIVE_EXPIRE_CYCLE_FAST_DURATION/4*effort` | 1000 微秒 |
| `config_cycle_slow_time_perc` | `ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC + 2*effort` | 25 |
| `config_cycle_acceptable_stale` | `ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE - effort` | 10 |

四个基线常量定义在 `src/expire.c` 顶部：`ACTIVE_EXPIRE_CYCLE_KEYS_PER_LOOP` 为 20、`ACTIVE_EXPIRE_CYCLE_FAST_DURATION` 为 1000（微秒）、`ACTIVE_EXPIRE_CYCLE_SLOW_TIME_PERC` 为 25、`ACTIVE_EXPIRE_CYCLE_ACCEPTABLE_STALE` 为 10（百分比）。也就是说 effort 调大，每轮处理的键更多、快速周期预算更大、慢速周期能占用的 CPU 比例更高、可以容忍的「已过期但还没回收」比例更低。

**跨调用的静态状态。** 函数里有三个 `static` 变量承担增量推进：`current_db`（下一个要测试的库）、`timelimit_exit`（上一轮是否因超时退出）、`last_fast_cycle`（上一次快速周期的起始时刻）。注释写明「This function has some global state in order to continue the work incrementally across calls.」

**前置早退。** 若 `isPausedActionsWithUpdate(PAUSE_ACTION_EXPIRE)` 为真，直接返回——过期动作被暂停时不删任何键。注释解释：暂停结束时键会被正常过期，或者故障转移后新主节点会把这个过期时间发过来。

快速周期还有两条早退。第一条：若上一轮不是因超时退出（`!timelimit_exit`），且估计的过期键比例 `server.stat_expired_stale_perc * 100` 低于 `config_cycle_acceptable_stale`，就不开始——注释说是「除非估计的过期键比例太高」。第二条：`start < last_fast_cycle + config_cycle_fast_duration*2` 时不开始，注释说「never repeat a fast cycle for the same period as the fast cycle total duration itself」，即两次快速周期之间至少间隔两个快速周期预算。

**数据库轮转。** `dbs_per_call` 初值是 `CRON_DBS_PER_CALL`（`src/server.h` 里定义为 16）；若库数少于它，或者上一轮是因超时退出的（`timelimit_exit`），就把 `dbs_per_call` 改成 `server.dbnum`——注释给的理由是「上次撞到了时间上限，这一轮想把所有库都扫一遍，因为某些库里确实有活儿干，不能让过期键占内存太久」。

**时间预算。** 慢速周期的预算是 `timelimit = config_cycle_slow_time_perc * 1000000 / server.hz / 100`，注释写明「我们每轮最多能用 CPU 时间的这个百分比；由于这个函数每秒被调用 server.hz 次，下面就是我们在里面能花的最大微秒数」。算出后若不大于 0 就钳到 1。快速周期则直接用 `config_cycle_fast_duration` 作为微秒预算。

**主循环。** 外层是 `for (j = 0; dbs_performed < dbs_per_call && timelimit_exit == 0 && j < server.dbnum; j++)`，三个退出条件分别是：已检查够数据库数、超时、库已全部遍历。每轮：

1. 取 `db = server.db + (current_db % server.dbnum)`，然后**立刻** `current_db++`——注释说明这是为了「万一在当前库里用完时间，下次从下一个库重新开始，从而把时间均匀分摊到各库」。
2. 先调 `activeSubexpiresCycle(type)` 处理成员级过期（哈希字段与集合成员各自带 TTL 的情形）。注释解释顺序理由：「交错进行子过期与键过期，先处理键过期是因为 ebuckets 是为主动过期优化的」。
3. 只有 `kvstoreSize(db->expires)` 非零的库才计入 `dbs_performed`。

**内层采样循环。** 进入 `do { ... } while (repeat)`：

1. `num = kvstoreSize(db->expires)`；若为 0，把 `db->avg_ttl` 归零并跳出。
2. `data.now = mstime()` 取当前时间——注释指出扫描回调里要避免反复调 `gettimeofday` 系统调用。
3. 若 `num` 大于 `config_keys_per_loop` 就截断到它，即每轮最多采样这么多键。
4. 桶预算 `max_buckets = num * 20`。注释解释了为什么需要桶预算：「哈希表里某些位置可能是空的，所以我们还要一个关于扫描了多少个桶的停止条件。不过扫描空闲桶非常快——我们在缓存行里顺序扫一段 NULL 指针数组，所以能比扫键快得多。」
5. 扫描循环 `while (data.sampled < num && checked_buckets < max_buckets)`：每次调 `kvstoreScan(db->expires, db->expires_cursor, -1, expireScanCallback, expirySamplingShouldSkipDict, &data)`，游标存回 `db->expires_cursor`；游标归零说明这个库扫完了，置 `db_done = 1` 并跳出。
6. `repeat` 的判定：`db_done ? 0 : (data.sampled == 0 || (data.expired * 100 / data.sampled) > config_cycle_acceptable_stale)`——扫完了就不再重复；一个键都没采到说明这个库没有带 TTL 的键，也不再重复；只有「采到的键里过期比例超过可容忍阈值」才继续扫这个库。

**时间检查的节奏。** 时间上限每 16 轮才检查一次：`if ((iteration & 0xf) == 0 || !repeat)`。注释说这样每 16 轮更新一次平均 TTL 统计，并在将要退出时也更新一次；其中 `if ((iteration & 0xf) == 0)` 分支里比较 `elapsed = ustime() - start` 与 `timelimit`，超时则置 `timelimit_exit = 1` 并让 `server.stat_expired_time_cap_reached_count++`。注释解释「我们不能在这里永远阻塞，哪怕有很多键要过期；所以花掉一定量微秒后就返回调用方，等另一个主动过期周期再来」。

**平均 TTL 的滑动估计。** `db->avg_ttl` 用扫描中未过期键的 TTL 采样求平均，然后做指数平滑。源码注释保留了原始实现（`for (int i = 0; i < update_avg_ttl_times; i++) db->avg_ttl = (db->avg_ttl/50)*49 + (avg_ttl/50);`）并给出了等价的几何级数推导，最终化成 `db->avg_ttl = avg_ttl + (db->avg_ttl - avg_ttl) * avg_ttl_factor[update_avg_ttl_times - 1]`。`avg_ttl_factor` 是 `src/expire.c` 顶部的一张 16 项常量表，注释写明是「从 pow(0.98,1) 到 pow(0.98,16)」，用来避免每次都算一次幂。

**收尾统计。** 循环结束后累加 `server.stat_expire_cycle_time_used`，并用 `latencyAddSampleIfNeeded("expire-cycle", elapsed/1000)` 记一条延迟样本。最后更新「已逻辑过期但仍在库里」的估计比例：`server.stat_expired_stale_perc = current_perc*0.05 + (server.stat_expired_stale_perc*0.95)`——注释写的是「用这个样本按 5% 计入的滑动平均」。

**扫描回调。** `expireScanCallback` 对每个条目取 `ttl = kvobjGetExpire(kv) - data->now`，调 `activeExpireCycleTryExpire` 尝试过期；成功则 `expired++`；若 `ttl > 0`（还没到期）则累加 `ttl_sum` 与 `ttl_samples` 用于平均 TTL 估计；最后 `sampled++`。`activeExpireCycleTryExpire` 的动作是：`now < kvobjGetExpire(kv)` 就直接返回 0；否则 `enterExecutionUnit(1, 0)` → 造一个键对象 → `deleteExpiredKeyAndPropagate(db, keyobj)` → `server.stat_expiredkeys_active++` → `decrRefCount` → `exitExecutionUnit()` → `postExecutionUnitOperations()`（注释写明这一步是「传播 DEL 命令」）。

**跳过不该采样的字典。** `expirySamplingShouldSkipDict` 有两个跳过条件：一是桶填充率低于 1%（`buckets > DICT_HT_INITIAL_SIZE && numkeys * 100 / buckets < 1`），注释说「填充率低于 1% 时采样键空间太贵，先停在这里等更好的时机；字典很快就会扩容」；二是槽位迁移导入中的槽（`clusterCanAccessKeysInSlot` 为假），那些键处于中间态，不能过期。

**真实调用方。** `src/server.c` 的 `databasesCron` 在 `server.active_expire_enabled` 且 `iAmMaster()` 时调 `activeExpireCycle(ACTIVE_EXPIRE_CYCLE_SLOW)`；不是主节点时改走 `expireSlaveKeys()`——副本不主动过期，等主节点合成 DEL。`src/server.c` 的 `beforeSleep` 在 `server.active_expire_enabled && iAmMaster()` 时调 `activeExpireCycle(ACTIVE_EXPIRE_CYCLE_FAST)`。`server.active_expire_enabled` 不是普通配置项，`src/server.h` 的注释说它是「Can be disabled for testing purposes」，运行时只有 `src/debug.c` 的 `DEBUG SET-ACTIVE-EXPIRE <0|1>` 能改。

**退化路径。** 三条：过期动作被暂停（`PAUSE_ACTION_EXPIRE`）时整个函数直接返回，键留在库里等暂停结束后处理；副本上根本不会进入这条路径（`databasesCron` 分流到 `expireSlaveKeys`）；快速周期在两条早退条件命中时直接返回，等下一次事件循环。慢速周期一旦因超时退出，`timelimit_exit` 会被置位，此时下一次调用会把 `dbs_per_call` 扩到全部库、并且快速周期在过期比例不高时会被抑制——注释把这条写成了「慢速周期退出是因为用时太久时，快速周期也拒绝运行」。

## 关键取舍

**源码事实：** 扫描预算由三个可配参数共同决定：`active-expire-effort`（1 到 10，默认 1）缩放每轮键数、快速周期时长、慢速周期 CPU 百分比与可容忍过期比例。**源码事实：** 库之间轮转推进，命中超时那轮会把库数上限扩到全部库。**源码事实：** 时间上限每 16 轮检查一次，是精度与开销的折中。**源码事实：** 桶预算 `num*20` 是为稀疏哈希表设的护栏。**源码事实：** 填充率低于 1% 的字典被整体跳过。**源码事实：** 过期比例的滑动估计用 5% 权重。**源码事实：** 副本不参与主动过期。

**架构推论：** 用「时间预算 + 固定键数上限」而不是「一次扫完」是把过期回收做成协作式后台工作的核心：Redis 的单线程主循环既要跑命令又要回收过期键，只有把每次回收的份额钉死，才能保证延迟敏感的命令不被回收拖住。代价是过期键的回收有滞后——最坏情况下一个键过期后要等若干轮扫描才会被真正删掉，期间它仍占内存。主核是常驻的多写者引擎，若把回收做成后台线程，就不再需要时间预算这种自我限流，但也需要额外处理「回收线程与前台事务争同一份数据」的问题。**架构推论：** 库轮转加游标持久化（`db->expires_cursor` 存回库里）是为了避免每次都从同一处开始扫描、造成部分键长期得不到回收；这个设计隐含了「扫描进度可以跨调用保留」的假设，在有多个扫描者（多线程回收）时需要额外的进度协调。**架构推论：** 「填充率低就跳过整张表」是把内存布局当作扫描成本信号——Redis 的字典扩容有独立策略（e01），所以这里可以放心等它扩容；主核若用页式存储，稀疏表的扫描成本特征完全不同，不能照搬这个跳过条件。**架构推论：** 把「过期比例」做成滑动平均并用它反过来抑制快速周期，是一个自调节的负反馈：过期压力小就少干活、压力大就多干活；主核做类似回收时若没有这个反馈信号，容易出现要么回收不足、要么回收过度抢资源的两极。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/expire.c` | `activeExpireCycle` | 主动过期扫描的主体：effort 缩放参数、快慢周期分流、库轮转、采样与时间预算控制 |

## 参考

- [Redis 源码：expire.c](https://github.com/redis/redis/blob/unstable/src/expire.c)
- [Redis 文档：键空间与过期](https://redis.io/docs/latest/develop/use/keyspace/)
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/)
- [Redis 命令：DEBUG](https://redis.io/docs/latest/commands/debug/)

## 与主核 / AI 的关系

「时间配额 + 固定批量 + 跨调用轮转」是一套可直接移植的回收调度骨架，主核的多模态存储做向量段或 embedding 行的过期回收时可以照搬：每次回收有明确的毫秒预算与条数上限，扫描进度跨调用保留并轮转分摊到多个分区。

但 Redis 的这套回收跑在单线程主循环里，与命令争同一份时间片；主核是多写者引擎，把回收放到独立后台线程后就不必再用「CPU 百分比」这样的间接预算，但必须补上 Redis 免费获得的东西：回收线程删除一条记录时，如何与并发事务的可见性判定协调。Redis 的做法是删除动作走与命令同一套传播机制（`deleteExpiredKeyAndPropagate` → `propagateDeletion`），主核需要在自己的 WAL 与快照里表达等价的语义。

AI 侧如果用 Redis 存向量并依赖 TTL 回收，主动过期的滞后就是召回结果里「已过期但仍在库里」的窗口：这段时间内按 TTL 过滤仍能挡掉它，但按内存占用估算容量时会高估。调高 `active-expire-effort` 能缩短这个窗口，代价是每个 hz 周期里回收占用的 CPU 份额上升——而这恰恰是 AI 推理服务里最贵的资源，需要和向量检索的延迟预算一起权衡。
