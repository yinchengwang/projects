---
project: postgres
feature: recovery-wal-prefetch
page_type: feature
schema_version: 1
refs:
  - path: src/backend/access/transam/xlogprefetcher.c
    symbols: [XLogPrefetcherAllocate, XLogPrefetcherReadRecord, XLogPrefetchReconfigure]
---

# Redo WAL 预取

## 一句话职责

`XLogPrefetcher` 在恢复进程应用一条 WAL record 的同时向前扫描后续 record 的块引用，并提前请求主 fork 数据页 I/O，尽可能把磁盘等待藏到 redo 计算后面。

## 机制是什么

redo 已经按 WAL 顺序执行，但每条 record 可能触及一个或多个数据页；若应用 record 时才逐页读盘，恢复进程会把 I/O 延迟串在单线程回放路径上。预取器包装 `XLogReaderState`：它通过 `XLogReadAhead` 解码未来 WAL，遍历 `DecodedXLogRecord.blocks[]`，对未来将读入的页面调用 `PrefetchSharedBuffer`，并把请求按目标 record LSN 放进 `LsnReadQueue`。等 redo 走到对应 LSN，队列把更早的 I/O 视为已完成并补充新请求。源码文件头很谨慎：main fork 才考虑预取，而且只有 `PrefetchBuffer()` 能有效发出 read-ahead advice 的平台上可能有收益。

它不重排 redo，也不负责执行 rmgr 的 redo 函数；记录次序仍由恢复流程决定，预取只改变「页面读何时开始」。它也不是 A 组通用 read-stream：`XLogPrefetcher` 只存在于恢复 WAL 读路径，输入是解码后的 WAL block refs，行为受 recovery prefetch GUC 和 `maintenance_io_concurrency` 控制；一般关系扫描的 read-stream 是另一套消费 buffer/block sequence 的机制。预取器还必须防止在 replay 之前读取尚未创建、刚被截断或将被重用的文件号段，因此它不只是无脑扫 block refs，而带有按关系/数据库生效、按 LSN 解除的 filter。

## 怎么实现的

1. **恢复入口创建 wrapper。** `xlogrecovery.c` 创建 `XLogReaderState` 后把它交给 `XLogPrefetcherAllocate`。分配器设置 `reader`，建 `filter_table`（键是 `RelFileLocator`，值是 `XLogPrefetcherFilter`），初始化 filter 队列，清共享统计值；`streaming_read` 延后到首次读取时按 GUC 创建。释放时先释放队列，再销毁哈希表。
2. **按需创建有界环队列。** `LsnReadQueue` 保存回调、私有上下文、`max_inflight`、`inflight`、`completed`、`head`/`tail` 与柔性数组元素 `{io, lsn}`。环形数组留一个空格区分满/空；`lrq_prefetch` 只在 `inflight < max_inflight` 且尚有队列容量时调用 `XLogPrefetcherNextBlock`。该回调给出 `LRQ_NEXT_IO` 就计入在途 I/O，`LRQ_NEXT_NO_IO` 计为已完成（即不需要等 I/O），没更多 WAL 则 `LRQ_NEXT_AGAIN` 停止补充。
3. **超前读并跨越 record 边界。** `XLogPrefetcherNextBlock` 保留 `prefetcher->record` 与 `next_block_id` 作为游标。若无待处理 record，基于 reader 队列状态决定 `XLogReadAhead` 是否 nonblocking：已有可回放 record/error 时不阻塞；若没有东西可做，恢复进程可以阻塞等 WAL。无数据或错误时返回 AGAIN，并记录 `no_readahead_until`，避免反复对同一空缺位置发请求。预取被关掉时仍可能让 reader 解码一条记录，但明确不发数据页 I/O。
4. **处理结构变化导致的潜在误读。** 预读到 shutdown checkpoint 或 end-of-recovery record 时，暂停 readahead 直到 replay 过该记录，防 timeline 改变时 read TLI/replay TLI 不一致。database file-copy create、SMGR create 与 truncate 会建立 filter，直到对应 LSN 被 replay 才清；这可避免看到旧文件、重用 relfilenumber 或文件复制还未落盘时预取出错。即使没有相应 WAL 过滤，读物理文件前也会查 relation 是否存在、`smgrnblocks` 是否足够；不满足就把范围记入 filter，而不是产生 ENOENT 或越界读取。
5. **逐块执行过滤与去重。** 对每个 `DecodedBkpBlock`，跳过非 main fork、带 full-page image（redo 可直接用镜像）、`BKPBLOCK_WILL_INIT`（页面将零初始化）、filter 命中，以及最近 `XLOGPREFETCHER_SEQ_WINDOW_SIZE` 个相同 relation/block（常量当前为 4）窗口里的重复引用。之后 `smgropen`、检查 relation 存在和块数，再调用 `PrefetchSharedBuffer`。buffer 已命中就把 `recent_buffer` 附在 decoded block 上供后续 redo 复用；miss 且发起了 I/O 则由 LSN 队列登记为在途。
6. **replay 推进队列并重配。** `XLogPrefetcherReadRecord` 先检查全局 `XLogPrefetchReconfigureCount` 是否变化；GUC change 只递增这个版本计数，reader 在下一次调用时重建队列。启用时 `max_inflight = maintenance_io_concurrency`，`max_distance = max_inflight * XLOGPREFETCHER_DISTANCE_MULTIPLIER`（源码常量为 4）；禁用时两者都设为 1，让 reader 仍能按包装接口读取但不会执行预取。随后 `XLogReleasePreviousRecord` 得到已 replay 到哪，`XLogPrefetcherCompleteFilters` 删除已过期 filter，`lrq_complete_lsn` 将更早 LSN 对应的 I/O 从 in-flight 计数中回收并继续填队列，最后 `XLogNextRecord` 交付下一条 record。

并发语义上，队列、过滤器和当前记录游标属于 startup/recovery backend 的本地对象；共享统计通过 `pg_atomic_uint64` 计数器与普通瞬时字段写入，`pg_stat_recovery_prefetch` 读取。它不需要跨进程锁协调每个预取请求；共享面只用于统计，而不是任务队列。

## 关键取舍

**只预取 main fork，不覆盖所有 fork。** 源码文件头明确标为「Currently, only the main fork is considered」。放弃的是 FSM/VM 等 fork 的潜在缓存命中；换来的是块引用类型处理更简单，并避免对没有 redo 数据读需求的 fork 发出多余 read-ahead。扩展到所有 fork 会要求仔细验证各 fork 的 redo 访问时点与结构操作过滤，不是仅把 fork 条件删掉即可。

**异步预取而非恢复线程同步等待每个页。** `PrefetchSharedBuffer` 的 miss 路径只发起 I/O，redo 后续真正 `XLogReadBufferForRedo` 才需要页面；若到时 I/O 尚未完成，等待仍会发生。放弃的是「完全消除读延迟」的保证，换来可在处理中间 WAL 时并行推进磁盘读取。退化路径在 `XLogPrefetcherNextBlock` 可见：buffer 已命中、full-page image、初始化页、过滤页、重复页或平台不支持预取时都返回 `LRQ_NEXT_NO_IO`，不会等待。真实 caller 是 `xlogrecovery.c` 的 `ReadRecord` 包装：它只在下一次取 WAL record 时推进队列，redo 顺序并未改写。

**用 LSN filter 保守跳过可能暂时不存在的页，而非提前同步创建/查明数据文件代际。** 比如 `XLOG_SMGR_CREATE` 到 replay 前，relation 物理文件可能不存在；预取到旧 relfilenumber 的请求也可能指向另一个代际。源码用 `XLogPrefetcherAddFilter` 记录 `filter_until_replayed` 与 `filter_from_block`，只要 `filter_until_replayed >= replaying_lsn` 就继续过滤。放弃的是这段时间的潜在并行 I/O，避免把 redo 尚未执行的文件系统变化错误地当作当前状态。替代方案是让预取器复制 redo 的文件操作并维护代际版本，复杂度更高，也容易与真正的 redo 顺序脱节。

**超前距离不是无限扩大。** `max_distance` 由 `maintenance_io_concurrency * XLOGPREFETCHER_DISTANCE_MULTIPLIER`（乘数宏定义为 4）界定，`max_inflight` 直接受该 GUC 限制。放弃的是在高吞吐存储上尽量多地提前读盘的可能，换来有界内存队列与可配置 I/O 压力；不设上限会把恢复所需缓存页大量挤占 shared buffers，并在大量随机块引用时制造过多并发 I/O。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/access/transam/xlogprefetcher.c` | `XLogPrefetcherAllocate` | 把既有 `XLogReaderState` 包成恢复期预取器，创建关系 filter 哈希与队列状态，但把 I/O 队列推迟到首次取 record 时按配置初始化 |
| `src/backend/access/transam/xlogprefetcher.c` | `XLogPrefetcherReadRecord` | 对外保持 `XLogReadRecord` 式读取接口；回收已 replay 的 LSN 队列项、处理结构过滤、按 GUC 重建受限队列，再交付下一条 WAL record |
| `src/backend/access/transam/xlogprefetcher.c` | `XLogPrefetchReconfigure` | GUC assign hook 调用时只增加变更代数；真正销毁/重建队列发生在下一次 `XLogPrefetcherReadRecord`，避免配置写入点直接改正在使用的队列 |

## 参考

- [PostgreSQL 文档：恢复配置 `recovery_prefetch`](https://www.postgresql.org/docs/current/runtime-config-wal.html) —— 预取开关与 `maintenance_io_concurrency` 配置关系
- [PostgreSQL 文档：恢复期间统计视图](https://www.postgresql.org/docs/current/monitoring-stats.html) —— `pg_stat_recovery_prefetch` 的观测字段
- [PostgreSQL 官方源码：xlogprefetcher.c](https://github.com/postgres/postgres/blob/master/src/backend/access/transam/xlogprefetcher.c) —— 预读窗口、过滤与队列实现

## 与主核 / AI 的关系

暂无直接对应。
