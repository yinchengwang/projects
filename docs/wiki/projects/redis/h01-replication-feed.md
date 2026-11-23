---
project: redis
feature: replication-feed
page_type: feature
schema_version: 1
refs:
  - path: src/replication.c
    symbols: [replicationFeedSlaves, replicaofCommand]
---

# 主从复制传播

## 一句话职责

主从复制在 Redis 里是「命令流复制」：主节点把执行过的写命令重新按 RESP 编码成字节流，汇进一份全局复制缓冲区，各副本连接各自从这段字节流里取走自己那份，从而把一致性建立在**同一条命令流的顺序复制**上。

## 机制是什么

本机制由两个方向相反的入口组成。

**主节点方向**——`replicationFeedSlaves` 是把客户端写命令转成复制流的入口。它不负责连接本身，也不负责把数据写进 socket：它只把命令序列化成字节，追加进全局复制缓冲区（`server.repl_buffer_blocks`），并为每个副本维护「我从这块缓冲区的哪个位置开始读」的引用。副本真正拿到字节走的是它自己的客户端输出缓冲。

**副本方向**——`replicaofCommand` 是用户可见的 `REPLICAOF` / `SLAVEOF` 入口，负责切换本实例的主从身份。它自己不碰网络：`REPLICAOF host port` 调 `replicationSetMaster` 落定目标地址并触发连接；`REPLICAOF NO ONE` 调 `replicationUnsetMaster` 把自己提升为主。连接建立之后的握手与全量/增量同步由 `syncWithMaster`、`slaveTryPartialResynchronization` 驱动——本页只交代入口与状态推进，同步数据本身的格式与落盘写出属于 g01 的范围。

副本的连接维护与偏移推进由 `replicationCron` 承担：它的函数注释写明「called 1 time per second」，实际由 `serverCron` 按周期调用（正常路径约每秒一次，处于故障转移中时更频繁），负责连接超时、全量传输超时、重连、保活 PING、断开僵死副本、回收无主 backlog。

## 怎么实现的

### 主节点侧：一条写命令如何变成复制流

`replicationFeedSlaves` 的函数体按固定顺序做这些事。

**1. 参数断言。** 函数第一行是 `serverAssert(dictid == -1 || (dictid >= 0 && dictid < server.dbnum))`。`dictid == -1` 表示这条命令不触碰键空间，函数上方的注释举例是 `PING` 与 `REPLCONF`——这种情况不需要复制 `SELECT`。

**2. 两条早退路径。**

- `if (server.masterhost != NULL) return;`：本节点自己是副本。源码注释给的理由是：非顶层主节点只做代理，把从主节点收到的流原样转发给下游，好让下游副本**对外宣告与顶层主相同的 replication ID**（因为它共享同一份复制历史、同一份 backlog 与同一组偏移）。这条路径由 `replicationFeedStreamFromMasterStream` 承接，它把收到的原始字节直接喂进自己的复制缓冲区，不重新序列化。
- `if (server.current_client && server.current_client->flags & CLIENT_MASTER) return;`：当前命令的来源客户端就是主节点连接（`CLIENT_MASTER`），说明这条命令本身是从主节点转发来的，再序列化一遍就是多余。函数上方的注释补充：槽位原子迁移期间同样走代理。

**3. 无副本且无 backlog 的早退。** `if (server.repl_backlog == NULL && listLength(slaves) == 0)` 时只把 `server.master_repl_offset` 加 1 就返回。源码注释解释：这个偏移即使没有复制活动也要推进，因为 AOF 的 fsync 追踪依赖它；并注明「如果 AOF 也关掉了，这段代码不会走到」。

**4. 先装副本的写通道，再喂数据。** `prepareReplicasToWrite()` 的位置被源码注释明确约束：必须在把复制流喂进全局复制缓冲**之前**调用，因为 `prepareClientToWrite` 里的 `clientHasPendingReplies` 会访问全局复制缓冲做判断。`prepareReplicasToWrite` 遍历 `server.slaves`，先用 `canFeedReplicaReplBuffer` 过滤，排除四类副本：只想要 RDB 的（`CLIENT_REPL_RDBONLY`）、槽迁移目标（`CLIENT_ASM_MIGRATING`）、还在等 BGSAVE 开始或等 RDB 通道的（`SLAVE_STATE_WAIT_BGSAVE_START` / `SLAVE_STATE_WAIT_RDB_CHANNEL`）、以及即将被关闭的（`CLIENT_CLOSE_ASAP`）。通过筛选的调 `prepareClientToWrite`，成功则计数。

**5. 按需补一条 SELECT。** 只有 `dictid != -1` 且 `server.slaveseldb != dictid` 时才补。db 号小于 `PROTO_SHARED_SELECT_CMDS`（`src/server.h` 里定义为 10）时直接复用预生成的 `shared.select[dictid]`；否则现场拼 `"*2\r\n$6\r\nSELECT\r\n$<len>\r\n<id>\r\n"`。补完后调 `clusterSlotStatsDecrNetworkBytesOutForReplication` 把这段字节从按槽统计的网络流出里扣掉——源码注释说明 SELECT 不属于任何槽，但写入复制缓冲的过程把它算进去了，所以要抵消。写完更新 `server.slaveseldb = dictid`。

**6. 序列化命令本身。** 用 `replBufWriter` 这个批量写结构（`src/replication.c` 内定义，注释写明「数据写入只是 memcpy 进尾部块，全部记账推迟到 `replBufWriterEnd`」）先写多 bulk 计数 `*<argc>`，再逐参数写 `$<len>` 与内容。长度头走 `replBufWriterAppendBulkLen`：值落在 `OBJ_SHARED_BULKHDR_LEN`（`src/server.h` 里定义为 32）以内时直接取预建的 `shared.bulkhdr` / `shared.mbulkhdr` 表项，否则手工格式化。参数对象若 `encoding == OBJ_ENCODING_INT`（共享整数对象），用 `ll2string` 转成十进制文本写入——因为对象里存的是 `long` 的二进制内存，直接写出去从库无法解析。

**7. 偏移推进与引用维护（发生在 `replBufWriterEnd`）。** `server.master_repl_offset += wr->total_len`、`server.repl_backlog->histlen += wr->total_len`；随后遍历副本，把还没建立引用的副本的 `ref_repl_buf_node` / `ref_block_pos` 指向本次写入的起点块并增加该块引用计数；只有当本次分配了新块时才调 `closeClientOnOutputBufferLimitReached` 检查副本的输出缓冲上限。若本次新分配了块，还会调 `incrementalTrimReplicationBacklog(REPL_BACKLOG_TRIM_BLOCKS_PER_CALL)` 增量裁剪 backlog（该常量在 `src/server.h` 里定义为 64）。

**真实 caller（谁会调它）。** 一共四处：`src/server.c` 的 `propagateNow`（`PROPAGATE_REPL` 分支，即写命令执行后的传播登记点）；`src/server.c` 的 `sendGetackToReplicas`（上一轮有客户端在等复制确认，向副本索取 ACK）；`src/replication.c` 的 `replicationCron`（保活 PING，见下）；`src/rdb.c` 的 RDB 载入路径（载入时把已过期的键补一条 `DEL`/`UNLINK` 传播给副本）。另有 `src/debug.c` 的 `DEBUG replicate` 与 `src/cluster_asm.c` 的槽迁移路径会直接调用它。

### 副本侧：`REPLICAOF` 如何切换身份

`replicaofCommand` 先做三道门禁：集群模式下直接回 `"REPLICAOF not allowed in cluster mode."`（注释说明集群模式用当前节点地址自动配置复制）；故障转移进行中回 `"REPLICAOF not allowed while failing over."`；若客户端本身带 `CLIENT_SLAVE` 标记则回 `"Command is not valid when client is a replica."`——源码注释给的理由是：这条命令会 flush 掉所有副本的连接，而当前客户端也在其中。

参数上，主机名原样取 `c->argv[1]->ptr`，端口用 `getRangeLongFromObjectOrReply` 校验落在 0–65535。若当前已连着同一个 host:port，函数不做任何动作，回 `"+OK Already connected to specified master\r\n"`。

`REPLICAOF NO ONE`（大小写不敏感）走 `replicationUnsetMaster`：它先把 `server.masterhost` 置空再 `freeClient(server.master)`，源码注释解释了这个顺序——`freeClient` 会触发 `replicationHandleMasterDisconnection`，而后者可能在函数内部直接发起重连。随后调 `shiftReplicationId()`（注释：从主节点继承来的复制 ID 在当前偏移之前作为次 ID 保留，同时生成新的复制 ID 继续新的复制历史）、`disconnectSlaves()`（注释：副本必须知道复制 ID 变了，但因为它们仍能部分重同步，重连会很快）、把 `server.repl_state` 置为 `REPL_STATE_NONE`、把 `server.slaveseldb` 置为 -1（注释：保证新的主节点会以一条 SELECT 开始复制流），并把 `server.repl_no_slaves_since` 设为当前时间。

`REPLICAOF host port` 走 `replicationSetMaster`：它同样先释放旧主客户端，再置 `server.masterhost`——源码注释强调「必须在 `freeClient` 之后设置 `masterhost`」，理由与上面相同。若本实例原本是主（`was_master`），会先 `replicationDiscardCachedMaster()` 再 `replicationCacheMasterUsingMyself()`，注释写明这是为了之后能用新主节点做 PSYNC。它**不会**断开已有的下游副本——注释说它们有可能与新主做部分重同步，只有在复制 ID 变化或全量同步完成时才会被断开以强制重同步。最后它把 `server.repl_state` 置为 `REPL_STATE_CONNECT`、清零同步尝试计数、记日志 `"Connecting to MASTER %s:%d"`，并调 `connectWithMaster()`。

`connectWithMaster` 走非阻塞连接：`connCreate` 后 `connConnect(server.el, connTypeOfReplication())` 连到 `masterhost:masterport`，连接回调是 `syncWithMaster`；失败则记 `"Unable to connect to MASTER: %s"`、关连接、返回 `C_ERR`；成功则置 `server.repl_state = REPL_STATE_CONNECTING` 并记 `"MASTER <-> REPLICA sync started"`。

### 副本侧：握手、连接维护与偏移推进

**握手状态机在 `syncWithMaster` 里。** 它按 `server.repl_state` 逐段推进：连上后先发 `PING`，接受的有效回复只有 `+` 开头（源码注释说明老版本 Redis 用的是 `"operation not permitted"`，所以 `-NOPERM`、`-ERR operation not permitted` 也放行）；随后在 `REPL_STATE_SEND_HANDSHAKE` 段依次发出：需要时 `AUTH`（带 `masteruser` 时发用户名+密码）、`REPLCONF listening-port`、`REPLCONF ip-address`（仅当配了 `slave-announce-ip`）、`REPLCONF rdb-no-compress 1 rdb-no-checksum 1`（仅当走 diskless load，注释说这样能加快 RDB 交付与载入）、`REPLCONF compression <level>`（仅当 `repl-compression` 大于 0 且连接有可用的 IO 线程，否则记日志说明不请求）、以及 `REPLCONF capa eof capa psync2`（外加 RDB 通道能力）。之后才是 PSYNC。

**部分/全量同步的判据在 `slaveTryPartialResynchronization`。** 有缓存主客户端时用它的 replication ID 与 `reploff+1` 发 `PSYNC <replid> <offset>`，否则发 `PSYNC ? -1` 并记日志 `"Partial resynchronization not possible (no cached master)"`。主节点的三种回复各有分支：`+FULLRESYNC` 解析出新 replid 与初始偏移；`+RDBCHANNELSYNC` 表示部分同步不可行、改走专用 RDB 通道；`+CONTINUE` 表示部分同步被接受——此时若主节点的复制 ID 变了，副本把旧 ID 存为 `replid2`、把 `second_replid_offset` 设为当前偏移加一，并 `disconnectSlaves()` 通知自己的下游。

**偏移回报。** 副本侧 `replicationSendAck` 发 `REPLCONF ACK <reploff>`，当 `server.fsynced_reploff != -1` 时还附带 `FACK <fsynced_reploff>`。主节点侧由 `replconfCommand` 的 `ack` 分支接收：只有带 `CLIENT_SLAVE` 标记的客户端才被接受，且 `c->repl_ack_off` 只在「新值大于旧值」时前进（单调不回退），`fack` 同理更新 `c->repl_aof_off`；最后把 `c->repl_ack_time` 置为当前时间，这就是超时判定用的时间戳。

**连接维护在 `replicationCron` 里（函数注释写明「called 1 time per second」）。** 它依次处理：连接超时的副本侧检查（处于 `REPL_STATE_CONNECTING` 或握手中且 `time(NULL)-server.repl_transfer_lastio` 超过 `server.repl_timeout` → 记 `"Timeout connecting to the MASTER..."` 并 `cancelReplicationHandshake(1)`）；全量传输超时（`REPL_STATE_TRANSFER` 且同样超过 `repl_timeout` → 记 `"Timeout receiving bulk data from MASTER..."`）；`repl_state` 回到 `REPL_STATE_CONNECT` 时重连；给副本发保活 PING（条件是本实例不是副本、有副本、且距上次发复制流已超过 `server.repl_ping_slave_period`——该配置项在 `src/config.c` 里默认值为 10 秒）；给等待 BGSAVE 的副本写一个换行保活（注释说明这类副本只收代理数据，没有显式 ping，以免改动复制偏移）；断开僵死副本（在线状态副本的 `repl_ack_time` 超过 `repl_timeout` 就 `freeClient`；磁盘化全量同步的副本在 `SLAVE_STATE_WAIT_BGSAVE_END` 阶段也检查部分写超时）；最后在没有副本且空闲超过 `repl_backlog_time_limit` 时回收 backlog（注释详细解释了为什么必须同时 `changeReplicationId()` 与 `clearReplicationId2()`：否则旧主与新主可能持有相同 replid，导致后续 PSYNC 接受却数据不一致）。

保活 PING 走的就是 `replicationFeedSlaves(server.slaves, -1, ping_argv, 1)`——`dictid` 传 -1 表示它不触碰键空间、也不需要 SELECT。但源码注释指出：集群手动故障转移进行中不发这个 PING，因为它会改动主从双方的复制偏移，与 `mf_master_offset` 记录的状态对不上。

## 关键取舍

**源码事实：** 复制流是**命令级**而非数据级的——主节点不复制键值，而是复制命令的 RESP 字节；这意味着副本必须能重放同样的命令才能得到同样的结果。**源码事实：** 副本不直接消费主节点的 socket，而是共享一份全局复制缓冲区，各自用块引用计数标记读取位置（`ref_repl_buf_node` / `ref_block_pos`），因此多个副本的读取进度彼此独立。**源码事实：** backlog 的裁剪是**增量**的：`incrementalTrimReplicationBacklog` 每次最多释放 `REPL_BACKLOG_TRIM_BLOCKS_PER_CALL`（64）个块，源码注释说明原因是「若副本断开时需要一次释放很多块，会花很久并冻结服务端」。**源码事实：** 引用计数阻止裁剪——源码注释写明：只要还有副本引用第一个缓冲块，就不裁剪 backlog，即使此时 backlog 实际大小已经超过配置值；这等于「隐式地把 backlog 撑大」以换取尽可能多的部分重同步机会。**源码事实：** `replicationFeedSlaves` 写共享整数对象时会转成十进制文本，而不是直接写 `long` 的内存。**源码事实：** 保活 PING 与给预同步副本写的换行都刻意区分：前者走正常复制流会改动偏移，后者是带外数据不影响偏移。

**架构推论：** 用「全局缓冲 + 每副本独立引用」代替「主节点为每个副本单独维护一份队列」，把 N 个副本的内存占用从 O(N×数据量) 降到 O(数据量 + N×块引用)，代价是多了一层引用计数与裁剪逻辑，也带来了「有副本引用时 backlog 撑大」的不可预测内存占用。**架构推论：** 命令级复制把一致性责任推给了副本的执行环境：副本重放命令时若读到不同的键空间状态，就会与主节点分叉。Redis 用「单线程 + 相同命令序列」来保证两者一致，这套前提在主核的多写者引擎里不成立——主核若要复制，不能照搬命令流。**架构推论：** `replicaofCommand` 把「身份切换」做成一条命令而不是配置项，意味着切换是运行期可逆的；代价是切换瞬间的复制流是不连续的，需要靠 PSYNC/backlog 去缝合。**架构推论：** 把超时判定集中在 `replicationCron` 而不是分散在事件回调里，好处是判定口径统一（都用 `server.unixtime` 秒级时间戳），代价是最坏要等一个 cron tick 才发现僵死连接。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/replication.c` | `replicationFeedSlaves` | 主节点侧的写命令传播入口：早退判定、补 SELECT、序列化命令、推进偏移与副本引用 |
| `src/replication.c` | `replicaofCommand` | `REPLICAOF` / `SLAVEOF` 命令实现：门禁校验、切换主从身份 |

## 参考

- [Redis 源码：replication.c](https://github.com/redis/redis/blob/unstable/src/replication.c)
- [Redis 文档：复制](https://redis.io/docs/latest/operate/oss_and_stack/management/replication/)
- [Redis 文档：PSYNC 部分重同步](https://redis.io/docs/latest/commands/psync/)
- [Redis 命令：REPLICAOF](https://redis.io/docs/latest/commands/replicaof/)

## 与主核 / AI 的关系

Redis 的复制是**单写者 + 命令流**模型：一致性来自「只有主节点执行写命令，副本重放同一条命令流」。主核是多写者引擎，写可以来自任意会话与任意节点，不存在「一个主节点产生唯一命令序列」的前提，因此不能照搬这套传播机制；主核若做复制，更可能复制 WAL 记录或带版本戳的数据版本，而不是重放命令。

AI 侧若用 Redis 做向量缓存的副本读扩展，复制延迟会直接表现为「主库刚写入的向量在副本上暂时查不到」。Redis 本身给的一致性保证是**最终一致**且**不保证副本读到的一定是最新**——`repl_ack_off` 与 `master_repl_offset` 的差值正是这个滞后的度量，运维时应盯这个差值而不是假设副本强一致。另外，命令级复制意味着副本会重放主节点执行过的**所有**写命令，包括那些业务上并不希望被副本执行的副作用（如本地文件写入），这类副作用在主核的多租户场景里需要额外的写入抑制标记。
