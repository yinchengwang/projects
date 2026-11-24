---
project: mysql-server
feature: derived-table-materialization
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_derived.cc
    symbols: [setup_materialized_derived, materialize_derived]
---

# 派生表与 CTE 的物化

## 一句话职责

`sql/sql_derived.cc` 里的 `Table_ref::setup_materialized_derived()` 负责在**解析期**给派生表 / 视图 / CTE 准备好一张承载其行的临时表（建表、克隆共享表、标记只读），`Table_ref::materialize_derived()` 负责在**执行期**真正把派生表的行算出来灌进这张临时表；两者合起来决定了派生表与公用表表达式在计划期是「物化成临时表」还是「合并进外层查询」这条落地路径。

## 机制是什么

**这一层解决的是「派生表里的行从哪里来、外层怎么读到它」的问题。** 一条 `SELECT ... FROM (SELECT ...) AS d` 里，派生表 `d` 是一张虚拟表：它在解析期还不存在物理载体。MySQL 给它两条互斥的落地路径：

- **合并（MERGE）**：把派生表的查询块拆开，把它的基表直接挂进外层查询块的 `leaf_tables` 链上，外层像读普通基表一样读它。合并不产生中间结果集。
- **物化（TEMPORARY TABLE）**：把派生表的查询结果算出来写进一张内部临时表，外层把这张临时表当成基表读。

**选择发生在计划期，落在 `Table_ref` 的两个互斥标志位上**：合并走 `is_merged()`，物化走 `uses_materialization()`。`sql/sql_derived.cc` 顶部的注释把这一层划成两个阶段：

- `setup_materialized_derived()` 负责**准备阶段**：建出那张临时表，但里面还没有行。
- `materialize_derived()` 负责**填充阶段**：执行派生表的查询表达式，把行写进临时表。

**物化的代价是显式的中间结果集。** 派生表的行要完整算出来才能被外层读到，这既是额外的一次写 + 一次读，也是外层无法把谓词下推进派生表内部的原因（除非单独开启条件下推，见下文「关键取舍」）。

**CTE 在这条路径上多一层「共享物化」。** 一个非递归 CTE 被引用多次时，源码的做法是：**只有第一个引用真正物化，其余引用共享同一份数据**。`sql/sql_derived.cc` 的文件头注释写得很明确——第一个引用调 `setup_materialized_derived()` 里的 `create_tmp_table()`，其余引用调 `open_table_from_share()`；第一个引用调 `materialize_derived()` 执行子查询并完成全部写入，其余引用只负责建立读访问路径。共享靠 `TABLE_SHARE::ref_count >= 2` 通知存储引擎，使引擎知道这些 handler 是共享同一份数据的。

**递归 CTE 走的是另一套时序。** 同样按文件头注释：递归引用**不调** `setup_materialized_derived()`、不调 `create_derived()`、不调 `materialize_derived()`；非递归引用刚做完 `setup_materialized_derived()` 之后，它的递归引用立刻被换成该引用的克隆（用 `open_table_from_share()`）；由第一个非递归引用在 `materialize_derived()` 里发起 with-recursive 算法，写入与读取交错进行。

## 怎么实现的

### 计划期入口：`Query_block::resolve_placeholder_tables()`

`sql/sql_resolver.cc` 的 `Query_block::resolve_placeholder_tables()` 是这一层的调度点，它对当前查询块表列表里的每个派生表 / 视图 / 表函数按固定顺序处理：

1. 已经 `is_merged()` 或已经 `uses_materialization()` 的直接跳过（源码注释说明这是给 `WL#6570 transform_grouped_to_derived()` 用的，它会先调 `setup_tables()` 再调本函数）。
2. 调 `tl->resolve_derived(thd, apply_semijoin)` 把派生表解析出来。
3. 若不在视图上下文分析中，且 `tl->is_mergeable()`，调 `merge_derived(thd, tl)` 尝试合并；合并后 `tl->is_merged()` 为真就 `continue`。
4. 走到这里说明**没能合并**，于是走物化准备：表函数调 `setup_table_function(thd)`；否则在 `tl->table == nullptr` 时调 `tl->setup_materialized_derived(thd)`。
5. 最后 `materialized_derived_table_count++`。

注意第 4 步的条件是 `tl->table == nullptr`——派生表如果已经有 `table`，说明它已被别的路径准备过（例如前面被合并过又被撤销，或 CTE 复用了已有临时表），此时不重复建表。

### 合并的判定：`Query_block::merge_derived()`

`sql/sql_resolver.cc` 的 `Query_block::merge_derived()` 决定派生表能不能被合并。它的注释给出了**优先级顺序**：

> - ALGORITHM says MERGE or TEMPTABLE
> - hint specifies MERGE or NO_MERGE (=materialization)
> - optimizer_switch's derived_merge is ON and heuristic suggests merge

逐条对应到代码：

1. **外层查询的全局开关。** `if ((master_query_expression() == lex->unit && !lex->can_use_merged()) || lex->can_not_use_merged()) return false;` —— 外层处于视图上下文分析且不允许合并，或整个 LEX 标记为不可合并，就放弃合并。
2. **ALGORITHM 子句与结构可合并性。** `if (derived_table->algorithm == VIEW_ALGORITHM_TEMPTABLE || !derived_query_expression->is_mergeable()) return false;`
3. **算法未指定时才走启发式 + 提示。** `if (derived_table->algorithm == VIEW_ALGORITHM_UNDEFINED)` 时算 `merge_heuristic`，再交给 `hint_table_state(thd, derived_table, DERIVED_MERGE_HINT_ENUM, merge_heuristic ? OPTIMIZER_SWITCH_DERIVED_MERGE : 0)`。`hint_table_state()` 在 `sql/opt_hints.cc` 里实现：先看该表上有没有已解析的表级提示（`table_list->opt_hints_qb` / `opt_hints_table`），有就返回提示里的状态；没有就退回 `thd->optimizer_switch_flag(optimizer_switch)` 读 `optimizer_switch` 系统变量。也就是说**提示优先于 `optimizer_switch`**，而 `optimizer_switch` 的 `derived_merge` 位又只在启发式说「适合合并」时才被当作允许。
4. **STRAIGHT_JOIN 与半连接互斥。** `if ((active_options() & SELECT_STRAIGHT_JOIN) && (derived_query_block->has_sj_nests || derived_query_block->has_aj_nests)) return false;`
5. **表数量上限。** `if (leaf_table_count + derived_query_block->leaf_table_count - 1 > MAX_TABLES) return false;` —— 上限值是常量 `MAX_TABLES`，其定义在头文件里，不在本次检出的源码文件中。减 1 是因为派生表自身这一张表要被合并掉。

全部通过后 `derived_table->set_merged()`，并向 optimizer trace 写入 `merged: true`。

**结构可合并性 `Query_expression::is_mergeable()`** 定义在 `sql/sql_lex.cc`，它的注释列出了不能合并的情形，代码是一条短路合取：

```
if (is_set_operation()) return false;
return !select->is_grouped() && select->having_cond() == nullptr &&
       !select->is_distinct() && select->has_tables() &&
       !select->has_limit() && !select->has_windows();
```

即：集合运算（UNION 等）、带 GROUP BY、带 HAVING、带 DISTINCT、无表（`SELECT 1`）、带 LIMIT、带窗口函数 —— 任一命中即不可合并。

**启发式 `Query_expression::merge_heuristic()`** 也在 `sql/sql_lex.cc`，只有两条：

- `if (lex->set_var_list.elements != 0) return false;` —— 查询里有变量赋值时不合并，源码注释说明理由是「try to preserve the original structure of the query. This is less likely to cause changes in variable assignment order」。
- 遍历 `select->visible_fields()`，若某个 SELECT 列表项 `item->has_subquery() && !item->const_for_execution()` 则不合并。注释解释：这种派生表是被当作 left outer join 内表用的，每次读外层一行都要重新求值 SELECT 列表里的相关子查询，合并不划算。

**另一层可合并性 `Table_ref::is_mergeable()`** 在 `sql/table.cc`，比上面那条更严，多加一条：`algorithm == VIEW_ALGORITHM_TEMPTABLE` 直接不可合并；并且若 CTE 被引用 `references.size() >= 2` 次且 `derived->uncacheable & UNCACHEABLE_RAND`，也不可合并——注释写明理由是「If the table's content is non-deterministic and the query references it multiple times, merging it has the risk of creating different contents」。

### 物化的准备：`Table_ref::setup_materialized_derived()`

`sql/sql_derived.cc` 的 `Table_ref::setup_materialized_derived()` 函数体只有一行实质逻辑：

```
return setup_materialized_derived_tmp_table(thd) ||
       derived->check_materialized_derived_query_blocks(thd);
```

即**先建临时表，再检查各查询块的权限与只读属性**；`||` 的短路语义保证建表失败时不再往下走。

`Table_ref::setup_materialized_derived_tmp_table()` 是真正干活的地方，要点：

- **前置断言。** `assert(is_view_or_derived() && !is_merged() && table == nullptr);` —— 只有「是派生表/视图、没被合并、还没有临时表」才进这里。
- **标记物化与只读。** `set_uses_materialization();` 然后 `set_readonly();`，注释说明「From resolver POV, columns of this table are readonly」。
- **CTE 共享路径。** `if (m_common_table_expr && m_common_table_expr->tmp_tables.size() > 0)` 时，向 trace 写 `reusing_tmp_table: true`，然后 `table = m_common_table_expr->clone_tmp_table(thd, this);`。这就是「多次引用的 CTE 共享一份物化结果」的实现入口。
- **首次物化路径。** `table == nullptr` 时：建表选项 `create_options = derived->first_query_block()->active_options() | TMP_TABLE_ALL_COLUMNS`（常量 `TMP_TABLE_ALL_COLUMNS` 表示临时表保留所有列，不做投影裁剪）；若用户指定了派生表列名（`m_derived_column_names`），先用 `swap_column_names_of_unit_and_tmp_table()` 临时把 SELECT 列表项的名字换成用户指定的名字，建完表后再换回来。
- **UNION DISTINCT 的去重索引。** `bool is_distinct = derived->can_materialize_directly_into_result() && derived->has_top_level_distinct();`，作为参数传给 `derived_result->create_result_table(...)`。`Query_expression::can_materialize_directly_into_result()` 在 `sql/sql_union.cc`，实现是「必须是集合运算，且 `global_parameters()->order_list.elements == 0`」——有 ORDER BY 就不能直接物化进结果表，因为排序需要独立的中间表。
- **权限检查与列标记。** `Query_expression::check_materialized_derived_query_blocks()` 在 `sql/sql_derived.cc`，对派生表的每个查询块：`set_tables_readonly()`、`propagate_unique_test_exclusion()`（注释说明「Derived tables/view are materialized prior to UPDATE, thus we can skip them from table uniqueness check」）、`check_view_privileges(thd_arg, SELECT_ACL, SELECT_ACL)`，再用 `Mark_field mf(MARK_COLUMNS_READ)` 遍历 `sl->fields` 标记列已读。

### 临时表的克隆：`Common_table_expr::clone_tmp_table()`

`sql/sql_derived.cc` 的 `Common_table_expr::clone_tmp_table()` 负责为共享 CTE 造出第二个、第三个 `TABLE` 对象：

- 断言 `tl->common_table_expr() == this`。
- 从 `first->s->mem_root.Alloc(sizeof(TABLE))` 分配新的 `TABLE`，与首个引用共享同一个 `TABLE_SHARE`。
- 调 `open_table_from_share(thd, first->s, tl->alias, 0, EXTRA_RECORD | DELAYED_OPEN, 0, t, false, nullptr)` 打开。源码注释解释了 `db_stat` 传 0 是「to delay opening of table in SE」，以及 `DELAYED_OPEN` 的必要性：「Otherwise it would be shared with 'first' and thus a write to tmp table would modify the row just read by readers」。
- 断言 `t->s == first->s && t != first && t->file != first->file`。
- `t->s->increment_ref_count();` 与 `t->s->tmp_handler_count++;` —— 这正是文件头注释里说的「TABLE_SHARE::ref_count>=2」共享机制。
- 若首个表有哈希键（`first->hash_field`），克隆表也设 `t->hash_field = t->field[0]`。
- 结尾把新引用压入 `tmp_tables`，并 `t->set_not_started()`。

**递归 CTE 的递归引用替换**由 `Common_table_expr::substitute_recursive_reference()` 完成：它调 `clone_tmp_table(thd, tl)` 造克隆，然后把「哑查询块」消掉——`tl->derived_query_expression()->exclude_tree();` 与 `tl->set_derived_query_expression(nullptr);`，再 `tl->set_privileges(SELECT_ACL)`。

### 计划期优化与提前物化：`Table_ref::optimize_derived()`

`sql/sql_derived.cc` 的 `Table_ref::optimize_derived()` 由 `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 调用——该处遍历 `query_block->leaf_tables`，对每个 `tl->is_view_or_derived()` 的表调 `tl->optimize_derived(thd)`。

`optimize_derived()` 的步骤：

- 若 `!table->has_storage_handler()`，用 `Derived_refs_iterator` 遍历各引用，对每个还没存储引擎的表调 `setup_tmp_table_handler(thd, t, unit->first_query_block()->active_options() | TMP_TABLE_ALL_COLUMNS)`，然后 `t->set_not_started()`。
- 调 `unit->optimize(thd, table, /*create_iterators=*/false, /*finalize_access_paths=*/true)`。注意 `create_iterators=false`：派生表此刻不生成迭代器，因为还没有行可迭代。
- **const 派生表提前物化。** `if (!thd->lex->using_hypergraph_optimizer())` 时，若 `materializable_is_const()` 为真，就立刻 `create_materialized_table(thd)` + `materialize_derived(thd)`。源码注释说明为什么超图优化器不这么做：「The hypergraph optimizer doesn't care about const tables, though, so it prefers to do this at execution time (in fact, it will get confused and crash if it has already been materialized)」。

**`Table_ref::materializable_is_const()`** 定义在 `sql/table.cc`，判定条件是两个：

```
return unit->query_result()->estimated_rowcount <= 1 &&
       (unit->first_query_block()->active_options() &
        OPTION_NO_SUBQUERY_DURING_OPTIMIZATION) == 0;
```

即估算行数不超过 1，且没有设置「优化期不做子查询」这个选项。

### 创建临时表：`Table_ref::create_materialized_table()`

`sql/sql_derived.cc` 的 `Table_ref::create_materialized_table()` 只建表不填行：

- 若 `!table->is_created()`，用 `Derived_refs_iterator` 找出**第一个已经 created 的引用**，把 `table->in_use` 设为 `thd`，调 `open_tmp_table(table)`，然后 `break`。这正是共享 CTE 中「第一个引用建表、其余引用只开表」的落点。
- **两种情况不建表**：表已经 created；或者 `query_block->join != nullptr && (query_block->join->const_table_map & map())` —— 即这是常量表且全部为 NULL 值。源码注释说明例外是「Exception being a shared CTE」。
- 否则调 `instantiate_tmp_table(thd, table)` 真正创建。
- 最后 `table->file->ha_extra(HA_EXTRA_IGNORE_DUP_KEY);` —— 忽略重复键错误。

### 执行期填充：`Table_ref::materialize_derived()`

`sql/sql_derived.cc` 的 `Table_ref::materialize_derived()` 把派生表的行算出来写进临时表：

- 前置断言 `is_view_or_derived() && uses_materialization()` 与 `table && table->is_created() && !table->materialized`。
- **共享 CTE 短路。** 用 `Derived_refs_iterator` 遍历，若任一引用 `t->materialized` 已为真，就直接 `table->materialized = true; table->set_not_started(); return false;` —— 不重复计算。
- **递归 CTE 的主键约束。** `if (unit->is_recursive()) { assert(table->s->primary_key == MAX_KEY); }`。理由写在文件头注释里：with-recursive 算法要求表扫描按插入顺序返回行，MEMORY 与 TempTable 引擎满足，InnoDB 的扫描按主键顺序返回，只有当主键不是 MySQL 自增生成的 rowid 时插入顺序才与主键顺序不同，因此断言该表没有 MySQL 创建的主键。`MAX_KEY` 是「没有主键」的常量取值。
- **哈希索引初始化。** `if (table->hash_field) { table->file->ha_index_init(0, false); }`。
- **执行。** `unit->force_create_iterators(thd)` 后 `bool res = unit->execute(thd);` —— 注释写明「execute unit without cleaning up」。函数头注释还警告：由于 LIMIT 的求值时机问题，**这个函数不能在 prepare 阶段调用**。
- **收尾。** 若 `table->hash_field` 则 `table->file->ha_index_or_rnd_end();`；若 `res` 为假则 `derived_result->flush()`（注释说这里是「entirely fix both Table_ref and list of SELECT's as there were no derived tables」）；最后 `table->materialized = true;` 与 `table->set_not_started();`。

### 执行期入口：`MaterializeIterator::Init()`

真正在运行时驱动物化的是 `sql/iterators/composite_iterators.cc` 的 `MaterializeIterator::Init()`：

- 若 `!table()->materialized` 且 `table()->pos_in_table_list->is_view_or_derived()`，调 `table()->pos_in_table_list->create_materialized_table(thd())` 建表。源码 TODO 注释说明这里「has some logic to deal with more complicated cases like multiple reference to the same CTE」，并提出把它与排序用的 `instantiate_tmp_table()` 统一。
- **共享 CTE 物化结果复用。** `use_shared_cte_materialization` 的判定是：`!table()->materialized && m_cte != nullptr && !m_rematerialize` 且 `m_cte->tmp_tables` 里存在某个 `table_ref->table->materialized` 为真的引用。成立时只更新 invalidator 的 generation 并置 `table()->materialized = true`，不再重算。
- **lateral 依赖导致的重物化。** 若已物化但 invalidator 的 `generation()` 变了，则 `rematerialize = true`。注释说明这是为了应对 lateral 表在上次物化后有了新行。

### 其他调用 `setup_materialized_derived()` 的地方

- `sql/sql_resolver.cc` 的 `Query_block::resolve_placeholder_tables()`（上文已述）。
- `sql/sql_resolver.cc` 里标量子查询转派生表的路径（`transform_scalar_subqueries_to_join_with_derived`），在 `assert(tl->table == nullptr);` 之后调 `tl->setup_materialized_derived(thd)`；注释说明此处「We skip resolve_derived(), as the subquery has already been resolved before the conversion to derived table」。
- `sql/sql_show.cc` 的 `mysqld_list_fields()` —— 为了读出列清单，先 `resolve_derived(thd, false)` 再 `setup_materialized_derived(thd)`。
- `sql/auth/sql_authorization.cc` 里也有 `setup_materialized_derived(thd)` 的调用点。
- `sql/sql_union.cc` 的 `Query_expression::prepare()` 里，当 `sl == first_recursive` 时调 `derived_table->setup_materialized_derived_tmp_table(thd)`——注释说明「All next query blocks will read the temporary table, which we must thus create now」。这是递归 CTE 建表的时机点。

## 关键取舍

**合并 vs 物化：省一次中间结果集 vs 失去下推机会。** 合并后派生表的基表直接进入外层连接，谓词可以正常作用到基表上；物化则先把派生表整表算出来，外层的 WHERE 只能在临时表上过滤。因此 `is_mergeable()` 对带 GROUP BY / HAVING / DISTINCT / LIMIT / 窗口函数 / 集合运算的查询一律判为不可合并——这些语义本身就要求「先把结果算成一个整体」。

**启发式只否掉、不肯定。** `merge_heuristic()` 的两条否定条件都不涉及「应该合并」，只是排除明显不该合并的情形；真正拍板的是 `optimizer_switch` 的 `derived_merge` 位与表级提示。源码里的优先级注释写得很清楚：ALGORITHM 子句 > 提示 > `optimizer_switch` + 启发式。

**共享物化用 `TABLE_SHARE::ref_count` 通知引擎。** `clone_tmp_table()` 里 `increment_ref_count()` 与 `tmp_handler_count++` 不是记账用的，而是让 MEMORY / InnoDB 引擎知道这些 handler 共享同一份数据，从而在写入改变索引树结构时刷新所有读游标（文件头注释提到 InnoDB 的 `row_search_no_mvcc()`）。

**递归 CTE 强制无主键。** `materialize_derived()` 里 `assert(table->s->primary_key == MAX_KEY)` 不是形式检查：with-recursive 要求扫描按插入顺序返回行，而 InnoDB 的扫描按主键顺序返回，只有在「没有 MySQL 创建的主键」时插入顺序才与扫描顺序一致。

**const 派生表在优化期就物化，但超图优化器不做。** `optimize_derived()` 里 `if (!thd->lex->using_hypergraph_optimizer())` 这个判断是硬编码的例外——源码注释直言超图优化器在派生表已物化的情况下「will get confused and crash」。

**条件下推是独立的一条路。** `Table_ref::can_push_condition_to_derived()`（`sql/sql_derived.cc`）决定能否把外层条件下推进物化化的派生表，六条否定条件分别是：提示未允许下推（`hint_table_state(..., DERIVED_CONDITION_PUSHDOWN_HINT_ENUM, OPTIMIZER_SWITCH_DERIVED_CONDITION_PUSHDOWN)`）、派生查询有 LIMIT、派生表是外连接的内表（注释说会「lead to more NULL-complemented rows」）、CTE 被引用多次或本身递归（注释说共享物化下只有一张临时表）、查询里有用户变量赋值、以及派生表来自标量转派生表且依赖基数检查（`!unit->m_reject_multiple_rows`）。**架构推论：** 这条路径与物化本身正交——它不改「是否物化」，只在物化之后额外把一部分外层条件搬进派生表内部，从而缩小写入临时表的行数。

**共享 CTE 的两条落地路径有分歧。** `sql/sql_derived.cc` 的文件头注释描述的是「首个引用建表、其余引用 `open_table_from_share()`」；而 `MaterializeIterator::Init()` 的注释 TODO 承认当前 `create_materialized_table()` 里「has some logic to deal with more complicated cases like multiple reference to the same CTE」，并提议与排序用的 `instantiate_tmp_table()` 路径统一。**架构推论：** 这说明「共享物化」在计划期（`setup_materialized_derived_tmp_table` → `clone_tmp_table`）与执行期（`MaterializeIterator::Init`）是两套实现，计划期那套更完整。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/sql_derived.cc | `Table_ref::setup_materialized_derived` | 物化准备的总入口：先建临时表，再检查派生表各查询块的权限与只读属性 |
| sql/sql_derived.cc | `Table_ref::setup_materialized_derived_tmp_table` | 建出承载派生表行的临时表；CTE 共享时改走 `clone_tmp_table` |
| sql/sql_derived.cc | `Common_table_expr::clone_tmp_table` | 为多次引用的 CTE 造共享 `TABLE_SHARE` 的克隆表，并递增 `ref_count` 与 `tmp_handler_count` |
| sql/sql_derived.cc | `Common_table_expr::substitute_recursive_reference` | 把递归 CTE 的递归引用换成首个非递归引用的临时表克隆，并消掉哑查询块 |
| sql/sql_derived.cc | `Table_ref::resolve_derived` | 解析派生表：校验递归 CTE 的 UNION 结构要求，准备其查询表达式 |
| sql/sql_derived.cc | `Query_expression::check_materialized_derived_query_blocks` | 对物化派生表的每个查询块做只读标记、唯一性检查排除与 SELECT 权限校验 |
| sql/sql_derived.cc | `Table_ref::optimize_derived` | 派生表的计划期优化入口；const 派生表在此提前物化 |
| sql/sql_derived.cc | `Table_ref::create_materialized_table` | 只建表不填行；共享 CTE 中由第一个已创建的引用开表 |
| sql/sql_derived.cc | `Table_ref::materialize_derived` | 执行期填充临时表；递归 CTE 断言无主键；共享 CTE 已物化则短路 |
| sql/sql_derived.cc | `Table_ref::can_push_condition_to_derived` | 判断能否把外层条件下推进入物化化的派生表 |
| sql/sql_derived.cc | `Condition_pushdown::make_cond_for_derived` | 构造可下推的条件并下推到派生表 |
| sql/sql_resolver.cc | `Query_block::resolve_placeholder_tables` | 计划期总调度：解析 → 尝试合并 → 走物化准备 |
| sql/sql_resolver.cc | `Query_block::merge_derived` | 合并判定：ALGORITHM 子句 > 提示 > `optimizer_switch` 与启发式，并检查 `MAX_TABLES` 上限 |
| sql/sql_lex.cc | `Query_expression::is_mergeable` | 结构可合并性：集合运算 / GROUP BY / HAVING / DISTINCT / 无表 / LIMIT / 窗口函数均不可合并 |
| sql/sql_lex.cc | `Query_expression::merge_heuristic` | 合并启发式：查询含变量赋值或 SELECT 列表含非常量子查询时不建议合并 |
| sql/table.cc | `Table_ref::is_mergeable` | 追加约束：`ALGORITHM=TEMPTABLE` 或多次引用的非确定 CTE 不可合并 |
| sql/table.cc | `Table_ref::materializable_is_const` | 判定派生表是否为常量表：估算行数不超过 1 且未设 `OPTION_NO_SUBQUERY_DURING_OPTIMIZATION` |
| sql/sql_union.cc | `Query_expression::can_materialize_directly_into_result` | 集合运算且无 ORDER BY 时才允许直接物化进结果表 |
| sql/sql_union.cc | `Query_expression::prepare` | 递归 CTE 在 `first_recursive` 处建临时表，并替换递归引用 |
| sql/sql_optimizer.cc | `JOIN::optimize` | 遍历叶表，对每个派生表调 `optimize_derived` |
| sql/opt_hints.cc | `hint_table_state` | 表级提示优先于 `optimizer_switch`；派生表的合并与条件下推都经它判定 |
| sql/iterators/composite_iterators.cc | `MaterializeIterator::Init` | 执行期建表、复用共享 CTE 的物化结果、按 lateral 依赖决定重物化 |

## 与主核 / AI 的关系

**架构推论：** 本页描述的是 MySQL 优化器里「子查询结果如何落地」的一个横切关注点，主核自研数据库若实现派生表 / CTE，同样要回答「合并还是物化」这同一个问题，但 MySQL 的答案不可直接照搬：它的合并判定被一串与 `optimizer_switch`、表级提示、视图上下文分析绑定的开关切碎，这些开关的存在前提是 MySQL 需要在同一份 SQL 文本上同时支持多种语义（视图定义、用户提示、可更新视图）。

**架构推论：** MySQL 这套实现里，「计划期决定走哪条路」与「执行期真正填数据」是分开的两个函数（`setup_materialized_derived` 与 `materialize_derived`），中间隔着优化与迭代器构建。AI 辅助改写 SQL 时若只改写查询文本而不考虑这两处的分界，容易把一条本该合并的派生表推成物化形态，性能特征随之改变——`EXPLAIN` 输出里派生表呈现为 `MATERIALIZED` 还是并入外层，是判断这一点最直接的手段。

## 参考

- [MySQL 源码：`sql/sql_derived.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_derived.cc)
- [MySQL 源码：`sql/sql_resolver.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_resolver.cc)
- [MySQL 源码：`sql/sql_lex.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex.cc)
- [MySQL 源码：`sql/sql_union.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_union.cc)
- [MySQL 源码：`sql/iterators/composite_iterators.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/iterators/composite_iterators.cc)
- [MySQL 参考手册：派生表（Derived Tables）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/derived-tables.html)
- [MySQL 参考手册：公用表表达式（WITH）](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/with.html)
- [MySQL 参考手册：视图算法与 `ALGORITHM` 子句](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/view-algorithms.html)
- [MySQL 参考手册：`EXPLAIN` 输出格式](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/explain-output.html)
- [MySQL 参考手册：子查询优化](https://docs.oracle.com/cd/E17952_01/mysql-8.0-en/subquery-optimization.html)
