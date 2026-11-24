---
project: sqlite
feature: global-initialization
page_type: feature
schema_version: 1
refs:
  - path: src/main.c
    symbols: [sqlite3_initialize, sqlite3_shutdown]
---

# 全局初始化与关闭

## 一句话职责

`sqlite3_initialize()` 在进程首次使用 SQLite 时把互斥量子系统、内存子系统、页缓存子系统与 VFS 一次性拉起来，`sqlite3_shutdown()` 在进程退出时把这套全局状态整体拆掉。

## 机制是什么

这一对接口是 SQLite 的「进程级」边界，不是「连接级」的。库里有两类完全不同的状态：

- **库级全局状态**：互斥量实现（`sqlite3GlobalConfig.mutex`）、内存分配器实现（`sqlite3GlobalConfig.m`）、页缓存实现（`sqlite3GlobalConfig.pcache2`）、VFS 链表、进程本地存储（PLS）槽位、内置函数哈希表（`sqlite3BuiltinFunctions`）。这些状态整个进程只有一份。
- **连接级状态**：`sqlite3` 句柄、lookaside 内存池、语句对象。属于 A01「数据库连接与打开」与 A02「连接关闭与僵尸连接」的管辖范围。

`sqlite3_initialize()` 只管第一类。它的设计目标是「嵌入式系统可以在进程启动/退出时显式拉起与拆掉整个库」，所以它的返回值是错误码而不是句柄：成功返回 `SQLITE_OK`，失败返回别的错误码。

官方文档（`src/sqlite.h.in` 里 `CAPI3REF: Initialize The SQLite Library` 一节）把这两者区分为「有效调用（effective call）」：

- `sqlite3_initialize()` 的第一次调用，或紧跟一次 `sqlite3_shutdown()` 之后的第一次调用，是有效调用；其余调用都是无害的 no-op。
- `sqlite3_shutdown()` 自上次 `sqlite3_initialize()` 以来的第一次调用是有效调用；其余调用同样是无害的 no-op。

线程安全性上两者不对称：`sqlite3_initialize()` 是线程安全的，`sqlite3_shutdown()` 不是，文档要求它只能在单个线程里调用，且调用前必须关掉所有连接、释放所有 SQLite 资源。

## 怎么实现的

### 入口分两层：薄壳 + 干活的

`src/main.c` 里把初始化刻意拆成两半：

- `sqlite3_initialize()` 是薄壳。先在 `SQLITE_OMIT_WSD` 编译下调用应用自带的 `sqlite3_wsd_init(4096, 24)` 建立进程本地存储（两个魔数 4096 与 24 直接写在调用处；`src/test_wsd.c` 的 `sqlite3_wsd_init()` 用它们算出 `N + sizeof(ProcessLocalStorage) + J*sizeof(ProcessLocalVar)` 的分配字节数，`PLS_HASHSIZE` 宏取 43 作为哈希桶数）。随后只做一件事：判断 `sqlite3GlobalConfig.isInit`，已初始化就执行一次 `sqlite3MemoryBarrier()` 后直接 `return SQLITE_OK`，否则转调 `sqlite3Initialize()`。
- `sqlite3Initialize()` 是 `static SQLITE_NOINLINE int` 函数，承担全部实际工作。源码注释明确写了这个拆法的理由：`sqlite3_initialize()` 被调用得非常频繁，而 `sqlite3Initialize()` 极少运行；拆开是为了避免在不需要初始化时白白付出局部变量的栈帧开销。

`isInit` 是 `sqlite3GlobalConfig` 里的一个 `int` 字段（`src/sqliteInt.h` 里注释为 `True after initialization has finished`），它是整个全局状态机的核心标志位。

### sqlite3Initialize() 的实际顺序

按函数体顺序，初始化分三段：

**第一段：互斥量子系统。** 先 `rc = sqlite3MutexInit()`，失败立刻返回。`sqlite3MutexInit()`（`src/mutex.c`）在 `sqlite3GlobalConfig.mutex.xMutexAlloc` 为 0 时，说明用户没通过 `sqlite3_config(SQLITE_CONFIG_MUTEX)` 装自己的实现，于是按 `bCoreMutex` 决定拷哪套方法进来：`bCoreMutex` 为真用 `sqlite3DefaultMutex()`（`SQLITE_THREAD_MISUSE_WARNINGS` 下用 `multiThreadedCheckMutex()`），为假用 `sqlite3NoopMutex()`；拷完执行 `sqlite3MemoryBarrier()` 再设置 `xMutexAlloc`，最后调 `mutex.xMutexInit()`。写入 `xMutexAlloc` 前后各有一次 `sqlite3MemoryBarrier()`，注释写明这是配对的屏障，防止弱内存序处理器上读到未初始化的 `xMutexInit`。

**第二段：内存子系统与初始化专用递归互斥量。** 这一段在 `SQLITE_MUTEX_STATIC_MAIN` 互斥量保护下进行（`sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MAIN)`，常量值 2，定义在 `src/sqlite.h.in`）。顺序是：

1. 置 `sqlite3GlobalConfig.isMutexInit = 1`；
2. 若 `isMallocInit` 为 0，调用 `sqlite3MallocInit()`；
3. 成功则置 `isMallocInit = 1`，并（若 `pInitMutex` 为空）用 `sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE)` 分配一个递归互斥量存进 `pInitMutex`；若 `bCoreMutex` 为真而这次分配失败，`rc = SQLITE_NOMEM_BKPT`；
4. 成功则 `sqlite3GlobalConfig.nRefInitMutex++`。

`sqlite3MallocInit()`（`src/malloc.c`）的函数体是：若 `sqlite3GlobalConfig.m.xMalloc == 0` 就调 `sqlite3MemSetDefault()` 装默认分配器；分配 `SQLITE_MUTEX_STATIC_MEM` 互斥量（常量值 3）；若 `pPage == 0 || szPage < 512 || nPage <= 0` 三个条件任一成立，就把 `pPage` 和 `szPage` 清零（页面大小下限 512 就写在这个判断里）；最后调 `sqlite3GlobalConfig.m.xInit(sqlite3GlobalConfig.m.pAppData)`，若返回非 `SQLITE_OK` 就把 `mem0` 整体 `memset` 清零。

若第二段结束时 `rc != SQLITE_OK`，函数直接返回错误码。

**第三段：其余子系统，全部在 `pInitMutex` 这把递归互斥量下做。** 先 `sqlite3_mutex_enter(sqlite3GlobalConfig.pInitMutex)`，然后判断 `sqlite3GlobalConfig.isInit == 0 && sqlite3GlobalConfig.inProgress == 0`，两个条件同时成立才继续。`inProgress` 是 `sqlite3GlobalConfig` 里的另一个 `int` 字段，注释为 `True while initialization in progress`。

之所以要用递归互斥量而不是普通互斥量：源码注释写明递归调用「通常来自 `sqlite3_os_init()` 内部的 `sqlite3_vfs_register()`」。用递归锁，同一线程的递归调用立刻返回，不会自锁死；而别的线程（源码用 X、Y 两个线程举例）会被挡在锁外，直到 X 的最外层调用结束。

第三段内部的执行顺序是：

1. 置 `inProgress = 1`；
2. `SQLITE_ENABLE_SQLLOG` 编译下调用 `sqlite3_init_sqllog()`；
3. `memset(&sqlite3BuiltinFunctions, 0, sizeof(sqlite3BuiltinFunctions))` 后调 `sqlite3RegisterBuiltinFunctions()`（`src/func.c`，把 `aBuiltinFunc[]` 数组里的函数定义塞进全局函数哈希表；该数组不能是 const，因为启动时要改写 `FuncDef.pHash`）；
4. 若 `isPCacheInit == 0`，调 `sqlite3PcacheInitialize()`（`src/pcache.c`：若 `pcache2.xInit == 0` 就先 `sqlite3PCacheSetDefault()` 装默认页缓存实现，然后调 `pcache2.xInit(pcache2.pArg)`）；
5. 成功则置 `isPCacheInit = 1`，调 `sqlite3OsInit()`（`src/os.c`：先 `sqlite3_malloc(10)` 再 `sqlite3_free(p)` 造一次可注入的分配失败，分配失败返回 `SQLITE_NOMEM_BKPT`，否则转调平台相关的 `sqlite3_os_init()`）；
6. `SQLITE_OMIT_DESERIALIZE` 未定义时，成功则调 `sqlite3MemdbInit()`（`src/memdb.c`：取当前最低 VFS 的 `szOsFile`，据此设置 `memdb_vfs.szOsFile`，再 `sqlite3_vfs_register(&memdb_vfs, 0)` 注册 memdb VFS）；
7. 成功则调 `sqlite3PCacheBufferSetup(sqlite3GlobalConfig.pPage, sqlite3GlobalConfig.szPage, sqlite3GlobalConfig.nPage)`（`src/pcache1.c`：`pBuf == 0` 时 `sz = n = 0`，`n == 0` 时 `sz = 0`，`sz` 用 `ROUNDDOWN8` 向下取整到 8 的倍数；`pcache1.nReserve` 取 `n > 90 ? 10 : (n/10 + 1)`）；
8. `SQLITE_EXTRA_INIT_MUTEXED` 编译下调 `SQLITE_EXTRA_INIT_MUTEXED(0)`；
9. 成功则 `sqlite3MemoryBarrier()` 后置 `sqlite3GlobalConfig.isInit = 1`，并（`SQLITE_EXTRA_INIT` 编译下）置 `bRunExtraInit = 1`；
10. 置 `inProgress = 0`，释放 `pInitMutex`。

第三段结束后，函数回到 `SQLITE_MUTEX_STATIC_MAIN` 互斥量下把 `pInitMutex` 引用计数减一：`nRefInitMutex--`，若结果 `<= 0`（并断言 `nRefInitMutex == 0`），则 `sqlite3_mutex_free(sqlite3GlobalConfig.pInitMutex)` 并把 `pInitMutex` 置 0。源码注释说明这一步是为了「防止资源泄漏」——`pInitMutex` 只在初始化期间需要，用完就释放。

最后，在 `NDEBUG` 未定义且 `SQLITE_OMIT_FLOATING_POINT` 未定义时，函数做一次浮点自检：把 `(((u64)1)<<63)-1` 的位模式拷进一个 `double`，断言 `sizeof(x)==8`、`sizeof(x)==sizeof(y)` 且 `sqlite3IsNaN(y)`。这段代码注释写明只在初始化时跑一次，「不想太频繁地运行它而白白消耗 CPU 周期」。

`SQLITE_EXTRA_INIT` 编译下，函数最后调 `SQLITE_EXTRA_INIT(0)` 并把它的返回值作为 `sqlite3Initialize()` 的返回值。

### sqlite3_shutdown() 的实际顺序

`sqlite3_shutdown()` 的函数体比初始化短得多，同样先在 `SQLITE_OMIT_WSD` 下调 `sqlite3_wsd_init(4096, 24)`，然后按四段顺序拆解，每段都由一个独立的标志位守卫，因此可以做到「只拆已建起来的部分」：

1. **若 `isInit`**：调 `SQLITE_EXTRA_SHUTDOWN()`（对应编译宏存在时），然后 `sqlite3_os_end()`（平台相关的 OS 层收尾，Unix 版在 `src/os_unix.c`，Windows 版在 `src/os_win.c`），再 `sqlite3_reset_auto_extension()`（`src/loadext.c`：在 `SQLITE_MUTEX_STATIC_MAIN` 互斥量下释放自动扩展名数组 `wsdAutoext.aExt` 并把 `nExt` 清零），最后置 `isInit = 0`。
2. **若 `isPCacheInit`**：调 `sqlite3PcacheShutdown()`（`src/pcache.c`：仅当 `pcache2.xShutdown` 非空时才调它，源码注释指出 `xShutdown()` 允许为 NULL），然后置 `isPCacheInit = 0`。
3. **若 `isMallocInit`**：调 `sqlite3MallocEnd()`（`src/malloc.c`：若 `m.xShutdown` 非空则调它，然后 `memset(&mem0, 0, sizeof(mem0))`），置 `isMallocInit = 0`。在 `SQLITE_OMIT_SHUTDOWN_DIRECTORIES` 未定义时，还要把 `sqlite3_data_directory` 与 `sqlite3_temp_directory` 置 0——源码注释解释：堆子系统已经关掉，这两个全局变量本应为空或指向 `sqlite3_malloc()` 得到的内存，现在必须确保它们不指向刚被作废的堆内存。
4. **若 `isMutexInit`**：调 `sqlite3MutexEnd()`（`src/mutex.c`：仅当 `mutex.xMutexEnd` 非空时才调它），置 `isMutexInit = 0`。

函数末尾无条件 `return SQLITE_OK`。

注意 `sqlite3_reset_auto_extension()` 内部（在 `SQLITE_OMIT_AUTOINIT` 未定义时）会先调一次 `sqlite3_initialize()`——也就是说 `sqlite3_shutdown()` 内部会走一次初始化路径。这是源码里真实存在的递归/回环调用。

### 失败语义与「部分初始化」

初始化失败时的契约由 `src/test_init.c` 的文件头注释写明，那段注释明确说这是「用于生产系统的源码中不使用的部分」，但它准确描述了设计意图：

1. 返回一个错误码给用户；
2. 后续一次 `sqlite3_shutdown()` 会去调那些「已经成功初始化」的子系统的 shutdown 方法；
3. 后续一次 `sqlite3_initialize()` 会尝试初始化剩下那些尚未初始化的子系统。

这与 `isInit` / `isPCacheInit` / `isMallocInit` / `isMutexInit` 四个标志位分工的写法一致：每个子系统单独记账，失败时已经建起来的部分不会泄漏，后续调用能补齐没建成的部分。

### 谁会替应用去调

除非编译了 `SQLITE_OMIT_AUTOINIT`，否则大量内部接口都会自动调 `sqlite3_initialize()`。源码里能查到的真实调用方包括：

- `src/main.c` 的 `sqlite3_open_v2()` 与 `sqlite3_open16()`，两处都包在 `#ifndef SQLITE_OMIT_AUTOINIT` 里；
- `src/malloc.c` 的 `sqlite3MallocInit()` 之外的 `sqlite3_malloc()`、`sqlite3_malloc64()`、`sqlite3_realloc64()`、`sqlite3MallocZero()` 等分配路径；
- `src/os.c` 的 `sqlite3_vfs_find()`、`sqlite3_vfs_register()`、`sqlite3_vfs_unregister()`；
- `src/mutex.c` 的 `sqlite3_mutex_alloc()`（当 `id <= SQLITE_MUTEX_RECURSIVE` 时）；
- `src/printf.c` 的 `sqlite3_mprintf()` 与 `sqlite3_vmprintf()`；
- `src/loadext.c` 的 `sqlite3_load_extension()` 与 `sqlite3_reset_auto_extension()`；
- `src/random.c` 的 `sqlite3_randomness()`；
- `src/complete.c` 的补全接口。

### 与 sqlite3_config() 的先后关系

`sqlite3_config()`（`src/main.c`）在函数开头检查 `sqlite3GlobalConfig.isInit`：一旦库已初始化，除 `SQLITE_CONFIG_LOG` 与 `SQLITE_CONFIG_PCACHE_HDRSZ` 这两个被 `mAnytimeConfigOption` 白名单放行的选项外，其余操作返回 `SQLITE_MISUSE_BKPT`。也就是说，全局配置只能在 `sqlite3_initialize()` 之前做——`sqlite3_config()` 是「初始化前的配置钩子」，`sqlite3_initialize()` 是「配置生效的分界线」。

`sqlite3_config(SQLITE_CONFIG_HEAP)` 还有一条专门的边界处理：`mnReq` 小于 1 时被抬到 1，大于 `(1<<12)` 时被截到 `(1<<12)`（源码注释写作 `cap min request size at 2^12`）。

## 关键取舍

**把「是否已初始化」的判断放在薄壳里，代价是每次调用都要查一次全局标志。** `sqlite3_initialize()` 几乎总是 no-op，但它在热路径上被反复调用（分配器、格式化、VFS 查找都在调）。拆成薄壳 + `SQLITE_NOINLINE` 的干函数，是用一次全局读换取省掉局部变量栈帧与寄存器压力。注释把这个取舍写得很直白。

**初始化用递归互斥量，牺牲一点点性能换取「不会自锁死」。** `pInitMutex` 用 `SQLITE_MUTEX_RECURSIVE` 而不是 `SQLITE_MUTEX_FAST`，因为 `sqlite3_os_init()` 会经由 `sqlite3_vfs_register()` 递归回 `sqlite3_initialize()`。同一线程递归进入必须立刻返回，别的线程必须阻塞等待——这正是 `inProgress` 标志位存在的意义：递归进入时 `isInit == 0 && inProgress == 0` 不成立，直接跳过整个初始化体。

**用四个独立标志位而不是一个 `isInit` 记账，换来「失败可恢复」。** 如果只有一个总标志位，初始化在中途失败后，关闭时无法知道该拆哪几个子系统，就只能整体泄漏。拆成 `isMutexInit` / `isMallocInit` / `isPCacheInit` / `isInit` 四个之后，关闭逻辑才能对每个子系统分别判断。

**`pInitMutex` 用完即释放。** 初始化期间需要它做递归保护，但初始化完成后不再需要，留在全局里只是一个长生命周期资源的隐患。引用计数 `nRefInitMutex` 减到 0 就 `sqlite3_mutex_free()` 掉。

**关闭路径对「子系统未初始化」和「回调为空」双重容错。** `sqlite3PcacheShutdown()` 检查 `pcache2.xShutdown` 是否为空再调，`sqlite3MutexEnd()` 检查 `mutex.xMutexEnd` 是否为空再调——插件式的子系统允许只提供一半的方法。

**`sqlite3_shutdown()` 不做线程安全保证，接口文档直接写明。** 关闭时库内可能还有别的线程在跑，SQLite 选择不提供保护，把约束推给调用方：必须先关掉所有连接、释放所有资源，且只在单线程里调用。

**`sqlite3_initialize()` 里的浮点自检只在 debug 构建存在，且只跑一次。** 它用 `assert()` 输出（源码注释写明「这段代码唯一的『输出』就是 assert 语句」），`NDEBUG` 或 `SQLITE_OMIT_FLOATING_POINT` 任一成立就整段不编译。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/main.c | `sqlite3_initialize` | 初始化入口薄壳：WSD 准备 + `isInit` 快速判定，不初始化时转调 `sqlite3Initialize()` |
| src/main.c | `sqlite3Initialize` | 实际初始化：互斥量 → 内存 → 页缓存 → VFS → memdb → 静态页缓存缓冲区的完整顺序与错误分支 |
| src/main.c | `sqlite3_shutdown` | 关闭入口：按 `isInit` / `isPCacheInit` / `isMallocInit` / `isMutexInit` 四段逆序拆解全局状态 |
| src/main.c | `sqlite3_config` | 初始化前的全局配置钩子；库已初始化时除白名单选项外一律返回 `SQLITE_MISUSE_BKPT` |
| src/mutex.c | `sqlite3MutexInit` | 选择并安装默认/无操作/检查型互斥量方法，调用 `xMutexInit` |
| src/mutex.c | `sqlite3MutexEnd` | 调用 `xMutexEnd` 关闭互斥量子系统 |
| src/malloc.c | `sqlite3MallocInit` | 装默认分配器、分配 `SQLITE_MUTEX_STATIC_MEM`、校验 `szPage < 512` 等页缓存参数、调用 `m.xInit` |
| src/malloc.c | `sqlite3MallocEnd` | 调用 `m.xShutdown` 并把 `mem0` 清零 |
| src/pcache.c | `sqlite3PcacheInitialize` | `pcache2.xInit` 为空时装默认页缓存，再调用 `pcache2.xInit` |
| src/pcache.c | `sqlite3PcacheShutdown` | `pcache2.xShutdown` 非空时调用它 |
| src/pcache1.c | `sqlite3PCacheBufferSetup` | 建立静态页缓存缓冲区：`ROUNDDOWN8` 对齐页大小、`nReserve` 取 `n > 90 ? 10 : (n/10 + 1)` |
| src/os.c | `sqlite3OsInit` | 用一次 `sqlite3_malloc(10)`/`sqlite3_free` 制造可注入的分配失败，再转调平台 `sqlite3_os_init()` |
| src/memdb.c | `sqlite3MemdbInit` | 注册 memdb VFS，其 `szOsFile` 取自当前最低 VFS 的 `szOsFile` |
| src/func.c | `sqlite3RegisterBuiltinFunctions` | 把 `aBuiltinFunc[]` 填进全局函数哈希表 `sqlite3BuiltinFunctions` |
| src/loadext.c | `sqlite3_reset_auto_extension` | 关闭时释放自动扩展名数组；内部会先调 `sqlite3_initialize()` |
| src/global.c | `sqlite3GlobalConfig` | 全局配置结构体：`isInit` / `inProgress` / `isMutexInit` / `isMallocInit` / `isPCacheInit` / `nRefInitMutex` / `pInitMutex` 等状态位的定义处 |
| src/sqliteInt.h | `SQLITE_DEFAULT_PCACHE_INITSZ` | 默认静态页缓存页数，取值 20，赋给 `sqlite3GlobalConfig.nPage` |
| src/test_init.c | 文件头注释 | 写明初始化失败后的三条契约：返回错误码、关闭只拆已初始化部分、再初始化补齐剩余部分 |

## 与主核 / AI 的关系

**架构推论：** 主核（`engine/db/`）的分层规范里禁止跨层依赖，L0 common 之上每一层只允许向下依赖。主核自身的内存分配与并发原语若各自实现一套「全局一次性初始化」，就等于在主核里重造 SQLite 的 `sqlite3GlobalConfig` + `isInit` 状态机；更贴合的做法是把主核的全局状态收敛到一层，并且明确「哪些是进程级一次性初始化、哪些是连接级构造」的边界，与 SQLite 这里划的界一致。

**架构推论：** SQLite 把「初始化是否已完成」暴露为一个全局标志位（`isInit`），并让 `sqlite3_config()` 在该标志置位后拒绝除白名单外的配置。这个「配置窗口只在初始化之前存在」的做法，对主核意味着：任何影响全局行为的旋钮（分配器、锁实现、页大小）若在初始化之后才允许改，就必须有一份与 SQLite 同构的「配置已关闭」判定，否则多线程下的配置竞态无从收敛。

**架构推论：** 主核的嵌入式 SDK（`engine/src/sdk/`）只依赖 SQLite 一个外部库。若 SDK 把 SQLite 当作库级全局状态来用，就必须在 SDK 的入口处显式调用 `sqlite3_initialize()`，否则在 `SQLITE_OMIT_AUTOINIT` 构建下会直接段错误——`sqlite.org` 的编译选项文档对该选项的描述正是「若在未调用 `sqlite3_initialize()` 的情况下调用 `sqlite3_malloc()`、`sqlite3_vfs_find()` 或 `sqlite3_open()`，很可能导致段错误」。

## 参考

- [Initialize The SQLite Library — sqlite.org C 接口文档](https://sqlite.org/c3ref/initialize.html)：`sqlite3_initialize()` / `sqlite3_shutdown()` / `sqlite3_os_init()` / `sqlite3_os_end()` 的官方语义，含「有效调用」定义与线程安全约束。
- [sqlite/sqlite 仓库 src/main.c — GitHub 源码页](https://github.com/sqlite/sqlite/blob/master/src/main.c)：`sqlite3_initialize()`、`sqlite3Initialize()`、`sqlite3_shutdown()` 与 `sqlite3_config()` 的实现原文。
- [sqlite.org Fossil 源码树 src/main.c](https://sqlite.org/src/doc/trunk/src/main.c)：SQLite 官方 Fossil 仓库中 `src/main.c` 的在线浏览版。
- [Compile-time Options — sqlite.org](https://sqlite.org/compile.html#omit_autoinit)：`SQLITE_OMIT_AUTOINIT`、`SQLITE_DEFAULT_PCACHE_INITSZ` 等编译期选项的官方说明。
