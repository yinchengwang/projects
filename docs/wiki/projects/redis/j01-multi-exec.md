---
project: redis
feature: multi-exec
page_type: feature
schema_version: 1
refs:
  - path: src/multi.c
    symbols: [multiCommand, execCommand]
---

# 事务与命令排队

## 一句话职责

`multiCommand` 只把客户端标成 `CLIENT_MULTI` 并回 `+OK`，真正的排队发生在 `processCommand` 调 `queueMultiCommand` 时；`execCommand` 按入队顺序对每条命令逐条 `call()`，把它们的回复拼成一个定长数组返回。

## 机制是什么

一次 `MULTI`/`EXEC` 可以切成两段，两段落在不同文件的函数里：

- **入队段。** 客户端带 `CLIENT_MULTI` 之后，`processCommand` 的末尾把命令分流：命令不是 `EXEC`、`DISCARD`、`MULTI`、`WATCH`、`QUIT`、`RESET` 这六者之一时，调 `queueMultiCommand(c, cmd_flags)` 并回 `shared.queued`（`+QUEUED`）；否则照常 `call(c, CMD_CALL_FULL)` 立即执行。
- **执行段。** `execCommand` 先按 `c->mstate.count` 写定回复数组长度，再对 `j` 从 0 到 `count-1` 逐条把客户端当前的 `argc`/`argv`/`cmd` 换成 `c->mstate.commands[j]` 里保存的那一条，调 `call()`，然后把被命令实现改写过的 argv 写回队列，最后 `discardTransaction(c)` 清空队列。

本页只讲这两段与它们的状态搬运；EXEC 内部的原子性来自单线程事件循环（a01）与 `server.in_exec` 标记，命令本身的执行细节属于 a05，脚本路径上的类事务语义属于 j03。

## 怎么实现的

**入队的准入条件。** `processCommand` 的判定是一个六个命令的排除集：`c->cmd->proc != execCommand && != discardCommand && != multiCommand && != watchCommand && != quitCommand && != resetCommand`，全部条件成立才 `queueMultiCommand(c, cmd_flags)` 并 `addReply(c,shared.queued)`。所以 `QUIT` 与 `RESET` 在事务里仍然立即执行，不进队列。

**入队时的直接丢弃。** `queueMultiCommand` 第一句就是：`c->flags & (CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC)` 非零时 `return`。源码注释写明了动机：「No sense to waste memory if the transaction is already aborted… useful in case client sends these in a pipeline, or doesn't bother to read previous responses」。这条 `return` 发生在 `popPendingCommandFromHead` 之前，因此该命令既不进 `mstate`，也不计入 `count`，但仍回复 `+QUEUED`（回复由 `processCommand` 无条件发出）。

**容量增长。** `count == 0` 时申请两个指针槽位：`zmalloc(sizeof(pendingCommand*)*2)`，`alloc_count = 2`，注释说明「assuming it is used to execute at least two commands」。`count == alloc_count` 时按 `alloc_count < INT_MAX/2 ? alloc_count*2 : INT_MAX` 增长再 `zrealloc`。队列长度没有单独的硬上限常量，唯一的封顶就是这条 `INT_MAX/2` 判断之后的 `INT_MAX`。

**pendingCommand 的搬移。** 已解析的命令对象在 `c->pending_cmds` 链表里（`pendingCommand` 由 `addPendingCommand` 追加到尾部），入队时用 `popPendingCommandFromHead` 从头部摘出、指针存入 `mstate.commands + count`，并把 `c->current_pending_cmd` 置 `NULL`；随后 `count++`，并把 `c->argv`/`c->argc`/`c->argv_len` 清成 `NULL`/0/0，注释说明「we moved them into the mstate and shouldn't reference them from 'c' anymore」。该 `pendingCommand` 的 `argv_len_sum` 被搬进 `mstate.argv_len_sums` 并从 `c->all_argv_len_sum` 扣除，然后清零，注释说明之后不再通过 `all_argv_len_sum` 追踪它。

**标志聚合。** 每一次入队都做 `mstate.cmd_flags |= cmd_flags` 与 `mstate.cmd_inv_flags |= ~cmd_flags`。这两个字段被 `processCommand` 用来给 EXEC 判定准入：`is_read_command`、`is_write_command`、`is_denyoom_command`、`is_denystale_command`、`is_denyloading_command`、`is_may_replicate_command`、`is_deny_async_loading_command` 这七个量的判据都写成 `c->cmd->proc == execCommand && (c->mstate.cmd_flags & ...)` 或 `(c->mstate.cmd_inv_flags & ...)`。`cmd_flags` 是"队列里至少一条带此标志"，`cmd_inv_flags` 是"至少一条不带此标志"——因此 `cmd_inv_flags & CMD_STALE` 为真意味着队列里存在一条不容忍 stale 数据的命令。`src/cluster.c` 的 `is_write_command` 用同款写法读 `c->mstate.cmd_flags & CMD_WRITE`。

**`multiCommand` 与 `discardCommand` 的两个正面校验。** `multiCommand` 在已带 `CLIENT_MULTI` 时回 `"MULTI calls can not be nested"`，否则置位并回 `shared.ok`；`discardCommand` 在未带 `CLIENT_MULTI` 时回 `"DISCARD without MULTI"`，否则 `discardTransaction(c)` 后回 `shared.ok`。`discardTransaction` 做四件事：`freeClientMultiState`（逐条 `freePendingCommand` 再 `zfree` 数组）、`initClientMultiState`、清 `CLIENT_MULTI|CLIENT_DIRTY_CAS|CLIENT_DIRTY_EXEC`、`unwatchAllKeys(c)`。

**EXEC 的四道前置检查。** 顺序固定：

1. 未带 `CLIENT_MULTI` → 回 `"EXEC without MULTI"` 并返回。
2. `isWatchedKeyExpired(c)` 为真 → 置 `CLIENT_DIRTY_CAS`。该函数在 `c->watched_keys` 为空时直接返回 0；否则遍历，`wk->expired` 为真时 `continue`（WATCH 当时就已过期的键不算变更，注释写「was expired when WATCH was called」），其余用 `keyIsExpired` 命中即返回 1。
3. `CLIENT_DIRTY_CAS | CLIENT_DIRTY_EXEC` 任一为真 → 若带 `CLIENT_DIRTY_EXEC` 回 `shared.execaborterr`（`-EXECABORT Transaction discarded because of previous errors.`），否则回 `shared.nullarray[c->resp]`（RESP2 是 `*-1\r\n`，RESP3 是 `_\r\n`），然后 `discardTransaction(c)` 并返回。
4. 通过后：`old_flags = c->flags`；置 `CLIENT_DENY_BLOCKING`（注释「we do not want to allow blocking commands inside multi」）；`unwatchAllKeys(c)`（注释「Unwatch ASAP otherwise we'll waste CPU cycles」）；`server.in_exec = 1`；备份 `orig_argv`/`orig_argv_len`/`orig_argc`/`orig_cmd`/`orig_all_argv_len_sum`，并把 `c->all_argv_len_sum` 换成 `c->mstate.argv_len_sums`（注释说明多态队列的参数内存不通过 `all_argv_len_sum` 追踪，否则客户端结束时它回不到 0）。

**逐条执行的循环。** 数组长度在循环之前写定：`addReplyArrayLen(c,c->mstate.count)`。循环体内：

- 取参：`c->argc`/`c->argv`/`c->argv_len`/`c->cmd` 全部换成 `c->mstate.commands[j]` 的字段（`cmd` 同时赋给 `c->realcmd`）。
- ACL 复查：`skip_acl_check = server.loading && c->id == CLIENT_ID_AOF`；不跳过时调 `ACLCheckAllPerm(c,c->mstate.commands[j],&acl_errpos)`——传的是被排队的命令本身，注释说明「so that the key permissions are checked against its keys and not against the EXEC command we are currently running」。失败时按返回值选理由：`ACL_DENIED_CMD` → `"no permission to execute the command or subcommand"`，`ACL_DENIED_KEY` → `"no permission to touch the specified keys"`，`ACL_DENIED_CHANNEL` → `"no permission to access one of the channels used as arguments"`，其余 `"no permission"`；调 `addACLLogEntry(c,acl_retval,ACL_LOG_CTX_MULTI,acl_errpos,NULL,NULL)`，回 `-NOPERM ACLs rules changed between the moment the transaction was accumulated and the EXEC call…`，该条跳过 `call()`，数组元素就是这条错误。
- 执行：`c->mstate.executing_cmd = j` 后 `if (c->id == CLIENT_ID_AOF) call(c,CMD_CALL_NONE); else call(c,CMD_CALL_FULL)`——AOF 客户端在重放时不传播、不记统计。
- 断言 `(c->flags & CLIENT_BLOCKED) == 0`，这是 `CLIENT_DENY_BLOCKING` 的兜底。
- `server.fire_keyed_jobs_between_subcommands` 为真时调 `firePerKeyJobsBetweenSubcommands()`，让模块的 per-key post-notification 任务在子命令之间被排干；注释说明放在这里而不是 `afterCommand()` 上，是为了让普通命令不付这份代价。该标志位在 `src/module.c` 里被置 1、在 `firePerKeyJobsBetweenSubcommands` 末尾清 0。
- 回写：`c->mstate.commands[j]->argc/argv/argv_len/cmd` 重新赋成 `c->` 现值的，注释写「Commands may alter argc/argv, restore mstate」。

**收尾。** 循环之后：只在 `old_flags` 里没有 `CLIENT_DENY_BLOCKING` 时才清该位（注释「restore old DENY_BLOCKING value」）；恢复 `orig_argv`/`orig_argv_len`/`orig_argc`/`orig_cmd`（同时写回 `c->realcmd`）与 `orig_all_argv_len_sum`；`discardTransaction(c)`；最后 `server.in_exec = 0`。

**入队期错误如何变成 `CLIENT_DIRTY_EXEC`。** `flagTransaction(c)` 只在带 `CLIENT_MULTI` 时置 `CLIENT_DIRTY_EXEC`，因此在事务外调用它是空操作。真实调用方是 `rejectCommand` 与 `rejectCommandSds`：两者都先 `flagTransaction(c)`，随后判 `c->cmd && c->cmd->proc == execCommand`——成立就 `execCommandAbort(c, ...)`，不成立才 `addReplyErrorObject` / `addReplyErrorSds`。`execCommandAbort` 内部先 `discardTransaction(c)`，剥掉错误串开头的 `-`，再回 `-EXECABORT Transaction discarded because of: %s`；最后 `replicationFeedMonitors(c,server.monitors,c->db->id,c->argv,c->argc)`，注释说明 MULTI 已经发给 MONITOR 了、队列里的命令没发，所以要补一条 EXEC 让监控端看到事务结束。

**`CMD_NO_MULTI` 的拒绝。** `processCommand` 里：`c->flags & CLIENT_MULTI && c->cmd->flags & CMD_NO_MULTI` → `rejectCommandFormat(c,"Command not allowed inside a transaction")`，这条同样经 `rejectCommand` 家族标脏，使后续 EXEC 以 `-EXECABORT` 收场。`CMD_NO_MULTI` 的定义在 `src/server.h`。

**集群重定向对 EXEC 的差异处置。** `processCommand` 的 `getNodeByQuery` 分支里：目标不是自己时，`c->cmd->proc == execCommand` → `discardTransaction(c)`，否则 `flagTransaction(c)`，然后 `clusterRedirectClient`。同一段的豁免条件里 EXEC 也被显式排除在"无键命令不进重定向"之外。

**WATCH 侧的配合。** `watchCommand` 在带 `CLIENT_MULTI` 时回 `"WATCH inside MULTI is not allowed"`；已带 `CLIENT_DIRTY_CAS` 时直接回 `shared.ok` 而不挂键（注释「No point in watching if the client is already dirty」）。`touchWatchedKey` 命中被改的键时给所有观察者置 `CLIENT_DIRTY_CAS` 并立刻 `unwatchAllKeys(c)`，注释说明既然已经脏了就没必要保留到 EXEC、也不再让后来的修改重复扫到它。`touchAllWatchedKeysInDb` 处理 FLUSHDB/FLUSHALL/SWAPDB/槽位 trim：只在该键在旧库存在、或在新库存在时才标脏，并对 `wk->expired` 做补偿（过期键被删是逻辑无变化，于是清 `expired` 而非标脏）；函数注释说明这里不能像 `touchWatchedKey` 那样调 `unwatchAllKeys`，否则会释放迭代器持有的下一个节点。

**RESET 与连接复位。** `resetCommand` 调 `clearClientConnectionState`，后者内部 `discardTransaction(c)`；主从链路的 `replicationCacheMaster` 也在带 `CLIENT_MULTI` 时 `discardTransaction(c)`，把未执行的事务随缓存的主连接状态一起丢掉。

**AOF 重放路径。** `createAOFClient` 造的假客户端把 `flags` 直接置成 `CLIENT_DENY_BLOCKING`、`id` 置成 `CLIENT_ID_AOF`（`UINT64_MAX`）。`loadSingleAppendOnlyFile` 读到 `MULTI` 之后的命令时，因为 `queueMultiCommand` 要求一个 `pendingCommand`，就地 `zmalloc` 一个、`initPendingCommand`、`addPendingCommand` 后交给 `queueMultiCommand(fakeClient, cmd->flags)`；注释说明 AOF 路径不走 `processCommand`，所以不必算 `evalGetCommandFlags`。

**内存记账。** `multiStateMemOverhead(c)` 累加四项：`mstate.argv_len_sums`；`listLength(c->watched_keys) * (sizeof(listNode) + sizeof(watchedKey))` 再加 `c->watched_keys_mem`；`mstate.alloc_count * sizeof(pendingCommand*)`；`mstate.count * sizeof(pendingCommand)`。`getClientMemoryUsage` 调它，因此事务与 WATCH 的内存进客户端内存统计。

**执行期阻塞命令的退化路径。** 因为 `execCommand` 在整段执行期置了 `CLIENT_DENY_BLOCKING`，各阻塞命令实现的分支都会走"当作超时"的旁路：`src/t_list.c` 的 `blockingPopGenericCommand`（`BLPOP` / `BRPOP` 直接调它，`BLMPOP` 经 `lmpopGenericCommand` 转进来）与 `src/t_zset.c` 的 `blockingGenericZpopCommand`（`BZPOPMIN` / `BZPOPMAX` 直接调它，`BZMPOP` 经 `zmpopGenericCommand` 转进来）在 `CLIENT_DENY_BLOCKING` 命中时都 `addReplyNullArray`；`blmoveGenericCommand` 走 `addReplyNull`；`blmovemGenericCommand` 走 `addReplyNullArray`，源码注释写「Blocking is not allowed (e.g. inside MULTI/Lua): behave like a timeout and move nothing」。订阅侧相反：`subscribeCommand` 与 `psubscribeCommand` 判 `(c->flags & CLIENT_DENY_BLOCKING) && !(c->flags & CLIENT_MULTI)` 才报错，源码注释说明对 MULTI 保留特殊处理是为了向后兼容；`ssubscribeCommand` 不带这个例外，命中 `CLIENT_DENY_BLOCKING` 一律报 `"SSUBSCRIBE isn't allowed for a DENY BLOCKING client"`。

**事务期暂停的标记。** `pauseActions` 在 `server.in_exec` 为真时不立即生效，只置 `server.client_pause_in_transaction = 1`，注释说明是为了让已排队和随后到达的写命令照常执行、且不在 `propagateNow` 里断言失败；`call` 的尾部在 `!server.in_exec` 时才把它清 0（注释「Client pause takes effect after a transaction has finished. This needs to be located after everything is propagated.」），`propagateNow` 的断言正是用这个标记区分"事务内"与"真违约"。

## 关键取舍

**源码事实：** 原子性不来自锁也不来自回滚——`execCommand` 把整个队列放在一段连续的同步调用里跑完，`server.in_exec` 只是供其他模块查询的标记位（`RM_GetContextFlags` 据此给出 `REDISMODULE_CTX_FLAGS_MULTI`）；进程在 EXEC 中途退出时，已执行的部分不会撤销，这正是 `execCommand` 里没有任何 undo 路径的原因。

**源码事实：** 入队期错误与执行期错误处置不同——前者被压成 `CLIENT_DIRTY_EXEC` 这一个位，EXEC 时整队丢弃并回 `-EXECABORT`；后者只占回复数组里的一个元素，其余命令照跑，因为循环里 `call()` 之后没有跳出。

**源码事实：** 事务的准入判定在 `processCommand` 里复用命令级标志，靠 `mstate.cmd_flags` 与 `mstate.cmd_inv_flags` 把队列压成两个 int 字段；因此"一条写命令"就足以让整个 EXEC 在只读副本、maxmemory、min-replicas 等检查上按写命令处理。

**源码事实：** 队列容量从 2 起步、按 `INT_MAX/2` 封顶翻倍，源码里没有独立的队列长度上限常量；对事务体积的约束落在客户端内存统计（`multiStateMemOverhead`）与更外层的驱逐、缓冲限制上，二者都不在 `src/multi.c`。

**架构推论：** 把"排队"放在 `processCommand`、把"执行"放在 `execCommand`，使事务成为纯粹附着在客户端结构上的一段状态（`multiState` 的全部七个字段与 `CLIENT_MULTI` / `CLIENT_DIRTY_CAS` / `CLIENT_DIRTY_EXEC` 三个标志位），不需要服务端侧的全局事务表；代价是每条被排队的命令都要先走完整套预检，而预检看到的是未入队的单条命令，与执行期看到的事务整体并不等价——ACL 之所以要在循环里再查一遍，就是为了消掉这个缝隙。

**架构推论：** 用 `CLIENT_DENY_BLOCKING` 而不是"禁止阻塞命令入队"来处理阻塞语义，让 BLPOP 这类命令在事务里退化成一次立即返回的 null 而不报错；好处是"事务里的阻塞命令一定有同步结果"，代价是这个结果在协议上与真正的超时无法区分。

**架构推论：** `discardTransaction` 在 `execCommand` 末尾被无条件调用，把"跑完"与"清态"绑成一步；因此 `server.in_exec = 1` 之后的任何提前返回都必须自带 `discardTransaction`，否则客户端会卡在 `CLIENT_MULTI` 上；`in_exec` 之前的提前返回有两处，`!CLIENT_MULTI` 那处客户端本就未进事务、无需清态，脏标志分支那处自带 `discardTransaction`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/multi.c` | `multiCommand` | 置 `CLIENT_MULTI` 并回 `+OK`；嵌套 MULTI 直接报错 |
| `src/multi.c` | `queueMultiCommand` | 把 `pendingCommand` 从解析队列搬进 `mstate.commands`，聚合 `cmd_flags`/`cmd_inv_flags` 与 `argv_len_sums`，按 2 起步翻倍扩容 |
| `src/multi.c` | `execCommand` | 写定回复数组长度、逐条 ACL 复查 + `call()`、回写 argv、清态并复位 `server.in_exec` |
| `src/multi.c` | `discardTransaction` | 释放队列、重置 `multiState`、清三个标志位、`unwatchAllKeys` |
| `src/multi.c` | `flagTransaction` | 仅在 `CLIENT_MULTI` 时置 `CLIENT_DIRTY_EXEC` |
| `src/multi.c` | `execCommandAbort` | 回 `-EXECABORT` 并向 MONITOR 补发 EXEC |
| `src/multi.c` | `isWatchedKeyExpired` | 把"WATCH 之后被过期"的键转成 `CLIENT_DIRTY_CAS` |
| `src/multi.c` | `multiStateMemOverhead` | 队列与 WATCH 的内存记账，供 `getClientMemoryUsage` 使用 |
| `src/server.c` | `processCommand` | 判定入队还是立即执行；`CMD_NO_MULTI` 拒绝；集群重定向时对 EXEC 直接 `discardTransaction` |
| `src/server.c` | `rejectCommand` | 入队期错误的统一入口，转 `DIRTY_EXEC` 或对 EXEC 走 `execCommandAbort` |
| `src/server.c` | `call` | 被 `execCommand` 逐条调用；EXEC 自身不显式传播 |
| `src/server.c` | `propagatePendingCommands` | 多条待传播操作被包成一对 `shared.multi` / `shared.exec` |
| `src/server.h` | `CLIENT_DENY_BLOCKING` | 执行期禁止阻塞的标志位，`1ULL<<41` |
| `src/server.h` | `multiState` | `commands` 数组、`count`、`cmd_flags`、`cmd_inv_flags`、`argv_len_sums`、`alloc_count`、`executing_cmd` |
| `src/aof.c` | `loadSingleAppendOnlyFile` | 重放时临时造 `pendingCommand` 交给 `queueMultiCommand` |

## 与主核 / AI 的关系

`multi.c` 给出的是一份"延迟执行队列"的最小实现：队列元素是指向已解析命令的指针，状态挂在客户端上，失败用两个位区分"入队期坏"与"执行期坏"。主核若要多写者事务，可直接借走这套状态机与位语义，但有两处必须换掉：`execCommand` 的顺序执行依赖单线程事件循环，主核需要真正的提交边界；`CLIENT_DENY_BLOCKING` 这种"全局置位、阻塞命令各自解释"的做法在并发下会变成跨线程可见的共享状态，得改成显式的事务上下文。AI 侧把 Redis 当事务型缓存时，这里的事实决定了两条工程结论：队列里的命令在执行期还要过一次 ACL，权限变更会以「单元素错误」的形式出现；`cmd_flags` 聚合让"事务里混了一条写命令"改变整个 EXEC 的准入判定，客户端在只读副本或接近 maxmemory 时的失败会以 EXEC 整体被拒的形式出现，而不是逐条失败。

## 参考

- [Redis 命令：MULTI](https://redis.io/docs/latest/commands/multi/) —— `multiCommand` 的对外语义、`+OK` 回复与嵌套限制。
- [Redis 命令：EXEC](https://redis.io/docs/latest/commands/exec/) —— `execCommand` 的两种回复形态（数组 / null）与 `EXEC` 的复杂度描述。
- [Redis 文档：Transactions](https://redis.io/docs/latest/develop/using-commands/transactions/) —— 入队期错误与执行期错误的区分、无回滚的取舍、WATCH 的 CAS 语义。
- [Redis 源码：src/multi.c](https://github.com/redis/redis/blob/unstable/src/multi.c) —— 队列搬移、EXEC 循环、WATCH 链表与内存记账的实际实现。
