---
project: mysql-server
feature: binlog-rotation
page_type: feature
schema_version: 1
refs:
  - path: sql/binlog.cc
    symbols: [MYSQL_BIN_LOG::rotate, MYSQL_BIN_LOG::purge_logs, MYSQL_BIN_LOG::add_log_to_index]
---

# binlog 轮转与清理

## 一句话职责

`sql/binlog.cc` 里的 binlog 轮转与清理机制负责三件事：**按大小或显式指令切换当前 binlog 文件**、**按保留期删除过期文件**、**用「崩溃安全」的写-改-改名流程维护 binlog 索引文件**（`binlog.index`）。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 binlog 文件的轮转、过期文件清理与索引文件的维护；**不负责事件内容（L01）**、**不负责从库的重放（M 组）**。

## 机制是什么

### 三个动作、三份落盘状态

binlog 在磁盘上有三类文件，机制围绕它们各司其职：

| 对象 | 作用 | 本页涉及的函数 |
|---|---|---|
| binlog 数据文件（如 `binlog.000042`） | 存放事件字节 | `MYSQL_BIN_LOG::rotate` 负责切换到新文件 |
| binlog 索引文件（`binlog.index`） | 按行列出全部 binlog 文件名 | `MYSQL_BIN_LOG::add_log_to_index` 负责追加 |
| 崩溃安全索引文件（临时文件） | 写入中转站 | `open_crash_safe_index_file` / `move_crash_safe_index_file_to_index_file` |

`add_log_to_index()` 的函数注释直接写明了这套三步设计的目的：「To make crash safe, we copy all the content of index file to crash safe index file firstly and then append the log file name to the crash safe index file. Finally move the crash safe index file to index file.」——**写索引不直接改 `binlog.index`，而是先写临时文件再原子改名**。

### 「过期」的判定基准：文件 mtime，不是文件名

清理的核心判据是**文件的最后修改时间**。`purge_logs_before_date()` 的主循环里：

```c
else if (stat_area.st_mtime < purge_time) {
```

只有 `stat_area.st_mtime < purge_time` 成立，文件才进入待清理集合。`purge_time` 由 `calculate_auto_purge_lower_time_bound()` 算出，基准是「当前时间减去保留窗口」。

### 保留窗口由两个互斥系统变量决定

`calculate_auto_purge_lower_time_bound()` 的判据顺序是：

1. `binlog_expire_logs_seconds > 0` → `expiration_time = current_time - binlog_expire_logs_seconds`；
2. 否则 `expire_logs_days > 0` → `expiration_time = current_time - expire_logs_days * static_cast<int64>(SECONDS_IN_24H)`。

`SECONDS_IN_24H` 是「一天秒数」常量（定义在 `include/` 下的时间头文件里，本地稀疏检出未包含该头文件）。函数末尾的 `assert(binlog_expire_logs_seconds > 0 || expire_logs_days > 0)` 说明**这两个变量至少要有一个大于 0**，否则该函数不被允许调用。

**架构推论：** `binlog_expire_logs_seconds` 的优先级高于 `expire_logs_days`，即同时设置两者时以秒级变量为准。这一优先级来自函数体内 `if / else if` 的短路顺序，源码注释也自述这两个变量「are two mutually exclusive variables that control the purge period and one of them is deprecated」。

## 怎么实现的

### 第一步：`MYSQL_BIN_LOG::rotate()` 判断要不要轮转

`rotate(bool force_rotate, bool *check_purge)` 的函数体极短，判断条件是四个析取项：

```c
if (DBUG_EVALUATE_IF("force_rotate", 1, 0) || force_rotate ||
    (m_binlog_file->get_real_file_size() >= (my_off_t)max_size) ||
    DBUG_EVALUATE_IF("simulate_max_binlog_size", true, false)) {
  error = new_file_without_locking(nullptr);
  *check_purge = true;
}
```

四个条件分别是：调试开关 `force_rotate`、调用方传入的 `force_rotate` 形参、**当前文件真实大小达到 `max_size`**、调试开关 `simulate_max_binlog_size`。任一成立就调 `new_file_without_locking(nullptr)` 建新文件，并把 `*check_purge` 置为 true 表示「本次发生了轮转，可以顺手清理过期文件」。

两个细节：

- 函数体开头有 `assert(!is_relay_log)` 与 `mysql_mutex_assert_owner(&LOCK_log)`——**轮转只对 binlog 生效，不处理 relay log，且调用方必须已持有 `LOCK_log`**。
- `max_size` 是 `MYSQL_BIN_LOG` 的成员，类型 `ulong`。它由 `open_binlog()` 的 `max_size_arg` 参数写入（`max_size = max_size_arg;`），而 `open_binlog()` 的这个参数在启动路径上由 `sql/mysqld.cc` 传入系统变量 `max_binlog_size`（`mysql_bin_log.open_binlog(opt_bin_logname, nullptr, max_binlog_size, ...)`），在运行期由 `MYSQL_BIN_LOG::set_max_size()` 改写（`if (is_open()) max_size = max_size_arg;`）。
- `get_real_file_size()` 来自 `Binlog_ofile`，其定义是 `m_position + m_encrypted_header_size`——**加密文件的真实大小含加密头**，所以轮转阈值对加密与非加密 binlog 是同一套口径。

### 第二步：`new_file_without_locking()` → `new_file_impl()` 建新文件

`new_file_without_locking()` 只是包装：转调 `new_file_impl(false /*need_lock_log=false*/, extra_description_event)`。`new_file()` 则是 `new_file_impl(true /*need_lock_log=true*/, ...)`。两个包装器的存在是为了**不让外部代码在没持锁的情况下访问类**（源码注释：allows external access to the class without a lock, which is not possible with private new_file_without_locking method）。

`new_file_impl()` 的关键顺序：

1. `if (!is_open()) return error;` —— binlog 没开就直接返回成功，不做事。
2. 拿 `LOCK_log`（`need_lock_log` 为真时）并 `mysql_mutex_lock(&LOCK_xids)`，随后 `while (get_prep_xids() > 0) mysql_cond_wait(&m_prep_xids_cond, &LOCK_xids);`——**等待所有 prepared XID 归零**。源码注释解释：等待存储引擎提交以使 `m_atomic_prep_xids` 递减，同时保持 `LOCK_log` 挡住新事务写 binlog。
3. `mysql_mutex_lock(&LOCK_index)`，并断言同时持有 `LOCK_log` 与 `LOCK_index`。
4. `DBUG_EVALUATE_IF("expire_logs_always", 0, 1) && (error = ha_flush_logs(true))` —— 先让存储引擎刷盘。
5. **非 relay log 时**调 `gtid_state->save_gtids_of_last_binlog_into_table()`，把上一个 binlog 文件的 GTID 集合落到 `mysql.gtid_executed` 表。这一步失败且错误码是 `ER_RPL_GTID_TABLE_CANNOT_OPEN` 时分两种走向：
   - **当前文件未达 `max_size`**（`m_binlog_file->get_real_file_size() < static_cast<my_off_t>(max_size)` 且未命中 `simulate_max_binlog_size`）→ `close_on_error` 保持 false，报 `ER_BINLOG_UNABLE_TO_ROTATE_GTID_TABLE_READONLY` 并提示「Current binlog file was flushed to disk and will be kept in use.」，**放弃轮转、保留当前文件继续用**；
   - **当前文件已达 `max_size`** → `close_on_error` 置 true，拼出 `ER_RPL_GTID_TABLE_CANNOT_OPEN` 的错误串；此时若 `binlog_error_action != ABORT_SERVER`，额外报一条 WARNING 级 `ER_BINLOG_UNABLE_TO_ROTATE_GTID_TABLE_READONLY`，文案是「Binary logging going to be disabled.」。
   - 错误码不是 `ER_RPL_GTID_TABLE_CANNOT_OPEN`（例如 OOM）→ `close_on_error = true`，错误串为 `ER_OOM_SAVE_GTIDS`。
6. `generate_new_name(new_name, name)` 生成新文件名；失败则**沿用旧名**、置 `close_on_error`、错误串为 `ER_NO_UNIQUE_LOGFILE`。
7. 若当前 binlog 文件已打开，先写一个 `Rotate_log_event`（`Rotate_log_event r(new_name + dirname_length(new_name), 0, LOG_EVENT_OFFSET, is_relay_log ? Rotate_log_event::RELAY_LOG : 0)`），写失败置 `close_on_error`、错误串为 `ER_ERROR_ON_WRITE`；写成功再 `m_binlog_file->flush()`，失败错误串为「Either disk is full or file system is read only」。
8. `close(LOG_CLOSE_TO_BE_OPENED | LOG_CLOSE_INDEX, false, false)` 关闭旧文件与旧索引。
9. `open_index_file(index_file_name, nullptr, false)` 重开索引文件（源码注释标注 BUG#34582），成功后 `open_binlog(old_name, new_name_ptr, max_size, true /*null_created_arg=true*/, false /*need_lock_index=false*/, true /*need_sid_lock=true*/, extra_description_event)` 重开并新建 binlog 文件；失败报 `ER_CANT_OPEN_FILE`。
10. 收尾 `atomic_log_state = LOG_OPENED;`、`m_dependency_tracker.rotate();`、`close_purge_index_file();`、`update_binlog_end_pos();`。

### 第三步：`add_log_to_index()` 维护索引文件

`add_log_to_index(uchar *log_name, size_t log_name_len, bool need_lock_index)` 的四步失败分支逐一对应错误码：

| 步骤 | 失败错误码 |
|---|---|
| `open_crash_safe_index_file()` 失败 | `ER_BINLOG_CANT_OPEN_TMP_INDEX` |
| `copy_file(&index_file, &crash_safe_index_file, 0)` 失败 | `ER_BINLOG_CANT_COPY_INDEX_TO_TMP` |
| `my_b_write` + `flush_io_cache` + `mysql_file_sync` 失败 | `ER_BINLOG_CANT_APPEND_LOG_TO_TMP_INDEX` |
| `close_crash_safe_index_file()` 失败 | `ER_BINLOG_CANT_CLOSE_TMP_INDEX` |
| `move_crash_safe_index_file_to_index_file(need_lock_index)` 失败 | `ER_BINLOG_CANT_MOVE_TMP_TO_INDEX` |

任何一步失败都 `goto err` 返回 `-1`。成功路径返回 `0`。

写入内容是**文件名加一个换行符**：`my_b_write(&crash_safe_index_file, log_name, log_name_len)` 之后紧跟 `my_b_write(&crash_safe_index_file, pointer_cast<const uchar *>("\n"), 1)`，然后 `flush_io_cache` + `mysql_file_sync(crash_safe_index_file.file, MYF(MY_WME))` 强制落盘。

**唯一调用方**在 `open_binlog()` 里：`if (DBUG_EVALUATE_IF("fault_injection_updating_index", 1, 0) || add_log_to_index((uchar *)log_file_name, strlen(log_file_name), need_lock_index))`——**新 binlog 文件在 `open_binlog()` 里落盘后就立刻写进索引**，这是索引文件与数据文件保持一致的时点。

`move_crash_safe_index_file_to_index_file()` 里有一段值得注意的重试逻辑：删除旧索引文件与重命名临时文件各自有一个 `while` 重试循环，次数由 `MYSQL_BIN_LOG::MAX_RETRIES_FOR_DELETE_RENAME_FAILURE` 控制（该常量定义在 `sql/binlog.h`，本地稀疏检出未包含该头文件）。源码注释解释了原因：外部程序（如 MEB）会临时加锁 `binlog.index` 以便查看内容，在 Windows 这类平台上删除会失败，因此服务端应重试若干次而不是立即 panic。两次重试之间的等待是 `my_sleep(1000)`。

### 第四步：`purge_logs()` 删除过期文件

`purge_logs(const char *to_log, bool included, bool need_lock_index, bool need_update_threads, ulonglong *decrease_log_space, bool auto_purge)` 的执行顺序：

1. 锁 `LOCK_index`（`need_lock_index` 为真时；否则断言已持有）。
2. `find_log_pos(&log_info, to_log, false)` 定位目标文件，失败报 `ER_BINLOG_PURGE_LOGS_CALLED_WITH_FILE_NOT_IN_INDEX`。
3. `open_purge_index_file(true)` 打开清理登记文件，失败报 `ER_BINLOG_PURGE_LOGS_CANT_SYNC_INDEX_FILE`。
4. 再次 `find_log_pos(&log_info, NullS, false)` 从索引头开始遍历。
5. 遍历循环 `while ((compare_log_name(to_log, log_info.log_file_name) || (exit_loop = included)))`，循环体里有两个「停下来」的分支：
   - **`is_active(log_info.log_file_name)`** 为真（当前正在写的文件）→ 跳出。`is_active()` 的实现是 `!compare_log_name(log_file_name, log_file_name_arg)`，即与当前活动文件名相同。手动清理时 `push_warning_printf` 报 `ER_WARN_PURGE_LOG_IS_ACTIVE`；`auto_purge` 为真时**不报**。
   - **`log_in_use(log_info.log_file_name)`** 非零（正被从库读取）→ 跳出。`log_in_use()` 遍历 `log_info_set`，用 `strncmp` 匹配，命中则报一条 WARNING 级 `ER_BINLOG_FILE_BEING_READ_NOT_PURGED` 并计数。手动清理时 `push_warning_printf` 报 `ER_WARN_PURGE_LOG_IN_USE`。
   - 两个分支都 `break` 而**不是 `continue`**——**遇到活动文件或被占用文件就整段停止**，后面的文件一个都不删。
6. 通过 `register_purge_index_entry(log_info.log_file_name)` 把待删文件名写进登记文件（每行一个文件名，后面跟 `\n`），失败报 `ER_BINLOG_PURGE_LOGS_CANT_COPY_TO_REGISTER_FILE`。
7. `sync_purge_index_file()` 把登记文件刷盘，失败报 `ER_BINLOG_PURGE_LOGS_CANT_FLUSH_REGISTER_FILE`。
8. `remove_logs_from_index(&log_info, need_update_threads)` 更新索引文件，失败报 `ER_BINLOG_PURGE_LOGS_CANT_UPDATE_INDEX_FILE`。该函数内部同样走「复制到崩溃安全文件 → 改名」的流程，只是 `copy_file` 的第三参是 `log_info->index_file_start_offset`（只复制保留部分），最后 `if (need_update_threads) adjust_linfo_offsets(log_info->index_file_start_offset);` 修正各线程记录的索引偏移。
9. **非 relay log 时**更新 GTID 丢失集合：在 `global_sid_lock` 写锁下 `init_gtid_sets(nullptr, const_cast<Gtid_set *>(gtid_state->get_lost_gtids()), opt_source_verify_checksum, false, nullptr, nullptr)`。**这一段被删文件的 GTID 会进入 `lost_gtids`**，供后续 `gtid_purged` 语义使用。
10. `err:` 标签处，若前面无错误且登记文件已初始化，`purge_index_entry(thd, decrease_log_space, false)` 真正删文件，失败报 `ER_BINLOG_PURGE_LOGS_FAILED_TO_PURGE_LOG`；再 `close_purge_index_file()`。
11. 错误码优先级：`error = error ? error : (error_index ? error_index : close_error_index);`——**主流程错误 > 删除文件错误 > 关闭登记文件错误**。

`purge_index_entry()` 里对 `ENOENT` 的处理值得一提：`mysql_file_stat` 返回 `ENOENT` 时**不算致命**，只 `push_warning_printf` 报 `ER_LOG_PURGE_NO_FILE` 并 `LogErr(INFORMATION_LEVEL, ER_CANT_STAT_FILE, ...)`，然后 `set_my_errno(0)` 继续；其它 errno 才是致命错误（报 `ER_BINLOG_PURGE_FATAL_ERR`，文案建议「consider examining correspondence of your binlog index file to the actual binlog files」）。

### 触发路径：谁会调起轮转与清理

| 入口 | 调用链 | 触发条件 |
|---|---|---|
| `FLUSH BINARY LOGS` | `sql_yacc.yy` 把 `BINARY_SYM LOGS_SYM` 置为 `REFRESH_BINARY_LOG` → `sql_reload.cc` 的 `handle_reload_request()` → `mysql_bin_log.rotate_and_purge(thd, true)` | 显式 SQL |
| `FLUSH LOGS` | `sql_reload.cc` 中 `options & REFRESH_LOG` 分支把 `REFRESH_BINARY_LOG` 一并置位 | 显式 SQL |
| SIGHUP | 同样走 `handle_reload_request()`；无 THD 时 `sql_reload.cc` 会 `new THD` 造一个临时 THD 来执行轮转 | 信号 |
| binlog 写满 `max_size` | `ordered_commit()` 里 `Binlog_group_commit_ctx::aggregate_rotate_settings(final_queue)` 取出 `check_rotate` / `force_rotate`，再 `mysql_mutex_lock(&LOCK_log)` → `rotate(force_rotate, &check_purge)` | 容量阈值 |
| 轮转成功后 | `rotate_and_purge()` 与 `ordered_commit()` 两处都是 `if (!error && check_purge) auto_purge();` | 轮转的副产物 |

`ordered_commit()` 里的轮转分支有一处关键设计。源码注释写明：

> Normally the rotate is not forced as several consecutive groups may request unnecessary rotations. Without force, rotate() will double check whether max size has been exceed and skip if not, this is an optimization to avoid taking LOCK_log.

即**非强制轮转时，`rotate()` 内部会再查一次 `max_size` 是否真的超出，没超就跳过**——这样就不必为此抢 `LOCK_log`。这就是 `rotate()` 里 `m_binlog_file->get_real_file_size() >= (my_off_t)max_size` 这一项的实际用途：它既是「强制轮转时的判据」，也是「非强制轮转时的复核」。而尺寸超限的原始信号来自 `process_flush_stage_queue()` 末尾的 `first_seen->rpl_thd_ctx.binlog_group_commit_ctx().set_max_size_exceeded(total_bytes > 0 && (m_binlog_file->get_real_file_size() >= (my_off_t)max_size || DBUG_EVALUATE_IF("simulate_max_binlog_size", true, false)))`。

`ordered_commit()` 里轮转前先调 `(void)finish_commit(thd);`，源码注释给出理由：「Finish the commit before executing a rotate, or run the risk of a deadlock.」轮转失败则 `thd->commit_error = THD::CE_COMMIT_ERROR`。

`rotate_and_purge(THD *thd, bool force_rotate)` 是「轮转 + 清理」的组合入口：

1. `thd->set_skip_readonly_check()`——`FLUSH BINARY LOGS` 忽略 `read_only` / `super_read_only`，源码注释说明目的是让 `mysql.gtid_executed` 复制仓库表能被更新。
2. `ha_binlog_wait(thd)`——等待 handlerton 把待写信息插进 binlog。
3. `mysql_mutex_lock(&LOCK_log)` → `rotate(force_rotate, &check_purge)` → `mysql_mutex_unlock(&LOCK_log)`。源码注释明确写出**清锁顺序的理由**：「Run purge_logs wo/ holding LOCK_log because it does not need the mutex. Otherwise causes various deadlocks.」
4. `if (!error && check_purge) auto_purge();`

`auto_purge()` 的前置检查：

- `check_auto_purge_conditions()` 返回 true（即**不允许清理**）时直接返回。该函数的两个返回 true 条件是：`!opt_binlog_expire_logs_auto_purge`（自动清理被关闭）、`binlog_expire_logs_seconds == 0 && expire_logs_days == 0`（没配保留窗口）。
- `check_purge_conditions(*this)` 返回 `std::pair<bool, int>`。其实现只检查一项：`if (!log.is_open()) return std::make_pair(true, 0);`——**binlog 没打开就不许清理**。错误码 `LOG_INFO_MEM` 走 `exec_binlog_error_action_abort()`，错误码 `LOG_INFO_BACKUP_LOCK` 走 `LogErr(WARNING_LEVEL, ER_LOG_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK)`。
- `Shared_backup_lock_guard backup_lock{current_thd};` 拿 BACKUP 锁，三种结果分别对应：已锁 → 继续；未锁 → 报 `ER_LOG_CANNOT_PURGE_BINLOG_WITH_BACKUP_LOCK` 并返回；OOM → `exec_binlog_error_action_abort(ER_OUT_OF_RESOURCES_MSG)`。
- `ha_flush_logs()`（注释：Flush logs for storage engines, so that the last transaction is persisted inside storage engines），然后 `purge_logs_before_date(purge_time, auto_purge)`。

另有 `auto_purge_at_server_startup()`：启动时先跑 `check_auto_purge_conditions()`，通过后直接 `purge_logs_before_date(purge_time, auto_purge)`——**启动时不重复拿 BACKUP 锁**。

### 手动清理的两个 SQL 入口

- `PURGE BINARY LOGS TO 'xxx'` → `purge_source_logs_to_file()`。它先 `check_purge_conditions(mysql_bin_log)`，再 `Shared_backup_lock_guard`，最后 `mysql_bin_log.make_log_name(search_file_name, to_log)` 后调 `mysql_bin_log.purge_logs(search_file_name, include_to_log /*=false*/, need_index_lock /*=true*/, need_update_threads /*=true*/, nullptr, auto_purge /*=false*/)`，之后 `ha_binlog_index_purge_wait(thd)`。
- `PURGE BINARY LOGS BEFORE 'date'` → `purge_source_logs_before_date()`，最后调 `mysql_bin_log.purge_logs_before_date(purge_time, auto_purge /*=false*/)`。

两条路径都在 `purge_logs()` 传 `auto_purge = false`，这是 `purge_logs()` 里「手动清理要 `push_warning_printf` 报 `ER_WARN_PURGE_LOG_IS_ACTIVE` / `ER_WARN_PURGE_LOG_IN_USE`，自动清理不报」这个分支的来源。

`purge_logs_before_date()` 的遍历逻辑与 `purge_logs()` 不同：它以 `is_active()` 为循环条件（`while (!(log_is_active = is_active(log_info.log_file_name)))`），对每个文件 `mysql_file_stat` 取 mtime，`stat_area.st_mtime < purge_time` 才算过期；过期文件还要过 `log_in_use()` 这一关。`ENOENT` 不算致命（`set_my_errno(0)` 后继续），其它 errno 致命（`error = LOG_INFO_FATAL`）。最后若确实删了文件，`to_log` 非空才调 `purge_logs(to_log, true /*included*/, false, true, nullptr, auto_purge)`。

## 关键取舍

### 取舍一：索引文件用「临时文件 + 改名」而不是直接追加

直接往 `binlog.index` 追加，一旦写到一半崩溃，索引文件就会多出一行残缺文件名。反过来，改名方案即使中途崩溃，最多丢失**最后一行**（新文件名没进索引），不会产生半行脏数据。代价是每次追加都要复制整个索引文件——`copy_file(&index_file, &crash_safe_index_file, 0)` 的第三参为 `0` 表示从偏移 0 开始复制全部内容。

### 取舍二：轮转与清理分成两个锁区间

`rotate_and_purge()` 显式在 `rotate()` 后释放 `LOCK_log`，再去做清理。源码注释直接给出原因：清理不需要这把锁，持有会造成死锁。这是「锁粒度宁细勿粗」的具体落点。

### 取舍三：GTID 表打不开时，放弃轮转而不是强行轮转

`new_file_impl()` 里 `ER_RPL_GTID_TABLE_CANNOT_OPEN` 的分支有一个关键设计：**如果当前文件还没到 `max_size`，就不轮转**，保留现有文件继续写入（报错文案「Current binlog file was flushed to disk and will be kept in use.」）。只有已经达到 `max_size` 才必须硬着头皮继续，并在 `binlog_error_action != ABORT_SERVER` 时报 WARNING「Binary logging going to be disabled.」。

**架构推论：** 这样做的目的是保住 GTID 集合与 binlog 内容的对应关系——若在 `mysql.gtid_executed` 未落盘时强行轮转，新文件里的 GTID 与表里记录的集合会出现缺口。这是从 `save_gtids_of_last_binlog_into_table()` 失败后 `close_on_error` 的取值分支反推出来的语义，源码本身只给出条件判断，没有直接陈述该意图。

### 取舍四：清理遇到「活动文件」或「被占用文件」就整段停止

`purge_logs()` 的遍历循环里，`is_active()` 与 `log_in_use()` 两个分支都是 `break`，不是 `continue`。这意味着**只要有一个文件正被写入或正被从库读取，其后面的所有过期文件都不会被删除**。这是保守取向：宁可少删，不可删掉正在读的文件。

### 取舍五：自动清理静默，手动清理告警

同一个 `purge_logs()` 里，所有 `push_warning_printf` 都被 `if (!auto_purge)` 包住。自动清理是后台行为，不该往客户端会话里塞警告；手动 `PURGE BINARY LOGS` 则必须让用户知道「有些文件没删掉」。

### 取舍六：删不掉的旧文件不阻塞整体流程

`purge_index_entry()` 里 `mysql_file_stat` 返回 `ENOENT` 时只记 INFORMATION 级日志并继续，因为索引里列了但磁盘上已经没有的文件，不影响其他文件的清理。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/binlog.cc | `MYSQL_BIN_LOG::rotate` | 轮转决策点：四条件析取（`force_rotate` 形参 / `force_rotate` 调试开关 / 真实文件大小达 `max_size` / `simulate_max_binlog_size` 调试开关），成立则调 `new_file_without_locking(nullptr)` 并置 `*check_purge` |
| sql/binlog.cc | `MYSQL_BIN_LOG::new_file_without_locking` | 轮转落地入口，转调 `new_file_impl(false /*need_lock_log=false*/, extra_description_event)` |
| sql/binlog.cc | `MYSQL_BIN_LOG::new_file_impl` | 新文件创建的完整流程：等 `prep_xids` 归零、`save_gtids_of_last_binlog_into_table`、`generate_new_name`、写 `Rotate_log_event`、`close` 旧文件与索引、`open_index_file` + `open_binlog` |
| sql/binlog.cc | `MYSQL_BIN_LOG::purge_logs` | 按文件名清理：定位 `to_log`、遍历索引、跳过活动文件与被占用文件、写登记文件、更新索引、更新 `gtid_state->get_lost_gtids()`、最后删文件 |
| sql/binlog.cc | `MYSQL_BIN_LOG::purge_logs_before_date` | 按 mtime 清理：`stat_area.st_mtime < purge_time` 且未被 `log_in_use()` 命中的文件进入待删集合 |
| sql/binlog.cc | `MYSQL_BIN_LOG::add_log_to_index` | 索引文件追加：开崩溃安全文件 → 复制全量索引 → 追加文件名与换行 → flush+fsync → 关闭 → 改名回索引文件 |
| sql/binlog.cc | `MYSQL_BIN_LOG::open_crash_safe_index_file` | 打开崩溃安全索引文件（`O_RDWR | O_CREAT`，`init_io_cache` 用 `WRITE_CACHE`） |
| sql/binlog.cc | `MYSQL_BIN_LOG::move_crash_safe_index_file_to_index_file` | 删除旧索引并把临时文件改名回索引；删除与改名各有 `MAX_RETRIES_FOR_DELETE_RENAME_FAILURE` 次重试，间隔 `my_sleep(1000)` |
| sql/binlog.cc | `MYSQL_BIN_LOG::remove_logs_from_index` | 删除索引条目：`copy_file` 带 `log_info->index_file_start_offset` 只保留后半段，再 `adjust_linfo_offsets` 修正线程偏移 |
| sql/binlog.cc | `MYSQL_BIN_LOG::register_purge_index_entry` | 把待删文件名写入登记文件（文件名 + `\n`） |
| sql/binlog.cc | `MYSQL_BIN_LOG::purge_index_entry` | 读登记文件逐个删文件；`ENOENT` 非致命，其余 errno 报 `ER_BINLOG_PURGE_FATAL_ERR` |
| sql/binlog.cc | `MYSQL_BIN_LOG::is_active` | 判断某文件是否为当前活动文件：`!compare_log_name(log_file_name, log_file_name_arg)` |
| sql/binlog.cc | `MYSQL_BIN_LOG::log_in_use` | 遍历 `log_info_set` 用 `strncmp` 统计正在读该文件的线程数，命中报 `ER_BINLOG_FILE_BEING_READ_NOT_PURGED` |
| sql/binlog.cc | `MYSQL_BIN_LOG::find_log_pos` | 在索引文件里按行查找定位，返回 `LOG_INFO_EOF` / `LOG_INFO_IO` |
| sql/binlog.cc | `MYSQL_BIN_LOG::rotate_and_purge` | 组合入口：`set_skip_readonly_check` → `ha_binlog_wait` → 持 `LOCK_log` 调 `rotate` → 释放锁 → `if (!error && check_purge) auto_purge()` |
| sql/binlog.cc | `MYSQL_BIN_LOG::ordered_commit` | 组提交尾部的轮转点：`aggregate_rotate_settings` 取 `check_rotate` / `force_rotate`，先 `finish_commit(thd)` 再持 `LOCK_log` 调 `rotate`；失败置 `thd->commit_error = THD::CE_COMMIT_ERROR` |
| sql/binlog.cc | `MYSQL_BIN_LOG::process_flush_stage_queue` | 组提交 flush 阶段末尾用 `set_max_size_exceeded(...)` 标记文件是否已达 `max_size`，作为后续轮转决策的原始信号 |
| sql/binlog.cc | `Binlog_group_commit_ctx::aggregate_rotate_settings` | 汇总本组各线程的轮转设置，产出 `check_rotate` / `force_rotate` 一对布尔量 |
| sql/binlog.cc | `MYSQL_BIN_LOG::auto_purge` | 自动清理：两道前置检查 + BACKUP 锁 + `ha_flush_logs()` + `purge_logs_before_date()` |
| sql/binlog.cc | `MYSQL_BIN_LOG::auto_purge_at_server_startup` | 启动时清理：只跑 `check_auto_purge_conditions()`，不取 BACKUP 锁 |
| sql/binlog.cc | `check_auto_purge_conditions` | 自动清理开关：`!opt_binlog_expire_logs_auto_purge` 或两个保留窗口变量都为 0 时返回 true（禁止清理） |
| sql/binlog.cc | `check_purge_conditions` | 清理前置校验：只检查 `log.is_open()`，未打开返回 `(true, 0)` |
| sql/binlog.cc | `calculate_auto_purge_lower_time_bound` | 计算保留期下界：`binlog_expire_logs_seconds` 优先，其次 `expire_logs_days * SECONDS_IN_24H` |
| sql/binlog.cc | `purge_source_logs_to_file` | `PURGE BINARY LOGS TO` 入口：备份锁守卫后调 `purge_logs()`，`auto_purge=false` |
| sql/binlog.cc | `purge_source_logs_before_date` | `PURGE BINARY LOGS BEFORE` 入口：备份锁守卫后调 `purge_logs_before_date()`，`auto_purge=false` |
| sql/binlog.cc | `MYSQL_BIN_LOG::set_max_size` | 运行期改写 `max_size`：加 `LOCK_log` 后 `if (is_open()) max_size = max_size_arg;` |
| sql/binlog.cc | `MYSQL_BIN_LOG::adjust_linfo_offsets` | 索引被截断后修正各读取线程记录的 `index_file_offset` |
| sql/binlog.cc | `MYSQL_BIN_LOG::Binlog_ofile::get_real_file_size` | 真实文件大小 = `m_position + m_encrypted_header_size`，含加密头 |
| sql/binlog.cc | `MYSQL_BIN_LOG::open_binlog` | 打开/新建 binlog 文件；`max_size = max_size_arg` 落在这里；`write_file_name_to_index_file` 为真时调 `add_log_to_index` |
| sql/sql_reload.cc | `handle_reload_request` | `REFRESH_BINARY_LOG` 分支调 `mysql_bin_log.rotate_and_purge(thd, true)`；无 THD 时新建临时 THD |
| sql/sql_yacc.yy | `BINARY_SYM LOGS_SYM` 语法动作 | `FLUSH BINARY LOGS` 置 `REFRESH_BINARY_LOG` |
| sql/rpl_log_encryption.cc | `Key_rotation_step::ROTATE_LOGS` | 密钥轮换时调 `mysql_bin_log.rotate_and_purge(current_thd, true)` 后再 `reencrypt_logs()` |
| sql/mysqld.cc | `mysql_bin_log.open_binlog(opt_bin_logname, nullptr, max_binlog_size, ...)` | 启动路径把系统变量 `max_binlog_size` 传给 `open_binlog()`，成为 `max_size` 初值 |

## 与主核 / AI 的关系

binlog 轮转与清理在 MySQL 里不是「文件管理」这种边缘功能，而是**复制链路的生命周期管理器**。

- 对复制链路而言：从库通过 `com_binlog_dump` 读 binlog（L04），而**删除判定必须知道「谁正在读」**。`log_in_use()` 遍历 `log_info_set` 统计读者数，正是这条链路的直接体现——删错了会直接把从库的 dump 流打断。清理逻辑因此对「活动文件」和「被占用文件」都保守停止，而不是尽力删除。
- 对主核（存储引擎）而言：轮转不是纯 IO 操作，它要**先 `ha_flush_logs(true)` 把引擎侧待写记录刷出**，再 `save_gtids_of_last_binlog_into_table()` 把 GTID 集合落表，最后才建新文件。`new_file_impl()` 开头等 `get_prep_xids()` 归零，也是为了让 2PC 的 prepared 事务先落定。**binlog 的文件边界是被引擎提交节奏约束的**，不是纯按字节数切。
- 对 AI / CDC 链路而言：binlog 文件被删除后，AI 侧若尚未消费完就断了输入。`purge_logs()` 里「被从库占用就停止删除」的语义对 AI 消费端并不自动成立——`log_in_use()` 只遍历 `log_info_set` 统计读者数，而往里登记的入口是 `MYSQL_BIN_LOG::register_log_info()`（`sql/binlog.cc` 的 `SHOW BINLOG EVENTS` 路径调用它，用 `unregister_log_info()` 注销）；AI 自己的 binlog 消费器不走这条登记路径。

**架构推论：** 因此，AI 侧要独立维护「已消费到哪个 binlog 文件/位点」的水位，并与保留期对齐；否则保留期短于 AI 消费延迟时，AI 会遇到文件已消失的缺口。这一条是从 `log_in_use()` 的判定依据（只有 `log_info_set` 里的复制线程被计数）加上 `purge_logs_before_date()` 的 mtime 判据推出来的，源码并未直接讨论非复制型消费方。

**架构推论：** `rotate()` 的四个判据里没有任何一个「文件必须写满才切」——判据是「当前大小 ≥ `max_size`」，且 `rotate()` 在 `ordered_commit()` 里是**整个事务组提交完之后**才被调用（`finish_commit(thd)` 之后）。因此单个 binlog 文件可以大于 `max_size`：一个大事务会整体写进当前文件，写完才触发轮转。源码里 relay log 轮转处的注释把这条语义写得很直白——「A transaction is written in one chunk to the binary log, so it is never split between several binary logs. Therefore, if you have big transactions, you might see binary log files larger than max_binlog_size.」这是从 `rotate()` 的判据形式与 `ordered_commit()` 中的调用位置推出来的，源码本身没有对二进制日志的该行为作直接陈述。

## 参考

- [MySQL 参考手册：二进制日志（Binary Log）](https://dev.mysql.com/doc/refman/8.0/en/binary-log.html) —— 二进制日志整体语义的官方说明，是本页「文件 / 索引文件 / 轮转」三元结构的官方口径。
- [MySQL 参考手册：二进制日志选项（`max_binlog_size`、`binlog_expire_logs_seconds`、`binlog_expire_logs_auto_purge`）](https://dev.mysql.com/doc/refman/8.0/en/replication-options-binary-log.html) —— 轮转阈值与保留期相关系统变量的官方定义与取值范围。
- [MySQL 参考手册：`PURGE BINARY LOGS` 语句](https://dev.mysql.com/doc/refman/8.0/en/purge-binary-logs.html) —— 手动清理语句的官方语义，与本页 `purge_source_logs_to_file` / `purge_source_logs_before_date` 两个入口对应。
- [MySQL 参考手册：`FLUSH BINARY LOGS` 语句](https://dev.mysql.com/doc/refman/8.0/en/flush-binary-logs.html) —— 显式触发轮转的官方语义，对应本页 `rotate_and_purge` 的调用路径。
- [MySQL 参考手册：服务器系统变量（`max_binlog_size`、`binlog_expire_logs_seconds`、`expire_logs_days`）](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html) —— 本页 `max_size` 与保留窗口计算所用变量的官方说明。
- [MySQL Server 源码：`sql/binlog.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/binlog.cc) —— 本页全部三个 refs 符号（`MYSQL_BIN_LOG::rotate` / `MYSQL_BIN_LOG::purge_logs` / `MYSQL_BIN_LOG::add_log_to_index`）及 `new_file_impl`、`purge_logs_before_date`、`auto_purge`、`move_crash_safe_index_file_to_index_file` 的真实实现。
- [MySQL Server 源码：`sql/sql_reload.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/sql_reload.cc) —— `REFRESH_BINARY_LOG` / `REFRESH_LOG` 分支如何调用 `mysql_bin_log.rotate_and_purge(thd, true)`。
- [MySQL Server 源码：`sql/rpl_log_encryption.cc`（8.4 分支）](https://github.com/mysql/mysql-server/blob/8.4/sql/rpl_log_encryption.cc) —— 密钥轮换场景下调用 `rotate_and_purge` 后 `reencrypt_logs()` 的实现。
