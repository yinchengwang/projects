---
project: mysql-server
feature: subquery-transform
page_type: feature
schema_version: 1
refs:
  - path: sql/item_subselect.cc
    symbols: [Item_subselect::exec, Item_in_subselect::transform]
---

# 子查询改写

## 一句话职责

子查询改写层负责在**计划期**决定一个子查询谓词用什么形态执行：把 `IN (SELECT ...)` 改写成半连接（semi-join / antijoin）、改写成相关 `EXISTS`、或者改写成「物化到临时表再查」的形式，并把 `ALL/ANY/SOME` 的单列形式改写成 `MIN/MAX` 聚合子查询、把不含表的标量子查询直接折叠成外层表达式；它同时判定不相关子查询能否提前求值（靠 `uncacheable` 位与 `UNCACHEABLE_DEPENDENT` 标记）。这一层只产出「形状」，不负责子查询在运行期怎么迭代行。

## 机制是什么

**这一层解决的是「同一个子查询有多种等价执行形态，选哪一个」。** MySQL 为一个 `IN` 子查询准备了三条互斥的落地路径：

1. **半连接（semi-join）/ 反连接（antijoin）** —— 把子查询的内表拉进外层连接计划，成为一个「半连接 nest」，内表只匹配、不输出重复行。这是 `NOT IN` 的自然形态。
2. **相关 `EXISTS`（IN-to-EXISTS）** —— 把 `oe IN (SELECT ie FROM ... WHERE w)` 改写成 `EXISTS (SELECT 1 FROM ... WHERE w AND oe = ie)`，把外层表达式「注入」到子查询的 `WHERE`/`HAVING` 里，使子查询变成相关子查询。这条路适合内表小、外层行数少的情形。
3. **子查询物化（subquery materialization）** —— 把子查询结果一次性灌进临时表，外层每行对这个临时表做一次索引查找。适合外层行数多、内表结果集小且不相关的情形。

**这三条路径的选择不是拍脑袋，而是有代价比较的。** `sql/sql_optimizer.cc` 的 `JOIN::compare_costs_of_subquery_strategies()` 逐项算出两条路径的代价并比大小：

```
cost_exists = subq_executions * saved_best_read
cost_mat    = cost_mat_table + subq_executions * sjm.lookup_cost.total_cost()
mat_chosen  = (cost_mat < cost_exists)
```

其中 `subq_executions` 由同文件的 `calculate_subquery_executions()` 逐层向外累乘「父查询的扇出」算出：`parent_fanout` 取自 `parent_tab->position()->rows_fetched`，若该子查询挂在某张表的条件上，还要再乘前一张表的 `prefix_rowcount`。

**「相关」这件事在 MySQL 里是用一个位标记表达的，而不是靠数据结构形状。** `Query_expression::uncacheable` 上的 `UNCACHEABLE_DEPENDENT` 位表示「这个子查询依赖外层值，不能提前求值」；`UNCACHEABLE_RAND` 表示含非确定性函数。`sql/item_subselect.cc` 的 `Item_in_subselect::single_value_transformer()` 里 `!unit->uncacheable` 这一条就是「不相关」的判据——它同时是 `> ALL/ANY` 能否改写成 `MIN/MAX` 的前提条件之一。

**IN-to-EXISTS 的核心手法是「谓词注入 + 打标记」。** `Item_in_subselect::single_value_in_to_exists_transformer()` 把比较条件用 `func->create(m_injected_left_expr, orig_item)` 造出来，塞进子查询的 `WHERE`（`select->set_where_cond(and_items(...))`）或 `HAVING`。每个新造出来的条件都调 `set_created_by_in2exists()` 打标记。这个标记是后面「撤销」的唯一依据——`remove_in2exists_conds()` 遍历合取树，凡是带这个标记的条件一律剔除，重新拼成条件树。

**NULL 语义是这一层最麻烦的部分。** SQL 的 `IN` 有三值逻辑（TRUE / FALSE / UNKNOWN），而 `EXISTS` 只有两值。改写必须保住这个差别，所以源码里有两套机制：

- **`Item_func_trig_cond` + `Item_is_not_null_test`** —— 当 `!abort_on_null && left_expr->is_nullable()` 时，把注入的条件包进一个触发条件（`get_cond_guard(0)`），并额外注入 `Item_is_not_null_test` 用于区分 NULL 与 FALSE。`abort_on_null` 本身由 `Item_in_subselect::fix_fields()` 依据 `value_transform` 决定：`BOOL_IS_TRUE` 或 `BOOL_NOT_TRUE` 时置真（此时 NULL 与 FALSE 可以合并处理）。
- **`Item_in_subselect::val_bool_naked()`** —— 改写后真正求值走的是它：`if (was_null && !value) null_value = true;`，即「无匹配且内层有 NULL」翻译成 UNKNOWN。取反与 `IS TRUE/FALSE` 的处理留给外层的 `Item_in_optimizer`（源码注释明确「Negation, or applying of IS TRUE/FALSE, is left to the parent Item_in_optimizer」）。

**`Item_in_subselect` 本身在求值时已经被「替换」掉了。** `Item_in_subselect::val_int()` / `val_real()` / `val_str()` / `val_bool()` 全部是 `assert(false)` 加 `my_error(ER_INTERNAL_ERROR, MYF(0), "Invalid function call")`——注释写得很清楚：「Substituted with Item_in_optimizer, so this function is never used」。真正被替换进去的是 `Item_in_optimizer`，它在 `select_in_like_transformer()` 里被创建（`optimizer = new Item_in_optimizer(this)`），并持有原 `Item_in_subselect` 的指针。

**`Item_in_subselect::transform()` 做的只是「左表达式先递归改写，再把改写权交出去」。**

```
Item *Item_in_subselect::transform(Item_transformer transformer, uchar *arg) {
  left_expr = left_expr->transform(transformer, arg);
  if (left_expr == nullptr) return nullptr;
  return (this->*transformer)(arg);
}
```

它把 `left_expr`（IN 的左操作数）先走一遍 transformer，若结果为 `nullptr` 就整条链短路返回 `nullptr`；否则调用 `this->*transformer` —— 也就是虚函数指针形式的 transformer，把「按谓词种类分派」的工作交给调用方传入的 transformer（`select_transformer` / `truth_transformer` 等）。**注意这里 `transform()` 本身不产生任何 IN/EXISTS 的形状变更，形状变更发生在 transformer 指向的 `select_transformer()` 里。**

**标量子查询（scalar subquery）有自己的一套折叠规则。** `Item_singlerow_subselect::select_transformer()` 的判定条件是：不是集合运算、**没有表**、没有聚合、没有窗口函数、没有 `WHERE`、没有 `HAVING`。满足时置 `have_to_be_excluded = true`（让这个 query block 从计划里被剔除），把 `substitution = single_field`（子查询的 SELECT 表达式直接顶上来），并调 `outer->merge_contexts(select)` 把子查询的名字解析上下文合并进父块、`substitution->fix_after_pullout(outer, select)` 修好被提升的表达式。`Item_singlerow_subselect::no_rows_in_result()` 里还有一个相关细节：只有 `unit->uncacheable & UNCACHEABLE_DEPENDENT` 时才置 `no_rows = true`。

## 怎么实现的

### 入口与调用顺序

改写的**第一个决策点**在 `sql/sql_resolver.cc` 的名称解析阶段（`Query_block` 解析谓词时）。那里有一份编号为 0 到 16 的**半连接准入条件清单**，源码注释逐条列明，例如：

- 条件 0：`semijoin_enabled(thd)`——半连接开关打开。
- 条件 2：`is_simple_query_block()`——子查询不能是集合运算或带括号的查询表达式。
- 条件 3、4、5：`no_aggregates`（合成 `!is_grouped() && !with_sum_func && having_cond() == nullptr && !has_windows()`）。
- 条件 6：子查询谓词必须在 `ON`/`WHERE` 子句里，且处于该子句 AND 树的顶层（`outer->condition_context == enum_condition_context::ANDS`）；`ON` 子句里的半连接在超图优化器下被禁用（注释 6x 说明理由：半连接会让 join nest 不再是可接受的连接树，扰动超图优化器）。
- 条件 13：`deterministic`——IN 的左操作数必须确定性。
- 条件 16：`is_row_count_valid_for_semi_join()`——`OFFSET` 必须从第一行开始且 `LIMIT` 不能为 0。

满足全部条件时，谓词的 `strategy` 被置为 `Subquery_strategy::CANDIDATE_FOR_SEMIJOIN`，并被登记进 `outer->sj_candidates`。**若半连接不成立，同一段代码尝试「转派生表」**（条件 1 到 17，`try_convert_to_derived` 由 `thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SUBQUERY_TO_DERIVED)` 或二级引擎优化模式触发），置 `CANDIDATE_FOR_DERIVED_TABLE`。

接着 `sql/sql_resolver.cc` 的 `Query_block::flatten_subqueries()` 分三批处理：

1. **先处理已经是 `SEMIJOIN` 的**：调 `convert_subquery_to_semijoin()`，把它变成 join nest。`OPT_TRACE_TRANSFORM` 记录的转换名是 `"IN (SELECT)"` → `"semijoin"` 或 `"antijoin"`（取决于 `subq_item->can_do_aj`）。
2. **再处理 `UNSPECIFIED` 的**（即没被半连接选中的）：调 `subq_item->select_transformer(thd, subq_item->unit->first_query_block())`——这一步才真正进入 `Item_in_subselect::select_transformer()`。
3. **最后**把 `substitution`（若非空）用 `replace_subcondition()` 塞回外层的 `WHERE` 或 `join_cond_ref()`。

### `select_in_like_transformer()`：IN/ALL/ANY 的分派

`sql/item_subselect.cc` 的 `Item_in_subselect::select_in_like_transformer()` 是 IN/ALL/ANY/SOME 的统一入口：

- 若 `changed` 已置位（prepared statement 的重复执行）直接返回，不再改写。
- 创建 `optimizer = new Item_in_optimizer(this)`（若尚未创建）。
- 若 `strategy == Subquery_strategy::UNSPECIFIED`，置为 `CANDIDATE_FOR_IN2EXISTS_OR_MAT`——源码注释说这是「至少暂时」选 IN-to-EXISTS。
- 按 `left_expr->cols()` 分派：`cols() == 1` 走 `single_value_transformer()`；否则要求 `func == &eq_creator`（行操作只支持 `=ANY`(IN) 与 `<>ALL`(NOT IN)），否则报 `ER_OPERAND_COLUMNS`，再走 `row_value_transformer()`。

`Item_allany_subselect::select_transformer()` 是同一套逻辑的入口（`return select_in_like_transformer(thd, select, func);`），区别是它会先 `if (upper_item) upper_item->show = true;`。

### `single_value_transformer()`：三条分支

单列 IN 的改写有三条互斥分支，按顺序判断：

**分支一：ALL/ANY 改写成 MIN/MAX。** 准入条件是 `!func->eqne_op()`（比较符是 `>` / `<` / `>=` / `<=` 而非 `=` / `<>`）、`!unit->uncacheable`（不相关）、以及 UNKNOWN 结果被当作 FALSE（`abort_on_null`、或 `upper_item->ignore_unknown()`、或左右都不可空）。再往下还要子查询本身「干净」：未显式分组、无 `HAVING`、无聚合与窗口函数、单 query block、有表；并且 `ALL` 形式下内层不可空（源码注释：`For ALL: MIN ignores NULL`）。

改写分两种形态：
- 子查询「干净」时，把 `select->base_ref_items[0]` 换成 `Item_sum_max` 或 `Item_sum_min`（按 `func->l_op()` 选），再造一个 `Item_singlerow_subselect` 包起来。这是「SELECT(MIN)」形态。
- 否则造 `Item_maxmin_subselect(this, select, func->l_op(), substype() == ANY_SUBS)`，保留原 WHERE。这是「MIN (SELECT)」形态。

两条路径都会 `thd->lex->m_subquery_to_derived_is_impossible = true`（或等价的 `row_value_in_to_exists_transformer()` 里那一句），表示这条谓词不能再被转成派生表。

**分支二：IN-to-EXISTS 的准备。** 当 `substitution == nullptr` 时（即这是集合操作的第一个或唯一一个 query block）：

- `substitution = optimizer`，即整个 IN 谓词被 `Item_in_optimizer` 接管。
- 调 `optimizer->fix_left(thd, nullptr)` 解析左侧表达式。
- 造一个 `Item_ref` 指向 `optimizer->get_cache()`，作为「被注入到子查询里的左表达式」——记在 `m_injected_left_expr`。
- 调 `mark_as_outer(left_expr, 0)` 判断左表达式是否要标记为「相对于子查询是外部的」。`mark_as_outer()` 的三个判据是：左列不是常量项；或（非 `abort_on_null` 且左列可空）；或（左表达式本身是子查询且不是基本常量项）。标记后置 `left->depended_from = select->outer_query_block()`。源码注释解释了为什么要这么做：被标记为 outer 后，`JOIN::optimize()` 不会把用到它的触发条件当成常量去求值，只在 `JOIN::exec()`（由 `Item_in_optimizer::val_int()` 调用）里求值。
- 记录 `in2exists_info->dependent_before = unit->uncacheable & UNCACHEABLE_DEPENDENT`；若左表达式不是常量项则 `unit->uncacheable |= UNCACHEABLE_DEPENDENT`（改写把子查询变成了相关的），并记录 `in2exists_info->dependent_after`。

**分支三：调用注入器。** 最后 `return single_value_in_to_exists_transformer(thd, select, func);`。

### `single_value_in_to_exists_transformer()`：注入的两套形状

这个函数的注释直接给出了改写形状。它分两种情况：

**情况 A：子查询有 `HAVING`、显式分组、聚合函数或窗口函数。** 注入到 `HAVING`：

```
SELECT ie FROM ... HAVING subq_having AND trigcond(oe $cmp$ ref_or_null_helper<ie>)
```

此时用 `Item_ref_null_helper` 包住 `select->base_ref_items[0]`，让内层值可空。`Item_ref_null_helper` 的引入还带来一个额外的指针记账问题：源码注释说明，如果 `selected->type() == SUM_FUNC_ITEM`，需要把这个引用记进 `selected_sum->referenced_by[0]`（或 `[1]`），否则外层做 `split_sum_func` 时会漏掉替换点。

**情况 B：普通子查询。** 注入到 `WHERE`：

```
SELECT 1 FROM ... WHERE (oe $cmp$ ie) AND subq_where
```

其中 `oe $cmp$ ie` 用 `func->create(m_injected_left_expr, orig_item)` 造，且**只有当 `!select->source_table_is_one_row() || select->where_cond() != nullptr` 时才注入**——若子查询只有一行且没有 `WHERE`，走的是更下面的「直接替换成等值」分支。

`in2exists_info->added_to_where = true` 在注入 `WHERE` 后置位；`in2exists_info->added_to_where = false` 在函数开头先置位（因为有 `HAVING` 分支不会动 `WHERE`）。这个标志的用途在 `sql/sql_optimizer.cc` 的 `compare_costs_of_subquery_strategies()` 里：只有 `in_pred->in2exists_added_to_where()` 为真时，才需要为物化方案**另跑一遍连接顺序搜索**（`optimize_semijoin_nests_for_materialization(this)` 后跟 `Optimize_table_order(thd, this, nullptr).choose_table_order()`）；否则源码注释说「物化方案与 EXISTS 方案的计划相同，不要再算一遍」，对应 trace 里的 `surely_same_plan_as_EXISTS`。

### 定案：EXISTS 还是物化

`sql/sql_optimizer.cc` 的 `JOIN::decide_subquery_strategy()` 是定案点，它按 `query_expression()->item->substype()` 分派：`IN_SUBS` / `ALL_SUBS` / `ANY_SUBS` 三种继续，其它类型直接返回 false（不改写）。然后：

- 若 `strategy == CANDIDATE_FOR_IN2EXISTS_OR_MAT`，调 `compare_costs_of_subquery_strategies(&chosen_method)` 做代价比较。
- 定案为 `SUBQ_EXISTS` 时调 `in_pred->finalize_exists_transform(thd, query_block)`；若子查询带窗口函数则报 `ER_NOT_SUPPORTED_YET`（注释标注了 issue 号 WL#10431）。
- 定案为 `SUBQ_MATERIALIZATION` 时调 `in_pred->finalize_materialization_transform(thd, this)`。

**`finalize_exists_transform()` 做的事很少**：把 `unit->global_parameters()->select_limit` 设为 `Item_int(1)`（只取一行就够判定存在性）、置 `m_internal_limit = true`、调 `unit->set_limit()`、调 `unit->finalize(thd)`，然后 `query_block->join->allow_outer_refs = true`（供 `JOIN::set_prefix_tables()` 用），并置 `strategy = SUBQ_EXISTS`。源码注释承认：「if the subquery is "SELECT1 UNION SELECT2" then this is not working optimally (Bug#14215895)」。

**`finalize_materialization_transform()` 做的事是「撤销 IN-to-EXISTS 的注入」**：

- 置 `strategy = SUBQ_MATERIALIZATION`。
- `join->where_cond = remove_in2exists_conds(join->where_cond)`；`join->having_cond` 同理。源码注释解释原因：「The conditions added by in2exists depend on the concrete value from the outer query block, so they need to be removed before we materialize.」
- `unit->change_to_access_path_without_in2exists(thd)`——注释标注这一步「只对超图优化器相关」。
- 断言 `!in2exists_info->dependent_before`（原来就不相关才能物化）。
- 清除依赖标记：`join->query_block->uncacheable &= ~UNCACHEABLE_DEPENDENT; unit->uncacheable &= ~UNCACHEABLE_DEPENDENT;`
- 造 `subselect_hash_sj_engine` 并调 `new_engine->setup(thd, *unit->get_unit_column_types())`；失败则 `new_engine->cleanup(); destroy(new_engine); return true;`（报错，注释说「For some reason we cannot use materialization for this IN predicate」）。
- 置 `join->allow_outer_refs = false`。

### 物化引擎：`subselect_hash_sj_engine`

`subselect_hash_sj_engine::setup()` 的建表过程值得单列，因为它决定了物化表的物理形态：

- 造 `Query_result_union` 作为结果接收器，调 `create_result_table(thd, tmp_columns, /*Eliminate duplicates=*/true, thd->variables.option_bits | TMP_TABLE_ALL_COLUMNS, "<materialized_subquery>", true, true)`。**去重是显式开启的**（第二个布尔参数为 true），临时表名固定为 `"<materialized_subquery>"`。
- 索引形态二选一：若 `tmp_table->hash_field` 非空（走了唯一约束路径，额外加了一个 hash 字段），则 `tmp_key_parts = CountVisibleFields(tmp_columns)`、`key_length = ALIGN_SIZE(tmp_table->s->reclength)`；否则用 `tmp_key->user_defined_key_parts` 且 `key_length = ALIGN_SIZE(tmp_key->key_length) * 2`。
- 访问类型由唯一性决定：`type = (tmp_table->key_info[0].flags & HA_NOSAME) ? JT_EQ_REF : JT_REF;`
- 为每个 key part 造一个 `Item_func_eq` 作为「后置过滤条件」（`cond` 是 `Item_cond_and`）。源码注释说明为什么要这个后置过滤：一是外层列类型比内层长时索引查找会用截断值当搜索键，产生假阳性；二是索引建在 `hash_field` 上时并不唯一。
- NULL 处理：若临时表列可空且 `!item->abort_on_null`，则设 `ref.null_ref_key = cur_ref_buff` 且 `mat_table_has_nulls = NEX_UNKNOWN`；否则 `ref.null_ref_key = nullptr` 且 `mat_table_has_nulls = NEX_IRRELEVANT_OR_FALSE`。

`subselect_hash_sj_engine::exec()` 的执行顺序：

1. 若还未物化（`!is_materialized`），调 `m_iterator->Init()` 触发物化，置 `is_materialized = true`。
2. 判定是否零行：若 `table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT`，直接看 `table->file->stats.records == 0`；否则用 `TableScanIterator` 扫一遍。**零行则 `item->value = false` 并直接返回**（注释：「The correct answer is FALSE」）。
3. 若左表达式第一列是 NULL（`item->left_expr->element_index(0)->null_value`），则 `item->value = true` 并返回——注释解释这是「做一次所有触发条件都被禁用的搜索，肯定能找到一行」，调用方会把它翻译成 UNKNOWN。
4. 否则 `hash = 0;` 后调 `ExecuteExistsQuery(thd, item->unit, m_iterator.get(), &found)`，`item->value = found; item->assigned(true);`。
5. 若无精确匹配且 `mat_table_has_nulls != NEX_IRRELEVANT_OR_FALSE`，还要补一次「内层是否存在 NULL」的探测（`mat_table_has_nulls == NEX_UNKNOWN` 时用 `safe_index_read()` 查一次并把结果记成 `NEX_TRUE` 或 `NEX_IRRELEVANT_OR_FALSE`）；若确认内层有 NULL，则 `item->value = false; item->was_null = true;`——即答案应为 UNKNOWN。

`safe_index_read()` 调的是 `table->file->ha_index_read_map(table->record[0], ref.key_buff, make_prev_keypart_map(ref.key_parts), HA_READ_KEY_EXACT)`，出错时走 `report_handler_error(table, error)`。

### 运行期：`Item_subselect::exec()` 与 `Item_in_subselect::exec()`

`sql/item_subselect.cc` 的 `Item_subselect::exec()` 是所有子查询求值的公共入口，顺序固定：

1. `if (thd->is_error() || thd->killed) return true;` —— 有错或被 kill 就不执行。
2. 断言 `!thd->lex->is_view_context_analysis()`（分析视图时不应求值子查询）。
3. `DBUG_EXECUTE_IF("subselect_exec_fail", return true;);` —— 调试钩子，用来模拟子查询执行失败。
4. Opt_trace：若 `traced_before` 且未开启 `Opt_trace_context::REPEATED_SUBSELECT`，则用 `Opt_trace_disable_I_S` 关掉本次追踪；trace 结构为 `subselect_execution` 对象 + `steps` 数组，并记录 `unit->first_query_block()->select_number`。
5. 决定是否要建迭代器：`should_create_iterators = !(indexsubquery_engine != nullptr && indexsubquery_engine->engine_type() == subselect_indexsubquery_engine::HASH_SJ_ENGINE);` —— 物化引擎自带迭代器，不走 `exec()`。
6. 若 `!unit->is_optimized()`，调 `unit->optimize(thd, /*materialize_destination=*/nullptr, /*create_iterators=*/false, /*finalize_access_paths=*/false)`。源码注释解释这是懒优化路径（`DO` / `SET` 语句可能走到这里），并明确承认：「as it stands, we will always implicitly choose in2exists in these cases (DO/SET)」——即这两种语句**总是**走 IN-to-EXISTS，不做代价比较。
7. 若需要建迭代器且 `unit->root_access_path() != nullptr`，调 `unit->finalize(thd)` 与 `unit->force_create_iterators(thd)`。
8. 分派：`if (indexsubquery_engine != nullptr) return indexsubquery_engine->exec(thd); else return subquery->exec(thd);`

`Item_in_subselect::exec()` 在此之上多做了两件事：

- **左侧缓存。** `if (need_expr_cache && !left_expr_cache && strategy == Subquery_strategy::SUBQ_MATERIALIZATION && init_left_expr_cache(thd)) return true;`——**只有物化策略才建左侧缓存**。源码注释解释了为什么延迟到此刻才建：`Cached_item` 直接持有已解析的 field（不是 item），在创建临时表的情况下这些 field 可能指向错误的 field。
- **缓存命中即短路。** `update_item_cache_if_changed(*left_expr_cache)` 返回值小于 0 表示「新值与上次相同」，此时直接 `return false`，复用上次的 `value` / `was_null` / `null_value`，不做完整执行。
- **重复执行时清状态。** `const bool uncacheable = unit->uncacheable || indexsubquery_engine != nullptr;` 若 `unit->is_executed() && uncacheable`，则清 `null_value = false; was_null = false;`。源码注释说物化引擎与索引子查询引擎「都是 uncacheable（因为依赖左侧），即使底层 unit 没被标记」。

### 物化准入条件：`subquery_allows_materialization()`

`sql/sql_resolver.cc` 的 `Item_in_subselect::subquery_allows_materialization()` 逐条给出物化的拒绝理由，每条都对应一个 `cause` 字符串：

| 条件 | cause |
|---|---|
| `substype() != Item_subselect::IN_SUBS` | `not an IN predicate` |
| `m_subquery_used_tables & RAND_TABLE_BIT` | `non-deterministic` |
| `!query_block->is_simple_query_block()` | `in set operation or a parenthesized query expression` |
| 内层无表 | `no inner tables` |
| `!outer->join` | `parent query has no JOIN` |
| `!outer->leaf_tables` | `no tables in outer query` |
| `dependent_before_in2exists()` | `correlated` |
| `!types_allow_materialization(outer_item, inner_item)` | `type mismatch` |

`correlated` 这一条的源码注释里有一段自我批评：这是一个「overly restrictive condition」，并写出了想放宽的方向（子查询不相关，或相关到 IN 谓词之外的外层，或相关到紧邻外层且不含 GROUP BY / ORDER BY [LIMIT] / 聚合函数，且谓词不在 `NOT IN` 之下）。类型限制那条注释引用了 BUG#36752。

`sql/sql_optimizer.cc` 的 `compare_costs_of_subquery_strategies()` 在比较代价前还会做三道前置检查：

- 若 `allowed_strategies == CANDIDATE_FOR_IN2EXISTS_OR_MAT` 且 `query_expression()->uncacheable & UNCACHEABLE_RAND`，则强制降级为 `SUBQ_EXISTS`——非确定性子查询不能物化，除非被强制指定。
- 若 `!parent_join || !parent_join->child_subquery_can_materialize` 则直接返回 false（不物化）。`child_subquery_can_materialize` 在 `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 里被刻意**晚一点**置真，源码注释解释了原因：置得晚就能让 `compare_costs_of_subquery_strategies()` 借此判断「常量条件求值是否已结束」，而这本身会影响扇出估计。
- 再调一次 `in_pred->subquery_allows_materialization(...)`。源码注释说明为什么每次优化都要重测：「each execution of a prepared statement may use a different type of parameter」。

### 状态回滚：`Item_in_subselect::cleanup()`

`sql/item_subselect.cc` 的 `Item_in_subselect::cleanup()` 按 `strategy` 分支处理：

- `SUBQ_MATERIALIZATION`：若 `in2exists_info->dependent_after` 为真，把 `UNCACHEABLE_DEPENDENT` 重新加回 `unit->first_query_block()->uncacheable` 与 `unit->uncacheable`；然后 fallthrough 到下一分支。
- `SUBQ_EXISTS`（以及 fallthrough 过来的物化分支）：把 `unit->global_parameters()->select_limit = nullptr`，并把 `strategy` 退回 `CANDIDATE_FOR_IN2EXISTS_OR_MAT`。源码注释说这是「Back to EXISTS_OR_MAT, so that next execution of this statement can choose between the two」——即 prepared statement 的第二次执行会重新做一次代价比较。

同时 `cleanup()` 会销毁左侧缓存（`left_expr_cache->destroy_elements(); destroy(left_expr_cache); left_expr_cache = nullptr;`）并复位 `left_expr_cache_filled = false; need_expr_cache = true;`。

## 关键取舍

**代价比较只在「两个候选都还活着」时才发生，而且发生在计划期的后段。** `compare_costs_of_subquery_strategies()` 依赖 `join->{best_positions, best_read, best_rowcount}` 里已有 EXISTS 方案的执行计划——源码注释明确写了这个前置条件，并要求「This plan choice has to happen before calling functions which set up execution structures, like JOIN::get_best_combination()」。代价是：为了比较物化方案，可能要在 `in2exists_added_to_where()` 为真时**把整个连接顺序搜索再跑一遍**（`Optimize_table_order(thd, this, nullptr).choose_table_order()`），并额外分配 `POSITION[tables + sj_nests]` 数组（源码注释说明「Up to one extra slot per semi-join nest is needed (if materialized)」）。

**IN-to-EXISTS 与物化共用一套「改写过的」中间状态，物化要把它撤销。** 这是本层最不优雅但最关键的设计取舍：`finalize_materialization_transform()` 必须调用 `remove_in2exists_conds()` 把注入的条件从 `WHERE`/`HAVING` 里摘干净，还要调 `change_to_access_path_without_in2exists()` 修超图优化器的访问路径，还要清 `UNCACHEABLE_DEPENDENT`。**代价**：注入时打的 `set_created_by_in2exists()` 标记必须覆盖所有分支（源码注释专门提到 `item` 与 `OR` 节点都要打），否则撤销会漏条件。

**「相关」与「可缓存」被拆成两个独立维度。** `uncacheable` 是 `uint8` 位掩码，`UNCACHEABLE_DEPENDENT`（相关）与 `UNCACHEABLE_RAND`（非确定性）是两个独立的位。这意味着一个子查询可以「不相关但非确定性」——这种组合在 `single_value_transformer()` 里恰好把 MIN/MAX 改写否掉（因为要求 `!unit->uncacheable`），在 `compare_costs_of_subquery_strategies()` 里又恰好把物化否掉（因为要求没有 `UNCACHEABLE_RAND`），最终只剩 EXISTS 一条路。

**NULL 正确性优先于改写简洁性。** `Item_func_trig_cond` 包裹、`Item_is_not_null_test` 注入、`Item_ref_null_helper` 包装、`mat_table_has_nulls` 三态（`NEX_UNKNOWN` / `NEX_TRUE` / `NEX_IRRELEVANT_OR_FALSE`）这一整套机制，全部是为了保住 `IN` 的三值语义。**代价**：`single_value_in_to_exists_transformer()` 里同一个「是否需要区分 NULL 与 FALSE」的判断（`!abort_on_null && left_expr->is_nullable()`）在函数体内出现了多次，每次都要重复包一层 `Item_func_trig_cond`，生成的树节点数明显膨胀。

**「不相关」判定被明确标注为过度保守。** `subquery_allows_materialization()` 里 `dependent_before_in2exists()` 判为 `correlated` 的那条，源码注释自己写着「This is an overly restrictive condition」，并给出了想放宽的完整条件式。**代价**：相当一部分实际上不相关、完全可以物化的子查询被强制走 EXISTS。

**标量子查询的折叠条件极窄，且折叠后 query block 被整个剔除。** `Item_singlerow_subselect::select_transformer()` 要求「无表 + 无聚合 + 无窗口 + 无 WHERE + 无 HAVING + 非集合运算」，任一条不满足就不折叠。折叠的代价是 `have_to_be_excluded = true`——子查询的 query block 从计划里消失，`unit->exclude_level()` 被调用。**收益**：EXPLAIN 里这类子查询被标记为优化掉（`CTX_OPTIMIZED_AWAY_SUBQUERY`，由 `Item_subselect::subq_opt_away_processor()` 设置）。

**`DO` / `SET` 语句不走代价比较，直接隐式选 IN-to-EXISTS。** `Item_subselect::exec()` 的源码注释自己承认这一点，并说「We should probably have made a cost-based choice between the two here, but as it stands, we will always implicitly choose in2exists in these cases (DO/SET)」。这是被显式记录在案的已知不精确之处。

**半连接的 ON 子句限制是为超图优化器让路。** `sql/sql_resolver.cc` 的条件 6a/6x 注释解释：`ON` 子句里的半连接变换会让 join nest 不再是可接受的连接树，扰动超图优化器，所以禁用；但在二级引擎优化（`Secondary_engine_optimization::SECONDARY`）时又打开，因为此时拒绝错误的计划更容易。

**物化方案在「内层有 NULL」时的正确性靠一次额外的索引探测兜底。** `subselect_hash_sj_engine::exec()` 在无精确匹配时还要判断内层是否存在 NULL，否则 FALSE 与 UNKNOWN 会被混淆。**代价**：多一次 `safe_index_read()`（`ha_index_read_map` + `HA_READ_KEY_EXACT`）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/item_subselect.cc` | `Item_in_subselect::transform` | IN 谓词的 transform 入口：先递归改写 `left_expr`，再分派给传入的 transformer；本函数本身不改写形状 |
| `sql/item_subselect.cc` | `Item_subselect::exec` | 所有子查询求值的公共入口：错误/killed 短路、Opt_trace 追踪、懒优化路径、迭代器建立、按 `indexsubquery_engine` 是否存在分派执行 |
| `sql/item_subselect.cc` | `Item_in_subselect::exec` | IN 谓词求值：仅物化策略建左侧缓存、缓存命中即短路、重复执行时清 `null_value` 与 `was_null` |
| `sql/item_subselect.cc` | `Item_in_subselect::select_in_like_transformer` | IN/ALL/ANY/SOME 统一分派：建 `Item_in_optimizer`、置默认策略、按 `left_expr->cols()` 分派单列/行值两条路 |
| `sql/item_subselect.cc` | `Item_in_subselect::single_value_transformer` | 单列 IN 的三条分支：ALL/ANY 改 MIN/MAX、IN-to-EXISTS 的准备（`m_injected_left_expr`、`in2exists_info`）、调用注入器 |
| `sql/item_subselect.cc` | `Item_in_subselect::single_value_in_to_exists_transformer` | IN-to-EXISTS 的谓词注入器：按有无 HAVING/分组/聚合分两套形状，构造 `trig_cond` 与 `is_not_null_test`，打 `created_by_in2exists` 标记 |
| `sql/item_subselect.cc` | `Item_in_subselect::row_value_transformer` | 行值 IN（多列）的 IN-to-EXISTS 准备路径 |
| `sql/item_subselect.cc` | `Item_in_subselect::row_value_in_to_exists_transformer` | 行值 IN 的注入器：逐列造等值条件与 NULL 判定，注入 WHERE 或 HAVING |
| `sql/item_subselect.cc` | `Item_in_subselect::mark_as_outer` | 判定注入的左表达式是否要标记为「相对于子查询是外部的」，避免被当作常量提前求值 |
| `sql/item_subselect.cc` | `Item_in_subselect::finalize_exists_transform` | EXISTS 方案定案：加 `LIMIT 1`（`Item_int(1)` + `m_internal_limit`）、`unit->finalize()`、`allow_outer_refs = true` |
| `sql/item_subselect.cc` | `Item_in_subselect::finalize_materialization_transform` | 物化方案定案：撤销 IN-to-EXISTS 注入、清 `UNCACHEABLE_DEPENDENT`、建 `subselect_hash_sj_engine` |
| `sql/item_subselect.cc` | `remove_in2exists_conds` | 按 `created_by_in2exists()` 标记把注入的条件从合取树里剔除并重拼 |
| `sql/item_subselect.cc` | `Item_in_subselect::cleanup` | 状态回滚：销毁左侧缓存、按策略恢复 `UNCACHEABLE_DEPENDENT`、把 `strategy` 退回 `CANDIDATE_FOR_IN2EXISTS_OR_MAT` |
| `sql/item_subselect.cc` | `Item_in_subselect::val_bool_naked` | 改写后的真实求值点：`was_null && !value` 翻译为 UNKNOWN，取反与 IS TRUE/FALSE 交由 `Item_in_optimizer` |
| `sql/item_subselect.cc` | `Item_in_subselect::init_left_expr_cache` | 物化策略下建左侧 `Cached_item` 缓存；外层无 `qep_tab` 时放弃建缓存 |
| `sql/item_subselect.cc` | `Item_singlerow_subselect::select_transformer` | 标量子查询折叠：无表无聚合无窗口无 WHERE 无 HAVING 时把 SELECT 表达式顶上来并剔除该 query block |
| `sql/item_subselect.cc` | `Item_singlerow_subselect::no_rows_in_result` | 仅当 `UNCACHEABLE_DEPENDENT` 时置 `no_rows = true` |
| `sql/item_subselect.cc` | `Item_exists_subselect::choose_semijoin_or_antijoin` | 按 `value_transform` 判定能否做半连接或反连接，并记录 `can_do_aj` |
| `sql/item_subselect.cc` | `Item_exists_subselect::resolve_type` | EXISTS 语义下加 `LIMIT 1`（`Item_int(1)` + `m_internal_limit`） |
| `sql/item_subselect.cc` | `subselect_hash_sj_engine::setup` | 建物化临时表：去重、`TMP_TABLE_ALL_COLUMNS`、hash 索引或普通索引、逐 key part 造后置等值过滤条件 |
| `sql/item_subselect.cc` | `subselect_hash_sj_engine::exec` | 物化执行：首次 `Init()` 触发物化、零行短路、左值为 NULL 时置 UNKNOWN 语义、命中查找、内层 NULL 探测 |
| `sql/item_subselect.cc` | `safe_index_read` | 物化表上的精确索引读：`ha_index_read_map` + `HA_READ_KEY_EXACT`，错误走 `report_handler_error` |
| `sql/sql_optimizer.cc` | `JOIN::decide_subquery_strategy` | 定案点：按 `substype()` 分派，触发代价比较或直接落 EXISTS / 物化 |
| `sql/sql_optimizer.cc` | `JOIN::compare_costs_of_subquery_strategies` | EXISTS 与物化的代价比较：`cost_exists` 对 `cost_mat`，必要时另跑一遍连接顺序搜索 |
| `sql/sql_optimizer.cc` | `calculate_subquery_executions` | 逐层向外累乘父查询扇出，算出子查询被执行多少次 |
| `sql/sql_optimizer.cc` | `JOIN::optimize` | 把 `child_subquery_can_materialize` 置为 true 的位置（刻意置晚，让常量条件求值状态可被代价比较利用） |
| `sql/sql_resolver.cc` | `Item_in_subselect::subquery_allows_materialization` | 物化准入的逐条判定，每条拒绝理由对应一个 `cause` 字符串 |
| `sql/sql_resolver.cc` | `Query_block::flatten_subqueries` | 三批处理：已选半连接的转 join nest、`UNSPECIFIED` 的走 `select_transformer()`、把 `substitution` 塞回外层条件 |
| `sql/sql_resolver.cc` | `Query_block::convert_subquery_to_semijoin` | 把 IN 子查询转成半连接 nest 并生成等值条件 |
| `sql/sql_resolver.cc` | `Query_block::resolve_placeholder_tables` 内的半连接准入判断 | 条件 0 到 16 的半连接准入清单（简单 query block、无聚合、AND 顶层、确定性、`OFFSET`/`LIMIT` 合法等） |

## 与主核 / AI 的关系

主核（`engine/db`）目前没有子查询改写这一层：`engine/db` 的执行器是单表扫描加过滤，`query/` 层也没有把谓词里的嵌套查询提升成连接 nest 或物化表的机制。

**架构推论：** MySQL 这套子查询改写有三点结构值得主核借鉴。其一是**「改写」与「执行」被分成两个阶段并用显式状态位衔接**：`Subquery_strategy` 这个枚举把「未定（`UNSPECIFIED`）」、「候选做 IN-to-EXISTS 或物化（`CANDIDATE_FOR_IN2EXISTS_OR_MAT`）」、「候选做半连接（`CANDIDATE_FOR_SEMIJOIN`）」、「候选转派生表（`CANDIDATE_FOR_DERIVED_TABLE`）」、「已定 EXISTS（`SUBQ_EXISTS`）」、「已定物化（`SUBQ_MATERIALIZATION`）」分开记，主核若把「是否相关」直接编码成数据结构形状（嵌套 vs 扁平），就没法在执行前反复重新决策。其二是**「标记 + 反向撤销」的可逆改写手法**：注入谓词时给每个新节点打 `created_by_in2exists()`，改主意时按标记整段摘除——这个模式让「先按一条路改写、发现不合适再退回」成为可能，而不必一开始就做完备的前置判定。其三是**「代价比较」放在有完整执行计划之后**：`compare_costs_of_subquery_strategies()` 要求 `best_positions` / `best_read` / `best_rowcount` 已经算好才动手，主核若要在计划期比较「物化 vs 逐行求值」，同样得先把两条路的估算都做出来。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「模型写的子查询会不会被优化器救回来」。MySQL 的做法暴露了一个现实：**子查询改写对模型是不可见的**。模型写 `WHERE x IN (SELECT y FROM t2)`，优化器可能把它转成半连接（内表拉进 join nest）、可能转成物化、也可能转成相关 EXISTS——三种形态的执行代价可以差一个数量级，但模型在写 SQL 时无从得知自己落在哪一种上。**架构推论：** 主核若要让 AI 参与计划决策（而不只是生成 SQL 文本），就应该把 MySQL 藏在 `Optimizer Trace` 后面的这层决策暴露成可查询结构：MySQL 的 `OPT_TRACE_TRANSFORM` 机制已经在记录每次转换（`"IN (SELECT)"` → `"EXISTS (CORRELATED SELECT)"` / `"materialization"` / `"semijoin"` / `"antijoin"`），`compare_costs_of_subquery_strategies()` 也在 trace 里记下了 `cost_to_create_and_fill_materialized_table`、`cost_of_one_EXISTS`、`number_of_subquery_evaluations`、`cost_of_materialization`、`cost_of_EXISTS`、`chosen` 六个字段——**「为什么选了这条改写路」本身被当成一等可观测对象**，这个做法对 AI 场景比对用户暴露 EXPLAIN 更有价值：AI 需要知道的是「改写选择的依据」，而不只是「改写后的计划长什么样」。

## 参考

- [MySQL 源码：`sql/item_subselect.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/item_subselect.cc)
- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_optimizer.cc)
- [MySQL 源码：`sql/sql_resolver.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_resolver.cc)
- [MySQL 源码：`sql/item_subselect.cc`（raw 原文）](https://raw.githubusercontent.com/mysql/mysql-server/8.0/sql/item_subselect.cc)
- [MySQL 参考手册：子查询优化（Subquery Optimization）](https://dev.mysql.com/doc/refman/8.0/en/subquery-optimization.html)
- [MySQL 参考手册：半连接（Semijoin）策略](https://dev.mysql.com/doc/refman/8.0/en/semijoin.html)
- [MySQL 参考手册：子查询物化（Subquery Materialization）](https://dev.mysql.com/doc/refman/8.0/en/subquery-materialization.html)
- [MySQL 参考手册：EXPLAIN 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html)
- [MySQL 参考手册：优化器开关（optimizer_switch）](https://dev.mysql.com/doc/refman/8.0/en/optimizer-switch.html)
