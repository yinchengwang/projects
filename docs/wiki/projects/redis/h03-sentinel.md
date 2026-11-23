---
project: redis
feature: sentinel
page_type: feature
schema_version: 1
refs:
  - path: src/sentinel.c
    symbols: [sentinelHandleDictOfRedisInstances, sentinelSendPeriodicCommands, sentinelCheckSubjectivelyDown]
---

# 哨兵监控与故障转移

## 一句话职责

Sentinel 靠周期性的 INFO / PING / PUBLISH 探测把每个被监控实例的「活着没有」变成一个本地的主观判断（SDOWN），再用其他哨兵的报告把它升级成带多数票的客观判断（ODOWN），达到法定人数后走选举与状态机把某个副本提升为主、并让其余副本改挂新主。

## 机制是什么

Sentinel 不是一个独立程序，而是 Redis 自身跑在 `server.sentinel_mode` 下的一个模式：它复用同一套事件循环、命令表与异步连接库，但角色完全不同——它连接的是别人，自己不存数据。

三个符号各占一段职责：

- `sentinelHandleDictOfRedisInstances` 是**驱动骨架**：遍历全部被监控实例，主实例还要递归带上它的副本与它所知的其他哨兵。
- `sentinelSendPeriodicCommands` 是**探测侧**：按自适应周期给每个实例发 INFO、PING，并向 hello 通道 PUBLISH 自己的存在。
- `sentinelCheckSubjectivelyDown` 是**判定侧**：既负责在链路活跃度过低时主动断开重连，也负责置位与清除 SDOWN 标记。

故障转移本体（选举、选副本、状态机）不由这三个函数直接实现，而是由 `sentinelHandleRedisInstance` 在「只有主节点」分支里依次驱动 `sentinelCheckObjectivelyDown`、`sentinelStartFailoverIfNeeded`、`sentinelFailoverStateMachine` 完成。本页讲清这条链路上的判定与推进，但数据面的复制传播不在范围内。

## 怎么实现的

### 一、驱动骨架与调度节奏

真实 caller 是 `src/server.c` 的 `serverCron`：它在哨兵模式下调 `sentinelTimer()`。`sentinelTimer` 做四件事：

1. `sentinelCheckTiltCondition()` —— TILT 模式检测。源码注释写明：两次定时器中断之间若出现**负的时间差**或**过大的时间差**就进入 TILT，列出的两种成因是「哨兵进程因各种原因被阻塞（负载过大、机器在 I/O 里冻结、进程被信号停止）」与「系统时钟被大幅改动」。注释同时说明：这两种情况下所有实例都会莫名其妙地显示超时和失败，于是宁可进入 TILT 等 `sentinel_tilt_period`（源码里初值为 `SENTINEL_PING_PERIOD * 30`，即 30000 毫秒）过去再行动——**TILT 期间仍然收集信息，只是不行动**。
2. `sentinelHandleDictOfRedisInstances(sentinel.masters)` —— 主体。
3. `sentinelRunPendingScripts()` / `sentinelCollectTerminatedScripts()` / `sentinelKillTimedoutScripts()` —— 脚本调度。
4. **随机化 tick 频率**：`server.hz = CONFIG_DEFAULT_HZ + rand() % CONFIG_DEFAULT_HZ`。源码注释解释：持续改变「定时器中断」的频率，让各哨兵互相失步；否则同一时刻启动的哨兵会一直同步地问「选我当 leader 吗」，结果谁也赢不了选举。

`sentinelHandleDictOfRedisInstances` 的函数体很短：遍历实例字典，对每个实例调 `sentinelHandleRedisInstance(ri)`；若该实例是主（`SRI_MASTER`），再**递归**调自己处理 `ri->slaves` 与 `ri->sentinels` 两个字典。它同时记一个 `switch_to_promoted`：当某个主的 `failover_state == SENTINEL_FAILOVER_STATE_UPDATE_CONFIG` 时把它记下；**整棵树遍历结束后**才调 `sentinelFailoverSwitchToPromotedSlave(switch_to_promoted)`。这个顺序很关键——切换主节点表会在遍历中途改数据结构，因此必须延后到遍历之外。

`sentinelHandleRedisInstance` 把工作明确分成两半：

- **MONITORING HALF**（对所有类型的实例）：`sentinelReconnectInstance(ri)` 与 `sentinelSendPeriodicCommands(ri)`。
- **ACTING HALF**：先判 TILT——若处于 TILT 且 `mstime()-sentinel.tilt_start_time < sentinel_tilt_period` 就直接返回；否则清 TILT 标记并记 `-tilt mode exited`。然后 `sentinelCheckSubjectivelyDown(ri)`。只有主节点才继续往下：`sentinelCheckObjectivelyDown(ri)` → `sentinelStartFailoverIfNeeded(ri)`（返回真则额外调 `sentinelAskMasterStateToOtherSentinels(ri, SENTINEL_ASK_FORCED)`）→ `sentinelFailoverStateMachine(ri)` → `sentinelAskMasterStateToOtherSentinels(ri, SENTINEL_NO_FLAGS)`。

### 二、探测侧：`sentinelSendPeriodicCommands`

函数体按顺序做这些事：

1. **链路不可用直接返回**：`ri->link->disconnected` 为真时立刻返回。
2. **挂起命令数上限**：`ri->link->pending_commands >= SENTINEL_MAX_PENDING_COMMANDS * ri->link->refcount` 时返回。`SENTINEL_MAX_PENDING_COMMANDS` 在 `src/sentinel.c` 里定义为 100。源码注释解释：不想因为某条链路工作不正常而吃掉大量内存；括号里还补了一句「反正这里有冗余保护——长时间的超时条件会导致链路被断开重连」。
3. **INFO 周期自适应**：默认 `info_period = sentinel_info_period`（源码静态初值 10000 毫秒，可由 `SENTINEL CONFIG SET` 改）。但如果本实例是**副本**，且它的主处于 O_DOWN 或故障转移进行中，或者它报告自己与主的链路已经断了（`ri->master_link_down_time != 0`），就把 `info_period` 降到 1000 毫秒。源码注释说明理由：这种状态下要密切监视副本，因为它们可能被别的哨兵或管理员提升为主；同时副本报告与主断开时也要更频繁地取 INFO，好拿到新鲜的断开时长。
4. **PING 周期**：`ping_period = ri->down_after_period`，若它大于 `sentinel_ping_period`（源码里 `SENTINEL_PING_PERIOD` 定义为 1000）就取后者。源码注释：每次「上次收到的 pong 比配置的下线时间还老」时 ping 一次，但 `down-after-milliseconds` 大于 1 秒时无论如何每秒 ping 一次。
5. **发 INFO**：只发给非哨兵实例（主与副本），条件是 `ri->info_refresh == 0`（从没取过）或距上次刷新超过 `info_period`。用 `redisAsyncCommand` 异步发，回调是 `sentinelInfoReplyCallback`；命令名经 `sentinelInstanceMapCommand(ri, "INFO")` 映射。
6. **发 PING**：条件是「距上次 pong 超过 `ping_period`」**且**「距上次 ping 超过 `ping_period/2`」——双重条件保证不会堆积 PING。`sentinelSendPing` 内部只在 `ri->link->act_ping_time == 0` 时把它置为本次 ping 时间，源码注释解释：只有收到上次 ping 的 pong 才更新活跃 ping 时间，否则技术上我们一直等待的是**最早那个没被回复的 ping**。这个字段正是下一步判定 SDOWN 的计时基准。
7. **发 hello**：距上次发布超过 `sentinel_publish_period`（源码初值 2000 毫秒）时调 `sentinelSendHello(ri)`。

`sentinelSendHello` 向 `__sentinel__:hello` 通道（`SENTINEL_HELLO_CHANNEL`）PUBLISH 一个逗号分隔的负载，格式由源码注释给出：`sentinel_ip,sentinel_port,sentinel_runid,current_epoch,master_name,master_ip,master_port,master_config_epoch`。这个 hello 一举两得：既让其他哨兵发现本哨兵，也让它们知道本哨兵当前认为的主是谁。

`sentinelInstanceMapCommand` 是命令名重映射：若实例有 `ri->master`（副本与哨兵都有）就先把实例替换成它的主，再从 `ri->renamed_commands` 字典查该命令的重命名版本，查不到则返回原名。效果是——发给哨兵对端的 `INFO` 会被改写成 `SENTINEL INFO`，`SENTINEL IS-MASTER-DOWN-BY-ADDR` 同理。哨兵间协议与普通实例协议因此共用同一条连接与同一个回调框架。

### 三、判定侧：`sentinelCheckSubjectivelyDown`

函数体三段：

**第一段：算 elapsed。** `ri->link->act_ping_time` 非零时 `elapsed = mstime() - ri->link->act_ping_time`（在等 pong 的时长）；否则若链路已断开，`elapsed = mstime() - ri->link->last_avail_time`（上次链路可用到现在）；两者都没有则 `elapsed` 保持 0。

**第二段：低活性链路主动重连。**

- **命令链路**：链路存在、连接时长已超过 `sentinel_min_link_reconnect_period`（源码初值 15000 毫秒）、有挂起的 ping、并且「挂起 ping 的等待时间超过 `down_after_period/2`」**且**「距上次 pong 也超过 `down_after_period/2`」——满足就 `instanceLinkCloseConnection` 关掉命令链路。源码注释称之为「检测到链路活跃度低，需要重连」。
- **Pub/Sub 链路**：链路存在、连接时长超过 `sentinel_min_link_reconnect_period`、且 Pub/Sub 通道上超过 `sentinel_publish_period*3` 没有活动——同样主动关闭。

这一段的意义是：**不等到判定下线**就主动断链，让重连逻辑立刻开始工作。代价是可能反复重建连接，但 `sentinelReconnectInstance` 里有 `if (now - ri->link->last_reconn_time < sentinel_ping_period) return;` 兜底，重连至少间隔一个 ping 周期。

**第三段：更新 SDOWN 标记。** 三个条件任一成立即认为主观下线：

- `elapsed > ri->down_after_period`（不回话超过下线时间；源码里 `sentinel_default_down_after` 初值为 30000 毫秒）。
- 本实例被认为是主，但它报告自己是**副本**，且这种报告持续的时间超过 `down_after_period + sentinel_info_period*2`。源码注释说明：需要「足够长到满足 down_after_period，再加上拿到两次 INFO 报告的时间」。
- 本实例带 `SRI_MASTER_REBOOT` 标记（主重启），且从 `master_reboot_since_time` 起超过 `ri->master_reboot_down_after_period`。这个标记来自 INFO 解析：主节点的 runid 变了（触发 `+reboot` 事件）且配置了 `master-reboot-down-after-period` 时置位。

刚变成 SDOWN 时记 `+sdown` 事件并记 `s_down_since_time`；不再是 SDOWN 时记 `-sdown`，并同时清掉 `SRI_SCRIPT_KILL_SENT` 标记。

### 四、客观下线与故障转移

**ODOWN 是弱 quorum。** `sentinelCheckObjectivelyDown` 只在本哨兵认为主已 SDOWN 时才开始数票：本哨兵自己算 1 票，再遍历 `master->sentinels` 数带 `SRI_MASTER_DOWN` 标记的哨兵；票数达到 `master->quorum` 就置 `SRI_O_DOWN`。源码注释明确写道：ODOWN 是弱 quorum，只意味着「在某个时间范围内有足够多的哨兵报告它不可达」；由于消息可能延迟，**对 N 个实例同时就下线状态达成一致并没有强保证**。

**票从哪来。** `sentinelAskMasterStateToOtherSentinels` 向其他哨兵发 `SENTINEL IS-MASTER-DOWN-BY-ADDR <addr> <port> <epoch> <leader>`，回复由 `sentinelReceiveIsMasterDownReply` 处理：回复数组首项为 1 就置 `SRI_MASTER_DOWN`，为 0 就清掉；回复里带具体 runid（不是 `*`）说明对方投了票，把 `leader` 与 `leader_epoch` 记下来。另一个哨兵的状态超过 `sentinel_ask_period*5`（源码初值 1000 毫秒，即 5 秒）没更新，就清掉它的 `SRI_MASTER_DOWN` 标记。

**启动条件。** `sentinelStartFailoverIfNeeded` 的三个前置条件由源码注释逐条列出：主必须处于 O_DOWN；没有故障转移正在进行；上一次尝试距今不足 `failover_timeout*2`（源码里 `sentinel_default_failover_timeout` 初值为 `60*3*1000`，即 3 分钟）。注释补充：我们还不知道能不能赢得选举，所以可能启动了故障转移却最终无法行动。

`sentinelStartFailover` 做四件事：状态置 `SENTINEL_FAILOVER_STATE_WAIT_START`、置 `SRI_FAILOVER_IN_PROGRESS` 标记、`failover_epoch = ++sentinel.current_epoch`、记 `+new-epoch` 与 `+try-failover` 事件；并把 `failover_start_time` 设为 `mstime()+rand()%SENTINEL_MAX_DESYNC`（源码里 `SENTINEL_MAX_DESYNC` 定义为 1000）——**随机延迟**是避免多个哨兵同时发起故障转移的经典手法。

**六个状态的故障转移状态机**（`sentinelFailoverStateMachine`）：

1. `WAIT_START` → `sentinelFailoverWaitStart`：用 `sentinelGetLeader(ri, ri->failover_epoch)` 判断自己是不是本轮 leader。不是 leader 且不是强制故障转移时，最多等到 `min(sentinel_election_timeout, failover_timeout)`（源码里 `sentinel_election_timeout` 初值 10000 毫秒）就记 `-failover-abort-not-elected` 并 `sentinelAbortFailover`。
2. `SELECT_SLAVE` → `sentinelFailoverSelectSlave`：用 `sentinelSelectSlave` 选候选。选不到就记 `-failover-abort-no-good-slave` 并中止；选到就打 `SRI_PROMOTED` 标记、记 `+selected-slave`，进入下一状态。`sentinelSelectSlave` 的筛选条件由源码注释逐条列出：不能是 S_DOWN / O_DOWN / 断连；上次回 pong 距今不超过 `sentinel_ping_period*5`；`info_refresh` 距今不超过有效期（主在 SDOWN 时用 `sentinel_ping_period*5`，否则用 `sentinel_info_period*3`）；`master_link_down_time` 不超过 `(now - master->s_down_since_time) + down_after_period*10`——源码注释自嘲这「基本算是黑魔法」，但给出的思路是：主从我们看已经不可用，副本可能落后，但不该落后太久；最后 `slave_priority` 不能为 0，否则直接淘汰。候选之间的排序键在 `compareSlavesForPromotion` 里：先 `slave_priority` 小的，再 `slave_repl_offset` 大的，最后 runid 字典序小的；runid 为 NULL 视为最大（源码注释说这是为兼容不报 runid 的老版本）。
3. `SEND_SLAVEOF_NOONE` → `sentinelFailoverSendSlaveOfNoOne`：给提升目标发 `SLAVEOF NO ONE`。源码注释解释：回复不检查，改为**间接观察**——下一次 INFO 里的 role 变成 master 就算成功。目标此时断连就原地重试，直到超过 `failover_timeout` 才记 `-failover-abort-slave-timeout`。
4. `WAIT_PROMOTION` → `sentinelFailoverWaitPromotion`：同样只处理超时；推进到下一状态靠 INFO 回调里发现 role 变化。
5. `RECONF_SLAVES` → `sentinelFailoverReconfNextSlave`：给尚未重配的副本发 `SLAVEOF <新主>`，并发数受 `master->parallel_syncs` 限制（源码里 `SENTINEL_DEFAULT_PARALLEL_SYNCS` 初值为 1）。已发出但超过 `sentinel_slave_reconf_timeout`（源码初值 10000 毫秒）仍无进展的副本会被直接标记为 `SRI_RECONF_DONE`——源码注释解释：Sentinel 之后会把这个副本识别为配置错误并再修一次。
6. `UPDATE_CONFIG` → `sentinelFailoverSwitchToPromotedSlave`：记 `+switch-master` 事件，然后 `sentinelResetMasterAndChangeAddress` 把主表里的条目换成被提升的副本。

**改配置靠一个事务。** `sentinelSendSlaveOf` 发的是 MULTI 事务，源码注释列出事务里做的四件事：按指定 host/port 重配实例、`CONFIG REWRITE` 把新配置写到磁盘、断开所有其他客户端（但不发这条命令的连接除外）以触发客户端重连时的「问主」协议。注释还说明：**不检查这些命令的回复**，改为在下一次 INFO 输出里观察效果。

### 五、退化与中止路径

- **TILT 模式**：跳过整个 ACTING HALF（不判下线、不做故障转移），但 MONITORING HALF 照常运行——数据照收，动作不做。
- **主动断链**：`sentinelCheckSubjectivelyDown` 里的 `instanceLinkCloseConnection` 关掉连接后，由 `sentinelReconnectInstance` 在后续轮次重建，重建节奏受 `sentinel_ping_period` 约束。
- **故障转移中止**共有四条真实路径：不是 leader（`-failover-abort-not-elected`）、选不到合格副本（`-failover-abort-no-good-slave`）、提升目标断连或等待提升超时（`-failover-abort-slave-timeout`）、以及副本重配超时后的强制收尾（`+failover-end-for-timeout`）。`sentinelAbortFailover` 有前置断言：**只能在「被提升的副本确认切换之前」中止**（`failover_state <= SENTINEL_FAILOVER_STATE_WAIT_PROMOTION`），之后只能让它跑完或超时。
- **选副本失败时没有第二候选重试**：源码里 `sentinelFailoverSelectSlave` 在 `slave == NULL` 时直接中止整个故障转移，并不回到选举重来。

## 关键取舍

**源码事实：** 探测分 INFO / PING / PUBLISH 三种，各有独立周期与独立回调；INFO 只发给主与副本，不发给哨兵对端；PING 三类都发。**源码事实：** SDOWN 是纯本地的单个哨兵判断，ODOWN 是基于「其他哨兵报告过它不可达」的弱 quorum，源码注释明说不保证多个哨兵在同一时刻达成一致。**源码事实：** 候选副本筛选包含 `slave_priority`（为 0 即淘汰）、复制偏移与 `master_link_down_time` 上限，注释对这个上限自嘲是「黑魔法」。**源码事实：** 故障转移开始时间带随机抖动（`rand()%SENTINEL_MAX_DESYNC`），`sentinelTimer` 还随机化 `server.hz` 让哨兵互相失步。**源码事实：** `sentinelSendSlaveOf` 不检查命令回复，只观察 INFO 里的 role 变化。**源码事实：** 故障转移的中止只允许发生在提升确认之前。**源码事实：** TILT 模式用于应对时钟跳变与进程长时间阻塞，期间只收集信息不行动。**源码事实：** INFO 周期会自适应降到 1000 毫秒——当副本的主正处于 O_DOWN / 故障转移中，或副本报告自己与主断开时。

**架构推论：** 用「弱 quorum + 随机延迟 + 投票」代替共识协议，是把**可用性放在一致性前面**：多数派哨兵同意即可行动，不必等全体。代价是网络分区时可能出现两个独立的故障转移、各自提升不同的副本，Redis 并不提供自动消解这种双主的机制，只能靠 `SENTINEL RESET` 或人工介入。**架构推论：** 「不看命令回复、只看 INFO 里的状态变化」是一种**最终状态驱动**的控制方式：被控端不需要理解哨兵协议（`SLAVEOF` 是普通 Redis 命令），代价是每一次状态推进都要等一个 INFO 周期，故障转移的耗时下限由此决定。**架构推论：** TILT 模式是防御性设计：宁可暂时什么都不做，也不要在时钟跳变时做出错误的故障转移决定——这是把「误判下线导致双主」的代价看得比「晚几十秒恢复」更高。**架构推论：** 用「命令链路 + Pub/Sub 链路」两条连接分别承载控制流与 hello 广播，使得 hello 的丢包不会阻塞 INFO/PING 的探测，代价是每个被监控实例要维护两条连接、连接数翻倍。**架构推论：** 把主从角色的切换做成普通 Redis 命令（SLAVEOF / CONFIG REWRITE / CLIENT KILL）而非私有协议，让哨兵可以监控任何兼容的 Redis 实例；代价是无法要求被控端做「原子切换」，只能分步下发再靠 INFO 观测收敛。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/sentinel.c` | `sentinelHandleDictOfRedisInstances` | 遍历全部被监控实例（主 → 副本 → 哨兵递归），并在整棵树遍历结束后执行切换到新主 |
| `src/sentinel.c` | `sentinelSendPeriodicCommands` | 探测侧：按自适应周期发 INFO / PING / PUBLISH hello，并限制挂起命令数 |
| `src/sentinel.c` | `sentinelCheckSubjectivelyDown` | 判定侧：低活性链路主动断开重连，以及 SDOWN 标记的置位与清除 |

## 参考

- [Redis 源码：sentinel.c](https://github.com/redis/redis/blob/unstable/src/sentinel.c)
- [Redis 文档：Sentinel](https://redis.io/docs/latest/operate/oss_and_stack/management/sentinel/)
- [Redis 文档：Sentinel 作为分布式系统与故障转移](https://redis.io/docs/latest/operate/oss_and_stack/management/sentinel/#sentinel-as-a-distributed-system)
- [Redis 命令：ROLE](https://redis.io/docs/latest/commands/role/)

## 与主核 / AI 的关系

Sentinel 提供了三样可以借鉴的东西。一是**两级下线判定**：本地主观判断（SDOWN）先行、分布式确认（ODOWN）随后，把「我这边连不上」和「大家都连不上」分开，避免单点网络抖动触发切换；主核若做多副本角色切换，同样需要这层区分。二是**最终状态驱动而非命令回复驱动**：它不检查 `SLAVEOF` 的回复，而是靠下一次 INFO 里 role 的变化来判断成功，这种方式能作用于任何兼容实例；代价是每一步推进都要付一个观测周期，主核若要求更快收敛，就得引入带版本号的显式确认。三是**时钟异常的防御**：TILT 模式说明在依赖超时的控制系统里，**时钟本身是需要被监控的输入**——主核的租约过期、连接超时、复制延迟判定全都建立在时钟上，时钟跳变会直接放大成误判。

AI 侧若用 Redis Sentinel 托管向量缓存的主从，读路径上要注意两件事：一是 Sentinel 只保证**主从角色**的高可用，它切换的是「谁是主」，不会帮你做向量索引的分片；二是故障转移期间副本被提升时使用的是它自己的复制偏移，被提升副本上缺失的那段增量数据需要由新主补齐，若客户端在切换窗口内写入，**这部分数据可能在切换完成前处于不可读状态**。因此向量检索这类强一致读的场景，不应把「连到当前主」当作强一致保证——必须显式处理写后立即读（read-your-writes）与切换窗口内的降级路径。
