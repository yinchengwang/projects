---
project: mysql-server
feature: join-optimize
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_optimizer.cc
    symbols: [JOIN::optimize]
---

# JOIN 的整体优化

## 一句话职责

`JOIN::optimize()` 是 MySQL 传统优化器对**一个查询块**做整体优化的总编排入口：它从收集 WHERE/HAVING 条件开始，依次做条件化简与常量判定、聚合短路、分区裁剪，再把连接顺序与访问路径交给 `JOIN::make_join_plan()` 决定，最后把计划落成 `AccessPath` 树并下推给存储引擎；当查询块被判定为空结果、全部表都是 const、或被 `subselect_indexsubquery_engine` 接管时，它走各自的短路出口提前返回。

## 机制是什么

**它是「一次查询块的优化」这一层，不是一个独立模块。** `JOIN::optimize()` 定义在 `sql/sql_optimizer.cc`，它自身不含任何代价模型实现，只做**顺序编排 + 短路判定 + 计划落地**三件事：

1. **顺序编排**——按固定次序调用各个子步骤：`optimize_cond()` 化简条件、`optimize_aggregated_query()` 尝试聚合短路、`make_join_plan()` 决定连接顺序与访问路径、`optimize_distinct_group_order()` 处理 DISTINCT/GROUP BY/ORDER BY、`make_join_readinfo()` 落地读信息、`create_access_paths()` 生成执行计划。
2. **短路判定**——在正式规划前识别出「不需要规划」的形态：LIMIT 为 0、条件恒假、聚合可完全在优化期求值、const 表为空。此时不生成连接计划，只生成一个「零行」访问路径。
3. **计划落地**——把规划结果转成 `AccessPath` 树，写入 `m_root_access_path`，并把计划状态置为 `PLAN_READY`（或空结果时的 `ZERO_RESULT`）后暴露给其他连接（EXPLAIN）。

**它和 Hypergraph 优化器是两条互斥的路径。** `JOIN::optimize()` 开头用 `thd->lex->using_hypergraph_optimizer()` 判断走哪条：走 Hypergraph 时，本函数只做前置准备（条件收集、派生表优化、`remove_in2exists_conds()` 拆分条件），真正的计划生成交给 `sql/join_optimizer/join_optimizer.cc` 的 `FindBestQueryPlan()`；走传统优化器时，本函数后半段执行从 `make_join_plan()` 到 `push_to_engines()` 的全部逻辑。源码在传统路径入口处留了明确断言 `assert(!thd->lex->using_hypergraph_optimizer())`，并注释说明「The hypergraph optimizer does not do const tables, nor does it evaluate subqueries during optimization」。

**「短路出口」由一个 C++ label 实现。** 函数体内有 `setup_subq_exit:` 标签，凡是判定为空结果的分支都 `goto setup_subq_exit`，在那里统一收尾：置 `child_subquery_can_materialize = true`、在 optimizer trace 里写 `"empty_result"` 与 `zero_result_cause`、必要时补建 QEP_TAB、把计划状态置为 `ZERO_RESULT`。

**代价估算不在本函数里。** 连接顺序的搜索在 `sql/sql_planner.cc` 的 `Optimize_table_order::choose_table_order()`，单表访问路径与行数估算在 `JOIN::estimate_rowcount()` 等函数里。

## 怎么实现的

### 入口防护与前置准备

`JOIN::optimize(bool finalize_access_paths)` 一开始做三件事：

- **幂等防护。** `if (optimized) return false;` —— 源码注释写明「to prevent double initialization on EXPLAIN」。这保证 EXPLAIN 与实际执行共用同一个 `JOIN` 对象时优化不会跑第二遍。
- **状态断言。** `assert(query_block->leaf_table_count == 0 || thd->lex->is_query_tables_locked() || query_block == set_operand_block);` 与 `assert(tables == 0 && primary_tables == 0 && tables_list == (Table_ref *)1);`，保证进入时 JOIN 还是「干净」的。
- **阶段标记与 trace。** `THD_STAGE_INFO(thd, stage_optimizing);` 设置优化阶段标记；trace 侧建立 `Opt_trace_object trace_optimize(trace, "join_optimization")` 与 `Opt_trace_array trace_steps(trace, "steps")`，所以 EXPLAIN 的 optimizer trace 里能直接看到 `join_optimization` 这一节。

接着按序做：窗口函数准备（`Window::setup_windows2()`）、ROLLUP 处理（`optimize_rollup()`）、聚合函数分配（`alloc_func_list()`）、可优化条件收集（`query_block->get_optimizable_conditions(thd, &where_cond, &having_cond)`）、派生切片分配（`alloc_indirection_slices()`）、并把查询块的基础引用项接管过来（`ref_items[REF_SLICE_ACTIVE] = query_block->base_ref_items;`）。

### 派生表与表函数的递归优化

遍历 `query_block->leaf_tables`：

- **派生表/视图。** `if (tl->is_view_or_derived()) { if (tl->optimize_derived(thd)) return true; }` —— 派生表本体在这里被递归优化，源码注释说这一轮覆盖「所有派生表和视图，包括半连接里的」。
- **表函数。** 若表函数没有存储引擎句柄，走 `setup_tmp_table_handler(thd, table, query_block->active_options() | TMP_TABLE_ALL_COLUMNS)` 建临时表句柄；随后 `table->file->stats.records = 2;` —— 这是一个**写死的行数估计**，源码没有解释 2 的来源，但 `sql/table.cc` 里 `Table_ref::fetch_number_of_rows()` 的注释提到旧优化器在拿不到行数时用 `PLACEHOLDER_TABLE_ROW_ESTIMATE`，其值是 2。

### LIMIT 与 FOUND_ROWS 的处理

```cpp
row_limit = ((select_distinct || !order.empty() || !group_list.empty())
                 ? HA_POS_ERROR
                 : query_expression()->select_limit_cnt);
m_select_limit = query_expression()->select_limit_cnt;
```

`HA_POS_ERROR` 是「无上限」的哨兵值：只要查询带 DISTINCT、ORDER BY 或 GROUP BY，`row_limit` 就退化成「不限制」，因为排序/去重需要读完全部行。

`calc_found_rows` 的计算条件是 `m_select_limit != HA_POS_ERROR && (!query_expression()->is_set_operation() || query_block == set_operand_block)`，且外层 `active_options()` 必须带 `OPTION_FOUND_ROWS`。源码注释解释：只有最外层、且 LIMIT 作用于最终 UNION 结果的那个块才需要继续计数。

### 条件的化简与常量判定

WHERE 与 HAVING 分别过一遍 `optimize_cond()`：

```cpp
if (where_cond || query_block->outer_join) {
  if (optimize_cond(thd, &where_cond, &cond_equal, &query_block->m_table_nest,
                    &query_block->cond_value)) { error = 1; return true; }
  if (query_block->cond_value == Item::COND_FALSE) { ... }
}
```

`optimize_cond()` 内部按三步改写条件：先 `build_equal_items()` 建立多重等值（仅对 WHERE / JOIN 条件，HAVING 不做）；再 `propagate_cond_constants()` 把「字段 = 常量」化简成常量；最后 `remove_eq_conds()` 删掉 `item == item` 这类恒真条件并给出 `cond_value`（`COND_OK` / `COND_TRUE` / `COND_FALSE`）。

**恒假条件直接短路。** `query_block->cond_value == Item::COND_FALSE` 时设 `zero_result_cause = "Impossible WHERE"`，`best_rowcount = 0`，调 `create_access_paths_for_zero_rows()` 后跳到 `setup_subq_exit`。HAVING 侧对应 `"Impossible HAVING"`。

### 聚合短路：`optimize_aggregated_query`

当 `tables_list` 非空、`implicit_grouping` 成立且没有 `OPTION_NO_CONST_TABLES` 时，调 `sql/opt_sum.cc` 的 `optimize_aggregated_query()`。它返回四态（枚举 `aggregate_evaluated`）：

| 返回值 | 含义 | `JOIN::optimize()` 的处理 |
|---|---|---|
| `AGGR_REGULAR` | 没能整体求值 | 回退到常规优化 |
| `AGGR_DELAYED` | 需执行期用 `ha_records()` 求行数 | 回退到常规优化，但置 `select_count = true` |
| `AGGR_COMPLETE` | 所有 SELECT 表达式已完全求值 | `tables_list = nullptr`、`best_rowcount = 1`，构造 `NewFakeSingleRowAccessPath` 后跳到 `setup_subq_exit` |
| `AGGR_EMPTY` | 源表为空 | `zero_result_cause = "No matching min/max row"`，`create_access_paths_for_zero_rows()` 后跳到 `setup_subq_exit` |

`AGGR_COMPLETE` 分支里有一段关键逻辑：它断言「WHERE 中已无表无关条件」，然后在非 EXPLAIN 时把 `where_cond` 置空（源码注释：「There are no relevant conditions left from the WHERE」），EXPLAIN 时则保留条件以便打印。

`optimize_aggregated_query()` 内部还有两处硬性限制：WHERE 条件若涉及 `OUTER_REF_TABLE_BIT` 或 `RAND_TABLE_BIT` 就直接放弃；查询块存在半连接嵌套（`!select->sj_nests.empty()`）也直接放弃，因为半连接的去重语义无法在优化期静态求值。

### 分区裁剪

`if (query_block->partitioned_table_count && prune_table_partitions())` —— `JOIN::prune_table_partitions()` 对每张分区表找到可用于裁剪的最外层谓词（沿 `Table_ref::embedding` 链向上找 `join_cond_optim()`），再调 `prune_partitions(thd, tbl->table, query_block, cond)`。

### 「没有表」与错误状态位的切换

`tables_list == nullptr` 时（表全被优化掉）：`best_rowcount = 1`、`error = 0`、`make_tmp_tables_info()`、再调一次 `count_field_types()`、`set_plan_state(NO_TABLES)`、`create_access_paths()`，然后 `return false`。

进入正式规划前，源码把 `error = -1;`，注释是「Error is sent to client」——`-1` 表示错误尚未上报给客户端，后续各子函数出错时由这一层统一上报。

### `make_join_plan()`：连接顺序与访问路径

这是 `JOIN::optimize()` 里最重的一步，源码注释写「Set up join order and initial access paths」，进入前会设 `THD_STAGE_INFO(thd, stage_statistics);`。

`JOIN::make_join_plan()` 内部顺序（`sql/sql_optimizer.cc`）：

1. `init_planner_arrays()` —— 分配 `join_tab` / `best_ref` / `map2table` / `positions` / `best_positions` 五组数组。`best_ref` 的容量是 `table_count + sj_nests + 2 + m_windows.elements`，源码注释解释多出的 2 个槽位：规划阶段用 0 终止需要 +1，`get_best_combination()` 之后改用非 0 终止，需要 +2 来容纳最多 2 个排序/分组/去重临时表。
2. `propagate_dependencies()` —— 外连接依赖传播。递归 CTE 的非法连接顺序在这里被拦下，报 `ER_CTE_RECURSIVE_FORBIDDEN_JOIN_ORDER`。
3. `update_ref_and_keys()` —— 构建 ref 访问所需的关键字使用信息。
4. `pull_out_semijoin_tables()` —— 按依赖把半连接表拉平（只做一次，靠 `query_block->sj_pullout_done` 标记）。
5. `extract_const_tables()` / `extract_func_dependent_tables()` —— 识别 0/1 行的 const 表与函数依赖表。
6. `estimate_rowcount()` —— 每张表的行数与读代价初估。
7. `apply_join_order_hints()` —— 应用连接顺序提示（`JOIN_FIXED_ORDER` 与 `STRAIGHT_JOIN` 除外）。
8. `set_semijoin_embedding()` / `update_semijoin_strategies()` —— 半连接策略。
9. `optimize_keyuse()` —— 为每个 `Key_use` 估算 `ref_table_rows`。源码里有一处写死的下限：`keyuse->ref_table_rows = max<ha_rows>(tmp_table->file->stats.records, 100);`，注释解释为「To avoid bad matches, we don't make ref_table_rows less than 100」。
10. `optimize_semijoin_nests_for_materialization()` —— 物化型半连接的代价优化。
11. `Optimize_table_order(thd, this, nullptr).choose_table_order()` —— 真正的连接顺序搜索。
12. `decide_subquery_strategy()` —— 子查询在 IN-to-EXISTS 与物化之间的取舍。
13. `refine_best_rowcount()` —— 用 LIMIT 修正行数估计：`best_rowcount = std::min(best_rowcount, query_expression()->select_limit_cnt);`；若块本身是派生表且估计 ≤ 1 行，则抬到 `PLACEHOLDER_TABLE_ROW_ESTIMATE`（`sql/table.cc` 的注释说明其值为 2）。
14. `get_best_combination()` —— 把搜索结果固化成完整的执行计划骨架。

**`choose_table_order()` 的搜索深度是一个写死的常量。** `sql/sql_planner.cc` 的 `Optimize_table_order::determine_search_depth()` 里 `const uint max_tables_for_exhaustive_opt = 7;`：参与优化的表数 ≤ 7 时 `search_depth = table_count + 1`（穷举），> 7 时 `search_depth = max_tables_for_exhaustive_opt`（贪心）。源码注释承认这是「an extremely simplistic implementation that serves as a stub for a more advanced analysis」，并留了两条 TODO。`SELECT_STRAIGHT_JOIN` 时不走贪心，而是 `optimize_straight_join()` 按固定顺序选访问路径。

**`make_join_plan()` 会自行判定空结果。** 末尾 `if (const_table_map != found_const_table_map) zero_result_cause = "no matching row in const table";`，这个 `zero_result_cause` 回到 `JOIN::optimize()` 里被检查并触发 `create_access_paths_for_zero_rows()`。

### 计划落地前的条件处理

`make_join_plan()` 之后，`JOIN::optimize()` 依次做：

- **const 表解锁。** `mysql_unlock_some_tables(thd, ct, const_tables)`（在 `!thd->locked_tables_mode` 且没有 `SELECT_NO_UNLOCK` 时）。
- **等值传播。** `substitute_for_best_equal_field(thd, where_cond, cond_equal, map2table)`，以及对每个有位置的非 const 表的 join 条件做同样处理（`tab->set_join_cond(substitute_for_best_equal_field(...))`）。
- **ref 访问初始化。** `init_ref_access()`（实现在 `sql/sql_select.cc`）与 `update_depend_map()`。
- **条件挂表。** `make_join_query_block(this, where_cond)`（静态函数，在 `sql/sql_optimizer.cc`）：先 `add_not_null_conds(join)` 补 IS NOT NULL 条件，再用 `make_cond_for_table()` 把只依赖 const 表的条件抽出来求值——若该条件求值为假，直接返回 `true` 并让 `JOIN::optimize()` 设 `zero_result_cause = "Impossible WHERE noticed after reading const tables"`；剩余条件按依赖的表挂到对应的 `JOIN_TAB` 上。
- **类型兼容处理。** 对 WHERE、HAVING、SELECT 列表、GROUP BY 列表分别 `walk(&Item::cast_incompatible_args, enum_walk::POSTFIX, nullptr)` 注入必要的 CAST 节点。

### 特殊路径：`replace_index_subquery`

`JOIN::replace_index_subquery()` 尝试把 `IN (SELECT ...)` 换成 `subselect_indexsubquery_engine`。它的前置条件很严格：无 GROUP BY、子查询类型必须是 `Item_subselect::IN_SUBS`、`primary_tables == 1`、有 WHERE、不是集合运算。命中引擎的条件是：首表的访问类型为 `JT_EQ_REF` 或 `JT_REF`（且 ref 的字段名等于 `in_left_expr_name`），或者 `JT_REF_OR_NULL` 且 HAVING 由 IN-to-EXISTS 转换而来。

命中后：`finalize_table_conditions(thd)` → `alloc_qep(tables)` → `unplug_join_tabs()` → 构造 `subselect_indexsubquery_engine` → `set_plan_state(PLAN_READY)` → **直接 `return false`**，源码注释解释：「We leave optimize() because the rest of it is only about order/group which those subqueries don't have and about setting up plan which we're not going to use due to different execution method.」

### Join Buffering 与临时表决策

**`setup_join_buffering()`** 为每张表决定是否用 join buffer。关键常量与判定：

- `uint bufsz = 4096;` —— 缓冲区初始大小。
- `if (tab->ref().null_rejecting == keypart_map) join_cache_flags |= HA_MRR_NO_NULL_ENDPOINTS;` —— 全部 key part 都是 null-rejecting 时才能承诺这个标志。
- 禁用条件一串：`no_jbuf_after` 限制（`JOIN::optimize()` 里初值 `UINT_MAX`，表示默认不禁用；一旦启用 `SELECT_NO_JOIN_CACHE`、存在 FT 函数、或首表有 `force_index_order` / `force_index_group` 提示，就置为 0）、`QS_DYNAMIC_RANGE`、外连接嵌套的首个内表与外层表都没用 join cache、依赖的表函数、半连接策略为 `SJ_OPT_LOOSE_SCAN`、物化型半连接的首表。
- BNL / BKA 由 `hint_table_state(join->thd, tab->table_ref, BNL_HINT_ENUM, OPTIMIZER_SWITCH_BNL)` 与 `BKA_HINT_ENUM` 决定。

只要有表用了 join cache，`simple_order` 与 `simple_group` 就被置为 false——源码注释解释：join buffering 会改变行序，不能再依赖首个非 const 表的索引扫描有序性。

**`need_tmp_before_win` 的八条判定**（源码注释编号 (1)–(8)）：ROLLUP 存在且带 DISTINCT/窗口/ORDER BY；隐式分组且含非聚合的全文检索列（`contains_non_aggregated_fts()`）；GROUP BY 未被优化掉；DISTINCT 或 ORDER BY 未被索引/排序消解；ORDER BY 与 GROUP BY 顺序不同；用户要求 `OPTION_BUFFER_RESULT` 且没有窗口；第一个窗口步骤需要排序且参与连接的表数 `primary_tables - const_tables > 1`。

### 读信息与计划生成

- `alloc_qep(tables)` —— 分配 `QEP_TAB` 数组，容量 `n + 1`，最后一个槽位只放最终 `op_type`。
- `test_skip_sort()` —— 尝试用索引免排序。
- `finalize_table_conditions(thd)` —— 收敛每张表的最终条件，并用 `Item::cache_const_expr_transformer` 缓存常量表达式。
- `make_join_readinfo(this, no_jbuf_after)`（实现在 `sql/sql_select.cc`）—— 落地每张表的读方式：按 `JT_EQ_REF` / `JT_REF_OR_NULL` / `JT_REF` / `JT_SYSTEM` / `JT_CONST` / `JT_ALL` / `JT_INDEX_SCAN` / `JT_RANGE` / `JT_INDEX_MERGE` 分派，决定是否 `set_keyread(true)`、是否 `push_index_cond()`、并更新状态计数器（如 `inc_status_select_full_join()`）。
- `make_tmp_tables_info()` —— 决定是否需要临时表。
- **代价回填。** `if (sort_cost > 0.0 && !explain_flags.any(ESP_USING_FILESORT, ESC_WINDOWING)) { best_read -= sort_cost; sort_cost = 0.0; }` —— 最终决定不用排序时，把之前计入的排序代价扣回来。

### `create_access_paths()` 与 `push_to_engines()`

`create_access_paths()` 实现在 `sql/sql_executor.cc`，它做三步：建根路径（`create_root_access_path_for_join()`）、挂 HAVING 与 LIMIT（`attach_access_paths_for_having_and_limit()`）、挂 UPDATE/DELETE（`attach_access_path_for_update_or_delete()`）。根路径的构造分三种：`select_count` 为真时返回 `NewUnqualifiedCountAccessPath()`；全部是 const 表时返回 `NewFakeSingleRowAccessPath()`（并按需再挂一层 `NewMaterializeAccessPath`）；否则调 `ConnectJoins()` 把 `qep_tab` 串成连接树，剩下的重复行去重交给 `NewWeedoutAccessPathForTables()`。

`push_to_engines()` 在 `sql/sql_optimizer.cc`：遍历 `query_block->leaf_tables`，找 `tl->table->file->hton_supporting_engine_pushdown()`，若非空则调 `hton->push_to_engine(thd, m_root_access_path, this)`，并 `break`（源码注释：「Assume that at most a single handlerton per query support pushdown」）。源码注释明确要求：迭代器必须在 `push_to_engines()` 之后才能创建，因为下推会修改 `AccessPath` 本身。

最后 `set_plan_state(PLAN_READY);` 把计划暴露出去，`error = 0; return false;`。

### Hypergraph 路径的差异

走 Hypergraph 时，`JOIN::optimize()` 的收尾完全不同：

- 前置阶段会先给所有内层查询表达式调 `unit->optimize(thd, nullptr, /*create_iterators=*/false, /*finalize_access_paths=*/false)`，并给当前块加上 `OPTION_NO_CONST_TABLES | OPTION_NO_SUBQUERY_DURING_OPTIMIZATION`。
- `remove_in2exists_conds(where_cond)` 与 `remove_in2exists_conds(having_cond)` 分别取出「带 IN-to-EXISTS 谓词」与「去掉这些谓词」两版条件。
- `m_root_access_path = FindBestQueryPlan(thd, query_block, trace_ptr);`，随后若 `finalize_access_paths` 为真则调 `FinalizePlanForQueryBlock(thd, query_block)`。
- 若两版条件不同（说明发生过 IN-to-EXISTS 转换），再规划一次得到 `m_root_access_path_no_in2exists`，供外层块在物化更划算时通过 `JOIN::change_to_access_path_without_in2exists()` 切换。源码注释解释了这个二次规划的代价与理由：「there are so many places such filters could be added ... that trying to plan paths both with and without them incurs complexity that is not justified by the small computational gain it would bring」。
- Hypergraph 路径**不做** const 表优化、**不**在优化期求值子查询，`sql/join_optimizer/join_optimizer.cc` 的 `FindBestQueryPlan()` 里对此有对应断言。

## 关键取舍

**「短路优先」于「统一规划」。** `JOIN::optimize()` 为 LIMIT 0、恒假条件、聚合可完全求值、const 表为空这些形态各设了专用出口，跳过完整的连接顺序搜索。代价是优化期要维护多套收尾逻辑（`zero_result_cause` 的多个取值、`create_access_paths_for_zero_rows()` 与 `NewFakeSingleRowAccessPath()` 两条不同的零行路径）；收益是这些形态的优化开销接近零。

**「零行」有两种语义。** `create_access_paths_for_zero_rows()` 按 `send_row_on_empty_set()` 分叉：真时用 `NewZeroRowsAggregatedAccessPath()`（空结果集也要聚合出一行），假时用 `NewZeroRowsAccessPath()`（连行都不发，也就不必检查 HAVING 与 LIMIT）。这是「无 GROUP BY 的空集返回零行」与「有聚合的空集返回一行 NULL」两种 SQL 语义的分叉点。

**「EXPLAIN 保留更多信息」。** `AGGR_COMPLETE` 分支在 EXPLAIN 下不清空 `where_cond`，只对真实执行清空；同理 `create_access_paths()` 前的 `set_plan_state()` 才是「计划终态」的分界点（源码注释：「starting from this call, we expose the plan to other connections (via EXPLAIN CONNECTION) so the plan has to be final」）。代价是同一条优化路径要照顾两种输出目标。

**「传统优化器与 Hypergraph 优化器并存，用一个布尔开关分流」。** `JOIN::optimize()` 前半段是两者共用的准备阶段，后半段完全分叉，并且传统路径入口处有 `assert(!thd->lex->using_hypergraph_optimizer())` 把关。代价是同一文件里存在两套「计划生成」逻辑，维护成本翻倍；收益是可以通过 `optimizer_switch` 在线切换而不必重编译。源码注释还记录了一条约束：预处理语句和存储程序使用**准备时**生效的优化器，每次执行不再检查 `optimizer_switch`。

**「连接顺序搜索深度写死为 7」。** `Optimize_table_order::determine_search_depth()` 里 `const uint max_tables_for_exhaustive_opt = 7;` 是一个明确的常量，源码自己标注为「extremely simplistic implementation that serves as a stub for a more advanced analysis」，并留了「应该基于统计动态决定」的 TODO。代价是超过 7 张表时搜索不再穷举；收益是优化耗时有确定上界。

**「代价模型可插拔，但计划形态固定」。** `JOIN::cost_model()` 返回 `const Cost_model_server *`，`sql/opt_costconstants.cc` 里 `Server_cost_constants::ROW_EVALUATE_COST = 0.1`、`KEY_COMPARE_COST = 0.05` 等常量可被用户覆盖；但 `AccessPath` 的树形结构（`ConnectJoins()` 产出的嵌套）不随代价模型变化。

**「IN 子查询优先尝试专用引擎」。** `replace_index_subquery()` 的前置条件写得很死（必须单表、必须是 `IN_SUBS`、必须能走 `JT_EQ_REF`/`JT_REF`），一旦命中就直接 `return false`，跳过 DISTINCT/GROUP BY/ORDER BY 的全部处理——源码注释说明理由是「那些子查询没有 order/group，而且计划不会被用到」。代价是不满足条件的 `IN (SELECT ...)` 拿不到这条路径；收益是命中时省掉整套规划。

**「条件简化和计划生成分离成两趟」。** 条件先经 `optimize_cond()` 化简并挂到表上（`make_join_query_block()`），再在 `finalize_table_conditions()` 里按选定访问方式收敛（`reduce_cond_for_table()` 删掉访问方法已保证为真的谓词）。代价是同一个谓词会被处理两次；收益是每一步的输入输出都更小。

**「下推给存储引擎放在最后」。** `push_to_engines()` 在 `create_access_paths()` 之后、`set_plan_state(PLAN_READY)` 之前执行。源码注释说明理由：下推会修改 `AccessPath`（移除或改写 FILTER、改 JOIN 算法、改聚合表达式），所以迭代器必须在它之后才建。代价是下推无法影响迭代器形状；收益是执行器与存储引擎的边界清晰。

**「一次优化产出两个计划」。** Hypergraph 路径为 IN-to-EXISTS 的取舍预留了第二棵计划树 `m_root_access_path_no_in2exists`，由外层块在物化更划算时通过 `change_to_access_path_without_in2exists()` 切换。源码注释明确承认这是「somewhat wasteful」，理由是同时规划两条路径会把代码搅乱。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_optimizer.cc` | `JOIN::optimize` | 优化总编排：条件收集、聚合短路、分区裁剪、计划落地、下推存储引擎，并管理全部短路出口 |
| `sql/sql_optimizer.cc` | `JOIN::make_join_plan` | 连接顺序与访问路径的确定：依赖传播、const 表提取、行数初估、提示应用、半连接策略 |
| `sql/sql_optimizer.cc` | `JOIN::init_planner_arrays` | 分配 `join_tab` / `best_ref` / `map2table` / `positions` / `best_positions` 五组规划数组 |
| `sql/sql_optimizer.cc` | `optimize_cond` | 条件化简三步：多重等值建立、常量传播、恒真条件删除 |
| `sql/sql_optimizer.cc` | `JOIN::estimate_rowcount` | 每表行数与读代价初估，并触发范围分析 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_keyuse` | 为每个 `Key_use` 设定 `ref_table_rows`，下限写死为 100 |
| `sql/sql_optimizer.cc` | `JOIN::refine_best_rowcount` | 用 LIMIT 修正行数估计；派生表场景抬到 `PLACEHOLDER_TABLE_ROW_ESTIMATE` |
| `sql/sql_optimizer.cc` | `JOIN::prune_table_partitions` | 分区裁剪：找最外层可用谓词后调 `prune_partitions()` |
| `sql/sql_optimizer.cc` | `make_join_query_block` | 抽取 const 表相关条件求值，并把剩余条件挂到各表的 `JOIN_TAB` 上 |
| `sql/sql_optimizer.cc` | `JOIN::finalize_table_conditions` | 按选定访问方式收敛表条件，并缓存常量表达式 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_distinct_group_order` | DISTINCT / GROUP BY / ORDER BY 的消除与简化 |
| `sql/sql_optimizer.cc` | `JOIN::replace_index_subquery` | 判定 `IN (SELECT ...)` 能否改用 `subselect_indexsubquery_engine` |
| `sql/sql_optimizer.cc` | `setup_join_buffering` | 决定每张表是否使用 join buffer，含 `bufsz = 4096` 与 `HA_MRR_NO_NULL_ENDPOINTS` 判定 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_fts_query` | 全文检索提示设置（`FT_SORTED` / `FT_NO_RANKING`）与 FT 初始化 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_rollup` | 置 `rollup_state = RollupState::INITED` 并关闭 `allow_group_via_temp_table` |
| `sql/sql_optimizer.cc` | `JOIN::get_best_combination` | 把搜索结果固化为完整计划骨架，含临时表槽位数计算 |
| `sql/sql_optimizer.cc` | `JOIN::alloc_qep` | 分配 `QEP_TAB` 数组，容量为表数 +1 |
| `sql/sql_optimizer.cc` | `JOIN::set_plan_state` | 计划状态终态标记（`NO_TABLES` / `PLAN_READY` / `ZERO_RESULT`），并置 `keyread_optim` |
| `sql/sql_optimizer.cc` | `JOIN::push_to_engines` | 通过 `hton_supporting_engine_pushdown()` 把计划下推给存储引擎 |
| `sql/sql_optimizer.cc` | `JOIN::create_access_paths_for_zero_rows` | 空结果的两条路径：有聚合发一行 / 无聚合不发行 |
| `sql/sql_optimizer.cc` | `JOIN::change_to_access_path_without_in2exists` | 外层块在物化更划算时切换到去掉 IN-to-EXISTS 谓词的第二棵计划 |
| `sql/sql_planner.cc` | `Optimize_table_order::choose_table_order` | 连接顺序搜索入口：STRAIGHT_JOIN 走固定顺序，否则走贪心搜索 |
| `sql/sql_planner.cc` | `Optimize_table_order::determine_search_depth` | 搜索深度决策常量 `max_tables_for_exhaustive_opt = 7` |
| `sql/sql_planner.cc` | `Optimize_table_order::optimize_straight_join` | STRAIGHT_JOIN 下按固定顺序逐表选访问路径 |
| `sql/sql_select.cc` | `JOIN::init_ref_access` | ref 访问字段的初始化，被 `JOIN::optimize()` 在等值传播后调用 |
| `sql/sql_select.cc` | `JOIN::make_tmp_tables_info` | 临时表信息与临时表需求的最终决定 |
| `sql/sql_select.cc` | `make_join_readinfo` | 按 `JT_*` 访问类型落地读信息：covering index、index condition、状态计数 |
| `sql/sql_executor.cc` | `JOIN::create_access_paths` | 生成 `AccessPath` 树：根路径 + HAVING/LIMIT + UPDATE/DELETE |
| `sql/sql_executor.cc` | `JOIN::create_root_access_path_for_join` | 根路径三种形态：`NewUnqualifiedCountAccessPath` / `NewFakeSingleRowAccessPath` / `ConnectJoins` |
| `sql/opt_sum.cc` | `optimize_aggregated_query` | 聚合短路判定，返回 `AGGR_REGULAR` / `AGGR_DELAYED` / `AGGR_COMPLETE` / `AGGR_EMPTY` |
| `sql/join_optimizer/join_optimizer.cc` | `FindBestQueryPlan` | Hypergraph 优化器的计划生成入口，由 `JOIN::optimize()` 在 Hypergraph 路径下调 |
| `sql/opt_costconstants.cc` | `Server_cost_constants` | 可覆盖的代价常量，如 `ROW_EVALUATE_COST = 0.1`、`KEY_COMPARE_COST = 0.05` |

## 与主核 / AI 的关系

主核（`engine/db`）目前没有查询优化器这一层，SQL 走手写递归下降 + 固定执行策略，因此不存在 MySQL 这套「代价驱动的连接顺序搜索」。

**架构推论：** MySQL 的 `JOIN::optimize()` 有三点结构值得主核借鉴。其一是**「短路出口」用统一的 goto 标签收口**：`setup_subq_exit:` 把「LIMIT 为 0 / 恒假条件 / 聚合短路 / const 表为空」四类判定汇到同一段收尾逻辑，避免每条分支各写一遍收尾；主核若把这类判定散落在各处，很容易漏掉某个分支的临时资源清理。其二是**「优化期求值」被显式区分于「执行期求值」**：`optimize_aggregated_query()` 返回四态枚举，`AGGR_DELAYED` 专门表示「优化期算不出、留到执行期用 `ha_records()` 算」；主核若有类似短路需求，用状态枚举而不是布尔值能避免「优化期/执行期」两种语义被压进同一个 false/true。其三是**「条件处理分两趟」**：`optimize_cond()` 先做与访问方式无关的化简，`finalize_table_conditions()` 再做与访问方式相关的收敛；两趟各自输入更小，代价是同一谓词会被处理两次。

对 AI 场景（模型生成 SQL 后交给数据库执行）而言，这一层是「执行计划质量」的决定者，也是「慢查询归因」的关键。MySQL 这套机制里对 AI 最有参考价值的是**「优化结果可观测」**：`JOIN::optimize()` 全程往 `thd->opt_trace` 里写结构化 trace（`join_optimization` / `steps` / `condition_processing` / `equality_propagation` / `constant_propagation` / `trivial_condition_removal` / `considered_execution_plans` / `rows_estimation` / `empty_result` 等键），并且 `zero_result_cause` 用固定字符串标注了短路原因（如 `"Impossible WHERE"`、`"Zero limit"`、`"Select tables optimized away"`、`"no matching row in const table"`）。这意味着：当模型写出的 SQL 跑出空结果或走了意外计划时，可以从 trace 里定位到**是哪一步决策**导致的，而不必猜测。

**架构推论：** 对 AI 驱动的数据库，最值得补的不是更复杂的代价模型，而是「决策可解释」——即把「为什么选这个连接顺序」「为什么短路成空结果」这类信息以结构化形式保留下来。MySQL 用 `Opt_trace` + `zero_result_cause` 字符串常量实现了这一点，主核若要在 AI 场景下支持自动改写与自动重试，这套 trace 形态可以直接借鉴：键名固定、值为标量或结构化对象，便于程序解析而非仅供人读。

## 参考

- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_optimizer.cc)
- [MySQL 源码：`sql/sql_planner.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_planner.cc)
- [MySQL 源码：`sql/sql_select.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_select.cc)
- [MySQL 源码：`sql/sql_executor.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/sql_executor.cc)
- [MySQL 源码：`sql/opt_sum.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/opt_sum.cc)
- [MySQL 源码：`sql/join_optimizer/join_optimizer.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/join_optimizer/join_optimizer.cc)
- [MySQL 源码：`sql/opt_costconstants.cc`（GitHub `mysql-8.0.40` tag）](https://github.com/mysql/mysql-server/blob/mysql-8.0.40/sql/opt_costconstants.cc)
- [MySQL 参考手册：优化器与成本模型](https://dev.mysql.com/doc/refman/8.0/en/optimizer.html)
- [MySQL 参考手册：`optimizer_switch` 系统变量与优化器开关](https://dev.mysql.com/doc/refman/8.0/en/optimizer-switch.html)
- [MySQL 参考手册：JOIN 与连接顺序](https://dev.mysql.com/doc/refman/8.0/en/join.html)
- [MySQL 参考手册：EXPLAIN 输出的 `join_type` 与访问方式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html)
