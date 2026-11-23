---
project: redis
feature: module-api
page_type: feature
schema_version: 1
refs:
  - path: src/module.c
    symbols: [moduleInitModulesSystem]
---

# 模块系统

## 一句话职责

`moduleInitModulesSystem` 是模块子系统唯一的建仓函数：它在 `src/server.c` 的 `main` 里紧跟 `initServerConfig` 与 `ACLInit` 之后被调用，把模块要用到的全部容器（模块名表、两张 API 函数表、加载队列、配置队列、事件监听表、定时器 rax、线程唤醒管道、GIL）一次建好就返回；它不加载任何模块，也不解析任何配置。真正的模块进进程发生在稍后的 `moduleLoadFromQueue`（`main` 在 `initServer` 之后调用），单次加载的实现是 `moduleLoad` → `moduleOnLoad`。

## 机制是什么

模块是动态库。core 侧把每个导出的 API 实现写成 `RM_<name>`，模块侧在 `redismodule.h` 里拿到去掉 `RM_` 前缀的函数指针：core 用宏 `REGISTER_API(name)` 把 `"RedisModule_" #name` 与实际函数地址登记进 `server.moduleapi`，模块侧 `RedisModule_Init` 逐个用 `REDISMODULE_GET_API(name)` 回调 `RM_GetApi` 取指针。两侧命名刻意错开，`src/module.c` 的注释写明这是为了避免动态链接器用主执行文件里的同名符号覆盖模块自己的全局函数指针。

`moduleInitModulesSystem` 建起来的容器分四类：

- **一张模块名表**：全局 `dict *modules`，类型 `modulesDictType` 定义在 `src/server.c`，键用 `dictSdsCaseHash` / `dictSdsKeyCaseCompare`（模块名大小写不敏感），value 是 `struct RedisModule *`；该 dictType 的 val destructor 为 `NULL`，字典不负责释放模块对象。
- **两张 API 表**：`server.moduleapi` 与 `server.sharedapi`，都由 `moduleRegisterCoreAPI` 用 `moduleAPIDictType` 创建；这个 dictType 的键是裸 C 字符串，hash 走 `dictCStringKeyHash`（`dictGenHashFunction` + `strlen`），比较用 `strcmp`（大小写敏感），key/value 的 dup 与 destructor 全为 `NULL`——里面存的是静态符号地址或模块自持内存。
- **三条加载/配置/阻塞队列**：`server.loadmodule_queue`（`loadmodule` 指令与 `MODULE LOAD` 的待办项，元素类型 `struct moduleLoadQueueEntry`，定义在 `src/server.h`）、`server.module_configs_queue`（`moduleConfigQueueDictType`，键大小写不敏感，value 是 `moduleConfigEntry`，由 `moduleConfigEntryDestructor` 释放 value 与 entry）、`moduleUnblockedClients`（配 `moduleUnblockedClientsMutex`）。
- **五条监听/作业链表**：`moduleKeyspaceSubscribers`、`modulePostExecUnitJobs`、`moduleCommandFilters`、`moduleAuthCallbacks`、`RedisModule_EventListeners`；另有定时器 rax `Timers = raxNewEx(0, NULL, sizeof(uint64_t))`，键是 big-endian 的 `uint64_t` timer ID（源码注释写明注释为 BE）。

另外三样不是 list/dict，但同样在这里定型：一是线程唤醒管道 `server.module_pipe`，用 `anetPipe(server.module_pipe, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK)` 建成两端都非阻塞且 close-on-exec 的管道，失败时 `serverLog(LL_WARNING, "Can't create the pipe for module threads: %s", ...)` 后 `exit(1)`；二是 `server.module_gil_acquiring` 原子标志清零；三是全局互斥量 `moduleGIL` 在函数末尾被 `pthread_mutex_lock(&moduleGIL)` **初始锁住**，源码注释写明 GIL 以「已加锁」状态起步，只在安全时刻解锁。

## 怎么实现的

### 一、初始化顺序

`moduleInitModulesSystem` 内部的顺序是固定的：

1. `moduleUnblockedClients = listCreate()`、`server.loadmodule_queue = listCreate()`、`server.module_configs_queue = dictCreate(&moduleConfigQueueDictType)`。
2. `server.module_gil_acquiring = 0`；`modules = dictCreate(&modulesDictType)`；`moduleAuthCallbacks = listCreate()`。
3. `moduleKeyspaceSubscribers = listCreate()`。配套的「哪些事件类型有订阅者」位图 `moduleKeyspaceSubscribersTypes` 与 `moduleKeyspaceSubscribersWithSubkeysTypes` 是文件静态变量、初值 0，由 `moduleUpdateKeyspaceSubscribersTypes` 在订阅/退订后重新遍历 `moduleKeyspaceSubscribers` 累加 `sub->event_mask` 刷新（带 subkeys 回调的订阅者另计入 `moduleKeyspaceSubscribersWithSubkeysTypes`）。
4. `modulePostExecUnitJobs = listCreate()`；`moduleCommandFilters = listCreate()`。
5. `moduleRegisterCoreAPI()`——这一步才创建 `server.moduleapi` 与 `server.sharedapi`，并逐条 `REGISTER_API`。本检出中 `moduleRegisterCoreAPI` 函数体内共有 401 条 `REGISTER_API(` 展开（对函数体内以 `REGISTER_API(` 起始的行计数得到，无重复条目）。
6. `anetPipe` 建 `server.module_pipe`，失败即 `exit(1)`；管道可读回调是 `modulePipeReadable`，它用 `char buf[128]` 循环读到不满 128 字节为止，然后调 `eventLoopHandleOneShotEvents()`。
7. `Timers = raxNewEx(0, NULL, sizeof(uint64_t))`。
8. `RedisModule_EventListeners = listCreate()`。
9. `serverAssert(sizeof(moduleEventVersions)/sizeof(moduleEventVersions[0]) == _REDISMODULE_EVENT_NEXT)`——把「事件版本表长度必须等于事件枚举总数」做成启动期断言。`moduleEventVersions` 是 `src/module.c` 的静态数组，本检出 21 项；`_REDISMODULE_EVENT_NEXT` 在 `src/redismodule.h` 定义为 `21`。
10. `pthread_mutex_lock(&moduleGIL)`，让 GIL 以已锁状态交接给事件循环。

`main` 侧的接入点是两段：第一段在 `initServerConfig` / `bitroarInit` / `ACLInit` 之后调 `moduleInitModulesSystem`；第二段在 `initServer` 之后、且仅非 Sentinel 模式下依次调 `moduleInitModulesSystemLast()` → `moduleLoadInternalModules()` → `moduleLoadFromQueue()`。`moduleInitModulesSystemLast` 当前是空函数，源码注释写明它只是给「必须在 server 初始化之后才能跑的模块初始化步骤」预留的位置，并点名历史原因：`createClient` 里的 `selectDb()` 要求 `server.db` 已初始化（注释引用 issue #7323）。`moduleLoadInternalModules` 只在 `INCLUDE_VEC_SETS` 被定义时（`src/Makefile` 在 `SKIP_VEC_SETS != yes` 时加 `-DINCLUDE_VEC_SETS=1`）用 `moduleOnLoad` 加载内置的 `VectorSets_OnLoad`，并 `serverAssert(retval == C_OK)`。

### 二、模块加载与注册骨架

`moduleLoadFromQueue` 遍历 `server.loadmodule_queue`：对每个 `moduleLoadQueueEntry` 调 `moduleLoad(path, argv, argc, 0, source)`；返回 `C_ERR` 就 `serverLog(LL_WARNING, "Can't load module from %s: server aborting", ...)` 再 `exit(1)`。队列跑完后若 `dictSize(server.module_configs_queue)` 非零，说明配置文件里出现了没有任何 `loadmodule` 认领的模块配置，源码逐条把 `'name value'` 打进日志，再以 `"Module Configuration detected without loadmodule directive or no ApplyConfig call: aborting"` 退出。注释明确了这个函数的立场：带着缺失模块启动不认为是合理状态（客户端依赖命令存在、AOF 回放需要模块、从库要能理解主库命令）。

`moduleLoad` 只做三件事：

1. `stat(path, &st)` 成功时检查可执行位（`S_IXUSR | S_IXGRP | S_IXOTH`），注释承认这是 best effort；缺位点直接返回 `C_ERR`。
2. `dlopen(path, RTLD_NOW|RTLD_LOCAL)`；返回 `NULL` 就记日志并返回 `C_ERR`。
3. `dlsym(handle, "RedisModule_OnLoad")`；取不到就 `dlclose(handle)` 并报「不导出 RedisModule_OnLoad 符号，模块未加载」，返回 `C_ERR`。

然后转交 `moduleOnLoad`。它用 `moduleCreateContext(&ctx, NULL, REDISMODULE_CTX_TEMP_CLIENT)` 造上下文——源码注释说明这里传 `NULL` 是因为此时还没有模块对象；模块对象由模块在 `OnLoad` 里经 `RedisModule_Init` → `RM_SetModuleAttribs` 建出：`RM_SetModuleAttribs` 在 `ctx->module != NULL` 时直接返回（幂等），否则 `zmalloc` 一个 `struct RedisModule`，建 `types` / `usedby` / `using` / `filters` / `module_configs` 五个 list（`module_configs` 还设了 `moduleListConfigMatch` 匹配与 `moduleListFree` 释放方法），把 `onload` 置 1，最后挂到 `ctx->module`。

`onload` 回调返回 `REDISMODULE_ERR` 时的回滚顺序由 `moduleOnLoad` 固定：`moduleUnregisterCleanup(ctx.module)` → `moduleRemoveCateogires(ctx.module)` → `moduleFreeModuleStructure(ctx.module)` → `moduleFreeContext` →（若有 handle）`dlclose(handle)`。其中 `moduleUnregisterCleanup` 内部依次是 `moduleFreeAuthenticatedClients`、`moduleUnregisterCommands`、`moduleUnsubscribeNotifications`、`moduleUnregisterPostNotificationJobs`、`moduleUnregisterSharedAPI`、`moduleUnregisterUsedAPI`、`moduleUnregisterFilters`、`moduleUnsubscribeAllServerEvents`、`moduleRemoveConfigs`、`moduleUnregisterAuthCBs`；`moduleRemoveCateogires` 只在 `num_acl_categories_added` 非零时调 `ACLCleanupCategoriesOnFailure`。

成功路径则是：`dictAdd(modules, ctx.module->name, ctx.module)` 登记名字；把 `blocked_clients` 清零、`handle` 写回模块、新建一个 `moduleLoadQueueEntry` 记下 `path` 与 `argv`（对每个 argv `incrRefCount`），供 `MODULE LIST` 与配置重写复用；若 `num_commands_with_acl_categories` 非零就调 `ACLRecomputeCommandBitsFromCommandRulesAllUsers`；最后 `moduleFireServerEvent(REDISMODULE_EVENT_MODULE_CHANGE, REDISMODULE_SUBEVENT_MODULE_LOADED, ctx.module)` 广播加载事件。

注册之后还有两道后置校验，任一命中都视作加载失败并把模块卸掉：一是模块登记了 `module_configs` 却 `configs_initialized` 不含 `MODULE_CONFIGS_USER_VALS`（等价于模块没调 `LoadConfigs`），二是本次是 LOADEX 且 `server.module_configs_queue` 还留着没被消耗的配置。两处都走 `serverAssert(moduleUnload(ctx.module->name, NULL, 1) == C_OK)`——真实调用方就是 `moduleOnLoad`，`forced_unload` 传 1 来绕过「模块导出数据类型不允许卸载」这条限制。

### 三、命令注册

`RM_CreateCommand` 的拒绝分支按顺序是：不是从 `OnLoad` 进来的（`!ctx->module->onload`）、`strflags` 解析失败（`commandFlagsFromString` 返回 `-1`）、带 `no-cluster` 而 `server.cluster_enabled` 为真、命令名非法（`isCommandNameValid` 为假）、命令名已被占用（`lookupCommandByCString` 非 `NULL`）。最后一条正是两个模块注册同名命令时后加载方失败的原因。

通过后先由 `moduleCreateCommandProxy` 造代理：`group` 固定 `COMMAND_GROUP_MODULE`、`proc` 固定 `RedisModuleCommandDispatcher`、`flags` 追加 `CMD_MODULE`；`firstkey != 0` 才建一条 `keySpec`（`CMD_KEY_FULL_ACCESS`，`KSPEC_BS_INDEX` 定位起点、`KSPEC_FK_RANGE` 取键，`lastkey < 0` 时原样保留，否则换算成相对偏移），否则 `key_specs_num = 0`。arity 的默认值是源码写死的 `cmdfunc ? -1 : -2`：有实现的命令用 -1（可变参数），纯容器命令用 -2（至少一个子命令）。真正插入命令表的两处 `dictAdd`（`server.commands` 与 `server.orig_commands`）被 `pauseAllIOThreads()` / `resumeAllIOThreads()` 包住并以 `serverAssert` 断言成功，最后用 `ACLGetCommandID(declared_name)` 拿 ACL 用的命令 ID。

### 四、配置队列

配置文件里不认识的指令不会被直接判错：`loadServerConfig` 把 `loadmodule` 交给 `queueLoadModule`，把其它未知指令的 `name value` 交给 `queueModuleConfig`；后者新建 `moduleConfigEntry`（记 `value` 与 `source`）并 `dictReplace` 进 `server.module_configs_queue`，`dictReplace` 返回假时再 `sdsfree(name)`（即 name 的所有权交给了字典）。`MODULE LOADEX` 的 `CONFIG` 关键字走同一函数：`parseLoadexArguments` 逐项识别 `CONFIG name value` 与 `ARGS`，前者连续入队两个 sds，后者把 `module_argv` 指向 `ARGS` 之后的部分；`i + 2 >= argc` 时报 `"CONFIG specified without name value pair"`，遇到其它未知词报 `"Syntax Error from arguments to loadex around %s."`，两者都返回 `REDISMODULE_ERR`。

消费端是 `loadModuleConfigs`，顺序固定：先按 `module_config->name` `dictUnlink`，拿不到再按 `module_config->alias` 取；命中就 `performModuleConfigSetFromName` 应用（失败则打日志、`dictFreeUnlinkedEntry`、`dictEmpty(server.module_configs_queue)` 并返回 `REDISMODULE_ERR`）；没命中且 `configs_initialized` 不含 `MODULE_CONFIGS_DEFAULTS` 时走 `performModuleConfigSetDefaultFromName` 填默认值；全部走完把 `configs_initialized` 置 `MODULE_CONFIGS_ALL_APPLIED`。这就是「回退到默认值」的真实调用方：`loadModuleConfigs` 自己，不是 `moduleOnLoad`。

### 五、事件、定时器与共享 API 的容器语义

- `RM_SubscribeToServerEvent` 先挡两类入参：`event.id >= _REDISMODULE_EVENT_NEXT`，以及 `event.dataver > moduleEventVersions[event.id]`（注释写明是「模块编译时用了比本服务端更新的 redismodule.h」）。已有同模块同事件的监听项就就地改 `callback`（`callback == NULL` 则从 `RedisModule_EventListeners` 删项并 `zfree`），没有才 `listAddNodeTail` 新项。
- `moduleFireServerEvent` 开头是明确的快路径：`listLength(RedisModule_EventListeners) == 0` 立刻返回；遍历时只匹配 `el->event.id == eid`。`REDISMODULE_EVENT_MODULE_CHANGE` 分支会跳过「被通知的模块自己」（`m == el->module` 时 `moduleFreeContext` 后 `continue`）；`CLIENT_CHANGE` 事件用 `REDISMODULE_CTX_NONE` 并把真实 `client*` 直接挂到 `ctx.client`，其余事件用 `REDISMODULE_CTX_TEMP_CLIENT`。
- `RM_ExportSharedAPI` / `RM_GetSharedAPI` 用 `server.sharedapi`：导出是 `dictAdd`，重名返回 `REDISMODULE_ERR` 并 `zfree` 临时结构；取用时除返回函数指针外，还把「谁用谁」记进 `usedby` / `using` 两条链表，装载卸载顺序因此可检查——`moduleUnload` 就是以 `usedby` 非空作为拒绝条件之一。
- `Timers` 是 rax：`moduleHoldsTimer` 用 `raxStart` / `raxSeek("^")` / `raxNext` 全表扫一遍判断模块是否还持有未触发的定时器，`moduleUnload` 用它作为拒绝卸载的条件之一。

### 六、GIL 与临时客户端池

GIL 的生命周期由事件循环驱动：`beforeSleep` 末尾 `if (moduleCount()) moduleReleaseGIL();`，源码在那里放了警告注释块「`moduleReleaseGIL` 之后不准再写任何东西」；`afterSleep` 在 `!ProcessingEventsWhileBlocked` 时 `atomicSet(server.module_gil_acquiring, 1)` → `moduleAcquireGIL()` → `atomicSet(..., 0)`。`moduleAcquireGIL` / `moduleTryAcquireGIL` / `moduleReleaseGIL` 同时维护线程局部标记 `threadHoldsGIL`，`moduleThreadHoldsGIL` 读它。模块线程侧的加锁语义由 `moduleGILAfterLock` 补齐：断言 `server.execution_nesting == 0`，再 `enterExecutionUnit(1, 0)` 抬高一层嵌套，避免线程里的 `RM_Call` 立刻传播；解锁侧 `moduleGILBeforeUnlock` 断言嵌套为 1，再 `exitExecutionUnit()` 与 `postExecutionUnitOperationsEx(moduleTakeUnaccountedBackgroundDuration(ctx))`。

临时客户端池的规则：`moduleAllocTempClient` 优先复用池内 `client`，池空才 `createClient(NULL)` 并打上 `CLIENT_MODULE`、`user = NULL`（根用户）；`moduleReleaseTempClient` 的扩容是「满了就翻倍，首次从 32 起步」（`moduleTempClientCap ? moduleTempClientCap*2 : 32`），回收前要 `clearClientConnectionState`、清空回复与字节计数、`resetClient`、`c->flags = CLIENT_MODULE`、`user = NULL`，并把挂在客户端上的异步 `RM_Call` promise 摘掉后 `freeRedisModuleAsyncRMCallPromise`。池的收缩在 `modulesCron`：`serverCron` 用 `run_with_period(100)` 包裹 `if (moduleCount()) modulesCron();`（该宏展开为 `_run_with_period(server.cronloops, 100, server.hz)` 的周期判断），`modulesCron` 每轮最多释放 `iteration = 50` 个，且只在 `moduleTempClientMinCount > min_client`（`min_client = 8`）时释放，收尾把 `moduleTempClientMinCount` 对齐为 `moduleTempClientCount`；数组本身在 `moduleTempClientCap > 32 && moduleTempClientCap > moduleTempClientCount * 4` 时除以 4 收缩。

### 七、卸载与命令入口

`moduleUnload` 的拒绝条件是固定顺序的一组：名字查不到（`dictFetchValue(modules, name)` 为空，报 `"no such module with that name"`）→ `loadmod->path` 长度为 0 的内置模块（报 `"the module can't be unloaded"`）→ 模块导出数据类型且 `forced_unload` 为假 → `usedby` 非空（别的模块还在用它的 API）→ `blocked_clients` 非零 → `moduleHoldsTimer` 为真。通过后 `dlsym(module->handle, "RedisModule_OnUnload")`，存在则建上下文调用，返回 `REDISMODULE_ERR` 就把 `errno` 置 `ECANCELED` 并取消卸载；`OnUnload` 之后再检查 `module->reply_buffers`（注释写明要放在它返回之后查，避免它释放或新建缓冲后留下悬垂指针），非零则拒绝。卸载末尾从 `modules` 删名、`moduleFreeModuleStructure`，再调 `ACLRecomputeCommandBitsFromCommandRulesAllUsers`。

`MODULE` 命令由 `moduleCommand` 实现，子命令是 `help` / `load` / `loadex` / `unload` / `list`。`MODULE LOAD` 走 `moduleLoad(..., is_loadex=0, CONFIG_SOURCE_RUNTIME)`；`MODULE LOADEX` 先 `parseLoadexArguments` 再 `moduleLoad(..., is_loadex=1, ...)`，失败时 `dictEmpty(server.module_configs_queue)` 回滚队列再报 `"Error loading the extension. Please check the server logs."`。`MODULE LIST` 交给 `addReplyLoadedModules`，每个模块输出 `name` / `ver` / `path` / `args` 四个字段；`moduleCount()` 就是 `dictSize(modules)`。这条命令在 `processCommand` 里受 protected 动作判定：`allowProtectedAction(server.enable_module_cmd, c)` 为假即被 `rejectCommandFormat` 拒绝，并提示需要设置 `enable-module-command`。

## 关键取舍

- **源码事实：** 容器创建与模块装载被拆到两个时点：`moduleInitModulesSystem` 在配置解析之前，`moduleLoadFromQueue` 在 `initServer` 之后。中间靠 `server.loadmodule_queue` 与 `server.module_configs_queue` 承载，配置阶段只入队。两个队列的「未消费即 abort」策略在 `moduleLoadFromQueue` 里实现。
- **源码事实：** GIL 以「已加锁」状态创建（初始化末尾显式 `pthread_mutex_lock(&moduleGIL)`），再由 `beforeSleep` / `afterSleep` 对称释放与重新获取，避免模块线程在主线程尚未进入事件循环时先跑起来。
- **源码事实：** `moduleEventVersions` 的长度由启动期 `serverAssert` 与 `_REDISMODULE_EVENT_NEXT` 对齐，`RM_SubscribeToServerEvent` 再用 `event.dataver > moduleEventVersions[event.id]` 拒绝更高版本的订阅——版本兼容靠「启动断言 + 订阅期比较」两处兜。
- **源码事实：** `server.moduleapi` 与 `server.sharedapi` 复用同一个 `moduleAPIDictType`，键是裸 C 字符串、`strcmp` 大小写敏感、无 dup 无 destructor；而 `modules` 表用 `dictSdsCaseHash`，模块名大小写不敏感。两张表对名字大小写的口径不同，是源码直接写出的。
- **源码事实：** 模块命令插入命令表被 `pauseAllIOThreads` / `resumeAllIOThreads` 包住，并以 `serverAssert` 断言 `dictAdd` 成功：重复键不是可恢复错误，而是不变式违例。
- **源码事实：** 模块配置「未应用即卸载」是硬规则：漏调 `LoadConfigs`、或 LOADEX 传了消耗不掉的配置，`moduleOnLoad` 都走 `moduleUnload` 强制卸载，不是警告后继续。
- **架构推论：** 把「建容器」与「装模块」拆成两个函数、两个时点，是为了让 `main` 在配置解析阶段就能把 `loadmodule` 与模块配置安全地写进队列，而不需要在配置阶段就 `dlopen`——否则配置写错的进程会在建好 `server.db` 之前就带进外部代码。
- **架构推论：** 模块 API 表存静态函数地址、且 dictType 不做内存释放，意味着这张表在进程生命周期内只增不减；`moduleRegisterCoreAPI` 的唯一调用者是 `moduleInitModulesSystem`，代码里不存在「重新初始化模块系统」的路径。
- **架构推论：** 临时客户端池的阈值组合（保留 `min_client = 8`、每轮最多释放 50 个、容量超过 32 且超过 4 倍才除以 4）是用少量常驻空闲客户端换掉 create/free client 的分配成本，收缩刻意限速以免单轮 cron 抖动。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/module.c` | `moduleInitModulesSystem` | 建全部模块容器、登记 core API、建唤醒管道与定时器 rax、锁住 GIL |
| `src/module.c` | `moduleInitModulesSystemLast` | server 初始化之后的模块初始化预留位（当前空实现） |
| `src/module.c` | `moduleRegisterCoreAPI` | 创建 `server.moduleapi` / `server.sharedapi` 并逐条 `REGISTER_API` |
| `src/module.c` | `moduleRegisterApi` / `RM_GetApi` | 单条 API 登记、按名取函数指针 |
| `src/module.c` | `moduleLoadFromQueue` | 消费 `loadmodule_queue`；加载失败或残留模块配置即 `exit(1)` |
| `src/module.c` | `moduleLoad` / `moduleOnLoad` | `dlopen` + `dlsym` + `OnLoad` 调用、模块登记、后置校验与回滚 |
| `src/module.c` | `RM_SetModuleAttribs` | 创建并初始化 `struct RedisModule` |
| `src/module.c` | `RM_CreateCommand` / `RM_CreateSubcommand` / `moduleCreateCommandProxy` | 模块命令与子命令登记、key spec 与 arity 定型 |
| `src/module.c` | `moduleUnregisterCleanup` / `moduleUnload` | 加载失败回滚与卸载的全部解绑步骤 |
| `src/module.c` | `queueModuleConfig` / `parseLoadexArguments` / `loadModuleConfigs` | 模块配置入队、LOADEX 解析、配置应用与默认值回退 |
| `src/module.c` | `RM_SubscribeToServerEvent` / `moduleFireServerEvent` | 事件监听登记与广播（含 `MODULE_CHANGE` 跳过自身） |
| `src/module.c` | `RM_ExportSharedAPI` / `RM_GetSharedAPI` | 模块间共享 API 的导出、取用与依赖记账 |
| `src/module.c` | `moduleAllocTempClient` / `moduleReleaseTempClient` / `modulesCron` | 临时客户端池的取用、回收与限速收缩 |
| `src/module.c` | `moduleAcquireGIL` / `moduleReleaseGIL` / `moduleGILAfterLock` / `moduleGILBeforeUnlock` | GIL 加解与嵌套层抬升、回落 |
| `src/module.c` | `moduleCommand` / `addReplyLoadedModules` / `moduleCount` | `MODULE` 子命令、`MODULE LIST` 输出、已注册模块计数 |
| `src/module.c` | `modulePipeReadable` | 管道可读回调：读空管道后处理事件循环一次性事件 |
| `src/server.c` | `main` | 两段式接入：配置解析前调 `moduleInitModulesSystem`，`initServer` 之后调 `moduleInitModulesSystemLast` / `moduleLoadInternalModules` / `moduleLoadFromQueue` |
| `src/server.c` | `modulesDictType` | 模块名表的 dictType：大小写不敏感、不释放 value |
| `src/server.c` | `beforeSleep` / `afterSleep` | 事件循环前后对称释放与获取 GIL |
| `src/server.c` | `serverCron` | 以 `run_with_period(100)` 周期调用 `modulesCron` |
| `src/server.c` | `processCommand` | `MODULE` 命令的 protected 动作判定 |
| `src/config.c` | `loadServerConfig` / `queueLoadModule` | 未知指令进模块配置队列、`loadmodule` 进加载队列 |
| `src/server.h` | `struct RedisModule` / `ModuleConfigsApplied` / `struct moduleLoadQueueEntry` | 模块对象、配置应用位、加载队列元素的结构定义 |
| `src/redismodule.h` | `RedisModule_Init` / `REDISMODULE_GET_API` / `_REDISMODULE_EVENT_NEXT` | 模块侧 API 指针抓取与事件枚举上界 |

## 与主核 / AI 的关系

对主核（`engine/db/`）而言，这套骨架提供的是**受控的外部代码接入点**：core 只负责建表、登记、记账（`usedby` / `using`）、卸载前逐项解绑，业务语义全部留给模块自己。主核若要允许外挂索引或外挂模态实现，可以直接借鉴「名字表 + API 函数表 + 依赖记账」这三件套——卸载时先查依赖、查阻塞客户端、查未触发定时器，再允许回滚。

**架构推论：** 这套设计的重心不在 API 数量（本检出 `moduleRegisterCoreAPI` 内 401 条登记），而在「卸载可判定」：`moduleUnload` 用一串前置条件把不可安全卸载的状态显式列出来，而不是靠引用计数猜测。主核接外挂组件时，把这串条件写成可枚举的拒绝原因，比事后做内存审计便宜得多。

**架构推论：** `moduleInitModulesSystem` 刻意不碰配置、不碰网络，全部副作用只落在内存容器，这使它能在 `main` 的很早期被调用而不引入时序耦合。主核若要把插件系统做成可测试单元，同理应先做「纯容器初始化」再单独做「装载」。

## 参考

- [Redis 文档：Redis modules API](https://redis.io/docs/latest/develop/reference/modules/) —— 模块的动态库加载方式、`RedisModule_OnLoad` 入口与 API 版本约定。
- [Redis 文档：MODULE LOAD](https://redis.io/docs/latest/commands/module-load/) —— 运行时加载模块的命令语义。
- [Redis 文档：MODULE LOADEX](https://redis.io/docs/latest/commands/module-loadex/) —— 加载时同时传模块配置的 `CONFIG` / `ARGS` 形式。
- [Redis 源码：src/module.c](https://github.com/redis/redis/blob/unstable/src/module.c) —— 本页所引全部符号的真实定义位置。
- [Redis 源码：src/redismodule.h](https://github.com/redis/redis/blob/unstable/src/redismodule.h) —— `RedisModule_Init`、`REDISMODULE_GET_API` 与事件枚举上界的定义。
