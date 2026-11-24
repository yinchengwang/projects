---
project: mysql-server
feature: join-plan
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_optimizer.cc
    symbols: [JOIN::make_join_plan]
---

# 连接计划与表访问顺序

## 一句话职责

`JOIN::make_join_plan()` 是传统优化器里「定连接顺序 + 定每张表怎么访问」的编排函数：它先按行数与统计精确性把 0/1 行的表摘成 const 表并提前读出，再为每张表估行数、估算索引可用性，最后调用 `Optimize_table_order::choose_table_order()` 在依赖约束下搜索出一条代价最低的表访问序列，并用 `JOIN::get_best_combination()` 把这条序列落地成 `JOIN_TAB` 数组。

## 机制是什么

**这一层要产出两个东西：一张表的访问顺序，和每张表的访问路径。**

- **访问顺序（join order）**：哪张表先读、哪张表后读。它受两类约束夹住：一是**表依赖**（外连接的内外表、派生表的 lateral 依赖），二是**代价**（先读哪张表能让中间结果集更小）。
- **访问路径（access path）**：每张表用哪种方式读——`ref`（用索引等值查找）、`range`（范围扫描）、`index`（索引全扫）、`ALL`（全表扫描）。这一层的细节归 C03「索引与范围访问选择」。

MySQL 传统优化器把这两件事放在**同一个搜索过程**里：搜索树的每一层是「下一张要读的表」，每层同时为这张表试算所有访问路径并取代价最小者。搜索结果落在 `JOIN::best_positions`（`POSITION` 数组）与 `JOIN::best_ref`（`JOIN_TAB` 指针数组）里。

**四个承载数据的结构**（都在 `sql/sql_optimizer.cc` / `sql/sql_planner.cc` 里被 `JOIN::init_planner_arrays()` 分配）：

| 结构 | 类型 | 含义 |
|---|---|---|
| `JOIN::join_tab` | `JOIN_TAB *` | 每张表一个 `JOIN_TAB`，装着 `type()`、`keyuse()`、`const_keys`、`found_records` 等 |
| `JOIN::best_ref` | `JOIN_TAB **` | 表的**访问顺序**，同时被 `greedy_search()` 原地重排 |
| `JOIN::positions` | `POSITION[]` | 当前搜索中的部分计划 |
| `JOIN::best_positions` | `POSITION[]` | 搜索结束后的最优计划，落地时由 `get_best_combination()` 读 |

**`POSITION` 与 `JOIN_TAB` 的分工是理解这一层的前提。** `POSITION` 描述「在这个位置怎么读这张表」：`table`（哪个 `JOIN_TAB`）、`key`（用哪个 `Key_use`）、`rows_fetched`（读多少行）、`read_cost`（读的成本）、`filter_effect`（条件下过滤比例）、`prefix_rowcount`（前缀结果行数）、`prefix_cost`（前缀总成本）、`sj_strategy`（该位置上的半连接策略）、`n_sj_tables`（该半连接 nest 占几张表）、`use_join_buffer`。`JOIN_TAB` 描述「这张表本身」：`type()`（`JT_SYSTEM` / `JT_CONST` / `JT_REF` / `JT_RANGE` / `JT_ALL` / `JT_INDEX`）、`keyuse()`（可用索引列表）、`const_keys`、`skip_scan_keys`、`found_records`、`read_time`、`worst_seeks`、`embedding_map`、`dependent`。

**搜索策略是「小表穷举、大表贪心」。** `Optimize_table_order::determine_search_depth()` 里写死了分界常量 `max_tables_for_exhaustive_opt = 7`：待优化表数不超过 7 时 `search_depth = table_count + 1`（穷举），超过 7 时 `search_depth = 7`（贪心）。这个函数自己的注释承认这是「极其粗糙的实现」，是给更精细的分析留的桩。

**代价模型是 MySQL 独有的「前缀代价」累加。** 每确定一张表，`POSITION::set_prefix_join_cost()` 把这张表的读成本加上前一张表的 `prefix_rowcount` 乘以每行求值成本，再乘上过滤比例。`get_partial_join_cost()` 展示了同一个公式：`rowcount *= pos->rows_fetched; cost += pos->read_cost + cost_model->row_evaluate_cost(rowcount); rowcount *= pos->filter_effect;`。

**const 表是这一层的第一道剪枝。** 行数为 0 或 1 的表不参与连接顺序搜索，直接在计划期把行读出来（`join_read_const_table()`），它的行值还能反过来当作常量去生成新的 sargable 谓词。

**半连接（semi-join）与外连接依赖在计划期被特殊处理。** 半连接 nest 的内表在 `pull_out_semijoin_tables()` 里按函数依赖被「拉出」到外层；`propagate_dependencies()` 用 Warshall 算法算外连接依赖的传递闭包，并拒绝自依赖。

## 怎么实现的

### 编排：`JOIN::make_join_plan()`

`sql/sql_optimizer.cc` 的 `JOIN::make_join_plan()` 是这一层的总入口。它的返回值约定是 **`false` = 成功，`true` = 出错**（MySQL 优化器里 `true` 表示错误，与直觉相反）。函数体按固定顺序执行下列步骤：

1. **`init_planner_arrays()`** —— 分配 `join_tab` / `best_ref` / `map2table` / `positions` / `best_positions` 五个数组，并逐表做初始化。数组大小的算式写死了两条余量：`table_count + sj_nests + 2 + m_windows.elements`，源码注释解释「+2」是因为「planning stage uses 0-termination so needs +1」以及「after get_best_combination, we don't use 0-termination but need +2, to host at most 2 tmp sort/group/distinct tables」。
   - 逐表初始化里有一处硬逻辑：`tl->fetch_number_of_rows()` 失败时直接 `table->file->print_error(err, MYF(0))` 并返回错误。
   - 递归 CTE 的约束写在这里：递归引用表必须排在第一位（`tab->dependent |= query_block->recursive_reference->map()`），而递归引用表本身**不能用任何索引**（`table->covering_keys.clear_all()` 同时清掉 `keys_in_use_for_group_by` 与 `keys_in_use_for_order_by`）。
2. **`propagate_dependencies()`** —— 仅当 `query_block->outer_join || query_block->is_recursive()` 时执行。它算外连接依赖的传递闭包；若发现某张表依赖自己，报 `ER_CTE_RECURSIVE_FORBIDDEN_JOIN_ORDER`，错误里带上 `query_block->recursive_reference->alias`。随后调 `init_key_dependencies()`。
3. **`update_ref_and_keys()`** —— 仅当 `where_cond || query_block->outer_join` 时执行。它从 WHERE 条件里收集可用的索引访问（`Key_use`）与 sargable 谓词（`SARGABLE_PARAM`）。该函数是 `sql/sql_optimizer.cc` 里的 `static bool` 函数，参数里的 `normal_tables` 传的是 `~query_block->outer_join`。
4. **`pull_out_semijoin_tables()`** —— 仅当 `!query_block->sj_pullout_done && !query_block->sj_nests.empty()` 时执行一次，之后置 `query_block->sj_pullout_done = true`。它把「在键上有 `eq_ref` 候选」的内表从半连接 nest 里拉出来，源码注释解释为什么表不能被拉出：「with `t1 SEMIJOIN (t2 LEFT JOIN t3 ON ...) ON t1.a=t2.pk`, `t2` cannot be pulled out because `t3` depends on it」。
5. **`extract_const_tables()` / `extract_func_dependent_tables()`** —— 仅当查询没有 `OPTION_NO_CONST_TABLES` 时执行。前者按行数摘 const 表，后者按函数依赖摘 const 表（可重复多轮，源码用 `goto more_const_tables_found` 回跳）。
6. **`update_sargable_from_const(sargables)`** —— 仅当 `const_tables && sargables` 同时成立才调。用刚读出的 const 表的值生成新的 sargable 谓词。
7. **`estimate_rowcount()`** —— 为每张表估行数与读成本。
8. **`query_block->opt_hints_qb->apply_join_order_hints(this)`** —— 仅当有优化器提示且**没有** `SELECT_STRAIGHT_JOIN` 时应用连接顺序提示。
9. **`set_semijoin_embedding()` + `query_block->update_semijoin_strategies(thd)`** —— 仅当 `sj_nests` 非零时执行。
10. **`optimize_keyuse()`** —— 仅当 `!plan_is_const()` 时执行，给每个 `Key_use` 补 `ref_table_rows`。
11. **`optimize_semijoin_nests_for_materialization(this)`** —— 仅当 `sj_nests` 非零时执行，为每个可物化的半连接 nest **单独跑一遍连接顺序搜索**，把结果存进 `sj_nest->nested_join->sjm.positions`。
12. **`Optimize_table_order(thd, this, nullptr).choose_table_order()`** —— 主搜索。
13. **杀点检查。** `DBUG_EXECUTE_IF("bug13820776_1", thd->killed = THD::KILL_QUERY;);` 之后 `if (thd->killed || thd->is_error()) return true;`。
14. **`decide_subquery_strategy()`** —— 仅当 `query_expression()->item` 非空（即这是个子查询）时执行，在 IN-to-EXISTS 与物化之间二选一。
15. **`refine_best_rowcount()`** —— 用 LIMIT 与结果接收器修正行数估计。
16. **`get_best_combination()`** —— 把 `best_positions` 落地成 `best_ref` 里的 `JOIN_TAB` 序列。
17. **`finalize_derived_keys()`** —— 仅当 `query_block->materialized_derived_table_count` 非零时执行，清理 `update_ref_and_keys()` 为派生表加的键。
18. **空结果标记。** `if (const_table_map != found_const_table_map) zero_result_cause = "no matching row in const table";` —— 记下「const 表里没有匹配行」这个原因。

**`JOIN::optimize()` 里的调用点与后续。** `sql/sql_optimizer.cc` 的 `JOIN::optimize()` 在 `make_join_plan()` 返回真时先 `thd->send_kill_message()` 再报错。成功后紧跟一句断言 `ASSERT_BEST_REF_IN_JOIN_ORDER(this)`，注释写明此时「join_tab==NULL, JOIN_TABs are listed in order by best_ref」。若 `zero_result_cause != nullptr`，则调 `create_access_paths_for_zero_rows()` 后直接跳到收尾。

### 内存准备：`JOIN::init_planner_arrays()`

`sql/sql_optimizer.cc` 的 `JOIN::init_planner_arrays()` 逐表填 `JOIN_TAB`。要点：

- **依赖初始化。** `tab->dependent = tl->dep_tables;`
- **嵌套连接的 embedding map。** 外连接的内外表之间要维护 `embedding_map`：`tab->embedding_map |= nested_join->nj_map;`
- **可更新/可删除的表不能读索引。** `if (tl->is_updated() || tl->is_deleted()) table->no_keyread = true;`
- **lateral 派生表标记。** `if (tl->is_derived() && tl->derived_query_expression()->m_lateral_deps) has_lateral = true;`
- **系统表行数写死为 2。** `if (tl->schema_table) table->file->stats.records = 2;`

### 行数与代价估计：`JOIN::estimate_rowcount()`

`sql/sql_optimizer.cc` 的 `JOIN::estimate_rowcount()` 逐表算三样东西：

- **const/system 表。** `tab->type() == JT_SYSTEM || tab->type() == JT_CONST` 时，`tab->set_records(tab->found_records = 1);`，`tab->worst_seeks = tab->table()->file->worst_seek_times(1.0);`，`tab->read_time = tab->worst_seeks;`。
- **普通表。** `tab->set_records(tab->found_records = tab->table()->file->stats.records);`，读成本取 `tab->table()->file->table_scan_cost().total_cost()`。
- **最坏寻道代价。** `find_worst_seeks()` 的算式是 `min(table->file->worst_seek_times(num_rows / 10), table_scan_cost * 3)`，再与 `worst_seek_times(2.0)` 取大者——源码注释写明「Fix for small tables」。
- **范围扫描。** 只有当 `!tab->const_keys.is_clear_all() || !tab->skip_scan_keys.is_clear_all()` 时才调 `get_quick_record_count(thd, tab, row_limit, condition)`。返回 `HA_POS_ERROR` 表示无法估计；返回 0 且 `tab->table()->reginfo.impossible_range` 为真则判定条件恒假（`always_false_cond = true`）。

### 顺序搜索：`Optimize_table_order::choose_table_order()`

`sql/sql_planner.cc` 的 `Optimize_table_order::choose_table_order()` 是连接顺序搜索的入口：

- **全 const 的特例。** `if (join->const_tables == join->tables)` 时直接把 `positions` 拷进 `best_positions`，`join->best_read = 1.0;`，`join->best_rowcount = 1;`。
- **预先排序。** 有 `SELECT_STRAIGHT_JOIN` 时用 `Join_tab_compare_straight()` 排序（按依赖关系重排，依赖表排在被依赖表之后）；否则用 `Join_tab_compare_default()`（按记录数预排序）。**这个预排序是贪心剪枝的基础**——源码注释在 `best_extension_by_limited_search()` 里说「All items should be uncond. swapped to maintain '#rows-ordered' best_ref[]. This is critical for early pruning of bad plans.」
- **COND_FANOUT_FILTER 开关。** `if (thd->optimizer_switch_flag(OPTIMIZER_SWITCH_COND_FANOUT_FILTER) && join->where_cond)` 时清空 `cond_set` 并用 `Item::add_field_to_cond_set_processor` 填上参与谓词的列位。
- **两条搜索路径。** 有 `SELECT_STRAIGHT_JOIN` 走 `optimize_straight_join(join_tables)`（不重排，只按既定顺序算代价）；否则走 `greedy_search(join_tables)`。
- **收尾。** 非半连接 nest 时调 `fix_semijoin_strategies()` 定案半连接策略。

### 贪心搜索：`Optimize_table_order::greedy_search()`

`sql/sql_planner.cc` 的 `greedy_search()` 的循环骨架：

- 每一轮先 `join->best_read = DBL_MAX; join->best_rowcount = HA_POS_ERROR;`，然后调 `best_extension_by_limited_search(remaining_tables, idx, search_depth)` 找当前前缀的最优扩展。
- **终止条件。** `if (size_remain <= search_depth || use_best_so_far)` 时，`join->best_positions` 已是完整最优扩展，直接返回。
- **推进。** 否则取 `join->best_positions[idx]` 作为「最有希望的下一张表」，把它 `memmove` 到 `join->best_ref[idx]` 位置，并从 `remaining_tables` 里清掉它的位图，然后 `recalculate_lateral_deps_incrementally(idx + 1)`。

### 有限深度搜索：`Optimize_table_order::best_extension_by_limited_search()`

`sql/sql_planner.cc` 的这个函数是实际展开搜索树的地方：

- **候选表筛选。** 条件是 `(remaining_tables & real_table_bit) && !(eq_ref_extended & real_table_bit) && !(remaining_tables & s->dependent) && (!idx || !check_interleaving_with_nj(s))` —— 依赖未满足的表不进入本层，被 `eq_ref` 扩展过的表本层不再考虑。
- **代价计算。** `best_access_path(s, remaining_tables, idx, false, idx ? (position - 1)->prefix_rowcount : 1.0, position);` 之后 `position->set_prefix_join_cost(idx, cost_model);`。
- **剪枝。** `if (position->prefix_cost >= join->best_read && found_plan_with_allowed_sj)` 就跳过这个扩展——除非当前最优计划用了被禁用的半连接策略。
- **半连接状态推进。** `if (has_sj) advance_sj_state(remaining_tables, s, idx); else position->no_semijoin();`。源码注释给出一个量化理由：「Even if there are no semijoins, advance_sj_state() has a significant cost (takes 9% of time in a 20-table plan search)」。
- **杀点检查。** 函数入口有 `DBUG_EXECUTE_IF("bug13820776_2", thd->killed = THD::KILL_QUERY;);` 与 `if (thd->killed) return true;`。

### 访问路径选择：`Optimize_table_order::best_access_path()`

`sql/sql_planner.cc` 的 `best_access_path()` 为一张表选访问方式：

- **ref 访问先算。** 条件是 `tab->keyuse() != nullptr && (table->file->ha_table_flags() & HA_NO_INDEX_ACCESS) == 0`，然后调 `find_best_ref(...)`。
- **不走扫描的条件。** `if (rows_fetched < tab->found_records && best_read_cost <= tab->read_time)` 成立时**不**测试表扫描。源码注释把这条规则拆成 (1a)(1b)(2)(3)(4) 五条并逐条解释，其中 (1b) 的依据是「COST(ref_per_partial_row) * E(#partial_rows) vs COST(single_scan)」，(4) 是「FORCE INDEX(...) is used for table and there is 'ref' access path, but there is no quick select」。
- **join buffer 的禁用条件。** `disable_jbuf = disable_jbuf || idx == join->const_tables || !hint_table_state(join->thd, tab->table_ref, BNL_HINT_ENUM, OPTIMIZER_SWITCH_BNL) || join->deps_of_remaining_lateral_derived_tables & ~remaining_tables;` —— 第一个表不能缓冲、BNL 被关、有未满足的 lateral 依赖，三者任一成立就禁用。
- **扫描代价里的过滤假设。** `Optimize_table_order::calculate_scan_cost()` 里，当 `OPTIMIZER_SWITCH_COND_FANOUT_FILTER` 关闭且存在过滤条件时，用 `*rows_after_filtering = tab->found_records * 0.75;`——源码注释解释这是「assume that 25% of the rows will be filtered out by this condition」，目的是「force tables used in exprZ to be before this table in join order」。

### 计划落地：`JOIN::get_best_combination()`

`sql/sql_optimizer.cc` 的 `get_best_combination()` 把 `best_positions` 展开成 `best_ref`：

- **临时表槽位数。** `num_tmp_tables` 按 GROUP BY / DISTINCT / ORDER BY / 结果缓冲 / 窗口函数逐项累加，最后被 `if (num_tmp_tables > (2 + m_windows.elements)) num_tmp_tables = 2 + m_windows.elements;` 夹住上限。
- **半连接物化的位置调整。** 物化 nest 里的表被替换成对物化临时表的引用，物化临时表排在中间表之后。源码注释举了个布局例子：「outer1 - sj_mat_tmp1 - outer2 - sj_mat_tmp2 - outer3」。
- **半途失败的容错。** 源码注释明确写：物化临时表分配失败时不能立刻返回，否则 `best_ref` 与 `join_tab` 两个数组长度不一致会让清理逻辑无从遍历；因此要「record the error but continue filling best_ref」。

### 半连接策略定案：`Optimize_table_order::fix_semijoin_strategies()`

`sql/sql_planner.cc` 的 `fix_semijoin_strategies()` 从后往前扫 `best_positions`，按 `sj_strategy` 分别处理：`SJ_OPT_MATERIALIZE_LOOKUP`、`SJ_OPT_MATERIALIZE_SCAN`、`SJ_OPT_FIRST_MATCH`、`SJ_OPT_LOOSE_SCAN`、`SJ_OPT_DUPS_WEEDOUT`。其中物化分支要把 `optimize_semijoin_nests_for_materialization()` 阶段算出的局部计划 `memcpy` 回最终计划；源码注释承认这里有陈旧策略污染：「the source QEP had picked some semijoin strategy (noted SJY), different from materialization... this memcpy() is going to copy the source stale strategy SJY, wrongly」，并说明修正方式是「fixed for the first table of that range right after the memcpy(), and fixed for the rest of that range at the end of this iteration by setting it to SJ_OPT_NONE」。

### const 表的摘取与读取：`JOIN::extract_const_tables()`

`sql/sql_optimizer.cc` 的 `extract_const_tables()` 的判定分三档（枚举 `enum_const_table_extraction`：`extract_no_table` / `extract_empty_table` / `extract_const_table`）：

- **嵌套连接里的表不摘。** `tl->outer_join_nest()` → `extract_no_table`。
- **半连接里的表不摘。** `tl->embedding && tl->embedding->is_sj_or_aj_nest()` → `extract_no_table`，源码注释：「We do not currently pull out const tables from semi-join nests」。
- **外连接的唯一内表只摘空表。** `tab->join_cond()` → `extract_empty_table`，且要求 `table->file->stats.records == 0 || all_partitions_pruned_away` 同时 `(table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT)`。
- **普通表摘 const 的三个条件。** `(table->s->system || table->file->stats.records <= 1 || all_partitions_pruned_away)`、`!tab->dependent`、`(table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT)`、`!tl->is_fulltext_searched()`。

**读取路径。** 摘出的表逐个调 `join_read_const_table(tab, p_pos)`（定义在 `sql/sql_executor.cc`），返回值语义是：`0` = 找到一行或造出 NULL 补齐行；`-1` = 没找到行也没造 NULL 补齐行；`1` = 错误。`join_read_const_table()` 内部有一段针对 multi-UPDATE / multi-DELETE 的特殊处理：这类语句里 const 表会 `bitmap_set_all(table->read_set)` 全列读取，源码注释解释这是为了打破「read_set → 更新方式 → 访问路径 → 查询计划 → 哪些表是 const → 读 const 表 → read_set」这个循环依赖。

**`JOIN::mark_const_table()` 落位。** 它把 `POSITION` 写成一个「零成本、单行」的条目：`position->rows_fetched = 1.0; position->filter_effect = 1.0; position->prefix_rowcount = 1.0; position->read_cost = 0.0; position->loosescan_key = MAX_KEY; position->sj_strategy = SJ_OPT_NONE;`，然后把该表在 `best_ref` 里**尽量往后挪**再放到 `const_tables` 位置，最后 `tab->set_type(key ? JT_CONST : JT_SYSTEM);`。

### 子查询策略：`JOIN::decide_subquery_strategy()`

`sql/sql_optimizer.cc` 的 `decide_subquery_strategy()` 只处理 `IN_SUBS` / `ALL_SUBS` / `ANY_SUBS` 三种子查询（其余 `return false`）。当前策略是 `CANDIDATE_FOR_IN2EXISTS_OR_MAT` 时调 `compare_costs_of_subquery_strategies()` 比较；定案为 `SUBQ_EXISTS` 时调 `in_pred->finalize_exists_transform(thd, query_block)`，定案为 `SUBQ_MATERIALIZATION` 时调 `in_pred->finalize_materialization_transform(thd, this)`。**一个硬错误分支**：EXISTS 策略下若 `query_block->m_windows.elements > 0`，报 `ER_NOT_SUPPORTED_YET`。

### 行数估计的收尾修正：`JOIN::refine_best_rowcount()`

`sql/sql_optimizer.cc` 的 `refine_best_rowcount()` 做两件事：

- **派生表不能被当成 const。** `if (best_rowcount <= 1 && ... linkage == DERIVED_TABLE_TYPE) best_rowcount = PLACEHOLDER_TABLE_ROW_ESTIMATE;` —— 源码注释解释：估计成 0 或 1 行会让派生表被标成 const，「the query block will not be evaluated during optimization」。
- **LIMIT 封顶。** `best_rowcount = std::min(best_rowcount, query_expression()->select_limit_cnt);`

### 键使用信息的预处理：`JOIN::optimize_keyuse()`

`sql/sql_optimizer.cc` 的 `optimize_keyuse()` 给每个 `Key_use` 填 `ref_table_rows`：

- 只有一个表参与时取 `max<ha_rows>(tmp_table->file->stats.records, 100)`——源码注释写明「To avoid bad matches, we don't make ref_table_rows less than 100」。
- 外层引用（`used_tables == OUTER_REF_TABLE_BIT`）时 `ref_table_rows = 1`，源码注释：「Outer reference (external field) is constant for single executing of subquery」。

### 依赖传播：`JOIN::propagate_dependencies()`

`sql/sql_optimizer.cc` 的 `propagate_dependencies()` 用 Warshall 算法算传递闭包，源码注释给出复杂度 O((number of tables)^3) 并说明「most of the iterations will be shortcircuited when there are no dependencies to propagate」。它有一处「回退重做」逻辑：当某张已处理过的表的依赖集合被改变（`i > j && tab->dependent != was_dependent`）时，把外层循环指针回退 `i = j - 1; break;`。函数末尾检查自依赖：`if ((tab->dependent & tab->table_ref->map())) return true;`。

## 关键取舍

**「小表穷举、大表贪心」是硬编码的常量，不是自适应策略。** `max_tables_for_exhaustive_opt = 7` 这个常量直接决定了 7 张表以内是精确最优、8 张表以上是近似解。`determine_search_depth()` 的注释自称「This is an extremely simplistic implementation that serves as a stub for a more advanced analysis」，并把 `max_tables_for_exhaustive_opt = 7` 标成 TODO。代价是 8 表以上的计划不保证最优；收益是搜索时间不随表数爆炸。

**预排序 `best_ref` 是剪枝的前提，而它本身就是一种牺牲。** `Join_tab_compare_default()` 按记录数预排序后，`best_extension_by_limited_search()` 的无条件 `std::swap` 维护这个顺序，注释说这对「early pruning of bad plans」是 critical。这意味着搜索并非在所有排列上平等对待——记录数少的表天然被优先尝试。

**ref 访问「赢」扫描的判定依赖一个被显式承认粗糙的启发式。** `calculate_scan_cost()` 在关掉 `COND_FANOUT_FILTER` 时直接假设「25% 的行会被过滤掉」，源码注释写明这纯粹是为了让条件里引用的表排在前面。这是用统计假设换连接顺序质量。

**代价模型里有一条写死的倍率：`table_scan_cost * 3`。** `find_worst_seeks()` 用它给最坏寻道成本封顶，避免优化器因为寻道代价过高而放弃索引改走全表扫描。同一个函数用 `worst_seek_times(2.0)` 兜底，源码注释标注「Fix for small tables」。

**半连接物化要单独跑一遍搜索，代价是重复计算。** `optimize_semijoin_nests_for_materialization()` 对每个可物化的 nest 单独调一次 `Optimize_table_order(thd, join, sj_nest).choose_table_order()`，把局部计划存进 `sjm.positions`。这换来的是「主搜索可以按整 nest 的代价比较物化 vs 非物化」。

**物化计划回填时存在已知的状态污染，用「先污染后修正」处理。** `fix_semijoin_strategies()` 的 `memcpy` 会把局部计划里的陈旧半连接策略带进最终计划，源码注释承认这是错的（「wrongly」），修正方式是先给区间首表定案、其余位置留到本轮末尾统一置 `SJ_OPT_NONE`。

**「ref_table_rows 不低于 100」是硬编码下限。** `optimize_keyuse()` 里 `max<ha_rows>(tmp_table->file->stats.records, 100)`，注释理由是「To avoid bad matches」。代价是小表的 ref 扇出被高估。

**计划期的成本估算与执行期的实际行数必然有偏差，MySQL 的选择是「在计划期多算几轮」。** `estimate_rowcount()` 之后还要跑 `refine_best_rowcount()`，用 LIMIT 与结果接收器再修一次。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_optimizer.cc` | `JOIN::make_join_plan` | 本页主体：连接顺序与访问路径的总编排，串联下列全部步骤 |
| `sql/sql_optimizer.cc` | `JOIN::init_planner_arrays` | 分配 `join_tab` / `best_ref` / `positions` / `best_positions` 等数组并逐表初始化依赖 |
| `sql/sql_optimizer.cc` | `JOIN::propagate_dependencies` | 用 Warshall 算法算外连接依赖的传递闭包，拒绝自依赖（递归 CTE 顺序非法） |
| `sql/sql_optimizer.cc` | `JOIN::extract_const_tables` | 按行数与统计精确性把 0/1 行表摘成 const 表 |
| `sql/sql_optimizer.cc` | `JOIN::extract_func_dependent_tables` | 按函数依赖摘 const 表，可多轮回跳 |
| `sql/sql_optimizer.cc` | `JOIN::mark_const_table` | 把 const 表写进 `positions` 并挪到 `best_ref` 的 const 区，设定 `JT_CONST` / `JT_SYSTEM` |
| `sql/sql_optimizer.cc` | `JOIN::update_sargable_from_const` | 用刚读出的 const 表值生成新的 sargable 谓词 |
| `sql/sql_optimizer.cc` | `JOIN::estimate_rowcount` | 逐表估行数、算扫描成本与最坏寻道成本，触发范围分析 |
| `sql/sql_optimizer.cc` | `find_worst_seeks` | 最坏寻道成本封顶：`min(worst_seek_times(num_rows/10), table_scan_cost * 3)` |
| `sql/sql_optimizer.cc` | `update_ref_and_keys` | 从 WHERE 条件收集 `Key_use` 与 `SARGABLE_PARAM` |
| `sql/sql_optimizer.cc` | `pull_out_semijoin_tables` | 按 `eq_ref` 候选把半连接 nest 的内表拉出到外层 |
| `sql/sql_optimizer.cc` | `find_eq_ref_candidate` | 判定某张表是否有 `eq_ref(outer_tables)` 候选 |
| `sql/sql_optimizer.cc` | `JOIN::set_semijoin_embedding` | 给每个 `JOIN_TAB` 标注它所属的半连接 nest |
| `sql/sql_optimizer.cc` | `optimize_semijoin_nests_for_materialization` | 为每个可物化 nest 单独跑一遍连接顺序搜索并存下局部计划 |
| `sql/sql_optimizer.cc` | `semijoin_types_allow_materialization` | 判定半连接比较类型是否允许物化策略 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_keyuse` | 给每个 `Key_use` 填 `ref_table_rows`，下限 100 |
| `sql/sql_optimizer.cc` | `JOIN::get_best_combination` | 把 `best_positions` 展开成 `best_ref` 的 `JOIN_TAB` 序列，插入临时表槽位 |
| `sql/sql_optimizer.cc` | `JOIN::refine_best_rowcount` | 用 LIMIT 与派生表语义修正最终行数估计 |
| `sql/sql_optimizer.cc` | `JOIN::decide_subquery_strategy` | 在 IN-to-EXISTS 与子查询物化之间定案 |
| `sql/sql_optimizer.cc` | `JOIN::compare_costs_of_subquery_strategies` | 比较 EXISTS 与物化两种子查询策略的代价 |
| `sql/sql_optimizer.cc` | `JOIN::optimize` | `make_join_plan()` 的调用方；成功后检查 `zero_result_cause` |
| `sql/sql_optimizer.cc` | `JOIN::set_prefix_tables` | 连接顺序定案后给各表分配可用前缀表集合 |
| `sql/sql_executor.cc` | `join_read_const_table` | 计划期读取 const 表内容，返回 0 / -1 / 1 三态 |
| `sql/sql_planner.cc` | `Optimize_table_order::choose_table_order` | 连接顺序搜索入口：预处理排序后走贪心或直连路径 |
| `sql/sql_planner.cc` | `Optimize_table_order::determine_search_depth` | 搜索深度决策：常量 `max_tables_for_exhaustive_opt = 7` |
| `sql/sql_planner.cc` | `Optimize_table_order::greedy_search` | 贪心搜索主循环，逐轮把最有希望的表推进 `best_ref` |
| `sql/sql_planner.cc` | `Optimize_table_order::best_extension_by_limited_search` | 有限深度搜索树展开，含依赖过滤与代价剪枝 |
| `sql/sql_planner.cc` | `Optimize_table_order::best_access_path` | 为单表选访问路径：ref / range / index / ALL |
| `sql/sql_planner.cc` | `Optimize_table_order::find_best_ref` | 在 `Key_use` 列表里找代价最低的 ref 访问 |
| `sql/sql_planner.cc` | `Optimize_table_order::calculate_scan_cost` | 扫描代价计算，含「过滤掉 25% 行」的启发式 |
| `sql/sql_planner.cc` | `Optimize_table_order::fix_semijoin_strategies` | 半连接策略定案：物化 / FirstMatch / LooseScan / DupsWeedout |
| `sql/sql_planner.cc` | `Optimize_table_order::check_interleaving_with_nj` | 判定新表能否安全嵌入当前嵌套连接前缀 |
| `sql/sql_planner.cc` | `Optimize_table_order::advance_sj_state` | 推进半连接状态机 |
| `sql/sql_planner.cc` | `get_partial_join_cost` | 部分连接顺序的代价与行数计算（前缀代价累加公式） |
| `sql/sql_planner.cc` | `Optimize_table_order::optimize_straight_join` | `SELECT_STRAIGHT_JOIN` 下的不重排搜索路径 |
| `sql/sql_planner.cc` | `Optimize_table_order::lateral_derived_cost` | lateral 派生表的额外代价 |

## 与主核 / AI 的关系

主核（`engine/db`）目前没有多表连接计划这一层，`engine/db` 的执行器是单表扫描加过滤。

**架构推论：** MySQL 这套「连接顺序 + 访问路径」有三点结构值得主核借鉴。其一是**「表依赖」与「代价」被分成两套正交的约束**：依赖用 `table_map` 位图表达（`tab->dependent`、`remaining_tables & s->dependent`），代价用 `POSITION::prefix_cost` 表达，两者互不干扰；主核若把「必须先读哪张表」直接编码进枚举顺序，就无法在加表时保持正确性。其二是**「搜索空间被常量夹住」的做法**：`max_tables_for_exhaustive_opt = 7` 这样一个写死的常量就把「精确解」与「近似解」的分界划清楚了，而不是试图做一个自适应但难以验证的启发式。其三是**「代价剪枝建立在排序不变式上」**：`best_ref` 按记录数预排序后，搜索里用无条件 `std::swap` 维护这个顺序，源码注释称之为「critical for early pruning of bad plans」——主核若要加连接顺序搜索，这个不变式必须先想清楚，否则剪枝会漏。

对 AI 场景（模型生成 SQL 并由数据库执行）而言，这一层决定的是「同样的 SQL，模型写的连接顺序会不会被优化器救回来」。MySQL 的做法暴露了一个现实：**优化器的连接顺序优化对模型不可见**。模型写出 `FROM a JOIN b ON a.id = b.aid`，优化器可以把它改成先读 `b`；但如果模型写的是笛卡尔积再过滤，优化器只能靠 `extract_const_tables()` 与 `estimate_rowcount()` 兜。**架构推论：** 主核若要让 AI 参与计划决策（而不只是生成 SQL 文本），就必须把 `POSITION` 这类「位置 × 路径 × 代价」的中间表示暴露成可查询的结构——MySQL 内部通过 `Optimizer Trace` 的 `considered_execution_plans` 数组暴露了它（`sql/sql_planner.cc` 的 `choose_table_order()` 里建的 `Opt_trace_array trace_plan(&join->thd->opt_trace, "considered_execution_plans", Opt_trace_context::GREEDY_SEARCH);`），这个「把搜索过程本身当作一等可观测对象」的做法，对 AI 场景比对用户暴露 EXPLAIN 更有价值。

## 参考

- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_optimizer.cc)
- [MySQL 源码：`sql/sql_planner.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_planner.cc)
- [MySQL 源码：`sql/sql_executor.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_executor.cc)
- [MySQL 参考手册：JOIN 与连接优化](https://dev.mysql.com/doc/refman/8.0/en/join.html)
- [MySQL 参考手册：Block Nested-Loop 与 Join Buffer 连接优化](https://dev.mysql.com/doc/refman/8.0/en/optimization-index-joins.html)
- [MySQL 参考手册：优化器开关（optimizer_switch）](https://dev.mysql.com/doc/refman/8.0/en/optimizer-switch.html)
- [MySQL 参考手册：EXPLAIN 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html)
- [MySQL 参考手册：半连接（semijoin）策略](https://dev.mysql.com/doc/refman/8.0/en/semijoin.html)
