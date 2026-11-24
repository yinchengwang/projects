---
project: mysql-server
feature: name-resolution
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_resolver.cc
    symbols: [Query_block::resolve_placeholder_tables]
  - path: sql/item.cc
    symbols: [Item_field::fix_outer_field]
---

# 名称解析与列绑定

## 一句话职责

MySQL 的名称解析层把查询文本里的表名、列名、别名翻译成具体的 `Table_ref` 与 `Field` 指针：它在当前查询块的 `FROM` 列表里逐表查找列名，判定歧义并报 `ER_NON_UNIQ_ERROR` / `ER_BAD_FIELD_ERROR`，找不到时按「当前块 → 外层块」逐级向上搜索直到 `SELECT`/`GROUP BY` 列表，并把跨块引用标记为 `Item_outer_ref`（相关子查询）。

## 机制是什么

**名称解析不是一个独立模块，而是「语义分析（resolve）阶段」里的一组函数**，分散在 `sql/sql_resolver.cc`（查询块编排）与 `sql/item.cc`（表达式项的绑定）两个文件里，`sql/sql_base.cc` 提供查表原语。

**核心概念是「名称解析上下文」（name resolution context）。** 每个 `Item_field` 在构造时记下自己所属的上下文 `context`，上下文里有 `first_name_resolution_table` / `last_name_resolution_table` 两个 `Table_ref` 指针，界定「本块可见的 FROM 列表」；上下文还有 `outer_context` 指针，形成从内到外的上下文链，这是相关子查询能找到外层列的结构基础。

**绑定分三个层次。** 第一层是「当前 FROM 列表里的列」——由 `find_field_in_tables()` 完成，它是整个机制的入口；第二层是「当前块的 SELECT / GROUP BY 列表里的别名与派生列」——由 `find_item_in_list()` 与 `resolve_ref_in_select_and_group()` 完成；第三层是「外层查询块的列」——由 `Item_field::fix_outer_field()` 完成。

**解析顺序在 `Item_field::fix_fields()` 的注释里写成了伪代码**：先在本块 FROM 里找；找不到且存在外层查询时，对每个外层块 `Q_k`（从最内层开始）先找 FROM 再找 SELECT 与 GROUP。

**歧义判定靠「找到第二个同名列」。** `find_field_in_tables()` 遍历 FROM 列表时用一个 `found` 指针记账：第一次命中就记下来并继续扫；若第二张表又命中同名列，就说明该名字不唯一，报 `ER_NON_UNIQ_ERROR`。但如果列名带了库名限定（`db.col`），则命中后直接返回——源码注释写明「完全限定的字段不可能重复」。

**视图列不是 `Field` 而是表达式。** MERGE 视图的列用 `Field_translator` 表示，命中时 `find_field_in_view()` 通过 `Field_iterator_view::create_item()` 造出一个 `Item`，写进调用方的 `*reference` 参数，并返回哨兵值 `view_ref_found`（定义在 `sql/sql_base.cc` 为 `Field *view_ref_found = (Field *)0x2`）告诉调用方「真身不在返回值里，而在 `reference` 里」。

**NATURAL/USING 连接的结果列是第三种形态。** `Natural_join_column` 列表既可能是视图列也可能是基表列，`find_field_in_natural_join()` 遍历它时若发现两个同名列同样报 `ER_NON_UNIQ_ERROR`。

**「派生表/视图占位表」是名称解析的前置环节。** `Query_block::resolve_placeholder_tables()` 负责在正式绑定之前，把 `FROM` 里的派生表与视图解析成真正的表引用——要么合并进当前块（`merge_derived()`），要么准备成待物化的临时表（`setup_materialized_derived()`）。

## 怎么实现的

### 编排层：`Query_block::prepare()` 的顺序

`sql/sql_resolver.cc` 的 `Query_block::prepare()` 把整个解析期的先后顺序写死了：

1. `setup_tables(thd, get_table_list(), false)` —— 建叶表列表、给每张表编号、核对索引提示。编号用 `tr->set_tableno(tableno)`，并有硬上限：`tableno >= MAX_TABLES` 时报 `ER_TOO_MANY_TABLES`（常量 `MAX_TABLES`）。
2. `if ((derived_table_count || table_func_count) && resolve_placeholder_tables(thd, true)) return true;` —— 只有存在派生表或表函数时才调。
3. `check_view_privileges(thd, SELECT_ACL, SELECT_ACL)` —— 等派生表全部解析完再做视图的权限检查（源码注释：「Wait with privilege checking until all derived tables are resolved」）。
4. `is_item_list_lookup = true;` —— 这一行打开「别名查找」开关，之前为 `false`（源码注释解释：LATERAL 连接里的表达式不能引用 item 列表，所以建表阶段必须关掉）。
5. `setup_natural_join_row_types(thd, m_current_table_nest, &context)` —— 预先算出 NATURAL/USING 连接的行类型。
6. `setup_wild(thd)` → `setup_base_ref_items(thd)` → `setup_fields(...)` —— 展开 `*`、建 `base_ref_items` 数组、绑定 SELECT 列表里的表达式。

### 占位表解析：`Query_block::resolve_placeholder_tables`

`sql/sql_resolver.cc` 的 `Query_block::resolve_placeholder_tables(THD *thd, bool apply_semijoin)` 遍历本块的 `next_local` 链，对每个派生表/视图/表函数：

- 入口有断言 `assert(derived_table_count > 0 || table_func_count > 0)`。
- 跳过条件：`if (!tl->is_view_or_derived() && !tl->is_table_function()) continue;`。
- 已经合并或已物化的跳过：`if (tl->is_merged() || tl->uses_materialization()) continue;`——源码注释说明 `transform_grouped_to_derived()` 内部已经调过一次 `setup_tables()` 与 `resolve_placeholder_tables()`，所以这里不能重复做（注释原文：「Merging is only done once and must not be repeated for prepared execs」）。
- `if (tl->resolve_derived(thd, apply_semijoin)) return true;` —— 解析派生表本体。
- 合并路径：`if (!thd->lex->is_view_context_analysis()) { if (tl->is_mergeable() && merge_derived(thd, tl)) return true; }`。视图上下文分析模式下**不合并**。
- 物化路径：表函数走 `tl->setup_table_function(thd)`；派生表在 `tl->table == nullptr` 时走 `tl->setup_materialized_derived(thd)`。
- 末尾 `materialized_derived_table_count++`。

**真实调用方：** `Query_block::prepare()` 在 `setup_tables()` 之后调它；`Query_block::transform_grouped_to_derived()` 在建好新派生表、重新 `setup_tables()` 之后也调它（源码注释写明这次调用需要普通 arena，所以在 `Prepared_stmt_arena_holder` 作用域之外）。

### 查表原语：`find_field_in_tables`

`sql/sql_base.cc` 的 `find_field_in_tables()` 是绑定列名的入口，签名参数 `report_error` 取四个枚举值：`IGNORE_ERRORS`、`IGNORE_EXCEPT_NON_UNIQUE`、`REPORT_EXCEPT_NON_UNIQUE`、`REPORT_ALL_ERRORS`，决定错误报到什么程度。

它的执行骨架：

- **限定名归一。** `if (!table_name || !table_name[0]) { table_name = nullptr; db = nullptr; }` —— 没写表名就同时丢掉库名限定。
- **`allow_rowid` 的计算。** `allow_rowid = table_name || (first_table && !first_table->next_local);` —— 只有「显式写了表名」或「FROM 里只有一张表」时才允许解析出 `_rowid` 伪列。
- **预处理语句的缓存捷径。** `if (item->cached_table)` 分支直接复用上次解析到的表，跳过整轮遍历；源码注释写明这是一个自觉的取舍：「With this optimization we also miss case when addition of one more field makes some prepared query ambiguous and so erroneous, but we accept this trade off」。
- **库名大小写。** `if (db && (lower_case_table_names || is_infoschema_db(db, strlen(db))))` 才把库名折叠成小写；`information_schema` 被当作大小写不敏感，源码注释说明它在 `lower_case_table_names=0` 时也在 FROM 子句里按不敏感处理。
- **遍历。** `for (cur_table = first_table; cur_table != last_table; cur_table = cur_table->next_name_resolution_table)`，每张表调 `find_field_in_table_ref()`。
- **完全限定直接返回。** `if (db) return cur_field;` —— 源码注释：「If we found a fully qualified field we return it directly as it can't have duplicates」。
- **歧义报错。** `if (found) { ... my_error(ER_NON_UNIQ_ERROR, MYF(0), table_name ? item->full_name() : name, thd->where); return (Field *)nullptr; }`，报错条件是 `report_error == REPORT_ALL_ERRORS || report_error == IGNORE_EXCEPT_NON_UNIQUE`。
- **未知表 vs 未知列。** 遍历结束后若 `table_name && (cur_table == first_table)`（说明一个表都没扫到）报 `ER_UNKNOWN_TABLE`；否则在 `want_privilege == 0` 或列权限检查通过时报 `ER_BAD_FIELD_ERROR`，否则报列权限错误——源码注释解释了这个顺序：「If user does not have grant, then we should throw error stating 'access denied'. If user does have right then we can give proper error like column does not exist」。

### 分派：`find_field_in_table_ref`

`sql/sql_base.cc` 的 `find_field_in_table_ref()` 先做限定名匹配：表名用 `my_strcasecmp(table_alias_charset, table_list->alias, table_name)` 比，库名对普通表用 `strcmp` 比、对 `schema_table` 用 `my_strcasecmp(system_charset_info, ...)` 比；不匹配直接返回 `nullptr`。源码注释说明嵌套 join 被排除在匹配之外（「Exclude nested joins」），因为嵌套 join 的列来自不同表。

然后按表引用的三种形态分派：

- `table_list->field_translation` 非空 → MERGE 视图或 information_schema 表 → `find_field_in_view()`，返回 `view_ref_found`。
- `!table_list->nested_join` → 普通存储表 → `find_field_in_table(table_list->table, name, allow_rowid, field_index_ptr)`。
- 否则是 NATURAL/USING join → 带限定名时递归进 `table_list->nested_join->m_tables` 里逐个表找；不带限定名时走 `find_field_in_natural_join()`。

**基表查找 `find_field_in_table()`** 遍历 `table->field` 数组，用 `my_strcasecmp(system_charset_info, (*field_ptr)->field_name, name)` 比对。源码注释记录了一个已知偏差：「This should probably be strncollsp() instead of my_strcasecmp(); in particular, Ñ != N for my_strcasecmp()」，理由是改用排序规则比较会破坏已有表定义。找不到时才尝试 `_rowid`：`field = table->field[table->s->rowid_field_offset - 1];`。

**视图查找 `find_field_in_view()`** 用 `Field_iterator_view` 遍历视图列，比中之后调 `field_it.create_item(thd)` 生成视图列表达式，写进 `*ref` 并返回 `view_ref_found`。源码注释说明：视图列被用户起了别名时，要同时把别名写到新 item 和它的 `real_item()` 上。

**NATURAL/USING 查找 `find_field_in_natural_join()`** 遍历 `Natural_join_column` 列表，第二个同名列出现时报 `ER_NON_UNIQ_ERROR`；视图列走 `nj_col->create_item(thd)` 返回 `view_ref_found`，基表列则先检查 `nj_col->table_field->fixed`，必要时补一次 `fix_fields()`——源码注释说明这是为了处理「表重开后列被并发连接删掉」的情况。

### 表达式层：`Item_field::fix_fields`

`sql/item.cc` 的 `Item_field::fix_fields(THD *thd, Item **reference)` 是单个列引用的绑定主流程：

- **克隆字段的快捷路径。** `if (table_ref != nullptr)` —— 条件推下到派生表时克隆出来的字段已经有表引用与 `Field`，直接调 `set_field(field)` 再把 `table_ref` 复原（源码注释解释：UPDATE 语句在 `setup update_tables` 时改过表引用，而条件推下发生在那之后，所以必须把原始表引用设回去）。
- **主查表。** `from_field = find_field_in_tables(thd, this, context->first_name_resolution_table, context->last_name_resolution_table, reference, thd->lex->use_only_table_context ? REPORT_ALL_ERRORS : IGNORE_EXCEPT_NON_UNIQUE, any_privileges ? 0 : thd->want_privilege, true);` —— 报错级别由 `thd->lex->use_only_table_context` 决定；权限位在 `any_privileges` 为真时传 `0`（不检查）。
- **别名查找。** 表里找不到时，若 `qb->is_item_list_lookup` 为真，用 `find_item_in_list(thd, this, &qb->fields, &res, &counter, &resolution)` 在本块 SELECT 列表里找别名。找到且 `resolution == RESOLVED_AGAINST_ALIAS` 时调 `set_alias_of_expr()`。若命中的是 `Item::FIELD_ITEM`（select 列表里指向另一列），复用其 `field` 并 `set_field(new_field)`；否则造一个 `Item_ref` 替换掉解析器造的 `Item_field`。
- **外层回退。** 表里没找到、别名也没找到时调 `fix_outer_field()`；此外还有一个补充路径：若 `cached_table->query_block != context->query_block`（缓存里的列属于别的查询块），也调 `fix_outer_field()`。
- **视图列返回。** `if (from_field == view_ref_found)` 时，若 `is_null_on_empty_table(thd, this)` 为真则把结果标记为可空，然后返回。
- **隐藏列报错。** `if (from_field->is_hidden_by_system())` → `my_error(ER_BAD_FIELD_ERROR, ...)`，源码注释：「This field is either hidden by the storage engine or SQL layer. In either case, report column "not found" error」。
- **最终绑定。** 调 `set_field(from_field)`。`Item_field::set_field()` 负责把 `table_ref`、`field_index`、`field`、`result_field`、可空性、字符集、排序规则、数据类型、小数位、无符号标志、`max_length` 全部从 `Field` 复制到 `Item` 上，最后置 `fixed = true`。

### 外层搜索：`Item_field::fix_outer_field`

`sql/item.cc` 的 `Item_field::fix_outer_field(THD *thd, Field **from_field, Item **reference)` 是相关子查询的关键。函数头注释给出的伪代码是：

```
for each outer query Q_k beginning from the inner-most one
{
  search for a column or derived column named col_ref_i
  [in table T_j] in the FROM clause of Q_k;

  if such a column is not found
    Search for a column or derived column named col_ref_i
    [in table T_j] in the SELECT and GROUP clauses of Q_k.
}
```

实现要点：

- **从内往外遍历上下文链。** `for (; outer_context; outer_context = outer_context->outer_context)`，每轮 `select = outer_context->query_block; last_checked_context = outer_context; upward_lookup = true;`。
- **LATERAL 派生表看不到外层表。** `if (place == CTX_DERIVED && select->end_lateral_table == nullptr) continue;` —— 源码注释：「A non-lateral derived table cannot see tables of its owning query」。
- **缓存与上下文不匹配时跳过。** `if (field_found && outer_context->query_block != cached_table->query_block) continue;`。
- **在 FROM 里查。** `(*from_field = find_field_in_tables(thd, this, outer_context->first_name_resolution_table, outer_context->last_name_resolution_table, reference, IGNORE_EXCEPT_NON_UNIQUE, thd->want_privilege, true)) != not_found_field`。
- **在 SELECT / GROUP 里查。** `if (select_alias_referencable(place) && outer_context->resolve_in_select_list)` 时调 `resolve_ref_in_select_and_group(thd, this, select, &ref)`；注释明确写着返回 -1 表示「Some error occurred (e.g. ambiguous names)」。
- **分组查询里的替换。** 若 `!last_checked_context->query_block->having_fix_field && select->group_list.elements && (place == CTX_SELECT_LIST || place == CTX_HAVING)`，则把该字段替换成 `Item_outer_ref`——源码注释：「If an outer field is resolved in a grouping select then it is replaced for an Item_outer_ref object. Otherwise an Item_field object is used」。
- **集合函数层级修正。** 若 `thd->lex->in_sum_func && thd->lex->in_sum_func->base_query_block->nest_level >= select->nest_level`，把 `in_sum_func->max_aggr_level` 抬到 `select->nest_level`。
- **最终错误。** 走到末尾 `*from_field` 仍是 `not_found_field` 时：`if (upward_lookup) my_error(ER_BAD_FIELD_ERROR, MYF(0), full_name(), thd->where);` —— 源码注释说「We can't say exactly what absent table or field」；否则再调一次 `find_field_in_tables()` 只为报出更精确的错误。
- **返回值语义。** 头注释定义：返回 `1` 表示列已解析成功、`fix_fields()` 应继续；返回 `0` 表示列已完全绑定、`fix_fields()` 应返回 false；返回 `-1` 表示出错。

### 别名与派生列：`resolve_ref_in_select_and_group`

`sql/item.cc` 的 `resolve_ref_in_select_and_group()` 处理 HAVING 与外层引用里的别名。函数注释写明算法顺序：先在 SELECT 列表找 → 再在 GROUP BY 列表找 → 若两处是不同字段则**告警**（`push_warning_printf(thd, Sql_condition::SL_WARNING, ER_NON_UNIQ_ERROR, ...)`）→ 返回 GROUP BY 里的列（若找到），否则返回 SELECT 里的列。

配套的 `find_field_in_group_list()` 按「匹配度」打分：字段名相同记 1 分，表名也相同再记 1 分，库名也相同再记 1 分；`if (cur_match_degree > found_match_degree)` 更新最佳候选；若两者匹配度相等但 `!(found_group->item)->eq(cur_field, false)`（指向不同列），报 `ER_NON_UNIQ_ERROR`。

**前向引用报错。** `resolve_ref_in_select_and_group()` 里若 `select->base_ref_items[counter] == nullptr`，报 `ER_ILLEGAL_REFERENCE`，错误文案是 `"forward reference in item list"`——源码注释举了两个例子：`SELECT (SELECT ... WHERE a = 1) AS a ...` 与 `SELECT (SELECT ... WHERE b = 1) AS a, (SELECT ...) AS b ...`。

### 依赖标记：`mark_as_dependent`

`sql/item.cc` 的静态函数 `mark_as_dependent()` 负责把「这个引用依赖外层查询块」这件事落到数据上：给 `mark_item` 与 `resolved_item` 的 `depended_from` 置上外层 `Query_block` 指针，再调 `current->mark_as_dependent(last, false)` 沿上下文链向上传播。EXPLAIN 时额外压一条 `SL_NOTE` 级别的 `ER_WARN_FIELD_RESOLVED` 提示，说明该字段是在哪个 select number 解析到外层 select number 的。

## 关键取舍

**「先查表、再查别名」的固定顺序，与 SQL 标准允许的顺序相反。** `Item_field::fix_fields()` 的注释明确说：「Notice that compared to Item_ref::fix_fields, here we first search the FROM clause, and then we search the SELECT and GROUP BY clauses」。代价是 `SELECT a AS b FROM t` 里的 `b` 在 `t` 里也有同名列时，列优先；收益是基表列名不会被别名遮蔽。

**歧义一律报错，不做「优先选第一个」。** `find_field_in_tables()` 扫到第二个同名列就直接 `ER_NON_UNIQ_ERROR`。代价是 `SELECT * FROM a JOIN b` 里两张表共有同名列时必须显式限定；收益是语义没有隐式歧义。

**完全限定名短路，代价是额外的分支。** `if (db) return cur_field;` 让 `db.tbl.col` 立刻返回，跳过歧义检查。源码注释写明理由是限定名不可能重复。

**预处理语句用缓存换时间，接受「新加一列导致本该歧义的查询不再歧义」的不精确。** `item->cached_table` 分支跳过整轮遍历，源码注释原文承认：「With this optimization we also miss case when addition of one more field makes some prepared query ambiguous and so erroneous, but we accept this trade off」。

**视图列用「哨兵返回值 + 出参」表达，而不是统一返回 `Field`。** `view_ref_found`（值为 `(Field *)0x2`）与 `not_found_field`（值为 `(Field *)0x1`）是两个特殊指针，`WRONG_GRANT` 定义为 `(Field *)-1`。代价是调用方必须逐个比对这三个哨兵值；收益是三类「非普通 Field 的结果」在同一个返回通道里表达，`find_field_in_tables()` 因此能保持单一返回类型。

**NATURAL/USING 的结果列单独建一套数据结构。** `Natural_join_column` 列表让连接结果列与「表列」区分开，代价是查列路径多一层分派；收益是 NATURAL/USING 连接后的列名合并结果不必物化成真表。

**错误信息按「权限」与「存在性」分层。** `find_field_in_tables()` 末尾先查列权限再决定报「列不存在」还是报「无权访问」；`fix_outer_field()` 在向上搜索失败时只报笼统的 `ER_BAD_FIELD_ERROR`，源码注释说「We can't say exactly what absent table or field」——因为此时已无法确定到底是哪张表缺哪列。

**解析器一次做完所有绑定，代价是错误信息里的表名/列名可能来自外层。** `mark_as_dependent()` 在 EXPLAIN 时额外压 `ER_WARN_FIELD_RESOLVED` 提示，正是为了说明这个字段实际解析到了哪个外层 select number。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_resolver.cc` | `Query_block::resolve_placeholder_tables` | 解析本块的派生表/视图/表函数：合并或准备物化，是列绑定的前置环节 |
| `sql/sql_resolver.cc` | `Query_block::prepare` | 名称解析的编排：setup_tables → resolve_placeholder_tables → 视图权限 → 别名查找开关 → setup_fields |
| `sql/sql_resolver.cc` | `Query_block::setup_tables` | 建叶表列表、给表编号（上限常量 `MAX_TABLES`）、核对索引提示 |
| `sql/sql_resolver.cc` | `Query_block::merge_derived` | 把可合并的派生表并入当前查询块 |
| `sql/sql_resolver.cc` | `Query_block::transform_grouped_to_derived` | 把分组查询改写成派生表，改写后重新调 `resolve_placeholder_tables` |
| `sql/sql_resolver.cc` | `Query_block::check_only_full_group_by` | 分组语义校验：`Group_check::check_query` 与 `Distinct_check::check_query` |
| `sql/item.cc` | `Item_field::fix_outer_field` | 外层上下文链上的列搜索：先 FROM 后 SELECT/GROUP，命中后改写为 `Item_outer_ref` |
| `sql/item.cc` | `Item_field::fix_fields` | 单个列引用的绑定主流程：查表 → 查别名 → 回退外层 → set_field |
| `sql/item.cc` | `Item_field::set_field` | 把 `Field` 的类型、字符集、可空性、max_length 等属性复制到 Item 上并置 `fixed` |
| `sql/item.cc` | `resolve_ref_in_select_and_group` | 在 SELECT / GROUP BY 列表里解析别名与派生列，含前向引用报错 |
| `sql/item.cc` | `find_field_in_group_list` | GROUP BY 列表的匹配度打分查找，同分不同列时报 `ER_NON_UNIQ_ERROR` |
| `sql/item.cc` | `mark_as_dependent` | 标记跨查询块的依赖关系并沿上下文链传播 |
| `sql/item.cc` | `is_null_on_empty_table` | 判定列引用在聚合无分组场景下是否会被置 NULL |
| `sql/sql_base.cc` | `find_field_in_tables` | 查表入口：遍历 FROM 列表、歧义检测、错误分级与错误种类选择 |
| `sql/sql_base.cc` | `find_field_in_table_ref` | 按表引用形态分派：视图 / 基表 / NATURAL-USING 连接 |
| `sql/sql_base.cc` | `find_field_in_table` | 在基表的 `table->field` 数组里按名字找列，含 `_rowid` 回退 |
| `sql/sql_base.cc` | `find_field_in_view` | 在 MERGE 视图的列里查找，命中时返回 `view_ref_found` 并经出参给出视图表达式 |
| `sql/sql_base.cc` | `find_field_in_natural_join` | 在 `Natural_join_column` 列表里查找，第二个同名列报 `ER_NON_UNIQ_ERROR` |
| `sql/sql_base.cc` | `find_item_in_list` | 在 SELECT 列表里按别名查找，返回匹配类型 `RESOLVED_AGAINST_ALIAS` 等 |
| `sql/sql_base.cc` | `not_found_field` / `view_ref_found` / `WRONG_GRANT` | 三个特殊返回值哨兵：未找到、命中视图列、权限不足 |
| `sql/auth/sql_authorization.cc` | `check_column_grant_in_table_ref` | 列级权限检查；临时表与内部表直接跳过 |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 前端是手写递归下降，名称解析与语法分析不分离，列绑定直接写在语法动作里，没有「上下文链」与「外层引用」这一层结构。

**架构推论：** MySQL 这套机制里有三点结构值得主核借鉴。其一是**「查表」与「查别名」分层且顺序固定**：`Item_field::fix_fields()` 先扫 FROM 列表、再扫 SELECT 列表，两层用不同函数（`find_field_in_tables()` 与 `find_item_in_list()`）承担，语义边界清晰；主核若把别名解析与列解析混在一个函数里，改动一边很容易踩到另一边。其二是**「结果不是 Field」用哨兵值 + 出参表达**：`view_ref_found` / `not_found_field` / `WRONG_GRANT` 三个特殊指针让 `find_field_in_tables()` 保持单一返回类型，调用方逐个比对即可；这比「返回 `optional<variant<Field*, Item*>>`」在 C++ 里更容易跨层传递。其三是**「外层引用」被显式建模为上下文链上的 `outer_context`**，而不是靠递归传参隐式携带；代价是每个 `Item` 都要带一个上下文指针，收益是相关子查询的依赖传播（`mark_as_dependent()`）可以沿链一次走完。

对 AI 场景而言，名称解析是「模型生成 SQL」最容易翻车的一环：模型爱写 `SELECT a.id, b.name FROM ...` 而两张表共有 `id`，或者忘了限定表名。MySQL 的做法值得借鉴的是**错误信息分层**：表级错误报 `ER_UNKNOWN_TABLE`、列级歧义报 `ER_NON_UNIQ_ERROR`、列不存在报 `ER_BAD_FIELD_ERROR`、权限不足报列权限错误——这四类错误语义完全不同，若统一成「SQL 执行失败」，模型无法据此修正。代价是查列路径上要多带一个 `report_error` 枚举参数并在若干处分支判断。

## 参考

- [MySQL 源码：`sql/sql_resolver.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_resolver.cc)
- [MySQL 源码：`sql/item.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/item.cc)
- [MySQL 源码：`sql/sql_base.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_base.cc)
- [MySQL 源码：`sql/auth/sql_authorization.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/auth/sql_authorization.cc)
- [MySQL 参考手册：列、索引与别名](https://dev.mysql.com/doc/refman/8.0/en/column-indexes.html)
- [MySQL 参考手册：子查询](https://dev.mysql.com/doc/refman/8.0/en/subqueries.html)
- [MySQL 参考手册：`ONLY_FULL_GROUP_BY`](https://dev.mysql.com/doc/refman/8.0/en/sql-mode.html#sqlmode_only_full_group_by)
- [MySQL 参考手册：视图机制](https://dev.mysql.com/doc/refman/8.0/en/view-algorithms.html)
- [MySQL 参考手册：NATURAL 与 USING 连接](https://dev.mysql.com/doc/refman/8.0/en/join.html)
