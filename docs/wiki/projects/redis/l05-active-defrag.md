---
project: redis
feature: active-defrag
page_type: feature
schema_version: 1
refs:
  - path: src/defrag.c
    symbols: [activeDefragCycle]
---

# 主动碎片整理

## 一句话职责

在 `activedefrag` 打开后，Redis 按「CPU 百分比配额 + 分阶段 + 跨调用保留游标」的方式逐段扫描键空间，把每一处可疑分配交给 jemalloc 判断值不值得搬；值得就复制到新地址、释放旧地址，持续压回外部碎片。它只改内存位置，不改数据语义；分配器的 bin/arena 策略不归它管，内存淘汰也不归它管。

## 机制是什么

`src/defrag.c` 文件头注释写明两件事：目的是「Try to find key / value allocations that need to be re-allocated in order to reduce external fragmentation」，做法是「scanning the keyspace and for each pointer we have, we can try to ask the allocator if moving it to a new address will help reduce fragmentation」。也就是说，决定搬不搬的不是 Redis，而是 jemalloc 侧的碎片提示接口 `je_get_defrag_hint`——注释说明它「was added to jemalloc in order to help us understand which pointers are worthwhile moving and which aren't」。

碎片指标由 `getAllocatorFragmentation` 算：调 `zmalloc_get_allocator_info` 拿 jemalloc 的 small bin 碎片字节与已分配字节，再令 `frag_pct = frag_smallbins_bytes / allocated * 100`。注释解释了为什么只取 small bin：「if most of the memory usage is large bins, we may show high percentage, despite the fact it's not a lot of memory for the user」——只有 small bin 的浪费是搬得动的。若 `server.lua_arena != UINT_MAX`，先把 lua arena 的四个量从 total 里减掉，理由是 lua 自己的 arena 不该计入 Redis 键空间的碎片。

是否启动由三项配置共同判定，默认值都写在 `src/config.c` 的配置表里：

| 配置项 | 默认值 | 在判定中的角色 |
|---|---|---|
| `activedefrag` | 0（关闭） | 总开关，映射到 `server.active_defrag_enabled` |
| `active-defrag-ignore-bytes` | `100<<20`（100MB） | 碎片浪费绝对值不到它就不启动 |
| `active-defrag-threshold-lower` | 10 | 碎片百分比不到它就不启动 |
| `active-defrag-threshold-upper` | 100 | 力度插值的上端 |
| `active-defrag-cycle-min` | 1 | 目标 CPU 百分比下限 |
| `active-defrag-cycle-max` | 25 | 目标 CPU 百分比上限 |
| `active-defrag-max-scan-fields` | 1000 | 单键内部字段数超过它就延后处理 |

`computeDefragCycles` 的判据是 `frag_pct < server.active_defrag_threshold_lower || frag_bytes < server.active_defrag_ignore_bytes`，且这个判据只在 `!server.active_defrag_running`（还没在跑）时生效——已经在跑就把这轮走完，不因为碎片降下来了中途撤。

力度来自线性插值：`INTERPOLATE(frag_pct, threshold_lower, threshold_upper, cycle_min, cycle_max)`，即碎片百分比从下限走到上限，目标 CPU 百分比从 1 走到 25。若 `threshold_upper <= threshold_lower`，`computeDefragCycles` 直接取 `active_defrag_cycle_max`，注释把这种配置称为「invalid threshold configuration」。

这套机制有编译期前置条件。`src/zmalloc.h` 里 `HAVE_DEFRAG` 的定义条件是 `(defined(USE_JEMALLOC) && defined(JEMALLOC_FRAG_HINT)) || defined(DEBUG_DEFRAG_FORCE)`；没有它时 `src/config.c` 的 `isValidActiveDefrag` 拒绝打开开关并给出错误信息「requires a Redis server compiled with a modified Jemalloc」，`src/defrag.c` 的 `#else HAVE_DEFRAG` 分支则把 `activeDefragCycle` 做成空函数、`activeDefragAlloc` 直接返回 NULL。

## 怎么实现的

**入口 `activeDefragCycle` 只有三步。** 第一，`!server.active_defrag_enabled` 直接返回——调用方不判开关，门槛在函数内。第二，`hasActiveChildProcess()` 为真直接返回，该函数体就是 `server.child_pid != -1`——有子进程时不搬内存，避免动到 COW 页。第三，`computeDefragCycles()` 更新 `server.active_defrag_running`，若它大于 0 且 `defragIsRunning()`（判据是 `defrag.timeproc_id > 0`）为假，就 `beginDefragCycle()` 开新一轮。

**`computeDefragCycles` 的取数与钳制顺序。** 先取碎片数据：`defragFragCacheTake` 命中就直接用本 cron tick 缓存的值，命中条件是缓存的 `cronloops` 等于当前 `server.cronloops`；未命中才调 `getAllocatorFragmentation`。缓存的生产者是 `src/server.c` 的 `cronUpdateMemoryStats`，它在采样完 allocator 信息后调 `defragFragCachePut(frag, alloc)`，并且先做与 `getAllocatorFragmentation` 同样的 lua arena 扣减（源码注释写明「the cached value matches the fall-through real measurement exactly」）。取到 `cpu_pct` 之后依次做三件事：`cpu_pct *= defrag.decay_rate`、`LIMIT(cpu_pct, active_defrag_cycle_min, active_defrag_cycle_max)`、以及只在 `cpu_pct > server.active_defrag_running || server.active_defrag_configuration_changed` 时写回。注释解释了「只升不降」：扫描中允许加大力度，但不该因为碎片下降就降低力度；例外是配置变更，`src/config.c` 的 `updateDefragConfiguration` 会把 `active_defrag_configuration_changed` 置 1，让新一轮的配置立刻生效。

**`beginDefragCycle` 装配阶段链。** 先 `moduleDefragStart()` 通知各模块，然后建一条 `defrag.remaining_stages` 链表，按顺序压入：每个 db 四个阶段（`defragStageDbKeys` 扫主字典、`defragStageExpiresKvstore` 扫过期字典、`defragStageBlessedKvstore` 扫 blessed-keys 索引、`defragStageSubexpires` 扫字段级过期桶）；全局的 `pubsub_channels` 与 `pubsubshard_channels` 两个阶段（共用 `defragStagePubsubKvstore`，靠 `getPubSubChannels` 函数指针区分）；单发的 `defragLuaScripts`；哈希模板注册表的三段 `defragStageHashTemplatesByFields` / `...ByFieldsLp` / `...ById`；最后对 `modules` 字典里每个「`defrag_cb` 或 `defrag_cb_2` 非空」的模块各加一个 `defragModuleGlobals` 阶段。收尾记录 `start_cycle`、`start_defrag_hits`、`start_defrag_misses`、`start_frag_pct`，并 `aeCreateTimeEvent(server.el, 0, activeDefragTimeProc, NULL, NULL)` 注册一个 0 毫秒触发的时间事件。

**它不是挂在 serverCron 上的。** 搬移进度由独立定时器 `activeDefragTimeProc` 驱动，注释写明「While defrag is running, this is called much more often than the server cron. Frequent short calls provides low latency impact」。每次进入先做两件事：`!server.active_defrag_enabled` 为真则 `endDefragCycle(0)`（异常终止）并返回 AE_NOMORE；`hasActiveChildProcess()` 为真则把 `defrag.timeproc_end_time` 清成 0（注释：prevent starvation recovery，避免把暂停时长算进下一次配额）并 `return 100`，即 100 毫秒后再试。

**每次能花多少 CPU：`computeDefragCycleUs`。** 基准是常量 `DEFRAG_CYCLE_US`，值为 500（微秒），注释称其为「Standard duration of defrag cycle」。有上次结束时刻时按占空比反解：注释给出方程 `D / (D + W) = P / 100`，解得 `dutyCycleUs = targetCpuPercent * waitedUs / (100 - targetCpuPercent)`，再减去累积的 `timeproc_overage_us`。算出来小于 `DEFRAG_CYCLE_US` 时不缩短周期（注释：that would increase overhead），而是把差额记进 `timeproc_overage_us` 并仍按 500 微秒运行；算出来大于 `DEFRAG_CYCLE_US * 10` 就钳到 5000 微秒（注释：等待已经很长时不该再加大延迟）。首轮或刚恢复（`timeproc_end_time == 0`）时清掉 overage 直接用 500 微秒。

**下次隔多久：`computeDelayMs`。** 先记 `timeproc_end_time`，把 `endtime` 的超时量累进 `timeproc_overage_us`（负值也允许抵扣，但归零后不再累负）。间隔按「整周期」反算而不是简单比例：`totalCycleTimeUs = DEFRAG_CYCLE_US * 100 / targetCpuPercent`，`delayUs = totalCycleTimeUs - DEFRAG_CYCLE_US`，再加上 overage 里属于非 duty 的那部分 `overage * (100 - targetCpuPercent) / 100`；注释用例子说明「run for 1ms and delay 10ms is NOT 10%, because the total cycle time is 11ms」。

**阶段推进。** 定时器里是 `do { ... } while (haveMoreWork && getMonotonicUs() <= endtime - DEFRAG_CYCLE_US)`：取当前阶段（没有就取链表头），调 `stage_fn(stage->ctx, endtime)`，返回 DEFRAG_DONE 就从链表摘除该节点并把 `current_stage` 置空。循环条件是双重：还有阶段，且剩余时间够一个完整的 500 微秒标准周期——注释说明提前做完一个阶段时可以再开下一个（「This can happen when defrag is running infrequently, and starvation protection has increased the duty-cycle」）。末尾 `latencyAddSampleIfNeeded("active-defrag-cycle", latency)` 记一条延迟样本；有剩余工作就 `return computeDelayMs(endtime)`，否则 `endDefragCycle(1)` 并返回 AE_NOMORE 结束定时器。

**kvstore 阶段骨架 `defragStageKvstoreHelper`。** 第一阶段用特殊 slot 值 `ITER_SLOT_DEFRAG_LUT` 做「结构层搬移」：循环 `kvstoreDictLUTDefrag(state->kvs, state->cursor, dictDefragTables)` 直到 cursor 归零，这一步只搬 dict 结构体与两张哈希表数组，不搬 key/value；每个 cursor 步进都检查 `getMonotonicUs() >= endtime`，超时返回 DEFRAG_NOT_DONE。搬完把 slot 改成 `ITER_SLOT_UNASSIGNED`，进入按 slot 的循环：cursor 为空时用 `kvstoreGetFirstNonEmptyDictIndex` / `kvstoreGetNextNonEmptyDictIndex` 换下一个非空字典，全部走完返回 DEFRAG_DONE；否则 `kvstoreDictScanDefrag(state->kvs, state->slot, state->cursor, scan_fn, defragfns, ctx)`。时钟读取被显式节流：只有 `++iterations > 16` 或搬移增量超过 512 或扫描增量超过 64 时才读一次 `getMonotonicUs()` 并决定是否退出。

**主键阶段 `defragStageDbKeys` 的两道闸。** 第一道是 kvstore 身份校验：`db->keys != defrag_keys_ctx->kvstate.kvs` 直接返回 DEFRAG_DONE，注释说明这是应对 flushdb、swapdb 之类「There has been a change of the kvs」。第二道是 `defragfns` 只给 `defragAlloc`，把 `defragKey` 与 `defragVal` 留 NULL——真正的搬运在回调 `dbKeysScanCallback` 里做。这个回调包一层计数：调 `defragKey` 前后比较 `stat_active_defrag_hits`，变了算 key_hits，没变算 key_misses，再 `stat_active_defrag_scanned++`。

**单键搬运 `defragKey`。** 顺序是：先记 `expire = kvobjGetExpire(ob)`，若不为 -1 就 `kvstoreDictFindLink(db->expires, slot, kvobjGetKey(ob), NULL)` 先把过期字典里的链接找出来——注释解释了为什么必须提前找「We can't search in db->expires for that KV after we've released the pointer it holds, since it won't be able to do the string compare」。然后调 `activeDefragKvobj` 搬键对象本体；它拒绝 `kv->refcount != 1` 的对象，并在 EMBSTR 场景先算 `offsetEmbstr = kv->ptr - kv`、搬完后按同一偏移重算 `kvNew->ptr`，因为 EMBSTR 的字符串数据内嵌在同一个分配里。搬成之后 `kvstoreDictSetAtLink(db->keys, slot, kvnew, &link, 0)`，并在有 expires 条目时 `kvstoreDictSetAtLink(db->expires, slot, kvnew, &exlink, 0)` 同步更新两处引用。接着按 `ob->type` + `ob->encoding` 分派到具体结构的搬运函数：quicklist 走 `defragQuicklist`、skiplist 走 `defragZsetSkiplist`、btree 走 `defragZsetBtree`、哈希走 `defragHash`、集合走 `defragSet`、数组走 `defragArray`、bitmap 走 `defragBitmapObject`、模板数组走 `defragTmplArray`、流走 `defragStream`、模块走 `defragModule`；listpack / intset 这类单块分配直接 `activeDefragAlloc(ob->ptr)`。遇到未知编码 `serverPanic`。带字段级 TTL 的哈希（`hashTypeGetMinExpire` 不为 `EB_EXPIRE_TIME_INVALID`）与已注册进 subexpires 的集合，键对象本体这一步跳过，注释说留到 subexpires 阶段统一处理。

**大对象延后：`defragLater` / `defragLaterStep` / `defragLaterItem`。** 各类结构在搬之前先比 `server.active_defrag_max_scan_fields`（默认 1000）：quicklist 比 `ql->len`、哈希/集合比 `dictSize`、skiplist 比 `dictSize(zs->dict)`、btree 比 `zbtreeLength`、数组比 `arCount`、模板哈希比 `hta->field_count`、bitmap 用 `bitroarContainerCountUpTo` 数到上限加一。超了就 `defragLater`——把 key 名 `sdsdup` 后挂进 `ctx->defrag_later` 链表，主扫描继续往前走，注释说明目的是「to prevent latency spikes when handling large items」。`defragLaterStep` 作为 kvstore 助手的前置回调，每次从链表头取 key 名、`kvstoreDictFind` 回查（注释解释了为何按名字重查：key 在这之前可能被删或改），再交 `defragLaterItem` 按类型增量推进：quicklist 用 `quicklistBookmarkCreate(ql, "_AD", node)` 建书签记录位置（`++iterations > 128` 之后才看时间），skiplist/哈希/集合用 `dictScanDefrag` 续游标，btree 用 `zbtreeDefragStep`，stream 用 `scanLaterStreamListpacks`，模块走 `moduleLateDefrag`。游标归零表示该项做完，从链表摘除；超时则 break，把剩余留到下次。

**收尾 `endDefragCycle` 与递归重启。** 正常终止先断言 `!defrag.current_stage` 且阶段链表为空；异常终止 `aeDeleteTimeEvent` 删掉定时器并摘掉当前阶段。之后测一次碎片、写一条 LL_VERBOSE 日志（含耗时、搬移次数、碎片百分比、碎片字节数）、`stat_total_active_defrag_time += elapsedUs(stat_last_active_defrag_time)`、把 `active_defrag_running = 0`、`updateDefragDecayRate(frag_pct)`、`moduleDefragEnd()`，最后**直接再调一次** `activeDefragCycle()`，注释写着「Immediately check to see if we should start another defrag cycle」。

**自适应降速 `updateDefragDecayRate`。** 用本轮开始时的快照算两个量：搬移命中率（`stat_active_defrag_hits` 与 `..._misses` 的增量）和碎片变化 `last_frag_pct_change = start_frag_pct - frag_pct`。满足 `fabs(last_frag_pct_change) > 2`（碎片变动超过 2 个百分点）或 `last_frag_pct_change < 0 && last_hits >= (last_hits + last_misses) * 0.01`（碎片在降且命中率不低于 1%）时把 `decay_rate` 复位成 `1.0f`，否则 `decay_rate *= 0.9`。这个系数下一轮乘在 `cpu_pct` 上——注释的说法是「When defragmentation efficiency is low, we gradually reduce the speed for the next cycle to avoid CPU waste」。

**载入阻塞期的补位 `defragWhileBlocked`。** 开头 `server.active_defrag_paused` 为真直接返回；这个计数器只有 `src/aof.c` 在加载 AOF 文件头的 RDB preamble 时对 `rdbLoadRio(&rdb, RDBFLAGS_AOF_PREAMBLE, NULL)` 前后做 `++` / `--`。然后：没在跑就先调 `activeDefragCycle()`；仍没跑就返回；否则保存 `timeproc_id`，**直接模拟一次** `activeDefragTimeProc` 调用，返回值是 AE_NOMORE 时用 `aeDeleteTimeEvent` 把原来那个事件删掉，否则忽略它返回的重调度间隔，交给事件循环下次处理定时器时接管。它的调用方只有 `src/server.c` 的 `whileBlockedCron`，且限定在 `isAOFLoadingContext()` 为真时调用。

**结果怎么观察。** `src/server.c` 的 INFO 输出 `active_defrag_running`、`active_defrag_hits`、`active_defrag_misses`、`active_defrag_key_hits`、`active_defrag_key_misses`、`total_active_defrag_time`、`current_active_defrag_time`；`src/debug.c` 另有 `DEBUG DEFRAG-FRAG-CACHE-STATS`，输出 `defrag_frag_cache_hits`。

**回退路径点名。** 开关关闭：`src/server.c` 的 `databasesCron` 无条件调 `activeDefragCycle`，由函数内的 `!server.active_defrag_enabled` 早退。有子进程：`activeDefragCycle` 与 `activeDefragTimeProc` 各自返回，前者不开始新一轮、后者退到 100 毫秒后再试。编译期没有 `HAVE_DEFRAG`：`activeDefragCycle` 是空函数、`activeDefragAlloc` 返回 NULL 让各调用点当作「没搬」，`src/config.c` 的 `isValidActiveDefrag` 让 `activedefrag` 根本打不开。kvstore 被 flushdb/swapdb 换掉：`defragStageDbKeys` 等阶段返回 DEFRAG_DONE 直接跳过。

## 关键取舍

**源码事实：** 阈值是「百分比 + 绝对字节」双重门槛，`computeDefragCycles` 用逻辑或判定，任一项不达标就不启动，因此小实例的高百分比碎片不会触发整理（`active-defrag-ignore-bytes` 默认 `100<<20`）。**源码事实：** 力度线性插值后先乘 `decay_rate` 再 `LIMIT` 回 `[cycle_min, cycle_max]`，配置允许范围是 1 到 99（`createIntConfig` 的 min/max 参数）。**源码事实：** 力度只升不降，只有 `active_defrag_configuration_changed` 能触发重新计算。**源码事实：** 单次 duty 下限 500 微秒、上限 5000 微秒（`DEFRAG_CYCLE_US` 与 `DEFRAG_CYCLE_US * 10`），周期短于下限时不缩短而是记 overage。**源码事实：** 时钟读取按 16 次迭代 / 512 次搬移 / 64 次扫描三档节流。**源码事实：** 单键字段数超过 `active-defrag-max-scan-fields`（默认 1000）就改为按 key 名延后，quicklist 延后路径在 128 次迭代后才看一次时间。**源码事实：** `defragLuaScripts` 里的 `UNUSED(endtime)` 说明这个阶段整段做完才返回，不参与时间预算。**源码事实：** 有子进程时完全不搬；AOF 的 RDB preamble 载入期靠 `active_defrag_paused` 完全停摆。**源码事实：** 副本上同样会跑 `activeDefragCycle`——`databasesCron` 没有主从分流（过期扫描才有 `iAmMaster()` 分流）。

**架构推论：** 用「CPU 百分比」而不是「每轮固定字节数」做预算，是把搬移成本折算成主线程最稀缺的资源——搬一字节的代价取决于结构形状（quicklist 节点是几十字节的复制，百万字段的 dict 是成千上万次 memcpy），字节数无法代表耗时；而 CPU 百分比可以直接和命令处理抢同一个时间片来定价。**架构推论：** 必须配独立定时器（`aeCreateTimeEvent` 传 0 延迟）而不能只靠 `serverCron`，因为 `serverCron` 每 `1/server.hz` 秒才跑一次，而这里的配额粒度是 500 微秒级：挂在 cron 上只能每次啃一大块，正是注释里要避免的「latency spike」。**架构推论：** 分阶段 + 每阶段自带游标的代价是「同一轮里键空间可能已经被改过」，所以每个阶段开头都要重新验证 kvstore 身份、延后对象要用 key 名重查；这换来的是任何一次 timerproc 都不会超过 duty 预算，也不会因为一次 TABLE 级别的 memcpy 而让整轮失效。**架构推论：** 「力度只升不降」和「无效则 0.9 倍降速」是两个方向相反的自适应：前者防抖（避免碎片小幅波动导致力度来回横跳），后者防浪费（命中率低时逐步退让）。两者叠加时降速系数能把力度压到下限，但压不下去的部分靠阈值判定在下一轮退出——这是把「什么时候停」交给启动门槛、把「跑多猛」交给插值的分工。**架构推论：** 搬移要求 `refcount == 1`，而共享对象（pubsub channel 名）单独用 `activeDefragStringObEx(channel, dictSize(clients) + 1)` 按「调用方声明的期望引用数」搬并逐个更新 client 字典里的引用，说明原则是「引用数可枚举才搬」：只要存在 Redis 看不见的外部引用（IO 线程、模块指针），搬移就会变成悬垂指针。**架构推论：** 阈值判定基于 small bin 碎片而非 RSS/allocated 比值，是因为只有 small bin 的空洞能被 reloc 填上；用 RSS 比值当触发条件会让整理器在「大块内存占主体」的实例上空转。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/defrag.c` | `activeDefragCycle` | 周期入口：开关与子进程门槛、调 `computeDefragCycles`、必要时开新轮 |
| `src/defrag.c` | `computeDefragCycles` | 取碎片指标（缓存优先）、双阈值门槛、插值出目标 CPU 百分比并钳制 |
| `src/defrag.c` | `beginDefragCycle` | 按 db 与全局对象装配阶段链、记录起点统计、注册独立时间事件 |
| `src/defrag.c` | `activeDefragTimeProc` | 自有定时器：开关/子进程/配额换算、阶段推进、重调度或收尾 |
| `src/defrag.c` | `computeDefragCycleUs` / `computeDelayMs` | 占空比正反算：单次 duty 微秒数与下次触发间隔 |
| `src/defrag.c` | `defragStageKvstoreHelper` | kvstore 阶段骨架：先搬 dict 结构层，再逐桶扫描，按三档节流读时钟 |
| `src/defrag.c` | `defragKey` | 单键搬运：先定位 expires 链接，搬 kvobj，再按类型/编码分派内部结构 |
| `src/defrag.c` | `activeDefragKvobj` | 键对象本体搬移：拒绝 refcount≠1，EMBSTR 搬后重算内嵌指针偏移 |
| `src/defrag.c` | `defragLater` / `defragLaterStep` / `defragLaterItem` | 大对象按 key 名延后，按类型增量推进并在超时前保存进度 |
| `src/defrag.c` | `endDefragCycle` | 收尾：删事件、清阶段链、累计耗时、评估降速、递归判断重启 |
| `src/defrag.c` | `updateDefragDecayRate` | 无效周期按 0.9 倍降速，碎片变动或命中率达标时复位 |
| `src/defrag.c` | `getAllocatorFragmentation` | 碎片度量：small bin 碎片字节占已分配字节的百分比，扣减 lua arena |
| `src/defrag.c` | `defragFragCachePut` / `defragFragCacheTake` | 同一 cron tick 内的碎片测量缓存，避免重复调 allocator |
| `src/defrag.c` | `defragWhileBlocked` | 载入阻塞期的补位执行，模拟一次 timerproc |
| `src/config.c` | `activedefrag` / `active-defrag-*` 配置项与 `isValidActiveDefrag` / `updateDefragConfiguration` | 开关默认关闭、阈值与力度默认值、无 `HAVE_DEFRAG` 时拒绝开启 |
| `src/server.c` | `databasesCron` / `cronUpdateMemoryStats` / `whileBlockedCron` | 常规调用方、碎片缓存生产者、AOF 载入期的调用方 |

## 参考

- [Redis 源码：src/defrag.c](https://github.com/redis/redis/blob/unstable/src/defrag.c)
- [Redis 源码：redis.conf 中的 active defrag 配置段](https://github.com/redis/redis/blob/unstable/redis.conf)
- [Redis 文档：内存优化（含碎片与分配器行为）](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)
- [Redis 文档：配置文件与运行时修改](https://redis.io/docs/latest/operate/oss_and_stack/management/config/)

## 与主核 / AI 的关系

**架构推论：** 「阶段链 + 每阶段游标 + 全局 CPU 配额 + 独立定时器」这套骨架可以直接搬到主核的多模态存储：给每种存储形态（页、向量段、倒排表、embedding 行缓存）各定义一个阶段函数，用一个统一的 `endtime` 预算把每次执行切成 500 微秒级碎片，超预算就返回「未完」。Redis 在这里给出的两个关键接口细节值得照抄：阶段函数签名固定为 `(ctx, endtime) -> done/not_done`，以及阶段之间不共享状态、每个阶段自己在 ctx 里带游标——这样任何一个阶段都能在任何时刻安全退出。

**架构推论：** 但主核拿不到 Redis 免费的午餐。一是分配器：Redis 的整理循环完全依赖 jemalloc 的 `je_get_defrag_hint` 判断「搬了是否值得」，没有这个提示的分配器（如默认 glibc malloc）上，同样的循环会变成盲目 memcpy；主核若自持页式分配，应该把「值得搬」的判断换成自己的空闲页账本，而不是抄 `activeDefragAlloc` 的拷贝语义。二是并发：Redis 的整条整理路径跑在主线程里，所以它敢于「先找 expires 链接、再搬、再按链接写回」，中间无人插队；主核是多写者引擎，搬移必须在 latch 或快照隔离下进行，且搬移后的记录地址要让并发读者拿到——Redis 那套「引用数可枚举才搬」的规则在这里会升级为「事务快照里可见的地址不能原地改」。

AI 侧如果把 Redis 当作向量或 embedding 缓存，主动碎片整理的直接收益是 RSS 不因反复增删向量而膨胀；代价是与检索争 CPU。默认 `active-defrag-cycle-max` 为 25 表示碎片到上限时最多吃掉四分之一的时间片，对延迟敏感的向量检索应把力度上限压低、把 `active-defrag-ignore-bytes` 抬高，让整理只在真正浪费了 100 MB 以上时才启动；同时要接受 `activedefrag` 默认关闭、且需要带 jemalloc 碎片提示的构建才能打开这个事实。
