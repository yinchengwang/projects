---
project: mysql-server
feature: prepare-execute-protocol
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_prepare.cc
    symbols: [Prepared_statement]
---

# 预处理语句的两阶段执行

## 一句话职责

`sql/sql_prepare.cc` 里的 `Prepared_statement` 是 MySQL 预处理语句的句柄对象：它把「一次 COM_STMT_PREPARE 的解析与元数据准备」和「后续若干次 COM_STMT_EXECUTE 的参数装配与执行」分成两个阶段，中间用 `thd->stmt_map` 里的句柄 ID 把两阶段缝在一起，并负责在元数据变化时重整（reprepare）以及在长数据（long data）分片送来的情况下把错误推迟到 execute 时才报。

## 机制是什么

**协议层是两阶段、三种命令加两个辅助命令。** 客户端先发 `COM_STMT_PREPARE` 加查询文本；服务端解析后回一个语句 ID、参数个数与参数/结果集元数据，但**不执行**语句。客户端随后反复发 `COM_STMT_EXECUTE`（带语句 ID 与参数值）复用同一份解析结果。`COM_STMT_SEND_LONG_DATA` 让客户端把超长参数分片送来，`COM_STMT_FETCH` 取游标行，`COM_STMT_CLOSE` 释放句柄，`COM_STMT_RESET` 把句柄复位回 prepare 后的状态。

**阶段一（prepare）只解析与校验，不产生副作用。** 文件头部注释写明了这条边界：prepare 期间语句用到的表会被打开，但**不取任何锁**，因为此时既不读也不改数据；表在 prepare 结束后被关闭。这一设计的直接后果写在 `Prepared_statement::prepare()` 里：prepare 结束前显式调 `close_thread_tables(thd)`，并用 `thd->mdl_context.rollback_to_savepoint(mdl_savepoint)` 回滚校验期间取得的元数据锁——`mdl_savepoint` 正是为此在 `prepare()` 里取的快照。

**阶段二（execute）才真正开表加锁并跑语句。** `Prepared_statement::execute()` 调 `mysql_execute_command(thd, true)` 完成执行，表的打开与加锁方式与普通（非预处理）语句执行完全一致，执行后解锁关表。

**同一份代码服务两套前端协议。** 二进制协议（`COM_STMT_*`）与 SQL 层协议（`PREPARE stmt FROM ...` / `EXECUTE stmt USING ...`）复用同一批 `mysqld_stmt_prepare()` / `mysqld_stmt_execute()` / `mysql_sql_stmt_prepare()` / `mysql_sql_stmt_execute()` 函数，区别只在参数的来源与结果集的发送协议。`mysqld_stmt_prepare()` 与 `mysqld_stmt_execute()` 在 classic 协议下会先 `thd->push_protocol(thd->protocol_binary)` 切到二进制协议并在结束时 `thd->pop_protocol()` 弹回。

**句柄的复用范围是「一条连接内的语句表」。** `Prepared_statement` 对象由 `thd->stmt_map` 持有，`mysql_stmt_precheck()` 用 `thd->stmt_map.find(stmt_id)` 按 ID 找回句柄；SQL 层协议则用 `thd->stmt_map.find_by_name(name)` 按名字找回。

## 怎么实现的

### 阶段一：prepare

`mysqld_stmt_prepare(THD *, const char *query, uint length, Prepared_statement *stmt)` 是二进制协议侧的入口。它做四件事：

1. 若当前是 classic 协议，先把客户端能力集复制到 `thd->protocol_binary` 再 `push_protocol` 切过去（顺序不能反，注释里明确写了 "set the current client capabilities before switching the protocol"）。
2. 把存储引擎优化目标置为 `Secondary_engine_optimization::PRIMARY_TENTATIVELY`，并记下原值准备恢复；随后建 PS 表项 `MYSQL_CREATE_PS(stmt, stmt->m_id, thd->m_statement_psi, stmt->name().str, stmt->name().length, nullptr, 0)`。
3. 调 `stmt->prepare(thd, query, length, nullptr)` 做真正的解析与准备。
4. 若 `prepare()` 返回错误，则 `MYSQL_DESTROY_PS(stmt->m_prepared_stmt)` 销掉 PS 表项，并 `thd->stmt_map.erase(stmt)` 把语句从语句表移除。

`Prepared_statement::prepare(THD *thd, const char *query_str, size_t query_length, Item_param **orig_param_array)` 是真正的阶段一实现。它的顺序是这套机制的骨架：

- `m_lex = new (m_arena.mem_root) st_lex_local`——LEX 分配在语句自己的 arena 上，不是 THD 的主内存根上。
- `set_db(thd->db())` 记住 prepare 时的当前库。源码注释给出理由：当前库会影响权限、`@@character_set_database` 等执行环境，所以执行时必须切回去。
- `Statement_backup stmt_backup; stmt_backup.set_thd_to_ps(thd, this); stmt_backup.save_rlb(thd);`——把 THD 的当前 LEX 与查询串备份下来，换成语句自己的；`save_rlb()` 另存改写后的查询串。
- `thd->swap_query_arena(m_arena, &arena_backup)` 把 THD 的查询 arena 换成语句的 arena，让后续分配落在语句的 MEM_ROOT 上。
- `Parser_state parser_state; parser_state.init(...)`，并设 `parser_state.m_lip.stmt_prepare_mode = true`、`parser_state.m_lip.multi_statements = false`——预处理语句禁止多语句。
- `lex_start(thd)` 后置 `m_lex->context_analysis_only |= CONTEXT_ANALYSIS_ONLY_PREPARE`。
- digest 的处理是条件性的：`if (max_digest_length > 0) token_array = (unsigned char *)thd->alloc(max_digest_length);`，`digest.reset(token_array, max_digest_length)`，且 `if (get_max_digest_length() != 0) parser_state.m_input.m_compute_digest = true;`——摘要长度被配成 0 时不算摘要。
- `parse_sql(thd, &parser_state, nullptr)` 解析；解析成功后调 `invoke_post_parse_rewrite_plugins(thd, true)`，再调 `init_param_array(thd, this)` 建立参数数组。
- `m_lex->m_sql_cmd->set_owner(this)` 把 SQL 命令对象绑定到本语句。
- `error = prepare_query(thd)` 做语句级准备。
- 收尾：`m_lex->cleanup(true)`、`m_lex->clear_values_map()`、`close_thread_tables(thd)`、`thd->mdl_context.rollback_to_savepoint(mdl_savepoint)`、`lex_end(m_lex)`、`rewrite_query(thd)`、`cleanup_stmt(thd)`、`stmt_backup.restore_thd(thd, this)`。
- 成功时 `m_arena.set_state(Query_arena::STMT_PREPARED)`、`m_in_use = false`，并 `setup_stmt_logging(thd)`。

**参数数量的上限来自 `init_param_array()`。** 它先取 `stmt->m_param_count = lex->param_list.elements`，然后 `if (stmt->m_param_count > static_cast<uint>(UINT_MAX16)) { my_error(ER_PS_MANY_PARAM, MYF(0)); return true; }`——上限是 `UINT_MAX16`，超出报 `ER_PS_MANY_PARAM`。参数数组用 `thd->mem_root->ArrayAlloc<Item_param *>(stmt->m_param_count)` 一次性分配，注释说明了动机：为了让 `mysql_stmt_get_longdata` 能按序号直接取到 `Item_param`，避免在解析器里做 malloc/realloc。

**哪些语句不允许 prepare，由 `prepare_query()` 的 switch 决定。** 三类被显式拒绝：

- `m_lex->keep_diagnostics == DA_KEEP_COUNTS` 或 `DA_KEEP_DIAGNOSTICS` 时，`my_error(ER_UNSUPPORTED_PS, MYF(0))`。
- `SQLCOM_CREATE_VIEW` 且 `m_lex->create_view_mode == enum_view_create_mode::VIEW_ALTER` 时，同样报 `ER_UNSUPPORTED_PS`。
- `SQLCOM_CREATE_TABLE` 且 `m_lex->create_info->m_transactional_ddl` 为真时，报 `ER_UNSUPPORTED_PS`——注释写明 "CREATE TABLE ... START TRANSACTION is not supported with prepared statements"。

`SQLCOM_PREPARE` / `SQLCOM_EXECUTE` / `SQLCOM_DEALLOCATE_PREPARE` 以及 switch 的 `default` 分支走「平凡检查」：只有当 `sql_command_flags[sql_command]` 带 `CF_STATUS_COMMAND` 且不带 `CF_DIAGNOSTIC_STMT` 时才放行，否则 `my_error(ER_UNSUPPORTED_PS, MYF(0))`。

**元数据回给客户端由 `send_statement()` 完成。** 它先 `thd->get_protocol()->store_ps_status(stmt->id(), no_columns, stmt->m_param_count, thd->get_stmt_da()->current_statement_cond_count())`，再在 `stmt->m_param_count != 0` 时把 `stmt->m_lex->param_list` 里的每个 `Item_param` 塞进 `mem_root_deque<Item *> param_list` 并调 `thd->send_result_metadata(param_list, Protocol::SEND_EOF)`，随后断言 `CountVisibleFields(param_list) == stmt->m_param_count`。结果集元数据则由 `result->send_result_set_metadata(thd, *types, Protocol::SEND_EOF)` 单独发。

### 阶段二：execute

`mysqld_stmt_execute(THD *thd, Prepared_statement *stmt, bool has_new_types, ulong execute_flags, PS_PARAM *parameters)` 的流程是：切协议 → `MYSQL_EXECUTE_PS(thd->m_statement_psi, stmt->m_prepared_stmt)` → 置存储引擎优化目标 → `stmt->set_parameters(thd, &expanded_query, has_new_types, parameters)` → `stmt->execute_loop(thd, &expanded_query, open_cursor)`，其中 `open_cursor` 由 `execute_flags & (ulong)CURSOR_TYPE_READ_ONLY` 决定。

**参数装配在 `insert_parameters()`。** 它遍历 `m_param_array` 的每个 `Item_param`：

- 若 `has_new_types` 为真，先 `set_parameter_type(param, parameters[i].type, parameters[i].unsigned_type, thd->variables.character_set_client)`。`set_parameter_type()` 内部按类型分派：BLOB 系（`MYSQL_TYPE_TINY_BLOB` / `MEDIUM_BLOB` / `LONG_BLOB` / `BLOB`）把 collation 源设为 `&my_charset_bin`；`VARCHAR` / `JSON` / `VAR_STRING` / `STRING` 用客户端字符集 `cs_source`；其他类型落到 `default` 分支被置为 `MYSQL_TYPE_INVALID, false`——这代表客户端给了非法元数据，`insert_parameters()` 随即 `my_error(ER_WRONG_ARGUMENTS, MYF(0), "mysqld_stmt_execute")`。
- 若 `param->param_state() == Item_param::LONG_DATA_VALUE`，说明该参数走的是长数据分片流。此时必须 `is_param_long_data_type(param)` 为真，否则同样报 `ER_WRONG_ARGUMENTS`；`is_param_long_data_type()` 的判据是 `param->data_type_source() >= MYSQL_TYPE_TINY_BLOB && param->data_type_source() <= MYSQL_TYPE_STRING`。通过后把实际类型置为 `MYSQL_TYPE_VARCHAR`，collation 按「源是 bin 就保持 bin，否则取参数自身的 collation，若也是 bin 则退回 `current_thd->variables.collation_connection`」三段选择。
- 否则若 `parameters[i].null_bit` 为真，`param->set_null()`。
- 否则 `set_parameter_value(param, &parameters[i].value, parameters[i].length)` 按二进制协议解码值。这个函数按 `param->data_type_source()` 逐类型解码：整型用 `sint2korr` / `sint4korr` / `sint8korr`，浮点用 `float4get` / `float8get`，十进制用 `param->set_decimal((const char *)*pos, len)`。**TIME 类型的解码带一个上限**：小时数由 `tm.hour = (uint)to[5] + day * 24` 算出，随后 `if (tm.hour > 838) { tm.hour = 838; tm.minute = 59; tm.second = 59; }`——小时上限 838 就是 MySQL TIME 的最大值，源码里还留着 `/* TODO: add warning 'Data truncated' here */`，即这里**静默截断不报 warning**。DATETIME 解码的断言写死了允许的长度：`assert(len == 0 || len == 4 || len == 7 || len == 11 || len == 13)`。
- 解码后若 `param->param_state() == Item_param::NO_VALUE`，报 `ER_WRONG_ARGUMENTS`——注释说明这是「客户端坏了、没给元数据」。
- 若 `param->is_type_pinned()`（类型被钉住），则只接受整数或字符串：字符串走 `param->val_int()` 转整数；既不是 `MYSQL_TYPE_VARCHAR` 也不是 `MYSQL_TYPE_LONGLONG` 就报 `ER_WRONG_ARGUMENTS`。还有符号一致性检查：钉住的参数无符号而实参有符号（或反之）且 `param->value.integer < 0` 时，`my_error(ER_DATA_OUT_OF_RANGE, MYF(0), "signed integer", "mysqld_stmt_execute")`。

**日志展开查询串的容量预留是显式常量。** `if (m_with_log && query->reserve(m_query_string.length + 32 * m_param_count))`——每个占位符预留 32 字节。展开后还要检查总长：`if (query->length() + num_bytes + val->length() > std::numeric_limits<uint32>::max())` 则报 `ER_WRONG_ARGUMENTS`。

`m_with_log` 由 `setup_stmt_logging(THD *thd)` 决定，条件是：二进制日志打开且是更新类语句且（行格式非行基或带 `CF_AUTO_COMMIT_TRANS`）、或 `opt_general_log`、或 `opt_slow_log`、或是 `SQLCOM_SELECT` 且 `m_lex->safe_to_cache_query` 且不是 EXPLAIN、或是 `is_global_audit_mask_set()`。任一成立则 `m_with_log = true`。函数里的 `@todo WL#6570` 注释指出：`is_global_audit_mask_set()` 因为 `caching_sha2` 默认加载而总为真，导致 `m_with_log` 实际总为真。

### 重整（reprepare）与元数据校验

**重整的触发源有三个，都在 `execute_loop()` 里。**

1. **参数类型不匹配。** `if (!check_parameter_types()) { assert(!reprepared_for_types); reprepared_for_types = true; if (reprepare(thd)) return true; goto reexecute; }`——源码注释写明 "Only one reprepare is required in case of parameter mismatch"，所以这里只有一次机会，用 `reprepared_for_types` 标志加断言守住。
2. **UDF 引用。** `if (m_lex->has_udf() && reprepare(thd)) return true;`——无条件重整。
3. **元数据版本变化（DDL 插在 prepare 与 execute 之间）。** 这条靠观察者实现：只有 `sql_command_flags[m_lex->sql_command] & CF_REEXECUTION_FRAGILE` 的命令才会 `reprepare_observer.reset_reprepare_observer()` 并 `thd->push_reprepare_observer(stmt_reprepare_observer)`。执行出错后，若 `reprepare_observer.is_invalidated()` 为真，则 `assert(thd->get_stmt_da()->mysql_errno() == ER_NEED_REPREPARE)`，再由 `reprepare_observer.can_retry()` 决定是否 `thd->clear_error()` 后 `reprepare(thd)`。`Reprepare_observer::report_error()` 把错误压进诊断区但不建 `Sql_condition`：`thd->get_stmt_da()->set_error_status(thd, ER_NEED_REPREPARE); m_invalidated = true; m_attempt++;`，注释说明这是纯内部错误，用来让执行一路回溯到 `Prepared_statement::execute_loop()`。不能重试时，`execute_loop()` 自己补压一条 `Sql_condition`（`da->push_warning(thd, da->mysql_errno(), da->returned_sqlstate(), Sql_condition::SL_ERROR, da->message_text())`），注释指明这对应 `Reprepare_observer::report_error()` 的说明。

**`check_parameter_types()` 是一张逐类型的兼容表。** 它先放过三类：`item->is_type_inherited()`、`item->is_type_pinned()`、`item->param_state() == Item_param::NULL_VALUE`——源码注释逐条解释了理由（继承类型相当于动态转换需要运行期检查；钉住的类型不强制重整但值要另查；NULL 对所有参数类型都合法）。然后按 `item->data_type()` 分派：

- 整型族（`BOOL` / `TINY` / `SHORT` / `INT24` / `LONG` / `LONGLONG` / `BIT` / `YEAR`）：要求 `item->data_type_actual() == MYSQL_TYPE_LONGLONG`，且除 `YEAR` 外实参与解析类型的有符号性必须一致，否则 `return false`（触发重整）。
- `MYSQL_TYPE_NEWDECIMAL`：接受 `LONGLONG` 或 `NEWDECIMAL`。注释说明依据是 `assert(item->decimal_precision() - item->decimals >= 20)`，即 DECIMAL 参数的精度足够容纳任何整数值。
- `MYSQL_TYPE_FLOAT` / `DOUBLE`：接受 `LONGLONG` / `NEWDECIMAL` / `DOUBLE`。
- `MYSQL_TYPE_DATE`：`LONGLONG` / `NEWDECIMAL` / `DOUBLE` / `DATE` 放过；实参是 `DATETIME` 或 `TIME` 则 `return false` 触发重整为 DATETIME。注释用 `date_expr = numeric literal` 与 `date_expr = 其他数值表达式` 的差别解释了为什么需要这个特例。
- `MYSQL_TYPE_TIME`：`LONGLONG` / `NEWDECIMAL` / `DOUBLE` / `TIME` / `DATETIME` 放过；实参是 `DATE` 则重整为 DATETIME。
- `MYSQL_TYPE_DATETIME`：`LONGLONG` / `NEWDECIMAL` / `DOUBLE` / `DATETIME` / `DATE` / `TIME` 全放过。
- 字符串族（`VARCHAR` / `JSON` / `TINY_BLOB` / `MEDIUM_BLOB` / `BLOB` / `LONG_BLOB`）：实参是 `LONGLONG` / `NEWDECIMAL` / `DOUBLE` / `DATETIME` / `TIMESTAMP` / `DATE` / `TIME` 任一都 `return false` 触发重整。

字符串实参配整型目标另有一条专门分支：`if (item->data_type_actual() == MYSQL_TYPE_VARCHAR)` 且 `item->result_type() == INT_RESULT` 时，先 `str2my_decimal` 试转 DECIMAL；若返回 `E_DEC_TRUNCATED` 或 `E_DEC_BAD_NUM` 就 `continue`（注释说执行时 `val_int()` 会自己报 warning/error，不归本函数管）；若 `E_DEC_OK` 再用 `my_decimal2int` 检查是否在整数范围内，`result != E_DEC_OK` 则 `item->set_decimal(&decimal_value); return false;` 触发重整为 DECIMAL。

**重整的落地是「换内存根 + 重新 prepare」。** `Prepared_statement::reprepare(THD *thd)` 造一个中间的 `Prepared_statement copy(thd)`，调 `swap_prepared_statement(&copy)` 把本对象 MEM_ROOT 分配的数据搬进 copy，并挂一个 `create_scope_guard` 在函数退出时再 swap 回来——这就是「重整失败就把原数据换回去」的兜底。随后 `thd->status_var.com_stmt_reprepare++`，重建名字（`set_name(copy.m_name)`），用 `mysql_opt_change_db(thd, copy.m_db, &saved_cur_db_name, true, &cur_db_changed)` 临时切库并在结束后用 `mysql_change_db` 恢复。关键的一行是重整期间压制元数据下发：`const unsigned saved_sql_prepare = is_sql_prepare(); set_sql_prepare();`，注释写明 "Suppress sending metadata to the client while repreparing. It was sent during the initial preparation."。重整后 `validate_metadata(thd, &copy)` 比对结果集列数。

`swap_prepared_statement()` 换的是一整套 MEM_ROOT 分配物：`m_mem_root`、arena（`m_arena.swap_query_arena`）、`m_lex`、`m_query_string`、`m_arena.mem_root`、`m_param_array`、`m_param_count`、`m_name`、`m_db`、`m_cursor_result`、`m_regular_result`、`m_aux_result`、`m_cursor`。其中 `m_name` 与 `m_db` 的交换有注释说明原因：「旧名字分配在错误的内存根上」，交换后要在原对象上重新 `set_name`。参数数组的搬运由 `swap_parameter_array()` 完成，逐个调 `(*dst)->set_param_type_and_swap_value(*src)` 并 `sync_clones()`；它的注释给了两个存在理由：二进制协议只在首次 execute 发送参数元数据，所以重整时要从旧参数数组取类型；长数据分片在最后一片组装后原始分片已丢失，只能从旧参数里取整值。

`validate_metadata()` 目前只比列数：`if (m_lex->query_block->num_visible_fields() != copy->m_lex->query_block->num_visible_fields())` 则置 `thd->server_status |= SERVER_STATUS_METADATA_CHANGED`。函数开头 `if (is_sql_prepare() || m_lex->is_explain()) return false;`——SQL 层 prepare 或 EXPLAIN 直接返回 false，注释说明此时原 SELECT 的元数据本来就没发给客户端。

### 次级存储引擎的重试链

`execute_loop()` 里还有一条独立的重试分支，处理「该走次级引擎还是主引擎」的判定。当错误码是 `ER_PREPARE_FOR_PRIMARY_ENGINE` 或 `ER_PREPARE_FOR_SECONDARY_ENGINE` 时（源码断言此时 `thd->secondary_engine_optimization() == Secondary_engine_optimization::PRIMARY_TENTATIVELY` 且 `!m_lex->unit->is_executed()`），清错误后重整：走次级引擎则置 `Secondary_engine_optimization::SECONDARY` 并 `MYSQL_SET_PS_SECONDARY_ENGINE(m_prepared_stmt, true)`，走主引擎则置 `PRIMARY_ONLY` 并置 false。为避免同一条查询在通用日志里写两遍，这里先 `general_log_temporarily_disabled |= disable_general_log(thd)` 临时关掉通用日志（`disable_general_log()` 通过置 `thd->variables.option_bits |= OPTION_LOG_OFF` 实现），执行完再 `thd->variables.option_bits &= ~OPTION_LOG_OFF` 恢复。

若这次是为次级引擎做的重整且失败了，则再降级一次：`thd->set_secondary_engine_optimization(Secondary_engine_optimization::PRIMARY_ONLY)`、`MYSQL_SET_PS_SECONDARY_ENGINE(m_prepared_stmt, false)` 后再次 `reprepare(thd)`；成功后 `m_lex->m_sql_cmd->disable_secondary_storage_engine()` 明确把次级引擎关掉，且断言 `!m_lex->m_sql_cmd->using_secondary_storage_engine()`。若语句用了外部表，失败原因由 `set_external_engine_fail_reason(m_lex, thd->get_stmt_da()->message_text())` 记下。

### 长数据、游标与复位

**长数据分片由 `mysql_stmt_get_longdata()` 接收。** 它 `param->set_longdata((char *)str, length)` 把分片挂到 `stmt->m_param_array[param_number]` 上，并且**不向客户端回任何包**——函数体内没有发送逻辑，只有 `query_logger.general_log_print(thd, thd->get_command(), NullS)`。错误被记账而非立刻返回：`thd->get_stmt_da()->is_error()` 为真时置 `stmt->m_arena.set_state(Query_arena::STMT_ERROR)`、`stmt->m_last_errno` 与 `stmt->m_last_error`。这个延迟到 execute 的设计写在 `execute_loop()` 开头：`if (m_arena.get_state() == Query_arena::STMT_ERROR) { my_message(m_last_errno, m_last_error, MYF(0)); return true; }`。

**长数据的越界检查在 `mysql_stmt_precheck()`。** `COM_STMT_SEND_LONG_DATA` 分支里：`if (com_data->com_stmt_send_long_data.param_number >= (*stmt)->m_param_count)` 则置 `Query_arena::STMT_ERROR`、`m_last_errno = ER_WRONG_ARGUMENTS` 并 `sprintf((*stmt)->m_last_error, ER_THD(thd, ER_WRONG_ARGUMENTS), "mysql_stmt_precheck")`，源码注释写明 "Error will be sent in execute call"。

**执行时参数个数必须对得上。** `mysql_stmt_precheck()` 的 `COM_STMT_EXECUTE` 分支：若客户端支持 `CLIENT_QUERY_ATTRIBUTES`，则只在 `(*stmt)->m_param_count > com_data->com_stmt_execute.parameter_count` 时报 `ER_WRONG_ARGUMENTS`（注释说明多出来的参数留给 component service 消费）；否则必须严格相等 `(*stmt)->m_param_count != com_data->com_stmt_execute.parameter_count` 就报 `ER_WRONG_ARGUMENTS`。找不到句柄时统一走 `not_found:` 标签，报 `ER_UNKNOWN_STMT_HANDLER`。

**游标由 `execute()` 打开、由 `mysqld_stmt_fetch()` 取行。** `execute()` 在 `open_cursor` 为真时检查 `sql_cmd == nullptr` 则报 `ER_WRONG_ARGUMENTS`（错误串是 `"with cursor"`），再检查 `sql_cmd->query_result()->check_supports_cursor()`——命中说明是 `SELECT ... INTO` 这类不支持游标的语句，直接返回。游标结果对象按协议创建：classic 协议下 `new (m_arena.mem_root) Query_fetch_protocol_binary(thd)`，否则 `new (m_arena.mem_root) Query_result_send()`；随后 `sql_cmd->set_lazy_result()` 标记结果需延迟到执行时准备。`mysqld_stmt_fetch()` 检查 `cursor == nullptr || !cursor->is_open()`，不满足则报 `ER_STMT_HAS_NO_OPEN_CURSOR`；取完行后 `if (!cursor->is_open()) reset_stmt_parameters(stmt)`。

**复位由 `mysqld_stmt_reset()` 完成。** 它 `stmt->close_cursor()` 关游标、`reset_stmt_parameters(stmt)` 清参数、`stmt->m_arena.set_state(Query_arena::STMT_PREPARED)` 把 arena 状态退回 prepare 后，最后 `my_ok(thd)`。

### 递归保护与错误分支

**递归保护有三处，都报 `ER_PS_NO_RECURSION`。** `Prepared_statement::execute()` 里 `if (m_in_use) { my_error(ER_PS_NO_RECURSION, MYF(0)); return true; } m_in_use = true;`——这是执行期的重入保护；`mysql_sql_stmt_prepare()` 里同名语句正在使用时报 `ER_PS_NO_RECURSION`；`mysql_sql_stmt_close()` 里语句正在使用时同样报 `ER_PS_NO_RECURSION`。

**密码过期检查在 `execute_loop()` 开头。** `if (unlikely(!thd->security_context()->account_is_locked() && thd->security_context()->password_expired() && m_lex->sql_command != SQLCOM_SET_PASSWORD && m_lex->sql_command != SQLCOM_ALTER_USER))` 则报 `ER_MUST_CHANGE_PASSWORD`——`SET PASSWORD` 与 `ALTER USER` 被排除在外，否则改密码语句自己就执行不了。

**`execute()` 用 scope guard 做收尾。** 错误路径下若开了游标要 `close_cursor()`；若执行期间切过库则 `mysql_change_db(thd, to_lex_cstring(saved_cur_db_name), true)` 强制切回（源码注释说必须强制切换，因为库的 `m_db` 可能是 NULL，此时 `mysql_change_db` 会报错）；`cleanup_stmt(thd)` 清理 arena 上的 Item；`m_lex->release_plugins()`；资源组切回 `mgr_ptr->restore_original_resource_group(thd, src_res_grp, dest_res_grp)`；最后 `m_arena.get_state() == Query_arena::STMT_PREPARED` 则推进为 `Query_arena::STMT_EXECUTED`。

## 关键取舍

**prepare 阶段开表但不加锁。** 文件头注释给出完整理由：prepare 期间打开表会阻塞 DDL，但不需要锁，因为此时既不读也不改数据。代价是 prepare 与 execute 之间存在 DDL 可插入的窗口——这个窗口不能靠锁堵死，只能靠 execute 时的元数据校验与重整来兜。这是 `execute_loop()` 需要 `CF_REEXECUTION_FRAGILE` 观察者机制的根本原因。

**参数类型不匹配只允许重整一次。** `check_parameter_types()` 返回 false 后的分支里 `assert(!reprepared_for_types)` 是硬断言。代价是「第一次实参类型与解析类型冲突、且重整后仍冲突」的情况不再有第二次机会，直接报错。好处是重整的代价（一次完整解析）不会因类型反复摇摆而无限放大。

**元数据用「只比列数」的弱校验。** `validate_metadata()` 只比 `num_visible_fields()`，列数变了就置 `SERVER_STATUS_METADATA_CHANGED` 通知客户端，但函数仍返回 false，即不因此拒绝执行。代价是列类型变了但列数没变的情况检测不出来。

**重整用「swap 内存根」而不是「原地重建」。** `swap_prepared_statement()` 把旧数据整体搬进临时 `copy`，失败时靠 scope guard 换回来。好处是失败路径上原语句完好可用；代价是每次重整都要付一次完整 prepare 的解析成本，外加一次参数数组的搬运。

**长数据的错误延迟到 execute 才报。** `mysql_stmt_get_longdata()` 不回包，错误记在 `m_last_errno` / `m_last_error` 上并把 arena 状态置为 `STMT_ERROR`，直到 `execute_loop()` 开头才 `my_message()` 抛出来。代价是客户端发完长数据看不到任何反馈；好处是长数据分片这条路径不引入额外的往返。

**长数据只允许字符串类类型。** `is_param_long_data_type()` 的判据是 `data_type_source()` 落在 `MYSQL_TYPE_TINY_BLOB` 到 `MYSQL_TYPE_STRING` 的区间内，不满足就报 `ER_WRONG_ARGUMENTS`。函数注释还点出一处协议细节：VARCHAR 在客户端—服务器协议里是 `MYSQL_TYPE_STRING` 而不是 `MYSQL_TYPE_VARCHAR`。

**TIME 解码上限静默截断。** `tm.hour > 838` 时被截到 838 时 59 分 59 秒，源码里留着 `/* TODO: add warning 'Data truncated' here */`。代价是超范围数据被悄悄改值而不报 warning。

**次级引擎重试要临时关通用日志。** 同一查询在通用日志里不能写两遍，所以重整前后用 `disable_general_log()` 关一次，执行完再恢复。代价是这段时间内该会话的其它日志事件也一并被压掉。

**架构推论：** `m_with_log` 的判定条件里 `is_global_audit_mask_set()` 一项会让 `m_with_log` 实际恒为真（源码 `@todo WL#6570` 注释指出原因是 `caching_sha2` 默认加载），因此 `insert_parameters()` 里「为日志展开查询串」这条路径在实践中几乎总是开启。这是从注释与条件的组合推出的，不是在源码里直接陈述的结论。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_prepare.cc` | `Prepared_statement` | 预处理语句句柄对象，持有 LEX、参数数组、arena 与结果集对象 |
| `sql/sql_prepare.cc` | `Prepared_statement::Prepared_statement` | 构造句柄：`m_id` 取自 `++thd_arg->statement_id_counter`，内存根用 `thd_arg->variables.query_alloc_block_size` |
| `sql/sql_prepare.cc` | `Prepared_statement::prepare` | 阶段一主体：换 arena、解析、`parse_sql`、`prepare_query`、关表与回滚 MDL savepoint |
| `sql/sql_prepare.cc` | `Prepared_statement::prepare_query` | 按 `sql_command` 分派语句级准备，并拒绝 `ER_UNSUPPORTED_PS` 的几类语句 |
| `sql/sql_prepare.cc` | `init_param_array` | 建参数数组，超过 `UINT_MAX16` 报 `ER_PS_MANY_PARAM` |
| `sql/sql_prepare.cc` | `send_statement` | 回发语句 ID、参数个数、参数元数据与结果集元数据 |
| `sql/sql_prepare.cc` | `mysqld_stmt_prepare` | `COM_STMT_PREPARE` 入口：切协议、建 PS 表项、调 `prepare()`、失败时从 `stmt_map` 移除 |
| `sql/sql_prepare.cc` | `mysql_stmt_precheck` | 按命令类型查句柄、校验参数个数与长数据参数序号 |
| `sql/sql_prepare.cc` | `Prepared_statement::insert_parameters` | 从协议包装配参数值，含类型判定、长数据判定与日志查询串展开 |
| `sql/sql_prepare.cc` | `set_parameter_type` | 按客户端元数据设置参数源类型与源 collation |
| `sql/sql_prepare.cc` | `set_parameter_value` | 按二进制协议逐类型解码参数值，含 TIME 的 838 小时上限 |
| `sql/sql_prepare.cc` | `Prepared_statement::check_parameter_types` | 解析类型与实参类型的兼容表，决定是否触发重整 |
| `sql/sql_prepare.cc` | `Prepared_statement::execute_loop` | 执行主循环：类型检查、观察者安装、重整重试、次级引擎降级链 |
| `sql/sql_prepare.cc` | `Prepared_statement::execute` | 阶段二主体：换库、换 arena、开游标、`mysql_execute_command(thd, true)` |
| `sql/sql_prepare.cc` | `Prepared_statement::reprepare` | 重整：swap 内存根、临时切库、压制元数据下发、重解析后校验列数 |
| `sql/sql_prepare.cc` | `Prepared_statement::swap_prepared_statement` | 交换两组 MEM_ROOT 分配物（含 LEX、参数数组、结果集对象） |
| `sql/sql_prepare.cc` | `swap_parameter_array` | 重整时把旧参数的类型与值搬到新参数上 |
| `sql/sql_prepare.cc` | `Prepared_statement::validate_metadata` | 重整后比对结果集列数，变化时置 `SERVER_STATUS_METADATA_CHANGED` |
| `sql/sql_prepare.cc` | `Reprepare_observer::report_error` | 元数据失效时压入 `ER_NEED_REPREPARE` 并置 `m_invalidated` |
| `sql/sql_prepare.cc` | `ask_to_reprepare` | 请求重整的入口，取 THD 上的观察者并调 `report_error` |
| `sql/sql_prepare.cc` | `mysqld_stmt_execute` | `COM_STMT_EXECUTE` 入口：切协议、装配参数、调 `execute_loop` |
| `sql/sql_prepare.cc` | `mysql_stmt_get_longdata` | `COM_STMT_SEND_LONG_DATA` 接收分片，错误延迟记账到 execute |
| `sql/sql_prepare.cc` | `mysqld_stmt_fetch` | `COM_STMT_FETCH` 取游标行，游标关闭后清参数 |
| `sql/sql_prepare.cc` | `mysqld_stmt_reset` | `COM_STMT_RESET` 关游标、清参数、arena 退回 `STMT_PREPARED` |
| `sql/sql_prepare.cc` | `mysqld_stmt_close` | `COM_STMT_CLOSE` 销毁 PS 表项并从 `stmt_map` 摘除 |
| `sql/sql_prepare.cc` | `mysql_sql_stmt_prepare` / `mysql_sql_stmt_execute` / `mysql_sql_stmt_close` | SQL 层 `PREPARE` / `EXECUTE` / `DEALLOCATE PREPARE` 的实现 |
| `sql/sql_prepare.cc` | `Statement_backup` | 备份与恢复 THD 的 LEX、查询串、改写查询串 |
| `sql/sql_prepare.cc` | `setup_stmt_logging` | 依日志、审计与语句类型决定 `m_with_log` 是否展开查询串 |
| `sql/sql_prepare.cc` | `disable_general_log` | 重整次级引擎期间临时关通用日志 |

## 与主核 / AI 的关系

主核（`engine/db`）把「一次工作单元」建模为显式的 arena 上下文，而 MySQL 这里把同一件事拆成两半：`Prepared_statement` 自己持有 `m_arena` 与 `m_mem_root`，执行时用 `Statement_backup` 把 THD 整体换过去再换回来。

**架构推论：** 这套「句柄自持 arena + 执行时换挂 THD」的模型对主核有借鉴价值。主核的查询层若要支持「同一份查询文本反复执行、只换参数」，可以照这个形状做：把解析产物与参数槽位挂在语句对象自己的 arena 上，执行时整体换进执行上下文，避免每次执行都重新解析也避免污染会话级状态。

**架构推论：** `check_parameter_types()` 那张逐类型兼容表是 MySQL 特有但可移植的经验——它把「解析出的参数类型」与「本次实参类型」的关系显式建模为一个可判定的兼容性谓词，而不是在求值时才隐式转换。主核的多模态参数（向量、JSON、标量混合）同样会遇到这个问题，这张表的形状（按解析目标类型分派、每类列出可接受的实参类型集合、超出则返回 false）可以直接套用。

对 AI 场景而言，两阶段协议给了代理一个明确的切面：一次工具调用可以先 prepare 拿到元数据（确认参数个数与语义），再多次 execute 换不同的参数值。`insert_parameters()` 里那套类型推导与 `check_parameter_types()` 的兼容表，加上 `execute_loop()` 的重整重试链，共同保证「同一段 SQL 文本、不同的实参组合」不会因为类型不匹配而给出静默错误的结果。

## 参考

- [MySQL 源码：`sql/sql_prepare.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_prepare.cc)
- [MySQL 源码：`sql/sql_prepare.h`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_prepare.h)
- [MySQL 源码：`sql/sql_class.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_class.cc)
- [MySQL Server 开发文档：`class Prepared_statement`](https://dev.mysql.com/doc/dev/mysql-server/latest/classPrepared__statement.html)
- [MySQL 参考手册：SQL Prepared Statements](https://dev.mysql.com/doc/refman/8.0/en/sql-prepared-statements.html)
- [MySQL 参考手册：C API Prepared Statements](https://dev.mysql.com/doc/refman/8.0/en/capi-prepared-statements.html)
