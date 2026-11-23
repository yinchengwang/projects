---
project: redis
feature: shutdown-sequence
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [prepareForShutdown]
---

# 关停序列

## 一句话职责

`prepareForShutdown` 是关停的唯一守门函数：先归一化标志位，再判断是否必须等副本追平复制偏移；要等就只登记一个截止时刻并返回 `C_ERR`，把续跑交给 `serverCron`；不用等就立刻调 `finishShutdown` 做落盘、通知与资源释放，返回 `C_OK` 后由调用方 `exit(0)`。

## 机制是什么

关停是**两段式加一个可选等待窗口**，不是一条直落到底的收尾链：

1. **准备段**（`prepareForShutdown`）：定标志、向 systemd 报状态、取消集群 ASM 任务、判定副本是否落后。
2. **等待窗口**（可选）：存在落后副本、未设 `SHUTDOWN_NOW`、且 `shutdown-timeout` 不为 0 时，只写 `server.shutdown_mstime` 后返回，不阻塞事件循环。
3. **收尾段**（`finishShutdown`）：阻止并回收子进程、fsync AOF、按标志决定是否落最终 RDB、通知模块、删 pid 文件、关监听、释放集群配置锁。

`prepareForShutdown` 在全仓有四个调用点，归为三类入口：

- `shutdownCommand`（`src/db.c`）逐词解析 `NOSAVE` / `SAVE` / `NOW` / `FORCE` / `ABORT`，拒绝 `ABORT` 与其他标志混用、拒绝 `NOSAVE` 与 `SAVE` 同时出现；合法时先 `blockClientShutdown` 把客户端挂成 `BLOCKED_SHUTDOWN`，再调 `prepareForShutdown`。
- 信号路径：`sigShutdownHandler` 只置 `server.shutdown_asap` 并记下 `server.last_sig_received`，真正的关停由 `serverCron` 消费；若此时正在加载数据，则由 `whileBlockedCron` 以 `SHUTDOWN_NOSAVE` 调 `prepareForShutdown`。
- 原地重启：`restartServer` 在 `RESTART_SERVER_GRACEFULLY` 下以 `SHUTDOWN_NOW` 调 `prepareForShutdown`，明确不让位等副本。

标志位定义在 `src/server.h`：`SHUTDOWN_NOFLAGS` 为 0、`SHUTDOWN_SAVE` 为 1、`SHUTDOWN_NOSAVE` 为 2、`SHUTDOWN_NOW` 为 4、`SHUTDOWN_FORCE` 为 8。`shutdown-on-sigint` / `shutdown-on-sigterm` 两个枚举配置项的取值表 `shutdown_on_sig_enum`（`src/config.c`）用的就是这同一组数字，`default` 映射为 0，`isValidShutdownOnSigFlags` 只额外拦 `NOSAVE` 与 `SAVE` 的组合。

## 怎么实现的

### 一、准备段的判定顺序（`prepareForShutdown`）

第一句是 `if (isShutdownInitiated()) return C_ERR;`。`isShutdownInitiated()` 的判据是 `server.shutdown_mstime != 0`，所以「关停已发起」在源码里等价于「截止时刻已登记」——同一场关停不会被再走一遍准备段。

接着是标志归一化：`server.loading || server.sentinel_mode` 时执行 `flags = (flags & ~SHUTDOWN_SAVE) | SHUTDOWN_NOSAVE`，注释给出的理由是加载期间落盘会用半份数据覆盖现有 DB，sentinel 模式则一律不保存。归一化后的 `flags` 被写进 `server.shutdown_flags`，`finishShutdown` 后续只从这里读。

随后按序执行：`serverLog(LL_NOTICE, "User requested shutdown...")`；`server.supervised_mode == SUPERVISED_SYSTEMD` 时 `redisCommunicateSystemd("STOPPING=1\n")`；`clusterAsmCancel(NULL, "server shutdown")` 取消所有 ASM（原子槽迁移）任务。

等待分支的条件是三项同时成立：

- 未设 `SHUTDOWN_NOW`；
- `server.shutdown_timeout != 0`（该配置项 `shutdown-timeout` 由 `src/config.c` 的 `createIntConfig` 注册，默认值来自该宏的 `default` 参数即 10，取值范围来自 `lower` / `upper` 参数即 0..INT_MAX）；
- `!isReadyToShutdown()`。

进入等待分支后做四件事：`server.shutdown_mstime = server.mstime + server.shutdown_timeout * 1000`（秒换成毫秒）；若 `!isPausedActions(PAUSE_ACTION_REPLICA)` 就调 `sendGetackToReplicas()`，把共享对象 `shared.getack`（内容 `GETACK`）通过 `replicationFeedSlaves` 立即发给所有副本要 ack；`pauseActions(PAUSE_DURING_SHUTDOWN, LLONG_MAX, PAUSE_ACTIONS_CLIENT_WRITE_SET)` 冻结写入等动作；最后返回 `C_ERR`。

`PAUSE_ACTIONS_CLIENT_WRITE_SET` 的定义在 `src/server.h` 里是四位或：`PAUSE_ACTION_CLIENT_WRITE`（1<<0）、`PAUSE_ACTION_EXPIRE`（1<<2）、`PAUSE_ACTION_EVICT`（1<<3）、`PAUSE_ACTION_REPLICA`（1<<4）。`PAUSE_DURING_SHUTDOWN` 是 `pause_purpose` 枚举里独立的一项，`pauseActions` 与 `unpauseActions` 都按 purpose 分别记录 `end` 与 `paused_actions`，所以这场关停的暂停不会和 `CLIENT PAUSE`、failover、slot handoff 的暂停互相覆盖。传入的结束时刻是 `LLONG_MAX`，而 `updatePausedActions` 只有在 `p->end > server.mstime` 时才把该 purpose 的动作并进 `server.paused_actions`，因此这组冻结只能由 `unpauseActions(PAUSE_DURING_SHUTDOWN)` 解除，不会自行到期。

冻结的真正落地位置是 `processCommand`：客户端非副本、且 `isPausedActions(PAUSE_ACTION_CLIENT_ALL)` 为真或「`PAUSE_ACTION_CLIENT_WRITE` 为真且该命令 `is_may_replicate_command`」时，它调 `blockPostponeClient` 把客户端挂到 `BLOCKED_POSTPONE` 而不执行命令，之后由 `updatePausedActions` 发现暂停变宽松时调 `unblockPostponedClients` 重排。副本客户端带 `CLIENT_SLAVE` 标志，不受这条判断影响——这正是关停窗口里副本仍能收到 `GETACK` 并回报 ack 的前提。

`isReadyToShutdown()` 的行为：`listLength(server.slaves) == 0` 直接返回 1；否则遍历副本列表，跳过带 `CLIENT_ASM_MIGRATING` 标志的迁移目的端副本，只要有一个副本的 `repl_ack_off != server.master_repl_offset` 就返回 0。

条件不满足时走 `return finishShutdown();`，即同步做完收尾段。

### 二、等待窗口如何被续跑（`serverCron`）

`serverCron` 里有两段互斥的关停处理：

- `shouldShutdownAsap() && !isShutdownInitiated()`：从 `server.last_sig_received` 判断信号种类，`SIGINT` 取 `server.shutdown_on_sigint`、`SIGTERM` 取 `server.shutdown_on_sigterm` 作为 `shutdownFlags`，缺省为 `SHUTDOWN_NOFLAGS`，然后 `prepareForShutdown(shutdownFlags) == C_OK` 才 `exit(0)`。
- `isShutdownInitiated()`：当 `server.mstime >= server.shutdown_mstime || isReadyToShutdown()` 时调 `finishShutdown()`，`C_OK` 才 `exit(0)`，失败则记日志后继续运行。

也就是说等待窗口的到期判定是「超时到点」与「副本已追平」两者取其一，不是只看超时。`INFO` 的 Server 段在关停进行中会额外输出 `shutdown_in_milliseconds`（值为 `server.shutdown_mstime - commandTimeSnapshot()`）。

`whileBlockedCron` 的加载分支是 `shouldShutdownAsap() && server.loading`：`prepareForShutdown(SHUTDOWN_NOSAVE) == C_OK` 则 `exit(0)`；失败则打警告并把 `server.shutdown_asap` 与 `server.last_sig_received` 清零，让信号请求作废。注释写明了原因：加载过程中不能在信号处理器里直接关停。

### 三、收尾段的顺序（`finishShutdown`）

先取三个掩码结果：`int save = server.shutdown_flags & SHUTDOWN_SAVE;`、`int nosave = server.shutdown_flags & SHUTDOWN_NOSAVE;`、`int force = server.shutdown_flags & SHUTDOWN_FORCE;`。

**副本统计**：遍历 `server.slaves`，跳过 `CLIENT_ASM_MIGRATING`，对每个副本读 `repl_ack_off`；若副本跑在非主 IO 线程（`replica->running_tid != IOTHREAD_MAIN_THREAD_ID`，该常量为 0），先 `pauseIOThread(replica->tid)`、读完再 `resumeIOThread(replica->tid)`，注释说明这是为了避免数据竞争。落后的副本逐个打 `Lagging replica ... reported offset ... behind master` 日志（`lag` 只在 `replstate == SLAVE_STATE_ONLINE` 时按 `time(NULL) - repl_ack_time` 计算），最后汇总一条「%d of %d replicas are in sync」。

**然后严格按此顺序**：

1. `ldbKillForkedSessions()` 杀掉所有 Lua 调试器的 fork 会话。
2. `server.child_type == CHILD_TYPE_RDB`：打警告、`killRDBChild()`、`rdbRemoveTempFile(server.child_pid, 0)`、`resetChildState()`。这里额外手工调 `rdbRemoveTempFile` 的原因写在注释里：正常路径由 `backgroundSaveDoneHandler` 做清理，而关停时那条路径走不到。
3. `server.child_type == CHILD_TYPE_MODULE`：`TerminateModuleForkChild(server.child_pid, 0)`。
4. `server.child_type == CHILD_TYPE_AOF`：若 `server.aof_state == AOF_WAIT_REWRITE` 且未设 `force`，打「Writing initial AOF, can't exit.」并 `goto error`；否则 `killAppendOnlyChild()`。
5. `server.aof_state != AOF_OFF`：`flushAppendOnlyFile(1)` 后 `redis_fsync(server.aof_fd)`，fsync 失败只打日志不中断。
6. `(server.saveparamslen > 0 && !nosave) || save`：`rdbPopulateSaveInfo` 填好 `rdbSaveInfo`，再 `rdbSave(SLAVE_REQ_NONE, server.rdb_filename, rsiptr, RDBFLAGS_KEEP_CACHE)`；`RDBFLAGS_KEEP_CACHE`（`src/rdb.h`，值为 1<<4）的含义是不回收页缓存，源码 `rdbSave` 调用处的注释原话是 keep the page cache since it's likely to restart soon。保存失败且未设 `force` 时打「Error trying to save the DB, can't exit.」并 `goto error`；设了 `force` 只打「Exit anyway.」继续。
7. `updateCurIncrAofEndOffset()` 把当前 INCR AOF 的 `end_offset` 记为 `server.master_repl_offset` 并落 manifest。
8. `server.aof_manifest` 非空则 `aofManifestFree`。
9. `moduleFireServerEvent(REDISMODULE_EVENT_SHUTDOWN, 0, NULL)` 触发模块关停事件。
10. `server.daemonize || server.pidfile` 时 `unlink(server.pidfile)`。
11. `flushSlavesOutputBuffers()` 尽力把待发数据推给副本。
12. `closeListeningSockets(1)`：遍历 `server.listeners` 逐个 `close` fd，`cluster_enabled` 时再关 `server.clistener` 的 fd，参数 1 表示同时 `unlink` unix socket。
13. 非 `__sun` 平台、且 `server.cluster_enabled && server.cluster_config_file_lock_fd != -1` 时 `flock(..., LOCK_UN|LOCK_NB)` 释放集群配置文件锁。
14. 打「%s is now ready to exit, bye bye...」（按 `server.sentinel_mode` 写 Redis 或 Sentinel），`return C_OK`。

失败出口是 `error:` 标签：打「Errors trying to shut down the server. Check the logs for more information.」，调 `cancelShutdown()`，返回 `C_ERR`，进程继续运行。

### 四、取消与打断

`cancelShutdown()` 依次清 `server.shutdown_asap`、`server.shutdown_flags`、`server.shutdown_mstime`、`server.last_sig_received`，调 `replyToClientsBlockedOnShutdown()` 给挂在 `BLOCKED_SHUTDOWN` 上的客户端回 `-Errors trying to SHUTDOWN. Check logs.` 并解除阻塞，最后 `unpauseActions(PAUSE_DURING_SHUTDOWN)` 解冻上一节那组暂停。

`abortShutdown()` 分三种情况：已发起关停则 `cancelShutdown()` 并返回 `C_OK`；只收到过信号则仅清 `server.shutdown_asap` 后返回 `C_OK`；两者都没有则返回 `C_ERR`，命令层据此回 `No shutdown in progress.`。

`sigShutdownHandler` 里还有一条硬退出：若 `shouldShutdownAsap()` 已为真且这次信号仍是 `SIGINT`，则 `rdbRemoveTempFile(getpid(), 1)` 后直接 `exit(1)`，注释给的语义是「用户真的坚持要立刻退出」，以非干净关停的错误码退出。

## 关键取舍

**等待副本不是阻塞等待。** 准备段登记 `shutdown_mstime` 就返回，靠 `serverCron` 每轮判定「到点或已追平」再进 `finishShutdown`。代价是关停被拉长到一个 cron 轮次粒度；收益是事件循环在这段时间仍可服务只读请求。`shutdown-timeout` 默认为 10 秒（`src/config.c` 该配置项的默认值参数），显式设为 0 等于放弃等待窗口、准备段直接进收尾。等待期间不靠拒绝所有命令实现「不丢写」，而是 `pauseActions` 冻结 `PAUSE_ACTIONS_CLIENT_WRITE_SET` 那四类动作，并主动 `sendGetackToReplicas` 催 ack。

**收尾失败不退出进程。** RDB 保存失败、初始 AOF 尚未写完这两种错误都走 `goto error`，由 `cancelShutdown` 回滚状态继续运行；只有 `SHUTDOWN_FORCE` 把它们降级为警告。这条取舍的方向很明确：宁可关停不了，也不带着没落盘的数据退出。真正无法回滚的退出只有信号路径里 SIGINT 的第二次打断。

**加载期与 sentinel 模式一律不落盘。** `prepareForShutdown` 直接把 `SHUTDOWN_SAVE` 抹掉换成 `SHUTDOWN_NOSAVE`；信号路径也固定传 `SHUTDOWN_NOSAVE`。理由是加载中的数据是半份，写出去会覆盖原有 DB。

**关停发起后不可重入。** `isShutdownInitiated()` 以 `shutdown_mstime != 0` 为准，第二次 `prepareForShutdown` 一律 `C_ERR`；`shutdownCommand` 因此把客户端挂成 `BLOCKED_SHUTDOWN` 再调用准备段：`prepareForShutdown` 返回 `C_OK` 就直接 `exit(0)`，函数走到末尾只对应两种情况——关停仍在进行（客户端保持阻塞，由 `replyToClientsBlockedOnShutdown` 统一回包）或关停失败（客户端已收到错误）。

**架构推论：** 两段式把「等多久」与「做什么」彻底分开，等待窗口的全部状态只有一个 `shutdown_mstime` 加一组按 purpose 记账的 `pause_event`，因此这条路径天然是幂等的可中断状态机——`abortShutdown` 能安全回滚正因为它只需清这四个字段并解冻那一组暂停。反过来，`finishShutdown` 里的顺序一旦被打破就会引入竞态（在 `killRDBChild` 之后手工补 `rdbRemoveTempFile` 正是这种顺序依赖的显式补丁），所以收尾段不适合做成可重排的阶段。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `prepareForShutdown` | 归一化标志、取消 ASM 任务、判定副本是否落后、决定让位等待还是直接进 `finishShutdown` |
| `src/server.c` | `finishShutdown` | 收尾段主体：杀子进程、fsync AOF、按标志落 RDB、触发模块事件、删 pid、关监听、释放集群锁 |
| `src/server.c` | `isReadyToShutdown` | 无副本或全部副本 `repl_ack_off == master_repl_offset` 时才返回 1，跳过 `CLIENT_ASM_MIGRATING` |
| `src/server.c` | `isShutdownInitiated` | 以 `server.shutdown_mstime != 0` 判定关停是否已发起 |
| `src/server.c` | `abortShutdown` | 分别处理「已发起」与「仅收到信号」两种取消场景，两者都不成立时返回 `C_ERR` |
| `src/server.c` | `cancelShutdown` | 清四个关停字段、回复并解除 `BLOCKED_SHUTDOWN` 客户端、解冻 `PAUSE_DURING_SHUTDOWN` |
| `src/server.c` | `serverCron` | 消费 `shutdown_asap` 或按 `shutdown_mstime` 到期/副本追平续跑 `finishShutdown` |
| `src/server.c` | `whileBlockedCron` | 加载数据期间收到终止请求时以 `SHUTDOWN_NOSAVE` 走关停，失败则作废信号请求 |
| `src/server.c` | `sigShutdownHandler` | 置 `shutdown_asap` 与 `last_sig_received`；SIGINT 二次打断时 `exit(1)` |
| `src/server.c` | `sendGetackToReplicas` | 向所有副本广播共享对象 `GETACK`，催其在关停前回报 ack |
| `src/server.c` | `closeListeningSockets` | 关闭全部监听 fd，按参数决定是否 unlink unix socket 文件 |
| `src/server.c` | `restartServer` | `RESTART_SERVER_GRACEFULLY` 时以 `SHUTDOWN_NOW` 复用关停准备段做原地重启 |
| `src/db.c` | `shutdownCommand` | 解析 `NOSAVE`/`SAVE`/`NOW`/`FORCE`/`ABORT`，校验非法组合，挂起客户端后调 `prepareForShutdown` |
| `src/blocked.c` | `blockClientShutdown` | 把发起 SHUTDOWN 的客户端挂到 `BLOCKED_SHUTDOWN` |
| `src/blocked.c` | `replyToClientsBlockedOnShutdown` | 对挂在该阻塞类型上的客户端回 `-Errors trying to SHUTDOWN. Check logs.` 并解除阻塞 |
| `src/server.h` | `SHUTDOWN_SAVE` | 标志常量：强制在关停时保存，值为 1 |
| `src/server.h` | `SHUTDOWN_NOSAVE` | 标志常量：关停时不保存，值为 2 |
| `src/server.h` | `SHUTDOWN_NOW` | 标志常量：不等副本追平，值为 4 |
| `src/server.h` | `SHUTDOWN_FORCE` | 标志常量：落盘错误不阻止关停，值为 8 |
| `src/server.h` | `PAUSE_ACTIONS_CLIENT_WRITE_SET` | 等待窗口冻结的动作集：客户端写、过期、淘汰、副本流量 |
| `src/config.c` | `shutdown_on_sig_enum` | `shutdown-on-sigint` / `shutdown-on-sigterm` 的取值表，复用同一组 SHUTDOWN 标志 |

## 与主核 / AI 的关系

关停序列里可复用的形态是「把不可中断的收尾段与可等待的准备段切开」：Redis 用 `shutdown_mstime` 一个时间戳承载等待状态，把「等多久」交给周期任务判定，避免在收尾路径里做阻塞等待。主核若也要实现优雅退出，同样需要把「外部请求、状态冻结、落盘、资源释放」分成可回滚与不可回滚两半，并让可回滚的那半有明确的取消入口。**架构推论：** 收尾段的顺序依赖（后台子进程必须在本进程落盘之前被杀死或等待、集群锁必须在关闭监听之后释放）无法由函数边界表达，只能靠注释与单点实现维持，这与主核把跨层顺序写成显式门禁脚本是同一种工程取向。

## 参考

- [Redis 源码：server.c（关停段）](https://github.com/redis/redis/blob/unstable/src/server.c)
- [Redis 源码：db.c（SHUTDOWN 命令解析）](https://github.com/redis/redis/blob/unstable/src/db.c)
- [Redis 文档：SHUTDOWN 命令](https://redis.io/docs/latest/commands/shutdown/)
- [Redis 文档：复制与副本 ack](https://redis.io/docs/latest/operate/oss_and_stack/management/replication/)
- [Redis 文档：Redis 运维管理](https://redis.io/docs/latest/operate/oss_and_stack/management/admin/)
