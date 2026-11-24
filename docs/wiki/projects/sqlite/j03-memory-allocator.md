---
project: sqlite
feature: memory-allocator
page_type: feature
schema_version: 1
refs:
  - path: src/malloc.c
    symbols: [sqlite3MallocInit, sqlite3MallocSize, sqlite3DbMallocSize, sqlite3HeapNearlyFull, sqlite3_soft_heap_limit64, sqlite3_hard_heap_limit64]
---

# 内存分配器

## 一句话职责

给全库提供统一的内存申请封装：把每次申请的尺寸记账到 `SQLITE_STATUS_MEMORY_USED` / `SQLITE_STATUS_MALLOC_COUNT` 统计里，并依据软/硬堆上限在临界点主动释放页缓存或直接拒绝分配。

## 机制是什么

这一层是 SQLite 内部内存的**策略层**，夹在业务代码与底层分配器之间，业务侧调用 `sqlite3Malloc()` / `sqlite3DbMallocRaw()` / `sqlite3Realloc()`，实际分配动作通过 `sqlite3GlobalConfig.m` 里的函数指针（`xMalloc` / `xFree` / `xRealloc` / `xSize` / `xRoundup` / `xInit` / `xShutdown`）转发到系统 malloc（`src/mem1.c`）或应用自定义实现（经 `sqlite3_config(SQLITE_CONFIG_MALLOC, ...)` 注册）。

它本身负责三件事：

1. **封装与尺寸统计**：`mallocWithAlarm` 先用 `xRoundup` 把请求尺寸向上取整（`src/mem1.c` 的 `sqlite3MemRoundup` 实现为 `ROUND8(n)`），申请成功后用 `xSize` 回读真实占用，分别累加到 `SQLITE_STATUS_MEMORY_USED` 和 `SQLITE_STATUS_MALLOC_COUNT` 两个状态计数器上。
2. **堆上限**：`sqlite3_soft_heap_limit64` 设软上限（触到就触发释放），`sqlite3_hard_heap_limit64` 设硬上限（触到就返回空指针制造 OOM）。硬上限默认是编译期常量 `SQLITE_MAX_MEMORY`，未定义时取 `0`，`0` 的含义是"无上限"。
3. **压力信号**：`sqlite3HeapNearlyFull` 返回一个原子标志 `mem0.nearlyFull`，供页缓存与外排排序器判断是否处于内存压力下。

## 怎么实现的

### 初始化

`sqlite3MallocInit` 做三件事：

- 若 `sqlite3GlobalConfig.m.xMalloc` 仍为 0（即应用没有注册自己的分配器），调用 `sqlite3MemSetDefault()` 装上默认实现。
- 用 `sqlite3MutexAlloc(SQLITE_MUTEX_STATIC_MEM)` 申请内存子系统的静态互斥量 `mem0.mutex`，此后所有统计与上限检查都在它保护下进行。
- 校验页缓存预分配参数：`sqlite3GlobalConfig.pPage==0 || szPage<512 || nPage<=0` 任一成立就把 `pPage` 与 `szPage` 双双清零（`src/main.c` 的 `sqlite3PCacheBufferSetup` 消费这三个值）。
- 最后调用 `xInit(pAppData)`；返回非 `SQLITE_OK` 时 `memset(&mem0, 0, sizeof(mem0))` 把整个子系统状态清零。

### 申请路径

`sqlite3Malloc(n)` 的分支：

- `n==0` 或 `n>SQLITE_MAX_ALLOCATION_SIZE` 时直接返回空指针。`SQLITE_MAX_ALLOCATION_SIZE` 定义在 `src/sqliteLimit.h`，默认 `2147483391`，且该文件里有一条 `#error` 断言其上界不得超过 `2147483391`。
- 若 `sqlite3GlobalConfig.bMemstat` 为真（统计开启，由 `sqlite3_config(SQLITE_CONFIG_MEMSTATUS, ...)` 决定），进入互斥量后调用 `mallocWithAlarm`；否则直接 `xMalloc((int)n)`，完全跳过统计与上限逻辑。

`mallocWithAlarm` 里的顺序是：

1. `nFull = xRoundup(n)` 得到实际申请尺寸。
2. `sqlite3StatusHighwater(SQLITE_STATUS_MALLOC_SIZE, n)` 记录单次申请尺寸的高水位。
3. 若 `mem0.alarmThreshold>0`（软上限已启用）：读 `nUsed = sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)`，判断 `nUsed >= mem0.alarmThreshold - nFull`。成立则把 `nearlyFull` 置 1 并调用 `sqlite3MallocAlarm(nFull)`；随后若 `mem0.hardLimit` 非 0，再判断 `nUsed >= mem0.hardLimit - nFull`，成立就直接 `*pp = 0` 返回失败。不成立（未到软上限）则把 `nearlyFull` 置 0。
4. 真正调用 `xMalloc(nFull)`。仅在定义了 `SQLITE_ENABLE_MEMORY_MANAGEMENT` 时，若首次申请返回 0 且软上限有效，会先 `sqlite3MallocAlarm(nFull)` 释放内存再重试一次。
5. 成功时用 `sqlite3MallocSize(p)` 回读尺寸，累加 `SQLITE_STATUS_MEMORY_USED`，并给 `SQLITE_STATUS_MALLOC_COUNT` 加 1。

`sqlite3MallocAlarm` 在 `alarmThreshold<=0` 时直接返回；否则先 `sqlite3_mutex_leave(mem0.mutex)`，调用 `sqlite3_release_memory(nByte)`，再重新进入互斥量。这是**先释放互斥量再释放内存**的顺序，避免释放路径里再次进入需要同一把锁的代码。

### 释放路径

`sqlite3_free(p)`：`p==0` 时直接返回；统计开启时进入互斥量，先 `sqlite3StatusDown(SQLITE_STATUS_MEMORY_USED, sqlite3MallocSize(p))` 扣账，再 `sqlite3StatusDown(SQLITE_STATUS_MALLOC_COUNT, 1)` 减计数，然后 `xFree(p)`。

### 软上限的设置与副作用

`sqlite3_soft_heap_limit64(n)`：

- 负参数是"只查询"语义：加锁读出 `mem0.alarmThreshold` 后原样返回。
- 若硬上限有效（`mem0.hardLimit>0`）且 `n` 超过硬上限或 `n==0`，把 `n` 强制收敛到 `mem0.hardLimit`。也就是说软上限在硬上限存在时**无法被关闭**，也不能高于硬上限。
- 设置后立刻 `AtomicStore(&mem0.nearlyFull, n>0 && n<=nUsed)`，其中 `nUsed` 来自 `sqlite3StatusValue(SQLITE_STATUS_MEMORY_USED)`。
- 出锁后计算 `excess = sqlite3_memory_used() - n`，`excess>0` 时调用 `sqlite3_release_memory((int)(excess & 0x7fffffff))` 立刻回收超额部分。

`sqlite3_soft_heap_limit(int n)` 是 32 位包装：`n<0` 先归 0，再调 `sqlite3_soft_heap_limit64(n)`。

### 硬上限的设置

`sqlite3_hard_heap_limit64(n)`：负参数同样只查询。`n>=0` 时写 `mem0.hardLimit = n`；并且若 `n<mem0.alarmThreshold` 或软上限当前为 0，就把软上限一并压到 `n`。**设置硬上限会自动激活软上限**，并把软上限约束在硬上限之内。

### 尺寸查询的两个入口

- `sqlite3MallocSize(p)`：断言该指针属于 `MEMTYPE_HEAP`，直接返回 `xSize(p)`。
- `sqlite3DbMallocSize(db, p)`：先判断指针是否落在 `db->lookaside` 区间内。落在 `pMiddle` 之上（两尺寸 lookaside 的小池）返回常量 `LOOKASIDE_SMALL`；落在 `pStart` 到 `pMiddle` 之间返回 `db->lookaside.szTrue`；都不落在 lookaside 区间才回落 `xSize(p)`。`LOOKASIDE_SMALL` 定义在 `src/sqliteInt.h`，在两尺寸 lookaside 打开时为 `128`，编译期定义 `SQLITE_OMIT_TWOSIZE_LOOKASIDE` 时为 `0`。`lookasideMallocSize` 是 `sqlite3DbRealloc` 的辅助函数，用同一套区间判断取旧块长度。

### 压力信号的下游

`sqlite3HeapNearlyFull()` 只做一件事：`return AtomicLoad(&mem0.nearlyFull);`。

两处真实调用方：

- `src/pcache1.c` 的 `pcache1UnderMemoryPressure`：当页缓存条目比预分配的 `pcache1.szSlot` 小时读自己的 `bUnderPressure` 标志，否则返回 `sqlite3HeapNearlyFull()`。
- `src/vdbesort.c` 的排序器 PMA（Priority Memory Allocation）扩容判定：条件之一是 `sqlite3HeapNearlyFull()` 返回真。

### 释放内存的实际去处

`sqlite3_release_memory(n)` 在定义了 `SQLITE_ENABLE_MEMORY_MANAGEMENT` 时转发给 `sqlite3PcacheReleaseMemory(n)`（`src/pcache1.c`），由它从 LRU 链尾部摘除未固定的页并累加释放字节数；未定义该宏时 `sqlite3_release_memory` 是返回 0 的空操作。

### 尺寸重整

`sqlite3Realloc(pOld, nBytes)`：`pOld==0` 等价于 `sqlite3Malloc(nBytes)`；`nBytes==0` 等价于 `sqlite3_free(pOld)` 并返回 0；`nBytes>SQLITE_MAX_ALLOCATION_SIZE` 返回 0。新尺寸经 `xRoundup` 得到 `nNew`，若 `nOld==nNew` 直接复用原指针；否则在互斥量下先比较 `nDiff = nNew - nOld` 与软/硬上限，超硬上限时直接返回 0（失败时调 `test_oom_breakpoint(1)`，仅 `SQLITE_DEBUG` 构建下有效）。`xRealloc` 返回空且软上限有效时，同样先 `sqlite3MallocAlarm` 再重试一次。

## 关键取舍

- **统计可关闭**：`sqlite3GlobalConfig.bMemstat` 一关，`sqlite3Malloc` 就绕过整个 `mallocWithAlarm` 路径，直接调 `xMalloc`，连上限检查都不做。代价是失去内存计量能力与硬上限保护，换来热路径上少一次互斥量进出。这是用"可观测性"换吞吐的典型取舍。
- **超限不是直接失败，而是先尝试自救**：`sqlite3MallocAlarm` 在触及软上限时先释放页缓存（`sqlite3_release_memory`），只有释放后仍越过硬上限才让 `mallocWithAlarm` 返回空指针。软/硬两级因此不是简单的阈值重复，而是"先回收、再拒绝"。
- **硬上限会绑定软上限**：`sqlite3_hard_heap_limit64` 一旦设了非 0 值，`sqlite3_soft_heap_limit64` 就无法再关闭软上限，也无法把它抬到硬上限之上。设计意图是让硬上限成为不可绕过的天花板，避免应用误以为软上限可以被当作唯一约束。
- **`nearlyFull` 是缓存标志而非实时计算**：`sqlite3HeapNearlyFull` 读的是 `mallocWithAlarm` 里顺带更新的原子标志，不是现场去读 `SQLITE_STATUS_MEMORY_USED` 再比较阈值。这样下游（页缓存、排序器）每次判断只是一次原子读，不必自己去摸状态计数器。
- **lookaside 与堆分开计量**：`sqlite3DbMallocSize` 会对 lookaside 区间内的指针直接返回槽位尺寸而不去调 `xSize`，因为 lookaside 块来自一块连续内存，其真实尺寸由 `db->lookaside.szTrue` / `LOOKASIDE_SMALL` 决定，交给 `xSize` 去问系统分配器会得到错误答案。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| src/malloc.c | sqlite3MallocInit | 初始化内存子系统：装默认分配器、申请静态互斥量、校验页缓存预分配参数、调用 xInit |
| src/malloc.c | sqlite3_soft_heap_limit64 | 设置/查询软堆上限；硬上限存在时把参数收敛到硬上限；设置后立即回收超额内存 |
| src/malloc.c | sqlite3_hard_heap_limit64 | 设置/查询硬堆上限；顺带把软上限压到不超过硬上限 |
| src/malloc.c | sqlite3HeapNearlyFull | 返回堆是否接近满的原子标志 `mem0.nearlyFull` |
| src/malloc.c | sqlite3MallocSize | 返回单块堆分配的实际占用字节数（走 `xSize`） |
| src/malloc.c | sqlite3DbMallocSize | 返回与连接关联的分配块尺寸：先判 lookaside 区间，再回落 `xSize` |

## 与主核 / AI 的关系

**架构推论：** 主核 `engine/db/` 自己实现了带页布局的堆分配（`engine/db/src/storage/heap.c`），与 SQLite 的这一层不是同一套东西：SQLite 这里是"函数指针 + 状态计数器 + 阈值回调"的策略层，不管理单块内存的页布局；单块内存的按页分配与复用由 `src/mem0.c` 的小内存块分配器承担（对应本组的 j04 页）。

**架构推论：** 主核的 `engine/src/sdk/` 直接依赖 SQLite，嵌入式 SDK 需要在受限内存环境里跑，因此 SQLite 这一层暴露的 `sqlite3_soft_heap_limit64` / `sqlite3_hard_heap_limit64` 正是 SDK 可用的现成手段：用软上限配页缓存回收，用硬上限给整个进程划出内存天花板。

## 参考

- SQLite 内存分配接口文档：<https://sqlite.org/c3ref/mem.html>
- SQLite 内存管理与 `sqlite3_release_memory`：<https://sqlite.org/memory.html>
- `sqlite3_soft_heap_limit64` API 说明：<https://sqlite.org/c3ref/soft_heap_limit64.html>
- `sqlite3_hard_heap_limit64` API 说明：<https://sqlite.org/c3ref/hard_heap_limit64.html>
- 源码镜像 `src/malloc.c`：<https://github.com/sqlite/sqlite/blob/master/src/malloc.c>
- 源码镜像 `src/sqliteInt.h`（`LOOKASIDE_SMALL` 定义）：<https://github.com/sqlite/sqlite/blob/master/src/sqliteInt.h>
