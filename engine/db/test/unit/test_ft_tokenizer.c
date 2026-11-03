/*
 * test_ft_tokenizer.c —— 分词器的规则必须逐条钉死（v1.3 步骤 1）
 *
 * 分词规则是**索引格式的一部分**：写入端和查询端必须切成同一个样子。
 * 切得不一样 → 写进去的 term 和查出来的 term 对不上 →
 * 检索结果永远是空的，**而且不报错**。这种 bug 极难查，
 * 所以这里把每一条规则都变成一个断言。
 *
 * 6 项：
 *   1. 基本切分 + 小写化（数字算词的一部分）
 *   2. 连字符是分隔符（"state-of-the-art" → 4 个词）
 *   3. 连续分隔符不产生空词；首尾分隔符无害
 *   4. 超长词截断（保留 63 字节，剩余整段丢弃，不在截断处再切）
 *   5. 输出缓冲不足 → E_NOSPACE，并报告需要的词数（且不写半截结果）
 *   6. 参数非法 → E_INVAL；非 ASCII 字节当分隔符；text 恒以 '\0' 结尾
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ft/tokenizer.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 一个词是不是正好等于 s */
static int tok_is(const FtToken *t, const char *s)
{
    size_t n = strlen(s);

    return t->len == (u16)n && memcmp(t->text, s, n) == 0;
}

/* ---------------- 1) 基本切分 + 小写化 ---------------- */

static void t1_basic(void)
{
    printf("  1) 基本切分 + 小写化\n");

    const char *s = "Hello, World! 123 abc123 3.14";
    FtToken out[16];
    u32 n = 0;
    int rc = ft_tokenize(s, (u32)strlen(s), out, 16, &n);

    CHECK(rc == OK);
    CHECK(n == 6);

    if (n == 6) {
        CHECK(tok_is(&out[0], "hello"));
        CHECK(tok_is(&out[1], "world"));
        CHECK(tok_is(&out[2], "123"));
        /* 数字是词的一部分，不单独切 */
        CHECK(tok_is(&out[3], "abc123"));
        /* '.' 是分隔符 → "3" 和 "14" */
        CHECK(tok_is(&out[4], "3"));
        CHECK(tok_is(&out[5], "14"));
    }

    /* 空文本 → 0 个词 */
    rc = ft_tokenize("", 0, out, 16, &n);
    CHECK(rc == OK);
    CHECK(n == 0);

    /* NULL + 长度 0 也算空文本 */
    rc = ft_tokenize(NULL, 0, out, 16, &n);
    CHECK(rc == OK);
    CHECK(n == 0);
}

/* ---------------- 2) 连字符是分隔符 ---------------- */

static void t2_hyphen(void)
{
    printf("  2) 连字符是分隔符\n");

    const char *s = "state-of-the-art";
    FtToken out[8];
    u32 n = 0;
    int rc = ft_tokenize(s, (u32)strlen(s), out, 8, &n);

    CHECK(rc == OK);
    CHECK(n == 4);

    if (n == 4) {
        CHECK(tok_is(&out[0], "state"));
        CHECK(tok_is(&out[1], "of"));
        CHECK(tok_is(&out[2], "the"));
        CHECK(tok_is(&out[3], "art"));
    }

    /* 下划线也是分隔符 */
    rc = ft_tokenize("a_b", 3, out, 8, &n);
    CHECK(rc == OK);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(tok_is(&out[0], "a"));
        CHECK(tok_is(&out[1], "b"));
    }
}

/* ---------------- 3) 连续分隔符不产生空词 ---------------- */

static void t3_empty_words(void)
{
    printf("  3) 连续分隔符不产生空词\n");

    const char *s = "  ,,,a,,b,,  ";
    FtToken out[8];
    u32 n = 0;
    int rc = ft_tokenize(s, (u32)strlen(s), out, 8, &n);

    CHECK(rc == OK);
    CHECK(n == 2);

    if (n == 2) {
        CHECK(tok_is(&out[0], "a"));
        CHECK(tok_is(&out[1], "b"));
    }

    /* 全是分隔符 → 0 个词 */
    rc = ft_tokenize("...  ,,", 7, out, 8, &n);
    CHECK(rc == OK);
    CHECK(n == 0);
}

/* ---------------- 4) 超长词截断 ---------------- */

static void t4_long_token(void)
{
    printf("  4) 超长词截断\n");

    char buf[128];
    FtToken out[4];
    u32 n = 0;
    int rc;
    int i;

    /* 63 字节：正好装下，不截断 */
    for (i = 0; i < 63; i++) {
        buf[i] = 'a';
    }
    buf[63] = '\0';
    rc = ft_tokenize(buf, 63, out, 4, &n);
    CHECK(rc == OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].len == 63);
        CHECK(out[0].text[63] == '\0');
        CHECK(out[0].text[0] == 'a');
    }

    /* 64 字节：超了 → 截成 63（text[] 要给 '\0' 留位置） */
    buf[63] = 'b';
    rc = ft_tokenize(buf, 64, out, 4, &n);
    CHECK(rc == OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].len == 63);
        CHECK(out[0].text[63] == '\0');
        CHECK(out[0].text[62] == 'a');
    }

    /*
     * 100 字节的一长串 → **只出 1 个词**。
     *
     * 关键：不能在截断处再切一刀。切了就是凭空造出一个词边界，
     * 后半截根本不是词，拿它当 term 匹配等于"允许命中任意后缀"。
     */
    for (i = 0; i < 100; i++) {
        buf[i] = 'x';
    }
    buf[100] = '\0';
    rc = ft_tokenize(buf, 100, out, 4, &n);
    CHECK(rc == OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(out[0].len == 63);
        CHECK(out[0].text[63] == '\0');
    }

    /* 超长词后面还有正常词 → 正常词照样要切出来 */
    {
        char s2[120];
        for (i = 0; i < 100; i++) {
            s2[i] = 'x';
        }
        s2[100] = ' ';
        s2[101] = 'z';
        s2[102] = '\0';
        rc = ft_tokenize(s2, 102, out, 4, &n);
        CHECK(rc == OK);
        CHECK(n == 2);
        if (n == 2) {
            CHECK(out[0].len == 63);
            CHECK(tok_is(&out[1], "z"));
        }
    }
}

/* ---------------- 5) 输出缓冲不足 ---------------- */

static void t5_nospace(void)
{
    printf("  5) 输出缓冲不足 → E_NOSPACE\n");

    const char *s = "a b c d";
    FtToken out[2];
    u32 n = 0;
    int rc;

    /* 先放个哨兵：失败时不该被改写 */
    out[0].len = 0xFFFFu;
    out[0].text[0] = 'Z';

    rc = ft_tokenize(s, (u32)strlen(s), out, 2, &n);
    CHECK(rc == E_NOSPACE);
    /* 报告需要的词数 */
    CHECK(n == 4);
    /* 一个词都不写（不留半截结果） */
    CHECK(out[0].len == 0xFFFFu);
    CHECK(out[0].text[0] == 'Z');

    /* 刚好装下就成功 */
    {
        FtToken out2[4];
        rc = ft_tokenize(s, (u32)strlen(s), out2, 4, &n);
        CHECK(rc == OK);
        CHECK(n == 4);
    }

    /* "只数不切"的用法：out = NULL、max_out = 0 */
    rc = ft_tokenize(s, (u32)strlen(s), NULL, 0, &n);
    CHECK(rc == E_NOSPACE);
    CHECK(n == 4);
}

/* ---------------- 6) 非法参数 / 非 ASCII / '\0' 结尾 ---------------- */

static void t6_edges(void)
{
    printf("  6) 非法参数、非 ASCII、'\\0' 结尾\n");

    const char *s = "hello";
    FtToken out[4];
    u32 n = 0;

    /* n_out 为 NULL */
    CHECK(ft_tokenize(s, 5, out, 4, NULL) == E_INVAL);
    /* text 为 NULL 但长度非 0 */
    CHECK(ft_tokenize(NULL, 5, out, 4, &n) == E_INVAL);
    /* out 为 NULL 但 max_out 非 0 */
    CHECK(ft_tokenize(s, 5, NULL, 4, &n) == E_INVAL);

    /*
     * 非 ASCII 字节（UTF-8 的中文）当分隔符。
     *
     * 本版不做中文分词：把一串 UTF-8 当成一个词，
     * 得到的既不是词也不是字，只是乱码 term。
     */
    {
        /* "中文abc中文" = 6 个 UTF-8 字节 + "abc" + 6 个 UTF-8 字节 */
        const char *mix = "\xE4\xB8\xAD\xE6\x96\x87" "abc" "\xE4\xB8\xAD\xE6\x96\x87";
        int rc = ft_tokenize(mix, (u32)strlen(mix), out, 4, &n);

        CHECK(rc == OK);
        CHECK(n == 1);
        if (n == 1) {
            CHECK(tok_is(&out[0], "abc"));
        }
    }

    /* 每个词的 text 都以 '\0' 结尾（下游可以安全地 printf("%s")） */
    {
        const char *s2 = "one two three";
        u32 i;
        int rc = ft_tokenize(s2, (u32)strlen(s2), out, 4, &n);

        CHECK(rc == OK);
        CHECK(n == 3);
        for (i = 0; i < n && i < 4; i++) {
            CHECK(out[i].text[out[i].len] == '\0');
        }
    }
}

int main(void)
{
    printf("test_ft_tokenizer:\n");

    t1_basic();
    t2_hyphen();
    t3_empty_words();
    t4_long_token();
    t5_nospace();
    t6_edges();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过\n");
    return 0;
}
