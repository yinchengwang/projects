---
project: mysql-server
feature: query-execution
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_select.cc
    symbols: [Sql_cmd_dml::execute, Sql_cmd_dml::execute_inner]
  - path: sql/sql_union.cc
    symbols: [Query_expression::execute]
---

# 查询执行入口

## 一句话职责

`sql/sql_select.cc` 的 `Sql_cmd_dml::execute()` 负责把一条 DML 语句从「已解析」推到「已执行」：准备阶段补齐表与权限、加锁、优化、建迭代器，然后由 `sql/sql_union.cc` 的 `Query_expression::execute()` 驱动根迭代器逐行取数并把结果集发给客户端；这一层只管**执行骨架与阶段调度**，不决定用哪个索引、也不实现单个扫描算子内部算法。

## 机制是什么

**这一层要解决的是「一条已经优化完的语句，谁负责在什么时刻把它跑起来」。** MySQL 8.0 把这件事拆成两段入口加一个执行体：

- `Sql_cmd_dml::execute()` 是**语句级入口**：负责准备与执行之间的全部前置工作（错误处理器、语句计时器、表打开与加锁、权限检查、副引擎（secondary engine）裁决），并把真正的执行委派出去。
- `Sql_cmd_dml::execute_inner()` 是**语句级执行体**：调用 `unit->optimize()` 产出计划与迭代器，估算语句代价，然后决定是走 `EXPLAIN` 分支还是真正执行。
- `Query_expression::execute()` 是**查询表达式级入口**：它只做一次「能不能复用上次结果」的短路判断，然后交给 `ExecuteIteratorQuery()` 真正拉数据。

**上下文的建立是这一层的核心产物。** 「执行上下文」在 MySQL 里不是单一对象，而是一组被逐步置位的状态：

- `THD::stage_*`（`THD_STAGE_INFO(thd, stage_init)` / `stage_executing` / `stage_end`）标记语句当前处于哪个阶段，`performance_schema` 的 `stage/sql/*` 计时器据此分桶。
- `Query_expression` 的状态机由 `is_prepared()` / `is_optimized()` / `is_executed()` 三个谓词串起，`execute_inner()` 里 `unit->optimize()` 之前断言 `is_prepared() && !is_optimized()`，`Query_expression::execute()` 里断言 `is_optimized()`。
- 错误处理器是**栈式**的：`thd->push_internal_handler(&ignore_handler)` 压入、结束时 `thd->pop_internal_handler()` 弹出。

**驱动迭代的部分在 `ExecuteIteratorQuery()` 里。** 它建立的结果集输出通道是 `Query_result`，执行的核心是一个固定的取行循环：

```
m_root_iterator->Init()
for (;;) {
  int error = m_root_iterator->Read();
  if (error > 0 || thd->is_error()) return true;   // 致命错误
  else if (error < 0) break;                        // 记录结束
  else if (thd->killed) { thd->send_kill_message(); return true; }
  ++*send_records_ptr;
  query_result->send_data(thd, *fields);
}
```

`RowIterator::Read()` 的返回值语义由 `sql/iterators/row_iterator.h` 的 `RowIterator::Read()` 注释给出：`0` 表示正常取到一行，`-1` 表示记录结束，`1` 表示错误。行数据本身不随返回值返回，而是写进表的记录缓冲区。

**「执行上下文」里还包含计划与代价。** `accumulate_statement_cost(lex)` 在优化后累加每个查询块的 `join->best_read`，写进 `thd->m_current_query_cost`；这个数字随后被 `retry_with_secondary_engine()` 拿去与 `thd->variables.secondary_engine_cost_threshold` 比较。

## 怎么实现的

### 入口链：从命令分发到取行

`sql/sql_parse.cc` 的 `mysql_execute_command()` 按 `lex->m_sql_cmd` 的类型分发，在 `SQLCOM_REPLACE` / `SQLCOM_INSERT` / `SQLCOM_UPDATE` / `SQLCOM_DELETE` / `SQLCOM_CREATE_TABLE` 等分支里统一调用 `lex->m_sql_cmd->execute(thd)`；`SQLCOM_SELECT` 分支同样调用 `lex->m_sql_cmd->execute(thd)`。因此 `Sql_cmd_dml::execute()` 是 DML 与 SELECT 共用的语句级入口。

`Sql_cmd_dml::execute()` 的完整顺序（按源码语句顺序）：

1. `lex = thd->lex;` 取出 `Query_expression *const unit = lex->unit;`。
2. **语句计时器**：`if (is_timer_applicable_to_statement(thd)) statement_timer_armed = set_statement_timer(thd);`。`is_timer_applicable_to_statement()` 的判定条件是 `thd->lex->sql_command == SQLCOM_SELECT && have_statement_timeout == SHOW_OPTION_YES && !thd->slave_thread && !thd->timer && (thd->lex->max_execution_time || thd->variables.max_execution_time) && !thd->sp_runtime_ctx`——即只有 SELECT、服务器实现了超时能力、非复制线程、当前无定时器、语句级或会话级 `max_execution_time` 非零、且不来自存储程序时才武装定时器。
3. **错误处理器**：仅当 `is_data_change_stmt()` 时。`lex->is_ignore()` 为真时压入 `Ignore_error ignore_handler`（并把 `UPDATE IGNORE` 标记为 `LEX::BINLOG_STMT_UNSAFE_UPDATE_IGNORE`，因为注释明确写 "UPDATE IGNORE can be unsafe"，故改用基于行的日志）；否则若 `thd->is_strict_mode()` 为真则压入 `Strict_error_handler`。
4. **准备**：`if (!is_prepared()) { if (prepare(thd)) goto err; }`；已准备的语句走另一条路——`cleanup(thd)` 后 `open_tables_for_query(thd, lex->query_tables, 0)`，再 `restore_cmd_properties(thd)`、`check_privileges(thd)`，最后在 `m_lazy_result` 为真时用 `Prepared_stmt_arena_holder` 做 `result->prepare(...)`。
5. **副引擎裁决**：`thd->variables.use_secondary_engine == SECONDARY_ENGINE_OFF` 且 `lex->has_external_tables()` 时报 `ER_SECONDARY_ENGINE_PLUGIN` 并置 `external_table_not_offloaded = true` 后 `goto err`；若 `thd->secondary_engine_optimization() == Secondary_engine_optimization::PRIMARY_ONLY`（且 `use_secondary_engine != SECONDARY_ENGINE_FORCED`）且有外部表，则调用 `external_engine_fail_reason(lex)` 取出外部引擎的错误原因、`set_external_engine_fail_reason(lex, nullptr)` 清空，再 `goto err`。随后 `validate_use_secondary_engine(lex)` 做最终校验；`validate_use_secondary_engine()` 内部的判定是：`use_secondary_engine == SECONDARY_ENGINE_FORCED` 时，SELECT 需 `lex->table_count >= 1`，`SQLCOM_INSERT_SELECT` / `SQLCOM_CREATE_TABLE` 需 `lex->table_count >= 2`。
6. `lex->set_exec_started();` 标记执行开始。
7. **DML 前置钩子**：`if (is_data_change_stmt() && run_before_dml_hook(thd)) goto err;`。
8. **加锁**：`if (!is_empty_query()) { if (lock_tables(thd, lex->query_tables, lex->table_count, 0)) goto err; }`。源码注释解释了加锁位置的取舍："Locking of tables is done after preparation but before optimization. This allows to do better partition pruning and avoid locking unused partitions."
9. `if (execute_inner(thd)) goto err;` 进入执行体。
10. **副引擎计数**：`if (using_secondary_storage_engine() && lex->unit->is_executed()) ++thd->status_var.secondary_engine_execution_count;`。
11. **收尾**：`thd->pop_internal_handler()` 弹出错误处理器、`THD_STAGE_INFO(thd, stage_end)`、`lex->cleanup(false)`（注释说明是部分清理，"preserve plans for EXPLAIN"）、`lex->clear_values_map()`、`lex->set_secondary_engine_execution_context(nullptr)`、`result->cleanup()`、`thd->save_current_query_costs()`、`thd->update_previous_found_rows()`、`reset_statement_timer(thd)`。

**错误分支 `err:` 的处理与成功分支不同**：它断言 `thd->is_error() || thd->killed`，同样做 `lex->cleanup(false)` 与 `lex->clear_values_map()`，但额外用 `get_secondary_engine_fail_reason(lex)` 读取失败原因并在为空时用 `thd->get_stmt_da()->message_text()` 填充（若 `lex->has_external_tables()` 则同时写入外部引擎失败原因），然后 `result->abort_result_set(thd)` 与 `result->cleanup()`，最后 `return thd->is_error();`。

### 执行体：`Sql_cmd_dml::execute_inner()`

```
if (unit->optimize(thd, /*materialize_destination=*/nullptr,
                   /*create_iterators=*/true, /*finalize_access_paths=*/true))
  return true;
accumulate_statement_cost(lex);
if (optimize_secondary_engine(thd)) return true;
lex->set_exec_completed();
if (lex->is_explain()) {
  if (explain_query(thd, thd, unit)) return true;
} else {
  if (unit->execute(thd)) return true;
}
```

四处要点：

- **优化与建迭代器合在一次调用里。** `unit->optimize()` 的第三个实参 `create_iterators=true`、第四个 `finalize_access_paths=true`。`Query_expression::optimize()` 内部在 `create_iterators && IteratorsAreNeeded(thd, m_root_access_path)` 成立时调用 `CreateIteratorFromAccessPath(thd, m_root_access_path, join, /*eligible_for_batch_mode=*/true)` 把访问路径变成根迭代器 `m_root_iterator`。
- **代价估算在执行之前。** `accumulate_statement_cost(lex)` 遍历 `lex->all_query_blocks_list`，累加 `query_block->join->best_read`；若该查询块所属的 `Item_subselect` 非空且 `!query_block->is_cacheable()`，则把该块代价乘以 `calculate_subquery_executions(item, trace)` 估算执行次数，最后写入 `lex->thd->m_current_query_cost`。
- **副引擎重试是一个独立的失败分支。** `optimize_secondary_engine(thd)` 内部先调 `retry_with_secondary_engine(thd)`；后者要求 `thd->secondary_engine_optimization() == Secondary_engine_optimization::PRIMARY_TENTATIVELY`，且语句 `eligible_secondary_storage_engine()` 非空、未被 `secondary_storage_engine_disabled()` 禁用、`thd->is_secondary_storage_engine_eligible()` 为真，并且（在无外部表时）`thd->m_current_query_cost > thd->variables.secondary_engine_cost_threshold`。满足条件时 `optimize_secondary_engine()` 会 `reset_diagnostics_area()` 后 `set_error_status(thd, ER_PREPARE_FOR_SECONDARY_ENGINE)` 并返回 true——即**不是回退执行，而是主动报一个「请为副引擎重新准备」的错误**，由上层重新走 prepare。
- **另一条重准备分支**：若 `secondary_engine_optimization()` 是 `PRIMARY_TENTATIVELY` 且 `m_sql_cmd->is_optional_transform_prepared()` 为真，说明上一次准备做了副引擎专用变换而这次主引擎没要求，于是 `set_optional_transform_prepared(false)` 后 `set_error_status(thd, ER_PREPARE_FOR_PRIMARY_ENGINE)` 返回 true。
- **最后一步是委派。** 非 EXPLAIN 时 `unit->execute(thd)`，也就是 `Query_expression::execute()`。

### 查询表达式级：`Query_expression::execute()`

```
assert(is_optimized());
if (is_executed() && !uncacheable) return false;
assert(!unfinished_materialization());
Change_current_query_block save_query_block(thd);
return ExecuteIteratorQuery(thd);
```

- **短路条件**：已执行过且 `!uncacheable` 时直接返回 false，不重复取数。`uncacheable` 非零的表达式（例如不可缓存的子查询）每轮都要重算。
- **`Change_current_query_block` 是作用域守卫**：源码注释解释它的用途是 "Even if we return \"true\" the statement might continue (e.g. ER_SUBQUERY_1_ROW in stmt with IGNORE), so we want to restore current_query_block()"。即出错返回后当前查询块状态也要复原。

### 取行主体：`Query_expression::ExecuteIteratorQuery()`

这是真正「执行」的地方，顺序为：

1. `THD_STAGE_INFO(thd, stage_executing);` + `DEBUG_SYNC(thd, "before_join_exec");`。
2. **optimizer trace**：`Opt_trace_object trace_exec(trace, "join_execution");`，简单查询额外 `trace_exec.add_select_number(first_query_block()->select_number);`。
3. `if (ClearForExecution()) return true;`。`ClearForExecution()` 做三件事：若 `is_executed()` 则先 `clear_correlated_query_blocks()`，再对 `query_terms<QTC_POST_ORDER>()` 中类型为 `QT_QUERY_BLOCK` 或已物化的 `Query_term_set_op` 调 `sl->join->reset()`；随后对每个查询块把 `join->examined_rows = 0`、`join->set_executed()`（注释说动态范围优化器依赖这个状态），并在 `join->m_windows.elements > 0 && !join->m_windowing_steps` 时对每个窗口调 `w.reset_all_wf_state()`（因为 `end_write_wf()` 会被短路）。
4. **结果集通道**：`Query_result *query_result = this->query_result();` 后 `query_result->start_execution(thd)`，再 `query_result->send_result_set_metadata(thd, *fields, Protocol::SEND_NUM_ROWS | Protocol::SEND_EOF)`。两个标志位 `SEND_NUM_ROWS` 与 `SEND_EOF` 一起传给协议层。
5. `set_executed();` 标记查询表达式已执行。
6. **副引擎接管分支**：若 `first_query_block()->join->override_executor_func != nullptr`，则遍历所有查询块调用 `select->join->override_executor_func(select->join, query_result)`，累加 `select->join->send_records` 到 `thd->current_found_rows`；未设置 `OPTION_FOUND_ROWS` 时按源码注释（"for backwards compatibility reasons only; we have documented that without SQL_CALC_FOUND_ROWS, we return the actual number of rows returned"）执行 `thd->current_found_rows = std::min(thd->current_found_rows, select_limit_cnt);`，然后 `query_result->send_eof(thd)` 返回。
7. **子查询项复位**：若 `item` 非空，`item->reset_value_registration()`；若 `item->assigned()` 为真，则 `item->assigned(false)`（注释：Prepare for re-execution of this unit）并 `item->reset()`。
8. **计数落点选择**：`send_records_ptr` 三选一——简单查询取 `&first_query_block()->join->send_records`；非简单且 `set_operation()->m_is_materialized` 取 `&query_term()->query_block()->join->send_records`；否则（UNION 但没有 fake_query_block）取 `&send_records`。随后 `*send_records_ptr = 0;`。取这个三选一是因为要支持 `SQL_CALC_FOUND_ROWS`：`LimitOffsetIterator` 会把被 OFFSET/LIMIT 跳过的行数写进 JOIN 的 `send_records`。
9. **`join_cleanup` 作用域守卫**：`create_scope_guard` 在退出时对每个查询块调 `join->join_free()` 并 `thd->inc_examined_row_count(join->examined_rows)`；非简单且已物化时再额外累加 `query_term()->query_block()->join->examined_rows`。源码注释强调 "join_cleanup must be done before we send EOF, so that we get the row counts right."
10. **取行循环**：`m_root_iterator->Init()` 成功后进入 `for (;;)` 循环调用 `m_root_iterator->Read()`，按 `0` / `-1` / `1` 三态分别处理「继续」「结束」「致命错误」；`thd->killed` 为真时 `thd->send_kill_message()` 后返回 true。每取到一行先 `++*send_records_ptr;` 再 `query_result->send_data(thd, *fields)`，成功后 `thd->get_stmt_da()->inc_current_row_for_condition();`。
11. **Performance Schema 批量模式**：`PFSBatchMode pfs_batch_mode(m_root_iterator.get());` 覆盖整个取行循环。
12. **收尾**：`thd->current_found_rows = *send_records_ptr;` 然后 `query_result->send_eof(thd)`。

### 计划生成侧：`Query_expression::optimize()`

虽然优化属于 C 组，但执行入口必须调用它，所以这里只列与执行入口契约相关的部分：

- 函数开头断言 `is_prepared() && !is_optimized()`，并 `assert(!finalize_access_paths)` 时 `assert(!create_iterators)`——即「建迭代器」蕴含「访问路径已定稿」。
- 逐个查询块 `set_limit(thd, query_block)`（注释 "LIMIT is required for optimization"），然后 `query_block->optimize(thd, finalize_access_paths)`。`Query_block::optimize()` 在 `sql/sql_select.cc` 里新建 `JOIN` 对象并在 `thd->lock_query_plan()` / `thd->unlock_query_plan()` 保护下赋给 `Query_block::join`（注释说明这是为了避免 `EXPLAIN FOR CONNECTION` 竞争），随后 `join->optimize(finalize_access_paths)`。
- **行数估算**：`contributes_to_rowcount_estimate(query_block)` 为真时累加 `query_block->join->best_rowcount`，但隐式分组（`is_implicitly_grouped()`）或 `group_optimized_away` 时按 `1` 计。
- **物化路径的三选一**：若使用副引擎则 `create_access_paths(thd)`（注释 "Not supported when using secondary storage engine."）；否则若 `estimated_rowcount <= 1` 或 `use_iterator(materialize_destination, query_term())` 则 `create_access_paths(thd)`（注释说明 const 表不能这么做，因为 `optimize_derived()` 要在优化期就跑查询、需要迭代器）；否则若 `materialize_destination != nullptr && can_materialize_directly_into_result()` 则走 `set_operation()->setup_materialize_set_op(thd, materialize_destination, /*union_distinct_only=*/false, calc_found_rows)` 直接物化进调用方给的表；否则 `create_access_paths(thd)`。
- **uncacheable 的行数保护**：`if ((uncacheable & UNCACHEABLE_DEPENDENT) && estimated_rowcount <= 1)` 时把 `estimated_rowcount` 改写为 `PLACEHOLDER_TABLE_ROW_ESTIMATE`，注释解释这是防止被换成常量。
- **`max_join_size` 熔断**：在最外层（`outer_query_block() == nullptr`）且 `!Overlaps(thd->variables.option_bits, OPTION_BIG_SELECTS)` 且 `!thd->lex->is_explain()` 时，用 `EstimateRowAccesses(m_root_access_path, /*num_evaluations=*/1.0, std::numeric_limits<double>::infinity())` 与 `thd->variables.max_join_size` 比较，超过则 `my_error(ER_TOO_BIG_SELECT, MYF(0))`。
- 结尾 `set_optimized();`。

## 关键取舍

**取舍一：把「加锁」放在优化之前，而不是执行之前。** `Sql_cmd_dml::execute()` 里 `lock_tables()` 在 `execute_inner()` 之前，源码注释给了理由："Locking of tables is done after preparation but before optimization. This allows to do better partition pruning and avoid locking unused partitions." 代价是加锁阶段拿不到数据统计信息（注释说 "prepare stage can rely only on metadata about tables used and not data from them"），换来的是不锁用不到的分区。

**取舍二：错误处理器用栈而不是全局状态。** `Ignore_error_handler` 与 `Strict_error_handler` 都通过 `thd->push_internal_handler()` 压入、`thd->pop_internal_handler()` 弹出，成对出现在成功分支与 `err:` 分支。代价是任何中途 `return true` 的路径都必须保证弹栈——`execute()` 里把弹栈放在 `err:` 标签之后统一处理。

**取舍三：EXPLAIN 与真正执行共用同一条优化路径，但分叉在最后一米。** `execute_inner()` 里 `unit->optimize()` 对两者都执行且都建迭代器，差异只在 `lex->is_explain()` 这个判断上：`EXPLAIN` 走 `explain_query(thd, thd, unit)`，普通语句走 `unit->execute(thd)`。对应地，收尾时 `lex->cleanup(false)` 只做部分清理，注释写明是 "preserve plans for EXPLAIN"。

**取舍四：副引擎不是「回退执行」，而是「报错要求重新准备」。** 这一点容易被误读。`optimize_secondary_engine()` 命中条件时并不降级到本地执行，而是 `reset_diagnostics_area()` + `set_error_status(thd, ER_PREPARE_FOR_SECONDARY_ENGINE)` 返回 true，让上层带着副引擎上下文重新 prepare。另一方向 `ER_PREPARE_FOR_PRIMARY_ENGINE` 同理：上一次为副引擎准备过，这次要回到主引擎，于是清掉 `set_optional_transform_prepared(false)` 再报错要求重新准备。真正「判定不需要副引擎」的函数是 `retry_with_secondary_engine()`，它返回 false 时才继续走本地执行。

**取舍五：语句计时器只武装给 SELECT。** `is_timer_applicable_to_statement()` 的第一个条件就是 `thd->lex->sql_command == SQLCOM_SELECT`，并且要求 `have_statement_timeout == SHOW_OPTION_YES`（服务器编译并初始化了超时支持）、非复制线程、当前无定时器、语句级或会话级 `max_execution_time` 非零、不来自存储程序（`!thd->sp_runtime_ctx`）。复制线程不受语句级超时约束。

**取舍六：`send_records` 的落点按查询形态分三处。** 简单查询落在 JOIN 上、已物化的集合运算落在 `query_term()->query_block()->join->send_records` 上、UNION 但无 fake_query_block 时落在 `Query_expression::send_records` 上。源码注释解释了原因：`LimitOffsetIterator` 需要把被 OFFSET/LIMIT 跳过的行数写进 JOIN 的 `send_records`，以便 `SQL_CALC_FOUND_ROWS` 能拿到正确值。

**取舍七：错误路径也要保留代价信息。** 两条路径都有 `thd->save_current_query_costs()`，`err:` 分支的注释说 "There are situations where we want to know the cost of a query that has failed during execution, e.g because of a timeout."

**取舍八：执行用迭代器，代价用访问路径。** `Query_expression` 同时持有 `m_root_access_path`（计划）和 `m_root_iterator`（运行时对象）；`optimize()` 负责从前者造出后者，`ExecuteIteratorQuery()` 只用后者。`Query_expression::cleanup(true)` 里 `clear_root_access_path()` 释放访问路径，而 `cleanup(false)` 保留——注释说 "Do partial cleanup (preserve plans for EXPLAIN)"。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/sql_select.cc | `Sql_cmd_dml::execute` | 语句级执行入口：计时器、错误处理器、准备/开表/权限、加锁、副引擎裁决、委派 `execute_inner()`、成功与错误两条收尾路径 |
| sql/sql_select.cc | `Sql_cmd_dml::execute_inner` | 语句级执行体：调 `unit->optimize()` 建迭代器、`accumulate_statement_cost()` 估代价、`optimize_secondary_engine()` 裁决副引擎、分叉 EXPLAIN 与真实执行 |
| sql/sql_select.cc | `accumulate_statement_cost` | 累加各查询块 `join->best_read`，不可缓存子查询按 `calculate_subquery_executions()` 折算次数，写入 `thd->m_current_query_cost` |
| sql/sql_select.cc | `optimize_secondary_engine` | 副引擎裁决入口：命中 `retry_with_secondary_engine()` 时报 `ER_PREPARE_FOR_SECONDARY_ENGINE`；主引擎方向报 `ER_PREPARE_FOR_PRIMARY_ENGINE`；否则调 `handlerton::optimize_secondary_engine` |
| sql/sql_select.cc | `retry_with_secondary_engine` | 判定是否值得改走副引擎：`PRIMARY_TENTATIVELY`、语句与环境均可用、且 `m_current_query_cost > secondary_engine_cost_threshold`（有外部表时免此比较） |
| sql/sql_select.cc | `validate_use_secondary_engine` | 副引擎最终校验：`SECONDARY_ENGINE_FORCED` 下 SELECT 需 `table_count >= 1`，`SQLCOM_INSERT_SELECT` / `SQLCOM_CREATE_TABLE` 需 `table_count >= 2` |
| sql/sql_select.cc | `is_timer_applicable_to_statement` | 语句计时器的适用条件判定（SELECT、`have_statement_timeout == SHOW_OPTION_YES`、非复制线程、`max_execution_time` 非零、非存储程序） |
| sql/sql_select.cc | `Query_block::optimize` | 建 `JOIN` 对象并在 `THD::lock_query_plan()` 保护下挂到 `Query_block::join`，再调 `join->optimize(finalize_access_paths)` |
| sql/sql_union.cc | `Query_expression::execute` | 查询表达式级入口：断言已优化、已执行且可缓存时短路返回、复位当前查询块、委派 `ExecuteIteratorQuery()` |
| sql/sql_union.cc | `Query_expression::ExecuteIteratorQuery` | 取行主体：建 optimizer trace、`ClearForExecution()`、`start_execution()` + `send_result_set_metadata()`、副引擎接管分支、三选一计数落点、`m_root_iterator->Init()`/`Read()` 取行循环、`send_data()` 逐行下发、`send_eof()` 收尾 |
| sql/sql_union.cc | `Query_expression::optimize` | 计划生成与迭代器创建：`set_limit()` → `Query_block::optimize()` → 累加 `best_rowcount`/`best_read` → 物化路径三选一 → `CreateIteratorFromAccessPath()` → `max_join_size` 熔断 → `set_optimized()` |
| sql/sql_union.cc | `Query_expression::ClearForExecution` | 执行前复位：`clear_correlated_query_blocks()`、`join->reset()`、`examined_rows = 0`、`join->set_executed()`、`w.reset_all_wf_state()` |
| sql/sql_union.cc | `Query_expression::cleanup` | 清理：`UC_CLEAN` / `UC_PART_CLEAN` 两档；`full` 时额外 `clear_root_access_path()` |
| sql/iterators/row_iterator.h | `RowIterator` | 取行接口契约：`Init()` 必先于 `Read()`；`Read()` 返回 `0` 正常、`-1` 记录结束、`1` 错误，行数据写入表的记录缓冲区 |
| sql/sql_parse.cc | `mysql_execute_command` | 命令分发：`SQLCOM_SELECT` 与各 DML 分支统一调用 `lex->m_sql_cmd->execute(thd)` |

## 与主核 / AI 的关系

**与主核的关系：** 本页是 MySQL 自身执行内核的「主干入口」，与主数据库内核 `engine/db/` 的 `L3 query/ spi/` 层是同类角色的两个独立实现。主核把查询拆成解析 → 计划 → 执行 → 结果输出四段，执行段的接口是 `spi` 层的入口；MySQL 则拆成 prepare → optimize → execute 三段，执行段的接口是 `Sql_cmd_dml::execute()` 与 `Query_expression::execute()`。可对照的抽象点有三处：主核的 `L3 query/` 层与 MySQL 的 `sql/sql_select.cc` 都负责「一次执行的上下文建立」；主核的存储接口与 MySQL 的 `RowIterator` 都把「取一行」抽象成可组合的迭代器；主核的事务边界与 MySQL 的 `Query_expression::execute()` 中 `uncacheable` / `Change_current_query_block` 的作用域守卫都在解决「执行的可重入与状态复原」这同一类问题。

**对 AI 辅助写 SQL 的直接含义：** 这层暴露了两个只有真正读过函数体才能知道的判定条件，会直接影响 AI 生成语句的成败。第一个是 `max_join_size` 熔断：`Query_expression::optimize()` 用 `EstimateRowAccesses()` 估算的行访问数超过 `thd->variables.max_join_size` 就报 `ER_TOO_BIG_SELECT`，而这个检查被 `!thd->lex->is_explain()` 排除，即 `EXPLAIN` 不会触发熔断——AI 若只靠 `EXPLAIN` 验证语句，会漏掉线上真实的失败。第二个是 `is_timer_applicable_to_statement()` 的六个合取条件：即使会话设了 `max_execution_time`，若语句来自存储程序（`thd->sp_runtime_ctx` 非空）或来自复制线程（`thd->slave_thread` 为真），语句级超时也不会生效。AI 在生成带超时的批处理脚本时若把语句放进存储过程，`max_execution_time` 不会按预期兜底。

**对 AI 理解执行代价的直接影响：** `accumulate_statement_cost()` 的代价不是行数而是「读取代价之和」——各查询块 `join->best_read` 之和，不可缓存子查询还要乘以 `calculate_subquery_executions()` 的执行次数估计。也就是说 `EXPLAIN` 里 `rows` 列的乘积不等于 `thd->m_current_query_cost`，后者才是与 `secondary_engine_cost_threshold` 比较、被用来决定是否改走副引擎的那个数。

## 参考

- [MySQL 源码：`sql/sql_select.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_select.cc)
- [MySQL 源码：`sql/sql_union.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_union.cc)
- [MySQL 源码：`sql/iterators/row_iterator.h`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/iterators/row_iterator.h)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 参考手册：理解查询执行计划](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/execution-plan-information.html)
- [MySQL 参考手册：`EXPLAIN` 输出格式](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/explain-output.html)
- [MySQL 参考手册：服务器系统变量（含 `max_join_size`、`max_execution_time`、`secondary_engine_cost_threshold`）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/server-system-variables.html)
- [MySQL 参考手册：InnoDB 架构](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/innodb-architecture.html)
