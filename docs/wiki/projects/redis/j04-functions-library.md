---
project: redis
feature: functions-library
page_type: feature
schema_version: 1
refs:
  - path: src/functions.c
    symbols: [functionsCreateWithLibraryCtx, fcallCommand, functionLoadCommand]
---

# 函数库

## 一句话职责

`src/functions.c` 负责把「一个库名 + 一段源码」注册成一组可调用函数，并提供两条对外通道：`FUNCTION LOAD` 走 `functionLoadCommand` → `functionsCreateWithLibraryCtx`，把源码交给引擎编译、把库和函数挂进当前 `functionsLibCtx`；`FCALL` 走 `fcallCommand`，按函数名在库里查 `functionInfo`，先过 `scriptPrepareForRun` 的权限/状态检查，再调 `engine->call` 真正执行。边界之外：临时脚本（`EVAL` / `EVALSHA`）的缓存与执行归 `src/eval.c`（j03），这里只在 `functionStatsCommand` 里读取脚本运行态的只读信息（`scriptIsRunning`、`scriptCurrFunction`、`scriptRunDuration`）。

## 机制是什么

函数库的存续状态全部挂在两个静态变量上（都在 `src/functions.c` 里定义）：`engines` 是「引擎名 → `engineInfo`」的全局注册表，`curr_functions_lib_ctx` 是当前的 `functionsLibCtx`。后者有四个字段：`libraries`（库名 → `functionLibInfo`）、`functions`（函数名 → `functionInfo`，跨库的扁平索引）、`cache_memory`（这些结构自身的内存开销）、`engines_stats`（每引擎的 `{n_lib, n_functions}` 计数）。

三层结构是分开建的：

- `functionLibInfo`（定义在 `src/functions.h`）：`name`、`functions`（库内函数 dict）、`ei`（指回引擎）、`code`（**整段原始负载**，含 `#!` 元数据行）。`functionsCreateWithLibraryCtx` 调 `engineLibraryCreate(md.name, ei, code)` 时传的是入参 `code` 而不是解析出的 `md.code`，因此 `li->code` 保留元数据行；交给引擎的才是去掉 shebang 之后的 `md.code`。这一点决定了 RDB / AOF 只需存 `li->code` 就能完整重建库。
- `functionInfo`：`name`、`function`（引擎负责解释的不透明编译产物）、`li`、`desc`、`f_flags`。
- `engineInfo`：`name`、`engine`（七条回调的 `engine` 结构）、`c`（一个独立 `client`）。这个 client 由 `functionsRegisterEngine` 用 `createClient(NULL)` 造出来，置上 `CLIENT_DENY_BLOCKING | CLIENT_SCRIPT`，之后被 `fcallCommandGeneric` 当作 `scriptPrepareForRun` 的 `engine_client` 参数，函数就在这个「伪客户端」上下文里执行命令。

命名大小写在两套 dict 类型上不一致，这是源码直接写明的：

- `librariesDictType` 用 `dictSdsHash` + `dictSdsKeyCompare`，**库名大小写敏感**。
- `functionDictType` 与 `libraryFunctionDictType` 用 `dictSdsCaseHash` + `dictSdsKeyCaseCompare`，**函数名大小写不敏感**。
- `engineDictType` 同样大小写不敏感。

因此 `FUNCTION DELETE foo` 与 `FUNCTION DELETE FOO` 是两个不同的目标，而 `FCALL myfunc` 与 `FCALL MYFUNC` 命中同一个函数。

## 怎么实现的

### 一、初始化：一个引擎，一个 ctx

`functionsInit` 只做三件事：`engines = dictCreate(&engineDictType)`；调 `luaEngineInitEngine()`，失败就返回 `C_ERR`（`initServer` 见到 `C_ERR` 直接 `serverPanic`）；再 `curr_functions_lib_ctx = functionsLibCtxCreate()`。`functionsLibCtxCreate` 会遍历此刻已注册的 `engines`，为每个引擎在 `engines_stats` 里预先放一个 `zcalloc` 出来的计数结构——所以 ctx 创建必须先有引擎注册。

Lua 引擎在 `src/function_lua.c` 的 `luaEngineInitEngine` 结尾通过 `functionsRegisterEngine(LUA_ENGINE_NAME, lua_engine)` 注册，`LUA_ENGINE_NAME` 就是字面量 `"LUA"`。`functionsRegisterEngine` 查重后 `dictAdd(engines, ...)`，并把 `zmalloc_size(ei)`、`sdsZmallocSize(ei->name)`、`zmalloc_size(engine)`、`engine->get_engine_memory_overhead(...)` 累加进静态变量 `engine_cache_memory`。

### 二、`FUNCTION LOAD`：元数据解析 → 引擎编译 → 链接进 ctx

`functionLoadCommand` 的解析很窄：只从 `argv[2]` 起（`argc_pos`）逐个数到 `argc-1` 之前，唯一认识的选项是 `REPLACE`，其它一律 `"Unknown option given: %s"`；循环结束后若 `argc_pos >= argc` 报 `"Function code is missing"`。超时取 `LOAD_TIMEOUT_MS`（同文件顶部定义为 500，单位毫秒）；`mustObeyClient(c)` 为真时改成 0（`mustObeyClient` 在 `src/server.c` 里判定 `c->id == CLIENT_ID_AOF || c->flags & CLIENT_MASTER`），即复制链路和 AOF 回放加载函数不受这 500 毫秒限制。

真正的加载全在 `functionsCreateWithLibraryCtx(code, replace, err, lib_ctx, timeout)` 里，顺序是固定的：

1. `functionExtractLibMetaData`：要求负载以 `#!` 开头，否则 `"Missing library metadata"`；`strchr(payload, '\n')` 找不到换行报 `"Invalid library metadata"`；shebang 行交给 `sdssplitargs`，切不开报同一个错误。`parts[0]` 用 `sdsrange(engine, 2, -1)` 去掉 `#!` 前缀当引擎名；其余参数只接受 `name=` 前缀（`strncasecmp` 比较 5 个字符），重复给 `name=` 报 `"Invalid metadata value, name argument was given multiple times"`，非 `name=` 报 `"Invalid metadata value given: %s"`；没有 `name` 报 `"Library name was not given"`。另外逐段检查 `\r` / `\n`，命中报 `"Invalid symbols in library metadata"`。`md.code = sdsnewlen(shebang_end, sdslen(payload) - shebang_len)`，起点就是那个换行符本身。
2. `functionsVerifyName(md.name)`：只允许 `a-z`、`A-Z`、`0-9`、`_`，且长度必须大于 0，否则报 `"Library names can only contain letters, numbers, or underscores(_) and must be at least one character long"`。
3. `dictFetchValue(engines, md.engine)` 找不到报 `"Engine '%S' not found"`。
4. 同库名已存在且没给 `REPLACE`：报 `"Library '%S' already exists"`；给了 `REPLACE` 就先 `libraryUnlink(lib_ctx, old_li)` 把旧库摘下来（错误路径会再 `libraryLink` 挂回去）。
5. `engineLibraryCreate` 造新库，`engine->create(engine->engine_ctx, new_li, md.code, timeout, err)` 编译并执行库体。Lua 侧实现在 `luaEngineCreate`：先放开全局表保护、把库 API 表设成 `__index`，再 `luaL_loadbuffer` 编译（失败报 `"Error compiling function: %s"`），然后 `lua_sethook(lua, luaEngineLoadHook, LUA_MASKCOUNT, 100000)` 并 `lua_pcall` 执行库体（失败报 `"Error registering functions: %s"`）；`luaEngineLoadHook` 在 `load_ctx->timeout > 0 && duration > load_ctx->timeout` 时推 `"FUNCTION LOAD timeout"` 并 `luaError`。库体执行期间调用的 `redis.register_function` 由 `luaRegisterFunction` 落到 `functionLibCreateFunction`。
6. 库内一个函数都没注册：报 `"No functions registered"`（判断是 `dictSize(new_li->functions) == 0`）。
7. 逐个比对 `lib_ctx->functions`，任一函数名已被别的库占用就报 `"Function %s already exists"`。
8. `libraryLink` 把库和全部函数挂进 ctx，再 `engineLibraryFree(old_li)` 释放被替换的旧库，返回库名（`md.name` 移交所有权，剩下元数据由 `functionFreeLibMetaData` 释放）。

`functionLibCreateFunction` 自身的语义是「同名覆盖」：`dictReplace(li->functions, fi->name, fi)` 走 `dictAddRaw`，新增时返回 1 并直接写值，键已存在时返回 0 并先 `dictSetVal` 新值、再调 `valDestructor` 释放旧值；`src/functions.c` 只在返回 0 时记一条 `"Function %s was overwritten in library %s"` 的 `LL_NOTICE` 日志。它先校验函数名，但复用的是 `functionsVerifyName` 和那句以 `Library names` 开头的错误串——这里校验的是函数名，文案说的是库名。

### 三、链接与摘链：`libraryLink` / `libraryUnlink` 的对称账

两个函数各自维护三本账，顺序在两处都是「先函数、后库、最后统计」：

- `libraryLink`：遍历 `li->functions`，逐个 `dictAdd(lib_ctx->functions, fi->name, fi)`（失败只记 `"Function %s already exists when linking library %s"`，不中断），每加一个就 `cache_memory += functionMallocSize(fi)`；然后 `dictAdd(lib_ctx->libraries, li->name, li)` 并 `cache_memory += libraryMallocSize(li)`；最后取 `engines_stats` 里该引擎的计数结构，`n_lib++`、`n_functions += dictSize(li->functions)`。
- `libraryUnlink`：反向执行，`dictDelete(lib_ctx->functions, fi->name)` 前面挂着 `serverAssert(ret == DICT_OK)`，即「从库里摘的每个函数必须确实在扁平索引里」；随后 `dictUnlink(lib_ctx->libraries, li->name)` → `dictSetVal(..., NULL)` → `dictFreeUnlinkedEntry`，因为 `librariesDictType` 的 val destructor 是 `engineLibraryDispose`，先置空 value 才能把库对象留给调用方继续使用；最后 `n_lib--`、`n_functions -= dictSize(li->functions)`。

`functionMallocSize` 的口径是 `zmalloc_size(fi) + sdsZmallocSize(fi->name) + 描述字符串(可选) + engine->get_function_memory_overhead(fi->function)`；`libraryMallocSize` 是 `zmalloc_size(li) + 库名 + 库代码`。这两组值在 `functionsMemoryEngine` 里先 `+= curr_functions_lib_ctx->cache_memory`，再叠加 `dictMemUsage(engines)`、`dictMemUsage(curr_functions_lib_ctx->functions)`、`sizeof(functionsLibCtx)` 和 `engine_cache_memory`，最终由 `getMemoryOverheadData`（`src/object.c`）计入 `functions_caches`。

### 四、`FCALL`：先取标志，再取键，再准备运行上下文

`FCALL` 的调用链分两处介入：

- 命令进入 `processCommand` 后，`src/server.c` 的 `getCommandFlags` 对 `fcallCommand` / `fcallroCommand` 特判，调 `fcallGetCommandFlags`：把 `c->cur_script = dictFind(curr_functions_lib_ctx->functions, c->argv[1]->ptr)`，找不到就原样返回命令表标志，找到就 `scriptFlagsToCmdFlags(cmd_flags, fi->f_flags)`。后者先清掉 `CMD_STALE | CMD_DENYOOM | CMD_WRITE`，再按脚本标志重设：没有 `allow-oom` 也没有 `no-writes` 就补 `CMD_DENYOOM`，没有 `no-writes` 就补 `CMD_WRITE`，有 `allow-stale` 就补 `CMD_STALE`，最后无条件清 `CMD_MAY_REPLICATE`。因此函数声明了 `no-writes` 后，`FCALL` 在 `processCommand` 眼里不再是写命令。
- 键提取由命令表登记的 `functionGetKeys`（`src/db.c`）完成，它只是 `genericGetKeys(0, 2, 3, 1, ...)`：键数量在 `argv[2]`，第一个键在 `argv[3]`，步长 1；`num < 1` 或 `num > (argc - firstKeyOfs)/keyStep` 时直接 `result->numkeys = 0`。
- 真正执行在 `fcallCommandGeneric(c, ro)`，`fcallCommand` 传 `ro = 0`、`fcallroCommand` 传 `ro = 1`。第一步是 `replicationFeedMonitors(c, server.monitors, c->db->id, c->argv, c->argc)`，源码注释写的理由是「Functions need to be fed to monitors before the commands they execute」——`FCALL` 在 `src/commands.def` 的命令表里带 `CMD_SKIP_MONITOR`，`src/server.c` 的 `call` 里那段统一喂送（条件为 `!(c->cmd->flags & (CMD_SKIP_MONITOR|CMD_ADMIN))`）会跳过它，所以这里要显式先喂一条。随后取 `c->cur_script`——`fcallGetCommandFlags` 在此之前已经把它填成同一个 `dictFind` 的结果，命中时直接复用，为空才再 `dictFind` 一次；仍为空报 `"Function not found"`。`getLongLongFromObject(c->argv[2], &numkeys)` 失败报 `"Bad number of keys provided"`；`numkeys > c->argc - 3` 报 `"Number of keys can't be greater than number of args"`；`numkeys < 0` 报 `"Number of keys can't be negative"`。之后 `scriptPrepareForRun(&run_ctx, fi->li->ei->c, c, fi->name, fi->f_flags, ro)`，返回非 `C_OK` 就直接返回（错误回复已由该函数写进调用方），成功才 `engine->call(&run_ctx, engine->engine_ctx, fi->function, c->argv + 3, numkeys, c->argv + 3 + numkeys, c->argc - 3 - numkeys)`，最后 `scriptResetRun`。

`scriptPrepareForRun`（`src/script.c`）是函数库和脚本共用的准入闸门，`FCALL` 会撞上的拒绝路径逐条列在源码里：`no-cluster` 标志 + 集群模式报 `"Can not run script on cluster, 'no-cluster' flag is set."`；主从断开且 `repl_serve_stale_data` 为 0 且没声明 `allow-stale` 报 `-MASTERDOWN`；未声明 `no-writes` 时依次检查只读副本（`-READONLY`）、磁盘错误（两种 `-MISCONF` 文案）、`ro` 参数为真（`"Can not execute a script with write flag using *_ro command."`）、`checkGoodReplicasStatus` 失败（`shared.noreplicaserr`）；最后是内存检查，未声明 `allow-oom` 也没声明 `no-writes`，且 `server.maxmemory` 非 0、`server.pre_command_oom_state` 为真时报 `"-OOM allow-oom flag is not set on the script..."`。通过后它把调用方的 `CLIENT_MULTI` 复制进伪客户端、`selectDb` 到调用方的库、把伪客户端 `resp` 重置为 2，并按标志置 `SCRIPT_READ_ONLY`（`ro` 为真，或非 EVAL 兼容模式下声明了 `no-writes`）、`SCRIPT_ALLOW_OOM`（调用方本就带 `CLIENT_ALLOW_OOM`，或非 EVAL 兼容模式下声明了 `allow-oom`）、`SCRIPT_ALLOW_CROSS_SLOT`（EVAL 兼容模式，或声明了 `allow-cross-slot-keys`），最后把 `curr_run_ctx` 指向本次运行。

### 五、`FUNCTION STATS` / `LIST` / `DELETE` / `KILL`

- `functionStatsCommand`：先判 `scriptIsRunning() && scriptIsEval()`，命中就回 `shared.slowevalerr`（`createSharedObjects` 里造出的 `-BUSY Redis is busy running a script. You can only call SCRIPT KILL or SHUTDOWN NOSAVE.`），即不打断正在跑的 `EVAL`。正常回一个两键 map：`running_script` 在无运行时是 null，有运行时是含 `name`（`scriptCurrFunction()`）、`command`（`scriptGetCaller()` 的 `argc` / `argv` 逐个搬出来）、`duration_ms`（`scriptRunDuration()`，即 `elapsedMs(curr_run_ctx->start_time)`）的三键 map；`engines` 的长度是 `dictSize(engines)`，每项是 `libraries_count` / `functions_count`，直接读 `engines_stats` 里的计数，不现场遍历库。
- `functionListCommand`：只认 `WITHCODE` 与 `LIBRARYNAME`（后者必须后跟一个参数，否则 `"library name argument was not given"`），其它报 `"Unknown argument %s"`。无过滤时数组长度直接用 `dictSize(libraries)`；有过滤时 `addReplyDeferredLen` + 末尾 `setDeferredArrayLen`，过滤用 `stringmatchlen(library_name, sdslen(library_name), li->name, sdslen(li->name), 1)`，`nocase = 1`。每个库是 3 键 map（`library_name` / `engine` / `functions`），`WITHCODE` 时变 4 键多一个 `library_code`；每个函数是 3 键 map（`name` / `description`（空则 null）/ `flags`），flags 由 `functionListReplyFlags` 先按 `scripts_flags_def` 数出命中数再 `addReplySetLen` 回写。
- `functionDeleteCommand`：按 `c->argv[2]->ptr` 在 `libraries` 里查（命令表 arity 定死为 3），查不到报 `"Library not found"`，查到就 `libraryUnlink` + `engineLibraryFree`，`server.dirty++` 后回 `+OK`。
- `functionKillCommand` 只有一行 `scriptKill(c, 0)`（`is_eval = 0`）。`scriptKill` 的拒绝条件依次是：无 `curr_run_ctx` 报 `-NOTBUSY`；`mustObeyClient` 下报 `-UNKILLABLE`；已置 `SCRIPT_WRITE_DIRTY` 报 `-UNKILLABLE`；`is_eval` 为真而要杀的是函数、或 `is_eval` 为假而要杀的是 `EVAL`，都按类型不符回对应的 busy 错误对象。通过才置 `SCRIPT_KILLED`。
- `functionFlushCommand`：`argc > 3` 走 `addReplySubcommandSyntaxError`；`argc == 3` 认 `SYNC` / `ASYNC`，其它报 `"FUNCTION FLUSH only supports SYNC|ASYNC option"`；`argc == 2` 时由 `server.lazyfree_lazy_user_flush` 决定 async，该配置在 `src/config.c` 用 `createBoolConfig` 注册、默认值 0。最终都调 `functionsLibCtxClearCurrent(async)`，它有两分支：同步分支直接 `functionsLibCtxFree(curr_functions_lib_ctx)` + `dictRelease(engines)`；异步分支把旧 ctx 和旧 `engines` 交给 `freeFunctionsAsync`；两分支最后都再跑一次 `functionsInit()` 重建空 ctx。

### 六、序列化：RDB / AOF / DUMP / RESTORE

- RDB 侧：`rdbSaveFunctions`（`src/rdb.c`）遍历 `functionsLibGet()` 返回的 `libraries`，每个库先写 `RDB_OPCODE_FUNCTION2`（`src/rdb.h` 定义为 245）再 `rdbSaveRawString(li->code)`，即只存源码。`RDB_OPCODE_FUNCTION_PRE_GA`（246）是 7.0 rc1 / rc2 的旧格式：`rdbLoadRioWithLoadingCtxInternal` 遇到它调 `rdbReportCorruptRDB("Pre-release function format not supported.")` 后 `exit(1)`，`functionRestoreCommand` 遇到同一个 opcode 则回 `"Pre-GA function format not supported"`。
- 读取侧：`rdbFunctionLoad` 先 `rdbGenericLoadStringObject(rdb, RDB_LOAD_SDS, NULL)` 拿到负载，`lib_ctx` 非空才 `functionsCreateWithLibraryCtx(final_payload, rdbflags & RDBFLAGS_ALLOW_DUP, &error, lib_ctx, 0)`——**超时传 0**，`replace` 由 `RDBFLAGS_ALLOW_DUP`（`(1<<2)`）决定。`redis-check-rdb` 拿 `lib_ctx = NULL` 调同一函数，走的是「只校验结构、不真正建库」。
- AOF 侧：`rewriteFunctions`（`src/aof.c`）每个库写一条 `*3` 的 `FUNCTION LOAD <code>`，不带 `REPLACE`。
- `FUNCTION DUMP`：`createFunctionDumpPayload` 用 `rioInitWithBuffer(payload, sdsempty())` 先 `rdbSaveFunctions`，再追加 2 字节 `RDB_VERSION`（按 `& 0xff` / `>> 8` 拆开写入，`src/rdb.h` 定义为 17），最后对整个缓冲区算 `crc64` 并 `memrev64ifbe` 后追加 8 字节。这三种尾巴的字节数在 `verifyDumpPayload`（`src/cluster.c`）里被写死：`len < 10` 直接 `C_ERR`，footer 落在 `p + (len-10)`，前 2 字节读版本（`rdbver > RDB_VERSION` 即失败），后 8 字节是 CRC（`server.skip_checksum_validation` 为真时跳过，CRC 为 0 视为「无校验」）。
- `FUNCTION RESTORE`：`argc > 4` 走子命令语法错误；策略默认 `restorePolicy_Append`，`argv[3]` 只认 `append` / `replace` / `flush`，其它报 `"Wrong restore policy given, value should be either FLUSH, APPEND or REPLACE."`；`verifyDumpPayload` 失败报 `"DUMP payload version or checksum are wrong"`。然后新建一个临时 `functionsLibCtx`，循环条件就是 `while (data_len - payload.io.buffer.pos > 10)`——用同样的 10 字节尾巴口径停读；每轮 `rdbLoadType` 为 -1 报 `"can not read data type"`，读到 `RDB_OPCODE_FUNCTION_PRE_GA` 报 `"Pre-GA function format not supported"`，不是 `RDB_OPCODE_FUNCTION2` 报 `"given type is not a function"`，`rdbFunctionLoad(... RDBFLAGS_NONE, &err)` 失败且 `err` 为空时补 `"failed loading the given functions payload"`。加载成功后：`FLUSH` 走 `functionsLibCtxSwapWithCurrent(functions_lib_ctx)` 并把局部指针置空（避免末尾重复释放）；`APPEND` / `REPLACE` 走 `libraryJoin(curr_functions_lib_ctx, functions_lib_ctx, 是否 Replace, &err)`。失败统一跳到 `load_error` 标签，按 `err` 有无分别回错误或 `+OK`，并释放临时 ctx。

`libraryJoin` 是这条路径上唯一带回滚的代码：先只做「查冲突」，把要替换的旧库 `libraryUnlink` 下来并挂进一个 `listCreate` 出来的 `old_libraries_list`（其 free method 是 `engineLibraryFreeGeneric`）；冲突检查一旦失败就 `goto done`，在 `done` 里把列表里的旧库逐个 `libraryLink` 回目标 ctx，保证目标 ctx 不留半改状态。没有冲突才逐个 `libraryLink` 源 ctx 的库并把源 dict 的 value 置空，最后 `functionsLibCtxClear(functions_lib_ctx_src)`。

### 七、复制、清理与异步释放

- 全量同步的两种路径在 `src/replication.c` 分开处理：`REPL_DISKLESS_LOAD_SWAPDB` 用 `functionsLibCtxCreate()` 造临时 ctx 装载，失败时 `functionsLibCtxFree(functions_lib_ctx)` 丢弃，成功时 `functionsLibCtxSwapWithCurrent` 整体换入；其它路径用 `functionsLibCtxGetCurrent()` 拿现成 ctx 并先 `functionsLibCtxClear` 清空。`FUNCTION RESTORE FLUSH` 复用的是同一个 swap 手法。
- 异步释放的阈值在 `src/lazyfree.c`：`#define LAZYFREE_THRESHOLD 64`。`freeFunctionsAsync` 只在 `functionsLibCtxFunctionsLen(functions_lib_ctx) > LAZYFREE_THRESHOLD`（即函数数超过 64）时 `bioCreateLazyFreeJob(lazyFreeFunctionsCtx, 2, ...)`，否则同步 `functionsLibCtxFree` + `dictRelease(engs)`——所以 `FUNCTION FLUSH ASYNC` 在小库上仍然是同步行为。
- `functionsLibCtxClear` 只 `dictEmpty` 掉 `functions` 与 `libraries`、把 `engines_stats` 里每项的 `n_lib` / `n_functions` 归零、`cache_memory = 0`；`engines_stats` 本身和 `engines` 表都不动，计数器结构留给下一轮复用。
- 对外计数：`functionsNum` / `functionsLibNum` 分别回 `dictSize`，`src/server.c` 的 `INFO` 用它们出 `number_of_functions` / `number_of_libraries`；`functionsMemoryVM` 汇总各引擎的 `get_used_memory`，单独进 `used_memory_vm_functions`，在 `getMemoryOverheadData`（`src/object.c`）里与 `evalScriptsMemoryVM()` 相加成 `script_vm`；`functionsMemoryEngine` 则单独记为 `functions_caches`。

## 关键取舍

- **源码事实：** 库名大小写敏感、函数名大小写不敏感（`librariesDictType` 用 `dictSdsHash` / `dictSdsKeyCompare`，`functionDictType`、`libraryFunctionDictType`、`engineDictType` 用 `dictSdsCaseHash` / `dictSdsKeyCaseCompare`）。后果直接落在命令层：`FUNCTION DELETE` 的名称匹配是敏感的，`FCALL` 的函数匹配不敏感。
- **源码事实：** 存进 RDB / AOF / `FUNCTION DUMP` 的是 `li->code`，也就是**带 `#!` 元数据行的原始负载**，恢复时重新走一遍 `functionExtractLibMetaData`。于是库名和引擎名不必单独持久化，代价是每次加载都要重新解析和重新编译。
- **源码事实：** 同库重载（`LOAD REPLACE`）是先 `libraryUnlink` 旧库、再编译新库，编译失败时在 `error` 标签里 `libraryLink` 把旧库挂回；`libraryJoin` 用 `old_libraries_list` 做同样的回滚。两处回滚只覆盖「已摘链的旧库」，不覆盖「已部分 link 的新库」。
- **源码事实：** `FUNCTION LOAD` 的超时是 500（`LOAD_TIMEOUT_MS`），但 `mustObeyClient` 时就改成 0；`rdbFunctionLoad` 则一律传 0。也就是说「加载时限」只约束客户端发起的 `FUNCTION LOAD`。
- **源码事实：** `FUNCTION FLUSH` 的异步性有两道闸门：命令层由 `lazyfree-lazy-user-flush`（默认 0）决定，`lazyfree` 层再由 `LAZYFREE_THRESHOLD`（64 个函数）决定是否真的丢给后台线程。
- **源码事实：** `FCALL` 的命令表标志带 `CMD_SKIP_MONITOR`，但 `fcallCommandGeneric` 自己先 `replicationFeedMonitors` 一次；同时 `getCommandFlags` 用函数声明的 `f_flags` 改写本次调用的命令标志，`no-writes` 会把 `CMD_WRITE` 与 `CMD_DENYOOM` 抹掉。两者合起来说明「函数是一次运行期才确定的命令」——静态命令表和运行期声明各自负责一半语义。
- **架构推论：** 以「库」为持久化与替换单位、以「函数」为调用单位，是为了让 RDB / AOF / 迁移都只搬运源码文本，而不是序列化编解码产物；代价是引擎编译产物（`fi->function`）永远只在内存里，重启后必须重编译，而 `engine->create` 存在返回 `C_ERR` 的失败分支——这就是 `libraryJoin` 必须带回滚、`functionsCreateWithLibraryCtx` 必须严格「先检冲突后 link」的原因。
- **架构推论：** 每个引擎配一个常驻 `client`，是把「函数的执行上下文」伪装成普通客户端，从而复用已有的脚本运行机制：`scriptPrepareForRun` 负责前置准入，`scriptCall` 在每次执行命令前把 `c->user` 指到调用方的用户并跑 `scriptVerifyACL`、`scriptVerifyCommandArity`、`CMD_NOSCRIPT` 检查与写权限检查。代价是伪客户端与调用方之间必须靠 `run_ctx->original_client` 显式传递身份与库号——`selectDb` 与 `c->user` 这一行就是这条传递链的全部。`CLIENT_DENY_BLOCKING` 的实际效果是让阻塞型命令立刻降级成「超时」：`src/t_list.c` 的 `blockingPopGenericCommand` 与 `src/t_zset.c` 的 `blockingGenericZpopCommand` 在该标志下直接 `addReplyNullArray` 返回，`src/t_stream.c` 的 `xreadCommand` 在同一个标志下同样 `addReplyNullArray` 后 `goto cleanup`，三者都不进 `blockClient`。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/functions.c` | `functionsCreateWithLibraryCtx` | 解析元数据、校验名、调引擎编译、链接进 ctx，含 `REPLACE` 回滚 |
| `src/functions.c` | `functionLoadCommand` | `FUNCTION LOAD [REPLACE]` 的参数解析与超时选择 |
| `src/functions.c` | `fcallCommand` / `fcallroCommand` | `FCALL` / `FCALL_RO` 入口，各自传 `ro` 给 generic |
| `src/functions.c` | `fcallCommandGeneric` | 先喂 MONITOR、查函数、校验 keys 数量、准备运行上下文后 `engine->call` |
| `src/functions.c` | `fcallGetCommandFlags` | 用函数 `f_flags` 改写本次命令标志 |
| `src/functions.c` | `libraryLink` / `libraryUnlink` | 库与函数在扁平索引、库索引、引擎计数上的对称增减 |
| `src/functions.c` | `libraryJoin` | `RESTORE APPEND/REPLACE` 的冲突检查与回滚 |
| `src/functions.c` | `functionLibCreateFunction` | 库内函数注册，同名覆盖并记日志 |
| `src/functions.c` | `functionStatsCommand` | `FUNCTION STATS`，含 EVAL 运行时的拒绝分支 |
| `src/functions.c` | `functionListCommand` / `functionDeleteCommand` / `functionKillCommand` / `functionFlushCommand` | 其余子命令实现 |
| `src/functions.c` | `createFunctionDumpPayload` / `functionRestoreCommand` | DUMP 尾部 2 字节版本 + 8 字节 CRC64，以及 RESTORE 的三种策略 |
| `src/functions.c` | `functionsLibCtxCreate` / `functionsLibCtxFree` / `functionsLibCtxClear` / `functionsLibCtxSwapWithCurrent` | ctx 生命周期 |
| `src/functions.c` | `functionsRegisterEngine` / `functionsInit` | 引擎注册与全局初始化 |
| `src/functions.c` | `functionsMemoryEngine` / `functionsNum` / `functionsLibNum` | 内存开销与计数对外接口 |
| `src/functions.h` | `engine` / `engineInfo` / `functionInfo` / `functionLibInfo` | 三层结构与引擎回调原型 |
| `src/function_lua.c` | `luaEngineInitEngine` / `luaEngineCreate` / `luaRegisterFunction` | LUA 引擎注册、编译执行库体、`redis.register_function` |
| `src/rdb.c` | `rdbSaveFunctions` / `rdbFunctionLoad` | 只存取 `li->code`，`replace` 由 `RDBFLAGS_ALLOW_DUP` 决定 |
| `src/rdb.h` | `RDB_OPCODE_FUNCTION2` / `RDB_OPCODE_FUNCTION_PRE_GA` / `RDB_VERSION` / `RDBFLAGS_ALLOW_DUP` | 245 / 246 / 17 / `(1<<2)` |
| `src/lazyfree.c` | `freeFunctionsAsync` / `lazyFreeFunctionsCtx` / `LAZYFREE_THRESHOLD` | 超过 64 个函数才丢后台线程释放 |
| `src/script.c` | `scriptPrepareForRun` / `scriptResetRun` / `scriptKill` / `scriptFlagsToCmdFlags` | 函数与脚本共用的准入闸门、收尾、杀进程、标志映射 |
| `src/db.c` | `functionGetKeys` → `genericGetKeys` | 键数量在 `argv[2]`、首键在 `argv[3]` |
| `src/server.c` | `getCommandFlags` / `initServer` / `INFO` 输出 | 对 `FCALL` 特判取标志、启动 `functionsInit`、输出 `number_of_libraries` |
| `src/replication.c` | `functionsLibCtxCreate` / `functionsLibCtxSwapWithCurrent` / `functionsLibCtxClear` 的调用点 | 全量同步的两条装载路径 |
| `src/aof.c` | `rewriteFunctions` | AOF 重写时把每个库写成一条 `FUNCTION LOAD` |

## 与主核 / AI 的关系

对主核（`engine/db/`）而言，函数库这套设计最值得抄的不是 Lua，而是**「持久化最小面 = 源码文本」**这个选择：`rdbSaveFunctions` 与 `rewriteFunctions` 都只写 `li->code`，恢复时重走一遍编译流程，于是快照体积与版本兼容问题都交给引擎自己。主核若要暴露「用户自定义谓词 / 打分函数」，同样的取舍成立：存源码，编译产物只留内存。

**架构推论：** 第二处可借鉴的是**两层准入**——`processCommand` 用静态命令标志做第一道筛（OOM、写、stale），函数自己声明的 `f_flags` 在 `fcallGetCommandFlags` 里改写这些标志做第二道筛。对既有写分支又有只读分支的函数，声明 `no-writes` 就能同时拿到「不被 `maxmemory` 计入 DENYOOM 拦截」与「只读副本上不被 `-READONLY` 拒绝」两种效果，而不必把函数拆成两个入口。

**架构推论：** 第三处是 `old_libraries_list` 式的回滚：先摘链、再检查、失败就挂回，保证目标 ctx 永不停在「删了一半」的状态。主核若要把函数库挂进 catalog 事务，这条「先验证、后提交、可回滚」的顺序比任何锁策略都更关键。

## 参考

- [Redis 文档：Functions 概览](https://redis.io/docs/latest/develop/programmability/functions-intro/) —— 库、函数、引擎三者的概念与 `FUNCTION` 子命令族的行为口径。
- [Redis 文档：FUNCTION LOAD](https://redis.io/docs/latest/commands/function-load/) —— `REPLACE` 选项与返回「库名」的约定。
- [Redis 文档：FUNCTION RESTORE](https://redis.io/docs/latest/commands/function-restore/) —— `FLUSH` / `APPEND` / `REPLACE` 三种策略的官方说明。
- [Redis 文档：FCALL](https://redis.io/docs/latest/commands/fcall/) —— `numkeys` 位置与键规格。
- [Redis 源码：src/functions.c](https://github.com/redis/redis/blob/unstable/src/functions.c) —— 本页所有符号、错误文案与常量的真实定义位置。
- [Redis 源码：src/function_lua.c](https://github.com/redis/redis/blob/unstable/src/function_lua.c) —— LUA 引擎注册与 `redis.register_function` 的实现。
- [Redis 源码：src/lazyfree.c](https://github.com/redis/redis/blob/unstable/src/lazyfree.c) —— `LAZYFREE_THRESHOLD` 与函数 ctx 的异步释放。
