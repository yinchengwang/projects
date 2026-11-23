---
project: redis
feature: client-input-parsing
page_type: feature
schema_version: 1
refs:
  - path: src/networking.c
    symbols: [readQueryFromClient, processInputBuffer]
  - path: src/networking.c
    symbols: [processMultibulkBuffer, processInlineBuffer]
---

# 客户端输入解析

## 一句话职责

Redis 把 socket 上读到的字节分成两种协议形态解析——RESP 多行批量与内联命令——把字节流还原成一组参数对象，并在参数齐备后交给命令分派。

## 机制是什么

客户端连接建立时注册的可读回调是 `readQueryFromClient`，它负责把 socket 里的字节搬进客户端的查询缓冲；`processInputBuffer` 是解析的主循环，负责判定协议形态、逐条切分命令、维护待执行命令队列；`processMultibulkBuffer` 与 `processInlineBuffer` 分别是两种形态的具体解析器。

Redis 同时支持两种线格式。**RESP 多行批量**是正式协议，形如 `*3\r\n$3\r\nSET\r\n$3\r\nkey\r\n$5\r\nvalue\r\n`；**内联命令**是给人手敲的 telnet 风格，形如 `SET key value\r\n`。两种形态的分界只看第一个字节是不是 `*`。

解析的产物不是直接写进客户端的 `argv` 字段，而是先放进一个 `pendingCommand` 结构（`src/server.h` 里的定义：参数个数、argv 数组、参数总长度、入站字节数、命令指针、键提取结果、复制偏移、标志位、槽位、读错误），再挂进客户端的 `pending_cmds` 双向链表。解析完成后才把 `pendingCommand` 的字段回填到客户端的旧字段上。

本页只讲从字节到参数的这一段；命令执行、回复编码、客户端对象的创建与释放都不在本页。

## 怎么实现的

**`readQueryFromClient` 的入口与缓冲策略。** 函数从 `connGetPrivateData(conn)` 取出客户端。两个提前返回的分支：读事件未启用，或压缩待处理（`c->compression_state` 为真但没有 `CLIENT_IO_COMPRESSION_ENABLED`），此时只置 `c->pending_read` 就返回，等压缩恢复后再读。

单次读取长度默认是 `PROTO_IOBUF_LEN`，在 `src/server.h` 里定义为 `(1024*16)`。但有一条特殊路径：当请求类型已是 `PROTO_REQ_MULTIBULK`、正在读一个长度参数、且该长度不小于 `PROTO_MBULK_BIG_ARG`（`src/server.h` 里定义为 `(1024*32)`）时，读取长度被改成「还差多少读多少」。源码注释解释了动机：让查询缓冲「contains exactly the SDS string representing the object」，这样 `processMultibulkBuffer` 就能免掉一次拷贝。

查询缓冲本身有三种来源。正在读大参数时用客户端私有缓冲；否则若客户端还没有缓冲，优先取线程级的可复用缓冲 `thread_reusable_qb`（同样是 `PROTO_IOBUF_LEN` 大小），并置 `CLIENT_IO_REUSABLE_QUERYBUFFER` 标记；若可复用缓冲已被占用（源码注释说这只发生在「commands are executed nested via processEventsWhileBlocked()」的场景），才退回到客户端私有缓冲。

缓冲扩容分两路：非主节点客户端且（正在读大参数，或当前分配小于 `PROTO_IOBUF_LEN`）走 `sdsMakeRoomForNonGreedy`，按需精确预留；否则走 `sdsMakeRoom`，并把本次读取长度改成 `sdsavail(c->querybuf)`，源码注释说这是「Read as much as possible from the socket to save read(2) system calls」。

**读取与错误分支。** `clientConnRead` 返回 `-1` 时，若连接状态仍是 `CONN_STATE_CONNECTED` 就跳到收尾等下一次；否则标记 `CLIENT_READ_CONN_DISCONNECTED` 并 `freeClientAsync`。返回 `0` 标记 `CLIENT_READ_CONN_CLOSED` 并 `freeClientAsync`。压缩开启时 `nread` 是解压后写入缓冲的字节数，`network_read` 才是真正从 socket 读到的字节数，两个计数分别进统计。

**查询缓冲上限。** `client_max_querybuf_len` 来自配置项 `client-query-buffer-limit`，在 `src/config.c` 里默认 `1024*1024*1024`（1GB），下限 `1024*1024`。超限时标记 `CLIENT_READ_REACHED_MAX_QUERYBUF`、`freeClientAsync`，并把 `server.stat_client_qbuf_limit_disconnections` 加一。未认证连接另有一条更严的 1MB 上限（源码里写死的 `1024*1024`），源码注释解释了原因：未认证时不允许命令在缓冲里堆积。

**`processInputBuffer` 的循环骨架。** 入口先 `atomicIncr(server.stat_total_client_process_input_buff_events, 1)`。每轮循环有五个中止条件：客户端被阻塞或待解除阻塞（`CLIENT_BLOCKED` / `CLIENT_UNBLOCKED`）；已经有待执行命令挂在 `c->argv` 上（`CLIENT_PENDING_COMMAND`）；客户端是主节点、当前在主线程、且身处可让出的长命令里（源码注释说这时主节点输入应当累积而不是回 `-BUSY`）；客户端已标记关闭（`CLIENT_CLOSE_AFTER_REPLY` / `CLIENT_CLOSE_ASAP`）。

**前瞻深度。** `lookahead` 决定一次最多解析出多少条待执行命令。未认证连接被强制降为 1，源码注释给了两条理由：减少内存开销，以及「AUTH can affect the handling of succeeding commands」——未认证状态下解析大命令可能导致后续命令被错误拒绝。其余情况用 `server.lookahead`，来自配置项 `lookahead`，在 `src/config.c` 里范围是 1 到 `INT_MAX`，默认值 `REDIS_DEFAULT_LOOKAHEAD`（`src/server.h` 里定义为 16）。

**协议形态判定与分支。** 首次解析时看 `c->querybuf[c->qb_pos]` 是不是 `*`：是则置 `PROTO_REQ_MULTIBULK`，否则置 `PROTO_REQ_INLINE`。内联分支直接 `acquirePendingCommand()` 取一条新的待解析命令交给 `processInlineBuffer`。多行批量分支先判断 `c->pending_cmds.len != c->pending_cmds.ready_len`——两者不等说明队列里有尚未解析完的命令，这时从**队尾**取出（`popPendingCommandFromTail`）接着解析同一条；否则新建一条。

解析出的命令 `addPendingCommand` 挂进链表后，若带读错误或 `PENDING_CMD_FLAG_INCOMPLETE` 标记就停下。随后记录该命令对应的复制偏移，调 `preprocessCommand` 预计算命令与键（这一步属于命令表那一页），再 `resetClientQbufState` 清掉 `reqtype`、`multibulklen`、`bulklen` 三个解析游标。

**解析深度统计。** 每轮比较解析前后的 `ready_len`，差值就是新解析出的命令数，累进 `server.stat_avg_pipeline_length_sum` / `_cnt` 与客户端级的 `c->stat_avg_pipeline_length_sum` / `_cnt`。

**取出一条命令并回填。** 从链表头取出就绪命令，把它的 `argc`、`argv`、`argv_len`、`input_bytes`（累加进 `c->net_input_bytes_curr_cmd`）、`reploff`（写进 `c->reploff_next`）、`slot`、`cmd`（写进 `c->lookedcmd`）、`read_error` 回填到客户端字段。源码注释说明了动机：「We populate the old client fields so we don't have to modify all existing logic to work with pendingCommands」——用回填换取存量逻辑不必改写。

**预取与执行分流。** 满足「本轮确实又解析出了命令、且在主线程、且就绪命令多于一条」时，做一次跨命令预取：`resetCommandsBatch`、`addCommandToBatch`、`prefetchCommands`。

致命读错误由 `isClientReadErrorFatal` 判定：读错误非 0 且不是「命令未找到」「参数个数不对」「跨槽」这三类可回复给客户端的错误，就跳出循环。`argc` 为 0 时（裸换行，主节点用来保活、副本用来刷新 ACK 时间）走 `prepareForNextCommand` 不执行命令。

**I/O 线程只解析不执行。** 若当前不在主线程（`c->running_tid != IOTHREAD_MAIN_THREAD_ID`），源码注释说「we can't really execute the command here」，于是置 `CLIENT_IO_PENDING_COMMAND` 并调 `enqueuePendingClientsToMainThread` 交给主线程，然后跳出。主线程上才调 `processCommandAndResetClient` 进入命令分派；它返回 `C_ERR` 表示客户端已失效，本函数立刻返回 `C_ERR`。

**缓冲裁剪。** 主节点客户端不能按 `qb_pos` 裁剪——它的查询缓冲还要代理给下级副本，所以按 `c->repl_applied` 裁剪并把 `qb_pos` 相应前移；源码注释列了三种不能按 `qb_pos` 裁剪的场景。其余客户端直接 `sdsrange` 裁掉已消费的前缀并把 `qb_pos` 归零。

**`processMultibulkBuffer` 的读头阶段。** 当 `c->multibulklen` 为 0 时先断言待解析命令的 `argc` 为 0。然后找 `'\r'`：找不到则若未解析的字节数超过 `PROTO_INLINE_MAX_SIZE`（`src/server.h` 里定义为 `(1024*64)`）标记 `CLIENT_READ_TOO_BIG_MBULK_COUNT_STRING`，否则返回 `C_ERR` 等更多数据。断言首字节是 `'*'`，用 `string2ll` 解析元素个数：解析失败或超过 `INT_MAX` 标记 `CLIENT_READ_INVALID_MULTIBUCK_LENGTH`；元素个数大于 10 且连接未认证时标记 `CLIENT_READ_UNAUTH_MBUCK_COUNT`——未认证连接的元素数上限是源码里写死的 10。随后 `c->qb_pos` 跳过 `*N\r\n`；个数小于等于 0 直接返回 `C_OK`（空命令）。最后把 `c->multibulklen` 设为个数、`c->bulklen` 设为 `-1`，并按需扩容 argv 数组——首次扩容上限是 `min(c->multibulklen, 1024)`。

**`processMultibulkBuffer` 的读参数阶段。** 循环条件是 `c->multibulklen` 非零。每轮先读长度：找 `'\r'`，找不到则超限报 `CLIENT_READ_TOO_BIG_BUCK_COUNT_STRING`，否则跳出等更多数据；断言首字节是 `'$'`，否则报 `CLIENT_READ_EXPECTED_DOLLAR`；`string2ll` 解析长度，解析失败、为负、或超过 `server.proto_max_bulk_len` 都报 `CLIENT_READ_INVALID_BUCK_LENGTH`。`proto_max_bulk-len` 在 `src/config.c` 里默认值是 `512ll*1024*1024`、下限 `1024*1024`；主节点客户端豁免这个上限，源码注释说主节点的查询缓冲「can only be trimmed after data applied and sent to replicas」。长度超过 16384 且连接未认证时，报 `CLIENT_READ_UNAUTH_BUCK_LENGTH`——未认证连接的单参数上限是源码里写死的 16384。

**大参数的两项优化。** 一是读长度之后，若不是主节点、长度不小于 `PROTO_MBULK_BIG_ARG`、且未解析的数据量不超过「长度 + 2」这个长度，就把查询缓冲前面的已消费部分剪掉、预留恰好够用的空间，并把峰值记进 `c->querybuf_peak`。源码注释解释：只有当「未解析的数据量 ≤ 长度 + 2」时才值得裁剪，否则「trimming querybuf is just a waste of time」。二是数据到齐之后，若不是主节点、`qb_pos` 为 0、长度不小于 `PROTO_MBULK_BIG_ARG`、且缓冲长度恰好等于「长度 + 2」，就走零拷贝路径：`createObject` 直接复用查询缓冲里的 sds，不复制内容；代价是必须换一个新的查询缓冲，源码注释说「Assume that if we saw a fat argument we'll see another one likely」，所以按 `bulklen + 2` 分配，但当 `bulklen` 不小于 `maxmemory / 32` 时改用 `PROTO_IOBUF_LEN` 大小，避免一次分配吃掉太多内存预算。条件不满足时回落到 `createStringObject` 复制。

**`processMultibulkBuffer` 的收尾。** `c->multibulklen` 归零时清掉 `PENDING_CMD_FLAG_INCOMPLETE` 并返回 `C_OK`；否则置上该标记同样返回 `C_OK`——返回成功但标记「还没解析完」，让调用方知道要等下一批数据。`readerr` 分支清掉该标记并返回 `C_ERR`，源码注释解释了原因：「A command that hit a read error is finished, however many reads it was parsed across, so it must not stay flagged as incomplete: otherwise `addPendingCommand()` will not count it as ready and the error is never reported to the client」。

**`processInlineBuffer` 的流程。** 找 `'\n'`：找不到则超过 `PROTO_INLINE_MAX_SIZE` 报 `CLIENT_READ_TOO_BIG_INLINE_REQUEST`，否则返回 `C_ERR`。若换行符前面紧邻 `'\r'`，则把换行位置前移并把 `linefeed_chars` 计数加一（用于正确推进 `qb_pos`）。接着按行取出一段，用 `sdssplitargs`（`src/sds.c`）拆分参数；拆分失败返回 `NULL` 时报 `CLIENT_READ_UNBALANCED_QUOTES`——引号不配对。

空行且客户端是副本时刷新 `c->repl_ack_time`（I/O 线程上写 `c->io_repl_ack_time` 以避免与主线程争用），源码注释说这是给正在载入大 RDB 文件的副本用来保活的手段。主节点发来非空的内联协议则报 `CLIENT_READ_MASTER_USING_INLINE_PROTOCOL`，源码注释判断这多半是协议失步（「for example because of a PSYNC gone bad」），但主节点发空行是允许的。

参数拆分完成后，为每个参数建字符串对象；源码在创建前用 `sdsRemoveFreeSpace` 修剪 `sdssplitargs` 的贪婪分配，并同步累计参数长度与客户端级的 `all_argv_len_sum`。入站字节数按 `argv_len_sum + (argc - 1) + 2` 计算，源码注释说明这是「every whitespace is of length 1, with the exception of the trailing '\r\n' being length 2」。

**协议错误的落地。** 读错误码由 `handleClientReadError` 翻译成具体的错误回复并关闭连接；`setProtocolError` 负责把现场写进日志——按 `PROTO_DUMP_LEN`（源码里定义为 128）截取一段查询缓冲内容并把不可打印字符替换成 `.`，最后给客户端打上 `CLIENT_CLOSE_AFTER_REPLY` 与 `CLIENT_PROTOCOL_ERROR`。

## 关键取舍

**源码事实：** 两种协议形态并存，判据只有首字节是不是 `*`；内联协议用 `sdssplitargs` 拆词，支持引号与转义。**源码事实：** 解析产物先进 `pendingCommand` 队列、再回填到客户端旧字段，源码注释写明这是为了不改写存量逻辑。**源码事实：** 大参数的阈值是 `PROTO_MBULK_BIG_ARG`（32KB），只有超过它才启用对齐读取与零拷贝复用。**源码事实：** 未认证连接被收紧到四处：前瞻深度 1、单命令元素数上限 10、单参数长度上限 16384、查询缓冲上限 1MB。**源码事实：** I/O 线程只做解析不做执行，解析完把客户端交给主线程。

**架构推论：** 把「解析」与「执行」拆成两段、并在中间放一个待执行命令队列，让 Redis 能一次性解析多条命令并跨命令预取执行函数，代价是每个客户端多一份 argv 存储与一次字段回填。**架构推论：** 未认证连接的收紧限额是把未认证流量当作不可信输入处理的典型做法——用四条硬限制换取「未认证阶段不消耗过多内存、也不让大命令在认证语义未定时被误解析」；代价是未认证客户端发大命令会直接失败，而不是等认证后再发。**架构推论：** 大参数的零拷贝路径依赖「缓冲里恰好只有这一个参数」这一强条件，条件不满足就回落到复制——这是一次用条件复杂度换内存带宽的取舍。**架构推论：** 把解析深度做成 `lookahead` 配置项而不是写死，是把流水线效率调成一个可运营的旋钮；代价是它在多写者场景里没有对应物，主核必须自己定义「一次读多少、预取多少」的策略。**架构推论：** 主节点客户端不按 `qb_pos` 裁剪缓冲，是因为那份缓冲还兼作下游副本的数据源——一个缓冲承担两种用途的代价是裁剪逻辑要分叉。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/networking.c` | `readQueryFromClient` | 连接可读回调：把 socket 字节搬进查询缓冲并施加缓冲上限 |
| `src/networking.c` | `processInputBuffer` | 解析主循环：判定协议形态、维护待执行命令队列、把就绪命令交给分派 |
| `src/networking.c` | `processMultibulkBuffer` | RESP 多行批量的解析器：读元素个数与逐个读参数 |
| `src/networking.c` | `processInlineBuffer` | 内联命令的解析器：按行取出并用 `sdssplitargs` 拆词 |

## 参考

- [Redis 源码：networking.c](https://github.com/redis/redis/blob/unstable/src/networking.c) —— 输入缓冲、协议解析与待执行命令队列的实现。
- [Redis 开发文档：RESP 协议规范](https://redis.io/docs/latest/develop/reference/protocol-spec/) —— 多行批量协议的线格式定义。
- [Redis 开发文档：客户端](https://redis.io/docs/latest/develop/clients/) —— 客户端连接、缓冲与内存占用的整体说明。
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/) —— 查询缓冲与客户端内存占用相关的调优项。

## 与主核 / AI 的关系

「按字节流切分命令 → 排队 → 逐条交给执行」的骨架可直接迁移到主核的语句解析层，但 Redis 的解析器完全信任连接已通过 TLS 与认证这两道关卡，主核若支持多租户连接，必须把未认证限额那套思路重新设计一遍，因为主核的「认证前可执行的动作」与 Redis 不同。解析与执行分离、允许先批量解析再批量执行，也是主核做语句级预编译（prepare 阶段）的自然前提。AI 侧若用 Redis 承载向量缓存，`lookahead` 与查询缓冲上限会直接影响高并发短命令下的内存占用与尾延迟——AI 推理服务常见的「批量小请求」形态恰好落在解析路径最密集的区域。
