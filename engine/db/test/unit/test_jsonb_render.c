/*
 * test_jsonb_render.c —— JSONB → JSON 文本（v1.6 C5.3）
 *
 * 【为什么需要它】
 * JSONB 是二进制。用户 `SELECT doc FROM t` 时不能看到原始字节 ——
 * 那会在内嵌 NUL、控制字符、非文本 tag 上截断或打印乱码。
 * 值层（`val_to_str`）在 T_JSON 分支上明确说"请用 db_json_decode 解码"，
 * 所以这里必须真的有一个可用的解码回调。
 *
 * 6 项：
 *   1. 标量往返：null / true / false / 整数 / 小数 / 字符串
 *   2. 容器往返：空对象 / 空数组 / 嵌套
 *   3. 数字精度：int32 / int64 边界值原样回来（不变成浮点）
 *   4. 转义：引号、反斜杠、控制字符、非 ASCII（UTF-8 直接出，不转 \u）
 *   5. 键序：JSONB 里键已排序，渲染按那个顺序（不是原始输入顺序）
 *   6. 缓冲不足 ⇒ E_NOSPACE，且**不写半个文本**
 *
 * 【第 3 与第 4 项是核心】
 * 数字变成浮点 = 静默改值（9223372036854775807 会变成 9.2e18）。
 * 控制字符不转义 = 输出的"JSON"根本不可再解析。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/doc/jsonb.h"
#include "modalities/doc/json_parse.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/*
 * 文本 → JSONB → 文本，返回第二次的文本。
 * 失败时返回 NULL 并打印原因。
 */
static char *render(const char *text, u32 *out_len)
{
    JsonbValue *tree = NULL;
    u8 buf[65536];
    u32 blen = 0;
    char *txt = NULL;
    u32 tlen = 0;

    if (json_parse_str(text, &tree) != OK) {
        printf("    解析失败: %s\n", text);
        return NULL;
    }
    if (jsonb_encode(tree, buf, sizeof(buf), &blen) != OK) {
        printf("    编码失败: %s\n", text);
        jsonb_free(tree);
        return NULL;
    }
    jsonb_free(tree);

    if (jsonb_render(buf, blen, NULL, 0, &tlen) != E_NOSPACE) {
        printf("    问长度没返回 E_NOSPACE: %s\n", text);
        return NULL;
    }
    txt = malloc(tlen + 1);
    if (!txt)
        return NULL;
    if (jsonb_render(buf, blen, txt, tlen, &tlen) != OK) {
        printf("    渲染失败: %s\n", text);
        free(txt);
        return NULL;
    }
    txt[tlen] = '\0';
    if (out_len)
        *out_len = tlen;
    return txt;
}

/* 断言 文本 → JSONB → 文本 等于期望串 */
static void expect_render(const char *in, const char *want)
{
    char *got = render(in, NULL);
    if (!got) {
        failures++;
        return;
    }
    if (strcmp(got, want) != 0) {
        printf("    FAIL 渲染不符\n      输入: %s\n      期望: %s\n      实际: %s\n",
               in, want, got);
        failures++;
    }
    free(got);
}

int main(void)
{
    printf("test_jsonb_render:\n");

    /* ---------------- 1. 标量 ---------------- */
    expect_render("null", "null");
    expect_render("true", "true");
    expect_render("false", "false");
    expect_render("0", "0");
    expect_render("-1", "-1");
    expect_render("1.5", "1.5");
    expect_render("\"hello\"", "\"hello\"");

    /* ---------------- 2. 容器 ---------------- */
    expect_render("{}", "{}");
    expect_render("[]", "[]");
    expect_render("[1,2,3]", "[1,2,3]");
    expect_render("{\"a\":1}", "{\"a\":1}");
    expect_render("{\"a\":{\"b\":[1,2,{\"c\":null}]}}",
                  "{\"a\":{\"b\":[1,2,{\"c\":null}]}}");

    /* ---------------- 3. 数字精度：整数不许变成浮点 ---------------- */
    expect_render("{\"big\":9223372036854775807}",
                  "{\"big\":9223372036854775807}");
    expect_render("{\"neg\":-9223372036854775808}",
                  "{\"neg\":-9223372036854775808}");

    /* ---------------- 4. 转义 ---------------- */
    /* 引号与反斜杠 */
    expect_render("\"a\\\"b\"", "\"a\\\"b\"");
    expect_render("\"a\\\\b\"", "\"a\\\\b\"");
    /* 控制字符（\n 与 \t） */
    expect_render("\"a\\nb\"", "\"a\\nb\"");
    expect_render("\"a\\tb\"", "\"a\\tb\"");
    /* 非 ASCII：UTF-8 直接输出，不转成 \uXXXX */
    expect_render("\"\\u4e2d\\u6587\"", "\"中文\"");

    /* ---------------- 5. 键序：按 JSONB 的排序，不是输入顺序 ---------------- */
    /*
     * 输入键序 z, a, m；JSONB 内部按 (长度, 字典序) 排 —— 三个都是单字节，
     * 所以按字典序 ⇒ a, m, z。
     */
    expect_render("{\"z\":1,\"a\":2,\"m\":3}", "{\"a\":2,\"m\":3,\"z\":1}");

    /* ---------------- 6. 缓冲不足 ⇒ E_NOSPACE，不写半个文本 ---------------- */
    {
        JsonbValue *tree = NULL;
        u8 buf[256];
        u32 blen = 0;
        char small[4];
        u32 tlen = 0;

        CHECK(json_parse_str("{\"a\":1}", &tree) == OK);
        CHECK(jsonb_encode(tree, buf, sizeof(buf), &blen) == OK);
        jsonb_free(tree);

        memset(small, 'X', sizeof(small));
        int rc = jsonb_render(buf, blen, small, 3, &tlen);
        CHECK(rc == E_NOSPACE);
        /* 不写半个文本：small 里不能有 '{' 混进去 */
        for (int i = 0; i < 4; i++)
            CHECK(small[i] == 'X');
    }

    if (failures == 0) {
        printf("  6 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
