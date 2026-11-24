---
project: mysql-server
feature: grammar-parser
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_yacc.yy
    symbols: [MYSQLparse, yyparse]
---

# 语法分析器

## 一句话职责

MySQL 的语法分析器由 bison 从 `sql/sql_yacc.yy` 生成，入口是 `MYSQLparse`：它把词法分析器（`sql/sql_lex.cc` 的 `MYSQLlex`）递送上来的记号流按文法归约，边归约边构造解析树节点（`PT_query_expression`、`PT_query_specification`、`PT_union`、`PT_common_table_expr` 等），最后由 `LEX::make_sql_cmd` 把解析树转成可执行的 `Sql_cmd`。

## 机制是什么

**语法分析器不是手写递归下降，而是 bison 生成的 LALR(1) 自动机。** `sql/sql_yacc.yy` 文件头部的宏定义把这层关系写死了：`#define YYP (YYTHD->m_parser_state)`、`#define YYLIP (& YYTHD->m_parser_state->m_lip)`、`#define YYMEM_ROOT (YYTHD->mem_root)`。也就是说 bison 生成的分析器不持有任何自己的状态，所有可变状态都挂在 `THD` 上：词法输入流在 `m_parser_state->m_lip`，bison 栈在 `m_parser_state->m_yacc`，解析树节点全部从 `THD::mem_root` 分配。

**`%define api.pure` 声明纯解析器。** 文件里 `%define api.pure` 后面跟着注释 `/* We have threads */`，配合 `%parse-param { class THD *YYTHD }`、`%parse-param { class Parse_tree_root **parse_tree }` 与 `%lex-param { class THD *YYTHD }`：bison 生成的分析器是**可重入**的，不使用全局变量，THD 通过参数一路传到取词函数。文件顶部的注释补充了这条链路：「`YYTHD` is passed as an argument to `yyparse()`, and subsequently to `yylex()`」。

**文法是"有歧义但用优先级消解"的。** 文件里紧跟 `%define api.pure` 之后的注释明确写了两条维护纪律：「1. We do not accept any reduce/reduce conflicts」和「2. We should not introduce new shift/reduce conflicts any more.」，紧接着是 `%expect 63`——即当前文法允许 63 个已知的移进/归约冲突，bison 编译时若冲突数不等于 63 就报错。这 63 个冲突不是疏漏，而是 MySQL 的语法扩展（如 `INTO` 出现在 `FROM` 之前或之后、`SELECT` 结果集可含括号当表达式用）刻意保留的，靠优先级声明与人工记号解决。

**优先级声明是分层自底向上的运算符优先级表。** 从 `%left KEYWORD_USED_AS_IDENT` 起，依次声明 `%left UNION_SYM EXCEPT_SYM`、`%left INTERSECT_SYM`、`%left OR_SYM OR2_SYM`、`%left XOR`、`%left AND_SYM AND_AND_SYM`、`%left EQ EQUAL_SYM GE GT_SYM LE LT NE IS LIKE REGEXP IN_SYM`、`%left '|'`、`%left '&'`、`%left SHIFT_LEFT SHIFT_RIGHT`、`%left '-' '+'`、`%left '*' '/' '%' DIV_SYM MOD_SYM`、`%left '^'`、`%left OR_OR_SYM`、`%left NEG '~'`、`%right NOT_SYM NOT2_SYM`、`%right BINARY_SYM COLLATE_SYM`、`%left INTERVAL_SYM`。注意 `INTERSECT_SYM` 的优先级声明在 `UNION_SYM EXCEPT_SYM` **之后**，因此 `INTERSECT` 的结合强度高于 `UNION`/`EXCEPT`。

**两个"人工记号"承担消歧职责。** `%left SUBQUERY_AS_EXPR` 与 `%left EMPTY_FROM_CLAUSE` 是源码里注释点名的两个 artificial token：

- `SUBQUERY_AS_EXPR` 解决"括号属于子查询还是属于查询表达式"的歧义。源码注释写明：定义一个不允许外层括号的 `query_expression`，然后用「优先级低于括号」的人工记号解决 `<subquery>` 规则残留的移进/归约冲突，保证解析器「consumes as many parentheses as it can」，只在失败时才归约。
- `EMPTY_FROM_CLAUSE` 解决 `INTO` 位置歧义。源码注释解释：`INTO` 被声明为 `%right INTO`（右结合）且优先级高于 `%left EMPTY_FROM_CLAUSE`，于是当分析器看到 `INTO` 时总是移进而非归约，`INTO` 被解释成前置的那个。

**同一份文法还兼做"局部语法分析"。** 文件里定义了一族 synthetic token：`GRAMMAR_SELECTOR_EXPR`（916）、`GRAMMAR_SELECTOR_GCOL`（917）、`GRAMMAR_SELECTOR_PART`（918）、`GRAMMAR_SELECTOR_CTE`（919）、`GRAMMAR_SELECTOR_DERIVED_EXPR`（1158），注释统一标为 `/* synthetic token: ... */`。`start_entry` 规则为每种 selector 各开一条分支，把同一套文法复用于「解析一个表达式」「解析生成列表达式」「解析分区表达式」「解析 CTE」「解析派生表达式」这五种局部场景。

**记号编号被刻意冻结。** 文件里维护者注释有一节专门讲 token 值：bison 按声明顺序分配记号值，而记号值会进入 query DIGEST，为了让摘要稳定，新增记号必须追加到 8.0 段末尾；删除旧记号时不能真删，要改名成 `OBSOLETE_TOKEN_<NNN>` 并在注释里写明原来是什么。文件里确实存在这种写法，例如 `%token OBSOLETE_TOKEN_271 271 /* was: ANALYSE_SYM */` 与 `%token OBSOLETE_TOKEN_388 388 /* was: DES_KEY_FILE */`。该过程由 `gen_lex_token` 强制，违规会触发构建失败。

**分析器与语义分析是分离的两段。** 语法分析器只构造解析树（`PT_*` 节点），把「名字绑定到表列」「表达式求值语义」都推给后续阶段。文件里为此定义了两个包装宏：

- `CONTEXTUALIZE(x)`：构造 `context_t pc(YYTHD, Select)`，在 `YYTHD->is_error()` 或 `lex->will_contextualize && (x)->contextualize(&pc)` 为真时 `MYSQL_YYABORT`。`CONTEXTUALIZE_VIEW(x)` 额外多一步 `pc.finalize_query_expression()`。
- `ITEMIZE(x, y)`：构造 `Parse_context pc(YYTHD, Select)`，调 `(x)->itemize(&pc, (y))`，失败即中止。

注意两个宏都带 `lex->will_contextualize` 这个开关：`LEX` 构造函数把 `will_contextualize(true)` 作为初值（见 `sql/sql_lex.cc` 的 `LEX::LEX()`），而 `LEX::make_sql_cmd` 在 `!will_contextualize` 时直接 `return false`。也就是说存在一种"只建树、不做语义化"的解析模式。

## 怎么实现的

### 入口：`MYSQLparse` 与 `THD::sql_parser`

`sql/sql_class.cc` 的 `THD::sql_parser()` 是唯一调用生成分析器的地方，函数体很短：

1. 声明 `extern int MYSQLparse(class THD * thd, class Parse_tree_root * *root);`——注意这里是**外部声明**，`MYSQLparse` 的实体在 bison 生成的 `sql/sql_yacc.cc` 里，不在 `sql_yacc.yy` 本身。
2. `Parse_tree_root *root = nullptr;`，然后 `if (MYSQLparse(this, &root) || is_error())` 就调 `cleanup_after_parse_error()` 并 `return true`。
3. 成功时 `if (root != nullptr && lex->make_sql_cmd(root)) return true;`。

函数上方的注释说明了 `root` 为空的情形：「If "root" is NULL on success, then the parser has already called `lex->make_sql_cmd()` internally.」——这正是 `MAKE_CMD` 宏在文法内部提前造好 `Sql_cmd` 的场景。

`THD::cleanup_after_parse_error()` 的注释写明它的存在理由：「Restore the original LEX if it was replaced when parsing a stored procedure. We must ensure that a parsing error does not leave any side effects in the THD.」它调用 `sp->m_parser_data.finish_parsing_sp_body(this)`，并在 `sp_runtime_ctx == nullptr` 时 `sp_head::destroy(sp)` 且把 `lex->sphead` 置空。

### 外层包装：`parse_sql`

`sql/sql_parse.cc` 的 `parse_sql(THD *, Parser_state *, Object_creation_ctx *)` 是 `THD::sql_parser()` 的包装，负责解析前后的环境准备与收尾，函数体按顺序做了这些事：

1. `assert(thd->m_parser_state == nullptr)`，然后 `thd->m_parser_state = parser_state;`，并把 `parser_state->m_digest_psi` 与 `parser_state->m_lip.m_digest` 置空。
2. 摘要装配：源码注释写「Partial parsers (GRAMMAR_SELECTOR_*) are not supposed to compute digests.」，对应的断言是 `assert(!parser_state->m_lip.is_partial_parser() || !parser_state->m_input.m_has_digest);`。只有在 `parser_state->m_input.m_has_digest` 且 `thd->m_digest != nullptr` 时才启动 `MYSQL_DIGEST_START`，并把 `parser_state->m_lip.m_digest` 指向 `thd->m_digest`，同时设 `m_digest_storage.m_charset_number = thd->charset()->number`。
3. **内存上限**：`thd->mem_root->set_max_capacity(thd->variables.parser_max_mem_size)`，紧跟 `thd->mem_root->set_error_for_capacity_exceeded(true)`。解析结束后再 `set_max_capacity(0)` 与 `set_error_for_capacity_exceeded(false)` 复原。
4. **OOM 处理器**：压入 `Parser_oom_handler poomh`。该处理器（`sql/sql_parse.cc` 中 `Parser_oom_handler::handle_condition`）在遇到 `EE_CAPACITY_EXCEEDED` 时报 `ER_CAPACITY_EXCEEDED`，消息里带上 `thd->variables.parser_max_mem_size` 的值和变量名 `parser_max_mem_size`；遇到 `EE_OUTOFMEMORY` 时报 `ER_OUT_OF_RESOURCES` 并带 `ME_FATALERROR`。源码注释写明「Out of memory error is reported only once. Return as handled」。
5. **诊断区隔离**：`thd->get_parser_da()` 与 `thd->get_stmt_da()` 分开，解析期间用临时的 `parser_da`。解析结束后若 `parser_da->current_statement_cond_count() != 0`，则当命令不是 `SQLCOM_SHOW_WARNS` 也不是 `SQLCOM_GET_DIAGNOSTICS` 时执行 `da->reset_condition_info(thd)`（源码注释：解析期错误将成为当前语句唯一的条件），并置 `thd->lex->keep_diagnostics = DA_KEEP_PARSE_ERROR`。
6. `thd->pop_diagnostics_area()`、`thd->m_parser_state = nullptr`、恢复 creation context。
7. 收尾断言：`assert(!mysql_parse_status || (mysql_parse_status && thd->is_error()) || (mysql_parse_status && thd->get_internal_handler()));`。返回值是 `mysql_parse_status || thd->is_fatal_error()`；成功且有 digest 时调 `MYSQL_DIGEST_END`。

### 错误回调：`MYSQLerror`

`sql/sql_yacc.yy` 里定义了 bison 的错误回调 `MYSQLerror(YYLTYPE *location, THD *thd, Parse_tree_root **, const char *s)`，函数体是一个三分支的字符串比较：

- `strcmp(s, "syntax error") == 0` → `thd->syntax_error_at(*location)`（带位置）。
- `strcmp(s, "memory exhausted") == 0` → `my_error(ER_DA_OOM, MYF(0))`。
- 其他 → `assert(false); my_error(ER_UNKNOWN_ERROR, MYF(0));`，源码注释提醒「Find omitted error messages in the generated file (sql_yacc.cc) and fix」。

该函数上方的注释明确它**只在语法错误或 OOM 时被调用**，语义动作里主动 `YYABORT` / `YYACCEPT` 时不调用；语义动作里应当用 `syntax_error` 或 `my_error` 往错误栈里压错误，再用 `MYSQL_YYABORT` 中止。

### 栈溢出处理：`my_yyoverflow`

`sql/sql_yacc.yy` 重定义了 bison 的栈增长宏 `yyoverflow`：

```
#define yyoverflow(A,B,C,D,E,F,G,H)           \
  {                                           \
    ulong val= *(H);                          \
    if (my_yyoverflow((B), (D), (F), &val))   \
    {                                         \
      yyerror(NULL, YYTHD, NULL, (const char*) (A));\
      return 2;                               \
    }                                         \
    ...
```

即：栈不够时调 `my_yyoverflow` 增长；增长失败则调 `yyerror` 报语法错误并 `return 2`。

`my_yyoverflow` 的实现在 `sql/sql_parse.cc`，两个常量和三个动作都写死了：

- `#define MY_YACC_INIT 1000`（注释 `// Start with big alloc`）。
- `#define MY_YACC_MAX 32000`（注释 `// Because of 'short'`）。
- 若 `*yystacksize >= MY_YACC_MAX` 则 `return true`（失败）。
- 否则 `*yystacksize = set_zone((*yystacksize) * 2, MY_YACC_INIT, MY_YACC_MAX)`——即每次翻倍，最小 1000，最大 32000。
- 用 `my_realloc`（key 为 `key_memory_bison_stack`）重新分配 `yacc_yyvs` / `yacc_yyss` / `yacc_yyls` 三块。首次调用时（`old_info` 非零，即 `!state->yacc_yyvs`）要把原栈 `memcpy` 过去；源码注释解释这是「when replacing a static stack (YYINITDEPTH) by a dynamic stack」。

对应地，`sql/sql_yacc.yy` 里也声明了 bison 的静态栈初值与上限：`#define YYINITDEPTH 100` 与 `#define YYMAXDEPTH 3200`（注释 `/* Because of 64K stack */`）。

### 文法骨架：`start_entry` 与 `sql_statement`

文法本体从 `%%` 开始，第一条规则是 `start_entry`，它有六条分支：

- `sql_statement`：正常语句入口。
- `GRAMMAR_SELECTOR_EXPR bit_expr END_OF_INPUT`：局部解析一个表达式，语义动作 `ITEMIZE($2, &$2);` 后把结果写进 `static_cast<Expression_parser_state *>(YYP)->result`。
- `GRAMMAR_SELECTOR_PART partition_clause END_OF_INPUT`：解析分区子句，`CONTEXTUALIZE($2);` 后把 `&$2->part_info` 写进 `Partition_expr_parser_state::result`。
- `GRAMMAR_SELECTOR_GCOL IDENT_sys '(' expr ')' END_OF_INPUT`：解析生成列表达式。语义动作先检查标识符必须是 `PARSE_GCOL_EXPR`——`if (!is_identifier($2, "PARSE_GCOL_EXPR")) MYSQL_YYABORT;`，再 `NEW_PTN Value_generator` 并 `ITEMIZE($4, &$4);`，把 `gcol_info->expr_item = $4`。
- `GRAMMAR_SELECTOR_CTE table_subquery END_OF_INPUT`：写进 `Common_table_expr_parser_state::result`。
- `GRAMMAR_SELECTOR_DERIVED_EXPR expr END_OF_INPUT`：写进 `Derived_expr_parser_state::result`。

`sql_statement` 规则本身处理三件事：空输入（`END_OF_INPUT`）时，若既不是 bootstrap 系统线程、也没有注释，则 `my_error(ER_EMPTY_QUERY, MYF(0))` 并 `MYSQL_YYABORT`；否则置 `lex->sql_command = SQLCOM_EMPTY_QUERY`。`simple_statement_or_begin` 之后，若协议具备 `CLIENT_MULTI_QUERIES` 能力、且 `lip->multi_statements` 为真、且尚未 `eof()`，则强制 `lip->next_state = MY_LEX_END` 并记 `lip->found_semicolon = lip->get_ptr()`——源码注释说明这是「force the parser to stop after the ';'; mark the start of the next query for the next invocation of the parser」；否则（单语句）置 `lip->found_semicolon = NULL`。

### 语义动作的通用形态

整份文法的语义动作高度模板化，模式是「`NEW_PTN` 造节点 → 判空 → `MYSQL_YYABORT`」。`NEW_PTN` 定义为 `new(YYMEM_ROOT)`，即从 `THD::mem_root` 分配。

典型例子是 `with_list`：

```
with_list:
          with_list ',' common_table_expr
          {
            if ($1->push_back($3))
              MYSQL_YYABORT;
          }
        | common_table_expr
          {
            $$= NEW_PTN PT_with_list(YYTHD->mem_root);
            if ($$ == NULL || $$->push_back($1))
              MYSQL_YYABORT;    /* purecov: inspected */
          }
```

`common_table_expr` 的语义动作会记录子查询文本在原始查询里的偏移：`LEX_STRING subq_text; subq_text.length= @4.cpp.length(); subq_text.str= YYTHD->strmake(@4.cpp.start, subq_text.length);` 以及 `uint subq_text_offset= @4.cpp.start - YYLIP->get_cpp_buf();`，然后 `NEW_PTN PT_common_table_expr($1, subq_text, subq_text_offset, $4, &$2, YYTHD->mem_root)`。

### 查询表达式：`query_expression` 与集合运算

`query_expression` 规则有两条分支——一条带 `with_clause` 一条不带。源码注释解释了为什么不用 `opt_with_clause` 合并：「it would make Bison try to cram both rules into a single state, where it would have to decide whether to reduce a with_clause before seeing the rest of the input. This way we force Bison to parse the entire query expression before trying to reduce.」

```
query_expression:
          query_expression_body opt_order_clause opt_limit_clause
          { $$ = NEW_PTN PT_query_expression($1.body, $2, $3); }
        | with_clause query_expression_body opt_order_clause opt_limit_clause
          { $$= NEW_PTN PT_query_expression($1, $2.body, $3, $4); }
```

`query_expression_body` 描述集合运算，`union_option` 规则给出 `DISTINCT` / `ALL` 的取值：空与 `DISTINCT` 都取 `1`，`ALL` 取 `0`。语义动作分别构造 `PT_union`、`PT_except`、`PT_intersect`，并把右侧的 `is_parenthesized` 一并记下：

```
| query_expression_body UNION_SYM union_option query_expression_body
  { $$ = {NEW_PTN PT_union($1.body, $3, $4.body, $4.is_parenthesized), false}; }
```

`query_primary` 有三个分支：`query_specification`、`table_value_constructor`（`NEW_PTN PT_table_value_constructor($1)`）、`explicit_table`（构造 `PT_select_item_list` 并塞入一个 `Item_asterisk`）。

`query_specification` 的两条分支覆盖「有 INTO」与「无 INTO」两种 SELECT，各自构造 `PT_query_specification`，最后一个参数是 `@5.raw.is_empty()` 或 `@4.raw.is_empty()`，源码注释标为 `// implicit FROM`。

### 表达式：优先级如何变成语义动作

`expr` 规则用 `%prec` 显式标注优先级，并直接决定构造哪种 `Item`：

- `expr or expr %prec OR_SYM` → `flatten_associative_operator<Item_cond_or, Item_func::COND_OR_FUNC>(YYMEM_ROOT, @$, $1, $3)`。
- `expr XOR expr %prec XOR` → `NEW_PTN Item_func_xor(@$, $1, $3)`，注释标注「XOR is a proprietary extension」。
- `expr and expr %prec AND_SYM` → `flatten_associative_operator<Item_cond_and, Item_func::COND_AND_FUNC>`。
- `NOT_SYM expr %prec NOT_SYM` → `NEW_PTN PTI_truth_transform(@$, $2, Item::BOOL_NEGATED)`。
- `bool_pri IS TRUE_SYM %prec IS` → `PTI_truth_transform(@$, $1, Item::BOOL_IS_TRUE)`；`IS not TRUE_SYM` → `Item::BOOL_NOT_TRUE`；`IS FALSE_SYM` → `Item::BOOL_IS_FALSE`；`IS not FALSE_SYM` → `Item::BOOL_NOT_FALSE`。
- `bool_pri IS UNKNOWN_SYM %prec IS` → `NEW_PTN Item_func_isnull(@$, $1)`；`IS not UNKNOWN_SYM` → `NEW_PTN Item_func_isnotnull(@$, $1)`。

注意 `OR` / `AND` 用的是 `flatten_associative_operator`——把同优先级左结合的链压平成一棵平衡的树，而不是左深链。`IS` 系列则拆在 `expr` 与 `bool_pri` 两层规则里，靠 `%prec IS` 保证优先级一致。

`select_item` 规则只两种：`table_wild` 原样透传；`expr select_alias` → `NEW_PTN PTI_expr_with_alias(@$, $1, @1.cpp, to_lex_cstring($2))`。

### `LIMIT` 与 `ORDER BY`

`limit_options` 规则区分三种写法：`limit_option`（单值）、`limit_option ',' limit_option`（先 offset 后 limit，`is_offset_first = true`）、`limit_option OFFSET_SYM limit_option`（先 limit 后 offset）。`limit_option` 可以是标识符（`PTI_limit_option_ident`）、参数标记（`PTI_limit_option_param_marker`）或整数字面量（`Item_uint`）。

`ordering_direction` 只有 `ASC`（`ORDER_ASC`）与 `DESC`（`ORDER_DESC`）两种；`opt_ordering_direction` 为空时取 `ORDER_NOT_RELEVANT`。

### 存储过程体：语法分析器直接生成指令

存储过程体是语法分析器的另一份重负载工作。`sp_proc_stmt` 规则列出 13 种语句形态（`sp_proc_stmt_statement`、`sp_proc_stmt_return`、`sp_proc_stmt_if`、`case_stmt_specification`、`sp_labeled_block`、`sp_unlabeled_block`、`sp_labeled_control`、`sp_proc_stmt_unlabeled`、`sp_proc_stmt_leave`、`sp_proc_stmt_iterate`、`sp_proc_stmt_open`、`sp_proc_stmt_fetch`、`sp_proc_stmt_close`）。

`sp_proc_stmt_statement` 的语义动作是理解这套设计的钥匙：先 `sp->reset_lex(thd)` 与 `sp->m_parser_data.set_current_stmt_start_ptr(yylloc.raw.start)`，然后解析 `simple_statement`，再 `MAKE_CMD($2)` 造出 `Sql_cmd`；接着 `sp->m_flags |= sp_get_flags_for_command(lex)`；若 `lex->sql_command == SQLCOM_CHANGE_DB` 则报 `ER_SP_BADSTATEMENT`（注释：「"USE db" doesn't work in a procedure」）；若 `lex->m_sql_cmd != NULL` 则 `lex->m_sql_cmd->set_as_part_of_sp()`；最后用 `make_string(thd, sp->m_parser_data.get_current_stmt_start_ptr(), @2.raw.end)` 把语句原文从 token 流里截出来，构造 `sp_instr_stmt` 并 `sp->add_instr(thd, i)`。

`sp_decls` 规则用累加计数的方式检测声明顺序违规，源码注释解释原因：「We check for declarations out of (standard) order this way because letting the grammar rules reflect it caused tricky shift/reduce conflicts with the wrong result. (And we get better error handling this way.)」违规时分别报 `ER_SP_VARCOND_AFTER_CURSHNDLR`（变量或条件出现在游标/handler 之后）与 `ER_SP_CURSOR_AFTER_HANDLER`（游标出现在 handler 之后）。

`CASE` 语句走三个辅助函数 `case_stmt_action_case`、`case_stmt_action_then`、`case_stmt_action_end_case`，它们在语法动作里直接操作 `sp_pcontext` 的标签栈与回填（backpatch）。这些函数的注释里给出了一段完整的 `SHOW PROCEDURE CODE` 输出示例，说明 backpatch 是为生成跳转指令服务的。

### 局部语法分析：五种 selector 的真实调用方

五种 `GRAMMAR_SELECTOR_*` 分支都有可查的实际调用方：

| selector | 状态类 | 真实调用方 |
|---|---|---|
| `GRAMMAR_SELECTOR_EXPR` | `Expression_parser_state` | 定义在 `sql/sql_lex.cc`；由 `sql/sql_yacc.yy` 的 `start_entry` 分支消费 |
| `GRAMMAR_SELECTOR_PART` | `Partition_expr_parser_state` | `sql/sql_partition.cc` 的 `mysql_unpack_partition()`：`Partition_expr_parser_state parser_state; parser_state.init(thd, part_buf, part_info_len)`，随后 `parse_sql(thd, &parser_state, nullptr)` |
| `GRAMMAR_SELECTOR_GCOL` | `Gcol_expr_parser_state` | `sql/table.cc` 的 `unpack_value_generator()`：`Gcol_expr_parser_state parser_state; parser_state.init(thd, gcol_expr_str, str_len);` 后 `parse_sql(thd, &parser_state, nullptr)` |
| `GRAMMAR_SELECTOR_CTE` | `Common_table_expr_parser_state` | `sql/sql_parse.cc` 的 `reparse_common_table_expr()`：建 `Common_table_expr_parser_state parser_state;` 并 `parser_state.init(thd, text, text_length)`，然后直接调 `thd->sql_parser()`（源码注释说明「It intentionally uses THD::sql_parser() directly without the parse_sql() wrapper」） |
| `GRAMMAR_SELECTOR_DERIVED_EXPR` | `Derived_expr_parser_state` | `sql/sql_derived.cc` 的 `parse_expression()`：`Derived_expr_parser_state parser_state;` 后 `parse_sql(thd, &parser_state, view_creation_ctx)` |

`reparse_common_table_expr()` 里对局部解析做了三处专门设置：`parser_state.m_lip.stmt_prepare_mode = old->m_lip.stmt_prepare_mode;`、`parser_state.m_lip.multi_statements = false;`（源码注释 `// A safety measure.`）、`parser_state.m_lip.m_digest = nullptr;`。

`sql/sql_derived.cc` 的 `parse_expression()` 则是「把已构造好的 Item 打印成文本再重新解析」的克隆路径：先 `item->print(thd, &str, enum_query_type(QT_NO_DATA_EXPANSION | QT_TO_ARGUMENT_CHARSET))` 把表达式打印成 SQL 文本，然后建 `Derived_expr_parser_state` 去重新解析。源码注释解释两个 flag 的用途：前者保证重预备时 `?` 仍打印成 `?` 而不是实际数据，后者保证字符集引导符正确。

### 从解析树到 `Sql_cmd`

`LEX::make_sql_cmd(Parse_tree_root *)` 在 `sql/sql_lex.cc`：

```
bool LEX::make_sql_cmd(Parse_tree_root *parse_tree) {
  if (!will_contextualize) return false;
  m_sql_cmd = parse_tree->make_cmd(thd);
  if (m_sql_cmd == nullptr) return true;
  assert(m_sql_cmd->sql_command_code() == sql_command);
  return false;
}
```

`Sql_cmd` 的具体生成在 `sql/parse_tree_nodes.cc`。以 `PT_select_stmt::make_cmd(THD *)` 为例，函数体顺序是：置 `thd->lex->sql_command = m_sql_command` → `m_qe->contextualize(&pc)` → 检查 `thd->lex->result != nullptr`（即查询块内部已有 INTO）与 `m_into != nullptr` 同时成立时报 `ER_MULTIPLE_INTO_CLAUSES` → `contextualize_safe(&pc, m_into)` → `pc.finalize_query_expression()` → 按 `m_into != nullptr && m_has_trailing_locking_clauses` 等条件压 `ER_WARN_DEPRECATED_INNER_INTO` 弃用警告 → 最后按 `thd->lex->sql_command == SQLCOM_SELECT` 返回 `new (thd->mem_root) Sql_cmd_select(thd->lex->result)`，否则返回 `Sql_cmd_do(nullptr)`。

集合运算的语义化在 `PT_set_operation::contextualize_setop()`：压 `QueryLevel` 栈、归约左右两侧、调 `pc->thd->lex->new_set_operation_query(pc->select)` 建新的查询块、按 `setop_type` 分别 `new (pc->mem_root) Query_term_union` / `Query_term_except` / `Query_term_intersect`，再 `merge_descendants(pc, setop, ql)` 与 `setop->label_children()`，最后 `setop->set_block(qe->create_post_processing_block(setop))`。`PT_union::contextualize()` 传的是 `m_is_distinct ? SC_UNION_DISTINCT : SC_UNION_ALL`，`PT_except` / `PT_intersect` 同理。

### 弃用与告警：语法动作里直接压警告

语法动作里大量直接 `push_warning` / `push_deprecated_warn`，把「语法上合法但已弃用」这件事在解析期就报出来：

- `SQL_CALC_FOUND_ROWS` → `push_warning(YYTHD, Sql_condition::SL_WARNING, ER_WARN_DEPRECATED_SYNTAX, ER_THD(YYTHD, ER_WARN_DEPRECATED_SQL_CALC_FOUND_ROWS));`，选项值仍取 `OPTION_FOUND_ROWS`。
- `SQL_NO_CACHE_SYM` → `push_deprecated_warn_no_replacement(YYTHD, "SQL_NO_CACHE");`，源码注释写明「Ignored since MySQL 8.0.」，选项值置 `0`。
- `MASTER_AUTO_POSITION`、`MASTER_HOST`、`MASTER_BIND` 等复制相关选项 → `push_deprecated_warn(YYTHD, "MASTER_AUTO_POSITION", "MASTER_AUTO_POSITION")` 这类成对调用。
- `STOP SLAVE` / `START SLAVE` → 分别压 `"STOP SLAVE"` → `"STOP REPLICA"`、`"START SLAVE"` → `"START REPLICA"` 的弃用警告。
- `YEAR(4)` → `push_deprecated_warn(YYTHD, "YEAR(4)", "YEAR")`。
- `ZEROFILL` → `push_warning(... ER_WARN_DEPRECATED_ZEROFILL)`。
- `ASCII` / `UNICODE` 关键字 → 分别压 `"ASCII"` → `"CHARACTER SET charset_name"`、`"UNICODE"` → `"CHARACTER SET charset_name"`。
- `nat_charset` 相关 → `warn_about_deprecated_national(YYTHD)`。

### 调试开关

`sql/sql_yacc.yy` 里有一个仅在 debug 构建下存在的开关 `turn_parser_debug_on()`，它把 bison 的 `yydebug` 置 1。函数注释给出了用法：`mysqld --debug="d,parser_debug" ...` 或 `mysql-test-run.pl --mysqld="--debug=d,parser_debug" ...`，输出到 `var/log/master.err`。`YYDEBUG` 宏在 `#ifndef NDEBUG` 时为 1，否则为 0。

## 关键取舍

**用 bison 生成 LALR(1) 自动机，而不是手写递归下降。** 收益是整套 SQL 方言（包括 `UNION`/`EXCEPT`/`INTERSECT`、`WITH`、`CTE`、分区、存储过程体、触发器、事件、复制管理语句）只需要维护一份 `.yy` 文件，新增语句就是加一条规则。代价是冲突必须靠优先级声明与人工记号来收拾——文件里 `%expect 63` 明确承认了 63 个移进/归约冲突的存在，且维护者注释禁止再引入新的移进/归约冲突。

**把「关键字当标识符」当成一等公民，用三个记号表达。** `%left KEYWORD_USED_AS_IDENT`、`%nonassoc TEXT_STRING`、`%left KEYWORD_USED_AS_KEYWORD` 三个记号解决 `SELECT TIMESTAMP'...'` 与 `CREATE TABLE t1 (timestamp INT)` 这类歧义。源码注释写明设计意图：「The use as an ident is allowed, but must never take precedence over the use as an actual keyword.」

**在语法阶段就把「语法合法但语义上非法」的部分规则挑出来。** 典型的三例：`INTO` 只能出现在顶层——`sql/sql_yacc.yy` 的注释说「We solve this by manually throwing parse errors whenever we reduce a nested query expression if it contains an into clause」，实际的错误码 `ER_MULTIPLE_INTO_CLAUSES` 是在 `sql/parse_tree_nodes.cc` 的 `PT_select_stmt::make_cmd` 里抛的；`sp_decls` 的声明顺序检查；`COMMIT AND CHAIN RELEASE` 这类互斥组合用 `MYSQL_YYABORT_UNLESS($3 != TVL_YES || $4 != TVL_YES)` 拒绝。

**用人工记号把「同一份文法」复用成五种局部解析。** `GRAMMAR_SELECTOR_EXPR` / `_GCOL` / `_PART` / `_CTE` / `_DERIVED_EXPR` 五个 synthetic token 让「解析一个表达式」「解析生成列表达式」这类需求复用主文法的全部规则，而不必另写小文法。代价是局部解析路径必须显式关掉摘要计算——`parse_sql` 里的断言 `assert(!parser_state->m_lip.is_partial_parser() || !parser_state->m_input.m_has_digest);` 就是这条纪律的守卫。

**给 bison 栈做动态增长，而不是靠静态栈够用。** 静态栈是 `YYINITDEPTH 100` / `YYMAXDEPTH 3200`（注释 `/* Because of 64K stack */`），但 `my_yyoverflow` 会在需要时把它换成动态栈，上限 `MY_YACC_MAX 32000`（注释 `// Because of 'short'`），每次翻倍、最小 `MY_YACC_INIT 1000`。失败时不是崩溃而是走 `yyerror` 报语法错误并 `return 2`。代价是 bison 的 `short` 类型限制了栈上限。

**把解析内存占用变成可配置的上限，而不是靠 OOM 兜底。** `parse_sql` 在解析前设 `thd->mem_root->set_max_capacity(thd->variables.parser_max_mem_size)`，并由 `Parser_oom_handler` 把 `EE_CAPACITY_EXCEEDED` 转成 `ER_CAPACITY_EXCEEDED`（消息里带上限值和变量名 `parser_max_mem_size`）。源码注释说「Out of memory error is reported only once. Return as handled」——即只报一次，避免同一处 OOM 触发多条错误。

**语法树节点全部挂在 `THD::mem_root` 上，语句结束即整体回收。** `NEW_PTN` 就是 `new(YYMEM_ROOT)`，而 `YYMEM_ROOT` 就是 `YYTHD->mem_root`。代价是一旦语句结束，解析树不可持久化——所以 CTE 重解析与派生表条件克隆都必须「打印成文本再重新解析」。

**记号编号冻结，把摘要稳定性放在编译期强制。** 新增记号必须追加到 8.0 段末尾，删除的记号改名成 `OBSOLETE_TOKEN_<NNN>`，违规由 `gen_lex_token` 触发构建失败。代价是记号表里留着一堆 `OBSOLETE_TOKEN_*` 死记号。

**把 `INTO` 位置歧义交给优先级解决，而不是复制整棵语法树。** `sql/sql_yacc.yy` 的注释直说：写一份无歧义文法「would force us to duplicate the entire <select statement> syntax all the way down to the <into clause>」，所以「we solve it by writing an ambiguous grammar and use precedence rules to sort out the shift/reduce conflict」。

**用一个前瞻记号换取 LALR(1) 的文法（与词法层的配合）。** `sql/sql_lex.cc` 的 `MYSQLlex` 在遇到 `WITH` 时会再取一个记号，若第二个是 `ROLLUP_SYM` 就合并返回 `WITH_ROLLUP_SYM`。这直接决定了 `sql/sql_yacc.yy` 里 `WITH` 规则的形状——语法分析器因此不需要 2 个前瞻。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_yacc.yy` | `MYSQLparse` | bison 生成的解析器入口，源码注释描述其成功/失败返回约定 |
| `sql/sql_yacc.yy` | `yyparse` | 文件头注释指出 `YYTHD` 经由 `yyparse()` 传给 `yylex()` |
| `sql/sql_yacc.yy` | `MYSQLerror` | bison 错误回调：区分 "syntax error" / "memory exhausted" / 其他 |
| `sql/sql_yacc.yy` | `yyoverflow` | 重定义 bison 栈增长宏，失败时 `yyerror` 并 `return 2` |
| `sql/sql_yacc.yy` | `CONTEXTUALIZE` / `CONTEXTUALIZE_VIEW` / `ITEMIZE` | 解析树节点语义化与表达式 itemize 的包装宏 |
| `sql/sql_yacc.yy` | `MAKE_CMD` | `LEX::make_sql_cmd` 的包装宏，把 OOM 时的错误消息提出来 |
| `sql/sql_yacc.yy` | `NEW_PTN` | `new(YYMEM_ROOT)`，所有解析树节点的分配方式 |
| `sql/sql_yacc.yy` | `start_entry` | 文法入口，六条分支覆盖正常语句与五种局部解析 selector |
| `sql/sql_yacc.yy` | `sql_statement` | 处理空查询、多语句的 `found_semicolon` 标记与 `;` 结束 |
| `sql/sql_yacc.yy` | `simple_statement_or_begin` | 语句与 `BEGIN`（事务/存储程序块）的分派点 |
| `sql/sql_yacc.yy` | `query_expression` | 构造 `PT_query_expression`，带/不带 `with_clause` 两条分支 |
| `sql/sql_yacc.yy` | `query_expression_body` | 集合运算：`PT_union` / `PT_except` / `PT_intersect` 的构造 |
| `sql/sql_yacc.yy` | `union_option` | `DISTINCT`/`ALL` 取值：空与 `DISTINCT` 为 1，`ALL` 为 0 |
| `sql/sql_yacc.yy` | `query_specification` | 构造 `PT_query_specification`，区分有无 INTO 两型 |
| `sql/sql_yacc.yy` | `expr` | 表达式优先级规则：`flatten_associative_operator`、`Item_func_xor`、`PTI_truth_transform` |
| `sql/sql_yacc.yy` | `bool_pri` | 比较运算与 `IS NULL` / `IS NOT NULL` 的规则 |
| `sql/sql_yacc.yy` | `select_item` / `select_item_list` | 投影列表构造，含 `Item_asterisk` 分支 |
| `sql/sql_yacc.yy` | `limit_options` / `limit_option` | LIMIT 的三种写法与三种取值来源（标识符/参数标记/整数字面量） |
| `sql/sql_yacc.yy` | `with_clause` / `with_list` / `common_table_expr` | CTE 构造，记录子查询文本偏移 `subq_text_offset` |
| `sql/sql_yacc.yy` | `sp_proc_stmt` | 存储过程体的 13 种语句形态 |
| `sql/sql_yacc.yy` | `sp_proc_stmt_statement` | SP 内嵌语句：造 `Sql_cmd`、截原文、生成 `sp_instr_stmt` 指令 |
| `sql/sql_yacc.yy` | `sp_decls` | SP 声明顺序校验，报 `ER_SP_VARCOND_AFTER_CURSHNDLR` / `ER_SP_CURSOR_AFTER_HANDLER` |
| `sql/sql_yacc.yy` | `case_stmt_action_case` / `case_stmt_action_then` / `case_stmt_action_end_case` | CASE 语句的标签栈与 backpatch 操作 |
| `sql/sql_yacc.yy` | `is_native_function` | 判断名字是否为原生函数：`find_native_function_builder` 或 `is_lex_native_function` |
| `sql/sql_yacc.yy` | `init_index_hints` | 给索引提示列表逐项设置 `type` 与 `clause` |
| `sql/sql_yacc.yy` | `turn_parser_debug_on` | debug 构建下打开 bison 的 `yydebug` 跟踪 |
| `sql/sql_class.cc` | `THD::sql_parser` | 调 `MYSQLparse`，失败时 `cleanup_after_parse_error`，成功时 `lex->make_sql_cmd(root)` |
| `sql/sql_class.cc` | `THD::cleanup_after_parse_error` | 解析出错时恢复 SP 相关状态，避免在 THD 上留副作用 |
| `sql/sql_parse.cc` | `parse_sql` | `THD::sql_parser()` 的包装：摘要装配、内存上限、诊断区隔离与收尾 |
| `sql/sql_parse.cc` | `Parser_oom_handler` | 把 `EE_CAPACITY_EXCEEDED` / `EE_OUTOFMEMORY` 转成 `ER_CAPACITY_EXCEEDED` / `ER_OUT_OF_RESOURCES` |
| `sql/sql_parse.cc` | `my_yyoverflow` | bison 栈动态增长：`MY_YACC_INIT 1000`、`MY_YACC_MAX 32000`，每次翻倍 |
| `sql/sql_parse.cc` | `reparse_common_table_expr` | CTE 重解析：`Common_table_expr_parser_state` + 直接调 `thd->sql_parser()` |
| `sql/sql_lex.cc` | `LEX::make_sql_cmd` | 解析树转 `Sql_cmd`；`will_contextualize` 为假时直接返回 |
| `sql/sql_lex.cc` | `LEX::LEX` | `LEX` 构造函数，`will_contextualize(true)` 为初值 |
| `sql/sql_lex.cc` | `Partition_expr_parser_state` / `Gcol_expr_parser_state` / `Expression_parser_state` / `Common_table_expr_parser_state` / `Derived_expr_parser_state` | 五种局部解析状态的构造，各自绑定一个 `GRAMMAR_SELECTOR_*` |
| `sql/parse_tree_nodes.cc` | `PT_select_stmt::make_cmd` | 语义化 `PT_query_expression`，检查 INTO 冲突，报 `ER_MULTIPLE_INTO_CLAUSES` |
| `sql/parse_tree_nodes.cc` | `PT_set_operation::contextualize_setop` | 集合运算语义化：建 `Query_term_union` / `_except` / `_intersect` 并回填 |
| `sql/parse_tree_nodes.cc` | `PT_union::contextualize` / `PT_except::contextualize` / `PT_intersect::contextualize` | 按 `m_is_distinct` 选 `SC_UNION_DISTINCT` / `SC_UNION_ALL` 等上下文 |
| `sql/parse_tree_nodes.cc` | `PT_query_expression::contextualize_order_and_limit` | ORDER BY / LIMIT 能否被查询体吸收的判定与上下文切换 |
| `sql/table.cc` | `unpack_value_generator` | 生成列表达式重解析：`Gcol_expr_parser_state` + `parse_sql` |
| `sql/sql_partition.cc` | `mysql_unpack_partition` | 分区信息重解析：`Partition_expr_parser_state` + `parse_sql` |
| `sql/sql_derived.cc` | `parse_expression` | 派生表条件克隆：打印成文本后用 `Derived_expr_parser_state` 重解析 |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 前端是手写递归下降，语法与语义在同一次递归里完成，没有独立的解析树节点层，也没有 bison 那一套。

**架构推论：** MySQL 语法分析器有三处结构性设计值得主核借鉴。其一是**「同一份文法复用成多种局部解析」**：`GRAMMAR_SELECTOR_*` 五个 synthetic token 让「解析整条语句」和「解析一个表达式」共用同一套规则，局部解析路径只需换掉起始记号并关掉摘要。这避免了为派生表条件、分区表达式、生成列表达式各写一套小解析器。其二是**「语义下沉到专门的节点方法」**：解析树节点自己带 `contextualize()` / `itemize()` / `make_cmd()`，语法动作只负责造节点，因此「解析」与「绑定/求值」的边界非常清晰，出错时的错误码也落在具体节点上。其三是**「冲突被显式记账」**：`%expect 63` 把已知的移进/归约冲突数量写进构建，任何人新增规则引入新冲突都会立刻构建失败，而不是悄悄改变解析行为。

对 AI 场景而言，语法分析器是「模型生成 SQL」这道关口里最结构化的一层。MySQL 的做法值得借鉴的是它把**「歧义」当成显式设计对象而不是缺陷**：每个歧义点都配一个具名的人工记号（`SUBQUERY_AS_EXPR`、`EMPTY_FROM_CLAUSE`、`KEYWORD_USED_AS_IDENT`），并在源码注释里写清楚「为什么这里有歧义、为什么这样消解」。这对模型生成的 SQL 尤其重要——模型很容易写出 `SELECT ... INTO ... FROM ...` 这类位置敏感、或者把保留字当列名的语句；一套「歧义点有名字、有注释、有消解手段」的文法，比事后在执行器里报错要可控得多。另一处可直接照搬的是**「语法阶段就压弃用警告」**：`SQL_CALC_FOUND_ROWS`、`SQL_NO_CACHE`、`YEAR(4)`、`ASCII`/`UNICODE` 这些都在语义动作里直接 `push_deprecated_warn`，模型生成的 SQL 里出现这些写法时会在解析期就得到带位置的明确提示，而不是在优化器里被静默忽略。

## 参考

- [MySQL 源码：`sql/sql_yacc.yy`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_yacc.yy)
- [MySQL 源码：`sql/sql_class.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_class.cc)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 源码：`sql/sql_lex.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex.cc)
- [MySQL 源码：`sql/parse_tree_nodes.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/parse_tree_nodes.cc)
- [MySQL 源码：`sql/sql_derived.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_derived.cc)
- [MySQL 源码：`sql/table.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/table.cc)
- [MySQL 源码：`sql/sql_partition.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_partition.cc)
- [MySQL 源码：`sql/sql_hints.yy`（GitHub 8.0 分支，提示的独立 bison 文法）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_hints.yy)
- [MySQL 参考手册：WITH 子句（CTE）](https://dev.mysql.com/doc/refman/8.0/en/with.html)
- [MySQL 参考手册：UNION 操作符](https://dev.mysql.com/doc/refman/8.0/en/union.html)
- [MySQL 参考手册：SELECT ... INTO](https://dev.mysql.com/doc/refman/8.0/en/select-into.html)
- [MySQL 参考手册：存储程序](https://dev.mysql.com/doc/refman/8.0/en/stored-programs.html)
- [MySQL Server 开发文档：Parser](https://dev.mysql.com/doc/dev/mysql-server/latest/page_parser.html)
