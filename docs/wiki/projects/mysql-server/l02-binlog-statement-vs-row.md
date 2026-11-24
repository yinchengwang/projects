---
project: mysql-server
feature: binlog-statement-vs-row
page_type: feature
schema_version: 1
refs:
  - path: sql/log_event.cc
    symbols: [Rows_log_event::Rows_log_event, Query_log_event::Query_log_event]
---

# 语句格式与行格式

## 一句话职责

本机制负责决定一条写操作最终以**语句格式**（`Query_log_event`）还是**行格式**（`Rows_log_event` 的三个子类 `Write_rows_log_event` / `Update_rows_log_event` / `Delete_rows_log_event`）记入 binlog，并负责构造行镜像本身——即 before image / after image 的列位图与列值打包。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责决定一条写操作以语句格式还是行格式记入 binlog、以及行镜像的构造规则；**不负责事件的文件格式（L01）**、**不负责复制端的重放（M 组）**。

## 机制是什么

### 两种格式的事件长什么样

| | 语句格式 | 行格式 |
|---|---|---|
| 事件类 | `Query_log_event` | `Write_rows_log_event` / `Update_rows_log_event` / `Delete_rows_log_event`（共同基类 `Rows_log_event`） |
| 载荷内容 | SQL 原文 + 一串「status 变量」（sql_mode、字符集、时区、auto_increment 参数等） | 列位图（`m_cols` / `m_cols_ai`）+ 打包后的列值（null 位图 + 各字段值） |
| 从库重放方式 | 重新执行 SQL | 按 before image 定位行、按 after image 写回 |

`Query_log_event::write()`（`sql/log_event.cc`）把语句格式事件拆成「固定头 + 变长 status 段 + db 名 + SQL 正文」四段写入。固定头写入 `slave_proxy_id`、`exec_time`、`db_len`、`error_code`；status 段按**代码递增顺序**逐项追加，源码注释写明理由是「这样略旧的从库也能解析它认识的部分」。status 段可含的项按源码里实际写出的分支包括：`flags2`、`sql_mode`、`catalog`、`auto_increment_increment` / `auto_increment_offset`（仅当两者不同时为 1 时才写）、`charset`（3 个 2 字节编号）、`time_zone_str`、`lc_time_names`、`charset_database_number`、`table_map_for_update`、`Q_INVOKER`（执行者身份）、`Q_UPDATED_DB_NAMES`、`Q_MICROSECONDS`、`Q_EXPLICIT_DEFAULTS_FOR_TIMESTAMP`、`Q_DDL_LOGGED_WITH_XID`、`Q_DEFAULT_COLLATION_FOR_UTF8MB4`、`Q_SQL_REQUIRE_PRIMARY_KEY`、`Q_DEFAULT_TABLE_ENCRYPTION`。status 段总长记入 `status_vars_len` 并断言不超过 `MAX_SIZE_LOG_EVENT_STATUS`（该常量的定义在 `libbinlogevents` 头文件中，不在本页的源码检出范围内）。

`Rows_log_event::write_data_body()` 的写出顺序是固定的三段：`m_width`（用 `net_store_length` 编码）、`m_cols` 位图、UPDATE 事件再追加 `m_cols_ai` 位图，最后是整块行数据 `m_rows_buf`。

### 行格式的三个事件类型与它们的列位图来源

- `Write_rows_log_event` 只写 after image，位图来自 `tbl_arg->write_set`。
- `Delete_rows_log_event` 只写 before image，位图来自 `tbl_arg->read_set`。
- `Update_rows_log_event` 同时写 before image（`tbl_arg->read_set`）和 after image（`tbl_arg->write_set`）——构造时先以 `read_set` 初始化基类的 `m_cols`，再调 `init(tbl_arg->write_set)` 初始化 `m_cols_ai`。

事件类型码由 `Update_rows_log_event::get_update_rows_event_type()` 决定：`binlog_row_value_options != 0` 时用 `binary_log::PARTIAL_UPDATE_ROWS_EVENT`；否则 `log_bin_use_v1_row_events` 为真用 `binary_log::UPDATE_ROWS_EVENT_V1`，为假用 `binary_log::UPDATE_ROWS_EVENT`。`log_bin_use_v1_row_events` 定义在 `sql/mysqld.cc`，初值为 `false`。`Write_rows_log_event` 与 `Delete_rows_log_event` 的类型码同样受 `log_bin_use_v1_row_events` 控制（分别选 `WRITE_ROWS_EVENT_V1` / `WRITE_ROWS_EVENT`、`DELETE_ROWS_EVENT_V1` / `DELETE_ROWS_EVENT`）。

### 「语句格式还是行格式」这个决定在哪里做出

决定点有两层：

**第一层：语句级判定——`THD::decide_logging_format()`（`sql/binlog.cc`）。** 它在语句执行期、表被锁定后被调用（调用点见 `sql/sql_base.cc` 的 `lock_tables` 路径、`sql/sql_insert.cc` 的 `INSERT` 路径、`sql/sql_table.cc` 的 DDL 路径、`sql/sql_parse.cc`）。它读 `variables.binlog_format`（取值 `BINLOG_FORMAT_STMT` / `BINLOG_FORMAT_ROW` / `BINLOG_FORMAT_MIXED`），并遍历语句涉及的表，收集每个存储引擎的 `ha_table_flags()`，累积出两个集合：

- `flags_write_some_set`：所有被写引擎的能力**并集**。
- `flags_write_all_set`：所有被写引擎的能力**交集**，初值是 `HA_BINLOG_ROW_CAPABLE | HA_BINLOG_STMT_CAPABLE`。

判定分支（源码里编号 1～7 的注释）：

1. `flags_write_all_set` 同时不含 `HA_BINLOG_ROW_CAPABLE` 和 `HA_BINLOG_STMT_CAPABLE`（既不能行记也不能语句记）→ 报 `ER_BINLOG_ROW_ENGINE_AND_STMT_ENGINE`。
2. `flags_write_all_set` 不含 `HA_BINLOG_ROW_CAPABLE`（只有语句能力引擎）：
   - 行注入（`lex->is_stmt_row_injection()`）→ `ER_BINLOG_ROW_INJECTION_AND_STMT_ENGINE`。
   - `binlog_format == BINLOG_FORMAT_ROW` 且该命令能产生行事件 → `ER_BINLOG_ROW_MODE_AND_STMT_ENGINE`。
   - `binlog_format == BINLOG_FORMAT_MIXED` 且语句被标为 unsafe → `ER_BINLOG_UNSAFE_AND_STMT_ENGINE`。
   - 否则（unsafe 且是写语句）→ **仍然按语句格式记**，并把 unsafe 标记累加到 `binlog_unsafe_warning_flags`，后续发 `ER_BINLOG_UNSAFE_STATEMENT` 警告。
3. 无语句专属引擎、且 `binlog_format == BINLOG_FORMAT_STMT`：
   - 行注入 → `ER_BINLOG_ROW_INJECTION_AND_STMT_MODE`。
   - `flags_write_all_set` 不含 `HA_BINLOG_STMT_CAPABLE`（引擎只能行记）且命令能产生行事件 → `ER_BINLOG_STMT_MODE_AND_ROW_ENGINE`。
   - unsafe 且是写语句 → 仍按语句格式记 + 累加 unsafe 警告标记。
4. 无语句专属引擎、且 `binlog_format != BINLOG_FORMAT_STMT`（即 ROW 或 MIXED）：只要满足下列任一条件，就调 `set_current_stmt_binlog_format_row_if_mixed()` 切成行格式：
   - `lex->is_stmt_unsafe()`；
   - `lex->is_stmt_row_injection()`；
   - `lex->is_stmt_unsafe_with_mixed_mode()`；
   - `flags_write_all_set` 不含 `HA_BINLOG_STMT_CAPABLE`；
   - 语句读过临时事务表（`LEX::STMT_READS_TEMP_TRANS_TABLE`）或临时非事务表（`LEX::STMT_READS_TEMP_NON_TRANS_TABLE`）；
   - `is_create_drop_temp_table`（建/删临时表）。

   这就是 MIXED 的实际语义：**默认语句格式，命中上述条件才切行格式**。

决策最终落在 `is_current_stmt_binlog_format_row()` 这个 THD 级标志上。函数末尾的调试输出直接把它写死成一句话：`decision: logging in %s format`，取值来自 `is_current_stmt_binlog_format_row() ? "ROW" : "STATEMENT"`。

**第二层：语句级判定之外的缓存选择——`Query_log_event::Query_log_event()`（`sql/log_event.cc`）。** 这个构造函数除了填事件字段，还负责把事件放进哪个 binlog 缓存、以及用哪种 logging 类型。它的执行顺序是：

1. 先给基类 `binary_log::Query_event` 传入 SQL 原文、catalog、db、thread id、`sql_mode`、`auto_increment_increment`、`auto_increment_offset`、`lc_time_names`、`table_map_for_update`、`errcode`。
2. 计算 `exec_time = end_time.tv_sec - thd_arg->query_start_in_secs()`。
3. `flags2 = thd_arg->variables.option_bits & (OPTIONS_WRITTEN_TO_BIN_LOG & ~OPTION_NOT_AUTOCOMMIT)`——强制把 AUTOCOMMIT 置 1，源码注释给出的理由是 BUG#29288：这样才能让「非事务表 + 事务表」的复制与 XA→非 XA 的错误恢复按预期工作。
4. 写三个字符集编号（`character_set_client` / `collation_connection` / `collation_server`，各 2 字节）；若 `thd_arg->variables.collation_database != thd_arg->db_charset` 再单独记 `charset_database_number`。
5. 若 `thd_arg->time_zone_used` 为真，记时区名（长度断言 `MAX_TIME_ZONE_NAME_LENGTH`）。
6. **分类**：按 `lex->sql_command` 分派，算出两个布尔量 `cmd_can_generate_row_events`（能否产生行事件）和 `cmd_must_go_to_trx_cache`（是否必须走事务缓存）：
   - `SQLCOM_DROP_TABLE`：`cmd_can_generate_row_events = lex->drop_temporary && thd->in_multi_stmt_transaction_mode()`。
   - `SQLCOM_CREATE_TABLE`：`cmd_must_go_to_trx_cache = !lex->query_block->field_list_is_empty() && thd->is_current_stmt_binlog_format_row()`；`cmd_can_generate_row_events` 为「建临时表且在多语句事务中」或「`cmd_must_go_to_trx_cache`」。
   - `SQLCOM_SET_OPTION`：`lex->autocommit` 为真时两个标志都置假，否则 `cmd_can_generate_row_events` 置真。
   - `SQLCOM_RELEASE_SAVEPOINT` / `SQLCOM_ROLLBACK_TO_SAVEPOINT` / `SQLCOM_SAVEPOINT` / `SQLCOM_XA_PREPARE`：两个标志都置真。
   - 其他命令：`cmd_can_generate_row_events = sqlcom_can_generate_row_events(thd->lex->sql_command)`——这个函数在 `sql/sql_parse.cc` 里实现为查 `sql_command_flags[command] & CF_CAN_GENERATE_ROW_EVENTS`。
   - 若 `immediate` 为真，则断言 `!using_trans`，并且整个分派块被跳过（不分类）。
7. **选缓存**：
   - `cmd_can_generate_row_events` 为真时：若 `cmd_must_go_to_trx_cache`、或本语句已更新过事务表（`stmt_has_updated_trans_table`）、或 `thd->lex->is_mixed_stmt_unsafe(...)`、或（`!binlog_direct_non_trans_update` 且 `trans_has_updated_trans_table(thd)`）任一成立 → `event_cache_type = EVENT_TRANSACTIONAL_CACHE`，否则 `EVENT_STMT_CACHE`。两种情况 `event_logging_type` 都是 `EVENT_NORMAL_LOGGING`。
   - 否则若 `is_atomic_ddl(thd, using_trans)` → 取事务的 XID 存进 `ddl_xid`，用 `EVENT_TRANSACTIONAL_CACHE` + `EVENT_NORMAL_LOGGING`。
   - 否则若 `SQLCOM_CREATE_TABLE` 且 `lex->create_info->m_transactional_ddl` → `EVENT_TRANSACTIONAL_CACHE` + `EVENT_NORMAL_LOGGING`，并置 `thd->rli_slave->ddl_not_atomic = true`。
   - 否则（兜底，含 `SQLCOM_XA_COMMIT` / `SQLCOM_XA_ROLLBACK`）→ `EVENT_IMMEDIATE_LOGGING` + `EVENT_STMT_CACHE`，同样置 `ddl_not_atomic = true`。
8. `need_sql_require_primary_key = is_sql_require_primary_key_needed(lex)`——`SQLCOM_CREATE_TABLE` / `SQLCOM_ALTER_TABLE` 返回真。
9. `needs_default_table_encryption = is_default_table_encryption_needed(lex)`。
10. 断言 `event_cache_type != EVENT_INVALID_CACHE` 且 `event_logging_type != EVENT_INVALID_LOGGING`。

构造函数的注释里还写明了一条重要事实：**DDL 一律按语句格式记**——DDL 属于「non-row producer」，源码注释原文是「Non-row producers, DDL in general, are treated as the immediate flag was set」，即被当作 `immediate` 处理，写进 stmt-cache 并立即落盘。

### 行镜像的构造规则

行镜像的构造发生在 `sql/rpl_record.cc` 的 `pack_row()` 里，由 `sql/binlog.cc` 的三个 THD 方法调用：

- `THD::binlog_write_row()`：`pack_row(table, table->write_set, row_data, record, enum_row_image_type::WRITE_AI)`，随后 `ev->add_row_data(row_data, len)`。
- `THD::binlog_update_row()`：先 `binlog_prepare_row_images(this, table)`，再 `pack_row(..., table->read_set, ..., enum_row_image_type::UPDATE_BI)` 与 `pack_row(..., table->write_set, ..., enum_row_image_type::UPDATE_AI, variables.binlog_row_value_options)`，最后两次 `add_row_data` 分别塞入 before 与 after。执行完调用 `table->column_bitmaps_set_no_signal(old_read_set, old_write_set)` 恢复原始位图，并 `bitmap_clear_all(&table->tmp_set)`。
- `THD::binlog_delete_row()`：`pack_row(table, table->read_set, row_data, record, enum_row_image_type::DELETE_BI)`。

`pack_row()` 的内部规则：

1. 用 `cs::util::ReplicatedColumnsView fields{table}` 建立列视图，并加一个 `outbound_func_index` 过滤器。
2. 把 `columns_in_image` 的位拷进 `table->pack_row_tmp_set`，再与 `fields.get_included_fields_bitmap()` 求交——**目的是把隐藏生成列排除在 binlog 之外**（源码注释明确说明这一点）。
3. `image_column_count = bitmap_bits_set(&table->pack_row_tmp_set)`，即镜像里实际写入的列数。
4. null 位图占 `(image_column_count + 7) / 8` 字节。
5. 逐列写值：`field->is_null(rec_offset)` 为真则只置 null 位，否则调 `pack_field()` 写实际值。
6. **部分 JSON 更新**：当 `value_options & PARTIAL_JSON_UPDATES` 非零且 `row_image_type == UPDATE_AI` 时，先统计 JSON 列数 `json_column_count`，并检查是否有任一 JSON 列确实带 diff（`has_any_json_diff`）；写入 `net_store_length(pack_ptr, has_any_json_diff ? value_options : 0)`，随后追加 `(json_column_count + 7) / 8` 字节的部分位图。**只有确实存在 diff 时才写 value_options 与部分位图**。

### `binlog_row_image` 如何裁剪镜像

`binlog_prepare_row_images()`（`sql/binlog.cc`）负责按 `binlog_row_image` 裁掉不必要的列。它的前置条件是：表有主键（`table->s->primary_key < MAX_KEY`）、`thd->variables.binlog_row_image < BINLOG_ROW_IMAGE_FULL`、且存储引擎没有 `HTON_NO_BINLOG_ROW_OPT` 标志。然后按 `thd->variables.binlog_row_image` 分派：

- `BINLOG_ROW_IMAGE_MINIMAL`：`table->mark_columns_used_by_index_no_reset(table->s->primary_key, &table->tmp_set)`——只保留主键列。
- `BINLOG_ROW_IMAGE_NOBLOB`：先 `bitmap_union(&table->tmp_set, table->read_set)`，再遍历所有字段，把**不带 `PRI_KEY_FLAG`** 的 `MYSQL_TYPE_BLOB` 字段从位图里清掉。
- `default` 分支是 `assert(0)`（源码注释标注 impossible）。

### 行事件的分组合并与大小上限

`THD::binlog_prepare_pending_rows_event()`（`sql/binlog.cc`）决定「当前这行是并进已有的 pending 事件，还是新开一个事件」。需要**新开**事件的条件（源码里的判断式，任一成立即新开）：

- 没有 pending 事件；
- `pending->server_id != serv_id`；
- `pending->get_table_id() != table->s->table_map_id`（换了表）；
- `pending->get_general_type_code() != general_type_code`（Write/Update/Delete 之间换了类型）；
- `pending->get_data_size() + needed > binlog_row_event_max_size`（超了上限）；
- `pending->read_write_bitmaps_cmp(table) == false`（列位图变了）；
- `pending->m_extra_row_info.compare_extra_row_info(...)` 不匹配（extra row info 变了）。

超限判断用的常量是全局变量 `binlog_row_event_max_size`（定义在 `sql/mysqld.cc`，官方文档给出默认值 8192 字节、最小值 256）。官方文档同时说明这是**软上限**：无法拆分的事件可以超过它。

新开事件时，`binlog_prepare_pending_rows_event` 用 `new RowsEventT(this, table, table->s->table_map_id, is_transactional, extra_row_info)` 构造，然后调 `mysql_bin_log.flush_and_set_pending_rows_event(this, ev, is_transactional)` 把旧 pending 刷出去。

### `Rows_log_event` 基类构造函数的实际动作

`Rows_log_event::Rows_log_event(THD*, TABLE*, const Table_id&, MY_BITMAP const*, bool, Log_event_type, const unsigned char*)`（`sql/log_event.cc`）做的是「行事件骨架的搭建」：

1. 基类 `Log_event` 按 `using_trans` 选择缓存：`using_trans ? EVENT_TRANSACTIONAL_CACHE : EVENT_STMT_CACHE`。
2. `common_header->type_code = event_type`；`m_row_count = 0`；`m_table_id = tid`。
3. `m_column_view = cs::util::ReplicatedColumnsViewFactory::get_columns_view_with_outbound_filters(thd_arg, tbl_arg)`。
4. **`m_width = tbl_arg ? m_column_view->filtered_size() : 1`**——镜像宽度来自过滤后的列视图大小，不是表的总列数。
5. 按 `thd_arg->variables.option_bits` 打标志：`OPTION_NO_FOREIGN_KEY_CHECKS` → `NO_FOREIGN_KEY_CHECKS_F`；`OPTION_RELAXED_UNIQUE_CHECKS` → `RELAXED_UNIQUE_CHECKS_F`。
6. 分区信息：`get_rpl_part_id(part_info)` 拿分区号，不是 `binary_log::Rows_event::Extra_row_info::UNDEFINED` 时调 `m_extra_row_info.set_partition_id(part_id)`。
7. `extra_row_ndb_info` 非空时，把它的长度从 `extra_row_ndb_info[EXTRA_ROW_INFO_LEN_OFFSET]` 读出（断言 ≥ `EXTRA_ROW_INFO_HEADER_LENGTH`），再调 `m_extra_row_info.set_ndb_info(...)`。
8. `bitmap_init(&m_cols, ...)` 初始化列位图；`cols` 非空时 `bitmap_n_copy(&m_cols, cols)` 拷入。源码注释原文是「'cols' may have additional hidden columns at the end.」，因此断言 `cols->n_bits >= m_cols.n_bits`。位图初始化失败时把 `m_cols.bitmap` 置 `nullptr`。
9. `bitmap_init(&write_set_backup, nullptr, tbl_arg->s->fields)` 备份写集。
10. `common_header->set_is_valid(m_rows_buf && m_cols.bitmap && write_set_backup.bitmap)`。

反序列化方向的构造函数 `Rows_log_event::Rows_log_event(const char *buf, const Format_description_event*)` 则从字节流里恢复位图：`columns_before_image` 非空时 `memcpy(m_cols.bitmap, &columns_before_image[0], n_bits_len)` 并 `create_last_word_mask(&m_cols)`；`m_cols_ai` 只在 `UPDATE_ROWS_EVENT` / `UPDATE_ROWS_EVENT_V1` / `PARTIAL_UPDATE_ROWS_EVENT` 三种类型下单独初始化。

### 事件的变长头

`Rows_log_event::write_data_header()` 按 `log_bin_use_v1_row_events` 分两支：

- 为假（v2）时，先写 `ROWS_HEADER_LEN_V2` 字节的定长头（表 id 6 字节 + flags 2 字节 + 变长头长度 2 字节），再按需追加变长载荷：NDB 信息用 `enum_extra_row_info_typecode::NDB` 类型码 + 内容，分区信息用 `enum_extra_row_info_typecode::PART` 类型码 + 2 字节分区号（UPDATE 事件再追加 2 字节源分区号）。变长头长度写的是「含长度字段自身」的总长，初值 `EXTRA_ROW_INFO_HEADER_LENGTH`。
- 为真（v1）时只写 `ROWS_HEADER_LEN_V1` 字节的定长头。

`Rows_log_event::get_data_size()` 与之对应：v2 事件的基数是 `Binary_log_event::ROWS_HEADER_LEN_V2`，v1 是 `ROWS_HEADER_LEN_V1`，再叠加 `no_bytes_in_map(&m_cols)`、`m_width` 的变长编码长度、UPDATE 事件的 `no_bytes_in_map(&m_cols_ai)`、以及行数据长度 `m_rows_cur - m_rows_buf`。

## 怎么实现的

### 入口：`THD::decide_logging_format()`

判定顺序（对应源码里的执行次序）：

**第一步，前置条件。** 只有在 `mysql_bin_log.is_open()`、`(variables.option_bits & OPTION_BIN_LOG)` 非零、且不满足「`binlog_format == BINLOG_FORMAT_STMT` 但当前库被 binlog 过滤规则排除」时才进入判定；否则走 else 分支，只对 `no_replicate` 的表做 GTID 表修改告警，不做格式判定。

**第二步，标记 unsafe 语句。** 仅当 `variables.binlog_format != BINLOG_FORMAT_ROW` 时执行（ROW 格式不区分安全/不安全）：

- `has_write_table_with_auto_increment_and_query_block(tables)` → `LEX::BINLOG_STMT_UNSAFE_WRITE_AUTOINC_SELECT`。
- `has_write_table_auto_increment_not_first_in_pk(tables)` → `LEX::BINLOG_STMT_UNSAFE_AUTOINC_NOT_FIRST`。
- `lex->requires_prelocking()` 且 `has_write_table_with_auto_increment(lex->first_not_own_table())` → `LEX::BINLOG_STMT_UNSAFE_AUTOINC_COLUMNS`。
- `lex->requires_prelocking()` 且 `has_write_table_with_nondeterministic_default(...)` → `LEX::BINLOG_STMT_UNSAFE_DEFAULT_EXPRESSION_IN_SUBSTATEMENT`。
- `has_acl_table_read(this, tables)` → `LEX::BINLOG_STMT_UNSAFE_ACL_TABLE_READ_IN_DML_DDL`。

**第三步，遍历表收集引擎能力。** 对每个 `Table_ref`：

- 占位表（`table->is_placeholder()`）先单独处理：若是 `SQLCOM_CREATE_TABLE` 且带 `HA_LEX_CREATE_TMP_TABLE`、或是 `SQLCOM_DROP_TABLE` / `SQLCOM_TRUNCATE` 且命中临时表，则置 `is_create_drop_temp_table = true`，然后 `continue`。
- `table->table->no_replicate` 为真时，**整张表被完全忽略**（源码注释列出的这类表包括 `mysql.slow_log`、`mysql.general_log`、`mysql.slave_relay_log_info`、`mysql.slave_master_info`、`mysql.slave_worker_info`、`performance_schema.*`），其引擎能力不参与并集/交集计算。
- 其余表取 `table->table->file->ha_table_flags()`，参与能力集合的并集（`flags_write_some_set`）与交集（`flags_write_all_set`）计算，并据此更新 `multi_write_engine`、`multi_access_engine`、`is_write`、`write_to_some_transactional_table`、`write_to_some_non_transactional_table`、`write_all_non_transactional_are_tmp_tables`、`replicated_tables_count`、`non_replicated_tables_count` 等标记。

**第四步，混合语句安全性。** `multi_stmt_trans = lex->sql_command != SQLCOM_CREATE_TABLE && in_multi_stmt_transaction_mode()`。`lex->is_mixed_stmt_unsafe(multi_stmt_trans, binlog_direct, trans_table, tx_isolation)` 为真 → `LEX::BINLOG_STMT_UNSAFE_MIXED_STATEMENT`；否则若 `multi_stmt_trans && trans_table && !binlog_direct && lex->stmt_accessed_table(LEX::STMT_WRITES_NON_TRANS_TABLE)` → `LEX::BINLOG_STMT_UNSAFE_NONTRANS_AFTER_TRANS`。

**第五步，多引擎冲突。** `multi_write_engine && (flags_write_some_set & HA_HAS_OWN_BINLOGGING)` → 报 `ER_BINLOG_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE`（因为这种语句无法原子记入 binlog）；`multi_access_engine && (flags_access_some_set & HA_HAS_OWN_BINLOGGING)` → 标 `LEX::BINLOG_STMT_UNSAFE_MULTIPLE_ENGINES_AND_SELF_LOGGING_ENGINE`。

**第六步，XA。** `is_write` 且事务的 XID 状态不是 `XID_STATE::XA_NOTR` → `LEX::BINLOG_STMT_UNSAFE_XA`。

**第七步，按前面的 1～4 号分支做最终判定**，产出「错误 / 警告 / 保持语句格式 / 切行格式」四种结果之一。

**第八步，本地表过滤与 GTID 一致性。** `non_replicated_tables_count > 0` 时：若 `replicated_tables_count == 0` 或不是写语句 → `set_binlog_local_stmt_filter()`（整条语句不记）；否则若不是行格式 → 报 `ER_BINLOG_STMT_MODE_AND_NO_REPL_TABLES`，是行格式 → `clear_binlog_local_stmt_filter()`。没有本地表时直接 `clear_binlog_local_stmt_filter()`。随后 `is_dml_gtid_compatible(...)` 为假则 `error = 1`，`error` 非零时函数返回 `-1`，**这条语句不记 binlog**。

**第九步，收集 MTS 需要的库名。** 是写语句且不是从库重放（`lex->sql_command != SQLCOM_END`）时遍历表：若 `table->table->s->is_referenced_by_foreign_key()` → `binlog_accessed_db_names = nullptr` 并 `add_to_binlog_accessed_dbs("")`（标记该事件必须串行应用），否则若不是行格式才 `add_to_binlog_accessed_dbs(table->db)`。

**第十步，BLACKHOLE 引擎告警。** `binlog_format == BINLOG_FORMAT_ROW` 且命令是 `SQLCOM_UPDATE` / `SQLCOM_UPDATE_MULTI` / `SQLCOM_DELETE` / `SQLCOM_DELETE_MULTI` 时，收集 `db_type == DB_TYPE_BLACKHOLE_DB` 且锁描述符 `lock_descriptor().type >= TL_WRITE_ALLOW_WRITE` 的表名，用 `WARN_ON_BLOCKHOLE_IN_RBR` 告警——因为行事件在 BLACKHOLE 引擎上并不产生。

### 行格式路径：`THD::binlog_write_row()` / `binlog_update_row()` / `binlog_delete_row()`

三个函数都以 `assert(is_current_stmt_binlog_format_row() && mysql_bin_log.is_open())` 开头——**没有经过 `decide_logging_format` 判定为行格式的语句走不到这里**。

`binlog_write_row()` 的顺序：

1. `Row_data_memory memory(table, record)` 分配行缓冲；无内存时返回 `HA_ERR_OUT_OF_MEM`。
2. `pack_row(table, table->write_set, row_data, record, enum_row_image_type::WRITE_AI)`。
3. `binlog_prepare_pending_rows_event<Write_rows_log_event>(table, server_id, len, is_trans, extra_row_info)` 取出（必要时新建）pending 事件。
4. `ev->add_row_data(row_data, len)`。

`binlog_update_row()` 的顺序（比 write 多三步）：

1. 保存 `table->read_set` / `table->write_set` 原指针。
2. `binlog_prepare_row_images(this, table)` 按 `binlog_row_image` 裁剪 read_set。
3. `Row_data_memory row_data(table, before_record, after_record, variables.binlog_row_value_options)`，两个槽分别放 before 与 after。
4. 两次 `pack_row`。
5. 若是分区表，`get_parts_for_update(...)` 拿到源分区号与新分区号。
6. `binlog_prepare_pending_rows_event<Update_rows_log_event>(..., source_part_id)`。
7. `ev->m_extra_row_info.set_source_partition_id(source_part_id)`。
8. `ev->add_row_data(before_row, before_size) || ev->add_row_data(after_row, after_size)`。
9. 恢复位图：`table->column_bitmaps_set_no_signal(old_read_set, old_write_set)`，再 `bitmap_clear_all(&table->tmp_set)`。

`Rows_log_event::do_add_row_data()` 的行为：`length == 0` 时只 `m_row_count++` 并返回 0（源码注释说明这不是优化，而是因为 `realloc(size=0)` 等于 `free()`）；否则断言 `m_rows_buf <= m_rows_cur` 与 `m_rows_buf == nullptr || (m_rows_end && m_rows_buf < m_rows_end)` 后追加。

### 事件类之间的关系：`Rows_event` 与 `Log_event` 的虚继承

`sql/log_event.h` 的注释给出继承结构：

```
        Binary_log_event
               ^
               |
          Rows_event  Log_event
                 \       /
           <<vir>>\     /
                   \   /
              Rows_log_event
```

`Rows_log_event` 用**虚继承**同时继承 `binary_log::Rows_event` 和 `Log_event`，注释说明这是为了解决 `Write_rows_log_event` / `Update_rows_log_event` / `Delete_rows_log_event` 三者的菱形继承问题。`Rows_log_event` 定义的 `row_lookup_mode` 枚举（`ROW_LOOKUP_UNDEFINED` / `ROW_LOOKUP_NOT_NEEDED` / `ROW_LOOKUP_INDEX_SCAN` / `ROW_LOOKUP_TABLE_SCAN` / `ROW_LOOKUP_HASH_SCAN`）与 `enum_error` 枚举（`ERR_OPEN_FAILURE` / `ERR_OK` / `ERR_TABLE_LIMIT_EXCEEDED` / `ERR_OUT_OF_MEM` / `ERR_BAD_TABLE_DEF` / `ERR_RBR_TO_SBR`）都在这个基类里。

### 从库侧：`unpack_current_row()` 的镜像还原

`Rows_log_event::unpack_current_row()`（`sql/log_event.cc`）把字节流还原成行。它按事件类型与是 before 还是 after 选出 `enum_row_image_type`：

- after image：`UPDATE_ROWS_EVENT` → `UPDATE_AI`，否则 → `WRITE_AI`（同时断言不是 `DELETE_ROWS_EVENT`）。
- before image：`UPDATE_ROWS_EVENT` → `UPDATE_BI`，否则 → `DELETE_BI`（同时断言不是 `WRITE_ROWS_EVENT`）。

`has_value_options` 仅在 `get_type_code() == binary_log::PARTIAL_UPDATE_ROWS_EVENT` 时为真。随后调 `::unpack_row(rli, m_table, m_width, m_curr_row, cols, &m_curr_row_end, m_rows_end, row_image_type, has_value_options, only_seek)`（实现在 `sql/rpl_record.cc`）。函数先断言 `m_curr_row <= m_rows_end`，否则返回 `HA_ERR_CORRUPT_EVENT`。

还原完成后还要处理生成列：`m_table->has_gcol()` 为真时构造 `Table_columns_view<>`，按一串筛选规则排除不该在从库重算的生成列（排除函数索引列、排除 before image 中基列不可用的生成列、排除虚拟生成列、保留带索引的生成列、保留基列发生变化的生成列、排除源端已存在的生成列），再调 `update_generated_columns()`。`update_generated_columns()` 会调整 `write_set_backup` 的位宽（不匹配时释放并按 `m_table->s->fields` 重建）、`bitmap_copy` 备份写集、`bitmap_union` 合并待更新列、调 `update_generated_write_fields()` 计算值，然后 `bitmap_copy` 还原写集。

### 语句格式路径：`Query_log_event::write()`

`Query_log_event::write(Basic_ostream*)` 的执行顺序：

1. `if (!query) return true;`——没有 SQL 文本直接算失败。
2. 写固定头：`int4store(buf + Q_THREAD_ID_OFFSET, slave_proxy_id)`、`int4store(buf + Q_EXEC_TIME_OFFSET, exec_time)`、`buf[Q_DB_LEN_OFFSET] = db_len`、`int2store(buf + Q_ERR_CODE_OFFSET, error_code)`。
3. 按代码递增顺序写 status 变量（详见上文「两种格式的事件长什么样」一节）。写 catalog 与时区时用 `write_str_with_code_and_len()`，它断言长度 ≤ 255（1 字节存长度）。
4. `status_vars_len = (uint)(start - start_of_status)`，断言 ≤ `MAX_SIZE_LOG_EVENT_STATUS`，然后 `int2store(buf + Q_STATUS_VARS_LEN_OFFSET, status_vars_len)`。
5. `event_length = (uint)(start - buf) + get_post_header_size_for_derived() + db_len + 1 + q_len`——那个 `+1` 是 db 名后的 `\0`。
6. 依次写：事件头（`write_header(ostream, event_length)`）、固定头、post header、status 段、db 名（含结尾 `\0`）、SQL 正文。

这里有一个关键设计：**语句格式事件必须在写的时候才知道长度**——源码注释写明「we have to modify the header to have the correct EVENT_LEN_OFFSET as we don't yet know how many status variables we will print」。所以 `write()` 是覆写 `Log_event::write()` 的重载，而不是走 `write_data_header` / `write_data_body` 的虚接口。

## 关键取舍

### 语句格式 vs 行格式：确定性换空间

| | 语句格式 | 行格式 |
|---|---|---|
| 记什么 | SQL 原文 | 行的 before / after 镜像 |
| 体积 | 与语句长度成正比，一条语句一条事件 | 与**受影响行数 × 列宽**成正比 |
| 确定性 | 依赖语句本身可重放；官方文档把「重放结果会随环境不同而不同」的语句列为 unsafe | 不依赖重放，只依赖列值 |
| 适用 | DDL、非确定性低、可重放 | 大批量 DML、非确定性函数、LOADABLE FUNCTION |

官方文档给出的默认是 `binlog_format = ROW`，并且自 MySQL 8.0.34 起 `binlog_format` 已被标记为 deprecated——文档原文是「support for logging formats other than row-based is also subject to removal in a future release」。

**取舍的本质：把「重放的正确性」从「语句语义」转移到「数据值」。** 行格式不关心语句怎么执行，只关心改成了什么值，代价是事件体积随行数放大。

### 不安全语句的三种处置

同一个 unsafe 标记，在三种 `binlog_format` 下走向完全不同（源码分支 2/3/4）：

- `STATEMENT`：**照记语句格式**，只是累加 `binlog_unsafe_warning_flags` 并发 `ER_BINLOG_UNSAFE_STATEMENT` 警告——语句继续执行、继续复制，风险由运维承担。
- `MIXED`：若引擎两边都支持，切行格式（`set_current_stmt_binlog_format_row_if_mixed()`）；若引擎只有语句能力，报 `ER_BINLOG_UNSAFE_AND_STMT_ENGINE`，**语句不执行**。
- `ROW`：不区分安全与否，一律行格式。

**取舍：ROW 模式把「静默的不一致」变成「确定的重放」，代价是无法记录部分只能语句表达的东西。** `sql/binlog.cc` 里「`binlog_format != BINLOG_FORMAT_ROW` 才做 unsafe 标记」这一行就是这条取舍的代码化。

### 引擎能力用「并集 + 交集」两个集合表达

`flags_write_some_set`（并集）与 `flags_write_all_set`（交集）这个双集合设计，把多引擎场景压缩成三个布尔判断：交集既不含 ROW 也不含 STMT（完全不能记）、交集不含 ROW（有语句专属引擎）、交集不含 STMT（只能行记）。**取舍：牺牲了逐引擎的精细描述，换来了 O(1) 的分支判定。** 代价是「一个只能行记的引擎 + 一个只能语句记的引擎」这种组合无法精确表达，只能落到「完全不能记」的错误 `ER_BINLOG_ROW_ENGINE_AND_STMT_ENGINE`。

### `binlog_row_image`：三种裁剪强度

`binlog_prepare_row_images()` 提供三档（`FULL` / `MINIMAL` / `NOBLOB`）的镜像裁剪，裁剪只在**有主键**且引擎未设 `HTON_NO_BINLOG_ROW_OPT` 时生效。

**取舍：MINIMAL 只记主键 + 变更列，体积最小，但要求从库能靠主键唯一定位到行**——这正是官方文档把 MINIMAL 描述为「Log only changed columns, and columns needed to identify rows」的原因。没有主键的表用不了 MINIMAL，因为 `table->s->primary_key < MAX_KEY` 这个前置条件不成立。

### 行事件合并 vs 事件数

`binlog_prepare_pending_rows_event()` 把同表、同类型、同位图的多行合并进一个事件，直到逼近 `binlog_row_event_max_size`（默认 8192 字节）。**取舍：合并减少事件数、减少事件头开销，但一个超长单行无法拆分。** 官方文档明确说这是软上限（soft limit）：「If an event cannot be split, the maximum size can be exceeded」。触发换事件的条件不止超限——换表、换类型、位图变化、extra row info 变化也都会断开合并。

### 部分 JSON 更新：只在真有 diff 时才付代价

`pack_row()` 里部分 JSON 的写入是**条件性**的：`has_any_json_diff` 为假时，`net_store_length` 写入的是 `0` 而不是 `value_options`，部分位图也不写。**取舍：没有 JSON 变更的 UPDATE 不为 JSON 付任何额外字节**，代价是每行多一次 JSON 列扫描来判断 `has_any_json_diff`。

### 语句事件的缓存选择与事务原子性

`Query_log_event::Query_log_event()` 把「进哪个缓存」拆成 `event_cache_type`（`EVENT_TRANSACTIONAL_CACHE` / `EVENT_STMT_CACHE`）与 `event_logging_type`（`EVENT_NORMAL_LOGGING` / `EVENT_IMMEDIATE_LOGGING`）两个维度，组合出四种行为。DDL 走 `EVENT_IMMEDIATE_LOGGING` + `EVENT_STMT_CACHE`（立即落盘，不等事务提交），原子 DDL 走 `EVENT_TRANSACTIONAL_CACHE` + `EVENT_NORMAL_LOGGING` 并携带 XID。

**取舍：DDL 立即落盘保证了「从库不会长时间看不到 DDL」，代价是 DDL 与同一事务里的 DML 不在同一缓存中。** 源码注释在 `is_atomic_ddl` 分支里说明，原子 DDL 走事务缓存是为了「使用单个事务执行 CREATE-TABLE 与 INSERT 的事件应用」。

### `binlog_row_value_options` 与 v1 行事件的互斥

`sql/mysqld.cc` 里的启动检查给出三条互斥规则：`binlog_row_value_options != 0` 时，

- 二进制日志未开启 → `ER_BINLOG_ROW_VALUE_OPTION_IGNORED`，原因「the binary log is disabled」；
- `binlog_format == BINLOG_FORMAT_STMT` → 同上错误码，原因「binlog_format=STATEMENT」；
- `log_bin_use_v1_row_events` 为真 → `ER_BINLOG_USE_V1_ROW_EVENTS_IGNORED`，原因「binlog_row_value_options=PARTIAL_JSON」；
- `binlog_row_image == BINLOG_ROW_IMAGE_FULL` → `ER_BINLOG_ROW_VALUE_OPTION_USED_ONLY_FOR_AFTER_IMAGES`，原因「binlog_row_image=FULL」。

**取舍：部分 JSON 更新只在「行格式 + v2 行事件 + 非 FULL 镜像」这三者同时成立时才有意义**，任何一项不满足就整体忽略该选项并告警。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/log_event.cc | `Rows_log_event::Rows_log_event` | 行事件基类构造函数（写入方向）：按 `using_trans` 选缓存、用 `m_column_view->filtered_size()` 定 `m_width`、打 `NO_FOREIGN_KEY_CHECKS_F` / `RELAXED_UNIQUE_CHECKS_F` 标志、设分区号与 NDB extra info、初始化 `m_cols` 位图与 `write_set_backup`，最后置 `is_valid` |
| sql/log_event.cc | `Rows_log_event::Rows_log_event` | 行事件基类构造函数（读取方向）：从 `columns_before_image` / `columns_after_image` 恢复 `m_cols` / `m_cols_ai` 位图，`UPDATE_ROWS_EVENT` 系类型才单独初始化 after image 位图 |
| sql/log_event.cc | `Query_log_event::Query_log_event` | 语句格式事件构造函数：填基类字段、算 `exec_time`、强制 AUTOCOMMIT=1 写 `flags2`、记字符集与时区；按 `lex->sql_command` 分派算出 `cmd_can_generate_row_events` / `cmd_must_go_to_trx_cache`，据此选 `event_cache_type` 与 `event_logging_type`；原子 DDL 分支取事务 XID 写入 `ddl_xid` |
| sql/log_event.cc | `Query_log_event::write` | 语句格式事件序列化：写固定头（thread id、执行时长、db 长度、错误码），按代码递增顺序写 status 变量段并记 `status_vars_len`，算 `event_length` 后依次写出事件头、固定头、post header、status 段、db 名、SQL 正文 |
| sql/log_event.cc | `Rows_log_event::write_data_header` | 行事件头：v2 分支写 `ROWS_HEADER_LEN_V2` 定长头 + 变长载荷（NDB 信息 / 分区信息，类型码分别为 `enum_extra_row_info_typecode::NDB` 与 `PART`），v1 分支只写 `ROWS_HEADER_LEN_V1` 定长头 |
| sql/log_event.cc | `Rows_log_event::write_data_body` | 行事件体：写 `m_width` 的变长编码、`m_cols` 位图、UPDATE 事件的 `m_cols_ai` 位图，最后整块写出行数据 |
| sql/log_event.cc | `Rows_log_event::get_data_size` | 计算行事件数据长度：按 v1/v2 取 `ROWS_HEADER_LEN_V1` / `ROWS_HEADER_LEN_V2`，叠加两张位图字节数、`m_width` 编码长度与行数据长度 |
| sql/log_event.cc | `Rows_log_event::do_add_row_data` | 向行事件追加一行数据；`length == 0` 时只增 `m_row_count` 并返回（避免 `realloc(0)` 触发 `free()`） |
| sql/log_event.cc | `Rows_log_event::unpack_current_row` | 从库侧还原一行：按事件类型与 before/after 选出 `enum_row_image_type`，调 `unpack_row` 解包；再按规则处理生成列更新 |
| sql/log_event.cc | `Rows_log_event::update_generated_columns` | 备份写集、合并待更新生成列、调 `update_generated_write_fields` 计算值，再恢复写集 |
| sql/log_event.cc | `Write_rows_log_event::Write_rows_log_event` | 写方向构造：类型码按 `log_bin_use_v1_row_events` 选 `WRITE_ROWS_EVENT_V1` / `WRITE_ROWS_EVENT`，列位图取自 `tbl_arg->write_set` |
| sql/log_event.cc | `Write_rows_log_event::binlog_row_logging_function` | 写入钩子：转调 `thd_arg->binlog_write_row(table, is_transactional, after_record, nullptr)` |
| sql/log_event.cc | `Delete_rows_log_event::Delete_rows_log_event` | 写方向构造：类型码按 `log_bin_use_v1_row_events` 选 `DELETE_ROWS_EVENT_V1` / `DELETE_ROWS_EVENT`，列位图取自 `tbl_arg->read_set` |
| sql/log_event.cc | `Update_rows_log_event::get_update_rows_event_type` | 选 UPDATE 事件类型码：`binlog_row_value_options != 0` → `PARTIAL_UPDATE_ROWS_EVENT`；否则按 `log_bin_use_v1_row_events` 选 `UPDATE_ROWS_EVENT_V1` / `UPDATE_ROWS_EVENT` |
| sql/log_event.cc | `Update_rows_log_event::Update_rows_log_event` | 写方向构造：基类用 `tbl_arg->read_set` 建 before image 位图，再 `init(tbl_arg->write_set)` 建 after image 位图，最后 `set_is_valid(Rows_log_event::is_valid() && m_cols_ai.bitmap)` |
| sql/log_event.cc | `Update_rows_log_event::init` | 用 `bitmap_init` + `bitmap_n_copy` 把写集拷进 `m_cols_ai` |
| sql/log_event.cc | `Update_rows_log_event::binlog_row_logging_function` | 写入钩子：转调 `thd_arg->binlog_update_row(table, is_transactional, before_record, after_record, nullptr)` |
| sql/log_event.cc | `get_rpl_part_id` | 从 `partition_info` 取分区号，无分区时返回 `binary_log::Rows_event::Extra_row_info::UNDEFINED` |
| sql/binlog.cc | `THD::decide_logging_format` | 语句格式 vs 行格式的判定点：按 `binlog_format`、引擎能力（并集/交集）、unsafe 标记、临时表访问等决定报错 / 警告 / 语句格式 / 行格式 |
| sql/binlog.cc | `THD::binlog_write_row` | 写方向行镜像构造：打包 after image、准备或复用 pending 事件、追加行数据 |
| sql/binlog.cc | `THD::binlog_update_row` | 写方向行镜像构造：裁剪 read_set、打包 before/after 两份镜像、写源分区号、追加两份行数据、恢复原始位图 |
| sql/binlog.cc | `THD::binlog_delete_row` | 写方向行镜像构造：裁剪 read_set、打包 before image、追加行数据、恢复原始位图 |
| sql/binlog.cc | `THD::binlog_prepare_pending_rows_event` | 行事件合并决策：同 server_id、同表、同类型、位图一致、extra info 一致且未超 `binlog_row_event_max_size` 才复用，否则新建并把旧 pending 刷出 |
| sql/binlog.cc | `THD::binlog_get_pending_rows_event` | 按事务性取出 binlog 缓存里当前的 pending 行事件 |
| sql/binlog.cc | `binlog_prepare_row_images` | 按 `binlog_row_image` 裁剪 read_set：`MINIMAL` 只留主键列，`NOBLOB` 清掉不带 `PRI_KEY_FLAG` 的 BLOB 列 |
| sql/rpl_record.cc | `pack_row` | 行镜像打包：排除隐藏生成列、算 `image_column_count`、写 null 位图、逐列写值；部分 JSON 更新时按 `has_any_json_diff` 决定是否写 value_options 与部分位图 |
| sql/rpl_record.cc | `unpack_row` | 行镜像解包（从库侧），按 `enum_row_image_type` 与 `has_value_options` 还原字段值 |
| sql/sql_parse.cc | `sqlcom_can_generate_row_events` | 判定某条 SQL 命令能否产生行事件：查 `sql_command_flags[command] & CF_CAN_GENERATE_ROW_EVENTS` |
| sql/mysqld.cc | `log_bin_use_v1_row_events` | 全局布尔量（初值 `false`），决定行事件用 v1 还是 v2 格式；同时参与 `binlog_row_value_options` 的启动期互斥检查 |
| sql/log_event.h | `Rows_log_event` | 行事件基类声明：虚继承 `binary_log::Rows_event` 与 `Log_event`（注释说明是为解决三个子类的菱形继承）；含 `row_lookup_mode` / `enum_error` 枚举、`m_cols` / `m_cols_ai` / `m_local_cols` / `write_set_backup` 位图与 `m_row_count` |

## 与主核 / AI 的关系

主核（`engine/db/`）有一套自己的日志实现，与 MySQL 的 binlog 没有代码级关联，但**在「写操作以何种粒度记入日志」这个问题上是同构的**，可作对照：

- **主核的 WAL 记的是「页/行的变更内容」，而不是「原始 SQL」。** 这与 MySQL 的行格式（`Rows_log_event` 记 before/after 镜像）在形态上一致：都是把「意图」翻译成「确定的状态转移」，重放端不需要重新执行原始语句。MySQL 的语句格式则相反——它记的是意图，重放端要重新求值。
- **主核同样需要「变更内容不能过大」的保护。** MySQL 侧的保护是 `binlog_row_event_max_size`（默认 8192 字节）控制单事件体积、`binlog_row_image`（`FULL` / `MINIMAL` / `NOBLOB`）控制单行镜像包含哪些列。主核侧对应的取舍需要另行核对主核 WAL 实现，本页不作断言。
- **主核也需要「哪些操作可以按原样重放」的分类。** MySQL 的做法是给语句打 unsafe 标记（`LEX::BINLOG_STMT_UNSAFE_*`），并按引擎能力（`HA_BINLOG_ROW_CAPABLE` / `HA_BINLOG_STMT_CAPABLE`）决定走哪条路。

**架构推论：** 主核 WAL 可以借鉴本页的两点设计——(1) 把「记什么粒度」做成**显式的能力集合**（MySQL 用引擎能力位 + unsafe 标记位），而不是靠语句类型硬编码；(2) 让「不可重放的操作」在**写入侧**就被降级或拒绝，而不是等到重放时才发现不一致。第二点是**从 MySQL `decide_logging_format` 的错误分支读出来的做法**（`ER_BINLOG_UNSAFE_AND_STMT_ENGINE` / `ER_BINLOG_STMT_MODE_AND_ROW_ENGINE`），主核侧是否存在等价实现需另行核对主核源码，本页不作断言。

## 参考

- [Replication Formats（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-formats.html) —— SBR / RBR / MIXED 三种格式的官方定义，`binlog_format` 的默认值 `ROW` 与自 8.0.34 起的 deprecated 状态。
- [Mixed Binary Logging Format（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/binary-log-mixed.html) —— MIXED 模式下从语句格式切到行格式的完整条件清单，以及按「语句类型 × binlog_format × 引擎能力（SLC/RLC）」给出的判定表。
- [Determination of Safe and Unsafe Statements in Binary Logging（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-rbr-safe-unsafe.html) —— 安全/不安全语句的官方分类，以及三种 `binlog_format` 下对 unsafe 语句的不同处置。
- [Binary Logging Options and Variables（MySQL 8.0 Reference Manual，Oracle 文档镜像）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/replication-options-binary-log.html) —— `binlog_format`、`binlog_row_image`（默认 `full`，取值 `full` / `minimal` / `noblob`）、`binlog_row_event_max_size`（默认 8192 字节、最小 256，官方明确定义为 soft limit）的官方说明。
- [sql/log_event.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/log_event.cc) —— 本页 refs 声明的两个符号 `Rows_log_event::Rows_log_event` 与 `Query_log_event::Query_log_event` 的权威来源，以及 `Rows_log_event::write_data_header` / `write_data_body` / `get_data_size` / `unpack_current_row` 等行事件实现。
- [sql/binlog.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/binlog.cc) —— `THD::decide_logging_format` 的格式判定、`THD::binlog_write_row` / `binlog_update_row` / `binlog_delete_row` 的镜像构造、`THD::binlog_prepare_pending_rows_event` 的事件合并与 `binlog_prepare_row_images` 的镜像裁剪。
- [sql/rpl_record.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/rpl_record.cc) —— `pack_row` / `unpack_row` 的行镜像打包与解包实现，含部分 JSON 更新的条件写入逻辑。
