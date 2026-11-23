---
project: redis
feature: scripting-eval
page_type: feature
schema_version: 1
refs:
  - path: src/eval.c
    symbols: [evalGenericCommand, scriptCommand]
---

# 脚本执行

## 一句话职责

`evalGenericCommand` 是 `EVAL` / `EVALSHA` 两个命令的共同执行体：它把脚本体算成 `f_<40 位十六进制 SHA1>` 的函数名，在 Lua registry 里查这个函数，缺失时按 `evalsha` 参数分流（`EVAL` 走 `luaCreateFunction` 现场编译，`EVALSHA` 直接回 `-NOSCRIPT`），然后 `scriptPrepareForRun` 建运行上下文、`luaCallFunction` 下栈调用、`scriptResetRun` 收尾。解释器生命周期由 `scriptingInit` / `scriptingRelease` / `scriptingReset` 负责，缓存增删由 `luaCreateFunction` / `luaDeleteFunction` / `luaScriptsLRUAdd` 负责，调试会话由 `ldb*` 一族负责；`scriptCommand` 只是 `SCRIPT` 的子命令分发器，本身不执行脚本。

## 机制是什么

Redis 的脚本执行模型有三条互相独立的轴：

1. **解释器是全局单例。** `struct luaCtx` 里只有 `lua_State *lua` 一个解释器，源码注释直接写明「We use just one for all clients」。伴随它的是一个**非连接客户端** `lua_client`：`scriptingInit` 用 `createClient(NULL)` 造它，置上 `CLIENT_SCRIPT`，再置 `CLIENT_DENY_BLOCKING`——脚本里调用的每条 Redis 命令都借这个假客户端执行，因此阻塞类命令在脚本内被这一标志挡掉。
2. **脚本缓存是「SHA1 → 脚本对象」的字典。** 键是 40 字符十六进制 SHA1（`sds`），值是 `luaScript`（`flags`、`body`、LRU 链节点）。字典类型 `shaScriptObjectDictType` 用 `dictStrCaseHash` + `dictSdsKeyCaseCompare`，即键比较**大小写不敏感**，所以 `EVALSHA` 传大写 SHA 也能命中。
3. **脚本不被持久化。** 缓存只存在于进程内存里：`EVALSHA` 命中失败就回 `shared.noscripterr`，字面量在 `createSharedObjects` 里是 `-NOSCRIPT No matching script. Please use EVAL.`。源码里没有任何把脚本写进 RDB / AOF 的路径——本仓的复制改为「复制脚本产生的写命令效果」，所以缓存是可以随时丢的。

缓存淘汰是**只针对 `EVAL` 的单向 FIFO**，不看命中频率：`luaScriptsLRUAdd` 在 `evalsha` 非零时立刻 `return NULL`（`SCRIPT LOAD` 载入的脚本永远不会被淘汰），否则只要 `listLength(lctx.lua_scripts_lru_list) >= LRU_LIST_LENGTH` 就从表头摘、调 `luaDeleteFunction`、`server.stat_evictedscripts++`。`LRU_LIST_LENGTH` 在 `src/eval.c` 里定义为 **500**。真正的「最近使用」只体现在执行侧：`evalGenericCommand` 在调用前把该脚本的链节点 `listUnlinkNode` 再 `listLinkNodeTail`，把命中项挪到队尾。

两条命令路径与两个 `_RO` 变体之间的差异是刻意做薄的：

- `evalCommand` 先 `replicationFeedMonitors`（注释说明这是为了让 MONITOR 里脚本产生的命令排在 `EVAL` 之后），再按 `CLIENT_LUA_DEBUG` 分叉：无该标志走 `evalGenericCommand(c,0)`，有则走 `evalGenericCommandWithDebugging(c,0)`。`evalRoCommand` 原样转调 `evalCommand`，只读约束由命令表 `READONLY` / `evalsha_ro.json` 的 `READONLY` 标志和 `scriptPrepareForRun` 的 `ro` 参数承担。
- `evalShaCommand` 先判 `sdslen(c->argv[1]->ptr) != 40`：长度不对直接 `addReplyErrorObject(c, shared.noscripterr)`，注释说明这是为了让 `evalGenericCommand` 内部**不必再做长度校验**。随后同样按 `CLIENT_LUA_DEBUG` 分叉，但调试态下拒绝执行，回 `"Please use EVAL instead of EVALSHA for debugging"`。

## 怎么实现的

### 一、解释器与缓存的初始化

`scriptingInit(int setup)` 是唯一入口，`initServer` 在 `luaEnvInit()` 之后以 `setup = 1` 调它。`setup` 为真时先把 `lctx.lua_client` 置 `NULL`、`server.script_disable_deny_script` 置 0、`ldbInit()`。随后：

1. `createLuaState()` 建解释器，失败就 `serverLog(LL_WARNING, "Failed creating the lua VM.")` 并 `exit(1)`——不做降级。
2. `dictCreate(&shaScriptObjectDictType)` 建脚本字典、`listCreate()` 建 LRU 链、`lua_scripts_mem = 0`。
3. `luaRegisterRedisAPI(lua)` 挂上 `redis.call` / `redis.pcall` 等，再手工把 `redis.breakpoint` / `redis.debug` / `redis.replicate_commands` 三个调试/兼容函数 `lua_settable` 进 `redis` 表。其中 `luaRedisReplicateCommandsCommand` 已废弃：函数体只 `lua_pushboolean(lua,1)` 恒返回 true。
4. 编译一段内联 Lua 定义 `__redis__err__handler`——它 `debug.getinfo(2,'nSl')` 取调用者位置、C 函数帧则退到 `getinfo(3)`，把非 table 的错误包成 `{err='ERR ' .. tostring(err)}`，再补 `source` / `line`。这就是「错误包装」的产出形态，后续 `luaCallFunction` 只按这张表拼回复。
5. `lctx.lua_client == NULL` 时才建假客户端（注释：`scriptingReset()` 复用时不重建）。
6. `lua_pushvalue(lua, LUA_GLOBALSINDEX)` 后 `luaSetErrorMetatable` + `luaSetTableProtectionRecursively`，再 `luaSetTableProtectionForBasicTypes`——把全局表与基本类型元表锁成只读，脚本写全局变量会直接报错。
7. 最后 `lctx.lua = lua`。

`scriptingReset(int async)` = `scriptingRelease(async)` + `scriptingInit(0)`。`scriptingRelease` 按 `async` 二选一：真则 `freeLuaScriptsAsync`，假则 `freeLuaScriptsSync`（后者 `dictRelease` + `listRelease` + `lua_gc(LUA_GCCOLLECT)` + `lua_close`）。`freeLuaScriptsAsync` 在 `src/lazyfree.c`，判据是 `dictSize(lua_scripts) > LAZYFREE_THRESHOLD`（该文件定义为 **64**）：超过就 `bioCreateLazyFreeJob` 丢给后台线程，否则**回退到 `freeLuaScriptsSync` 同步释放**——这是脚本子系统里唯一的一处真实降级路径。

### 二、函数名与 shebang 解析

`evalCalcFunctionName(int evalsha, sds script, char *out_funcname)` 产出 43 字节缓冲（`f`、`_`、40 字符 SHA1、结尾 NUL）：`EVAL` 分支用 `sha1hex` 现算；`EVALSHA` 分支逐字符把 A–Z 映射成小写（注释说明特意不用 `tolower`，因为它在 profiler 里开销可观）。

`evalExtractShebangFlags` 处理脚本首行 `#!`：`strncmp(body, "#!", 2)` 命中才进解析。失败分支共五种，都返回 `C_ERR` 并由调用方把 `err` 写回客户端：找不到 `\n`（`"Invalid script shebang"`）、`sdssplitargs` 得 0 段（`"Invalid engine in script shebang"`）、任一段含 `\r` 或 `\n`（`"Invalid symbols in script shebang"`，源码注释说明这是防 `sdssplitargs` 解码转义后把换行带进错误回复）、首段不是 `#!lua`（`"Unexpected engine in script shebang: %s"`）、`flags=` 里出现表外项或出现非 `flags=` 选项（`"Unexpected flag in script shebang: %s"` / `"Unknown lua shebang option: %s"`）。无 shebang 时 `script_flags` 保持 `SCRIPT_FLAG_EVAL_COMPAT_MODE`，即兼容模式。`flags=` 的合法取值来自 `src/script.c` 的 `scripts_flags_def[]`：`no-writes`、`allow-oom`、`allow-stale`、`no-cluster`、`allow-cross-slot-keys`，以 `{flag = 0, .str = NULL}` 收尾。

### 三、编译与入库（`luaCreateFunction`）

顺序是固定的：

1. `sha1hex(funcname+2, body->ptr, sdslen(body->ptr))`，随后 `dictFind(lctx.lua_scripts, funcname+2)`——**已存在就直接 `return dictGetKey(de)`**，即幂等，不重编译。
2. `evalExtractShebangFlags`：出错则 `addReplyErrorSds(c, err)` 并 `return NULL`。
3. `luaL_loadbuffer(lctx.lua, body->ptr + shebang_len, sdslen(body) - shebang_len, "@user_script")`——**跳过 shebang 但保留行尾换行**，注释写明是为了不打乱用户脚本的行号。编译失败回 `"Error compiling script (new function): %s"`，`lua_pop` 后 `luaGC`，返回 `NULL`。
4. `serverAssert(lua_isfunction(...))`，`lua_setfield(LUA_REGISTRYINDEX, funcname)` 把函数存进 registry。
5. `zcalloc` 出 `luaScript`，填 `body` 与 `flags`；`sdsnewlen(funcname+2,40)` 造 SHA 键；`l->node = luaScriptsLRUAdd(c, sha, evalsha)`；`dictAdd` 后 `lctx.lua_scripts_mem += sdsZmallocSize(sha) + getStringObjectSdsUsedMemory(body)`；`incrRefCount(body)`。
6. 最后再 `luaGC`——注释解释「放在 LRU 插入之后，因为脚本会在插入期间被淘汰」。

`luaDeleteFunction` 是逆操作：先在 registry 里把 `f_<sha>` 置 nil，再 `dictUnlink` + `listDelNode` + `serverAssert(l->node)` + `dictFreeUnlinkedEntry`，并减去对应内存。注释明确「只删 `EVAL` 脚本，它们一定在 LRU 链里」。调用方只有两处：`luaScriptsLRUAdd` 的淘汰循环，以及它自己的声明。

### 四、执行序列（`evalGenericCommand`）

1. `getLongLongFromObjectOrReply` 解析 `numkeys`；`numkeys > (c->argc - 3)` 回 `"Number of keys can't be greater than number of args"`，`numkeys < 0` 回 `"Number of keys can't be negative"`。
2. 取函数名：`c->cur_script` 非空（`evalGetCommandFlags` 在 `processCommand` 阶段已经算好并缓存进客户端）时直接从 `dictGetKey(c->cur_script)` `memcpy` 出 40 字节；否则调 `evalCalcFunctionName`。
3. `lua_getglobal(lua, "__redis__err__handler")` 压入错误处理器，`lua_getfield(lua, LUA_REGISTRYINDEX, funcname)` 查函数。`lua_isnil` 时：`evalsha` 为真就弹掉两层栈、`addReplyErrorObject(c, shared.noscripterr)` 返回；否则 `luaCreateFunction(c, c->argv[1], evalsha)`，返回 `NULL` 时注释说明「错误已由 `luaCreateFunction` 自己发给客户端」，直接返回；成功则重新 `lua_getfield` 取函数并 `serverAssert(!lua_isnil(...))`。
4. `ro = (c->cmd->proc == evalRoCommand || c->cmd->proc == evalShaRoCommand)`。
5. `scriptPrepareForRun(&rctx, lctx.lua_client, c, lua_cur_script, l->flags, ro)`——非 `C_OK` 时 `lua_pop(lua,2)` 清理栈后返回（错误回复已由该函数写出）。
6. `rctx.flags |= SCRIPT_EVAL_MODE`，注释说明这是为了让 FCALL 与 EVAL 的错误文案与日志能区分。
7. 若 `l->node` 非空，`listUnlinkNode` + `listLinkNodeTail` 挪到 LRU 队尾。
8. `luaCallFunction(&rctx, lua, c->argv+3, numkeys, c->argv+3+numkeys, c->argc-3-numkeys, ldb.active)`。
9. `lua_pop(lua,1)` 弹错误处理器，`scriptResetRun(&rctx)`，`luaGC(lua, &gc_count)`。

源码在该函数末尾留了一段注释：之后**不能再碰 `l`**，因为 `activedefrag` 会在长脚本的 AOF 载入期间把它搬走。

### 五、运行上下文与守卫（`scriptPrepareForRun`）

`scriptPrepareForRun` 开头 `serverAssert(!curr_run_ctx)`——同一时刻只允许一个脚本在跑。先算四个外部状态：`client_allow_oom`（调用方带 `CLIENT_ALLOW_OOM`）、`running_stale`（有主、复制断开、且 `repl_serve_stale_data == 0`）、`obey_client = mustObeyClient(caller)`。

非兼容模式（带 shebang）的检查按顺序返回：

- `SCRIPT_FLAG_NO_CLUSTER` 且集群开启 → `"Can not run script on cluster, 'no-cluster' flag is set."`；随后**单独**再判一次该标志并 `server.stat_cluster_incompatible_ops++`。
- `running_stale` 且无 `SCRIPT_FLAG_ALLOW_STALE` → `-MASTERDOWN`。
- 无 `SCRIPT_FLAG_NO_WRITES`（即脚本声明了可写）时四连判：只读副本（`-READONLY Can not run script with write flag on readonly replica`）、`writeCommandsDeniedByDiskError()` 非 `NONE`（按 RDB / AOF 分别给两条 `-MISCONF`，AOF 那条额外拼 `strerror(server.aof_last_write_errno)`）、`ro`（`"Can not execute a script with write flag using *_ro command."`）、`checkGoodReplicasStatus()` 失败（`shared.noreplicaserr`）。
- OOM 判据是 `!client_allow_oom && server.pre_command_oom_state && server.maxmemory && !(SCRIPT_FLAG_ALLOW_OOM|SCRIPT_FLAG_NO_WRITES)` → `-OOM allow-oom flag is not set on the script, ...`。

兼容模式（无 shebang）只判 `running_stale` → `shared.masterdownerr`，其余检查全部跳过——这是 `EVAL` 的历史行为得以保持的原因。

通过后写上下文：`run_ctx->c = engine_client`、`original_client = caller`、`funcname`、`slot`、`cluster_compatibility_check_slot`；`selectDb(script_client, curr_client->db->id)`；`script_client->resp = 2`（注释：脚本可以自行改）；调用方在 `MULTI` 里则给假客户端也置 `CLIENT_MULTI`；`start_time = getMonotonicUs()`；`repl_flags = PROPAGATE_AOF | PROPAGATE_REPL`。随后的三个 run flag 是：

- `SCRIPT_READ_ONLY`：`ro` 为真，或非兼容模式且带 `SCRIPT_FLAG_NO_WRITES`。
- `SCRIPT_ALLOW_OOM`：`client_allow_oom` 为真，或非兼容模式且带 `SCRIPT_FLAG_ALLOW_OOM`。注释说明不必叠加 `no-writes`，因为只有写命令才 deny-oom。
- `SCRIPT_ALLOW_CROSS_SLOT`：兼容模式，或带 `SCRIPT_FLAG_ALLOW_CROSS_SLOT`。

最后 `curr_run_ctx = run_ctx` 并返回 `C_OK`。`scriptResetRun` 对称收尾：清 `CLIENT_MULTI`、`himportFieldsetsFree`、超时则 `exitScriptTimedoutMode` + `unprotectClient`、`slot = -1` / `cluster_compatibility_check_slot = -2`、`preventCommandPropagation`、`curr_run_ctx = NULL`。

### 六、命令层按 `scriptCall` 逐条验

`luaCallFunction` 在 `src/script_lua.c`：先 `luaSaveOnRegistry(lua, REGISTRY_RUN_CTX_NAME, run_ctx)`（注释说明必须在设 hook 之前，否则 hook 里取不到上下文）；`server.busy_reply_threshold > 0 && !debug_enabled` 时 `lua_sethook(lua, luaMaskCountHook, LUA_MASKCOUNT, 100000)`；注入 `KEYS` / `ARGV` 两张全局表时**临时解除全局表只读**（`lua_enablereadonlytable(..., 0)` 赋值后再置回 1）；然后按 `SCRIPT_EVAL_MODE` 分叉 `lua_pcall`——EVAL 模式 0 参 1 返回、错误处理器在 -2，函数模式 2 参 1 返回、处理器在 -4。返回后错误分支按前面那张错误表的形态拼回复：非 table 用 `"Error running script %s, %.100s\n"`（`%.100s` 截断），table 则 `"-%s"` 加 `" script: %s, on %s:%s."`；收尾 `lua_sethook(lua,NULL,0,0)` 并在 registry 里把 run ctx 置空。

脚本里每调一条 Redis 命令都进 `scriptCall`，检查顺序是：`serverAssert(clientTotalPubSubSubscriptionCount(c) == 0)` + `c->user = run_ctx->original_client->user` → `moduleCallCommandFilters` → `lookupCommand` → `scriptVerifyCommandArity`（参数个数不符回 `"Wrong number of args calling Redis command from script"`，命令未知回 `"Unknown Redis command called from script"`）→ `CMD_NOSCRIPT` 且 `!server.script_disable_deny_script` 回 `"This Redis command is not allowed from script"` → `scriptVerifyAllowStale`（`"Can not execute the command on a stale replica"`）→ `scriptVerifyACL` → `scriptVerifyWriteCommandAllow`（`"Write commands are not allowed from read-only scripts."`，判据是 `SCRIPT_READ_ONLY` 且命令带 `CMD_WRITE|CMD_MAY_REPLICATE`）→ `scriptVerifyOOM` → 带 `CMD_WRITE` 则置 `SCRIPT_WRITE_DIRTY` → `scriptVerifyClusterState` → `scriptCheckClusterCompatibility` → 按 `repl_flags` 拼 `CMD_CALL_PROPAGATE_AOF|CMD_CALL_PROPAGATE_REPL` 后 `call(c, call_flags)` → `serverAssert((c->flags & CLIENT_BLOCKED) == 0)`。失败统一走 `goto error`：`afterErrorReply` + `incrCommandStatsOnError(cmd, ERROR_COMMAND_REJECTED)`。

### 七、超时与中断

`luaMaskCountHook` 每 10 万条 Lua 指令被打一次，调 `scriptInterrupt`。`scriptInterrupt` 的逻辑是：已 `SCRIPT_TIMEDOUT` 就只 `processEventsWhileBlocked()`，按 `SCRIPT_KILLED` 返回 `SCRIPT_KILL` / `SCRIPT_CONTINUE`；否则算 `elapsedMs(start_time)`，小于 `server.busy_reply_threshold` 直接 `SCRIPT_CONTINUE`；超过就打日志 `"Slow script detected: still in execution after %lld milliseconds."`（文案里的命令名按 `SCRIPT_EVAL_MODE` 在 `SCRIPT KILL` / `FUNCTION KILL` 之间选），`enterScriptTimedoutMode` 置标志并 `blockingOperationStarts()`，再 `protectClient(run_ctx->original_client)`——注释解释必须屏蔽发脚本的客户端，否则它中途断开会让它「在 `EVAL` 返回时已经不在了」，最后再 `processEventsWhileBlocked()`。`busy_reply_threshold` 的默认值 **5000**（毫秒）来自 `src/config.c` 的 `createLongLongConfig("busy-reply-threshold", "lua-time-limit", ...)`，旧名 `lua-time-limit` 作为别名保留。

`scriptKill(client *c, int is_eval)` 的拒绝分支按顺序是：无 `curr_run_ctx` → `-NOTBUSY No scripts in execution right now.`；`mustObeyClient(original_client)` → `-UNKILLABLE ... sent by a master instance ...`；已 `SCRIPT_WRITE_DIRTY` → `-UNKILLABLE ... already executed write commands ... SHUTDOWN NOSAVE`；`is_eval` 但当前不是 EVAL 模式 → `shared.slowscripterr`；`!is_eval` 但当前是 EVAL 模式 → `shared.slowevalerr`。全过才 `SCRIPT_KILLED` 并回 `shared.ok`。`scriptCommand` 的 `kill` 子命令传 `is_eval = 1`。

### 八、`scriptCommand` 的子命令面

`SCRIPT` 本身不执行任何脚本。只认 5 组子命令，其余 `addReplySubcommandSyntaxError`：

- `help`：`addReplyHelp` 列出 `DEBUG` / `EXISTS` / `FLUSH` / `KILL` / `LOAD`，其中 `FLUSH` 的帮助文案写明「未指定模式时行为由 `lazyfree-lazy-user-flush` 决定」，该配置在 `src/config.c` 里默认 **0**。
- `flush`：三参 `sync` → `async = 0`、三参 `async` → `async = 1`、两参 → `async = server.lazyfree_lazy_user_flush ? 1 : 0`，其他组合回 `"SCRIPT FLUSH only support SYNC|ASYNC option"`。然后 `scriptingReset(async)` 并回 OK。
- `exists`：`addReplyArrayLen(c, c->argc-2)` 后逐个 `dictFind(lctx.lua_scripts, ...)`，命中 `shared.cone`、否则 `shared.czero`。
- `load`：`luaCreateFunction(c, c->argv[2], 1)`——注意这里 `evalsha` 传 **1**，正是这个值让 `luaScriptsLRUAdd` 直接返回 `NULL`，使 `SCRIPT LOAD` 的脚本不受 500 条 FIFO 淘汰。成功则 `addReplyBulkCBuffer(c, sha, 40)`。
- `debug`：先 `clientHasPendingReplies(c)` 判管道，命中回 `"SCRIPT DEBUG must be called outside a pipeline"`；`no` / `yes` / `sync` 三态分别走 `ldbDisable` / `ldbEnable`（`sync` 额外 `c->flags |= CLIENT_LUA_DEBUG_SYNC`），其余回 `"Use SCRIPT DEBUG YES/SYNC/NO"`。

### 九、调试会话（LDB）

`evalGenericCommandWithDebugging` 是薄壳：`ldbStartSession(c)` 返回 1 才真正执行 `evalGenericCommand` 再 `ldbEndSession(c)`；返回 0（父进程侧或 `fork` 失败）则只 `ldbDisable(c)`。

`ldbStartSession` 先定 `ldb.forked = (c->flags & CLIENT_LUA_DEBUG_SYNC) == 0`——**默认就是 fork 模式**，`SCRIPT DEBUG SYNC` 才就地执行。fork 分支调 `redisFork(CHILD_TYPE_LDB)`：失败回 `"Fork() failed: can't run EVAL in debugging mode: %s"` 返回 0；子进程忽略 `SIGTERM` / `SIGINT` 继续；父进程把 pid 记进 `ldb.children` 后 `freeClientAsync(c)` 返回 0。之后 `connBlock(ldb.conn)`、`connSendTimeout(ldb.conn, 5000)`、`ldb.active = 1`，把 `c->argv[1]` 复制后裁掉尾部连续的 `\n` / `\r`，再按 `"\n"` `sdssplitlen` 成 `ldb.src`。

断点容量是 `LDB_BREAKPOINTS_MAX`（`src/eval.c` 定义 **64**），`ldbAddBreakpoint` 在 `line <= 0 || line > ldb.lines` 或已满时返回 0。日志截断用 `ldb.maxlen`，初值 `LDB_MAX_LEN_DEFAULT`（**256**），`ldbLogWithMaxLen` 超长就 `sdsrange` 到 `maxlen-1` 再接 `" ..."`，并只在首次截断时追加一条提示 `<hint> The above reply was trimmed. Use 'maxlen 0' to disable trimming.`。`ldbRepl` 的协议解析上限写死在函数体里：`*argcp <= 0 || *argcp > 1024` 或单参 `slen <= 0 || slen > 1024` 即 `protoerr`，累计缓冲超过 `1<<20`（1 MB）就清空并 `luaError("max client buffer reached")`；读不到数据（`connRead` 返回 <= 0）时把 `ldb.step = 0`、`ldb.bpcount = 0` 并返回 `C_ERR`，让脚本无输入地跑完。

`luaLdbLineHook` 是每行的实际断点判定：先从 registry 取 run ctx，`strstr(ar->short_src, "user_script") == NULL` 的行直接跳过（只管用户脚本）；`LUA_HOOKCOUNT` 且既没 step 也没断点时按 `server.busy_reply_threshold ? server.busy_reply_threshold : 5000` 判超时，超了才 `timeout = 1` 并强制 `step = 1`；停下时记 `"* Stopped at %d, stop reason = %s"`，原因在 `redis.breakpoint() called` / `break point` / `step over` / `timeout reached, infinite loop?` 之间选，然后 `ldbRepl`，返回 `C_ERR` 且是超时导致的就 `luaPushError("timeout during Lua debugging with client closing connection")` 并 `luaError`，最后重置 `start_time`。

### 十、与命令调度层、可观测面的接缝

- `src/server.c` 的 `getCommandFlags(c)` 在 `processCommand` 里被调用；当 `c->cmd->proc` 是四个 EVAL/EVALSHA 函数之一时转调 `evalGetCommandFlags`，后者先算 `evalsha`、长度不是 40 就原样返回命令表标志，再 `evalCalcFunctionName` 并 `dictFind` 到 `c->cur_script`；命中则用 `l->flags`，未命中且是 `EVAL` 才现场 `evalExtractShebangFlags`；最后 compat 模式返回原标志，否则 `scriptFlagsToCmdFlags`——后者清 `CMD_STALE|CMD_DENYOOM|CMD_WRITE`、按 `no-writes` / `allow-oom` / `allow-stale` 重设，并一律 `&= ~CMD_MAY_REPLICATE`。这条路径的价值在于**执行前的 OOM / 只读 / stale 判定就能按脚本声明的标志做**，而不是等进了 `scriptPrepareForRun`。
- `c->cur_script` 由 `resetClientInternal` 清空（与 `argv` 释放同一处），也由 `createClient` 初始化成 `NULL`——因此缓存只在单条命令的生命周期内有效。
- 内存口径：`evalScriptsMemoryVM` 返回 `luaMemory(lctx.lua)`（即 `lua_gc(LUA_GCCOUNT, 0) * 1024`），`evalScriptsMemoryEngine` 返回 `lua_scripts_mem + dictMemUsage + dictSize*sizeof(luaScript) + listLength*sizeof(listNode)`，被 `getMemoryOverheadData` 填进 `mh->eval_caches`，最终出现在 `genRedisInfoString` 的 `used_memory_scripts_eval` / `number_of_cached_scripts`。`getMemoryDoctorReport` 在 `dictSize(evalScriptsDict()) > 1000` 时把 `many_scripts` 标成一条告警。
- 键提取走 `src/db.c` 的 `evalGetKeys`，它只是 `genericGetKeys(0, 2, 3, 1, argv, argc, result)`：`num < 1 || num > (argc - 3)` 时 `result->numkeys = 0`，即语法错误时不留半份键集合。AOF 载入路径（`loadSingleAppendOnlyFile`）刻意**不**调 `evalGetCommandFlags`，注释说明「AOF 下 `processCommand` 的检查本来就不做」。

## 关键取舍

- **源码事实：** 一个解释器服务所有客户端（`luaCtx` 注释「We use just one for all clients」），换来的是脚本之间无状态隔离成本为零、`KEYS`/`ARGV` 只需两张全局表；代价是任一脚本都能看到 registry 里别的脚本函数，只能靠全局表只读化 + `__redis__err__handler` 来约束。
- **源码事实：** 淘汰只挂在 `EVAL` 上（`luaScriptsLRUAdd` 的 `if (evalsha) return NULL`），`LRU_LIST_LENGTH` 为 500，且是「表头出、表尾进」的 FIFO；注释给的理由是 `EVAL` 才是滥用缓存的那条路径，而 `SCRIPT LOAD` 的脚本一旦被悄悄淘汰会让流水线里的 `EVALSHA` 失败。
- **源码事实：** 缓存不持久化，`EVALSHA` 落空统一回 `-NOSCRIPT No matching script. Please use EVAL.`；长度不等于 40 的 SHA 在 `evalShaCommand` 里就被挡掉，以避免在 `evalGenericCommand` 内重复校验。
- **源码事实：** 一切「写」相关的前置检查都集中在 `scriptPrepareForRun`（只读副本、磁盘错误、`*_ro`、副本数量、OOM），而兼容模式（无 shebang）跳过除 stale 之外的全部检查——这是让老 `EVAL` 行为不变的显式分支。
- **源码事实：** 错误信息的两种形态由 `__redis__err__handler` 返回值的类型决定：table 才带 `source` / `line`，其他一律 `"Error running script %s, %.100s\n"`。
- **源码事实：** 超时不是强杀：先置 `SCRIPT_TIMEDOUT`、保护原客户端、进事件循环；真正的终止要等客户端发出 `SCRIPT KILL`，且脚本已写过数据就无法终止（`SCRIPT_WRITE_DIRTY`）。
- **源码事实：** 调试默认 fork，`fork` 模式在父进程里 `freeClientAsync(c)` 并直接放弃本次执行（返回 0），只有 `SCRIPT DEBUG SYNC` 才不解释器地就地调试。
- **架构推论：** 把「函数名」而不是「脚本体」放在 registry 里、把脚本体单独存在 `luaScript`，是为了让 `EVALSHA` 走一次字典查找就能定位，代价是两份数据必须靠 `lua_scripts_mem` 手工记账同步。
- **架构推论：** `evalGetCommandFlags` 在 `processCommand` 阶段就把 shebang 标志翻译成命令标志，等于把脚本的「自我声明」提前到命令准入阶段；这是一种用一次额外编译（`evalExtractShebangFlags` 只解析首行、不编译函数体）换取准入正确性的设计。
- **架构推论：** `script_disable_deny_script` 这个只有 `DEBUG` 能改的开关说明 `CMD_NOSCRIPT` 的拦截位置在调用期而非声明期，因此测试可以绕过它来验证命令实现，而生产路径无法绕过。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/eval.c` | `evalGenericCommand` | EVAL/EVALSHA 的共同执行体：取函数名、缺失则编译或报 NOSCRIPT、建上下文、调 Lua、收尾 |
| `src/eval.c` | `scriptCommand` | `SCRIPT` 子命令分发：`HELP`/`FLUSH`/`EXISTS`/`LOAD`/`KILL`/`DEBUG` |
| `src/eval.c` | `evalCommand` / `evalShaCommand` | 两个真正的命令入口，含 MONITOR 喂入、SHA 长度检查、调试态分叉 |
| `src/eval.c` | `evalRoCommand` / `evalShaRoCommand` | 只读变体，直接转调上两者，只读性由命令表标志与 `ro` 参数承担 |
| `src/eval.c` | `scriptingInit` / `scriptingRelease` / `scriptingReset` | 解释器与缓存的生命周期起点；初始化含全局表只读化与错误处理器注入 |
| `src/eval.c` | `luaCreateFunction` / `luaDeleteFunction` | 脚本编译入库与删除，含幂等短路、shebang 跳过、内存记账 |
| `src/eval.c` | `luaScriptsLRUAdd` | `EVAL` 脚本的 FIFO 淘汰，容量 `LRU_LIST_LENGTH` |
| `src/eval.c` | `evalCalcFunctionName` / `evalExtractShebangFlags` | 生成 `f_<sha1>` 函数名；解析 `#!lua flags=...` |
| `src/eval.c` | `evalGetCommandFlags` | 命令准入阶段把脚本声明的标志翻成命令标志 |
| `src/eval.c` | `evalGenericCommandWithDebugging` / `ldbStartSession` / `ldbEndSession` | 调试会话的 fork / 同步两种模式与收尾 |
| `src/eval.c` | `luaLdbLineHook` | 每行断点判定、步进、超时模拟断点 |
| `src/script.c` | `scriptPrepareForRun` | 运行上下文构建与全部写/OOM/stale/集群前置检查 |
| `src/script.c` | `scriptCall` | 脚本内每条 Redis 命令的参数、NOSCRIPT、ACL、写权限、OOM、集群校验后 `call()` |
| `src/script.c` | `scriptResetRun` | 清 `CLIENT_MULTI`、退出超时模式、复位 slot、停止命令传播 |
| `src/script.c` | `scriptInterrupt` / `scriptKill` | 超时检测与进入事件循环；`SCRIPT KILL` 的全部拒绝分支 |
| `src/script.c` | `scriptFlagsToCmdFlags` / `scripts_flags_def` | 脚本标志与命令标志的双向映射表 |
| `src/script_lua.c` | `luaCallFunction` | 设 hook、注入 `KEYS`/`ARGV`、`lua_pcall`、错误回复拼装 |
| `src/script_lua.c` | `luaGC` / `luaMaskCountHook` | 每 `LUA_GC_CYCLE_PERIOD` 次做一次 GC 步进；每 10 万指令触发中断检查 |
| `src/server.c` | `getCommandFlags` / `processCommand` | 准入阶段调用 `evalGetCommandFlags`，让脚本标志提前生效 |
| `src/server.c` | `initServer` / `genRedisInfoString` | 启动时 `luaEnvInit` + `scriptingInit(1)`；INFO 暴露脚本缓存体积与条数 |
| `src/server.c` | `createSharedObjects` | `shared.noscripterr`、`slowscripterr`、`slowevalerr` 等固定错误对象 |
| `src/lazyfree.c` | `freeLuaScriptsAsync` | 超过 `LAZYFREE_THRESHOLD` 丢后台线程，否则回退 `freeLuaScriptsSync` |
| `src/networking.c` | `createClient` / `resetClientInternal` | 造 `CLIENT_SCRIPT` + `CLIENT_DENY_BLOCKING` 的假客户端；命令结束清 `cur_script` |
| `src/db.c` | `evalGetKeys` / `genericGetKeys` | 按键数参数提取键集合，语法错误时返回 0 个键 |
| `src/config.c` | `createLongLongConfig` / `createBoolConfig` | `busy-reply-threshold`（别名 `lua-time-limit`）默认 5000；`lazyfree-lazy-user-flush` 默认 0 |

## 与主核 / AI 的关系

对主核（`engine/db/`）而言，这套实现给出的是一条**嵌入解释器的沙箱范式**：单例解释器 + 只读全局表 + 假客户端。真正可复用的是「假客户端」这一层——脚本不直接碰存储引擎，而是把 `KEYS`/`ARGV` 交给一个带 `CLIENT_SCRIPT`/`CLIENT_DENY_BLOCKING` 标志的内部客户端去执行命令，于是「脚本内禁用阻塞命令」「脚本内改库号」「脚本内改 RESP」都变成了客户端标志与上下文切换，而不是散落在每条命令里的 `if (in_script)`。

**架构推论：** 对 AI 场景最有迁移价值的是「效果复制 + 缓存可丢」这两个决定的组合：脚本不进持久化、复制的是它产生的写命令效果。这意味着脚本缓存是一个**纯派生缓存**，重启即失效、`EVALSHA` 落空就回 `-NOSCRIPT` 并让调用方改用 `EVAL`（源码里这个回退动作是客户端职责，`shared.noscripterr` 的文案就写着 `Please use EVAL.`）。主核若要缓存「LLM 生成的查询计划」或「算子 DSL 编译产物」，同样应该把它设为派生缓存并给出确定的失效错误码，而不是当持久对象来维护一致性。

**架构推论：** 反过来，`scriptPrepareForRun` 把写权限、OOM、stale、集群约束集中成一个**前置闸门**，而 `scriptCall` 在每条命令上再查一遍 ACL 与 arity——这种「一次准入 + 逐条复核」的双层结构，恰是 AI 生成的程序化操作最需要的形态：准入层可以拒绝整段计划的属性（是否带 `no-writes`、是否允许跨槽），复核层才能拦住单条越权命令。只做其中一层都不够。

## 参考

- [Redis 文档：Scripting with Lua](https://redis.io/docs/latest/develop/programmability/eval-intro/) —— `EVAL` / `EVALSHA` 参数语义、`KEYS` 与 `ARGV`、脚本缓存的易失性与 `NOSCRIPT` 回退口径。
- [Redis 文档：EVAL 命令](https://redis.io/docs/latest/commands/eval/) —— 命令签名、`numkeys` 位置与「返回值取决于脚本」的说明。
- [Redis 文档：Debugging Lua scripts](https://redis.io/docs/latest/develop/programmability/lua-debugging/) —— LDB 的 fork / 同步两种模式、断点与 `redis.breakpoint()`、`maxlen` 截断。
- [Redis 源码：src/eval.c](https://github.com/redis/redis/blob/unstable/src/eval.c) —— 本页 `evalGenericCommand`、`scriptCommand`、`luaCreateFunction`、`luaScriptsLRUAdd` 与 `ldb*` 的真实定义位置。
- [Redis 源码：src/script.c](https://github.com/redis/redis/blob/unstable/src/script.c) —— `scriptPrepareForRun`、`scriptCall`、`scriptKill`、`scripts_flags_def` 的定义位置。
