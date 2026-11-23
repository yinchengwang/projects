---
project: postgres
feature: parser-pipeline
page_type: feature
schema_version: 1
refs:
  - path: src/backend/parser/parser.c
    symbols: [raw_parser]
  - path: src/backend/parser/scansup.c
    symbols: [downcase_truncate_identifier, scanner_isspace]
---

# SQL 词法与语法解析

## 一句话职责

解析流水线把 SQL 源文本变成只含语法结构、尚未解析语义的 raw parse tree。

## 机制是什么

`raw_parser` 组合 flex 词法分析器与 bison 语法分析器：`scanner_init` 初始化扫描器并把关键字表接入，`parser_init` 初始化语法分析状态，`base_yyparse` 产出 `yyextra.parsetree`。返回值是按 `RawParseMode` 组织的一个或多个 `RawStmt`，其内部只保留语法糖层面的节点，例如 `RangeVar`、`A_Expr`、`SelectStmt` 等。

这一阶段刻意不做表访问，源码注释明确说明这样即使处在已中止事务中也能完成基本解析，代价是产出的树必须交给语义分析补全语义。因此本机制不负责类型推断、函数解析、权限检查或规则改写；它也不负责执行，连表名是否存在都不确认。标识符的大小写折叠与截断属于词法支持，目标列、子句和表达式的语义留给后续阶段。

## 怎么实现的

1. 调用方（如 `postgres.c` 处理查询串、`parse_type.c` 解析类型名、`spi.c` 处理预备语句）传入源文本和 `RawParseMode`；模式决定是否预置 lookahead token。
2. `raw_parser` 用 `scanner_init` 建立 `core_yyscan_t`，把 `ScanKeywords` 和 `ScanKeywordTokens` 交给扫描器，使关键词能被识别成独立 token。
3. 当 `mode != RAW_PARSE_DEFAULT` 时，函数从 `mode_token[]` 取对应的 `MODE_*` token 存入 `yyextra.lookahead_token` 并置 `have_lookahead = true`。这让同一套文法可以解析表达式片段、类型名或 PL/pgSQL 赋值目标。
4. `parser_init` 初始化 bison 状态，`base_yyparse` 消费 token 并调用文法动作构造 raw 节点；词法层的 `base_yylex` 过滤器通过一次 token 前瞻把 `NOT`、`WITH`、`NULLS_P`、`FORMAT`、`WITHOUT` 等替换为 `*_LA` 形式，避免文法需要多 token 前瞻而破坏 LALR(1)。
5. 由 `scansup.c` 提供词法支持：`downcase_truncate_identifier` 对未加引号标识符做折叠与截断，内部 `downcase_identifier` 对 7 位 ASCII 直接转小写，对单字节编码的高位字符才用 `tolower()`，随后在超过 `NAMEDATALEN` 时调用 `truncate_identifier`，并按 `pg_mbcliplen` 保证不切断多字节字符。
6. `scanner_isspace` 用硬编码的空格、制表、换行、回车、垂直制表、换页字符集合判断空白，源码注明必须与 `scan.l` 的 `{space}` 集合一致，避免使用可能受 locale 影响的 `isspace()`。
7. `scanner_finish` 释放扫描器；`yyresult` 非零表示语法错误，此时返回 `NIL`，否则返回 `yyextra.parsetree`。整条路径都在当前内存上下文分配，无共享状态或锁。

## 关键取舍

- **源码事实：**解析器被约束为不访问表，因此 raw tree 保留 `RangeVar`、`A_Expr` 等未解析形式。**架构推论：**这换来在事务已中止等异常状态下仍可解析的能力，代价是同一份语义工作必须集中在 analyze 阶段；若把名字解析放进文法动作，文法就要访问系统目录，错误恢复与事务状态处理会复杂得多。
- **源码事实：**`base_yylex` 用前瞻过滤器而不是在 `scan.l` 里识别多词 token，注释指出这样避免扫描器回溯。**架构推论：**用一次前瞻换取 LALR(1) 文法与扫描器性能；若在文法里直接容纳多词 token，需要允许词间注释并引入回溯，常规解析成本上升。
- **源码事实：**未加引号标识符的折叠只对单字节编码使用 locale `tolower()`，多字节字符原样保留；截断用 `pg_mbcliplen`。**架构推论：**放弃了 SQL99 的完整 Unicode 大小写规范化，以避免依赖尚未具备的基础设施；若强行按 locale 折叠多字节字符，可能产生长度变化并破坏后续字符边界。

## 源码落点

| 路径 | 符号 | 本文对应作用 |
|---|---|---|
| `src/backend/parser/parser.c` | `raw_parser` | 初始化扫描器与文法、选择解析模式并返回 raw parse tree |
| `src/backend/parser/scansup.c` | `downcase_truncate_identifier` | 未加引号标识符的大小写折叠与按字节安全截断入口 |
| `src/backend/parser/scansup.c` | `scanner_isspace` | 判定与 `scan.l` 一致的空白字符集合 |

## 参考

- [PostgreSQL 文档：词法结构](https://www.postgresql.org/docs/current/sql-syntax-lexical.html) —— 标识符大小写折叠与长度限制的用户可见规则
- [PostgreSQL 文档：解析器阶段](https://www.postgresql.org/docs/current/parser-stage.html) —— 词法/语法解析与后续语义分析的阶段划分
- [PostgreSQL 源码：parser.c](https://github.com/postgres/postgres/blob/master/src/backend/parser/parser.c) —— raw parse tree 解析入口

## 与主核 / AI 的关系

主核同样需要把 SQL 文本转成语法树，本页的分阶段边界（语法结构 vs 语义解析）可作为设计参照，具体节点与文法不可照搬。暂无直接 AI 对应。
