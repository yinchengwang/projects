---
project: mysql-server
feature: replica-applier
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_replica.cc
    symbols: [start_slave, stop_slave]
  - path: sql/rpl_channel_service_interface.cc
    symbols: [start_slave]
---

# 从库应用线程

## 一句话职责

`sql/rpl_replica.cc` 里的 `start_slave` / `stop_slave` 是从库应用侧（SQL 线程）的启停入口：它们按通道（channel）把 `handle_slave_sql` 这条应用线程拉起或停下，并把并行度、提交顺序、UNTIL 条件、跳过计数等应用侧参数固化到 `Relay_log_info` 上；`sql/rpl_channel_service_interface.cc` 里的 `channel_start` 复用同一个 `start_slave`，让复制服务接口（`START REPLICA` / 复制通道管理）走同一条启动路径。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责从中继日志读取事件并按并行度协调应用、以及回放位点的持久化；**不负责事件拉取（M01）**、**不负责 GTID 集合（M03）**。

## 机制是什么

从库复制在源码里被拆成两条线程，共用同一个 `Master_info` / `Relay_log_info` 结构：

- **IO 线程**（`handle_slave_io`）：把源库 binlog 的字节流搬进中继日志文件。
- **应用线程**（`handle_slave_sql`）：从中继日志读出事件并执行；并行模式下由它担任**协调者（Coordinator）**，把事件分组派发给 **worker** 线程。

应用侧有三层结构，职责各不相同：

1. **启停层**：`start_slave` / `stop_slave`（多通道版本）与 `start_slave` / `stop_slave`（单通道重载）负责权限校验、参数固化、锁与线程拉起/终止。
2. **协调者层**：`handle_slave_sql` 是协调者线程的入口，负责读中继日志、判定跳过与 UNTIL、分派事务组、推进 checkpoint。
3. **worker 层**：`slave_worker_exec_job_group`（`sql/rpl_rli_pdb.cc`）在 worker 线程里真正执行事件；`Commit_order_manager`（`sql/rpl_replica_commit_order_manager.cc`）在开启提交顺序保持时保证事务按源库顺序提交。

### 并行度的两种子模式

协调者启动时按 `rli->channel_mts_submode` 选择 MTS 子模式：

- `MTS_PARALLEL_TYPE_LOGICAL_CLOCK` → `new Mts_submode_logical_clock()`（逻辑时钟，按事务的提交依赖关系并行）。
- `MTS_PARALLEL_TYPE_DB_NAME` → `new Mts_submode_database()`（按库名分片，同一库的串行行串行）。

两者的差别不只是分片键：等待语义完全不同。`Mts_submode_logical_clock::wait_for_workers_to_finish` 的等待条件是 `delegated_jobs > jobs_done`，循环体内调用 `mta_checkpoint_routine(rli, true)` 推进 checkpoint，返回 `-1` 表示出错；`Mts_submode_database::wait_for_workers_to_finish` 则是遍历 `rli->mapping_db_to_worker`，对每个 `entry->usage > 0` 的条目在 `rli->slave_worker_hash_cond` 上等待直到 `entry->usage` 归零，并把该 worker 持有的临时表资源用 `mts_move_temp_tables_to_thd` 迁走。若任一 worker 的 `running_status != Slave_worker::RUNNING`，返回 `-1`。

### 事务组与 GAQ

并行执行时，协调者不直接执行事件，而是把事件打包成组交给 worker：

- `apply_event_and_update_pos` 里 `ev->worker != rli` 且 `ev->worker` 非空时，构造 `Slave_job_item`，调 `append_item_to_jobs(job_item, w, rli)` 把事件推入该 worker 的队列，并把 `*ptr_ev = nullptr` 标记「事件已交给 worker」。
- 组的序号存在 `ev->mts_group_idx` 里，取值是 `rli->gaq->assigned_group_index`——GAQ（`Slave_committed_queue`）在 `slave_start_workers` 里以 `rli->checkpoint_group` 为容量构造：`rli->gaq = new Slave_committed_queue(rli->checkpoint_group, n)`。
- `slave_worker_exec_job_group` 通过 `rli->gaq->get_job_group(ev->mts_group_idx)` 取回组信息，若该组带有新的 `new_fd_event`，先 `worker->set_rli_description_event(ptr_g->new_fd_event)` 更新中继日志格式描述事件。

## 怎么实现的

### 启动路径：`start_slave` 的两个重载

`sql/rpl_replica.cc` 里有两个同名 `start_slave`：

- `start_slave(THD *thd)`：多通道入口。函数体先判 `channel_map.get_num_instances() == 1`——只有默认通道时取 `channel_map.get_default_channel_mi()`，转调单通道重载；多通道时先在 `LOCK_sql_replica_skip_counter` 保护下检查 `sql_replica_skip_counter > 0 && !(thd->lex->slave_thd_opt & SLAVE_IO)`，命中则报 `ER_REPLICA_CHANNEL_SQL_SKIP_COUNTER` 并直接返回真。随后遍历 `channel_map`，对每个满足 `mi->inited || mi->reset` 且 `mi->host[0]` 非空的通道逐个调单通道重载；单个通道失败只记 `ER_RPL_REPLICA_CANT_START_REPLICA_FOR_CHANNEL` 错误日志并置 `error = true`，不中断其他通道。全部成功才调 `my_ok(thd)`。
- `start_slave(THD *thd, LEX_SLAVE_CONNECTION *connection_param, LEX_MASTER_INFO *master_param, int thread_mask_input, Master_info *mi, bool set_mts_settings)`：单通道实现，函数体顺序是：

  1. `thd->set_skip_readonly_check()`——START REPLICA 必须能写 `mysql.slave_master_info` / `mysql.slave_relay_log_info` 元数据表，所以忽略只读检查。
  2. 权限校验：`sctx->check_access(SUPER_ACL)` 或 `sctx->has_global_grant("REPLICATION_SLAVE_ADMIN")`，两者都不满足报 `ER_SPECIFIC_ACCESS_DENIED_ERROR`。
  3. `mi->channel_wrlock()` 拿通道写锁。
  4. 若 `connection_param->user || connection_param->password` 且 `!thd->get_ssl()`，推 `ER_INSECURE_PLAIN_TEXT` 警告。
  5. `lock_slave_threads(mi)` 后调 `init_thread_mask(&thread_mask, mi, true /* inverse */)` 得到**已停止**线程的掩码，再与 `thread_mask_input` 相与：`thread_mask &= thread_mask_input`——用户只要 IO 线程时不碰 SQL 线程。
  6. `load_mi_and_rli_from_repositories(mi, false, thread_mask)` 从元数据表装载位点；失败报 `ER_CONNECTION_METADATA`。
  7. 若 `thread_mask & SLAVE_IO`，把命令行给的 `user` / `password` / `plugin_auth` / `plugin_dir` 写进 `mi`。
  8. 若 `thread_mask & SLAVE_SQL`，做三件事：
     - 在 `LOCK_sql_replica_skip_counter` 保护下把全局 `sql_replica_skip_counter` 拷进 `mi->rli->slave_skip_counter`，然后把 `sql_replica_skip_counter` 置 0。**拷贝有条件**：仅当 `m_assign_gtids_to_anonymous_transactions_info.get_type() != AGAT_OFF` 或 `global_gtid_mode.get() != Gtid_mode::ON` 时才拷贝——即在 GTID 开启且匿名事务 GTID 分配关闭时不拷。
     - 当 `set_mts_settings` 为真时固化并行参数：`mi->rli->opt_replica_parallel_workers = opt_mts_replica_parallel_workers`；若 `mi->is_gtid_only_mode()` 且 `opt_mts_replica_parallel_workers == 0`，把 `opt_replica_parallel_workers` 抬到 `1`（GTID-only 模式至少要一个 worker）；`mts_parallel_option == MTS_PARALLEL_TYPE_DB_NAME` 时置 `channel_mts_submode` 为 `MTS_PARALLEL_TYPE_DB_NAME`，否则为 `MTS_PARALLEL_TYPE_LOGICAL_CLOCK`；`mi->rli->checkpoint_group = opt_mta_checkpoint_group`。
     - `mi->rli->init_until_option(thd, master_param)` 解析 UNTIL 条件，非零则 `my_error(slave_errno)`；再调 `check_slave_sql_config_conflict(mi->rli)` 检查并行配置冲突。
  9. 若只启 IO 线程但命令里带了 `master_param->pos || master_param->relay_log_pos || master_param->gtid`，推 `ER_UNTIL_COND_IGNORED` 警告。
  10. 调 `start_slave_threads(false /*need_lock_slave*/, true /*wait_for_start*/, mi, thread_mask)` 真正拉起线程。
  11. 若 `thread_mask` 为 0（线程本来就在跑），只推 `ER_REPLICA_CHANNEL_WAS_RUNNING` 警告，不报错。
  12. 收尾：若 `is_error && (thread_mask & SLAVE_IO) == SLAVE_IO`，调 `mi->reset_start_info()` 清掉刚才写进去的账号密码，避免凭据残留。

### 拉起线程：`start_slave_threads`

`start_slave_threads(bool need_lock_slave, bool wait_for_start, Master_info *mi, int thread_mask)` 的函数体：

1. 若 `!mi->inited || !mi->rli->inited`，按哪一侧未初始化分别报 `ER_REPLICA_CM_INIT_REPOSITORY` / `ER_REPLICA_AM_INIT_REPOSITORY` 并返回真。
2. `check_replica_configuration_errors(mi, thread_mask)`。
3. 若 `global_gtid_mode.get() == Gtid_mode::ON && mi->is_auto_position() && rli->until_condition == Relay_log_info::UNTIL_SQL_AFTER_MTS_GAPS`，推 `ER_WARN_SQL_AFTER_MTS_GAPS_GAP_NOT_CALCULATED` 警告——GTID + 自动定位模式下没有 gaps 信息，该 UNTIL 条件无效。
4. `thread_mask & SLAVE_IO` 时调 `start_slave_thread(key_thread_replica_io, handle_slave_io, lock_io, lock_cond_io, cond_io, &mi->slave_running, &mi->slave_run_id, mi)`。
5. 若启了 IO 或 MONITOR 且 `mi->is_source_connection_auto_failover()` 且监控进程没在跑，调 `Source_IO_monitor::get_instance()->launch_monitoring_process(key_thread_replica_monitor_io)`；失败则调 `terminate_slave_threads(mi, thread_mask & (SLAVE_IO | SLAVE_MONITOR), rpl_stop_replica_timeout, need_lock_slave)` 回退。
6. `thread_mask & SLAVE_SQL` 时：若 `mi->rli->recovery_parallel_workers != 0`，先调 `mts_recovery_groups(mi->rli)` 做 MTS 恢复间隙收集，失败报 `ER_MTA_RECOVERY_FAILURE`；然后调 `start_slave_thread(key_thread_replica_sql, handle_slave_sql, lock_sql, lock_cond_sql, cond_sql, &mi->rli->slave_running, &mi->rli->slave_run_id, mi)`。失败同样调 `terminate_slave_threads(mi, thread_mask & (SLAVE_IO | SLAVE_MONITOR), rpl_stop_replica_timeout, need_lock_slave)` 回退。

### 协调者主循环：`handle_slave_sql`

`handle_slave_sql(void *arg)` 的函数体（`arg` 是 `Master_info *`，取其 `rli`）：

1. `my_thread_init()` 后在 `rli->run_lock` 保护下建 `THD`，赋给 `rli->info_thd`。
2. 按 `rli->channel_mts_submode` 建 MTS 子模式：非 `MTS_PARALLEL_TYPE_DB_NAME` 建 `Mts_submode_logical_clock`，否则建 `Mts_submode_database`。
3. 建提交顺序管理器：`if (opt_replica_preserve_commit_order && !rli->is_parallel_exec() && rli->opt_replica_parallel_workers > 1)` 时 `commit_order_mngr = new Commit_order_manager(rli->opt_replica_parallel_workers)`。**三个条件缺一不可**——非并行模式不建，worker 数不大于 1 也不建。建好后 `rli->set_commit_order_manager(commit_order_mngr)`。
4. 设通道类型：`group_replication_applier` → `GR_APPLIER_CHANNEL`，`group_replication_recovery` → `GR_RECOVERY_CHANNEL`，其余 → `RPL_STANDARD_CHANNEL`。
5. 置运行标志：`rli->slave_run_id++`、`rli->slave_running = 1`、`rli->sql_thread_kill_accepted = false`。
6. `init_replica_thread(thd, SLAVE_THD_SQL)`；失败则广播 `rli->start_cond`、报 `ER_REPLICA_FATAL_ERROR` 并跳 `err`。
7. `rli->rpl_filter->is_on()` 为真时建 `rli->deferred_events = new Deferred_log_events()`。
8. `slave_start_workers(rli, rli->opt_replica_parallel_workers, &mts_inited)` 启动 worker 池；失败报 `ER_REPLICA_FATAL_ERROR`。
9. `rli->clear_error()` 清错，并逐个 `rli->get_worker(i)->clear_error()` 清 worker 的错。
10. `rli->update_is_transactional()` 失败则报 `ER_REPLICA_FATAL_ERROR`；`!rli->is_transactional()` 则报一条 WARNING，说明崩溃时中继日志信息不保证一致。
11. 广播 `rli->start_cond` 并释放 `rli->run_lock`——这一步唤醒等待 `wait_for_start` 的调用方。
12. `applier_reader.open(&errmsg)` 打开中继日志读取器，失败报 `ER_REPLICA_FATAL_ERROR`。
13. `check_temp_dir(rli->slave_patternload_file, rli->get_channel())` 校验临时目录。
14. 权限检查：`rli->check_privilege_checks_user()` 与 `rli->initialize_applier_security_context()`，任一失败报权限错误。
15. 若 `opt_init_replica.length` 非零，`execute_init_command(thd, &opt_init_replica, &LOCK_sys_init_replica)` 执行初始化命令。
16. 主循环 `while (!main_loop_error && !sql_slave_killed(thd, rli))`：
    - `mysql_mutex_lock(&rli->data_lock)` 后调 `applier_reader.read_next_event()` 取事件，再解锁。
    - 若 `rli->is_parallel_exec() && rli->current_mts_submode != nullptr`，调 `rli->current_mts_submode->set_multi_threaded_applier_context(*rli, *ev)` 为调度器补齐上下文。
    - 调 `exec_relay_log_event(thd, rli, &applier_reader, ev)`，按返回的 `enum_slave_apply_event_and_update_pos_retval` 分派：
      - `SLAVE_APPLY_EVENT_AND_UPDATE_POS_OK`、`SLAVE_APPLY_EVENT_UNTIL_REACHED`、`SLAVE_APPLY_EVENT_RETRY`：继续循环。
      - `SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPLY_ERROR`、`SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR`、`SLAVE_APPLY_EVENT_AND_UPDATE_POS_APPEND_JOB_ERROR`：置 `main_loop_error = true`。
17. 退出后：若 `main_loop_error` 且线程未被 kill，调 `report_apply_event_error(thd, rli)` 上报；置 `rli->atomic_is_stopping = true`；跑 `applier_stop` hook；调 `slave_stop_workers(rli, &mts_inited)` 停 worker 池；`delete rli->current_mts_submode` 并置空；调 `rli->clear_mts_recovery_groups()`；`rli->cleanup_context(thd, true)`；最后在 `rli->data_lock` 下 `applier_reader.close()`、置 `rli->slave_running = 0`、`rli->atomic_is_stopping = false`、`rli->set_rli_description_event(nullptr)` 忘掉中继日志格式，并广播 `rli->data_cond` 唤醒 `source_pos_wait()`。

### 读取中继日志：`Rpl_applier_reader`

`sql/rpl_applier_reader.cc` 里的 `Rpl_applier_reader` 是协调者的读侧。

- `open(const char **errmsg)`：先用 `rli->relay_log.find_log_pos(&m_linfo, rli->get_group_relay_log_name(), true)` 定位当前中继日志文件，失败报 `"Could not find relay log file."`；再 `m_relaylog_file_reader.open(m_linfo.log_file_name, rli->get_group_relay_log_pos(), &fdle)` 打开读端。在 `m_rli->data_lock` 保护下调 `rli->set_rli_description_event(fdle)` 设置格式描述事件；并把 `relay_log_purge == 0 && rli->log_space_limit > 0` 的情况处理为把 `rli->log_space_limit` 置 0 并报 `ER_RELAY_LOG_SPACE_LIMIT_DISABLED`。
- `read_next_event()`：当 `m_reading_active_log && m_relaylog_file_reader.position() >= m_log_end_pos`（已读到当前活跃文件末尾）时进入等待循环：先 `sql_slave_killed` 判 kill，再 `read_active_log_end_pos()` 看是否有新数据；没有则释放 `data_lock` 后在 `m_rli->is_time_for_mta_checkpoint()` 成立时调 `mta_checkpoint_routine(m_rli, false)`（失败报 `"Failed to synchronize worker threads"`）；然后用 `Stage_controller` 在 `m_rli->relay_log.get_binlog_end_pos_lock()` 上进入 `stage_replica_has_read_all_relay_log` 阶段，调 `wait_for_new_event()` 等待 IO 线程写入。读到事件后 `m_rli->set_event_start_pos(...)`、`ev = m_relaylog_file_reader.read_event_object()`，并设 `ev->future_event_relay_log_pos`。
- 读到 `Binlog_read_error::READ_EOF` 且不在活跃日志里时，若 `m_rli->is_receiver_waiting_for_rl_space.load() && !m_rli->is_in_group()`，则置 `force_purging = true`；并行模式下先调 `m_rli->current_mts_submode->wait_for_workers_to_finish(m_rli)`（返回 `-1` 时报 `"Failed to compute mta checkpoint"`）。随后调 `move_to_next_log(force_purging)`，成功则递归 `read_next_event()`。
- `move_to_next_log(bool force)`：关掉当前读端，`m_rli->relay_log.find_next_log(&m_linfo, true)` 找下一个文件（失败报 `"error switching to the next log"`），把 `event_relay_log_pos` 置 `BIN_LOG_HEADER_SIZE`；不在组内时若需要清当前日志就把 `group_relay_log_pos` 也推到新文件，然后调 `purge_applied_logs()`；在组内则置 `m_rli->force_flush_postponed_due_to_split_trans = true`。并行模式下还要 `m_rli->reset_notified_relay_log_change()`。
- `purge_applied_logs()`：先看 `relay_log_purge`，为 0 直接返回。用 `Shared_backup_lock_guard` 拿备份锁——拿不到时记 `ER_LOG_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK` 并返回假；内存不足（`oom`）时报 `ER_OUT_OF_RESOURCES_MSG` 并返回真。然后 `m_rli->flush_info(Relay_log_info::RLI_FLUSH_IGNORE_SYNC_OPT)`，再在 `m_rli->log_space_lock` 下调 `m_rli->relay_log.purge_logs(m_rli->get_group_relay_log_name(), false /* include */, false /*need_lock_index*/, false /*need_update_threads*/, &current_log_space, true)`，最后置 `m_rli->coordinator_log_after_purge` 并广播 `m_rli->log_space_cond`。

### 位点持久化：checkpoint 与 `update_pos`

位点推进分两层：

- **单线程模式**：`apply_event_and_update_pos` 里调 `ev->update_pos(rli)`。触发条件是 `*ptr_ev` 非空，且（事件不是 `XID_EVENT` 且 `!is_committed_ddl(*ptr_ev)`）或 `skip_event` 为真，或满足 MTS recovery 的那组条件。**`XID_EVENT` 不在这里更新位点**——源码注释说明：为了让从库崩溃安全，位点必须在处理 XID 事件时就更新，因此不需要在这里再更新一次。不满足条件时走 `else` 分支，只调 `rli->inc_event_relay_log_pos()`——**只推中继日志内的位置，不动源库位点**。更新失败会报 `ER_UNKNOWN_ERROR` 并返回 `SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR`。
- **并行模式**：位点由 `mta_checkpoint_routine(Relay_log_info *rli, bool force)` 统一推进。函数体调 `rli->gaq->move_queue_head(&rli->workers)` 取回已完成的任务数 `cnt`，`cnt == 0` 时直接返回；否则把 `rli->gaq->lwm.group_master_log_pos` / `group_relay_log_pos` 写进 `rli->set_group_master_log_pos` / `set_group_relay_log_pos`，再调 `rli->flush_info(Relay_log_info::RLI_FLUSH_IGNORE_SYNC_OPT)` 落盘，并广播 `rli->data_cond`。

### 提交顺序保持：`Commit_order_manager`

`sql/rpl_replica_commit_order_manager.cc` 里的 `Commit_order_manager` 保证并行 worker 的事务按源库顺序提交：

- `register_trx(Slave_worker *worker)`：把 worker 推进提交队列，状态从 `FINISHED` 转到 `REGISTERED`。
- `wait(Slave_worker *worker)`：仅当状态为 `REGISTERED` 时进入 `wait_on_graph(worker)`。
- `wait_on_graph(Slave_worker *worker)`：若自己不是队首（`this->m_workers.front() != worker->id`），先用 `worker->found_commit_order_deadlock()` 查死锁（命中则置回滚状态并返回真），再构造 `Commit_order_lock_graph ticket` 交给 `worker_thd->mdl_context.will_wait_for(&ticket)` 并 `find_deadlock()`，然后在 `LONG_TIMEOUT` 上等 `stage_worker_waiting_for_its_turn_to_commit`。返回的等待状态映射为：`GRANTED` 正常；`TIMEOUT` 报 `ER_LOCK_WAIT_TIMEOUT`；`KILLED` 时若 `worker_thd->is_killed() == ER_QUERY_TIMEOUT` 报 `ER_QUERY_TIMEOUT`，否则报 `ER_QUERY_INTERRUPTED`；`VICTIM` 报 `ER_LOCK_DEADLOCK`。任何失败都调 `worker->report_commit_order_deadlock()`、置回滚状态并返回真。
- 拿到提交权后若 `m_rollback_trx` 为真，说明前序事务已失败，本事务必须回滚：调 `finish_one(worker)` 并报 `ER_REPLICA_WORKER_STOPPED_PREVIOUS_THD_ERROR`。
- 正常路径上，若 `worker_thd->is_current_stmt_binlog_disabled()`，把 `worker_thd->durability_property = HA_IGNORE_DURABILITY`、`tx_commit_pending = true`、`next_to_commit = nullptr`——**先不立即刷盘，而是把所有 applier worker 攒成一组一起刷**。
- `flush_engine_and_signal_threads(Slave_worker *worker)`：用 `Commit_stage_manager::get_instance().enroll_for(Commit_stage_manager::COMMIT_ORDER_FLUSH_STAGE, worker->info_thd, nullptr, mysql_bin_log.get_log_lock())` 报名到刷盘阶段；作为 leader 时调 `ha_flush_logs(true)` 把事务成组刷到存储引擎，再调 `gtid_state->update_commit_group(first)` 把这组事务加进 `@@global.gtid_executed`，最后唤醒其余等待线程。
- worker 侧的挂接点在 `Slave_worker::slave_worker_ends_group`（`sql/rpl_rli_pdb.cc`）：成功分支调 `Commit_order_manager::wait_and_finish(info_thd, false)`；失败分支把 `running_status` 置 `ERROR_LEAVING`，调 `Commit_order_manager::wait_and_finish(info_thd, true)` 通知后续事务回滚，并调 `c_rli->info_thd->awake(THD::KILL_QUERY)` 杀掉协调者以暴露最终一致性问题。

### 停止路径：`stop_slave` 的两个重载

- `stop_slave(THD *thd)`：多通道入口。单通道时取 `channel_map.get_default_channel_mi()` 转调重载；多通道时遍历 `channel_map`，对每个 `Master_info::is_configured(mi)` 的通道调重载，单个失败记 `ER_REPL_REPLICA_CANT_STOP_REPLICA_FOR_CHANNEL` 并置 `error = 1`。无错时 `my_ok(thd)`。
- `stop_slave(THD *thd, Master_info *mi, bool net_report, bool for_one_channel, bool *push_temp_tables_warning)`：单通道实现。函数体先 `thd->set_skip_readonly_check()`，再做与启动路径相同的 `SUPER_ACL` / `REPLICATION_SLAVE_ADMIN` 权限校验（不满足报 `ER_SPECIFIC_ACCESS_DENIED_ERROR` 返回 1）。`mi->channel_wrlock()` 后 `lock_slave_threads(mi)`，调 `init_thread_mask(&thread_mask, mi, false /* not inverse*/)` 得到**正在运行**的线程掩码。若 `thd->lex->slave_thd_opt` 非零则 `thread_mask &= thd->lex->slave_thd_opt`；且当 `(thread_mask & SLAVE_IO) && mi->is_source_connection_auto_failover()` 时把 `SLAVE_MONITOR` 一并置入——**停 IO 线程时要连带停 IO 监控线程**。掩码非零时调 `terminate_slave_threads(mi, thread_mask, rpl_stop_replica_timeout, false /*need_lock_term=false*/)`；掩码为零（线程本来就没跑）时只推 `ER_REPLICA_CHANNEL_WAS_NOT_RUNNING` 警告。若 `mi->rli->atomic_channel_open_temp_tables` 且 `*push_temp_tables_warning` 为真，推一条 `ER_WARN_OPEN_TEMP_TABLES_MUST_BE_ZERO` 警告并把该标志置假（多通道时避免重复告警）。错误码若是 `ER_STOP_REPLICA_SQL_THREAD_TIMEOUT` / `ER_STOP_REPLICA_MONITOR_IO_THREAD_TIMEOUT` / `ER_STOP_REPLICA_IO_THREAD_TIMEOUT`，只推警告并记 `ER_RPL_REPLICA_SQL_THREAD_STOP_CMD_EXEC_TIMEOUT` 或 `ER_RPL_REPLICA_IO_THREAD_STOP_CMD_EXEC_TIMEOUT` 到错误日志，不向客户端报错。

`terminate_slave_threads(Master_info *mi, int thread_mask, ulong stop_wait_timeout, bool need_lock_term)` 的函数体顺序：SQL 分支先置 `mi->rli->abort_slave = true`，再调 `terminate_slave_thread(mi->rli->info_thd, sql_lock, &mi->rli->stop_cond, &mi->rli->slave_running, &total_stop_wait_timeout, need_lock_term)`，返回 1 映射为 `ER_STOP_REPLICA_SQL_THREAD_TIMEOUT`；成功后调 `mi->rli->flush_info(Relay_log_info::RLI_FLUSH_IGNORE_SYNC_OPT)`——**无论 `sync_relay_log_info` 选项如何都刷**，失败返回 `ER_ERROR_DURING_FLUSH_LOGS`。MONITOR 分支只在 `channel_map.get_number_of_connection_auto_failover_channels_running() == 1`（这是唯一在跑的故障转移通道）时才终止监控进程。IO 分支置 `mi->abort_slave = true`，并特殊处理磁盘空间不足：若 `mi->slave_running && mi->info_thd->is_waiting_for_disk_space()`，则在服务器关闭（`thread_mask & SLAVE_FORCE_ALL`）时用 `force_io_stop` 强制杀掉 IO 线程，否则记 `ER_STOP_REPLICA_IO_THREAD_DISK_SPACE` 警告让用户决定。**超时上限来自 `rpl_stop_replica_timeout`**（`sql/mysqld.cc` 里定义为 `ulong rpl_stop_replica_timeout = LONG_TIMEOUT`），且源码注释写明：这是**两个线程合计**的关闭等待时间，不是每个线程各等这么久——`total_stop_wait_timeout` 是两个分支共用的同一个变量。

### 复制服务接口：`channel_start`

`sql/rpl_channel_service_interface.cc` 里的 `channel_start` 是服务接口侧的启动入口，函数体：

1. 在 `LOCK_sql_replica_skip_counter` 保护下检查 `sql_replica_skip_counter > 0`——**服务通道不允许使用跳过计数器**，命中则返回 `RPL_CHANNEL_SERVICE_SLAVE_SKIP_COUNTER_ACTIVE`。
2. 取 `channel_map` 写锁，`Master_info *mi = channel_map.get_mi(channel)`；为空则返回 `RPL_CHANNEL_SERVICE_CHANNEL_DOES_NOT_EXISTS_ERROR`。
3. 把服务接口的线程标志翻译成内部的线程掩码：`threads_to_start & CHANNEL_APPLIER_THREAD` → `SLAVE_SQL`；`threads_to_start & CHANNEL_RECEIVER_THREAD` → `SLAVE_IO`。掩码为零则直接返回（什么都不用做）。
4. 从 `Rpl_channel_credentials::get_instance().get_credentials(channel, user, pass, auth)` 取该通道的凭据，填进 `LEX_SLAVE_CONNECTION lex_connection`。
5. 翻译 UNTIL 条件：`CHANNEL_UNTIL_APPLIER_AFTER_GTIDS` → `UNTIL_SQL_AFTER_GTIDS`；`CHANNEL_UNTIL_APPLIER_BEFORE_GTIDS` → `UNTIL_SQL_BEFORE_GTIDS`；`CHANNEL_UNTIL_APPLIER_AFTER_GAPS` → `lex_mi.until_after_gaps = true`；`CHANNEL_UNTIL_VIEW_ID` → `lex_mi.view_id`。
6. 若 `wait_for_connection && (thread_mask & SLAVE_IO)`，先记下 `mi->slave_run_id`。
7. 若当前没有 `THD`，用 `create_surrogate_thread()` 造一个代理线程。
8. **调 `start_slave(thd, &lex_connection, &lex_mi, thread_mask, mi, use_server_mta_configuration)`**——与 SQL 命令走同一个单通道 `start_slave`。
9. 若要求等待连接且启动成功，在 `mi->run_lock` 上等 `mi->slave_run_id` 变化（`mysql_cond_wait(&mi->start_cond, &mi->run_lock)`），然后循环等 `mi->slave_running` 变成 `MYSQL_SLAVE_RUN_CONNECT`；若中途变成 `MYSQL_SLAVE_NOT_RUN`，返回 `RPL_CHANNEL_SERVICE_RECEIVER_CONNECTION_ERROR`。
10. 收尾：释放 `channel_map` 锁，若自建了代理线程则 `delete_surrogate_thread(thd)`。

### 并行配置冲突检查

`check_slave_sql_config_conflict(const Relay_log_info *rli)`（`sql/rpl_replica.cc`）检查两组冲突：

- 若 `opt_replica_preserve_commit_order && replica_parallel_workers > 0` 且 `channel_mts_submode == MTS_PARALLEL_TYPE_DB_NAME`，报 `ER_DONT_SUPPORT_REPLICA_PRESERVE_COMMIT_ORDER`——**提交顺序保持与按库分片互斥**。
- 若 `rli` 非空且 `replica_parallel_workers > 0`，且（子模式不是 `MTS_PARALLEL_TYPE_LOGICAL_CLOCK`，或虽是逻辑时钟但 `!opt_replica_preserve_commit_order`），且该通道是 group replication 的 applier 通道，则报 `ER_REPLICA_CHANNEL_OPERATION_NOT_ALLOWED`。

## 关键取舍

### 单通道重载复用：一条路径，两种入口

`START REPLICA` 命令走 `start_slave(THD *)`，复制服务接口走 `channel_start` → 同一个单通道 `start_slave`。两条入口的参数准备不同（前者用 `thd->lex->slave_connection` / `thd->lex->mi`，后者用 `LEX_SLAVE_CONNECTION` / `LEX_MASTER_INFO` 加 `Rpl_channel_credentials`），但权限校验、跳过计数处理、MTS 参数固化、until 解析、线程拉起完全共用。

**取舍：权限与参数固化只有一份实现，不会出现「命令路径能起、服务接口起不来」的不一致；代价是服务接口必须自己把 `CHANNEL_APPLIER_THREAD` 之类的服务语义翻译成 `SLAVE_SQL` 掩码。**

### 提交顺序保持与并行分片互斥

`check_slave_sql_config_conflict` 明确禁止 `opt_replica_preserve_commit_order` 与 `MTS_PARALLEL_TYPE_DB_NAME` 同时生效；而 `handle_slave_sql` 里建 `Commit_order_manager` 的条件是 `opt_replica_preserve_commit_order && !rli->is_parallel_exec() && rli->opt_replica_parallel_workers > 1`——**worker 数不大于 1 就不建管理器**。

**取舍：牺牲一部分并行度换取与源库一致的提交顺序。** 单 worker 本身顺序提交，不需要额外机制；按库分片的并行度在库之间天然冲突，源码选择直接报错而不是静默降级。

### GTID-only 模式下把 worker 数抬到 1

`start_slave` 单通道重载里，若 `mi->is_gtid_only_mode() && opt_mts_replica_parallel_workers == 0`，把 `mi->rli->opt_replica_parallel_workers` 置为 1。

**取舍：GTID-only 模式不允许无 worker 运行，哪怕用户显式配了 0 个 worker。** 这是在启动期一次性改写参数，而不是在运行期拒绝。

### 跳过计数器不跨通道生效

多通道 `start_slave(THD *)` 里，若 `sql_replica_skip_counter > 0` 且命令没有带 `SLAVE_IO`，报 `ER_REPLICA_CHANNEL_SQL_SKIP_COUNTER` 直接失败——**同一时刻只允许一个通道的应用线程带跳过计数启动**。`channel_start` 更严格：`sql_replica_skip_counter > 0` 一律返回 `RPL_CHANNEL_SERVICE_SLAVE_SKIP_COUNTER_ACTIVE`，服务通道根本不允许用跳过计数。

单通道重载里把全局 `sql_replica_skip_counter` 拷进 `mi->rli->slave_skip_counter` 后立即把全局值清零——**跳过计数只对第一个被启动的应用线程生效**。

**取舍：跳过事件是运维手段，跨通道叠加会让「跳过了几条」无法解释，因此限制为单通道单次。**

### 停止超时不按线程倍增

`terminate_slave_threads` 里 `total_stop_wait_timeout` 是 SQL 与 IO 两个分支共用的同一个变量，源码注释写明这是为了让用户配的超时值表示「两个线程的关闭总耗时」。

**取舍：用户配的 `rpl_stop_replica_timeout`（默认 `LONG_TIMEOUT`）是总量预算而非每线程预算；代价是其中一个线程耗尽预算时另一个线程也要在剩余额度内收尾。**

### 停止 IO 线程时连带停止 IO 监控线程

`stop_slave` 单通道重载里，当 `thread_mask & SLAVE_IO` 且 `mi->is_source_connection_auto_failover()` 时把 `SLAVE_MONITOR` 加入掩码。

**取舍：自动故障转移的监控线程依附于 IO 线程的生命周期；留下监控线程会让它继续尝试重连已停止的通道。**

### 提交顺序保持用「成组刷盘」而非逐事务刷盘

`Commit_order_manager::wait` 在拿到提交权后把 `worker_thd->durability_property = HA_IGNORE_DURABILITY`、`tx_commit_pending = true`，把实际刷盘推迟到 `flush_engine_and_signal_threads` 里由 leader 调 `ha_flush_logs(true)` 成组完成。

**取舍：用「延迟到 leader 统一刷」换取按序提交，代价是事务在刷盘前处于未持久化状态。** 源码注释明确指出这会打断 InnoDB 的组提交——注释原文是「It will break innodb group commit, but it should rarely happen.」

### 死锁用 MDL 的等待图检测

`wait_on_graph` 不是自己实现等待图，而是复用 MDL 的机制：构造 `Commit_order_lock_graph ticket` 交给 `worker_thd->mdl_context.will_wait_for(&ticket)` 并调 `mdl_context.find_deadlock()`。

**取舍：复用 MDL 的死锁检测，代价是这个机制只能在 `mdl_context` 可用的线程上下文中工作；命中死锁时 worker 调 `report_commit_order_deadlock()` 上报并回滚。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_replica.cc | `start_slave` | 多通道启动入口：单通道时转调单通道重载；多通道时检查 `sql_replica_skip_counter` 与 `SLAVE_IO` 的组合，遍历 `channel_map` 逐通道启动，失败只记错误日志不中断 |
| sql/rpl_replica.cc | `stop_slave` | 多通道停止入口：单通道时转调单通道重载；多通道时遍历 `channel_map` 对 `Master_info::is_configured(mi)` 的通道逐个停止 |
| sql/rpl_replica.cc | `handle_slave_sql` | 应用线程（协调者）入口：建 THD 与 MTS 子模式、按条件建 `Commit_order_manager`、启动 worker 池、读中继日志并派发事务组、退出时停 worker 并清理上下文 |
| sql/rpl_replica.cc | `start_slave_threads` | 线程拉起编排：校验元数据仓已初始化、按 `SLAVE_IO` / `SLAVE_SQL` 分别调 `start_slave_thread`、MTS 恢复间隙收集、启动失败时回退到 `terminate_slave_threads` |
| sql/rpl_replica.cc | `terminate_slave_threads` | 线程终止编排：按 SQL / MONITOR / IO 三段分别终止，SQL 段后无条件 `flush_info`，超时上限取 `rpl_stop_replica_timeout`（两线程共用的总量预算） |
| sql/rpl_replica.cc | `exec_relay_log_event` | 单事件执行入口：拿 `data_lock`、做 MTS checkpoint、判 kill 与 until、处理 GTID 重连后的 FD 回滚、调 `apply_event_and_update_pos` 后处理销毁与瞬时错误重试 |
| sql/rpl_replica.cc | `apply_event_and_update_pos` | 应用单个事件并更新位点：做 `shall_skip` 判定、调 `ev->apply_event(rli)`、并行模式下把事件派给 worker、按条件调 `ev->update_pos(rli)` 或 `rli->inc_event_relay_log_pos()` |
| sql/rpl_replica.cc | `mta_checkpoint_routine` | 并行模式的位点推进：`gaq->move_queue_head` 取已完成任务数，把 `gaq->lwm` 的位点写回 `rli`，再 `flush_info` 落盘 |
| sql/rpl_replica.cc | `slave_start_workers` | worker 池启动：按 `max(n, rli->recovery_parallel_workers)` 决定实际 worker 数，建 GAQ（容量 `checkpoint_group`）、初始化分片哈希、逐个 `slave_start_single_worker` |
| sql/rpl_replica.cc | `slave_stop_workers` | worker 池停止：通知 worker 置 `STOP_ACCEPTED` 并等其确认，再由协调者收尾 MTS 状态 |
| sql/rpl_replica.cc | `check_slave_sql_config_conflict` | 并行配置冲突检查：提交顺序保持与按库分片互斥、group replication applier 通道的并行度约束 |
| sql/rpl_replica.cc | `sql_slave_killed` | 线程终止判定：检查 `sql_thread_kill_accepted`、`connection_events_loop_aborted()`、`thd->killed`、`rli->abort_slave`，并对组内非事务性改动给出数据不一致警告文案 |
| sql/rpl_replica.cc | `Rpl_applier_reader` 相关调用 | 协调者读中继日志的入口：`applier_reader.open` 定位并打开日志文件、`read_next_event` 取事件、`close` 收尾 |
| sql/rpl_applier_reader.cc | `Rpl_applier_reader::open` | 打开中继日志读端：`find_log_pos` 定位文件、`relaylog_file_reader.open` 打开读端、在 `data_lock` 下设置格式描述事件 |
| sql/rpl_applier_reader.cc | `Rpl_applier_reader::read_next_event` | 读一个事件：读到活跃文件末尾时在 `LOCK_binlog_end_pos` 上等 IO 线程写入，等待期间按 checkpoint 周期调 `mta_checkpoint_routine`；遇 `READ_EOF` 时切下一个日志文件 |
| sql/rpl_applier_reader.cc | `Rpl_applier_reader::move_to_next_log` | 切换到下一个中继日志文件：关当前读端、`find_next_log` 定位、置事件位点为 `BIN_LOG_HEADER_SIZE`、必要时清已应用日志 |
| sql/rpl_applier_reader.cc | `Rpl_applier_reader::purge_applied_logs` | 清理已应用的中继日志：先取备份锁（拿不到则跳过并告警），`flush_info` 后调 `relay_log.purge_logs` |
| sql/rpl_mta_submode.cc | `Mts_submode_logical_clock::wait_for_workers_to_finish` | 逻辑时钟子模式的同步点：等待 `delegated_jobs > jobs_done`，循环内调 `mta_checkpoint_routine(rli, true)`，检查各 worker 的 `running_status` |
| sql/rpl_mta_submode.cc | `Mts_submode_database::wait_for_workers_to_finish` | 按库分片子模式的同步点：遍历 `mapping_db_to_worker`，等各分片的 `usage` 归零并迁移临时表资源 |
| sql/rpl_mta_submode.cc | `Mts_submode_database::set_multi_threaded_applier_context` | 为事务负载事件计算分片信息：解析内部事件涉及的库，命中 `OVER_MAX_DBS_IN_EVENT_MTS` 时标记为隔离执行 |
| sql/rpl_rli_pdb.cc | `slave_worker_exec_job_group` | worker 线程的执行循环：取任务项、执行 `slave_worker_exec_event`、出错时 `retry_transaction` 重试、组结束调 `slave_worker_ends_group` |
| sql/rpl_rli_pdb.cc | `Slave_worker::slave_worker_ends_group` | worker 组结束处理：成功时 `Commit_order_manager::wait_and_finish(info_thd, false)`；失败时置 `ERROR_LEAVING`、`wait_and_finish(info_thd, true)` 并 `awake(THD::KILL_QUERY)` 杀掉协调者 |
| sql/rpl_replica_commit_order_manager.cc | `Commit_order_manager::register_trx` | worker 事务登记进提交队列，状态由 `FINISHED` 转 `REGISTERED` |
| sql/rpl_replica_commit_order_manager.cc | `Commit_order_manager::wait_on_graph` | 等待提交权：非队首时用 MDL 的 `will_wait_for` + `find_deadlock` 检测死锁，按 `GRANTED` / `TIMEOUT` / `KILLED` / `VICTIM` 映射错误码 |
| sql/rpl_replica_commit_order_manager.cc | `Commit_order_manager::wait` | 提交权等待的外层逻辑：前序事务失败时置回滚并报 `ER_REPLICA_WORKER_STOPPED_PREVIOUS_THD_ERROR`；正常时置 `HA_IGNORE_DURABILITY` 延迟刷盘 |
| sql/rpl_replica_commit_order_manager.cc | `Commit_order_manager::flush_engine_and_signal_threads` | leader 成组刷盘：`enroll_for` 报名刷盘阶段后调 `ha_flush_logs(true)`，再 `gtid_state->update_commit_group(first)` 批量更新 `gtid_executed`，最后唤醒其余线程 |
| sql/rpl_channel_service_interface.cc | `channel_start` | 复制服务接口的通道启动：拒绝服务通道使用跳过计数、把 `CHANNEL_APPLIER_THREAD` / `CHANNEL_RECEIVER_THREAD` 翻译成 `SLAVE_SQL` / `SLAVE_IO`、取通道凭据、翻译 UNTIL 条件，然后调同一个单通道 `start_slave`，并在需要时等 IO 线程连上 |
| sql/mysqld.cc | `rpl_stop_replica_timeout` | 停止复制线程的等待上限，定义为 `ulong rpl_stop_replica_timeout = LONG_TIMEOUT` |
| sql/mysqld.cc | `opt_mts_replica_parallel_workers` / `opt_replica_preserve_commit_order` / `mts_parallel_option` / `opt_mta_checkpoint_group` | 并行应用的系统变量：worker 数、是否保持提交顺序、并行子模式、checkpoint 组长；启动时由 `start_slave` 固化到 `rli` 上 |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 WAL 层，与本页的复制应用侧没有代码级关联，但机制形态上可对照之处：

- **两者都是「先写日志、再推进可见位点」的顺序纪律。** 本页的对应点是 `apply_event_and_update_pos` 里的 `ev->update_pos(rli)`——位点更新与数据变更分离，`XID_EVENT` 的位点在处理时就更新以保证崩溃安全。**架构推论：** 主核 WAL 侧是否存在同等严格的位点推进顺序，本页不断言。
- **两者都有「日志空间背压」这一层。** 本页是 `purge_applied_logs` 的 `Shared_backup_lock_guard` + `relay_log.purge_logs` 组合——先确认没有备份锁占用，再清理已应用日志。**架构推论：** 这条纪律的核心是「存储引擎要刷盘的地方与清理日志的地方必须互斥」，主核 WAL 的刷盘策略是否采用同一条纪律，本页不断言。
- **两者都把「并行提交的顺序」当作显式问题。** 本页用 `Commit_order_manager` 把顺序问题从 SQL 层下沉成独立的等待图机制。**架构推论：** 主核若要支持并行提交，同样的问题会重新出现一次。

## 参考

- [Replication](https://dev.mysql.com/doc/refman/8.0/en/replication.html)（MySQL 8.0 Reference Manual）——复制总体模型：Source / Replica 角色、复制线程的职责划分、基于位点与基于 GTID 的两种复制方式。
- [Replication Threads](https://dev.mysql.com/doc/refman/8.0/en/replication-threads.html)（MySQL 8.0 Reference Manual）——IO 线程与 SQL 线程职责分离的官方说明，是本页「协调者 + worker」分工的上位背景。
- [Replica Parallel Workers](https://dev.mysql.com/doc/refman/8.0/en/replication-options.html#sysvar_replica_parallel_workers)（MySQL 8.0 Reference Manual）——`replica_parallel_workers`、`replica_parallel_type`、`replica_preserve_commit_order` 三个变量的官方语义与约束。
- [sql/rpl_replica.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_replica.cc)（MySQL Server 8.0 源码，GitHub）——`start_slave` / `stop_slave` / `handle_slave_sql` / `start_slave_threads` / `terminate_slave_threads` / `exec_relay_log_event` / `apply_event_and_update_pos` / `mta_checkpoint_routine` 的权威来源。
- [sql/rpl_applier_reader.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_applier_reader.cc)（MySQL Server 8.0 源码，GitHub）——`Rpl_applier_reader` 读中继日志、切文件、清理已应用日志的实现。
- [sql/rpl_mta_submode.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_mta_submode.cc)（MySQL Server 8.0 源码，GitHub）——两种 MTS 子模式的同步点与分片计算。
- [sql/rpl_replica_commit_order_manager.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_replica_commit_order_manager.cc)（MySQL Server 8.0 源码，GitHub）——`Commit_order_manager` 的提交顺序保持、MDL 等待图死锁检测与成组刷盘。
- [sql/rpl_channel_service_interface.cc](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_channel_service_interface.cc)（MySQL Server 8.0 源码，GitHub）——`channel_start` 复用 `start_slave` 的服务接口路径。
