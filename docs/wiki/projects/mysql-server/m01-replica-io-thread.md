---
project: mysql-server
feature: replica-io-thread
page_type: feature
schema_version: 1
refs:
  - path: sql/rpl_replica.cc
    symbols: [exec_relay_log_event, apply_event_and_update_pos]
---

# 从库 IO 线程

## 一句话职责

`sql/rpl_replica.cc` 里的 `handle_slave_io` 是从库 IO 线程的入口：它建立到源库的连接、发起 binlog dump 请求、逐个事件读取源库发来的字节流，再通过 `queue_event` 把事件写进本机中继日志（relay log）并推进源库位点；与之配套的 `exec_relay_log_event` / `apply_event_and_update_pos` 构成中继日志的事件执行路径。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责从库连接源库拉取 binlog 并写入中继日志；**不负责事件的应用（M02）**、**不负责 GTID 的集合运算（M03）**。

## 机制是什么

从库复制在源码里被拆成两个线程，两者共用同一个 `Master_info` / `Relay_log_info` 结构：

- **IO 线程**（`handle_slave_io`）：只做「取」，把源库 binlog 的字节流搬进中继日志文件。它不解析事件语义、不改数据。
- **应用线程**（`handle_slave_sql`）：只做「用」，从中继日志读出事件并执行。

两者的启动点在 `start_slave_threads`：函数先按 `thread_mask & SLAVE_IO` 调 `start_slave_thread(key_thread_replica_io, handle_slave_io, ...)`，再按 `thread_mask & SLAVE_SQL` 调 `start_slave_thread(key_thread_replica_sql, handle_slave_sql, ...)`。

### IO 线程的状态机

`handle_slave_io` 的函数体是一条以标签 `connect_init` / `connected` 为骨架的循环：

1. `connect_init` 标签处先 `mysql_init` 建立 `MYSQL` 句柄，再调 `safe_connect` 连到源库；失败直接 `goto err`。
2. `connected` 标签处依次做三件事：`get_master_version_and_clock` 取源库版本与时钟、`get_master_uuid` 取源库 UUID、`io_thread_init_commands` 执行初始化命令。三者任一失败，`ret` 非 0。
3. 若 `mi->is_source_connection_auto_failover()` 为真，还要调 `Async_conn_failover_manager::get_source_quorum_status` 判定 quorum 状态：`fatal_error` 与 `no_quorum_error` 映射为 `ret = 1`（致命，走 `goto err`），`transient_network_error` 映射为 `ret = 2`（瞬时，走重连）。
4. `register_slave_on_master` 在源库上注册本从库。
5. 进入 `while (!io_slave_killed(thd, mi))` 主循环：先 `request_dump` 发起 dump 请求，再进内层 `while (!io_slave_killed(thd, mi))` 逐事件 `read_event`。

`safe_connect` / `safe_reconnect` / `get_master_version_and_clock` / `register_slave_on_master` / `request_dump` / `read_event` / `queue_event` / `heartbeat_queue_event` / `write_rotate_to_master_pos_into_relay_log` / `write_ignored_events_info_to_relay_log` 全部定义在同一个文件 `sql/rpl_replica.cc` 内。

### 事件进中继日志的路径

`queue_event(Master_info *mi, const char *buf, ulong event_len, bool do_flush_mi)` 是「把一个源库事件写进中继日志」的唯一入口，它的返回值是三态枚举 `QUEUE_EVENT_RESULT`：`QUEUE_EVENT_OK`、`QUEUE_EVENT_ERROR_QUEUING`、`QUEUE_EVENT_ERROR_FLUSHING_INFO`。

函数体顺序：

1. 算校验和算法 `checksum_alg`：优先取 `mi->checksum_alg_before_fd`，若它是 `BINLOG_CHECKSUM_ALG_UNDEF` 才回退到 `rli->relay_log.relay_log_checksum_alg`。断言该值只能是 `BINLOG_CHECKSUM_ALG_OFF`、`BINLOG_CHECKSUM_ALG_UNDEF` 或 `BINLOG_CHECKSUM_ALG_CRC32` 三者之一。
2. 若事件类型是 `binary_log::FORMAT_DESCRIPTION_EVENT`，用 `Log_event_footer::get_checksum_alg(buf, event_len)` 单独取出 FD 自带的算法描述符。
3. 用 `Log_event_footer::event_checksum_test` 校验事件校验和；不通过则报 `ER_NETWORK_READ_EVENT_CHECKSUM_FAILURE` 并跳到 `err`（返回 `QUEUE_EVENT_ERROR_QUEUING`）。
4. `mysql_mutex_lock(log_lock)` 拿住中继日志锁——源码注释写明「从此直到事件入队结束，不允许其他线程写中继日志或轮转它」。
5. 若 `mi->get_mi_description_event()` 为空，报 `ER_REPLICA_QUEUE_EVENT_FAILED_INVALID_CONFIGURATION` 并跳到 `err`。
6. 按事件类型分派 `inc_pos`（本次事件让源库位点前进多少），其中若干类型有专门处理：
   - `FORMAT_DESCRIPTION_EVENT`：反序列化出新的 `Format_description_log_event`，装进 `mi->set_mi_description_event`，并把 `relay_log.relay_log_checksum_alg` 改成新 FD 的算法。`inc_pos = uint4korr(buf + LOG_POS_OFFSET) ? event_len : 0`——只有源库明确请求了这个事件才推进位点。
   - `HEARTBEAT_LOG_EVENT` / `HEARTBEAT_LOG_EVENT_V2`：交给 `heartbeat_queue_event` 处理后直接 `goto end`（不写中继日志）。
   - `PREVIOUS_GTIDS_LOG_EVENT`：不写事件本体，改为调 `write_rotate_to_master_pos_into_relay_log` 写一个 Rotate 事件，并把 `do_flush_mi` 置假（因为 `write_rotate_to_master_pos_into_relay_log` 已经刷过 master info）。
   - `GTID_LOG_EVENT`：若 `global_gtid_mode.get() == Gtid_mode::OFF` 报 `ER_CANT_REPLICATE_GTID_WITH_GTID_MODE_OFF`；否则解析出 `gtid.sidno` / `gtid.gno` 与两个提交时间戳。
   - `ANONYMOUS_GTID_LOG_EVENT`：若 `mi->is_auto_position()` 报 `ER_CANT_REPLICATE_ANONYMOUS_WITH_AUTO_POSITION`。
   - `TRANSACTION_PAYLOAD_EVENT`：解析压缩类型与压缩前后字节数，更新 GTID 监控信息。
7. **服务器 ID 过滤**：取 `s_id = uint4korr(buf + SERVER_ID_OFFSET)`，再 `s_id &= opt_server_id_mask` 掩掉无关位。若 `s_id == ::server_id && !mi->rli->replicate_same_server_id`，或命中 `mi->ignore_server_ids` 的动态忽略列表（且该事件不是源的 `FORMAT_DESCRIPTION_EVENT` / `ROTATE_EVENT`），则**不写中继日志**，但仍推进 `mi->set_master_log_pos` 并记录 `rli->ign_master_log_name_end` / `rli->ign_master_log_pos_end`。
8. 否则走 `rli->relay_log.write_buffer(buf, event_len, mi)` 把事件字节写进中继日志文件；写成功后在 `mi->data_lock` 保护下 `mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos)`。**写失败时调 `mi->transaction_parser.rollback()`** 回滚事务边界解析器状态，避免重启 IO 线程时把失败事件当作已入队。
9. 若是 `GTID_LOG_EVENT` 或 `ANONYMOUS_GTID_LOG_EVENT`，调 `mi->started_queueing(gtid, original_commit_timestamp, immediate_commit_timestamp)` 标记事务开始入队。
10. `end:` 标签处，若 `res == QUEUE_EVENT_OK && do_flush_mi`，则调 `flush_master_info(mi, false, lock_count == 0, false, mi->is_gtid_only_mode())` 刷连接元数据；失败时把 `res` 改成 `QUEUE_EVENT_ERROR_FLUSHING_INFO`。

### 中继日志空间的背压

IO 线程写中继日志前有一道空间检查。`exceeds_relay_log_limit(Relay_log_info *rli, std::size_t queued_size)` 的判定式是：

```
return (rli->log_space_limit != 0 &&
        rli->log_space_limit < rli->log_space_total + queued_size);
```

即 `log_space_limit` 为 0 表示不限。调用点在 `handle_slave_io` 的事件循环里，条件是 `rli->log_space_limit && exceeds_relay_log_limit(rli, queued_size) && !mi->transaction_parser.is_inside_transaction()`——**事务内不允许等待**，避免把事务截断在中间。

等待由 `wait_for_relay_log_space(Relay_log_info *rli, std::size_t queued_size)` 完成：它先置 `rli->is_receiver_waiting_for_rl_space` 为真，再调 `rotate_relay_log(mi, true, true, true)` 主动轮转一次中继日志，然后在 `rli->log_space_lock` 上等 `rli->log_space_cond`，直到空间够了、被 kill、或协调者清理过日志。

`queued_size` 不是简单的事件长度：若事件是 GTID 类事件，IO 线程会构造 `Gtid_log_event gtid_ev(event_buf, mi->get_mi_description_event())` 并用 `gtid_ev.get_trx_length()` 作为 `queued_size`——**按整个事务的长度做空间判断，而不是按单个事件**。

### 重连策略

`try_to_reconnect(THD *thd, MYSQL *mysql, Master_info *mi, uint *retry_count, bool suppress_warnings, const Reconnect_messages &messages)` 是唯一的重连入口，函数注释写明其语义：「终止当前到主库的连接，睡眠 `mi->connect_retry` 毫秒后用 `safe_reconnect()` 发起新连接。`retry_count` 指向的变量会自增——若它超过 `mi->retry_count` 则不再重建连接并返回错误。」

函数体的关键判断：

```
if ((*retry_count)++) {
  if (*retry_count > mi->retry_count) return 1;  // Don't retry forever
  slave_sleep(thd, mi->connect_retry, io_slave_killed, mi);
}
```

也就是说**重试次数上限是 `mi->retry_count` 这个配置项**，等待间隔是 `mi->connect_retry` 毫秒。第一次进入（`*retry_count` 为 0 时）不自增、不 sleep，直接重连。

`try_to_reconnect` 的调用方在 `handle_slave_io` 里有四处，分别对应四类错误：注册失败、`request_dump` 失败、`read_event` 失败、以及源库版本/UUID 探测阶段的瞬时错误（`ret == 2`）。四处的 `goto connected` 都是回到状态机的 `connected` 标签重新走一遍，而不是从头 `connect_init`。

`read_event` 返回 `packet_error` 时，`handle_slave_io` 按 `mysql_errno(mysql)` 分三种致命错误直接 `goto err`： `CR_NET_PACKET_TOO_LARGE`（报 `ER_RPL_LOG_ENTRY_EXCEEDS_REPLICA_MAX_ALLOWED_PACKET`）、`ER_SOURCE_FATAL_ERROR_READING_BINLOG`、`ER_OUT_OF_RESOURCES`（报 `ER_RPL_REPLICA_STOPPING_AS_SOURCE_OOM`）。其余错误走 `try_to_reconnect`。

### 中继日志事件的执行路径

`exec_relay_log_event(THD *thd, Relay_log_info *rli, Rpl_applier_reader *applier_reader, Log_event *in)` 是「执行一个中继日志事件」的高层函数，函数体顺序：

1. `mysql_mutex_lock(&rli->data_lock)` 拿住 `rli->data_lock`。
2. 若 `rli->rli_checkpoint_seqno >= rli->checkpoint_group`（需要强制 checkpoint）或 `rli->is_time_for_mta_checkpoint()`，则释放 `data_lock`、调 `mta_checkpoint_routine(rli, force)`；checkpoint 失败则 `delete ev` 并返回 1。
3. `sql_slave_killed(thd, rli)` 为真时释放锁、删事件、报 `ER_RPL_REPLICA_ERROR_READING_RELAY_LOG_EVENTS` 并返回 1。
4. 更新 `rli->last_master_timestamp`：仅当 `(!rli->is_parallel_exec() || rli->last_master_timestamp == 0)` 且事件不是 artificial event、不是 relay log event、不是 `FORMAT_DESCRIPTION_EVENT`、`ev->server_id != 0` 时才更新为 `ev->common_header->when.tv_sec + (time_t)ev->exec_time`。
5. `rli->is_until_satisfied_before_dispatching_event(ev)` 为真时置 `rli->abort_slave = true`，返回 `SLAVE_APPLY_EVENT_UNTIL_REACHED`。
6. 若事件是来自其他源库的 `FORMAT_DESCRIPTION_EVENT`（`ev->server_id != ::server_id && ev->common_header->log_pos != 0`）且并行执行中且已见过 GTID（`rli->curr_group_seen_gtid`），先调 `coord_handle_partial_binlogged_transaction(rli, ev)` 让 worker 回滚当前不完整分组。
7. 调 `apply_event_and_update_pos(ptr_ev, thd, rli)`，返回值是枚举 `enum_slave_apply_event_and_update_pos_retval`。
8. 事件销毁有条件：若 `*ptr_ev` 仍非空（说明事件没被交给 MTS worker），且事件类型既不是 `FORMAT_DESCRIPTION_EVENT` 也不是 `ROWS_QUERY_LOG_EVENT`，则 `delete ev`。
9. `exec_res >= SLAVE_APPLY_EVENT_AND_UPDATE_POS_UPDATE_POS_ERROR` 时直接返回 1。
10. **瞬时错误重试**：若 `slave_trans_retries` 为真，且 `exec_res` 非零、不是 MTS worker、`rli->has_temporary_error(thd, 0, &silent)` 为真、且事务可安全回滚，则当 `rli->trans_retries < slave_trans_retries` 时执行重试流程：`rli->cleanup_context(thd, true)` 回滚上下文、`load_mi_and_rli_from_repositories` 重载、`applier_reader->close()` 后 `applier_reader->open(&errmsg)` 重开，然后 `slave_sleep(thd, min<ulong>(rli->trans_retries, MAX_SLAVE_RETRY_PAUSE), sql_slave_killed, rli)` 等待，再 `rli->trans_retries++` 与 `rli->retried_trans++`。**重试上限是系统变量 `slave_trans_retries`，单次等待上限是常量 `MAX_SLAVE_RETRY_PAUSE`**。若已超过上限则 `thd->fatal_error()` 并报「Replica SQL thread retried transaction N time(s) in vain, giving up. Consider raising the value of the replica_transaction_retries variable.」

### `apply_event_and_update_pos` 的内部

`apply_event_and_update_pos(Log_event **ptr_ev, THD *thd, Relay_log_info *rli)` 是真正「应用一个事件并更新位点」的函数，它的几个关键分支：

- 先把事件属性搬到线程上下文：`thd->server_id = ev->server_id`、`thd->unmasked_server_id = ev->common_header->unmasked_server_id`、`thd->set_time()`、`ev->thd = thd`。
- 跳过判定：`reason = ev->shall_skip(rli)`，但**在 MTS recovery 场景下跳过该判定**——条件是 `!(rli->is_mts_recovery() && bitmap_is_set(&rli->recovery_groups, rli->mts_recovery_index))`。
- `reason == Log_event::EVENT_SKIP_COUNT` 时 `--rli->slave_skip_counter` 并置 `skip_event = true`。
- `reason == Log_event::EVENT_SKIP_NOT` 时才真正执行：`sql_delay_event(ev, thd, rli)`（处理 `sql_delay_ms`）、`rli->set_group_source_log_start_end_pos(ev)`、然后 `exec_res = ev->apply_event(rli)`。
- **并行执行时事件不在本线程落地**：`!exec_res && (ev->worker != rli)` 且 `ev->worker` 非空时，构造 `Slave_job_item` 交给 `append_item_to_jobs` 投递给 worker，并把 `*ptr_ev = nullptr` 标记「事件已交给 worker」。
- 位置更新：只有 `*ptr_ev` 非空、且事件不是 `XID_EVENT`、且 `!is_committed_ddl(*ptr_ev)`，或 `skip_event` 为真，或处于 MTS recovery 的特定条件下，才调 `ev->update_pos(rli)`。**XID 事件不更新位置**——源码注释说明「为了使从库具备崩溃安全性，位置必须在处理 XID 事件时就更新，因此这里不需要再更新」。
- 跳过事件时调 `thd->mem_root->ClearForReuse()` 释放内存——源码注释解释：若不清，`slave_skip_counter` 很大时持续跳过并更新位点，整个过程占用的内存会一直不释放。

## 怎么实现的

### 一次完整的 IO 线程事件往返

**第一步：发起 dump 请求。** `request_dump` 根据 `mi->is_auto_position()` 选命令：`COM_BINLOG_DUMP_GTID` 或 `COM_BINLOG_DUMP`。标志位固定是 `binlog_flags = 0` 再 `|= USE_HEARTBEAT_EVENT_V2`——源码注释解释 `binlog_flags` 保持为 0 是为兼容早期 5.6 RC 的行为，最低三位不得使用。GTID 模式下还要把本从库的 `rli->get_gtid_set()` 与 `gtid_state->get_executed_gtids()` 合并进 `gtid_executed` 一起发给源库。

**第二步：读事件字节。** `read_event` 调 `mysql_binlog_fetch(mysql, rpl)`。返回值是事件字节数；出错时返回 `packet_error`。若错误码是 `ER_NET_READ_INTERRUPTED`（正常读超时），置 `*suppress_warnings = true`，**重连时不打错误日志**——源码注释说明「正常读超时引起的重连不应惊动用户」。

**第三步：空间检查与入队。** 见上文「中继日志空间的背压」。`queue_event` 被调用的位置在 `handle_slave_io` 中，参数是 `(mi, event_buf, event_len)`，`event_buf = (const char *)mysql->net.read_pos + 1`（跳过包头一字节）。

**第四步：落盘与刷元数据。** `queue_event` 内部真正写文件的是 `rli->relay_log.write_buffer(buf, event_len, mi)`，即 `sql/binlog.cc` 里的 `MYSQL_BIN_LOG::write_buffer`。它先断言 `is_relay_log` 与持有 `LOCK_log`，写成功后 `bytes_written += len`，再调 `after_write_to_relay_log(mi)`。

`MYSQL_BIN_LOG::after_write_to_relay_log`（`sql/binlog.cc`）做三件事：

- `bool can_rotate = mi->transaction_parser.is_not_inside_transaction();`——**只有不在事务内才允许按大小轮转**。
- `flush_and_sync(false)` 刷盘；失败则报 `ER_REPLICA_RELAY_LOG_WRITE_FAILURE` 并调 `truncate_relaylog_file(mi, atomic_binlog_end_pos)` 把文件截回已提交位置。
- 若 `can_rotate`，把刚入队事务的 GTID 加进 `mi->rli->add_logged_gtid(...)`（源码注释：若不加入，轮转后的下一个中继日志文件的 Previous GTIDs 里就没有它）。

**第五步：事件交给应用侧。** 中继日志写完后，`Rpl_applier_reader::read_next_event()`（`sql/rpl_applier_reader.cc`）被应用线程调用，读到文件末尾（`m_relaylog_file_reader.position() >= m_log_end_pos`）时会等在 `rli->relay_log.get_log_cond()` 上，直到 IO 线程写完并更新了 `LOCK_binlog_end_pos`。读到的事件交给 `exec_relay_log_event` → `apply_event_and_update_pos` 执行。

### 源库位点的推进规则

`inc_pos` 的取值按事件类型分派，是「源库位点如何前进」的唯一真源：

| 事件类型 | `inc_pos` |
|---|---|
| `FORMAT_DESCRIPTION_EVENT` | `uint4korr(buf + LOG_POS_OFFSET) ? event_len : 0`（源库没请求则不推进） |
| `PREVIOUS_GTIDS_LOG_EVENT` | `0`（位置改由 `mi->set_master_log_pos(mi->get_master_log_pos() + event_len)` 单独推进） |
| `TRANSACTION_PAYLOAD_EVENT` | `event_len` |
| `GTID_LOG_EVENT` | `event_len` |
| 其它类型 | `event_len`（函数体内 `inc_pos` 初始为 0，各 case 分别赋值） |

`inc_pos` 只在 `queue_event` 的「写进中继日志」分支里通过 `mi->set_master_log_pos(mi->get_master_log_pos() + inc_pos)` 生效；被服务器 ID 过滤掉的事件走的是另一条分支（`rli->ign_master_log_pos_end = mi->get_master_log_pos()`），但同样推进位点。

### 心跳事件

`heartbeat_queue_event(bool is_valid, Master_info *&mi, std::string binlog_name, uint64_t position, unsigned long &inc_pos, bool &do_flush_mi)` 校验心跳内容的一致性：

- `!is_valid` 报 `ER_REPLICA_HEARTBEAT_FAILURE`。
- 心跳带的文件名与 `mi->get_master_log_name()` 不同则报错。
- 心跳带的位置 `position` 小于 `mi->get_master_log_pos()` 则报错——源码注释解释「心跳只在对应坐标的事件之后才发送」。
- GTID 协议下，若 `mi->is_auto_position()` 且 `mi->get_master_log_pos() < position` 且文件名非空，则 `mi->set_master_log_pos(position)`，然后把心跳**转成一个 Rotate 事件写进中继日志**（`write_rotate_to_master_pos_into_relay_log`），并置 `inc_pos = 0`、`do_flush_mi = false`。

心跳还会更新 `mi->received_heartbeats++` 与 `mi->last_heartbeat = my_getsystime() / 10`。

### 中继日志的轮转与 Rotate 事件的合成

`write_rotate_to_master_pos_into_relay_log(THD *thd, Master_info *mi, bool force_flush_mi_info)` 合成一个 Rotate 事件写进中继日志，用来记录「源库位点已推进到某处但事件被过滤掉」这类信息：

- 构造 `Rotate_log_event ev(mi->get_master_log_name(), 0, mi->get_master_log_pos(), Rotate_log_event::DUP_NAME)`。
- 若 `mi->get_mi_description_event()` 非空，把它的 `checksum_alg` 复制给该 Rotate 事件。
- **`ev->server_id = 0`**——源码注释写明「不要被 slave SQL 线程忽略」。
- 调 `rli->relay_log.write_event(ev, mi)` 写入；失败报 `ER_REPLICA_RELAY_LOG_WRITE_FAILURE`。
- 在 `mi->data_lock` 下调 `flush_master_info(mi, force_flush_mi_info, false, false, mi->is_gtid_only_mode())`；失败报 `ER_RPL_REPLICA_CANT_FLUSH_CONNECTION_METADATA_REPOS`。
- `Rotate_log_event` 构造失败（内存不足）时报 `ER_REPLICA_CREATE_EVENT_FAILURE`。

`write_ignored_events_info_to_relay_log(THD *thd, Master_info *mi)` 是它的配套函数：当 `rli->ign_master_log_name_end[0]` 非空时，清掉该标记并调 `write_rotate_to_master_pos_into_relay_log(thd, mi, false)`，让应用线程能通过读这个 Rotate 拿到被忽略事件后的坐标。

### 中继日志事件的执行与位点

`apply_event_and_update_pos` 里位点更新的条件写成一个大 `if`：

```
if (*ptr_ev &&
    ((ev->get_type_code() != binary_log::XID_EVENT &&
      !is_committed_ddl(*ptr_ev)) ||
     skip_event ||
     (rli->is_mts_recovery() && !is_gtid_event(ev) &&
      (ev->ends_group() || !rli->mts_recovery_group_seen_begin) &&
      bitmap_is_set(&rli->recovery_groups, rli->mts_recovery_index))))
```

否则走 `else` 分支，只调 `rli->inc_event_relay_log_pos()`——**只推进中继日志内的位置，不动源库位点**。

MTS recovery 场景下还有一段分组恢复逻辑：事件结束时 `rli->mts_recovery_index++` 且 `--rli->mts_recovery_group_cnt == 0` 时，恢复完成，打印 `ER_RPL_MTA_RECOVERY_COMPLETE`，把 `rli->until_condition` 从 `UNTIL_SQL_AFTER_MTS_GAPS` 改为 `UNTIL_DONE`，再调 `rli->mts_finalize_recovery()` 与 `Rpl_info_factory::reset_workers(rli)`。

## 关键取舍

### 拉取与应用分离：两个线程、两个元数据结构

IO 线程只搬字节、应用线程只执行事件，两者通过中继日志文件解耦。源码里这个分工由 `start_slave_threads` 里两次独立的 `start_slave_thread` 调用固化：IO 线程用 `mi->run_lock` / `mi->start_cond` 与 `&mi->slave_running` / `&mi->slave_run_id`，SQL 线程用 `rli->run_lock` / `rli->start_cond` 与 `&rli->slave_running` / `&rli->slave_run_id`。

**取舍：多一次磁盘往返和一份中继日志占用的磁盘空间，换来「源库连接抖动不会中断正在执行的事务」与「中继日志可被多个应用线程并行读取」。** 源码注释在 `Rpl_applier_reader::read_next_event` 里说明了协调者与 worker 的锁序约束（`LOCK_thd_data` 与 `LOCK_binlog_end_pos` 的获取顺序），这正是双线程解耦带来的额外约束。

### 事务级空间判断而非事件级

空间检查用 `gtid_ev.get_trx_length()` 而不是 `event_len`。理由写在源码的注释里：「allow waiting only if we are outside of a transaction」。

**取舍：一次事务可以整体等待空间，代价是事务内绝不等待——如果中继日志空间在事务中途耗尽，IO 线程会把该事务剩余事件继续写入已超限的日志文件，直到事务结束。** `after_write_to_relay_log` 里 `can_rotate = mi->transaction_parser.is_not_inside_transaction()` 是同一条纪律的延续：**事务中途不轮转**。

### `inc_pos` 按事件类型分别赋值

位点推进不是一个统一公式，而是按事件类型在 `queue_event` 的 `switch` 里逐类决定。FD 事件只在源库明确请求时才推进，PREVIOUS_GTIDS 事件推进方式又是另一套（改 `mi->set_master_log_pos` 而非 `inc_pos`）。

**取舍：位点语义必须逐事件类型精确，否则会出现「位点推进过快导致事件被跳过」或「推进过慢导致重复拉取」。** 这也是 `queue_event` 复杂度的主要来源。

### 重连次数有上限，由 `mi->retry_count` 控制

`try_to_reconnect` 的 `if (*retry_count > mi->retry_count) return 1;` 明确写在代码里，注释是「Don't retry forever」。等待间隔 `mi->connect_retry` 毫秒。

**取舍：有限次重连保证 IO 线程不会无限占用资源并反复冲击源库；代价是长时间网络中断后需要人工介入重启复制。**

### 瞬时错误重试有独立上限

`exec_relay_log_event` 里 `rli->trans_retries < slave_trans_retries` 是重试闸门，单次暂停用 `min<ulong>(rli->trans_retries, MAX_SLAVE_RETRY_PAUSE)`。

**取舍：重试次数与暂停时长都封顶，避免应用线程在锁等待超时这类瞬时错误上无限自旋。** 源码在放弃时给出的提示文本直接指向配置项名：`Consider raising the value of the replica_transaction_retries variable.`

### 跳过事件时主动释放 `mem_root`

`apply_event_and_update_pos` 在 `skip_event` 为真时调 `thd->mem_root->ClearForReuse()`。源码注释给出的场景是：`slave_skip_counter` 很大、从库持续跳过事件并更新位点时，这期间的内存一直不释放，直到开始真正执行事件。

**取舍：多一次显式释放，代价是跳过路径与执行路径在内存管理上分成两套。**

### 写失败要回滚事务边界解析器

`queue_event` 在 `rli->relay_log.write_buffer` 返回非零时调 `mi->transaction_parser.rollback()`，注释写明原因：否则重启 IO 线程时（非 GTID 自动定位模式），解析器会认为失败的那个事件已经入队。

**取舍：多一次状态回滚，换来「写失败后重启 IO 线程不会跳过未入队的事件」。**

### 校验和算法在 FD 事件处换口径

`queue_event` 里 `checksum_alg` 优先取 `mi->checksum_alg_before_fd`，只有它是 `BINLOG_CHECKSUM_ALG_UNDEF` 才用当前中继日志的算法；而遇到 FD 事件本身时又用 `Log_event_footer::get_checksum_alg` 单独取 FD 自带的描述符。

**取舍：中继日志的校验和算法始终跟随源库格式，而源库格式可能在中途变更（FD 事件即变更点），所以必须保留 FD 之前的算法值备用。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/rpl_replica.cc | `handle_slave_io` | IO 线程入口：以 `connect_init` / `connected` 标签为骨架的状态机，负责连接源库、注册、发起 dump、逐事件读取并 `queue_event` 入队；内层循环按 `mysql_errno` 区分致命错误与可重连错误 |
| sql/rpl_replica.cc | `exec_relay_log_event` | 中继日志事件的执行入口：拿 `rli->data_lock`、做 MTS checkpoint、判断 kill 与 until 条件、处理并行执行下的 FD 回滚，调用 `apply_event_and_update_pos` 后处理事件销毁与瞬时错误重试 |
| sql/rpl_replica.cc | `apply_event_and_update_pos` | 应用单个事件并更新位点：做跳过判定（`shall_skip` / `EVENT_SKIP_COUNT`）、执行 `ev->apply_event(rli)`、在并行模式下把事件投递给 worker，最后按条件调 `ev->update_pos(rli)` 或 `rli->inc_event_relay_log_pos()` |
| sql/rpl_replica.cc | `queue_event` | 把源库事件写入中继日志的唯一入口：校验校验和、按事件类型决定 `inc_pos`、做服务器 ID 过滤、调用 `rli->relay_log.write_buffer` 落盘、推进源库位点并刷连接元数据，返回 `QUEUE_EVENT_RESULT` 三态 |
| sql/rpl_replica.cc | `try_to_reconnect` | 唯一重连入口：自增 `*retry_count`，超过 `mi->retry_count` 则返回 1，否则 `slave_sleep(thd, mi->connect_retry, ...)` 后调 `safe_reconnect` |
| sql/rpl_replica.cc | `request_dump` | 发起 binlog dump 请求：按 `mi->is_auto_position()` 选 `COM_BINLOG_DUMP_GTID` 或 `COM_BINLOG_DUMP`，标志位固定含 `USE_HEARTBEAT_EVENT_V2` |
| sql/rpl_replica.cc | `read_event` | 从源库读一个事件字节流，底层调 `mysql_binlog_fetch`；出错返回 `packet_error`，读超时（`ER_NET_READ_INTERRUPTED`）时置 `suppress_warnings` |
| sql/rpl_replica.cc | `exceeds_relay_log_limit` | 中继日志空间判定式：`log_space_limit != 0 && log_space_limit < log_space_total + queued_size` |
| sql/rpl_replica.cc | `wait_for_relay_log_space` | 空间不足时的等待：置 `is_receiver_waiting_for_rl_space`、主动 `rotate_relay_log`、在 `rli->log_space_cond` 上等待 |
| sql/rpl_replica.cc | `heartbeat_queue_event` | 心跳事件处理：校验文件名与位置一致性；GTID 模式下把心跳转成 Rotate 事件写入中继日志并推进位点 |
| sql/rpl_replica.cc | `write_rotate_to_master_pos_into_relay_log` | 合成 Rotate 事件写入中继日志以记录源库位点；`server_id` 置 0 以免被应用线程忽略，随后刷连接元数据 |
| sql/rpl_replica.cc | `write_ignored_events_info_to_relay_log` | 被过滤事件的后续处理：清 `ign_master_log_name_end` 标记并补写 Rotate 事件，让应用线程拿到忽略后的坐标 |
| sql/rpl_replica.cc | `start_slave_threads` | 复制线程启动编排：先按 `SLAVE_IO` 启 IO 线程，再按 `SLAVE_SQL` 启 SQL 线程；两者用不同的锁、条件变量与运行标志 |
| sql/rpl_replica.cc | `handle_slave_sql` | 应用线程入口：初始化 MTS submode 与提交序管理器、恢复临时表、逐事件读取中继日志并交给执行路径 |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_buffer` | 中继日志裸字节写入：`write_buffer` 写文件成功后 `bytes_written += len` 并调 `after_write_to_relay_log`；失败则 `truncate_relaylog_file` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_event` | 中继日志事件写入版本：`binary_event_serialize` 序列化后 `bytes_written += ev->common_header->data_written`，再调 `after_write_to_relay_log` |
| sql/binlog.cc | `MYSQL_BIN_LOG::after_write_to_relay_log` | relay log 写后处理：`can_rotate = mi->transaction_parser.is_not_inside_transaction()` 决定能否按大小轮转；`flush_and_sync(false)` 失败则截回文件；成功且可轮转时把事务 GTID 加入 `add_logged_gtid` |
| sql/rpl_applier_reader.cc | `Rpl_applier_reader::read_next_event` | 应用线程读中继日志：读到文件末尾时在 `relay_log.get_log_cond()` 上等待，直到 IO 线程写完并更新 `LOCK_binlog_end_pos` |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 WAL 层，与本页的复制 IO 线程没有代码级关联，但机制形态上有可对照之处：

- **两者都是「先写日志、再推进可见位点」的顺序纪律。** 本页的对应点是 `queue_event` 里「先 `rli->relay_log.write_buffer` 落盘、落盘成功后才 `mi->set_master_log_pos` 推进位点」的顺序——位点永远不领先于已落盘的数据。**架构推论：** 主核 WAL 侧是否存在等价机制需另行核对主核源码，本页不作断言。
- **两者都有「日志空间背压」这一层。** 本页是 `exceeds_relay_log_limit` + `wait_for_relay_log_space` 的组合，且严格限定「事务内不等待」。**架构推论：** 这条「事务边界优先于资源回收」的纪律是任何提供事务语义的存储引擎都要面对的共同约束，主核 WAL 的刷写策略是否遵循同一条纪律，本页不作断言。

## 参考

- [Replication（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/replication.html) —— 复制总体模型：source / replica 角色、复制线程的职责划分、基于位点与基于 GTID 的两种复制方式的上位背景。
- [The Binary Log（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/binary-log.html) —— binlog 的官方定位：记录数据变更与可能产生变更的语句、只记录完整事件或完整事务、校验和语义。
- [SHOW REPLICA STATUS（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/show-replica-status.html) —— `SHOW REPLICA STATUS` 各状态字段的官方解释，是 IO 线程位点与心跳计数在运维侧的对应出口。
- [Replication Threads（MySQL 8.0 Reference Manual）](https://dev.mysql.com/doc/refman/8.0/en/replication-threads.html) —— IO 线程与 SQL 线程职责分离的官方说明，是本页「机制是什么」开头两线程分工的上位依据。
- [sql/rpl_replica.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_replica.cc) —— `handle_slave_io`、`queue_event`、`try_to_reconnect`、`exec_relay_log_event`、`apply_event_and_update_pos` 等本页全部符号的权威来源。
- [sql/binlog.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/binlog.cc) —— `MYSQL_BIN_LOG::write_buffer` / `write_event` / `after_write_to_relay_log` 三个中继日志落盘函数的权威来源。
