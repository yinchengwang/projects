---
project: redis
feature: blocked-clients
page_type: feature
schema_version: 1
refs:
  - path: src/blocked.c
    symbols: [blockForKeys, handleClientsBlockedOnKeys, unblockClient]
---

# 阻塞客户端与唤醒

## 一句话职责

`blockForKeys` 把要等键变化的客户端置为阻塞态，并在「客户端等哪些键」「哪些键有哪些客户端在等」两张表上双向登记；`handleClientsBlockedOnKeys` 在写命令结束后消费就绪键队列，按 FIFO 逐个重放命令；`unblockClient` 做按阻塞类型的清理、撤销登记，并把客户端交回事件循环立刻续跑。

## 机制是什么

阻塞状态挂在客户端自己身上：`client` 的 `bstate` 字段是一个 `blockingState` 结构（定义在 `src/server.h`），持有 `btype`（阻塞类型）、`timeout`（单调时间毫秒截止点，取 0 表示无限等待）、`unblock_on_nokey`、`keys`（该客户端正在等的键字典），以及只在 `WAIT`/`WAITAOF` 用的 `numreplicas`、`numlocal`、`reploffset`、只在模块阻塞用的 `module_blocked_handle` 与 `async_rm_call_handle`、只在异步释放用的 `lazyfreeStartTime`。初始化由 `initClientBlockingState` 完成：`btype` 置 `BLOCKED_NONE`、`timeout` 置 0，并用 `objectKeyHeapPointerValueDictType` 建出 `keys` 字典。

`btype` 的取值来自 `src/server.h` 里的 `blocking_type` 枚举：`BLOCKED_NONE`、`BLOCKED_LIST`（BLPOP 一类）、`BLOCKED_WAIT`、`BLOCKED_WAITAOF`、`BLOCKED_MODULE`、`BLOCKED_STREAM`（XREAD）、`BLOCKED_ZSET`（BZPOP 一类）、`BLOCKED_POSTPONE`、`BLOCKED_POSTPONE_TRIM`、`BLOCKED_SHUTDOWN`、`BLOCKED_LAZYFREE`、`BLOCKED_LIST_NONEMPTY`（BLMOVEM EXACTLY 这类等已有列表长够元素的场景），末尾两项是 `BLOCKED_NUM` 与 `BLOCKED_END`。`server.blocked_clients_by_type` 就是按 `BLOCKED_NUM` 定长的计数数组。

登记用两张反方向的表，都挂在 `redisDb` 上（`src/server.h` 的 `redisDb` 结构）：`blocking_keys` 是「键 → 阻塞在该键上的客户端链表」，`blocking_keys_unblock_on_nokey` 是前者子集，只记「键被删除或被覆盖时也要唤醒的客户端」并以引用计数形式存放。就绪信号走两级队列去重：每个库的 `ready_keys` 字典保证同一个键在一次传播里只入队一次，`server.ready_keys` 列表存放 `readyList` 结构（`db`、`key`、`wake_modules`），由 `handleClientsBlockedOnKeys` 消费。

被阻塞的客户端并没有停止读事件：源码注释写明它仍会把收到的数据收进 query buffer，只是不再解析执行。所以解除阻塞后必须有人主动补跑一次，`queueClientForReprocessing` 负责把客户端塞进 `server.unblocked_clients` 并置 `CLIENT_UNBLOCKED`（`src/server.h` 中定义为 `1<<7`），`processUnblockedClients` 在 `beforeSleep` 里把这批客户端的输入缓冲处理掉。

## 怎么实现的

**第一步：登记等待的键（`blockForKeys`）。** 入参是阻塞类型 `btype`、键数组、键个数、`timeout` 与 `unblock_on_nokey`。函数只在客户端没有 `CLIENT_REEXECUTING_COMMAND` 时才写入新 `timeout`，重放命令时保留最初的截止点。对每个键先 `dictAddRaw` 到 `c->bstate.keys`，返回 NULL 说明这个键之前已登记过，直接 `continue`；否则 `incrRefCount` 后对 `c->db->blocking_keys` 做 `dictAddRaw`，新建条目时创建一个客户端链表并把键再 `incrRefCount` 一次，已有条目则复用旧链表，然后 `listAddNodeTail` 把客户端挂到尾部，并用 `dictSetVal` 把链表节点地址写回 `c->bstate.keys`，这样解除时可以 O(1) 摘链。若 `unblock_on_nokey` 为真，再往 `blocking_keys_unblock_on_nokey` 加键：首次出现时 `dictSetUnsignedIntegerVal` 置 1，已存在则 `dictIncrUnsignedIntegerVal` 累加。收尾写 `c->bstate.unblock_on_nokey`；`btype` 不是 `BLOCKED_MODULE` 时置 `CLIENT_PENDING_COMMAND`（`1<<30`），因为原生阻塞命令要在解除后按原命令重放，模块有自己的重放通道；最后调 `blockClient`。

**第二步：置阻塞位（`blockClient`）。** 先用 `serverAssert` 断言「主客户端（`CLIENT_MASTER`）不能被阻塞，除非类型是 `BLOCKED_MODULE`、`BLOCKED_LAZYFREE`、`BLOCKED_POSTPONE`、`BLOCKED_POSTPONE_TRIM`」，然后置 `CLIENT_BLOCKED`（`src/server.h` 中定义为 `1<<4`）、写 `c->bstate.btype`。计数只在非 `CLIENT_MODULE` 客户端上累加 `server.blocked_clients`，但 `server.blocked_clients_by_type[btype]` 对所有类型都加一；随后 `addClientToTimeoutTable`。

**第三步：写侧发就绪信号。** `signalKeyAsReadyLogic` 是三个包装函数（`signalKeyAsReady`、`signalKeyAsReadyNonEmptyList`、`signalDeletedKeyAsReady`）的共同核心。它先按类型做快速返回：`BLOCKED_NONE` 表示该对象类型从不产生阻塞等待者，直接返回；再算 `wake_modules`（`btype != BLOCKED_LIST_NONEMPTY` 才为真，即「已有列表变长」这类信号不唤醒模块客户端），然后检查是否真的有人在等——`BLOCKED_LIST` 信号要把 `BLOCKED_LIST` 与 `BLOCKED_LIST_NONEMPTY` 两个计数都算上，其它类型只看自己那一格，条件写成「本类型无人等，且（`wake_modules` 为真时）`blocked_clients_by_type[BLOCKED_MODULE]` 也为 0」就返回。之后按 `deleted` 分支：删除侧要求键存在于 `blocking_keys_unblock_on_nokey`，写入侧要求键存在于 `blocking_keys`，否则不入队。入队前用 `dictAddRaw(db->ready_keys, key, &existing)` 去重，返回 NULL 说明已经排过队；成功时 `incrRefCount`，再 `zmalloc` 一个 `readyList` 填 `key`、`db`、`wake_modules` 后 `listAddNodeTail(server.ready_keys, rl)`。

真实调用方分别是：`dbAddInternal`（新建键时发 `signalKeyAsReady`）、`setKeyByLink`（覆盖已有键时，同类型列表发 `signalKeyAsReadyNonEmptyList`、类型变化发 `signalKeyAsReady`）、`dbGenericDelete`（删除键时发 `signalDeletedKeyAsReady`）、`dbSetValue`（覆盖前发 `signalDeletedKeyAsReady`）、`pushGenericCommand`（`lpushCommand`/`rpushCommand`/`lpushxCommand`/`rpushxCommand` 的共同实现）、`linsertCommand`、`lmoveHandlePush`、`lmovemMoveAndReply`（这四个都在向**已存在**的列表写时发 `signalKeyAsReadyNonEmptyList`）、`xaddCommand`/`xgroupCommand`/`xnackCommand` 等流命令（发 `signalKeyAsReady(..., OBJ_STREAM)`）、`RM_SignalKeyAsReady`（模块侧）、`scanDatabaseForReadyKeys` 与 `scanDatabaseForDeletedKeys`（swapdb / flush 后补发）、`handleClaimableStreamEntries`（`XREADGROUP` 带 `MINIDLE` 时，PEL 条目过期可领取则发就绪信号）。

**第四步：消费就绪队列（`handleClientsBlockedOnKeys`）。** 函数用一个静态 `in_handling_blocked_clients` 关门，重入直接返回，注释给出的理由是「避免破坏公平性」。入口处还有一个断言：`server.also_propagate.numops == 0`，即只能在传播调用的基本状态下被调用。主体是两层循环：外层在 `server.ready_keys` 非空时，把当前列表存到局部变量 `l`、把 `server.ready_keys` 换成 `listCreate()` 的新列表，于是循环里再调 `signalKeyAsReady` 就能安全追加新键（BLMOVE 这类会被自己解阻塞再阻塞的命令正是靠这一点）；内层逐个取出 `readyList`，先 `dictDelete(rl->db->ready_keys, rl->key)` 放开该键的去重标记，再交给 `handleClientsBlockedOnKey`，之后 `decrRefCount` 键、`zfree` 结构、摘链。

`handleClientsBlockedOnKey` 从 `rl->db->blocking_keys` 取出该键的客户端链表，并用 `long count = listLength(clients)` 记住初始长度，循环条件写成 `count-- > 0`，注释说明这是为了防止「重放后又阻塞」导致死循环。每轮都重新 `dictFind` 一遍链表（前一个客户端的处理会把链表删空），取表头节点、先 `listRotateHeadToTail` 把该客户端旋到尾部再处理：该节点会在本轮被摘掉，而重放中还会把别的等待者挤出链表，先旋转才能既保留其余人的相对次序又推进到下一个。然后用 `lookupKeyReadWithFlags(..., LOOKUP_NOEFFECTS)` 读键，用 `getBlockedTypeByType` 把对象类型翻成阻塞类型，匹配规则是三条并列：原生类型对上（`receiver->bstate.btype == obtype`，其中列表对象额外接纳 `BLOCKED_LIST_NONEMPTY` 等待者）、键存在且等待者是 `BLOCKED_MODULE`、或者等待者带 `unblock_on_nokey`。命中后，等待者不是模块客户端就调 `unblockClientOnKey`，是模块客户端则仅在 `rl->wake_modules` 为真时调 `moduleUnblockClientOnKey`。

**第五步：重放命令（`unblockClientOnKey`）。** 先 `dictFind` 出该键在客户端 `keys` 字典里的条目，用 `releaseBlockedEntry(c, de, 1)` 摘链并删掉该条目；随后 `serverAssert` 断言类型只能是 `BLOCKED_STREAM`、`BLOCKED_LIST`、`BLOCKED_LIST_NONEMPTY`、`BLOCKED_ZSET`。接着 `unblockClient(c, 0)`——注释说明必须先解除阻塞再处理命令，免得 `processCommandAndResetClient` 看到 `CLIENT_BLOCKED` 就放弃执行。若客户端带 `CLIENT_PENDING_COMMAND`，清掉该位、置 `CLIENT_REEXECUTING_COMMAND`（`1ULL<<50`），把 `server.current_client` 换成它，进 `enterExecutionUnit(1, 0)` 后调 `processCommandAndResetClient`；返回 `C_ERR` 说明客户端在命令处理中被释放，直接 `exitExecutionUnit` 并恢复 `server.current_client` 后返回。命令跑完若客户端没有再阻塞：模块客户端走 `moduleCallCommandUnblockedHandler`，其余走 `queueClientForReprocessing`。之后 `exitExecutionUnit`、`afterCommand`、清 `CLIENT_REEXECUTING_COMMAND`、恢复 `server.current_client`。

**第六步：解除清理（`unblockClient` + `releaseBlockedEntry`）。** `unblockClient` 按 `btype` 分派：`BLOCKED_LIST`/`BLOCKED_LIST_NONEMPTY`/`BLOCKED_ZSET`/`BLOCKED_STREAM` 调 `unblockClientWaitingData`；`BLOCKED_WAIT`/`BLOCKED_WAITAOF` 调 `unblockClientWaitingReplicas`（定义在 `src/replication.c`）；`BLOCKED_MODULE` 在 `moduleClientIsBlockedOnKeys` 为真时先走 `unblockClientWaitingData`，再 `unblockClientFromModule`；`BLOCKED_POSTPONE`/`BLOCKED_POSTPONE_TRIM` `listDelNode` 掉 `server.postponed_clients` 上的节点；`BLOCKED_SHUTDOWN` 与 `BLOCKED_LAZYFREE` 不做额外清理；其它取值直接 `serverPanic`。分派完统一做：非模块客户端减 `server.blocked_clients`、任何类型减 `server.blocked_clients_by_type[btype]`、清 `CLIENT_BLOCKED`、`btype` 归 `BLOCKED_NONE`、清 `unblock_on_nokey`、`removeClientFromTimeoutTable`，最后按 `queue_for_reprocessing` 决定是否 `queueClientForReprocessing`。

`unblockClientWaitingData` 在 `dictSize(c->bstate.keys)` 为 0 时直接返回，否则遍历客户端 `keys` 字典逐项 `releaseBlockedEntry(c, de, 0)`，最后 `dictEmpty` 一次清空——注释解释为什么不在遍历里逐个删键：一个客户端可以等很多键，只要一个键触发就整体解除。

`releaseBlockedEntry` 从条目里取键与链表节点，`listUnlinkNode` 摘掉该客户端，再按链表是否为空分两种情况：空表就 `dictDelete` 掉 `blocking_keys` 与 `blocking_keys_unblock_on_nokey` 里该键；非空但该客户端带 `unblock_on_nokey` 时对 `blocking_keys_unblock_on_nokey` 里的计数做 `dictIncrUnsignedIntegerVal(..., -1)`，减到 0 再删条目。注释给出理由：同一个键上还有别的客户端在等，但它们不要求删除即唤醒，计数归零才是真的没人要求这种唤醒。入参 `remove_key` 为真才把键从 `c->bstate.keys` 删掉。

**第七步：超时。** `bstate.timeout` 是单调毫秒截止点，`addClientToTimeoutTable` 在 `timeout == 0` 时直接返回（无限等待不进表），否则按 `CLIENT_ST_KEYLEN`（定义在 `src/timeout.c`，值为 16）编码成 8 字节大端截止时间加 8 字节客户端 ID，用 `raxTryInsert` 插进 `server.clients_timeout_table` 并置 `CLIENT_IN_TO_TABLE`（`1ULL<<38`）。`handleBlockedClientsTimeout` 用 `getMonotonicUs() / 1000` 取当前毫秒，`raxSeek` 到最小键后顺序 `raxNext`，遇到 `timeout >= now` 就 `break`（后面的都在未来），否则清标志、调 `checkBlockedClientTimeout`、`raxRemove` 后重新 `raxSeek` 到最小键。`checkBlockedClientTimeout` 的条件是「客户端阻塞中且 `timeout != 0` 且 `timeout < now` 严格小于」，成立就调 `unblockClientOnTimeout`。`unblockClientOnTimeout` 先判模块客户端是否已在模块解除队列里（`isModuleClientUnblocked`），然后 `replyToBlockedClientTimedOut` 回包、清 `CLIENT_PENDING_COMMAND`（已经回过超时回包，不能再重放）、`unblockClient(c, 1)`。

`replyToBlockedClientTimedOut` 按类型回不同内容：`BLOCKED_LAZYFREE` 在命令处理函数是 `sflushCommand` 时回空数组、否则回 `shared.ok`；`BLOCKED_LIST`/`BLOCKED_LIST_NONEMPTY`/`BLOCKED_ZSET`/`BLOCKED_STREAM` 回 `addReplyNullArray` 并 `updateStatsOnUnblock`；`BLOCKED_WAIT` 回 `replicationCountAcksByOffset` 的结果；`BLOCKED_WAITAOF` 回两元素数组（`server.fsynced_reploff >= c->bstate.reploffset` 的布尔结果与 `replicationCountAOFAcksByOffset` 的结果）；`BLOCKED_MODULE` 走 `moduleBlockedClientTimedOut`；其它类型 `serverPanic`。

**第八步：外部强制解除。** 四条真实路径：

- `CLIENT UNBLOCK`：`clientCommand` 里按参数算出 `unblock_error`，`lookupClientByID` 拿到目标后要求「目标已阻塞且 `blockedClientMayTimeout` 为真」，然后按 `unblock_error` 分别调 `unblockClientOnError`（错误串 `-UNBLOCKED client unblocked via CLIENT UNBLOCK`）或 `unblockClientOnTimeout`，成功回 1、否则回 0。`blockedClientMayTimeout` 对 `BLOCKED_MODULE` 转调 `moduleBlockedClientMayTimeout`，对 `BLOCKED_LIST`/`BLOCKED_LIST_NONEMPTY`/`BLOCKED_ZSET`/`BLOCKED_STREAM`/`BLOCKED_WAIT`/`BLOCKED_WAITAOF` 返回 1，其余返回 0——注释说明没有超时回调的阻塞类型也不接受 CLIENT UNBLOCK。
- 主从角色切换：`replicationSetMaster` 调 `disconnectAllBlockedClients`，该函数跳过 `BLOCKED_POSTPONE`/`BLOCKED_POSTPONE_TRIM`，`BLOCKED_LAZYFREE` 单独回包后 `unblockClient(c, 1)`，其余走 `unblockClientOnError` 发 `-UNBLOCKED force unblock from blocking operation, instance state changed (master -> replica?)`，两类分支最后都置 `CLIENT_CLOSE_AFTER_REPLY`（`1<<6`）断开连接。
- 集群槽位迁移：`clientsCronHandleTimeout` 在 `server.cluster_enabled` 时调 `clusterRedirectBlockedClientIfNeeded`，返回真就 `unblockClientOnError(c, NULL)`（不额外回错误串）。
- 异步 FLUSH 完成：`blockClientForAsyncFlush` 在 `bstate.timeout = 0` 的前提下置 `CLIENT_PENDING_COMMAND` 并 `blockClient(c, BLOCKED_LAZYFREE)`；后台任务完成回调 `kvsAsyncFreeDoneCB` 调 `unblockClientForAsyncFlush`，后者校验「客户端仍存在且仍阻塞」后 `updateStatsOnUnblock`、回包、`unblockClient(c, 1)`，再按 `CLIENT_PENDING_COMMAND` 走 `commandProcessed` 补完复制偏移记账。
- SHUTDOWN：`replyToClientsBlockedOnShutdown` 在 `blocked_clients_by_type[BLOCKED_SHUTDOWN]` 为 0 时直接返回，否则遍历 `server.clients`，对阻塞在 `BLOCKED_SHUTDOWN` 的客户端回 `Errors trying to SHUTDOWN. Check logs.` 后 `unblockClient(c, 1)`。
- 客户端释放：`freeClient`（`src/networking.c`）在释放前若见 `CLIENT_BLOCKED` 就 `unblockClient(c, 1)`，随后 `dictRelease(c->bstate.keys)`。

**第九步：不允许阻塞时的降级。** 客户端带 `CLIENT_DENY_BLOCKING`（`1ULL<<41`）时不会进入本模块，各命令实现自己在原地返回。真实调用点是 `blockingPopGenericCommand`（`src/t_list.c`，回 `addReplyNullArray`）、`blmoveGenericCommand`（回 `addReplyNull`）、`blmovemGenericCommand`（回 `addReplyNullArray`）、`blockingGenericZpopCommand`（`src/t_zset.c`，回 `addReplyNullArray`）、`bzmoveCommand`（源键与目标键相同或带该标志时回 `addReplyNullArray`）、`xreadCommand`（`src/t_stream.c`，回 `addReplyNullArray` 后 `goto cleanup`）。`WAIT`/`WAITAOF` 侧在 `replication.c` 里以 `c->flags & CLIENT_DENY_BLOCKING` 为条件直接回当前 ACK 计数与 fsync 状态。这个标志由 `multi.c`（MULTI 内禁止阻塞）、`eval.c` 与 `functions.c`（Lua 客户端）、`aof.c`、`module.c` 设置。

**第十步：一轮内的收尾顺序。** `blockedBeforeSleep` 依次做：`handleBlockedClientsTimeout` 处理精确超时、`handleClaimableStreamEntries` 处理可领取的待定条目、`clients_waiting_acks` 非空时 `processClientsWaitingReplicas`、`handleClientsBlockedOnKeys`、有模块时 `moduleHandleBlockedClients`、`server.unblocked_clients` 非空时 `processUnblockedClients`。`processUnblockedClients` 逐个摘出节点、清 `CLIENT_UNBLOCKED`，无 `CLIENT_PENDING_COMMAND` 时先 `freeClientOriginalArgv` 与 `prepareForNextCommand`，模块客户端在仍未阻塞时走 `moduleCallCommandUnblockedHandler` 后 `continue`，其余在仍未阻塞时调 `processPendingCommandAndInputBuffer`，最后 `beforeNextClient`。

## 关键取舍

**源码事实：** `handleClientsBlockedOnKeys` 用静态变量关掉重入，入口断言 `server.also_propagate.numops == 0`，并且靠「换出新列表」的做法允许处理过程中继续追加就绪键。**源码事实：** `handleClientsBlockedOnKey` 用初始链表长度作为循环上界，并在处理前把客户端旋到尾部，注释把这套做法直接解释为两点：防止重放又阻塞造成死循环，以及保住其余等待者的相对次序。**源码事实：** 就绪键有两级去重（`db->ready_keys` 字典 + `server.ready_keys` 列表），消费时先删库内标记再处理。**源码事实：** `signalKeyAsReadyLogic` 里 `BLOCKED_LIST` 信号同时算 `BLOCKED_LIST` 与 `BLOCKED_LIST_NONEMPTY` 两个计数，而「列表变长」信号只算自己那一格且 `wake_modules` 为假，源码注释把这条写成对模块的既有契约：模块客户端只被 `RM_SignalKeyAsReady`、键新建、键删除或覆盖唤醒。**源码事实：** 超时只处理严格小于当前毫秒的截止点，`timeout` 为 0 的客户端不进 radix 树。**源码事实：** 计时用单调时间，`bstate.timeout` 的单位是毫秒，秒制入参在 `getTimeoutFromObjectOrReplyInternal` 里经 `ftval *= 1000.0` 与 `ceill` 换算后再加上当前时刻。

**架构推论：** 双向登记（客户端侧 `keys` 字典 + 库侧 `blocking_keys` 链表 + 链表节点回写进客户端字典）把「找到等这个键的所有人」和「摘掉某个等待者」都压到 O(1)，代价是两边的引用计数（键本身与 `blocking_keys_unblock_on_nokey` 的计数）都要手工配对，漏一处就是内存错误或误唤醒。**架构推论：** 用两级队列而不是遍历所有阻塞客户端，把每次写操作的成本从「阻塞客户端总数」降到「本命令实际触碰到的就绪键数」，代价是就绪信号与执行结果之间多了一次去重记账，写侧必须保证每个会产生可服务状态的路径都调了 `signalKeyAsReady*` 家族。**架构推论：** 阻塞客户端保持可读、只累积 query buffer，省掉了每次阻塞都重新装读事件的开销；代价是解除阻塞必须由 `queueClientForReprocessing` 主动补跑一轮，否则客户端会因为「没有新的可读事件」而卡住。**架构推论：** 按 FIFO 只在同类等待者内部成立；同一键上既有列表等待者又有有序集合等待者时，类型不匹配的客户端会被旋到另一侧，源码注释自己就把这处点名为公平性的破坏点。**架构推论：** 把「不允许阻塞」做成客户端标志并在各个命令实现里就地降级，而不是在 `blockForKeys` 里统一拦截，避免了错误语义从底层向上层渗透，代价是同一条降级分支要在每个阻塞命令里各写一遍，且返回值形态并不统一（有回空数组的，也有回 null 的）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/blocked.c` | `blockForKeys` | 在客户端与数据库两侧登记等待的键，设置超时与 `unblock_on_nokey`，最后调 `blockClient` |
| `src/blocked.c` | `blockClient` | 断言阻塞类型合法性，置 `CLIENT_BLOCKED`，累加 `blocked_clients` 与 `blocked_clients_by_type`，入超时树 |
| `src/blocked.c` | `unblockClient` | 按 `btype` 分派清理，撤销计数与标志，按需把客户端排进 `unblocked_clients` |
| `src/blocked.c` | `unblockClientWaitingData` | 遍历客户端 `keys` 逐项 `releaseBlockedEntry`，最后一次性清空字典 |
| `src/blocked.c` | `releaseBlockedEntry` | 摘链、清空链表时删键条目、`unblock_on_nokey` 计数递减至零时删条目 |
| `src/blocked.c` | `handleClientsBlockedOnKeys` | 关重入、断言传播状态、双层循环消费 `server.ready_keys` |
| `src/blocked.c` | `handleClientsBlockedOnKey` | 对单个就绪键按 FIFO 旋转链表并重放命令，`wake_modules` 控制是否唤醒模块客户端 |
| `src/blocked.c` | `unblockClientOnKey` | 摘掉该键登记、解除阻塞、置 `CLIENT_REEXECUTING_COMMAND` 后重放命令 |
| `src/blocked.c` | `signalKeyAsReadyLogic` | 按类型与有无人等待做快速返回，`db->ready_keys` 去重后入 `server.ready_keys` |
| `src/blocked.c` | `queueClientForReprocessing` | 置 `CLIENT_UNBLOCKED` 并挂到 `server.unblocked_clients` |
| `src/blocked.c` | `processUnblockedClients` | `beforeSleep` 内处理被解除客户端累积的输入缓冲 |
| `src/blocked.c` | `blockedBeforeSleep` | 一轮 `beforeSleep` 里阻塞相关动作的固定顺序 |
| `src/blocked.c` | `replyToBlockedClientTimedOut` | 按 `btype` 生成超时回包内容 |
| `src/blocked.c` | `unblockClientOnTimeout` | 回超时包、清 `CLIENT_PENDING_COMMAND`、`unblockClient(c, 1)` |
| `src/blocked.c` | `unblockClientOnError` | 可选错误串回包、`updateStatsOnUnblock`、`unblockClient(c, 1)` |
| `src/blocked.c` | `blockedClientMayTimeout` | 判定某阻塞类型是否接受超时/CLIENT UNBLOCK 式解除 |
| `src/blocked.c` | `disconnectAllBlockedClients` | 主从切换时发 `-UNBLOCKED` 并置 `CLIENT_CLOSE_AFTER_REPLY` |
| `src/blocked.c` | `blockForReplication` / `blockForAofFsync` | `WAIT`/`WAITAOF` 的登记入口，挂到 `server.clients_waiting_acks` |
| `src/blocked.c` | `blockPostponeClient` / `blockClientShutdown` | `CLIENT PAUSE` 与 `SHUTDOWN` 的阻塞入口 |
| `src/timeout.c` | `addClientToTimeoutTable` / `handleBlockedClientsTimeout` | 用 `CLIENT_ST_KEYLEN` 编码截止时间与客户端 ID，按 radix 树顺序到期解除 |
| `src/timeout.c` | `checkBlockedClientTimeout` | 严格小于当前毫秒才算超时，命中调 `unblockClientOnTimeout` |
| `src/timeout.c` | `getTimeoutFromObjectOrReplyInternal` | 秒/毫秒换算与溢出校验，把相对时长加上当前时刻 |
| `src/server.h` | `blocking_type` / `blockingState` / `readyList` | 阻塞类型枚举、客户端阻塞状态、就绪键节点结构 |
| `src/db.c` | `dbAddInternal` / `setKeyByLink` / `dbGenericDelete` / `dbSetValue` | 写侧发就绪信号的真实调用点 |
| `src/db.c` | `scanDatabaseForReadyKeys` / `scanDatabaseForDeletedKeys` | swapdb、flush 后按现有键补发就绪或删除信号 |
| `src/t_list.c` | `blockingPopGenericCommand` / `blmoveGenericCommand` / `blmovemGenericCommand` | 列表侧阻塞入口与 `CLIENT_DENY_BLOCKING` 降级 |
| `src/t_zset.c` | `blockingGenericZpopCommand` / `bzmoveCommand` | 有序集合侧阻塞入口与降级 |
| `src/t_stream.c` | `xreadCommand` / `handleClaimableStreamEntries` | 流阻塞入口（`unblock_on_nokey` 取 `xreadgroup`）与可领取条目监控 |
| `src/replication.c` | `unblockClientWaitingReplicas` / `processClientsWaitingReplicas` | `WAIT`/`WAITAOF` 的解除与就绪处理 |
| `src/networking.c` | `clientCommand`（CLIENT UNBLOCK 分支） | 按 `blockedClientMayTimeout` 校验后用错误或超时方式强制解除 |
| `src/networking.c` | `freeClient` | 释放客户端前解除阻塞并释放 `bstate.keys` |

## 与主核 / AI 的关系

Redis 的阻塞是「单线程事件循环里的协作式等待」：客户端不占线程，状态存在客户端自己身上，唤醒靠写侧显式打信号。主核若采用多工作者并发写入，「某个写操作触发了哪些等待者」必须由写路径自己判定并投递，不能像这里一样在命令结束时集中扫描一遍就绪键。双向登记表可以直接借用，但引用计数要在锁内维护，否则并发的登记与解除会互相踩。AI 侧的长时请求更不能直接套用这套模型：Redis 的超时靠 `beforeSleep` 里遍历单调时间 radix 树来保证精度，长任务一旦把事件循环占住，超时就整体延后；主核需要独立的取消通道，而不是把取消挂在同一个循环的每轮收尾上。

## 参考

- [Redis 源码：`src/blocked.c`](https://github.com/redis/redis/blob/unstable/src/blocked.c)
- [Redis 源码：`src/timeout.c`](https://github.com/redis/redis/blob/unstable/src/timeout.c)
- [Redis 命令文档：BLPOP](https://redis.io/docs/latest/commands/blpop/)
- [Redis 命令文档：CLIENT UNBLOCK](https://redis.io/docs/latest/commands/client-unblock/)
