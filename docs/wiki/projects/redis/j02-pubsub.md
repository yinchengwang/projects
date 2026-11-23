---
project: redis
feature: pubsub
page_type: feature
schema_version: 1
refs:
  - path: src/pubsub.c
    symbols: [pubsubPublishMessage]
---

# 发布订阅通道

## 一句话职责

`pubsubPublishMessage` 是发布侧的唯一入口：它先按 `sharded` 参数在 `pubSubType` 与 `pubSubShardType` 两个类型描述符之间二选一，再在服务端保存的「名字 → 订阅者集合」反向索引里定位目标集合，把消息逐条写进每个订阅者的回复缓冲，返回本次在本节点真正写出消息的接收者数量。通道与订阅关系本身的增删由同文件的 `pubsubSubscribeChannel` / `pubsubUnsubscribeChannel` / `pubsubSubscribePattern` / `pubsubUnsubscribePattern` 负责，`pubsubPublishMessage` 只读这些索引，不改任何订阅状态。

## 机制是什么

Pub/Sub 与键空间完全无关：频道名不进 `server.db[].keys`，发布不受 `SELECT` 的库号影响，也没有持久化。它在 `server` 里维护三套并行的反向索引：

- `server.pubsub_channels`：全局频道表，类型是 `kvstore`，由 `initServer` 以 `slot_count_bits = 0` 创建，因此只有一个 dict。源码注释写明它特意选 `kvstore` 而不是 `dict`，只是为了与 `server.pubsubshard_channels` 同型、让 `pubsubtype.serverPubSubChannels` 能用统一签名。
- `server.pubsubshard_channels`：分片频道表，同样由 `initServer` 创建，但 `slot_count_bits` 在集群模式下取 `CLUSTER_SLOT_MASK_BITS`（`src/cluster.h` 定义为 14，即 16384 个桶），并额外带 `KVSTORE_FREE_EMPTY_DICTS`；非集群模式下该值为 0，退化成单 dict。
- `server.pubsub_patterns`：模式表，是普通 `dict`，按 `objToDictDictType` 建，不做槽分桶。

三张表的 value 都是「订阅者集合」：前两张的值是 `dictCreate(&clientDictType)` 造出的、以 `client*` 为键的 dict（`clientDictType` 带 `no_value = 1`，只存键不存值），模式表的值同型。客户端一侧对称地持有三张表 `c->pubsub_channels` / `c->pubsub_patterns` / `c->pubsubshard_channels`，它们的 value 不是计数而是 **ACL 归属标记**：`NULL` 表示「归当前 `c->user` 所有」，非 `NULL` 的 `user*` 表示身份切换时冻结下来的归属快照。

订阅计数的口径在源码里是分开的两套：

- `clientSubscriptionsCount` = `dictSize(c->pubsub_channels) + dictSize(c->pubsub_patterns)`，全局频道与模式相加，被 `pubSubType.subscriptionCount` 使用。
- `clientShardSubscriptionsCount` = `dictSize(c->pubsubshard_channels)`，只算分片频道，被 `pubSubShardType.subscriptionCount` 使用。
- `clientTotalPubSubSubscriptionCount` = 前两者相加，用于判断「客户端是否还处于订阅态」。

因此 `subscribe` 回复里第三个元素的数字，在全局订阅下是「频道 + 模式」总数，在分片订阅下**只算分片频道**；而「能否退出订阅态」始终按三者之和判断。

投递语义是 at-most-once：发布路径只把消息写进接收方的回复缓冲，不落任何持久队列，写失败的订阅者拿不回这条消息。需要至少一次语义的调用方要改用 Stream（`f07` / `f08`）。

## 怎么实现的

### 一、`pubsubtype`：把「全局」与「分片」压成一套代码

`src/pubsub.c` 顶部定义的 `pubsubtype` 结构把两类通道的差异收进一张函数表，字段为 `shard`、`clientPubSubChannels`、`subscriptionCount`、`serverPubSubChannels`、`subscribeMsg`、`unsubscribeMsg`、`messageBulk`。两个实例在文件里是静态全局的：

- `pubSubType`：`shard = 0`，客户端表取 `getClientPubSubChannels`，计数取 `clientSubscriptionsCount`，服务端表指向 `server.pubsub_channels`，回复类型词取 `shared.subscribebulk` / `shared.unsubscribebulk` / `shared.messagebulk`。
- `pubSubShardType`：`shard = 1`，客户端表取 `getClientPubSubShardChannels`，计数取 `clientShardSubscriptionsCount`，服务端表指向 `server.pubsubshard_channels`，回复类型词取 `shared.ssubscribebulk` / `shared.sunsubscribebulk` / `shared.smessagebulk`。

这些类型词不是每次现拼字符串，而是 `createSharedObjects` 里预先造好的共享对象，字面量直接写在源码里：`"$7\r\nmessage\r\n"`（13 字节）、`"$8\r\npmessage\r\n"`（14 字节）、`"$9\r\nsubscribe\r\n"`（15 字节）、`"$11\r\nunsubscribe\r\n"`（18 字节）、`"$10\r\nssubscribe\r\n"`（17 字节）、`"$12\r\nsunsubscribe\r\n"`（19 字节）、`"$8\r\nsmessage\r\n"`（14 字节）、`"$10\r\npsubscribe\r\n"`（17 字节）、`"$12\r\npunsubscribe\r\n"`（19 字节）。长度都按源码给出的常量计数。

### 二、订阅：先建客户端索引，再挂到服务端索引

`pubsubSubscribeChannel` 的顺序是固定的：

1. `dictAddRaw(type.clientPubSubChannels(c), channel, NULL)` 先在客户端表里占位。返回非 `NULL` 才说明是**新订阅**（`retval = 1`），否则只是重复订阅。
2. 新订阅时，若 `server.cluster_enabled && type.shard` 为真，用 `getKeySlot(channel->ptr)` 取槽号，否则 `slot` 保持 0。
3. `kvstoreDictAddRaw(*type.serverPubSubChannels, slot, channel, &existing)`。`existing` 非 `NULL` 说明该通道已有人订阅，于是复用已有值：`clients = dictGetVal(existing)`，并把 `channel` 换成 `dictGetKey(existing)` 这个**服务端规范对象**，同时给 `server.stat_clients_watch_pubsub_shared_overcount` 加上该名字的对象内存。没有 `existing` 时新建 `clientDictType` 的 dict 并 `incrRefCount(channel)`。
4. `dictAdd(clients, c, NULL)` 把订阅者塞进集合，随后把客户端条目的键改成规范对象、值显式写成 `NULL`（源码注释强调 `dictAddRaw` 不会初始化 value，必须显式写 `NULL` 来表达「归当前用户所有」），再 `incrRefCount(channel)` 并累加 `c->pubsub_names_mem`。
5. 无论新旧订阅，末尾都调用 `addReplyPubsubSubscribed` 回复一次。

`pubsubSubscribePattern` 走另一套路径：先 `dictFind(server.pubsub_patterns, pattern)` 找已存在的模式并改用其规范键，再 `dictAdd(c->pubsub_patterns, pattern, NULL)`；新订阅时才把该模式登记进服务端表。`subscribeCommand` / `psubscribeCommand` / `ssubscribeCommand` 在循环外各自调 `markClientAsPubSub`，后者以 `CLIENT_PUBSUB` 标志为准做幂等，并维护 `server.pubsub_clients` 计数。

### 三、发布：`pubsubPublishMessage` 的两段式投递

`pubsubPublishMessage(channel, message, sharded)` 只有一行实质逻辑：按 `sharded` 选类型后转调 `pubsubPublishMessageInternal`。后者是真正的投递循环：

1. 若 `server.cluster_enabled && type.shard`，用 `keyHashSlot(channel->ptr, sdslen(channel->ptr))` 算槽，否则 `slot = 0`。
2. `kvstoreDictFind(*type.serverPubSubChannels, slot, channel)` 直接按名字索引到订阅者集合，命中才遍历。这与 `PUBSUB NUMSUB` 走的 `kvstoreDictFetchValue` 是同一张表、同一段键。
3. 对集合里每个 `client*`：先 `addReplyPubsubMessage(c, channel, message, *type.messageBulk)`，再在 `clusterSlotStatsEnabled(CLUSTER_SLOT_STATS_NET)` 为真时调 `clusterSlotStatsAddNetworkBytesOutForShardedPubSubInternalPropagation(c, slot)`，最后 `updateClientMemUsageAndBucket(c)` 刷新该客户端的内存分桶，`receivers++`。
4. 若 `type.shard` 为真，**到此直接返回**——源码注释写明「Shard pubsub ignores patterns」，分片频道永远不匹配模式。
5. 全局类型才继续第二段：先 `getDecodedObject(channel)` 拿到可安全读取的字符串对象，遍历 `server.pubsub_patterns` 全部模式，用 `stringmatchlen(pattern, sdslen(pattern), channel, sdslen(channel), 0)` 判断是否命中（大小写敏感，`nocase = 0`），命中的模式再遍历其订阅者，逐个 `addReplyPubsubPatMessage(c, pattern, channel, message)` 并同样刷新内存分桶。遍历结束后 `decrRefCount` 解码对象。

同一客户端可被投递多次：既订阅了频道又订阅了匹配该频道的模式时，会分别收到一条 `message` 和一条 `pmessage`。

### 四、回复帧：为什么要在 pubsub 里改写 `CLIENT_PUSHING`

`addReplyPubsubMessage` 先备份 `c->flags`，置上 `CLIENT_PUSHING`（`src/server.h` 定义为 `1ULL<<46`），然后按协议版本分叉：`c->resp == 2` 时用共享头 `shared.mbulkhdr[3]` 写出 `*3` 数组头，否则用 `addReplyPushLen(c, 3)`；之后依次追加类型词、频道名、消息体。收尾时只在「进来时本来没有该标志」的前提下清除它，避免把外层已置位的状态清掉。

这个标志有两处实际作用：`_prepareClientToWrite` 里 `CLIENT_REPLY_OFF|CLIENT_REPLY_SKIP` 的静默逻辑遇到 `CLIENT_PUSHING` 会放行，保证客户端关掉回复后仍能收到推送；`_addReplySegmentsToBufferOrList` 里当推送目标是 `server.current_client` 且当前不是 SUBSCRIBE 家族命令时，把字节挂到 `server.pending_push_messages` 而不是 `c->reply`，等命令回复写完再由 `afterCommandEx` 通过 `listJoin(c->reply, server.pending_push_messages)` 追加，避免 MULTI/EXEC 里推送插到回复中间。判断「是不是 SUBSCRIBE 家族」用的是 `cmdHasPushAsReply`，它认的函数正是 `subscribeCommand` / `unsubscribeCommand` / `psubscribeCommand` / `punsubscribeCommand` / `ssubscribeCommand` / `sunsubscribeCommand`。

`addReplyPubsubPatMessage` 同构，只是数组头变成 `mbulkhdr[4]` / `addReplyPushLen(c, 4)`，元素为 `shared.pmessagebulk`、模式、频道、消息体。`addReplyPubsubSubscribed` / `addReplyPubsubUnsubscribed` 都是 3 元素：类型词、频道名、当前订阅计数；频道为空时（`addReplyPubsubUnsubscribed` 收到 `NULL`）用 `addReplyNull` 占位。`addReplyPubsubPatSubscribed` / `addReplyPubsubPatUnsubscribed` 固定用 `clientSubscriptionsCount` 做计数。

### 五、命令层与错误分支

- `subscribeCommand`：先判断 `(c->flags & CLIENT_DENY_BLOCKING) && !(c->flags & CLIENT_MULTI)`，命中就 `addReplyError` 报 `"SUBSCRIBE isn't allowed for a DENY BLOCKING client"` 并返回。源码注释说明 MULTI 例外是为向后兼容。`psubscribeCommand` 逻辑相同，只换错误词。`ssubscribeCommand` 则是**只判 `CLIENT_DENY_BLOCKING`，没有 MULTI 例外**。
- `unsubscribeCommand`：`c->argc == 1` 走 `pubsubUnsubscribeAllChannels(c, 1)`，否则逐个 `pubsubUnsubscribeChannel(c, c->argv[j], 1, pubSubType)`；结束时若 `clientTotalPubSubSubscriptionCount(c) == 0` 就 `unmarkClientAsPubSub`。`punsubscribeCommand` / `sunsubscribeCommand` 同构，分片版走 `pubSubShardType`。
- 批量退订的空回复：`pubsubUnsubscribeAllChannelsInternal` 与 `pubsubUnsubscribeAllPatterns` 在 `notify` 为真但一个都没退掉时，仍会调 `addReplyPubsubUnsubscribed(c, NULL, type)` / `addReplyPubsubPatUnsubscribed(c, NULL)` 回一条计数为 0 的通知。
- `processCommand` 的限制：RESP2 且带 `CLIENT_PUBSUB` 时，只放行 `pingCommand`、六个 SUBSCRIBE 家族命令、`quitCommand`、`resetCommand`，其余走 `rejectCommandFormat` 报 `"Can't execute '%s': only (P|S)SUBSCRIBE / (P|S)UNSUBSCRIBE / PING / QUIT / RESET are allowed in this context"`。RESP3 不受限。`pingCommand` 也据此分叉：订阅态 RESP2 下回 `mbulkhdr[2]` 包的 `pong` 而不是 `+PONG`。
- `publishCommand`：`server.sentinel_mode` 为真时**不投递**，直接交给 `sentinelPublishCommand`，后者只接受 `SENTINEL_HELLO_CHANNEL`（`src/sentinel.c` 定义为 `"__sentinel__:hello"`）频道，否则报错 `"Only HELLO messages are accepted by Sentinel instances."`。正常路径调 `pubsubPublishMessageAndPropagateToCluster(c->argv[1], c->argv[2], 0)`，非集群时再 `forceCommandPropagation(c, PROPAGATE_REPL)`——`PUBLISH` 的命令表带 `MAY_REPLICATE` 而**不**带 `WRITE`，`forceCommandPropagation` 开头的 `serverAssert` 正是靠这两者之一放行。`spublishCommand` 同构，`sharded` 传 1。
- `pubsubCommand` 的子命令：`CHANNELS` / `SHARDCHANNELS` 走 `channelList`（遍历 `kvstoreNumDicts` 个桶，用 `stringmatchlen` 过滤后写 deferred 长度）；`NUMSUB` 固定查 `server.pubsub_channels` 的 0 号 dict（该表只有一个 dict）；`SHARDNUMSUB` 用 `calculateKeySlot` 算槽；`NUMPAT` 直接回 `dictSize(server.pubsub_patterns)`；都不匹配则 `addReplySubcommandSyntaxError`。

### 六、退订、清理与槽迁移

- `pubsubUnsubscribeChannel` 用 `dictUnlink` 摘掉客户端条目后才 `dictFreeUnlinkedEntry`，并从服务端集合 `dictDelete(clients, c)`；集合空了就 `kvstoreDictDelete` 整个通道，源码注释的意图是避免空通道长期累积。`notify` 为 0 时不回包（客户端正在被释放）。这里算槽特意**不用 `getKeySlot`**，而用 `keyHashSlot`，注释写明退订的触发者可以是另一个客户端，`getKeySlot` 会返回那个客户端缓存的槽号。
- `pubsubShardUnsubscribeAllChannelsInSlot` 是分片频道唯一的整槽清理入口，真实调用方是 `src/cluster.c` 的 `removeChannelsInSlot`（它先按 `countChannelsInSlot` 判空直接返回）。`removeChannelsInSlot` 的调用方有两处：`clusterNotifyTopologyChanged`（槽不再归本分片时逐槽调用）与 `src/cluster_legacy.c` 的 `removeAllNotOwnedShardChannelSubscriptions`（后者遍历 `CLUSTER_SLOTS` 逐槽比对 `server.cluster->slots[j] != currmaster`，实际调用点是 `clusterSetMaster`）。
- `src/networking.c` 的 `clearClientPubSubState` 是客户端侧的统一清理：仅当 `CLIENT_PUBSUB` 置位时才依次调 `pubsubUnsubscribeAllChannels(c, 0)`、`pubsubUnsubscribeShardAllChannels(c, 0)`、`pubsubUnsubscribeAllPatterns(c, 0)`、`updateClientWatchPubsubMemoryUsage(c)`、`unmarkClientAsPubSub(c)`（`notify = 0`，不推送）。它的调用方是 `clearClientConnectionState`（`RESET`）、`deauthenticateAndCloseClient`（ACL 杀客户端）和 `freeClient`。
- ACL 归属快照：`clientSetUser` 在 `auth_changed` 为真时调 `pubsubStampCurrentUser`，把仍是 `NULL` 的订阅条目写成当前用户（`c->user` 为空则写哨兵 `pubsubNoAuthUser`）并置 `CLIENT_PUBSUB_REAUTHED`（`1ULL<<55`）；无用户场景靠这个哨兵避免写 `NULL`（`NULL` 有「当前用户」的语义）。`ACLFreeUserAndKillClients` 用 `pubsubClientHasStampedOwner` 找到持有该用户快照的客户端并 `deauthenticateAndCloseClient`；`ACLShouldKillPubsubClient` 用 `pubsubDictHasDeniedSubForOwner` 判定；`ACLLoadFromFile` 用 `pubsubACLLoadReconcileClient` 做「先只读校验、后改写键」两阶段，被杀客户端不会留下半改状态。

## 关键取舍

- **源码事实：** 分片频道与模式订阅在结构上互斥——`pubsubPublishMessageInternal` 在 `type.shard` 分支直接返回，`SPUBLISH` 的消息永远不参与模式匹配。
- **源码事实：** 全局频道表故意用「只有一个 dict 的 `kvstore`」而不是 `dict`，注释写明的理由是与分片表同型、让 `pubsubtype` 用统一签名，代价是多一层 kvstore 间接。
- **源码事实：** 通道在最后一个订阅者离开时被整体删除，换来的是「空闲通道不占内存」，代价是 `PUBSUB CHANNELS` 看不到历史通道名。
- **源码事实：** 订阅计数口径分裂：全局回复用「频道 + 模式」，分片回复只算分片频道，但退出订阅态统一用三者之和。这是两套 `subscriptionCount` 函数指针带来的直接结果。
- **源码事实：** 推送帧固定改写 `CLIENT_PUSHING`，使推送绕过 `CLIENT REPLY OFF` / `SKIP`，并把与当前命令同客户端的推送推迟到 `server.pending_push_messages` 之后拼接，避免事务回复被推送打断。
- **源码事实：** 订阅态在 RESP2 下被收窄成一小组命令，RESP3 才解除限制；`PING` 在订阅态下也被改写成 2 元素推送形状。
- **架构推论：** 用「客户端表 + 服务端反向表」双写而不是单向索引，是为了让订阅与退订都是 O(1) 名字查找，而发布只需一次服务端查找；代价是两侧必须严格同步，因此代码里大量出现 `serverAssert` 与 `serverAssertWithInfo` 兜底。
- **架构推论：** 分片频道按 `keyHashSlot` 的 `crc16 & 0x3FFF` 分槽，却不参与键的读写路径，说明它复用的是「槽 → 节点」的路由语义，而不是键空间语义；这也是 `SSUBSCRIBE` 不受跨槽约束的原因。
- **架构推论：** ACL 归属用「NULL 代表当前用户、非 NULL 代表快照」而不是给每条订阅存独立用户字段，是为了让绝大多数客户端（从未在订阅中切换身份）省掉额外字段和遍历；`CLIENT_PUBSUB_REAUTHED` 就是这个优化的快速路径标记。
- **架构推论：** `PUBLISH` 不带 `WRITE` 只带 `MAY_REPLICATE`，说明在命令表语义里它不被当作写命令，但非集群模式仍需要显式 `forceCommandPropagation` 才能进复制流——即「发布要跨节点可见」这件事由集群总线或复制链各自承担，而不是靠命令表自动传播。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/pubsub.c` | `pubsubPublishMessage` | 发布入口，按 `sharded` 选类型后转调内部投递 |
| `src/pubsub.c` | `pubsubPublishMessageInternal` | 频道集合投递 + 全局类型的模式匹配投递，返回接收者数 |
| `src/pubsub.c` | `pubsubSubscribeChannel` / `pubsubUnsubscribeChannel` | 全局与分片频道通用的订阅/退订，维护两侧索引 |
| `src/pubsub.c` | `pubsubSubscribePattern` / `pubsubUnsubscribePattern` | 模式订阅/退订，只动 `server.pubsub_patterns` |
| `src/pubsub.c` | `addReplyPubsubMessage` / `addReplyPubsubPatMessage` | 写 `message` / `pmessage` 推送帧，管理 `CLIENT_PUSHING` |
| `src/pubsub.c` | `pubsubShardUnsubscribeAllChannelsInSlot` | 清空某槽全部分片频道订阅，由 `removeChannelsInSlot` 调用 |
| `src/pubsub.c` | `publishCommand` / `spublishCommand` | 命令实现，含 Sentinel 分支与集群传播 |
| `src/pubsub.c` | `pubsubCommand` | `PUBSUB` 的 `CHANNELS` / `NUMSUB` / `NUMPAT` / 分片子命令 |
| `src/pubsub.c` | `pubsubStampCurrentUser` | 身份切换前冻结订阅归属快照 |
| `src/server.c` | `initServer` | 创建三套服务端索引并定槽桶位数 |
| `src/server.c` | `processCommand` | RESP2 订阅态的命令白名单 |
| `src/networking.c` | `clearClientPubSubState` | 客户端释放/重置时的统一退订 |
| `src/cluster_legacy.c` | `clusterPropagatePublish` | 全局广播或仅本分片扩散 `PUBLISH` / `SPUBLISH` |
| `src/cluster.c` | `removeChannelsInSlot` | 槽迁移后清理分片频道订阅 |
| `src/acl.c` | `clientSetUser` | 身份切换触发订阅归属标记 |

## 与主核 / AI 的关系

对主核（`engine/db/`）而言，Pub/Sub 的价值是一条**与存储解耦的旁路广播**：订阅者收到的是「发生了什么事」，而不是「读到了什么数据」。主核若要把变更通知暴露给上层（缓存失效、索引增量刷新、流式结果分发），可以照搬这套「名字 → 订阅者集合」的反向索引，因为它完全不碰键空间，不需要事务、不需要 WAL，也不会引入额外的持久化路径。

**架构推论：** 对 AI 场景，这套设计最值得借鉴的是「推送帧不进入命令回复流」的隔离手法——`server.pending_push_messages` 保证推送永远附在命令回复之后。LLM 的流式 token 分发如果直接塞进正常响应缓冲，就会在事务或批量请求下错位；主核若要给流式结果加推送通道，这条隔离规则比反向索引本身更关键。

**架构推论：** 反向的另一面是代价：`at-most-once` 意味着主核不能拿它做可靠的变更队列。标记失效丢一条就少一次刷新，这在缓存场景可接受，在索引一致性场景不可接受——那时应改用可回放的事件日志（对应 Redis 的 Stream 而非 Pub/Sub）。

## 参考

- [Redis 文档：Pub/Sub](https://redis.io/docs/latest/develop/pubsub/) —— 频道/模式/分片三类订阅的行为口径与 at-most-once 语义。
- [Redis 文档：PUBLISH 命令](https://redis.io/docs/latest/commands/publish/) —— 返回值的含义与集群下只统计本节点的限制。
- [Redis 源码：src/pubsub.c](https://github.com/redis/redis/blob/unstable/src/pubsub.c) —— 本页所有符号与常量的真实定义位置。
- [Redis 源码：src/commands/subscribe.json](https://github.com/redis/redis/blob/unstable/src/commands/subscribe.json) —— `SUBSCRIBE` 的命令表标志与参数定义。
