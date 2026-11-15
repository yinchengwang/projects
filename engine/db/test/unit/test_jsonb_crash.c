/*
 * test_jsonb_crash.c —— JSONB 的"抗损坏 / 不爆栈 / 不崩"单测
 *                     （v1.6 步骤 1 · T13 · A1）
 *
 * 共用工具见 jsonb_test_common.h；
 * 语法 / 语义 / 编码那一半在 test_jsonb.c。
 *
 * ==================== 这一步要证明什么 ====================
 *
 * 这一组测的是**"坏输入不许把内核带崩"**——
 * JSON 是用户塞进来的数据，字节流也可能是磁盘上损坏的数据。
 * 这两条要求：
 *
 *   9.  **不建树取值**：jsonb_obj_get / jsonb_arr_get / jsonb_tag_of
 *        在**编码后的字节流**上取值，结果与在树上取值一致
 *  10. **损坏输入 ⇒ E_CORRUPT**：逐字节破坏 + 逐字节截断
 *  11. **深度**：嵌套 100 层不许爆栈；超限要**报错**而不是崩
 *  12. **fuzz**：固定种子的随机字节跑几千轮 ⇒ 一律返回错误码，**绝不崩**
 *
 * ==================== 第 10 项的断言为什么是"要么报错、要么不同" ====================
 *
 * JSONB 的格式里**没有校验和**（坑 M7-14）⇒ 字符串载荷里的**一个字节**
 * 被翻转，在原理上就**检测不出来**（解码出来的是一个"合法但内容不同"的字符串）。
 *
 * 所以这里**不**断言"任何破坏都必须报错"（那是格式做不到的事），
 * 而是断言"破坏之后**绝不**悄悄给出和原来一模一样的东西" ——
 * 要么报错，要么结果与原来不同。
 *
 * 结构性破坏（长度、计数、tag、尾随垃圾）**必须**报错 —— 那一半是强校验管的。
 */

#include "jsonb_test_common.h"

static int failures = 0;

/* ---------------- 9) 不建树取值 ---------------- */

static void t9_direct_get(void)
{
    static const char *doc = "{\"a\":1,\"b\":[10,20],\"c\":{\"d\":\"x\"}}";
    JsonbValue *t = NULL;
    u8 *buf = NULL;
    u32 len = 0;
    const u8 *val = NULL;
    u32 vlen = 0;
    JsonbTag tag;

    printf("  9) 不建树取值：obj_get / arr_get / tag_of\n");

    if (json_parse(doc, (u32)strlen(doc), &t) != OK) {
        printf("    FAIL 解析失败\n");
        failures++;
        return;
    }
    CHECK(jsonb_encode(t, NULL, 0u, &len) == OK);
    buf = (u8 *)malloc(len);
    CHECK(buf != NULL);
    if (!buf) {
        jsonb_free(t);
        return;
    }
    CHECK(jsonb_encode(t, buf, len, &len) == OK);

    /* 取 "a"（整数） */
    CHECK(jsonb_obj_get(buf, len, "a", 1, &val, &vlen) == OK);
    CHECK(val && vlen == 5u);
    CHECK(jsonb_tag_of(val, vlen, &tag) == OK);
    CHECK(tag == JSONB_INT32);
    CHECK((i64)(i32)get_u32(val + 1) == 1);
    /* 与树上的取值一致 */
    {
        const JsonbValue *tv = jsonb_test_obj_find(t, "a");

        CHECK(tv && tv->tag == JSONB_INT32 && tv->u.i == 1);
    }

    /* 取 "b"（数组）→ 第 1 个元素 */
    CHECK(jsonb_obj_get(buf, len, "b", 1, &val, &vlen) == OK);
    CHECK(jsonb_tag_of(val, vlen, &tag) == OK);
    CHECK(tag == JSONB_ARRAY);
    {
        const u8 *elem = NULL;
        u32 elen = 0;

        CHECK(jsonb_arr_get(val, vlen, 0, &elem, &elen) == OK);
        CHECK(jsonb_tag_of(elem, elen, &tag) == OK);
        CHECK(tag == JSONB_INT32);
        CHECK((i64)(i32)get_u32(elem + 1) == 10);

        CHECK(jsonb_arr_get(val, vlen, 1, &elem, &elen) == OK);
        CHECK((i64)(i32)get_u32(elem + 1) == 20);

        /* 越界 ⇒ E_NOTFOUND */
        CHECK(jsonb_arr_get(val, vlen, 2, &elem, &elen) == E_NOTFOUND);
    }

    /* 取 "c"（对象）→ 里面的 "d" */
    CHECK(jsonb_obj_get(buf, len, "c", 1, &val, &vlen) == OK);
    CHECK(jsonb_tag_of(val, vlen, &tag) == OK);
    CHECK(tag == JSONB_OBJECT);
    {
        const u8 *s = NULL;
        u32 slen = 0;

        CHECK(jsonb_obj_get(val, vlen, "d", 1, &s, &slen) == OK);
        CHECK(jsonb_tag_of(s, slen, &tag) == OK);
        CHECK(tag == JSONB_STRING);
        CHECK(slen == 1u + 4u + 1u);      /* tag + len + "x" */
    }

    /* 不存在的键 ⇒ E_NOTFOUND */
    CHECK(jsonb_obj_get(buf, len, "zzz", 3, &val, &vlen) == E_NOTFOUND);

    /* 对着**非对象**取成员 ⇒ E_INVAL（不是 E_NOTFOUND） */
    {
        JsonbValue *one = NULL;
        u8 onebuf[8];
        u32 onelen = 0;

        CHECK(json_parse("1", 1, &one) == OK);
        CHECK(jsonb_encode(one, onebuf, sizeof(onebuf), &onelen) == OK);
        CHECK(jsonb_obj_get(onebuf, onelen, "a", 1, &val, &vlen) == E_INVAL);
        jsonb_free(one);
    }

    free(buf);
    jsonb_free(t);
    printf("    字节流上取值与树上取值一致\n");
}

/* ---------------- 10) 损坏输入 ⇒ E_CORRUPT ---------------- */

static void t10_corrupt(void)
{
    static const char *doc =
        "{\"a\":1,\"b\":[10,20],\"c\":{\"d\":\"xy\"},\"e\":null,\"f\":1.5}";
    JsonbValue *t = NULL;
    JsonbValue *orig = NULL;
    u8 *buf = NULL;
    u32 len = 0;
    u32 i;

    printf("  10) 损坏输入 ⇒ E_CORRUPT（逐字节破坏 + 逐字节截断）\n");

    if (json_parse(doc, (u32)strlen(doc), &t) != OK) {
        printf("    FAIL 解析失败\n");
        failures++;
        return;
    }
    CHECK(jsonb_encode(t, NULL, 0u, &len) == OK);
    buf = (u8 *)malloc(len);
    CHECK(buf != NULL);
    if (!buf) {
        jsonb_free(t);
        return;
    }
    CHECK(jsonb_encode(t, buf, len, &len) == OK);
    CHECK(jsonb_decode(buf, len, &orig) == OK);

    /* 逐字节破坏：要么报错，要么结果与原来**不同**（绝不悄悄给一样的） */
    for (i = 0; i < len; i++) {
        u8 save = buf[i];
        JsonbValue *bad = NULL;
        int rc;

        buf[i] = (u8)(save ^ 0xFFu);
        rc = jsonb_decode(buf, len, &bad);
        if (rc == OK) {
            if (bad && jsonb_test_tree_equal(bad, orig)) {
                printf("    FAIL 第 %u 字节被破坏，却解出了**一模一样**的树\n", i);
                failures++;
            }
            jsonb_free(bad);
        } else if (rc != E_CORRUPT && rc != E_NOSPACE) {
            printf("    FAIL 第 %u 字节被破坏，返回了怪错误码 %d\n", i, rc);
            failures++;
        }
        buf[i] = save;
    }

    /* 逐字节截断：任何**短**的片段都不是一个完整的 JSONB ⇒ 必须报错 */
    for (i = 0; i < len; i++) {
        JsonbValue *bad = NULL;
        int rc = jsonb_decode(buf, i, &bad);

        if (rc == OK) {
            printf("    FAIL 截断到 %u 字节却解码成功了\n", i);
            failures++;
            jsonb_free(bad);
        }
    }

    /* 结构性破坏（长度 / 计数 / tag）必须报错 */
    {
        u8 bad[64];
        JsonbValue *tree = NULL;

        memcpy(bad, buf, len);
        bad[1] = (u8)(bad[1] + 1u);        /* 对象计数 +1 */
        CHECK(jsonb_decode(bad, len, &tree) == E_CORRUPT);
        jsonb_free(tree);
        tree = NULL;

        memcpy(bad, buf, len);
        bad[0] = 0x7Fu;                    /* 不认识的 tag */
        CHECK(jsonb_decode(bad, len, &tree) == E_CORRUPT);
        jsonb_free(tree);
        tree = NULL;
    }

    free(buf);
    jsonb_free(orig);
    jsonb_free(t);
    printf("    %u 个字节逐个破坏、截断都处理正确\n", len);
}

/* ---------------- 11) 深度 ---------------- */

/* 造 n 层嵌套：{"a":{"a":{...1...}}}，返回文本长度 */
static u32 build_nested(char *buf, u32 cap, u32 n)
{
    u32 pos = 0;
    u32 i;

    for (i = 0; i < n; i++) {
        if (pos + 6u > cap) {
            return 0u;
        }
        memcpy(buf + pos, "{\"a\":", 5);
        pos += 5;
    }
    if (pos + 2u > cap) {
        return 0u;
    }
    buf[pos++] = '1';
    for (i = 0; i < n; i++) {
        buf[pos++] = '}';
    }
    buf[pos] = '\0';
    return pos;
}

static void t11_depth(void)
{
    char *deep;

    printf("  11) 深度：嵌套 100 层不许爆栈；超限要报错\n");

    deep = (char *)malloc(210u * 6u + 16u);
    CHECK(deep != NULL);
    if (!deep) {
        return;
    }

    /* 100 层（lead 要求的深度）⇒ 必须能解析 */
    {
        JsonbValue *t = NULL;
        u32 len = build_nested(deep, 210u * 6u + 16u, 100u);

        CHECK(len > 0u);
        CHECK(json_parse(deep, len, &t) == OK);
        CHECK(t != NULL);
        jsonb_free(t);
        t = NULL;
    }

    /* 正好在上限 ⇒ 能解析（路径上的值不超过 200 个） */
    {
        JsonbValue *t = NULL;
        u32 len = build_nested(deep, 210u * 6u + 16u, 199u);

        CHECK(len > 0u);
        CHECK(json_parse(deep, len, &t) == OK);
        CHECK(t != NULL);
        jsonb_free(t);
        t = NULL;
    }

    /* 超一个值 ⇒ 报错（**不是崩**） */
    {
        JsonbValue *t = NULL;
        u32 len = build_nested(deep, 210u * 6u + 16u, 200u);

        CHECK(len > 0u);
        CHECK(json_parse(deep, len, &t) == E_INVAL);
        CHECK(t == NULL);
        jsonb_free(t);
    }

    free(deep);
    printf("    100 层与 199 层能解析；200 层返回 E_INVAL\n");
}

/* ---------------- 12) fuzz ---------------- */

/*
 * 固定种子的 LCG（不用 rand() —— 它的实现各平台不一样，
 * "可复现"这件事必须自己掌握）。
 */
static u32 g_seed = 20261104u;

static u32 rnd(void)
{
    g_seed = g_seed * 1103515245u + 12345u;
    return (g_seed >> 8) & 0xFFFFFFu;
}

static void t12_fuzz(void)
{
    u8 buf[64];
    int i;

    printf("  12) fuzz：几千轮随机字节 ⇒ 一律返回错误码，绝不崩\n");

    for (i = 0; i < 5000; i++) {
        u32 len = 1u + (rnd() % (u32)sizeof(buf));
        u32 k;
        JsonbValue *t = NULL;
        int rc;

        for (k = 0; k < len; k++) {
            buf[k] = (u8)(rnd() & 0xFFu);
        }

        /* 解析器：要么成功、要么一个像样的错误码；绝不能崩 */
        rc = json_parse((const char *)buf, len, &t);
        if (rc != OK && rc != E_INVAL && rc != E_NOSPACE) {
            printf("    FAIL 解析器返回了怪错误码 %d\n", rc);
            failures++;
            break;
        }
        if (rc == OK && t == NULL) {
            printf("    FAIL 解析成功了却没给树\n");
            failures++;
            break;
        }
        jsonb_free(t);
        t = NULL;

        /* 解码器：同样 */
        rc = jsonb_decode(buf, len, &t);
        if (rc != OK && rc != E_CORRUPT && rc != E_NOSPACE && rc != E_INVAL) {
            printf("    FAIL 解码器返回了怪错误码 %d\n", rc);
            failures++;
            break;
        }
        jsonb_free(t);
        t = NULL;
    }
    printf("    5000 轮随机字节：解析器与解码器都没崩\n");
}

/* ---------------- 入口 ---------------- */

int main(void)
{
    printf("test_jsonb_crash:\n");

    t9_direct_get();
    t10_corrupt();
    t11_depth();
    t12_fuzz();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  4 组验收全部通过\n");
    return 0;
}
