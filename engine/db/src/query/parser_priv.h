#ifndef DB_QUERY_PARSER_PRIV_H
#define DB_QUERY_PARSER_PRIV_H

#include "query/parser.h"

#include <stdbool.h>
#include <stdio.h>

/*
 * 解析器内部共享的东西（parser.c / parser_stmt.c 之间）。
 *
 * 【为什么拆文件】
 * 硬规则：单文件不超过 800 行。
 * 加了聚合函数（parse_agg_call）、REAL 类型、浮点/大整数字面量之后，
 * parser.c 涨到了 816 行 —— 超了。
 *
 * 拆成三个：
 *   parser.c      词法辅助 + 表达式解析 + parse_sql
 *   parser_stmt.c 语句解析（CREATE / INSERT / SELECT / UPDATE / DELETE …）
 *   parser_priv.h 两者共享的 Parser 结构与辅助函数
 *
 * 拆分的界线是"**解析什么**"：
 *   表达式是 SQL 的"值"，语句是 SQL 的"动作"。
 *
 * 【为什么辅助函数要带 p_ 前缀】
 * 它们不再是 static（两个文件都要用），所以名字得够独特 ——
 * "fail" / "advance" 这种通用名进了全局符号表容易撞车。
 */

/* 用一个不与 E_* 冲突的值表示"解析失败"，调用方只看 rc != OK */
#define E_PARSE_DUMMY 99

typedef struct {
    Lexer lx;
    Token cur;
    char *err;
    int   errcap;
    bool  failed;
} Parser;

/* 词法辅助 */
void p_fail(Parser *p, const char *msg);
void p_advance(Parser *p);
bool p_accept_kw(Parser *p, const char *kw);
bool p_accept_punct(Parser *p, const char *s);
bool p_expect_punct(Parser *p, const char *s);
bool p_expect_ident(Parser *p, char *out);

/* 表达式 */
Expr *p_expr_new(ExprKind k);
Expr *p_parse_expr(Parser *p);

/* 语句（parser_stmt.c） */
int  p_parse_type(Parser *p, u8 *out);
int  p_parse_create(Parser *p, Stmt *st);
int  p_parse_create_index(Parser *p, Stmt *st);
int  p_parse_vacuum(Parser *p, Stmt *st);
int  p_parse_set_isolation(Parser *p, Stmt *st);
int  p_parse_insert(Parser *p, Stmt *st);
int  p_parse_select(Parser *p, Stmt *st);
int  p_parse_update(Parser *p, Stmt *st);
int  p_parse_delete(Parser *p, Stmt *st);

#endif /* DB_QUERY_PARSER_PRIV_H */
