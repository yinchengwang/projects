---
project: redis
feature: cluster-bus
page_type: feature
schema_version: 1
refs:
  - path: src/cluster_legacy.c
    symbols: [clusterInit, clusterProcessPacket]
---

# 集群总线与槽位

## 一句话职责

集群总线是 Redis Cluster 节点之间专用的第二条 TCP 通道，`clusterInit` 负责把这条通道所需的本地状态、配置与前置校验装配起来，`clusterProcessPacket` 负责把收到的一帧总线报文翻译成对本地节点表与槽位表的修改。

## 机制是什么

一个集群节点同时开两条网络连接：面向客户端的数据端口，和面向其他节点的**集群总线端口**。总线端口默认是数据端口加 10000（`CLUSTER_PORT_INCR`，定义在 `src/cluster_legacy.h`）；`clusterInit` 会检查数据端口加 10000 是否超过 65535，超过就直接退出——源码日志把这条规则写死成「Redis 端口号必须不大于 55535」。

总线报文有统一的二进制头 `clusterMsg`：4 字节签名 `"RCmb"`、协议版本 `CLUSTER_PROTO_VER`（定义为 1）、总长度、类型、发送者节点 ID（`CLUSTER_NAMELEN` = 40 字节）、本节点公告的 IP、槽位位图、configEpoch 等。报文类型共 11 种（`CLUSTERMSG_TYPE_COUNT` = 11）：`PING` / `PONG` / `MEET` / `FAIL` / `PUBLISH` / `PUBLISHSHARD` / `FAILOVER_AUTH_REQUEST` / `FAILOVER_AUTH_ACK` / `MFSTART` / `UPDATE` / `MODULE`。

槽位模型是固定的 16384 个槽（`CLUSTER_SLOTS`，定义在 `src/cluster.h`），每个槽归属一个主节点，归属关系靠 `(节点, configEpoch)` 二元组仲裁：`configEpoch` 更大的节点说了算，相等则触发冲突处理。

`clusterInit` 做的是**状态与前置条件**的装配，不监听端口；真正的 `connListen` 与 accept handler 注册在 `clusterInitLast` 里，它由 `src/server.c` 的 `main()` 在 `initServer()` 之后单独调用。

## 怎么实现的

### 一、`clusterInit`：装配集群状态

`clusterInit` 在 `src/server.c` 的 `main()` 里于 `clusterCommonInit()` 之后被调用（注释说明 `clusterCommonInit()` 初始化的是 `clusterInit()` 依赖的槽位统计），函数体按顺序做这些事：

1. **分配并清零集群状态**：`server.cluster = zmalloc(sizeof(struct clusterState))`，`myself` 置 NULL、`currentEpoch` 置 0、`state` 置 `CLUSTER_FAIL`、`size` 置 0、`todo_before_sleep` 置 0。
2. **建三张表**：节点表 `nodes`（`clusterNodesDictType`）、分片表 `shards`（`clusterSdsToListType`）、节点黑名单 `nodes_black_list`（`clusterNodesBlackListDictType`）。
3. **复位选举状态**：`failover_auth_time` / `failover_auth_count` / `failover_auth_rank` / `failover_auth_epoch` 全置 0，`cant_failover_reason` 置 `CLUSTER_CANT_FAILOVER_NONE`，`lastVoteEpoch` 置 0。
4. **复位总线统计**：`stats_bus_messages_sent[i]` 与 `stats_bus_messages_received[i]` 按 `CLUSTERMSG_TYPE_COUNT` 清零，另加 `stats_pfail_nodes = 0`、`stat_cluster_links_buffer_limit_exceeded = 0`。
5. **清空槽位表**：`memset(server.cluster->slots, 0, ...)` 后调 `clusterCloseAllSlots()`；同时清空 `owner_not_claiming_slot` 位图。
6. **独占集群配置文件**：`server.cluster_config_file_lock_fd = -1`，然后 `clusterLockConfig(server.cluster_configfile)`，失败直接 `exit(1)`。源码注释写明意图：「锁住集群配置文件，确保每个节点用的是自己的 nodes.conf」。
7. **载入或新建配置**：`clusterLoadConfig(server.cluster_configfile)` 失败时，用 `createClusterNode(NULL, CLUSTER_NODE_MYSELF|CLUSTER_NODE_MASTER)` 造一个随机名的自己，`clusterAddNode(myself)` 并 `clusterAddNodeToShard(myself->shard_id, myself)`，置 `saveconf = 1`，随后 `clusterSaveConfigOrDie(1)` 落盘。源码注释说：找不到配置就用 `createClusterNode()` 给的随机名。
8. **端口合理性检查**：当没有显式配置 `server.cluster_port`、且 `defaultClientPort()` 大于 `65535 - CLUSTER_PORT_INCR` 时退出。源码注释解释：握手期的另一个端口检查触发得太晚，拦不住用过高集群端口号的情况；日志把规则写死成「Cluster communication port is 10,000 port numbers higher than your Redis port. Your Redis port number must be 55535 or less.」
9. **必须有绑定地址**：`server.bindaddr_count` 为 0 时退出，日志是「No bind address is configured, but it is required for the Cluster bus.」
10. **填自己的公告端口**：`deriveAnnouncedPorts(&myself->tcp_port, &myself->tls_port, &myself->cport)`。源码注释说明 IP 不在这里定，要靠 MEET 报文发现。
11. **复位手动故障转移状态**：`mf_end = 0`、`mf_slave = NULL`、`resetManualFailover()`。
12. **同步自己的对外公告字段**：`clusterUpdateMyselfFlags()`、`clusterUpdateMyselfIp()`、`clusterUpdateMyselfHostname()`、`clusterUpdateMyselfHumanNodename()`。
13. **生成内部密钥**：`getRandomHexChars(server.cluster->internal_secret, CLUSTER_INTERNALSECRETLEN)`，长度 40。

### 二、`clusterProcessPacket`：一帧报文的处理

函数头的注释划清了职责边界：报文从 `link->rcvbuf` 开始，**释放缓冲区是调用方的事**；函数只负责处理更高层语义并修改集群状态；返回 1 表示处理后 link 仍有效，返回 0 表示 link 已被释放（例如收到来自错误 sender ID 的 PONG）。

真实 caller 是 `clusterReadHandler`：它先把 8 字节头部读齐以拿到 `totlen`，再读满整包，`rcvbuflen == ntohl(hdr->totlen)` 时调 `clusterProcessPacket(link)`；返回 1 就把接收缓冲收缩回 `RCVBUF_INIT_LEN` 并继续读，返回 0 直接 `return`（link 已失效）。

**第一阶段：校验。**

1. 读出头部的 `totlen`、`type`，取 `now = mstime()`；`type < CLUSTERMSG_TYPE_COUNT` 时累加 `stats_bus_messages_received[type]`，并记一条 debug 日志。
2. `totlen < 16` 返回 1——源码注释写明至少要有 signature、version、totlen、count 四个字段。
3. `totlen > link->rcvbuf_len` 返回 1。
4. `ntohs(hdr->ver) != CLUSTER_PROTO_VER` 返回 1（协议版本不同无法处理）。
5. `type == server.cluster_drop_packet_filter` 时按调试过滤器丢包返回 1。
6. **按类型计算期望长度 `explen`**，然后要求 `totlen == explen`，不等就记警告返回 1。各类别的期望长度算法不同：
   - `PING`/`PONG`/`MEET`：`sizeof(clusterMsg)-sizeof(union clusterMsgData)` 加上 `sizeof(clusterMsgDataGossip)*count`；`totlen < explen` 记警告返回 1。
   - 若报文带 `CLUSTERMSG_FLAG0_EXT_DATA`，还要逐个校验扩展：每个扩展必须放得下；`extlen < sizeof(clusterMsgPingExt) || extlen % 8 != 0` 记警告（没按 8 字节补齐）；按扩展类型分别校验——`HOSTNAME` / `HUMAN_NODENAME` 必须以 `\0` 结尾并通过 `isValidHostnameString` / `isValidAuxString`；`FORGOTTEN_NODE`、`SHARDID`、`INTERNALSECRET` 各有最小长度检查，`SHARDID` 还要过 `verifyClusterNodeId`。源码注释说明了严格校验的理由：这些字符串会进 nodes.conf 也会回给客户端，控制字符或分隔符会同时污染两者。
   - `FAIL`：加 `sizeof(clusterMsgDataFail)`。
   - `PUBLISH` / `PUBLISHSHARD`：加 `sizeof(clusterMsgDataPublish)-8` 再加频道长度与消息长度，并检查加法溢出。
   - `FAILOVER_AUTH_REQUEST` / `FAILOVER_AUTH_ACK` / `MFSTART`：只有头部。
   - `UPDATE`：加 `sizeof(clusterMsgDataUpdate)`。
   - `MODULE`：加 `sizeof(clusterMsgModule)-3` 再加消息长度，并检查溢出。
   - 未知类型：`explen = totlen`，注释说「不知道这个包，就假定它格式正确」。
7. **发送方地址校验**：`isValidClusterMsgIp(hdr->myip)` 不通过则记 verbose 日志返回 1。源码注释解释：公告地址会被写进 nodes.conf、CLUSTER NODES 与重定向回复，写坏了会污染持久化状态。

**第二阶段：定位发送方并同步 epoch。**

8. `getNodeFromLinkAndMsg(link, hdr)` 定位发送方。源码注释说明：知道 sender 节点却还没和 link 关联时，这一定是**入站** link——因为只有入站 link 在创建时不知道该关联谁——于是补上 `setClusterNodeToInboundClusterLink`。若报文带扩展数据标记，则给 sender 打上 `CLUSTER_NODE_EXTENSIONS_SUPPORTED`。
9. `sender->data_received = now`。源码注释给出理由：否则一个因为 Pub/Sub 而在总线上大量发数据的节点会被误判超时。
10. 非握手中状态下：若 `senderCurrentEpoch > server.cluster->currentEpoch` 就抬高本节点的 currentEpoch；若 `senderConfigEpoch > sender->configEpoch` 就更新它并置 `CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_FSYNC_CONFIG`。同时刷新 `sender->repl_offset` 与 `repl_offset_time`；若本节点是正在做手动故障转移的副本、且主节点发来的报文带 `CLUSTERMSG_FLAG0_PAUSED`，则记下 `server.cluster->mf_master_offset`。

**第三阶段：按类型处理。**

11. **`PING` / `MEET` 的附加动作**：
    - **自地址发现**：收到 `MEET` 时（或者自己还没有 IP 时）用 `connAddrSockName` 从 socket 反推对端 IP 覆盖 `myself->ip`。源码注释解释得很清楚：只有其他集群节点会在握手、入群或改地址时给我们发 MEET，那些节点会用我们的官方地址连回来，所以从 socket 反推自己的地址是最简单的发现方式；没有地址时即使收到普通 PING 也先更新，错了后面 MEET 会纠正。
    - **MEET 且不认识 sender**：用 `nodeIp2String` 取 IP，失败就 `freeClusterLink(link)` 并返回 0——注释说拿不到有效 IP 的节点在集群里没法用，可能是连接已经关闭。成功则 `createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE)`、`clusterAddNode(node)`，置 `CLUSTER_TODO_SAVE_CONFIG`。注释还说明此时不急着设主从指针，这些细节要等收到该节点的 PONG 才定。
    - **不认识 sender 的 MEET 也要处理 gossip**：`clusterProcessGossipSection(hdr, link)`，注释解释：因为消息类型本身要求信任发送方。
    - **一律回 PONG**：`clusterSendPing(link, CLUSTERMSG_TYPE_PONG)`。
12. **`PING`/`PONG`/`MEET` 的配置信息处理**：
    - **出站 link 的握手收尾**：若 `link->node` 处于 `CLUSTER_NODE_HANDSHAKE`：认识 sender 就更新地址、然后 `clusterDelNode(link->node)` 并返回 0（注释：free 掉这个节点会连带 free 掉 link）；不认识 sender 就 `clusterRenameNode(link->node, hdr->sender)` 换成真名、去掉 HANDSHAKE 标记、按 sender 公告的 flags 设置 MASTER/SLAVE。
    - **ID 不匹配**：出站 link 且 `link->node->name` 与 `hdr->sender` 不一致时，给节点打 `CLUSTER_NODE_NOADDR`、清空 ip 与各端口、`freeClusterLink(link)`、返回 0。
    - **复制 `CLUSTER_NODE_NOFAILOVER` 标志**：源码注释说这是动态标志，必须信发送方的最新状态，因为投票时要用它判断各副本参与竞争的真实延迟。
    - **地址更新**：`nodeUpdateAddressIfNeeded`。
    - **PONG 到达**：记 `pong_received`、清 `ping_sent`；若 `nodeTimedOut` 则清 PFAIL 标记；若已 FAIL 则调 `clearNodeFailureIfNeeded`。
    - **角色切换检测**：按 `hdr->slaveof` 判定 sender 是主还是从。**主转从**时：若新主与 sender 属于同一 shard 且 `sender->configEpoch > senderConfigEpoch`，记「忽略陈旧消息」并**直接返回 1**——源码注释解释：忽略这个包的剩余部分，防止它回退更新的拓扑变更、造出非法的复制链；否则同 shard 就 `clusterMoveNodeSlots(sender, master)` 把槽位移给新主、`clusterSetNodeAsMaster(master)` 并更新其 configEpoch；跨 shard 则 `clusterDelNodeSlots(sender)` 收回槽位归属。**副本换主**时更新 `slaveof` 指针与主从列表，并更新 shard_id。
    - **子副本自愈**：若 `grandmaster = myself->slaveof->slaveof` 存在且我们认为它是主，则 `clusterSetMaster(grandmaster)` 把自己改挂到孙主上，并置 `CLUSTER_TODO_SAVE_CONFIG|UPDATE_STATE|FSYNC_CONFIG|BROADCAST_PONG`。源码注释解释了这个保护为什么必要：我们的主可能刚被降级，而 `clusterUpdateSlotsConfigWith()` 里的同类检查覆盖不到，因为降级处理已经把槽位交到新主手里、差异消失了；这个检查跑在每个包上，因此也能救回「先知道降级、后知道新主」的节点；并且只跟随我们认为是主的孙主，避免尚未稳定的链让我们从一个不持有槽位的节点重新同步。
    - **槽位归属比对**：若 sender 是主且它公告的槽位位图与我们记录的它的槽位位图不一致（`dirty_slots`），调 `clusterUpdateSlotsConfigWith(sender, senderConfigEpoch, hdr->myslots)`。
    - **反向通知陈旧配置**：若 sender 公告的某个槽位当前属主的 configEpoch 更大（说明 sender 的配置已过时），给它发一条 `clusterSendUpdate`。源码注释给了完整场景：A/B 是槽 1,2,3 的主从，A 被隔离后 B 提升；B 再被隔离、A 恢复时，通常是 B 主动 PING A 宣告自己的槽位与 configEpoch，但分区期间 B 通知不到 A，所以要靠别的节点把 UPDATE 发过去。
    - **configEpoch 冲突**：自己与 sender 都是主且 configEpoch 相同时调 `clusterHandleConfigEpochCollision(sender)`。
    - **gossip 与扩展**：调 `clusterProcessGossipSection(hdr, link)` 与 `clusterProcessPingExtensions(hdr, link, sender)`。
13. **`FAIL`**：sender 已知、被报告节点存在且不是自己、也还没被标 FAIL 时，打 FAIL 标记、记 `fail_time`、清 PFAIL 标记；sender 未知则记「忽略来自未知节点的 FAIL 消息」。
14. **`PUBLISH` / `PUBLISHSHARD`**：sender 未知直接返回；只有本地确实有订阅者时才创建 channel/message 对象并调 `pubsubPublishMessage`——总线在这里只是跨节点扩散的一个转发点，订阅通道的维护不在本页范围。
15. **`FAILOVER_AUTH_REQUEST`**：`clusterSendFailoverAuthIfNeeded(sender, hdr)`。
16. **`FAILOVER_AUTH_ACK`**：只有 sender 是持有非零槽位的主、且它的 currentEpoch 不小于我们发起选举时的 epoch，才计一票 `failover_auth_count++`，并置 `CLUSTER_TODO_HANDLE_FAILOVER` 让它尽快检查是否达到法定多数。
17. **`MFSTART`**：只接受「我是主且 sender 是我的副本」。处理是：取消所有 ASM 任务、`resetManualFailover()`、`mf_end = now + CLUSTER_MF_TIMEOUT`（该常量定义为 5000 毫秒）、`mf_slave = sender`、`pauseActions(PAUSE_DURING_FAILOVER, now + CLUSTER_MF_TIMEOUT * CLUSTER_MF_PAUSE_MULT, PAUSE_ACTIONS_CLIENT_WRITE_SET)`（`CLUSTER_MF_PAUSE_MULT` 定义为 2），最后回一条 PING。源码注释解释为什么必须回这条 PING：它会带上 `server.cluster->mf_master_offset`，副本据此知道主节点把客户端暂停在哪个偏移上，从而可以安全地把 `mf_can_start` 置 1，尽快完成故障转移。
18. **`UPDATE`**：sender 未知、被报告节点未知都返回；`n->configEpoch >= reportedConfigEpoch` 时返回（没有新信息）；若该节点在我们视图里是副本则 `clusterSetNodeAsMaster(n)`；更新 configEpoch 并置 `CLUSTER_TODO_SAVE_CONFIG|CLUSTER_TODO_FSYNC_CONFIG`；最后 `clusterUpdateSlotsConfigWith(n, reportedConfigEpoch, hdr->data.update.nodecfg.slots)`。
19. **`MODULE`**：sender 未知则返回——源码注释写明这是「保护模块不受未知节点影响」；随后取 `module_id`、`type`、`len` 调 `moduleCallClusterReceivers`。
20. 未知类型：记警告「Received unknown packet type」。

### 三、槽位归属的仲裁

**`clusterUpdateSlotsConfigWith`** 是槽位表真正被改写的地方。它遍历全部 16384 个槽：

- sender 仍声明该槽位、且该槽位已归 sender → 清掉 `owner_not_claiming_slot` 中对应位，继续。
- 该槽位正在 import（`server.cluster->importing_slots_from[j]` 非空）→ 跳过。源码注释说明：这种状态只能在 redis-cli 里手工改（例如分片迁移中迁出侧已经关闭、正在公告新配置）。
- 满足「槽位未分配，或原属主的 configEpoch 小于 sender 的 configEpoch，且我们不在 import」→ 若槽位原属我、里面还有键、且 sender 不是我，记进 `dirty_slots` 列表（注释：失去槽位归属但里面还有键，常见于故障转移或管理员手工重配）；然后 `clusterDelSlot(j)` + `clusterAddSlot(sender, j)`，并置 `CLUSTER_TODO_SAVE_CONFIG|UPDATE_STATE|FSYNC_CONFIG`。
- **sender 不再声明该槽位、但槽位仍绑在 sender 上时，不立即解绑**，而是置 `owner_not_claiming_slot` 中对应位。源码注释给出理由：立即解绑可能让集群进入 FAIL 状态并给客户端报错；保留旧属主只会多几次客户端重定向而不会报错；同时把这个归属的不确定性记下来，避免用 UPDATE 报文把错误信息传播出去。

**gossip 段**（`clusterProcessGossipSection`）负责节点级信息的传播：

- 先用 `verifyGossipSectionNodeIds` 校验 gossip 里的所有节点 ID，有非法 ID 就整段放弃——注释说非法 ID 表明发送方内存损坏。
- 逐条处理时，`isValidClusterMsgIp(g->ip)` 不通过就只跳过这一条。源码注释解释：发送方只是转发（可能转的是它自己 nodes.conf 里的地址），坏条目不该让它丢掉自己携带的状态和 PONG。
- 对已知节点：若 sender 是主且 gossip 带 FAIL/PFAIL 标志 → `clusterNodeAddFailureReport`；否则 `clusterNodeDelFailureReport`（节点恢复上报）。
- 若该节点在我们视图里是活的、我们没有待发的 ping、也没有失败报告 → 用 gossip 里的 `pong_received` 刷新它的时间戳，但要求它「比我方视图更大、且不在我方时钟未来 500 毫秒以内」。
- 若该节点被我们判为 FAIL/PFAIL，但 gossip 里的地址与我们记录的不同、且对方在 gossip 里是活的 → 释放旧 link、替换地址与端口、清掉 NOADDR 标记。
- 对未知节点：只有当 **sender 本身是已知节点**、gossip 条目没带 NOADDR、且不在黑名单里，才 `createClusterNode` 并加入节点表与分片表。源码注释强调了两条约束：一是**不能**直接对这个 IP/端口发起握手，因为 IP/端口可能已被复用，否则会误加入另一个集群；二是要求 gossip 的发送方是已知节点，否则同样有加入别的集群的风险。

### 四、总线的发送侧

- `clusterSendPing` 构造 PING/PONG 包头并填充 gossip 段。gossip 条目数 `wanted = floor(节点数/10)`，不足 3 条则补到 3 条。源码注释详细推导了这个比例：在一个 `node_timeout` 内每个节点至少交换 4 个包，失败报告的有效期是 `node_timeout*2`，由此算出「1/10 条」能在窗口内拿到约 80% 节点数的失败报告，超过多数，且能容忍多个主同时故障。所有 PFAIL 节点会被额外追加进包里，注释说是「加快 PFAIL 传播到 FAIL」。
- `clusterBroadcastMessage` 遍历节点表，跳过自己与处于 HANDSHAKE 的节点，把同一个消息块发给其余每个节点。源码注释保证这个函数不会让任何 `node->link` 失效，因此可以安全地从事件回调里调。
- `clusterSendMessage` 把消息块挂到 link 的发送队列尾部；队列原本为空且消息非空时用 `connSetWriteHandlerWithBarrier` 装上写事件。
- `clusterLinkConnectHandler` 在连接建立后立刻注册读处理器，并马上发一条 PING。源码注释说这一步「非常关键，能避免失败检测的误报」：若节点带 MEET 标记则发 MEET 而不是 PING，发完清掉 MEET 标记。

### 五、入站侧：接受总线连接

握手有两端。**出站端**是 `clusterStartHandshake`：它先做 IP 与端口的合法性检查（IP 必须是可被 `inet_pton` 解析的 v4/v6，端口与集群端口都必须在 1–65535），把 IP 归一化后用 `clusterHandshakeInProgress` 拒绝重复握手，然后 `createClusterNode(NULL, CLUSTER_NODE_HANDSHAKE|CLUSTER_NODE_MEET)` 建一个**地址是随机值**的节点。源码注释解释为什么不能直接按对方通告的 IP/端口建节点：端口可能被别的集群复用，那样会误加入另一个集群——真正的地址要等握手拿到对方的真名后再定。

**入站端**是 `clusterAcceptHandler`：单次调用最多接受 `MAX_CLUSTER_ACCEPTS_PER_CALL`（源码里定义为 1000）个连接；服务端正在载入数据且本机不是副本时直接不接，源码注释给的理由是「UPDATE 消息可能与数据库内容发生交互」。每条连接都用 `TLS_CLIENT_AUTH_YES` 且 `is_cluster_bus = 1` 的 TLS 接受配置，开了 `TCP_NODELAY`，并把 keepalive 设为 `cluster_node_timeout / 1000 * 2`（`cluster-node-timeout` 在 `src/config.c` 里默认 15000 毫秒）。源码注释还写明：当配置了 `tls-expected-peer-name` 时，会在 TLS 握手完成前校验对端证书的 SAN/CN——注释说这是「堵上集群总线的节点 ID 冒充向量：一张能链到 CA 但缺少集群身份的证书（例如共享 CA 下的兄弟证书）无法打开总线连接并注入伪造消息」。

## 关键取舍

**源码事实：** 总线是**独立端口**而不是复用客户端端口，代价是多占一个监听端口与一个连接，收益是集群内部流量与客户端流量在 TCP 层就分开，不会互相拖累。**源码事实：** 槽位数固定为 16384 且每槽一位，用固定位图而不是动态表，换来的是「用位图比对判断归属是否变化」这种 O(16384/8) 的廉价操作。**源码事实：** 仲裁用的是 `(节点, configEpoch)` 而不是共识协议：每个主节点自己递增 configEpoch，谁大谁说了算；相等时靠 `clusterHandleConfigEpochCollision` 处理冲突。**源码事实：** gossip 是**概率性**的：每个包只带约 1/10 节点的条目，其余靠多次交换补齐。**源码事实：** 对「sender 不再声明某个槽位」这类不确定状态，Redis 选择**不立即解绑**而是标记 `owner_not_claiming_slot`。**源码事实：** 报文处理里对长度、扩展对齐、字符串终止符、IP 合法性都有显式检查，且长度检查用加法溢出判断。

**架构推论：** 用 configEpoch 的大小关系代替 Paxos/Raft 这类共识协议，让 Redis Cluster 的故障转移能在几秒内完成，代价是它**不是**强一致：网络分区两侧可能同时出现两个自称拥有同一批槽的主节点（split-brain），Redis 用「客户端收到 MOVED 后重定向」和「多数派授权投票」来收敛，而不是靠共识本身保证唯一。**架构推论：** gossip 用概率抽样而非全量广播，把总线带宽从 O(N²) 压到 O(N×N/10)；代价是失败报告的传播有延迟，源码注释里 80% 的推导正是为了让这个延迟仍在法定多数的判定窗口内。**架构推论：** 保留旧槽位归属而不是立即解绑，是把「一致性问题」换成了「可用性问题」——客户端可能收到指向已降级节点的重定向，但不会收到「槽位无主」这种硬错误。**架构推论：** 总线报文处理是单线程串行的，意味着一个节点在同一时刻只能处理一帧报文；Redis 因此刻意把 gossip 段做小，代价是节点数很大时新节点的发现会变慢。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/cluster_legacy.c` | `clusterInit` | 集群状态的装配：建表、清槽位、独占 nodes.conf、载入或新建配置、端口与绑定地址校验、生成内部密钥 |
| `src/cluster_legacy.c` | `clusterProcessPacket` | 单帧总线报文的完整处理：长度与扩展校验、发送方定位、epoch 同步、按类型分派、槽位与角色更新 |

## 参考

- [Redis 源码：cluster_legacy.c](https://github.com/redis/redis/blob/unstable/src/cluster_legacy.c)
- [Redis 文档：集群规范](https://redis.io/docs/latest/operate/oss_and_stack/reference/cluster-spec/)
- [Redis 文档：Redis Cluster](https://redis.io/docs/latest/operate/oss_and_stack/management/scaling/)
- [Redis 命令：CLUSTER](https://redis.io/docs/latest/commands/cluster/)

## 与主核 / AI 的关系

集群总线的核心机制是「**用单调递增的版本号代替共识协议**」：主核若做多副本协调，同样面对「谁说了算」的问题。Redis 的答案是 configEpoch——谁的版本号大谁赢，冲突时再走一次多数派授权投票。这套做法实现简单、收敛快，但只在「少数派节点最终会重新加入」的前提下成立。主核是多写者引擎且没有单一写节点，无法直接套用「槽位归属 + 版本号」这套模型；若主核需要跨节点的写入协调，更可能需要显式的租约或共识层，而不是靠版本号大小关系。

AI 侧若把 Redis Cluster 当向量检索的横向扩展单元，要特别注意一点：**集群总线里的槽位归属表与真实数据分布是两套东西**。槽位只决定「键归谁管」，不保证「语义相近的向量落在同一个节点」——Redis Cluster 的哈希分片对向量检索毫无帮助。因此若要做向量检索的集群化，正确的做法是自己维护「向量 → 节点」的路由表，让检索请求直接打到持有目标向量的节点，而不是依赖 `MOVED` 重定向；后者会退化成逐节点重试，延迟与召回率都不可控。
