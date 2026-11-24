---
project: mysql-server
feature: handler-api
page_type: feature
schema_version: 1
refs:
  - path: sql/handler.cc
    symbols: [ha_open, handler::ha_open, handler::ha_write_row]
---

# handler 存储引擎接口

## 一句话职责

handler 存储引擎接口是 MySQL server 层与可插拔存储引擎之间的唯一调用面：server 层通过 `handler` 类的公有方法（`ha_open` / `ha_rnd_next` / `ha_index_read_map` / `ha_write_row` / `ha_update_row` / `ha_delete_row` / `ha_external_lock` / `ha_create` / `ha_delete_table` 等）把「打开表、定位行、读写行、加锁、建表删表」这些动作转交给引擎的具体实现，而 `sql/handler.cc` 中的 `handler::ha_*` 方法负责在真正下发到引擎虚函数之前补齐 server 侧的前置条件——断言锁状态、标记事务为读写、包裹 Performance Schema 计时、注入 binlog 行事件——并在引擎返回错误码之后统一做错误分类与提示。

## 机制是什么

MySQL 的存储引擎不是以「继承某个基类、覆盖若干方法」的方式接入的，而是**双向绑定**的：引擎侧注册一个 `handlerton`（`sql/handler.cc` 的 `get_new_handler()` 通过 `db_type->create(...)` 拿到 `handler` 实例），server 侧只认 `handler` 这个抽象基类的公有方法，引擎只需实现基类里的虚函数（`open` / `rnd_next` / `index_read_map` / `write_row` / `update_row` / `delete_row` / `external_lock` / `create` / `delete_table` 等）。

**`handler::ha_*` 这一层不是简单转发，它承担三类 server 侧职责。**

1. **状态与锁的前置断言**。`handler::ha_write_row()` 的函数体第一行是 `assert(table_share->tmp_table != NO_TMP_TABLE || m_lock_type == F_WRLCK);`——写入行之前必须已持有写锁，或者这是一张临时表（临时表不参与事务锁）。`handler::ha_rnd_next()` 与 `handler::ha_index_read_map()` 的对应断言是 `assert(table_share->tmp_table != NO_TMP_TABLE || m_lock_type != F_UNLCK);`，即读路径要求「已加锁（任意类型）或非临时表」。`handler::ha_external_lock()` 里还有两条更强的断言：`assert(next_insert_id == 0);`（用于验证 `get_auto_increment()` 拿到的自增区间已由 `ha_release_auto_increment()` 归还）与「连续加锁而不解锁」是不允许的。

2. **事务状态标记**。`handler::mark_trx_read_write()` 的函数体注释写明：「当存储引擎方法被调用时，事务必须已经启动，除非是 DDL 调用——DDL 由存储引擎内部启动并提交事务，不登记在 ha_list 中。」函数体因此先判 `ha_info->is_started()`，再判 `table_share == nullptr || table_share->tmp_table == NO_TMP_TABLE`（**TempTable 与 Heap 表不支持也不使用事务**），才调 `ha_info->set_trx_read_write()`。所有写路径（`ha_write_row` / `ha_update_row` / `ha_delete_row`）以及 DDL 路径（`ha_create` / `ha_delete_table` / `ha_drop_table` / `ha_repair`）都先调它。

3. **观测与副作用的挂载点**。写路径被 `MYSQL_TABLE_IO_WAIT(PSI_TABLE_WRITE_ROW, MAX_KEY, error, { error = write_row(buf); })` 包住，读路径被 `MYSQL_TABLE_IO_WAIT(PSI_TABLE_FETCH_ROW, ...)` 包住，外部锁被 `MYSQL_TABLE_LOCK_WAIT(PSI_TABLE_EXTERNAL_LOCK, lock_type, ...)` 包住——这三者分别是 Performance Schema 的「表 I/O 等待」「表锁等待」两类事件的采样点。写路径还要在引擎写成功之后调 `binlog_log_row(table, nullptr, buf, log_func)` 把行变化写进 binlog。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责 server 层与存储引擎之间的接口约定：打开表、读写行、索引操作、统计信息上报；**不负责某个引擎的内部实现**（G/K 组），**不负责优化器怎么用这些接口**（C 组）。

**接口层同时定义了「哪些错误可以忽略」。** `handler::is_ignorable_error(int error)` 的函数体是一个封闭的 switch，只承认四种：`0`（不是错误）、`HA_ERR_FOUND_DUPP_KEY`、`HA_ERR_FOUND_DUPP_UNIQUE`、`HA_ERR_ROW_IS_REFERENCED`、`HA_ERR_NO_REFERENCED_ROW`；其余一律返回 `false`。配套的 `handler::is_fatal_error(int error)` 先调 `is_ignorable_error`（可忽略的一定不致命），再单独把三个错误判为非致命：`HA_ERR_LOCK_WAIT_TIMEOUT`、`HA_ERR_LOCK_DEADLOCK`（注释说明这两者会导致事务/语句回滚，因此 `THD::is_fatal_sub_stmt_error` 会被置位，存储程序与触发器里的 handler 也无法捕获）、`HA_ERR_NULL_IN_SPATIAL`。

**错误码到用户消息的映射是集中注册的。** `handler::ha_init_errors()` 用 `SETMSG(nr, msg)` 宏把 `HA_ERR_*` 逐个填进 `handler_errmsgs` 数组（数组长度常量 `HA_ERR_ERRORS`，下标基准 `HA_ERR_FIRST`），末尾调 `my_error_register(get_handler_errmsg, HA_ERR_FIRST, HA_ERR_LAST)`。同一函数里也把「引擎错误码 → 引擎提供的可读文本」的转发放在 `handler::get_handler_errmsg(int nr)` 里。

**「打开表」这件事在接口层有两条路径：读失败降级为只读。** `handler::ha_open()` 的函数体先调虚函数 `open(name, mode, test_if_locked, table_def)`；若返回 `EACCES` 或 `EROFS`，且请求的 `mode == O_RDWR`，且 `table->db_stat` 带 `HA_TRY_READ_ONLY` 位，则置 `table->db_stat |= HA_READ_ONLY` 并**用 `O_RDONLY` 重试一次**。这是接口层唯一的内置降级路径。

## 怎么实现的

### 打开表：`handler::ha_open()`

`handler::ha_open(TABLE *table_arg, const char *name, int mode, int test_if_locked, const dd::Table *table_def)` 的函数体按顺序做五件事：

1. 绑定并断言前置状态：`table = table_arg;`、`assert(table->s == table_share);`、`assert(m_lock_type == F_UNLCK);`。
2. 选内存根：`MEM_ROOT *mem_root = (test_if_locked & HA_OPEN_TMP_TABLE) ? &table->s->mem_root : &table->mem_root;`，随后 `assert(alloc_root_inited(mem_root));`。
3. 调虚函数 `open(...)`，并执行上面说的 `O_RDWR → O_RDONLY` 降级重试。失败则 `set_my_errno(error)`（源码注释标注为 "Safeguard"）并返回。
4. 成功后做四件事：Performance Schema 侧 `m_psi = PSI_TABLE_CALL(open_table)(share_psi, this);`；若 `table->s->db_options_in_use & HA_OPTION_READ_ONLY_DATA` 则 `table->db_stat |= HA_READ_ONLY`；调 `(void)extra(HA_EXTRA_NO_READCHECK);`（源码注释写明「Not needed in SQL」）；最后 `cached_table_flags = table_flags();`。
5. 分配读游标缓冲：`if (!ref && !(ref = (uchar *)mem_root->Alloc(ALIGN_SIZE(ref_length) * 2)))` 则 `ha_close()` 并返回 `HA_ERR_OUT_OF_MEM`；否则 `dup_ref = ref + ALIGN_SIZE(ref_length);`。随后 `memset(ref, 0, ref_length);`——源码注释解释这是「给表一个确定的初始游标，即使它从不 seek 或写入」，点名了 weedout on const tables 这一场景。

`ref` 的分配是**幂等**的：`handler::clone()` 的函数体在调用 `new_handler->ha_open(...)` 之前先自己做了一次 `mem_root->Alloc(ALIGN_SIZE(ref_length) * 2)`，注释写明「否则 ha_open 会把它分配到 this->table->mem_root 上，我们就没法在 clone handler 对象销毁时回收这块内存」。`handler::ha_open()` 里的 `if (!ref && ...)` 正是为这个场景留的接口。

### 真正的调用方：`sql/table.cc` 的 `open_table_from_share()`

`sql/table.cc` 的 `open_table_from_share()` 是 `handler::ha_open()` 在 server 侧的主要调用方，函数体里的调用形态是：

```
if ((ha_err = (outparam->file->ha_open(
         outparam, share->normalized_path.str,
         (db_stat & HA_READ_ONLY ? O_RDONLY : O_RDWR),
         ((db_stat & HA_OPEN_TEMPORARY     ? HA_OPEN_TMP_TABLE
           : (db_stat & HA_WAIT_IF_LOCKED) ? HA_OPEN_WAIT_IF_LOCKED
           : (db_stat & (HA_ABORT_IF_LOCKED | HA_GET_INFO))
               ? HA_OPEN_ABORT_IF_LOCKED
               : HA_OPEN_IGNORE_IF_LOCKED) |
            ha_open_flags),
         table_def))))
```

三个参数各自带来源：**打开模式**由 `db_stat & HA_READ_ONLY` 决定 `O_RDONLY` 还是 `O_RDWR`；**锁等待策略**由四个互斥分支按优先级选出 `HA_OPEN_TMP_TABLE` / `HA_OPEN_WAIT_IF_LOCKED` / `HA_OPEN_ABORT_IF_LOCKED` / `HA_OPEN_IGNORE_IF_LOCKED`，再或上调用方传入的 `ha_open_flags`；**表定义**是 `dd::Table*`，为 `nullptr` 时函数体先 `thd->dd_client()->acquire(share->db.str, share->table_name.str, &table_def)` 取，取不到则 `error = 1; set_my_errno(ENOENT);`。

`ha_open` 返回错误码后，函数体按 `switch (ha_err)` 分四类处理：

- `HA_ERR_TABLESPACE_MISSING` → `error = 1`（表空间缺失，对应 InnoDB 的 .ibd 丢失或表空间头损坏）；
- `HA_ERR_NO_SUCH_TABLE` → `error = 1; set_my_errno(ENOENT);`（注释写明「在存储引擎里表不存在，用与 .frm 文件不存在相同的错误消息」）；
- `EMFILE` → `error = 1; set_my_errno(EMFILE);`（注释写明「打开的文件过多，用与 .frm 文件打不开相同的错误消息」）；
- `default` → `outparam->file->print_error(ha_err, MYF(0)); error_reported = true;`，并且**只有两个错误码会进一步细分 `error` 值**：`HA_ERR_TABLE_DEF_CHANGED` → `error = 7`，`HA_ERR_ROW_FORMAT_CHANGED` → `error = 8`。

第 4 类的第二个分支（`error = 7` / `error = 8`）有专门的上层处理：`sql/sql_base.cc` 的 `open_table_from_share()` 调用点在 `error == 8` 时新建一个 handler、调 `file->get_real_row_type(&create_info)` 求出正确的行格式，再调 `dd::fix_row_type(thd, table_def, correct_row_type)` 修正数据字典，成功则 `trans_rollback_stmt` + `trans_rollback` 或 `trans_commit_stmt` + `trans_commit`，并把该 share 从表缓存里 `tdc_remove_table(...)` 掉以便下次重建。

还有一条独立的 `crashed` 标记：`share->crashed = ((ha_err == HA_ERR_CRASHED_ON_USAGE) && outparam->file->auto_repair() && !(ha_open_flags & HA_OPEN_FOR_REPAIR));`——引擎报告「使用时崩溃」且允许自动修复、且本次不是以修复模式打开时，才把 share 标记为 crashed。

### 写行：`handler::ha_write_row()`

`handler::ha_write_row(uchar *buf)` 的函数体只有十几行，但顺序是硬约束：

```
int handler::ha_write_row(uchar *buf) {
  int error;
  Log_func *log_func = Write_rows_log_event::binlog_row_logging_function;
  assert(table_share->tmp_table != NO_TMP_TABLE || m_lock_type == F_WRLCK);

  DBUG_TRACE;
  DBUG_EXECUTE_IF("inject_error_ha_write_row", return HA_ERR_INTERNAL_ERROR;);
  DBUG_EXECUTE_IF("simulate_storage_engine_out_of_memory",
                  return HA_ERR_SE_OUT_OF_MEMORY;);
  mark_trx_read_write();

  DBUG_EXECUTE_IF(
      "handler_crashed_table_on_usage",
      my_error(HA_ERR_CRASHED, MYF(ME_ERRORLOG), table_share->table_name.str);
      set_my_errno(HA_ERR_CRASHED); return HA_ERR_CRASHED;);

  MYSQL_TABLE_IO_WAIT(PSI_TABLE_WRITE_ROW, MAX_KEY, error,
                      { error = write_row(buf); })

  if (unlikely(error)) return error;

  if (unlikely((error = binlog_log_row(table, nullptr, buf, log_func))))
    return error; /* purecov: inspected */

  DEBUG_SYNC_C("ha_write_row_end");
  return 0;
}
```

四个要点：

1. **锁断言**：写入前必须 `m_lock_type == F_WRLCK`（写锁）或表是临时表。
2. **三个故障注入点**：`inject_error_ha_write_row` → `HA_ERR_INTERNAL_ERROR`；`simulate_storage_engine_out_of_memory` → `HA_ERR_SE_OUT_OF_MEMORY`；`handler_crashed_table_on_usage` → 记 `my_error(HA_ERR_CRASHED, MYF(ME_ERRORLOG), table_share->table_name.str)` 并设 `set_my_errno(HA_ERR_CRASHED)`。
3. **引擎写失败就立即返回，binlog 不写**：`if (unlikely(error)) return error;` 在 `binlog_log_row` 之前——这是行格式 binlog 的基本保证（引擎没写成功的行不会进 binlog）。
4. **binlog 的 `Log_func` 是按操作类型选的**：`Write_rows_log_event::binlog_row_logging_function`。对照三个同族函数：`handler::ha_update_row()` 用 `Update_rows_log_event::binlog_row_logging_function` 并传 `(table, old_data, new_data, log_func)`；`handler::ha_delete_row()` 用 `Delete_rows_log_event::binlog_row_logging_function` 并传 `(table, buf, nullptr, log_func)`。

`handler::ha_update_row()` 额外有两条对引擎的布局要求：`assert(new_data == table->record[0]);` 与 `assert(old_data == table->record[1]);`，函数体注释写明「有些存储引擎要求新记录在 record[0]、旧记录在 record[1]」。

`handler::ha_delete_row()` 同样有布局断言：`assert(buf == table->record[0] || buf == table->record[1]);`（注释：通常用 record[0]，有时用 record[1]）。

### 调用方对 `ha_write_row` 返回码的处理

`sql/sql_insert.cc` 的 `write_record()` 是最典型的调用方，函数体里的循环形态是：

```
while ((error = table->file->ha_write_row(table->record[0]))) {
  uint key_nr;
  if (table->file->insert_id_for_cur_row > 0)
    insert_id_for_cur_row = table->file->insert_id_for_cur_row;
  else
    table->file->insert_id_for_cur_row = insert_id_for_cur_row;
  bool is_duplicate_key_error;
  if (!table->file->is_ignorable_error(error)) goto err;
  is_duplicate_key_error =
      (error == HA_ERR_FOUND_DUPP_KEY || error == HA_ERR_FOUND_DUPP_UNIQUE);
  ...
```

三点值得注意：

- **重试循环的守卫是 `is_ignorable_error(error)`**——不可忽略的错误直接 `goto err`，不重试。
- **重复键的判定是显式的两码比较**：`HA_ERR_FOUND_DUPP_KEY` 或 `HA_ERR_FOUND_DUPP_UNIQUE`，而不是「不可忽略」的反面。函数体注释写明这一分支的差别：「我们走到了这里，说明错误是可忽略的但不是重复键错误……如果是 IGNORE 选项，handler 错误会被降级为警告，不必停止迭代。」
- **取重复键序号走 `handler::get_dup_key(int error)`**：该函数体在 `error == HA_ERR_FOUND_DUPP_KEY || error == HA_ERR_FOUND_DUPP_UNIQUE || error == HA_ERR_NULL_IN_SPATIAL || error == HA_ERR_DROP_INDEX_FK` 时调 `table->file->info(HA_STATUS_ERRKEY | HA_STATUS_NO_LOCK);`，再返回 `table->file->errkey`。调用方拿到负值就改判为 `HA_ERR_FOUND_DUPP_KEY`（注释：「数据库找不到键」）。

### 读路径：`ha_rnd_next` / `ha_index_read_map`

`handler::ha_rnd_next(uchar *buf)` 与 `handler::ha_rnd_pos(uchar *buf, uchar *pos)` 的函数体结构完全一致：

1. `m_update_generated_read_fields = table->has_gcol();`——有生成列时才需要在读后回填。
2. `MYSQL_TABLE_IO_WAIT(PSI_TABLE_FETCH_ROW, MAX_KEY, result, { result = rnd_next(buf); })`（`rnd_pos` 同形）。
3. 读成功后若 `m_update_generated_read_fields` 则 `update_generated_read_fields(buf, table)` 并把标志复位为 `false`。
4. `table->set_row_status_from_handler(result);` 把行状态交回 TABLE。

`handler::ha_index_read_map()` 的函数体多一段**多值索引的去重过滤**：

```
if (!result && !mrr_have_range && m_unique != nullptr && filter_dup_records())
  result = HA_ERR_KEY_NOT_FOUND;
```

源码注释说明：`m_unique != nullptr` 意味着这是多值索引读；范围扫描情形下的重复记录过滤在 `multi_range_read_next()` 里做。

`handler::ha_index_read_last_map()` 是反向读，函数体里对应的是「加入重复记录过滤」而非「过滤」：`if (!result && !mrr_have_range && m_unique != nullptr) filter_dup_records();`。

### 索引与随机扫描的初始化：`ha_index_init` / `ha_rnd_init`

`handler::ha_index_init(uint idx, bool sorted)` 的函数体：`if (!(result = index_init(idx, sorted))) inited = INDEX;`，然后 `mrr_have_range = false; end_range = nullptr;`。

`handler::ha_rnd_init(bool scan)` 的函数体：`inited = (result = rnd_init(scan)) ? NONE : RND;`，同样复位 `end_range = nullptr;`。

`handler::ha_index_end()` / `handler::ha_rnd_end()` 则把 `inited` 复位为 `NONE`、`end_range = nullptr`、`m_record_buffer = nullptr`，`ha_index_end` 额外调 `if (m_unique) m_unique->reset(false);`。

**`inited` 状态机是接口层的一致性守卫**：`ha_index_read_map` 断言 `inited == INDEX`，`ha_rnd_next` 断言 `inited == RND`，`ha_index_end` 断言 `inited == INDEX`，`ha_rnd_end` 断言 `inited == RND`，而 `handler::ha_reset()` 断言 `inited == NONE`（注释：「确保 ha_index_end / ha_rnd_end 已被调用」）。

### 统计信息上报：`handler::records()`

`handler::records(ha_rows *num_rows)` 的函数体给出两条互斥路径：

- 若 `ha_table_flags() & HA_COUNT_ROWS_INSTANT`，直接 `*num_rows = stats.records; return 0;`——引擎能瞬时给出精确行数，不做扫描。
- 否则走全表扫描：`start_psi_batch_mode()` → `ha_rnd_init(true)` → 循环 `ha_rnd_next(table->record[0])` 累加 `++rows`，循环条件是 `!table->in_use->killed`（被 kill 就退出）→ `end_psi_batch_mode()`。

这条路径的收尾逻辑值得注意：`if (error != HA_ERR_END_OF_FILE) *num_rows = HA_POS_ERROR;`——**只有正常读到文件尾，行数才有效**；否则置 `HA_POS_ERROR`。此外扫描中途若 `ha_rnd_next` 返回 `HA_ERR_RECORD_DELETED` 则 `continue`（跳过该行），其他错误 `break`。

### DDL 路径与锁路径

`handler::ha_create(const char *name, TABLE *form, HA_CREATE_INFO *info, dd::Table *table_def)` 的函数体只有三行有效逻辑：`assert(m_lock_type == F_UNLCK); mark_trx_read_write(); return create(name, form, info, table_def);`。`handler::ha_delete_table()` 与 `handler::ha_drop_table()` 同形，只是虚函数分别是 `delete_table` 与 `drop_table`。

`handler::ha_external_lock(THD *thd, int lock_type)` 的函数体：先 `assert(next_insert_id == 0);`，再断言「连续加锁而不解锁」不被允许（`lock_type != F_UNLCK && m_lock_type == F_UNLCK`，或 `lock_type == F_UNLCK`），再断言 `inited == NONE || table->open_by_handler`（注释：「SQL HANDLER 调用会在扫描期间（RND/INDEX）加锁/解锁」），然后 `ha_statistic_increment(&System_status_var::ha_external_lock_count);`，再 `MYSQL_TABLE_LOCK_WAIT(PSI_TABLE_EXTERNAL_LOCK, lock_type, { error = external_lock(thd, lock_type); })`。

**锁成功后才刷新表标志缓存**，这是 `handler::ha_open()` 与 `handler::ha_external_lock()` 之间的一处一致性设计：

```
if (error == 0) {
  m_lock_type = lock_type;
  cached_table_flags = table_flags();
}
```

`handler::ha_external_lock()` 的注释补充了失败时的语义：「如果加锁成功，我们缓存表标志；否则保持它们在 ha_open() 中取到的样子。」而 `handler::ha_open()` 里那句 `cached_table_flags = table_flags();` 正是这个「初始快照」的来源——两处赋值构成一对。

### 引擎实例的创建：`get_new_handler()`

`get_new_handler(TABLE_SHARE *share, bool partitioned, MEM_ROOT *alloc, handlerton *db_type)` 的函数体给出降级链：

```
if (db_type && db_type->state == SHOW_OPTION_YES && db_type->create) {
  if ((file = db_type->create(db_type, share, partitioned, alloc)))
    file->init();
  return file;
}
return get_new_handler(share, partitioned, alloc,
                       ha_default_handlerton(current_thd));
```

**引擎未安装（`state != SHOW_OPTION_YES`）或没有 `create` 回调时，回退到 `ha_default_handlerton(current_thd)` 指定的默认引擎**，函数头注释写明「若通过 db_type 参数指定的引擎不可用，本函数会尝试使用默认存储引擎」。注意 `file->init()` 只在 `create` 返回非空时调用。

`sql/table.cc` 的 `open_table_from_share()` 在建 handler 时传的引擎是 `share->db_type()`，并在传参时用 `share->m_part_info != nullptr` 告知是否分区表——**分区表的 handler 由引擎自己再包一层分区 handler**。

## 关键取舍

- **源码事实：接口层与引擎层之间是一层「厚转发」，不是薄转发。** `handler::ha_write_row()` 在引擎的 `write_row()` 前后各加了三件事：前置的锁断言与事务标记、后置的 binlog 行事件与 `DEBUG_SYNC_C("ha_write_row_end")` 同步点。收益是引擎实现者只需关心「把这一行写进存储」，不必重复实现 binlog、事务标记与观测采样；代价是 server 层的语义（binlog 顺序、事务可见性、PSI 采样）无法在引擎侧定制，且这层转发的开销落在每一次行操作上。

- **源码事实：`handler::ha_open()` 把「可写打开失败」降级成「只读打开」，但只降级一次，且降级条件是三个与运算。** 函数体是 `if ((error == EACCES || error == EROFS) && mode == O_RDWR && (table->db_stat & HA_TRY_READ_ONLY))`。收益是只读介质（如只读挂载的数据目录）上的表仍能读；代价是 `EACCES` 有两种完全不同的成因（权限不足 vs 只读挂载），降级会把前者的权限错误也变成只读成功，需要靠 `table->db_stat |= HA_READ_ONLY` 这个标记把「本来就想只读」与「降级成只读」区分开。

- **源码事实：错误码分类被压缩成三套固定清单，而不是交给引擎自由表达。** `is_ignorable_error()` 只承认 4 个错误码，`is_fatal_error()` 只额外承认 3 个非致命错误码，其余一律按「不可忽略且致命」处理。收益是 server 层的重试/忽略/回滚策略是一张可穷举的表，不用逐引擎协商；代价是引擎想表达一种新的「可忽略」错误，必须先在 server 层加分支。

- **源码事实：同一份错误码有两套消息来源。** `ha_init_errors()` 用 `SETMSG` 把 `HA_ERR_*` 映射到 server 层的错误消息（如 `HA_ERR_FOUND_DUPP_KEY` → `ER_DEFAULT(ER_DUP_KEY)`，`HA_ERR_KEY_NOT_FOUND` → `ER_DEFAULT(ER_KEY_NOT_FOUND)`），同时 `handler::get_handler_errmsg(int nr)` 提供让引擎自己给消息的通道；`handler::print_error()` 负责在两者之间协调。收益是常见错误有统一措辞、引擎特有错误仍有出口；代价是多一层间接，读错误消息要同时看两个来源。

- **源码事实：`ha_write_row` 的错误注入点有三个，覆盖三种不同的失败语义。** `inject_error_ha_write_row`（`HA_ERR_INTERNAL_ERROR`）测的是通用写失败；`simulate_storage_engine_out_of_memory`（`HA_ERR_SE_OUT_OF_MEMORY`）测的是引擎内存耗尽；`handler_crashed_table_on_usage`（`HA_ERR_CRASHED`）测的是「表已损坏」，且只有第三个会调 `my_error(..., MYF(ME_ERRORLOG), ...)` 落错误日志并 `set_my_errno`。收益是测试能区分「写不进去」与「表坏了」这两类完全不同的用户可见行为；代价是这三个开关硬编码在接口层，不是配置项。

- **源码事实：`ha_open` 里 `ref` 的分配做了幂等处理，代价是 clone 路径要自己先分配一次。** `handler::clone()` 的函数体在调 `ha_open` 前先 `mem_root->Alloc(ALIGN_SIZE(ref_length) * 2)`，注释写明原因是要在 clone handler 销毁时能回收内存。收益是 `ha_open` 对「ref 已分配」与「ref 未分配」两种情形都正确；代价是同一块缓冲的分配逻辑分散在两个函数里，且 `ALIGN_SIZE(ref_length) * 2` 这个尺寸表达式出现了两次（`clone()` 与 `ha_open()`）。

- **源码事实：行数统计在「引擎能瞬时给出」与「全表扫描」之间二选一，判据是表标志位 `HA_COUNT_ROWS_INSTANT`。** `handler::records()` 的函数体第一条分支就是 `if (ha_table_flags() & HA_COUNT_ROWS_INSTANT)`。收益是 InnoDB 这类引擎能零成本给出精确行数，避免全表扫描；代价是标志位由引擎自己设置，server 层无法核验其真实性，且走扫描分支时统计结果受 `killed` 中断影响——非正常结束一律置 `HA_POS_ERROR`。

- **源码事实：binlog 的行事件类型在接口层按操作类型硬编码。** `ha_write_row` 用 `Write_rows_log_event::binlog_row_logging_function`，`ha_update_row` 用 `Update_rows_log_event::binlog_row_logging_function`，`ha_delete_row` 用 `Delete_rows_log_event::binlog_row_logging_function`。收益是引擎完全不参与 binlog 格式；代价是接口层成为行格式 binlog 的耦合点——任何新增的行级操作都要在这里加一个分支与一个 `Log_func`。

- **架构推论：** 把「binlog 写入」放在引擎写成功之后、接口层之内（而不是引擎内部或 server 层更上层），决定了行格式 binlog 的事务性语义：引擎写失败则该行不进 binlog，而 binlog 写失败则整个语句失败并回滚。`sql/handler.cc` 中 `binlog_log_row()` 的函数体也印证了这一点——它内部先 `write_locked_table_maps(thd)` 补齐表映射事件，再调 `(*log_func)(thd, table, has_trans, before_record, after_record)`，而 `has_trans` 的取值是 `thd->lex->sql_command == SQLCOM_CREATE_TABLE || table->file->has_transactions()`，注释写明这是为了兼容基于语句的复制。

- **架构推论：** `handler` 作为唯一抽象面，意味着「加一种新的存储能力」的边际成本是固定的——在 `handler` 基类加一个虚函数、在 `sql/handler.cc` 加一个 `ha_*` 转发、在每个引擎实现一遍。MySQL 选择这条路而不是给引擎一份面向查询的接口（让引擎自己决定怎么执行），换来的是 server 层对计划、事务、binlog、权限的完全掌控，代价是引擎无法为特定查询形态做端到端优化。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/handler.cc` | `get_new_handler` | 引擎实例创建与降级：引擎 `state == SHOW_OPTION_YES` 且有 `create` 回调时用 `db_type->create(...)` 并 `file->init()`，否则回退到 `ha_default_handlerton(current_thd)` 指定的默认引擎 |
| `sql/handler.cc` | `handler::ha_open` | 打开表：`table = table_arg` 与三条断言、按 `HA_OPEN_TMP_TABLE` 选内存根、调虚函数 `open(...)`、`EACCES`/`EROFS` + `O_RDWR` + `HA_TRY_READ_ONLY` 时降级重试为 `O_RDONLY`、PSI `open_table`、`HA_OPTION_READ_ONLY_DATA` 置 `HA_READ_ONLY`、`extra(HA_EXTRA_NO_READCHECK)`、分配 `ref`/`dup_ref` 各 `ALIGN_SIZE(ref_length)`、`memset(ref, 0, ref_length)`、`cached_table_flags = table_flags()` |
| `sql/handler.cc` | `handler::ha_write_row` | 写行：断言写锁或临时表、三个故障注入点、`mark_trx_read_write()`、`MYSQL_TABLE_IO_WAIT(PSI_TABLE_WRITE_ROW, MAX_KEY, ...)` 包裹 `write_row(buf)`、引擎写失败立即返回不进 binlog、成功后 `binlog_log_row(table, nullptr, buf, Write_rows_log_event::binlog_row_logging_function)` |
| `sql/handler.cc` | `handler::ha_update_row` | 更新行：断言 `new_data == table->record[0]` 且 `old_data == table->record[1]`（引擎布局要求）、`PSI_TABLE_UPDATE_ROW` 包裹 `update_row`、成功后 `binlog_log_row(table, old_data, new_data, Update_rows_log_event::binlog_row_logging_function)` |
| `sql/handler.cc` | `handler::ha_delete_row` | 删除行：断言 `buf` 落在 `table->record[0]` 或 `record[1]`、`inject_error_ha_delete_row` 与 `simulate_error_ha_delete_row_lock_wait_timeout` 两个注入点、`PSI_TABLE_DELETE_ROW` 包裹 `delete_row`、成功后 `binlog_log_row(table, buf, nullptr, Delete_rows_log_event::binlog_row_logging_function)` |
| `sql/handler.cc` | `handler::ha_rnd_next` | 随机扫描取行：断言已加锁或非临时表且 `inited == RND`、`m_update_generated_read_fields = table->has_gcol()`、`PSI_TABLE_FETCH_ROW` 包裹 `rnd_next(buf)`、生成列回填 `update_generated_read_fields(buf, table)`、`table->set_row_status_from_handler(result)` |
| `sql/handler.cc` | `handler::ha_rnd_pos` | 按位置取行：与 `ha_rnd_next` 同形，虚函数为 `rnd_pos(buf, pos)` |
| `sql/handler.cc` | `handler::ha_index_read_map` | 索引等值读：断言 `inited == INDEX`、断言 `!pushed_idx_cond || buf == table->record[0]`、`PSI_TABLE_FETCH_ROW` 包裹 `index_read_map(buf, key, keypart_map, find_flag)`、多值索引去重 `filter_dup_records()` 命中则改判 `HA_ERR_KEY_NOT_FOUND` |
| `sql/handler.cc` | `handler::ha_index_read_last_map` | 索引反向读：虚函数为 `index_read_last_map(buf, key, keypart_map)`，多值索引只做 `filter_dup_records()` 入过滤集 |
| `sql/handler.cc` | `handler::ha_index_init` | 索引扫描初始化：`index_init(idx, sorted)` 成功后置 `inited = INDEX`，并复位 `mrr_have_range = false; end_range = nullptr;` |
| `sql/handler.cc` | `handler::ha_rnd_init` | 随机扫描初始化：`rnd_init(scan)` 成功后置 `inited = RND`，否则 `NONE`，并复位 `end_range = nullptr;` |
| `sql/handler.cc` | `handler::ha_index_end` / `handler::ha_rnd_end` | 扫描收尾：`inited = NONE`、`end_range = nullptr`、`m_record_buffer = nullptr`，`ha_index_end` 额外 `m_unique->reset(false)` |
| `sql/handler.cc` | `handler::ha_reset` | 复位到 open 之后的状态：断言 `inited == NONE`、`free_io_cache(table)`、`table->default_column_bitmaps()`、`extra(HA_EXTRA_NO_IGNORE_DUP_KEY)` 与 `extra(HA_EXTRA_WRITE_CANNOT_REPLACE)`、`cancel_pushed_idx_cond()`、最后调虚函数 `reset()` |
| `sql/handler.cc` | `handler::ha_external_lock` | 表级外部锁：断言 `next_insert_id == 0`、断言不允许连续加锁不断开、断言 `inited == NONE || table->open_by_handler`、`ha_statistic_increment(&System_status_var::ha_external_lock_count)`、`MYSQL_TABLE_LOCK_WAIT(PSI_TABLE_EXTERNAL_LOCK, lock_type, ...)` 包裹 `external_lock`、成功后同时更新 `m_lock_type` 与 `cached_table_flags` |
| `sql/handler.cc` | `handler::ha_create` / `handler::ha_delete_table` / `handler::ha_drop_table` | DDL 路径：`assert(m_lock_type == F_UNLCK)` + `mark_trx_read_write()` 后转 `create` / `delete_table` / `drop_table` 虚函数 |
| `sql/handler.cc` | `handler::records` | 行数上报：`HA_COUNT_ROWS_INSTANT` 时直接取 `stats.records`；否则 `ha_rnd_init(true)` 全表扫描计数，遇 `HA_ERR_RECORD_DELETED` 则 `continue`，非 `HA_ERR_END_OF_FILE` 结束则行数置 `HA_POS_ERROR` |
| `sql/handler.cc` | `handler::is_ignorable_error` | 可忽略错误清单：`0`、`HA_ERR_FOUND_DUPP_KEY`、`HA_ERR_FOUND_DUPP_UNIQUE`、`HA_ERR_ROW_IS_REFERENCED`、`HA_ERR_NO_REFERENCED_ROW` 共 4 类，其余返回 `false` |
| `sql/handler.cc` | `handler::is_fatal_error` | 非致命错误清单：先排除可忽略错误，再把 `HA_ERR_LOCK_WAIT_TIMEOUT`、`HA_ERR_LOCK_DEADLOCK`、`HA_ERR_NULL_IN_SPATIAL` 判为非致命 |
| `sql/handler.cc` | `handler::get_dup_key` | 取重复键序号：`HA_ERR_FOUND_DUPP_KEY`/`HA_ERR_FOUND_DUPP_UNIQUE`/`HA_ERR_NULL_IN_SPATIAL`/`HA_ERR_DROP_INDEX_FK` 时调 `info(HA_STATUS_ERRKEY \| HA_STATUS_NO_LOCK)`，返回 `table->file->errkey` |
| `sql/handler.cc` | `handler::ha_init_errors` / `get_handler_errmsg` | 错误码到消息的集中注册：数组长度常量 `HA_ERR_ERRORS`、下标基准 `HA_ERR_FIRST`、末位 `my_error_register(get_handler_errmsg, HA_ERR_FIRST, HA_ERR_LAST)` |
| `sql/handler.cc` | `handler::mark_trx_read_write` | 事务标记：`ha_info->is_started()` 且（`table_share == nullptr` 或 `tmp_table == NO_TMP_TABLE`）时 `ha_info->set_trx_read_write()`；注释说明 TempTable 与 Heap 表不支持事务 |
| `sql/handler.cc` | `handler::clone` | handler 克隆：`get_new_handler` 后 `set_ha_share_ref(ha_share)`，**先自己**分配 `ref`（`ALIGN_SIZE(ref_length) * 2`）再调 `new_handler->ha_open(table, name, table->db_stat, HA_OPEN_IGNORE_IF_LOCKED, nullptr)` |
| `sql/handler.cc` | `binlog_log_row` | 行事件写 binlog：`check_table_binlog_row_based` 判定、`transaction_write_set_extraction != HASH_ALGORITHM_OFF` 时算写集、`binlog_row_image == 0` 时补标 `HA_NOSAME` 索引用到的列、`write_locked_table_maps(thd)` 补表映射、`has_trans` 取 `SQLCOM_CREATE_TABLE \|\| table->file->has_transactions()` 后调 `(*log_func)(...)` |
| `sql/handler.cc` | `handler::ha_statistic_increment` | 状态计数：`if (table && table->in_use) (table->in_use->status_var.*offset)++;` |
| `sql/table.cc` | `open_table_from_share` | `handler::ha_open` 的主调用方：按 `db_stat` 选 `O_RDONLY`/`O_RDWR` 与四种锁等待策略（`HA_OPEN_TMP_TABLE` / `HA_OPEN_WAIT_IF_LOCKED` / `HA_OPEN_ABORT_IF_LOCKED` / `HA_OPEN_IGNORE_IF_LOCKED`）再或上 `ha_open_flags`；按 `switch (ha_err)` 把 `HA_ERR_TABLESPACE_MISSING`/`HA_ERR_NO_SUCH_TABLE`/`EMFILE` 归为 `error = 1`，`default` 分支才调 `print_error`，且 `HA_ERR_TABLE_DEF_CHANGED`→`error = 7`、`HA_ERR_ROW_FORMAT_CHANGED`→`error = 8`；`share->crashed` 由 `HA_ERR_CRASHED_ON_USAGE` + `auto_repair()` + 非 `HA_OPEN_FOR_REPAIR` 三条件决定 |
| `sql/sql_insert.cc` | `write_record` | `ha_write_row` 的调用方形态：`while ((error = table->file->ha_write_row(table->record[0])))` 循环，`is_ignorable_error(error)` 为假即 `goto err`，重复键显式判 `HA_ERR_FOUND_DUPP_KEY`/`HA_ERR_FOUND_DUPP_UNIQUE`，`get_dup_key` 返回负值则改判 `HA_ERR_FOUND_DUPP_KEY` |
| `sql/sql_base.cc` | `open_table_from_share` 调用点 | `error == 8`（行格式变了）时的修复路径：`get_new_handler` + `file->get_real_row_type(&create_info)` + `dd::fix_row_type(thd, table_def, correct_row_type)`，随后 `trans_rollback_stmt`/`trans_rollback` 或 `trans_commit_stmt`/`trans_commit`，并 `tdc_remove_table` 移出表缓存 |
| `sql/sql_tmp_table.cc` | 临时表 handler 创建 | `get_new_handler((TABLE_SHARE *)nullptr, false, thd->mem_root, heap_hton)` 建 Heap handler；`table->file->ha_open(table, table->s->table_name.str, O_RDWR, ...)` 打开临时表 |
| `storage/innobase/handler/handler0alter.cc` | `ha_innopart::write_row_in_new_part` | 引擎侧的 handler 覆写示例：分区表改写时定位目标分区（找不到则返回 `HA_ERR_ROW_IN_WRONG_PARTITION`），再调基类实现 `ha_innobase::write_row(table->record[0])` |

## 与主核 / AI 的关系

主核（`engine/db`）的存储层目前没有 MySQL 这样的「server 层统一接口 + 引擎侧实现」的双向绑定结构：主核的 `storage/` 层是内核内部的一层，查询层直接依赖它，没有 `handler` 这样的中间抽象。

**架构推论：** MySQL 的 handler 接口层有一个值得主核借鉴的显式约束：**状态机 + 断言前置条件**。`handler::ha_write_row()` 断言写锁、`handler::ha_index_read_map()` 断言 `inited == INDEX`、`handler::ha_reset()` 断言 `inited == NONE`——这些断言把「调用顺序契约」写进了代码而不是文档。主核若有类似的存储层抽象，应当让每个操作入口都断言它依赖的初始化状态已完成，这样调用顺序错误会在开发期就崩掉，而不是在并发下表现为难以复现的数据错乱。

**架构推论：** 「错误分类清单化」这一做法对主核同样成立。MySQL 把「哪些错误可忽略」「哪些错误非致命」压缩成两张穷举的 switch 清单，server 层据此决定重试、忽略还是回滚。主核若要把存储层错误向 AI 暴露（例如让模型决定「冲突了要不要重试」），这张清单就应该被显式建模成结构化数据，而不是散落在错误码比较里。

**架构推论：** `handler::ha_write_row()` 把 binlog 写入放在引擎写成功之后、接口层之内，这个位置选择对 AI 参与写入路径有直接含义：**AI 只能看到「行写成功 / 行写失败」这个二值结果与错误码，看不到 binlog 这一层的存在**。这意味着若主核让 AI 参与事务与复制的决策，需要显式地把「引擎写成功」与「变更已持久化」这两个时刻分开建模，否则模型会把「写成功」误当作「已落盘可复制」。

对 AI 场景而言，handler 接口层的错误码体系本身是可用的信号源：`is_ignorable_error()` 的清单（重复键、外键违例）恰好对应「模型生成的 SQL 里最容易出错、且可以自动重试或降级」的那几类问题。

## 参考

- [MySQL 源码：`sql/handler.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/handler.cc) —— `handler::ha_open` 的打开与只读降级逻辑、`handler::ha_write_row` / `ha_update_row` / `ha_delete_row` 的写路径与 binlog 挂载点、`get_new_handler` 的引擎降级链、`ha_init_errors` 的错误码注册、`is_ignorable_error` / `is_fatal_error` 的错误分类清单。
- [MySQL 源码：`sql/handler.h`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/handler.h) —— `handler` 基类的虚函数声明（`open` / `rnd_next` / `index_read_map` / `write_row` / `update_row` / `delete_row` / `external_lock` / `create` / `delete_table`），即引擎需要实现的那一面。
- [MySQL 源码：`sql/table.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/table.cc) —— `open_table_from_share()` 里调用 `handler::ha_open()` 的参数构造与 `switch (ha_err)` 错误分类，`share->crashed` 的判定条件。
- [MySQL 源码：`sql/sql_insert.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_insert.cc) —— `write_record()` 对 `ha_write_row()` 返回码的重试循环、`is_ignorable_error` 与重复键判定、`get_dup_key` 的使用方式。
- [MySQL 源码：`sql/sql_base.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_base.cc) —— `error == 8`（行格式变更）时的 `get_real_row_type` + `dd::fix_row_type` 修复路径与表缓存失效处理。
- [MySQL 源码：`storage/innobase/handler/ha_innodb.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/handler/ha_innodb.cc) —— InnoDB 侧对 `handler` 虚函数的实现（`ha_innobase::open` / `write_row` / `rnd_next` / `index_read_map` / `external_lock`），即接口的另一端。
- [MySQL 源码：`storage/innobase/handler/handler0alter.cc`（GitHub mysql-8.0.40）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/storage/innobase/handler/handler0alter.cc) —— 引擎侧覆写 `handler` 行为的实例：`ha_innopart::write_row_in_new_part` 在分区改写场景下如何复用基类 `ha_innobase::write_row`。
- [MySQL 参考手册：The Pluggable Storage Engine Interface](https://dev.mysql.com/doc/refman/8.0/en/server-storage-engine-pluggable.html) —— 官方对存储引擎接口、handler 类与可插拔引擎角色的说明。
- [MySQL 参考手册：INNODB 存储引擎内部机制](https://dev.mysql.com/doc/refman/8.0/en/innodb-internals.html) —— 引擎侧（InnoDB）的内部机制背景，用于理解 handler 接口的另一端在做什么。
