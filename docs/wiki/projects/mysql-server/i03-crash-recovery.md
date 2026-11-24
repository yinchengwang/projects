---
project: mysql-server
feature: crash-recovery
page_type: feature
schema_version: 1
refs:
  - path: storage/innobase/log/log0recv.cc
    symbols: [recv_recovery_from_checkpoint_start, recv_recovery_from_checkpoint_finish, recv_init_crash_recovery]
---

# 崩溃恢复

## 一句话职责

`storage/innobase/log/log0recv.cc` 里的崩溃恢复机制负责在 InnoDB 启动阶段定位最新的 redo 检查点、向前扫描 redo 日志并把已提交但尚未落盘的页修改重新应用到表空间文件中，使数据文件回到「最后一次检查点 + 其后 redo」的等价状态；**不负责 undo 回滚（K 组）**、**不负责 redo 的写入（I01）**。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责启动时扫描 redo 日志、重做已提交但未落盘的修改；**不负责 undo 回滚（K 组）**、**不负责日志的写入（I01）**。

## 机制是什么

### 三个函数划出的生命周期

`log0recv.cc` 用三个函数把恢复切成三段：

| 函数 | 角色 | 关键副作用 |
|---|---|---|
| `recv_recovery_from_checkpoint_start(log_t &log, lsn_t flush_lsn)` | 找检查点 + 扫描 redo + 启动应用 | 置 `recv_recovery_on = true`；调 `recv_init_crash_recovery()`；调 `recv_recovery_begin()` 扫描；调 `log_start()` 重新开日志；置 `recv_sys->apply_log_recs = true` |
| `recv_recovery_from_checkpoint_finish(bool aborting)` | 收尾 | 等 `recv_writer` 线程退出；`recv_recovery_on = false`；`recv_sys_free()`；返回 `MetadataRecover *` |
| `recv_init_crash_recovery()` | 判定「确实需要恢复」并拉起刷脏线程 | 置 `recv_needed_recovery = true`；调 `recv_sys->dblwr->recover()`；起 `recv_writer_thread` |

### 全局状态标记

`log0recv.cc` 里有一组文件级全局量，把「恢复正在进行」这件事暴露给其它模块：

- `recv_recovery_on`（`volatile bool`）：源码注释写明「true when applying redo log records during crash recovery; false otherwise. Note that this is false while a background thread is rolling back incomplete transactions.」。`recv_recovery_from_checkpoint_start()` 里置真，`recv_recovery_from_checkpoint_finish()` 里置假。
- `recv_needed_recovery`（`bool`）：注释为「true when recv_init_crash_recovery() has been called.」。`recv_init_crash_recovery()` 的第一件事就是 `ut_a(!recv_needed_recovery)`，然后置真——即**一个进程生命周期内只允许初始化一次恢复环境**。
- `recv_lsn_checks_on`：`buf_page_is_corrupted()` 是否要检查 `FIL_PAGE_LSN` 是否超前；注释说「Initially false, and set by recv_recovery_from_checkpoint_start()」。
- `recv_no_ibuf_operations`：`log0write.cc` 的 `log_write_up_to()` 里第一个分支就是 `if (recv_no_ibuf_operations) { return Wait_stats{0}; }`，注释解释它「becomes true if the log record hash table becomes too full」，此时日志文件暂不允许操作。
- `recv_max_page_lsn`（文件内 `static lsn_t`）：注释为「The maximum lsn we see for a page during the recovery process. If this is bigger than the lsn we are able to scan up to, that is an indication that the recovery failed and the database may be corrupt.」。

### 恢复起点是检查点，但检查点 LSN 未必是记录边界

`recv_recovery_from_checkpoint_start()` 用 `recv_find_max_checkpoint(log, checkpoint)` 在**所有现存 redo 文件**里找 `m_checkpoint_lsn` 最大的那个。该函数逐文件打开（`Log_file_access_mode::READ_ONLY`），在文件内遍历 `Log_checkpoint_header_no::HEADER_1` 与 `HEADER_2` 两个检查点头，读失败且错误码不是 `DB_CORRUPTION` 时 `ut_a(err == DB_CORRUPTION)` 断言后 `continue`。

关键细节：`checkpoint_lsn` 可能是**某条 redo 记录中间**的位置。`recv_scan_log_recs()` 里有一段长注释解释了处理办法：

> We start to parse log records even before checkpoint_lsn, from the beginning of the log block which contains the checkpoint_lsn. That's because the first group of log records in the log block, starts before checkpoint_lsn, and checkpoint_lsn could potentially point to the middle of some log record.

解决办法是设 `recv_sys->bytes_to_ignore_before_checkpoint = recv_sys->checkpoint_lsn - recv_sys->parse_start_lsn`，并断言 `bytes_to_ignore_before_checkpoint <= OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE`。之后 `recv_update_bytes_to_ignore_before_checkpoint(len)` 在每次解析记录时扣减这个计数，扣到 0 之前的记录一律不落哈希表。**被忽略的字节数上限由 `OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE` 这个常量约束**（512 - 12 = 500 字节）。

### 扫描与哈希表

`recv_recovery_begin(log_t &log, const lsn_t checkpoint_lsn)` 是扫描主循环：

1. 重置 `recv_sys->len`、`recovered_offset`、`n_addrs`，调 `recv_sys_empty_hash()` 清空哈希表。
2. 设 `recv_sys->parse_start_lsn = 0`、`bytes_to_ignore_before_checkpoint = 0`、`checkpoint_lsn`、`scanned_lsn`、`recovered_lsn` 全部指向检查点 LSN。
3. 计算可用内存上限 `max_mem = UNIV_PAGE_SIZE * (buf_pool_get_n_pages() - (recv_n_pool_free_frames * srv_buf_pool_instances))`。
4. 起点对齐到块边界：`start_lsn = ut_uint64_align_down(checkpoint_lsn, OS_FILE_LOG_BLOCK_SIZE)`。
5. 循环调用 `recv_read_log_seg(log, log.buf, start_lsn, start_lsn + RECV_SCAN_SIZE)` 读一段，再调 `recv_scan_log_recs()` 扫这一段。`recv_read_log_seg()` 返回 0 表示出错，函数返回 `DB_ERROR`；返回值等于 `start_lsn` 表示「刚好在文件切换处断了」，直接 `break`。

`recv_scan_log_recs()` 逐块处理，每块做四道校验：

- **块号校验**：`block_header.m_hdr_no != log_block_convert_lsn_to_hdr_no(scanned_lsn)` → 视为「Garbage or an incompletely written log block」，`finished = true` 收工。
- **校验和校验**：`log_block_checksum_is_ok(log_block)` 为假 → 打 `ER_IB_MSG_720`，同样当作 redo 日志的突然结束。
- **epoch 校验**：`log_block_epoch_no_is_valid(block_header.m_epoch_no, recv_sys->scanned_epoch_no)` 为假 → 注释说是「Garbage from a log buffer flush which was made before the most recent database recovery」，收工。该判定函数只接受「等于上一个 epoch」或「等于上一个 epoch + 1」两种值。
- **首个记录组定位**：`block_header.m_first_rec_group > 0` 时设 `recv_sys->parse_start_lsn = scanned_lsn + block_header.m_first_rec_group`。

`log_block_checksum_is_ok()` 的实现是 `!srv_log_checksums || log_block_get_checksum(block) == log_block_calc_checksum(block)`——**校验和可以整体关闭**（`srv_log_checksums` 为假时直接返回真）。

### 解析与哈希

`recv_parse_log_recs()` 在解析缓冲里循环取记录，按首字节的 `MLOG_SINGLE_REC_FLAG` 位分流到 `recv_single_rec()` 或 `recv_multi_rec()`。

`recv_add_to_hash_table(type, space_id, page_no, body, rec_end, start_lsn, end_lsn)` 是记录进哈希表的唯一入口：

- 断言排除了 `MLOG_FILE_DELETE` / `MLOG_FILE_CREATE` / `MLOG_FILE_RENAME` / `MLOG_FILE_EXTEND` / `MLOG_DUMMY_RECORD` / `MLOG_INDEX_LOAD`——这些类型在解析阶段就单独处理了。
- 记录体按不超过 `RECV_DATA_BLOCK_SIZE` 的块切分存储。`RECV_DATA_BLOCK_SIZE` 定义为 `(MEM_MAX_ALLOC_IN_BUF - sizeof(recv_data_t))`，注释说明「this must be less than UNIV_PAGE_SIZE as it is stored in the buffer pool」。
- 首次遇到某个 `(space_id, page_no)` 时新建 `recv_addr_t`，`state = RECV_NOT_PROCESSED`，并 `++recv_sys->n_addrs`。

`recv_single_rec()` 与 `recv_multi_rec()` 都有同一道闸门：只有 `recv_recovery_on` 为真、且（`space_id == TRX_SYS_SPACE` 或 `fil_tablespace_lookup_for_recovery(space_id)` 非空）时才把记录加进哈希表。`recv_multi_rec()` 另外用 `recv_sys->save_rec()` / `recv_sys->get_saved_rec()` 暂存多条记录，槽位数上限由 `recv_sys_t::MAX_SAVED_MLOG_RECS` 决定（`recv_sys_init()` 里 `recv_sys->saved_recs.resize(recv_sys_t::MAX_SAVED_MLOG_RECS)`）。

### 应用阶段

`recv_apply_hashed_log_recs(log_t &log, bool allow_ibuf)` 遍历哈希表把 redo 应用到页：

- 打开表空间用 `fil_tablespace_open_for_recovery(space.first)`；返回 `DB_CORRUPTION` 时直接 `return err` 并中止启动（注释：「Page couldn't be recovered from doublewrite, we cannot proceed with recovery.」）；返回其他错误则认定表空间已被 drop，把该空间的页全部标为 `RECV_DISCARDED`。
- `recv_apply_log_rec(recv_addr)` 对每个页：页在缓冲池里就 `buf_page_get()` 取 X latch 后调 `recv_recover_page(false, block)`；页不在缓冲池就调 `recv_read_in_area(page_id)` 预读。
- 进度输出分两档：静态常量 `PCT = 10`（每 10% 打一次 `ER_IB_MSG_708`），以及 `PRINT_INTERVAL` 时间间隔打一次 `ER_IB_MSG_709`（带两位小数的百分比）。当 `unit = batch_size / PCT <= PCT` 时改成一次性打 100%。

`recv_recover_page_func(bool just_read_in, buf_block_t *block)` 才是真正改页的函数：

- `recv_sys->apply_log_recs == false` 时直接返回。
- 取出该页的 `recv_addr_t`，若为 `nullptr` 或状态已是 `RECV_BEING_PROCESSED` / `RECV_PROCESSED` 则返回。
- 置 `recv_addr->state = RECV_BEING_PROCESSED`，开一个 `mtr_t` 并 `mtr_set_log_mode(&mtr, MTR_LOG_NONE)`——**恢复期间不回写新的 redo**。
- 逐条遍历 `recv_addr->rec_list`，跳过 `recv->start_lsn < page_lsn` 的记录（`page_lsn` 取页头 `FIL_PAGE_LSN`，若 `buf_page_get_newest_modification()` 非零则取更新的那个）。
- 应用后写回页 LSN：`mach_write_to_8(FIL_PAGE_LSN + page, end_lsn)`，其中 `end_lsn = recv->start_lsn + recv->len`。
- 收尾时更新 `recv_max_page_lsn`（取最大值）、置 `state = RECV_PROCESSED`、`--recv_sys->n_addrs`。

### 错误分支

`recv_report_corrupt_log()` 是损坏日志的统一出口：打 `ER_IB_MSG_694` / `ER_IB_MSG_695`，按 `limit = 100` 字节的窗口把出错位置前后的原始字节 `ut_print_buf()` 打到 stderr。**它的返回值决定是否继续**：

```
if (srv_force_recovery == 0) {
  ib::info(ER_IB_MSG_697);
  return false;
}
ib::warn(ER_IB_MSG_LOG_CORRUPT, FORCE_RECOVERY_MSG);
```

即 `srv_force_recovery == 0` 时返回假 → 中止恢复；否则只警告并继续。

`recv_recovery_from_checkpoint_start()` 尾部有三组失败出口：

```
if ((recv_sys->found_corrupt_log && srv_force_recovery == 0) ||
    recv_sys->found_corrupt_fs) {
  return DB_ERROR;
}
```

以及 `recovered_lsn < checkpoint_lsn` 时（此时非只读模式直接 `ut_error`）返回 `DB_ERROR`。

## 怎么实现的

### 阶段一：`recv_recovery_from_checkpoint_start()`

执行顺序：

**第一步，初始化 flush 红黑树并处理强制恢复模式。**

```
buf_flush_init_flush_rbt();

if (srv_force_recovery >= SRV_FORCE_NO_LOG_REDO) {
  ib::info(ER_IB_MSG_728);
  /* We leave redo log not started and this is read-only mode. */
  ut_a(log.sn == 0);
  ut_a(srv_read_only_mode);
  return DB_SUCCESS;
}
```

`SRV_FORCE_NO_LOG_REDO` 这个阈值来自 `innodb_force_recovery`：达到该级别时**完全跳过 redo 应用**，日志也不启动。

**第二步，置恢复开关并找检查点。**

```
recv_recovery_on = true;
ut_a(log.m_format == Log_format::CURRENT);

Log_checkpoint_location checkpoint;
if (!recv_find_max_checkpoint(log, checkpoint)) {
  ib::error(ER_IB_MSG_RECOVERY_CHECKPOINT_NOT_FOUND);
  return DB_ERROR;
}
```

**第三步，读检查点头并取出 `checkpoint_lsn`。** 用 `log_checkpoint_header_read(checkpoint_file_handle, checkpoint.m_checkpoint_header_no, checkpoint_header)` 读出检查点头，读失败直接返回该错误码。断言 `checkpoint_lsn == checkpoint_header.m_checkpoint_lsn`。

**第四步，判定「是否需要恢复」。** 这是 `checkpoint_lsn` 与 `flush_lsn` 的比较：

```
if (checkpoint_lsn != flush_lsn) {
  if (checkpoint_lsn < flush_lsn) {
    ib::warn(ER_IB_MSG_RECOVERY_CHECKPOINT_FROM_BEFORE_CLEAN_SHUTDOWN, ...);
  }
  if (!recv_needed_recovery) {
    ib::info(ER_IB_MSG_RECOVERY_IS_NEEDED, ...);
    if (srv_read_only_mode) {
      ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY);
      return DB_ERROR;
    }
    recv_init_crash_recovery();
  }
}
```

即：检查点 LSN 与「上次刷盘到的 LSN」相等 → 干净关闭，无需恢复；不等 → 需要恢复。**只读模式下需要恢复直接失败返回 `DB_ERROR`**。

**第五步，扫描 redo。** `err = recv_recovery_begin(log, checkpoint_lsn);` 失败返回该错误码。

**第六步，只读模式的二次检查。**

```
if (srv_read_only_mode && log.m_scanned_lsn > checkpoint_lsn) {
  ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY);
  return DB_ERROR;
}
```

**第七步，一致性检查。** 把 `log.m_scanned_lsn` 在块边界上补 `LOG_BLOCK_HDR_SIZE` 后，与 `checkpoint_lsn`、`recv_max_page_lsn` 比较，不满足则打 `ER_IB_MSG_737`。

**第八步，重启日志。**

```
err = log_start(log, checkpoint_lsn, recovered_lsn, false);
```

第四个参数 `allow_checkpoints` 传 `false`，注释写明「Disallow checkpoints until recovery is finished」。`log_start()` 在 `log0log.cc` 里把 `log.recovered_lsn = start_lsn`、`log.last_checkpoint_lsn = checkpoint_lsn`、`log.available_for_checkpoint_lsn = checkpoint_lsn`，并调 `log_files_start(log)`。

**第九步，把检查点写进另一个头。**

```
log.next_checkpoint_header_no =
    log_next_checkpoint_header(checkpoint.m_checkpoint_header_no);
err = log_files_next_checkpoint(log, checkpoint_lsn);
```

源码注释解释了原因：「Make the preservation of max checkpoint info on disk certain by writing the checkpoint also to the other checkpoint header. After that both headers will have the same checkpoint_lsn.」这一步只在非只读模式下做。

**第十步，打开应用开关。**

```
mutex_enter(&recv_sys->mutex);
recv_sys->apply_log_recs = true;
mutex_exit(&recv_sys->mutex);
```

注释说明此时「The database is now ready to start almost normal processing of user transactions: transaction rollbacks and the application of the log records in the hash table can be run in background.」

### 阶段二：`recv_init_crash_recovery()`

```
static void recv_init_crash_recovery() {
  ut_ad(!srv_read_only_mode);
  ut_a(!recv_needed_recovery);

  recv_needed_recovery = true;

  ib::info(ER_IB_MSG_726);
  ib::info(ER_IB_MSG_727);

  recv_sys->dblwr->recover();

  if (srv_force_recovery < SRV_FORCE_NO_LOG_REDO) {
    srv_threads.m_recv_writer =
        os_thread_create(recv_writer_thread_key, 0, recv_writer_thread);
    srv_threads.m_recv_writer.start();
  }
}
```

三件事：

1. 置 `recv_needed_recovery = true`，并用 `ut_a(!recv_needed_recovery)` 保证只做一次。
2. 调 `recv_sys->dblwr->recover()`——先从 doublewrite buffer 抢救页，再谈 redo 重做。
3. 起 `recv_writer_thread` 线程，负责「flushing dirty pages from the buffer pools」。源码注释给出的完整时序是：`recv_recovery_from_checkpoint_start()` 里启动它 → `recv_recovery_from_checkpoint_finish()` 里等它结束。

`recv_writer_thread()` 的主循环每轮 `std::this_thread::sleep_for(std::chrono::milliseconds(100))`——**轮询间隔 100 毫秒**；退出条件是 `srv_shutdown_state.load() == SRV_SHUTDOWN_NONE` 为假，或 `recv_recovery_on` 变假。它做的动作是设 `recv_sys->flush_type = BUF_FLUSH_LRU` 并通过 `recv_sys->flush_start` / `flush_end` 两个事件驱动一次 LRU 刷脏。

`recv_init_crash_recovery()` 还有一个调用点：`recv_scan_log_recs()` 里，当 `!recv_needed_recovery && scanned_lsn > recv_sys->checkpoint_lsn` 时（扫描过程中发现检查点之后还有日志）也会调用它。这是**扫描中途才判定需要恢复**的路径。

### 阶段三：`recv_recovery_from_checkpoint_finish()`

```
mutex_enter(&recv_sys->writer_mutex);

if (recv_sys->is_meb_db) dblwr::g_mode = recv_sys->dblwr_state;

recv_recovery_on = false;

buf_flush_wait_LRU_batch_end();

mutex_exit(&recv_sys->writer_mutex);

uint32_t count = 0;
while (recv_writer_is_active()) {
  ++count;
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
  if (count >= 600) {
    ib::info(ER_IB_MSG_738);
    count = 0;
  }
}
```

要点：

- **先置 `recv_recovery_on = false` 再等线程退出**——`recv_writer_thread()` 的循环条件就是检查这个标志。
- 等 `recv_writer` 线程的轮询间隔是 100 毫秒；`count >= 600` 时打一次 `ER_IB_MSG_738` 并把计数清零（600 × 100 毫秒 = 60 秒）。
- `recv_writer_is_active()` 的实现是 `srv_thread_is_active(srv_threads.m_recv_writer)`。

之后：

```
MetadataRecover *metadata{};
if (!aborting) {
  std::swap(metadata, recv_sys->metadata_recover);
}
recv_sys_free();
```

`recv_sys_free()` 释放恢复系统的数据结构，并调 `recv_sys->dblwr->recovered()`（`recv_sys_finish()` 里）。

非 aborting 时还会做四页类型校验：

```
verify_page_type({IBUF_SPACE_ID, FSP_IBUF_HEADER_PAGE_NO}, FIL_PAGE_TYPE_SYS);
verify_page_type({TRX_SYS_SPACE, FSP_FIRST_RSEG_PAGE_NO}, FIL_PAGE_TYPE_SYS);
verify_page_type({TRX_SYS_SPACE, TRX_SYS_PAGE_NO}, FIL_PAGE_TYPE_TRX_SYS);
verify_page_type({TRX_SYS_SPACE, FSP_DICT_HDR_PAGE_NO}, FIL_PAGE_TYPE_SYS);
```

`verify_page_type()` 的注释说这是「Validate a few system page types that were left uninitialized by older versions of MySQL.」，类型不匹配时 `ib::fatal(UT_LOCATION_HERE, ER_IB_MSG_INVALID_PAGE_TYPE, ...)`。

最后 `buf_flush_free_flush_rbt()` 释放第一步建的红黑树。

## 关键取舍

### 检查点 LSN 不保证落在记录边界，必须容忍「向前多解析一点」

`recv_scan_log_recs()` 里的做法是：即使 `parse_start_lsn < checkpoint_lsn`，也从包含检查点的那个日志块的开头开始解析，但用 `bytes_to_ignore_before_checkpoint` 把检查点之前的记录丢弃。

**取舍：多解析少量字节（上限 `OS_FILE_LOG_BLOCK_SIZE - LOG_BLOCK_HDR_SIZE`），换取「不需要在日志块内部精确定位记录起点」这个更简单的实现。** 源码注释明确说明了动机：必须遍历该块内所有记录组才能发现它们的起点，而遍历就意味着要解析。

### 损坏日志：默认中止，`srv_force_recovery` 打开后才放行

`recv_report_corrupt_log()` 里 `srv_force_recovery == 0` 时返回假 → 整个恢复中止；非零时打 `ER_IB_MSG_LOG_CORRUPT` 警告后继续。同样地，`recv_recovery_from_checkpoint_start()` 尾部 `recv_sys->found_corrupt_log && srv_force_recovery == 0` 也返回 `DB_ERROR`。

**取舍：默认优先保证数据正确性（宁可启动失败也不静默丢数据），代价是需要人工介入时必须显式设置 `innodb_force_recovery`。** 这与官方文档「Forcing InnoDB Recovery」一节给出的处置路径一致。

### `SRV_FORCE_NO_LOG_REDO`：一条硬闸门，把 redo 完全踢出流程

`recv_recovery_from_checkpoint_start()` 在 `srv_force_recovery >= SRV_FORCE_NO_LOG_REDO` 时立刻 `return DB_SUCCESS`，不设 `recv_recovery_on`、不扫日志、不起 `recv_writer` 线程；`recv_init_crash_recovery()` 里也在同一条件下跳过起线程。

**取舍：这条路径只保证「能启动、能 dump 数据」，不保证数据一致性**——源码注释直言「We leave redo log not started and this is read-only mode.」。

### 恢复期间不回写新的 redo

`recv_recover_page_func()` 开 mtr 后立刻 `mtr_set_log_mode(&mtr, MTR_LOG_NONE)`，并在提交后断言 `ut_a(mtr.get_log_mode() == MTR_LOG_NONE)`；`recv_apply_hashed_log_recs()` 在冲刷阶段还临时置 `log.disable_redo_writes = true`（`ut_d` 包裹，调试构建生效）。

**取舍：恢复期间产生的页修改不写 redo，降低了「重做产生新 redo、再重做新 redo」的复杂度；代价是这段期间的页修改只存在于内存/脏页里，崩溃会再次丢失。** 这条安全性由 `recv_writer_thread` 持续刷脏页来补偿——恢复期间的脏页由独立线程按 `BUF_FLUSH_LRU` 刷出。

### 哈希表满了就提前应用一批

`recv_scan_log_recs()` 里：

```
if (recv_heap_used() > max_memory) {
  recv_apply_hashed_log_recs(log, false);
}
```

`max_memory` 来自 `recv_recovery_begin()` 的计算：`UNIV_PAGE_SIZE * (buf_pool_get_n_pages() - (recv_n_pool_free_frames * srv_buf_pool_instances))`。而 `recv_n_pool_free_frames` 在 `recv_sys_var_init()` 里被设为 **256**，注释说「This many frames must be left free in the buffer pool when we scan the log... This is the default value. If the actual size of the buffer pool is larger than 10 MB we'll set this value to 512.」（512 这个后置调整发生在本文件之外，故此处只标注来源）。

**取舍：哈希表无限增长会吃掉缓冲池的可用帧，所以到阈值就先应用一批把页落地再继续扫；代价是恢复过程中的页 I/O 次数变多。** `recv_apply_hashed_log_recs(log, false)` 第二个参数 `allow_ibuf` 为假，意味着期间禁止 change buffer 操作（`recv_no_ibuf_operations = true`）。

### 只读模式在两处被拒

第一处在 `recv_recovery_from_checkpoint_start()` 第四步：需要恢复且 `srv_read_only_mode` → `ib::error(ER_IB_MSG_RECOVERY_IN_READ_ONLY)` 返回 `DB_ERROR`。第二处在第六步：只读且 `log.m_scanned_lsn > checkpoint_lsn` → 同样返回 `DB_ERROR`。

**取舍：只读实例无法做 redo 应用，崩溃后无法自愈；这条约束也写在 `recv_scan_log_recs()` 里——只读模式下改为 `ib::warn(ER_IB_MSG_RECOVERY_SKIPPED_IN_READ_ONLY_MODE)` 并直接返回。**

### 旧格式 redo 的兼容路径

`recv_verify_log_is_clean_pre_8_0_30(log_t &log)` 处理 8.0.30 之前的 redo 格式（`ut_a(log.m_format < Log_format::CURRENT)`）。它按旧版的方式「comparing checkpoints' numbers instead of checkpoints' LSN values」在 `HEADER_1` / `HEADER_2` 里选 `m_checkpoint_no` 更大的那个，源码注释解释这是为兼容性刻意保留旧行为。它还检查 redo 是否「clean」：读回检查点所在块，`log_block_get_data_len(buf) != (source_offset & (OS_FILE_LOG_BLOCK_SIZE - 1))` 时打 `ER_IB_MSG_LOG_FORMAT_OLD_AND_NO_CLEAN_SHUTDOWN` 返回 `DB_ERROR`。

### 加密 redo 的处理

`recv_read_log_seg()` 里 `log_data_blocks_read()` 返回 `DB_UNSUPPORTED` 时（说明块是加密的），先调 `log_encryption_read(log)` 取密钥再重试一次；重试仍是 `DB_UNSUPPORTED` 则打 `ER_IB_MSG_CANT_DECRYPT_REDO_LOG` 返回 0（`recv_recovery_begin()` 见到 0 返回 `DB_ERROR`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| storage/innobase/log/log0recv.cc | `recv_recovery_from_checkpoint_start` | 恢复第一阶段：初始化 flush 红黑树、处理 `SRV_FORCE_NO_LOG_REDO`、置 `recv_recovery_on`、用 `recv_find_max_checkpoint` 找检查点、比较 `checkpoint_lsn` 与 `flush_lsn` 判定是否需要恢复、调 `recv_recovery_begin` 扫描、一致性检查后调 `log_start` 重启日志并把检查点写进另一个头、最后置 `recv_sys->apply_log_recs = true` |
| storage/innobase/log/log0recv.cc | `recv_init_crash_recovery` | 置 `recv_needed_recovery = true`（前置断言 `ut_a(!recv_needed_recovery)`）、调 `recv_sys->dblwr->recover()`、在 `srv_force_recovery < SRV_FORCE_NO_LOG_REDO` 时创建并启动 `recv_writer_thread` |
| storage/innobase/log/log0recv.cc | `recv_recovery_from_checkpoint_finish` | 恢复第三阶段：持 `writer_mutex` 置 `recv_recovery_on = false`、调 `buf_flush_wait_LRU_batch_end`、以 100 毫秒间隔轮询等 `recv_writer` 线程退出（计数到 600 打 `ER_IB_MSG_738`）、交换并返回 `MetadataRecover *`、调 `recv_sys_free`、校验四个系统页类型、释放 flush 红黑树 |
| storage/innobase/log/log0recv.cc | `recv_recovery_begin` | 扫描主循环：重置恢复状态、算 `max_mem`、起点按 `OS_FILE_LOG_BLOCK_SIZE` 向下对齐、循环调 `recv_read_log_seg` 与 `recv_scan_log_recs`，读失败返回 `DB_ERROR` |
| storage/innobase/log/log0recv.cc | `recv_scan_log_recs` | 逐日志块扫描：校验块号与校验和、校验 epoch、定位 `parse_start_lsn`、设 `bytes_to_ignore_before_checkpoint`、必要时调 `recv_init_crash_recovery`、调 `recv_parse_log_recs` 解析、超内存时调 `recv_apply_hashed_log_recs` |
| storage/innobase/log/log0recv.cc | `recv_read_log_seg` | 按段读 redo 文件：跨文件时切到下一个文件；返回 `DB_UNSUPPORTED` 时调 `log_encryption_read` 取密钥后重试 |
| storage/innobase/log/log0recv.cc | `recv_find_max_checkpoint` | 遍历所有现存 redo 文件找 `m_checkpoint_lsn` 最大的检查点；检查点 LSN 落在文件范围外时打 `ER_IB_MSG_RECOVERY_CHECKPOINT_OUTSIDE_LOG_FILE` 并跳过该文件 |
| storage/innobase/log/log0recv.cc | `recv_parse_log_recs` | 在解析缓冲里按 `MLOG_SINGLE_REC_FLAG` 分流到 `recv_single_rec` 或 `recv_multi_rec` |
| storage/innobase/log/log0recv.cc | `recv_single_rec` | 解析单条 redo 记录：算 `new_recovered_lsn`、更新 `recovered_offset`、扣减 `bytes_to_ignore_before_checkpoint`，满足 `recv_recovery_on` 与表空间可见条件时调 `recv_add_to_hash_table` |
| storage/innobase/log/log0recv.cc | `recv_multi_rec` | 解析一个 mtr 内的多条记录：用 `save_rec` / `get_saved_rec` 暂存，遇 `MLOG_MULTI_REC_END` 收尾，再逐条落哈希表 |
| storage/innobase/log/log0recv.cc | `recv_add_to_hash_table` | 把 redo 记录加入按 (space_id, page_no) 索引的哈希表：新建 `recv_addr_t` 时 `state = RECV_NOT_PROCESSED` 且 `++recv_sys->n_addrs`；记录体按 `RECV_DATA_BLOCK_SIZE` 分块存储 |
| storage/innobase/log/log0recv.cc | `recv_apply_hashed_log_recs` | 应用阶段主循环：用 `fil_tablespace_open_for_recovery` 打开表空间（`DB_CORRUPTION` 直接中止启动）、逐页调 `recv_apply_log_rec`、按 `PCT = 10` 或 `PRINT_INTERVAL` 输出进度、结束后清空哈希表 |
| storage/innobase/log/log0recv.cc | `recv_apply_log_rec` | 单页应用：页在缓冲池则 `buf_page_get` 取 X latch 后调 `recv_recover_page`，否则调 `recv_read_in_area` 预读；表空间缺失或被 drop 时标 `RECV_PROCESSED` 并记入 `missing_ids` |
| storage/innobase/log/log0recv.cc | `recv_recover_page_func` | 真正改页的函数：`apply_log_recs` 为假直接返回、置 `RECV_BEING_PROCESSED`、开 `mtr` 并设 `MTR_LOG_NONE`、跳过 `start_lsn < page_lsn` 的记录、应用后写回 `FIL_PAGE_LSN`、更新 `recv_max_page_lsn` 并置 `RECV_PROCESSED` |
| storage/innobase/log/log0recv.cc | `recv_report_corrupt_log` | 损坏日志的统一出口：打 `ER_IB_MSG_694` / `ER_IB_MSG_695`、按 `limit = 100` 字节窗口 `ut_print_buf` 打印原始字节；`srv_force_recovery == 0` 时返回假中止恢复，否则警告后继续 |
| storage/innobase/log/log0recv.cc | `recv_verify_log_is_clean_pre_8_0_30` | 8.0.30 之前格式的 redo 兼容路径：按 `m_checkpoint_no` 而非 LSN 选检查点、校验 redo 是否 clean、失败打 `ER_IB_MSG_LOG_FORMAT_OLD_AND_NO_CLEAN_SHUTDOWN` |
| storage/innobase/log/log0recv.cc | `recv_writer_thread` | 恢复期间的刷脏线程：每 100 毫秒轮询一次 `recv_recovery_on`，通过 `flush_start` / `flush_end` 事件驱动 `BUF_FLUSH_LRU` 刷脏 |
| storage/innobase/log/log0recv.cc | `recv_read_in_area` | 按 `RECV_READ_AHEAD_AREA = 32` 的页号窗口预读，把待恢复页读进缓冲池 |
| storage/innobase/log/log0recv.cc | `recv_update_bytes_to_ignore_before_checkpoint` | 扣减 `bytes_to_ignore_before_checkpoint`，控制检查点之前那批记录不进入哈希表 |
| storage/innobase/log/log0recv.cc | `recv_track_changes_of_recovered_lsn` | 跟踪 `recovered_lsn` 跨块变化，更新 `recv_sys->last_block_first_mtr_boundary` |
| storage/innobase/log/log0recv.cc | `log_block_epoch_no_is_valid` | 判定日志块的 epoch 是否合法：只接受等于上一个 epoch 或等于上一个 epoch + 1 |
| storage/innobase/log/log0recv.cc | `recv_sys_var_init` | 重置恢复相关全局量：`recv_n_pool_free_frames = 256`、`recv_max_page_lsn = 0` 等 |
| storage/innobase/log/log0recv.cc | `recv_sys_init` | 分配解析缓冲（`RECV_PARSING_BUF_SIZE` 与 `srv_log_buffer_size` 取小）、初始化 `saved_recs`（大小为 `recv_sys_t::MAX_SAVED_MLOG_RECS`）、创建 `dblwr` 与 `MetadataRecover` |
| storage/innobase/log/log0recv.cc | `recv_sys_resize_buf` | 解析缓冲按 2 倍扩容，上限为 `srv_log_buffer_size`；达上限时打 `ER_IB_MSG_723` 返回假 |
| storage/innobase/log/log0recv.cc | `recv_reset_buffer` | 解析缓冲内已消费数据左移，`recovered_offset` 归零 |
| storage/innobase/log/log0recv.cc | `MetadataRecover::parseMetadataLog` | 解析动态元数据 redo（`MLOG_TABLE_DYNAMIC_META`），解析器缺失或内容损坏时置 `recv_sys->found_corrupt_log = true` |
| storage/innobase/log/log0recv.cc | `MetadataRecover::store` | 把恢复期收集的动态元数据写回 `mysql.innodb_dynamic_metadata` |
| storage/innobase/log/log0log.cc | `log_start` | 被 `recv_recovery_from_checkpoint_start` 调用：设 `log.recovered_lsn` / `last_checkpoint_lsn` / `available_for_checkpoint_lsn`、调 `log_files_start`，`allow_checkpoints` 传 `false` 表示恢复完成前禁止检查点 |
| storage/innobase/buf/buf0rea.cc | `buf_read_recv_pages` | 被 `recv_read_in_area` 调用，把待恢复的页读进缓冲池；克隆库场景下按 `recv_sys->is_cloned_db` 扩展表空间 |

## 与主核 / AI 的关系

主核（`engine/db/`）有独立的 WAL 实现，与本页描述的 InnoDB redo 没有代码级关联，但**恢复阶段的结构形态同构**，可作对照：

- **都是「先定位恢复起点，再向前扫描重做」**：本页的 `recv_find_max_checkpoint` 找检查点 LSN、`recv_recovery_begin` 从该处向前扫描；主核 WAL 的恢复同样需要先确定一个重做起点。
- **都把「重做」与「回滚」分成两件事**：本页明确把 undo 回滚排除在边界之外（K 组），恢复只做前滚重做；主核侧的回滚也是独立机制。
- **都有「恢复期间不写新日志」的纪律**：本页的 `recv_recover_page_func` 用 `mtr_set_log_mode(&mtr, MTR_LOG_NONE)` 明确禁止恢复过程产生新 redo。

**架构推论：** 主核 WAL 可以借鉴本页的两点设计——(1) 恢复起点允许落在记录中间，用「向前多解析一点 + 计数丢弃」的思路避免在记录内部精确定位；(2) 恢复期间禁止回写新日志，避免「重做产生新日志、新日志再被重做」的递归。**这两点是从 InnoDB 的实现读出来的做法，主核侧是否存在等价实现需另行核对主核源码，本页不作断言。**

## 参考

- [InnoDB Recovery（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-recovery.html) —— 「InnoDB Crash Recovery」一节给出官方口径的恢复步骤顺序：表空间发现 → redo 应用 → 不完整事务回滚 → change buffer 合并 → purge；并说明 redo 应用在初始化期间、接受连接之前完成。
- [InnoDB Checkpoints（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-checkpoints.html) —— 说明 fuzzy checkpointing 与「恢复时找到检查点标签后向前扫描日志并应用修改」，是本页「检查点即重做起点」的官方依据。
- [Redo Log（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— redo 日志的官方定义、LSN 语义，以及「During recovery, all redo log files are checked and recovery starts at the latest checkpoint LSN」这一与 `recv_find_max_checkpoint` 直接对应的说明。
- [storage/innobase/log/log0recv.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0recv.cc) —— 本页全部主符号的权威来源。
- [storage/innobase/log/log0log.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/storage/innobase/log/log0log.cc) —— `log_start()` 的实现与 `log_t` 结构定义，是本页「恢复后重启日志」一步的权威来源。
