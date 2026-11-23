---
project: redis
feature: command-dispatch-call
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [call, processCommand, alsoPropagate]
---

# 命令执行与传播登记

## 一句话职责

`processCommand` 负责执行前的全部预检并决定放行还是拒绝，`call` 负责真正调用命令实现并把耗时、统计与慢日志记账，`alsoPropagate` 负责把写命令登记进待传播队列。

## 机制是什么

一条命令从「已经在客户端的 argv 里」到「数据集被改过、并且这个改动被登记为要写进 AOF 与复制流」，中间隔着三层：

- `processCommand` 是门卫。它按固定顺序跑完一串预检——命令是否存在、参数个数、认证、ACL、集群槽位归属、内存上限、磁盘可写、副本数量、只读副本、事务上下文、Pub/Sub 上下文、载入状态、繁忙状态、暂停状态——任何一项不通过就当场回复错误并中止；全部通过才把命令交给执行层。
- `call` 是执行层。它调用命令的实现函数指针，测量耗时，把结果记进命令级统计、慢日志、延迟直方图、热点键统计，并判断这条命令到底要不要传播。
- `alsoPropagate` 是传播登记处。写命令不是直接写进 AOF 或复制流，而是先登记到 `server.also_propagate` 这个操作队列里，等最外层执行单元结束时统一刷出。

本页只讲这三层的编排；命令表查找本身、协议解析与回复缓冲、AOF 文件本身的写入格式都不在本页。

## 怎么实现的

**`processCommand` 的入口约束。** 函数开头断言 `!server.in_exec` 且 `!scriptIsRunning()`（`scriptIsTimedout()` 时例外），源码注释解释：「Both EXEC and scripts call call() directly so there should be no way in_exec or scriptIsRunning() is 1」——事务与脚本直接调 `call()`，不经过这一层。

**重放识别。** `client_reprocessing_command = c->cmd ? 1 : 0`。源码注释写明：「in case we are starting to Processcommand and we already have a command we assume this is a reprocessing of this command, so we do not want to perform some of the actions again」。被阻塞后重新执行的命令会跳过模块命令过滤器与一整轮预检。

**查找命令。** 优先用 `c->lookedcmd`（解析阶段已经查好的结果）；没有才走 `isCommandReusable` 复用上一条命令或 `lookupCommand` 查表——查表细节属于命令表那一页。查不到时有一个安全兜底：若 `argv[0]` 是 `"host:"` 或 `"post"`（HTTP 代理误连到 Redis 端口），调 `securityWarningCommand` 并返回 `C_ERR`。随后 `CMD_INTERNAL` 命令对非内部连接不可见，源码注释说明「masters and AOF loads are implicitly internal」。

**预检清单**（按源码顺序，每一项失败都是「回复错误 + `return C_OK`」，即命令被拒绝但连接继续服务）：

1. `commandCheckExistence` 与 `commandCheckArity`——命令存在性与参数个数。
2. `CMD_PROTECTED` 命令（`DEBUG`、`MODULE`）需要配置项放行：`enable-debug-command` 与 `enable-module-command`。
3. 认证：`authRequired(c)` 为真且命令没有 `CMD_NO_AUTH` 标志时拒绝。
4. 事务：`CLIENT_MULTI` 且命令带 `CMD_NO_MULTI` 标志时拒绝，报 `"Command not allowed inside a transaction"`。
5. ACL：`ACLCheckAllPerm` 不通过时报 `-NOPERM`。
6. 集群重定向：`getNodeByQuery` 算出目标节点不是自己就 `clusterRedirectClient`，并把 `c->cmd->rejected_calls` 加一。跳过重定向的条件写在源码注释里：发送方是自己的主节点，或者命令没有键参数（`key_specs_num == 0` 且没有 `CMD_MOVABLE_KEYS`，`EXEC` 除外）。
7. 集群兼容性采样：`cluster_compatibility_sample_ratio` 打开且未启用集群时抽样检查键是否同槽，跨槽则 `server.stat_cluster_incompatible_ops` 加一。
8. `evictClients()`——按客户端内存驱逐；若当前客户端把自己驱逐掉了，直接返回 `C_ERR` 中止处理。
9. maxmemory：`performEvictions()` 返回 `EVICT_FAIL` 且命令带 `CMD_DENYOOM` 标志时拒绝；随后把结果存进 `server.pre_command_oom_state`，注释说明是为了不让脚本栈和参数的内存干扰这个判断。
10. 磁盘写错误：`writeCommandsDeniedByDiskError()` 非空时拒绝写命令与 `PING`。主节点客户端是例外——按 `repl_ignore_disk_write_error` 决定是 `serverPanic` 还是打日志，日志间隔由源码里的 `log_interval_ms = 10000` 控制。
11. `checkGoodReplicasStatus()`——`min-replicas-to-write` 不满足时拒绝写命令。
12. 只读副本：`repl_slave_ro` 打开且自身是副本时拒绝写命令。
13. 槽位迁移期间主节点下发写命令：`asmGetTrimmingSlotForCommand` 命中待清理的槽位时，用 `blockPostponeClientWithType(c, BLOCKED_POSTPONE_TRIM)` 阻塞主连接，而不是拒绝。
14. Pub/Sub 上下文：RESP2 下只允许 `(P|S)SUBSCRIBE`、`(P|S)UNSUBSCRIBE`、`PING`、`QUIT`、`RESET`，其余一律拒绝。
15. 副本链路断开且 `repl_serve_stale_data` 为 0 时，拒绝没有 `CMD_STALE` 标志的命令。
16. 载入中：`server.loading` 时拒绝没有 `CMD_LOADING` 标志的命令；`server.async_loading` 时拒绝带 `CMD_NO_ASYNC_LOADING` 标志的命令。
17. 繁忙（正在跑脚本或模块）：只允许带 `CMD_ALLOW_BUSY` 标志的命令，否则回 `-BUSY` 类错误。
18. 副本客户端（`CLIENT_SLAVE`）不允许碰键空间，报 `"Replica can't interact with the keyspace"`。
19. 服务被暂停：`isPausedActions` 命中全客户端暂停或写客户端暂停时，用 `blockPostponeClient` 挂起而非拒绝。

**事务分流。** 最后一步：客户端在事务中且命令不是 `EXEC`、`DISCARD`、`MULTI`、`WATCH`、`QUIT`、`RESET` 时，走 `queueMultiCommand` 排队并回 `+QUEUED`；否则 `call(c, CMD_CALL_FULL)` 真正执行，执行完若 `server.ready_keys` 非空则调 `handleClientsBlockedOnKeys`。这里用的是 `src/server.h` 里的定义 `#define CMD_CALL_FULL (CMD_CALL_PROPAGATE)`——注意 `call()` 上方那段注释把 `CMD_CALL_FULL` 描述成「Alias for SLOWLOG|STATS|PROPAGATE」，与当前头文件的实际定义已经不一致，按头文件为准。

**`call` 的准备阶段。** 几件事按顺序发生：

- `server.executing_client` 指向当前客户端，函数末尾恢复。
- `update_command_stats = !isAOFLoadingContext()`。`isAOFLoadingContext` 是 `src/server.c` 里的宏，判据是当前客户端是不是 AOF 客户端。源码注释说：「When call() is issued during loading the AOF we don't want commands called from module, exec or LUA to go into the slowlog or to populate statistics」——这是 AOF 载入期的统计退化路径。
- 清掉 `CLIENT_FORCE_AOF`、`CLIENT_FORCE_REPL`、`CLIENT_PREVENT_PROP` 三个标志，注释说这些标志「must be set by the command on demand」，即由命令实现自己在执行期按需设置。
- 记下 `dirty = server.dirty` 与 `old_master_repl_offset = server.master_repl_offset`，前者用来判断数据集是否被改过，后者用来判断复制偏移是否变化。
- 计时优先用单调钟：`monotonicGetType() == MONOTONIC_CLOCK_HW` 时用 `getMonotonicUs()`，否则回落到 `ustime()`。源码注释解释了动机：「In order to avoid performance implication due to querying the clock using a system call 3 times, we use a monotonic clock, when we are sure its cost is very low, and fall back to non-monotonic call otherwise」。缓存时间的刷新有两个阈值：单调钟与上次刷新点相差超过 10 微秒，或累计命令数超过 25 次，就调一次 `updateCachedTime(0)`。
- `enterExecutionUnit(1, call_timer)` 开一个执行单元；`c->flags |= CLIENT_EXECUTING_COMMAND`。
- 记 `ops_before = server.also_propagate.numops`，作为嵌套调用的分界点。

**`call` 的传播目标收窄。** `getPropagateTargetsForCall(c, flags)` 按调用方的 flags 与客户端上的 `CLIENT_MODULE_PREVENT_AOF_PROP` / `CLIENT_MODULE_PREVENT_REPL_PROP` 算出 `call_targets`，然后 `server.allowed_propagate_targets = prev_targets & call_targets`。源码注释给了一个三步例子：模块一的命令不带复制地调模块二的命令，模块二的命令内部又带复制地调 `INCR`，最外层用 `RM_ReplicateVerbatim` 传播——期望副本只看到模块一的命令，看不到那个 `INCR`。注释还说明这个「执行期收窄」正是让 `alsoPropagate()` 排队的效果命令（`SPOP` 传播成 `SREM`、`RM_Replicate()` 等）也能遵守 Lua 的 `redis.set_repl()` 与选择性 `RM_Call()` 的原因。真正执行命令的是 `c->cmd->proc(c)`，执行完立刻把 `server.allowed_propagate_targets` 恢复成 `prev_targets`。

**`call` 的记账阶段**（命令返回后）：

- 算 `duration`（单调钟差值或 `ustime()` 差值），累加到 `c->duration`。
- `dirty = server.dirty - dirty`，负数归零。
- `incrCommandStatsOnError(real_cmd, ERROR_COMMAND_FAILED)` 更新失败计数。
- 延迟采样：`CMD_FAST` 命令记 `"fast-command"` 事件，其余记 `"command"`；最外层调用（`server.execution_nesting == 0`）额外记 `EL_DURATION_TYPE_CMD`。
- 慢日志：`slowlogPushCurrentCommand`，但阻塞中的客户端不记——源码注释说「If the client is blocked we will handle slowlog when it is unblocked」。
- MONITOR 广播：`replicationFeedMonitors`，排除带 `CMD_SKIP_MONITOR` 或 `CMD_ADMIN` 的命令、重放中的命令以及 AOF 载入上下文。
- 命令级统计：`real_cmd->calls++`、`real_cmd->microseconds += c->duration`、延迟直方图 `updateCommandLatencyHistogram`（传入 `c->duration * 1000`，单位是纳秒）、`clusterSlotStatsAddCpuDuration`。
- 热点键统计：`hotkeyStatsPreCurrentCmd` 与 `hotkeyStatsUpdateCurrentCmd`。

**`call` 的传播登记。** 触发条件是四个都要满足：`flags & CMD_CALL_PROPAGATE`、客户端没有 `CLIENT_PREVENT_PROP`、命令不是 `EXEC`、命令不是模块命令（`CMD_MODULE`）。源码注释说明：EXEC 从不显式传播（由传播队列的隐式处理负责），模块命令自己负责传播。满足条件后算出 `propagate_flags`：

- `dirty` 非零 → 加上 `PROPAGATE_AOF` 与 `PROPAGATE_REPL`。
- 客户端带 `CLIENT_FORCE_REPL` → 加 `PROPAGATE_REPL`；带 `CLIENT_FORCE_AOF` → 加 `PROPAGATE_AOF`。这两个标志由 `forceCommandPropagation` 设置，语义是「即使没改数据集也要传播」。
- 客户端带 `CLIENT_PREVENT_REPL_PROP` → 去掉 `PROPAGATE_REPL`；带 `CLIENT_PREVENT_AOF_PROP` → 去掉 `PROPAGATE_AOF`。
- 最后 `propagate_flags &= call_targets`，与本次调用的目标取交集。

只有 `propagate_flags != PROPAGATE_NONE` 时才调 `alsoPropagateEx(c->db->id, c->argv, c->argc, propagate_flags, duration)`——注意登记进去的是**客户端当前的 argv**，不是 `original_argv`。登记完成后 `call` 把三个传播标志恢复成进入时的状态，源码注释说明「since call() can be executed recursively」。

**`alsoPropagate` 的登记语义。** `alsoPropagate` 是 `alsoPropagateEx` 的薄封装，额外参数 `duration` 传 `PROP_DURATION_UNKNOWN`（`src/server.h` 里定义为 `-1`）。`alsoPropagateEx` 做三件事：

1. `target &= server.allowed_propagate_targets`——丢掉当前命令不允许到达的目标。源码注释说这是为了防止「命令的效果命令落进被 Lua 的 `redis.set_repl()` 或选择性 `RM_Call()` 排除掉的 AOF / 副本」。
2. `shouldPropagate(target)` 判定是否值得登记：`PROPAGATE_NONE` 或 `server.loading` 直接返回 0；带 AOF 位且 `server.aof_state != AOF_OFF` 返回 1；带复制位且自身不是副本、同时存在复制积压缓冲或副本客户端（或有槽位迁移在进行）返回 1。
3. 复制一份 argv 并对每个对象 `incrRefCount`，追加到 `server.also_propagate` 队列。源码注释说明「The function does not take a reference to the passed 'argv' vector, so it is up to the caller to release the passed argv」——函数自己加引用计数。

**队列什么时候刷出。** `call` 结尾调 `afterCommandEx`，其中 `postExecutionUnitOperationsEx` 在最外层（`server.execution_nesting` 为 0）时调 `propagatePendingCommands`，把队列里的每条操作按各自的 target 交给 `propagateNow`；`propagateNow` 才是真正调用 `feedAppendOnlyFile` 与 `replicationFeedSlaves` 的地方——那两步属于 AOF 与复制那一组，本页不展开。若队列里有多条操作且目标一致，`propagatePendingCommands` 会先发一对 `MULTI`/`EXEC` 把它们包成事务。

**旁路登记接口。** `alsoPropagateForced` 临时把 `server.allowed_propagate_targets` 置为 `PROPAGATE_AOF|PROPAGATE_REPL` 再调 `alsoPropagate`，源码注释说明它的用途：「To be used only for implicit changes the server decided to make by itself (expired or evicted keys, slots trimmed after a migration): those must always reach the AOF and the replicas, no matter what the command that happened to trigger them asked for」。真实调用点都能对上这条说明：`src/db.c` 的 `propagateDeletion` 把过期或被淘汰的键登记成 `DEL` 或 `UNLINK`；`src/t_set.c` 把集合成员的过期登记成 `SREM`；`src/t_hash.c` 把哈希字段的过期登记成 `HDEL`；`src/cluster_asm.c` 的 `propagateTrimSlots` 把槽位清理登记成一条 `TRIMSLOTS` 命令。

## 关键取舍

**源码事实：** 预检有明确固定的顺序，本页列出的十九项每一项失败要么「回错误拒绝」要么「挂起客户端」，两种处置方式按语义分开——能等就等（槽位 trim、暂停），不等就拒（ACL、内存、磁盘）。**源码事实：** 命令是否传播由四类因素共同决定：数据集是否被改（`dirty`）、客户端是否强制或禁止传播、命令是否本身特殊（`EXEC`、模块命令）、本次调用允许触达哪些目标。**源码事实：** 传播目标在**执行期**收窄而不是只在登记时收窄，效果命令与主命令因此遵守同一套限制。**源码事实：** AOF 载入期关闭统计与慢日志（`isAOFLoadingContext`）。**源码事实：** 复制与 AOF 的实际写出发生在执行单元结束时统一进行，而不是每条命令各写一次。

**架构推论：** 把「是否传播」与「怎么传播」分成登记与刷出两段，让一次命令可以派生多条传播操作（`SPOP` 拆成多条 `SREM`、模块的 `RM_Replicate`），而且这批操作能作为一个事务整体送达；代价是传播时序被推迟到执行单元末尾，崩溃时 AOF 与复制流的落点更靠近命令边界而非数据修改点。**架构推论：** 用 `dirty` 计数而不是逐命令声明「是否写」来决定传播，减少了命令实现的负担——代价是写命令实现里任何一次误改都会被当成有效改动而传播，这正是 `preventCommandPropagation` 这类显式禁止接口存在的原因。**架构推论：** 预检集中在一个函数里顺序执行，让「为什么这条命令被拒」只有一处答案；代价是这个函数已经很臃肿，任何新预检都得插进固定顺序里，且顺序本身成了隐式契约（例如磁盘错误检查在 maxmemory 之后）。**架构推论：** 上列的预检里混用「拒绝」和「挂起」两种语义，对主核这类多写者引擎来说更难处理——并发下「挂起」意味着要在锁外等待，而 Redis 的单线程让这件事退化成一次状态置位。**架构推论：** 统计与慢日志在 AOF 载入期关闭，是为了让重放不污染运行期指标；代价是重放期间排障看不到命令级耗时。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `processCommand` | 执行前的全部预检与分流，决定拒绝、挂起、排队还是执行 |
| `src/server.c` | `call` | 调用命令实现并记账：耗时、统计、慢日志、延迟采样与传播登记 |
| `src/server.c` | `alsoPropagate` | 把一条待传播操作登记进 `server.also_propagate` 队列 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c) —— 预检清单、`call` 的记账与传播登记。
- [Redis 命令：COMMAND](https://redis.io/docs/latest/commands/command/) —— 命令元数据的对外视图，与内部命令表的对应关系。
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/) —— 慢日志与延迟监控在排障中的用法。
- [Redis 开发文档：脚本编程](https://redis.io/docs/latest/develop/programmability/eval-intro/) —— 脚本嵌套命令调用时传播限制的来源。
- [Redis 命令：ACL](https://redis.io/docs/latest/commands/acl/) —— 预检中 ACL 判权环节的对外语义。

## 与主核 / AI 的关系

「预检—执行—登记」三段式是可直接借用的骨架，但 Redis 的十九项预检里有相当一部分依赖单线程假设：主核是多写者引擎，`performEvictions` 之后的 `evictClients` 那种「把自己驱逐掉就中止」的路径在并发下没有对应物，必须换成更明确的失败语义。传播登记的两段式（登记队列 + 执行单元末尾统一刷出）对主核的启发在于「派生操作与原命令同批送达」，但主核的事务提交边界不同，不能照搬这个落点。AI 侧若把 Redis 当推理结果缓存，命令执行与统计路径决定了 QPS 与延迟的分布形态：`call` 里那段单调钟与缓存时间的刷新逻辑说明 Redis 对每命令的时钟开销很敏感，AI 侧的高频短命令同样会撞上同一类瓶颈。
