---
project: redis
feature: socket-writeback
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [writeToClient, sendReplyToClient]
---

# 回复写回 socket

## 一句话职责

把客户端输出缓冲里的字节真正送进 socket：处理部分写、按需注册或注销可写事件、以及写失败后把客户端断开。

## 机制是什么

Redis 有**两条**写回路径：

1. **事件驱动**：`sendReplyToClient` 是注册在 socket 上的可写事件回调，由 ae 事件循环在 fd 可写时触发。
2. **主动冲刷**：`beforeSleep` 里的 `handleClientsWithPendingWrites` 遍历「待写队列」，直接调 `writeToClient`，能一次写完就不必再等一轮事件循环。

两条路径最终都汇到同一个函数 `writeToClient(client *c, int handler_installed)`。`handler_installed` 参数的含义是「可写事件处理器当前是否已装在连接上」：事件回调里传 1，其它地方传 0。这个区分不是可有可无的——源码注释写明 `aeDeleteFileEvent()` **不是线程安全的**，而 `writeToClient` 会在 I/O 线程里被调用，所以线程里必须传 0。

本页只讲写回本身。回复怎么编码进缓冲是 b02，缓冲越界了怎么判是 b03。

## 怎么实现的

**`sendReplyToClient`。** 只有两行：从 `connGetPrivateData(conn)` 取出客户端指针，调 `writeToClient(c, 1)`。这个私有数据指针正是在 `createClient` 里用 `connSetPrivateData(conn, c)` 挂上的。

**`writeToClient` 的骨架。**

```c
int writeToClient(client *c, int handler_installed) {
    if (!(c->io_flags & CLIENT_IO_WRITE_ENABLED)) return C_OK;
    atomicIncr(IOThreads[c->running_tid].io_writes_processed, 1);
    ...
```

`CLIENT_IO_WRITE_ENABLED` 是 I/O 线程层面的写开关，关掉就直接返回。随后给本线程的 `io_writes_processed` 计数加一。

**两条分支。** `clientTypeIsSlave(c)` 为真走副本分支：

```c
while(_clientHasPendingRepliesSlave(c)) {
    int ret = _writeToClientSlave(c, &nwritten);
    if (ret == C_ERR) break;
    totwritten += nwritten;
}
atomicIncr(server.stat_net_repl_output_bytes, totwritten);
```

源码注释给出理由：**副本客户端要尽量多发**，否则在高速流量下复制缓冲会无限增长。所以副本不受下面的单次写出上限约束。

非副本分支：

```c
const int is_normal_client = !(c->flags & CLIENT_SLAVE);
while (_clientHasPendingRepliesNonSlave(c)) {
    int ret = _writeToClientNonSlave(c, &nwritten);
    if (ret == C_ERR) break;
    totwritten += nwritten;
    if (totwritten > NET_MAX_WRITES_PER_EVENT &&
        (server.maxmemory == 0 ||
        zmalloc_used_memory() < server.maxmemory) &&
        is_normal_client) break;
}
atomicIncr(server.stat_net_output_bytes, totwritten);
```

这一段有三重与条件，每一条都有出处：

- `NET_MAX_WRITES_PER_EVENT`（`src/server.h` 定义为 `1024*64`，即 64KB）。注释解释：单线程服务端一次事件里最多写这么多是个好主意，这样即使来一个「极快链路上不断发来的巨大请求」（注释举例是对回环接口执行 `KEYS *`），也能匀出时间去服务别的客户端。
- **超 maxmemory 时忽略这个上限**，注释写得很直接：「然而如果我们超过了 maxmemory 限制，我们就不理会这一点，尽力把能送的数据都送出去」。
- `is_normal_client` 的判定是 `!(c->flags & CLIENT_SLAVE)`。注释点明：能走到这个分支但仍带 `CLIENT_SLAVE` 标志的，是 MONITOR 或槽位迁移客户端——它们被打上了副本标志，但对外按普通客户端暴露。

**部分写怎么记。** `c->sentlen` 是已写出的字节数，`c->bufpos` 是静态缓冲里的有效字节数。`_writeToClientNonSlave` 的分支：

- `c->reply` 链表非空 → 走 `_writevToClient`。注释：回复列表非空时用 writev 能省下系统调用和 TCP 包。
- 否则 `c->bufpos > 0`：若 `c->buf_encoded`（编码缓冲）也走 writev，因为里面装的是 bulk string 引用，需要拆成多段；否则直接 `connWrite(c->conn, c->buf + c->sentlen, c->bufpos - c->sentlen)`，写完累加 `c->sentlen`；整个缓冲写完就把 `c->bufpos` 与 `c->sentlen` 双双清零。

**`_writevToClient` 的组装。** `iovmax = min(IOV_MAX, c->conn->iovcnt)`，`struct iovec iov[iovmax]` 放在栈上。先把静态缓冲挂上去（普通缓冲直接挂；编码缓冲要走 `processEncodedBufferForWrite` 逐段拆开），再遍历回复链表把每个非空 `clientReplyBlock` 挂上去。停止条件由 `replyIOVReachLimit` 判定：iovec 槽位用满，或累计字节数达到 `NET_MAX_WRITES_PER_EVENT`。写调用是 `connWritev(c->conn, iov, reply_iov.iovcnt)`；返回字节数后按剩余量定位「还有数据的那一个节点」，把它前面的节点都释放掉。

**副本的写回。** `_writeToClientSlave` 在主线程上从 `server.repl_buffer_blocks` 里按 `c->ref_repl_buf_node` / `c->ref_block_pos` 逐块发：当前块还有剩余就 `connWrite`，发完一块则该块 `refcount--`、下一块 `refcount++`、指针前移，并调 `incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL)` 顺带裁剪复制积压。在 I/O 线程上则走 `_writeToClientSlaveInIOThread`：它受 `c->io_bound_repl_node` / `c->io_bound_block_pos` 这条边界约束（注释：IO 线程不能发出超过边界的数据），并且在「副本已追平当前可用数据」时会强制 flush 压缩器里缓存的数据——注释解释：持续负载下副本很难在两次写之间追平，压缩器会把数据攒在当前帧里，不强制 flush 就要等 `repl-compression-max-latency` 到期才能送达。

**写失败的处置。** `if (nwritten == -1)` 且 `connGetState(c->conn) != CONN_STATE_CONNECTED`：

```c
serverLog(LL_VERBOSE, "Error writing to client: %s", connGetLastError(c->conn));
if (c->running_tid == IOTHREAD_MAIN_THREAD_ID &&
    c == server.master &&
    (server.repl_main_ch_state & REPL_MAIN_CH_STREAMING_BUF))
{
    connSetWriteHandler(c->conn, NULL);
    connSetReadHandler(c->conn, NULL);
    server.repl_main_ch_state |= REPL_MAIN_CH_CLOSE_ASAP;
} else {
    freeClientAsync(c);
}
return C_ERR;
```

注释解释了这个特判：全量同步期间副本正在应用已缓冲的命令时，ACK 写**可能**失败；现在就关掉它会丢掉已收到的命令，副本只能从更老的 offset 重新 PSYNC——如果那段历史已经从主库的 backlog 里消失，就得再来一次全量同步。因此这里只是摘掉读写回调并置 `REPL_MAIN_CH_CLOSE_ASAP`，注释说等缓冲应用结束后 `rdbChannelStreamReplDataToDb()` 会调 `freeClient` 关连接、缓存已应用的 offset 并重连发起 PSYNC。

**写成功后的收尾。** `totwritten > 0` 时更新 `c->lastinteraction = server.unixtime`，但 `CLIENT_MASTER` 除外——注释解释：主库连接总是发 REPLCONF ACK 命令，那只是填 socket 输出缓冲、耗点时间而已，不算一次交互；超时判定只依赖收到的数据或 ping。

`!clientHasPendingReplies(c)`（缓冲已空）时做四件事：

1. `c->sentlen = 0`。
2. 若 `handler_installed` 则 `connSetWriteHandler(c->conn, NULL)` 注销可写事件。注释再次强调：`writeToClient` 会被线程调用，而 `aeDeleteFileEvent()` 不线程安全，所以线程里调用时 `handler_installed` 必须是 0。
3. 若带 `CLIENT_CLOSE_AFTER_REPLY` 标志则 `freeClientAsync(c)` 并返回 `C_ERR`——把整条回复写完后才关连接。
4. 在主线程上 `tryUnlinkClientFromPendingRefReply(c, 1)`；副本客户端若在 I/O 线程上且没有待读的 ACK，则 `enqueuePendingClientsToMainThread(c, 0)` 交回主线程（注释：这样它能尽快拿到新的复制数据；但若还有待读 ACK 就仍留在 I/O 线程）。

最后若在主线程则 `updateClientMemUsageAndBucket(c)` 更新内存占用（注释：因为这不是线程安全的，所以有条件地做）。

**主动冲刷路径 `handleClientsWithPendingWrites`。** 这个函数在 `beforeSleep` 里被调用（见 a02）。它的注释说明了存在的理由：希望「在重新进入事件循环之前就能直接把回复写进客户端的 socket，省掉一次装可写事件的系统调用」。循环体：

- 跳过在 I/O 线程上的副本客户端（注释：由 `putReplicasInPendingClientsToIOThreads` 处理）。
- 清掉 `CLIENT_PENDING_WRITE` 标志并从 `server.clients_pending_write` 摘除。
- 跳过带 `CLIENT_PROTECTED` 的客户端（注释：不要做任何可能触发写错误或重建 handler 的事）。
- 跳过带 `CLIENT_CLOSE_ASAP` 的客户端（本来就要关）。
- 有 I/O 线程且客户端在主线程上、且不是 `CLIENT_CLOSE_AFTER_REPLY`、且 `isClientMustHandledByMainThread(c)` 为假时，`assignClientToIOThread(c)` 交给 I/O 线程。
- 否则 `writeToClient(c, 0)`。写完若仍有数据没发完，`installClientWriteHandler(c)` 装上可写事件，等下一轮事件循环继续。

**`putClientInPendingWriteQueue` 与 `installClientWriteHandler`。** 前者只打标志并把客户端挂到 `server.clients_pending_write` 链表头，**不**装可写事件——它的注释说得很清楚：这样在重新进入事件循环之前可以试着直接写 socket，只有在一次写不完时才真正装 handler。它还对副本做了状态检查（`replstate` 必须是 `REPL_STATE_NONE`、`SLAVE_STATE_SEND_BULK_AND_STREAM`，或 `SLAVE_STATE_ONLINE` 且没有 `repl_start_cmd_stream_on_ack`）。

后者用 `connSetWriteHandlerWithBarrier(c->conn, sendReplyToClient, ae_barrier)`。注释解释了 barrier 的用途：当 `server.aof_state == AOF_ON && server.aof_fsync == AOF_FSYNC_ALWAYS` 时置 barrier=1，**保证同一个 fd 不会在同一轮事件循环里既被读又被写**——这样就能在处理查询的中间、在把回复交给客户端之前，先调 `beforeSleep` 做真正的 AOF fsync。

## 关键取舍

**源码事实：** 每次事件最多写 64KB（`NET_MAX_WRITES_PER_EVENT`），但已超 maxmemory 时忽略该上限；副本客户端完全不受该上限约束。**源码事实：** `handler_installed` 参数区分事件回调与线程调用，因为 `aeDeleteFileEvent` 非线程安全。**源码事实：** 写失败一律异步关闭，但全量同步缓冲期的主库连接走特判：摘掉读写回调 + 置 `REPL_MAIN_CH_CLOSE_ASAP`，延后到缓冲应用结束再关。**源码事实：** 主库连接的成功写不更新 `lastinteraction`。**源码事实：** 有两条写回路径——事件回调与 `beforeSleep` 里的主动冲刷。**源码事实：** write barrier 只在 `appendfsync always` 时启用。

**架构推论：** 「待写队列 + 睡前直写」把写回从纯事件驱动改成「事件驱动 + 主动冲刷」的混合模型，在回复量小的常见场景下省掉一次系统调用与一轮事件循环延迟；代价是 `beforeSleep` 的耗时会直接计入命令延迟。**架构推论：** 写失败就断开而不重试，是因为在 TCP 层错误语义不可靠的前提下，重试可能把数据写进一个已失效的连接；断连让客户端用自己的重连语义处理更可靠。**架构推论：** 副本走独立的全局 `repl_buffer_blocks` 而不是客户端私有输出缓冲，天然绕开了输出缓冲上限（b03 的判定里副本是特判分支）；代价是副本的发送进度由共享缓冲的引用计数决定，比私有缓冲多一层 bookkeeping。**架构推论：** 单次写出上限是为了公平——单线程下不能让一个高速客户端吃掉整轮事件循环；但这个上限在内存吃紧时又被主动放弃，说明「公平」与「不 OOM」两个目标冲突时，Redis 选了后者。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/networking.c` | `writeToClient` | 写回主循环：副本与非副本两条分支、部分写记账、单次写出上限、写失败处置、缓冲清空后的收尾 |
| `src/networking.c` | `sendReplyToClient` | 可写事件回调：取出连接上的客户端并转调 `writeToClient`，此时可写事件已安装 |

## 参考

- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— 写回、部分写记账、writev 组装与失败处置。
- [Redis 源码：server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— `NET_MAX_WRITES_PER_EVENT` 等网络相关常量与 `client` 结构定义。
- [Redis 文档：诊断延迟问题](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/latency/) —— 写回节流与事件循环延迟的关系。
- [Redis 文档：协议规范](https://redis.io/docs/latest/develop/reference/protocol-spec/) —— 客户端如何消费返回的字节流。

## 与主核 / AI 的关系

「每次事件写出量有上限、且内存吃紧时可以放弃」这组规则，暴露的是 Redis 单线程事件循环下的公平性假设：写回耗时必须可预测，否则一个慢客户端就能拖垮所有客户端。主核是多写者引擎，写回发生在多个线程上，这条公平性约束不再需要同样的形式——但**每连接写出量上限**仍然值得保留，因为它直接对应单连接的内存占用上限。对 AI 流式输出尤其关键：一次推理可能产生数 MB 的 token 流，若照搬 Redis 的 64KB 上限而不做流式背压，缓冲区会迅速膨胀；主核更该做的是按消费者速度施加背压，而不是靠断开连接来止住内存增长。Redis 写失败即断开的策略也不适用于 AI 场景——推理结果不可重放，断连等于让整次推理白做。
