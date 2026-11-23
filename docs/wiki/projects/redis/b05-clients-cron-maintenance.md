---
project: redis
feature: clients-cron-maintenance
page_type: feature
schema_version: 1
refs:
  - path: src/server.c
    symbols: [clientsCron, clientsCronResizeQueryBuffer, clientsCronResizeOutputBuffer]
---

# 客户端定期维护

## 一句话职责

周期性给客户端做体检：回收查询缓冲里多余的空间、按实际峰值调整输出静态缓冲的大小、并记下最近占用最大的客户端。

## 机制是什么

`clientsCron` 是 `serverCron` 每轮都会调的维护入口（见 a03）。它要面对一个矛盾：客户端可能有成千上万个，但每一轮 cron 的时间预算有限，逐个全量处理会把 cron 变成延迟来源。

Redis 的解法是**增量式公平分摊**——每轮只处理 `numclients / server.hz` 个客户端，靠把链表头节点轮转到尾部来保证每个客户端最终都会被轮到。真正对单个客户端动手的是三个函数：`clientsCronResizeQueryBuffer`（回收输入缓冲冗余）、`clientsCronResizeOutputBuffer`（调整输出静态缓冲大小）、以及配套的峰值跟踪。

本页只讲周期性维护本身。客户端的创建与释放在 b01；因为内存压力而**主动**踢客户端是 b06。

## 怎么实现的

**`clientsCron` 的分摊逻辑。**

```c
int numclients = listLength(server.clients);
int iterations = numclients/server.hz;
if (iterations < CLIENTS_CRON_MIN_ITERATIONS)
    iterations = (numclients < CLIENTS_CRON_MIN_ITERATIONS) ?
                 numclients : CLIENTS_CRON_MIN_ITERATIONS;
```

`CLIENTS_CRON_MIN_ITERATIONS` 在 `src/server.h` 里定义为 5。注释解释：即使达不到「每秒处理每个客户端一次」的契约，也至少处理几个客户端。

然后是峰值槽的推进：

```c
CurrentPeakMemUsageSlot = server.unixtime % CLIENTS_PEAK_MEM_USAGE_SLOTS;
int zeroidx = (CurrentPeakMemUsageSlot+1) % CLIENTS_PEAK_MEM_USAGE_SLOTS;
ClientsPeakMemInput[zeroidx] = 0;
ClientsPeakMemOutput[zeroidx] = 0;
```

`CLIENTS_PEAK_MEM_USAGE_SLOTS` 在 `src/server.c` 里定义为 8。这一段的注释解释了整个环形数组方案：数组按 `UNIXTIME % 8` 索引，每个槽对应最近的一秒；想知道「最近几秒的峰值」只需扫这 8 个槽取最大值，而不必对全部客户端做一次 O(N) 扫描——注释说这是为了避免 `INFO` 命令变慢。注释也坦白了它的不精确之处：若 `serverCron` 因慢命令而没能按正常频率运行，数组里可能残留超过 8 秒的旧数据；「但这不是问题，因为我们只想知道从内存占用的角度看，最近是否出现过特别奢侈的客户端」。

遍历循环：

```c
while(listLength(server.clients) && iterations--) {
    head = listFirst(server.clients);
    c = listNodeValue(head);
    listRotateHeadToTail(server.clients);
    if (c->tid != IOTHREAD_MAIN_THREAD_ID) continue;
    clientsCronRunClient(c);
}
```

`listRotateHeadToTail` 就是公平性的实现——注释写明「取当前头节点，处理它，然后把头转到尾，这样我们就能一步一步公平地迭代所有客户端」。`c->tid != IOTHREAD_MAIN_THREAD_ID` 的客户端被跳过，注释说明由 I/O 线程的 `IOThreadClientsCron`（定义在 `src/iothread.c`）处理。

**`clientsCronRunClient` 是单客户端的维护流水线**（`src/server.c`）。它的注释定下了协议：各个检查函数返回非零表示客户端应被终止。它依次做：

1. `clientsCronHandleTimeout(c, now)`（定义在 `src/timeout.c`）——超时检查。
2. `clientsCronResizeQueryBuffer(c)`。
3. `clientsCronResizeOutputBuffer(c, now)`。
4. `clientsCronTrackExpansiveClients(c)`。
5. 每秒刷新一次非共享回复字节：`int unshared_refreshed = c->last_unshared_refresh + 1000 - 1000/server.hz <= now;`。注释解释这个「多加一个 tick 的余量」：否则一个客户端若在上次刷新后略少于一秒被访问到，就得再等一轮，刷新会被推迟到约 2 秒。
6. `updateClientMemUsageAndBucketInternal(c, unshared_refreshed)`；若返回 0（该客户端不可驱逐）则退化为 `updateClientMemoryUsage(c)`。注释解释了为什么不能直接遍历所有客户端算一遍：那太慢，会让 `INFO` 命令太慢，所以改成增量计算。
7. `closeClientOnOutputBufferLimitReached(c, 0)`——这是本组里**唯一**的同步关闭点（见 b03）。

**`clientsCronResizeQueryBuffer`：回收输入缓冲的冗余。**

```c
if (c->querybuf == NULL) return 0;          /* 用的是可复用查询缓冲，无事可做 */
size_t querybuf_size = sdsalloc(c->querybuf);
time_t idletime = server.unixtime - c->lastinteraction;
if (sdsavail(c->querybuf) > 1024*4) {
```

注释说：只有当缓冲确实浪费了至少几 KB 时才值得回收（`1024*4` 是硬编码字面量）。触发条件有两个：

**条件一：空闲超过 2 秒**（`idletime > 2`）。

```c
size_t remaining = sdslen(c->querybuf) - c->qb_pos;
if (!(c->flags & CLIENT_MASTER) && !remaining) {
    sdsfree(c->querybuf);
    c->querybuf = NULL;
} else {
    c->querybuf = sdsRemoveFreeSpace(c->querybuf, 1);
}
```

注释解释：客户端不是主库且没有待处理数据时，可以安全地在下次读取时改用可复用查询缓冲，所以直接把 `querybuf` 释放掉并置 NULL；**不立刻**把它挪进可复用缓冲，因为如果给这个客户端分配过私有查询缓冲，说明它很可能很快还会再用。

**条件二：缓冲相对近期峰值过大。**

```c
} else if (querybuf_size > PROTO_RESIZE_THRESHOLD && querybuf_size/2 > c->querybuf_peak) {
    size_t resize = sdslen(c->querybuf);
    if (resize < c->querybuf_peak) resize = c->querybuf_peak;
    if (c->bulklen != -1 && resize < (size_t)c->bulklen + 2) resize = c->bulklen + 2;
    c->querybuf = sdsResize(c->querybuf, resize, 1);
}
```

`PROTO_RESIZE_THRESHOLD` 在 `src/server.h` 里定义为 `1024*32`（32KB）。注释说明裁剪的边界：不裁到近期峰值以下，也不裁到小于当前 bulk 长度（正在读一个 bulk 时不能把缓冲缩到装不下它）。

收尾是把峰值重置，供下一轮比较：

```c
c->querybuf_peak = c->querybuf ? sdslen(c->querybuf) : 0;
if (c->bulklen != -1 && (size_t)c->bulklen + 2 > c->querybuf_peak)
    c->querybuf_peak = c->bulklen + 2;
return 0;
```

函数注释明确：**它永远返回 0，因为它从不终止客户端**。

**`clientsCronResizeOutputBuffer`：按峰值调整输出静态缓冲的大小。** 函数头的注释给出了完整规则：若最近观察到的峰值等于缓冲大小，就扩容一倍；若峰值不到缓冲大小的一半，就缩一半。峰值每 `server.reply_buffer_peak_reset_time` 毫秒重置一次。

```c
const size_t buffer_target_shrink_size = c->buf_usable_size/2;
const size_t buffer_target_expand_size = c->buf_usable_size*2;
if (!server.reply_buffer_resizing_enabled) return 0;
if (c->buf_encoded) return 0;
```

注释解释了第二条早退：编码缓冲里记录着「最后部分写入的 `payloadHeader` 指针」，重新分配会让这个指针失效，所以编码缓冲不参与 resize。

两条调整路径：

```c
if (buffer_target_shrink_size >= PROTO_REPLY_MIN_BYTES &&
    c->buf_peak < buffer_target_shrink_size )
{
    new_buffer_size = max(PROTO_REPLY_MIN_BYTES, c->buf_peak+1);
    server.stat_reply_buffer_shrinks++;
} else if (buffer_target_expand_size < PROTO_REPLY_CHUNK_BYTES*2 &&
    c->buf_peak == c->buf_usable_size)
{
    new_buffer_size = min(PROTO_REPLY_CHUNK_BYTES, buffer_target_expand_size);
    server.stat_reply_buffer_expands++;
}
```

`PROTO_REPLY_MIN_BYTES` 在 `src/server.h` 里定义为 `1024`（回复缓冲的下限），`PROTO_REPLY_CHUNK_BYTES` 是 `16*1024`。把两个条件合起来读：**收缩的下限是 1KB**（`max(PROTO_REPLY_MIN_BYTES, buf_peak+1)`）；**扩容止步于 16KB**——扩容分支的成立条件是 `buf_usable_size*2 < PROTO_REPLY_CHUNK_BYTES*2`，也就是「当前可用大小还不到 16KB」才允许扩，而扩完的新大小取 `min(PROTO_REPLY_CHUNK_BYTES, buf_usable_size*2)`，最多到 16KB。静态缓冲一旦涨到 16KB，条件 `16384*2 < 32768` 就不再成立，于是永远停在 16KB，超出的回复必然落到回复链表里。

新缓冲必须装得下已写入的字节，这一点由断言保证：

```c
serverAssertWithInfo(c, NULL, (!new_buffer_size) || (new_buffer_size >= (size_t)c->bufpos));
```

峰值重置：

```c
if (server.reply_buffer_peak_reset_time >= 0 &&
    now_ms - c->buf_peak_last_reset_time >= server.reply_buffer_peak_reset_time)
{
    c->buf_peak = c->bufpos;
    c->buf_peak_last_reset_time = now_ms;
}
```

注释说这是为了让空闲客户端开始收缩。

真正替换时用 `zmalloc_usable(new_buffer_size, &c->buf_usable_size)` 新分配、`memcpy` 走已用的 `c->bufpos` 字节、`zfree` 旧缓冲。函数同样**恒返回 0**。

**峰值跟踪 `clientsCronTrackExpansiveClients`。** 输入侧用量 = `querybuf` 的大小 + `all_argv_len_sum` + `argv` 数组本身；输出侧用量 = `getClientOutputBufferMemoryUsage(c)`。两者分别与当前秒槽里的历史最大值比较并取大。

## 关键取舍

**源码事实：** 每轮只处理 `numclients / server.hz` 个客户端，靠链表头尾轮转实现公平分摊；最少数是 5。**源码事实：** 两个 resize 函数都恒返回 0，从不终止客户端；终止判断集中在 `clientsCronRunClient` 的最后一行。**源码事实：** 查询缓冲的回收有两个互相独立的触发条件——空闲超 2 秒、或相对近期峰值过大（阈值 32KB）。**源码事实：** 输出静态缓冲的收缩下限是 1KB、扩容止步于 16KB（`PROTO_REPLY_CHUNK_BYTES`），编码缓冲不参与 resize。**源码事实：** 峰值跟踪用 8 槽环形数组，按 `unixtime % 8` 索引。**源码事实：** I/O 线程上的客户端由 `IOThreadClientsCron` 单独处理，主线程的 `clientsCron` 直接跳过。

**架构推论：** 把全量维护改成增量分摊，是把「每个客户端每秒一次」的软契约换成了「平均每秒一次、偶尔某个客户端被多等一轮」的硬摊销——源码注释自己承认这个契约「无法严格保证」，因为 `serverCron` 可能因为慢命令而低于 `server.hz` 的频率。**架构推论：** 输出缓冲「按峰值缩放」比「固定 16KB」更省内存，代价是客户端会经历缓冲反复扩缩带来的 `realloc` 抖动；把扩容止步在 16KB（`PROTO_REPLY_CHUNK_BYTES`）是刻意的分界——静态缓冲是热路径上的单块内存，再大就该交给回复链表分段管理，而不是让单块缓冲无节制地增长。**架构推论：** 查询缓冲在空闲后直接释放（而不是原地收缩）省掉了「下次还要再分配」的往返，代价是要靠 `c->querybuf == NULL` 这个额外状态来表示「当前没有私有缓冲」——Redis 用一个线程局部的可复用缓冲兜住了这个空窗。**架构推论：** 峰值跟踪用固定 8 槽而非精确时间戳，是拿精度换常数级开销；源码注释主动承认了这个不精确性并说明为什么可以接受。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `src/server.c` | `clientsCron` | 增量遍历客户端：按 `numclients/server.hz` 分摊迭代、推进峰值槽、跳过 I/O 线程客户端 |
| `src/server.c` | `clientsCronResizeQueryBuffer` | 回收查询缓冲冗余：空闲后释放、或按近期峰值裁剪，并重置峰值 |
| `src/server.c` | `clientsCronResizeOutputBuffer` | 按峰值缩放输出静态缓冲：过载扩容、闲置收缩，带上下限与编码缓冲豁免 |

## 参考

- [Redis 源码：server.c](https://github.com/redis/redis/blob/unstable/src/server.c) —— 客户端定期维护与内存桶更新的实现。
- [Redis 源码：server.h](https://github.com/redis/redis/blob/unstable/src/server.h) —— `PROTO_RESIZE_THRESHOLD`、`PROTO_REPLY_MIN_BYTES`、`PROTO_REPLY_CHUNK_BYTES`、`CLIENTS_CRON_MIN_ITERATIONS` 等常量。
- [Redis 文档：内存优化](https://redis.io/docs/latest/operate/oss_and_stack/management/optimization/memory-optimization/) —— 客户端缓冲占用与内存统计的官方说明。
- [Redis 命令：INFO](https://redis.io/docs/latest/commands/info/) —— 客户端内存与峰值统计的对外呈现。

## 与主核 / AI 的关系

「把周期性维护摊到时间片上、按增量而非全量做」是 Redis 在单线程里的必然选择，主核的多写者引擎可以直接用后台线程做全量维护，不必学这套摊销。但**按实际峰值缩放缓冲**这个思路值得主核借鉴：Redis 的客户端缓冲大小是自适应的，主核的会话/结果缓冲若是固定大小，在「少量大请求」与「大量小请求」两种负载下都会浪费——主核可以按会话历史峰值为每个连接分配缓冲档位。对 AI 场景尤其相关：一次推理的 token 输出量跨度极大，固定缓冲要么太小造成反复扩缩，要么太大长期占用；按会话峰值调整更贴合实际。此外 Redis 的「空闲即释放私有缓冲」也提示主核：长连接 AI 会话在两轮推理之间的空闲期应当归还内存，而不是一直占着。
