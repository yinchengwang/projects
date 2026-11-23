---
project: redis
feature: client-lifecycle
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [createClient, freeClient, resetClient]
---

# 客户端对象生命周期

## 一句话职责

`createClient` 造出一个客户端对象，`resetClient` 把它复位到可以处理下一条命令的状态，`freeClient` 把它连同持有的所有资源一起释放掉——三段合起来就是「一个客户端在 Redis 里的全部一生」。

## 机制是什么

Redis 的 `client` 结构体（定义在 `src/server.h`）是全仓最庞大的结构之一：它同时持有连接句柄、输入缓冲、输出缓冲、参数向量、阻塞状态、WATCH 键集合、订阅通道字典、复制状态、内存桶指针等等。与之配套的三段式生命周期是：

- **创建**：`createClient` 分配结构体并逐字段初始化，之后（仅当有真实连接时）把它挂进全局客户端链表。
- **复位**：`resetClient` 在每条命令执行完之后被调用，只清掉「与这条命令有关的状态」，连接、输入缓冲、输出缓冲一概不动。
- **释放**：`freeClient` 拆掉一切：释放缓冲、摘掉全局索引、关 socket、最后 `zfree` 掉结构体本身。

与之并行的还有一条**异步释放**路径 `freeClientAsync`：需要终止客户端但当前调用栈里还得继续用这个指针时，先打标记（`CLIENT_CLOSE_ASAP`），真正释放推迟到 `beforeSleep` 里的 `freeClientsInAsyncFreeQueue`。

本页只讲客户端对象的创建、复位与释放。协议解析在 a06，输出缓冲上限的判定在 b03，阻塞状态机在 i01。

## 怎么实现的

**`createClient` 的初始化顺序。** 第一行是 `zmalloc(sizeof(client))`，随后是一长串逐字段赋值——没有用 `memset` 清零，而是每个字段显式写值。

有连接时（`conn` 非 NULL）先做四件事：`connEnableTcpNoDelay(conn)` 关闭 Nagle、`connKeepAlive(conn, server.tcpkeepalive)` 按配置开 TCP keepalive、`connSetReadHandler(conn, readQueryFromClient)` 挂上读回调、`connSetPrivateData(conn, c)` 把 client 指针存进连接对象（`sendReplyToClient` 之后就是靠这个把连接还原成客户端的）。

`conn` 传 NULL 表示造一个「假客户端」。源码注释写明理由：所有命令都需要在客户端上下文里执行，当命令在别的上下文里跑时（比如 Lua 脚本）就需要一个没有连接的客户端。

接下来的字段初始化分几类：

- **标识与归属**：`atomicGetIncr(server.next_client_id, client_id, 1)` 分配全局唯一 id；`c->tid = c->running_tid = IOTHREAD_MAIN_THREAD_ID` 表示初始归属主线程；`selectDb(c, 0)` 选中 0 号库（`selectDb` 定义在 `src/db.c`）；`c->ctime = c->lastinteraction = server.unixtime`。
- **输出静态缓冲**：`c->buf = zmalloc_usable(PROTO_REPLY_CHUNK_BYTES, &c->buf_usable_size)`。`PROTO_REPLY_CHUNK_BYTES` 在 `src/server.h` 里定义为 `16*1024`。
- **协议版本**：`c->resp` 在 `LOG_REQ_RES` 编译开关打开时取 `server.client_default_resp`，否则固定为 2。
- **输出链表**：`c->reply = listCreate()`，并给它装上 `listSetFreeMethod(c->reply, freeClientReplyValue)` 与 `listSetDupMethod(c->reply, dupClientReplyValue)` 两个回调。
- **命令侧**：`c->argc`/`c->argv`/`c->argv_len`/`c->all_argv_len_sum` 清零，`c->cmd = c->lastcmd = c->realcmd = c->lookedcmd = NULL`，`c->pending_cmds` 置空。
- **阻塞状态**：`initClientBlockingState(c)`（定义在 `src/blocked.c`），把 `bstate.btype` 设为 `BLOCKED_NONE` 并建好 `bstate.keys` 字典。
- **订阅与监听**：`c->watched_keys = listCreate()`；`c->pubsub_channels`、`c->pubsub_patterns`、`c->pubsubshard_channels` 三个字典都用 `objectKeyPointerValueDictType`。
- **事务**：`initClientMultiState(c)` 建好 MULTI/EXEC 的队列状态。
- **计时**：`resetClientCommandDuration(c)`（`src/server.h` 里的 static inline）一次清掉 `duration`、`cpu_duration`、`cpu_nvcsw`、`cpu_nivcsw` 四个字段，注释说明这样写是为了「不漏字段」。

最后 `if (conn) linkClient(c)`——只有真实连接的客户端才进 `server.clients` 链表与 `server.clients_index`（rax 索引）。`linkClient` 的注释说明它记住了链表节点位置，这样 `unlinkClient` 摘除时是常数时间，不用线性扫。

**`resetClient` 只做一件事。** 它转调 `resetClientInternal(c, num_pcmds_to_free)`，内部逻辑是：

- 有 `c->current_pending_cmd` 就 `freeClientPendingCommands(c, num_pcmds_to_free)`，清空后断言 `c->all_argv_len_sum == 0`；否则若 `c->argv` 还在（源码注释举的例子是模块的 `RM_Call`），就 `freeClientArgvInternal(c, 1)` 并把 `all_argv_len_sum` 清零。
- 把 `argc`、`cmd`、`argv_len`、`argv`、`cur_script`、`slot`、`cluster_compatibility_check_slot` 归位，清掉 `CLIENT_EXECUTING_COMMAND`。
- `serverAssert(c->duration == 0)`——断言命令耗时必须已经记到某条命令上了。
- 释放 `deferred_reply_errors`。
- 三个标志各有一条「延后一跳」语义：`CLIENT_ASKING` 在不在 MULTI 里、且刚执行的命令不是 `askingCommand` 时清掉；`CLIENT_TRACKING_CACHING` 同理，参照 `clientCommand`；`CLIENT_REPLY_SKIP` 被清掉，而 `CLIENT_REPLY_SKIP_NEXT` 被提升成 `CLIENT_REPLY_SKIP`。
- 清 `net_input_bytes_curr_cmd` 与 `net_output_bytes_curr_cmd`。

关键在于 `resetClient` **不碰** `querybuf`、不碰 `reply` 链表、不碰 `c->buf`、不碰大部分 `flags`——这正是它与 `freeClient` 的分界线。

**`freeClient` 的释放顺序。** 开头有两道前置处理：

1. 若客户端带 `CLIENT_PROTECTED` 标志，直接退化为 `freeClientAsync(c)` 并返回。源码注释说明：受保护的客户端此刻必须释放，就至少走异步释放。`protectClient` 的注释解释了保护的理由——重新进入事件循环时客户端可能被别的路径释放。
2. 若 `c->running_tid != IOTHREAD_MAIN_THREAD_ID` 则 `fetchClientFromIOThread(c)`；若 `c->tid != IOTHREAD_MAIN_THREAD_ID` 则 `keepClientInMainThread(c)`。然后 `server.io_threads_clients_num[c->tid]--`。

接着是模块与统计通知：`moduleFireServerEvent(REDISMODULE_EVENT_CLIENT_CHANGE, REDISMODULE_SUBEVENT_CLIENT_CHANGE_DISCONNECTED, c)`、`moduleNotifyUserChanged(c)`、`moduleFreeAuthBlockedClient(c)`；若客户端已带 `CLIENT_CLOSE_ASAP`，还要从 `server.clients_to_close` 里把它摘掉（注释说明这必须在前面做，因为后面可能调 `replicationCacheMaster`，那时客户端应当已经从待关闭链表里移除了）。

**第一条退化路径：与主库断连。** `if (server.master && c->flags & CLIENT_MASTER)` 且客户端没有 `CLIENT_PROTOCOL_ERROR`/`CLIENT_BLOCKED` 标志时，Redis **不释放这个对象**，而是清掉 `CLIENT_CLOSE_ASAP`/`CLIENT_CLOSE_AFTER_REPLY`、调 `replicationCacheMaster(c)` 然后直接 return。源码注释写明意图：把状态缓存下来以便稍后尝试部分重同步。

**第二条退化路径：受保护客户端转异步。** 见上，`freeClientAsync` 在主线程上是置 `CLIENT_CLOSE_ASAP` 并挂进 `server.clients_to_close`（副本还要先 `freeReplicaReferencedReplBuffer(c)`）；在 I/O 线程上则 `pauseIOThread` → 置 `CLIENT_IO_CLOSE_ASAP` → `enqueuePendingClientsToMainThread(c, 1)` → `resumeIOThread`。注释说明它适用于「需要终止客户端但客户端在当前流程里还得有效」的场合。真正释放发生在 `freeClientsInAsyncFreeQueue`，它遍历 `server.clients_to_close` 逐个 `freeClient(c)`——这个函数在 `beforeSleep` 里被调用（见 a02）。

**资源回收的清单。** `freeClient` 依次处理：

- 压缩状态：`clientDestroyCompressionState(c)`。
- 查询缓冲：带 `CLIENT_IO_REUSABLE_QUERYBUFFER` 标志则先 `resetReusableQueryBuf(c)`，再 `sdsfree(c->querybuf)` 并置 NULL。
- 阻塞状态：`resetClientCommandDuration(c)`；带 `CLIENT_BLOCKED` 标志则 `unblockClient(c, 1)`（注释说在途命令的耗时不再记录）；`dictRelease(c->bstate.keys)`。
- WATCH：`unwatchAllKeys(c)` 后断言 `c->watched_keys_mem == 0`，再 `listRelease(c->watched_keys)`。
- 订阅：`clearClientPubSubState(c)` 后断言 `c->pubsub_names_mem == 0`，再释放三个 pubsub 字典。
- 输出缓冲：`releaseAllBufReferences(c)`（先释放编码缓冲里对字符串对象的引用，避免释放对象后缓冲里还挂着野指针），`listRelease(c->reply)`，`zfree(c->buf)`。
- 其余：`freeReplicaReferencedReplBuffer(c)`、`freeClientOriginalArgv(c)`、`freeClientDeferredObjects(c, 1)`、`freeClientIODeferredObjects(c, 1)`、`tryUnlinkClientFromPendingRefReply(c, 1)`。
- 内存统计回退三条：`server.stat_clients_type_memory[c->last_memory_type] -= c->last_memory_usage`、`server.stat_clients_shared_memory -= c->last_memory_shared`、`server.stat_clients_unshared_memory -= c->last_memory_unshared`。这里的源码注释特别点明了一个坑：判断条件是 `if (c->conn || c == server.cached_master)`——只按 `c->conn` 判断会漏掉被缓存的主库客户端，因为它的 conn 已经被 `replicationCacheMaster()` 摘掉了，那样每次丢弃部分重同步状态都会泄漏一份 `mem_clients_normal`。

**`unlinkClient` 才是真正断开的地方。** 它先把 `server.current_client` 置空（若指向该客户端），然后：从 `server.clients_index`（rax）里按 id 删除、从 `server.clients` 链表摘除、清理 diskless 复制用的 `server.rdb_pipe_conns`、调 `connShutdown`/`connClose` 关掉 socket 并把 `c->conn` 置 NULL；从 `server.clients_pending_write` 摘除；从 `server.unblocked_clients` 摘除；`freeClientPendingCommands(c, -1)` 清空待处理命令；带 `CLIENT_TRACKING` 标志则 `disableTracking(c)`。它的注释说明这一段会关 socket、移除 I/O handler，并把仍然有效的待处理命令一并清掉。

**最后一段。** `freeClientMultiState(c)` 后断言 `pending_cmds.len == 0`。主从相关的两条分支分别是：`CLIENT_SLAVE` 时——若没配置 `save`、处于 `SLAVE_STATE_WAIT_BGSAVE_END`、子进程是磁盘型 RDB、且没有别的副本在等，就 `killRDBChild()`；处于 `SLAVE_STATE_SEND_BULK` 时关掉 `c->repldbfd`、释放 `c->replpreamble`；从 `server.monitors` 或 `server.slaves` 摘除；副本数归零时记 `server.repl_no_slaves_since`；调 `refreshGoodSlavesCount()`；并发出 `REDISMODULE_EVENT_REPLICA_CHANGE` / `OFFLINE` 事件。`CLIENT_MASTER` 时调 `replicationHandleMasterDisconnection()`。之后把客户端从内存桶里摘掉（`c->mem_usage_bucket->mem_usage_sum -= c->last_memory_usage` 并 `listDelNode`），最后 `himportFieldsetsFree(c)`、`decrRefCount` 掉 `name`/`lib_name`/`lib_ver`、断言 `c->all_argv_len_sum == 0`、`sdsfree` 掉 `peerid`/`sockname`/`slave_addr`/`node_id`，`zfree(c)`。

## 关键取舍

**源码事实：** `createClient` 用逐字段赋值而不是 `memset` 清零，字段全部显式写值。**源码事实：** 同一个结构既表示「真实连接客户端」也表示「无连接假客户端」，二者的区分靠 `c->conn` 是否为 NULL。**源码事实：** `resetClient` 只清命令相关状态，连接与两个缓冲一概保留。**源码事实：** `freeClient` 内部有两条明确的「不释放」退化路径——受保护客户端转异步释放、与主库断连时改为缓存状态。**源码事实：** 异步释放被拆成「打标记」与「实际释放」两段，后者在 `beforeSleep` 的 `freeClientsInAsyncFreeQueue` 里发生。**源码事实：** 内存统计的回退特意用 `c->conn || c == server.cached_master` 来兜住已缓存的主库客户端。

**架构推论：** 把释放拆成「标记」与「实际释放」两段，是为了让命令执行栈上的 `client *` 在当前调用栈内始终有效——否则 `addReply` 之后紧跟一次可能释放客户端的调用，就会是典型的 use-after-free。**架构推论：** 逐字段初始化虽然冗长，但让「新增一个 client 字段」成为编译期可见的改动点；代价是 `createClient` 本身随字段数线性变长。**架构推论：** 假客户端与真实客户端共用一套结构和一套回复 API，使模块、Lua 脚本、AOF 载入都能复用同一批 `addReply*` 函数；代价是每个写入点都要先过 `_prepareClientToWrite` 判断能不能写。**架构推论：** 把主从的特殊处理内嵌在通用的 `freeClient` 里，使复制语义成为「释放」的一部分而不是独立的一层；代价是这个函数无法保持纯粹的「释放资源」职责，任何复制侧改动都要在这里落一刀。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/networking.c` | `createClient` | 分配并逐字段初始化客户端对象，装配连接回调、缓冲、字典与阻塞状态，真实连接才挂进全局链表 |
| `src/networking.c` | `resetClient` | 每条命令执行后的复位：清参数、命令指针、耗时与几个延后一跳的标志 |
| `src/networking.c` | `freeClient` | 释放全部资源：缓冲、字典、复制状态、内存统计回退、摘除全局索引、关连接，最后 `zfree` 结构体 |

## 参考

- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— 客户端对象的创建、复位、释放与全部回复写入路径。
- [Redis 源码：server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— `client` 结构体定义与各类标志位、缓冲尺寸常量。
- [Redis 文档：客户端](https://redis.io/docs/latest/develop/reference/clients/) —— 客户端分类与连接管理的官方说明。
- [Redis 命令：CLIENT LIST](https://redis.io/docs/latest/commands/client-list/) —— 从运维视角观察客户端对象。

## 与主核 / AI 的关系

「创建—复用—释放」三段式与主核的连接/会话对象模型可以直接对照，但 Redis 的复位是**每条命令**都做一遍的轻量复位，主核的会话往往跨多条事务、需要更强的隔离与可见性语义，不能照抄这个粒度。Redis 的「标记 + 延迟释放」是单线程事件循环下的必然选择；主核是多写者引擎，连接对象可能正被另一个线程持有，延迟释放需要配合引用计数或 epoch 回收才能安全。AI 侧若把推理请求挂在长连接上，客户端对象会长期驻留且反复累积输出缓冲，Redis 的 `resetClient` 只清命令状态、不回收缓冲这一点尤其值得注意——主核需要额外的会话级资源上限与回收策略。
