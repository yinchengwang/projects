/*
 * test_jsonb.c —— JSONB 模态的编码/解码单测（v1.6 步骤 1 · T13 · A1）
 *
 * 契约见 src/modalities/doc/jsonb.h 与 json_parse.h。
 * 共用工具见 jsonb_test_common.h；
 * 取值 / 损坏 / 深度 / fuzz 那几组在 test_jsonb_crash.c。
 *
 * ==================== 这一步要证明什么 ====================
 *
 * 本轮做的是"文本 → 树 → JSONB → 树"这条链的**自洽**。
 * （与 SQLite JSON1 的对照在步骤 2 的路径提取那侧做。）
 *
 * 本文件里的 8 组：
 *   1. **往返全等**：文本 → 树 → JSONB → 树 ⇒ 结构逐字段相等
 *      （⚠️ 用**深度优先比较**，不用 memcmp —— 键在 JSONB 里是排过序的，
 *        与输入顺序可能不同）
 *   2. **数字精度**：int32 / int64 / float64 的归类；1 与 1.0 必须能区分
 *   3. **字符串转义**：全部转义、代理对、裸 UTF-8、**带 NUL 的字符串**
 *   4. **重复键保留首个**
 *   5. **嵌套**：{"a":{"b":[1,2,{"c":null}]}}
 *   6. **边界**：空对象 / 空数组 / 空字符串
 *   7. **非法输入必须硬失败**（E_INVAL，**不是**返回半截）
 *   8. **编码稳定**：同一棵树编码两次 ⇒ 字节完全相同
 *
 * ==================== 为什么第 1 项要"逐字段相等" ====================
 *
 * JSON 最容易出的错是**静默丢字段**或者**类型悄悄变了**：
 * 键被覆盖、整数变成了浮点、转义解错了 ——
 * 结果就是查出来的值不对，**而且不报错**。
 * "能解析成功"这种验收抓不住这类 bug。
 */

#include "jsonb_test_common.h"

static int failures = 0;

/* ---------------- 1) 往返全等 ---------------- */

static void t1_roundtrip(void)
{
    static const char *docs[] = {
        "{}",
        "[]",
        "null",
        "true",
        "false",
        "0",
        "-1",
        "1.5",
        "\"hello\"",
        "{\"a\":1}",
        "{\"a\":1,\"b\":2}",
        "[1,2,3]",
        "{\"a\":[1,2,3],\"b\":{\"c\":\"x\"}}",
        "{\"a\":{\"b\":[1,2,{\"c\":null}]}}",
        "{\"z\":1,\"a\":2,\"m\":3}",
        "[{\"k\":\"v\"},[1,[2,[3]]]]",
        "{\"n\":null,\"t\":true,\"f\":false}",
        "{\"s\":\"\\u4e2d\\u6587\"}",
        "{\"big\":9223372036854775807}",
        "{\"neg\":-9223372036854775808}"
    };
    size_t i;

    printf("  1) 往返全等：文本 → 树 → JSONB → 树\n");

    for (i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        if (!jsonb_test_roundtrip(docs[i], NULL)) {
            printf("    FAIL 往返不相等：%s\n", docs[i]);
            failures++;
        }
    }
    printf("    %zu 个文档往返全等\n", sizeof(docs) / sizeof(docs[0]));
}

/* ---------------- 2) 数字精度 ---------------- */

typedef struct {
    const char *text;
    JsonbTag   tag;
    i64        ival;      /* 整数用例的期望值 */
    double     dval;      /* 浮点用例的期望值 */
} NumCase;

static void t2_numbers(void)
{
    static const NumCase cases[] = {
        { "0",                   JSONB_INT32,   0, 0.0 },
        { "-0",                  JSONB_INT32,   0, 0.0 },
        { "2147483647",          JSONB_INT32,   2147483647LL, 0.0 },
        { "-2147483648",         JSONB_INT32,   -2147483648LL, 0.0 },
        { "2147483648",          JSONB_INT64,   2147483648LL, 0.0 },
        { "-2147483649",         JSONB_INT64,   -2147483649LL, 0.0 },
        { "9223372036854775807", JSONB_INT64,   9223372036854775807LL, 0.0 },
        /* 超出 int64 ⇒ FLOAT64（不是错误） */
        { "9223372036854775808", JSONB_FLOAT64, 0, 9223372036854775808.0 },
        { "-9223372036854775809", JSONB_FLOAT64, 0, -9223372036854775808.0 },
        { "1.5",                 JSONB_FLOAT64, 0, 1.5 },
        { "1.0",                 JSONB_FLOAT64, 0, 1.0 },
        { "3.14159265358979",    JSONB_FLOAT64, 0, 3.14159265358979 },
        { "1E-999",              JSONB_FLOAT64, 0, 0.0 },
        { "-1e-3",               JSONB_FLOAT64, 0, -0.001 },
        { "0.1",                 JSONB_FLOAT64, 0, 0.1 }
    };
    size_t i;

    printf("  2) 数字精度：int32 / int64 / float64 的归类\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        JsonbValue *t = NULL;

        if (json_parse(cases[i].text, (u32)strlen(cases[i].text), &t) != OK) {
            printf("    FAIL 解析失败：%s\n", cases[i].text);
            failures++;
            continue;
        }
        if (t->tag != cases[i].tag) {
            printf("    FAIL %s 的类型是 %d（应为 %d）\n",
                   cases[i].text, (int)t->tag, (int)cases[i].tag);
            failures++;
        } else if (cases[i].tag == JSONB_FLOAT64) {
            if (jsonb_test_dbits(t->u.d) != jsonb_test_dbits(cases[i].dval)) {
                printf("    FAIL %s 的值不对\n", cases[i].text);
                failures++;
            }
        } else {
            if (t->u.i != cases[i].ival) {
                printf("    FAIL %s 的值不对\n", cases[i].text);
                failures++;
            }
        }
        jsonb_free(t);
    }

    /* 1 与 1.0 必须能区分开 */
    {
        JsonbValue *a = NULL;
        JsonbValue *b = NULL;

        CHECK(json_parse("1", 1, &a) == OK);
        CHECK(json_parse("1.0", 3, &b) == OK);
        CHECK(a && b && a->tag != b->tag);
        jsonb_free(a);
        jsonb_free(b);
    }

    /*
     * 1e999 ⇒ 溢出 ⇒ FLOAT64(inf)，**不许报错**。
     *
     * ⚠️ 这里**不写 1e999 这个字面量** —— 编译器会当常量溢出报错
     *    （-Werror=overflow）；改成"比 1e308 大"来判 inf。
     */
    {
        JsonbValue *t = NULL;

        CHECK(json_parse("1e999", 5, &t) == OK);
        CHECK(t && t->tag == JSONB_FLOAT64);
        CHECK(t && t->u.d > 1e308);       /* inf（NaN 过不了这一关） */
        jsonb_free(t);
    }
    printf("    %zu 个数字归类正确；1 与 1.0 可区分\n",
           sizeof(cases) / sizeof(cases[0]));
}

/* ---------------- 3) 字符串转义 ---------------- */

typedef struct {
    const char *text;
    const char *want;      /* 期望的字节（**不含**结尾 NUL，长度另给） */
    u32         want_len;
} StrCase;

static void t3_strings(void)
{
    static const StrCase cases[] = {
        { "\"a\\\"b\"",      "a\"b",      3 },
        { "\"a\\\\b\"",      "a\\b",      3 },
        { "\"a\\/b\"",        "a/b",       3 },
        { "\"a\\bb\"",       "a\bb",      3 },
        { "\"a\\fb\"",       "a\fb",      3 },
        { "\"a\\nb\"",       "a\nb",      3 },
        { "\"a\\rb\"",       "a\rb",      3 },
        { "\"a\\tb\"",       "a\tb",      3 },
        { "\"\\u0041\"",     "A",         1 },
        { "\"\\u00e9\"",     "\xc3\xa9",  2 },
        { "\"\\u4e2d\"",     "\xe4\xb8\xad", 3 },
        /* 代理对 ⇒ 4 字节 UTF-8（😀 = U+1F600） */
        { "\"\\uD83D\\uDE00\"", "\xf0\x9f\x98\x80", 4 },
        /* 裸 UTF-8：原样保留 */
        { "\"\xe4\xb8\xad\xe6\x96\x87\"", "\xe4\xb8\xad\xe6\x96\x87", 6 },
        { "\"\xf0\x9f\x98\x80\"",         "\xf0\x9f\x98\x80",         4 },
        /* \u0000 ⇒ 字符串里带 NUL 字节（长度必须带着走） */
        { "\"a\\u0000b\"",   "a\0b",      3 },
        { "\"\\u0000\"",      "\0",        1 },
        { "\"\"",            "",          0 }
    };
    size_t i;

    printf("  3) 字符串转义：转义 / 代理对 / 裸 UTF-8 / NUL\n");

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        JsonbValue *t = NULL;

        if (json_parse(cases[i].text, (u32)strlen(cases[i].text), &t) != OK) {
            printf("    FAIL 解析失败：%s\n", cases[i].text);
            failures++;
            continue;
        }
        if (t->tag != JSONB_STRING) {
            printf("    FAIL %s 不是字符串\n", cases[i].text);
            failures++;
        } else if (t->u.s.len != cases[i].want_len ||
                   (cases[i].want_len != 0u &&
                    memcmp(t->u.s.p, cases[i].want, cases[i].want_len) != 0)) {
            printf("    FAIL %s 解出来的字节不对（长度 %u，应为 %u）\n",
                   cases[i].text, t->u.s.len, cases[i].want_len);
            failures++;
        }
        jsonb_free(t);
    }

    /* 带 NUL 的字符串往返：字节流里也必须保住 NUL 与长度 */
    {
        JsonbValue *t1 = NULL;
        JsonbValue *t2 = NULL;
        u8 *buf = (u8 *)malloc(64);
        u32 len = 0;

        CHECK(buf != NULL);
        if (buf) {
            CHECK(json_parse("\"a\\u0000b\"", 10, &t1) == OK);
            CHECK(jsonb_encode(t1, buf, 64, &len) == OK);
            CHECK(jsonb_decode(buf, len, &t2) == OK);
            CHECK(t1 && t2 && jsonb_test_tree_equal(t1, t2));
            CHECK(t2 && t2->u.s.len == 3u);
            jsonb_free(t1);
            jsonb_free(t2);
            free(buf);
        }
    }
    printf("    %zu 个字符串用例正确\n", sizeof(cases) / sizeof(cases[0]));
}

/* ---------------- 4) 重复键保留首个 ---------------- */

static void t4_dup_keys(void)
{
    JsonbValue *t = NULL;
    const JsonbValue *v;

    printf("  4) 重复键保留首个\n");

    CHECK(json_parse("{\"a\":1,\"a\":2}", 13, &t) == OK);
    if (t) {
        CHECK(t->tag == JSONB_OBJECT);
        CHECK(t->u.obj.count == 1u);          /* 只剩一个成员 */
        v = jsonb_test_obj_find(t, "a");
        CHECK(v != NULL);
        if (v) {
            CHECK(v->tag == JSONB_INT32);
            CHECK(v->u.i == 1);               /* 保留的是**首个** */
        }
        jsonb_free(t);
        t = NULL;
    }

    /* 三个同名 ⇒ 还是只留首个 */
    CHECK(json_parse("{\"a\":1,\"b\":9,\"a\":2,\"a\":3}", 25, &t) == OK);
    if (t) {
        CHECK(t->u.obj.count == 2u);
        v = jsonb_test_obj_find(t, "a");
        CHECK(v && v->u.i == 1);
        jsonb_free(t);
        t = NULL;
    }

    /* ⚠️ 被丢弃的字段**照样要完整解析**：`{"a":1,"a":}` 是非法的 */
    CHECK(json_parse("{\"a\":1,\"a\":}", 12, &t) != OK);
    CHECK(t == NULL);
    jsonb_free(t);
    printf("    重复键保留首个；被丢弃的字段仍要过语法检查\n");
}

/* ---------------- 5) 嵌套 ---------------- */

static void t5_nested(void)
{
    JsonbValue *t = NULL;
    const JsonbValue *a;
    const JsonbValue *b;

    printf("  5) 嵌套：{\"a\":{\"b\":[1,2,{\"c\":null}]}}\n");

    CHECK(json_parse("{\"a\":{\"b\":[1,2,{\"c\":null}]}}", 28, &t) == OK);
    if (t) {
        a = jsonb_test_obj_find(t, "a");
        CHECK(a && a->tag == JSONB_OBJECT);
        if (a) {
            b = jsonb_test_obj_find(a, "b");
            CHECK(b && b->tag == JSONB_ARRAY);
            if (b) {
                CHECK(b->u.arr.count == 3u);
                CHECK(b->u.arr.items[0]->tag == JSONB_INT32);
                CHECK(b->u.arr.items[0]->u.i == 1);
                CHECK(b->u.arr.items[1]->u.i == 2);
                CHECK(b->u.arr.items[2]->tag == JSONB_OBJECT);
                {
                    const JsonbValue *c =
                        jsonb_test_obj_find(b->u.arr.items[2], "c");

                    CHECK(c && c->tag == JSONB_NULL);
                }
            }
        }
        jsonb_free(t);
        t = NULL;
    }
    CHECK(jsonb_test_roundtrip("{\"a\":{\"b\":[1,2,{\"c\":null}]}}", NULL));
    printf("    嵌套结构逐字段相等\n");
}

/* ---------------- 6) 边界 ---------------- */

static void t6_edges(void)
{
    JsonbValue *t = NULL;

    printf("  6) 边界：空对象 / 空数组 / 空字符串\n");

    CHECK(json_parse("{}", 2, &t) == OK);
    CHECK(t && t->tag == JSONB_OBJECT && t->u.obj.count == 0u);
    jsonb_free(t);
    t = NULL;

    CHECK(json_parse("[]", 2, &t) == OK);
    CHECK(t && t->tag == JSONB_ARRAY && t->u.arr.count == 0u);
    jsonb_free(t);
    t = NULL;

    CHECK(json_parse("\"\"", 2, &t) == OK);
    CHECK(t && t->tag == JSONB_STRING && t->u.s.len == 0u);
    jsonb_free(t);
    t = NULL;

    /* 空输入是非法的 */
    CHECK(json_parse("", 0, &t) != OK);
    CHECK(t == NULL);
    jsonb_free(t);

    CHECK(jsonb_test_roundtrip("{}", NULL));
    CHECK(jsonb_test_roundtrip("[]", NULL));
    CHECK(jsonb_test_roundtrip("\"\"", NULL));
    printf("    空对象 / 空数组 / 空字符串都正确；空输入非法\n");
}

/* ---------------- 7) 非法输入必须硬失败 ---------------- */

static void t7_invalid(void)
{
    static const char *bad[] = {
        "{\"a\":1,}",         /* 尾逗号 */
        "{'a':1}",            /* 单引号 */
        "{a:1}",             /* 键没引号 */
        "{\"a\":",            /* 截断 */
        "{} x",              /* 多余的字符 */
        "1e",                /* 指数位缺数字 */
        "NaN",               /* 非法字面量 */
        "Infinity",
        "-Infinity",
        "",                  /* 空输入 */
        "[",                 /* 截断 */
        "]",                 /* 多余的 */
        "{\"a\":1 \"b\":2}",  /* 少逗号 */
        "01",                /* 前导零 */
        "1.",                /* 小数点后没数字 */
        ".5",                /* 小数点前没数字 */
        "+1",                /* 不许 "+" 开头 */
        "\"unterminated",    /* 字符串没闭合 */
        "\"\\q\"",           /* 不认识的转义 */
        "\"\\uD83D\"",       /* 落单的高代理项 */
        "\"\\uDE00\"",       /* 落单的低代理项 */
        "[1,2,]",            /* 数组尾逗号 */
        "tru",               /* 字面量不完整 */
        "{,}",               /* 空成员 */
        "{\"a\" 1}",         /* 少冒号 */
        "\"a\tb\""           /* 裸的控制字符 */
    };
    size_t i;

    printf("  7) 非法输入必须硬失败（E_INVAL，不是半截）\n");

    for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        JsonbValue *t = NULL;
        int rc = json_parse(bad[i], (u32)strlen(bad[i]), &t);

        if (rc == OK) {
            printf("    FAIL 本该非法却解析成功了：%s\n", bad[i]);
            failures++;
        }
        if (t != NULL) {
            printf("    FAIL 非法输入却返回了树：%s\n", bad[i]);
            failures++;
        }
        jsonb_free(t);
    }

    /* 裸 UTF-8 非法字节必须拒绝，输入长度用显式值，不走 strlen。 */
    {
        static const u8 bad_utf8[][6] = {
            { '"', 0x80u, '"', 0, 0, 0 },
            { '"', 0xC2u, '"', 0, 0, 0 },
            { '"', 0xC0u, 0xAFu, '"', 0, 0 },
            { '"', 0xEDu, 0xA0u, 0x80u, '"', 0 },
            { '"', 0xF4u, 0x90u, 0x80u, 0x80u, '"' },
            /* 输入在序列中途被截断（3 字节序列缺第 3 字节） */
            { '"', 0xE4u, 0xB8u, 0, 0, 0 },
            /* 输入在序列中途被截断（4 字节序列缺第 4 字节） */
            { '"', 0xF0u, 0x9Fu, 0x98u, 0, 0 }
        };
        static const u32 bad_utf8_lens[] = { 3u, 3u, 4u, 5u, 6u, 3u, 4u };
        size_t j;

        for (j = 0; j < sizeof(bad_utf8_lens) / sizeof(bad_utf8_lens[0]); j++) {
            JsonbValue *t = NULL;
            int rc = json_parse((const char *)bad_utf8[j], bad_utf8_lens[j], &t);

            CHECK(rc == E_INVAL);
            CHECK(t == NULL);
            jsonb_free(t);
        }
    }

    printf("    %zu 个非法输入全部被拒\n", sizeof(bad) / sizeof(bad[0]));
}

/* ---------------- 8) 编码稳定 ---------------- */

static void t8_stable(void)
{
    static const char *docs[] = {
        "{\"z\":1,\"a\":2,\"m\":3}",
        "{\"b\":{\"y\":1,\"x\":2},\"a\":[3,2,1]}",
        "[1,[2,[3,[4]]]]"
    };
    size_t i;

    printf("  8) 编码稳定：同一棵树编码两次 ⇒ 字节完全相同\n");

    for (i = 0; i < sizeof(docs) / sizeof(docs[0]); i++) {
        JsonbValue *t = NULL;
        u8 *b1 = NULL;
        u8 *b2 = NULL;
        u32 l1 = 0;
        u32 l2 = 0;

        if (json_parse(docs[i], (u32)strlen(docs[i]), &t) != OK) {
            printf("    FAIL 解析失败：%s\n", docs[i]);
            failures++;
            continue;
        }
        CHECK(jsonb_encode(t, NULL, 0u, &l1) == OK);
        b1 = (u8 *)malloc(l1 ? l1 : 1u);
        b2 = (u8 *)malloc(l1 ? l1 : 1u);
        CHECK(b1 && b2);
        if (b1 && b2) {
            CHECK(jsonb_encode(t, b1, l1, &l1) == OK);
            CHECK(jsonb_encode(t, b2, l1, &l2) == OK);
            CHECK(l1 == l2);
            CHECK(memcmp(b1, b2, l1) == 0);

            /* 缓冲不够 ⇒ E_NOSPACE，且**一个字节都不写** */
            {
                u8 small[4];
                u32 need = 0;

                memset(small, 0xAA, sizeof(small));
                CHECK(jsonb_encode(t, small, sizeof(small), &need) == E_NOSPACE);
                CHECK(need == l1);            /* 报出需要多少 */
                CHECK(small[0] == 0xAA);      /* 没写任何东西 */
            }
        }
        free(b1);
        free(b2);
        jsonb_free(t);
    }
    printf("    编码是确定性的；缓冲不够时不写半个字节\n");
}

/* ---------------- 入口 ---------------- */

int main(void)
{
    printf("test_jsonb:\n");

    t1_roundtrip();
    t2_numbers();
    t3_strings();
    t4_dup_keys();
    t5_nested();
    t6_edges();
    t7_invalid();
    t8_stable();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 组验收全部通过\n");
    return 0;
}
