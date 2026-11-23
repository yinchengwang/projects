/*
 * test_lexer_string.c —— SQL 字符串字面量：精确长度，不许静默截断
 *
 * 【这个测试在钉什么】
 *
 * 词法器原来用 Token.text[256]，写满就停止拷贝但继续扫完输入 ——
 * 等于"读完了却只存了一半"，用户拿到的是被改过的值，而且**没有任何报错**。
 *
 * 分三组：
 *   1. 边界长度必须精确（254 / 255 / 256 / 257 / 1000 / 8192）
 *   2. 超上限（8193）必须报错，且错误文本里带上限数字
 *   3. 解析器必须接管长字面量的所有权（外部 T_TEXT + 精确长度）
 *
 * 【为什么第 3 组重要】
 * 词法器分配的 str_value 必须有人回收，否则每次解析长字面量都泄漏一块堆。
 * 契约是：词法器分配 → 解析器接管 → free_expr 释放。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/lexer.h"
#include "query/parser.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 造一个 SQL：SELECT '<text>'，扫描到字面量 token */
static int scan_literal(const char *text, size_t len, Token *out)
{
    /* "SELECT '" 是 8 个字节（SELECT 6 + 空格 1 + 引号 1） */
    char *sql = malloc(len + 16);
    if (!sql)
        return E_IO;
    memcpy(sql, "SELECT '", 8);
    memcpy(sql + 8, text, len);
    memcpy(sql + 8 + len, "'", 2);
    sql[8 + len + 1] = '\0';   /* 必须 NUL 结尾：词法器一直扫到 '\0' */

    Lexer lx;
    Token t;
    lex_init(&lx, sql);
    lex_next(&lx, &t);            /* SELECT */
    int rc = lex_next(&lx, &t);   /* 字面量 */
    /*
     * 失败时也要把 token 带出去 —— 词法器把错误原因写进 tok->text，
     * 调用方需要看到它（"上限多少"这种信息必须在错误文本里）。
     */
    *out = t;
    if (rc != OK && t.str_value) {
        free(t.str_value);
        out->str_value = NULL;
    }
    free(sql);
    return rc;
}

int main(void)
{
    printf("test_lexer_string:\n");

    /* ---------------- 1. 边界：长度必须精确 ---------------- */
    {
        size_t sizes[] = { 1, 254, 255, 256, 257, 1000, TK_STR_MAX };
        for (unsigned i = 0; i < sizeof(sizes) / sizeof(sizes[0]); i++) {
            size_t n = sizes[i];
            char *buf = malloc(n + 1);
            if (!buf)
                return 1;
            memset(buf, 'x', n);
            buf[n] = '\0';

            Token t;
            memset(&t, 0, sizeof(t));
            int rc = scan_literal(buf, n, &t);
            CHECK(rc == OK);
            CHECK(t.len == n);
            CHECK((t.len == n) &&
                  (memcmp(t.str_value ? t.str_value : t.text, buf, n) == 0));
            if (t.str_value)
                free(t.str_value);
            free(buf);
        }
    }

    /* ---------------- 2. 超上限：明确报错，不截断 ---------------- */
    {
        size_t n = (size_t)TK_STR_MAX + 1;
        char *buf = malloc(n + 1);
        if (!buf)
            return 1;
        memset(buf, 'x', n);
        buf[n] = '\0';

        Token t;
        memset(&t, 0, sizeof(t));
        int rc = scan_literal(buf, n, &t);
        CHECK(rc != OK);
        /* 错误文本要含上限数字 —— 用户得知道能写多少 */
        CHECK(strstr(t.text, "8192") != NULL);
        if (t.str_value)
            free(t.str_value);
        free(buf);
    }

    /* ---------------- 3. '' 转义按一个字节解码 ---------------- */
    {
        Token t;
        memset(&t, 0, sizeof(t));
        /* 'a''b' ⇒ 内容 a'b（3 字节） */
        int rc = scan_literal("a''b", 4, &t);
        CHECK(rc == OK);
        CHECK(t.len == 3);
        CHECK(memcmp(t.str_value ? t.str_value : t.text, "a'b", 3) == 0);
        if (t.str_value)
            free(t.str_value);
    }

    /* ---------------- 4. 解析器接管长字面量 ---------------- */
    {
        char *sql = malloc(TK_STR_MAX + 32);
        if (!sql)
            return 1;
        size_t n = 300;
        memcpy(sql, "SELECT '", 8);
        memset(sql + 8, 'y', n);
        memcpy(sql + 8 + n, "' FROM t", 8);
        sql[8 + n + 8] = '\0';   /* 必须 NUL 结尾：词法器一直扫到 '\0' */

        char err[128];
        Stmt st;
        memset(&st, 0, sizeof(st));
        int rc = parse_sql(sql, &st, err, (int)sizeof(err));
        CHECK(rc == OK);
        CHECK(st.ntargets == 1);
        CHECK(st.targets[0] != NULL);
        if (st.targets[0]) {
            /* 长字面量必须由 Expr 持有（text_owned 非 NULL） */
            CHECK(st.targets[0]->text_owned != NULL);
            CHECK(st.targets[0]->text_len == n);
            /* lit 是外部 T_TEXT：长度靠 detoast 问，不是内联 */
            CHECK(st.targets[0]->lit.type == T_TEXT);
            CHECK(st.targets[0]->lit.external);

            u8 *bytes = NULL;
            u32 blen = 0;
            CHECK(val_detoast(&st.targets[0]->lit, &bytes, &blen) == OK);
            CHECK(blen == n);
            free(bytes);
        }

        free_stmt(&st);          /* 释放后不得再访问 text_owned */
        free(sql);
    }

    /* ---------------- 5. 短字面量不进 text_owned（不付堆代价） ---------------- */
    {
        char err[128];
        Stmt st;
        memset(&st, 0, sizeof(st));
        int rc = parse_sql("SELECT 'abc' FROM t", &st, err, (int)sizeof(err));
        CHECK(rc == OK);
        CHECK(st.ntargets == 1);
        if (st.targets[0]) {
            CHECK(st.targets[0]->text_owned == NULL);
            CHECK(st.targets[0]->lit.type == T_TEXT);
            CHECK(st.targets[0]->lit.len == 3);
            CHECK(st.targets[0]->lit.external == 0);
        }
        free_stmt(&st);
    }

    if (failures == 0) {
        printf("  词法器字符串字面量验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
