#include "query/lexer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lex_init(Lexer *lx, const char *src)
{
    lx->src = src;
    lx->pos = 0;
}

static bool is_space(char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

static bool is_digit(char c) { return c >= '0' && c <= '9'; }

static bool is_ident_start(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
}

static bool is_ident_char(char c)
{
    return is_ident_start(c) || is_digit(c);
}

int lex_next(Lexer *lx, Token *tok)
{
    memset(tok, 0, sizeof(*tok));

    const char *s = lx->src;

    /* 跳空白 */
    while (is_space(s[lx->pos]))
        lx->pos++;

    tok->pos = lx->pos;

    if (s[lx->pos] == '\0') {
        tok->type = TK_EOF;
        return OK;
    }

    char c = s[lx->pos];

    /* 标识符 / 关键字 */
    if (is_ident_start(c)) {
        int n = 0;
        while (is_ident_char(s[lx->pos]) && n < TK_TEXT_MAX - 1) {
            tok->text[n++] = s[lx->pos++];
        }
        tok->text[n] = '\0';
        tok->len = (u16)n;
        tok->type = TK_IDENT;
        return OK;
    }

    /* 数字 */
    if (is_digit(c)) {
        u64 v = 0;
        bool ovf = false;
        int n = 0;
        while (is_digit(s[lx->pos]) && n < TK_TEXT_MAX - 1) {
            tok->text[n++] = s[lx->pos];
            u64 d = (u64)(s[lx->pos] - '0');
            /* 超过 64 位无符号范围 → 标记溢出（继续扫完，报错） */
            if (v > (UINT64_MAX - d) / 10u)
                ovf = true;
            else
                v = v * 10u + d;
            lx->pos++;
        }

        /*
         * 小数部分：1.5 / 0.25
         *
         * 【为什么要支持小数】
         * v1.1 加了 REAL 类型（AVG 的结果、地理坐标、时序数值、JSON 数字），
         * 但没有小数常量的话，REAL 列**只能存整数** ——
         * 那 REAL 就白加了。
         *
         * （'.' 后面必须跟数字：否则 "1." 会被当成整数 1 加一个点，
         *   而 "t1.c" 这种写法在 SQL 里也不该被当成小数。）
         */
        bool is_real = false;
        if (s[lx->pos] == '.' && is_digit(s[lx->pos + 1]) &&
            n < TK_TEXT_MAX - 1) {
            is_real = true;
            tok->text[n++] = s[lx->pos++];
            while (is_digit(s[lx->pos]) && n < TK_TEXT_MAX - 1)
                tok->text[n++] = s[lx->pos++];
        }

        /* 指数部分：1e10 / 2.5E-3 */
        if ((s[lx->pos] == 'e' || s[lx->pos] == 'E') &&
            n < TK_TEXT_MAX - 1 &&
            (is_digit(s[lx->pos + 1]) ||
             ((s[lx->pos + 1] == '+' || s[lx->pos + 1] == '-') &&
              is_digit(s[lx->pos + 2])))) {
            is_real = true;
            tok->text[n++] = s[lx->pos++];
            if (s[lx->pos] == '+' || s[lx->pos] == '-')
                tok->text[n++] = s[lx->pos++];
            while (is_digit(s[lx->pos]) && n < TK_TEXT_MAX - 1)
                tok->text[n++] = s[lx->pos++];
        }

        tok->text[n] = '\0';
        tok->len = (u16)n;
        tok->type = TK_NUM;

        if (is_real) {
            tok->is_real = true;
            tok->fnum = strtod(tok->text, NULL);
            return OK;
        }

        /*
         * 【为什么这里必须报错，而不是截断或转浮点】
         *
         * 原来这里是 `i64 v = 0; v = v * 10 + d;` —— 有符号溢出是 UB，
         * 9223372036854775807 会被算成 -1。
         * 那意味着 `INSERT INTO t VALUES (9223372036854775807)`
         * 静默写进去一个错值 —— 数据损坏，而且没有任何报错。
         *
         * 转浮点也不行：那是静默丢精度（同 SUM 整数溢出的道理）。
         * 超范围就**明确报错** ✓
         *
         * （幅值本身可以到 UINT64_MAX：范围由解析器判，
         *   因为 -9223372036854775808 的幅值 2^63 超出 i64 上限，
         *   但它是合法的 i64 —— 只有解析器知道前面有没有负号。）
         */
        if (ovf) {
            snprintf(tok->text, sizeof(tok->text),
                     "整数常量太大（超过 64 位无符号范围）");
            tok->len = (u16)strlen(tok->text);
            return E_INVAL;
        }

        tok->unum = v;
        return OK;
    }

    /* 字符串：'...'，'' 表示一个单引号 */
    if (c == '\'') {
        int n = 0;
        lx->pos++;
        for (;;) {
            char ch = s[lx->pos];
            if (ch == '\0') {
                tok->type = TK_EOF;
                snprintf(tok->text, sizeof(tok->text), "字符串没有闭合");
                tok->len = (u16)strlen(tok->text);
                return E_INVAL;          /* 字符串没闭合 */
            }
            if (ch == '\'') {
                if (s[lx->pos + 1] == '\'') {   /* 转义 */
                    if (n < TK_TEXT_MAX - 1)
                        tok->text[n++] = '\'';
                    lx->pos += 2;
                    continue;
                }
                lx->pos++;
                break;
            }
            if (n < TK_TEXT_MAX - 1)
                tok->text[n++] = ch;
            lx->pos++;
        }
        tok->text[n] = '\0';
        tok->len = (u16)n;
        tok->type = TK_STR;
        return OK;
    }

    /* 双字符运算符 */
    if (c == '<' && s[lx->pos + 1] == '>') {
        strcpy(tok->text, "<>");
        tok->len = 2;
        lx->pos += 2;
        tok->type = TK_PUNCT;
        return OK;
    }
    if (c == '<' && s[lx->pos + 1] == '=') {
        strcpy(tok->text, "<=");
        tok->len = 2;
        lx->pos += 2;
        tok->type = TK_PUNCT;
        return OK;
    }
    if (c == '>' && s[lx->pos + 1] == '=') {
        strcpy(tok->text, ">=");
        tok->len = 2;
        lx->pos += 2;
        tok->type = TK_PUNCT;
        return OK;
    }

    /* 单字符 */
    tok->text[0] = c;
    tok->text[1] = '\0';
    tok->len = 1;
    tok->type = TK_PUNCT;
    lx->pos++;
    return OK;
}

bool tok_is_kw(const Token *tok, const char *kw)
{
    if (tok->type != TK_IDENT)
        return false;
    size_t n = strlen(kw);
    if (tok->len != n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char a = tok->text[i];
        char b = kw[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = (char)(b - 'A' + 'a');
        if (a != b)
            return false;
    }
    return true;
}

bool tok_is_punct(const Token *tok, const char *p)
{
    if (tok->type != TK_PUNCT)
        return false;
    return strcmp(tok->text, p) == 0;
}
