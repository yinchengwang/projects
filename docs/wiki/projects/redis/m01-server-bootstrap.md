---
project: redis
feature: server-bootstrap
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [initServerConfig, initServer]
---

# 服务端启动装配

## 一句话职责

`initServerConfig` 把配置系统的默认值写进全局 `server` 结构，`initServer` 按固定顺序建出数据库、共享对象、事件循环、定时器与钩子，让进程具备进入 `aeMain` 的条件。

## 机制是什么

启动装配是一条**单向的线性序列**，除 fd 预算会下调 `maxclients` 之外没有回头步骤，其余任一步失败就 `exit(1)` 或 `serverPanic`。序列分五段：

1. **进程级准备**（`main` 前半段）：设随机种子、`crc64_init()`、哈希种子、`checkForSentinelMode` 判定运行模式。
2. **默认配置装配**（`initServerConfig`）：`initConfigValues()` 注册标准配置表，随后逐字段给出初值，最后建命令表。
3. **配置读入**（`loadServerConfig`）：配置文件、stdin、`--` 命令行选项按优先级叠加，覆盖第 2 段的默认值。
4. **资源与事件循环装配**（`initServer` → `initListeners` → `InitServerLast`）：数据库、共享对象、`aeEventLoop`、`serverCron` 定时器、睡前/睡后钩子。
5. **进入主循环**（`aeMain`）：先加载持久化数据（`loadDataFromDisk`），再接受连接。

分界很清楚：本页只讲「把进程装配到可运行」。`serverCron` 在这里只被**注册**，它每轮做什么属于运行期定时任务；`prepareForShutdown` 属于关停流程；两者都不在本页范围。

## 怎么实现的

### 一、进程级准备（`main`）

`tzset()` 先填好 `timezone` 全局（`initServerConfig` 里的 `getTimeZone()` 读的就是它）。随后 `zmalloc_set_oom_handler(redisOutOfMemoryHandler)` 挂上 OOM 回调，`srand` / `srandom` / `init_genrand64` 都用 `time(NULL) ^ getpid() ^ tv.tv_usec` 做种子——源码注释的说明是：容器环境下 `time()` 与 `getpid()` 存在相同取值的场景，靠微秒位区分。接着 `crc64_init()`、`umask(server.umask = umask(0777))` 存取当前掩码、`getRandomBytes` 取 16 字节喂给 `dictSetHashFunctionSeed`。

顺序上有一处关键的先后关系：`checkForSentinelMode(argc, argv, exec_name)` 在 `initServerConfig()` **之前**调用，因为 sentinel 模式的默认值走 `initSentinelConfig()` / `initSentinel()` 另一条路径。`initServerConfig()` 之后紧跟 `bitroarInit()`、`ACLInit()`、`moduleInitModulesSystem()`、`connTypeInitialize()`、`keyMetaInit()`；`ACLInit` 的注释写明必须尽早，因为建客户端与网络代码依赖 ACL。

`argv[0]` 的绝对路径与完整参数表被存进 `server.executable` / `server.exec_argv`，供 `restartServer` 用 `execve` 原地重启。

### 二、默认配置装配（`initServerConfig`）

第一句就是 `initConfigValues()`：它在 `config.c` 里遍历 `static_configs` 表，逐项调 `interface.init` 并把配置注册进字典，所以「默认值」的真正来源是 `config.c` 里各 `createXxxConfig` 的默认参数——`port` 为 6379、`databases` 为 16、`maxclients` 为 10000、`io-threads` 为 1、`maxmemory` 为 0、`maxmemory-clients` 为 0、`appendonly` 为 0、`hz` 为 `CONFIG_DEFAULT_HZ`。

之后的初值按源码顺序：

- **时间与身份**：`updateCachedTime(1)` 与 `server.cmd_time_snapshot = server.mstime`；`getRandomHexChars(server.runid, CONFIG_RUN_ID_SIZE)` 取 40 字符 runid 再补 `'\0'`；`changeReplicationId()` / `clearReplicationId2()`。
- **`hz` 单独说明**：赋 `CONFIG_DEFAULT_HZ`（10），源码注释写明「尽早初始化，即使后面读配置会更新它，因为在服务器初始化完成之前这个值就会被使用」。
- **`defrag_frag_cache.cronloops = -1`**：源码注释写明这是「Mark the defrag fragmentation cache as stale」，即把碎片整理缓存标记为过期。
- **机器与进程属性**：`arch_bits` 按 `sizeof(long) == 8` 定 64 或 32；`page_size = sysconf(_SC_PAGESIZE)`；`next_client_id = 1`（客户端 ID 从 1 起）。
- **监听地址**：`bindaddr_count = CONFIG_DEFAULT_BINDADDR_COUNT`（2），把 `CONFIG_DEFAULT_BINDADDR`（`"*"` 与 `"-::*"`）两条 `zstrdup` 进 `server.bindaddr`；`server.listeners` 数组整体 `memset` 成 0。
- **持久化基线**：`aof_state = AOF_OFF`；`resetServerSaveParams()` 后逐条 `appendServerSaveParams(configDefaultSaveParams[i])`，默认表是 `{3600 秒, 1 次}`、`{300 秒, 100 次}`、`{60 秒, 10000 次}` 三条，长度由 `configDefaultSaveParamsLen` 给出。
- **延迟百分位**：`latency_tracking_info_percentiles_len` 取 `configDefaultLatencyTrackingInfoPercentilesLen`，内容 `memcpy` 自 `configDefaultLatencyTrackingInfoPercentiles`（50.0、99.0、99.9）。
- **复制**：`masterport = 6379`、`repl_state = REPL_STATE_NONE`、`repl_syncio_timeout = CONFIG_REPL_SYNCIO_TIMEOUT`、`master_repl_offset = 0`。
- **限值表**：`client_obuf_limits` 按 `CLIENT_TYPE_OBUF_COUNT`（3）填 `clientBufferLimitsDefaults`；`oom_score_adj_values` 按 `CONFIG_OOM_COUNT`（3）填 `configOOMScoreAdjValuesDefaults`。
- **浮点常量**：用 `R_Zero = 0.0` 派生 `R_PosInf = 1.0/R_Zero`、`R_NegInf = -1.0/R_Zero`、`R_Nan = R_Zero/R_Zero`，供命令参数解析的 inf/nan 判断使用。
- **命令表**：最后才建 `server.commands = dictCreate(&commandTableDictType)` 与 `server.orig_commands = dictCreate(&commandTableDictType)`，再调 `populateCommandTable()`。注释给出的理由：命令表属于「初始配置」，因为 `redis.conf` 的 `rename-command` 会改命令名，所以它要在配置加载之前就存在。

`populateCommandTable` 的循环以 `declared_name == NULL` 作为结束哨兵；每条先 `sdsnew(declared_name)` 写 `fullname`，`populateCommandStructure` 返回 `C_ERR` 时 `continue` 跳过该条，否则同一个指针分别 `dictAdd` 进 `commands` 与 `orig_commands`，两次结果都用 `serverAssert(retval == DICT_OK)` 断言。双表的意义是：`commands` 会随 `rename-command` 改名，`orig_commands` 始终保有原名。

### 三、配置读入（`main` 后半段与 `loadServerConfig`）

`argv[1]` 不以 `-` 开头时被当作配置文件路径：`getAbsolutePath` 规范化后写回 `server.configfile` 与 `server.exec_argv[1]`，参数索引跳到 2。`--name value` 形式的选项被拼成 sds 文本，语义上「概念性地追加到配置文件之后」；源码注释给的总优先级是 File → stdin → 显式选项，最后一条生效。调用是 `loadServerConfig(server.configfile, config_from_stdin, options, CONFIG_SOURCE_MAIN)`。

`loadServerConfig` 里 `fopen` 失败即 `serverLog(LL_WARNING, ...)` 后 `exit(1)`；文件名含 `*`、`?`、`[` 时走 `glob`，匹配不到文件就当该行不存在（注释说明这是为了让空的 `conf.d` 目录可用），匹配到则逐个读入。`loadServerConfigFromString` 里有一条硬性收敛：`cluster_enabled` 且 `dbnum > 1` 时把 `dbnum` 改成 1 并打警告，因为集群模式只允许一个库。

### 四、`initServer` 的装配顺序

**信号与线程环境**：`signal(SIGHUP, SIG_IGN)`、`signal(SIGPIPE, SIG_IGN)`、`setupSignalHandlers()`、`ThreadsManager_init()`、`makeThreadKillable()`；`syslog_enabled` 时 `openlog`。

**从配置派生状态**：`aof_state = aof_enabled ? AOF_ON : AOF_OFF`、`fsynced_reploff = aof_enabled ? 0 : -1`、`hz = server.config_hz`、`pid = getpid()`、`in_fork_child = CHILD_TYPE_NONE`、`main_thread_id = pthread_self()`。

**客户端容器**：`errors = raxNew()`、`errors_enabled = 1`；`clients`、`clients_to_close`、`slaves`、`monitors`、`clients_pending_write`、`clients_pending_read`、`unblocked_clients`、`ready_keys`、`tracking_pending_keys`、`pending_push_messages`、`clients_waiting_acks`、`postponed_clients` 全部 `listCreate()`；`clients_index = raxNewEx(0, NULL, sizeof(uint64_t))`，`clients_timeout_table = raxNewEx(0, NULL, sizeof(uint64_t) * 2)`（键长为 8 字节大端 mstime 加 8 字节客户端 ID，这个键长约束的来源记在 `timeout.c` 的 `CLIENT_ST_KEYLEN` 约定里）。

**传播与暂停状态**：`allowed_propagate_targets = PROPAGATE_AOF|PROPAGATE_REPL`、`slaveseldb = -1`（强制首个 SELECT）、`paused_actions = 0`。

**回复与内存策略**：`reply_buffer_peak_reset_time = REPLY_BUFFER_DEFAULT_PEAK_RESET_TIME`（5000，即 5 秒）、`reply_buffer_resizing_enabled = 1`、`reply_copy_avoidance_enabled = 1`；`memory_tracking_enabled = server.key_memory_histograms || clusterSlotStatsEnabled(CLUSTER_SLOT_STATS_MEM)`，注释说明内存记账可在运行期关闭但**不能重新打开**，因此只能按启动配置一次性决定。

**区域与共享对象**：`setlocale(LC_COLLATE, server.locale_collate)` 返回 NULL 就 `serverLog(LL_WARNING)` 后 `exit(1)`——没有「用默认 locale 继续跑」的降级。随后 `hashTemplatesInit()`、`createSharedObjects()`、`adjustOpenFilesLimit()`。

`createSharedObjects()` 建的是全进程复用对象：`+OK`、`$0`、`:0`、`:1`、`*0`、`+PONG`、`+QUEUED` 等回复，`-WRONGTYPE`、`-ERR` 等错误，以及 `OBJ_SHARED_INTEGERS`（10000）个 0..9999 的整型对象和 `OBJ_SHARED_BULKHDR_LEN`（32）组 `*`、`$`、`%`、`~` 长度头。

**文件描述符预算**：`adjustOpenFilesLimit()` 以 `maxfiles = server.maxclients + CONFIG_MIN_RESERVED_FDS`（32）为目标。`getrlimit` 失败时走降级：把 `maxclients` 直接改成 `1024 - CONFIG_MIN_RESERVED_FDS`，即按假定值 1024 反推。`setrlimit` 设不上时以 `decr_step = 16` 反复递减重试；最终还是低于 `maxfiles` 就把 `maxclients` 压成 `bestlimit - 32`；若 `bestlimit <= 32` 则打「ulimit 不够」日志并 `exit(1)`。装配期的调用方是 `initServer`；同一函数也被 `maxclients` 配置项的更新回调 `updateMaxclients` 复用。

**时钟与事件循环**：`monotonicInit(monotonicLogCallback)` 打印时钟来源，`cpuSampleTake(&server.cpu_checkpoint)` 取首个 CPU 窗口；`server.el = aeCreateEventLoop(server.maxclients + CONFIG_FDSET_INCR)`，`CONFIG_FDSET_INCR` 定义为 `CONFIG_MIN_RESERVED_FDS + 96`，即 32+96=128 的额外槽位；返回 NULL 就 `exit(1)`。

**数据库**：`server.db = zmalloc(sizeof(redisDb) * server.dbnum)`。逐库装配前先定两个局部量：`slot_count_bits = 0`、`flags = KVSTORE_ALLOCATE_DICTS_ON_DEMAND`；`cluster_enabled` 时改成 `slot_count_bits = CLUSTER_SLOT_MASK_BITS`（14）、`flags |= KVSTORE_FREE_EMPTY_DICTS`。于是每库的 `keys` / `expires` 用 `kvstoreCreate`、`blessed_keys` 用 `blessedKvstoreCreate`、`subexpires` 用 `estoreCreate`，而 `blocking_keys`、`blocking_keys_unblock_on_nokey`、`stream_claim_pending_keys`、`stream_idmp_keys`、`ready_keys`、`watched_keys` 走 `dictCreate`。`evictionPoolAlloc()` 随后建 LRU 候选池。

**订阅**：`pubsub_channels` 与 `pubsubshard_channels` 都用 `kvstoreCreate`，`pubsub_patterns` 用 `dictCreate`；源码注释专门解释 `pubsub_channels` 被选成 kvstore（内含一个 dict，看起来别扭）是为了让类型与必须用 kvstore 的 `pubsubshard_channels` 一致，代码更干净。

**统计与计数器**：`cronloops = 0`、`in_exec = 0`、`child_pid = -1`、`lastsave = time(NULL)`（启动即视为已保存）、`dirty = 0`；`resetServerStats()` 后再单独把 `stat_starttime = time(NULL)`、`stat_peak_memory = 0`、`stat_peak_memory_time = server.unixtime` 设回去，注释写明这几个统计「不想被 reset」。

**命令池**：`cmd_pool.capacity = PENDING_COMMAND_POOL_SIZE`（16），并按该容量 `zmalloc` 指针数组。

**定时器与钩子**：`aeCreateTimeEvent(server.el, 1, serverCron, NULL, NULL)` 失败即 `serverPanic("Can't create event loop timers.")`；`aeCreateFileEvent(server.el, server.module_pipe[0], AE_READABLE, modulePipeReadable, NULL)` 失败即 `serverPanic`（这条事件是模块线程唤醒事件循环的入口）；`aeSetBeforeSleepProc(server.el, beforeSleep)` 与 `aeSetAfterSleepProc(server.el, afterSleep)`，注释指出必须在加载持久化之前装好，因为 `processEventsWhileBlocked` 会用它们。

**32 位兜底**：`arch_bits == 32 && maxmemory == 0` 时把 `maxmemory` 设成 `3072LL * (1024 * 1024)` 并选 `MAXMEMORY_NO_EVICTION`，日志原文写明这是为了避免 32 位实例因内存耗尽而无谓崩溃。

**脚本、慢日志与 ACL**：`luaEnvInit()`、`scriptingInit(1)`；`functionsInit()` 返回 `C_ERR` 就 `serverPanic`；`slowlogInit()`、`latencyMonitorInit()`、`ACLUpdateDefaultUserPassword(server.requirepass)`、`applyWatchdogPeriod()`。

**按需才建的分桶**：只有 `maxmemory_clients != 0` 才调 `initServerClientMemUsageBuckets()`，它分配 `CLIENT_MEM_USAGE_BUCKETS` 个桶，该常量定义为 `1 + CLIENT_MEM_USAGE_BUCKET_MAX_LOG - CLIENT_MEM_USAGE_BUCKET_MIN_LOG`，即 1+33-15=19 个；桶内各建一条 `listCreate()`。

**收尾**：`prefetchCommandsBatchInit()`。

### 五、`initListeners` 与 `InitServerLast`

`initListeners` 按配置填 `server.listeners[conn_index]`——数组长度是 `CONN_TYPE_MAX`（8，注释说「8 个足够扩展」）。三种监听各自的条件：`port != 0` 填 `CONN_TYPE_SOCKET`；`tls_port` 或 `tls_replication` 或 `tls_cluster` 时先 `connectionTypeTls()`（拿到 NULL 就日志 + `exit(1)`），再 `connTypeConfigure(ct_tls, &server.tls_ctx_config, 1)`，失败同样 `exit(1)`；`unixsocket != NULL` 时填 `CONN_TYPE_UNIX`，并把 `priv` 指向 `server.unixsocketperm`。

建监听循环里 `connListen(listener)` 失败、或 `createSocketAcceptHandler(listener, connAcceptHandler(listener->ct))` 返回非 `C_OK`，都 `goto listener_error`；该标签的收尾是 `closeListeningSockets(unix_socket_created)` 后 `exit(1)`。`listen_fds` 累计为 0（即一个监听都没建成）也走同一标签，日志是「Configured to not listen anywhere, exiting.」。

`InitServerLast` 的位置有源码注释解释：线程必须最后建，因为 `ld.so` 存在 TLS 初始化与 `dlopen` 冲突的竞争 bug（注释附了 sourceware 的 bug 编号链接）。它做四件事：`bioInit()`、`initThreadedIO()`、`set_jemalloc_bg_thread(server.jemalloc_bg_thread)`、`initial_memory_usage = zmalloc_used_memory()`。

### 六、`main` 的收尾顺序与进入主循环

`initServer()` 之后依次是：`background || server.pidfile` 时 `createPidFile()`、`redisSetProcTitle`、`redisAsciiArt`、`checkTcpBacklogSettings`；`cluster_enabled` 时先 `clusterCommonInit()` 再 `clusterInit()`（注释说明前者初始化后者要用的 slot 统计）；非 sentinel 模式才跑 `moduleInitModulesSystemLast()` / `moduleLoadInternalModules()` / `moduleLoadFromQueue()`；`ACLLoadUsersAtStartup()`；`initListeners()`；`cluster_enabled` 时 `clusterInitLast()`；`InitServerLast()`。

非 sentinel 分支再依次 `aofLoadManifestFromDisk()`、`loadDataFromDisk()`、`aofSetupAfterPreloadFile()`、`aofOpenIfNeededOnServerStart()`、`aofDelHistoryFiles()`、`applyAppendOnlyConfig()`，然后逐个监听器打「Ready to accept connections <type>」，`SUPERVISED_SYSTEMD` 时向 systemd 报 `READY=1`，最后 `warnAboutInsecureConfig()`。收尾是 `redisSetCpuAffinity(server.server_cpulist)`、`setOOMScoreAdj(-1)`、`aeMain(server.el)`，退出循环后 `aeDeleteEventLoop(server.el)` 并 `return 0`。

## 关键取舍

**默认值先落地，再被配置覆盖。** `main` 严格按 `initServerConfig()` → `loadServerConfig()` 调用，所以「默认值」是配置系统的注册结果（`initConfigValues()` 遍历 `static_configs`），不是 `redis.conf` 里的文本。好处是没给配置文件也能跑；代价是同一字段有两处赋值——`hz` 在 `initServerConfig` 里给 `CONFIG_DEFAULT_HZ`、在 `initServer` 里又改成 `server.config_hz`，源码用注释明确了这个双重赋值的理由。

**早期错误没有回退路径。** `setlocale` 失败、`aeCreateEventLoop` 返回 NULL、TLS 配置失败、`connListen` 失败、无监听端口，全部直接 `exit(1)`；定时器与模块管道注册失败用 `serverPanic`。「降级」只发生在 `adjustOpenFilesLimit()`：`getrlimit` 失败按 1024 反推 `maxclients`，`setrlimit` 反复失败按 `decr_step = 16` 递减重试，最终落到 `bestlimit - CONFIG_MIN_RESERVED_FDS`。这个降级有两个真实调用方：`initServer` 在装配阶段调一次；`updateMaxclients`（`maxclients` 配置项的更新回调，由 `createUIntConfig("maxclients", ...)` 注册）在运行期改配置时再调一次，并比对 `server.maxclients` 是否被这次降级改动，改了就返回配置错误。

**32 位实例的兜底是主动限写。** `arch_bits == 32` 且 `maxmemory == 0` 时写死 3GB 并配 `MAXMEMORY_NO_EVICTION`；源码注释给的唯一理由是「避免实例因内存耗尽而无谓崩溃」。**架构推论：** 在 `noeviction` 策略下超限的后果落在写命令的错误返回上，而不是内核 OOM 杀进程——这是该兜底的实际效果面。

**装配顺序本身是依赖声明。** `beforeSleep` / `afterSleep` 必须在 `loadDataFromDisk` 之前装好（注释点名 `processEventsWhileBlocked` 依赖它们），`bioInit` 与 `initThreadedIO` 必须在模块加载之后（`ld.so` 竞争），命令表必须在配置解析之前（`rename-command` 要能改它）。**架构推论：** 这些顺序无法靠函数内部的局部变量表达，只能靠 `main` 里的调用次序与注释维持，因此改动装配顺序比改动单个初始化函数更容易引入启动期故障。

**架构推论：** 命令表拆成 `commands` 与 `orig_commands` 两张字典，是把「配置可改写的名字」与「编译期声明名」在数据结构层分开；两张表都指向同一批 `struct redisCommand`，所以改名只影响查找键、不影响命令实现。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `main` | 串起进程级准备、配置读入、`initServer`、监听、加载数据与 `aeMain` 的整个次序 |
| `src/server.c` | `initServerConfig` | 通过 `initConfigValues()` 落地配置默认值，逐字段给初值，最后建命令表与 `populateCommandTable` |
| `src/server.c` | `initServer` | 建客户端容器、共享对象、事件循环、各库 kvstore、定时器与睡前/睡后钩子，并做 fd 预算与 32 位兜底 |
| `src/server.c` | `initListeners` | 按 port / tls_port / unixsocket 填 `server.listeners` 并建监听，失败统一走 `listener_error` 关闭套接字后退出 |
| `src/server.c` | `InitServerLast` | 建 bio 线程与 IO 线程、设 jemalloc 后台线程、记录启动内存基线 |
| `src/server.c` | `adjustOpenFilesLimit` | 按 `maxclients + CONFIG_MIN_RESERVED_FDS` 求 fd 上限，设不上时按 16 递减并下调 `maxclients` |
| `src/server.c` | `createSharedObjects` | 建共享回复、错误、整型对象与各类长度头 |
| `src/server.c` | `populateCommandTable` | 遍历 `redisCommandTable` 同时写入 `commands` 与 `orig_commands` |
| `src/config.c` | `loadServerConfig` | 读配置文件（支持 glob）、拼接 stdin 与命令行选项后交给配置解析 |
| `src/config.c` | `initConfigValues` | 遍历 `static_configs` 调各项 `interface.init` 并注册到配置字典 |
| `src/config.c` | `configDefaultSaveParams` | 默认存盘三档：3600 秒/1 次、300 秒/100 次、60 秒/10000 次 |

## 与主核 / AI 的关系

装配顺序的约束形态可以直接借用：主核若也有「配置→容器→事件循环→后台线程→加载数据」的启动链，应把「谁必须在谁之前」写成一份显式清单并配注释，因为这类依赖无法靠代码结构自证。Redis 在启动期对每个失败点选择直接退出而非带病运行，这一点与主核的分层门禁同源——启动期错误放大到运行期会污染数据。**架构推论：** AI 推理服务若把模型加载、线程池、事件循环都塞进同一段启动代码，会重演 Redis 用注释与调用次序维持顺序的脆弱性，更稳的做法是把每段装配声明成有序阶段。

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c)
- [Redis 源码：config.c](https://github.com/redis/redis/blob/unstable/src/config.c)
- [Redis 文档：Redis configuration](https://redis.io/docs/latest/operate/oss_and_stack/management/config/)
- [Redis 文档：管理 Redis](https://redis.io/docs/latest/operate/oss_and_stack/management/admin/)
