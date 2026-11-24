---
project: mysql-server
feature: parse-tree-hints
page_type: feature
schema_version: 1
refs:
  - path: sql/parse_tree_hints.cc
    symbols: [PT_hint_list::contextualize, find_qb_hints]
  - path: sql/opt_hints.cc
    symbols: [Opt_hints::check_unresolved, Opt_hints_qb::subquery_strategy]
---

# 优化器提示的解析

## 一句话职责

MySQL 把 SQL 里的 `/*+ ... */` 优化器提示注释解析成结构化提示对象：词法阶段用独立扫描器 `Hint_scanner` 抽掉提示文本并单独跑一遍 Bison 语法（`sql/sql_hints.yy`），语法动作生成 `PT_hint` 解析树节点，随后在语义阶段由 `PT_hint_list::contextualize` 把每个提示落到 `Opt_hints` 对象树的对应层级（全局 / 查询块 / 表 / 索引），并对冲突、未解析、非法用法发出 `ER_WARN_CONFLICTING_HINT`、`ER_WARN_UNKNOWN_QB_NAME`、`ER_UNRESOLVED_HINT_NAME` 等警告。

## 机制是什么

**提示走的是一条独立于主 SQL 语法的双通道。** `sql/sql_lex.cc` 的 `consume_optimizer_hints()` 在主词法器扫到 `SG_HINTABLE_KEYWORDS` 组的关键字（`SELECT`、`INSERT`、`REPLACE`、`UPDATE`、`DELETE`）之后，检测下一个非空白字符是否为 `/*+`；是则构造 `Hint_scanner` 并调 `HINT_PARSER_parse()`，解析结果挂到 `lip->yylval->optimizer_hints` 上，主词法器随后 `lip->yySkipn(...)` 跳过整段提示文本。提示语法由 `sql/sql_hints.yy` 单独定义，主文法 `sql/sql_yacc.yy` 不参与。

**提示语法产生一棵「解析树节点」树。** `sql/sql_hints.yy` 的 `hint_list` 规则把每个 `hint` 收进 `PT_hint_list`，`hint` 规则分七类：`index_level_hint`、`table_level_hint`、`qb_level_hint`、`qb_name_hint`、`max_execution_time_hint`、`set_var_hint`、`resource_group_hint`。每类节点在语法动作里用 `NEW_PTN` 构造出对应的 `PT_*` 节点。

**语义阶段把提示树翻译成「提示对象树」。** 提示对象树由 `Opt_hints` 类族构成，四个层级：

- `Opt_hints_global` —— 全局层，挂在 `LEX::opt_hints_global` 上，由 `get_global_hints()` 创建。
- `Opt_hints_qb` —— 查询块层，挂在 `Query_block::opt_hints_qb` 上，由 `get_qb_hints()` 创建。
- `Opt_hints_table` —— 表层，由 `get_table_hints()` 在所属查询块下创建。
- `Opt_hints_key` —— 索引层，在表层下按索引名创建。

每个 `Opt_hints` 节点都用 `Opt_hints_map` 记录「哪些提示类型在本层被指定过」以及「开关是开还是关」。提示类型是否可继承到上层由 `st_opt_hint_info` 表驱动，该表在 `sql/opt_hints.cc` 里定义，`switch_hint` 与 `check_upper_lvl` 两个字段决定「提示是布尔开关型」还是「带参数的复合型」以及「是否允许向上层检查」。

**提示类型是固定枚举，不是动态注册的。** `st_opt_hint_info opt_hint_info[]` 表里逐条列出提示名与三个布尔属性（`switch_hint`、`check_upper_lvl`、`irregular_hint`），共 23 条有效条目（末项 `nullptr` 结束）。其中 `switch_hint` 为真的布尔型包括 `BKA`、`BNL`、`ICP`、`MRR`、`NO_RANGE_OPTIMIZATION`、`MERGE`、`JOIN_FIXED_ORDER`、`HASH_JOIN`、`DERIVED_CONDITION_PUSHDOWN`；复合型（带参数）包括 `SEMIJOIN`、`SUBQUERY`、`JOIN_PREFIX`、`JOIN_SUFFIX`、`JOIN_ORDER`、`INDEX_MERGE`、`SKIP_SCAN`、`INDEX`、`JOIN_INDEX`、`GROUP_INDEX`、`ORDER_INDEX`。

**「解析」与「生效」严格分离。** 本机制只负责把提示变成结构化对象并做冲突/合法性检查；提示对执行计划的实际影响（哪些索引可用、连接顺序、子查询策略）由优化器阶段的 `hint_table_state()`、`hint_key_state()`、`compound_hint_key_enabled()` 等读取函数完成，不属于本页范围。

## 怎么实现的

### 入口：`PT_hint_list::contextualize`

`sql/parse_tree_hints.cc` 的 `PT_hint_list::contextualize(Parse_context *pc)` 是提示语义化的总入口：

1. `if (super::contextualize(pc)) return true;` —— 先让基类完成通用解析树上下文化。
2. `if (!get_qb_hints(pc, pc->select)) return true;` —— 为当前查询块建 `Opt_hints_qb`；建不出来即视为失败。
3. 遍历 `hints` 列表，对每个非空节点：
   - `if (pc->thd->lex->sql_command == SQLCOM_CREATE_VIEW && !(*h)->supports_view()) continue;` —— 建视图时不支持的提示被静默跳过。
   - `if ((*h)->contextualize(pc)) return true;` —— 交给具体提示类型的 `contextualize` 实现。

### 提示对象的创建：`get_global_hints` / `get_qb_hints` / `get_table_hints`

- `get_global_hints(Parse_context *pc)`：从 `pc->thd->lex->opt_hints_global` 取全局提示对象，没有就在 `pc->thd->mem_root` 上 `new` 一个 `Opt_hints_global`，随后 `lex->opt_hints_global->set_resolved()`。
- `get_qb_hints(Parse_context *pc, Query_block *select)`：若 `select->opt_hints_qb` 已有则直接返回；否则用 `new (pc->thd->mem_root) Opt_hints_qb(global_hints, pc->thd->mem_root, select->select_number)` 建对象，并 `global_hints->register_child(qb)`、`select->opt_hints_qb = qb`、`qb->set_resolved()`。
- `get_table_hints(Parse_context *pc, Hint_param_table *table_name, Opt_hints_qb *qb)`：先 `qb->find_by_name(&table_name->table, table_alias_charset)` 找同名表对象，找不到就 `new` 一个 `Opt_hints_table` 并 `qb->register_child(tab)`。

三级对象的分配全部走 `MEM_ROOT`，生命周期绑定语句执行，不需要单独的释放路径。

### 查询块定位：`find_qb_hints`

`sql/parse_tree_hints.cc` 的 `find_qb_hints(Parse_context *pc, const LEX_CSTRING *qb_name, PT_hint *hint)` 负责把提示里的查询块名映射到 `Opt_hints_qb`：

- **未写查询块名。** `if (qb_name->length == 0) return pc->select->opt_hints_qb;` —— 直接落在当前查询块。
- **按名字查。** `pc->thd->lex->opt_hints_global->find_by_name(qb_name, system_charset_info)` 在全局提示对象树里查子节点；`Opt_hints::find_by_name` 逐个比较 `child_array[i]->get_print_name()`，用 `cmp_lex_string`（底层是 `cs->coll->strnncollsp`）比较。
- **系统名回退。** 查不到时遍历 `lex->all_query_blocks_list`，用 `snprintf(buff, sizeof(buff), "%s%x", "select#", select->select_number)` 拼出系统名（缓冲长度 `32`），与提示里的名字比较；命中则对该查询块调 `get_qb_hints()` 建出对象。源码注释说明这条路径的用途是「proper parsing of view body」。
- **查不到就告警。** `if (qb == nullptr) hint->print_warn(pc->thd, ER_WARN_UNKNOWN_QB_NAME, qb_name, nullptr, nullptr, nullptr);` 并返回 `nullptr`。

### 三类提示的语义化

**查询块级：`PT_qb_level_hint::contextualize`**

- `SEMIJOIN_HINT_ENUM` 与 `SUBQUERY_HINT_ENUM` 互斥：已有 `qb->semijoin_hint` 再给 `SUBQUERY` 记 `conflict = true`，反之亦然。
- `JOIN_PREFIX_HINT_ENUM` / `JOIN_SUFFIX_HINT_ENUM` 与 `JOIN_FIXED_ORDER_HINT_ENUM` 互斥：若 `qb->get_switch(type())` 或 `qb->get_switch(JOIN_FIXED_ORDER_HINT_ENUM)` 已开，记冲突；否则 `qb->register_join_order_hint(this)`。
- `JOIN_ORDER_HINT_ENUM` 与 `JOIN_FIXED_ORDER_HINT_ENUM` 互斥，但自身可重复：`no_warn = true`（源码注释：多个 JOIN_ORDER 提示可以同时存在），照常 `qb->register_join_order_hint(this)`。
- `JOIN_FIXED_ORDER_HINT_ENUM` 生效时执行 `pc->select->add_base_options(SELECT_STRAIGHT_JOIN)` —— 这是提示直接改写查询块选项的唯一一处。
- 冲突或重复：`if (conflict || (qb->set_switch(switch_on(), type(), false) && !no_warn)) print_warn(pc->thd, ER_WARN_CONFLICTING_HINT, ...)`。

**表级：`PT_table_level_hint::contextualize`**

- `table_list` 为空表示这是查询块级提示，走 `find_qb_hints` + `qb->set_switch(switch_on(), type(), false)`。
- 非空时逐表处理：`const LEX_CSTRING *qb_name_str = qb_name.length > 0 ? &qb_name : &table_name->opt_query_block;` —— 语法 `@qb_name tbl` 用 `qb_name`，语法 `tbl@qb_name` 用 `table_name->opt_query_block`（源码注释明确说明这两种语法的分工）。
- 定位到表后 `tab->set_switch(switch_on(), type(), true)`，若返回真（已指定过）则报 `ER_WARN_CONFLICTING_HINT`。

**索引级：`PT_key_level_hint::contextualize`**

- 先 `find_qb_hints(pc, &table_name.opt_query_block, this)` 定位查询块，再 `get_table_hints(pc, &table_name, qb)` 定位表对象。
- `key_list` 为空表示表级提示；若类型是复合提示（`is_compound_hint(type())`）且 `tab->get_compound_key_hint(type())->is_hint_conflicting(tab, nullptr)` 成立，报冲突并返回。
- **非法用法检查。** `if (type() == INDEX_MERGE_HINT_ENUM && key_list.size() == 1 && switch_on())` → 报 `ER_WARN_INVALID_HINT`。源码依据：只指定一个索引的 `INDEX_MERGE` 没有意义。
- 逐索引处理：按索引名 `tab->find_by_name(key_name, system_charset_info)` 查找，找不到就新建 `Opt_hints_key` 并 `tab->register_child(key)`。
- **重复与冲突判定。** `bool is_specified = tab->is_specified(type()) || key->is_specified(type());` —— 已指定且不是复合提示，报 `ER_WARN_CONFLICTING_HINT` 并 `continue`；若是复合提示且 `is_hint_conflicting(tab, key)` 成立，置 `is_conflicting = true` 并 `break`。
- 收尾：`if (!is_conflicting)` 才逐个 `key->set_switch(switch_on(), type(), true)`；复合提示额外执行 `tab->get_compound_key_hint(type())->set_pt_hint(this)` 与 `(void)tab->set_switch(switch_on(), type(), false)`。

**复合提示的判定：`is_compound_hint`**（`sql/opt_hints.cc`）返回真的类型共 6 个：`INDEX_MERGE_HINT_ENUM`、`SKIP_SCAN_HINT_ENUM`、`INDEX_HINT_ENUM`、`JOIN_INDEX_HINT_ENUM`、`GROUP_INDEX_HINT_ENUM`、`ORDER_INDEX_HINT_ENUM`。复合型提示允许在同一张表上多次出现并累积索引列表。

### 名字类与全局类提示

**`PT_hint_qb_name::contextualize`** 给当前查询块命名。冲突判定是两路：`qb->get_name()` 已非空（名字已设过）或 `qb->get_parent()->find_by_name(&qb_name, system_charset_info)` 非空（名字已被别的块占用）→ 报 `ER_WARN_CONFLICTING_HINT`；否则 `qb->set_name(&qb_name)`。

**`PT_hint_max_execution_time::contextualize`** 有三重前置条件：`pc->thd->lex->sql_command != SQLCOM_SELECT`（不是 SELECT）、`pc->thd->lex->sphead`（在存储程序/触发器/事件里）、`pc->select != pc->thd->lex->query_block`（在子查询里），任一成立则压 `ER_WARN_UNSUPPORTED_MAX_EXECUTION_TIME` 警告并返回。重复指定（`global_hint->is_specified(type())`）报 `ER_WARN_CONFLICTING_HINT`。最终落到 `pc->thd->lex->max_execution_time = milliseconds;`。

**`PT_hint_sys_var::contextualize`**（`SET_VAR` 提示）先 `System_variable_tracker::make_tracker(...)` 再 `access_system_variable(pc->thd, {}, Suppress_not_found_error::YES)`：变量不存在报 `ER_UNRESOLVED_HINT_NAME`（提示名固定为 `"SET_VAR"`）；`!var_tracker.is_hint_updateable()` 报 `ER_NOT_HINT_UPDATABLE_VARIABLE`。变量可改时存入 `global_hint->sys_var_hint`（`Sys_var_hint` 对象）的 `var_list`。

**`PT_hint_resource_group::contextualize`** 两道检查：`resourcegroups::Resource_group_mgr::instance()->resource_group_support()` 为假 → `WARN_RESOURCE_GROUP_UNSUPPORTED`；`pc->thd->lex->sphead` 或 `pc->select != pc->thd->lex->query_block` → `WARN_RESOURCE_GROUP_UNSUPPORTED_HINT`。通过后把资源组名 memcpy 进 `pc->thd->resource_group_ctx()->m_switch_resource_group_str`。

### 语法阶段的两处硬性数字

- **`MAX_EXECUTION_TIME` 的上限。** `sql/sql_hints.yy` 的 `max_execution_time_hint` 规则用 `parse_int(&n, $3.str, $3.length) || n > UINT_MAX32` 判定，超过 `UINT_MAX32` 就调 `scanner->syntax_warning(ER_THD(thd, ER_WARN_BAD_MAX_EXECUTION_TIME))` 并把节点置 `NULL`。
- **半连接策略的位掩码。** `sql/sql_hints.yy` 的 `semijoin_strategy` 规则把 `FIRSTMATCH_HINT`、`LOOSESCAN_HINT`、`MATERIALIZATION_HINT`、`DUPSWEEDOUT_HINT` 分别映射到 `OPTIMIZER_SWITCH_FIRSTMATCH`、`OPTIMIZER_SWITCH_LOOSE_SCAN`、`OPTIMIZER_SWITCH_MATERIALIZATION`、`OPTIMIZER_SWITCH_DUPSWEEDOUT`，多个策略用按位或合并。

### 未解析提示的检查：`Opt_hints::check_unresolved`

`sql/opt_hints.cc` 的 `Opt_hints::check_unresolved(THD *thd)` 递归遍历对象树：

- `if (!is_resolved(MAX_HINT_ENUM)) print_warn_unresolved(thd);` —— `MAX_HINT_ENUM` 是「全部分析完成」的哨兵值，与具体提示类型不冲突。
- `if (!is_all_resolved())` 才递归 `child_array[i]->check_unresolved(thd)`。

`print_warn_unresolved(THD *thd)` 遍历 `i < MAX_HINT_ENUM` 的所有提示类型，凡 `is_specified(...)` 成立就 `push_warning_printf(thd, Sql_condition::SL_WARNING, ER_UNRESOLVED_HINT_NAME, ...)`，并调 `get_parent()->set_unresolved(static_cast<opt_hints_enum>(i))` 向上传播「未解析」状态。

**真实调用方：** `sql/sql_resolver.cc` 的 `Query_block::setup_tables()` 末尾有 `if (opt_hints_qb && !select_insert) opt_hints_qb->check_unresolved(thd);`。该处源码注释记录了已知的不精确：「consider calling this from SELECT::prepare() instead. It might save the test on select_insert to prevent check_unresolved() from being called twice for INSERT ... SELECT.」——即 `INSERT ... SELECT` 场景下检查会执行两次。

**解析状态的推进。** `Opt_hints_table::adjust_key_hints(Table_ref *tr)` 负责把索引级提示与真实索引对上：先 `set_resolved()`；若 `child_array_ptr()->size() == 0`（无索引级提示）则 `get_parent()->incr_resolved_children()` 返回；`if (keyinfo_array.size()) return;` 保证只调整一次；`if (!tr->is_base_table()) return;` —— 派生表、内部临时表、表函数没有索引名，跳过。之后 `keyinfo_array.resize(table->s->keys, nullptr)`，逐个 `KEY` 用 `cmp_lex_string` 比对索引名，命中则 `(*hint)->set_resolved()`、`keyinfo_array[j] = ...`、`incr_resolved_children()`、`set_compound_key_hint_map(*hint, j)`。收尾 `if (is_all_resolved()) get_parent()->incr_resolved_children();` —— 源码注释解释：不增加已解析计数是「important for check_unresolved() function」。

**提示与表引用的绑定。** `Opt_hints_qb::adjust_table_hints(Table_ref *tr)` 用 `tr->alias` 在本块找 `Opt_hints_table`（比较用 `table_alias_charset`），并设置 `tr->opt_hints_qb = this`；找不到表就返回 `nullptr`。真实调用方是 `sql/sql_resolver.cc` 的 `Query_block::setup_tables()`：在 `!tr->opt_hints_table` 时执行 `tr->opt_hints_table = opt_hints_qb->adjust_table_hints(tr);`。紧随其后是同一函数里的分叉：`if (!tr->opt_hints_table || !tr->opt_hints_table->update_index_hint_maps(thd, tr->table))` 成立时**回退到** `tr->process_index_hints(thd, table)`——即老式 `USE INDEX` / `FORCE INDEX` / `IGNORE INDEX` 语法；源码注释写明「Ignore old index hint processing if new style hints are specified」。

### 提示结果的读取：`Opt_hints_qb::subquery_strategy`

`sql/opt_hints.cc` 的 `Opt_hints_qb::subquery_strategy()` 是提示被优化器消费的一个具体例子：

```
if (subquery_hint) return static_cast<Subquery_strategy>(subquery_hint->get_args());
return Subquery_strategy::UNSPECIFIED;
```

**真实调用方：** `sql/sql_lex.cc` 的 `Query_block::choose_subquery_strategy()` 里 `if (opt_hints_qb) { Subquery_strategy strategy = opt_hints_qb->subquery_strategy(); if (strategy != Subquery_strategy::UNSPECIFIED) return strategy; }`。注释写着「No SUBQUERY hint given, base possible strategies on optimizer_switch」——即提示值优先，未指定时才落到 `thd->optimizer_switch_flag(OPTIMIZER_SWITCH_MATERIALIZATION)` 等开关。

同族函数 `Opt_hints_qb::semijoin_enabled(const THD *thd)` 演示了「提示覆盖开关」的语义：`subquery_hint` 存在则返回 `false`（SUBQUERY 提示禁用半连接）；`semijoin_hint` 存在且 `switch_on()` 为真则返回 `true`（源码注释：SEMIJOIN 提示无条件强制半连接）；`NO_SEMIJOIN` 且 `get_args() == 0` 返回 `false`；否则**回退到** `thd->optimizer_switch_flag(OPTIMIZER_SWITCH_SEMIJOIN)`。

`Opt_hints_qb::sj_enabled_strategies(uint opt_switches)` 处理策略掩码：`SEMIJOIN` 提示下 `strategies == 0` 时返回全部 `opt_switches`，否则返回 `strategies`；`NO_SEMIJOIN` 下返回 `~strategies & opt_switches`。

### 告警的统一出口：`PT_hint::print_warn`

`sql/parse_tree_hints.cc` 的 `PT_hint::print_warn` 负责把各类冲突/非法提示渲染成一条警告文本：

- 先拼提示名：`if (!state) str.append(STRING_WITH_LEN("NO_"));` 再 `str.append(opt_hint_info[hint_type].hint_name);` —— `NO_` 前缀由提示自身的 `state` 决定，而不是由 `opt_hint_info` 表决定。
- `ER_WARN_UNKNOWN_QB_NAME` 走特殊分支：文本形如「未知查询块名 + 提示名」，两个参数，末尾直接 return（不追加括号）。
- 其余错误码走统一分支：追加 `'('`，依次拼表名（`table_name_arg`）、`@` 加查询块名（`qb_name_arg`）、空格加索引名（`key_name_arg`），若还有额外参数则调 `hint->append_args(thd, &str)`，最后 `str.append(')')`。
- 最终 `push_warning_printf(thd, Sql_condition::SL_WARNING, err_code, ER_THD_NONCONST(thd, err_code), str.c_ptr_safe())`。

**提示名与参数的回显：** `PT_qb_level_hint::append_args` 按类型分派——`SEMIJOIN` 逐位检查 `args & OPTIMIZER_SWITCH_FIRSTMATCH`、`OPTIMIZER_SWITCH_LOOSE_SCAN`、`OPTIMIZER_SWITCH_MATERIALIZATION`、`OPTIMIZER_SWITCH_DUPSWEEDOUT`，用 `count` 变量控制逗号分隔；`SUBQUERY` 按 `Subquery_strategy` 分派（`SUBQ_MATERIALIZATION` → `" MATERIALIZATION"`，`SUBQ_EXISTS` → `" INTOEXISTS"`）；`JOIN_PREFIX` / `JOIN_SUFFIX` / `JOIN_ORDER` 遍历 `table_list` 逐个调 `append_table_name`。

### 提示的回显：`Opt_hints::print`

`sql/opt_hints.cc` 的 `Opt_hints::print(const THD *thd, String *str, enum_query_type query_type)` 遍历 `i < MAX_HINT_ENUM` 的提示类型，跳过 `opt_hint_info[i].irregular_hint` 为真的类型；输出条件是 `is_specified(hint) && !ignore_print(hint) && (is_resolved(hint) || query_type == QT_NORMALIZED_FORMAT)` —— 源码注释解释最后一项：打印规范化查询时未解析的提示也要打印，因为查询重写插件会在解析前请求规范化形式。随后调 `print_irregular_hints(thd, str)`，最后递归打印子节点。

`Opt_hints_qb::print_irregular_hints` 单独处理连接顺序提示：遍历 `join_order_hints`，用 `join_order_hints_ignored & (1ULL << i)` 跳过被忽略的项（该位掩码由 `Opt_hints_qb::apply_join_order_hints(JOIN *join)` 置位——`set_join_hint_deps()` 返回真时 `join_order_hints_ignored |= 1ULL << hint_idx`，源码注释说这是「Skip hint printing in EXPLAIN message」）。

### 语法错误的处理

`sql/sql_lex_hints.cc` 的 `HINT_PARSER_error(THD *thd, Hint_scanner *scanner, PT_hint_list **, const char *msg)`：若 `strcmp(msg, "syntax error") == 0` 则替换为 `ER_THD(thd, ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR)`，再调 `scanner->syntax_warning(msg)`。`Hint_scanner::syntax_warning` 用 `push_warning_printf(thd, Sql_condition::SL_WARNING, ER_PARSE_ERROR, ...)` 压警告——注意它复用的是 `ER_PARSE_ERROR` 这个错误码，文案由参数 `msg` 提供。

`Hint_scanner::add_hint_token_digest()` 负责把提示 token 计入查询摘要（digest）：首个提示时加 `TOK_HINT_COMMENT_OPEN`，结束时加 `TOK_HINT_COMMENT_CLOSE`；`HINT_ARG_IDENT` 后紧跟 `@` 时加 `TOK_IDENT_AT` 否则加 `IDENT`；`HINT_ARG_QB_NAME` 加 `'@'` 加 `IDENT`；数字类加 `NUM`；文本类加 `TEXT_STRING`。

**语法错误的降级路径：** `sql/sql_lex.cc` 的 `consume_optimizer_hints()` 里 `HINT_PARSER_parse()` 返回 `2` 是 Bison 内部 OOM，直接 `return true`；返回 `1`（YYABORT 或注释打开失败）时先 `lip->start_token()` 把错误文本指针调整到 `"/*+"` 再 `return true`。返回 `0` 时才把 `hint_list` 挂到 `lip->yylval->optimizer_hints`——源码注释标注「NULL in case of syntax error」。

## 关键取舍

**提示语法独立于主语法，代价是提示写错不影响主查询。** `sql/sql_lex.cc` 的 `consume_optimizer_hints()` 通过 `HINT_PARSER_parse()` 单独解析，语法错误只压警告（`ER_PARSE_ERROR` 携带 `ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR` 文案），主查询照常执行。收益是「提示写错不致命」，代价是提示语法与主语法各维护一份，且提示里的标识符不参与主查询的名称解析。

**提示语法与提示语义分两阶段，但 `PT_*::contextualize` 又是逐节点立即执行。** 语法阶段产出整棵 `PT_hint` 树，语义阶段由 `PT_hint_list::contextualize` 逐节点调 `contextualize`。收益是每类提示的语义规则可以独立演进；代价是 `contextualize` 里散落着「建对象」「查对象」「判冲突」「发警告」四类职责，跨类的冲突判定无法集中在一处。

**提示对象树用「父指针 + 子数组」而非多态继承树。** `Opt_hints` 基类持有 `child_array`，`Opt_hints_global` / `Opt_hints_qb` / `Opt_hints_table` / `Opt_hints_key` 通过 `parent` 指针串成树。收益是 `find_by_name()` 一个函数通吃四层查找；代价是类型判定靠 `static_cast` 硬转（`find_qb_hints` 里 `static_cast<Opt_hints_qb *>(...find_by_name(...))`），编译器无法校验。

**冲突一律降级为警告，不中断执行。** 所有冲突路径走 `PT_hint::print_warn` → `push_warning_printf(thd, Sql_condition::SL_WARNING, ...)`。唯一的例外是 `MAX_EXECUTION_TIME` 超上限与 `SET_VAR` 变量不存在，这两类在语法阶段就由 `scanner->syntax_warning()` / `push_warning` 处理，同样只是警告。

**「已指定过」的判定放在三处，且对复合提示有专门放宽。** `PT_key_level_hint::contextualize` 里 `is_specified = tab->is_specified(type()) || key->is_specified(type())`，但 `!is_compound_hint(type())` 才立即报冲突——复合提示允许重复出现并累积索引列表。`is_compound_hint` 覆盖 6 个类型，是「能否重复」的唯一判据。

**未解析提示的检查放在表建立之后，而不是解析当场。** `check_unresolved` 由 `Query_block::setup_tables()` 调用，此时索引名已经能对上真实 `KEY`，`adjust_key_hints` 才能判断提示是否落地。代价是 `INSERT ... SELECT` 场景下检查会执行两次（源码注释已记录此事）。

**新旧索引提示二选一，靠「表上是否挂了解析后的提示对象」判断。** `sql/sql_resolver.cc` 的 `Query_block::setup_tables()` 里 `if (!tr->opt_hints_table || !tr->opt_hints_table->update_index_hint_maps(thd, tr->table))` 成立时**回退到** `tr->process_index_hints(thd, table)` 处理老式 `USE INDEX` 语法。源码注释写明「Ignore old index hint processing if new style hints are specified」。

**系统生成的查询块名用「前缀 + 十六进制序号」。** `sql/opt_hints.cc` 的 `const LEX_CSTRING sys_qb_prefix = {"select#", 7};`，`Opt_hints_qb` 构造函数用 `snprintf(buff, sizeof(buff), "%s%x", sys_qb_prefix.str, select_number)` 拼出系统名。`find_qb_hints` 用同样的格式去匹配用户写的系统名（注释说明用途是「proper parsing of view body」）。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/parse_tree_hints.cc` | `PT_hint_list::contextualize` | 提示语义化总入口：建查询块提示对象、逐个提示下发给 `contextualize`、建视图时跳过不支持的提示 |
| `sql/parse_tree_hints.cc` | `find_qb_hints` | 按查询块名定位 `Opt_hints_qb`：无名取当前块，有名先查对象树再按系统名 `select#<hex>` 回退，都失败报 `ER_WARN_UNKNOWN_QB_NAME` |
| `sql/parse_tree_hints.cc` | `get_global_hints` | 建/取全局提示对象 `Opt_hints_global`，挂在 `LEX::opt_hints_global` 上 |
| `sql/parse_tree_hints.cc` | `get_qb_hints` | 建/取查询块级提示对象 `Opt_hints_qb`，注册为全局对象的子节点 |
| `sql/parse_tree_hints.cc` | `get_table_hints` | 在查询块下按表名建/取 `Opt_hints_table` |
| `sql/parse_tree_hints.cc` | `PT_hint::print_warn` | 统一告警出口：拼提示名（含 `NO_` 前缀）、表名、`@查询块名`、索引名与参数，压 `ER_WARN_CONFLICTING_HINT` 等警告 |
| `sql/parse_tree_hints.cc` | `PT_qb_level_hint::contextualize` | 查询块级提示：`SEMIJOIN`/`SUBQUERY`/`JOIN_PREFIX`/`JOIN_SUFFIX`/`JOIN_ORDER`/`JOIN_FIXED_ORDER` 的互斥与冲突判定 |
| `sql/parse_tree_hints.cc` | `PT_table_level_hint::contextualize` | 表级提示：处理 `@qb_name tbl` 与 `tbl@qb_name` 两种语法，定位后写表级开关 |
| `sql/parse_tree_hints.cc` | `PT_key_level_hint::contextualize` | 索引级提示：建 `Opt_hints_key`、判重复与复合提示冲突、检 `INDEX_MERGE` 只给一个索引的非法用法 |
| `sql/parse_tree_hints.cc` | `PT_hint_qb_name::contextualize` | 给查询块命名，检测「本块已命名」与「名字被别的块占用」两类冲突 |
| `sql/parse_tree_hints.cc` | `PT_hint_max_execution_time::contextualize` | `MAX_EXECUTION_TIME` 提示的三重前置校验与重复检测，最终写 `LEX::max_execution_time` |
| `sql/parse_tree_hints.cc` | `PT_hint_sys_var::contextualize` | `SET_VAR` 提示：查变量是否存在、是否可被 hint 更新，报 `ER_UNRESOLVED_HINT_NAME` / `ER_NOT_HINT_UPDATABLE_VARIABLE` |
| `sql/parse_tree_hints.cc` | `PT_hint_resource_group::contextualize` | `RESOURCE_GROUP` 提示：查资源组支持与所在上下文，写入 `resource_group_ctx` |
| `sql/opt_hints.cc` | `Opt_hints::check_unresolved` | 递归检查未解析提示并向上传播未解析状态 |
| `sql/opt_hints.cc` | `Opt_hints::print_warn_unresolved` | 逐提示类型扫 `is_specified`，对未解析项压 `ER_UNRESOLVED_HINT_NAME` |
| `sql/opt_hints.cc` | `Opt_hints_qb::subquery_strategy` | 返回 `SUBQUERY` 提示指定的子查询策略，无提示则返回 `Subquery_strategy::UNSPECIFIED` |
| `sql/opt_hints.cc` | `Opt_hints_qb::semijoin_enabled` | `SEMIJOIN`/`NO_SEMIJOIN`/`SUBQUERY` 提示对半连接开关的覆盖逻辑，无提示时回退到 `OPTIMIZER_SWITCH_SEMIJOIN` |
| `sql/opt_hints.cc` | `Opt_hints_qb::sj_enabled_strategies` | 计算半连接策略掩码：`SEMIJOIN` 取策略位，`NO_SEMIJOIN` 取 `~strategies & opt_switches` |
| `sql/opt_hints.cc` | `Opt_hints_qb::adjust_table_hints` | 按表别名定位 `Opt_hints_table` 并把查询块提示对象挂到 `Table_ref::opt_hints_qb` |
| `sql/opt_hints.cc` | `Opt_hints_table::adjust_key_hints` | 用真实 `KEY` 名字对齐索引级提示，标记已解析并推进已解析子节点计数 |
| `sql/opt_hints.cc` | `Opt_hints::find_by_name` | 在子数组里按打印名查找，用 `cmp_lex_string` 比较 |
| `sql/opt_hints.cc` | `is_compound_hint` | 判定 6 个复合型提示（`INDEX_MERGE`/`SKIP_SCAN`/`INDEX`/`JOIN_INDEX`/`GROUP_INDEX`/`ORDER_INDEX`），决定是否允许重复累积 |
| `sql/opt_hints.cc` | `Opt_hints::print` | 回显提示：`MAX_HINT_ENUM` 范围内遍历，跳过 `irregular_hint` 类型，未解析项仅在规范化格式下输出 |
| `sql/opt_hints.cc` | `st_opt_hint_info opt_hint_info[]` | 提示类型元表：`hint_name` 与 `switch_hint`/`check_upper_lvl`/`irregular_hint` 三个布尔属性 |
| `sql/opt_hints.cc` | `sys_qb_prefix` | 系统查询块名前缀 `"select#"`，与 `select_number` 十六进制拼成系统名 |
| `sql/sql_lex.cc` | `consume_optimizer_hints` | 词法入口：识别 `/*+`、构造 `Hint_scanner`、调 `HINT_PARSER_parse`、跳过提示文本并挂到 `optimizer_hints` |
| `sql/sql_lex.cc` | `Query_block::choose_subquery_strategy` | 提示生效点：优先用 `Opt_hints_qb::subquery_strategy()`，无提示时回退到 `optimizer_switch` |
| `sql/sql_lex_hints.cc` | `Hint_scanner::syntax_warning` | 提示语法错误压警告，复用 `ER_PARSE_ERROR` 错误码并附带行号 |
| `sql/sql_lex_hints.cc` | `Hint_scanner::add_hint_token_digest` | 把提示 token 计入查询摘要 |
| `sql/sql_hints.yy` | `hint_list` / `hint` | 提示文法入口与七类提示的分类规则 |
| `sql/sql_hints.yy` | `max_execution_time_hint` | `MAX_EXECUTION_TIME` 文法动作：解析整数并以 `UINT_MAX32` 为上限，超限报 `ER_WARN_BAD_MAX_EXECUTION_TIME` |
| `sql/sql_hints.yy` | `semijoin_strategy` | 半连接策略文法：映射到 `OPTIMIZER_SWITCH_FIRSTMATCH` / `LOOSE_SCAN` / `MATERIALIZATION` / `DUPSWEEDOUT` 四个开关位 |
| `sql/sql_resolver.cc` | `Query_block::setup_tables` | 提示与真实表/索引对齐的时机；末尾调 `check_unresolved`；无新式提示时回退到 `tr->process_index_hints` |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 前端是手写递归下降，没有「提示」这一层概念，解析器与执行器之间也不存在独立的提示对象树。

**架构推论：** MySQL 这套提示机制里有三点结构值得主核借鉴。其一是**「解析」与「生效」严格分层，且中间隔着一棵可遍历、可回显的对象树**：`PT_hint_list::contextualize` 只负责建对象与判冲突，实际影响由优化器阶段的 `hint_table_state()` / `hint_key_state()` / `compound_hint_key_enabled()` 读取；这让提示可以在不改动优化器的前提下增删类型。代价是提示对象树必须能被完整回显（`Opt_hints::print`），否则用户无法确认提示是否被接受。其二是**「每种提示一个 `contextualize` 虚函数 + 一张元数据表」的双轨登记**：行为在 `PT_*::contextualize` 里，名字与属性在 `st_opt_hint_info` 表里；代价是新增提示要同时改语法、虚函数、元数据三处，好处是元数据表可以驱动回显与冲突判定（`Opt_hints::print` 遍历的正是 `opt_hint_info` 覆盖的类型）。其三是**「提示写错不致命」的错误策略**：所有冲突路径都走 `push_warning_printf(..., Sql_condition::SL_WARNING, ...)`，只有语法错误与超限才是另一条路；代价是用户可能忽略警告导致提示静默失效。

对 AI 场景而言，提示解析是「模型生成 SQL」的高杠杆点：模型可以给不确定的查询加上 `/*+ MAX_EXECUTION_TIME(...) */` 或 `/*+ JOIN_ORDER(t1,t2) */` 来消歧，但模型往往写不对提示语法（引号、逗号、`@` 位置），且不知道自己写错了。MySQL 的做法值得借鉴的是**「提示语法错误降级为警告、不影响主查询」**这条边界——模型生成带提示的 SQL 即使提示无效，查询仍能返回数据，模型可以从警告里拿到具体原因（`ER_WARN_OPTIMIZER_HINT_SYNTAX_ERROR`、`ER_WARN_CONFLICTING_HINT`、`ER_UNRESOLVED_HINT_NAME`）再自我修正。代价是这类错误不会体现在执行失败率里，需要专门采集警告才可见。

## 参考

- [MySQL 源码：`sql/parse_tree_hints.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/parse_tree_hints.cc)
- [MySQL 源码：`sql/opt_hints.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/opt_hints.cc)
- [MySQL 源码：`sql/sql_hints.yy`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_hints.yy)
- [MySQL 源码：`sql/sql_lex_hints.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex_hints.cc)
- [MySQL 源码：`sql/sql_lex.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex.cc)
- [MySQL 源码：`sql/sql_resolver.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_resolver.cc)
- [MySQL 参考手册：优化器提示](https://dev.mysql.com/doc/refman/8.0/en/optimizer-hints.html)
- [MySQL 参考手册：索引提示（`USE INDEX` / `FORCE INDEX` / `IGNORE INDEX`）](https://dev.mysql.com/doc/refman/8.0/en/index-hints.html)
- [MySQL 参考手册：`optimizer_switch` 系统变量](https://dev.mysql.com/doc/refman/8.0/en/server-system-variables.html#sysvar_optimizer_switch)
- [MySQL 参考手册：半连接（semijoin）策略](https://dev.mysql.com/doc/refman/8.0/en/semijoin.html)
- [MySQL 参考手册：子查询优化与 `SUBQUERY` 提示](https://dev.mysql.com/doc/refman/8.0/en/subquery-optimization.html)
