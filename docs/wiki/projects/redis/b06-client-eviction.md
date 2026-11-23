---
project: redis
feature: client-eviction
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [clientEvictionAllowed]
---

# 客户端按内存驱逐

## 一句话职责

给「客户端占用的内存总量」设一个上限，超了就从占用最大的客户端开始断开——但主库连接、副本连接和被显式保护的客户端永远不在可驱逐之列。

## 机制是什么

键的内存有 `maxmemory` 管（键级淘汰在 `src/evict.c`），**客户端自身的内存**则由另一个独立的上限 `maxmemory-clients` 管：当所有普通客户端与 pubsub 客户端占用的内存总量超过这个上限时，Redis 按占用从大到小踢连接，直到降回上限以下。

`clientEvictionAllowed` 是这条链路的**准入闸门**——它回答「这个客户端允许被驱逐吗」。判定结果不只影响驱逐本身，还影响这个客户端**会不会被纳入内存统计桶**（不可驱逐的客户端根本不入桶）。

本页只讲客户端级驱逐。键级 `maxmemory` 淘汰是另一条路径；单个客户端的输出缓冲越界判定在 b03。

## 怎么实现的

**准入闸门 `clientEvictionAllowed`。** 函数体只有几行：

```c
int clientEvictionAllowed(client *c) {
    if (server.maxmemory_clients == 0 || c->flags & CLIENT_NO_EVICT || !c->conn) {
        return 0;
    }
    int type = getClientType(c);
    return (type == CLIENT_TYPE_NORMAL || type == CLIENT_TYPE_PUBSUB);
}
```

三个否决条件：

- `server.maxmemory_clients == 0`——驱逐功能整体关闭。
- `c->flags & CLIENT_NO_EVICT`——客户端被显式标记为不可驱逐。`CLIENT_NO_EVICT` 在 `src/server.h` 里定义为 `(1ULL<<43)`，注释是「This client is protected against client ...」（客户端自己可以用 `CLIENT NO-EVICT` 打开这个标志）。
- `!c->conn`——没有连接的假客户端没有可断的连接。

然后是类型判定。`getClientType`（定义在 `src/networking.c`）把客户端分成四类，函数注释列得很清楚：`CLIENT_TYPE_NORMAL`（普通客户端，含 MONITOR）、`CLIENT_TYPE_SLAVE`（副本）、`CLIENT_TYPE_PUBSUB`（订阅了发布订阅通道的客户端）、`CLIENT_TYPE_MASTER`（代表我们上游主库的连接）。实现上先看 `CLIENT_MASTER` 标志，再看 `CLIENT_SLAVE`（但 MONITOR 虽然被打上副本标志，对外按普通客户端暴露），再看 `CLIENT_PUBSUB`，最后落到普通。

**只有 `CLIENT_TYPE_NORMAL` 与 `CLIENT_TYPE_PUBSUB` 允许被驱逐**——主库连接与副本连接都不允许。

**内存桶。** `clientMemUsageBucket`（`src/server.h`）就是一个 `{list *clients; size_t mem_usage_sum;}` 的结构。分桶函数 `getMemUsageBucket(mem)` 做的事是一次 log2：

```c
int size_in_bits = 8*(int)sizeof(mem);
int clz = mem > 0 ? __builtin_clzl(mem) : size_in_bits;
int bucket_idx = size_in_bits - clz;
if (bucket_idx > CLIENT_MEM_USAGE_BUCKET_MAX_LOG) bucket_idx = CLIENT_MEM_USAGE_BUCKET_MAX_LOG;
else if (bucket_idx < CLIENT_MEM_USAGE_BUCKET_MIN_LOG) bucket_idx = CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
bucket_idx -= CLIENT_MEM_USAGE_BUCKET_MIN_LOG;
return &server.client_mem_usage_buckets[bucket_idx];
```

注释解释了这个设计的意图：「如果一个客户端的内存占用翻倍，它就移到下一个桶；如果减半，就往下移一个桶。」桶的下界是 `CLIENT_MEM_USAGE_BUCKET_MIN_LOG`（`src/server.h` 定义为 15，对应 32KB），上界是 `CLIENT_MEM_USAGE_BUCKET_MAX_LOG`（33，对应 4GB），桶的总数 `CLIENT_MEM_USAGE_BUCKETS` 由这两个常量算出（`1+33-15`）。

桶数组由 `initServerClientMemUsageBuckets`（`src/server.c`）分配，在 `initServer` 里当 `server.maxmemory_clients != 0` 时调用；`freeServerClientMemUsageBuckets` 负责释放。

**入桶与出桶。** `updateClientMemUsageAndBucketInternal` 的第一件事就是用准入闸门的结果决定去留：

```c
int allow_eviction = clientEvictionAllowed(c);
removeClientFromMemUsageBucket(c, allow_eviction);
if (!allow_eviction) {
    updateClientWatchPubsubMemoryUsage(c);
    return 0;
}
```

`removeClientFromMemUsageBucket(c, allow_eviction)` 的注释写得很直白：**如果这个客户端不能被驱逐，就把它从内存桶里移除**。也就是说——不可驱逐的客户端既不会被踢，也不会占用「可驱逐内存」的统计口径。

函数头的注释还点出一个已知偏差：它会过滤掉 no-evict、主库、副本三类客户端（无论驱逐是否开启），所以这几类客户端在 `INFO` 里的内存数值可能过时。

**限额 `getClientEvictionLimit`（`src/networking.c`）。**

```c
size_t maxmemory_clients_actual = SIZE_MAX;
if (server.maxmemory_clients < 0 && server.maxmemory > 0) {
    maxmemory_clients_bytes = (double)server.maxmemory * -(double)server.maxmemory_clients / 100;
    ...
} else if (server.maxmemory_clients > 0)
    maxmemory_clients_actual = server.maxmemory_clients;
else
    return 0;
if (maxmemory_clients_actual < 1024*128)
    maxmemory_clients_actual = 1024*128;
```

三种模式：`maxmemory-clients` 为负数时按 `maxmemory` 的百分比换算；为正数时直接作为字节数；为 0 时返回 0（不驱逐）。下限被钳到 128KB，注释解释原因：「不允许 `maxmemory-clients` 太小，以免出现我们完全无法与服务通信的糟糕配置」。

**驱逐循环 `evictClients`（`src/networking.c`）。** 它有两个真实调用点：

- `src/server.c` 的 `beforeSleep`——注释是「Disconnect some clients if they are consuming too much memory.」（见 a02）。
- `src/server.c` 的 `processCommand`——注释写明「We do this before key eviction, after the last command was executed and consumed some client output buffer memory.」，即在**键级淘汰之前**、在最后一条命令执行完并消耗掉一些客户端输出缓冲之后做客户端驱逐。紧接着的代码处理了一个自我驱逐的边界：`if (server.current_client == NULL) return C_ERR;`，注释是「If we evicted ourself then abort processing the command」——如果被踢掉的正好是当前正在服务自己的客户端，就中止这条命令的处理。

它的骨架是：

```c
if (!server.client_mem_usage_buckets) return;
int curr_bucket = CLIENT_MEM_USAGE_BUCKETS-1;      /* 从最大的桶开始 */
listRewind(server.client_mem_usage_buckets[curr_bucket].clients, &bucket_iter);
size_t client_eviction_limit = getClientEvictionLimit();
if (client_eviction_limit == 0) return;
while (subtractClientsWatchPubsubOvercount(
           server.stat_clients_type_memory[CLIENT_TYPE_NORMAL] +
           server.stat_clients_type_memory[CLIENT_TYPE_PUBSUB]) >= client_eviction_limit) {
    ...
}
```

循环条件读的是**两类可驱逐客户端的内存统计之和**，再减去 `subtractClientsWatchPubsubOvercount` 扣掉的部分——那个字段的注释是「被多于一个客户端持有的 WATCH 键与 Pub/Sub 名字带来的重复计费」。

桶内的处理：

```c
client *c = ln->value;
size_t last_memory = c->last_memory_usage;
int tid = c->running_tid;
if (tid != IOTHREAD_MAIN_THREAD_ID) {
    pauseIOThread(tid);
    updateClientMemUsageAndBucket(c);
}
if (c->last_memory_usage >= last_memory ||
    c->mem_usage_bucket == &server.client_mem_usage_buckets[curr_bucket])
{
    sds ci = catClientInfoString(sdsempty(),c);
    serverLog(LL_NOTICE, "Evicting client: %s", ci);
    freeClient(c);
    sdsfree(ci);
    server.stat_evictedclients++;
}
```

两处细节都有注释支撑。**其一**：客户端在 I/O 线程上时要先 `pauseIOThread` 并**重新**计算内存与桶。注释解释：内存用量与桶只在主线程里更新（处理命令时、`clientsCron` 里），可能已经滞后；不重算就可能「错误地驱逐」那些内存其实已经降下来的客户端。

**其二**：真正的驱逐条件是「内存没有下降 **或** 还在当前这个桶里」。重算之后如果内存确实降了、并且已经挪到了更小的桶，就跳过这个客户端。

踢掉之后若是 I/O 线程上的客户端，要 `resumeIOThread(tid)` 并**重置迭代器**——注释说明：更新内存用量与释放客户端都可能改变桶链表的 `next` 指针。

桶遍历完则 `curr_bucket--` 降一档并 `listRewind` 重新开始；降到底部仍超限时，记一条 `LL_WARNING` 日志「Over client maxmemory after evicting all evictable clients」并 `break`。

**配置切换 `applyClientMaxMemoryUsage`（`src/config.c`）。** `CONFIG SET maxmemory-clients` 会触发它。逻辑是：从 0 改成非 0 时先 `initServerClientMemUsageBuckets()` 建桶，然后遍历全部客户端逐个 `updateClientMemUsageAndBucket(c)`；从非 0 改成 0 时则对每个客户端 `removeClientFromMemUsageBucket(c, 0)`（第二个参数 0 表示「移除出桶」），最后 `freeServerClientMemUsageBuckets()` 释放桶数组。全过程包在 `pauseAllIOThreads()` / `resumeAllIOThreads()` 之间。

**退化路径。** 所有可驱逐客户端都踢完、内存仍超上限时，Redis **只记一条 WARNING 日志**，不做任何其它处置——主库连接与副本连接不会被踢（准入闸门挡住了），未认证或异常状态的客户端也不会被踢。这是有意的：宁可超限也不破坏复制链路。

## 关键取舍

**源码事实：** 准入闸门只从三个维度否决——功能关闭、`CLIENT_NO_EVICT`、无连接；类型上只放行 `CLIENT_TYPE_NORMAL` 与 `CLIENT_TYPE_PUBSUB`。**源码事实：** 主库连接与副本连接永不被驱逐。**源码事实：** 分桶按 log2，桶范围从 32KB 到 4GB。**源码事实：** 限额有 128KB 的硬下限。**源码事实：** 驱逐前会对 I/O 线程上的客户端重新计算内存占用。**源码事实：** 可驱逐客户端全部踢完仍超限时只记 WARNING 日志。**源码事实：** 客户端级驱逐与键级 `maxmemory` 淘汰是两条互不替代的独立路径。

**架构推论：** 用 log2 分桶把「找出最占内存的客户端」从 O(N) 扫描变成 O(桶数 + 被踢数)，让驱逐可以在每轮 `beforeSleep` 里安全地做——代价是同一桶内的客户端之间没有精确的占用顺序，驱逐顺序在桶内是链表顺序。**架构推论：** 只踢普通与 pubsub 客户端，是因为这两类连接的内存是「为自己服务的中间产物」（输出缓冲、订阅名、WATCH 键），而主库与副本连接承载的是复制语义——踢掉它们换来的内存远小于拓扑震荡的代价。**架构推论：** 不可驱逐的客户端干脆不入桶，解决了「统计口径」与「驱逐集合」两个集合不一致的常见矛盾：Redis 让两者严格相等，代价是这些客户端在 `INFO` 里的内存数值可能过时（源码注释主动承认了这点）。**架构推论：** 在 I/O 线程上暂停再重算，是用一次短暂停顿换准确性——若不重算，Redis 会踢掉那些「内存其实已经降下来」的客户端，这在流式大结果场景下会误伤正常用户。**架构推论：** 与键级淘汰分开，意味着两个上限可以独立触发、互不替代——客户端内存吃紧不会去淘汰键，键空间吃紧也不会去踢客户端；主核若同时有会话缓冲与页缓存两类内存，更需要显式决定它们的优先级关系。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `clientEvictionAllowed` | 驱逐准入闸门：功能开关、`CLIENT_NO_EVICT`、无连接三者否决；类型上只放行普通客户端与 pubsub 客户端 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c) —— 内存桶的分配、客户端入桶出桶与准入判定。
- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— `evictClients` 驱逐循环与限额换算。
- [Redis 文档：内存淘汰](https://redis.io/docs/latest/develop/reference/eviction/) —— `maxmemory-clients` 与客户端级驱逐的官方说明。
- [Redis 命令：CLIENT NO-EVICT](https://redis.io/docs/latest/commands/client-no-evict/) —— 客户端自我保护的标志。

## 与主核 / AI 的关系

Redis 的客户端级驱逐解决的是「连接占的资源该不该算进服务端内存上限」这个问题，主核若暴露网络接口就必须回答同一个问题；但 Redis 的驱逐单位是**连接**，主核的会话通常绑定在连接上却承载着跨请求的状态（事务上下文、订阅、暂存结果集），按连接踢人可能连带丢掉会话状态。Redis 的「不可驱逐客户端不入统计桶」这条规则在主核里需要小心照搬——它意味着这类会话的内存对运维是不可见的，主核的多写者引擎更应该保证统计与实际占用一致，否则限流决策会基于错误的分母。对 AI 场景，驱逐一个正在做推理的连接几乎必然浪费全部已完成的算力，主核更合理的做法是取消该推理的物化并保留连接，或把 AI 会话排除在驱逐集合之外——这正是 Redis 用 `CLIENT NO_EVICT` 提供给应用的那条路。
