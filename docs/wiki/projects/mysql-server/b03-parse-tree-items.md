---
project: mysql-server
feature: parse-tree-items
page_type: feature
schema_version: 1
refs:
  - path: sql/parse_tree_items.cc
    symbols: [PTI_comp_op::itemize, PTI_user_variable::itemize]
---

# 解析树到 Item 的转换

## 一句话职责

`sql/parse_tree_items.cc` 是 MySQL 解析阶段把「语法树节点」翻译成「可求值的 Item 表达式对象」的那一层：语法分析器在 `sql/sql_yacc.yy` 里用 `NEW_PTN PTI_xxx(...)` 造出 `PTI_*` 节点，`PTI_*::itemize()` 随后把每个节点变成 `Item_field`（列引用）、`Item_int`（常量）、`Item_func_*`（函数与运算）、`Item_singlerow_subselect`（标量子查询）这样的表达式树节点，并把「名字绑到表列」「函数名解析到内置函数 / UDF / 存储函数」「视图里不许出现变量」等**语义约束**在这一层就地落实。

## 机制是什么

**这一层是 bison 语义动作与语义分析之间的桥。** `sql/sql_yacc.yy` 的语法规则在归约时构造 `PTI_*` 节点（`PTI_comp_op`、`PTI_comp_op_all`、`PTI_user_variable`、`PTI_count_sym`、`PTI_odbc_date`、`PTI_text_literal_text_string`、`PTI_simple_ident_ident` 等），但节点本身只记录「语法上长什么样」；把语法形状变成「运行时怎么算」的是 `itemize()`。`PTI_comp_op::itemize` 的函数体只有三段：先让基类 `super::itemize()` 与左右操作数各自完成 `itemize`，再执行 `*res = (*boolfunc2creator)(false)->create(left, right)`，最后返回 `*res == nullptr`（即「分配失败才算错」）。

**转换是自顶向下递归的。** 每个 `PTI_*::itemize` 的典型形状是「先 `super::itemize(pc, res)`，再逐个把子节点 `itemize` 掉，最后造出真正的 `Item`」。子节点的 `itemize` 递归下去，整棵表达式树因此一次性建好。`PTI_comp_op_all::itemize` 展示了另一种形状：子查询节点不是 `Item` 而是解析树节点，所以它调的是 `subselect->contextualize(pc)` 而不是 `itemize`，然后把 `subselect->value()` 交给 `all_any_subquery_creator`。

**这一层顺带做语义收紧，而不是只做形状转换。** 例子有三类：

1. **视图里禁止变量。** `PTI_user_variable::itemize` 先查 `lex->parsing_options.allows_variable`，不满足就 `my_error(ER_VIEW_SELECT_VARIABLE, MYF(0))` 返回 true（错误）。同一检查也出现在 `PTI_variable_aux_set_var::itemize`、`PTI_get_system_variable::itemize`、`PTI_simple_ident_ident::itemize` 的存储程序分支里。
2. **语句可缓存性标记。** `PTI_user_variable::itemize` 调 `lex->set_uncacheable(pc->select, UNCACHEABLE_RAND)`；`PTI_function_call_nonkeyword_now::itemize`、`PTI_int_splocal::itemize`、`PTI_function_call_nonkeyword_sysdate::itemize` 则置 `lex->safe_to_cache_query = false`。
3. **复制安全性标记。** `PTI_function_call_nonkeyword_sysdate::itemize` 调 `lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION)`，函数注释写明理由：SYSDATE() 不受 `TIMESTAMP` 变量影响，即使本机 `sysdate_is_now=1`，从库也可能是 0，因此始终不安全。

**名字绑定按上下文分成两类 Item。** 裸标识符 `PTI_simple_ident_ident::itemize` 在 `pc->select->parsing_place != CTX_HAVING` 或 `pc->select->get_in_sum_expr() > 0` 时造 `Item_field`，否则造 `Item_ref`（源码注释在 `Item_ref` 那行标了 `/* For HAVING */`）。`PTI_simple_ident_q_2d`（`tbl.col`）、`PTI_simple_ident_q_3d`（`db.tbl.col`）、`PTI_simple_ident_nospvar_ident` 三者用的是同一套判断。

**函数名解析有明确的优先级顺序。** `PTI_function_call_generic_ident_sys::itemize` 的函数注释直接写出了顺序：MySQL 原生函数 → UDF → 存储函数（假定当前 `<use>` 库）。实现上先用 `find_native_function_builder(ident)` 找原生函数；找不到再看之前 `find_udf()` 找到的 UDF；两者都没有才用 `find_qualified_function_builder(thd)` 走存储函数路径（源码注释注明将来会随 WL#2128 SQL PATH 调整）。

**常量与字面量也要在这一层定型。** `PTI_text_literal_text_string::itemize` 决定字符串字面量的字符集与 repertoire；`PTI_text_literal_nchar_string::itemize` 用 `national_charset_info` 初始化；`PTI_temporal_literal::itemize` 调 `create_temporal_literal`；`PTI_odbc_date::itemize` 尝试把 `{d'2001-01-01'}` 这类 ODBC 字面量识别成日期/时间类型。

## 怎么实现的

### `PTI_comp_op::itemize`：比较运算符

`sql/parse_tree_items.cc` 里 `PTI_comp_op::itemize` 的完整函数体是：

```cpp
bool PTI_comp_op::itemize(Parse_context *pc, Item **res) {
  if (super::itemize(pc, res) || left->itemize(pc, &left) ||
      right->itemize(pc, res))
    return true;

  *res = (*boolfunc2creator)(false)->create(left, right);
  return *res == nullptr;
}
```

关键在于 `boolfunc2creator` 这个成员不是函数而是指针。`sql/sql_yacc.yy` 里 `%type <boolfunc2creator> comp_op` 声明了它的语义类型，`comp_op` 规则把 `EQ` 映射到 `&comp_eq_creator`、`EQUAL_SYM` 映射到 `&comp_equal_creator`、`GE` 映射到 `&comp_ge_creator`、`GT_SYM` 映射到 `&comp_gt_creator`、`LE` 映射到 `&comp_le_creator`、`LT` 映射到 `&comp_lt_creator`、`NE` 映射到 `&comp_ne_creator`。也就是说**运算符到 Item 类的映射发生在语法归约时**，转换阶段只做一次间接调用：先 `(*boolfunc2creator)(false)` 拿到 creator（`false` 是「不反转」参数），再 `->create(left, right)` 造出对应的 `Item_func_eq` / `Item_func_ge` / `Item_func_ne` 等。

`sql/sql_yacc.yy` 里 `bool_pri comp_op predicate` 规则构造的正是 `NEW_PTN PTI_comp_op(@$, $1, $2, $3)`。同一处文法还有一条 `bool_pri comp_op all_or_any table_subquery` 规则走 `PTI_comp_op_all`，该规则带一段特殊处理：当 `$2 == &comp_equal_creator` 时直接 `YYTHD->syntax_error_at(@2)`，源码注释解释这是「宁可手工抛解析错误，也不把 `comp_op` 拆成 null-safe 与非 null-safe 两条规则，因为那会增加 shift/reduce 冲突」。

### `PTI_user_variable::itemize`：用户变量

`sql/parse_tree_items.cc` 里 `PTI_user_variable::itemize` 的完整函数体是：

```cpp
bool PTI_user_variable::itemize(Parse_context *pc, Item **res) {
  if (super::itemize(pc, res)) return true;

  LEX *lex = pc->thd->lex;
  if (!lex->parsing_options.allows_variable) {
    my_error(ER_VIEW_SELECT_VARIABLE, MYF(0));
    return true;
  }
  lex->set_uncacheable(pc->select, UNCACHEABLE_RAND);
  return false;
}
```

这里有两处要点。**第一，`super::itemize(pc, res)` 成功后并不代表转换结束**——这个节点本身不造 `Item`，用户变量的 `Item` 由基类 `Parse_tree_item::itemize` 处理；`PTI_user_variable` 追加的是**约束与副作用**。**第二，`UNCACHEABLE_RAND` 的语义**：用户变量的值在同一条语句内可变，查询结果无法被缓存复用，所以要给 `pc->select` 打上不可缓存标记。

`sql/sql_yacc.yy` 里 `PTI_user_variable` 有两处构造点：`rvalue_system_or_user_variable` 规则的 `'@' ident_or_text` 分支，以及 `param_or_var` 规则的 `'@' ident_or_text` 分支。后者用于 `LIMIT` 一类的位置——`param_or_var` 规则同时接受 `param_marker`、裸 `ident`（构造 `PTI_int_splocal`）与 `'@' ident_or_text`（构造 `PTI_user_variable`）。

**同族节点 `PTI_variable_aux_set_var::itemize`** 走同样的 `allows_variable` 检查与 `UNCACHEABLE_RAND` 标记，区别是最后一步 `return lex->set_var_list.push_back(this)`——它把自己登记进 `LEX::set_var_list`，供后续 `SET` 语句处理。

### `change_truth_value_of_condition`：真值测试的三个落点

`sql/parse_tree_items.cc` 里有一个静态辅助函数 `change_truth_value_of_condition(Parse_context *pc, Item *expr, Item::Bool_test truth_test)`，是 `PTI_truth_transform::itemize` 与 SQL:2003 例外路径的共同实现。它的流程是：

1. `switch (truth_test)` 只接受 `BOOL_NEGATED`、`BOOL_IS_TRUE`、`BOOL_IS_FALSE`、`BOOL_NOT_TRUE`、`BOOL_NOT_FALSE` 五个取值，其余走 `assert(false)`。
2. 若 `!expr->is_bool_func()`，调 `make_condition(pc, expr)` 把「不完整谓词」补成布尔谓词；`make_condition` 返回空则直接返回 nullptr。
3. 调 `expr->truth_transformer(pc->thd, truth_test)`——表达式若自己实现了该变换就交给它，返回非空即采用。
4. 兜底：`truth_test == Item::BOOL_NEGATED` 时造 `Item_func_not(expr)`，否则造 `Item_func_truth(expr, truth_test)`。

`make_condition` 的实现在 `sql/item_cmpfunc.cc`：非布尔函数会被改写成 `(0 <> <expr>)` 并打上 `predicate->marker = Item::MARKER_IMPLICIT_NE_ZERO`；而 MATCH 表达式是特例——`is_function_of_type(item, Item_func::FT_FUNC)` 为真时改用 `Item_func_match_predicate` 包装，源码注释说明原因是 MATCH 既返回浮点值又能独立用在 `WHERE` 里。

### `handle_sql2003_note184_exception`：IN 谓词的 SQL:2003 例外

`sql/parse_tree_items.cc` 里的静态函数 `handle_sql2003_note184_exception(Parse_context *pc, Item *left, bool equal, Item *expr)` 处理 SQL:2003 Part 2 §8.4 的 Note 184。函数注释写明：标量子查询（`Item_singlerow_subselect`）出现在 `<in predicate>` 里时，应被重新解释为表子查询。函数的判定链是：`expr->type() == Item::SUBSELECT_ITEM` → `expr2->substype() == Item_subselect::SINGLEROW_SUBS` → 命中则调 `expr3->invalidate_and_restore_query_block()` 取出 `Query_block`，改造成 `new (pc->mem_root) Item_in_subselect(left, subselect)`；`equal` 为 false（即 `NOT IN`）时再走一次 `change_truth_value_of_condition(pc, result, Item::BOOL_NEGATED)`。

未命中该特例时，函数退化为普通比较：`equal` 为真造 `Item_func_eq(left, expr)`，否则造 `Item_func_ne(left, expr)`。

函数注释还说明这条规则是**递归**的：`IN (( <subquery> ))`、`IN ((( <subquery> )))` 直到 `IN '('^N <subquery> ')'^N` 都应解释为表子查询。

`PTI_handle_sql2003_note184_exception::itemize` 是它的薄包装：先 `super::itemize` 与左右子节点 `itemize`，再调 `handle_sql2003_note184_exception(pc, left, is_negation, right)`。

### `PTI_comp_op_all::itemize` 与 `all_any_subquery_creator`

`PTI_comp_op_all::itemize` 的函数体里，子查询走的是 `subselect->contextualize(pc)` 而非 `itemize`，然后：

```cpp
*res = all_any_subquery_creator(left, comp_op, is_all, subselect->value());
```

`all_any_subquery_creator` 实现在 `sql/sql_parse.cc`，它的四条分支值得记：

- `cmp == &comp_eq_creator` 且 `!all`（即 `= ANY`）→ 直接返回 `new Item_in_subselect(left_expr, query_block)`，源码注释写明 `= ANY <=> IN`。
- `cmp == &comp_ne_creator` 且 `all`（即 `<> ALL`）→ 先造 `Item_in_subselect`，再调 `i->truth_transformer(nullptr, Item::BOOL_NEGATED)`；变换失败则回退到 `new Item_func_not(i)`。源码注释写明 `<> ALL <=> NOT IN`。
- 其余情况造 `Item_allany_subselect`，`all` 为真时 `upper_item` 设为 `new Item_func_not_all(it)`（`ALL`），否则设为 `new Item_func_nop_all(it)`（`ANY`/`SOME`）。

注意这条路径的 `new Item_in_subselect` 没有用 `pc->mem_root` 的 placement new 写法，而是直接 `new`——这与 `parse_tree_items.cc` 里绝大多数 `new (pc->mem_root) Item_xxx` 的写法不同。

### `PTI_function_call_generic_ident_sys::itemize`：函数名解析的优先级

函数体先做 UDF 探测：`udf = nullptr;` 之后 `if (using_udf_functions && (udf = find_udf(ident.str, ident.length)) && udf->type == UDFTYPE_AGGREGATE)` 则 `pc->select->in_sum_expr++`。接着 `if (sp_check_name(&ident)) return true;` 做名字合法性检查（`sp_check_name` 实现在 `sql/sp.cc`）。

然后按注释给出的顺序解析：

1. `Create_func *builder = find_native_function_builder(ident);` 命中则 `builder->create_func(thd, ident, opt_udf_expr_list)`。
2. 否则若 `udf` 非空：若 `udf->type == UDFTYPE_AGGREGATE` 则先把 `in_sum_expr` 减回来（与前面的加法配对），再 `Create_udf_func::s_singleton.create(thd, udf, opt_udf_expr_list)`。
3. 否则 `builder = find_qualified_function_builder(thd); assert(builder);` 后走存储函数路径。

函数最后 `return *res == nullptr || (*res)->itemize(pc, res);`——造出的 `Item` 还要再走一次自己的 `itemize`。

### `PTI_simple_ident_ident::itemize`：裸标识符的两条路

函数体分成「存储程序变量」与「普通列引用」两支。

**存储程序分支**：先 `sp_pcontext *pctx = lex->get_sp_current_parsing_ctx();`，若 `pctx && (spv = pctx->find_variable(ident.str, ident.length, false))` 命中，说明正在编译存储程序且找到了同名变量。此时先检查 `lex->parsing_options.allows_variable`，不满足则 `my_error(ER_VIEW_SELECT_VARIABLE, MYF(0))` 返回 true；满足则 `*res = create_item_for_sp_var(thd, ident, spv, sp->m_parser_data.get_current_stmt_start_ptr(), raw.start, raw.end)`，并置 `lex->safe_to_cache_query = false`。

`create_item_for_sp_var` 实现在 `sql/parse_tree_helpers.cc`：它在 `spv` 为空时会自己用 `pctx->find_variable(name.str, name.length, false)` 再查一次；仍找不到就 `my_error(ER_SP_UNDECLARED_VAR, MYF(0), name.str)` 返回 nullptr。它还处理一个特殊情形——`lex->reparse_common_table_expr_at != 0 || lex->reparse_derived_table_condition` 时把 `query_start_ptr` 置空，源码注释说明原因是「这个变量在原查询里不存在：不该被替换用于日志」。最终造 `Item_splocal(name, spv->offset, spv->type, spv_pos_in_query, spv_len_in_query)`。

**普通列分支**：按 `parsing_place` 与 `in_sum_expr` 二选一造 `Item_field` 或 `Item_ref`，然后 `if (*res == nullptr || (*res)->itemize(pc, res)) return true;`。造出的 `Item_field(POS(), NullS, NullS, ident.str)` 里两个 `NullS` 是 schema 名与表名的占位——真正的绑定留给后续的名称解析阶段。

### `PTI_simple_ident_q_2d::itemize`：触发器里的 `NEW.x` / `OLD.x`

这是本文件里最复杂的一个 `itemize`。判定条件是三者同时成立：`sp` 非空、`sp->m_type == enum_sp_type::TRIGGER`、`table` 名经 `my_strcasecmp(system_charset_info, table, "NEW")` 或 `"OLD"` 比较命中（大小写不敏感）。

命中后的处理：

- `bool new_row = (table[0] == 'N' || table[0] == 'n');`——只看首字母定新旧行。
- 事件与行不匹配时报错：`sp->m_trg_chistics.event == TRG_EVENT_INSERT && !new_row` 报 `ER_TRG_NO_SUCH_ROW_IN_TRG`，参数为 `"OLD"` 与 `"on INSERT"`；`TRG_EVENT_DELETE && new_row` 报同一错误，参数为 `"NEW"` 与 `"on DELETE"`。
- 权限位：`const bool read_only = !(new_row && sp->m_trg_chistics.action_time == TRG_ACTION_BEFORE);`——只有「BEFORE INSERT 的 NEW 行」可写，其余（`OLD` 行、`AFTER` 动作）只读。
- 造 `Item_trigger_field(POS(), new_row ? TRG_NEW_ROW : TRG_OLD_ROW, field, SELECT_ACL, read_only)`，其中权限常量是 `SELECT_ACL`。
- 把该对象挂进 `lex->sphead->m_cur_instr_trig_field_items.link_in_list(trg_fld, &trg_fld->next_trg_field)`，源码注释写明这是「把该 item 加到触发器中所有 `Item_trigger_field` 对象的链表里」。

未命中触发器条件时走 `super::itemize(pc, res)`，即普通的两段式限定名处理。

### `PTI_function_call_nonkeyword_sysdate::itemize`：SYSDATE 与 NOW 的选择

函数体先 `lex->set_stmt_unsafe(LEX::BINLOG_STMT_UNSAFE_SYSTEM_FUNCTION);`，然后按 `global_system_variables.sysdate_is_now` 分叉：为 0 时造 `Item_func_sysdate_local(dec)`，否则造 `Item_func_now_local(dec)`。之后 `if (*res == nullptr) return true;`，最后 `lex->safe_to_cache_query = false;`。

`sysdate_is_now` 的默认值来自 `sql/mysqld.cc` 里 `sys_var` 表的 `"sysdate-is-now"` 条目，其默认值为 `1`（`NO_ARG, 0, 0, 1, nullptr, 1, nullptr`）。变量描述文本写明：「Since 5.0, SYSDATE() returns a 'dynamic' value different for different invocations, even within a single statement.」

注意函数注释给出的复制安全判断：即使 `sysdate_is_now=1`，`SYSDATE()` 依然被标记为不安全，因为从库可能是 `sysdate_is_now=0`。

### 字面量类节点的字符集与 repertoire

**`PTI_text_literal_text_string::itemize`** 取 `thd->variables.collation_connection`（连接字符集）与 `thd->variables.character_set_client`（客户端字符集）。repertoire 的判定是 `is_7bit && my_charset_is_ascii_based(cs_cli) ? MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30`。是否需要转码的判定是 `thd->charset_is_collation_connection || (repertoire == MY_REPERTOIRE_ASCII && my_charset_is_ascii_based(cs_con))`——成立就直接用原始文本，否则调 `thd->convert_string(&tmp, cs_con, literal.str, literal.length, cs_cli)` 转码，转码失败返回 true。最后 `init(tmp.str, tmp.length, cs_con, DERIVATION_COERCIBLE, repertoire)`。

**`PTI_text_literal_nchar_string::itemize`** 用 `is_7bit ? MY_REPERTOIRE_ASCII : MY_REPERTOIRE_UNICODE30` 定 repertoire，并断言 `my_charset_is_ascii_based(national_charset_info)`，然后以 `national_charset_info` 与 `DERIVATION_COERCIBLE` 初始化。

**`PTI_text_literal_concat::itemize`** 处理字符串拼接：它把头部 `Item_string` 取出来（`down_cast<Item_string *>(tmp_head)`），`head_str->append(literal.str, literal.length)` 追加新片段；若累积的 repertoire 还没有 `MY_REPERTOIRE_EXTENDED` 位，就再用 `my_string_repertoire(cs, literal.str, literal.length)` 补上，源码注释写明「如果字符串到目前为止都是纯 ASCII，就检查新增的部分」。

**`PTI_odbc_date::itemize`** 尝试把 ODBC 风格的日期时间字面量识别成类型化常量。前置条件是 `expr->type() == Item::STRING_ITEM && expr->collation.repertoire == MY_REPERTOIRE_ASCII`；长度上限是 `tmp_str->length() < MAX_DATE_STRING_REP_LENGTH * 4`（常量 `MAX_DATE_STRING_REP_LENGTH` 定义在 `my_time.h`）。类型判定按标识符长度与首字母：`'d'` → `MYSQL_TYPE_DATE`、`'t'` → `MYSQL_TYPE_TIME`、两字符的 `"ts"` → `MYSQL_TYPE_DATETIME`。识别成功则 `create_temporal_literal(pc->thd, str.ptr(), str.length(), system_charset_info, type, false)`；失败则 `if (*res == nullptr) *res = expr;` 回退到原表达式。

### 计数器与聚合参数

**`PTI_count_sym::itemize`** 只做一件事：`args[0] = new (pc->mem_root) Item_int(int32{0}, 1);`——把 `COUNT(*)` 的参数补成一个值为 0 的 `Item_int`（第二个参数 `1` 是「unsigned」标志）。它对应 `sql/sql_yacc.yy` 里 `COUNT_SYM '(' opt_all '*' ')'` 规则构造的 `PTI_count_sym`。

**`PTI_in_sum_expr::itemize`** 负责 `in_sum_expr` 计数器的进出配对：`pc->select->in_sum_expr++` 先加，待 `super::itemize` 与 `expr->itemize` 完成后 `pc->select->in_sum_expr--` 再减。它对应 `sql/sql_yacc.yy` 的 `in_sum_expr: opt_all expr` 规则，被 `AVG_SYM`、`COUNT_SYM`、`MIN_SYM`、`MAX_SYM`、`SUM_SYM`、`STD_SYM`、`VARIANCE_SYM`、`BIT_XOR_SYM` 等十余个聚合函数的语法规则使用。

这个计数器直接决定标识符被造 成 `Item_field` 还是 `Item_ref`：`PTI_simple_ident_ident`、`PTI_simple_ident_q_2d`、`PTI_simple_ident_q_3d`、`PTI_simple_ident_nospvar_ident` 四处都写着 `pc->select->get_in_sum_expr() > 0` 时造 `Item_field`，否则（且 `parsing_place == CTX_HAVING`）造 `Item_ref`。

### `PTI_get_system_variable::itemize`：诊断变量的特殊处理

函数体除了 `allows_variable` 检查外，还有一段针对诊断变量的特殊逻辑：当 `m_opt_prefix.str == nullptr` 且变量名是 `warning_count` 或 `error_count`（用 `is_identifier(m_name.str, "warning_count")` 判定）时，置 `lex->keep_diagnostics = DA_KEEP_COUNTS`。源码注释写明：这是「在非诊断语句里使用的诊断变量：保留我们需要的那部分信息，但因 `error_count` 而清空诊断区的其余部分」，并指向 `reset_condition_info()`。

最后 `*res = get_system_variable(pc, m_scope, m_opt_prefix, m_name, true);`。`get_system_variable` 实现在 `sql/item_func.cc`。

### `PTI_context::itemize`：解析上下文的设置与复位

`PTI_context::itemize` 管理 `pc->select->parsing_place`：先 `pc->select->parsing_place = m_parsing_place;`，再 `expr->itemize(pc, &expr)`，然后若 `!expr->is_bool_func()` 调 `make_condition(pc, expr)` 补成布尔谓词，最后把 `parsing_place` 复位为 `CTX_NONE` 并调 `expr->apply_is_true()`。

复位前后各有一个 `assert`：`assert(pc->select->parsing_place == m_parsing_place);` 确保复位的是「正确的那个 select」，`assert(expr != nullptr);` 确保谓词已就绪。

### `PTI_int_splocal::itemize`：存储程序整型变量

函数体先取 `sp_head *const sp = lex->sphead;`，再取 `const char *query_start_ptr = sp ? sp->m_parser_data.get_current_stmt_start_ptr() : nullptr;`。然后调 `create_item_for_sp_var(pc->thd, m_name, nullptr, query_start_ptr, m_location.raw.start, m_location.raw.end)`，返回值空则返回 true（注释标注为「undefined variable or OOM」）。

关键的额外约束是类型检查：`if (v->type() != Item::INT_ITEM)` 则 `pc->thd->syntax_error_at(m_location, ER_SPVAR_NONINTEGER_TYPE, m_name.str)` 返回 true。也就是说，存储程序里的整型局部变量只能用在要求整数的语法位置上。

### 其他节点

**`PTI_singlerow_subselect::itemize`** 调 `subselect->contextualize(pc)` 后造 `Item_singlerow_subselect(subselect->value())`，并把 `pc->select->n_scalar_subqueries++`。这个计数被 `sql/sql_lex.cc` 里的 `order_group_num + n_scalar_subqueries` 公式用来估算需要准备的字段槽位。

**`PTI_exists_subselect::itemize`** 同样先 `contextualize`，再造 `Item_exists_subselect(subselect->value())`，但不增加任何计数器。

**`PTI_udf_expr::itemize`** 处理 UDF 参数的名字：若有 `select_alias` 则用它覆盖 `expr->item_name`；否则若 `expr->type()` 既不是 `FIELD_ITEM` 也不是 `REF_ITEM`（源码注释标 `/* For HAVING */`），则用 `expr_loc` 的原文拷贝一份作为名字。函数注释解释了原因：「字段必须有正确的名字才能做名称解析，这一点只有解析出来才保证。如果劫持输入流，可能拿到带引号或转义的名称。」

**`PTI_expr_with_alias::itemize`** 处理列别名：若有别名，且 `pc->thd->lex->sql_command == SQLCOM_CREATE_VIEW` 且 `check_column_name(alias.str)` 为真，则 `my_error(ER_WRONG_COLUMN_NAME, MYF(0), alias.str)` 返回 true——建视图时列名非法直接报错。没有别名且 `expr->item_name` 未设置时，用 `expr_loc` 原文补一个名字。

**`PTI_limit_option_ident::itemize`** 与 `PTI_limit_option_param_marker::itemize`** 处理 `LIMIT` 的参数。`PTI_limit_option_ident::itemize` 把 `*res` 用 `down_cast<Item_splocal *>` 转型后置 `v->limit_clause_param = true`。`PTI_limit_option_param_marker::itemize` 的函数注释说明：因为 `Item_param::type()` 可能返回多种值，不能简单地比较 `type()` 再转型赋值，所以依赖 `Item_param::itemize()` 总会把输出参数替换成 `Item_param` 对象自身的 `this` 指针这一保证，直接 `assert(tmp_param == param_marker);` 后 `*res = param_marker;` 即可。

**`PTI_temporal_literal::itemize`** 只是 `*res = create_temporal_literal(pc->thd, literal.str, literal.length, cs, field_type, true);`，最后一个参数 `true` 与 `PTI_odbc_date` 里的 `false` 相对。

## 关键取舍

**把「运算符到 Item 类的映射」放在语法归约时，而不是转换时。** `boolfunc2creator` 是一个函数指针，由 `sql/sql_yacc.yy` 的 `comp_op` 规则在归约时按记号填好；转换阶段只做一次间接调用。代价是语法文件里要维护一张记号到 creator 的映射表；收益是 `PTI_comp_op::itemize` 不需要任何 `switch`，新增运算符只要改文法不改转换层。

**转换层同时承担语义收紧，而不是纯粹的形状映射。** `allows_variable` 检查、`UNCACHEABLE_RAND` 标记、`safe_to_cache_query = false`、`set_stmt_unsafe` 全部塞在 `itemize` 里。代价是 `itemize` 不再是纯函数——它会改 `LEX` 的状态；收益是不必为每类约束再设计一套并行的语义检查通道。

**给 `Item` 预留「自己实现变换」的钩子。** `change_truth_value_of_condition` 先问 `expr->truth_transformer(pc->thd, truth_test)`，拿到非空就直接采用，只有在表达式不自己实现时才兜底造 `Item_func_not` 或 `Item_func_truth`。这让已有 Item 类型可以在不改转换层的前提下支持新的真值语义。

**「不完整谓词」统一补成 `(0 <> expr)`，但给 MATCH 开特例。** `make_condition` 在 `sql/item_cmpfunc.cc` 里对普通表达式造 `Item_func_ne(item_zero, item)` 并打 `MARKER_IMPLICIT_NE_ZERO` 标记；只有 `is_function_of_type(item, Item_func::FT_FUNC)` 为真的 MATCH 表达式改走 `Item_func_match_predicate`。源码注释说明了原因：MATCH 既返回浮点值又能独立用于 `WHERE`。

**把 SQL 标准里的例外显式实现，而不是绕过。** `handle_sql2003_note184_exception` 是为了实现 SQL:2003 Note 184 而专门写的辅助函数，函数注释甚至列出了它依据的标准条款编号与页码。代价是标准符合性逻辑进了转换层；收益是 `IN (SELECT ...)` 与 `IN ((SELECT ...))` 行为一致，不需要在上层打补丁。

**同一语义在多个节点上重复检查，而不是集中到一处。** `allows_variable` 检查出现在 `PTI_user_variable`、`PTI_variable_aux_set_var`、`PTI_get_system_variable`、`PTI_simple_ident_ident` 四处；`in_sum_expr` 判断出现在四个标识符节点里。代价是重复代码；收益是每个节点独立可读，且 `PTI_simple_ident_q_2d` 这种「触发器专用」分支不必受通用路径的约束。

**区分「在 HAVING 里」与「在聚合函数里」来决定造哪种引用节点。** 四个标识符节点的判断条件完全一致：非 `CTX_HAVING` 或 `in_sum_expr > 0` 时造 `Item_field`，只有「HAVING 中且不在聚合参数里」才造 `Item_ref`。这说明 `Item_ref` 的语义是「指向一个已存在的表达式结果」，而不是「一个待绑定的列」。

**为 ODBC 字面量识别设长度上限，避免在大字符串上做无谓的类型解析。** `PTI_odbc_date::itemize` 的门槛是 `tmp_str->length() < MAX_DATE_STRING_REP_LENGTH * 4`；超长就直接回退到原表达式。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/parse_tree_items.cc` | `PTI_comp_op::itemize` | 比较运算符转换：`(*boolfunc2creator)(false)->create(left, right)` |
| `sql/parse_tree_items.cc` | `PTI_comp_op_all::itemize` | `ALL`/`ANY` 子查询转换：先 `contextualize` 再调 `all_any_subquery_creator` |
| `sql/parse_tree_items.cc` | `PTI_user_variable::itemize` | 用户变量节点：`allows_variable` 检查 + `set_uncacheable(UNCACHEABLE_RAND)` |
| `sql/parse_tree_items.cc` | `PTI_variable_aux_set_var::itemize` | `SET` 变量节点：同样的检查与标记，末尾登记进 `lex->set_var_list` |
| `sql/parse_tree_items.cc` | `PTI_get_system_variable::itemize` | 系统变量节点；`warning_count`/`error_count` 时置 `keep_diagnostics = DA_KEEP_COUNTS` |
| `sql/parse_tree_items.cc` | `PTI_simple_ident_ident::itemize` | 裸标识符：存储程序变量走 `create_item_for_sp_var`，否则造 `Item_field`/`Item_ref` |
| `sql/parse_tree_items.cc` | `PTI_simple_ident_q_2d::itemize` | `tbl.col`：识别触发器的 `NEW`/`OLD`，造 `Item_trigger_field` 并挂链表 |
| `sql/parse_tree_items.cc` | `PTI_simple_ident_q_3d::itemize` | `db.tbl.col`；`no_table_names_allowed` 时报 `ER_TABLENAME_NOT_ALLOWED_HERE` |
| `sql/parse_tree_items.cc` | `PTI_simple_ident_nospvar_ident::itemize` | 不允许存储程序变量的标识符位置 |
| `sql/parse_tree_items.cc` | `PTI_function_call_generic_ident_sys::itemize` | 函数名解析：原生函数 → UDF → 存储函数 |
| `sql/parse_tree_items.cc` | `PTI_function_call_generic_2d::itemize` | 限定名函数调用：走 `find_qualified_function_builder` 建存储函数 |
| `sql/parse_tree_items.cc` | `PTI_function_call_nonkeyword_sysdate::itemize` | `SYSDATE()`：按 `sysdate_is_now` 选 `Item_func_sysdate_local`/`Item_func_now_local`，并标记复制不安全 |
| `sql/parse_tree_items.cc` | `PTI_function_call_nonkeyword_now::itemize` | `NOW()`：置 `lex->safe_to_cache_query = false` |
| `sql/parse_tree_items.cc` | `PTI_udf_expr::itemize` | UDF 参数名：回填 `expr->item_name` |
| `sql/parse_tree_items.cc` | `PTI_expr_with_alias::itemize` | 列别名；建视图时用 `check_column_name` 校验 |
| `sql/parse_tree_items.cc` | `PTI_truth_transform::itemize` | 真值测试入口，转调 `change_truth_value_of_condition` |
| `sql/parse_tree_items.cc` | `PTI_handle_sql2003_note184_exception::itemize` | IN 谓词的 SQL:2003 Note 184 例外入口 |
| `sql/parse_tree_items.cc` | `PTI_singlerow_subselect::itemize` | 标量子查询：造 `Item_singlerow_subselect`，`n_scalar_subqueries++` |
| `sql/parse_tree_items.cc` | `PTI_exists_subselect::itemize` | `EXISTS` 子查询：造 `Item_exists_subselect` |
| `sql/parse_tree_items.cc` | `PTI_count_sym::itemize` | `COUNT(*)`：把参数补成 `Item_int(int32{0}, 1)` |
| `sql/parse_tree_items.cc` | `PTI_in_sum_expr::itemize` | 聚合参数：配对维护 `pc->select->in_sum_expr` |
| `sql/parse_tree_items.cc` | `PTI_text_literal_text_string::itemize` | 普通字符串字面量：字符集与 repertoire 判定、必要时转码 |
| `sql/parse_tree_items.cc` | `PTI_text_literal_nchar_string::itemize` | `N'...'` 字面量：用 `national_charset_info` 初始化 |
| `sql/parse_tree_items.cc` | `PTI_text_literal_concat::itemize` | 字符串拼接：追加片段并维护 repertoire |
| `sql/parse_tree_items.cc` | `PTI_temporal_literal::itemize` | 时间字面量：调 `create_temporal_literal` |
| `sql/parse_tree_items.cc` | `PTI_odbc_date::itemize` | ODBC 日期/时间字面量识别，长度上限 `MAX_DATE_STRING_REP_LENGTH * 4` |
| `sql/parse_tree_items.cc` | `PTI_int_splocal::itemize` | 存储程序整型变量：非 `INT_ITEM` 报 `ER_SPVAR_NONINTEGER_TYPE` |
| `sql/parse_tree_items.cc` | `PTI_limit_option_ident::itemize` | `LIMIT` 参数：置 `limit_clause_param = true` |
| `sql/parse_tree_items.cc` | `PTI_limit_option_param_marker::itemize` | `LIMIT` 占位符参数：断言 `tmp_param == param_marker` |
| `sql/parse_tree_items.cc` | `PTI_context::itemize` | 解析上下文：设/复位 `parsing_place`，补布尔谓词，调 `apply_is_true` |
| `sql/parse_tree_items.cc` | `change_truth_value_of_condition` | 真值变换三落点：`truth_transformer` → `Item_func_not` → `Item_func_truth` |
| `sql/parse_tree_items.cc` | `handle_sql2003_note184_exception` | 标量子查询在 IN 谓词里改解释为表子查询 |
| `sql/parse_tree_helpers.cc` | `create_item_for_sp_var` | 造 `Item_splocal`；变量未声明报 `ER_SP_UNDECLARED_VAR` |
| `sql/item_cmpfunc.cc` | `make_condition` | 非布尔表达式改写成 `(0 <> expr)`，MATCH 走 `Item_func_match_predicate` |
| `sql/item_cmpfunc.cc` | `Item_func_truth::resolve_type` | `Item_func_truth` 的类型解析（`Item_func_truth` 兜底路径的落点） |
| `sql/sql_parse.cc` | `all_any_subquery_creator` | `ALL`/`ANY`/`SOME` 子查询的 Item 构造，含 `= ANY`→`IN`、`<> ALL`→`NOT IN` 两条等价化简 |
| `sql/sql_yacc.yy` | `comp_op` | 记号到 creator 的映射：`EQ`→`comp_eq_creator`、`EQUAL_SYM`→`comp_equal_creator` 等 |
| `sql/sql_yacc.yy` | `simple_ident` | 构造 `PTI_simple_ident_ident` 与 `PTI_simple_ident_q_2d`/`_q_3d` |
| `sql/sql_yacc.yy` | `in_sum_expr` | 构造 `PTI_in_sum_expr`，被十余个聚合函数规则引用 |
| `sql/sql_yacc.yy` | `param_or_var` | `LIMIT` 参数的三个来源：`param_marker`、裸 `ident`（`PTI_int_splocal`）、`'@' ident_or_text`（`PTI_user_variable`） |
| `sql/sql_yacc.yy` | `rvalue_system_or_user_variable` | 构造 `PTI_user_variable` 与 `PTI_get_system_variable` |
| `sql/sp.cc` | `sp_check_name` | 存储程序对象名合法性检查，被函数调用与限定名函数调用复用 |
| `sql/mysqld.cc` | `sys_var` 表的 `sysdate-is-now` 条目 | `global_system_variables.sysdate_is_now` 的定义与默认值 `1` |
| `sql/item_func.cc` | `get_system_variable` | 系统变量表达式的构造，被 `PTI_get_system_variable` 调用 |
| `sql/sql_lex.cc` | `Lex_input_stream` 的字段槽位估算 | `order_group_num + n_scalar_subqueries` 公式用到 `n_scalar_subqueries` |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 前端是手写递归下降，表达式在递归下降过程中直接构造，不存在「语法树 → Item」的中间翻译层。

**架构推论：** MySQL 这一层值得主核借鉴的是「把语义收紧集中在一次转换里」的做法。它把「视图里不许有变量」「函数名按原生→UDF→存储函数解析」「触发器里 `OLD` 行只读」「`COUNT(*)` 的参数补成常量 `0`」这些分散的约束全部挂在同一个 `itemize` 入口上，每条约束都能定位到一个具体函数体。主核若要在递归下降之外加一层 AST 中间表示，同样需要一个「AST 节点 → 可执行表达式」的单一转换点，否则这些约束就会散落到各个递归下降分支里。

**架构推论：** `PTI_simple_ident_ident` 那一处「按 `parsing_place` 与 `in_sum_expr` 决定造 `Item_field` 还是 `Item_ref`」体现了 MySQL 对「引用」与「待绑定列」的区分。主核的表达式树目前把两者都建成同一种列引用节点，一旦需要表达「HAVING 里引用的是聚合结果而非基表列」，就需要引入可绑定的引用节点。

对 AI 场景而言，这一层是「模型生成的 SQL 文本」与「服务端真正要执行的表达式」之间的收敛点。MySQL 在这里做的收紧是有价值示范：**同一段 SQL 文本，在不同上下文里会被转成不同的 Item**——同一个标识符在 `WHERE` 里是 `Item_field`，在 `HAVING` 里是 `Item_ref`；`SYSDATE()` 与 `NOW()` 在 `sysdate_is_now` 开关两侧落到不同的 Item 类，并因此有不同的复制安全性。模型若只按字符串模式判断 SQL 语义，会漏掉这些上下文依赖的差异。

## 参考

- [MySQL 源码：`sql/parse_tree_items.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/parse_tree_items.cc)
- [MySQL 源码：`sql/sql_yacc.yy`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_yacc.yy)
- [MySQL 源码：`sql/parse_tree_helpers.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/parse_tree_helpers.cc)
- [MySQL 源码：`sql/item_cmpfunc.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/item_cmpfunc.cc)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 源码：`sql/item_func.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/item_func.cc)
- [MySQL 源码：`sql/sp.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sp.cc)
- [MySQL 源码：`sql/mysqld.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/mysqld.cc)
- [MySQL 源码：`sql/sql_lex.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex.cc)
- [MySQL 源码：`sql/item_subselect.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/item_subselect.cc)
- [MySQL 参考手册：运算符](https://dev.mysql.com/doc/refman/8.0/en/operators.html)
- [MySQL 参考手册：用户变量](https://dev.mysql.com/doc/refman/8.0/en/user-variables.html)
- [MySQL 参考手册：子查询限制](https://dev.mysql.com/doc/refman/8.0/en/subquery-restrictions.html)
- [MySQL 参考手册：ODBC 日期时间字面量](https://dev.mysql.com/doc/refman/8.0/en/odbc-date-time-literals.html)
- [MySQL Server 开发文档：Parser](https://dev.mysql.com/doc/dev/mysql-server/latest/page_parser.html)
