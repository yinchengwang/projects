---
project: redis
feature: aof-append
page_type: feature
schema_version: 1
refs:
  - path: src/aof.c
    symbols: [aofWrite, feedAppendOnlyFile]
---

# AOF 追加日志

## 一句话职责

`feedAppendOnlyFile` 把一条写命令编码成 RESP 追加进内存 AOF 缓冲，`aofWrite` 把这块缓冲完整地写到 AOF 文件描述符上——AOF 记的是命令本身，重放这些命令即可还原数据集。

## 机制是什么

AOF（Append Only File）与 RDB 是两种正交的持久化形态：RDB 记的是某一时刻的数据集切片，AOF 记的是**一条条写命令**。AOF 文件的内容是 RESP 协议的命令流，启动时按同样顺序把这些命令重新执行一遍，数据集就被还原出来。

落盘被切成两段。命令先进入内存缓冲 `server.aof_buf`，真正写文件由 `flushAppendOnlyFile` 统一做。源码注释写明了为什么这样切分：「Since we are required to write the AOF before replying to the client, and the only way the client socket can get a write is entering when the event loop, we accumulate all the AOF writes in a memory buffer and write it on disk using this function just before entering the event loop again.」——因为回复只能在事件循环里写出去，所以 AOF 必须在「再进一次事件循环之前」落盘。

文件组织是多部分 AOF：一个 manifest 记录 base 文件与 incr 文件列表，base 由 rewrite 生成并承载数据集快照形态，incr 是当前持续追加的文件；rewrite 期间新开一个 incr 文件承接写入。

fsync 策略有三档，定义在 `src/server.h`：`AOF_FSYNC_NO`（0）、`AOF_FSYNC_ALWAYS`（1）、`AOF_FSYNC_EVERYSEC`（2），配置项 `appendfsync` 的默认值是 `everysec`。AOF 自身状态也有三档：`AOF_OFF`、`AOF_ON`、`AOF_WAIT_REWRITE`。

本页只讲「追加进缓冲」与「落盘」这条链。命令什么时候被判定为「要写进 AOF」属于命令执行层（a05），快照格式属于 RDB（g01）。

## 怎么实现的

**追加侧：`feedAppendOnlyFile`。** 入参是目标库号 `dictid`、命令的 argv、参数个数与本命令耗时；开头断言库号合法（要么是 -1，要么落在 `server.dbnum` 范围内）。函数做四件事：

1. **时间戳注解。** `server.aof_timestamp_enabled` 打开时调 `genAofTimestampAnnotationIfNeeded(0)`：只有当「AOF 里当前记录的时间戳」小于「当前 unix 时间」时才生成一行 `#TS:<时间戳>\r\n`。函数注释写明注解格式是 "#TS:${timestamp}\r\n"，并说「TS is short of timestamp and this method could save extra bytes in AOF」；`force` 传 1 时无条件生成，用途是 AOF rewrite 子进程在开头无条件记一次时间戳。生成行的长度有断言上限 `AOF_ANNOTATION_LINE_MAX_LEN`（`src/server.h` 里是 1024）。配置项名是 `aof-timestamp-enabled`，默认关闭。
2. **SELECT 注入。** 若 `dictid` 不是 -1 且与 `server.aof_selected_db` 不同，先拼一条 `*2\r\n$6\r\nSELECT\r\n$<长度>\r\n<库号>\r\n` 追加进同一条缓冲，然后把 `server.aof_selected_db` 更新成新库号。`dictid` 为 -1 的语义是「这条命令不要写 SELECT」。
3. **命令体。** `catAppendOnlyGenericCommand` 按 RESP 写：先写 `*<argc>\r\n`，再对每个参数写 `$<长度>\r\n<字节>\r\n`。参数用 `getDecodedObject` 解码成 sds 后取长度与内容，写完 `decrRefCount` 放回。源码注释在这里点明设计原则：「All commands should be propagated the same way in AOF as in replication. No need for AOF-specific translation.」——AOF 里存的就是复制流里那一条命令，不做二次转换。
4. **进缓冲。** 只有两种状态会真正写进 `server.aof_buf`：`server.aof_state == AOF_ON`，或 `server.aof_state == AOF_WAIT_REWRITE` 且当前有 AOF rewrite 子进程在跑。同时把耗时累加进 `server.aof_cmd_duration`（这是「AOF 重放耗时估计」的输入）。

**追加侧的真实调用方。** `src/server.c` 的 `propagateNow` 是唯一调用点，条件是 `server.aof_state != AOF_OFF` 且传播目标带 `PROPAGATE_AOF` 位。命令本身何时被登记为要传播，属于命令执行层（a05），本页不重述。

**落盘侧：`flushAppendOnlyFile`。** 它不是本页的 refs 符号，但它是 `aofWrite` 的唯一调用者，逻辑必须交代清楚，否则 `aofWrite` 的返回语义无处安放。

- **空缓冲的分支。** `sdslen(server.aof_buf) == 0` 时不是简单返回：如果「已请求 fsync 的偏移」等于「当前 incr 文件大小」，说明数据都已落盘，此时把 `fsynced_reploff_pending` 推进到 `server.master_repl_offset`——注释解释这是为了避免 `WAITAOF` 挂住：模块用 `RM_Call` 加 `NO_AOF` 标志时复制偏移会前进，但 AOF 侧没有理由调 fsync，`WAITAOF` 可能等在一个只传播给副本、根本没进 AOF 的偏移上。否则，`everysec` 模式下距上次 fsync 超过 1000 毫秒且没有 fsync 在飞行中，就走 `try_fsync`；`always` 模式无条件走 `try_fsync`（注释说这一支也覆盖了「策略从 everysec 改成 always」的情况）。
- **推迟写入。** `everysec` 且 `force` 为 0 时，如果后台 fsync 还在进行中就推迟这次写：第一次推迟记下 `server.aof_flush_postponed_start = server.mstime`；两秒内再被调用就继续推迟；超过两秒则不再等，`server.aof_delayed_fsync` 加一并照写，同时打一条 NOTICE「Asynchronous AOF fsync is taking too long (disk is busy?). Writing the AOF buffer without waiting for fsync to complete, this may slow down Redis.」
- **真正写。** `aofWrite(server.aof_fd, server.aof_buf, sdslen(server.aof_buf))` 前后套 `latencyStartMonitor` / `latencyEndMonitor`，耗时按场景记成三个延迟事件之一：后台 fsync 在飞行中记 `"aof-write-pending-fsync"`，有活跃子进程记 `"aof-write-active-child"`，两者都没有记 `"aof-write-alone"`；此外还统一记一条 `"aof-write"`。写成功后 `server.aof_flush_postponed_start` 清零。写之前还有一条调试用的休眠：`server.aof_flush_sleep` 非零时先 `usleep`，这个值由 `DEBUG SLEEP` 设置，注释说是给测试用的。
- **短写与写失败。** 写出的字节数与缓冲长度不等时分两种：`aofWrite` 返回 -1 说明是写失败（errno 保留），记 `server.aof_last_write_errno = errno`；否则是短写，先用 `ftruncate(server.aof_fd, server.aof_last_incr_size)` 把这次写出的半截内容截掉——源码注释说截不掉就会在下次启动时拒绝加载这个 AOF——截成功则把 `nwritten` 置成 -1，失败只打一条 WARNING；`server.aof_last_write_errno` 记为 `ENOSPC`。错误日志按 `AOF_WRITE_LOG_ERROR_RATE`（源码里是 30 秒）限频。
- **错误后的两条路。** `always` 策略直接 `exit(1)`，注释说得很直接：回复已经在输出缓冲里、数据集改动也无法回滚，与用户有「已确认的写已同步到盘」的契约，所以只能退出。其他策略则把 `server.aof_last_write_status` 置为 `C_ERR`（注释说这是为了「stop accepting writes as long as the error condition is not cleared」），把没写出去的部分留在缓冲里（用 `sdsrange` 裁掉已写出的前缀），然后返回，下次调用再试。写入被拒的判定在 `src/server.c` 的 `writeCommandsDeniedByDiskError`：`aof_state` 不为 OFF 且 `aof_last_write_status == C_ERR`（或后台 fsync 的状态是错误）时返回 `DISK_ERROR_TYPE_AOF`，命令预检据此拒绝写命令。恢复路径是 `src/server.c` 的 cron 里每 1000 毫秒重试一次 `flushAppendOnlyFile(0)`；写成功后 `aof_last_write_status` 会被改回 `C_OK` 并打一条 NOTICE「AOF write error looks solved, Redis can write again.」
- **缓冲的复用。** 写成功后 `aof_current_size` 与 `aof_last_incr_size` 加上本次写出的字节数。缓冲本身尽量复用：空闲空间够大（`sdslen` 加 `sdsavail` 小于 4000）就 `sdsclear` 清空复用，否则释放并重建一个空的（源码注释说这个 4000 的上限来自 sds 的 4KB 分配档位减去一些开销，本身是任意的）。
- **fsync。** `try_fsync` 段：配置 `no-appendfsync-on-rewrite` 打开且有活跃子进程时直接返回不 fsync；`always` 策略调 `redis_fsync`（注释说 Linux 下它是 `fdatasync()`，为了不刷元数据），失败就 `exit(1)`，成功后更新 `aof_last_incr_fsync_offset`、`aof_last_fsync`，并把 `fsynced_reploff_pending` 推进到当前复制偏移；`everysec` 策略在距上次 fsync 超过 1000 毫秒且当前没有 fsync 在飞行中时，调 `aof_background_fsync` 把 fsync 交给后台 bio 线程——这就是「everysec」的实现方式。
- **后台 fsync 在飞行中的判定。** `aofFsyncInProgress` 查的是 `bioPendingJobsOfType(BIO_AOF_FSYNC) != 0`，也就是 bio 线程队列里有没有待做的 AOF fsync 任务。

**`aofWrite` 本身。** 它是 `write(2)` 的重试封装：循环写直到写完，`errno == EINTR` 直接重试；写失败时返回「已写出的字节数」（一个字节都没写成就返回 -1）。返回已写字节数而不是一律 -1 是关键——调用方靠它区分「完全没写出去」与「写了一半」，前者是错误，后者是短写，可以尝试用 `ftruncate` 收拾。函数头注释解释了为什么对块设备也要重试短写：「normally if the first call is short, there is a end-of-space condition, so the next is likely to fail. However apparently in modern systems this is no longer true, and in general it looks just more resilient to retry the write.」

**多部分 AOF 与文件切换。** `openNewIncrAofForAppend` 在 rewrite 时打开新的 incr 文件：`AOF_WAIT_REWRITE` 状态下先开一个临时 incr 文件承接写入，事后由 rewrite 收尾流程改名并写进 manifest；`AOF_ON` 状态下则复制一份 manifest、算出新 incr 文件名、打开文件并把 manifest 同步持久化，然后才替换 `server.aof_fd`。旧 fd 走 `aof_background_fsync_and_close`（后台 fsync 之后关闭）。注释明确说「开新文件」与「持久化 manifest」这两步是原子的，任一步失败整体回滚。

**开关路径。** `startAppendOnly` 把状态置为 `AOF_WAIT_REWRITE` 再触发后台 rewrite；有别的后台任务在跑或正在事务里时只置 `server.aof_rewrite_scheduled = 1` 推迟执行。`stopAppendOnly` 先 `flushAppendOnlyFile(1)` 强制落盘并 fsync，再关 fd、把状态置回 `AOF_OFF`、清空缓冲。注意 `startAppendOnly` 开头会显式忽略上一次的错误状态（`aof_last_write_status` 与后台 fsync 的状态），日志写的是「AOF reopen, just ignore the last error.」

**退化路径与真实调用方。** `flushAppendOnlyFile` 的调用点：`src/server.c` 的 `beforeSleep`（每轮事件循环睡前，force 为 0）、`src/server.c` 的 cron（推迟刷盘时 force 为 0；写失败后每 1000 毫秒重试一次，force 为 0）、`src/aof.c` 的 `stopAppendOnly`（force 为 1）、`src/aof.c` 的 `rewriteAppendOnlyFileBackground`（force 为 1）、`src/aof.c` 的备份封存路径（force 为 1）。rewrite 前那次强制刷盘带一条明确的放弃条件：缓冲没清空就拒绝这次 rewrite——源码注释说，如果缓冲里还有没写出去的命令就换新文件，这些命令会在新文件里再写一遍，重放时就会被执行两次，而部分写还可能让一条命令跨在两个文件之间。

## 关键取舍

**源码事实：** AOF 存命令而不是数据，重放即还原。**源码事实：** 命令先进内存缓冲、由事件循环睡前统一落盘。**源码事实：** fsync 策略三档，`always` 下写失败直接退出进程，`everysec` 下后台 fsync 最多被推迟两秒。**源码事实：** `aofWrite` 返回已写字节数，让调用方能区分短写与完全失败，并用 `ftruncate` 把半截内容截掉。**源码事实：** 写失败后 AOF 进入错误状态，`writeCommandsDeniedByDiskError` 据此拒绝写命令。**源码事实：** AOF 里存的命令与复制流里的是同一条，源码注释明确说不需要为 AOF 做单独转换。**源码事实：** rewrite 换文件前必须先把缓冲清空，否则命令会被写两遍。**源码事实：** `everysec` 的 fsync 由 bio 后台线程执行，主线程不阻塞。

**架构推论：** 「记命令」把持久化成本从「与数据集大小成正比」变成「与写命令条数成正比」，代价是同样数据量下 AOF 往往比 RDB 大，且重放耗时与写命令条数成正比而不是与数据量成正比。**架构推论：** 内存缓冲加睡前统一落盘把 N 次 `write` 系统调用合成 1 次，代价是落盘延迟被推到命令回复之前的那一轮事件循环，`appendfsync=always` 时这段延迟直接加在命令延迟上。**架构推论：** `always` 写失败就退出，是把「已确认的写」当成不可撤销的承诺，代价是一次磁盘故障会变成一次服务中断；`everysec` 则把同样的风险转成「最多丢一秒的写」。**架构推论：** 把 fsync 放进 bio 线程让主线程不被磁盘拖住，但 `everysec` 语义因此只是「后台线程每秒做一次」而不是「每秒一定完成」，磁盘繁忙时会滑到两秒以上。**架构推论：** AOF 与复制流共用同一种命令表示，使一条写命令的持久化路径只有一条，少了一处可能出错的地方；代价是无法表达「与复制不同的内容」，像 Lua 的 `redis.set_repl()` 这类传播限制会同时作用在 AOF 上。主核的多模态写入若要做类似的写前日志，日志粒度（一条写请求 vs 一个页）会直接决定重放成本——主核更适合页级日志而不是命令级日志。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/aof.c` | `feedAppendOnlyFile` | 把一条写命令编码成 RESP（含 SELECT 注入与时间戳注解）追加进内存 AOF 缓冲 |
| `src/aof.c` | `aofWrite` | `write(2)` 的重试封装，处理 EINTR 与短写，返回实际写出的字节数 |

## 参考

- [Redis 源码：aof.c](https://github.com/redis/redis/blob/unstable/src/aof.c) —— AOF 追加与落盘的实现。
- [Redis 源码：server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— AOF 状态、fsync 策略与相关常量的定义。
- [Redis 文档：持久化](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/) —— AOF 的三种 fsync 策略与取舍。
- [Redis 文档：AOF 与多部分文件组织](https://redis.io/docs/latest/operate/oss_and_stack/management/persistence/#append-only-file) —— base/incr 拆分与 rewrite 的对外描述。
- [Redis 命令：CONFIG SET](https://redis.io/docs/latest/commands/config-set/) —— 运行时开关 AOF 与切换 fsync 策略的入口。
- [Redis 命令：BGREWRITEAOF](https://redis.io/docs/latest/commands/bgrewriteaof/) —— 触发 AOF rewrite、切换 incr 文件的入口。

## 与主核 / AI 的关系

主核若要做写前日志，Redis 这套「命令级日志」的价值主要在方法论而不在格式：把「追加进内存缓冲」与「落盘」分成两段、并在回复之前保证落盘完成，是任何需要「已确认即持久」的存储引擎都要满足的不变式。但主核是多写者引擎且数据是变长的多模态对象，命令级日志的重放成本会随对象大小放大——Redis 的一条 `SET` 是定长的，主核的一条「写入一个 1536 维向量」不是。因此主核更该抄的是「持久化必须早于对客户端可见」这条顺序约束（Redis 把它固化在 `beforeSleep` 的注释与顺序里），而不是 RESP 命令流这种具体载体。

另一个可借的点是错误语义的分层：Redis 把「写失败」分成不可恢复（`always` 策略下退出）与可恢复（保留缓冲、下次重试、期间拒绝写命令）两种，并给可恢复路径配了明确的拒绝写入的判定入口。主核的多写者引擎在磁盘故障时同样需要这两条清晰的边界，而不是让部分写入悄悄留在日志里。

AI 侧若把 Redis 当作向量检索的缓存层，AOF 的存在会把「每一条缓存写入」变成一次要落盘的命令，缓存的写入频率通常远高于业务写入，磁盘会被大量可丢弃的数据刷满；更实际的做法是把 AI 缓存放在独立的、不开 AOF 的实例上，靠上层重建——这也正是 `appendfsync=always` 之外、AOF 本身就该在纯缓存场景被关掉的原因。
