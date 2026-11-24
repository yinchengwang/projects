---
project: mysql-server
feature: binlog-event-write
page_type: feature
schema_version: 1
refs:
  - path: sql/log_event.cc
    symbols: [Log_event::write, Rows_log_event]
  - path: sql/binlog.cc
    symbols: [MYSQL_BIN_LOG::write, MYSQL_BIN_LOG::do_write_cache]
---

# binlog 事件写入

## 一句话职责

`sql/log_event.cc` 与 `sql/binlog.cc` 共同实现 binlog 事件的序列化与落盘：`Log_event::write` 及其派生类把一个事件对象按 binlog 文件格式拆成「公共事件头 + 事件体 + 校验和」三段写进 `Basic_ostream`，`Rows_log_event` 则把行镜像、列位图与表标识写成本组的行事件；`MYSQL_BIN_LOG::do_write_cache` 是把事件缓存整体搬运进 binlog 文件的低层写入函数，向上由 `MYSQL_BIN_LOG::write_cache`、`MYSQL_BIN_LOG::write_transaction` 调用完成「GTID 事件先写、事务缓存后写」的顺序。

**边界（按 MECHANISM_SPECS 条目）：** 本机制负责把事件序列化成 binlog 文件格式并写入日志文件、维护事件缓存与事务边界事件；**不负责 binlog 的复制传输（M 组）**、**不负责 redo 日志（I 组）**。

## 机制是什么

### 一个事件在文件里的三段结构

`Log_event::write`（定义在 `sql/log_event.h` 的 `Log_event` 类内，`sql/log_event.cc` 里可见同名符号）是一个虚函数，函数体固定为四步串联：

```
return (write_header(ostream, get_data_size()) ||
        write_data_header(ostream) || write_data_body(ostream) ||
        write_footer(ostream));
```

四步里任何一步返回 `true` 就短路，后续不再写。因此一个事件在文件里的物理布局是：

1. **公共事件头**：`Log_event::write_header` 写固定长度的 `LOG_EVENT_HEADER_LEN` 字节缓冲。头部内容由 `Log_event::write_header_to_memory` 填：时间戳写入 `buf` 起始处，事件类型码写入 `buf[EVENT_TYPE_OFFSET]`，`server_id` 写入 `buf + SERVER_ID_OFFSET`，事件总长度写入 `buf + EVENT_LEN_OFFSET`，`log_pos` 写入 `buf + LOG_POS_OFFSET`，标志位写入 `buf + FLAGS_OFFSET`。
2. **事件体**：由 `write_data_header` 与 `write_data_body` 两个虚函数承载，基类默认实现都返回 `false`（不写任何字节），子类按事件类型覆写。
3. **校验和尾部**：`Log_event::write_footer` 在 `need_checksum()` 为真时写 `BINLOG_CHECKSUM_LEN` 字节的 CRC32 值，否则不写。

事件总长度的计算在 `Log_event::write_header` 里完成：

```
common_header->data_written = event_data_length + sizeof(header);

if (need_checksum()) {
  crc = checksum_crc32(0L, nullptr, 0);
  common_header->data_written += BINLOG_CHECKSUM_LEN;
}
```

即 `data_written` = 事件体长度 + `LOG_EVENT_HEADER_LEN` +（开启校验和时）`BINLOG_CHECKSUM_LEN`。**这个 `data_written` 既是写进头部的 `EVENT_LEN_OFFSET` 字段的值，也是 `MYSQL_BIN_LOG::write_event(Log_event *ev, Master_info *mi)` 累加 `bytes_written` 时用的量。**

### 头部长度有一个例外

`Log_event::write_header_to_memory` 的注释明确写着：头部对所有事件都是 `LOG_EVENT_HEADER_LEN` 字节，**除了 `FORMAT_DESCRIPTION_EVENT` 与 `ROTATE_EVENT`**——这两个事件用更短的 `LOG_EVENT_MINIMAL_HEADER_LEN`，因为读端在还不知道 binlog 格式的情况下就要先解析它们。

### 校验和的开关：`need_checksum()`

`Log_event::need_checksum()` 的判定分三支：

- 若 `common_footer->checksum_alg` 已被显式设为某个具体算法（不是 `BINLOG_CHECKSUM_ALG_UNDEF`），则结果取决于它是否等于 `BINLOG_CHECKSUM_ALG_OFF`。
- 否则，若 `binlog_checksum_options` 不为 `BINLOG_CHECKSUM_ALG_OFF` **且** `event_cache_type == Log_event::EVENT_NO_CACHE`，结果为 `binlog_checksum_options != 0`。
- 否则结果为 `false`。

也就是说：**走缓存的事件在写缓存阶段不算校验和**（此时 `log_pos` 还是无意义的 0，见 `write_header` 注释），校验和是在事件真正落进 binlog 文件时才补上。函数末尾还有一条断言：格式描述事件（FD）永远带校验和。

`Log_event::wrapper_my_b_safe_write` 是所有实际写字节的入口，它在写之前把数据并进 CRC：

```
if (size == 0) return false;

if (need_checksum() && size != 0) crc = checksum_crc32(crc, buf, size);

return ostream->write(buf, size);
```

### 写进缓存 vs 直接写进文件

事件缓存类型由 `Log_event::enum_event_cache_type` 定义，共四档（`EVENT_INVALID_CACHE`、`EVENT_STMT_CACHE`、`EVENT_TRANSACTIONAL_CACHE`、`EVENT_NO_CACHE`）。其中 `EVENT_NO_CACHE` 的注释是「该事件必须不经过任何缓存直接写入 binlog」。

日志方式由 `Log_event::enum_event_logging_type` 定义：`EVENT_NORMAL_LOGGING`（写进缓存，提交或回滚时落盘）与 `EVENT_IMMEDIATE_LOGGING`（写进空缓存后立刻落盘）。`Log_event::is_using_immediate_logging()` 判断后者。

`MYSQL_BIN_LOG::write_event(Log_event *event_info)`（`sql/binlog.cc`）是「把一个事件送进缓存」的入口，它的执行顺序是：

1. 若 `thd->binlog_evt_union.do_union` 为真（处于存储函数中），只置 `thd->binlog_evt_union.unioned_events = true` 并把 `unioned_events_trans` 与事件的 `is_using_trans_cache()` 取或，**事件本身不落盘**——函数返回时延迟到函数退出再统一记。
2. 计算 `end_stmt`：若 `thd->in_sub_stmt` 且 `sql_command == SQLCOM_SAVEPOINT` 则为真，否则取 `thd->locked_tables_mode && thd->lex->requires_prelocking()`。据此调用 `thd->binlog_flush_pending_rows_event(end_stmt, event_info->is_using_trans_cache())` 冲刷暂挂的行事件。
3. 过滤判断：若会话未开 `OPTION_BIN_LOG`，或命令是 `SQLCOM_ROLLBACK_TO_SAVEPOINT` / `SQLCOM_SAVEPOINT`，或（事件不是 `is_no_filter_event()` 且 `binlog_filter->db_ok(local_db)` 为假），则**直接返回 false，事件被丢弃**。
4. 调用 `binlog_start_trans_and_stmt(thd, event_info)`，再按 `event_info->is_using_trans_cache()` 取出事务缓存或语句缓存对象 `cache_data`。
5. 若当前语句不是行格式（`thd->is_current_stmt_binlog_format_row()` 为假），先补写上下文事件：`Intvar_log_event`（LAST_INSERT_ID_EVENT 与 INSERT_ID_EVENT 两类）、`Rand_log_event`、`User_var_log_event`（逐个用户变量）。
6. 真正写入事件本体：`cache_data->write_event(event_info)`。
7. 写失败时跳到 `err:`，调用 `report_cache_write_error(thd, is_trans_cache)`；若 `check_write_error(thd)` 为真且 `stmt_cannot_safely_rollback(thd)` 为真，则 `cache_data->set_incident()`。

### 缓存层：`binlog_cache_data::write_event`

`binlog_cache_data::write_event(Log_event *ev)` 负责把事件序列化进缓存的 `IO_CACHE`：

```
if (binary_event_serialize(ev, &m_cache)) { ... return 1; }
```

`binary_event_serialize` 是 `sql/log_event.h` 里的模板函数，实现只有一行 `return ev->write(ostream);`——它的存在意义是把「如何序列化」从调用方剥离，使调用方不受序列化过程变化影响。

写成功后，`binlog_cache_data::write_event` 逐项设置缓存的状态标志：

| 条件 | 标志 |
|---|---|
| `ev->get_type_code()` 是 `XID_EVENT` 或 `XA_PREPARE_LOG_EVENT` | `flags.with_xid = true` |
| `ev->is_using_immediate_logging()` | `flags.immediate = true` |
| `is_atomic_ddl_event(ev)` | `flags.with_xid = true`（DDL 在入缓存时就被标记为需要 xid） |
| `ev->is_sbr_logging_format()` | `flags.with_sbr = true` |
| `ev->is_rbr_logging_format()` | `flags.with_rbr = true` |
| `ev->starts_group()` | `flags.with_start = true` |
| `ev->ends_group()` | `flags.with_end = true` |
| 既不 starts_group 也不 ends_group | `flags.with_content = true` |

最后 `m_event_counter++` 记录本缓存里已缓存的事件数——这个计数后续被 `Gtid_log_event::set_trx_length_by_cache_size` 用来推算事务长度。

### 落盘层：`do_write_cache` 与 `Binlog_event_writer`

`MYSQL_BIN_LOG::do_write_cache(Binlog_cache_storage *cache, Binlog_event_writer *writer)` 的函数体极短：

```
bool error = false;
if (cache->copy_to(writer, &error)) {
  if (error) report_binlog_write_error();
  return true;
}
return false;
```

它只做一件事：把缓存里的字节搬进 `writer`。`copy_to` 出错时调 `report_binlog_write_error()`，该函数置 `write_error = true` 并打 `ERROR_LEVEL` 的 `ER_FAILED_TO_WRITE_TO_FILE`。函数开头还有一个调试注入点：`DBUG_EXECUTE_IF("simulate_do_write_cache_failure", ...)` 直接返回 `true`。

`Binlog_event_writer`（`sql/binlog.cc` 内的类定义）是真正的落盘执行者。它的类注释写明了这个类存在的理由：事件可能跨越多个缓存页，所以需要「分段喂入」的能力。它继承 `Basic_ostream`，构造函数接收目标 `Binlog_ofile`，并记录：

- `have_checksum = (binlog_checksum_options != binary_log::BINLOG_CHECKSUM_ALG_OFF)`；
- `initial_checksum = my_checksum(0L, nullptr, 0)`，`checksum` 初值相同；
- `end_log_pos = binlog_file->position()`——即落盘开始时的文件位置。

`Binlog_event_writer::write(const unsigned char *buffer, my_off_t length)` 的行为分两段：

**第一段（`event_len == 0`）：收集事件头。** 每次最多收 `std::min<uint32>(LOG_EVENT_HEADER_LEN - header_len, length)` 字节进 `header` 缓冲；收满 `LOG_EVENT_HEADER_LEN` 字节后调 `update_header()`。

`update_header()` 做四件事：

```
event_len = uint4korr(header + EVENT_LEN_OFFSET);
end_log_pos += event_len;
if (have_checksum) {
  int4store(header + EVENT_LEN_OFFSET, event_len + BINLOG_CHECKSUM_LEN);
  end_log_pos += BINLOG_CHECKSUM_LEN;
}
int4store(header + LOG_POS_OFFSET, end_log_pos);
if (have_checksum) checksum = my_checksum(checksum, header, header_len);
```

即：**先把头部里记录的事件长度读出来作为「还剩多少字节没写」的计数，再用它加上校验和长度改写回头部，把 `end_log_pos` 推进到本事件的末尾，最后把头部并进 CRC。**

**第二段（`event_len != 0`）：写事件体。** 每次写 `std::min<uint64>(length, event_len)` 字节进文件，并把这些字节并进 CRC；`event_len` 减到 0 时，若开启校验和，再写 `BINLOG_CHECKSUM_LEN` 字节的 CRC 值，并把 `checksum` 复位回 `initial_checksum`。

**架构推论：** 头部的 `LOG_POS_OFFSET` 字段与事件长度字段是在 `Binlog_event_writer` 里改写的，而不是在 `Log_event::write_header` 里——因为 `Log_event::write_header` 写缓存时还不知道自己在文件里的最终位置。类注释里「The following fields are fixed before writing the event: end_log_pos is set / the checksum is computed if checksums are enabled / the length is incremented by the checksum size if checksums are enabled」正是这个分工的书面说明。

### 上层：`write_cache` 与 `write_transaction`

`MYSQL_BIN_LOG::write_cache(THD *thd, binlog_cache_data *cache_data, Binlog_event_writer *writer)` 是「把整个缓存刷进文件」的函数。它的函数注释把自己与 `do_write_cache` 的关系写得很清楚：

> Comparison with do_write_cache:
> - do_write_cache is a lower-level function that only performs the actual write.
> - write_cache is a higher-level function that calls do_write_cache and additionally performs some maintenance tasks, including: report any errors that occurred / update gtid_state / update thd.binlog_next_event_pos

函数体的执行顺序是：

1. 取缓存对象 `cache_data->get_cache()`，断言持有 `LOCK_log`。
2. `assert(is_open())`；在 `likely(is_open())` 分支内，若 `!cache->is_empty()` 才真正刷。
3. 调试点 `crash_before_writing_xid` 会在 `do_write_cache` 之后 `flush_and_sync(true)` 再 `DBUG_SUICIDE()`。
4. 调 `do_write_cache(cache, writer)`，失败则 `goto err`。
5. 成功后调 `update_thd_next_event_pos(thd)`。
6. 出错路径：`thd->commit_error = THD::CE_FLUSH_ERROR`，返回 `true`。

`MYSQL_BIN_LOG::write_transaction(THD *thd, binlog_cache_data *cache_data, Binlog_event_writer *writer, bool parallelization_barrier)` 负责写出**事务边界事件**。它的顺序是：

1. 断言 `thd->owned_gtid.sidno` 合法（要么是 `THD::OWNED_SIDNO_ANONYMOUS`，要么大于 0）。
2. `m_dependency_tracker.get_dependency(thd, parallelization_barrier, sequence_number, last_committed)` 取得 MTS 用的逻辑时间戳。
3. 置 `thd->get_transaction()->last_committed = SEQ_UNINIT`。
4. 计算提交时间戳：`immediate_commit_timestamp = my_micro_time()`；`original_commit_timestamp` 取 `thd->variables.original_commit_timestamp`，为 `UNDEFINED_COMMIT_TIMESTAMP` 时——若是 `thd->slave_thread` 或 `thd->is_binlog_applier()` 则置 0（时间戳未知），否则置为 `immediate_commit_timestamp`。
5. 构造 `Gtid_log_event gtid_event(thd, cache_data->is_trx_cache(), last_committed, sequence_number, cache_data->may_have_sbr_stmts(), original_commit_timestamp, immediate_commit_timestamp, trx_original_server_version, trx_immediate_server_version)`。
6. `gtid_event.set_trx_length_by_cache_size(cache_data->get_byte_position(), writer->is_checksum_enabled(), cache_data->get_event_counter())` 设置事务长度。
7. `gtid_event.write(writer)`——**GTID 事件先于事务内容写出**。
8. 再调 `mysql_bin_log.write_cache(thd, cache_data, writer)` 写出事务本体。
9. 成功后更新事务压缩的监控统计。

`Gtid_log_event::set_trx_length_by_cache_size`（`sql/log_event.cc`）是本页唯一的「长度算式」，逐项加和：

```
transaction_length = cache_size;
if (is_checksum_enabled)
  transaction_length += event_counter * BINLOG_CHECKSUM_LEN;

transaction_length += LOG_EVENT_HEADER_LEN;
transaction_length += POST_HEADER_LENGTH;
transaction_length += get_commit_timestamp_length();
transaction_length += get_server_version_length();
transaction_length += is_checksum_enabled ? BINLOG_CHECKSUM_LEN : 0;

transaction_length += TRANSACTION_LENGTH_MIN_LENGTH;
if (transaction_length >= 251ULL) {
  transaction_length += 2;
  if (transaction_length >= 65536ULL) {
    transaction_length += 1;
    if (transaction_length >= 16777216ULL) {
      transaction_length += 5;
    }
  }
}
```

三个阈值 `251ULL` / `65536ULL` / `16777216ULL` 决定长度字段自身占几字节：源码注释给的例子是「假设事务长度不含长度字段时是 250，用 `net_length_size(250)` 会返回 1，但加上长度字段本身（+1）后变成 251，这个字段就得用两个字节表示，于是真实长度是 253」。函数里对应的断言把每一档的净增量钉死为 1 / 3 / 4 / 9 字节。

`Gtid_log_event::write_body_to_memory` 逐字段写 GTID 事件体：先写 `immediate_commit_timestamp`，若它与 `original_commit_timestamp` 不同则置最高位（`1ULL << ENCODED_COMMIT_TIMESTAMP_LENGTH`）作「本事务非本机产生」的提示；再按需写 `original_commit_timestamp`；然后 `net_store_length` 写事务长度；接着同样规则写 `immediate_server_version` 与 `original_server_version`；最后若 `commit_group_ticket` 不等于 `binlog::BgcTicket::kTicketUnset` 则写 `COMMIT_GROUP_TICKET_LENGTH` 字节。

### 行事件：`Rows_log_event`

`Rows_log_event` 是 `sql/log_event.h` 里声明的类，`class Rows_log_event : public virtual binary_log::Rows_event, public Log_event`。它承载行镜像数据：成员 `m_rows_buf`（打包好的行数据）、`m_rows_cur`（数据末尾的下一个指针）、`m_rows_end`（已分配空间的末尾），以及列位图 `m_cols`（before image）与 `m_cols_ai`（after image，仅 Update 事件用）、`m_local_cols` 与 `m_local_cols_ai`（本地可用列的位图）、表标识 `m_table_id`、标志 `m_flags`、行数 `m_row_count`。

行数据由 `Rows_log_event::do_add_row_data(uchar *row_data, size_t length)` 追加：长度为 0 时只 `m_row_count++` 并返回 0（源码注释说明这**不是**优化，因为 `realloc(size=0)` 等价于 `free()`）。

**数据大小**由 `Rows_log_event::get_data_size()` 算出（`sql/log_event.cc`）：

```
int data_size = 0;
bool is_v2_event = common_header->type_code > binary_log::DELETE_ROWS_EVENT_V1;
if (is_v2_event) {
  data_size = Binary_log_event::ROWS_HEADER_LEN_V2;
  if (m_extra_row_info.have_ndb_info())
    data_size += EXTRA_ROW_INFO_TYPECODE_LENGTH + m_extra_row_info.get_ndb_length();
  if (m_extra_row_info.have_part())
    data_size += EXTRA_ROW_INFO_TYPECODE_LENGTH + m_extra_row_info.get_part_length();
} else {
  data_size = Binary_log_event::ROWS_HEADER_LEN_V1;
}
data_size += no_bytes_in_map(&m_cols);
data_size += (uint)(end - buf);
if (general_type_code == binary_log::UPDATE_ROWS_EVENT)
  data_size += no_bytes_in_map(&m_cols_ai);
data_size += (uint)(m_rows_cur - m_rows_buf);
```

其中 `end - buf` 是 `net_store_length(buf, m_width)` 的返回值——**写的是位宽而不是字节数**（`write_data_body` 的注释明确指出这一点）。另有一条调试分支 `old_row_based_repl_4_byte_map_id_source`，此时长度按 6 字节表映射头计算。

**写事件头**由 `Rows_log_event::write_data_header(Basic_ostream *ostream)` 完成：

- 调试分支 `old_row_based_repl_4_byte_map_id_source` 下写 6 字节（4 字节表 id + 2 字节 `m_flags`）。
- 正常路径用 `int6store(buf + ROWS_MAPID_OFFSET, m_table_id.id())` 写 6 字节表标识、`int2store(buf + ROWS_FLAGS_OFFSET, m_flags)` 写标志。
- 若 `log_bin_use_v1_row_events` 为假（v2 事件），还要写变长头：先算 `extra_row_info_payloadlen`（起始为 `EXTRA_ROW_INFO_HEADER_LENGTH`，有 NDB 信息再加 `EXTRA_ROW_INFO_TYPECODE_LENGTH + m_extra_row_info.get_ndb_length()`，有分区信息再加 `EXTRA_ROW_INFO_TYPECODE_LENGTH + m_extra_row_info.get_part_length()`），用 `int2store(buf + ROWS_VHLEN_OFFSET, extra_row_info_payloadlen)` 写入「变长头长度含自身」；随后按 NDB、分区两块分别写出类型码与载荷。
- 分区信息的写出对 `binary_log::UPDATE_ROWS_EVENT` 特殊：更新事件要写两个分区 id——先写目标分区 `m_extra_row_info.get_partition_id()`，再写来源分区 `m_extra_row_info.get_source_partition_id()`，每个占 `EXTRA_ROW_PART_INFO_VALUE_LENGTH` 字节。

**写事件体**由 `Rows_log_event::write_data_body(Basic_ostream *ostream)` 完成，顺序是：

1. `net_store_length(sbuf, (size_t)m_width)` 写出列位宽（单位是**位**）。
2. 写 `m_cols.bitmap` 的 `no_bytes_in_map(&m_cols)` 字节。
3. 若事件类型是 `binary_log::UPDATE_ROWS_EVENT`，再写 `m_cols_ai.bitmap` 的 `no_bytes_in_map(&m_cols_ai)` 字节。
4. 写行数据本体 `m_rows_buf` 的 `m_rows_cur - m_rows_buf` 字节。

这四步的字节数与 `get_data_size()` 的四项加和**一一对应**——`get_data_size` 就是 `write_data_body` 的字节预算表。

### 事件类型码与行事件基类

`Rows_log_event` 声明了纯虚函数 `virtual Log_event_type get_general_type_code() = 0;`（General rows op type, no version），子类（`Write_rows_log_event` / `Update_rows_log_event` / `Delete_rows_log_event`）各自实现。`Rows_log_event` 还定义了 `enum row_lookup_mode`（`ROW_LOOKUP_UNDEFINED` / `ROW_LOOKUP_NOT_NEEDED` / `ROW_LOOKUP_INDEX_SCAN` / `ROW_LOOKUP_TABLE_SCAN` / `ROW_LOOKUP_HASH_SCAN`）与 `enum enum_error`（`ERR_OPEN_FAILURE` = -1、`ERR_OK` = 0、`ERR_TABLE_LIMIT_EXCEEDED` = 1、`ERR_OUT_OF_MEM` = 2、`ERR_BAD_TABLE_DEF` = 3、`ERR_RBR_TO_SBR` = 4）——**这些是应用端（从库重放）用的错误码，不是写端**。

### 表映射事件：`Table_map_log_event`

行事件之前必须有表映射事件说明表结构。`Table_map_log_event::write_data_header` 写 6 字节表 id（`int6store(buf + TM_MAPID_OFFSET, ...)`）与 2 字节标志（`int2store(buf + TM_FLAGS_OFFSET, m_flags)`），共 `Binary_log_event::TABLE_MAP_HEADER_LEN` 字节。`Table_map_log_event::write_data_body` 依次写：库名长度 + 库名、表名长度 + 表名、列数长度 + 列数、列类型数组 `m_coltype`（`m_colcnt` 字节）、字段元数据长度。

### 其他典型事件

- `Xid_log_event::write`：`write_header(ostream, sizeof(xid))` + 写 `xid` 字段 + `write_footer`。它是事务提交边界事件。
- `Query_log_event::write`：自己拼装头部缓冲，缓冲区大小是 `Binary_log_event::QUERY_HEADER_LEN + MAX_SIZE_LOG_EVENT_STATUS`；函数开头的注释说明它必须自己改写 `EVENT_LEN_OFFSET`，因为在写之前还不知道要输出多少状态变量。
- `Format_description_log_event::write`：写 `binlog_version`、服务器版本串、创建时间，并把 `LOG_EVENT_HEADER_LEN` 写进 `ST_COMMON_HEADER_LEN_OFFSET` 作为「公共头长度」；后面跟各事件类型的后置头长度数组 `post_header_len`，末尾追加 1 字节的校验和算法描述符（`BINLOG_CHECKSUM_ALG_DESC_LEN`，源码里有 `static_assert(BINLOG_CHECKSUM_ALG_DESC_LEN == 1, "")`）。
- `Rotate_log_event::write`、`Intvar_log_event::write`、`Rand_log_event::write`、`User_var_log_event::write`、`Append_block_log_event::write`、`Delete_file_log_event::write` 等各自实现自己的 `write`。

## 怎么实现的

### 一次行事务的完整落盘时序

**第一步：SQL 执行期，事件进缓存。**

`MYSQL_BIN_LOG::write_event(Log_event *event_info)` 被调用后，事件经 `binlog_cache_data::write_event` 序列化进 `IO_CACHE`。此时 `Log_event::write_header` 写出的头部长度字段里记录的事件总长是**不含校验和**的（因为 `need_checksum()` 对缓存事件返回 `false`），`log_pos` 也是 0。

**第二步：提交时，GTID 事件先写。**

`binlog_cache_data::flush`（`sql/binlog.cc`）构造 `Binlog_event_writer writer(mysql_bin_log.get_binlog_file())`，随后调 `mysql_bin_log.write_transaction(thd, this, &writer, parallelization_barrier)`。`write_transaction` 里 `gtid_event.write(writer)` 把 GTID 事件直接写进文件。

**第三步：事务本体写进文件。**

`write_transaction` 接着调 `mysql_bin_log.write_cache(thd, cache_data, writer)`，`write_cache` 内部调 `do_write_cache(cache, writer)`，后者调 `cache->copy_to(writer, &error)` 把缓存里已序列化的字节（含已算好的事件头）交给 `Binlog_event_writer::write`。

**第四步：`Binlog_event_writer` 逐段拼装并写文件。**

`Binlog_event_writer::write` 走的是「先收满 `LOG_EVENT_HEADER_LEN` 字节头 → 改写头的长度与 `log_pos` → 写事件体 → 写校验和」的循环。事件体可能跨多个缓存页，所以 `write` 允许被多次调用，每次只推进 `event_len` 剩余计数。

**第五步：推进位置。**

`MYSQL_BIN_LOG::write_event_to_binlog`（直接写文件路径，不经缓存）里：

```
ev->common_footer->checksum_alg =
    is_relay_log ? relay_log_checksum_alg
                 : static_cast<enum_binlog_checksum_alg>(binlog_checksum_options);
assert(ev->common_footer->checksum_alg != binary_log::BINLOG_CHECKSUM_ALG_UNDEF);

ev->common_header->log_pos = m_binlog_file->position();

if (binary_event_serialize(ev, m_binlog_file)) return true;

add_bytes_written(ev->common_header->data_written);
```

注意顺序：**先把 `log_pos` 设成事件在文件中的起始位置，再序列化**——`write_header` 里正是靠这个非 0 的 `log_pos` 在事件写完后把它加上 `data_written` 变成事件的结束位置。源码注释写明了这个约定的由来：「Usually events are written into binlog cache first... So it is set to 0. The log_pos field will be updated later when the events are being flushed into binlog file. In a few cases (e.g. rotation (FD, Rotate events)), events are written into binlog file directly through event->write(). In these cases, log_pos is updated to the begin position of the event before calling event->write().」

**第六步：关闭时写 Stop 事件并清标志位。**

`MYSQL_BIN_LOG::close` 在 `LOG_CLOSE_STOP_EVENT` 标志位下构造 `Stop_log_event s`，同样设置 `checksum_alg`，然后 `if (!write_event_to_binlog(&s) && !m_binlog_file->flush()) update_binlog_end_pos();`。关闭时（非 relay log）还要把文件头 `BIN_LOG_HEADER_SIZE + FLAGS_OFFSET` 处的 1 字节标志清零——源码注释说明这是「清除 `LOG_EVENT_BINLOG_IN_USE_F`」。

### relay log 的对照路径

`MYSQL_BIN_LOG::write_event(Log_event *ev, Master_info *mi)` 是 relay log 专用的重载，函数体开头 `assert(is_relay_log)`。成功时 `bytes_written += ev->common_header->data_written` 并调 `after_write_to_relay_log(mi)`；失败时向 `mi` 报 `ER_REPLICA_RELAY_LOG_WRITE_FAILURE`，并调 `truncate_relaylog_file(mi, atomic_binlog_end_pos)` 把文件截回已提交位置。同文件里的 `MYSQL_BIN_LOG::write_buffer(const char *buf, uint len, Master_info *mi)` 是对应的裸字节版本，差别只在于没有事件头要拼。

## 关键取舍

### 缓存序列化 vs 直接写文件：两套长度与校验和口径

事件先写进缓存、提交时再整体落盘，好处是**事务要么完整落盘、要么完全不落盘**（官方文档称「Only complete events or transactions are logged or read back」），也让 group commit 能把多个事务合并成一次批量写。代价是同一个事件在两个阶段有两套长度口径：

- 写缓存阶段，`Log_event::write_header` 算出的 `data_written` **不含**校验和长度，`need_checksum()` 对缓存事件返回 `false`。
- 落盘阶段，`Binlog_event_writer::update_header` 要把头的 `EVENT_LEN_OFFSET` 改写成 `event_len + BINLOG_CHECKSUM_LEN`，并把 `end_log_pos` 额外推进 `BINLOG_CHECKSUM_LEN`。

**取舍：多了一次头部改写与一次长度口径切换，换来了「事务级原子落盘」与「批量写」两个更重要的性质。**

### 头部改写放在 writer 而不是事件对象

`end_log_pos` 与最终事件长度的计算放在 `Binlog_event_writer` 而不是 `Log_event::write_header`，是因为事件被写进缓存时还不知道自己在文件中的最终位置。源码注释直接点明了这一点：「When events are being written into binlog cache, log_pos (a.k.a. end_log_pos) field is meaningless.」

**取舍：事件对象的序列化逻辑保持「与位置无关」，代价是落盘阶段必须有一个能改写事件头的中间层。** 这个中间层还要处理「事件跨缓存页」的问题，因此 `Binlog_event_writer::write` 是一个带状态的可重入循环，而不是简单的 `fwrite`。

### 直接写文件路径保留但收窄

`MYSQL_BIN_LOG::write_event_to_binlog` 与 `write_event_to_binlog_and_sync` 提供了一条**不经过缓存**的直写路径（`write_event_to_binlog_and_sync` 在写完后额外 `flush()` 并 `sync()`）。但源码里明确限定了它的适用范围：直接写文件只在「rotation (FD, Rotate events)」这类少数场合使用。

**取舍：牺牲通用性换正确性**——直写路径必须自己设置 `log_pos` 与 `checksum_alg`，任何忘记都会产生不可解析的事件，所以只开给少数受控场景。

### 校验和默认开启但可用 `binlog_checksum` 关闭

官方文档说明：`binlog_checksum` 系统变量控制是否写校验和，默认开启。源码里 `need_checksum()` 的三支判定还额外照顾了「调用方自带偏好」的少数事件类型——`BINLOG_CHECKSUM_ALG_OFF`、`STOP_EVENT`、`ROTATE_EVENT`、`PREVIOUS_GTIDS_LOG_EVENT`、`FORMAT_DESCRIPTION_EVENT`、`VIEW_CHANGE_EVENT` 在断言里被显式豁免。

**取舍：开启校验和能发现事件损坏，代价是每个事件多 `BINLOG_CHECKSUM_LEN` 字节并多一次 CRC 计算。** FD 事件被豁免的原因是它的校验和算法描述符本身就是格式的一部分。

### `do_write_cache` 极简，错误处理上移

`do_write_cache` 只做「搬运 + 上报错误」，不更新 `gtid_state`、不更新 `thd.binlog_next_event_pos`。这两项维护任务被上移到 `write_cache`。

**取舍：低层函数保持单一职责、可独立测试；但调用方必须记得走高层入口，否则会漏掉状态更新。** 源码注释把这条分工写成了显式对照表（见上文「怎么实现的」小节）。

### 事务长度的自描述长度字段

`Gtid_log_event::set_trx_length_by_cache_size` 里的长度字段用 `net_store_length` 编码，且长度字段自身的字节数会计入长度值——源码注释专门解释了为什么不能用 `net_length_size()` 一步算出来。

**取舍：多一次条件判断（`>= 251ULL` / `>= 65536ULL` / `>= 16777216ULL` 三档递进），换来从库能在读到长度的同时立刻知道还剩多少字节要读。**

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/log_event.h | `Log_event::write` | 事件序列化的虚基类实现：固定按「写头 → 写数据头 → 写数据体 → 写页脚」四步串联，任一步返回 `true` 即短路；定义在 `Log_event` 类内 |
| sql/log_event.h | `binary_event_serialize` | 模板包装，函数体仅 `return ev->write(ostream);`，把序列化细节从调用方剥离 |
| sql/log_event.h | `Log_event::enum_event_cache_type` | 缓存类型四档：`EVENT_INVALID_CACHE`、`EVENT_STMT_CACHE`、`EVENT_TRANSACTIONAL_CACHE`、`EVENT_NO_CACHE` |
| sql/log_event.h | `Log_event::enum_event_logging_type` | 日志方式两档：`EVENT_NORMAL_LOGGING`（入缓存，提交时落盘）与 `EVENT_IMMEDIATE_LOGGING`（入空缓存后立刻落盘） |
| sql/log_event.h | `Rows_log_event` | 行事件基类：持有 `m_rows_buf` / `m_rows_cur` / `m_rows_end`、列位图 `m_cols` 与 `m_cols_ai`、表标识 `m_table_id`、行数 `m_row_count`；声明 `write_data_header` / `write_data_body` 覆写点 |
| sql/log_event.h | `Rows_log_event::row_lookup_mode` | 从库端行查找模式的枚举：`ROW_LOOKUP_UNDEFINED` / `ROW_LOOKUP_NOT_NEEDED` / `ROW_LOOKUP_INDEX_SCAN` / `ROW_LOOKUP_TABLE_SCAN` / `ROW_LOOKUP_HASH_SCAN` |
| sql/log_event.cc | `Log_event::need_checksum` | 判定本事件是否带校验和；区分「调用方已指定算法」「`binlog_checksum_options` 不为 OFF 且事件为 `EVENT_NO_CACHE`」「否则不带」三支，并断言 FD 事件必带校验和 |
| sql/log_event.cc | `Log_event::wrapper_my_b_safe_write` | 所有写字节的入口：先把数据并进 CRC32，再 `ostream->write` |
| sql/log_event.cc | `Log_event::write_footer` | 写校验和尾部：`need_checksum()` 为真时写 `BINLOG_CHECKSUM_LEN` 字节的 CRC 值 |
| sql/log_event.cc | `Log_event::write_header` | 写公共事件头：算 `data_written`（事件体长 + `LOG_EVENT_HEADER_LEN` + 可选 `BINLOG_CHECKSUM_LEN`）、重置 CRC、在 `log_pos` 非 0 时把它推进到事件末尾、清 `LOG_EVENT_BINLOG_IN_USE_F` 标志 |
| sql/log_event.cc | `Log_event::write_header_to_memory` | 填头部缓冲各字段：时间戳、`EVENT_TYPE_OFFSET` 事件类型、`SERVER_ID_OFFSET`、`EVENT_LEN_OFFSET` 长度、`LOG_POS_OFFSET` 位置、`FLAGS_OFFSET` 标志；返回 `LOG_EVENT_HEADER_LEN` |
| sql/log_event.cc | `Rows_log_event::get_data_size` | 算行事件体字节数：v1/v2 头长度 + NDB/分区扩展 + 列位图字节 + 位宽长度 + 行数据长度（更新事件额外含 `m_cols_ai` 位图） |
| sql/log_event.cc | `Rows_log_event::do_add_row_data` | 把一行镜像追加进 `m_rows_buf`；长度为 0 时只增 `m_row_count` 并返回 0 |
| sql/log_event.cc | `Rows_log_event::write_data_header` | 写行事件头：6 字节表 id + 2 字节标志；v2 事件另写变长头长度与 NDB / 分区扩展块（更新事件写两个分区 id） |
| sql/log_event.cc | `Rows_log_event::write_data_body` | 写行事件体：列位宽（以位为单位）+ `m_cols` 位图 +（更新事件）`m_cols_ai` 位图 + 行数据本体 |
| sql/log_event.cc | `Table_map_log_event::write_data_header` | 写表映射事件头：6 字节表 id + 2 字节标志，共 `TABLE_MAP_HEADER_LEN` 字节 |
| sql/log_event.cc | `Table_map_log_event::write_data_body` | 写表映射事件体：库名、表名、列数、列类型数组、字段元数据长度 |
| sql/log_event.cc | `Gtid_log_event::write_data_header` | 写 GTID 事件头：`write_post_header_to_memory` 后写出 `POST_HEADER_LENGTH` 字节 |
| sql/log_event.cc | `Gtid_log_event::write_body_to_memory` | 写 GTID 事件体：提交时间戳（含「非本机产生」最高位标记）、原始提交时间戳、事务长度、服务器版本与原始版本、group commit ticket |
| sql/log_event.cc | `Gtid_log_event::set_trx_length_by_cache_size` | 按缓存字节数、事件计数与校验和开关推算事务长度，并按 `251ULL` / `65536ULL` / `16777216ULL` 三档阈值决定长度字段自身占 1 / 3 / 4 / 9 字节 |
| sql/log_event.cc | `Xid_log_event::write` | 事务提交边界事件：写头 + 写 `xid` 字段 + 写页脚 |
| sql/log_event.cc | `Query_log_event::write` | 语句事件：自建头部缓冲（`QUERY_HEADER_LEN + MAX_SIZE_LOG_EVENT_STATUS`），因事先不知状态变量数量而自行改写 `EVENT_LEN_OFFSET` |
| sql/log_event.cc | `Format_description_log_event::write` | 格式描述事件：写版本、服务器版本串、创建时间、公共头长度与各事件后置头长度数组，末尾追加 1 字节校验和算法描述符 |
| sql/binlog.cc | `Binlog_event_writer` | 落盘执行者类：构造时记录 `have_checksum`、`initial_checksum` 与起始 `end_log_pos`；`update_header` 改写事件长度与 `log_pos` 并把头部并入 CRC；`write` 支持事件跨缓存页的分段写入，事件体写完后补写校验和并复位 `checksum` |
| sql/binlog.cc | `binlog_cache_data::write_event` | 把事件序列化进缓存的 `IO_CACHE`（经 `binary_event_serialize`），并按事件类型设置 `with_xid` / `immediate` / `with_sbr` / `with_rbr` / `with_start` / `with_end` / `with_content` 标志，最后递增 `m_event_counter` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_event` | 把一个事件送进缓存的入口：处理存储函数合并、写前冲刷暂挂行事件、按 `OPTION_BIN_LOG` 与 `binlog_filter->db_ok` 过滤、补写 `Intvar` / `Rand` / `User_var` 上下文事件，失败时 `report_cache_write_error` 并在必要时 `set_incident` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write` | binlog 写入入口族：`write_transaction` 写 GTID 与事务体、`write_cache` 刷缓存、`write_event` 两版（缓存版与 relay log 版）、`write_event_to_binlog` 直写文件、`write_event_to_binlog_and_sync` 直写并刷盘同步、`write_buffer` 写裸字节、`write_stmt_directly` 直接记语句 |
| sql/binlog.cc | `MYSQL_BIN_LOG::do_write_cache` | 低层写入函数：调 `cache->copy_to(writer, &error)` 把缓存搬进 writer，失败时 `report_binlog_write_error`；带 `simulate_do_write_cache_failure` 调试注入点 |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_cache` | 高层刷缓存函数：持 `LOCK_log` 断言、判 `is_open()`、调 `do_write_cache`、成功后 `update_thd_next_event_pos`，失败置 `thd->commit_error = THD::CE_FLUSH_ERROR` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_transaction` | 写事务边界事件：取 MTS 依赖的 `sequence_number` 与 `last_committed`、计算提交与原始提交时间戳、构造并写出 `Gtid_log_event`、再调 `write_cache` 写事务体，最后更新事务压缩监控 |
| sql/binlog.cc | `MYSQL_BIN_LOG::report_binlog_write_error` | 写盘错误上报：置 `write_error = true`，打 `ERROR_LEVEL` 的 `ER_FAILED_TO_WRITE_TO_FILE` |
| sql/binlog.cc | `MYSQL_BIN_LOG::write_event_to_binlog` | 不经缓存直写文件：设定 `checksum_alg`、把 `log_pos` 设为事件起始位置、序列化后 `add_bytes_written` |
| sql/binlog.cc | `binlog_cache_data::flush` | 缓存刷盘编排：构造 `Binlog_event_writer`、检查 `commit_error`、调 `write_transaction`、按 `flags.with_xid` 回填 `wrote_xid`、最后 `reset()` 缓存 |

## 与主核 / AI 的关系

主核（`engine/db/`）有自己的 WAL 层，与本页的 binlog 没有代码级关联，但机制形态上有可对照之处：

- **主核 WAL 与 binlog 都是「先记日志、再改数据」的顺序纪律**，但服务的目的不同：主核 WAL 服务崩溃恢复，binlog 服务复制与基于时间点恢复。
- **主核 WAL 同样把「写」与「刷盘」分成两级**，对应本组 I01 页的 `log.write_lsn` 与 `log.flushed_to_disk_lsn`；本页对应的两级是 `Binlog_event_writer` 把事件写进 binlog 文件（`m_binlog_file->write`）与随后的 `m_binlog_file->sync()`，后者在 `MYSQL_BIN_LOG::write_event_to_binlog_and_sync` 里成对出现。

**架构推论：** 本页的 `Binlog_event_writer` 与主核 WAL 的「分段写入 + 事后统一提交校验和」在形态上是同构的——两者都选择把校验和放在数据写完之后单独追加，而不是边写边算。本页的依据是 `Binlog_event_writer::update_header` 里「先按头部里的长度读出事件总长、算出 `end_log_pos`、再把长度改写成 `event_len + BINLOG_CHECKSUM_LEN`、最后把头部并入 CRC」这一串动作；主核 WAL 侧是否存在等价实现需另行核对主核源码，本页不作断言。

## 参考

- [The Binary Log（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/binary-log.html) —— binlog 的官方定位：记录数据变更与可能产生变更的语句、只记录完整事件或完整事务、`binlog_checksum` 与 `sync_binlog` 的语义、事务在提交前整体写入 binlog 的时序。
- [Binary Logging Formats（MySQL 8.0 Reference Manual）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/binary-log-formats.html) —— 行格式 / 语句格式 / 混合格式的官方定义，是 `Rows_log_event` 与 `Query_log_event` 两类事件分工的上位背景。
- [Redo Log（MySQL 8.0 Reference Manual，InnoDB 内部机制页）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-redo-log.html) —— InnoDB redo 日志的官方说明，用于界定「binlog 不负责 redo（I 组）」这条边界。
- [sql/binlog.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/binlog.cc) —— `Binlog_event_writer`、`do_write_cache`、`write_cache`、`write_transaction` 与 `write_event` 各族的权威来源。
- [sql/log_event.cc（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/log_event.cc) —— `Log_event::write_header` / `write_footer` / `need_checksum`、`Rows_log_event` 的三个写函数与 `Gtid_log_event::set_trx_length_by_cache_size` 的权威来源。
- [sql/log_event.h（MySQL Server 8.0 源码，GitHub）](https://github.com/mysql/mysql-server/blob/8.0/sql/log_event.h) —— `Log_event` 与 `Rows_log_event` 的类定义、`Log_event::write` 的虚函数实现、`binary_event_serialize` 模板与缓存类型枚举的定义处。
