---
project: sqlite
feature: mutex-and-threading
page_type: feature
schema_version: 1
refs:
  - path: src/mutex.c
    symbols: [sqlite3_mutex_enter, sqlite3_mutex_leave, sqlite3_mutex_alloc, sqlite3MutexInit]
---

# 互斥与多线程

## 一句话职责

给 SQLite 提供一套可替换的互斥原语与三种线程模式（单线程 / 多线程 / 串行化）的选择机制：负责互斥实现方法的初始化、互斥量的分配与进入离开，以及"某条连接要不要自带互斥量"的判定。

## 机制是什么

`src/mutex.c` 不是互斥量的**具体实现**，而是互斥量子系统与 SQLite 其余部分之间的**分发层**。它把 `sqlite3_mutex_methods` 结构体里的函数指针（`xMutexInit` / `xMutexEnd` / `xMutexAlloc` / `xMutexFree` / `xMutexEnter` / `xMutexTry` / `xMutexLeave` / `xMutexHeld` / `xMutexNotheld`）装进全局配置 `sqlite3GlobalConfig.mutex`，然后 `sqlite3_mutex_enter` / `sqlite3_mutex_leave` / `sqlite3_mutex_alloc` 这几个公共 API 只是转发到这组指针。

真正的实现按平台分文件：

- `src/mutex_unix.c` 的 `sqlite3DefaultMutex`：在 `SQLITE_MUTEX_PTHREADS` 下返回基于 `pthread_mutex_t` 的方法集。
- `src/mutex_w32.c` 的 `sqlite3DefaultMutex`：在 `SQLITE_MUTEX_W32` 下返回基于 Win32 原语的方法集。
- `src/mutex_noop.c` 的 `sqlite3NoopMutex`：**不做任何互斥**的调试版实现（用 `cnt` 计数代替真锁），同时也是 `SQLITE_MUTEX_NOOP` 编译下 `sqlite3DefaultMutex` 的返回值。

平台选择由 `src/mutex.h` 的宏推导决定：`!SQLITE_THREADSAFE` → `SQLITE_MUTEX_OMIT`；否则 `SQLITE_OS_UNIX || __CYGWIN__` → `SQLITE_MUTEX_PTHREADS`；`SQLITE_OS_WIN` → `SQLITE_MUTEX_W32`；其余 → `SQLITE_MUTEX_NOOP`。

**三种线程模式**由两个布尔量组合表达，存放在全局配置 `sqlite3GlobalConfig` 里：

- `bCoreMutex`：是否启用**核心级**互斥（保护库级共享状态，如全局互斥量、共享缓存、页缓存组）。
- `bFullMutex`：是否启用**连接级**互斥（保护单个 `sqlite3` 连接）。

`src/main.c` 的 `sqlite3_config` 把 `SQLITE_CONFIG_SINGLETHREAD` / `SQLITE_CONFIG_MULTITHREAD` / `SQLITE_CONFIG_SERIALIZED` 三个选项分别映射为 `(bCoreMutex, bFullMutex)` 的 `(0,0)` / `(1,0)` / `(1,1)`。`src/global.c` 里 `sqlite3Config` 的初始值是 `bCoreMutex = 1`、`bFullMutex = SQLITE_THREADSAFE==1`，而 `src/sqliteInt.h` 在未显式定义时把 `SQLITE_THREADSAFE` 默认为 `1`。

**边界**：本模块只负责互斥模式的配置、进入与离开，以及线程模式的选择；**不负责 WAL 的读写锁**（那是 `src/wal.c` 的读标记与写锁机制，属于 H 组），**不负责文件锁**（那是 Pager / VFS 层的 `fcntl` / `LockFile` 调用，属于 G06）。这两类锁是"跨进程 / 跨机器"的持久化层同步，与这里的"同一进程内多线程"互斥是不同层次。

## 怎么实现的

### 编译期裁剪：SQLITE_MUTEX_OMIT

`src/mutex.h` 在 `SQLITE_MUTEX_OMIT` 下把整套互斥 API 变成宏，**连桩函数都不生成**：

```c
#define sqlite3_mutex_alloc(X)    ((sqlite3_mutex*)8)
#define sqlite3_mutex_free(X)
#define sqlite3_mutex_enter(X)
#define sqlite3_mutex_try(X)      SQLITE_OK
#define sqlite3_mutex_leave(X)
#define sqlite3_mutex_held(X)     ((void)(X),1)
#define sqlite3_mutex_notheld(X)  ((void)(X),1)
#define sqlite3MutexAlloc(X)      ((sqlite3_mutex*)8)
#define sqlite3MutexInit()        SQLITE_OK
#define sqlite3MutexEnd()
#define MUTEX_LOGIC(X)
```

注意 `sqlite3_mutex_alloc` 在这个模式下返回**固定指针 `((sqlite3_mutex*)8)`**，不是 NULL——这样 `if(p)` 形式的判空在有无互斥两种构建下行为一致。同一个文件里的 `MUTEX_LOGIC(X)` 宏在 OMIT 模式下展开为空，非 OMIT 模式展开为 `X`，用来把"需要互斥的那段语句"整块裁掉。

### 初始化：sqlite3MutexInit

`src/mutex.c` 的 `sqlite3MutexInit` 是这套子系统的入口，逻辑分两支：

**分支一：用户没有通过 `sqlite3_config(SQLITE_CONFIG_MUTEX, ...)` 安装自定义实现**（判据是 `sqlite3GlobalConfig.mutex.xMutexAlloc` 为空）。此时它按 `bCoreMutex` 选一套默认方法：

- `bCoreMutex` 为真 → `pFrom = sqlite3DefaultMutex()`（`src/mutex_unix.c` 或 `src/mutex_w32.c` 的真锁实现）。
- `bCoreMutex` 为假 → `pFrom = sqlite3NoopMutex()`（`src/mutex_noop.c`，不提供真锁）。

但如果编译时定义了 `SQLITE_THREAD_MISUSE_WARNINGS`，且 `bCoreMutex` 为真，则 `pFrom = multiThreadedCheckMutex()` —— `src/mutex.c` 里的包装实现，会在检测到违规并发时告警。

选定后把 8 个函数指针逐个拷进 `sqlite3GlobalConfig.mutex`（`xMutexInit` / `xMutexEnd` / `xMutexFree` / `xMutexEnter` / `xMutexTry` / `xMutexLeave` / `xMutexHeld` / `xMutexNotheld`），**`xMutexAlloc` 放在最后单独赋值**，中间夹一次 `sqlite3MemoryBarrier()`。

**分支二：`xMutexAlloc` 已被设置**（另一个线程刚刚装好了默认方法）。此时函数体只做一次 `sqlite3MemoryBarrier()` 就返回。

`sqlite3MemoryBarrier()` 定义在 `src/mutex_unix.c`：优先用编译期宏 `SQLITE_MEMORY_BARRIER`，否则在 `__GNUC__` 且 `GCC_VERSION>=4001000` 时用 `__sync_synchronize()`。

这两条分支的顺序**不能颠倒**，否则弱内存序处理器上会出现"读到已初始化的 `xMutexInit` 但还没看到 `xMutexAlloc`"的窗口。源码注释把这个配对关系写在了 `else` 分支里。

最后 `sqlite3MutexInit` 调用 `sqlite3GlobalConfig.mutex.xMutexInit()` 并返回其结果，同时在 `SQLITE_DEBUG` 下把 `mutexIsInit` 置 1。

**调用方**：`src/main.c` 的 `sqlite3_initialize` 在最前面调 `sqlite3MutexInit()`，返回非 0 则整个初始化直接返回错误码。

### 关闭：sqlite3MutexEnd

`src/mutex.c` 的 `sqlite3MutexEnd` 在 `sqlite3GlobalConfig.mutex.xMutexEnd` 非空时调它并返回其结果，同时在 `SQLITE_DEBUG` 下把 `mutexIsInit` 置 0。

**调用方**：`src/main.c` 的 `sqlite3_shutdown` 在 `sqlite3GlobalConfig.isMutexInit` 为真时调 `sqlite3MutexEnd()`。

### 分配：sqlite3_mutex_alloc

`src/mutex.c` 的 `sqlite3_mutex_alloc(id)` 分两层：

**第一层（自动初始化）**：在 `SQLITE_OMIT_AUTOINIT` 未定义时，

- `id <= SQLITE_MUTEX_RECURSIVE`（即 `SQLITE_MUTEX_FAST`=0 或 `SQLITE_MUTEX_RECURSIVE`=1）→ 调 `sqlite3_initialize()`；失败返回 0。
- `id > SQLITE_MUTEX_RECURSIVE`（静态互斥量，编号 ≥ 2）→ 调 `sqlite3MutexInit()`；失败返回 0。

**第二层**：调 `sqlite3GlobalConfig.mutex.xMutexAlloc(id)` 并返回结果。

对比 `src/mutex.c` 里的内部版本 `sqlite3MutexAlloc(id)`：它**不做自动初始化**，但在 `!sqlite3GlobalConfig.bCoreMutex` 时**直接返回 0**（单线程模式下不分配任何互斥量），并且断言 `mutexIsInit` 为真。

**静态互斥量的分配路径**（在 `src/mutex_unix.c` 的 `pthreadMutexAlloc`）：编号 ≥ 2 的类型走 `default` 分支，返回静态数组 `aMutex` 中第 `iType-2` 项的地址。该数组是 `static ALIGN128 union staticMutex { sqlite3_mutex m; char aSpacer[128]; } aMutex[]`，共 **12 个元素**，对应编号 2 到 13。`ALIGN128` 在 GCC 下是 `__attribute__((aligned(128)))`，源码注释说明用意是让相邻互斥量落在不同 cache line 上，避免伪共享。

`SQLITE_MUTEX_FAST` 与 `SQLITE_MUTEX_RECURSIVE` 则各自 `sqlite3MallocZero(sizeof(*p))` 分配一个**新**互斥量——同类常量每次调用返回不同对象。递归型的做法是设 `pthread_mutexattr_settype(&recursiveAttr, PTHREAD_MUTEX_RECURSIVE)` 后再 `pthread_mutex_init`；若平台没有递归互斥量，可以定义 `SQLITE_HOMEGROWN_RECURSIVE_MUTEX`，此时改为自己维护 `nRef` 计数与 `owner` 线程 ID。

静态互斥量的编号来自 `src/sqlite.h.in`：`SQLITE_MUTEX_FAST`=0、`SQLITE_MUTEX_RECURSIVE`=1、`SQLITE_MUTEX_STATIC_MAIN`=2、`SQLITE_MUTEX_STATIC_MEM`=3、`SQLITE_MUTEX_STATIC_OPEN`=4、`SQLITE_MUTEX_STATIC_PRNG`=5、`SQLITE_MUTEX_STATIC_LRU`=6、`SQLITE_MUTEX_STATIC_PMEM`=7、`SQLITE_MUTEX_STATIC_APP1`=8、`SQLITE_MUTEX_STATIC_APP2`=9、`SQLITE_MUTEX_STATIC_APP3`=10、`SQLITE_MUTEX_STATIC_VFS1`=11、`SQLITE_MUTEX_STATIC_VFS2`=12、`SQLITE_MUTEX_STATIC_VFS3`=13。`src/sqliteInt.h` 里另有 `SQLITE_MUTEX_STATIC_TEMPDIR` 定义为 `SQLITE_MUTEX_STATIC_VFS1`（与 VFS1 共用同一把锁），以及兼容别名 `SQLITE_MUTEX_STATIC_MASTER`=2、`SQLITE_MUTEX_STATIC_MEM2`=4、`SQLITE_MUTEX_STATIC_LRU2`=7。

### 进入与离开：sqlite3_mutex_enter / sqlite3_mutex_leave

`src/mutex.c` 的 `sqlite3_mutex_enter(p)`：参数为 NULL 时**直接返回，什么都不做**（非 NULL 时断言 `xMutexEnter` 存在，然后转发 `sqlite3GlobalConfig.mutex.xMutexEnter(p)`）。

`sqlite3_mutex_leave(p)`：同样是 NULL 参数即 no-op，否则转发 `xMutexLeave(p)`。

`sqlite3_mutex_try(p)`：NULL 参数返回 `SQLITE_OK`（初值 `rc = SQLITE_OK`），非 NULL 时返回 `xMutexTry(p)` 的结果。

**平台实现的差异**（`src/mutex_unix.c` 的 `pthreadMutexEnter` / `pthreadMutexTry` / `pthreadMutexLeave`）：

- `pthreadMutexEnter` 走 `pthread_mutex_lock`；`SQLITE_HOMEGROWN_RECURSIVE_MUTEX` 下则先比较 `p->owner` 与 `pthread_self()`，同一线程则 `p->nRef++` 直接返回，否则加锁并记录 owner。
- `pthreadMutexTry` 走 `pthread_mutex_trylock`，成功返回 `SQLITE_OK`，失败返回 `SQLITE_BUSY`。
- `pthreadMutexLeave` 走 `pthread_mutex_unlock`；自实现递归锁下只有 `nRef` 归 0 时才真正解锁。

**平台实现的差异**（`src/mutex_w32.c` 的 `winMutexEnter` / `winMutexTry` / `winMutexLeave`）：递归型用 `InitializeCriticalSection` / `EnterCriticalSection` / `LeaveCriticalSection`，非递归型用 `InitializeSRWLock` / `AcquireSRWLockExclusive` / `ReleaseSRWLockExclusive`。`winMutexTry` 用 `TryEnterCriticalSection` / `TryAcquireSRWLockExclusive`，成功返回 `SQLITE_OK`，失败返回 `SQLITE_BUSY`。

### 持有性断言：sqlite3_mutex_held / sqlite3_mutex_notheld

`src/mutex.c` 里这两个函数包在 `#ifndef NDEBUG` 中，只用于 `assert()` 语句内。它们在 `__has_feature(thread_sanitizer)` 成立时把 `p` 置 0，从而**总是返回 1**——源码注释说明原因是 TSAN 下这两个检查会产生假阳性告警。

### 违规并发检测：checkMutex 系列

`src/mutex.c` 在 `SQLITE_THREAD_MISUSE_WARNINGS` 下提供一套包装实现（`checkMutexInit` / `checkMutexEnd` / `checkMutexAlloc` / `checkMutexFree` / `checkMutexEnter` / `checkMutexTry` / `checkMutexLeave`），由 `multiThreadedCheckMutex()` 组装成 `sqlite3_mutex_methods` 返回。

`checkMutexAlloc` 的静态互斥量表 `staticMutexes[]` 有 **12 项**，对应编号 2 到 13，编号 < 2 的（FAST / RECURSIVE）走 `sqlite3MallocZero` 动态分配。

`checkMutexEnter` 的检测逻辑：若该互斥量的 `iType` 是 `SQLITE_MUTEX_WARNONCONTENTION`（宏值 `-1`），则先尝试 `xMutexTry`；成功就直接返回，失败则调 `sqlite3_log(SQLITE_MISUSE, "illegal multi-threaded access to database connection")`；若还定义了 `SQLITE_THREAD_MISUSE_ABORT`，紧接着调 `abort()`。无论是否检测到竞争，最后都会执行 `pGlobalMutexMethods->xMutexEnter(pCheck->mutex)`。

标记某个互斥量需要告警的入口是 `sqlite3MutexWarnOnContention(p)`：它先检查当前方法集是否就是 `checkMutexAlloc`（`sqlite3GlobalConfig.mutex.xMutexAlloc==checkMutexAlloc`），是则断言 `pCheck->iType==SQLITE_MUTEX_RECURSIVE` 并把 `iType` 改成 `SQLITE_MUTEX_WARNONCONTENTION`。

**调用方**：`src/main.c` 的 `sqlite3_open_v2` 在分配了连接级互斥量但判定为"非线程安全连接"时调 `sqlite3MutexWarnOnContention(db->mutex)`。

### 连接级互斥量的分配决策

`src/main.c` 的 `sqlite3_open_v2` 里，决定连接是否自带互斥量的判定顺序是：

1. `sqlite3GlobalConfig.bCoreMutex==0` → `isThreadsafe = 0`。
2. `flags & SQLITE_OPEN_NOMUTEX` → `isThreadsafe = 0`。
3. `flags & SQLITE_OPEN_FULLMUTEX` → `isThreadsafe = 1`。
4. 否则 → `isThreadsafe = sqlite3GlobalConfig.bFullMutex`。

两个标志的位值来自 `src/sqlite.h.in`：`SQLITE_OPEN_NOMUTEX` = `0x00008000`，`SQLITE_OPEN_FULLMUTEX` = `0x00010000`。

判定为需要互斥量时（`isThreadsafe` 为真，或定义了 `SQLITE_THREAD_MISUSE_WARNINGS` 且 `bCoreMutex` 为真），`src/main.c` 调 `sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE)` 分配连接级互斥量；分配失败则 `sqlite3_free(db)` 并以 `SQLITE_NOMEM` 返回。若 `isThreadsafe==0` 但仍然分配了互斥量（`SQLITE_THREAD_MISUSE_WARNINGS` 路径），则紧接调 `sqlite3MutexWarnOnContention(db->mutex)`。

### 三种模式的实际效果

`src/sqlite.h.in` 的 `sqlite3_open_v2` 文档给出官方语义：`SQLITE_OPEN_NOMUTEX` 让新连接使用 "multi-thread" 模式——允许不同线程同时使用 SQLite，前提是每个线程用不同的连接；`SQLITE_OPEN_FULLMUTEX` 让新连接使用 "serialized" 模式——多个线程可以安全地同时使用同一个连接。

`src/sqliteInt.h` 里 `SQLITE_THREADSAFE` 的三种取值语义也写明了：0 表示互斥永久禁用、库永不安全；1 表示串行化，是最高线程安全级别；2 表示多线程——只要没有两个线程同时使用同一个连接即可。默认值在 `src/sqliteInt.h` 中为 `1`（`#if !defined(SQLITE_THREADSAFE)` 分支里，若定义了旧的 `THREADSAFE` 宏则沿用它）。

`src/main.c` 的 `sqlite3_threadsafe()` 直接 `return SQLITE_THREADSAFE`，即返回**编译期**值，不是运行期配置。

### 静态互斥量的实际持有者

以下调用点可查（均为 `sqlite3MutexAlloc` / `sqlite3_mutex_alloc`）：

- `src/main.c` 的 `sqlite3_initialize`：`SQLITE_MUTEX_STATIC_MAIN` 保护 malloc 子系统初始化，并另外分配一把 `SQLITE_MUTEX_RECURSIVE` 作为 `pInitMutex`；`pInitMutex` 分配失败且 `bCoreMutex` 为真时返回 `SQLITE_NOMEM`。
- `src/btree.c`：`SQLITE_MUTEX_STATIC_OPEN` 保护 `sqlite3BtreeOpen`，`SQLITE_MUTEX_STATIC_MAIN` 保护共享缓存链表，`SQLITE_MUTEX_FAST` 分配为 B-tree 自己的互斥量。
- `src/mem2.c` / `src/mem3.c` / `src/mem5.c` / `src/malloc.c`：`SQLITE_MUTEX_STATIC_MEM` 保护分配器。
- `src/pcache1.c`：`SQLITE_MUTEX_STATIC_LRU` 保护 LRU 页列表，`SQLITE_MUTEX_STATIC_PMEM` 保护页内存分配。
- `src/random.c`：`SQLITE_MUTEX_STATIC_PRNG` 保护随机数发生器。
- `src/date.c` / `src/os.c` / `src/notify.c` / `src/loadext.c`：`SQLITE_MUTEX_STATIC_MAIN` 保护全局共享状态。
- `src/os_unix.c` / `src/os_win.c`：`SQLITE_MUTEX_STATIC_TEMPDIR`（等于 `SQLITE_MUTEX_STATIC_VFS1`）保护临时目录名；`src/os_unix.c` 与 `src/os_win.c` 各自分配 `SQLITE_MUTEX_STATIC_VFS1` / `SQLITE_MUTEX_STATIC_VFS2`。
- `src/memdb.c`：`SQLITE_MUTEX_STATIC_VFS1` 保护内存数据库的 VFS 对象。

### 回退路径

`sqlite3MutexInit` 在 `bCoreMutex` 为假时**回退到 `sqlite3NoopMutex()`**（`src/mutex_noop.c` 的 `sqlite3NoopMutex`），此时 `sqlite3MutexAlloc` 因为 `!sqlite3GlobalConfig.bCoreMutex` 直接返回 0，`src/mutex.h` 的 `MUTEX_LOGIC(X)` 又把所有 `sqlite3MutexAlloc` 调用点裁成空语句——**单线程模式下互斥量被彻底省略，而不是退化为无锁的真锁**。

`src/mutex_noop.c` 里 `sqlite3NoopMutex()` 在 `#ifdef SQLITE_MUTEX_NOOP` 下被 `sqlite3DefaultMutex()` 直接返回——即编译成 `SQLITE_MUTEX_NOOP` 时，"默认实现"就是"不做互斥"。

`src/mutex_unix.c` 的 `pthreadMutexAlloc` 在平台缺递归互斥量时**回退到自实现递归互斥**（`SQLITE_HOMEGROWN_RECURSIVE_MUTEX` 分支），由 `pthreadMutexEnter` / `pthreadMutexTry` / `pthreadMutexLeave` 用 `nRef` 与 `owner` 字段模拟；`src/mutex_unix.c` 的 `pthreadMutexAlloc` 在 `SQLITE_MUTEX_RECURSIVE` 分支就是这条回退路径的入口。

## 关键取舍

- **用函数指针表而不是直接调用平台原语**。**源码事实：**`src/mutex.c` 的 `sqlite3_mutex_enter` / `sqlite3_mutex_leave` / `sqlite3_mutex_alloc` 全部转发到 `sqlite3GlobalConfig.mutex.xMutex*`；`src/main.c` 的 `sqlite3_config` 支持 `SQLITE_CONFIG_MUTEX`（值为 10）与 `SQLITE_CONFIG_GETMUTEX`（值为 11）来替换 / 查询这组方法。**架构推论：**代价是每次加解锁多一次间接调用与一次判空；换来的是应用可以整体替换互斥实现（官方文档说明这是为性能剖析或测试用途预留的），且 SQLite 内部从不直接依赖 `pthread` 或 Win32 原语。
- **线程模式降级为两个布尔量**。**源码事实：**`src/global.c` 的 `sqlite3Config` 只有 `bCoreMutex` 与 `bFullMutex` 两个字段承载线程模式；`src/main.c` 的 `sqlite3_config` 把三种 `SQLITE_CONFIG_*` 映射到这两个字段的四种取值组合。**架构推论：**`bCoreMutex` 单独存在，是为了支持"库级串行化 + 连接级并发"这一 SQLite 未对外暴露的组合——它由 `SQLITE_CONFIG_MULTITHREAD`（`(1,0)`）表达，而 `bFullMutex` 才决定单条连接是否自带互斥量。
- **静态互斥量复用固定对象而非每次新建**。**源码事实：**`src/mutex_unix.c` 的 `pthreadMutexAlloc` 对编号 ≥ 2 的类型返回 `aMutex[iType-2]`；`src/sqlite.h.in` 文档写明静态互斥量集合"可能随 SQLite 版本变化"，同一类型号每次返回同一把锁。**架构推论：**避免为每个内存分配器 / 每个 VFS 各建一把锁，代价是这把锁成了全局串行点——所有 `sqlite3_malloc` 调用都要过 `SQLITE_MUTEX_STATIC_MEM`。
- **静态互斥量按 128 字节对齐防伪共享**。**源码事实：**`src/mutex_unix.c` 的 `aMutex` 是 `ALIGN128 union staticMutex { sqlite3_mutex m; char aSpacer[128]; }` 数组，`ALIGN128` 在 GCC 下为 `__attribute__((aligned(128)))`；源码注释明确说明"128 字节对齐对当代处理器都合适，实验显示 64 多数时候也行，但网上说 128 更好"。**架构推论：**在高频争用的场景下把不同锁摊到不同 cache line，省掉 cache line 乒乓。
- **单线程模式是彻底裁剪而不是退化**。**源码事实：**`src/mutex.h` 在 `SQLITE_MUTEX_OMIT` 下把 `sqlite3_mutex_alloc` 定义成 `((sqlite3_mutex*)8)`、`sqlite3_mutex_enter` 定义成空、`MUTEX_LOGIC(X)` 定义成空；`src/mutex.c` 的 `sqlite3MutexAlloc` 在 `!bCoreMutex` 时返回 0。**架构推论：**单线程构建里一行锁代码都不生成，省掉的是指令与分支，不是"少一次原子操作"——代价是多线程安全彻底不存在，编译期就必须定死。
- **`sqlite3_mutex_enter(NULL)` 是 no-op 而不是崩溃**。**源码事实：**`src/mutex.c` 的 `sqlite3_mutex_enter` / `sqlite3_mutex_leave` / `sqlite3_mutex_try` 都有 `if(p)` 前置判断；`src/mutex.h` 在 OMIT 模式下把 `sqlite3_mutex_alloc` 定义为固定非空指针 `((sqlite3_mutex*)8)`。**架构推论：**调用方无需在"有没有互斥"两种构建下写两套判空逻辑，`src/main.c` 里成对的 `sqlite3_mutex_enter(db->mutex)` / `sqlite3_mutex_leave(db->mutex)` 在两种构建下都能原样编译。
- **`sqlite3_mutex_try` 失败返回 `SQLITE_BUSY` 而非 `SQLITE_ERROR`**。**源码事实：**`src/mutex_unix.c` 的 `pthreadMutexTry` 与 `src/mutex_w32.c` 的 `winMutexTry` 在争用失败时都返回 `SQLITE_BUSY`。**架构推论：**与 SQLite 其余部分的"忙"语义统一，调用方可把 `SQLITE_BUSY` 直接当"稍后重试"处理，而不必区分锁竞争与其他错误。
- **TSAN 下关闭持有性断言**。**源码事实：**`src/mutex.c` 的 `sqlite3_mutex_held` / `sqlite3_mutex_notheld` 在 `__has_feature(thread_sanitizer)` 成立时把 `p` 置 0，因而恒返回 1；源码注释写明原因是"这两个例程在 TSAN 下会产生假阳性告警"。**架构推论：**牺牲断言换取 ThreadSanitizer 报告的干净度——否则 TSAN 会把 SQLite 自己的调试断言报成数据竞争。
- **违规并发检测是编译期可选的包装层**。**源码事实：**`src/mutex.c` 的 `checkMutex*` 系列只在 `SQLITE_THREAD_MISUSE_WARNINGS` 下编译，`checkMutexEnter` 在检测到竞争时调 `sqlite3_log(SQLITE_MISUSE, ...)`，仅当另外定义了 `SQLITE_THREAD_MISUSE_ABORT` 才 `abort()`；`sqlite3MutexWarnOnContention` 只在当前方法集就是 `checkMutexAlloc` 时才生效。**架构推论：**把"应用误用同一个连接"从静默的数据竞争变成可观测的日志，但只在测试构建里付出这份包装开销。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/mutex.c | sqlite3MutexInit | 互斥子系统初始化入口：未装自定义实现时按 `bCoreMutex` 选 `sqlite3DefaultMutex()` 或 `sqlite3NoopMutex()`，把 8 个方法指针拷进 `sqlite3GlobalConfig.mutex`，`xMutexAlloc` 最后赋值并夹 `sqlite3MemoryBarrier()`。 |
| src/mutex.c | sqlite3MutexEnd | 关闭互斥子系统：调 `xMutexEnd()` 并在 `SQLITE_DEBUG` 下清 `mutexIsInit`；由 `src/main.c` 的 `sqlite3_shutdown` 调用。 |
| src/mutex.c | sqlite3_mutex_alloc | 公共分配入口：动态类型（FAST/RECURSIVE）先走 `sqlite3_initialize()`，静态类型（编号 ≥ 2）先走 `sqlite3MutexInit()`，再转发 `xMutexAlloc`。 |
| src/mutex.c | sqlite3MutexAlloc | 内部分配入口：`!bCoreMutex` 时直接返回 0，否则转发 `xMutexAlloc`，并断言 `mutexIsInit`。 |
| src/mutex.c | sqlite3_mutex_enter | 进入互斥量：NULL 参数 no-op，否则转发 `xMutexEnter`；由 `src/main.c`、`src/vdbeapi.c`、`src/prepare.c` 等在 `db->mutex` 上成对调用。 |
| src/mutex.c | sqlite3_mutex_leave | 离开互斥量：NULL 参数 no-op，否则转发 `xMutexLeave`。 |
| src/mutex.c | sqlite3_mutex_try | 尝试进入互斥量：NULL 参数返回 `SQLITE_OK`，否则返回 `xMutexTry` 的结果（争用失败为 `SQLITE_BUSY`）。 |
| src/mutex.c | sqlite3_mutex_held / sqlite3_mutex_notheld | 仅供 `assert()` 使用的持有性检查；TSAN 下把参数置 0 从而恒返回 1。 |
| src/mutex.c | multiThreadedCheckMutex | `SQLITE_THREAD_MISUSE_WARNINGS` 下的方法集包装，返回组装好的 `sqlite3_mutex_methods`。 |
| src/mutex.c | checkMutexEnter | 违规并发检测：`SQLITE_MUTEX_WARNONCONTENTION` 类型先用 `xMutexTry` 试探，失败则 `sqlite3_log(SQLITE_MISUSE, ...)`，可选 `abort()`，最后仍执行 `xMutexEnter`。 |
| src/mutex.c | sqlite3MutexWarnOnContention | 把一个递归互斥量标记为"出现争用即告警"；由 `src/main.c` 的 `sqlite3_open_v2` 调用。 |
| src/mutex.h | MUTEX_LOGIC / SQLITE_MUTEX_OMIT 分支 | 编译期裁剪：`SQLITE_MUTEX_OMIT` 下 `sqlite3_mutex_alloc` 变成 `((sqlite3_mutex*)8)`、enter/leave 变空、`MUTEX_LOGIC(X)` 展开为空。 |
| src/mutex_unix.c | sqlite3DefaultMutex | Unix 默认方法集（`pthread_mutex_t` 实现）；`pthreadMutexAlloc` 对编号 ≥ 2 返回 `aMutex[iType-2]`，`ALIGN128` 保证 128 字节对齐。 |
| src/mutex_w32.c | sqlite3DefaultMutex | Win32 默认方法集：递归型用 `CRITICAL_SECTION`，非递归型用 `SRWLOCK`。 |
| src/mutex_noop.c | sqlite3NoopMutex | 不提供真锁的调试实现（`cnt` 计数）；`SQLITE_MUTEX_NOOP` 编译下由 `sqlite3DefaultMutex` 直接返回它。 |
| src/main.c | sqlite3_config | 把 `SQLITE_CONFIG_SINGLETHREAD` / `SQLITE_CONFIG_MULTITHREAD` / `SQLITE_CONFIG_SERIALIZED` 映射为 `bCoreMutex` 与 `bFullMutex` 的取值；`SQLITE_CONFIG_MUTEX` 替换方法集。 |
| src/main.c | sqlite3_open_v2 | 决定连接是否自带互斥量：`bCoreMutex` / `SQLITE_OPEN_NOMUTEX` / `SQLITE_OPEN_FULLMUTEX` / `bFullMutex` 四级判定后调 `sqlite3MutexAlloc(SQLITE_MUTEX_RECURSIVE)`。 |
| src/global.c | sqlite3Config | 全局配置初值：`bCoreMutex` = 1，`bFullMutex` = `SQLITE_THREADSAFE==1`。 |
| src/sqliteInt.h | SQLITE_THREADSAFE | 编译期线程安全级别：0 = 关闭互斥，1 = 串行化，2 = 多线程；未定义时默认 1。 |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 有自己的一套并发纪律——`latch_lock()` / `latch_unlock()` 成对出现、显式传 xid 而不藏进共享对象，这与 SQLite 这一层是同类问题的两种答案：SQLite 用**一把库级互斥量 + 每连接一把递归互斥量**的粗粒度方案，主核用**闩锁（latch）+ 事务号显式传递**的细粒度方案。要抄 SQLite 的话，值得抄的是"用 `sqlite3_mutex_methods` 函数指针表把平台差异与调用点解耦"这一条——主核若将来要支持多种 OS，同样需要一层可替换的原语抽象；而 SQLite 的"单线程模式下彻底裁剪锁代码"（`MUTEX_LOGIC(X)` 展开为空）对应主核的 `common/` 编译期开关，也是同一类工程判断。

**架构推论：** 本页可作为 AI 检索 SQLite 并发问题的落点：凡涉及"我的应用多线程下会不会崩"、"一个连接能不能给多个线程用"、"线程模式怎么配"、"互斥量能不能替换"的问题，检索落点应指向 `src/mutex.c` 与 `src/mutex.h`；而涉及"WAL 模式下多线程读写"、"跨进程文件锁"的问题，落点应转向 H 组（`src/wal.c`）与 G06（Pager 锁），不要停在互斥层。

## 参考

- SQLite 官方文档 Threading And The SQLite Library（三种线程模式与 `sqlite3_threadsafe()`）：<https://sqlite.org/threadsafe.html>
- SQLite 官方文档 File Locking And Concurrency In SQLite Version 3（文件锁由 pager 模块负责，与本页的进程内互斥分层）：<https://sqlite.org/lockingv3.html>
- SQLite 官方文档 Write-Ahead Logging（WAL 模式的读写并发由 wal-index 与共享内存锁保证，不在本页的互斥层）：<https://sqlite.org/wal.html>
- SQLite 官方文档 sqlite3_config（`SQLITE_CONFIG_MUTEX` / `SQLITE_CONFIG_GETMUTEX` 与线程模式选项）：<https://sqlite.org/c3ref/config.html>
- SQLite 官方 Fossil 源码 `src/mutex.c`（trunk）：<https://sqlite.org/src/file?name=src/mutex.c&ci=trunk>
- SQLite 官方 Fossil 源码 `src/mutex.h`（trunk，编译期裁剪宏）：<https://sqlite.org/src/file?name=src/mutex.h&ci=trunk>
- GitHub 源码镜像 `src/mutex.c`：<https://github.com/sqlite/sqlite/blob/master/src/mutex.c>
- GitHub 源码镜像 `src/mutex_unix.c`：<https://github.com/sqlite/sqlite/blob/master/src/mutex_unix.c>
- GitHub 源码镜像 `src/mutex_w32.c`：<https://github.com/sqlite/sqlite/blob/master/src/mutex_w32.c>
