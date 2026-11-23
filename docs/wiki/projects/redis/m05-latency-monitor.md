---
project: redis
feature: latency-monitor
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [rewriteClientCommandVector]
---

# 延迟监控

## 一句话职责

`latencyAddSample` 把「某个事件这一次花了多少毫秒」写进 `server.latency_events` 这张以事件名为键的时间序列字典，`latencyCommand` 再把序列按 HISTORY / LATEST / GRAPH / RESET / HISTOGRAM / DOCTOR 六种视角读出来；本条目 refs 指向的 `rewriteClientCommandVector` 承担另一件事——在命令传播到 AOF 与副本之前，把客户端命令行替换成可重新执行的等价命令向量。

## 机制是什么

延迟监控由三条互不依赖的统计线组成，阈值开关只作用于第一条：

**第一条：按事件分的定长时间序列。** 配置项 `latency-monitor-threshold`（单位毫秒）在 `src/config.c` 由 `createLongLongConfig` 注册，标志是 `MODIFIABLE_CONFIG`，取值范围 0..`LLONG_MAX`，默认值 0。`src/latency.h` 的三个宏 `latencyStartMonitor`、`latencyEndMonitor`、`latencyAddSampleIfNeeded` 第一句都是 `if (server.latency_monitor_threshold)`，所以默认值 0 等于把整条采样链短路掉：既不取时间戳，也不入账。

**每个事件 160 个槽。** `src/latency.h` 的 `LATENCY_TS_LEN` 是 160，`struct latencyTimeSeries` 由 `idx`、`max` 与 `samples[LATENCY_TS_LEN]` 三个成员构成，单个样本 `struct latencySample` 是 `int32_t time` 加 `uint32_t latency`。写入端对同一秒内的重复样本做合并，因此连续尖峰下至少保留 160 秒历史。

**第二条：每命令耗时直方图。** `struct redisCommand` 上的 `latency_histogram` 字段（`src/server.h`）由开关 `latency-tracking` 控制，该开关在 `src/config.c` 里由 `createBoolConfig` 注册、默认值 1，与 `latency-monitor-threshold` 完全无关。直方图记录范围来自 `src/server.h` 的 `LATENCY_HISTOGRAM_MIN_VALUE`（1，纳秒）与 `LATENCY_HISTOGRAM_MAX_VALUE`（1000000000，即 1 秒上限），精度由 `LATENCY_HISTOGRAM_PRECISION` 定为 2 位有效数字。

**第三条：事件循环累计桶。** `src/latency.h` 的 `DurationType` 枚举定义 `EL_DURATION_TYPE_EL`、`EL_DURATION_TYPE_CMD`、`EL_DURATION_TYPE_AOF`、`EL_DURATION_TYPE_CRON` 四个下标与哨兵 `EL_DURATION_TYPE_NUM`，数据落在 `server.duration_stats[]`，由 `durationAddSample` 累加。`durationAddSample` 自身没有阈值判断，但各调用点自带前置条件（`call` 里的 `EL_DURATION_TYPE_CMD` 样本要求 `update_command_stats` 为真且 `server.execution_nesting == 0`，`beforeSleep` 里的 `EL_DURATION_TYPE_EL` 样本要求 `server.el_start > 0`），这四个桶供 INFO 输出 `eventloop_*` 字段。

**边界的另一半是命令重写。** 本条目 refs 选中的是 `src/networking.c` 的 `rewriteClientCommandVector`：命令实现类代码在语义改写之后调用它，把 `c->argv` 换成新的对象向量，之后 `call()` 用这份新向量做传播登记。它属于传播链路的准备动作，与三条统计线正交。

## 怎么实现的

### 采样一对括号：三个宏

`latencyStartMonitor(var)` 在阈值非零时写 `var = mstime()`，阈值是 0 时写 `var = 0`；`latencyEndMonitor(var)` 在阈值非零时写 `var = mstime() - var`，阈值是 0 时什么都不做；`latencyAddSampleIfNeeded(event,var)` 要求「阈值非零」且 `(var) >= server.latency_monitor_threshold` 两个条件同时成立才调 `latencyAddSample`。比较用 `>=`，耗时恰好等于阈值的事件会被记录。

`src/latency.h` 还有一个 `latencyRemoveNestedEvent(event_var,nested_var)`，定义是 `event_var += nested_var`（注释写「从嵌套事件里减去时间」，实现却是相加）。全仓搜索这个宏只有它自己的定义一处，`src/` 下没有任何调用点——它是遗留的死宏。

**降级路径点名调用方：** 阈值被设成 0 时，`call()` 里那对 `latencyStartMonitor` / `latencyAddSampleIfNeeded` 就只剩一次赋值与一次条件判断，`redisFork`、`flushAppendOnlyFile`、`performEvictions` 等所有采样点同理，全部退化为「读一次 `server.latency_monitor_threshold` 然后跳过」。

### 入账：latencyAddSample

函数体按固定顺序做四件事：

1. `dictFetchValue(server.latency_events,event)` 取时间序列指针；为空就 `zmalloc(sizeof(*ts))`，把 `idx` 置 0、`max` 置 0、`memset` 清空 `samples`，再 `dictAdd(server.latency_events,zstrdup(event),ts)`——序列是按需建的，不预置固定事件列表，键由 `zstrdup` 复制一份。
2. `now = time(NULL)` 取秒级 Unix 时间。
3. `if (latency > ts->max) ts->max = latency;` 维护「自上次 RESET 以来的绝对最大值」。
4. 算上一个槽位下标 `prev = (ts->idx + LATENCY_TS_LEN - 1) % LATENCY_TS_LEN`；若 `samples[prev].time == now`，说明本次样本与上一次落在同一秒，此时只在 `latency > samples[prev].latency` 时把旧样本的 `latency` 抬到本次值，随后直接 `return`，不消耗新槽位；否则把 `now` 与 `latency` 写进 `samples[ts->idx]`，`idx` 自增，到 `LATENCY_TS_LEN` 时回绕成 0。

字典类型是 `latencyTimeSeriesDictType`：哈希用 `dictStringHash`（内部 `dictGenHashFunction(key, strlen(key))`），键比较用 `dictStringKeyCompare`（实现是 `strcmp(key1,key2) == 0`，区分大小写），键与值的析构函数都是 `dictVanillaFree`（定义在 `src/server.c`，实现是 `zfree(val)`）。所以删条目会连带释放时间序列本体，没有泄漏路径。

`latencyResetEvent` 用 `dictInitSafeIterator` 遍历同一个字典，靠 `strcasecmp(event,event_to_reset) == 0` 匹配，传 `NULL` 时匹配全部，逐个 `dictDelete` 并计数返回。匹配用 `strcasecmp` 而字典键比较用 `strcmp`，两者口径不同：`LATENCY RESET` 用大写事件名同样能删掉小写键。

### 初始化与归属

`latencyMonitorInit` 只有一行有效操作：`server.latency_events = dictCreate(&latencyTimeSeriesDictType)`。唯一调用点是 `src/server.c` 的 `initServer`，位置紧接在 `slowlogInit()` 之后。

### 读出：latencyCommand 的六个分支

分派顺序与参数个数条件都是固定的：

- `history` 且 `c->argc == 3`：`dictFetchValue` 查事件，查不到回空数组，查到就交给 `latencyCommandReplyWithSamples`；
- `graph` 且 `c->argc == 3`：`dictFind` 查条目，为空跳到函数尾部的 `nodataerr` 标签，回 `"No samples available for event '%s'"`；查到就用 `latencyCommandGenSparkeline` 生成文本再 `addReplyVerbatim(...,"txt")`；
- `latest` 且 `c->argc == 2`：`latencyCommandReplyWithLatestEvents`；
- `doctor` 且 `c->argc == 2`：调 `createLatencyReport` 并 `addReplyVerbatim`；
- `reset` 且 `c->argc >= 2`：无事件参数时 `latencyResetEvent(NULL)`，有则逐个 `latencyResetEvent(c->argv[j]->ptr)` 并累加，最后回整数；
- `histogram` 且 `c->argc >= 2`：无参数走 `latencyAllCommandsFillCDF(c, server.commands, &command_with_data)` 并用 `setDeferredMapLen` 回填充数量，有参数走 `latencySpecificCommandsFillCDF(c)`；
- `help` 且 `c->argc == 2`：回帮助文本；其余一律 `addReplySubcommandSyntaxError`。

`latencyCommandReplyWithSamples` 从 `(ts->idx + j) % LATENCY_TS_LEN` 起顺序读，跳过 `time == 0` 的空槽，因此输出是按写入游标排的最旧到最新。`latencyCommandReplyWithLatestEvents` 先用 `dictSize(server.latency_events)` 定数组长度，逐事件取 `last = (ts->idx + LATENCY_TS_LEN - 1) % LATENCY_TS_LEN`，回四元组「事件名、时间、该槽耗时、`ts->max`」。`latencyCommandGenSparkeline` 用 `LATENCY_GRAPH_COLS`（80）当画布列数，标签按距今秒数分档成 `%ds` / `%dm` / `%dh` / `%dd`，表头固定输出 `"%s - high %lu ms, low %lu ms (all time high %lu ms)"`，最后调 `sparklineRender(graph,seq,LATENCY_GRAPH_COLS,4,SPARKLINE_FILL)`。

### 统计：analyzeLatencyForEvent 与 createLatencyReport

`analyzeLatencyForEvent` 先按 `ts` 是否为空填 `all_time_high`（等于 `ts->max`），其余字段清零。第一遍扫 160 个槽，跳过 `time == 0`，边累加 `samples` 边维护 `min` / `max` / 求和，同时用 `ls->period == 0 || samples[j].time < ls->period` 记录最老样本时间戳；扫完后把和除成 `avg`，把最老时间戳换成「距今秒数」，结果为 0 时强制置 1。第二遍只算平均绝对偏差：`delta = (int64_t)ls->avg - samples[j].latency`，取绝对值后求和，最后 `mad = sum / samples`。

`createLatencyReport` 的入口守卫是 `dictSize(server.latency_events) == 0 && server.latency_monitor_threshold == 0`，命中就直接回「Latency monitoring is disabled in this Redis instance」并提示用 `CONFIG SET latency-monitor-threshold <milliseconds>`。通过守卫后逐事件输出「第几条、事件名、尖峰次数、均值、平均偏差、period/samples、`ts->max`」，再按事件名追加建议：

- `fork`：按 `server.stat_fork_rate` 分档，小于 10 记 `terrible`、小于 25 记 `poor`、小于 100 记 `good`、其余 `excellent`，前两档各自把 `advise_better_vm` 置 1；
- `command`：读 `server.slowlog_log_slower_than` 与 `server.slowlog_max_len`，前者小于 0 或后者为 0 时建议开启慢日志；`server.slowlog_log_slower_than/1000 > server.latency_monitor_threshold` 时建议重调慢日志阈值；
- `fast-command`：置 `advise_scheduler`（系统自身调度慢）；
- `aof-write-pending-fsync`、`aof-write-active-child`、`aof-write-alone`、`aof-fsync-always`、`aof-fstat`、`rdb-unlink-temp-file`、`aof-rewrite-diff-write`、`aof-rename`：各自置磁盘相关建议位；
- `expire-cycle`：置 `advise_hz` 与 `advise_large_objects`，前者在输出阶段还要求 `server.hz < 100` 才真正写进报告（建议 `CONFIG SET hz 100`）；
- `eviction-del`：置 `advise_large_objects`；
- `eviction-cycle`：置 `advise_mass_eviction`。

还有一条与事件无关的判断：`THPGetAnonHugePagesSize()` 大于 0（内部读 smap 的 `AnonHugePages:` 字段）时置 `advise_disable_thp` 并计数。收尾按三项组合：`eventnum == 0 && advices == 0` 回「没观测到尖峰」；`eventnum > 0 && advices == 0` 回「有事件但没有简易修复建议」；否则逐条输出已置位的建议。

### 直方图：fillCommandCDF 与两条 CDF 入口

`fillCommandCDF` 先 `addReplyMapLen(c,2)` 回两个键，`calls` 取 `histogram->total_count`，`histogram_usec` 用 `addReplyDeferredLen` 占位后填充：`hdr_iter_log_init(&iter,histogram,1024,2)` 以 1024 作为第一个报告层级的值单位、底数 2 逐层翻倍（`deps/hdr_histogram/hdr_histogram.c` 的实现把这个参数写进 `next_value_reporting_level`），循环里 `micros = iter.highest_equivalent_value / 1000`（纳秒截成微秒），只有 `cumulative_count > previous_count` 的桶才输出「微秒上界、累计计数」并在 `samples` 上加一，最后 `setDeferredMapLen(c,replylen,samples)`。源码注释写明覆盖范围从 1 纳秒到约 1 秒、最多约 `log2(1000000000)` 即 30 个桶。

`latencyAllCommandsFillCDF` 用 `dictInitSafeIterator` 遍历命令字典，`cmd->latency_histogram` 非空才输出 `cmd->fullname` 与该命令的 CDF，随后在 `cmd->subcommands` 非空时以自己的 `cmd->subcommands_dict` 递归同一函数——子命令有自己的直方图槽位。`latencySpecificCommandsFillCDF` 从 `c->argv` 的下标 2 起逐个 `lookupCommandBySds`，查不到命令名就 `continue` 跳过（注释说明「不认识的名字不回回复」），查到就输出自身与 `subcommands_dict` 里所有子命令的直方图，末尾用 `setDeferredMapLen(c,replylen,command_with_data)` 定长度。

### 累计桶：durationAddSample

函数体只有越界保护加三个字段更新：`if (type >= EL_DURATION_TYPE_NUM) return;`，然后取 `server.duration_stats[type]`，`cnt++`、`sum += duration`、`duration > max` 时更新 `max`。`server.duration_stats` 由 `resetServerStats`（`src/server.c`）用 `memset(server.duration_stats, 0, sizeof(durationStats) * EL_DURATION_TYPE_NUM)` 清零。这四个桶的读出口是 INFO：`eventloop_cycles`、`eventloop_duration_sum`、`eventloop_duration_cmd_sum`、`eventloop_duration_aof_sum`、`eventloop_duration_cron_sum`、`eventloop_duration_max`。

### 谁在写样本：真实调用点

按事件名把写入端列全（全部经 `latencyAddSampleIfNeeded`，除 `latencyAddSample` 直调）：

| 事件名 | 写入点 |
|---|---|
| `command` / `fast-command` | `src/server.c` 的 `call`，按 `real_cmd->flags & CMD_FAST` 二选一，值是 `duration/1000`（微秒整除成毫秒），且仅在 `update_command_stats` 为真时执行 |
| `fork` | `src/server.c` 的 `redisFork`，父进程分支写 `server.stat_fork_time/1000` |
| `aof-write` 与三分类 | `src/aof.c` 的 `flushAppendOnlyFile`，按 `sync_in_progress`、`hasActiveChildProcess()` 三选一后再无条件补记一次 `aof-write` |
| `aof-fsync-always` | 同函数的 `AOF_FSYNC_ALWAYS` 分支 |
| `aof-fstat` | `src/aof.c` 的 `getAppendOnlyFileSize` |
| `aof-rename` | `src/aof.c` 的 `backgroundRewriteDoneHandler`，两处分别记一次 |
| `rdb-unlink-temp-file` | `src/rdb.c` 的 `backgroundSaveDoneHandlerDisk`，在子进程被信号终止的分支里，于 `rdbRemoveTempFile` 前后取时间 |
| `while-blocked-cron` | `src/server.c` 的 `whileBlockedCron` |
| `module-acquire-GIL` | `src/server.c` 的 `afterSleep`，包住 `moduleAcquireGIL` 与模块事件派发 |
| `expire-cycle` | `src/expire.c` 的 `activeExpireCycle`，值 `elapsed/1000` |
| `eviction-cycle`、`eviction-lazyfree` | `src/evict.c` 的 `performEvictions`，前者包整个淘汰周期，后者包 `cant_free` 里那段等惰性释放的 `usleep` 循环 |
| `active-defrag-cycle` | `src/defrag.c` 的 `activeDefragTimeProc` |
| `expire-del` / `evict-del` | `src/db.c` 的 `deleteKeyAndPropagate`，按 `notify_type` 二选一，包 `dbGenericDelete` |
| `command-unblocking` | `src/blocked.c` 的 `updateStatsOnUnblock`，值 `reply_us/1000` |
| 任意（模块自定义） | `src/module.c` 的 `RM_LatencyAddSample` |

`updateCommandLatencyHistogram`（`src/server.c`）是直方图的唯一写入口：先把耗时 clamp 到 `[LATENCY_HISTOGRAM_MIN_VALUE, LATENCY_HISTOGRAM_MAX_VALUE]`，`*latency_histogram` 为空时用三个常量做 `hdr_init`，再 `hdr_record_value`。调用方两个——`call` 里在 `server.latency_tracking_enabled` 且客户端非阻塞时传 `c->duration*1000`；`updateStatsOnUnblock` 里传 `total_cmd_duration*1000`（`blocked_us + reply_us` 一起计入）。直方图本体在命令注销时由 `moduleFreeCommand` 用 `hdr_close` 释放并置空。

### 命令重写：rewriteClientCommandVector 到 replaceClientCommandVector

`rewriteClientCommandVector(client *c, int argc, ...)` 是变参入口：`zmalloc(sizeof(robj*)*argc)` 申请新向量，`va_arg` 逐个取 `robj*`、写进 `argv[j]` 并 `incrRefCount`，然后整份交给 `replaceClientCommandVector(c, argc, argv)`，最后 `va_end`。

`replaceClientCommandVector` 的顺序是：

1. `retainOriginalCommandVector(c)`——内部静态函数，若 `c->original_argv` 已存在直接返回，否则按当前 `c->argc` 复制一份并 `incrRefCount` 每个对象。所以**只有第一次重写会快照原始参数**，后续重写不会覆写快照。
2. 决定要同步哪个 pending command：`c->mstate.executing_cmd < 0` 时（非 MULTI 状态）若 `c->pending_cmds.ready_len > 0` 就取 `head`，并断言它不带 `PENDING_CMD_FLAG_INCOMPLETE`；否则走 MULTI 分支，断言 `executing_cmd < mstate.count` 后从 `mstate.commands[]` 取当前命令。
3. 拿到 `pcmd` 就断言 `pcmd->argv == c->argv`，再把 `argv` / `argc` / `argv_len` 三个字段同步过去——命令向量和 pending command 共用同一份数组，必须一起换。
4. `freeClientArgv(c)` 递减旧对象引用并释放旧数组，随后 `c->argv = argv; c->argc = c->argv_len = argc;`。
5. 非 MULTI 状态时重算 `all_argv_len_sum`：遍历新向量用 `getStringObjectLen` 求和；有 `pcmd` 时先把旧 `pcmd->argv_len_sum` 减掉、换新值再加回。源码注释说明 MULTI 状态不跟踪这个总和（写队列时另算）。
6. `c->cmd = lookupCommandOrOriginal(c->argv,c->argc);` 并回填 `pcmd->cmd`，最后断言 `c->cmd != NULL`。`lookupCommandOrOriginal`（`src/server.c`）先查 `server.commands`，查不到再查 `server.orig_commands`——这就是 `rename-command` 改过名之后重写向量仍能找到命令元数据的原因，函数注释把这条用途写死了。

**调用方按语义分三类。** 过期类：`src/expire.c` 的 `expireGenericCommand` 在 `checkAlreadyExpired(when)` 为真时把命令改成 `shared.unlink` 或 `shared.del`（取决于 `server.lazyfree_lazy_expire`），否则用 `rewriteClientCommandArgument(c,0,shared.pexpireat)` 改成绝对时间；`src/t_string.c` 的 `setGenericCommand` 在有 TTL 时改写成 `SET key val PXAT <毫秒>`（`c->argc == 5` 且 proc 是 `setCommand` 时走原地改两个参数的 `rewriteClientCommandArgument`，否则整份 `rewriteClientCommandVector`），`getexCommand` 分成 `PEXPIREAT` / `PERSIST` / `DEL` 三支。删除类：`src/db.c` 的 `delexCommand` 在 `deleted` 为真时改成 `shared.del`；`src/t_set.c` 的 `spopWithCountCommand` 在 `count >= size` 时改成 `unlink` 或 `del`；`src/cluster.c` 的 `restoreCommand` 在 `ttl && checkAlreadyExpired(ttl)` 且确实删掉了旧键时同样改成 `unlink` 或 `del`，`migrateCommand` 则在收到目标端确认（`del_idx > 1`）后把整条命令换成 `DEL`，并用 `replaceClientCommandVector` 接管 `newargv` 所有权。聚合类：`src/t_zset.c` 的 `genericZpopCommand` 把 `ZMPOP` 换成 `ZPOPMIN`/`ZPOPMAX` 带计数，`src/geo.c` 的 `geoaddCommand` 把经纬度换成 score 后先 `replaceClientCommandVector` 再直接调 `zaddCommand`。此外 `src/t_list.c`、`src/t_hash.c`、`src/gcra.c` 各自把简写命令换成显式命令。

**传播用的是重写后的向量，观测用的是原始向量。** `call()` 尾部调用 `alsoPropagateEx(c->db->id,c->argv,c->argc,propagate_flags,duration)`，传的是 `c->argv`——即重写后的可重新执行序列，之后由 `propagateNow` 分送 `feedAppendOnlyFile` 与 `replicationFeedSlaves`。反过来，`slowlogPushCurrentCommand` 用 `c->original_argv ? c->original_argv : c->argv`，MONITOR 播报用同一套选择逻辑，`hotkeys.c` 取键也优先读 `original_argv`。`freeClientOriginalArgv` 在 `call()` 末尾、`hotkeyStatsPostCurrentCmd` 之后执行，且只在客户端非 `CLIENT_BLOCKED` 时执行——被阻塞的客户端要留到解锁时再记慢日志。

## 关键取舍

**源码事实：模块入口与宏的阈值语义不对称。** `RM_LatencyAddSample` 的条件只有 `latency >= server.latency_monitor_threshold`，没有「阈值非零」这一层；阈值配成 0 时任何非负耗时都满足条件，模块仍能往 `server.latency_events` 写样本。宏路径（`latencyAddSampleIfNeeded`）在同一配置下完全静默。

**源码事实：写入端与 DOCTOR 判断端的事件名不是同一套字面量。** `src/db.c` 写的是 `expire-del` 与 `evict-del`，`createLatencyReport` 里比对的是 `eviction-del`；`aof-rewrite-diff-write` 只出现在 `createLatencyReport` 的判断里，`src/` 下没有任何用该字面量的 `latencyAddSampleIfNeeded`。结果是这两类事件在报告里拿不到对应建议。

**源码事实：160 槽环形 + 同秒合并 = 至少 160 秒历史，但只对「每秒都有尖峰」成立。** 空槽靠 `time == 0` 判空，`time(NULL)` 的秒级粒度决定了合并窗口就是一秒；`ts->idx` 回绕会覆写最老槽，历史深度由写入速率决定，不是固定时间窗。

**源码事实：`ts->max` 与样本数组互不同步清理。** 样本被回绕覆盖后 `ts->max` 仍是历史最大值；只有 `latencyResetEvent` 整条删掉字典项才会把两者一起丢掉。

**源码事实：`server.latency_events` 上没有任何锁。** `latencyAddSample` 全程无互斥，`latencyResetEvent` 只用 `dictInitSafeIterator` 保证遍历中删除自身安全。主线程上的所有采样点天然串行，但 `RM_LatencyAddSample` 是模块可见入口。

**架构推论：** 上面这条无锁约定依赖「采样只在主线程发生」这个隐含前提；模块线程直接调 `RM_LatencyAddSample` 时，字典会被两个线程并发 `dictAdd`，源码没有加锁也没有断言拦住它。

**源码事实：命令重写只快照一次原始参数。** `retainOriginalCommandVector` 用 `if (c->original_argv) return;` 短路，所以第二次重写不会把「已被重写过的向量」误当成原始命令记进慢日志或 MONITOR。

**源码事实：重写之后重新查表，且回落查原始表。** `replaceClientCommandVector` 末尾无条件重查 `c->cmd`，`lookupCommandOrOriginal` 的第二跳是 `server.orig_commands`，因此 `rename-command` 把命令改名后，重写出的规范名仍能解析到命令实现。

**架构推论：** 直方图把耗时 clamp 到 1 秒上限，`LATENCY HISTOGRAM` 的 `histogram_usec` 里 1 秒及以上的样本全部落进同一个上界桶，源码层面无法区分 1 秒与 10 秒的命令；要分辨就只能看 `LATENCY LATEST` 的毫秒值或慢日志。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/latency.c` | `latencyAddSample` | 按事件名取或建时间序列，合并同秒样本、维护 `max`、推进环形写游标 |
| `src/latency.c` | `latencyMonitorInit` | 建 `server.latency_events` 字典（唯一调用点是 `initServer`） |
| `src/latency.c` | `latencyResetEvent` | 按 `strcasecmp` 匹配删除单个或全部事件 |
| `src/latency.c` | `analyzeLatencyForEvent` | 两遍扫描算出 `all_time_high`、`avg`、`min`、`max`、`mad`、`samples`、`period` |
| `src/latency.c` | `createLatencyReport` | LATENCY DOCTOR 的报告生成与逐事件建议 |
| `src/latency.c` | `latencyCommand` | 六个子命令的分派与参数个数校验 |
| `src/latency.c` | `latencyCommandReplyWithSamples`、`latencyCommandReplyWithLatestEvents` | HISTORY 与 LATEST 两种回复形态 |
| `src/latency.c` | `latencyCommandGenSparkeline` | GRAPH 的 ASCII 图形渲染入口 |
| `src/latency.c` | `fillCommandCDF`、`latencyAllCommandsFillCDF`、`latencySpecificCommandsFillCDF` | 直方图转 `histogram_usec` 与 HISTOGRAM 的全量 / 指定命令两条路径 |
| `src/latency.c` | `durationAddSample` | 四个累计桶的 `cnt` / `sum` / `max` 更新与越界保护 |
| `src/latency.c` | `THPGetAnonHugePagesSize`、`latencyTimeSeriesDictType` | DOCTOR 的 THP 检测、事件字典的哈希/比较/析构定义 |
| `src/latency.h` | `LATENCY_TS_LEN`、`latencyStartMonitor`、`latencyEndMonitor`、`latencyAddSampleIfNeeded` | 160 槽上限与三个阈值宏 |
| `src/latency.h` | `latencySample`、`latencyTimeSeries`、`latencyStats`、`DurationType` | 样本、序列、统计结果与累计桶下标的类型定义 |
| `src/server.h` | `LATENCY_HISTOGRAM_MIN_VALUE`、`LATENCY_HISTOGRAM_MAX_VALUE`、`LATENCY_HISTOGRAM_PRECISION` | 直方图的值域与精度常量 |
| `src/server.c` | `call` | `command` / `fast-command` 采样点与直方图写入点，以及用重写向量做 `alsoPropagateEx` |
| `src/server.c` | `updateCommandLatencyHistogram` | clamp 耗时、惰性 `hdr_init`、`hdr_record_value` |
| `src/server.c` | `redisFork`、`whileBlockedCron`、`afterSleep` | `fork`、`while-blocked-cron`、`module-acquire-GIL` 三个采样点 |
| `src/server.c` | `lookupCommandOrOriginal` | 重写后重查命令元数据，并回落到 `server.orig_commands` |
| `src/server.c` | `alsoPropagateEx`、`propagateNow` | 把重写后的向量分送到 AOF 与副本 |
| `src/server.c` | `slowlogPushCurrentCommand` | 观测侧优先用 `original_argv` |
| `src/networking.c` | `rewriteClientCommandVector` | 变参入口：拷贝对象引用后交给 `replaceClientCommandVector` |
| `src/networking.c` | `replaceClientCommandVector`、`retainOriginalCommandVector`、`freeClientArgv` | 换向量、同步 pending command、快照原始参数、释放旧向量 |
| `src/config.c` | `latency-monitor-threshold`、`latency-tracking`、`latency-tracking-info-percentiles` | 三个配置项的注册、默认值与取值范围 |
| `src/module.c` | `RM_LatencyAddSample` | 模块自报事件的入口（无阈值非零判断） |

## 与主核 / AI 的关系

Redis 这套延迟监控值得抄的是三件事的组合方式：**阈值宏 + 按需建的时间序列 + 定长环形槽**。阈值宏把「要不要测」压成一次配置读取，采样点在热路径上几乎零成本；序列按需建，关闭时字典为空，内存基线不动；槽数固定，尖峰再多也不涨内存。主核若要在 `engine/db/` 上做同类能力，可直接沿用这个形状，但必须补两处 Redis 自己没做的收尾：事件名集中成一处枚举或表，避免写入端与报告端各写一套字面量；模块或后台线程可见的入账入口要么加锁，要么在入口处断言调用线程。

命令重写那套「规范向量用于传播、原始向量用于观测」的分层也值得照搬：它让「可重新执行」与「可审计」两个需求不必共用一份数据，代价是每客户端多一份数组快照，且只在第一次重写时拷贝。主核的复制与 WAL 若也做语义归一（例如把带相对 TTL 的写改写成绝对时间戳），这套双向量结构可以直接复用 `lookupCommandOrOriginal` 式的重查逻辑来保证元数据不错位。

AI 侧把 Redis 当向量或会话缓存时，可用的两个入口是 `INFO latencystats` 与 `LATENCY HISTOGRAM`：前者按 `latency-tracking-info-percentiles` 输出百分位，`src/config.c` 里该配置的默认值是 `{50.0, 99.0, 99.9}`（由 `configDefaultLatencyTrackingInfoPercentiles` 给出，长度由 `configDefaultLatencyTrackingInfoPercentilesLen` 算出），正好覆盖检索类命令关注的三个点；后者给每命令的累计分布。二者都受 `latency-tracking`（默认 1）控制，而 `latency-monitor-threshold` 默认 0 意味着「尖峰事件序列」这条线开箱是关的，需要显式 `CONFIG SET` 才会留下历史样本。

## 参考

- [Redis 文档：延迟监控（Latency monitoring）](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency-monitor/)
- [Redis 命令：LATENCY HISTORY](https://redis.io/docs/latest/commands/latency-history/)
- [Redis 命令：LATENCY DOCTOR](https://redis.io/docs/latest/commands/latency-doctor/)
- [Redis 命令：LATENCY HISTOGRAM](https://redis.io/docs/latest/commands/latency-histogram/)
