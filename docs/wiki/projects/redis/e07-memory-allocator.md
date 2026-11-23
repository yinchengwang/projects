---
project: redis
feature: memory-allocator
page_type: feature
schema_version: 1
refs:
  - path: src/zmalloc.c
    symbols: [zmalloc, zfree, zrealloc, zmalloc_used_memory, zmalloc_get_rss, zmalloc_get_private_dirty, zstrdup]
---

# 内存分配与用量统计

## 一句话职责

`src/zmalloc.c` 是 Redis 全部内存申请的单一入口：它在 `malloc` 之上加了一层可替换的分配器适配、一条 OOM 的统一处置路径、一套按线程分槽的用量记账，以及跨平台的 RSS 与私有脏页探测。

## 机制是什么

**分配器适配。** 编译期通过 `USE_JEMALLOC` / `USE_TCMALLOC` 决定底层分配器，`zmalloc.c` 用宏把 `malloc` / `calloc` / `realloc` / `free` 整体重定向到 `je_malloc` / `tc_malloc` 等。jemalloc 下还额外声明了 `je_malloc_with_usize` 一族接口——它们在分配的同时把**实际可用字节数**一并返回，Redis 因此能在分配那一刻就知道真实占用，而不必事后猜测。

**编译期调参。** `je_malloc_conf` 是 jemalloc 的配置串，源码注释写明用途：「raise per-bin tcache limits for small size classes so bursts of same size small allocations don't spill into the arena which reduces performance」，值为 `lg_tcache_nslots_mul:3,tcache_nslots_small_max:1000`——小对象线程缓存的槽位倍数从 2 倍提到 8 倍、单 bin 硬上限从 200 提到 1000。

**前缀头。** `PREFIX_SIZE` 在 `HAVE_MALLOC_SIZE` 下是 0（分配器能直接告诉你这块内存多大）；否则是 `sizeof(size_t)`（64 位上是 8），Redis 自己在每次分配的**前面**存一个 size 头，这样 `zmalloc_size` 就能反查大小。返回给调用方的指针是 `ptr + PREFIX_SIZE`。

**最小分配尺寸。** `MALLOC_MIN_SIZE(x)` 是 `(x) > 0 ? (x) : sizeof(long)`，注释解释原因：「use a minimum allocation size to match the jemalloc behavior that doesn't return NULL in this case」——保证申请 0 字节时不会拿到 NULL。

**按线程分槽的用量记账。** `used_memory` 是一个长度为 `MAX_ENTRIES`（`DEDICATED_ENTRIES` 8 + `SHARED_ENTRIES` 8 = 16）的数组，每项按缓存行对齐（`CACHE_LINE_SIZE`），每项含两个原子量：`used_memory`（该线程累计申请量）与 `last_peak_check`（上次做全局峰值检查时的累计量）。前 8 个是**专属槽**（主线程 + IO 线程），只有一个写者，可以用「load + store」这种不带 lock 前缀的廉价原子操作；第 9 个及以后的线程**共享 8 个槽位**，槽号由线程序号取模得到，必须用完整的原子 RMW。

**峰值统计是节流的。** `PEAK_CHECK_THRESHOLD` 是 `1024 * 100`，即 100KB。只有当本线程的累计量比上次检查时**多出超过 100KB**时，才会去扫描全局、尝试用 CAS 循环发布新的 `zmalloc_peak`。

本页只讲分配与统计这一层。各数据结构的内存布局是 e03 / e05 / e06；淘汰策略是 c05。

## 怎么实现的

**申请：`zmalloc(size)`。** 它调 `ztrymalloc_usable_internal(size, NULL)`，失败时走 `zmalloc_oom_handler(size)`。`ztrymalloc_usable_internal` 的流程：

1. **溢出保护**：`if (size >= SIZE_MAX/2) return NULL;`——源码注释写明「Possible overflow, return NULL, so that the caller can panic or handle a failed allocation.」
2. **实际申请**：`malloc(MALLOC_MIN_SIZE(size) + PREFIX_SIZE)`。
3. **三条记账路径**，取决于平台能力：
   - 有 `HAVE_ALLOC_WITH_USIZE`：`malloc_with_usize` 直接返回可用大小，按它记账。
   - 有 `HAVE_MALLOC_SIZE`：`zmalloc_size(ptr)` 反查大小，按它记账。
   - 都没有：把请求尺寸写进前缀头，按 `size + PREFIX_SIZE` 记账，返回的指针是 `ptr + PREFIX_SIZE`。

`ztrymalloc` 是失败返回 NULL 的版本；`zmalloc_usable` 额外把可用字节数通过出参带回。

**释放：`zfree(ptr)`。** 空指针直接返回。三条路径与申请时对称：有 `HAVE_ALLOC_WITH_USIZE` 用 `free_with_usize` 拿回旧大小；有 `HAVE_MALLOC_SIZE` 用 `zmalloc_size(ptr)`；都没有则从前缀头读出大小后交还 `ptr - PREFIX_SIZE`。`zfree_usable` 在释放的同时把被释放的字节数带给调用方；`zfree_with_size(ptr, size)` 是 jemalloc 专用路径，直接调 `je_sdallocx` 跳过 emap 查找，注释提醒「'size' must be the usable size to keep zmalloc used_memory accounting accurate」——**传错尺寸会破坏记账**。

**扩容：`zrealloc(ptr, size)`。** 走 `ztryrealloc_usable_internal`，失败且 `size != 0` 时调 OOM 处理器。注意 `size == 0` 的情况不触发 OOM——那是「释放」的语义。`ztryrealloc` 返回 NULL 而不 panic，`zrealloc_usable` 额外回填新旧两个可用尺寸。

**复制字符串：`zstrdup(s)`。** 先 `strlen(s) + 1` 算出长度，再 `zmalloc_usable(l, usable)` 申请，最后 `memcpy(p, s, l)`。`zstrdup_usable` 是带出参的版本。

**OOM 处置是可替换的。** 默认处理器 `zmalloc_default_oom` 往 stderr 打一行 `zmalloc: Out of memory trying to allocate %zu bytes`，`fflush` 后 `abort()`。全局函数指针 `zmalloc_oom_handler` 可以通过 `zmalloc_set_oom_handler` 换掉。真实调用方是 `src/server.c` 的 `redisOutOfMemoryHandler`——Redis 把自己那套「打日志、拒绝请求」的处置接了进来。

**用量汇总：`zmalloc_used_memory()`。** 读出 `num_active_threads`，**钳到 `MAX_ENTRIES`**，然后把前 `num_active_threads` 个槽位的 `used_memory` 原子读出来求和返回。钳位是必要的——因为溢出线程是按 `SHARED_ENTRIES_MASK` 取模共享槽位的，真实活跃线程数可能超过 16。

**峰值：`zmalloc_get_peak_memory()` / `zmalloc_get_peak_memory_time()`。** 峰值由 `update_zmalloc_stat_alloc` 维护：先更新本线程计数，再看 `thread_used - thread_last_peak_check_used > PEAK_CHECK_THRESHOLD` 是否成立；成立则取一次全局快照 `used_mem = zmalloc_used_memory()`，与已发布的 `zmalloc_peak` 比较，若更大就用 **CAS 循环**尝试发布——注释解释：「on failure, `old_peak` is refreshed with the latest peak value, and we retry only while our snapshot still exceeds it.」发布成功后记下 `zmalloc_peak_time = time(NULL)`。最后把 `last_peak_check` 更新为本线程当前计数，形成节流。

释放侧 `update_zmalloc_stat_free` 只减本线程计数，**不更新全局峰值**——峰值只增不减，符合「峰值」的定义。

**RSS 探测：`zmalloc_get_rss()`。** 按平台分派：Linux 走 `HAVE_PROC_STAT`，读 `/proc/self/stat` 的第 24 个字段（RSS，单位是页）再乘 `sysconf(_SC_PAGESIZE)`；macOS 走 `HAVE_TASKINFO` 用 `task_info` 取 `resident_size`；FreeBSD / DragonFly 走 `sysctl` 取 `ki_rssize` / `kp_vm_rssize`；NetBSD / OpenBSD 走 `sysctl` 取 `p_vm_rssize`；Haiku 遍历 area；Solaris 走 `HAVE_PSINFO` 的 ioctl。**兜底分支返回 `zmalloc_used_memory()`**，源码注释写明：「If we can't get the RSS in an OS-specific way for this system just return the memory usage we estimated in zmalloc()」，并补了一句后果——「in this case, the fragmentation ratio will be always 1」。

函数上方的警告值得原样记下：「the function zmalloc_get_rss() is not designed to be fast and may not be called in the busy loops where Redis tries to release memory expiring or swapping out objects.」

**私有脏页：`zmalloc_get_private_dirty(pid)`。** 它转调 `zmalloc_get_smap_bytes_by_field("Private_Dirty:", pid)`。Linux 下（`HAVE_PROC_SMAPS`）打开 `/proc/self/smaps`（`pid == -1`）或 `/proc/<pid>/smaps`，逐行用 `strncmp` 匹配字段名，找到后按 `k` 截断、`strtol` 读出 kB 数再 `* 1024` 累加。macOS 走 libproc 的 `proc_pidinfo`，其中 `AnonHugePages:` 直接返回 0，注释说明「THP feature is not supported in this platform」。**代价被写在注释里**：「depending on the platform and memory footprint of the process, this call can be slow, exceeding 1000ms!」

**其它两个辅助接口。** `zmadvise_dontneed(ptr)` 在 Linux + jemalloc 下把指针按页对齐后调 `madvise(..., MADV_DONTNEED)`，注释说明用途：「We do that in a fork child process to avoid CoW when the parent modifies these shared pages.」`zmalloc_get_memory_size()` 读物理内存大小。

**真实 caller。**

- **分配 / 释放 / 扩容。** `zmalloc` / `zfree` / `zrealloc` 是全仓的默认内存出口：`src/sds.c`、`src/dict.c`、`src/listpack.c`、`src/quicklist.c`、`src/object.c` 等结构层全部走这一层。
- **用量统计。** `zmalloc_used_memory` 的真实调用方遍布各处：`src/server.c` 在启动时用 `server.initial_memory_usage = zmalloc_used_memory()` 记录基线；在 `getMaxmemoryState` 里同时取当前用量与 `zmalloc_get_peak_memory()`；在 serverCron 里采样 `server.cron_malloc_stats.zmalloc_used`；还用它算 `server.stat_fork_rate`。`src/evict.c` 在淘汰决策时读当前用量；`src/config.c` 用 `zmalloc_used_memory() - freeMemoryGetNotCountedMemory()` 算「计入上限的用量」；`src/db.c` 在删除键前后对比用量以统计释放量；`src/object.c` 用它校验对象大小统计；`src/networking.c` 用它判断客户端输出缓冲是否超限；`src/module.c` 用它算模块的内存占比；`src/rdb.c` 在 RDB 里写 `used-mem` 辅助字段。
- **RSS 探测。** `zmalloc_get_rss` 在真实代码里只有一处调用：`src/server.c` 的 serverCron 里 `server.cron_malloc_stats.process_rss = zmalloc_get_rss();`——**放在定时任务里而不是热路径上**，与函数注释里的「不适合在忙循环里调用」相吻合。
- **私有脏页。** `zmalloc_get_private_dirty(-1)` 的真实调用方是 `src/childinfo.c`，用来测量 fork 子进程的写时复制内存。另有 `src/latency.c` 调 `zmalloc_get_smap_bytes_by_field("AnonHugePages:", -1)` 读透明大页用量。
- **OOM 处理器。** `zmalloc_set_oom_handler(redisOutOfMemoryHandler)` 在 `src/server.c` 的初始化路径里调用。
- **页释放。** `zmadvise_dontneed` 在 `src/server.c` 的 fork 子进程路径里调用，用来尽早把不再需要的页还给操作系统，避免父进程后续修改时触发写时复制。

**退化与回退路径。**

- **申请失败默认是终止进程。** `zmalloc` 在分配失败时走 OOM 处理器，默认实现是打日志后 `abort()`。Redis 用 `zmalloc_set_oom_handler` 换掉了这个行为，改成自己的错误处理。
- **有 `try` 变体可以避免终止。** `ztrymalloc` / `ztryrealloc` 返回 NULL 而不 panic；`sds` 的 `sdstrynewlen`、listpack 的 `lpInsert` 等上层接口在需要时用它们把失败往上抛。
- **尺寸溢出会被挡在分配之前。** `size >= SIZE_MAX/2` 直接返回 NULL，避免 `size + PREFIX_SIZE` 溢出成一个很小的数。
- **记账按「实际可用字节」而不是「请求字节」。** 在有 `HAVE_MALLOC_SIZE` 或 `HAVE_ALLOC_WITH_USIZE` 的平台上，分配器可能给出比请求更大的块，Redis 按真实大小记账；代价是这块内存**没有被任何人使用**也计入了用量。
- **共享槽位意味着统计是近似的。** 超过 8 个线程后，多个线程的用量被折叠进同一个槽位——总数仍然准确（求和不丢），但单线程的归因不再准确。
- **峰值检查被节流到 100KB 粒度。** 小于 100KB 的用量跳变不会立刻反映到峰值上——这是刻意的性能取舍。
- **RSS 拿不到时退化成用量。** `zmalloc_get_rss` 的兜底分支返回 `zmalloc_used_memory()`，源码注释直接点出后果：碎片率会**恒等于 1**。
- **私有脏页探测可能超过 1 秒。** 注释明写这个耗时，因此它只适合在 fork 子进程这种「一次性、可以慢」的场合调用。

## 关键取舍

**源码事实：** 全部内存申请经由 `zmalloc.c`，底层分配器在编译期切换。**源码事实：** 有 `HAVE_MALLOC_SIZE` 时无前缀头，否则自建 8 字节前缀。**源码事实：** 用量记账按线程分 8 个专属槽 + 8 个共享槽，专属槽用单写者原子。**源码事实：** 峰值检查以 100KB 为节流粒度，发布用 CAS 循环。**源码事实：** RSS 在 7 类平台上分别实现，兜底返回 `zmalloc_used_memory()`。**源码事实：** 私有脏页靠读 `/proc/self/smaps`。**源码事实：** OOM 处理器是可替换的函数指针，Redis 换成了自己的实现。

**架构推论：** 用「每线程一个缓存行对齐的计数槽」替代一把全局锁，把内存统计的开销从「每次分配都要争锁」降到「每线程一次 load + store」——这是典型的用空间（缓存行填充）换吞吐的做法，代价是 16 个槽位就占掉 16 条缓存行。**架构推论：** 峰值统计节流到 100KB，等于声明「峰值只需要近似准确」——这与 `used_memory` 本身要求精确形成对照，两者的精度需求被明确区分开了。**架构推论：** 「按实际可用字节记账」保证了碎片不会被算成「用户的内存」，但也意味着**用户用不到的内存照样计入 maxmemory 判定**；这是一个偏向「保守限流」的取舍。**架构推论：** OOM 默认 abort 而把替换点留给上层，是把「内存耗尽」当成不可恢复的状态；Redis 换成自己的处理器，说明它认为内存压力是**可恢复的运行时状态**（可以拒绝写、可以淘汰），这与很多数据库把 OOM 当 fatal 不同。主核是多写者引擎，这套分槽记账的思路**可以照搬**（每线程或每分片一个缓存行对齐的计数器），但共享槽位那部分不适合——主核的分片数可能远超 16 个，需要换成「按分片 ID 直接索引」的无锁方案。另一条值得带走的是「**读 `/proc/self/smaps` 拿私有脏页**」这个手段：它是 Linux 上唯一能区分「这块内存是自己写的」与「这块内存只是别人共享的」的低成本方法，对做写时复制优化的存储引擎价值很高。AI 侧则要警惕**记账精度与实际占用的偏差**：Redis 的 `used_memory` 是分配器视角的占用，与「模型权重实际占用」不是一回事；若把 Redis 的内存指标直接当作向量库容量规划依据，会在碎片与元数据上产生系统性高估。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/zmalloc.c` | `zmalloc` | 分配入口：适配底层分配器、做溢出与最小尺寸保护、记账，失败时走可替换的 OOM 处理器 |
| `src/zmalloc.c` | `zfree` | 释放入口：按平台取出已分配尺寸后归还，并回退本线程的用量计数 |
| `src/zmalloc.c` | `zrealloc` | 调整尺寸入口：复用内部实现，失败且尺寸非零时触发 OOM 处理 |
| `src/zmalloc.c` | `zmalloc_used_memory` | 用量汇总：累加各线程计数槽位的已分配字节 |
| `src/zmalloc.c` | `zmalloc_get_rss` | 进程常驻内存探测，按平台分别实现，拿不到时退化为已分配用量 |
| `src/zmalloc.c` | `zmalloc_get_private_dirty` | 私有脏页总量探测，读取 smaps 并按字段累加 |
| `src/zmalloc.c` | `zstrdup` | 字符串复制：算长度、申请、拷贝三步 |

## 参考

- [Redis 源码：zmalloc.c](https://github.com/redis/redis/blob/unstable/src/zmalloc.c)
- [Redis 源码：zmalloc.h](https://github.com/redis/redis/blob/unstable/src/zmalloc.h)
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/)

## 与主核 / AI 的关系

这一页对主核最有价值的是**「内存统计不是一个数，而是三个数」**这个区分：`zmalloc_used_memory` 是进程分配器视角的占用，`zmalloc_get_rss` 是操作系统视角的常驻，`zmalloc_get_private_dirty` 是「自己写脏的页」。三者能回答完全不同的问题——「我分配了多少」「我实际占着多少物理页」「我改动了多少共享页」。主核的多模态场景里这三者差距极大：模型权重是 mmap 出来的（RSS 大但 zmalloc 记账小），向量索引是堆上的（两者接近），而写时复制会制造「RSS 大但不是自己写脏的」的假象。把这三个数分开报，比报一个「内存占用」有用得多。

第二条是**「按线程分槽记账 + 缓存行填充」**这条无锁统计法，主核可以直接借用：多线程写入引擎里，每分片一个 `alignas(64)` 的计数器，统计时逐槽求和，不必引入全局锁。Redis 这版只留了 16 个槽是因为它的线程模型简单，主核需要按分片数扩展。

第三条是 `zmadvise_dontneed` 这个具体手段：在 fork 子进程里主动把不再需要的页还给内核，避免父进程后续修改这些共享页时触发写时复制。主核若用进程级隔离做多租户或快照，这条手段能显著降低 CoW 带来的内存放大。

AI 侧最实用的一条是**「私有脏页」这个指标**：Redis 用它来量化 fork 子进程写坏了多少共享页，从而判断 bgsave / AOF rewrite 期间内存膨胀的来源。做向量检索系统时，若用 mmap 加载索引文件并 fork 做查询隔离，同样可以用私有脏页判断「子进程是不是把索引页都写脏了」——这比只看 RSS 精准得多。
