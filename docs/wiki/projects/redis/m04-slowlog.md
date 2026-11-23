---
project: redis
feature: slowlog
page_type: feature
schema_version: 1
refs:
  - path: src/slowlog.c
    symbols: [slowlogPushEntryIfNeeded]
---

# 慢查询日志

## 一句话职责

`slowlogPushEntryIfNeeded` 是慢查询日志的唯一入账口：先用 `slowlogWouldLog` 判断这次命令耗时是否达到阈值，达到就把命令行封装成一个 `slowlogEntry` 头插进 `server.slowlog` 链表，再从尾部逐条淘汰到 `slowlog_max_len` 以内，返回 1 表示确实入账、返回 0 表示丢弃。

## 机制是什么

慢查询日志是一个**阈值过滤 + 定长环形链表**的内存结构，不是磁盘日志文件：

- 阈值是 `server.slowlog_log_slower_than`，单位微秒。配置项为 `slowlog-log-slower-than`，由 `src/config.c` 的 `createLongLongConfig` 注册：默认值 10000（即 10 毫秒），取值范围 -1..LLONG_MAX。
- 容量是 `server.slowlog_max_len`。配置项为 `slowlog-max-len`，由 `createULongConfig` 注册：默认 128，范围 0..LONG_MAX。
- 两个配置项都是 `MODIFIABLE_CONFIG`，运行期可改；关闭手段有两种——阈值设为负数，或容量设为 0，`slowlogWouldLog` 对两者都返回 0。
- 条目只存在于内存，对外只能通过 `SLOWLOG` 命令读（`slowlogCommand`），源码顶部注释明确写了 slow log 并不写进 Redis 日志文件。

边界：本页覆盖「判定 → 建条目 → 入环 → 淘汰 → 读取」这条链路，以及条目里的 CPU 时间估算。命令耗时本身怎么记到 `c->duration` 上属于命令执行路径；`latencyAddSampleIfNeeded` 那套延迟监控样本是另一条独立统计线，`latency-monitor-threshold` 与 `slowlog-log-slower-than` 互不影响。

## 怎么实现的

### 一、判定（`slowlogWouldLog`）

函数体三行，顺序固定：

```c
if (server.slowlog_log_slower_than < 0 || server.slowlog_max_len == 0) return 0;
return duration >= server.slowlog_log_slower_than;
```

两个关闭条件先于比较；比较用的是 `>=`，耗时恰好等于阈值的那条命令会被记录。容量为 0 直接判否，避免出现「每条都入账再立刻淘汰」的空转。

### 二、建条目（`slowlogCreateEntry`）

入参是客户端、原始参数向量、参数个数、耗时与 CPU 耗时。条目字段按 `src/slowlog.h` 的 `slowlogEntry` 定义填：`argv`、`argc`、`id`、`duration`、`time`、`cname`、`peerid`、`cmd_argc`、`cpu_duration`。

**参数个数裁剪**：`slargc` 初值为 `argc`，若超过 `server.slowlog_max_argc` 就截到该值（配置项 `slowlog-entry-max-argc`，`createIntConfig` 默认 32、范围 2..INT_MAX）。`se->argc` 存裁剪后的 `slargc`，`se->cmd_argc` 存原始的 `argc`。裁剪发生时，最后一个槽位被替换成占位对象 `"... (%d more arguments)"`，数量是 `argc-slargc+1`——即被丢弃的原参数个数，因此**实际保留的参数是 `slargc-1` 条**。

**单个参数裁剪**：只对 `type == OBJ_STRING` 且 `sdsEncodedObject` 为真、且 `sdslen` 超过 `server.slowlog_max_string_len`（配置项 `slowlog-entry-max-string-len`，`createULongConfig` 默认 128、范围 1..LONG_MAX）的参数做截断：取前 `slowlog_max_string_len` 字节，再接上 `"... (%lu more bytes)"`，后面的数是 `sdslen - slowlog_max_string_len`。

**对象归属**：`refcount == OBJ_SHARED_REFCOUNT` 的共享对象直接复用指针，不做复制；其余用 `dupStringObject` 复制一份。注释给出的理由是不复制就会让慢日志参数与键空间里的字符串对象共享同一对象，`FLUSHALL ASYNC` 释放共享对象时就会撞出竞态。

**元数据**：`time = time(NULL)`（秒级 Unix 时间），`id = server.slowlog_entry_id++`，`peerid` 取自 `getClientPeerId`（该函数首次调用时用 `genClientAddrString` 生成并缓存在客户端上），`cname` 取 `c->name` 的内容，未设置名字时为 `sdsempty()`。

### 三、入环与淘汰（`slowlogPushEntryIfNeeded`）

```c
if (slowlogWouldLog(duration)) {
    listAddNodeHead(server.slowlog,
                    slowlogCreateEntry(c,argv,argc,duration,cpu_duration));
    while (listLength(server.slowlog) > server.slowlog_max_len)
        listDelNode(server.slowlog,listLast(server.slowlog));
    return 1;
}
return 0;
```

新条目插到链表头部，因此**头是最新的、尾是最旧的**；淘汰用 `while` 而不是 `if`，一次可补掉多条溢出（`slowlog-max-len` 被调小时会一次性裁到新容量）。`listDelNode` 会调用 `listSetFreeMethod` 注册的 `slowlogFreeEntry`——在 `slowlogInit` 里设的就是它；`slowlogFreeEntry` 对每个保留参数 `decrRefCount`，而 `decrRefCount` 对 `OBJ_SHARED_REFCOUNT` 直接返回，所以共享对象不会被误释放。

### 四、谁在调用（真实调用方）

- `server.c` 的 `call()`：命令执行完、统计 `c->duration` 之后，若 `update_command_stats` 为真（`isAOFLoadingContext()` 为假，即当前客户端不是 `CLIENT_ID_AOF`）且客户端没被 `CLIENT_BLOCKED`，调 `slowlogPushCurrentCommand(c, real_cmd, c->duration, …)`。CPU 采样起点只在「`c->duration` 等于本次这一段耗时、`off_cpu_tracking` 开启、且主线程带 `cpu_tracked` 或模块线程已采样」时传入 `&window_start`，否则传 `NULL` 表示不测。
- `blocked.c` 的 `updateStatsOnUnblock`：被阻塞的命令在 `call()` 里被跳过，改由它在解除阻塞后记账，耗时是 `c->duration + blocked_us + reply_us`，且固定传 `NULL` CPU 起点，所以这类条目的 `cpu_duration` 为 0。该函数被 `replyToBlockedClientTimedOut`、`disconnectAllBlockedClients`、`moduleUnblockClientOnKey`、`unblockClientOnError`、`moduleHandleBlockedClients`、`moduleBlockedClientTimedOut`、`unblockClientWaitingReplicas`、`unblockClientForAsyncFlush` 调用。
- `slowlogPushCurrentCommand` 自身先做两道闸：命令带 `CMD_SKIP_SLOWLOG`（`src/server.h` 定义为 `1ULL<<12`）直接返回，命令不会被记录；再调 `slowlogWouldLog`，不会入账的命令连 CPU 采样都不做。`commands.def` 中 `exec` 带该标志；模块命令在 `RM_CreateCommand` / `RM_CreateSubcommand` 的 `strflags` 里写 `no-slowlog` 即可拿到同一语义，解析点是 `src/module.c` 的 `commandFlagsFromString`。
- 入账成功后 `slowlogPushCurrentCommand` 才累加 `server.stat_slowlog_count`、`server.stat_slowlog_time_us_sum`、`server.stat_slowlog_time_us_max`，以及该命令自己的 `cmd->slowlog_count` / `slowlog_time_us_sum` / `slowlog_time_us_max`。

### 五、CPU 时间估算（`slowlogMeasureCpu` / `slowlogReportOffCpu`）

条目第八个字段是「执行线程 CPU 时间上界估算」，不是精确计时：

1. `m->measured = c->duration`；只有 `start != NULL && measured >= CPU_STARVATION_MIN_OFFCPU_US`（该常量在 `src/slowlog.c` 定义为 10000 微秒）时才 `sampled = 1`，否则整个函数返回，`cpu` 保持 0。
2. 主线程：`cpuSampleTake` 再取一次样本，窗口是 `now.wall - start->wall`，窗口内 CPU 是两次 `cpu_us` 之差；`gap = window - window_cpu` 是这段窗口里可归属的 off-CPU 时间，`cpu = min(window_cpu, measured)`，`off_max = min(gap, measured)`，`off_min = measured - cpu` 并夹到不超过 `off_max`，最终 `m->cpu = measured - off_min`。函数末尾把 `server.cpu_checkpoint = now`，让下一条命令的窗口从此刻起算。
3. 模块线程（非主线程）：直接读 `call()` 累加的 `c->cpu_duration`，`cpu = min(c->cpu_duration, measured)`，`off_max` 与 `off_min` 都取 `measured - cpu`，上下文切换次数直接取 `c->cpu_nvcsw` / `c->cpu_nivcsw`。

`slowlogReportOffCpu` 的触发条件是 `sampled` 且 `off_min >= CPU_STARVATION_MIN_OFFCPU_US`，触发后累加 `server.stat_slow_command_off_cpu_events` 并调 `offCpuLog` 打警告。`offCpuLog` 自带每秒一条的限流：窗口是 1000000 微秒，窗口内的调用只累加 `suppressed`，下一条真正打印时在消息尾部追加被抑制的条数。

另外 `offCpuCheckEventLoopCycle` 处理「事件循环整轮被抢 CPU」：由 `beforeSleep` 调用，把 AOF 写盘耗时作为 `excluded_us` 从 off-CPU 里扣掉，条件是 `server.off_cpu_tracking` 为真、`slowlog_log_slower_than >= 0`、整轮耗时同时不低于阈值与 10000 微秒；命中后累加 `server.stat_eventloop_off_cpu_events`。

### 六、读取面（`slowlogCommand`）

按子命令分发：`help`（仅 `argc == 2`）、`reset`、`len`、`get`（`argc` 为 2 或 3），其余一律 `addReplySubcommandSyntaxError`。

- `reset` → `slowlogReset`：只要链表长度大于 0 就 `listDelNode(list, listLast(list))`，从尾部逐条删空，然后回 `+OK`。
- `len` → 回 `listLength(server.slowlog)`。
- `get` → 局部变量 `long count = 10;` 是默认条数。带第三个参数时用 `getRangeLongFromObjectOrReply` 解析，范围 -1..LONG_MAX，越界回 `count should be greater than or equal to -1`；`count == -1` 被显式解释为「全部」，改成 `listLength(server.slowlog)`；再夹到当前长度。回复是一个 `count` 元素的数组，每个条目固定 8 个元素，顺序为 id、时间戳、耗时（微秒）、参数数组、`peerid`、`cname`、原始参数个数、`cpu_duration`。遍历用 `listRewind`（从 `head` 开始）配 `listNext`，所以输出是**新到旧**。

### 七、统计出口与初始化

- `INFO` 的 `Stats` 段输出 `slowlog_commands_count`、`slowlog_commands_time_ms_max`、`slowlog_commands_time_ms_sum`（后两者由 `stat_slowlog_time_us_*` 除以 1000 换算成毫秒），以及 `slow_command_off_cpu_events`、`eventloop_off_cpu_events`。
- `INFO commandstats` 里若某命令 `c->slowlog_count > 0`，该行额外带 `slowlog_count`、`slowlog_time_ms_sum`、`slowlog_time_ms_max`。
- `CONFIG RESETSTAT` → `configResetStatCommand` → `resetServerStats`（清 `stat_slowlog_count` 等三项）与 `resetCommandTableStats`（清每个命令的 `slowlog_count` / `slowlog_time_us_sum` / `slowlog_time_us_max`）。
- `initServer` 调 `slowlogInit`：`listCreate()` + `slowlog_entry_id = 0` + `listSetFreeMethod(server.slowlog, slowlogFreeEntry)`。
- `createLatencyReport`（`src/latency.c`）在 `command` 事件上读同一组配置：阈值小于 0 或容量为 0 时给出「slowlog 未启用」的诊断建议，阈值折算成毫秒后大于 `latency_monitor_threshold` 时给出调参建议。

## 关键取舍

**用裁剪换内存。** 参数超过 `slowlog-entry-max-argc`（默认 32）就把尾部折叠成一行占位符，单参数超过 `slowlog-entry-max-string-len`（默认 128 字节）就截断并标注剩余字节数。代价是 `SLOWLOG GET` 看到的命令行不是原始全量，收益是一条慢日志条目不会被超大 value 的命令把内存吃穿。原始参数个数用 `cmd_argc` 单独保留，正是为了让被裁剪这件事可被观察。

**只在慢命令上付采样成本。** 主线程的 `call()` 不为每条命令调 `getrusage`，改用 `server.cpu_checkpoint` 这个滚动的检查点做差；另外 `sample_cpu` 要求 `!on_main_thread`，即主线程命令根本不单独采样，只有模块线程（`RM_Call`）才在 `call()` 里取样本。代价是主线程条目的 CPU 时间是区间估算（`cpu = measured - off_min`），不是精确测量；收益是快命令零系统调用。

**CPU 字段允许为 0。** `slowlogMeasureCpu` 在 `start == NULL` 或 `measured < CPU_STARVATION_MIN_OFFCPU_US`（10000 微秒）时直接留 0；被阻塞的命令走 `updateStatsOnUnblock`，固定传 `NULL`，也留 0；`off-cpu-tracking`（默认 1）关掉后同样为 0。不把「没测」伪装成「测出 0」以外的东西，读取方能靠 0 识别未测量。

**阻塞命令两段记账。** `call()` 里看到 `CLIENT_BLOCKED` 就不记慢日志，把记账推给 `updateStatsOnUnblock`，后者把阻塞时长与回包时长并进 `total_cmd_duration`。这保证一条 `BLOCKING` 命令只产生一条条目、且耗时含等待。

**条目归属靠链表头尾而不是时间戳排序。** 插入永远是 `listAddNodeHead`，淘汰永远是 `listLast`，因此链表位置天然就是时间序，`SLOWLOG GET` 不需要排序。**架构推论：** 这套「头插 + 尾删」把容量控制与时间序合并成一个 O(1) 操作，是定长滚动日志最省的形态；代价是容量一旦被调小，`slowlogPushEntryIfNeeded` 的 `while` 循环要在下一次入账时才把积压裁掉，而不是在 `CONFIG SET` 当场裁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/slowlog.c` | `slowlogPushEntryIfNeeded` | 慢日志入账口：判阈值、建条目、头插、尾部淘汰到 `slowlog_max_len` |
| `src/slowlog.c` | `slowlogWouldLog` | 阈值与容量双开关判定，比较用 `>=` |
| `src/slowlog.c` | `slowlogCreateEntry` | 建条目：参数个数裁剪、单参数截断、共享对象复用与 `dupStringObject` 复制、填 id/时间/peerid/cname |
| `src/slowlog.c` | `slowlogFreeEntry` | 释放条目：逐参数 `decrRefCount`、释放 argv/peerid/cname |
| `src/slowlog.c` | `slowlogInit` | 建 `server.slowlog` 链表、`slowlog_entry_id` 归零、注册释放回调 |
| `src/slowlog.c` | `slowlogReset` | 从尾部逐条删除清空慢日志，供 `reset` 子命令使用 |
| `src/slowlog.c` | `slowlogCommand` | `SLOWLOG` 的 `help`/`reset`/`len`/`get` 分发与 8 元素回复 |
| `src/slowlog.c` | `slowlogMeasureCpu` | 主线程区间估算与模块线程直接取值，产出 `slowlogCpu` |
| `src/slowlog.c` | `slowlogReportOffCpu` | off-CPU 超限计数与告警，判据是 `off_min` 与 10000 微秒门槛 |
| `src/slowlog.c` | `offCpuCheckEventLoopCycle` | 事件循环整轮 off-CPU 检查，由 `beforeSleep` 调用并扣除 AOF 写盘时间 |
| `src/slowlog.c` | `cpuSampleTake` | 取墙钟、用户+系统 CPU、自愿/非自愿上下文切换样本 |
| `src/slowlog.c` | `CPU_STARVATION_MIN_OFFCPU_US` | 常量 10000：低于该微秒数的命令不采样、off-CPU 不报告 |
| `src/slowlog.h` | `slowlogEntry` | 条目结构：argv/argc/id/duration/time/cname/peerid/cmd_argc/cpu_duration |
| `src/server.c` | `slowlogPushCurrentCommand` | 慢日志入口校验：`CMD_SKIP_SLOWLOG`、`slowlogWouldLog`、条目统计与 off-CPU 告警 |
| `src/server.c` | `call` | 命令执行后按 `update_command_stats` 与 `CLIENT_BLOCKED` 调用慢日志入口 |
| `src/server.c` | `initServer` | 启动时调 `slowlogInit` |
| `src/server.c` | `genRedisInfoString`（`Stats` 段） | 输出 `slowlog_commands_count` 与耗时汇总、off-CPU 事件计数 |
| `src/server.c` | `genRedisInfoStringCommandStats` | 在 `c->slowlog_count > 0` 时输出每命令慢日志统计 |
| `src/server.c` | `resetServerStats` | `CONFIG RESETSTAT` 清除全局慢日志统计 |
| `src/server.c` | `resetCommandTableStats` | `CONFIG RESETSTAT` 清除每命令慢日志统计 |
| `src/blocked.c` | `updateStatsOnUnblock` | 阻塞命令解除阻塞后统一调用慢日志入口，耗时含阻塞与回包，CPU 起点为 `NULL` |
| `src/config.c` | `createLongLongConfig`（`slowlog-log-slower-than`） | 注册阈值配置：默认 10000 微秒，范围 -1..LLONG_MAX |
| `src/config.c` | `createULongConfig`（`slowlog-max-len`） | 注册容量配置：默认 128，范围 0..LONG_MAX |
| `src/config.c` | `createIntConfig`（`slowlog-entry-max-argc`） | 注册参数个数上限：默认 32，范围 2..INT_MAX |
| `src/config.c` | `createULongConfig`（`slowlog-entry-max-string-len`） | 注册单参数长度上限：默认 128 字节，范围 1..LONG_MAX |
| `src/config.c` | `configResetStatCommand` | `RESETSTAT` 的触发点，串起 `resetServerStats` 与 `resetCommandTableStats` |
| `src/latency.c` | `createLatencyReport` | 依据阈值/容量是否关闭给出 slowlog 相关的延迟诊断建议 |
| `src/server.h` | `CMD_SKIP_SLOWLOG` | 命令标志 `1ULL<<12`：带此标志的命令不记慢日志 |
| `src/adlist.c` | `listAddNodeHead` / `listDelNode` | 条目头插与尾部淘汰的落地实现，删除时回调 `slowlogFreeEntry` |

## 与主核 / AI 的关系

慢查询日志的可复用形态是「阈值 + 定长环形缓冲 + 读取而落盘」：判定、插入、淘汰都在入账口一个函数里完成，读取与清空各自独立，因此这套结构能直接搬进任何需要「保留最近 N 条超阈事件」的内核。更值得抄的是 CPU 时间的记账方式——主线程用滚动检查点做差、只在慢命令上付出一次系统调用，而把「未测量」显式编码为 0，而不是伪造精度。**架构推论：** 主核若要做慢请求档案，同样应把「谁触发记账」与「记账成本」分开设计：快路径不采样、慢路径才补齐，并把补齐失败（阻塞、AOF 加载、下采样）编码成可区分的空值，否则统计字段会退化成无法归因的噪声。

## 参考

- [Redis 源码：slowlog.c](https://github.com/redis/redis/blob/unstable/src/slowlog.c)
- [Redis 源码：server.c（慢日志入口与 INFO 输出）](https://github.com/redis/redis/blob/unstable/src/server.c)
- [Redis 源码：blocked.c（阻塞命令解除后的记账）](https://github.com/redis/redis/blob/unstable/src/blocked.c)
- [Redis 文档：SLOWLOG GET](https://redis.io/docs/latest/commands/slowlog-get/)
- [Redis 文档：SLOWLOG LEN](https://redis.io/docs/latest/commands/slowlog-len/)
- [Redis 文档：SLOWLOG RESET](https://redis.io/docs/latest/commands/slowlog-reset/)
- [Redis 文档：配置项参考](https://redis.io/docs/latest/operate/oss_and_stack/management/config/)
