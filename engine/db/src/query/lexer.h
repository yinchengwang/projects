#ifndef DB_QUERY_LEXER_H
#define DB_QUERY_LEXER_H

#include "common/error.h"
#include "common/types.h"

#define TK_TEXT_MAX 256

typedef enum {
    TK_EOF = 0,
    TK_IDENT,      /* 标识符或关键字（关键字由 parser 判断） */
    TK_NUM,        /* 整数字面量 */
    TK_STR,        /* 单引号字符串，内容在 text 里 */
    TK_PUNCT       /* 运算符与标点 */
} TokType;

typedef struct {
    TokType type;
    char    text[TK_TEXT_MAX];
    u16     len;
    /*
     * 整数字面量的**幅值**（无符号）。
     *
     * 【为什么用 u64 而不是 i64】
     * 范围检查要放在解析器里做（因为负号是语法的一部分：
     * -9223372036854775808 = -(2^63)，幅值本身超出 i64 上限）。
     * 词法器只负责"这串数字有多大"，能不能表示由解析器判断 ✓
     */
    u64     unum;
    /*
     * 实数字面量（v1.1）。
     *
     * is_real = true 时：fnum 是值，unum 无意义。
     * is_real = false 时：unum 是整数的幅值，fnum 无意义。
     */
    double  fnum;
    bool    is_real;
    int     pos;
} Token;

typedef struct {
    const char *src;
    int pos;
} Lexer;

void lex_init(Lexer *lx, const char *src);
int  lex_next(Lexer *lx, Token *tok);

/* 大小写不敏感比较：tok 是否是关键字 kw */
bool tok_is_kw(const Token *tok, const char *kw);
/* tok 是否是标点/运算符 p */
bool tok_is_punct(const Token *tok, const char *p);

#endif /* DB_QUERY_LEXER_H */
