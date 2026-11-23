---
project: redis
feature: bio-background-jobs
page_type: feature
schema_version: 1
refs:
  - path: src/bio.c
    symbols: [bioSubmitJob]
---

# BIO 后台任务线程

## 一句话职责

`bioSubmitJob` 把三类慢 I/O 任务（延迟 close、AOF fsync、惰性释放内存）按任务类型投递到固定的后台线程队列，后台线程按 FIFO 顺序执行完，再通过一个非阻塞管道唤醒事件循环，由 `bioPipeReadJobCompList` 在主线程里回调完成通知。

## 机制是什么

机制是「每类任务一个锁、一个队列、一个至少一个 worker」的静态分派，外加一条独立的完成通知通道。

**3 个 worker 线程。** 数量不是配置项，而是由 `bio_worker_title` 数组的三个元素推出来的：`BIO_WORKER_NUM` 的定义是 `sizeof(bio_worker_title) / sizeof(*bio_worker_title)`，三个标题分别是 `"bio_close_file"`、`"bio_aof"`、`"bio_lazy_free"`。

**7 种任务 opcode。** `src/bio.h` 的 `bio_job_type_t` 定义 `BIO_CLOSE_FILE`、`BIO_AOF_FSYNC`、`BIO_LAZY_FREE`、`BIO_CLOSE_AOF` 四个真实任务，加三个完成请求 `BIO_COMP_RQ_CLOSE_FILE`、`BIO_COMP_RQ_AOF_FSYNC`、`BIO_COMP_RQ_LAZY_FREE`，`BIO_NUM_OPS` 是哨兵值。

**静态映射决定去向。** `bio_job_to_worker` 把 opcode 映射到 worker 序号：`BIO_CLOSE_FILE` 与 `BIO_COMP_RQ_CLOSE_FILE` 去 0，`BIO_AOF_FSYNC`、`BIO_CLOSE_AOF`、`BIO_COMP_RQ_AOF_FSYNC` 去 1，`BIO_LAZY_FREE` 与 `BIO_COMP_RQ_LAZY_FREE` 去 2。所以 AOF 的两个任务共享同一个 worker，而 close 任务独占一个。

**每 worker 一套同步原语。** `bio_mutex[]`、`bio_newjob_cond[]`、`bio_jobs[]` 都是按 worker 下标索引的数组，另有 `bio_jobs_counter[BIO_NUM_OPS]` 按 opcode 计数，供 `bioPendingJobsOfType` 无锁竞争地读取。

**完成通知走另一条路。** worker 不通过条件变量通知主线程，而是把 `bio_comp_item`（`func` / `arg` / `ptr` 三个字段）追加进 `bio_comp_list`，再向 `job_comp_pipe[1]` 写一个字节；事件循环因管道可读被唤醒，`bioPipeReadJobCompList` 把列表整表换走并逐个回调。

**完成请求是哨兵任务，不是回调注册表。** `bioCreateCompRq` 按 `assigned_worker` 选一个 `BIO_COMP_RQ_*` opcode 投进同一个队列。因为同一 worker 的队列是 FIFO，排在若干任务之后的完成请求必定在那些任务之后被执行；`src/bio.c` 的头注释把这一点写成设计意图（FLUSHALL 一次要投多个任务，只需一个哨兵就能覆盖）。

## 怎么实现的

### 投递：`bioSubmitJob`

函数体只有四步：先 `job->header.type = type`（`bio_job` 是 union，所有成员的第一个字段都是 `int type`，头注释写明这是必须遵守的布局约束）；再用 `bio_job_to_worker[type]` 取 worker 序号；加锁后 `listAddNodeTail(bio_jobs[worker], job)`、`bio_jobs_counter[type]++`、`pthread_cond_signal(&bio_newjob_cond[worker])`，然后解锁。投递者只触碰目标任务所属的那一把锁。

五个投递封装都是「填好 union 的专用成员，再交给 `bioSubmitJob`」：

- `bioCreateLazyFreeJob(free_fn, arg_count, ...)`：`zmalloc(sizeof(*job) + sizeof(void *) * arg_count)`，用 `va_arg` 把 `arg_count` 个指针写进 `free_args.free_args[]`，类型固定 `BIO_LAZY_FREE`。这个变长尾部就是 `bio_job` 里 `void *free_args[]` 的作用。
- `bioCreateCloseJob(fd, need_fsync, need_reclaim_cache)`：填 `fd_args.fd` 与两个 1-bit 标志，类型 `BIO_CLOSE_FILE`。
- `bioCreateFsyncJob(fd, offset, need_reclaim_cache)`：类型 `BIO_AOF_FSYNC`，`offset` 存进 `fd_args.offset`，用于成功后回填 `fsynced_reploff_pending`。
- `bioCreateCloseAofJob(fd, offset, need_reclaim_cache)`：类型 `BIO_CLOSE_AOF`，函数体里把 `need_fsync` 硬写成 1，所以关 AOF 之前一定 fsync。
- `bioCreateCompRq(assigned_worker, func, user_data, user_ptr)`：`switch` 三个合法 worker 得到对应 `BIO_COMP_RQ_*`，`default` 分支直接 `serverPanic`——非法 worker 不是运行时错误，是编程错误。

### 装配：`bioInit`

顺序是：先循环 `BIO_WORKER_NUM` 次 `pthread_mutex_init` / `pthread_cond_init` / `listCreate()`；再建 `bio_comp_list` 与 `bio_mutex_comp`；然后 `anetPipe(job_comp_pipe, O_CLOEXEC|O_NONBLOCK, O_CLOEXEC|O_NONBLOCK)`，两端都非阻塞、都 close-on-exec（注释说明是为了不阻塞任何一半，也避免 sentinel 之类的 fork-exec 泄漏 fd），失败则 `serverLog(LL_WARNING)` 后 `exit(1)`；接着 `aeCreateFileEvent(server.el, job_comp_pipe[0], AE_READABLE, bioPipeReadJobCompList, NULL)`，返回 `AE_ERR` 时 `serverPanic`。

栈大小单独处理：先 `pthread_attr_getstacksize`，取到 0 就置 1，然后 `while (stacksize < REDIS_THREAD_STACK_SIZE) stacksize *= 2`，其中 `REDIS_THREAD_STACK_SIZE` 是 `1024*1024*4`（4 MB），注释理由是后台线程要执行和主线程同样量级的工作。最后循环 `pthread_create`，参数是 worker 序号，失败同样 `exit(1)`。

唯一调用点是 `src/server.c` 的 `InitServerLast`，与 `initThreadedIO` 并列；源码注释写明它必须晚于模块加载，原因是 ld.so 里 TLS 初始化与 dlopen 存在竞争。

### worker 主体：`bioProcessBackgroundJobs`

进入循环前依次：`serverAssert(worker < BIO_WORKER_NUM)`；`redis_set_thread_title(bio_worker_title[worker])`；`redisSetCpuAffinity(server.bio_cpulist)`（`USE_SETCPUAFFINITY` 未定义时是空实现）；`makeThreadKillable()`，即 `pthread_setcancelstate(PTHREAD_CANCEL_ENABLE)` 加 `pthread_setcanceltype(PTHREAD_CANCEL_ASYNCHRONOUS)`；最后 `pthread_sigmask(SIG_BLOCK, &sigset, NULL)` 屏蔽 SIGALRM，让看门狗信号只落在主线程，失败只记 warning 而不退出。

循环的约定是「每轮开始都持锁」：

- 队列空 → `pthread_cond_wait`，然后 `continue`；
- 否则取 `listFirst` 拿到 job，**先解锁再执行**，因为此时 job 已经是独立结构；
- 按 `job->header.type` 分四类处理：

`BIO_CLOSE_FILE`：`need_fsync` 为真时调 `redis_fsync(fd)`，失败且 `errno` 不是 `EBADF`/`EINVAL` 才记 `LL_WARNING`；`need_reclaim_cache` 为真时调 `reclaimFilePageCache(fd, 0, 0)`，返回 -1 记 `LL_NOTICE`；末尾无条件 `close(fd)`。

`BIO_AOF_FSYNC` 与 `BIO_CLOSE_AOF`：`redis_fsync` 失败但 `errno` 是 `EBADF`/`EINVAL` 时视为「fd 已被主线程关闭或复用」，不算真失败（源码注释明确这一点）；真失败则先读 `aof_bio_fsync_status` 的旧值，置 `C_ERR`、写 `aof_bio_fsync_errno`，并且只在旧值是 `C_OK` 时打日志，避免持续失败反复刷屏；成功则置 `C_OK` 并把 `job->fd_args.offset` 写进 `fsynced_reploff_pending`。`need_reclaim_cache` 的处理同上，只有 `BIO_CLOSE_AOF` 额外 `close(fd)`。

`BIO_LAZY_FREE`：一行 `job->free_args.free_fn(job->free_args.free_args)`，释放逻辑完全由投递方给的函数指针决定。

`BIO_COMP_RQ_*`：`zmalloc` 一个 `bio_comp_item` 拷出 `fn`/`arg`/`ptr`，在 `bio_mutex_comp` 下 `listAddNodeTail(bio_comp_list, ...)`，解锁后向 `job_comp_pipe[1]` 写 1 字节 `"A"`。函数体里写失败的分支是空的，源码注释只交代「管道是非阻塞的，写满时 write() 会失败」——唤醒按尽力而为处理，不保证每次投递都产生一次事件。

其它 type → `serverPanic`。

收尾：`zfree(job)`，重新加锁，`listDelNode(bio_jobs[worker], ln)`、`bio_jobs_counter[job_type]--`、`pthread_cond_signal(&bio_newjob_cond[worker])`——这最后一下信号供 `bioDrainWorker` 的等待者醒来复查队列长度。

### 主线程侧回收：`bioPipeReadJobCompList`

第一步用 `char buf[128]` 把管道读空（`while (read(fd, buf, sizeof(buf)) == sizeof(buf));`）。第二步在 `bio_mutex_comp` 下判断 `bio_comp_list` 是否非空，非空就把整表交给局部变量 `bio_comp_list` 指向新表，形成「换表而不是搬表」。第三步若没换出表就直接返回；否则逐项 `rsp->func(rsp->arg, rsp->ptr)`，再 `zfree(rsp)`，最后 `listRelease(tmp_list)`。

### 可查询、可阻塞、可自检的接口

- `bioPendingJobsOfType(type)`：按 `bio_job_to_worker[type]` 取 worker 锁，返回 `bio_jobs_counter[type]`。调用方三处：`src/aof.c` 的 `aofFsyncInProgress` 返回 `bioPendingJobsOfType(BIO_AOF_FSYNC) != 0`；`src/server.c` 的 `genRedisInfoString` 输出 `aof_pending_bio_fsync` 字段；`src/evict.c` 的 `performEvictions` 用它判断惰性释放是否还在跑。
- `bioDrainWorker(job_type)`：在该 worker 锁上 `while (listLength(bio_jobs[worker]) > 0) pthread_cond_wait`。真实调用方两个：`src/aof.c` 的 `rewriteAppendOnlyFileBackground`，在 `aof_state == AOF_WAIT_REWRITE` 时排空 AOF 队列，注释写明是为了防止新旧 AOF 对 `fsynced_reploff_pending` 的更新竞争；`src/config.c` 的 `updateAppendFsync`，在 `aof_fsync == AOF_FSYNC_ALWAYS` 时排空。
- `bioIsLazyfreeWorker()`：`pthread_equal(pthread_self(), bio_threads[BIO_WORKER_LAZY_FREE])`。`src/t_hash.c` 的 `hashTemplateRecordPendingDrop` 用它 `serverAssert` 确认自己确实跑在 lazy-free 线程上。

### 谁在投递

**AOF fsync。** `flushAppendOnlyFile` 在 `aof_fsync == AOF_FSYNC_EVERYSEC`、`server.mstime - server.aof_last_fsync >= 1000` 且 `!sync_in_progress` 时调 `aof_background_fsync(server.aof_fd)`；后者就是 `bioCreateFsyncJob(fd, server.master_repl_offset, 1)`。`openNewIncrAofForAppend` 在换 `server.aof_fd` 之前调 `aof_background_fsync_and_close`，即 `bioCreateCloseAofJob`。

**延迟 close。** `src/aof.c` 的 `loadSingleAppendOnlyFile` 在 cleanup 段 `dup(fileno(fp))` 再 `fclose(fp)`，然后 `bioCreateCloseJob(fd, 0, 1)` 让后台线程关 fd 并回收页缓存；`src/rdb.c` 的 `rdbLoadWithEmptyFunc` 在加载成功后重新 `open` 一次再投递关闭任务；`src/replication.c` 的 `bg_unlink` 先 `unlink` 掉文件名再投递 close，头注释解释删除的存储空间只会在最后一个引用消失时释放；同文件的 `closeRepldbfd` 先遍历 `server.slaves`，只要还有另一个处于 `SLAVE_STATE_SEND_BULK` 的客户端共享这份 repl DB，就把 `reclaim` 置 0 并退回到同步 `close(myself->repldbfd)`，只有独占时才投递 `bioCreateCloseJob(myself->repldbfd, 0, 1)`；`readSyncBulkPayload` 则在把临时文件 `rename` 成 `server.rdb_filename` 之后，用 `bioCreateCloseJob(old_rdb_fd, 0, 0)` 关掉旧 RDB 的 fd。

**惰性释放。** `src/lazyfree.c` 的 `freeObjAsync` 在 `lazyfreeGetFreeEffort(...) > LAZYFREE_THRESHOLD` 且 `obj->refcount == 1` 时才投递，否则退化为同步 `decrRefCount(obj)`；`emptyDbDataAsync` 用 `bioCreateLazyFreeJob(lazyfreeFreeKvs, 7, ...)` 一次带走 7 个参数；`freeTrackingRadixTreeAsync`、`freeErrorsRadixTreeAsync`、`freeLuaScriptsAsync`、`freeFunctionsAsync`、`freeReplicationBacklogRefMemAsync` 各自比较自己的规模与 `LAZYFREE_THRESHOLD`（该文件里 `#define LAZYFREE_THRESHOLD 64`），超阈值投递、否则当场同步释放。`lazyfreeFreeObject` 与 `lazyfreeFreeKvs` 在执行时同时维护 `lazyfree_objects` / `lazyfreed_objects` 两个原子计数，它们是 INFO 里 `lazyfree_pending_objects`、`lazyfreed_objects` 的来源。

**完成请求。** `src/db.c` 的 `flushCommandCommon` 在 SYNC 被改写成阻塞 ASYNC 之后，先 `blockClientForAsyncFlush(c)`，再 `bioCreateCompRq(BIO_WORKER_LAZY_FREE, kvsAsyncFreeDoneCB, c->id, slots)`；回调里 `unblockClientForAsyncFlush` 通过 `lookupClientByID` 找回客户端，只有 `CLIENT_BLOCKED` 还在才继续，然后回 `shared.ok` 或 `replySlotsFlush`，并对标记了 `CLIENT_PENDING_COMMAND` 的客户端补跑 `commandProcessed`。`src/cluster_asm.c` 的 `asmTriggerBackgroundTrim` 先 `emptyDbDataAsync` 再投递 `asmBackgroundTrimDoneCB`，注释写明「在同一 BIO worker 上排在惰性释放任务之后」。

### 两条真实的回退路径

**AOF fsync 太慢时放弃等待。** `flushAppendOnlyFile` 在 `everysec` 且 `sync_in_progress` 为真时：若 `aof_flush_postponed_start` 还是 0，就记录当前时间并直接 return；若已推迟不足 2000 毫秒，继续 return；超过 2000 毫秒则落到正常写入路径，`server.aof_delayed_fsync++` 并打一条 NOTICE，说明磁盘忙。这里放弃的是「等后台 fsync」，不是 fsync 本身。

**淘汰等惰性释放只短等。** `performEvictions` 在结果为 `EVICT_FAIL` 时，以 `bioPendingJobsOfType(BIO_LAZY_FREE)` 非零为循环条件，每轮先 `getMaxmemoryState` 复查，再 `usleep(eviction_time_limit_us < 1000 ? eviction_time_limit_us : 1000)`。上限来自 `evictionTimeLimitUs()`：`maxmemory-eviction-tenacity` 不大于 10 时是 `50 * tenacity` 微秒，小于 100 时是 `500 * 1.15^(tenacity-10)` 微秒，等于 100 时返回 `ULONG_MAX`。超时就带着 `EVICT_FAIL` 返回，不做无限等待。

### 状态外溢：错误与偏移如何回到主线程

`aof_bio_fsync_status` 与 `aof_bio_fsync_errno` 是 worker 到主线程的唯一 fsync 错误通道：`src/server.c` 的 `writeCommandsDeniedByDiskError` 读它，`C_ERR` 时把 errno 抄进 `aof_last_write_errno` 并返回 `DISK_ERROR_TYPE_AOF`；`src/aof.c` 的 `startAppendOnly` 在重开 AOF 时读到 `C_ERR` 就记一条「忽略 bio 线程里的 fsync 错误」并复位成 `C_OK`。`fsynced_reploff_pending` 由 `src/server.c` 的 `beforeSleep` 在 `aof_state == AOF_ON && fsynced_reploff != -1` 时读进 `fsynced_reploff`，值发生变化还会置 `dont_sleep = 1`，让等 `WAITAOF` 的客户端尽快被唤醒。

## 关键取舍

**源码事实：** worker 数与任务去向都是编译期常量。线程数来自 `bio_worker_title` 的数组长度，去向来自静态数组 `bio_job_to_worker`；唯一能配的是亲和性 `bio-cpulist`，它在 `src/config.c` 里以 `IMMUTABLE_CONFIG` 注册，运行期改不了。

**源码事实：** 一个 worker 串行处理整条队列，所以同一 worker 上的任务严格按入队顺序执行；`bioDrainWorker` 和完成请求哨兵都建立在这个顺序保证之上。

**源码事实：** 主线程与 worker 之间没有全局队列锁，每类任务一把；完成通知走独立列表加管道，与任务队列完全解耦。

**源码事实：** 惰性释放的粒度门槛是 64（`LAZYFREE_THRESHOLD`），阈值以下直接同步 `decrRefCount`；`src/lazyfree.c` 的注释给的理由是「对象只有很少几块分配时，惰性释放反而更慢」。

**源码事实：** 五个惰性释放开关默认全关——`lazyfree-lazy-eviction`、`lazyfree-lazy-expire`、`lazyfree-lazy-server-del`、`lazyfree-lazy-user-del`、`lazyfree-lazy-user-flush` 在 `src/config.c` 里 `createBoolConfig` 的默认值都是 0。

**架构推论：** 用静态映射而不是动态负载均衡，换来的是「同一类任务严格串行、彼此可预测」；代价是某个 worker 卡住会连带拖住分到它名下的所有 opcode，`BIO_AOF_FSYNC` 与 `BIO_CLOSE_AOF` 挤在 worker 1 就是这种耦合的直接体现。

**架构推论：** 完成通知只写一个字节且允许写失败，说明它被当作「尽力唤醒」而非可靠信号；配合「读空管道 + 整表换出」的写法，丢字节不会丢回调，只会让回调推迟到下一次事件循环被其他事件驱动起来。

**架构推论：** `bioKillThreads` 用 `pthread_cancel` 直接杀线程，只在崩溃路径 `src/debug.c` 的 `killThreads`（被 `doFastMemoryTest` 调用）里使用，说明正常关停不依赖它——后台线程随进程结束消失，Redis 不为它们安排优雅退出。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/bio.c` | `bioSubmitJob` | 按 `bio_job_to_worker` 选 worker，入队、计数、发条件变量信号 |
| `src/bio.c` | `bioInit` | 建队列/锁/完成管道、注册读事件、把栈抬到 `REDIS_THREAD_STACK_SIZE` 并 spawn 3 个线程 |
| `src/bio.c` | `bioProcessBackgroundJobs` | worker 主体：分派 7 种 opcode，写完成列表并写管道唤醒主线程 |
| `src/bio.c` | `bioPipeReadJobCompList` | 主线程侧读空管道、换出完成列表并逐个回调 |
| `src/bio.c` | `bioPendingJobsOfType` | 按 opcode 读队列计数 |
| `src/bio.c` | `bioDrainWorker` | 等到指定 worker 的队列排空 |
| `src/bio.c` | `bioKillThreads` | 崩溃路径的 `pthread_cancel` 加 `pthread_join` |
| `src/bio.c` | `bioCreateLazyFreeJob`、`bioCreateCloseJob`、`bioCreateFsyncJob`、`bioCreateCloseAofJob`、`bioCreateCompRq` | 五个投递封装，各自填 union 的专用成员 |
| `src/bio.c` | `bioIsLazyfreeWorker` | 判定当前线程是不是 lazy-free worker |
| `src/bio.h` | `bio_job_type_t`、`bio_worker_t` | 7 个 opcode 与 3 个 worker 的枚举定义 |
| `src/server.c` | `InitServerLast` | `bioInit` 的唯一调用点 |
| `src/server.c` | `beforeSleep` | 把 `fsynced_reploff_pending` 应用成 `fsynced_reploff` |
| `src/server.c` | `writeCommandsDeniedByDiskError` | 读 `aof_bio_fsync_status` 判定是否因磁盘错误禁写 |
| `src/lazyfree.c` | `freeObjAsync`、`LAZYFREE_THRESHOLD` | 惰性释放的门槛判断与回退到同步释放 |
| `src/lazyfree.c` | `emptyDbDataAsync` | 把整库数据交给 `bioCreateLazyFreeJob` 的 7 参数形式 |
| `src/aof.c` | `flushAppendOnlyFile`、`aof_background_fsync` | everysec 触发后台 fsync，以及在 fsync 未完成时推迟写入 |
| `src/db.c` | `flushCommandCommon`、`kvsAsyncFreeDoneCB` | 阻塞 ASYNC flush 投递完成请求并回写回复 |

## 与主核 / AI 的关系

从 redis 的 BIO 可借的部分是「静态分派 + FIFO 哨兵」这套骨架：主核若要给慢 I/O 留后台线程，用固定类型的队列加一个完成请求任务就能把「主线程等一批任务做完」表达成单次唤醒，而不必为每批任务维护回调表。但主核是多写者引擎，`bio_mutex[]` 这种「每类任务一把锁」的写法照搬会与事务层的锁顺序叠加，需要重新确定加锁顺序；`aof_bio_fsync_status` 这种用原子量跨线程传错误的做法可以直接借鉴，因为它避免了在主线程里同步等待后台结果。AI 侧把 Redis 当作向量或会话缓存时，`lazyfree-lazy-*` 默认全关意味着大对象删除仍发生在主线程；一旦开启惰性释放，`LAZYFREE_THRESHOLD`（64）以下的小对象仍同步释放，回收时延取决于对象规模而不是配置开关。

## 参考

- [Redis 源码：bio.c](https://github.com/redis/redis/blob/unstable/src/bio.c)
- [Redis 源码：bio.h](https://github.com/redis/redis/blob/unstable/src/bio.h)
- [Redis 文档：持久化与 appendfsync 策略](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/)
- [Redis 文档：INFO 命令](https://redis.io/docs/latest/commands/info/)
