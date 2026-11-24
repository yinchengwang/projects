---
project: mysql-server
feature: log-checkpoint
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/log/log0log.cc
    symbols: [log_checkpoint]
  - path: storage/innobase/log/log0write.cc
    symbols: [log_write_up_to]
---

# 检查点

## 一句话职责

检查点机制负责把「脏页已刷盘」这一事实固化成 redo 日志文件头部的检查点块，从而划出一条 redo 记录不再需要被重做的边界（`log.last_checkpoint_lsn`），让 redo 日志空间可以被循环复用；推动者是后台 log checkpointer 线程 `log_checkpointer`，读取检查点并决定崩溃恢复起点的是 `recv_recovery_from_checkpoint_start`。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责推进检查点 LSN、决定哪些脏页可以不再要求 redo 重做；**不负责 redo 的写入（I01）**、**不负责脏页刷新（H02）**。

## 机制是什么

### 四个 LSN 与两条不变量

`storage/innobase/log/log0log.cc` 文件头的 `PAGE_INNODB_REDO_LOG` 一节（`sect_redo_log_general`）把 redo 日志的四个水位线讲清楚了：

| LSN | 含义 | 推进者 |
|---|---|---|
| `log.write_lsn` | redo 已经写进 redo 日志文件（进入 OS 缓存）到的位置 | log writer 线程 |
| `log.flushed_to_disk_lsn` | redo 已经 fsync 落盘到的位置 | log flusher 线程 |
| `log.buf_dirty_pages_added_up_to_lsn` | 到这个 LSN 为止，用户线程已经把全部脏页加进了 flush 列表 | 用户线程 |
| `log.available_for_checkpoint_lsn` | 到这个 LSN 为止，全部脏页都已刷盘；**这是本次检查点可以取的上限** | log checkpointer 线程 |
| `log.last_checkpoint_lsn` | 已写进 redo 文件头部的检查点位置，恢复从这里开始 | log checkpointer 线程 |

同节的 `subsect_redo_log_last_checkpoint_lsn` 给出两条硬性不变量：

```
log.last_checkpoint_lsn
<= log.available_for_checkpoint_lsn
<= log.buf_dirty_pages_added_up_to_lsn.
```

以及 `subsect_redo_log_buf_dirty_pages_added_up_to_lsn` 一节给出的第三条：

```
log.buf_dirty_pages_added_up_to_lsn <= log.buf_ready_for_write_lsn.
```

**为什么 `buf_dirty_pages_added_up_to_lsn` 是硬约束**：源码注释说明，用户线程加脏页进 flush 列表的顺序是「放松」的——用户线程预留 LSN 区间的顺序、往日志缓冲写数据的顺序、往 flush 列表加脏页的顺序，三者可以完全不同。因为读 flush 列表最小 `oldest_modification` 所需要的互斥量，正是用户线程写 redo 之前要获取的那个，没有额外互斥量保护，所以对任何大于 `buf_dirty_pages_added_up_to_lsn` 的 LSN，不能信任 flush 列表是完整的。

### 检查点要回答的问题只有一个

「哪些 redo 记录可以不再被重做？」答案是一个 LSN：`log.last_checkpoint_lsn`。`log0log.cc` 的 `subsect_redo_log_last_checkpoint_lsn` 一节写明——**小于这个 LSN 的数据字节不再需要，可能被覆盖（redo 文件是循环的），可以认为它们已被逻辑删除**。这正是 redo 空间能被回收的原因。

### 检查点块：每个 redo 文件头部两块交替写

`storage/innobase/log/log0write.cc` 文件头的 `PAGE_INNODB_REDO_LOG_FORMAT` 一节（`subsect_redo_log_format_header`）给出磁盘布局：

- 每个 redo 文件头部是 `LOG_FILE_HDR_SIZE` 字节，里面含两个检查点块 `LOG_CHECKPOINT_1` 和 `LOG_CHECKPOINT_2`，**每块各占 `OS_FILE_LOG_BLOCK_SIZE` = 512 字节**。
- 检查点块里含 `checkpoint_lsn`——恢复的起点。
- **两块交替更新**：源码注释给出的理由与 doublewrite buffer 相同——「若在更新过程中崩溃，另一块仍然有效」。

`storage/innobase/log/log0recv.cc` 的 `Log_checkpoint_header_no` 枚举只有 `HEADER_1` 与 `HEADER_2` 两个值，`log_next_checkpoint_header()` 负责在两者之间切换，`log.next_checkpoint_header_no` 记录下一次该写哪一块。

**当前格式按 LSN 值比较检查点新旧，不按检查点编号。** `log0recv.cc` 的注释写明：「after 8.0.30, checkpoints are compared by their LSN values because we no longer store checkpoint numbers」，旧格式分支（`ut_a(log.m_format < Log_format::CURRENT)`）才回到按 `m_checkpoint_no` 比较，为的是保持与 8.0.30 之前版本的完全兼容。

### 检查点 LSN 可以指向一条记录的中间

同一节 `subsect_redo_log_format_header` 明确：8.0 之前 `checkpoint_lsn` 直接指向第一条记录组的开头；**8.0 之后这个值可能指向某条日志记录的中间**。恢复时的处理是「跳过包含 checkpoint_lsn 的那个记录组，从下一组开始」。这带来的直接后果是：带这种检查点的日志文件**默认无法被旧版本 InnoDB 恢复**。

配套地，`log0recv.cc` 在扫描日志块时若发现 `parse_start_lsn < checkpoint_lsn`，会用 `recv_sys->bytes_to_ignore_before_checkpoint = recv_sys->checkpoint_lsn - recv_sys->parse_start_lsn` 记录需要跳过的字节数，并断言它不超过 `OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE`（512 − 12 = 500 字节）。

### 检查点推进与脏页刷盘是两件事

`storage/innobase/log/log0write.cc` 文件头的 `sect_redo_log_checkpointer` 一节说明 log checkpointer 线程的三项职责：

1. 检查是否需要写检查点（为了在检查点年龄变得过大之前把它降下来）。
2. 检查是否应该强制 page cleaner 线程同步刷脏页（因为 redo 空间不足或最老页太旧）。
3. 写检查点——**源码注释强调这是唯一被允许写检查点的线程**。

该节给出检查点线程更新 `log.available_for_checkpoint_lsn` 的计算公式：

```
min(log.buf_dirty_pages_added_up_to_lsn, max(0, oldest_lsn - L))
```

其中：

- `oldest_lsn` = **每个 flush 列表中最先加入的那个页的最老修改时间**的最小值（`buf_pool_get_oldest_modification_approx()` 的返回值）；
- `L` = **log recent closed 缓冲的槽位数**（对应 `srv_log_recent_closed_size`）。

减去 `L` 的原因，同节与 `log0log.cc` 的 `subsect_redo_log_available_for_checkpoint_lsn` 一节都有说明：加入 flush 列表的顺序是放松的，`buf_pool_get_oldest_modification_approx()` 返回的是「最早插入的某个页」的修改时间，不保证是最老的那个；但「flush 列表第一条」与「真正最老修改」之间的差距，**上限就是 log recent closed 缓冲的槽位数**。没有这个余量，检查点就可能越过某个尚未加入 flush 列表的脏页的 `oldest_modification`。

特殊情况：flush 列表里没有脏页时，该值就等于 `log.buf_dirty_pages_added_up_to_lsn`。

同节还记录了这个设计的动机：「Note that previously, all user threads were trying to calculate this lsn concurrently, causing contention on flush_list mutex... Now the lsn is updated in single thread.」——**把 LSN 计算从「所有用户线程并发算」收敛到「单一后台线程算」，是为了消除 flush 列表互斥量的争用**。

## 怎么实现的

### 检查点线程的创建与停止

`storage/innobase/log/log0log.cc` 的 `log_start_background_threads()` 里，log checkpointer 是六个后台 redo 线程之一：

```
srv_threads.m_log_checkpointer =
    os_thread_create(log_checkpointer_thread_key, 0, log_checkpointer, &log);
```

同一函数还创建 `log_flush_notifier`、`log_flusher`、`log_write_notifier`、`log_writer`、`log_files_governor` 六个线程，随后逐个 `.start()`。启动前先 `log_background_threads_inactive_validate()` 断言这些线程都还没起来。

停止路径在 `log_stop_background_threads()`：

```
while (log_checkpointer_is_active()) {
  os_event_set(log.checkpointer_event);
  std::this_thread::sleep_for(std::chrono::microseconds(10));
}
```

**注意这个函数开头有一条专门的断言**，解释了检查点线程与 `log.sn` X 锁之间的死锁风险：

```
/* We cannot stop threads when x-lock is acquired, because of scenario:
        * log_checkpointer starts log_checkpoint()
        * log_checkpoint() asks to persist dd dynamic metadata
        * dict_persist_to_dd_table_buffer() tries to write to redo
        * but cannot lock on log.sn
        * so log_checkpointer thread waits for this thread
          until the x-lock is released
        * but this thread waits until log background threads
          have been stopped - log_checkpointer is not stopped. */
ut_ad(!rw_lock_own(log.sn_lock_inst, RW_LOCK_X));
```

这段注释是源码直接陈述的事实：**`log_checkpoint()` 会请求持久化 DD 动态元数据**，而持久化动作要写 redo、要拿 `log.sn` 的 X 锁——检查点线程因此可能被持有 `log.sn` X 锁的线程反向阻塞。这条断言就是这条互锁链条的护栏。

### 检查点互斥量

`log_sys_create()` 里创建 `mutex_create(LATCH_ID_LOG_CHECKPOINTER, &log.checkpointer_mutex);`，与它并排的还有 `LATCH_ID_LOG_CLOSER`、`LATCH_ID_LOG_WRITER`、`LATCH_ID_LOG_FLUSHER`、`LATCH_ID_LOG_WRITE_NOTIFIER`、`LATCH_ID_LOG_FLUSH_NOTIFIER`、`LATCH_ID_LOG_LIMITS`、`LATCH_ID_LOG_FILES`、`LATCH_ID_LOG_SN_MUTEX`。

`log_checkpointer_mutex` 的用途在 `log_position_lock()` / `log_position_unlock()` 里体现：

```
log_position_lock(log_t &log) {
  log_buffer_x_lock_enter(log);
  log_checkpointer_mutex_enter(log);
}
```

`log_position_collect_lsn_info()` 在同时持有 `log.sn_lock_inst` 的 X 锁与 `log_checkpointer_mutex` 时，读出 `log.last_checkpoint_lsn.load()` 与当前 LSN，并断言 `*current_lsn >= *checkpoint_lsn`（当前 LSN 不可能落在检查点之前）。

**架构推论：** 这套「`log.sn` X 锁 + 检查点互斥量」的两级加锁，是为了让「复制位点读取」与「检查点推进」互斥——复制位点要拿到一个自洽的 (checkpoint_lsn, current_lsn) 组合。源码并未直接陈述这一意图，此处是从加锁配对与断言推出的。

### 检查点的两个出口：`log_start()` 与停机路径

**启动路径。** `log0log.cc` 的 `log_start(log_t &log, lsn_t checkpoint_lsn, lsn_t start_lsn, bool allow_checkpoints)` 在启动 redo 日志时同时设定两个水位线：

```
log.recovered_lsn = start_lsn;
log.last_checkpoint_lsn = checkpoint_lsn;
log.available_for_checkpoint_lsn = checkpoint_lsn;
log.m_allow_checkpoints.store(allow_checkpoints);
```

即**启动时 `available_for_checkpoint_lsn` 直接等于 `last_checkpoint_lsn`**——刚恢复完时还没有新脏页被刷盘，可检查点上限就是刚读到的那个检查点。

`allow_checkpoints` 参数控制的是「恢复期间是否允许写检查点」。`log0recv.cc` 的 `recv_recovery_from_checkpoint_start()` 调用时传的是 `false`：

```
/* Disallow checkpoints until recovery is finished, and changes gathered
in recv_sys->metadata_recover (dict_metadata) are transferred to
dict_table_t objects (happens in srv0start.cc). */

err = log_start(log, checkpoint_lsn, recovered_lsn, false);
```

**停机路径。** `log0log.cc` 的 `log_make_empty_and_stop_background_threads()` 反复写检查点直到日志逻辑为空：

```
while (log_make_latest_checkpoint(log)) {
  /* It could happen, that when writing a new checkpoint,
  DD dynamic metadata was persisted, making some pages
  dirty (with the persisted data) and writing new redo
  records to protect those modifications. In such case,
  current lsn would be higher than lsn and we would need
  another iteration to ensure, that checkpoint lsn points
  to the newest lsn. */
}
```

**这里的循环条件不是「检查点是否落后」，而是「写完这次检查点后，当前 LSN 是否又变高了」**——因为写检查点本身会持久化 DD 动态元数据，把页弄脏并产生新的 redo 记录，于是需要再写一次。源码注释把这个自反馈环写得很明确。

### 恢复侧：如何找到并使用最新检查点

`storage/innobase/log/log0recv.cc` 提供了完整的读取路径。

**第一步：找每个文件里最新的检查点。** `recv_find_max_checkpoint(log_t &, Log_file_handle &file_handle, Log_checkpoint_location &checkpoint)` 遍历 `HEADER_1` 与 `HEADER_2` 两块：

```
const lsn_t checkpoint_lsn = checkpoint_header.m_checkpoint_lsn;
if (checkpoint_lsn == 0) {
  continue;
}

if (!found || checkpoint_lsn > checkpoint.m_checkpoint_lsn) {
  ut_a(checkpoint_lsn >= LOG_START_LSN);
  found = true;
  checkpoint.m_checkpoint_file_id = file_handle.file_id();
  checkpoint.m_checkpoint_header_no = checkpoint_header_no;
  checkpoint.m_checkpoint_lsn = checkpoint_lsn;
}
```

`checkpoint_lsn == 0` 的块被跳过（还没写过）。`ut_a(checkpoint_lsn >= LOG_START_LSN)` 断言检查点 LSN 不会低于日志起点。

**第二步：跨文件取全局最新。** `recv_find_max_checkpoint(log_t &log, Log_checkpoint_location &checkpoint)` 用 `log_files_for_each(log.m_files, ...)` 遍历所有 redo 文件，并做一次关键校验：

```
if (!file.contains(checkpoint_in_file.m_checkpoint_lsn)) {
  ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_OUTSIDE_LOG_FILE, ...);
  return;
}
```

**检查点 LSN 必须落在它所在那个 redo 文件的 LSN 区间内**，否则报错 `ER_IB_MSG_RECOVERY_CHECKPOINT_OUTSIDE_LOG_FILE` 并跳过该文件。

**第三步：写入内存中的检查点并比对 flush 位点。** `recv_recovery_from_checkpoint_start(log_t &log, lsn_t flush_lsn)` 是恢复的总入口，它的顺序是：

```
recv_recovery_on = true;

ut_a(log.m_format == Log_format::CURRENT);

/* Look for the latest checkpoint */
Log_checkpoint_location checkpoint;
if (!recv_find_max_checkpoint(log, checkpoint)) {
  ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_NOT_FOUND);
  return DB_ERROR;
}

const auto checkpoint_file = log.m_files.find(checkpoint.m_checkpoint_lsn);
if (checkpoint_file == log.m_files.end()) {
  ut_d(ut_error);
  ut_o(return DB_ERROR);
}

log.last_checkpoint_lsn.store(checkpoint.m_checkpoint_lsn);
```

找不到任何检查点时报 `ER_IB_MSG_RECOVERY_CHECKPOINT_NOT_FOUND` 并返回 `DB_ERROR`；找到了则把它存进 `log.last_checkpoint_lsn`，同时打日志 `ER_IB_MSG_LOG_CHECKPOINT_FOUND` 报出检查点 LSN 与所在文件路径。

随后它重新打开该文件、把检查点块读回来并断言两者一致：

```
dberr_t err = log_checkpoint_header_read(checkpoint_file_handle,
                                         checkpoint.m_checkpoint_header_no,
                                         checkpoint_header);
...
const lsn_t checkpoint_lsn = checkpoint.m_checkpoint_lsn;
ut_a(checkpoint_lsn == checkpoint_header.m_checkpoint_lsn);
```

**第四步：判断是否需要崩溃恢复。** 比较检查点 LSN 与系统表空间里记录的 flush 位点：

```
if (checkpoint_lsn != flush_lsn) {
  if (checkpoint_lsn < flush_lsn) {
    ib::warn(ER_IB_MSG_RECOVERY_CHECKPOINT_FROM_BEFORE_CLEAN_SHUTDOWN,
             ulonglong{checkpoint_lsn}, ulonglong{flush_lsn});
  }

  if (!recv_needed_recovery) {
    ib::info(ER_IB_MSG_RECOVERY_IS_NEEDED, ulonglong{flush_lsn},
             ulonglong{checkpoint_lsn});

    if (srv_read_only_mode) {
      ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY);
      return DB_ERROR;
    }

    recv_init_crash_recovery();
  }
}
```

检查点 LSN 小于 flush 位点时告警 `ER_IB_MSG_RECOVERY_CHECKPOINT_FROM_BEFORE_CLEAN_SHUTDOWN`——意思是「上次是干净关闭，但检查点落后于数据」，需要补做恢复。

**第五步：把检查点写到另一块头部。** 这是本机制里最容易被忽略、但对崩溃安全最关键的一步：

```
/* Make the preservation of max checkpoint info on disk certain by writing
the checkpoint also to the other checkpoint header. After that both headers
will have the same checkpoint_lsn. This is an extra protection in case next
checkpoint write will become corrupted because of crash during the write. */

if (!srv_read_only_mode) {
  log.next_checkpoint_header_no =
      log_next_checkpoint_header(checkpoint.m_checkpoint_header_no);

  err = log_files_next_checkpoint(log, checkpoint_lsn);
  if (err != DB_SUCCESS) {
    return err;
  }
}
```

**恢复启动时，InnoDB 会把找到的检查点同时写进两块头部**，使两个块持有相同的 `checkpoint_lsn`。这样即使下一次写检查点时崩溃、破坏了正在写的那一块，另一块仍然保有最大检查点信息。

### 写入侧：检查点如何反过来约束 redo 写入

检查点不是单向的。`storage/innobase/log/log0write.cc` 里有一整套「写入端等检查点让空间」的机制。

**乐观路径。** `log_writer_wait_on_checkpoint_optimistic()` 计算写入上限：

```
const lsn_t checkpoint_lsn = log.last_checkpoint_lsn.load();

const lsn_t hard_limited_lsn =
    ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE) +
    log.m_capacity.hard_logical_capacity();

ut_a(last_write_lsn <= hard_limited_lsn);
ut_a(checkpoint_lsn < next_write_lsn);
```

**redo 的硬容量是以检查点为原点计算的**：`hard_logical_capacity()` 挂在对齐到块边界的检查点 LSN 之后。检查点不动，写入上限就不动。

**软上限与 extra margin。** `log_writer_extra_margin_check()` 用软容量：

```
const lsn_t soft_limited_lsn =
    ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE) +
    log.m_capacity.soft_logical_capacity();

if (next_write_lsn <= soft_limited_lsn) {
  if (log.m_writer_inside_extra_margin) {
    log_writer_exit_extra_margin(log);
  }
  return false;
} else {
  if (!log.m_writer_inside_extra_margin) {
    log_writer_enter_extra_margin(log);
  }
  return true;
}
```

越过软上限即进入 extra margin 应急态。

**反向更新：** `log_writer_check_if_exited_extra_margin()` 的注释说明了为什么检查点线程需要反过来更新写入侧的应急态：

```
/* Choose an option which minimizes the possibility that the current
value of log.m_writer_inside_extra_margin changes. ... This function exists
and is exported, because the log_checkpointer thread needs to update
the log.m_writer_inside_extra_margin when it advances checkpoint_lsn
if the log_writer is idle (because has nothing more to write). */
```

**检查点推进之后，如果 log writer 恰好空闲（没什么可写），就得由检查点线程来把 extra margin 状态退掉**——否则写入侧会一直以为自己还在应急态里。

**悲观路径：** 空间实在不够时，`log_writer_wait_on_checkpoint_pessimistic()` 进入等待循环：

```
os_event_set(log.checkpointer_event);

if (last_write_lsn + OS_FILE_LOG_BLOCK_SIZE <= hard_limited_lsn) {
  /* Write what we have - adjust the speed to speed of checkpoints
  going forward (to speed of page-cleaners). */
  return hard_limited_lsn;
}

...

if (Log_clock::now() - missing_space_started >= std::chrono::seconds(5)) {
  /* We could not reclaim even single redo block for 5sec */
  ib::error(ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE, ulonglong{checkpoint_lsn});
  missing_space_started = Log_clock::now();
  log_sync_point("log_writer_ran_out_space");
}
```

**连一个 512 字节的块都回收不出来、持续 5 秒**，就报 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE` 并把计时器归零（所以这个错误是每 5 秒重复一次，不是只报一次）。

然后是请求检查点：

```
/* We don't want to ask for sync checkpoint, because it
is possible, that the oldest dirty page is latched and
user thread, which keeps the latch, is waiting for space
in log buffer (for log_writer writing to disk). In such
case it would be deadlock (we can't flush the latched
page and advance the checkpoint). We only ask for the
checkpoint, and wait for some time. */
log_request_checkpoint(log, false);

os_event_wait_time_low(log.next_checkpoint_event,
                       std::chrono::microseconds(100),
                       next_checkpoint_sig_count);
```

**请求的是非同步检查点（第二个参数 `false`），等待间隔 100 微秒。** 源码注释给出了非同步的理由：最老脏页可能被闩住，持有闩的用户线程正在等日志缓冲空间写 redo——此时若要求同步刷这个脏页并推进检查点，就构成死锁。

**不允许写检查点时直接致命退出。** 同一函数里：

```
if (!log.m_allow_checkpoints.load()) {
  if (srv_force_recovery < 4) {
    ib::fatal(UT_LOCATION_HERE,
              ER_IB_MSG_RECOVERY_NO_SPACE_IN_REDO_LOG__SKIP_IBUF_MERGES);
  } else {
    ib::fatal(UT_LOCATION_HERE,
              ER_IB_MSG_RECOVERY_NO_SPACE_IN_REDO_LOG__UNEXPECTED);
  }
}
```

`log.m_allow_checkpoints` 正是 `log_start()` 里那个参数的落点，恢复期间它为 `false`，所以恢复期空间不足是致命错误而非等待。

**检查点不是唯一的消费者。** `log_writer_wait_on_consumers()` 的注释点明了分工：

```
/* This should not be a checkpointer nor archiver, as we've used dedicated
log_writer_wait_on_checkpoint() and log_writer_wait_on_archiver() to wait
for them already */
ut_ad(name == "MEB");
```

**检查点与归档器各有各的等待函数，消费者的通用等待路径上不会再等它们。** 剩下的通用消费者断言名字必须是 `"MEB"`（MySQL Enterprise Backup）。

### 用户线程侧：`log_write_up_to()` 与检查点的关系

`storage/innobase/log/log0write.cc` 的 `log_write_up_to(log_t &log, lsn_t end_lsn, bool flush_to_disk)` 是用户线程进入 redo 子系统的入口。它本身**不直接写检查点**，但它与检查点的关系体现在两处：

**第一，恢复期间直接返回，不做任何事。**

```
if (recv_no_ibuf_operations) {
  /* Recovery is running and no operations on the log files are
  allowed yet, which is implicitly deduced from the fact, that
  still ibuf merges are disallowed. */
  return Wait_stats{0};
}
```

注释进一步说明：恢复期间应用 redo 会把页改脏，页刷盘时发现 `newest_modification != 0` 会再回到 `log_write_up_to()`，但那时 redo 实际已经被刷过了。

**第二，背景写线程被暂停时，用户线程自己写盘，并会在空间不足时撞上检查点这堵墙。** `log_write_up_to()` 检测到 `log.writer_threads_paused` 为真时走 `log_self_write_up_to()`，由用户线程持 `log.writer_mutex` 亲自调 `log_writer_write_buffer()`——而 `log_writer_write_buffer()` 内部就包含 `log_writer_wait_on_checkpoint()`。

`log_self_write_up_to()` 里也有一处与检查点直接相关的等待——等日志缓冲里「已就绪」的 LSN 追上目标：

```
while (ready_lsn < end_lsn) {
  /* wait using event */
  log_closer_mutex_enter(log);
  if (log.current_ready_waiting_lsn == 0 &&
      os_event_is_set(log.closer_event)) {
    log.current_ready_waiting_lsn = end_lsn;
    log.current_ready_waiting_sig_count = os_event_reset(log.closer_event);
  }
  const auto sig_count = log.current_ready_waiting_sig_count;
  log_closer_mutex_exit(log);
  ++waits;
  os_event_wait_time_low(log.closer_event, std::chrono::milliseconds{100},
                         sig_count);
  log.recent_written.advance_tail();
  ready_lsn = log_buffer_ready_for_write_lsn(log);
}
```

**每次等 100 毫秒**，等的是 `log.closer_event`（redo 文件关闭 / 换文件事件），不是检查点事件。

### 对外暴露的检查点位点

`log0log.cc` 的 `log_update_exported_variables()` 把检查点 LSN 导出为服务器状态变量：

```
export_vars.innodb_redo_log_checkpoint_lsn = log_get_checkpoint_lsn(log);
export_vars.innodb_redo_log_flushed_to_disk_lsn =
    log.flushed_to_disk_lsn.load();
export_vars.innodb_redo_log_current_lsn = log_get_lsn(log);
```

`log_print()` 也把检查点打在状态输出里，格式为 `Last checkpoint at`：

```
fprintf(file,
        "Log sequence number          " LSN_PF
        ...
        "Added dirty pages up to      " LSN_PF
        "\n"
        "Pages flushed up to          " LSN_PF
        "\n"
        "Last checkpoint at           " LSN_PF
        ...
```

其中 `Pages flushed up to` 打印的正是 `log.available_for_checkpoint_lsn`（取值处带 `log_limits_mutex_enter` / `log_limits_mutex_exit`），`Last checkpoint at` 打印的是 `log.last_checkpoint_lsn`。

## 关键取舍

### 检查点写两块、交替更新

**取舍：每次写检查点要多写一个 512 字节的块，换来「写检查点途中崩溃不会丢失最大检查点信息」。** 源码注释给出的类比是 doublewrite buffer——用同样的双份交替技巧挡住「更新到一半崩溃」这一类故障。

### 恢复启动时把检查点补写到两块

`log_files_next_checkpoint(log, checkpoint_lsn)` 在恢复启动时多写一次，让两个头部持有相同的 `checkpoint_lsn`。

**取舍：多一次 redo 文件头写，换来「紧接着的第一次检查点写失败也不怕」。** 源码注释称之为 "an extra protection in case next checkpoint write will become corrupted because of crash during the write"。

### 检查点 LSN 允许指向记录的中间

**取舍：redo 格式失去「检查点必落在记录组开头」这一强不变式，恢复端必须多一段「找下一条记录组起点」的逻辑，换来的是检查点可以贴着脏页刷盘的实际进度取值、不必为对齐而保守回退。** 代价是旧版本 InnoDB 无法恢复这样的日志文件，且恢复端需要 `bytes_to_ignore_before_checkpoint` 这个额外计数器。

### 可检查点 LSN 减去 recent closed 缓冲槽位数

`max(0, oldest_lsn - L)` 里的 `L` 是 log recent closed 缓冲的槽位数（`srv_log_recent_closed_size`）。

**取舍：检查点位置比理论最老脏页保守一小段，redo 空间回收得慢一点，换来「不会越过尚未加入 flush 列表的脏页的 oldest_modification」。** 源码注释把这个差距的上界明确写为 log recent closed 缓冲的槽位数——这是个有界的、可计算的量，不是拍脑袋的余量。

### 计算 LSN 从「所有用户线程并发」收敛到「单一后台线程」

**取舍：多一个后台线程承担计算工作，换来消除用户线程在 flush 列表互斥量上的争用。** `log0write.cc` 的注释直接给出了动机：以前所有用户线程都试图并发算这个 LSN，会争用 flush 列表互斥量；现在由单线程更新。

### 写入端要检查点，只能请求非同步

`log_request_checkpoint(log, false)` ——第二个参数是「是否同步」。

**取舍：日志空间紧张时写入端要等（每次 100 微秒轮询），换取彻底避开与页闩持有者的死锁。** 源码注释把这个死锁场景写得很具体：最老脏页被闩住，持有闩的用户线程在等日志缓冲空间，此时同步刷该页并推进检查点就互锁。

### 检查点与归档器各有专属等待路径

`log_writer_wait_on_checkpoint()` 与 `log_writer_wait_on_archiver()` 是两个专用函数，通用消费者等待路径 `log_writer_wait_on_consumers()` 里断言剩余消费者名必须是 `"MEB"`。

**取舍：等待逻辑分成三条专用路径而不是一条通用路径，代价是代码里多了几个相似函数，换来每类等待者能用各自的节奏与超时策略。**

### 空间回收不动写,只动等

`log_writer_wait_on_checkpoint_pessimistic()` 里有一个「还能写就继续写」的分支：

```
if (last_write_lsn + OS_FILE_LOG_BLOCK_SIZE <= hard_limited_lsn) {
  /* Write what we have - adjust the speed to speed of checkpoints
  going forward (to speed of page-cleaners). */
  return hard_limited_lsn;
}
```

**取舍：写入不会因为检查点滞后而完全停摆，代价是必须持续等待并可能反复报 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE`。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/log/log0log.cc | `log_checkpoint` | 检查点写入的入口符号；`log0log.cc` 的 `log_stop_background_threads()` 注释记录了它「请求持久化 DD 动态元数据、因而会与 `log.sn` X 锁互锁」的行为，是本页面 checkpoint 侧语义在本地检出中唯一可核对的落点 |
| storage/innobase/log/log0log.cc | `log_start` | 启动 redo 日志：设 `log.last_checkpoint_lsn` 与 `log.available_for_checkpoint_lsn` 为传入的 `checkpoint_lsn`，并按 `allow_checkpoints` 参数落 `log.m_allow_checkpoints` |
| storage/innobase/log/log0log.cc | `log_sys_create` | 创建 `log.checkpointer_mutex`（`LATCH_ID_LOG_CHECKPOINTER`）与 `log.checkpointer_event`、`log.next_checkpoint_event` 等事件 |
| storage/innobase/log/log0log.cc | `log_start_background_threads` | 创建并启动 log checkpointer 线程（`os_thread_create(log_checkpointer_thread_key, 0, log_checkpointer, &log)`），同时创建另外五个 redo 后台线程 |
| storage/innobase/log/log0log.cc | `log_stop_background_threads` | 停机时等待 checkpointer 线程退出；开头断言「持有 `log.sn` X 锁时不得停线程」，注释给出 `log_checkpoint()` 与 `log.sn` X 锁互锁的完整场景 |
| storage/innobase/log/log0log.cc | `log_make_empty_and_stop_background_threads` | 停机前反复调用 `log_make_latest_checkpoint(log)` 直到日志逻辑为空；循环条件是「写完检查点后当前 LSN 是否又变高」 |
| storage/innobase/log/log0log.cc | `log_position_lock` | 取 `log.sn` X 锁 + `log.checkpointer_mutex`，把「复制位点」与「检查点推进」互斥开 |
| storage/innobase/log/log0log.cc | `log_position_collect_lsn_info` | 在上述两级锁下读出 `log.last_checkpoint_lsn` 与当前 LSN，并断言 `current_lsn >= checkpoint_lsn` |
| storage/innobase/log/log0log.cc | `log_update_exported_variables` | 把 `log_get_checkpoint_lsn(log)` 导出为 `export_vars.innodb_redo_log_checkpoint_lsn` |
| storage/innobase/log/log0log.cc | `log_print` | 打印 `Last checkpoint at`（`log.last_checkpoint_lsn`）与 `Pages flushed up to`（`log.available_for_checkpoint_lsn`，取值时持 `log_limits_mutex`） |
| storage/innobase/log/log0write.cc | `log_write_up_to` | 用户线程入口：恢复期（`recv_no_ibuf_operations`）直接返回；后台写线程被暂停时走 `log_self_write_up_to()`，其中间接进入检查点等待路径 |
| storage/innobase/log/log0write.cc | `log_self_write_up_to` | 后台写线程暂停时的用户线程自写路径：先按 `srv_n_spin_wait_rounds` 自旋等就绪 LSN，再按 100 毫秒间隔等 `log.closer_event` |
| storage/innobase/log/log0write.cc | `log_writer_wait_on_checkpoint_optimistic` | 计算写入上限 `hard_limited_lsn` = 对齐到块边界的 `log.last_checkpoint_lsn` + `log.m_capacity.hard_logical_capacity()`，并断言 `checkpoint_lsn < next_write_lsn` |
| storage/innobase/log/log0write.cc | `log_writer_wait_on_checkpoint_pessimistic` | 空间不足时的等待循环：置 `log.checkpointer_event`、5 秒无进展报 `ER_IB_MSG_LOG_WRITER_OUT_OF_SPACE`、`log_request_checkpoint(log, false)` 请求非同步检查点并等 100 微秒；`log.m_allow_checkpoints` 为假时 `ib::fatal` 退出 |
| storage/innobase/log/log0write.cc | `log_writer_extra_margin_check` | 以 `soft_logical_capacity()` 计算软上限，越过则调 `log_writer_enter_extra_margin()` 进入应急态 |
| storage/innobase/log/log0write.cc | `log_writer_check_if_exited_extra_margin` | 供 log checkpointer 线程在推进检查点后反向更新 `log.m_writer_inside_extra_margin`（log writer 空闲时由它负责退出应急态） |
| storage/innobase/log/log0write.cc | `log_writer_wait_on_consumers` | 通用消费者等待路径；断言剩余消费者名必须是 `"MEB"`，注释说明检查点与归档器各有专用等待函数 |
| storage/innobase/log/log0write.cc | `log_writer` | log writer 线程主体；循环调 `log_writer_write_buffer()`，后者内部含检查点等待 |
| storage/innobase/log/log0recv.cc | `recv_find_max_checkpoint` | 在单个 redo 文件的 `HEADER_1`/`HEADER_2` 两块中取 `checkpoint_lsn` 最大者；`checkpoint_lsn == 0` 的块跳过；断言 `checkpoint_lsn >= LOG_START_LSN` |
| storage/innobase/log/log0recv.cc | `recv_find_max_checkpoint` | 跨全部 redo 文件取全局最新检查点；检查点 LSN 不在所在文件的 LSN 区间内时报 `ER_IB_MSG_RECOVERY_CHECKPOINT_OUTSIDE_LOG_FILE` 并跳过该文件 |
| storage/innobase/log/log0recv.cc | `recv_recovery_from_checkpoint_start` | 恢复总入口：找检查点、存入 `log.last_checkpoint_lsn`、比对 flush 位点决定是否 `recv_init_crash_recovery()`、`log_start(log, checkpoint_lsn, recovered_lsn, false)` 禁检查点、最后 `log_files_next_checkpoint(log, checkpoint_lsn)` 把检查点补写到另一块头部 |
| storage/innobase/log/log0recv.cc | `recv_recovery_begin` | 以 `checkpoint_lsn` 为起点初始化恢复状态：`recv_sys->checkpoint_lsn`、`recv_sys->scanned_lsn`、`recv_sys->recovered_lsn`、`recv_sys->previous_recovered_lsn` 均置为该值 |

## 与主核 / AI 的关系

主核（`engine/db/`）有一套独立的 WAL 实现，checkpoint 的语义与本页高度同构，但**机制形态并不相同**——主核侧是「日志文件截断」，InnoDB 侧是「循环复用 + 文件头双块交替」。

- **主核的 WAL 也有 checkpoint_lsn 概念，且语义相同。** `engine/db/src/wal/wal.h` 的 `wal_truncate(Wal *w, u32 keep_lsn)` 注释写明：「截断的前提是『这些记录已经不需要了』——也就是它们造成的效果已经**写进数据文件**了（页已经刷盘）。这正是 checkpoint 的定义」，且「恢复阶段从 checkpoint_lsn 开始重放，恢复时间只取决于『上次 checkpoint 之后的日志量』」。这与 InnoDB 的 `log.last_checkpoint_lsn` 是同一个语义。
- **恢复端的过滤规则也一致。** `engine/db/src/wal/recovery.c` 的 `recovery_run(Pager *pg, Wal *w, u32 checkpoint_lsn, bool *did_recover)` 中，重放时按 `if (vec.v[i].rec.lsn <= checkpoint_lsn) continue;` 过滤；**但回滚不按 checkpoint_lsn 过滤**——注释说明「未提交事务的记录可能横跨 checkpoint 点」，回滚要的是前像，哪怕 `lsn <= checkpoint_lsn` 也得用。这与 InnoDB 侧「redo 的 undo 由独立的 undo 日志承载」（见 I01 与 K 组边界）是同一种分层。
- **主核侧把两条硬约束显式写在头文件注释里**，与 InnoDB 侧隐含在断言里的约束形成对照：`wal.h` 写明「LSN 不能倒退」以及「clog 必须先落盘，顺序反了 = 静默丢数据」。InnoDB 侧对应的保护在 `log0recv.cc` 的 `recv_recovery_begin()` 与 `log_start()` 的一组 `ut_a()` 断言里。

**架构推论：** 主核 WAL 可以借鉴本页的三点设计——(1) 把检查点信息写成**双份交替**（InnoDB 的 `LOG_CHECKPOINT_1` / `LOG_CHECKPOINT_2`）以挡住「更新到一半崩溃」；(2) 把「可回收位置的计算」收敛到**单一后台线程**（InnoDB 的 log checkpointer），避免所有用户线程并发争用 flush 列表互斥量；(3) 在「写入端空间不足」与「刷脏端持页闩」之间明确避免同步握手（InnoDB 的 `log_request_checkpoint(log, false)` 及其死锁注释）。这三点都是**从 InnoDB 的实现读出来的做法**，主核侧是否存在等价实现需另行核对主核 WAL 源码，本页不作断言。

## 参考

- [InnoDB Checkpoints（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-checkpoints.html) —— fuzzy checkpointing 的官方定义：InnoDB 分批把修改过的页从缓冲池刷出，不需一次性刷整个缓冲池；崩溃恢复时从日志文件里的检查点标签往前扫。
- [Redo Log（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— redo 的官方定义、LSN 语义、`innodb_redo_log_capacity` 与 redo 文件布局；并明确「做检查点时，InnoDB 把 checkpoint LSN 存到包含该 LSN 的文件头部，恢复时检查所有 redo 文件并从最新的 checkpoint LSN 开始」。
- [InnoDB Recovery（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-recovery.html) —— 崩溃恢复流程的官方说明，是本页「检查点 → 恢复起点」这一消费路径的上位背景。
- [storage/innobase/log/log0log.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0log.cc) —— `log_start()`、检查点线程的创建与停止、`log.last_checkpoint_lsn` / `log.available_for_checkpoint_lsn` 的维护，以及文件头 `PAGE_INNODB_REDO_LOG` 一节对四个 LSN 与两条不变量的定义。
- [storage/innobase/log/log0write.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0write.cc) —— 文件头 `PAGE_INNODB_REDO_LOG_THREADS` 的 `sect_redo_log_checkpointer` 一节（可检查点 LSN 的计算公式、检查点线程的三项职责、并发争用的历史动机）、`PAGE_INNODB_REDO_LOG_FORMAT` 的检查点块布局，以及 `log_writer_wait_on_checkpoint*()` 与 `log_write_up_to()`。
- [storage/innobase/log/log0recv.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0recv.cc) —— 检查点的读取侧实现：`recv_find_max_checkpoint()` 的单文件与跨文件两版、`recv_recovery_from_checkpoint_start()` 的完整恢复入口与「补写到另一块头部」步骤，以及 `recv_recovery_begin()` 对 checkpoint_lsn 的初始化。
