---
project: mysql-server
feature: lexer
page_type: feature
schema_version: 1
refs:
  - path: sql/sql_lex.cc
    symbols: [lex_start]
---

# 词法分析器

## 一句话职责

MySQL 的词法分析器把 SQL 文本切成记号流：它持有一份「原始缓冲区 + 预处理缓冲区」双份输入（`Lex_input_stream`），按字符集的状态表逐字符扫描，产出标识符、关键字、数值、字符串、运算符、注释与提示（hint）等记号，同时维护字符集状态、语句摘要（digest）与 utf8 正文镜像。

## 机制是什么

**词法分析器不是独立程序，而是 bison 语法分析器的取词器。** `sql/mysqld.cc` 里那张调用顺序图把链路写得很直白：`server -> parser: THD::sql_parser()` → `parser -> bison: MYSQLparse()` → `bison -> lexer: MYSQLlex()` → `bison <-- lexer: yylval, yylloc`。也就是说 bison 每需要一个记号就调一次 `MYSQLlex()`，取回记号编号与它的语义值；语法树的构造由 bison 完成，词法分析器只负责「切」。

**扫描器本体是一个状态机。** `lex_one_token()` 用 `enum my_lex_states` 驱动：`MY_LEX_START` 先用字符集状态表 `state_map` 把空白字符（状态 `MY_LEX_SKIP`）跳过，再取第一个真实字符，按 `state_map[c]` 跳进对应分支处理。状态表本身来自字符集：每个 `CHARSET_INFO` 带三张表——`main_map`（正文用）、`hint_map`（提示注释用）、`ident_map`（哪些字节算标识符字符）。扫描器每次取字符都从 `cs->state_maps->main_map` 与 `cs->ident_map` 取，因此换字符集等于换扫描规则。

**输入是双缓冲。** `Lex_input_stream::init()` 用 `thd->alloc(length + 1)` 分配一块 `m_cpp_buf` 作为「预处理缓冲区」，注释里写明这是为了「防止服务端处理大型多语句查询时内存污染」；而 `m_buf` 指向原始查询文本。`reset()` 每次换语句时把 `m_ptr`、`m_end_of_query`、`m_buf`、`m_buf_length` 重新指向新文本，但**不重新分配** `m_cpp_buf`。

**词法分析器同时维护字符集相关状态**，这是它区别于普通扫描器的地方：查询正文里可能出现多种字符集的字符串字面量（`_latin1'...'`、`_utf8mb4'...'`），扫描器要记住「当前正在读的字符串用的是哪个字符集」，并在需要时把正文重新编码成 utf8 以便后续的 utf8 正文镜像使用。

**它还兼职做两件别的事：** 一是语句摘要（digest）的记号级归约——`add_digest_token()` / `reduce_digest_token()` 把记号喂给摘要缓冲区，使得 `SELECT a FROM t` 与 `SELECT b FROM t` 归一成同一个摘要；二是优化器提示注释的切出——遇到 `/*+ ... */` 时切给独立的 `Hint_scanner`。

## 怎么实现的

### 入口：`lex_start` 与 LEX 的初始化

`lex_start(THD *thd)` 是「准备解析一条新语句」的唯一入口，函数体只有五步：`lex->thd = thd` → `lex->reset()` → `thd->init_cost_model()` → `lex->new_top_level_query()` → `lex->m_current_query_block = lex->query_block`，最后返回 `new_top_level_query()` 的结果。函数上方的注释明确写着「在每条要准备并执行的查询之前调用 `lex_start()`，因此这里绝对不能做太多事（我们已经做太多了）」。

`LEX::reset()` 负责把上一条语句的残留清干净：`unit`、`query_block`、`m_current_query_block` 全部置空（它们指向已被回收的 mem_root），并复位 `parsing_options`、`insert_table`、`part_info`、`duplicates`、`ignore`、`wild`、`spname`、`sphead`、`m_sql_cmd` 等几十个字段。

`LEX::new_top_level_query()` 建顶层查询块：`query_block = new_query(nullptr)`，再 `unit = query_block->master_query_expression()`；`new_query` 返回空则 `return true`（注释标为 `purecov: inspected`）。

**真实调用方：** `sql/sql_parse.cc` 的 `dispatch_sql_command()` 在 `mysql_reset_thd_for_next_command(thd)` 与 `thd->reset_rewritten_query()` 之后调 `lex_start(thd)`，紧接着设 `thd->m_parser_state = parser_state` 并调 `invoke_pre_parse_rewrite_plugins(thd)`。`sql/sql_prepare.cc` 的预处理语句路径在设完 `parser_state.m_lip.stmt_prepare_mode = true` 与 `parser_state.m_lip.multi_statements = false` 之后调 `lex_start(thd)`，随后置 `m_lex->context_analysis_only |= CONTEXT_ANALYSIS_ONLY_PREPARE`。`sql/sql_derived.cc` 在重解析派生表时也调 `lex_start(thd)`。

### 取词主循环：`MYSQLlex`

`MYSQLlex(YYSTYPE *yacc_yylval, YYLTYPE *yylloc, THD *thd)` 是交给 bison 的取词函数，三段结构：

1. **错误短路。** 若 `thd->is_error()` 且 `thd->get_parser_da()->has_sql_condition(ER_CAPACITY_EXCEEDED)`，直接返回 `ABORT_SYM`。
2. **预读回放。** 若 `lip->lookahead_token >= 0`，说明这个记号上次已经取过了，直接回放：把 `lookahead_token` 置 -1、`*yylval = *(lip->lookahead_yylval)`，填 `yylloc` 的 `cpp` 与 `raw` 两组区间，然后返回。
3. **正常取词。** 调 `lex_one_token(yylval, thd)`。

取词之后有一个特例：`MYSQLlex` 遇到 `WITH` 会**再取一个记号**。若第二个是 `ROLLUP_SYM`，合并返回 `WITH_ROLLUP_SYM`；否则把第二个记号存进 `lip->lookahead_yylval` 与 `lip->lookahead_token`，只返回 `WITH`。源码注释写明了原因：解析 `WITH` `ROLLUP` 需要 2 个前瞻，会让文法变成 LALR(2)，合并成单个 `WITH_ROLLUP` 记号后文法就是 LALR(1)，`sql_yacc.yy` 才能处理。

### 单记号扫描：`lex_one_token`

`lex_one_token()` 先取 `state = lip->next_state`（上一次留下的状态，默认为 `MY_LEX_START`），然后在一个 `for (;;)` 里按状态分派。关键分支：

**标识符。** `MY_LEX_IDENT` 分支按字符集分两路。`use_mb(cs)` 为真时逐字符用 `my_ismbchar()` 判断多字节字符长度并 `skip_binary(l - 1)` 跳过其余字节；否则走 `for (result_state = c; ident_map[c = lip->yyGet()]; result_state |= c)` 单字节快路。非 ASCII 字符（`result_state & 0x80`）会被标记成 `IDENT_QUOTED` 而非 `IDENT`。

**关键字查表。** 标识符扫完后调 `find_keyword(lip, length, c == '(')`。第三个参数 `function` 决定查哪张表：带 `(` 时查 `Lex_hash::sql_keywords_and_funcs`（关键字 ∪ 内置函数名），否则查 `Lex_hash::sql_keywords`。`find_keyword` 命中后把符号指针与长度写进 `yylval->keyword`，并按 `sql_mode` 做两处变形：`NOT_SYM` 在 `MODE_HIGH_NOT_PRECEDENCE` 下变成 `NOT2_SYM`；`OR_OR_SYM` 在没有 `MODE_PIPES_AS_CONCAT` 时压一条弃用警告并返回 `OR2_SYM`。

**哈希查找本身在 `sql/sql_lex_hash.cc`。** `Lex_hash::get_hash_symbol()` 是一张以长度分桶的字符树：先用 `uint4korr(hash_map + ((len - 1) * 4))` 取出该长度桶的首节点，再逐字节用 `to_upper_lex[]` 折叠大小写后向下走；`len > entry_max_len` 直接返回 `nullptr`。注意 `to_upper_lex` 表只覆盖 latin1 范围，源码注释写明「该数据基于 latin1 字符集，仅在比较关键字时使用」。

**数值。** `MY_LEX_NUMBER_IDENT` 处理以数字开头的记号：`0x` 开头且后面全是十六进制数字则返回 `HEX_NUM`；`0b` 开头且后面全是 0/1 则返回 `BIN_NUM`；否则进 `MY_LEX_INT_OR_REAL`。`MY_LEX_REAL` 分支处理小数与科学计数法：`e`/`E` 后可带符号，但符号后必须有数字，否则 `state = MY_LEX_CHAR` 回退逐字符处理。

**整数类型判定在 `int_token()`。** 它按长度与字典序决定记号类型 `NUM` / `LONG_NUM` / `ULONGLONG_NUM` / `DECIMAL_NUM`。判定用的常量都写在文件里：`long_str = "2147483647"`（`long_len = 10`）、`signed_long_str = "-2147483648"`、`longlong_str = "9223372036854775807"`（`longlong_len = 19`）、`signed_longlong_str = "-9223372036854775808"`（`signed_longlong_len = 19`）、`unsigned_longlong_str = "18446744073709551615"`（`unsigned_longlong_len = 20`）。长度小于 `long_len` 时直接返回 `NUM`；否则先剥掉符号与前导零，再按长度落到对应区间，最后用 `lex_casecmp` 式的逐字符比较定出是区间下界还是上界。

**字符串。** `get_text(lip, pre_skip, post_skip)` 负责取并反转义字符串字面量。转义语义受 `MODE_NO_BACKSLASH_ESCAPES` 控制：未开该模式时 `\` 起始的是转义序列，处理表在 `get_text` 的 `switch` 里逐项给出——`n` → `'\n'`、`t` → `'\t'`、`r` → `'\r'`、`b` → `'\b'`、`0` → NUL、`Z` → `'\032'`（注释说明 `^Z` 在 Win32 上必须转义）、`_` 与 `%` → 保留反斜杠（注释说明是为了记住通配符前缀）；其余字符原样输出。连续两个引号按「转义一个引号」处理。找到结束引号后，若 `found_escape` 为真就走逐字符展开的慢路，否则直接 `memcpy`。

`MY_LEX_STRING_OR_DELIMITER` 分支先看 `MODE_ANSI_QUOTES`：开了就把 `"` 当成引用标识符的分隔符（转到 `MY_LEX_USER_VARIABLE_DELIMITER`），没开则 `"` 走字符串分支。

**带引号的标识符。** `MY_LEX_USER_VARIABLE_DELIMITER` 分支处理反引号或双引号包裹的标识符：循环里遇到与起始相同的引号时，若下一个字符仍是同一引号则计一次 `double_quotes`（即 ` `` ` 转义），否则结束。结束时若 `double_quotes` 非零走 `get_quoted_token()`（去掉重复引号），否则走 `get_token()`。读到 NUL 且不匹配则返回 `ABORT_SYM`（注释标注为「Unmatched quotes」）。

**注释。** `--` 后面必须跟空白或控制字符才被当作注释（`my_isspace(cs, ...) || my_iscntrl(cs, ...)`），否则 `--` 是减减号运算符。`/* ... */` 由 `consume_comment()` 消费，只允许**一层**嵌套（`remaining_recursions_permitted` 只能是 0 或 1，函数里有 `assert` 守住）：遇到嵌套的 `/*` 时压弃用警告 `ER_WARN_DEPRECATED_NESTED_COMMENT_SYNTAX`，并把 `/*` 改写成 `(*`、把对应的 `*/` 改写成 `*)` 绕过去。

**版本条件注释。** `/*! ... */` 走 `MY_LEX_LONG_COMMENT` 分支：`/*!` 后面必须严格是 5 位数字（源码注释写明「1 位 major + 2 位 minor + 2 位 dot」，并举例 `32302 -> 3.23.02`、`50032 -> 5.0.32`、`50114 -> 5.1.14`），第 6 位不是空白则压 `ER_WARN_NO_SPACE_VERSION_COMMENT` 警告。解析出的 `version` 与 `MYSQL_VERSION_ID` 比较：`version <= MYSQL_VERSION_ID` 时把注释内容**当作真实代码展开**（`set_echo(true)` 后回到 `MY_LEX_START`）；否则把整段注释用 `yyUnput(' ')` 改写成空白丢弃，注释说明这是为了「避免它被无限传播（例如传播给从库）」。

**普通 `/* */` 注释的替换规则。** `MY_LEX_END_LONG_COMMENT` 分支在关闭注释时按 C 的做法：若当前指针位置的前一个字符不是空白，则用 `cpp_inject(' ')` 注入一个空格——源码注释引用了 ISO/IEC 9899:1999 §5.1.1.2。

**占位符与多语句。** `MY_LEX_CHAR` 分支里 `?` 只有在 `lip->stmt_prepare_mode` 为真且下一个字符不是标识符字符时才返回 `PARAM_MARKER`；源码注释解释了这个约束的理由：「因为 binlog：当占位符在写 binlog 的查询里被替换成它的值时，查询必须保持语法正确」。`sql/sql_prepare.cc` 是设置 `stmt_prepare_mode = true` 的地方。`multi_statements` 标志由 `sql/sql_yacc.yy` 的 `simple_statement_or_begin` 规则消费：只有当协议有 `CLIENT_MULTI_QUERIES` 能力、且 `lip->multi_statements` 为真、且尚未 eof 时，才把 `lip->next_state` 置为 `MY_LEX_END` 并记录 `lip->found_semicolon`。

**多语句的记号。** `MY_LEX_EOL` 分支在 `lip->eof()` 时置 `lip->next_state = MY_LEX_END` 并返回 `END_OF_INPUT`；`MY_LEX_END` 分支保持 `next_state = MY_LEX_END` 并返回 0。

**JSON 运算符。** `->` 与 `->>` 在 `MY_LEX_CHAR` 分支里特判：读到 `->` 后若再一个字符是 `>`，返回 `JSON_UNQUOTED_SEPARATOR_SYM`，否则返回 `JSON_SEPARATOR_SYM`。

**优化器提示。** `find_keyword` 里若命中的符号带 `SG_HINTABLE_KEYWORDS` 标志，就调 `consume_optimizer_hints(lip)` 切出 `/*+ ... */` 交给独立的提示扫描器；解析失败时 `find_keyword` 返回 `ABORT_SYM`，成功时置 `lip->skip_digest = true`（提示记号不单独进摘要，由提示扫描器自己归约）。

### 字符集状态维护

**当前字符集来自 THD。** `lex_one_token` 取 `const CHARSET_INFO *cs = thd->charset()`，状态表与标识符表都从它身上取。`Lex_input_stream::init()` 也把 `query_charset = thd->charset()` 存进成员。

**下划线字符集前缀。** 标识符分支里若标识符首字符是 `_`，则把后面的名字当字符集名去 `get_charset_by_csname(charset_name, MY_CS_PRIMARY, MYF(0))` 查：查到就走 `UNDERSCORE_CHARSET` 记号，把查到的字符集存进 `lip->m_underscore_cs`，并调 `lip->warn_on_deprecated_charset(underscore_cs, charset_name)` 压弃用警告。若查到的恰好是 `my_charset_utf8mb4_0900_ai_ci`，则换成 `thd->variables.default_collation_for_utf8mb4`——源码注释解释这是因为 utf8mb4 的默认排序规则由 `default_collation_for_utf8mb4` 系统变量决定。这里刻意用 `MYF(0)` 而不是 `MYF(MY_WME)`，源码注释说明是为了不因未知字符集名而产生错误。

`m_underscore_cs` 会被下一个字符串字面量消费：`MY_LEX_STRING` 分支里 `body_utf8_append_literal()` 的源字符集取 `lip->m_underscore_cs ? lip->m_underscore_cs : cs`，用完即 `lip->m_underscore_cs = nullptr` 复位。

**utf8 正文镜像。** `Lex_input_stream::body_utf8_start(thd, begin_ptr)` 分配一块 utf8 正文缓冲区，长度按 `(m_buf_length / thd->variables.character_set_client->mbminlen) * my_charset_utf8mb3_bin.mbmaxlen` 估算——即「客户端字符集的最小字节长度能装下多少字符」乘以「utf8 的最大字节长度」。`body_utf8_append(ptr, end_ptr)` 追加尚未处理的预处理缓冲区片段，`body_utf8_append_literal()` 则把文本字面量按需转码后追加：源字符集若与 `my_charset_utf8mb3_general_ci` 不同就调 `thd->convert_string()` 转码。

**字符集初始化在 `lex_init()`。** 它遍历 `all_charsets` 数组，对每个「有 ctype 且 `is_supported_parser_charset()` 为真」的字符集调 `init_state_maps(*cs)` 建状态表；任一失败（OOM）即返回 true。该函数由服务器启动路径调用，`lex_free()` 是与之配对的收尾函数。

### 摘要（digest）归约

`Lex_input_stream::add_digest_token(uint token, Lexer_yystype *yylval)` 在 `m_digest` 非空时调 `digest_add_token(m_digest, token, yylval)`；`reduce_digest_token(token_left, token_right)` 调 `digest_reduce_token`。摘要缓冲区本身由 `sql/sql_parse.cc` 的 `parse_sql()` 装配：若 `parser_state->m_input.m_has_digest` 且 `thd->m_digest` 非空，且调用方要求计算摘要（`m_input.m_compute_digest`）或性能模式已开了语句摘要，就把 `parser_state->m_lip.m_digest` 指过去，并把 `m_digest->m_digest_storage.m_charset_number` 设为 `thd->charset()->number`。

### 提示扫描器（独立的一层）

提示注释 `/*+ ... */` 不走主扫描器，而是交给 `sql/sql_lex_hints.cc` 的 `Hint_scanner`。它用**另一张**字符表：`cs->state_maps->hint_map`（构造函数里 `char_classes(cs->state_maps->hint_map)`），指针 `ptr` 初始化为 `input_buf + 3`（注释写明「skip `/*+`」）。`Hint_scanner::scan()` 按字符类分派：换行、空格、数字、标识符、多字节字符、三种引号、`*`、`@`、`.`、EOF。

`@` 的处理有上下文：`prev_token == '('` 或 `prev_token == HINT_ARG_IDENT && whitespaces == 0` 时走 `scan_query_block_name()`，否则按普通字节返回。`HINT_PARSER_parse()` 的返回值决定后续：返回 2 表示 bison 内部 OOM 直接失败，返回 1 表示语法错误或注释未闭合，此时 `lip->start_token()` 把错误消息的文本指针回退到 `/*+` 处。

提示记号不进主摘要缓冲区，而是由 `Hint_scanner::add_hint_token_digest()` 自己归约：普通标识符归一成 `IDENT`（`@` 前缀时归一成 `TOK_IDENT_AT`），数字归一成 `NUM`，查询块名拆成 `@` 加 `IDENT` 两个记号，首个提示加 `TOK_HINT_COMMENT_OPEN`，注释关闭加 `TOK_HINT_COMMENT_CLOSE`。

### 语法分析侧的接线

`sql/sql_yacc.yy` 里 `YYLIP` 宏定义为 `&YYTHD->m_parser_state->m_lip`——语法分析器直接操作同一个 `Lex_input_stream`。参数记号 `PARAM_MARKER` 的语义动作在 `param_marker:` 规则里：构造 `Item_param` 时用 `@1.raw.start - YYLIP->get_buf()` 算出参数在原始查询里的偏移。

## 关键取舍

**用字符集驱动的状态表，而不是硬编码的字符判断。** 扫描器对每个字节的分类都查 `cs->state_maps->main_map`，标识符字符判定查 `cs->ident_map`。代价是每个支持的字符集都要预先建表（`lex_init()` 里的 `init_state_maps()`），且多字节字符路径必须反复调 `my_ismbchar()`；收益是换字符集不需要改扫描器，且 `MY_LEX_IDENT` 分支能自然地按 `use_mb(cs)` 分成单字节快路与多字节慢路。

**字符集转换推迟到需要时做，而不是入口时统一转码。** 扫描器不预先把整条查询转成 utf8，而是维护一个 utf8 正文镜像，按需通过 `body_utf8_append()` / `body_utf8_append_literal()` 把片段追加进去；`body_utf8_append_literal()` 只在源字符集与 `my_charset_utf8mb3_general_ci` 不同时才调 `thd->convert_string()` 转码。代价是正文镜像的长度只能按最坏情况估算（`(m_buf_length / mbminlen) * utf8mb3_bin.mbmaxlen`），收益是不必为「其实全是 ASCII」的查询付出转码代价。

**用哈希表 + 字符树做关键字查找，而不是平衡树。** `Lex_hash::get_hash_symbol()` 以「词长」分桶、桶内是一棵按字符分支的树，比较时用 `to_upper_lex[]` 折叠大小写。代价是关键字表必须随版本重新生成，且 `to_upper_lex` 只覆盖 latin1 范围（源码注释明确说明「仅在比较关键字时使用」）；收益是查找是 O(词长) 的、无指针跳转。

**关键字与函数名分两张表。** `Lex_hash::sql_keywords` 与 `Lex_hash::sql_keywords_and_funcs` 是两张独立表，`find_keyword` 用第三个参数 `function`（即标识符后面是否紧跟 `(`）来选表。好处是 `COUNT` 这类既是关键字又是函数名的词可以按上下文给出不同记号；代价是词法分析器要额外判断一个字符。

**用一个前瞻记号换取 LALR(1) 文法。** `WITH` `ROLLUP` 需要 2 个前瞻会让文法退化成 LALR(2)，源码注释写明「替换为单个 `WITH_ROLLUP` 记号，把文法变成 LALR(1)」。代价是 `MYSQLlex` 里多了一段特判逻辑；收益是 `sql_yacc.yy` 的生成与冲突处理都简化了。

**把版本条件注释当作代码而非数据处理。** `/*!50000 ... */` 的内容在版本号不大于 `MYSQL_VERSION_ID` 时被直接展开进记号流。代价是同一段 SQL 在新旧服务器上语义不同，因此源码在丢弃分支里特意用 `yyUnput(' ')` 改写成空白，注释说明动机是「避免它被无限传播（例如传播给从库）」。

**`?` 占位符的识别条件是「后面不能跟标识符字符」。** `MY_LEX_CHAR` 分支的条件是 `c == '?' && lip->stmt_prepare_mode && !ident_map[lip->yyPeek()]`。源码注释给出的理由是 binlog 一致性：占位符被替换成实际值后，查询必须仍是语法正确的。代价是 `?` 后面紧跟标识符字符时不认作占位符。

**词法分析器兼职做摘要归约，而不是交给下游。** 摘要在取词时就地归约（`add_digest_token()`），提示注释则由提示扫描器单独归约后再合并（`add_hint_token_digest()`）。代价是词法分析器的状态里混进了摘要缓冲区的生命周期管理；收益是摘要不需要在记号流上再走一遍。

## 源码落点

| 路径 | 符号 | 作用 |
|---|---|---|
| `sql/sql_lex.cc` | `lex_start` | 准备解析一条新语句：重置 LEX、初始化代价模型、建顶层查询块 |
| `sql/sql_lex.cc` | `LEX::reset` | 清空上一条语句的残留字段，`unit`/`query_block` 置空 |
| `sql/sql_lex.cc` | `LEX::new_top_level_query` | 建顶层 `Query_block` 及其 `Query_expression` |
| `sql/sql_lex.cc` | `Lex_input_stream::init` / `reset` | 分配预处理缓冲区；换语句时复位指针与状态但不重分配 |
| `sql/sql_lex.cc` | `MYSQLlex` | 交给 bison 的取词函数：预读回放、`WITH`/`ROLLUP` 合并、摘要记号投递 |
| `sql/sql_lex.cc` | `lex_one_token` | 单记号扫描主循环，按 `enum my_lex_states` 分派 |
| `sql/sql_lex.cc` | `find_keyword` | 关键字查表，按 `sql_mode` 把 `NOT`/`||` 变形为 `NOT2_SYM`/`OR2_SYM` |
| `sql/sql_lex.cc` | `int_token` | 整数记号类型判定，阈值取自 `long_str`/`longlong_str` 等字面量 |
| `sql/sql_lex.cc` | `get_text` | 字符串字面量取值与反转义，转义表受 `MODE_NO_BACKSLASH_ESCAPES` 控制 |
| `sql/sql_lex.cc` | `get_token` / `get_quoted_token` | 标识符/字符串文本的拷贝，去掉重复引号 |
| `sql/sql_lex.cc` | `consume_comment` | 消费 `/* ... */`，只允许一层嵌套 |
| `sql/sql_lex.cc` | `lex_init` | 为每个受支持的字符集建状态表（`init_state_maps`） |
| `sql/sql_lex.cc` | `Lex_input_stream::body_utf8_start` / `body_utf8_append` / `body_utf8_append_literal` | 维护 utf8 正文镜像，按需转码字符串字面量 |
| `sql/sql_lex.cc` | `Lex_input_stream::add_digest_token` / `reduce_digest_token` | 摘要记号的投递与归约 |
| `sql/sql_lex.cc` | `consume_optimizer_hints` | 切出 `/*+ ... */` 并交给提示解析器 |
| `sql/sql_lex.cc` | `is_keyword` / `is_lex_native_function` | 对外暴露的关键字与内置函数名判定 |
| `sql/sql_lex_hash.cc` | `Lex_hash::get_hash_symbol` | 关键字哈希表查找，按词长分桶的字符树 |
| `sql/sql_lex_hash.cc` | `lex_casecmp` / `to_upper_lex` | latin1 范围的大小写折叠比较 |
| `sql/sql_lex_hints.cc` | `Hint_scanner::scan` | 提示注释的独立扫描，用 `state_maps->hint_map` |
| `sql/sql_lex_hints.cc` | `Hint_scanner::add_hint_token_digest` | 提示记号归一化后并入主摘要 |
| `sql/sql_yacc.yy` | `param_marker` | `PARAM_MARKER` 的语义动作，构造 `Item_param` 并记录参数偏移 |
| `sql/sql_yacc.yy` | `simple_statement_or_begin` | 消费 `multi_statements` 标志，决定是否在 `;` 处停住 |
| `sql/sql_parse.cc` | `dispatch_sql_command` | 词法分析的主要入口：调 `lex_start` 后调 `parse_sql` |
| `sql/sql_parse.cc` | `parse_sql` | 装配摘要缓冲区（`m_lip.m_digest`），设置 `m_digest_storage.m_charset_number` |
| `sql/sql_class.cc` | `THD::sql_parser` | 调 `MYSQLparse`，失败时调 `cleanup_after_parse_error` |
| `sql/sql_prepare.cc` | `Prepared_statement::prepare` | 预处理路径：置 `stmt_prepare_mode`/`multi_statements` 后调 `lex_start` |
| `sql/sql_derived.cc` | `parse_expression` | 派生表条件重解析：按 `CONTEXT_ANALYSIS_ONLY_PREPARE` 重设 `stmt_prepare_mode` 后调 `lex_start` |

## 与主核 / AI 的关系

主核（`engine/db`）的 SQL 前端是手写递归下降，词法与语法不分家，没有独立的字符集状态表这一层。

**架构推论：** MySQL 词法分析器有两点结构值得主核借鉴。其一是「字符分类由数据表驱动」：`state_map` 与 `ident_map` 把「哪些字节算空白、哪些算标识符字符」从代码里挪到了字符集对象上，扫描器因此可以按字符集换规则而不改逻辑。其二是「错误与降级有明确落点」：每一类扫描失败都落到一个确定的记号上——不匹配的引号、未闭合的注释、非法十六进制常量、奇数个十六进制数字，统一返回 `ABORT_SYM`，由语法分析器转成语法错误。主核若要把词法从手写递归下降里独立出来，这套「失败一律归到一个记号」的做法可以直接照搬。

对 AI 场景而言，词法分析器是「模型生成 SQL 到可执行语句」之间那道必须收紧的关口。MySQL 的做法值得借鉴的是它把**语义层面的收紧放在了词法阶段**：占位符 `?` 只在预处理模式下、且后面不跟标识符字符时才认；`--` 只有后面跟空白或控制字符才算注释；`/*!` 版本注释按版本号决定展开还是丢弃；`||` 在 `MODE_PIPES_AS_CONCAT` 未开时会被显式改写并压弃用警告。这些都是在字符级别就能判定的规则，代价只是几行状态机，收益是模型生成的 SQL 不会因为一个注释或一个符号而悄悄改变语义。

## 参考

- [MySQL 源码：`sql/sql_lex.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex.cc)
- [MySQL 源码：`sql/sql_lex_hash.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex_hash.cc)
- [MySQL 源码：`sql/sql_lex_hints.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_lex_hints.cc)
- [MySQL 源码：`sql/sql_yacc.yy`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_yacc.yy)
- [MySQL 源码：`sql/sql_parse.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_parse.cc)
- [MySQL 源码：`sql/sql_class.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_class.cc)
- [MySQL 源码：`sql/sql_prepare.cc`（GitHub 8.0 分支）](https://github.com/mysql/mysql-server/blob/8.0/sql/sql_prepare.cc)
- [MySQL 参考手册：SQL 词法结构](https://dev.mysql.com/doc/refman/8.0/en/lexical-structure.html)
- [MySQL 参考手册：字符串字面量与转义](https://dev.mysql.com/doc/refman/8.0/en/string-literals.html)
- [MySQL 参考手册：`sql_mode` 系统变量](https://dev.mysql.com/doc/refman/8.0/en/sql-mode.html)
- [MySQL 参考手册：运算符](https://dev.mysql.com/doc/refman/8.0/en/operators.html)
- [MySQL Server 开发文档：Parser](https://dev.mysql.com/doc/dev/mysql-server/latest/page_parser.html)
