---
project: redis
feature: output-buffer-limits
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [checkClientOutputBufferLimits, closeClientOnOutputBufferLimitReached]
---

# 输出缓冲上限与超限处置

## 一句话职责

按客户端类型（普通 / 副本 / pubsub）判定输出缓冲是否越界，越界了就把这个客户端断开——Redis 不做限流，只做断连。

## 机制是什么

一个不读回复的客户端可以无限期地把服务端内存堆在自己这边。Redis 对此的防线是 `client-obuf-limits` 配置项：它给每类客户端定一组三元组——**硬上限字节数**、**软上限字节数**、**软上限持续秒数**。

- 硬上限：越过立刻生效。
- 软上限：必须**连续**越界满指定秒数才算数；中间只要落回线下就重新计时。

判定函数是 `checkClientOutputBufferLimits`（返回非零表示已越界），处置函数是 `closeClientOnOutputBufferLimitReached`（真正把客户端关掉）。

本页只讲这条判定与处置链。回复怎么编码进来是 b02，字节怎么写出去是 b04。

## 怎么实现的

**怎么量。** `checkClientOutputBufferLimits` 的第一行是 `unsigned long used_mem = getClientOutputBufferLogicalSize(c);`。

`getClientOutputBufferLogicalSize` 等于 `getClientOutputBufferAllocSize(c)` 加上 `c->reply_bytes_shared`（副本除外）。它的注释说明了为什么要加上共享部分：这包含了与键空间共享的内存，确保一个通过 copy-avoidance 索取海量数据的客户端**同样**受输出缓冲上限约束。

`getClientOutputBufferAllocSize` 分两支：

- 副本客户端：不算自己的 `c->reply_bytes`（副本用的是全局共享的复制缓冲），而是按 `c->ref_repl_buf_node` 往后推算出还挂着的复制缓冲块的总字节数加上链表节点开销。它的注释强调这个函数非常快，可以被反复调用，主要用途就是执行输出缓冲上限。
- 普通客户端：`c->reply_bytes` 加上链表节点开销（`sizeof(listNode) + sizeof(clientReplyBlock)`）。

**未认证客户端的额外门槛。** `if (used_mem > 1024 && authRequired(c)) return 1;`。源码注释写明理由：对未认证客户端限制输出缓冲，是为了防止它们不读回复来滥用它。这里 1024 是硬编码的字面量，不是配置项。

**分类。** `class = getClientType(c)`，取值为 `CLIENT_TYPE_NORMAL`（普通请求-应答客户端与 MONITOR）、`CLIENT_TYPE_SLAVE`（副本）、`CLIENT_TYPE_PUBSUB`（订阅者）、`CLIENT_TYPE_MASTER`（主库连接）。接下来 `if (class == CLIENT_TYPE_MASTER) class = CLIENT_TYPE_NORMAL;`，注释说明理由：**对输出缓冲限制而言，主库按普通客户端处理**。

**副本的下限修正。** 源码注释在这里给出了一条重要约束：副本客户端的软限与硬限都**不应该**设得比 `repl-backlog-size` 还低，否则副本可能在一次成功的部分同步之后仍被断开。因此：

```c
if (clientTypeIsSlave(c) && hard_limit_bytes &&
    hard_limit_bytes < (unsigned long long)server.repl_backlog_size)
    hard_limit_bytes = server.repl_backlog_size;
```

软限做同样的修正。注释还补了一句：这个调整没有额外内存成本，因为副本客户端本来就共享 backlog 缓冲；而 ASM（槽位迁移）客户端虽然用副本的限制，但有自己的输出缓冲，所以不参与这个调整。

**硬限与软限的比较。** 两者的字节数都取自 `server.client_obuf_limits[class]`；为 0 表示这一档不限制：

```c
if (server.client_obuf_limits[class].hard_limit_bytes && used_mem >= hard_limit_bytes) hard = 1;
if (server.client_obuf_limits[class].soft_limit_bytes && used_mem >= soft_limit_bytes) soft = 1;
```

注意比较用的是**修正后的** `hard_limit_bytes`/`soft_limit_bytes` 局部变量，而开关判断用的是未修正的原值——语义一致（修正只会把 0 之外的值抬高，不会把非 0 变成 0）。

**软限的持续判定。** 客户端上有一个 `c->obuf_soft_limit_reached_time` 字段记录「第一次看到软限被越过」的时刻：

```c
if (soft) {
    if (c->obuf_soft_limit_reached_time == 0) {
        c->obuf_soft_limit_reached_time = server.unixtime;
        soft = 0;   /* 第一次看到软限被越过，不算 */
    } else {
        time_t elapsed = server.unixtime - c->obuf_soft_limit_reached_time;
        if (elapsed <= server.client_obuf_limits[class].soft_limit_seconds)
            soft = 0;   /* 还没持续够久，不算 */
    }
} else {
    c->obuf_soft_limit_reached_time = 0;   /* 落回线下，计时清零 */
}
```

返回值是 `soft || hard`。

**处置函数 `closeClientOnOutputBufferLimitReached(c, async)`。** 前置检查有三条：

- `if (!c->conn) return 0;`——注释说「释放假客户端是不安全的」。
- `serverAssert(c->reply_bytes < SIZE_MAX-(1024*64));`，注释解释：实际内存不会溢出，但**逻辑**内存可能超过 `SIZE_MAX`。
- 若 `c->reply_bytes == 0 && c->reply_bytes_shared == 0 && !clientTypeIsSlave(c)`，或者客户端已经带 `CLIENT_CLOSE_ASAP` 标志，则返回 0。副本之所以例外，注释写明是因为 `c->reply_bytes` 对副本不适用（它们用全局复制缓冲）。

确认越界后，用 `catClientInfoString(sdsempty(), c)` 生成客户端信息，然后按 `async` 参数二选一：

- `async` 为 1：`freeClientAsync(c)`，日志是 `Client %s scheduled to be closed ASAP for overcoming of output buffer limits.`
- `async` 为 0：`freeClient(c)`，日志是 `Client %s closed for overcoming of output buffer limits.`

两者都记 `LL_WARNING` 级别。最后 `server.stat_client_outbuf_limit_disconnections++`，返回 1 表示「客户端已被（标记为）关闭」。

函数的注释解释了**为什么默认必须异步**：「我们不得不异步关闭客户端，因为这个函数是从那些不能安全释放客户端的上下文里调用的，也就是那些往客户端输出缓冲里推数据的底层函数。」`async` 为 0 的场景注释写明是「从 cron 里调用时有用」。

**配置来源。** `clientBufferLimitsDefaults`（`src/config.c`）给出三档默认值：

- normal：`{0, 0, 0}`——硬限、软限、秒数全为 0，即**默认不限制普通客户端的输出缓冲**。
- slave：`{1024*1024*256, 1024*1024*64, 60}`——硬限 256MB，软限 64MB，软限持续 60 秒。
- pubsub：`{1024*1024*32, 1024*1024*8, 60}`——硬限 32MB，软限 8MB，软限持续 60 秒。

`updateClientOutputBufferLimit` 负责解析配置字符串：格式是 `<class> <hard> <soft> <soft_seconds>` 四元组一组，参数个数必须是 4 的倍数，否则报「Wrong number of arguments in buffer limit configuration.」；class 用 `getClientTypeByName` 解析，`master` 这一类被明确拒绝（「Invalid client class specified in buffer limit configuration.」）；数值用 `memtoull` 解析并检查错误，软限秒数用 `strtoll` 解析且不允许为负。

**真实调用点。** 超限检查分布在三处：

- `src/networking.c` 的 `_addReplyPayloadToList`：新建回复链表块时调用，`async = 1`。
- `src/networking.c` 的 `_addBulkStrRefToBuffer`：copy-avoidance 把字符串引用写进缓冲时调用，`async = 1`。注释特别说明：即便 bulk string 是按引用存储、底层内存是共享的，这部分共享内存**仍然**计入该客户端的输出缓冲占用，所以照样要检查上限。
- `src/networking.c` 的 `setDeferredReply`：新建延迟回复节点时调用，`async = 1`。
- `src/networking.c` 的 `AddReplyFromClient`：模块把一个客户端的回复搬到另一个时调用，`async = 1`。
- `src/replication.c` 的 `replicationFeedSlaves`：`if (wr->new_blocks) closeClientOnOutputBufferLimitReached(slave, 1);`。
- `src/server.c` 的 `clientsCronRunClient`：`closeClientOnOutputBufferLimitReached(c, 0)`——这是唯一的同步关闭点，走 cron 路径。

**退化路径。** 超限的后果只有一种：**断开连接**。没有降级、没有丢弃部分回复、没有把回复转存到别处。客户端必须自己重连，重连后从头开始。异步关闭的客户端带 `CLIENT_CLOSE_ASAP` 标志，真正释放要等到 `beforeSleep` 里的 `freeClientsInAsyncFreeQueue`（见 b01）。

## 关键取舍

**源码事实：** 软限需要**连续**越界满 `soft_limit_seconds` 才生效，落回线下即清零计时。**源码事实：** 未认证客户端有 1024 字节的硬编码门槛。**源码事实：** 副本的软限与硬限会被抬到不低于 `repl-backlog-size`。**源码事实：** 主库连接按普通客户端的档位判定。**源码事实：** 普通客户端默认**没有**输出缓冲上限（默认三元组全为 0）。**源码事实：** 越界的唯一后果是断开连接；同步关闭只发生在 cron 一处，其余都走异步标记。

**架构推论：** 按客户端类型分档而不是全局统一，根因是三类客户端的输出行为根本不同——普通客户端的输出由客户端自己的读取速度决定，副本与 pubsub 的输出是服务端主动推送、客户端往往不读回复，后者才是真正的内存风险源。**架构推论：** 副本上限被抬到 `repl-backlog-size`，是因为复制链路的语义优先级高于内存上限——宁可多占一点内存，也不能在刚完成部分同步后把副本踢掉。**架构推论：** 越界即断连而不是丢弃后续回复或限流，是因为「丢弃回复」会让客户端把后续字节错当成它期望的数据，协议直接错位，比断连更难恢复。**架构推论：** 把检查放在写入侧（新建缓冲块时）而不是定时轮询，保证了超限内存不会在两次轮询之间无限增长；代价是单个大块在被发现前已经占用了内存。**架构推论：** 未认证客户端的 1KB 门槛把「认证前」的内存占用压到常数级，是用一条硬编码常量换掉一整条配额配置。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/networking.c` | `checkClientOutputBufferLimits` | 按客户端类型判定硬限/软限是否越界，软限需持续满指定秒数 |
| `src/networking.c` | `closeClientOnOutputBufferLimitReached` | 越界后按 `async` 参数异步或同步关闭客户端，并累加断连统计 |

## 参考

- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— 输出缓冲上限判定与断连处置的实现。
- [Redis 源码：config.c](https://github.com/redis/redis/blob/unstable/src/config.c) —— `client-obuf-limits` 的默认值与解析逻辑。
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/) —— 客户端输出缓冲占用与 `client-output-buffer-limit` 的说明。
- [Redis 文档：客户端](https://redis.io/docs/latest/develop/reference/clients/) —— 客户端分类的官方口径。

## 与主核 / AI 的关系

Redis 的输出缓冲上限解决的是「客户端不读回复」的经典问题，主核若要暴露网络接口就必须有对应机制，但主核是**多写者**引擎：Redis 的上限是**单个客户端**的私有缓冲，判定完全无锁；主核的查询结果缓冲往往是共享的（比如同一个共享 arena 或同一个物化结果集），按连接计量就会出现「一个连接读得慢、其他人陪它一起被限」的耦合问题，主核需要额外的记账维度。另外 Redis 的越界后果是断连，对 AI 流式输出尤其粗暴——主核更适合的做法是「取消该请求的物化、保留连接」。软限「连续 N 秒」的时间语义也依赖单一时间源，主核在并发下需要重新定义「持续」的含义。
