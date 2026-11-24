---
project: mysql-server
feature: redo-log-write
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/log/log0write.cc
    symbols: [log_write_up_to, log_write_buffer]
---

# redo 日志写入

## 一句话职责

`storage/innobase/log/log0write.cc` 里的 redo 日志写入机制负责把用户线程产生在日志缓冲（log buffer）中的修改记录，按 512 字节的日志块（log block）整理好、带好块头与块尾校验和之后写入当前的 redo 日志文件，并用单调递增的 LSN（`log.write_lsn`）标记「已经写到文件里」的位置；后台 log writer 线程 `log_writer()` 是这条路径上唯一的写盘执行者，用户线程只负责通过 `log_write_up_to()` 请求「把 redo 写到某个 LSN」并在条件变量上等待。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责把修改记录进 redo 日志缓冲并按 LSN 推进落盘；**不负责崩溃恢复（I03）**、**不负责脏页写回（H02）**、**不负责 undo 日志（K 组）**。

## 机制是什么

### 三个 LSN 与两个推进者

redo 日志的生命周期在 `log0write.cc` 里由三个 LSN 划界：

| LSN | 含义 | 谁推进 |
|---|---|---|
| `log.write_lsn` | redo 已经写进 redo 日志文件（进入 OS 缓存）到的位置 | log writer 线程，在 `log_write_buffer()` 里 `log.write_lsn.store(new_write_lsn)` |
| `log.flushed_to_disk_lsn` | redo 已经 fsync 落盘到的位置 | log flusher 线程，在 `log_flush_low()` 里 `log.flushed_to_disk_lsn.store(flush_up_to_lsn)` |
| `log.last_checkpoint_lsn` | 检查点位置，是 redo 空间回收的基准 | log checkpointer 线程（本组页面 I02 覆盖） |

源码注释把这条分工写得很明确（`log0write.cc` 文件头 `PAGE_INNODB_REDO_LOG_THREADS` 一节）：log writer 只负责「copies data to system buffers」，做 fsync 的是 log flusher。

**写与刷是两件事**：写进文件不等于落盘。`log_write_up_to(log, end_lsn, flush_to_disk)` 的 `flush_to_disk` 参数决定这次请求只是「写」，还是「写完还得刷到盘」。

### 日志块格式：512 字节一块

`log0write.cc` 的块格式注释（`Log_data_block_header` 一节）给出了固定布局：

- 块大小 `OS_FILE_LOG_BLOCK_SIZE` = **512 字节**。
- 头部 `LOG_BLOCK_HDR_SIZE` = **12 字节**：包含 LSN、数据长度、首个记录组偏移。
- 尾部 `LOG_BLOCK_TRL_SIZE` = **4 字节**：校验和（checksum）。
- 数据区的合法偏移范围是 `[LOG_BLOCK_HDR_SIZE, OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE)`，也就是 `[12, 508)`。

这些常量直接决定了 `log_write_up_to()` 里的三条断言（`end_lsn % OS_FILE_LOG_BLOCK_SIZE` 的三种取值）以及 `validate_start_lsn()` 里的对齐检查——**写入的 LSN 必须落在块边界，或者落在头部之后、尾部之前**。

### 两条写入路径：直接从日志缓冲写 / 经 write-ahead 缓冲写

`log_write_buffer()` 里有一个关键分叉，由 `compute_how_much_to_write()` 计算出的 `write_from_log_buffer` 布尔量决定：

1. **直接从日志缓冲写**（`write_from_log_buffer == true`）：至少凑满一个完整块（`write_size >= OS_FILE_LOG_BLOCK_SIZE`）时，块头块尾在日志缓冲里就地写好（`prepare_full_blocks()`），然后 `write_buf = buffer` 直接从日志缓冲发出。
2. **经 write-ahead 缓冲写**（`write_from_log_buffer == false`）：块还没写满时，先 `copy_to_write_ahead_buffer()` 把数据 memcpy 到 `log.write_ahead_buf`，把不完整的尾块补齐（填 `0x00`、写块头、算校验和 `log_block_store_checksum()`），然后 `write_buf = log.write_ahead_buf` 发出。

对应的两个监控计数器是 `MONITOR_LOG_FULL_BLOCK_WRITES` 与 `MONITOR_LOG_PARTIAL_BLOCK_WRITES`。

### write-ahead 的目的是避免 read-on-write

`compute_next_write_ahead_end()` 把当前文件偏移按 `srv_log_write_ahead_size` 对齐后加上一个 `srv_log_write_ahead_size`，得到「下一个 write-ahead 区域的末尾」。`log.write_ahead_end_offset` 记录已经预先写过的区域末端。

`compute_how_much_to_write()` 里的判断逻辑是：

- 若当前 write-ahead 区域还有空间（`current_write_ahead_enough(log, real_offset, write_size)` 为真），且是从日志缓冲写，则把 `write_size` 向下对齐到块边界（`ut_uint64_align_down(write_size, OS_FILE_LOG_BLOCK_SIZE)`），只写完整块。
- 若当前 write-ahead 区域**已经没有空间**，再看下一个 write-ahead 区域是否够大：
  - 够大 → `write_from_log_buffer = false`，改从 write-ahead 缓冲写，顺便把更大的区域一起 write-ahead 出去。
  - 不够大 → 数据比 write-ahead 区域还多，只能从日志缓冲写，并把 `write_size` 截断到 `next_wa - real_offset`。

**架构推论：** write-ahead 区域的存在意义是让一次物理写入覆盖一段尚未写满的日志块区间的后继区域，从而在设备上把「读-改-写」变成纯顺序写。这里的依据是 `write_blocks()` 的两条断言——`real_offset + write_size <= log.write_ahead_end_offset` 或 `(real_offset + write_size) % srv_log_write_ahead_size == 0`，即每次写入的末端必须落在已预写的区域内或区域边界上。

### 空间不足时的两条「刹车」

redo 日志空间是循环复用的，写到文件末尾要开新文件，写不动了要等检查点回收。三处刹车机制：

1. **文件末尾**：`compute_how_much_to_write()` 发现剩余空间不够时，若连开头一个字节都放不下，返回 `write_size == 0`，`log_write_buffer()` 随即调用 `start_next_file()` 切到新文件（内部调 `log_files_produce_file()`），并把 `log.write_ahead_end_offset` 归零。
2. **检查点限制**：`log_writer_wait_on_checkpoint()` 用 `log.m_capacity.hard_logical_capacity()` 算出 `hard_limited_lsn`，把本次写入的 LSN 上限压到检查点允许的范围。
3. **extra margin**：`log_writer_extra_margin_check()` 比较 `checkpoint_lsn` 对齐后的软上限 `log.m_capacity.soft_logical_capacity()` 与 `next_write_lsn`，越过就调 `log_writer_enter_extra_margin()` 进入应急态，并打警告 `ER_IB_MSG_LOG_WRITER_ENTERED_EXTRA_MARGIN`。

## 怎么实现的

### 用户线程侧：`log_write_up_to()`

`log_write_up_to(log_t &log, lsn_t end_lsn, bool flush_to_disk)` 是用户线程进入本机制的入口。它的执行顺序是：

**第一步，前置检查与断言：**

```
ut_a(!srv_read_only_mode);

if (recv_no_ibuf_operations) {
  /* Recovery is running and no operations on the log files are
  allowed yet... */
  return Wait_stats{0};
}
```

`recv_no_ibuf_operations` 为真表示恢复还在进行、日志文件暂不允许操作，此时**直接返回、什么都不做**。恢复期间的刷盘需求由恢复流程自己保证——注释说明 redo 记录在恢复期间被应用到页上导致页变脏，刷脏时发现 `newest_modification != 0` 会再回到 `log_write_up_to()`，但此时 redo 实际已经被刷过。

随后是三条关于 LSN 合法性的断言：

```
ut_a(end_lsn != LSN_MAX);

ut_a(end_lsn % OS_FILE_LOG_BLOCK_SIZE == 0 ||
     end_lsn % OS_FILE_LOG_BLOCK_SIZE >= LOG_BLOCK_HDR_SIZE);

ut_a(end_lsn % OS_FILE_LOG_BLOCK_SIZE <=
     OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_TRL_SIZE);
```

**第二步，区分「后台线程在跑」和「后台线程被暂停」：**

```
retry:
  if (log.writer_threads_paused.load(std::memory_order_acquire)) {
    /* the log writer threads are paused not to waste CPU resource. */
    wait_stats +=
        log_self_write_up_to(log, end_lsn, flush_to_disk, &interrupted);

    if (UNIV_UNLIKELY(interrupted)) {
      /* the log writer threads might be working. retry. */
      goto retry;
    }

    DEBUG_SYNC_C("log_flushed_by_self");
    return wait_stats;
  }
```

`log.writer_threads_paused` 为真时走 `log_self_write_up_to()`，由**用户线程自己**把 redo 写出去——先自旋等 `log_buffer_ready_for_write_lsn()` 追上 `end_lsn`，自旋次数上限是 `srv_n_spin_wait_rounds`；还追不上就用 `os_event_wait_time_low(log.closer_event, std::chrono::milliseconds{100}, sig_count)` 等 100 毫秒一次。`interrupted` 标志表示后台线程已重新工作（源码注释原文：the log writer threads might be working），需要 `goto retry` 重来。

**第三步，`flush_to_disk` 为真（要落盘）：**

```
if (log.flushed_to_disk_lsn.load() >= end_lsn) {
  DEBUG_SYNC_C("log_flushed_by_writer");
  return wait_stats;
}

if (srv_flush_log_at_trx_commit != 1) {
  /* We need redo flushed, but because trx != 1, we have
  disabled notifications sent from log_writer to log_flusher.
  ... */
  if (log.write_lsn.load() < end_lsn) {
    wait_stats += log_wait_for_write(log, end_lsn, &interrupted);
  }
}

/* Wait until log gets flushed up to end_lsn. */
wait_stats += log_wait_for_flush(log, end_lsn, &interrupted);
```

`innodb_flush_log_at_trx_commit` **不等于 1** 时，log writer 不再唤醒 log flusher（见下文 `notify_about_advanced_write_lsn()`），所以这里必须先自己确保 `log.write_lsn >= end_lsn`，否则 log flusher 会去刷一段比 `end_lsn` 更早的数据然后重新睡 1 秒。

**第四步，`flush_to_disk` 为假（只写不刷）：**

```
if (log.write_lsn.load() >= end_lsn) {
  return wait_stats;
}

/* Wait until log gets written up to end_lsn. */
wait_stats += log_wait_for_write(log, end_lsn, &interrupted);
```

`log_wait_for_write()` 先 `os_event_set(log.writer_event)` 把后台 log writer 叫醒，然后在 `log.write_events[slot]` 上等待；`log_wait_for_flush()` 类似，但等的是 `log.flush_events[slot]`，并且额外用 `thd_wait_begin(nullptr, THD_WAIT_GROUP_COMMIT)` / `thd_wait_end(nullptr)` 把等待计入 Performance Schema 的 group commit 等待组。

**等待槽位的计算**由 `log_compute_wait_event_slot()` 给出：

```
return ((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) & (events_n - 1);
```

减 1 是为了让「块右边界」的 LSN 落到**前一块**的槽位上——源码注释解释：写或刷发生在块内时，所有关心该块内某个 LSN 的线程都应当被唤醒，所以宁可多唤醒一个槽位，也不要漏掉恰好等于该 LSN 的等待者。

**第五步，自旋次数的自适应。** `log_max_spins_when_waiting_in_user_thread()` 根据当前 CPU 使用率决定自旋轮数：

```
const uint32_t hwm = srv_log_spin_cpu_pct_hwm;

if (srv_cpu_usage.utime_abs < srv_log_spin_cpu_abs_lwm || cpu >= hwm) {
  /* Don't spin because either cpu usage is too high or it's
  almost idle so no reason to bother. */
  max_spins = 0;
} else if (cpu >= hwm / 2) {
  max_spins = min_non_zero_value;
} else {
  const double r = 1.0 * (hwm / 2 - cpu) / (hwm / 2);
  max_spins =
      static_cast<uint64_t>(min_non_zero_value + r * min_non_zero_value * 9);
}
```

CPU 使用率超过高水位 `srv_log_spin_cpu_pct_hwm` 就完全不转；使用率在 `hwm / 2` 与 `hwm` 之间用最小自旋轮数；低于 `hwm / 2` 时按线性比例放大到 `[minimum, 10*minimum]` 区间。`log_wait_for_flush()` 另有一条额外刹车：`log.flush_avg_time >= srv_log_wait_for_flush_spin_hwm` 时把 `max_spins` 强制置 0。

### 后台线程侧：`log_writer()`

`log_writer(log_t *log_ptr)` 是 log writer 线程主体。它创建自己的内部 THD（`create_internal_thd()`），进入 `log_writer_mutex`，然后进入主循环：

```
for (uint64_t step = 0;; ++step) {
  ...
  const auto wait_stats = waiting.wait(stop_condition);

  MONITOR_INC_WAIT_STATS(MONITOR_LOG_WRITER_, wait_stats);
  ...
  /* Do the actual work. */
  if (log.write_lsn.load() < ready_lsn) {
    log_writer_write_buffer(log, ready_lsn);

    if (step % 1024 == 0) {
      write_to_file_requests_monitor.update();

      log_writer_mutex_exit(log);

      std::this_thread::yield();

      log_writer_mutex_enter(log);
    }

  } else if (log.should_stop_threads.load() &&
             log_writer_is_allowed_to_stop(log)) {
    break;
  }
}
```

主循环的退出条件有两组：写完了（`log.write_lsn >= ready_lsn`）且被要求停机（`log.should_stop_threads`）且 `log_writer_is_allowed_to_stop()` 确认所有写入日志缓冲的操作都已完成。

等待策略由 `Log_thread_waiting` 结构体封装：先自旋（`srv_log_writer_spin_delay`），再退化为等事件；超时上限是 `std::chrono::seconds{1}`。另外当 CPU 占用低或写盘请求不频繁（`log_write_to_file_requests_are_frequent(req_interval)` 为假）时，自旋被关掉、超时压到 1 毫秒。

### 写一次：`log_writer_write_buffer()`

`log_writer_write_buffer(log_t &log, lsn_t next_write_lsn)` 把「要写多少」的决策和「怎么写」的决策分开。它的步骤是：

**第一步，处理日志缓冲的环形回绕。** 起点 `start_offset = last_write_lsn % log.buf_size`，终点 `end_offset = next_write_lsn % log.buf_size`；若 `start_offset >= end_offset`，说明本次写入跨过了日志缓冲的末尾，就把本次写入截断到缓冲末尾：

```
if (start_offset >= end_offset) {
  ut_a(next_write_lsn - last_write_lsn >= log.buf_size - start_offset);

  end_offset = log.buf_size;
  next_write_lsn = last_write_lsn + (end_offset - start_offset);
}
```

**第二步，等检查点让出空间。** `log_writer_wait_on_checkpoint(log, last_write_lsn, next_write_lsn)` 返回 `checkpoint_limited_lsn`，本次写入的 LSN 上限被压到它：

```
if (limit_for_next_write_lsn < next_write_lsn) {
  end_offset -= next_write_lsn - limit_for_next_write_lsn;
  next_write_lsn = limit_for_next_write_lsn;
  ...
}
```

`log_writer_wait_on_checkpoint()` 先试乐观路径 `log_writer_wait_on_checkpoint_optimistic()`：

```
const lsn_t hard_limited_lsn =
    ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE) +
    log.m_capacity.hard_logical_capacity();
```

乐观路径不通过时走 `log_writer_wait_on_checkpoint_pessimistic()`，它循环等待，循环体里做三件事：置 `log.checkpointer_event` 叫醒检查点线程；判断「哪怕只写一个块也写不下」的持续时间，`Log_clock::now() - missing_space_started >= std::chrono::seconds(5)` 时打错误日志 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE`；调用 `log_request_checkpoint(log, false)` 请求一个**非同步**检查点并等 100 微秒。源码注释说明了为什么不能要同步检查点：如果被闩住的最老脏页正好被一个正等待日志缓冲空间的用户线程持有，要求同步刷它会死锁。

**第三步，等归档器。** `arch_log_sys != nullptr` 时调 `log_writer_wait_on_archiver(log, next_write_lsn)`。该函数内部的重试间隔是 `SLEEP_BETWEEN_RETRIES_IN_US` = 100 微秒，警告间隔 `TIME_BETWEEN_WARNINGS_IN_US` = 100 毫秒，报错间隔 `TIME_UNTIL_ERROR_IN_US` = 1 秒。

**第四步，检测并发写并让出。** 三个等待函数都会临时释放 `log.writer_mutex`，所以要重新确认没有别人抢先写过：

```
if (last_write_lsn != log.write_lsn.load()) {
  return;
}
```

注释明确说明：在 `--innodb_log_writer_threads=OFF` 时另一个线程已经抢先写过（源码注释原文：another thread could have already performed a write to the redo log），此时绝不能重复写。

**第五步，真正落盘。**

```
const dberr_t err = log_write_buffer(
    log, buf_begin, buf_end - buf_begin,
    ut_uint64_align_down(last_write_lsn, OS_FILE_LOG_BLOCK_SIZE));

if (UNIV_UNLIKELY(err != DB_SUCCESS)) {
  ut_a(log.write_lsn.load() == last_write_lsn);
  log_writer_write_failed(log, err);
}
```

`buf_begin` 是起点向下对齐到块边界后的地址，`buf_end` 是终点地址。写失败交给 `log_writer_write_failed()` 处理，它只对 `DB_OUT_OF_DISK_SPACE` 特殊处理（打警告 `ER_IB_MSG_LOG_WRITER_WAIT_ON_NEW_LOG_FILE` 并调 `log_files_wait_for_next_file_available(log)` 等待新文件就绪），其余错误一律 `ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_LOG_WRITER_WRITE_FAILED, ...)`。

### 写盘的落点：`log_write_buffer()`

`log_write_buffer(log_t &log, byte *buffer, size_t buffer_size, lsn_t start_lsn)` 是真正把字节写进 redo 文件的函数，执行顺序：

1. `ut_ad(log_writer_mutex_own(log))` 断言持有写者互斥量。
2. `validate_buffer()` 与 `validate_start_lsn()` 做边界检查：缓冲区必须在 `log.buf` 之内，`start_lsn` 必须按 `OS_FILE_LOG_BLOCK_SIZE` 对齐，且写入末端必须落在块头之后、块尾之前。
3. `const auto real_offset = log.m_current_file.offset(start_lsn);` 算出文件内的物理偏移。
4. `compute_how_much_to_write()` 算出「写多少、从哪写」（前面已详述）。
5. `write_size == 0` 时 `return start_next_file(log, start_lsn);` 切新文件。
6. `prepare_full_blocks(log, buffer, write_size, start_lsn)` 给将要写出的完整块补块头：

```
const lsn_t block_lsn = start_lsn + buffer_offset;

Log_data_block_header block_header;
block_header.set_lsn(block_lsn);
block_header.m_data_len = OS_FILE_LOG_BLOCK_SIZE;
block_header.m_first_rec_group = log_block_get_first_rec_group(ptr);
log_data_block_header_serialize(block_header, ptr);
```

7. 分叉：完整块从日志缓冲直接写；不完整块先 `copy_to_write_ahead_buffer()`，必要时再 `prepare_for_write_ahead()` 计算出要额外预写多少字节。
8. `srv_stats.os_log_pending_writes.inc()` 计数后调 `write_blocks()`：

```
const dberr_t err = log_data_blocks_write(log.m_current_file_handle,
                                          real_offset, write_size, write_buf);

if (err != DB_SUCCESS) {
  return err;
}

meb::redo_log_archive_produce(write_buf, write_size);
```

`write_blocks()` 成功后会通知归档子系统 `meb::redo_log_archive_produce()`——这对应官方文档里的 redo log archiving 特性。

9. 推进 LSN 并通知等待者：

```
const lsn_t old_write_lsn = log.write_lsn.load();
const lsn_t new_write_lsn = start_lsn + lsn_advance;
ut_a(new_write_lsn > log.write_lsn.load());

log.write_lsn.store(new_write_lsn);

notify_about_advanced_write_lsn(log, old_write_lsn, new_write_lsn);
```

10. 更新日志缓冲的可用水位与统计：

```
log_update_buf_limit(log, new_write_lsn);

srv_stats.os_log_pending_writes.dec();
srv_stats.log_writes.inc();

ut_a(write_size >= written_ahead);
srv_stats.os_log_written.add(write_size - written_ahead);
MONITOR_INC_VALUE(MONITOR_LOG_PADDED, written_ahead);
```

11. 结算剩余空间并更新 `log.write_ahead_end_offset`：

```
int64_t free_space = log.m_capacity.soft_logical_capacity();
free_space -= new_write_lsn - log.last_checkpoint_lsn.load();

MONITOR_SET(MONITOR_LOG_FREE_SPACE, free_space);

log.n_log_ios++;

update_current_write_ahead(log, real_offset, write_size);
```

### 通知机制：槽位事件 + notifier 线程

`notify_about_advanced_write_lsn()` 是等待者被唤醒的地方：

```
if (!log.writer_threads_paused.load(std::memory_order_acquire)) {
  if (srv_flush_log_at_trx_commit == 1) {
    os_event_set(log.flusher_event);
  }

  const auto first_slot =
      log_compute_write_event_slot(log, old_write_lsn + 1);

  const auto last_slot =
      log_compute_write_event_slot(log, new_write_lsn);

  if (first_slot == last_slot) {
    log_sync_point("log_write_before_users_notify");
    os_event_set(log.write_events[first_slot]);
  } else {
    log_sync_point("log_write_before_notifier_notify");
    os_event_set(log.write_notifier_event);
  }
}

if (arch_log_sys && arch_log_sys->is_active()) {
  os_event_set(log_archiver_thread_event);
}
```

要点有三个：

- `srv_flush_log_at_trx_commit == 1` 时才唤醒 log flusher——这就是前文 `log_write_up_to()` 里「`!= 1` 时必须自己先等 write_lsn」的原因。
- 起点槽位和终点槽位**相同**时，直接置用户线程等的那个事件；**不同时**只置 `log.write_notifier_event`，由 `log_write_notifier()` 线程去逐槽位唤醒。**架构推论：** 这是「一个事件唤醒一批等待者」的分级设计，目的是避免 log writer 每写一次就遍历整个等待事件数组。
- 归档器活跃时，额外置 `log_archiver_thread_event`。

写满一个块的通知同理，`log_flush_low()` 里推进 `log.flushed_to_disk_lsn` 后：

```
if (!log.writer_threads_paused.load(std::memory_order_acquire)) {
  const auto first_slot =
      log_compute_flush_event_slot(log, last_flush_lsn + 1);

  const auto last_slot =
      log_compute_flush_event_slot(log, flush_up_to_lsn);

  if (first_slot == last_slot) {
    log_sync_point("log_flush_before_users_notify");
    os_event_set(log.flush_events[first_slot]);
  } else {
    log_sync_point("log_flush_before_notifier_notify");
    os_event_set(log.flush_notifier_event);
  }
} else {
  log_sync_point("log_flush_before_users_notify");
  log_sync_point("log_flush_before_notifier_notify");
  os_event_set(log.old_flush_event);
}
```

### 刷盘：`log_flush_low()`

`log_flush_low(log_t &log)` 由 log flusher 线程 `log_flusher()` 调用，做一次同步 fsync：

```
bool do_flush = srv_unix_file_flush_method != SRV_UNIX_O_DSYNC;

...

const lsn_t flush_up_to_lsn = log.write_lsn.load();

if (flush_up_to_lsn == last_flush_lsn) {
  os_event_set(log.old_flush_event);
  return;
}

log.last_flush_start_time = Log_clock::now();

ut_a(flush_up_to_lsn > last_flush_lsn);

if (do_flush) {
  log_sync_point("log_flush_before_fsync");
  log.m_current_file_handle.fsync();
}

log.last_flush_end_time = Log_clock::now();
...
log.flushed_to_disk_lsn.store(flush_up_to_lsn);
```

Unix 上当 `srv_unix_file_flush_method == SRV_UNIX_O_DSYNC` 时**不额外调 fsync**，因为 O_DSYNC 打开方式本身已经保证了同步语义；Windows 上 `do_flush` 恒为真。

`log_flusher()` 线程主体里，`srv_flush_log_at_trx_commit != 1` 时会额外按 `get_srv_flush_log_at_timeout()` 的间隔限速，避免刷盘过于频繁。线程退出前有：

```
if (log.write_lsn.load() > log.flushed_to_disk_lsn.load()) {
  log_flush_low(log);
}

ut_a(log.write_lsn.load() == log.flushed_to_disk_lsn.load());
```

即停机时必须把已写的 redo 全部刷完。

### 停机路径：`log_writer_is_allowed_to_stop()`

```
log_files_dummy_records_disable(log);

/* When log threads are stopped, we must first
ensure that all writes to log buffer have been
finished and only then we are allowed to set
the should_stop_threads to true. */

log_advance_ready_for_write_lsn(log);

return log.write_lsn.load() == log_buffer_ready_for_write_lsn(log);
```

**先关掉 dummy record，再确认「已写出的 LSN」等于「缓冲里已就绪的 LSN」**，两者相等才允许停机——即所有写进日志缓冲的内容都已落到文件里。

## 关键取舍

### 写与刷分离，用 `innodb_flush_log_at_trx_commit` 调节

把 redo「写进文件」和「fsync 落盘」拆成两个独立阶段，中间由 `srv_flush_log_at_trx_commit` 这一个开关控制联动方式：

| `srv_flush_log_at_trx_commit` | 行为 | 源码依据 |
|---|---|---|
| `1` | 每次提交都要求 redo 落盘，且 log writer 主动唤醒 log flusher | `notify_about_advanced_write_lsn()` 里 `if (srv_flush_log_at_trx_commit == 1) os_event_set(log.flusher_event);` |
| `2`（及 `0`） | 不要求每次提交落盘；log writer **不**唤醒 log flusher，需要落盘的用户线程自己先等 write_lsn 再等 flush | `log_write_up_to()` 里 `if (srv_flush_log_at_trx_commit != 1) { ... log_wait_for_write(...); }`，随后无条件 `log_wait_for_flush(...)` |

**取舍的本质：durability 与吞吐的直接交换。** `== 1` 时每笔提交都要过一次 fsync，代价是每次提交的延迟里含一次持久化往返；`!= 1` 时 fsync 由后台线程按 `get_srv_flush_log_at_timeout()` 的节奏批量做，提交延迟与 fsync 解耦，代价是崩溃时会丢掉最近若干笔已提交事务。

### 优先写完整块，代价是要等

`compute_how_much_to_write()` 里 `write_from_log_buffer = write_size >= OS_FILE_LOG_BLOCK_SIZE` 决定「是否值得直接从日志缓冲写」。当从日志缓冲写时还要 `ut_uint64_align_down(write_size, OS_FILE_LOG_BLOCK_SIZE)` 把长度对齐到块边界——**宁可少写一点、把不完整的尾块留到下一轮**，也不写半块。

**取舍：每次系统调用的粒度更规整（整块写），但把用户线程的「日志已就绪」推迟了。** 源码注释在文件头解释了原因：写完整块可以让 log writer 尽快把控制权还给用户线程，让用户线程去处理下一个事务；但为了不浪费已就绪的数据，末尾那个不完整块仍然会被特殊处理——先复制到 write-ahead 缓冲，补齐块头、填 `0x00`、算好校验和再写出去。

### 用 write-ahead 缓冲换取顺序写

不完整块不能直接从日志缓冲写（块头还没定型、后续还会被追加数据），所以走 `log.write_ahead_buf` 复制一份出来补齐。**取舍：多一次内存 memcpy，换来设备侧不用读-改-写。** `copy_to_write_ahead_buffer()` 里 `ut_a(size <= srv_log_write_ahead_size)` 限定了单次复制量不能超过 write-ahead 缓冲大小。

### 空间回收靠检查点，写入端让路

redo 空间被写满时，写者不能自己解决，只能等检查点推进。`log_writer_wait_on_checkpoint_pessimistic()` 的处理是「请求非同步检查点 + 每次等 100 微秒」，并且明确注释说明**不能请求同步检查点以避免死锁**。

**取舍：写入端在空间紧张时吞吐下降，但不与刷脏端互相锁死。** 相应的错误提示是 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE`（持续 5 秒连一个块都回收不了时打印）。源码注释给出的死锁场景是：闩住最老脏页的用户线程正在等日志缓冲空间，此时若要求同步刷该页就会互锁。

### 等待策略按负载自适应

`log_max_spins_when_waiting_in_user_thread()` 与 `Log_thread_waiting::wait()` 都做了「空闲时别烧 CPU」的适配：

- CPU 使用率超过 `srv_log_spin_cpu_pct_hwm` → 完全不自旋。
- CPU 使用率低于 `hwm / 2` → 自旋轮数在 `[minimum, 10*minimum]` 间线性放大。
- 写盘请求不频繁（`log_write_to_file_requests_are_frequent(req_interval)` 为假，即平均 1 毫秒一次或更少）→ 自旋关掉、超时压到 1 毫秒。

**取舍：低负载时响应略慢，但省电；高负载时快速自旋，代价是 CPU。**

### 槽位事件 vs notifier 线程

用户线程等待用的是「按 LSN 分槽的事件数组」`log.write_events[]` / `log.flush_events[]`，槽位索引由 `((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) & (events_n - 1)` 计算。log writer 发现本次推进跨越了多个槽位时，**不逐槽位唤醒**，只置 `log.write_notifier_event`，由 `log_write_notifier()` 线程去广播。

**取舍：极端情况下唤醒有额外延迟（多一次线程调度），但常态下 log writer 的推进路径是 O(1) 的。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/log/log0write.cc | `log_write_up_to` | 用户线程入口：请求把 redo 写到 `end_lsn`，`flush_to_disk` 决定是否还要落盘；内含前置断言、后台线程暂停时的自写路径、`log_wait_for_write` / `log_wait_for_flush` 的等待编排与 `interrupted` 重试循环 |
| storage/innobase/log/log0write.cc | `log_write_buffer` | 真正把日志缓冲的一段写入当前 redo 文件：校验、算写多少、准备块头、走 write-ahead 分叉、推进 `log.write_lsn`、通知等待者、结算剩余空间 |
| storage/innobase/log/log0write.cc | `log_writer` | log writer 线程主体：推进就绪 LSN、循环调用 `log_writer_write_buffer`、按 `step % 1024` 让出 CPU、停机时与 `log_writer_is_allowed_to_stop` 协同收尾 |
| storage/innobase/log/log0write.cc | `log_writer_write_buffer` | 单次写入的编排：处理日志缓冲环形回绕、等检查点让空间、等归档器、检测并发写并让出，最后调 `log_write_buffer` |
| storage/innobase/log/log0write.cc | `log_writer_wait_on_checkpoint` | 按 `hard_logical_capacity` 限制写入 LSN 上限；乐观路径不够时走悲观路径等检查点推进 |
| storage/innobase/log/log0write.cc | `log_writer_wait_on_checkpoint_pessimistic` | 空间不足时的等待循环：叫醒检查点线程、5 秒无进展时报 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE`、请求非同步检查点并等 100 微秒 |
| storage/innobase/log/log0write.cc | `log_writer_extra_margin_check` | 比较软容量上限与目标 LSN，决定进入 / 退出 extra margin 应急态 |
| storage/innobase/log/log0write.cc | `log_writer_write_failed` | 写失败处理：`DB_OUT_OF_DISK_SPACE` 走等待新文件，其余错误 `ib::fatal` 终止 |
| storage/innobase/log/log0write.cc | `log_writer_is_allowed_to_stop` | 停机条件判定：关闭 dummy record，并要求 `log.write_lsn` 等于缓冲就绪 LSN |
| storage/innobase/log/log0write.cc | `start_next_file` | 当前文件写满时切新文件：调 `log_files_produce_file`，并把 `log.write_ahead_end_offset` 归零 |
| storage/innobase/log/log0write.cc | `compute_how_much_to_write` | 决定本次写多少字节、从日志缓冲写还是从 write-ahead 缓冲写；统计完整块 / 部分块写入次数 |
| storage/innobase/log/log0write.cc | `compute_next_write_ahead_end` | 按 `srv_log_write_ahead_size` 对齐算出下一个 write-ahead 区域末端 |
| storage/innobase/log/log0write.cc | `prepare_full_blocks` | 为将要写出的完整块补块头：LSN、数据长度、首个记录组偏移 |
| storage/innobase/log/log0write.cc | `copy_to_write_ahead_buffer` | 把不完整块复制到 write-ahead 缓冲，补块头、填 `0x00`、写校验和 |
| storage/innobase/log/log0write.cc | `write_blocks` | 调 `log_data_blocks_write` 落盘，成功后调 `meb::redo_log_archive_produce` 交给归档子系统 |
| storage/innobase/log/log0write.cc | `notify_about_advanced_write_lsn` | 推进 `log.write_lsn` 后唤醒等待者：单槽位置用户事件，跨槽位置 notifier 事件，必要时唤醒归档器 |
| storage/innobase/log/log0write.cc | `log_compute_wait_event_slot` | 按 `((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) & (events_n - 1)` 算出等待事件槽位 |
| storage/innobase/log/log0write.cc | `log_wait_for_write` | 等 `log.write_lsn >= lsn`：先叫醒 log writer，再在 `log.write_events[slot]` 上等待 |
| storage/innobase/log/log0write.cc | `log_wait_for_flush` | 等 `log.flushed_to_disk_lsn >= lsn`：在 `log.flush_events[slot]` 上等待，并计入 group commit 等待组 |
| storage/innobase/log/log0write.cc | `log_self_write_up_to` | 后台线程被暂停时的自写路径：自旋等就绪 LSN，再按 100 毫秒间隔等事件 |
| storage/innobase/log/log0write.cc | `log_max_spins_when_waiting_in_user_thread` | 按 CPU 使用率与 `srv_log_spin_cpu_pct_hwm` 决定自旋轮数 |
| storage/innobase/log/log0write.cc | `log_flush_low` | 执行一次 fsync 并推进 `log.flushed_to_disk_lsn`、按槽位通知 flush 等待者；Unix 上 O_DSYNC 模式跳过额外 fsync |
| storage/innobase/log/log0write.cc | `log_flusher` | log flusher 线程主体：按 `get_srv_flush_log_at_timeout()` 限速，停机前把已写 redo 全部刷完 |
| storage/innobase/log/log0write.cc | `log_write_notifier` | write notifier 线程：跨槽位推进时逐槽位唤醒等待 `log.write_lsn` 的线程 |

## 与主核 / AI 的关系

主核（`engine/db/`）有独立的 WAL 实现，与本页描述的 InnoDB redo 没有代码级关联，但**机制形态高度同构**，可作对照：

- **两者都是「先写日志、再改数据」的 WAL 纪律**：主核 `engine/db/wal/` 层负责在数据页落盘前先把变更写入 WAL，本页描述的 InnoDB redo 同理——redo 记录的是「要改什么」，页的写回是后续动作。
- **两者都把「写」和「刷」分成两级**：主核 WAL 的写入与刷盘也是分开推进的，本页的 `log.write_lsn` 与 `log.flushed_to_disk_lsn` 是一对独立推进的水位线。
- **两者的空间回收都靠检查点**：主核 WAL 同样有 checkpoint 概念来回收已被数据页吸收的日志空间，对应本组 I02。

**架构推论：** 主核 WAL 可以借鉴本页的两点设计——(1) 把「等待 redo 落盘」的槽位事件数组与 notifier 线程分开，避免推进路径上遍历整个等待数组；(2) 让自旋轮数随 CPU 使用率自适应，低负载时把 CPU 让给别的工作负载。这两点都是**从 InnoDB 的实现读出来的做法**，主核侧是否存在等价实现需另行核对主核源码，本页不作断言。

## 参考

- [Redo Log（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— redo 日志的官方定义、LSN 语义、redo 文件布局与 `innodb_redo_log_capacity` 的配置说明。
- [InnoDB Checkpoints（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-checkpoints.html) —— fuzzy checkpointing 与检查点标签的官方说明，是本页「写入端等检查点让空间」这一机制的上位背景。
- [Optimizing InnoDB Redo Logging（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/optimizing-innodb-logging.html) —— `innodb_flush_log_at_trx_commit` 取值与 redo 写入/刷盘频率取舍的官方说明。
- [storage/innobase/log/log0write.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0write.cc) —— 本页全部主符号的权威来源。
- [storage/innobase/log/log0log.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0log.cc) —— `log_t` 结构定义、`srv_log_write_ahead_size` 的赋值点 `log_write_ahead_resize()`、以及 log writer / write notifier 线程的创建处 `log_start_background_threads()`。
