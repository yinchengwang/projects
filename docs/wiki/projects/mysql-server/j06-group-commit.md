---
project: mysql-server
feature: group-commit
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/log/log0write.cc
    symbols: [log_flush_low, log_flusher]
---

# 组提交

## 一句话职责

`storage/innobase/log/log0write.cc` 里的组提交机制负责把**多个事务的 redo 落盘请求合并成一次 fsync**：后台 log flusher 线程 `log_flusher()` 在一次 `log_flush_low()` 调用里把 `log.write_lsn` 之前的所有 redo 一次性 fsync 落盘，随后把 `log.flushed_to_disk_lsn` 一次性推进到该位置，所有等在这个 LSN 之前的事务**同时被唤醒**，于是 N 个并发提交只付出一份 fsync 的延迟。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责把多个事务的提交合并成一次日志落盘与 fsync，以摊薄提交延迟；**不负责单事务的提交路径（J01）**、**不负责 binlog 的组提交（L 组）**。

## 机制是什么

### 合并的载体是「单调推进的两个 LSN」

redo 日志在 `log0write.cc` 里由两个 LSN 划界（页 I01 展开讲）：

| LSN | 含义 | 谁推进 |
|---|---|---|
| `log.write_lsn` | redo 已经写进 redo 日志文件（进入 OS 缓存）的位置 | log writer 线程，在 `log_write_buffer()` 里 `log.write_lsn.store(new_write_lsn)` |
| `log.flushed_to_disk_lsn` | redo 已经 fsync 落盘的位置 | log flusher 线程，在 `log_flush_low()` 里 `log.flushed_to_disk_lsn.store(flush_up_to_lsn)` |

**组提交的本质就是「一次 fsync 覆盖一段 LSN 区间」。** `log_flush_low()` 里这一行是全部的合并点：

```
const lsn_t flush_up_to_lsn = log.write_lsn.load();
...
log.flushed_to_disk_lsn.store(flush_up_to_lsn);
```

`flush_up_to_lsn` 取的是 fsync **开始那一刻**的 `log.write_lsn`。在 fsync 执行期间新产生的 redo 不在本次覆盖范围内，要等下一次 fsync。因此一次 fsync 覆盖的区间 = 「上次 fsync 结束后的 write_lsn」到「本次 fsync 开始时的 write_lsn」，这段区间里所有事务的提交请求共享同一次 fsync 的成本。

### 唤醒机制：按 512 字节日志块分槽的事件数组

等待方不共用一个条件变量，而是各自等在**一个环形事件数组**的某个槽位上。槽位由 `log_compute_wait_event_slot()` 计算：

```
return ((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) & (events_n - 1);
```

`OS_FILE_LOG_BLOCK_SIZE` 是日志块大小，源码在文件头 `PAGE_INNODB_REDO_LOG_FORMAT` 一节写明为 **512 字节**；`events_n` 是事件数组大小。减去 1 的理由，函数注释写得很直接：把日志块的**右边界**归给代表该块的槽位，这样「写或刷发生在块内时，所有关心该块内任意 lsn 的线程都应被唤醒」。

事件数组的大小本身不是魔数——`log_compute_write_event_slot()` 与 `log_compute_flush_event_slot()` 都把 `log.write_events_size` / `log.flush_events_size` 作为 `events_n` 传入，位与运算要求它是 2 的幂。

### 单槽直唤醒 vs 跨槽交给 notifier

`log_flush_low()` 推进 `log.flushed_to_disk_lsn` 之后，按**旧值和新值各算一个槽位**：

```
const auto first_slot = log_compute_flush_event_slot(log, last_flush_lsn + 1);
const auto last_slot = log_compute_flush_event_slot(log, flush_up_to_lsn);

if (first_slot == last_slot) {
  os_event_set(log.flush_events[first_slot]);
} else {
  os_event_set(log.flush_notifier_event);
}
```

- **同槽**：直接 `os_event_set()` 唤醒这一槽上等着的所有事务。文件头 `sect_redo_log_flusher` 一节把这个优化写为「若上次 flush 之后只有单个日志块被刷过，log flusher 线程直接通知用户线程，而不通知 log flush_notifier 线程」，并说明该优化「对某些场景是正向的、对另一些是负向的，需要进一步调查；但因为从逻辑上看是合理的，所以被保留」。
- **跨槽**：置 `log.flush_notifier_event`，由 `log_flush_notifier()` 线程逐槽唤醒。它按 `OS_FILE_LOG_BLOCK_SIZE` 步长遍历：

  ```
  const lsn_t notified_up_to_lsn = ut_uint64_align_up(flush_lsn, OS_FILE_LOG_BLOCK_SIZE);
  while (lsn <= notified_up_to_lsn) {
    const auto slot = log_compute_flush_event_slot(log, lsn);
    lsn += OS_FILE_LOG_BLOCK_SIZE;
    os_event_set(log.flush_events[slot]);
  }
  ```

  **注意这个循环的粒度是 512 字节一块**，而不是一个 LSN 一个 LSN 地通知。

### 等待方：`log_wait_for_flush()` 与 group commit 等待组

用户线程等待刷盘的函数是 `log_wait_for_flush()`，其等待调用前后包着一对 `thd_wait_begin()` / `thd_wait_end()`：

```
thd_wait_begin(nullptr, THD_WAIT_GROUP_COMMIT);
const auto wait_stats =
    os_event_wait_for(log.flush_events[slot], max_spins,
                      get_srv_log_wait_for_flush_timeout(), stop_condition);
thd_wait_end(nullptr);
```

`THD_WAIT_GROUP_COMMIT` 是本次等待的归类标记——等待 redo 刷盘的会话在 Performance Schema 的等待事件里被归入 group commit 这一类。

等待的自旋轮数由 `log_max_spins_when_waiting_in_user_thread(srv_log_wait_for_flush_spin_delay)` 决定；这个函数按当前 CPU 使用率与高水位 `srv_log_spin_cpu_pct_hwm` 的关系分三档返回轮数（详见「关键取舍」）。另外 `log_wait_for_flush()` 里还有一条独立的降级判断：

```
if (log.flush_avg_time >= srv_log_wait_for_flush_spin_hwm) {
  max_spins = 0;
}
```

`log.flush_avg_time` 由 `log_flush_update_stats()` 维护——它按 `srv_flushing_avg_loops` 轮次或时间窗统计 fsync 平均耗时。

### 谁来发起「刷到某个 LSN」的请求

组提交的**发起方**在 `log_write_up_to()` 里，它按 `flush_to_disk` 分两路：

```
if (flush_to_disk) {
  if (log.flushed_to_disk_lsn.load() >= end_lsn) {
    return wait_stats;          // 已被前一次 fsync 覆盖，直接返回
  }

  if (srv_flush_log_at_trx_commit != 1) {
    if (log.write_lsn.load() < end_lsn) {
      wait_stats += log_wait_for_write(log, end_lsn, &interrupted);
    }
  }

  wait_stats += log_wait_for_flush(log, end_lsn, &interrupted);
  ...
} else {
  ...
}
```

这就是组提交在代码里的直接体现：**一个事务的提交请求如果发现 `log.flushed_to_disk_lsn >= end_lsn`，说明它已经被别的线程的某次 fsync 顺带刷下去了，自己不必再付一次 fsync 成本**。

### 从事务提交到组提交的接线

上游调用方在 `storage/innobase/trx/trx0trx.cc`。提交路径上的 `trx_flush_log_if_needed_low()` 按 `srv_flush_log_at_trx_commit` 分派：

```
switch (srv_flush_log_at_trx_commit) {
  case 2:
    flush = false;              // 只写不刷
    [[fallthrough]];
  case 1:
    wait_stats = log_write_up_to(*log_sys, lsn, flush);
    return;
  case 0:
    return;                     // 什么都不做
}
```

`trx_flush_log_if_needed()` 在 DDL 场景（`trx->ddl_operation || trx->ddl_must_flush`）走强制刷盘分支 `log_write_up_to(*log_sys, lsn, true)`。`trx_flush_log_if_needed()` 的调用方有 `trx_commit_complete_for_mysql()`（延迟刷盘的事务在提交完成后补刷）与提交路径本身。页 J01 展开单事务提交路径。

**架构推论：** 组提交发生在 InnoDB 这一层，即 redo 的 fsync 层面；binlog 的组提交是 server 层另一套机制（两阶段提交协调器），与本页的 `log_flusher` / `log_flush_low` 不是同一条路径。本页不覆盖 binlog 侧。

### 落盘本身：`log_flush_low()` 的完整流程

```
static void log_flush_low(log_t &log) {
  ut_ad(log_flusher_mutex_own(log));

#ifndef _WIN32
  bool do_flush = srv_unix_file_flush_method != SRV_UNIX_O_DSYNC;
#else
  bool do_flush = true;
#endif

  if (!log.writer_threads_paused.load(std::memory_order_acquire)) {
    os_event_reset(log.flusher_event);
  }

  const lsn_t last_flush_lsn = log.flushed_to_disk_lsn.load();
  const lsn_t flush_up_to_lsn = log.write_lsn.load();

  if (flush_up_to_lsn == last_flush_lsn) {
    os_event_set(log.old_flush_event);
    return;
  }

  log.last_flush_start_time = Log_clock::now();
  ut_a(flush_up_to_lsn > last_flush_lsn);

  if (do_flush) {
    log.m_current_file_handle.fsync();
  }

  log.last_flush_end_time = Log_clock::now();
  ...
  log.flushed_to_disk_lsn.store(flush_up_to_lsn);
  ...
}
```

要点：

1. **进入即断言持锁**：`ut_ad(log_flusher_mutex_own(log))` —— `log_flush_low()` 必须在 log flusher 互斥量保护下调用。
2. **`do_flush` 的取值**：Unix 上当 `srv_unix_file_flush_method == SRV_UNIX_O_DSYNC` 时不再额外调 fsync，因为 O_DSYNC 模式下写入本身已带同步语义；Windows 上恒为 `true`。
3. **无事可做时也要通知**：`flush_up_to_lsn == last_flush_lsn` 时不 fsync，但仍然 `os_event_set(log.old_flush_event)`——这个事件是 `log_self_write_up_to()` 那条自写路径等待的对象。
4. **时间戳用于统计**：`log.last_flush_start_time` / `log.last_flush_end_time` 是 `log_flush_update_stats()` 计算 fsync 平均耗时的输入。

## 怎么实现的

### log flusher 线程的循环

`log_flusher()` 是后台线程主体，用 `Log_thread_waiting` 等待结构：

```
Log_thread_waiting waiting{log, log.flusher_event, srv_log_flusher_spin_delay,
                           get_srv_log_flusher_timeout()};
```

其 `stop_condition` 判据是**「有东西没刷」**：

```
const lsn_t last_flush_lsn = log.flushed_to_disk_lsn.load();
ut_a(last_flush_lsn <= log.write_lsn.load());

if (last_flush_lsn < log.write_lsn.load()) {
  log_flush_low(log);
  if (step % 1024 == 0) {
    log_flusher_mutex_exit(log);
    std::this_thread::yield();
    log_flusher_mutex_enter(log);
  }
  return true;
}
```

`step % 1024 == 0` 时让出 CPU 一次，避免长时间霸占持锁状态。

### 唤醒 log flusher 的条件

log writer 每推进一次 `log.write_lsn`，由 `notify_about_advanced_write_lsn()` 决定是否叫醒 flusher：

```
if (!log.writer_threads_paused.load(std::memory_order_acquire)) {
  if (srv_flush_log_at_trx_commit == 1) {
    os_event_set(log.flusher_event);
  }
  ...
}
```

**只有 `srv_flush_log_at_trx_commit == 1` 时才主动叫醒 log flusher。** 源码注释说明了原因（`log_write_up_to()` 内）：当 `srv_flush_log_at_trx_commit != 1` 时，log writer 与 log flusher 之间的通知被关闭，log flusher 可能会睡满 1 秒，此时需要快速响应的用户线程必须自己叫醒 log flusher——但**叫醒之前必须先保证 `log.write_lsn >= lsn`**，否则 log flusher 刷的是更小 LSN 的数据，刷完又回去睡。

### `srv_flush_log_at_trx_commit != 1` 时的限速

`log_flusher()` 的循环体里有一段专门的限速逻辑：

```
if (srv_flush_log_at_trx_commit != 1) {
  const auto time_elapsed = ... current_time - log.last_flush_start_time;
  const auto flush_every = get_srv_flush_log_at_timeout();

  if (time_elapsed < flush_every) {
    log_flusher_mutex_exit(log);
    if (!log.should_stop_threads.load()) {
      os_event_wait_time_low(log.flusher_event, flush_every - time_elapsed, 0);
    }
    log_flusher_mutex_enter(log);
  }
}
```

限速窗口由 `get_srv_flush_log_at_timeout()` 给出；停机时不遵守这个限速（`log.should_stop_threads.load()` 为真时跳过等待）。

### 停机收尾：把没刷的刷完

线程退出前：

```
if (log.write_lsn.load() > log.flushed_to_disk_lsn.load()) {
  log_flush_low(log);
}
ut_a(log.write_lsn.load() == log.flushed_to_disk_lsn.load());
```

最后那条 `ut_a` 断言要求退出时写位点与刷盘位点严格相等。

### 降级路径：后台线程被暂停时用户线程自己刷

`log_write_up_to()` 的第一个分支处理「log writer 线程被暂停」的情况：

```
retry:
  if (log.writer_threads_paused.load(std::memory_order_acquire)) {
    wait_stats += log_self_write_up_to(log, end_lsn, flush_to_disk, &interrupted);
    if (UNIV_UNLIKELY(interrupted)) {
      goto retry;
    }
    DEBUG_SYNC_C("log_flushed_by_self");
    return wait_stats;
  }
```

`log_self_write_up_to()` 在这条路径上**自己调 `log_flush_low()`**：

```
if (flush_to_disk) {
  if (UNIV_UNLIKELY(log_flusher_mutex_enter_nowait(log))) {
    ...
    log_flusher_mutex_enter(log);
  }
  log_flush_low(log);
  log_flusher_mutex_exit(log);
  ...
}
```

这条路径上刷盘前有两道关卡：

- **第一道：有无 pending flush。** 若 `os_event_is_set(log.old_flush_event)` 为假，说明已有人发起过一次刷盘且尚未完成，此时释放 `log.writer_mutex`、在 `log.old_flush_event` 上等（`os_event_wait_low`），醒来后重新确认 `log.flushed_to_disk_lsn`；若仍小于 `end_lsn` 则置 `interrupted` 返回交由上层重试。
- **第二道：log flusher 互斥量。** 用 `log_flusher_mutex_enter_nowait(log)` 试抢。该调用返回真表示**没抢到**：此时若后台写线程已被恢复（`log.writer_threads_paused` 为假），则置 `log.old_flush_event`、置 `interrupted` 并返回；只有在线程仍处于暂停状态时才退化为阻塞的 `log_flusher_mutex_enter(log)`。抢到锁（或阻塞等到锁）后才调 `log_flush_low(log)`。

这条路径上有明确的 `DEBUG_SYNC_C("log_flushed_by_self")` 同步点（在 `log_write_up_to()` 的暂停分支返回前设置），可用于测试区分「被后台线程刷的」与「自己刷的」。

**降级路径点名：** 「回退到 `log_self_write_up_to()`」的调用方是 `log_write_up_to()`；`log_self_write_up_to()` 内部真正的刷盘执行者是 `log_flush_low()`。

### `interrupted` 重试环

`log_wait_for_flush()` 的 `stop_condition` 里有一条：

```
if (UNIV_UNLIKELY(log.writer_threads_paused.load(std::memory_order_relaxed))) {
  *interrupted = true;
  return true;
}
```

即：等待期间若后台写线程被暂停，本次等待作废，`log_write_up_to()` 跳回 `retry:` 标签重新走一遍。`log_write_up_to()` 里三条等待路径（`log_self_write_up_to` / `log_wait_for_write` / `log_wait_for_flush`）返回的 `interrupted` 都汇入这个重试环。

## 关键取舍

### 一次 fsync 覆盖一个 LSN 区间，而不是逐事务 fsync

**取舍：把「持久化一次」与「提交一个事务」解耦，换来 N 个并发提交共享一份 fsync 延迟；代价是单个事务的持久化点不能精确落在自己的提交 LSN 上，而是落在「本次 fsync 覆盖区间的末端」。** 这在源码里是显式成立的：`log_flush_low()` 的 `flush_up_to_lsn = log.write_lsn.load()` 一次性把 `log.flushed_to_disk_lsn` 推到这个位置，而唤醒是按槽位批量做的。

### 通知按 512 字节日志块粒度，而不是按事务粒度

**取舍：唤醒粒度粗到日志块，换来事件数组规模可控、通知次数少；代价是同一块内比目标 LSN 更小的等待者会被一并唤醒（虚假唤醒），必须由等待者自己重新校验。** 源码注释明确接受这一点：log flush notifier 线程「也允许在极可能满足条件时通知（lsn 值在同一个日志块内）。允许犯错，由被通知的用户线程负责确保 `flushed_to_disk_lsn` 被充分推进」。因此 `log_wait_for_flush()` 的 `stop_condition` 里第一件事就是检查 `log.flushed_to_disk_lsn.load() >= lsn`。

### 事件内部带互斥量，宁可虚假通知也不漏通知

**取舍：用「可能多唤醒」换「绝不漏唤醒」。** 文件头 `sect_redo_log_flush_notifier` 一节写明：「event 内部使用互斥量，以避免漏掉通知（漏通知比虚假通知更糟）」。同时存在一个最大等待超时（源码注释写的是默认 1 毫秒），超时后由用户线程重新检查 `_flushed_to_disk_lsn_`。

### 单块直唤醒 vs 交给 notifier 线程

**取舍：同槽时省掉一次线程切换，跨槽时把逐槽通知交给专门的 notifier 线程。** 源码注释承认这个优化「对某些场景正向、对另一些负向，仍需进一步调查，但因逻辑上看合理而保留」。

### 自旋轮数按 CPU 使用率与 fsync 平均耗时自适应

`log_max_spins_when_waiting_in_user_thread()` 按当前 CPU 使用率 `srv_cpu_usage.utime_pct` 与高水位 `srv_log_spin_cpu_pct_hwm` 的关系分三档：

- CPU 使用率绝对值低于 `srv_log_spin_cpu_abs_lwm`，或使用率已达高水位 → 自旋轮数取 **0**（不烧 CPU）；
- 使用率在高水位的一半以上 → 取 `min_non_zero_value`（最小允许轮数）；
- 使用率低于高水位的一半 → 在 `[min_non_zero_value, 10 * min_non_zero_value]` 区间内按线性插值取值。

另外 `log_wait_for_flush()` 还有一条独立降级：`log.flush_avg_time >= srv_log_wait_for_flush_spin_hwm` 时把自旋轮数直接置 0。

**取舍：磁盘慢时让等待方去睡觉而不是空转，换来 CPU 不被 fsync 等待占满；代价是慢盘上的提交延迟上界变差。** 源码注释在 `log_max_spins_when_waiting_in_user_thread()` 里写明「CPU 使用率过高时不自旋」。

### 后台线程自旋也要看请求是否频繁

`Log_thread_waiting::wait()` 里有一条独立的判断：若 CPU 使用率绝对值低于 `srv_log_spin_cpu_abs_lwm`，或者 `log_write_to_file_requests_are_frequent(req_interval)` 为假（即请求平均间隔在 1 毫秒量级或更稀疏），就把 `spin_delay` 置 0，并把最小超时压到 `min(req_interval, 1ms)`。源码注释列出两个理由：服务器 CPU 使用率极低（几乎空闲），或落盘请求稀疏——两种情况下都不值得在后台线程里空转。

请求间隔由 `Log_write_to_file_requests_monitor::update()` 维护：取增量时间除以增量请求数得到瞬时间隔，再按 `(m_request_interval * 63 + request_interval) / 64` 做指数平滑；若距上次有进展已超过 100 毫秒，则把间隔视为 **100 毫秒** 上限（源码注释说明「跟踪更高的值没有意义，因为它肯定已经高于任何合理的阈值」）。

### `srv_flush_log_at_trx_commit != 1` 时要自己叫醒 flusher

**取舍：牺牲一点常态下的通知开销，换来非常规配置下的响应速度。** `notify_about_advanced_write_lsn()` 只在 `srv_flush_log_at_trx_commit == 1` 时置 `log.flusher_event`；其余情况由用户线程在 `log_write_up_to()` 里自己置，并且必须先确保 `log.write_lsn >= end_lsn`（源码注释解释了为什么：否则 flusher 刷的是更小的 LSN，刷完又回去睡 1 秒）。

### O_DSYNC 模式下不额外 fsync

**取舍：`srv_unix_file_flush_method == SRV_UNIX_O_DSYNC` 时省掉一次 fsync 调用，换来设备层已保证同步；代价是依赖文件打开方式的语义，Windows 上没有对应路径，因此 `do_flush` 恒为 `true`。**

### 停机时必须把 redo 刷完

**取舍：停机路径上多一次 fsync，换来退出时写位点与刷盘位点严格相等（由 `ut_a` 断言保证）。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/log/log0write.cc | `log_flush_low` | 组提交的落盘执行点：断言持有 log flusher 互斥量；`do_flush` 在 Unix 上取 `srv_unix_file_flush_method != SRV_UNIX_O_DSYNC`、Windows 上恒为 true；取 `flush_up_to_lsn = log.write_lsn.load()` 作为本次 fsync 的覆盖上界；`flush_up_to_lsn == last_flush_lsn` 时不 fsync 但置 `log.old_flush_event` 并返回；否则调 `log.m_current_file_handle.fsync()`，然后 `log.flushed_to_disk_lsn.store(flush_up_to_lsn)`；通知时按旧值 `last_flush_lsn + 1` 与新值各算一个 flush 事件槽位，同槽直接置 `log.flush_events[first_slot]`，跨槽置 `log.flush_notifier_event`，后台线程被暂停时置 `log.old_flush_event`；末尾调 `log_flush_update_stats()` |
| storage/innobase/log/log0write.cc | `log_flusher` | log flusher 线程主体：以 `Log_thread_waiting waiting{log, log.flusher_event, srv_log_flusher_spin_delay, get_srv_log_flusher_timeout()}` 等待；`stop_condition` 在 `last_flush_lsn < log.write_lsn.load()` 时调 `log_flush_low(log)` 并返回 true（每次 `step % 1024 == 0` 时释放互斥量并 `std::this_thread::yield()`）；`srv_flush_log_at_trx_commit != 1` 时按 `get_srv_flush_log_at_timeout()` 限速；停机退出前若 `write_lsn > flushed_to_disk_lsn` 再调一次 `log_flush_low(log)`，随后断言两者相等 |
| storage/innobase/log/log0write.cc | `log_flush_update_stats` | fsync 耗时统计：维护 `fsync_max_time` / `fsync_total_time`，当迭代次数达到 `srv_flushing_avg_loops` 或时间窗达到该值时更新 `MONITOR_LOG_FLUSH_LSN_AVG_RATE`、`MONITOR_LOG_FLUSH_MAX_TIME`、`MONITOR_LOG_FLUSH_AVG_TIME`，并把 `log.flush_avg_time` 设为累计耗时除以迭代次数 |
| storage/innobase/log/log0write.cc | `log_wait_for_flush` | 等待 `log.flushed_to_disk_lsn >= lsn` 的用户线程等待点：先置 `log.flusher_event`，必要时置 `log.writer_event`；自旋轮数由 `log_max_spins_when_waiting_in_user_thread(srv_log_wait_for_flush_spin_delay)` 给出，`log.flush_avg_time >= srv_log_wait_for_flush_spin_hwm` 时置 0；在 `log.flush_events[slot]` 上以 `get_srv_log_wait_for_flush_timeout()` 为超时等待；等待前后包 `thd_wait_begin(nullptr, THD_WAIT_GROUP_COMMIT)` 与 `thd_wait_end(nullptr)`；等待期间后台写线程被暂停则置 `interrupted` 并退出等待 |
| storage/innobase/log/log0write.cc | `log_write_up_to` | 组提交的请求入口：`flush_to_disk` 为真时若 `log.flushed_to_disk_lsn.load() >= end_lsn` 直接返回（已被前一次 fsync 覆盖）；`srv_flush_log_at_trx_commit != 1` 且 `log.write_lsn < end_lsn` 时先 `log_wait_for_write()`；再 `log_wait_for_flush()`；`interrupted` 时跳回 `retry:` 重来；后台写线程被暂停时走 `log_self_write_up_to()` 分支 |
| storage/innobase/log/log0write.cc | `log_self_write_up_to` | 后台写线程被暂停时的自写路径：先按 `srv_n_spin_wait_rounds` 自旋等就绪 LSN，再按 100 毫秒间隔等 `log.closer_event`；刷盘前先看 `log.old_flush_event` 是否已置（表示有 pending flush），是则等该事件并在醒来后重新确认 `log.flushed_to_disk_lsn`，不足则置 `interrupted`；随后用 `log_flusher_mutex_enter_nowait(log)` 试抢 log flusher 互斥量，返回真表示未抢到，此时若后台写线程已恢复则置 `log.old_flush_event` 并置 `interrupted` 返回，仅在仍暂停时才退化为阻塞的 `log_flusher_mutex_enter(log)`；最后调 `log_flush_low(log)` 并释放互斥量 |
| storage/innobase/log/log0write.cc | `log_compute_wait_event_slot` | 计算等待事件槽位：`((lsn - 1) / OS_FILE_LOG_BLOCK_SIZE) & (events_n - 1)`；减 1 是为了把日志块右边界归给代表该块的槽位，使块内任意 lsn 的等待者都能被唤醒 |
| storage/innobase/log/log0write.cc | `log_compute_flush_event_slot` | 以 `log.flush_events_size` 为数组大小调用 `log_compute_wait_event_slot()`，得到刷盘等待的槽位 |
| storage/innobase/log/log0write.cc | `log_flush_notifier` | 跨槽通知线程：取 `lsn = log.flushed_to_disk_lsn.load() + 1` 起，按 `ut_uint64_align_up(flush_lsn, OS_FILE_LOG_BLOCK_SIZE)` 算出通知上界，以 `OS_FILE_LOG_BLOCK_SIZE` 为步长逐槽置 `log.flush_events[slot]`；`step % 1024 == 0` 时让出 CPU |
| storage/innobase/log/log0write.cc | `notify_about_advanced_write_lsn` | log writer 推进 `log.write_lsn` 后的通知：仅当 `srv_flush_log_at_trx_commit == 1` 时置 `log.flusher_event`（这是 log flusher 被叫醒的常规路径）；写入侧按旧值与新值算槽位，同槽置 `log.write_events[first_slot]`，跨槽置 `log.write_notifier_event`；归档器活跃时置 `log.archiver_thread_event` |
| storage/innobase/log/log0write.cc | `log_max_spins_when_waiting_in_user_thread` | 按 CPU 使用率 `srv_cpu_usage.utime_pct` 与高水位 `srv_log_spin_cpu_pct_hwm` 决定用户线程等待时的自旋轮数：使用率低于 `srv_log_spin_cpu_abs_lwm` 或已达高水位取 0；达到高水位一半取 `min_non_zero_value`；低于一半则在 `[min_non_zero_value, 10 * min_non_zero_value]` 区间线性插值 |
| storage/innobase/log/log0write.cc | `Log_thread_waiting` | 后台日志线程的等待结构：构造时把最小超时压到不超过 1 秒；`wait()` 中若 CPU 使用率绝对值低于 `srv_log_spin_cpu_abs_lwm` 或 `log_write_to_file_requests_are_frequent(req_interval)` 为假，则 `spin_delay` 置 0、最小超时压到 `min(req_interval, 1ms)` |
| storage/innobase/log/log0write.cc | `Log_write_to_file_requests_monitor` | 维护落盘请求的平均间隔：增量时间除以增量请求数得到瞬时间隔，按 `(m_request_interval * 63 + request_interval) / 64` 指数平滑；距上次有进展超过 100 毫秒时把间隔视为 100 毫秒上限；结果写入 `log.write_to_file_requests_interval` 并置 `MONITOR_LOG_WRITE_TO_FILE_REQUESTS_INTERVAL` |
| storage/innobase/trx/trx0trx.cc | `trx_flush_log_if_needed_low` | 事务提交路径上按 `srv_flush_log_at_trx_commit` 分派：值为 2 时置 `flush = false` 并 fallthrough 到值为 1 的分支，只写不刷；值为 1 时调 `log_write_up_to(*log_sys, lsn, flush)`；值为 0 时直接返回不做任何事。Unix 上 `flush` 初值为 `srv_unix_file_flush_method != SRV_UNIX_NOSYNC`，Windows 上恒为 true |
| storage/innobase/trx/trx0trx.cc | `trx_flush_log_if_needed` | `trx_flush_log_if_needed_low()` 的包装：`trx->ddl_operation || trx->ddl_must_flush` 为真时强制走 `log_write_up_to(*log_sys, lsn, true)`，否则走 `trx_flush_log_if_needed_low(lsn)`；等待期间把 `trx->op_info` 置为 `"flushing log"` |
| storage/innobase/trx/trx0trx.cc | `trx_commit_complete_for_mysql` | 延迟刷盘事务的补刷入口：`trx->must_flush_log_later` 为真且未请求忽略持久性时调 `trx_flush_log_if_needed(trx->commit_lsn, trx)`，随后清掉 `trx->must_flush_log_later` 与 `trx->ddl_must_flush` |

## 与主核 / AI 的关系

主核（`engine/db/`）有一套独立的 WAL 实现，与本页的 redo 组提交在**问题形态上同构**（多个事务的持久化请求共享一次落盘），但机制形态不同——InnoDB 侧是「后台 log flusher 线程 + 按 512 字节日志块分槽的事件数组 + 两个单调 LSN」，主核侧的实现需另行核对主核 WAL 源码，本页不作断言。

从本页能直接迁移到主核的设计要点有三条：

1. **「落盘覆盖上界」与「等待方目标 LSN」分离**：InnoDB 的 `log_flush_low()` 取 `flush_up_to_lsn = log.write_lsn.load()`，一次 fsync 覆盖一整段 LSN 区间，所有等在该区间内的等待者共享同一次落盘成本。这与「逐事务 fsync」是两种完全不同的提交延迟曲线。
2. **等待方归类为独立的等待组**：InnoDB 用 `thd_wait_begin(nullptr, THD_WAIT_GROUP_COMMIT)` 把这类等待显式打标，使 Performance Schema 能把「等 redo 刷盘」与「等行锁」「等 IO」区分开。主核若要做等待可观测性，这一层标记是必要条件。
3. **虚假唤醒优于漏唤醒**：InnoDB 的事件内部带互斥量，notifier 线程允许在「极可能满足条件」时提前通知，并把最终校验责任交给等待者（`log_wait_for_flush()` 的 `stop_condition` 首行即检查 `log.flushed_to_disk_lsn.load() >= lsn`）。主核侧若要在多个等待者间共享一个落盘完成条件，应采用同一取舍。

**架构推论：** 主核 WAL 若要实现等价的组提交，关键不在「合并 fsync 调用」本身——这一点只要把「要刷到哪个 LSN」与「实际刷到了哪个 LSN」两个概念分开就已成立——而在于**等待者唤醒的粒度与批量方式**。InnoDB 选择按 512 字节日志块分槽，代价是虚假唤醒、收益是事件数组规模可控。主核的 WAL 记录长度分布与 InnoDB 不同，是否沿用块粒度需要看主核 WAL 的记录格式，本页不作断言。

**AI 相关性（架构推论）：** 本页的机制对 AI 训练与推理负载的意义体现在「并发提交数」与「落盘延迟」的乘积关系上——同一批并发提交共享一次 fsync，提交吞吐随并发数上升而 fsync 次数不同比例上升。AI 场景下的批量导入通常是大量小事务并发提交，正落在这个机制受益的区间；而长事务加低并发则享受不到摊薄效果。这一推论基于本页描述的 LSN 覆盖语义与 `srv_flush_log_at_trx_commit` 的分派逻辑，未经主核或 AI 场景的实测验证。

## 参考

- [storage/innobase/log/log0write.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0write.cc) —— 本页全部 InnoDB 侧主符号的权威来源：`log_flush_low()` 与 `log_flusher()` 的完整实现；文件头 `PAGE_INNODB_REDO_LOG_THREADS` 的 `sect_redo_log_flusher`（log flusher 线程职责与单块直唤醒优化）、`sect_redo_log_flush_notifier`（通知的槽位映射、event 内部互斥量与 1 毫秒最大等待超时）、`sect_redo_log_waiting_for_flusher`（等待条件 `flushed_to_disk_lsn >= end_lsn` 与槽位计算）；以及 `PAGE_INNODB_REDO_LOG_FORMAT` 一节中 `OS_FILE_LOG_BLOCK_SIZE` 为 512 字节的说明。
- [storage/innobase/trx/trx0trx.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/trx/trx0trx.cc) —— 事务提交侧如何接入组提交：`trx_flush_log_if_needed_low()` 按 `srv_flush_log_at_trx_commit` 的三值分派、`trx_flush_log_if_needed()` 的 DDL 强制刷盘分支，以及 `trx_commit_complete_for_mysql()` 的延迟补刷入口。
- [Redo Log（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— redo 日志的官方定义与 LSN 语义；「redo log 数据在数据修改发生时追加进 redo log 文件，最旧的数据随着检查点推进被截断」；以及 `Innodb_redo_log_flushed_to_disk_lsn` 状态变量所对应的正是本页的 `log.flushed_to_disk_lsn`。
- [InnoDB Startup Options and System Variables（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-parameters.html#sysvar_innodb_flush_log_at_trx_commit) —— `innodb_flush_log_at_trx_commit` 的取值语义（0 / 1 / 2 三档）与持久性含义，与本页 `trx_flush_log_if_needed_low()` 的 `switch` 分派一一对应；同页还列出 `innodb_flush_log_at_timeout`、`innodb_flushing_avg_loops`、`innodb_log_spin_cpu_pct_hwm`、`innodb_log_spin_cpu_abs_lwm`、`innodb_log_wait_for_flush_spin_hwm` 等本页引用的可调参数。
- [Optimizing InnoDB Redo Logging（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/optimizing-innodb-logging.html) —— 官方对 redo 日志写入吞吐与持久性之间取舍的说明，是本页「关键取舍」小节的上位背景。
