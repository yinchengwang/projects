---
project: mysql-server
feature: partition-pruning
page_type: feature
schema_version: 1
refs:
  - path: sql/partition_info.cc
    symbols: [partition_info::can_prune_insert]
  - path: sql/sql_optimizer.cc
    symbols: [partition_pruning]
---

# 分区裁剪

## 一句话职责

分区裁剪负责在查询真正开始读数据之前，把 WHERE 条件（以及 INSERT 的待写行值）与分区定义比对，算出一张「本次需要访问哪些分区」的位图，把不可能命中的分区从读集合与锁集合里去掉；若条件把整张表的所有分区都排除掉，则把该查询块标记为空查询，直接返回零行。

## 机制是什么

**这一层解决的是「分区表有很多块，这次到底要碰哪几块」的问题。** 一张分区表在存储层是若干独立的分区（partition），子分区（subpartition）是分区下面的二级划分。裁剪就是在执行前用谓词把候选分区集合缩小，使得存储引擎只打开需要的分区。

裁剪的结果落在 `partition_info` 的两张位图上：

- `read_partitions`：本次查询要读的分区。
- `lock_partitions`：本次查询要加锁的分区。

这两张位图由 `sql/sql_partition.cc` 的 `set_up_partition_bitmaps()` 分配并初始化——它对 `read_partitions` 与 `lock_partitions` 各调一次 `partition_info::init_partition_bitmap()`，置 `bitmaps_are_initialized = true`，再调 `partition_info::set_partition_bitmaps(nullptr)` 做首次填值。

`partition_info::set_partition_bitmaps()` 是位图的「基线填值」入口，它做两件事：

- **用户显式指定的分区优先。** `table_list->partition_names` 非空时，先判 `table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION`：命中就报错 `ER_PARTITION_CLAUSE_ON_NONPARTITIONED` 并返回失败，源码注释写明「Don't allow PARTITION () clause on a NDB tables yet」；否则调 `set_read_partitions(table_list->partition_names)` 只点亮用户点名的分区。
- **没指定就全选。** 走 `bitmap_set_all(&read_partitions)`，对应调试输出 `Set all partitions`。

两条路径最后都汇到 `bitmap_copy(&lock_partitions, &read_partitions)`，并断言 `bitmap_get_first_set(&lock_partitions) != MY_BIT_NONE`（至少要有一个分区被锁）。函数开头还有 `is_pruning_completed = false;`，把「裁剪已完成」标志清掉，表示基线位图随时可能被裁剪结果覆盖。

**裁剪有两个时机点。** 第一次在解析/名称解析后（谓词已经是常量形式），第二次在表加锁之后（此时子查询与存储程序已被求值，谓词可以更精确）。源码注释把第一次的裁剪称为「only prune constant conditions, which will be used for lock pruning」。

**裁剪的粒度是分区，不是行。** 被裁掉的分区完全不参与后续的读与锁；被保留的分区内部还要靠分区函数的逐行计算再定位到具体子分区。

**裁剪结果通过 `TABLE::all_partitions_pruned_away` 向上冒泡。** 当裁剪把某张表的所有分区都排除时，这个标志置真，上层据此走「空结果」路径。

## 怎么实现的

### 计划期入口之一：`Query_block::resolve_placeholder_tables()` 里的首次裁剪

`sql/sql_resolver.cc` 的 `Query_block::resolve_placeholder_tables()` 在完成查询块合并之后做首次裁剪：

```
if (partitioned_table_count && prune) {
  for (Table_ref *tbl = leaf_tables; tbl; tbl = tbl->next_leaf) {
    if (prune_partitions(thd, tbl->table, this,
                         tbl->join_cond() ? tbl->join_cond() : m_where_cond))
      return true;
    if (tbl->table->all_partitions_pruned_away &&
        !tbl->is_inner_table_of_outer_join())
      set_empty_query();
  }
}
```

要点有三个：

- **入口条件是 `partitioned_table_count && prune`。** 只有查询块里确实有分区表、且调用方要求裁剪时才做。
- **谓词来源是二选一。** 优先用 `tbl->join_cond()`（该表在连接里的连接条件），没有就用 `m_where_cond`（WHERE 条件）。
- **空查询的判定带例外。** `all_partitions_pruned_away` 为真**且**该表不是外连接的内表（`!tbl->is_inner_table_of_outer_join()`）才调 `set_empty_query()`。外连接内表的分区全被裁掉不代表结果为空——外连接要给被裁掉的内表补 NULL 行。

### 计划期入口之二：`JOIN::optimize()` 里的加锁后裁剪

`sql/sql_optimizer.cc` 的 `JOIN::optimize()` 在 `optimize_cond()` 处理完 WHERE 与 HAVING 之后、聚合优化之前做一次裁剪：

```
if (query_block->partitioned_table_count && prune_table_partitions()) {
  error = 1;
  DBUG_PRINT("error", ("Error from prune_partitions"));
  return true;
}
```

注意它的位置：紧跟在 `optimize_cond()` 与 `having_cond` 的 `optimize_cond()` 之后，且在 `optimize_aggregated_query()` 之前。源码注释在 `JOIN::prune_table_partitions()` 头上写明前置条件「Requires that tables have been locked.」——因为这次裁剪要试非静态条件，这些条件只有在表锁住之后才能被探测。

### 谓词选取：`JOIN::prune_table_partitions()`

`sql/sql_optimizer.cc` 的 `JOIN::prune_table_partitions()` 遍历 `query_block->leaf_tables`，为每张分区表挑出用于裁剪的谓词。它的查找逻辑值得逐条看：

```
Item *cond = where_cond;
const table_map tbl_map = tbl->map();
for (Table_ref *nest = tbl; nest != nullptr; nest = nest->embedding) {
  if (nest->join_cond_optim() != nullptr &&
      Overlaps(tbl_map, nest->join_cond_optim()->used_tables())) {
    cond = nest->join_cond_optim();
    if (nest->is_aj_nest()) break;
  }
}
if (prune_partitions(thd, tbl->table, query_block, cond)) {
  return true;
}
```

- **默认用 `where_cond`。** 只有当某层连接嵌套的 `join_cond_optim()` 覆盖了这张表（`Overlaps(tbl_map, ...->used_tables())`）时，才改用该连接条件做裁剪。
- **只找最外层的那个条件。** 注释解释：用于裁剪的谓词必须放在最外层的 join nest 里；放在别的 join nest 或 WHERE 子句里的谓词，会导致外连接被转成内连接，那样就没有 join nest 图可以遍历了。
- **反连接要截断。** 源码注释写明：反连接（anti-join）会在反连接 nest 之上加一个合成的 left join nest，查找谓词时必须跳过它，所以命中 `nest->is_aj_nest()` 就 `break`。
- **入口断言 `query_block->partitioned_table_count`。** 该函数假定查询块里确实有分区表。

### 裁剪结果被上层消费：`JOIN::extract_const_tables()`

`sql/sql_optimizer.cc` 的 `JOIN::extract_const_tables()` 把「分区全被裁掉」当成「这张表没有行」：

```
const bool all_partitions_pruned_away = table->all_partitions_pruned_away;
...
case extract_empty_table:
  if ((table->file->stats.records == 0 || all_partitions_pruned_away) &&
      (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT))
    mark_const_table(tab, nullptr);
  break;
case extract_const_table:
  if ((table->s->system || table->file->stats.records <= 1 ||
       all_partitions_pruned_away) &&
      !tab->dependent &&
      (table->file->ha_table_flags() & HA_STATS_RECORDS_IS_EXACT) &&
      !tl->is_fulltext_searched())
    mark_const_table(tab, nullptr);
  break;
```

关键点是 `HA_STATS_RECORDS_IS_EXACT` 这个标志：即使 `all_partitions_pruned_away` 为真，也必须统计信息是精确的，才把该表标成 const 表。

### DML 路径：UPDATE / DELETE 的第二次裁剪

`sql/sql_update.cc` 与 `sql/sql_delete.cc` 都在加锁后做第二次裁剪，注释写明原因是「to prune when subqueries and stored programs can be evaluated」（`sql/sql_update.cc`）与「to be able to prune on subqueries in WHERE clause」（`sql/sql_delete.cc`）。

`sql/sql_update.cc` 的处理：

```
if (table->part_info && !no_rows) {
  if (prune_partitions(thd, table, query_block, conds))
    return true;
  if (table->all_partitions_pruned_away) {
    no_rows = true;
    if (thd->lex->is_explain()) {
      Modification_plan plan(thd, MT_UPDATE, table,
                             "No matching rows after partition pruning", true, 0);
      bool err = explain_single_table_modification(thd, thd, &plan, query_block);
      return err;
    }
    my_ok(thd);
    return false;
  }
}
```

`sql/sql_delete.cc` 的结构完全对称，唯一差别是构造的 `Modification_plan` 用 `MT_DELETE` 而非 `MT_UPDATE`。两处的 EXPLAIN 文案都是 `No matching rows after partition pruning`。**架构推论：** 这条路径是裁剪结果对 DML 的直接语义影响——分区裁剪不是只做性能优化，它能让 UPDATE / DELETE 提前返回「零行受影响」并给出一条专门的 EXPLAIN 原因。

### 存储引擎侧的落点：`get_partition_set()` 与 `prune_partition_set()`

`sql/sql_partition.cc` 的 `get_partition_set()` 是执行期把裁剪结果翻译成「要扫哪一段分区」的函数，签名是 `get_partition_set(const TABLE *table, uchar *buf, const uint index, const key_range *key_spec, part_id_range *part_spec)`。

它的起点是把范围铺满全表：

```
part_spec->start_part = 0;
part_spec->end_part = num_parts - 1;
```

其中 `num_parts = part_info->get_tot_partitions()`。随后它尝试用索引键把范围收窄，能收窄的判据是 `part_info->some_fields_in_PF.is_set(index)`（该索引至少含一个分区函数字段）：

- **整条键都给定**（`key_spec->length == key_info->key_length`）且 `part_info->all_fields_in_PF.is_set(index)`：调 `get_full_part_id_from_key()` 直接算出唯一分区，再调 `prune_partition_set()` 与裁剪位图求交集。
- **子分区表**（`part_info->is_sub_partitioned()`）：若 `all_fields_in_SPF.is_set(index)` 走 `get_sub_part_id_from_key()`；否则若 `all_fields_in_PPF.is_set(index)` 走 `get_part_id_from_key()`。这两个函数返回失败时，源码注释解释是「The value of the RANGE or LIST partitioning was outside of allowed values. Thus it is certain that the result of this scan will be empty.」，处理是 `part_spec->start_part = num_parts;` 直接返回。
- **只给了部分键**（`key_spec->length != key_info->key_length`）：先 `set_PF_fields_in_key()` 标记已绑定的分区字段，再用 `check_part_func_bound()` 判断分区函数是否已被完全绑定。

`prune_partition_set()` 是「与裁剪位图求交」的那一步，它遍历 `part_info->read_partitions` 位图，把 `part_spec` 的起止收缩到实际被置位的分区上：

```
if (i)
  i = bitmap_get_next_set(&part_info->read_partitions, i - 1);
else
  i = bitmap_get_first_set(&part_info->read_partitions);
...
if (last_partition == -1) /* No partition found in pruned bitmap */
  part_spec->start_part = part_spec->end_part + 1;
else
  part_spec->end_part = last_partition;
```

当位图里一个分区都没有时，`start_part` 被设成 `end_part + 1`，即「起点大于终点」——函数注释明确说明这表示「no partition needs to be scanned」。`get_partition_set()` 的注释同样定义了这套约定：`start_part > end_part` 表示不扫任何分区，`start_part == end_part` 表示只扫一个分区。

**架构推论：** `get_partition_set()` 里有一段空实现——源码原注释是「The next step is to analyse the table condition to see whether any information about which partitions to scan can be derived from there. Currently not implemented.」。也就是说「从表条件（WHERE）推导分区」这条更细的裁剪路径在 MySQL 8.0 的这份源码里是留白的，当前裁剪主要靠分区函数本身与索引键推导。

### INSERT 路径：`partition_info::can_prune_insert()`

INSERT 的裁剪走的是另一套机制：不看 WHERE 条件，而是看待插入的行值本身会落到哪个分区，从而只锁那个分区。这套逻辑由 `sql/partition_info.cc` 的 `partition_info::can_prune_insert()` 判定可行性，由 `partition_info::set_used_partition()` 落实。

`can_prune_insert()` 的返回值是三态枚举 `enum_can_prune`：`PRUNE_NO` / `PRUNE_YES` / `PRUNE_DEFAULTS`。函数体先 `*can_prune_partitions = PRUNE_NO;` 兜底，然后逐条否决：

1. **引擎自动分区。** `if (table->s->db_type()->partition_flags() & HA_USE_AUTO_PARTITION) return false;` —— 源码注释写明「Should not insert prune NDB tables」。
2. **BEFORE INSERT 触发器改写分区列。** 取 `table->triggers->get_triggers(TRG_EVENT_INSERT, TRG_ACTION_BEFORE)`，若 `trigger_chain->has_updated_trigger_fields(&full_part_field_set)` 则不能裁剪——注释解释触发器可能把行改到另一个分区。
3. **生成列参与分区。** `table->vfield` 里任一字段在 `full_part_field_set` 中则不能裁剪，注释是「their values are calculated much later」。
4. **生成列的默认值表达式参与分区。** `table->gen_def_fields_ptr` 同上。
5. **自增列参与分区。** `table->found_next_number_field` 在 `full_part_field_set` 中则不能裁剪。源码带一条 TODO 指出：若所有行都非 NULL 且不为 0（配合 `NO_AUTO_VALUE_ON_ZERO` sql_mode），其实是可裁剪的。
6. **ON DUPLICATE UPDATE 会改分区列。** 仅当 `duplic == DUP_UPDATE` 时检查三项：`update.function_defaults_apply_on_columns(&full_part_field_set)`、`is_fields_in_part_expr(update_fields)`、以及 BEFORE UPDATE 触发器改写分区列。
   - `is_fields_in_part_expr()` 定义在 `sql/partition_info.cc`，逐个比对 `fields` 里的字段是否落在 `full_part_field_set` 里。
   - `is_full_part_expr_in_fields()` 同样定义在 `sql/partition_info.cc`，判定是否**所有**分区字段都出现在 `fields` 中；源码注释说明 `full_part_field_array` 通常很短，所以直接线性遍历而不是构造位图。

函数接着决定是否需要拷贝默认值：

```
if (empty_values) {
  *prune_needs_default_values = true;  // like 'INSERT INTO t () VALUES ()'
} else if (!fields.empty()) {
  if (!is_full_part_expr_in_fields(fields))
    *prune_needs_default_values = true;
}
```

调用方 `sql/sql_insert.cc` 的 `Sql_cmd_insert_base::prepare_inner()` 对应三种 INSERT 语法规则，用 `const bool manage_defaults = column_count > 0 || value_count == 0;` 表达：`INSERT (columns) VALUES ...` 未列出的列需要默认值；`INSERT VALUES (), ...` 全部列需要默认值；`INSERT VALUES (expr, ...), ...` 不需要默认值。

最后是三态的落定：

```
if (!fields.empty() && !is_fields_in_part_expr(fields))
  *can_prune_partitions = PRUNE_DEFAULTS;
else
  *can_prune_partitions = PRUNE_YES;
```

`PRUNE_DEFAULTS` 表示「分区字段只取默认值，所有待插入行落在同一个分区」，因此只需检查第一行。

### 落实分区：`partition_info::set_used_partition()`

`sql/partition_info.cc` 的 `partition_info::set_used_partition()` 真正算出目标分区并点亮位图：

- **只接受常量。** 遍历 `values`，`if (!item->const_item()) return true;` —— 非常量值算不出分区。
- **需要默认值时先恢复默认记录。** `if (copy_default_values) restore_record(table, s->default_values);`
- **填值。** 字段列表为空时用 `table->field`，否则用传入的 `fields`，都调 `fill_record(...)`，只填 `full_part_field_set` 里的字段。
- **求函数默认值。** `if (info.function_defaults_apply_on_columns(&full_part_field_set)) { if (info.set_function_defaults(table)) return true; }` —— 处理 `CURRENT_TIMESTAMP` 这类 DEFAULT 函数。源码注释说明 `TIMESTAMP_AUTO_SET_ON_INSERT` 通过把 `Item_null` 转成语句开始时间实现，与 `write_row()` 一致，所以 `TIMESTAMP DEFAULT CURRENT_TIME` 是可裁剪的；而 `TIMESTAMP_AUTO_SET_ON_UPDATE` 若该列属于任何分区表达式则不可裁剪。
- **算分区号。** `const int rc = get_partition_id(this, &part_id, &func_value);`，前后用 `dbug_tmp_use_all_columns()` / `dbug_tmp_restore_column_map()` 临时放开读集合——源码注释解释原因：「This function is used in INSERT; 'values' are supplied by user, or are default values, not values read from a table, so read_set is irrelevant.」
- **点亮位图。** `bitmap_set_bit(used_partitions, part_id);`

### INSERT 的调用链：`Sql_cmd_insert_base::prepare_inner()`

`sql/sql_insert.cc` 的 `Sql_cmd_insert_base::prepare_inner()` 串起整条链路：

1. 构造 `COPY_INFO info(COPY_INFO::INSERT_OPERATION, &insert_field_list, manage_defaults, duplicates)` 与 `COPY_INFO update(COPY_INFO::UPDATE_OPERATION, &update_field_list, &update_value_list)`。
2. 调 `info.add_function_default_columns(insert_table, insert_table->write_set)`。源码在调用点上方注明「Must be done before can_prune_insert, due to internal initialization.」。
3. 调 `insert_table->part_info->can_prune_insert(thd, duplicates, update, update_field_list, insert_field_list, value_count == 0, &can_prune_partitions, &prune_needs_default_values, &used_partitions)`。
4. 若 `can_prune_partitions != partition_info::PRUNE_NO`，逐行调 `set_used_partition()`：
   - **第一行总是检查。** 无论 `PRUNE_YES` 还是 `PRUNE_DEFAULTS` 都检查第一行，因为 `PRUNE_DEFAULTS` 意味着「所有行落在同一分区」。
   - **后续行只在 `PRUNE_YES` 时检查。** 判定条件是 `if (can_prune_partitions == partition_info::PRUNE_YES)`。
   - **检查频率有上限。** `if (!(counter % num_partitions)) { if (bitmap_is_set_all(&used_partitions)) can_prune_partitions = partition_info::PRUNE_NO; }` —— `num_partitions` 来自 `insert_table->part_info->lock_partitions.n_bits`。源码注释解释这个 `counter % num_partitions` 是为了「Do it only from time to time to avoid overhead from bitmap_is_set_all() call.」，即每 `num_partitions` 行才检查一次是否已覆盖全部分区。
5. 若最终 `can_prune_partitions != partition_info::PRUNE_NO`，把裁剪结果与读/锁分区求交：

```
bitmap_intersect(&insert_table->part_info->read_partitions, &used_partitions);
bitmap_intersect(&insert_table->part_info->lock_partitions, &used_partitions);
```

源码注释说明了求交的安全性：「It is also safe for REPLACE, since all potentially conflicting records always belong to the same partition as the one which we try to insert a row. This is because ALL unique/primary keys must include ALL partitioning columns.」

**任何一步出错都会把 `can_prune_partitions` 复位成 `PRUNE_NO`**，即降级为「不裁剪、扫全部分区」。`set_used_partition()` 失败时的处理是 `can_prune_partitions = partition_info::PRUNE_NO; if (thd->is_error()) return true;` —— 错误照常上报，但裁剪被放弃。

### 位图的内存分配：`partition_info::init_partition_bitmap()`

`sql/partition_info.cc` 的 `partition_info::init_partition_bitmap()` 决定位图宽度：

```
uint bitmap_bits = num_subparts ? (num_subparts * num_parts) : num_parts;
uint bitmap_bytes = bitmap_buffer_size(bitmap_bits);
```

即**有子分区时位图覆盖全部子分区，没有子分区时覆盖全部分区**。缓冲区从 `mem_root` 分配，分配失败调 `mem_alloc_error(bitmap_bytes)` 并返回失败。

分区总数有硬上限：`sql/partition_info.cc` 在三处检查 `MAX_PARTITIONS`——`num_parts > MAX_PARTITIONS`、`(num_parts * num_subparts) > MAX_PARTITIONS`、`tot_partitions > MAX_PARTITIONS`。`MAX_PARTITIONS` 常量本身定义在本次未检出的头文件里。

## 关键取舍

**裁剪是「优化」还是「语义」？两者都是。** 一方面裁掉分区省掉了打开分区表、读分区元数据的开销；另一方面 `all_partitions_pruned_away` 会让 UPDATE / DELETE 提前返回零行并给出 `No matching rows after partition pruning` 的 EXPLAIN 原因。裁剪结果直接影响语句报告的影响行数。

**放弃裁剪永远是安全回退。** INSERT 路径上任何一步失败（`set_used_partition()` 失败、发现已覆盖全部分区）都把状态复位成 `PRUNE_NO`，退化为扫全部分区。源码注释在 `can_prune_insert()` 里给 `PRUNE_NO` 兜底赋值，在调用链里反复写 `can_prune_partitions = partition_info::PRUNE_NO; // set_used_partition may fail.`——裁剪失败从不让语句失败。

**触发器与生成列一律否决裁剪。** `can_prune_insert()` 对 BEFORE INSERT / BEFORE UPDATE 触发器改写分区列的检查用 `trigger_chain->has_updated_trigger_fields(&full_part_field_set)`；对生成列（`table->vfield`）与生成列默认表达式（`table->gen_def_fields_ptr`）逐字段查 `full_part_field_set`。理由在注释里：这些值在裁剪时点上算不出来，或算出来后行会落到别的分区。

**引擎自动分区（NDB）不参与。** `HA_USE_AUTO_PARTITION` 命中时 `can_prune_insert()` 直接返回，`set_partition_bitmaps()` 里用户写 `PARTITION ()` 子句直接报错 `ER_PARTITION_CLAUSE_ON_NONPARTITIONED`。`sql/sql_partition.cc` 里还有一处同标志的判断，注释解释这种引擎「can handle automatic partitioning and there is no primary key」，分区靠引擎自己的隐藏键完成。

**空查询判定要给外连接留例外。** `sql/sql_resolver.cc` 里 `all_partitions_pruned_away && !tbl->is_inner_table_of_outer_join()` 才 `set_empty_query()`。外连接的内表即使分区全被裁掉，仍要补 NULL 行。

**索引键推导优先于表条件推导。** `get_partition_set()` 先尝试从索引键推出分区，推不出来才落到「range with holes」的位图形态；而「从表条件推导分区」那一段在源码里是空的 TODO 块，注释写明「Currently not implemented.」。

**裁剪位图与锁位图必须同步收缩。** `set_partition_bitmaps()` 里 `bitmap_copy(&lock_partitions, &read_partitions)` 让两者初始一致；INSERT 路径上又同时对两者做 `bitmap_intersect()`。两处都保证「只锁要读的分区」。

**位图宽度按子分区展开。** `init_partition_bitmap()` 用 `num_subparts ? (num_subparts * num_parts) : num_parts`，子分区表的下标空间是「分区数 × 子分区数」。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| sql/partition_info.cc | `partition_info::can_prune_insert` | 判定 INSERT 能否裁剪分区；逐条否决引擎自动分区、触发器改写分区列、生成列、自增列、ON DUPLICATE UPDATE 改写分区列等情形，输出 `PRUNE_YES` / `PRUNE_DEFAULTS` / `PRUNE_NO` |
| sql/partition_info.cc | `partition_info::set_used_partition` | 算出待插入行落到的分区号并点亮 `used_partitions` 位图；只接受常量值，必要时恢复默认记录并求函数默认值 |
| sql/partition_info.cc | `partition_info::set_partition_bitmaps` | 裁剪位图的基线填值：用户指定分区时只点亮这些分区，否则全选；随后把读分区复制成锁分区 |
| sql/partition_info.cc | `partition_info::init_partition_bitmap` | 分配并初始化分区位图；位宽为 `num_subparts ? num_subparts * num_parts : num_parts` |
| sql/partition_info.cc | `partition_info::is_fields_in_part_expr` | 判断给定字段列表里是否至少有一个字段参与分区表达式 |
| sql/partition_info.cc | `partition_info::is_full_part_expr_in_fields` | 判断给定字段列表是否覆盖了全部分区表达式字段 |
| sql/sql_optimizer.cc | `partition_pruning` | 以头文件形式引入分区裁剪实现（`sql/range_optimizer/partition_pruning.h`），该头文件提供 `prune_partitions` 入口 |
| sql/sql_optimizer.cc | `JOIN::optimize` | 在 `optimize_cond()` 之后、聚合优化之前调 `prune_table_partitions()` 做加锁后裁剪 |
| sql/sql_optimizer.cc | `JOIN::prune_table_partitions` | 遍历叶表，用最外层 join nest 的 `join_cond_optim()`（无则用 `where_cond`）作为裁剪谓词调 `prune_partitions`；反连接处截断查找 |
| sql/sql_optimizer.cc | `JOIN::extract_const_tables` | 把 `all_partitions_pruned_away` 当作「这张表没有行」，在统计精确（`HA_STATS_RECORDS_IS_EXACT`）时把该表标成 const 表 |
| sql/sql_resolver.cc | `Query_block::resolve_placeholder_tables` | 查询块合并后的首次裁剪：按 `join_cond()` 或 `m_where_cond` 裁剪，分区全被裁掉且非外连接内表时置空查询 |
| sql/sql_insert.cc | `Sql_cmd_insert_base::prepare_inner` | INSERT 裁剪的调用链：判定可行性 → 逐行 `set_used_partition()` → 对读/锁分区做 `bitmap_intersect` |
| sql/sql_partition.cc | `set_up_partition_bitmaps` | 为分区表分配并初始化 `read_partitions` 与 `lock_partitions` 两张位图 |
| sql/sql_partition.cc | `get_partition_set` | 执行期把索引键推导出的分区范围与裁剪位图求交，得出实际要扫的分区区间 |
| sql/sql_partition.cc | `prune_partition_set` | 遍历 `read_partitions` 位图收缩分区区间；一个分区都没置位时把起点设成终点加一，表示不扫任何分区 |
| sql/sql_update.cc | `Sql_cmd_update` | 加锁后第二次裁剪（为裁剪子查询与存储程序）；分区全被裁掉时置 `no_rows` 并给出 `No matching rows after partition pruning` |
| sql/sql_delete.cc | `Sql_cmd_delete` | 加锁后第二次裁剪（为裁剪 WHERE 子句中的子查询）；分区全被裁掉时置 `no_rows` 并给出 `No matching rows after partition pruning` |

## 与主核 / AI 的关系

**架构推论：** MySQL 的分区裁剪把「读哪几个分区」与「锁哪几个分区」建模成同一张位图的两种派生结果（`read_partitions` 与 `lock_partitions`），裁剪一次、两个用途同时受益。主核若支持分区表，这两条位图应从一开始就分开建模，而不是等到裁剪实现时再补——`set_partition_bitmaps()` 里那句 `bitmap_copy(&lock_partitions, &read_partitions)` 以及 INSERT 路径上成对出现的两次 `bitmap_intersect()` 说明，这两张表的生命周期是绑定的。

**架构推论：** 裁剪的三个时机点（解析后、加锁后、逐行 INSERT 时）都对应「谓词能提供的确定性」在逐步提高。主核自研引擎若只实现第一个时机点，就只能裁掉常量条件的分区；子查询与存储程序求值后的二次裁剪是另一档能力。MySQL 在 UPDATE / DELETE 上明确做了第二次裁剪，代价是要在锁表后重走一遍谓词分析。

**架构推论：** `all_partitions_pruned_away` 这个标志把「裁剪结果」提升成了「查询语义的一部分」——它让 UPDATE / DELETE 提前结束并改变 EXPLAIN 的原因说明。AI 辅助改写 SQL 时，若只按逻辑等价性改写而不考虑分区裁剪，一条原本命中若干分区的 WHERE 改写后可能让所有分区都被裁掉，语句会从「影响 N 行」变成「影响 0 行」；反向也可能把本可裁成单分区的条件改成跨分区条件，让存储引擎打开更多分区。

## 参考

- [MySQL 源码：`sql/partition_info.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/partition_info.cc)
- [MySQL 源码：`sql/sql_optimizer.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_optimizer.cc)
- [MySQL 源码：`sql/sql_partition.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_partition.cc)
- [MySQL 源码：`sql/sql_insert.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_insert.cc)
- [MySQL 源码：`sql/range_optimizer/partition_pruning.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/range_optimizer/partition_pruning.cc)
- [MySQL 参考手册：分区裁剪（Partition Pruning）](https://dev.mysql.com/doc/refman/8.0/en/partition-pruning.html)
- [MySQL 参考手册：分区类型（Partition Types）](https://dev.mysql.com/doc/refman/8.0/en/partition-types.html)
- [MySQL 参考手册：`EXPLAIN` 输出格式](https://dev.mysql.com/doc/refman/8.0/en/explain-output.html)
- [MySQL 参考手册：`EXPLAIN ANALYZE`](https://dev.mysql.com/doc/refman/8.0/en/explain-analyze.html)
