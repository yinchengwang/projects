---
project: mysql-server
feature: binlog-cache-flush
page_type: feature
schema_version: 1
refs:
  - path: sql/binlog.cc
    symbols: [MYSQL_BIN_LOG::flush_cache_to_file, MYSQL_BIN_LOG::commit, MYSQL_BIN_LOG::finish_commit]
---

# binlog 缓存与刷盘

## 一句话职责

`sql/binlog.cc` 里的 binlog 缓存与刷盘机制负责把**事务内产生的事件先攒在会话级缓存里、等到事务提交时一次性写进 binlog 文件并落盘**，从而让 binlog 文件里的一个事务边界是完整且原子的。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责事务内事件的暂存缓存、事务提交时一次性落盘、以及 binlog 与 InnoDB 提交的两阶段配合；**不负责事件序列化（L01）**、**不负责从库接收（M 组）**。

## 机制是什么

### 一条 binlog 记录有三种物理形态

在 `sql/binlog.cc` 的 `binlog_cache_data` 里，一个事件从产生到落地经历三种形态：

1. **pending 事件**：`binlog_cache_data::m_pending`，类型是 `Rows_log_event *`，即还没序列化完成的行镜像暂存；
2. **缓存字节**：`binlog_cache_data::m_cache`，类型是 `Binlog_cache_storage`，是真正攒字节的地方，`is_binlog_empty()` 判断的正是 `pending() == nullptr && m_cache.is_empty()` 这两个条件同时成立；
3. **binlog 文件字节**：`m_binlog_file`，最终落盘目标。

会话里有一个 `binlog_cache_mngr`，内部含两个 `binlog_cache_data` 派生对象：`binlog_stmt_cache_data`（非事务语句缓存）和 `binlog_trx_cache_data`（事务缓存）。`binlog_cache_mngr::is_binlog_empty()` 返回 `stmt_cache.is_binlog_empty() && trx_cache.is_binlog_empty()`。

### 「事务原子性」靠的是缓存而非文件

事务缓存的存在意义是：**在事务真正提交之前，binlog 文件里看不到这个事务的任何字节**。写语句时事件先进缓存；只有走到提交路径、缓存被 finalize 之后，事件才会被拷进 binlog 文件。这保证了 binlog 文件里不会出现「半个事务」。

代价是缓存会占内存。这个内存预算由系统变量 `binlog_cache_size`（事务缓存）与 `binlog_stmt_cache_size`（语句缓存）控制，而它们各自有一个硬上限 `max_binlog_cache_size` 与 `max_binlog_stmt_cache_size`。上限的强制点在 `check_binlog_cache_size()` 与 `check_binlog_stmt_cache_size()`：一旦配置值超过上限，先用 `push_warning_printf()`（有 THD 时）或 `LogErr(WARNING_LEVEL, ...)`（无 THD 时）报一条 `ER_BINLOG_CACHE_SIZE_GREATER_THAN_MAX` / `ER_BINLOG_CACHE_SIZE_TOO_LARGE` 警告，然后把配置值**下调**到上限值：

```
if (binlog_cache_size > max_binlog_cache_size) {
  ...
  binlog_cache_size = static_cast<ulong>(max_binlog_cache_size);
}
```

缓存的初始打开在 `binlog_cache_mngr` 构造路径上完成，`trx_cache.open(binlog_cache_size, max_binlog_cache_size)` 与语句缓存对应的 `open(..., max_binlog_stmt_cache_size)` 把「配置值 + 硬上限」一起交给 `Binlog_cache_storage::open()`，因此运行时实际生效的缓存容量不会超过上限。

### 刷盘的粒度：事务边界，不是语句边界

刷盘发生在**提交**路径上，而不是每条语句后。`MYSQL_BIN_LOG::commit()` 里，事务缓存的 finalize 有四种情形，按判断顺序分别是：

| 情形 | 判据 | 结束事件 |
|---|---|---|
| XA PREPARE / XA COMMIT ONE PHASE | `is_loggable_xa_prepare(thd)` 或（`SQLCOM_XA_COMMIT` 且 `get_xa_opt(thd) == XA_ONE_PHASE` 且尚未 binlogged 且非手工 GTID） | `XA_prepare_log_event` |
| 原子 DDL | `cache_mngr->trx_cache.has_xid()` | 无结束事件（`finalize(thd, nullptr)`） |
| 2PC 事务 | `real_trans && xid && trn_ctx->rw_ha_count(trx_scope) > 1 && !trn_ctx->no_2pc(trx_scope)` | `Xid_log_event` |
| 其他 | 兜底分支 | `Query_log_event`（内容为 `"COMMIT"`） |

这四种情形对应的都是「事务缓存被封口」，随后缓存才在 flush 阶段被写进文件。

## 怎么实现的

### 第一步：`MYSQL_BIN_LOG::commit()` 把事件封口

`MYSQL_BIN_LOG::commit(THD *thd, bool all)` 的返回值是 `TC_LOG::enum_result`，三种取值：

- `RESULT_SUCCESS` —— 成功；
- `RESULT_ABORTED` —— 出错，事务既没记入 binlog 也没提交；
- `RESULT_INCONSISTENT` —— 出错，事务**已记入 binlog** 但没提交。

后两者是本页最关键的分界：binlog 与存储引擎的提交不是原子的，源码用两个不同返回值把「完全没写」和「写了但引擎提交失败」区分开。

函数体的关键顺序：

1. `SQLCOM_XA_COMMIT` 时先 `write_xa_to_cache(thd)`，注释写明原因是「XA commit must be written to the binary log prior to retrieving cache manager」——必须在取 cache manager 之前写进去，失败直接 `return RESULT_ABORTED`。
2. 取 `binlog_cache_mngr *cache_mngr = thd_get_cache_mngr(thd)`。`cache_mngr == nullptr` 表示没有东西要记，但仍要提交引擎：`trx_coordinator::commit_in_engines(thd, all)` 失败返回 `RESULT_ABORTED`，否则 `RESULT_SUCCESS`。
3. 语句缓存非空时先 `cache_mngr->stmt_cache.finalize(thd)`，并把 `stmt_stuff_logged` 置 true；同时 `trn_ctx->store_commit_parent(m_dependency_tracker.get_max_committed_timestamp())` 记录提交父标识。
4. 事务缓存非空、且 `ending_trans(thd, all)`、且事务内容还没记过时，走上表四种 finalize 分支之一，置 `trx_stuff_logged = true`。`ending_trans()` 的定义是 `all || ending_single_stmt_trans(thd, all)`，而 `ending_single_stmt_trans()` 是 `!all && !thd->in_multi_stmt_transaction_mode()`。
5. `!all` 时 `cache_mngr->trx_cache.set_prev_position(MY_OFF_T_UNDEF)`，这是语句回滚路径的一部分。
6. 只要 `stmt_stuff_logged || trx_stuff_logged` 为真，就走**两阶段配合**：
   - 先跑 `RUN_HOOK(transaction, before_commit, ...)`，钩子参数里带了 `max<my_off_t>(max_binlog_cache_size, max_binlog_stmt_cache_size)`。钩子返回真或命中 `simulate_failure_in_before_commit_hook` 调试开关时，`trx_coordinator::rollback_in_engines(thd, all)` 回滚引擎、`gtid_state->update_on_rollback(thd)` 回滚 GTID、`thd_get_cache_mngr(thd)->reset()` 清缓存，回报 `ER_RUN_HOOK_ERROR`，返回 `RESULT_ABORTED`。
   - 再检查插件是否要求回滚（`thd->get_transaction()->get_rpl_transaction_ctx()->is_transaction_rollback()`，或 `simulate_transaction_rollback_request` 调试开关）。要求回滚时同样 `rollback_in_engines` + `update_on_rollback` + `reset()`，回报 `ER_TRANSACTION_ROLLBACK_DURING_COMMIT`，返回 `RESULT_ABORTED`。
   - 通过后调用 `ordered_commit(thd, all, skip_commit)`，它返回真时 `return RESULT_INCONSISTENT`。
7. 若前面什么都没记（即 `stmt_stuff_logged` 与 `trx_stuff_logged` 都为假）且 `!skip_commit`，则 `trx_coordinator::commit_in_engines(thd, all)` 失败返回 `RESULT_INCONSISTENT`。

`skip_commit` 的来源是 `bool skip_commit = is_loggable_xa_prepare(thd);`，而 `is_loggable_xa_prepare()` 的实现是 `thd->get_transaction()->xid_state()->has_state(XID_STATE::XA_IDLE)`（`simulate_commit_failure` 调试开关下直接返回 false）。

### 第二步：`ordered_commit()` 的三阶段组提交

`ordered_commit()` 把刷盘拆成 flush / sync / commit 三个阶段。

**flush 阶段**：`process_flush_stage_queue()` 先 `fetch_and_process_flush_stage_queue()` 把整个 flush 队列取空（这样下一批才有 leader），并在这里调 `ha_flush_logs(true)` —— 源码注释说明「We flush prepared records of transactions to the log of storage engine (for example, InnoDB redo log) in a group right before flushing them to binary log」，也就是**先把引擎侧已 prepare 的记录刷出，再刷 binlog**，这是崩溃恢复的要求。

接着 `process_flush_stage_queue()` 遍历队列，对每个 THD 调 `flush_thread_caches(head)`。`flush_thread_caches()` 内部调 `cache_mngr->flush(thd, &bytes, &wrote_xid)`，即 `binlog_cache_mngr::flush()`；写入字节数大于 0 时记 `thd->set_trans_pos(log_file_name, m_binlog_file->position())`，若写了 XID 则 `inc_prep_xids(thd)`。

`binlog_cache_mngr::flush()` → `binlog_cache_data::flush()`，后者只在 `flags.finalized` 为真时才真正写；写之前先 `trn_ctx->sequence_number = mysql_bin_log.m_dependency_tracker.step();`，再用 `Binlog_event_writer writer(mysql_bin_log.get_binlog_file());` 调 `mysql_bin_log.write_transaction(thd, this, &writer, parallelization_barrier)`。写失败则 `thd->commit_error = THD::CE_FLUSH_ERROR`。写完调 `reset()`，并把 `*bytes_written` 设为写前的 `bytes_in_cache`（`m_cache.length()`）。

`MYSQL_BIN_LOG::write_transaction()` 里 `m_dependency_tracker.get_dependency(thd, parallelization_barrier, sequence_number, last_committed)` 取 MTS 依赖关系；`thd->get_transaction()->last_committed = SEQ_UNINIT;` 复位，以便 `binlog_cache_data::flush()` 在两个缓存都非空时把第二个缓存标成第一个的后继。

最终落到文件的是 `MYSQL_BIN_LOG::write_cache()`：它先 `mysql_mutex_assert_owner(&LOCK_log)`，然后 `if (!cache->is_empty())` 才调 `do_write_cache(cache, writer)`；`do_write_cache()` 内部是 `cache->copy_to(writer, &error)`，出错时 `report_binlog_write_error()`。`write_cache()` 的错误分支设 `thd->commit_error = THD::CE_FLUSH_ERROR` 并返回 true。

**第三步：`MYSQL_BIN_LOG::flush_cache_to_file()` 把字节推到内核**

`flush_cache_to_file()` 是本页三个 refs 之一，函数体极短：

```
int MYSQL_BIN_LOG::flush_cache_to_file(my_off_t *end_pos_var) {
  if (m_binlog_file->flush()) {
    THD *thd = current_thd;
    thd->commit_error = THD::CE_FLUSH_ERROR;
    return ER_ERROR_ON_WRITE;
  }
  *end_pos_var = m_binlog_file->position();
  return 0;
}
```

三点值得注意：

- 它的调用点**有条件**：`ordered_commit()` 里是 `if (flush_error == 0 && total_bytes > 0) flush_error = flush_cache_to_file(&flush_end_pos);` —— 没有新字节就不刷。
- 失败时把 `thd->commit_error` 置为 `THD::CE_FLUSH_ERROR`，返回 `ER_ERROR_ON_WRITE`。
- 成功时把 `m_binlog_file->position()` 写回 `*end_pos_var`。这个位置紧接着被 `after_flush` 钩子使用：`RUN_HOOK(binlog_storage, after_flush, (thd, file_name_ptr, flush_end_pos))`，其中 `file_name_ptr = log_file_name + dirname_length(log_file_name)`（只取文件名部分）。钩子失败则记 `ER_BINLOG_FAILED_TO_RUN_AFTER_FLUSH_HOOK` 并把 `flush_error` 置为 `ER_ERROR_ON_WRITE`。

同一个 flush 阶段里还有位置推进逻辑：`if (!update_binlog_end_pos_after_sync) update_binlog_end_pos();`，其中 `update_binlog_end_pos_after_sync = (get_sync_period() == 1)`。

**sync 阶段**：`sync_binlog_file(false)` 返回一个 `std::pair<bool, bool>`（第一项是错误标志，第二项是是否真的 sync 过）。函数体：

```
unsigned int sync_period = get_sync_period();
if (force || (sync_period && ++sync_counter >= sync_period)) {
  sync_counter = 0;
  if (DBUG_EVALUATE_IF("simulate_error_during_sync_binlog_file", 1,
                       m_binlog_file->is_open() && m_binlog_file->sync())) {
    THD *thd = current_thd;
    thd->commit_error = THD::CE_SYNC_ERROR;
    return std::make_pair(true, synced);
  }
  synced = true;
}
return std::make_pair(false, synced);
```

所以 **sync 的频率由 `get_sync_period()` 控制**，`sync_counter` 每比较一次加一；`sync_period` 为 0 时这段代码整体不进入，即不做 sync。注释同时说明「There is a chance that binlog file could be closed by 'RESET MASTER' or 'FLUSH LOGS' just after the leader releases LOCK_log and before it acquires LOCK_sync log. So it should check if m_binlog_file is opened.」——这就是 `m_binlog_file->is_open()` 判断的由来。

sync 阶段还有一条延迟让步逻辑：

```
if (!flush_error && (sync_counter + 1 >= get_sync_period()))
  Commit_stage_manager::get_instance().wait_count_or_timeout(
      opt_binlog_group_commit_sync_no_delay_count,
      opt_binlog_group_commit_sync_delay, Commit_stage_manager::SYNC_STAGE);
```

`opt_binlog_group_commit_sync_no_delay_count` 与 `opt_binlog_group_commit_sync_delay` 是两个系统变量（`binlog_group_commit_sync_delay` 与 `binlog_group_commit_sync_no_delay_count`），语义是「延迟 N 个组之后不再延迟」。

**commit 阶段**：由 `opt_binlog_order_commits || Clone_handler::need_commit_order()` 决定是否进入有序提交。进入后 `process_commit_stage_queue(thd, commit_queue)` 统一更新 GTID，源码注释解释了为什么不能各线程自己更新：那样「GTID order cannot be guaranteed and temporary gaps may appear in gtid_executed」，而增删区间要加锁，会降低性能。

### 第三步：`MYSQL_BIN_LOG::finish_commit()` 收尾

`finish_commit(THD *thd)` 是本页第三个 ref，负责所有线程（leader 与 follower）离开提交路径时的收尾：

1. `unlikely(!is_open())` 时清缓存：源码注释「In some unlikely situations, it can happen that binary log is closed before the thread flushes it's cache. In that case, clear the caches before doing commit.」，调 `cache_mngr->reset()`。二进制日志已关闭时，flush 与 sync 阶段被跳过，清缓存的工作被特意放在 `finish_commit` 里做，注释写明「so that leader and followers thread caches will be cleared」。
2. `thd->get_transaction()->sequence_number != SEQ_UNINIT` 时，在 `LOCK_replica_trans_dep_tracker` 保护下 `m_dependency_tracker.update_max_committed(thd)`。
3. `::finish_transaction_in_engines(thd, all, false)` —— 真正让存储引擎提交（InnoDB 侧）。
4. GTID 归属处理：`if (!thd->owned_gtid_is_empty())`，成功走 `gtid_state->update_on_commit(thd)`，失败走 `gtid_state->update_on_rollback(thd)`。注释说明这是 `--binlog-order-commits=0` 时的补丁路径。
5. `thd->get_transaction()->m_flags.xid_written` 为真时（XA PREPARE）：`trx_coordinator::set_prepared_in_tc_in_engines(thd, all)` + `dec_prep_xids(thd)`。
6. `committed_low && thd->commit_error != THD::CE_COMMIT_ERROR && run_hooks` 时跑 `RUN_HOOK(transaction, after_commit, (thd, all))`，并把 `run_hooks` 置 false。
7. 返回值：**只有 `THD::CE_COMMIT_ERROR` 才算错误**。源码注释写明「flush or sync errors are handled by the leader of the group (using binlog_error_action). Hence treat only COMMIT_ERRORs as errors.」

### 错误与降级路径

flush / sync 出错后由 leader 统一处理，入口是 `handle_binlog_flush_or_sync_error(thd, need_lock_log, message)`：

- `binlog_error_action == ABORT_SERVER`：拼出 `err_buff`（格式 `"%s Server is being stopped."`），调 `exec_binlog_error_action_abort(err_buff)` 停服务器；
- `binlog_error_action == IGNORE_ERROR`：在需要时取 `LOCK_log`，调 `close(LOG_CLOSE_INDEX | LOG_CLOSE_STOP_EVENT, false, true)` 关闭二进制日志（注释说明 close 要在 `LOCK_log` 保护下做以避免并行操作），随后 `if (check_write_error(thd)) thd->clear_error();` —— **这是明确的降级**：按 `IGNORE_ERROR` 语义清掉写错误，允许提交继续在存储引擎里发生。

错误文案由 `sprintf` 拼装，`%s` 处按 `thd->commit_error == THD::CE_FLUSH_ERROR ? "flush" : "sync"` 二选一。

flush 错误在 `ordered_commit()` 里于 `handle_binlog_flush_or_sync_error(thd, false, ...)` 处处理（此时已持有 `LOCK_log`，`need_lock_log` 传 false）；sync 错误在释放所有锁之后用 `handle_binlog_flush_or_sync_error(thd, true, nullptr)` 处理。flush 错误的 message 参数是条件表达式：`(thd->commit_error == THD::CE_FLUSH_GNO_EXHAUSTED_ERROR) ? ER_THD(thd, ER_GNO_EXHAUSTED) : nullptr`。

### 生命周期

`binlog_cache_data` 的析构函数断言 `is_binlog_empty()`，即缓存必须在对象销毁前被清空或写过。`reset()` 负责清理：`compute_statistics()`、`remove_pending_event()`、`m_cache.reset()`（失败记 `ER_BINLOG_CANT_RESIZE_CACHE` 警告），并清掉 `with_xid` / `immediate` / `finalized` / `with_sbr` / `with_rbr` / `with_start` / `with_end` / `with_content` 这些标志位。

`binlog_cache_data::finalize()` 之后会 `stmt_cache.reset(); trx_cache.reset();`（见 `binlog_cache_mngr` 的 finalize 路径）。

## 关键取舍

### 取舍一：缓存换原子性，代价是内存与「大事务拖尾」

**放弃的是**「每条语句立刻写进 binlog 文件」这种最省内存的做法。**换来的是** binlog 文件里的事务是完整的一段，崩溃时不会出现半个事务。

代价具体落在两处：`binlog_cache_size` / `binlog_stmt_cache_size` 这两个内存预算，以及它们各自的硬上限 `max_binlog_cache_size` / `max_binlog_stmt_cache_size`（超过就被 `check_binlog_cache_size()` / `check_binlog_stmt_cache_size()` 强制下调并告警）。

**换成 XXX 方案会怎样：** 若改成「语句级直接落盘」，binlog 文件里就会出现语句级边界而非事务级边界，两阶段提交的前提消失——崩溃后无法判断已落盘的那部分语句属于哪个事务，也就无法安全地决定引擎侧该不该提交。源码里 `commit()` 返回 `RESULT_INCONSISTENT`（「transaction was logged but not committed」）这一档的存在，正是这种设计的直接产物。

### 取舍二：flush 与 sync 分开，且 sync 可按周期摊薄

**放弃的是**「每组提交都 fsync」的确定性。**换来的是** 可配置的持久性/延迟比。

代价体现在 `sync_binlog_file()` 的条件 `force || (sync_period && ++sync_counter >= sync_period)`：`sync_period` 为 0 时本组不做 sync（`sync_period` 为 0 是判断条件的一部分，不是魔数）。fsync 频率由 `get_sync_period()` 决定，而 `get_sync_period()` 读的是构造 `MYSQL_BIN_LOG` 时传入的 `sync_period_ptr`（构造函数 `MYSQL_BIN_LOG::MYSQL_BIN_LOG(uint *sync_period, bool relay_log)` 保存 `sync_period_ptr(sync_period)`），它对应 `sync_binlog` 系统变量。

**换成 XXX 方案会怎样：** 若强制每组都 fsync，`sync_binlog=0` 的高吞吐场景就不可用，提交延迟会被磁盘 fsync 时间完全绑定；而反过来若完全不 sync，崩溃时会丢失已向客户端确认提交的事务。

### 取舍三：flush 错误不立刻报错，而是按 `binlog_error_action` 统一处置

**放弃的是**「flush 失败立即让本次提交返回错误」的简单做法。**换来的是** leader 可以对整组事务做一致处置。

代价是错误语义变复杂：`finish_commit()` 只把 `THD::CE_COMMIT_ERROR` 当作错误返回，flush / sync 错误交给 leader 通过 `binlog_error_action` 处理（注释明确写了这一点）。在 `IGNORE_ERROR` 模式下，`handle_binlog_flush_or_sync_error()` 会关闭二进制日志并 `thd->clear_error()`，让提交继续在引擎侧发生——**binlog 与引擎的一致性就此断开**。

**换成 XXX 方案会怎样：** 若每次 flush 失败都直接报错，组提交里的 follower 会因为 leader 的错误而跟着失败，组提交的批量收益被错误传播吃掉。

### 取舍四：GTID 更新统一放到 commit 阶段串行做

**放弃的是**「各线程提交完自己更新 GTID」的做法。**换来的是** `gtid_executed` 保持为单一区间、不出现临时空洞。

代价是 GTID 更新成为串行点，源码注释直接点明「adding and removing intervals requires a mutex, which would reduce performance」。对应地，`--binlog-order-commits=0` 时 `process_commit_stage_queue()` 不更新 GTID，`finish_commit()` 里的 `gtid_state->update_on_commit(thd)` / `update_on_rollback(thd)` 就是这条路径的补丁。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/binlog.cc | `MYSQL_BIN_LOG::commit` | binlog 提交总入口：判断语句/事务缓存是否非空、选择四种 finalize 分支封口、跑 `before_commit` 钩子、调用 `ordered_commit`，返回 `RESULT_SUCCESS` / `RESULT_ABORTED` / `RESULT_INCONSISTENT` |
| sql/binlog.cc | `MYSQL_BIN_LOG::flush_cache_to_file` | 把 binlog 文件的 I/O 缓存刷到文件；失败置 `THD::CE_FLUSH_ERROR` 并返回 `ER_ERROR_ON_WRITE`，成功回写 `m_binlog_file->position()` |
| sql/binlog.cc | `MYSQL_BIN_LOG::finish_commit` | 提交收尾：清缓存（binlog 已关闭时）、`finish_transaction_in_engines`、GTID 归属更新、XA prepared 标记、`after_commit` 钩子；仅 `THD::CE_COMMIT_ERROR` 视为错误 |
| sql/binlog.cc | `MYSQL_BIN_LOG::ordered_commit` | 三阶段（flush / sync / commit）组提交主干，串联 `process_flush_stage_queue`、`flush_cache_to_file`、`sync_binlog_file` 与 `process_commit_stage_queue` |
| sql/binlog.cc | `MYSQL_BIN_LOG::process_flush_stage_queue` | 取空 flush 队列、调 `ha_flush_logs(true)`、逐个 THD 调 `flush_thread_caches`，统计 `total_bytes` |
| sql/binlog.cc | `MYSQL_BIN_LOG::fetch_and_process_flush_stage_queue` | 取出整个 flush 队列并置空（保证下一批有 leader），刷引擎侧 prepared 记录 |
| sql/binlog.cc | `MYSQL_BIN_LOG::flush_thread_caches` | 对单个 THD 调 `cache_mngr->flush`，记录事务位置 `set_trans_pos`，必要时 `inc_prep_xids` |
| sql/binlog.cc | `MYSQL_BIN_LOG::sync_binlog_file` | 按 `get_sync_period()` 与 `sync_counter` 决定本组是否 fsync；失败置 `THD::CE_SYNC_ERROR` |
| sql/binlog.cc | `MYSQL_BIN_LOG::handle_binlog_flush_or_sync_error` | 按 `binlog_error_action`（`ABORT_SERVER` / `IGNORE_ERROR`）统一处置 flush/sync 错误；`IGNORE_ERROR` 下 `close()` 二进制日志并 `thd->clear_error()` |
| sql/binlog.cc | `binlog_cache_data::flush` | 缓存真正落盘处：取依赖关系序号、调 `write_transaction`、写失败置 `THD::CE_FLUSH_ERROR`、写后 `reset()` |
| sql/binlog.cc | `binlog_cache_data::reset` | 清缓存与事务标志位；`m_cache.reset()` 失败记 `ER_BINLOG_CANT_RESIZE_CACHE` |
| sql/binlog.cc | `binlog_cache_data::is_binlog_empty` | 判断缓存是否为空：`pending() == nullptr && m_cache.is_empty()` |
| sql/binlog.cc | `binlog_cache_mngr::is_binlog_empty` | 语句缓存与事务缓存同时为空才算空 |
| sql/binlog.cc | `binlog_cache_mngr` | 会话级缓存管理器，持有 `binlog_stmt_cache_data` 与 `binlog_trx_cache_data` |
| sql/binlog.cc | `binlog_cache_data` | 缓存基类：pending 事件 + `Binlog_cache_storage` 字节缓存 + 事务标志位 |
| sql/binlog.cc | `check_binlog_cache_size` | `binlog_cache_size` 超过 `max_binlog_cache_size` 时告警并下调到上限 |
| sql/binlog.cc | `check_binlog_stmt_cache_size` | `binlog_stmt_cache_size` 超过 `max_binlog_stmt_cache_size` 时告警并下调到上限 |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_transaction` | 写一个缓存到文件：取 MTS 依赖、构造 `Gtid_log_event`、设置事务长度 |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_cache` | 把缓存字节拷进 binlog 文件；空缓存直接返回；出错置 `THD::CE_FLUSH_ERROR` |
| sql/binlog.cc | `MYSQL_BIN_LOG::do_write_cache` | 低层写盘：`cache->copy_to(writer, &error)`，出错 `report_binlog_write_error()` |
| sql/binlog.cc | `ending_trans` | 判定是否在结束事务：`all || ending_single_stmt_trans(thd, all)` |
| sql/binlog.cc | `is_loggable_xa_prepare` | 判定 XA PREPARE 是否可记日志：`xid_state()->has_state(XID_STATE::XA_IDLE)` |
| sql/binlog.cc | `MYSQL_BIN_LOG::update_binlog_end_pos` | 推进 `atomic_binlog_end_pos` 并 `signal_update()`，供 dump 线程感知最新位置 |
| sql/binlog.cc | `call_after_sync_hook` | sync 成功后跑 `after_sync` 钩子，失败返回 `ER_ERROR_ON_WRITE` |

## 与主核 / AI 的关系

MySQL 的 binlog 缓存与刷盘是**存储引擎与复制之间的仲裁点**，不是一个孤立的 IO 优化。

- 对主核（存储引擎）而言：`commit()` 的 `before_commit` 钩子与 `trx_coordinator::rollback_in_engines()` 让引擎有机会在 binlog 落盘前否决提交；`finish_commit()` 里的 `::finish_transaction_in_engines(thd, all, false)` 才是引擎真正提交的时刻。**binlog 先落盘、引擎后提交**这个顺序，就是两阶段配合在代码里的落点。
- 对 AI / 数据同步链路而言：binlog 文件是 CDC（变更数据捕获）与向量库增量同步的输入源。`update_binlog_end_pos()` 推进的 `atomic_binlog_end_pos` 正是 dump 线程（L04）读取位置的基准；而「事务级完整边界」保证了 AI 侧的增量消费可以按事务为最小单位重放，不会拿到半个事务。

**架构推论：** 因为 flush 阶段由 leader 统一执行、且 `IGNORE_ERROR` 模式下会 `close()` 并 `clear_error()` 让引擎侧提交继续，AI 消费链路在 binlog 故障窗口内可能读到**缺失一段 binlog** 的数据流；此时需要靠 GTID（`gtid_executed`）与从库侧的 GTID 集合比对来发现缺口，不能只依赖 binlog 文件本身连续。这一条是从 `handle_binlog_flush_or_sync_error()` 的降级分支与 `update_on_commit` / `update_on_rollback` 的分叉推出来的，源码并未直接陈述对下游消费方的语义。

## 参考

- [MySQL 参考手册：二进制日志选项（`sync_binlog`、`binlog_cache_size`、`max_binlog_cache_size`、`binlog_group_commit_sync_delay`）](https://dev.mysql.com/doc/refman/8.0/en/replication-options-binary-log.html) —— 这些系统变量的官方定义与取值范围，是本页缓存上限与 sync 周期判断的官方口径。
- [MySQL 参考手册：服务器系统变量（`binlog_cache_size` / `binlog_stmt_cache_size` / `max_binlog_cache_size` / `max_binlog_stmt_cache_size`）](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html) —— 缓存内存预算与硬上限的官方说明。
- [MySQL Server 源码：`sql/binlog.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/binlog.cc) —— 本页全部三个 refs 符号（`MYSQL_BIN_LOG::flush_cache_to_file` / `MYSQL_BIN_LOG::commit` / `MYSQL_BIN_LOG::finish_commit`）及 `ordered_commit` / `process_flush_stage_queue` / `binlog_cache_data` 的真实实现。
- [MySQL Server 源码：`sql/rpl_commit_stage_manager.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_commit_stage_manager.cc) —— 组提交三个阶段（BINLOG_FLUSH_STAGE / SYNC_STAGE / COMMIT_STAGE）的队列与 leader 选举实现。
- [MySQL 参考手册：二进制日志（Binary Log）](https://dev.mysql.com/doc/refman/8.0/en/binary-log.html) —— 二进制日志整体语义的官方说明，用于确认「binlog 是事务边界」这一背景。
