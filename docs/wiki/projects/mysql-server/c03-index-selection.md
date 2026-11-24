---
project: mysql-server
feature: index-selection
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_optimizer.cc
    symbols: [JOIN::estimate_rowcount, get_best_group_min_max]
---

# 索引与范围访问选择

## 一句话职责

`JOIN::estimate_rowcount()` 与 `get_best_group_min_max()` 所在的这层，负责把「一张表有哪些候选索引、每条候选路径能读多少行、要花多少代价」算出来，再按代价从中挑出访问路径：具体表现为先按统计信息与覆盖索引估基线成本，再由 `test_quick_select()` 在 range、loose index scan（group min/max）、skip scan、index merge、covering index scan 之间比代价，胜者写回 `tab->range_scan()`，并由 `tab->set_type(calc_join_type(tab->range_scan()))` 决定最终的 `JT_*` 访问类型。

## 机制是什么

**这一层回答的问题是：给定一张表和一组条件，用哪条索引路径读最划算。**

MySQL 传统优化器里，一个表的读法最终被压成一个 `JOIN_TAB::type()`（`JT_SYSTEM` / `JT_CONST` / `JT_REF` / `JT_RANGE` / `JT_INDEX` / `JT_ALL` / `JT_INDEX_MERGE`）加一个可选的 `JOIN_TAB::range_scan()`（`AccessPath`）。选路径这件事分散在三层配合完成：

| 层 | 职责 | 关键符号 |
|---|---|---|
| 候选收集 | 从 WHERE 条件里抽出「哪些索引的哪些列可被等值/范围引用」 | `update_ref_and_keys()`、`add_key_part()`、`sort_keyuse()` |
| 候选扩充 | 把 GROUP BY / DISTINCT / skip scan 可用的索引补进候选集 | `add_loose_index_scan_and_skip_scan_keys()` |
| 代价竞争 | 在各条 range 类路径与基线扫描之间比代价并落地 | `test_quick_select()`、`get_best_group_min_max()` |

**「候选」有两个容器，含义不同。** `JOIN_TAB::const_keys` 存的是「索引的所有列都被常量或外部表列约束」的等值可用索引；`JOIN_TAB::skip_scan_keys` 存的是「可以拿来做 skip scan 前缀」的索引。二者在 `estimate_rowcount()` 里被合并成一个 `Key_map` 一起送进范围分析。

**基线成本先算，索引路径必须赢过它。** `test_quick_select()` 一进来就算出全表扫描的代价：`Cost_estimate cost_est = table->file->table_scan_cost(); cost_est.add_io(1.1); cost_est.add_cpu(scan_time);`，其中 `scan_time = cost_model->row_evaluate_cost(records) + 1`。随后所有索引路径的代价都要跟这个 `cost_est` 比。

**候选路径的种类是固定的一组，不是开放搜索。** 在 `test_quick_select()` 里被实际构造并参与竞争的有：

- **covering index scan**：`find_shortest_key(table, &table->covering_keys)` 选出最短的覆盖索引，用 `index_scan_cost(key_for_use, 1, records)` 加 `row_evaluate_cost(records)` 算代价。
- **group min/max（loose index scan）**：由 `get_best_group_min_max()` 构造。
- **skip scan**：由 `get_best_skip_scan()` 构造，受 `OPTIMIZER_SWITCH_SKIP_SCAN` 开关或 `SKIP_SCAN` 提示控制。
- **单索引 range scan 与索引交集**：`get_key_scans_params()` 与 `get_best_ror_intersect()`。
- **index merge union**：`get_best_disjunct_quick()`，遍历 `tree->merges` 里的每个 `SEL_IMERGE`。
- **index merge intersection**：`get_best_ror_intersect()`。

**`get_best_group_min_max()` 的定位是「没有可用 range tree 时也能构造的访问路径」。** 它在 `test_quick_select()` 里的调用位置很关键：先 `get_mm_tree()` 构造条件树，若树不是 `SEL_TREE::KEY` 类型（条件恒真或不能用于 range scan）则 `tree = nullptr`，但源码注释写明「If the tree can't be used for range scans, proceed anyway, as we can construct a group-min-max quick select」。随后无论有没有 `tree`，都调 `get_best_group_min_max(thd, &param, tree, interesting_order, skip_records_in_range, best_cost)`。也就是说 **group min/max 的候选生成不依赖 range tree 存在**。

**结果写回 `JOIN_TAB` 的两个字段。** 胜出的路径存进 `tab->set_range_scan(...)`（`get_quick_record_count()` 里是 `tab->set_range_scan(range_scan)`），行数存进 `tab->found_records`，读成本存进 `tab->read_time`。

## 怎么实现的

### 编排入口：`JOIN::estimate_rowcount()`

`sql/sql_optimizer.cc` 的 `JOIN::estimate_rowcount()` 在 `JOIN::make_join_plan()` 里被调用一次（源码注释写明「Make a first estimate of the fanout for each table in the query block」），返回值约定仍是 **`false` = 成功，`true` = 出错**。函数体逐表（`join_tab` 到 `join_tab + tables`）执行：

1. **const / system 表直接定案。** `tab->type() == JT_SYSTEM || tab->type() == JT_CONST` 时：`tab->set_records(tab->found_records = 1);`，`tab->worst_seeks = tab->table()->file->worst_seek_times(1.0);`，`tab->read_time = tab->worst_seeks;`，然后 `continue` 跳过后续分析。
2. **普通表先算基线。** `tab->set_records(tab->found_records = tab->table()->file->stats.records);`，`tab->read_time = tab->table()->file->table_scan_cost().total_cost();`，`tab->worst_seeks = find_worst_seeks(tab->table(), tab->found_records, tab->read_time);`。
3. **扩充候选索引。** 调 `add_loose_index_scan_and_skip_scan_keys(this, tab)`，把 GROUP BY / DISTINCT / skip scan 可用的索引并进 `const_keys` 与 `skip_scan_keys`。
4. **取条件。** 若 `tl->is_inner_table_of_outer_join()`，则沿 `t = tl; t != nullptr; t = t->embedding` 向上找第一个 `t->join_cond() != nullptr` 作为条件；否则直接用 `where_cond`。源码注释解释：内连接与半连接的连接条件取自 WHERE，而外连接的内表条件挂在嵌套 join nest 上。
5. **触发范围分析。** **仅当** `!tab->const_keys.is_clear_all() || !tab->skip_scan_keys.is_clear_all()` 时才调 `get_quick_record_count(thd, tab, row_limit, condition)`。源码注释写明这次调用「fills tab->range_scan() with the best range access method possible for this table, and only if it's better than table scan」。
6. **处理不可能条件。** `records == 0 && thd->is_error()` 时直接 `return true`（错误上抛）；`records == 0 && tab->table()->reginfo.impossible_range` 时置 `always_false_cond = true`。若 `records != HA_POS_ERROR`，则 `tab->found_records = records;`，`tab->read_time = tab->range_scan() ? tab->range_scan()->cost : 0.0;`。
7. **另一条恒假路径。** 即使没走范围分析，`tab->join_cond() != nullptr && tab->join_cond()->const_item() && tab->join_cond()->val_int() == 0` 也会置 `always_false_cond = true`。
8. **把恒假表标成 const。** 条件是 `always_false_cond && (!tl->is_inner_table_of_outer_join() || tl->embedding == nullptr) && (!(tl->embedding != nullptr && tl->embedding->is_sj_or_aj_nest()))`。此时调 `mark_const_table(tab, nullptr)` 并**强制覆写**类型：`tab->set_type(JT_CONST);`——源码注释说明这是「Override setting made in mark_const_table()」。若该表带 `join_cond()`（外连接场景），置 `found_const_table_map |= tl->map();` 并 `tab->table()->set_null_row();` 生成一条全 NULL 的空行；否则只在 trace 里记 `rows` 为 0 与 `cause` 为 `impossible_where_condition`。
9. **没做范围分析时补 trace。** `else if (!range_analysis_done)` 分支写一条名为 `table_scan` 的 trace，含 `rows` 与 `cost`。

### 范围分析入口：`get_quick_record_count()`

`sql/sql_optimizer.cc` 的 `static ha_rows get_quick_record_count(THD *thd, JOIN_TAB *tab, ha_rows limit, Item *condition)` 是 `estimate_rowcount()` 与范围优化器之间的桥。它的返回值语义在函数注释里写明：**`HA_POS_ERROR` = 派生表/视图或出错；`0` = 不可能有行的查询**。

- **先判定能否跳过 index dive。** `tab->set_skip_records_in_range(check_skip_records_in_range_qualification(tab, thd));`。
- **派生表跳过。** `if (!tl->uses_materialization())` 才走范围优化；派生表尚未填充、没有统计信息。表函数与可物化的 const 派生表走另一条路：`tl->fetch_number_of_rows(); return tl->table->file->stats.records;`。
- **构造候选 Key_map。** `Key_map keys_to_use = tab->const_keys; keys_to_use.merge(tab->skip_scan_keys);` —— **这正是 `add_loose_index_scan_and_skip_scan_keys()` 扩充结果的落地处**。
- **调用范围优化器。** `test_quick_select(thd, thd->mem_root, &temp_mem_root, keys_to_use, 0, 0, limit, false, ORDER_NOT_RELEVANT, tab->table(), tab->skip_records_in_range(), condition, &tab->needed_reg, tab->table()->force_index, tab->join()->query_block, &range_scan)`，随后 `tab->set_range_scan(range_scan);`。
- **三态返回。** `error == 1` 返回 `range_scan->num_output_rows()`；`error == -1` 时置 `tl->table->reginfo.impossible_range = true;` 并返回 0；其他情况返回 `HA_POS_ERROR`。

### 代价竞争的主战场：`test_quick_select()`

`sql/range_optimizer/range_optimizer.cc` 的 `int test_quick_select(...)` 是这一层的核心。它的返回值约定写在函数注释里：**`-1` = 不可能有行；`0` = 用不了 quick select；`1` = 找到可用 range 且构造成功**。

竞争顺序与判定条件：

1. **短路。** `if (keys_to_use.is_clear_all()) return 0;` —— 没有候选索引就直接放弃。
2. **建基线成本。** `records` 为 0 时先 `records++`（源码标注 `/* purecov: inspected */`）。`cost_est = table->file->table_scan_cost(); cost_est.add_io(1.1); cost_est.add_cpu(scan_time);`。若 `ignore_table_scan` 为真（即传入了 `force_index`），则 `scan_time = DBL_MAX; cost_est.set_max_cost();`——**FORCE INDEX 会把全表扫描成本顶到最大，从而使任何索引路径必然胜出**。
3. **LIMIT 触发强制走索引。** `if (limit < records)` 时 `cost_est.reset();` 后重新累加 `page_read_cost(records) + 1` 与 `scan_time`，源码注释「Force to use index」。
4. **成本阈值短路。** `else if (cost_est.total_cost() <= 2.0 && !force_quick_range) return 0;` —— 注释「No need for quick select」。即基线成本不超过 2.0 时不做范围分析。**注意这里只加 `cost_est.add_io(1.1)` 与 `add_cpu(scan_time)`，没有额外加索引比较成本**。
5. **候选集再过滤。** `keys_to_use.intersect(table->keys_in_use_for_query);` 然后 `if (keys_to_use.is_clear_all()) return 0;`。
6. **栈溢出保护。** `check_stack_overrun(thd, 3 * STACK_MIN_SIZE + sizeof(RANGE_OPT_PARAM), buff)`，源码注释解释乘 3 的原因：「range optimizer allocates big RANGE_OPT_PARAM structure and may evaluate a subquery expression」。
7. **covering index scan 参与竞争。** 若 `!table->covering_keys.is_clear_all()`，用 `find_shortest_key(table, &table->covering_keys)` 取最短覆盖索引（源码断言 `find_shortest_key() should return a valid key`，即 `assert(key_for_use != MAX_KEY)`），代价为 `index_scan_cost(key_for_use, 1, records)` 加 `row_evaluate_cost(records)`。**只有 `key_read_time < cost_est` 时才替换基线**，并在 trace 里记 `chosen` 与 `cause` 为 `cost`。
8. **条件树构造。** `get_mm_tree(thd, &param, prev_tables | INNER_TABLE_BIT, read_tables | INNER_TABLE_BIT, table->pos_in_table_list->map(), /*remove_jump_scans=*/true, cond)`。若树类型是 `SEL_TREE::IMPOSSIBLE`，记 `impossible_range` 为 true、`cost_est.reset()`、`cost_est.add_io(static_cast<double>(HA_POS_ERROR))`，然后 **`return -1`**。若树不是 `SEL_TREE::KEY`，记 `range_scan_possible` 为 false（条件恒真时额外记 `cause` 为 `condition_always_true`），并把 `tree` 置空。
9. **group min/max 参与竞争。** 调 `get_best_group_min_max(thd, &param, tree, interesting_order, skip_records_in_range, best_cost)`，返回非空时 `param.table->quick_condition_rows = min<double>(group_path->num_output_rows(), table->file->stats.records);`，随后 **`if (group_path->cost < best_cost)`** 才替换 `best_path`。**`best_cost` 初值是 `cost_est.total_cost()`**（即第 2 步算出的基线），所以 group min/max 必须比基线便宜才会被选中。
10. **skip scan 参与竞争。** 开关是 `thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SKIP_SCAN) || force_skip_scan`，其中 `force_skip_scan` 来自 `hint_table_state(thd, param.table->pos_in_table_list, SKIP_SCAN_HINT_ENUM, 0)`。判定为 `if (skip_scan_path->cost < best_cost || force_skip_scan)` —— **注意 `|| force_skip_scan`：强制提示可以无视代价胜出**。
11. **单索引 range / ROR 交集 / index merge union 参与竞争。** 条件是 `tree && (best_path == nullptr || !get_forced_by_hint(best_path))`。其中：
    - **ROR intersection**：`thd->lex->sql_command != SQLCOM_DELETE` 且（`index_merge_allowed` 或 `INDEX_MERGE` 提示）且 `interesting_order != ORDER_DESC`（注释说明 ROR-intersection 不能返回降序结果）。
    - **index merge union**：`!tree->merges.is_empty()` 且（`index_merge_allowed` 或 `INDEX_MERGE` 提示）且 `interesting_order != ORDER_DESC` 且 `param.table->file->stats.records` 非零。
    - `index_merge_allowed` 由 `thd->optimizer_switch_flag(OPTIMIZER_SWITCH_INDEX_MERGE)` 决定；union / sort union / intersect 三个子开关还额外要求父开关为真。
12. **最终落地。** `if (best_path && (table->file->ha_table_flags() & HA_NO_INDEX_ACCESS) == 0)` 才接受该路径，并置 `records = best_path->num_output_rows();`。**存储引擎声明 `HA_NO_INDEX_ACCESS` 时，即使算出了索引路径也会被丢弃**。
13. **返回三态。** `if (records == 0) return -1; else return *path != nullptr;`。

### `get_best_group_min_max()` 的调用契约

`get_best_group_min_max()` 的定义体不在本地稀疏检出范围内（`sql/range_optimizer/` 下只检出了 `index_merge.cc`、`index_range_scan.cc`、`range_optimizer.cc`），但在 `sql/range_optimizer/range_optimizer.cc` 里可以读到它的**调用契约与使用方式**，这是本页能确证的部分：

- **签名与实参。** `AccessPath *group_path = get_best_group_min_max(thd, &param, tree, interesting_order, skip_records_in_range, best_cost);` —— 六个实参分别是 `THD*`、`RANGE_OPT_PARAM*`、条件树指针、排序相关性、是否跳过 index dive、以及**当前最优成本上界**。
- **`tree` 可以为空。** 传入的 `tree` 在条件树不是 `SEL_TREE::KEY` 时已被置为 `nullptr`，函数仍被调用。
- **返回空表示无候选。** 返回 `nullptr` 时整个分支跳过，不改 `best_path`。
- **返回非空后的两个副作用。** 一是 `param.table->quick_condition_rows = min<double>(group_path->num_output_rows(), table->file->stats.records);`；二是 `param.table->quick_condition_rows` 会被写进表结构，供上层使用。
- **代价比较规则。** `if (group_path->cost < best_cost)` 才采纳，否则 trace 里记 `chosen` 为 false 且 `cause` 为 `cost`。
- **调试钩子。** `DBUG_EXECUTE_IF("force_lis_for_group_by", group_path->cost = 0.0;);` —— 强制把 loose index scan 的代价置 0，使它在任何比较中都胜出。
- **它的产出被单独识别。** `sql/sql_optimizer.cc` 里 `if (is_loose_index_scan(tab->range_scan())) join->tmp_table_param.precomputed_group_by = true;` —— loose index scan 命中后，GROUP BY 的结果被标记为「已预计算」。

### 候选索引的收集与排序

**`update_ref_and_keys()` 造候选。** `sql/sql_optimizer.cc` 的 `static bool update_ref_and_keys(...)` 分配 `Key_field` / `SARGABLE_PARAM` 共用内存，容量算式是 `max(sizeof(Key_field), sizeof(SARGABLE_PARAM)) * (((query_block->cond_count + 1) * 2 + query_block->between_count) * m + 1)`，其中 `m = max(query_block->max_equal_elems, 1U)`。内存布局上 `Key_field` 放前、`SARGABLE_PARAM` 放后，`(*sargables)[0].field = nullptr;` 充当数组哨兵。

**`add_key_part()` 把一个 `Key_field` 展开成多个 `Key_use`。** 它遍历 `table->s->keys` 个索引，跳过不在 `table->keys_in_use_for_query` 里的（`if (!(table->keys_in_use_for_query.is_set(key))) continue;`），再跳过全文本与空间索引（`if (table->key_info[key].flags & (HA_FULLTEXT | HA_SPATIAL)) continue;`），然后对每个 key part 做 `field->eq(table->key_info[key].key_part[part].field)` 比较，命中就压入一个 `Key_use`。

**`sort_keyuse()` 定义候选的优先级。** 排序键依次是：表序号升序 → 索引号升序 → key part 号升序 → **常量候选排在非常量之前** → **非 `KEY_OPTIMIZE_REF_OR_NULL` 的排在前面**。注意常量判定用的是 `a.used_tables & ~OUTER_REF_TABLE_BIT`（即排除外层引用位后仍非零才算常量）。

**排序后的裁剪。** `update_ref_and_keys()` 末尾用 `std::stable_sort(keyuse->begin(), keyuse->begin() + keyuse->size(), sort_keyuse)` 排序，然后逐条剔除：同一索引同一表下 key part 跳号（`prev->keypart + 1 < use->keypart`）的、以及「前一个 key part 是常量而当前 key part 也是常量」的。函数注释举了两个例子：`key(a,b)` 且条件是 `a=3 and b=7 and b=t2.d` 时跳过 `b=t2.d` 对应的 key part；`key(a,b,c)` 但只有 `b < 5` 或 `a=2 and c < 3` 被使用时，丢掉不连续的部分。

### 候选索引的扩充：`add_loose_index_scan_and_skip_scan_keys()`

`sql/sql_optimizer.cc` 的 `static void add_loose_index_scan_and_skip_scan_keys(JOIN *join, JOIN_TAB *join_tab)` 决定哪些索引能进候选集。函数注释列了三条来源：GROUP BY 的全部字段被某个索引覆盖 → 加入 `const_keys` 与 `keys`；DISTINCT 的全部 SELECT 字段被覆盖 → 加入 `const_keys` 与 `keys`（以便后续由 `GroupIndexSkipScanIterator` 处理）；无 GROUP BY 与聚合函数时收集 skip scan 的可能索引。

**skip scan 分支的条件很严。** `if (join->where_cond && join->primary_tables == 1 && join->group_list.empty() && !is_indexed_agg_distinct(join, &indexed_fields) && !join->select_distinct)`。跳过扫描是否启用由 `hint_table_state(join->thd, join_tab->table_ref, SKIP_SCAN_HINT_ENUM, OPTIMIZER_SKIP_SCAN)` 决定；若关闭且提示里的复合键提示 `get_compound_key_hint(SKIP_SCAN_HINT_ENUM)->is_key_map_clear_all()` 为真，则**直接 return**，不再收集可能索引。收集方式是遍历 WHERE 条件里所有 `Item_field`，取 `cur_item->field->part_of_key` 的交集。

**GROUP BY / DISTINCT 分支的交集逻辑。** 起始集是 `possible_keys = join_tab->table()->keys_in_use_for_query; possible_keys.merge(join_tab->table()->keys_in_use_for_group_by);`，然后对每个被引用字段 `possible_keys.intersect(cur_item->field->part_of_key);`。**只要有任一字段属于别的表（`cur_item->used_tables() != join_tab->table_ref->map()`），函数就 `return`，本表一个索引都不加。**

**采纳条件与 trace。** `if (!possible_keys.is_clear_all() && !possible_keys.is_subset(join_tab->const_keys))` 时才 `join_tab->const_keys.merge(possible_keys); join_tab->keys().merge(possible_keys);`，并调 `trace_indexes_added_group_distinct()` 记录原因字符串（`group_by` / `distinct` / `indexed_distinct_aggregate` / `skip_scan`）。

**`is_indexed_agg_distinct()` 判定可索引的 DISTINCT 聚合。** 它的拒绝条件：`join->primary_tables > 1`（引用多表）、`join->select_distinct`（有 DISTINCT）、`join->query_block->olap == ROLLUP_TYPE`（ROLLUP）。允许的聚合函数只有三类：`Item_sum::COUNT_DISTINCT_FUNC` 恒可；`AVG_DISTINCT_FUNC` / `SUM_DISTINCT_FUNC` 仅当 `sum_item->argument_count() == 1` 时可，否则 `[[fallthrough]]` 落入 `default: return false;`。每个聚合函数的参数必须是 `Item::FIELD_ITEM`（`if (expr->real_item()->type() != Item::FIELD_ITEM) return false;`）。多个聚合函数时，**它们的字段集合必须完全相同**——`if (first_aggdistinct_fields.is_clear_all()) first_aggdistinct_fields.merge(cur_aggdistinct_fields); else if (first_aggdistinct_fields != cur_aggdistinct_fields) return false;`。

### 索引 dive 的跳过：`check_skip_records_in_range_qualification()`

`sql/sql_optimizer.cc` 的 `static bool check_skip_records_in_range_qualification(JOIN_TAB *tab, THD *thd)` 决定是否省掉向存储引擎要精确行数的 index dive。函数注释列出的条件 F1/F2：

- **F1.a**：`table->force_index` 存在且 `table->keys_in_use_for_query.bits_set() != 1`（只强制了一个索引）。
- **F1.b**：`select->parent_lex->is_single_level_stmt()`（单层语句）。
- **F1.c**：`select->has_ft_funcs()` 为假（无全文本函数）。
- **F1.d**：`!select->is_grouped() && !select->is_distinct()`。
- **F2**：`select->m_current_table_nest->size() != 1`（多表查询不适用）。

不满足任一条就 `return false`（不能跳过 dive）。满足后还要看 ORDER BY：注释区分 **F1.e.I**（无 ORDER BY，则精确成本不需要，因为行访问已被 force index 约束）与 **F1.e.II**（有 ORDER BY 且被选索引能提供该顺序）。代码里 F1.e.I 写作 `if (!select->is_ordered()) return true;`；否则用 `test_if_order_by_key(&order_src, table, idx, &used_key_parts, &skip_quick)` 判断，**`return key_order != 0;`**（索引能提供顺序就跳过 dive）。

### 计划期的二次复查：`recheck_reason` 机制

`sql/sql_optimizer.cc` 里在连接顺序定案之后还有一轮「重新检查是否该用索引」的逻辑。触发前提是 `tab->type() == JT_ALL || tab->type() == JT_RANGE || tab->type() == JT_INDEX_MERGE || tab->type() == JT_INDEX_SCAN`，且 `tab->use_quick != QS_RANGE`。复查理由分两类，源码注释逐条列出：

- **NOT_FIRST_TABLE**：`cond && (tab->keys() != tab->const_keys) && (i > 0 || (join->query_block->master_query_expression()->item && cond->is_outer_reference()))`。对应注释里的 1a/1b/1c：有 range 优化器能处理的条件、有非常量条件、且某些非常量字段可能已被读过（不是第一张表，或是依赖外层表的子查询）。
- **LOW_LIMIT**：`!tab->const_keys.is_clear_all() && i == join->const_tables && (join->query_expression()->select_limit_cnt < (tab->position()->rows_fetched * tab->position()->filter_effect)) && !join->calc_found_rows`。对应注释里的 2a/2b/2c/2d。

**两类理由都会被抑制的三种情况**：存储引擎不支持索引访问（`if ((tab->table()->file->ha_table_flags() & HA_NO_INDEX_ACCESS) != 0) recheck_reason = DONT_RECHECK;`）、半连接策略已定为 loose scan（`tab->position()->sj_strategy == SJ_OPT_LOOSE_SCAN`）、以及 ORDER BY 已由既有索引满足。

**LOW_LIMIT 复查时用 `test_if_cheaper_ordering()` 做代价搜索。** 触发条件是「`prefer_ordering_index` 开关为真，或有 `force_index_order`，或当前类型是 `JT_ALL`」。此时 `usable_keys` 先与 `table->keys_in_use_for_order_by` 求交（仅当 `tab->table()->force_index_order` 为真），再调 `test_if_cheaper_ordering(tab, &join->order, tab->table(), usable_keys, -1, select_limit, &best_key, &read_direction, &select_limit)`。若 `best_key < 0` 则 `recheck_reason = DONT_RECHECK`（无可用键）；否则把 `usable_keys` 收缩成只有 `best_key` 一位。

**复查里调用 `test_quick_select()` 时传的参数与首轮不同。** 首轮（`get_quick_record_count()`）传的是 `prev_tables = 0`、`read_tables = 0`、`limit = row_limit`、`force_quick_range = false`、`interesting_order = ORDER_NOT_RELEVANT`。复查轮（LOW_LIMIT 且需要排序索引时）传的是 `used_tables & ~tab->table_ref->map()` 作 `prev_tables`、`interesting_order` 为 `ORDER_ASC` 或 `ORDER_DESC`（由 `read_direction` 决定），`limit` 传 `join->calc_found_rows ? HA_POS_ERROR : join->query_expression()->select_limit_cnt`。

**复查后无法确认条件可满足时的两级降级。** 第一次 `test_quick_select()` 返回值小于 0 时进入 `search_if_impossible` 分支：若 `!tab->join_cond()` 就 `return true`（整个查询是「Impossible WHERE」）；否则再跑一次 `test_quick_select()`（这次 `interesting_order` 固定为 `ORDER_NOT_RELEVANT`，`keys_to_use` 用 `tab->keys()`），若返回小于 0 则 `return true`（Impossible WHERE）。若第二次不小于 0，说明只是「Impossible ON」，trace 记 `without_ON_clause`。

**降级时把类型改回全表扫描。** 两次降级路径里都有 `if (tab->range_scan()) { destroy(tab->range_scan()); tab->set_type(JT_ALL); }`。**回退到 `JT_ALL` 的实际调用方是 `JOIN::optimize()` 内部这段复查代码本身**，而更早的一层降级在 `test_quick_select()` 内部：存储引擎带 `HA_NO_INDEX_ACCESS` 标志时 `best_path` 被丢弃，此时 `*path` 保持 `nullptr`，`test_quick_select()` 返回 0，`get_quick_record_count()` 落到 `return HA_POS_ERROR;`，`estimate_rowcount()` 因此保留 `tab->found_records` 为表总行数、`tab->read_time` 为表扫描成本。

### `get_best_group_min_max()` 与 skip scan 的开关语义差异

`get_best_group_min_max()` 的产出与 skip scan 产出在竞争时的判定条件不同：group min/max 必须 `group_path->cost < best_cost` 才被采纳；skip scan 是 `skip_scan_path->cost < best_cost || force_skip_scan`——**`force_skip_scan`（来自 `SKIP_SCAN` 提示）可以绕过代价比较直接胜出**。此外 skip scan 整体受 `OPTIMIZER_SWITCH_SKIP_SCAN` 开关约束，而 group min/max 没有对应的开关位，它受 `test_quick_select()` 的入口条件 `keys_to_use.is_clear_all()` 与 `cost_est.total_cost() <= 2.0` 这两道通用门槛约束。

## 关键取舍

**「候选路径是固定的一组」而不是开放式搜索，是这一层最根本的取舍。** `test_quick_select()` 里竞争的是 covering index scan、group min/max、skip scan、单索引 range、ROR intersection、index merge union 这六类，判定条件写死在代码里。收益是计划期开销可控、行为可预测；代价是遇到这六类之外的路径形态无从考虑。

**阈值 `cost_est.total_cost() <= 2.0` 是一道硬门槛，直接决定要不要做范围分析。** 基线扫描成本不超过 2.0 且没有 `force_quick_range` 时 `test_quick_select()` 直接 `return 0`。这意味着在小表上，即使存在可用的选择索引，也不会走范围分析——优化器在这里选择了「小表扫一遍更划算」的判断，而 2.0 这个数字本身没有任何统计依据，是写死的经验值。

**`records` 为 0 时先 `records++`，是刻意的下限保护。** 源码标注 `/* purecov: inspected */`，表示这条路径基本不会被测到。效果是空表的扫描成本不会退化成 0。

**`FORCE INDEX` 的实现方式是「把全表扫描成本顶到最大」，而不是「锁死某个索引」。** `test_quick_select()` 里 `if (ignore_table_scan) { scan_time = DBL_MAX; cost_est.set_max_cost(); }`，其中 `ignore_table_scan` 实参传的是 `tab->table()->force_index`。这意味着 FORCE INDEX 的效果是「索引路径必然胜出」，但胜出的是**哪个**索引仍由后续的代价比较决定。这与 `SKIP_SCAN` 提示的 `|| force_skip_scan` 是两种不同的强制机制。

**存储引擎的 `HA_NO_INDEX_ACCESS` 标志有一票否决权。** `test_quick_select()` 末尾 `if (best_path && (table->file->ha_table_flags() & HA_NO_INDEX_ACCESS) == 0)` 才接受路径；`JOIN::optimize()` 的复查段里 `if ((tab->table()->file->ha_table_flags() & HA_NO_INDEX_ACCESS) != 0) recheck_reason = DONT_RECHECK;` 也让复查直接跳过。代价是引擎不支持索引访问时，前面算出的所有索引成本都白算。

**`quick_condition_rows` 是悲观估计，源码自己承认。** `test_quick_select()` 的注释写明：它是「all considered table access methods (range and index_merge accesses over various indexes)」输出行数的最小值，「not a true E(#rows that satisfy table condition) but rather a pessimistic estimate」，并给出正确公式应是各条件独立时的乘法形式。注释里还留了一条 TODO：把 `quick_condition_rows` 从悲观估计改成真实估计。

**跳过 index dive 的判定条件极其严格，只在「一个索引 + 单层语句 + 无全文本 + 无 GROUP BY/DISTINCT」时成立。** `check_skip_records_in_range_qualification()` 的 F1.a 到 F2 五个条件缺一不可，且还要 ORDER BY 已被该索引满足。收益是省掉向存储引擎发起 index dive 的往返；代价是绝大多数查询拿不到这个优化。

**`add_loose_index_scan_and_skip_scan_keys()` 会引入「可被优化器考虑但不会被选中」的索引。** 函数注释自己写明了这个 corner case：「indexes that are not usable for resolving GROUP BY/DISTINCT may also be added in some corner cases. For example, an index covering 'a' and 'b' is not usable for the following query but is still added: "SELECT DISTINCT a+b FROM t1"」，并说明影响是「although the optimizer will consider using the index, it will not chose it (so minor calculation cost added but not wrong result)」。

**「字段属于别的表就整表放弃索引」是一条硬规则。** `add_loose_index_scan_and_skip_scan_keys()` 里 `if (cur_item->used_tables() != join_tab->table_ref->map()) return;` —— 没有任何部分生效的余地。

**候选索引的排序规则把「常量优先」和「非 REF_OR_NULL 优先」写死。** `sort_keyuse()` 里常量判定排除 `OUTER_REF_TABLE_BIT`，因此外层引用不算常量。这两条排序偏好决定了在多个索引都能用时哪一个先被尝试。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_optimizer.cc` | `JOIN::estimate_rowcount` | 本页主体之一：逐表算基线行数与扫描成本，触发范围分析，处理恒假条件并把表标成 `JT_CONST` |
| `sql/sql_optimizer.cc` | `get_best_group_min_max` | 本页主体之二（引用符号）；group min/max 访问路径的构造入口，见 `sql/range_optimizer/range_optimizer.cc` 的调用点 |
| `sql/sql_optimizer.cc` | `get_quick_record_count` | 范围分析的桥：合并 `const_keys` 与 `skip_scan_keys`，调 `test_quick_select()` 并按 -1/0/1 三态处理结果 |
| `sql/sql_optimizer.cc` | `check_skip_records_in_range_qualification` | 判定能否跳过向存储引擎发起的 index dive（F1.a–F1.e.II 与 F2 五个条件） |
| `sql/sql_optimizer.cc` | `add_loose_index_scan_and_skip_scan_keys` | 扩充候选索引：GROUP BY / DISTINCT / skip scan 可用的索引并入 `const_keys` 与 `skip_scan_keys` |
| `sql/sql_optimizer.cc` | `is_indexed_agg_distinct` | 判定查询是否可用 loose index scan 处理 `AGGFN(DISTINCT)`，含多聚合字段集必须相同的约束 |
| `sql/sql_optimizer.cc` | `trace_indexes_added_group_distinct` | 把因 GROUP BY / DISTINCT 而新增的索引记进 optimizer trace |
| `sql/sql_optimizer.cc` | `update_ref_and_keys` | 从 WHERE 条件收集 `Key_use` 与 `SARGABLE_PARAM`，排序后裁剪掉不连续的 key part |
| `sql/sql_optimizer.cc` | `add_key_part` | 把一个 `Key_field` 按索引与 key part 展开成多个 `Key_use`，跳过非候选索引与全文本/空间索引 |
| `sql/sql_optimizer.cc` | `sort_keyuse` | 定义候选优先级：表号 → 索引号 → key part 号 → 常量优先 → 非 `KEY_OPTIMIZE_REF_OR_NULL` 优先 |
| `sql/sql_optimizer.cc` | `JOIN::optimize_keyuse` | 给每个 `Key_use` 填 `ref_table_rows`，单表时下限 100，外层引用时为 1 |
| `sql/sql_optimizer.cc` | `JOIN::mark_const_table` | 恒假条件下把表写进 const 区并置 `JT_CONST`/`JT_SYSTEM` |
| `sql/sql_optimizer.cc` | `semijoin_types_allow_materialization` | 半连接比较类型的可物化性判定，含 `MAX_REF_PARTS` 上限与 blob 限制 |
| `sql/range_optimizer/range_optimizer.cc` | `test_quick_select` | 代价竞争的主战场：covering index scan / group min-max / skip scan / range / ROR / index merge union 六类路径的比价与落地 |
| `sql/range_optimizer/range_optimizer.cc` | `get_best_ror_intersect` | ROR-intersection 路径构造，参与代价竞争 |
| `sql/range_optimizer/range_optimizer.cc` | `get_best_disjunct_quick` | index merge union 的各 disjunct 路径构造与比价 |
| `sql/range_optimizer/range_optimizer.cc` | `get_ror_union_path` | `get_best_disjunct_quick()` 的辅助函数，处理 ROR union 构造 |
| `sql/range_optimizer/range_optimizer.cc` | `get_key_scans_params` | 单索引 range scan 与索引交集的路径参数构造 |
| `sql/range_optimizer/range_optimizer.cc` | `get_best_skip_scan` | skip scan 路径构造，由 `OPTIMIZER_SWITCH_SKIP_SCAN` 或 `SKIP_SCAN` 提示触发 |
| `sql/range_optimizer/range_optimizer.cc` | `get_mm_tree` | 把条件构造为 `SEL_TREE`，返回 `SEL_TREE::KEY` / `IMPOSSIBLE` / `ALWAYS` 等类型 |
| `sql/range_optimizer/range_optimizer.cc` | `setup_range_optimizer_param` | 构造并填充 `RANGE_OPT_PARAM` |
| `sql/range_optimizer/range_optimizer.cc` | `trace_basic_info` | 把选中的访问路径基本信息写入 optimizer trace |

## 与主核 / AI 的关系

主核（`engine/db`）当前没有索引选择这一层——`engine/db` 的执行器是单表扫描加过滤，索引由上层存储引擎（`engine/db/storage/`）承担。

**架构推论：** MySQL 这套「先算基线、再让索引路径赢过基线」的做法值得主核借鉴。其一是**基线必须先算出来**：MySQL 不是「有索引就用索引」，而是先有 `cost_est`（全表扫描成本）作为及格线，所有索引路径都要跟它比。主核若把「有索引就用索引」写成硬规则，就失去了「小表直接扫」这一档——MySQL 用 `cost_est.total_cost() <= 2.0` 这道门槛承载了这一档。其二是**降级路径要能精确回退**：`test_quick_select()` 里 `HA_NO_INDEX_ACCESS` 会把已算出的路径整个丢弃，`*path` 保持 `nullptr` 并返回 0，`get_quick_record_count()` 随之返回 `HA_POS_ERROR`，`estimate_rowcount()` 保留原基线值——这条「引擎不支持就整条退回扫描」的链路是显式的、可追踪的。主核若要做存储引擎可插拔，应当照此设计一条明确的回退链，而不是让引擎自己静默降级。

对 AI 场景（模型生成 SQL 并交给数据库执行）而言，这一层决定「模型写的条件能不能被优化器转成好计划」。**架构推论：** MySQL 把候选生成与代价竞争分成两段（`update_ref_and_keys()` 造候选，`test_quick_select()` 比价），这个分层对 AI 特别重要——它意味着「模型写的条件形式」只影响候选生成的完备性，而**代价比较本身是模型无法预判的黑盒**。模型能学到的是「条件该写成什么形式最容易命中候选收集」，学不到的是「哪条路径更便宜」。主核若要给 AI 反馈，最有价值的暴露点不是最终计划，而是 MySQL 的 optimizer trace：`test_quick_select()` 里的 `best_covering_index_scan`（含 `chosen` 与 `cause`）、`best_group_range_summary`、`best_skip_scan_summary`、`analyzing_range_alternatives`、`analyzing_index_merge_union` 这些段落，把「每个候选的代价与落选原因」逐条写了出来。

**架构推论：** `add_loose_index_scan_and_skip_scan_keys()` 那种「多收集一些候选索引，代价是可能选不中」的宽松做法，与 `check_skip_records_in_range_qualification()` 那种「条件极其严格才走快路径」的保守做法，在同一个文件里并存——它们分别对应优化器的两个不同诉求（探索完备性 vs. 计划期开销）。主核当前规模小，不必照搬两套策略，但应当把这两类决策点在设计上分开：哪些是「宁可多算也不漏」的地方，哪些是「宁可少算也不慢」的地方。

## 参考

- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_optimizer.cc)
- [MySQL 源码：`sql/range_optimizer/range_optimizer.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/range_optimizer/range_optimizer.cc)
- [MySQL 参考手册：Multiple-Column Indexes（多列索引的最左前缀与 range 优化）](https://dev.mysql.com/doc/refman/8.0/en/multiple-column-indexes.html)
- [MySQL 参考手册：Index Merge Optimizations（index merge union / intersection）](https://dev.mysql.com/doc/refman/8.0/en/index-merge-optimization.html)
- [MySQL 参考手册：Range Optimization（范围优化与索引跳跃扫描）](https://dev.mysql.com/doc/refman/8.0/en/range-optimization.html)
- [MySQL 参考手册：Index Skip Scan](https://dev.mysql.com/doc/refman/8.0/en/index-skip-scan.html)
- [MySQL 参考手册：GROUP BY 优化与 Loose Index Scan](https://dev.mysql.com/doc/refman/8.0/en/group-by-optimization.html)
- [MySQL 参考手册：EXPLAIN 输出格式（`type` 列的取值含义）](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html)
- [MySQL 参考手册：优化器开关（optimizer_switch，含 `index_merge`、`skip_scan`、`prefer_ordering_index`）](https://dev.mysql.com/doc/refman/8.0/en/optimizer-switch.html)
