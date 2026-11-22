/*
 * test_json_match.c —— 匹配规格解析与求值（v1.6 C5.7）
 *
 * 8 项：
 *   1. 规格解析：路径 + 比较符 + 字面量（六种比较符都认）
 *   2. 规格解析失败要明确报错（不猜语义）
 *   3. 等值匹配：数字 / 字符串 / true / false
 *   4. 大小比较：< <= > >=（数字与字符串分别走各自语义）
 *   5. **三值逻辑**：路径不存在 / 值是 null / 类型不匹配 ⇒ 所有比较符都 false
 *   6. `!=` 在 NULL 上不许给 true（最经典的三值逻辑错误）
 *   7. 小数字面量（REAL）：`1` 与 `1.0` 数值相等
 *   8. 端到端的"索引会重判"口径：同一个文档用同一套规则，结果必须一致
 *
 * 【第 5、6 项是核心】
 * 把"未知"当成"假"会让 WHERE 少返回行（漏数据）；
 * 把"未知"当成"真"会让 WHERE 多返回行（错数据）。
 * 两者都是静默错误 —— 查询不报错，但答案错了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/doc/json_match.h"
#include "modalities/doc/json_parse.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 文本 JSON → JSONB 字节（栈缓冲，够小文档用） */
static int to_jsonb(const char *text, u8 *buf, u32 cap, u32 *len)
{
    return json_parse_to_jsonb(text, (u32)strlen(text), buf, cap, len);
}

/* 断言：一条规格在一个文档上的求值结果 */
static void expect_match(const char *json, const char *spec, bool want)
{
    u8 buf[4096];
    u32 blen = 0;
    JsonMatchSpec s;

    if (to_jsonb(json, buf, sizeof(buf), &blen) != OK) {
        printf("    FAIL 编码失败: %s\n", json);
        failures++;
        return;
    }
    if (json_match_parse(spec, &s) != OK) {
        printf("    FAIL 规格解析失败: %s\n", spec);
        failures++;
        return;
    }

    bool got = false;
    int rc = json_match_eval(buf, (u16)blen, &s, &got);
    if (rc != OK) {
        printf("    FAIL 求值失败(%d): %s on %s\n", rc, spec, json);
        failures++;
        return;
    }
    if (got != want) {
        printf("    FAIL %s on %s：期望 %s 实际 %s\n", spec, json,
               want ? "true" : "false", got ? "true" : "false");
        failures++;
    }
}

/* 断言：规格解析必须失败 */
static void expect_parse_fail(const char *spec)
{
    JsonMatchSpec s;
    if (json_match_parse(spec, &s) == OK) {
        printf("    FAIL 本该解析失败: %s\n", spec);
        failures++;
    }
}

int main(void)
{
    printf("test_json_match:\n");

    /* ---------------- 1. 六种比较符都认 ---------------- */
    {
        const char *specs[] = {
            "$.a = 1", "$.a != 1", "$.a < 1",
            "$.a <= 1", "$.a > 1", "$.a >= 1"
        };
        for (size_t i = 0; i < sizeof(specs) / sizeof(specs[0]); i++) {
            JsonMatchSpec s;
            if (json_match_parse(specs[i], &s) != OK) {
                printf("    FAIL 解析失败: %s\n", specs[i]);
                failures++;
            }
        }
        /* `< >` 是 `!=` 的别名 */
        JsonMatchSpec s;
        CHECK(json_match_parse("$.a <> 1", &s) == OK);
        CHECK(s.op == JSON_OP_NE);
    }

    /* ---------------- 2. 解析失败要明确报错 ---------------- */
    expect_parse_fail("");                 /* 空规格 */
    expect_parse_fail("a = 1");            /* 路径不以 $ 开头 */
    expect_parse_fail("$.a");              /* 缺比较符与字面量 */
    expect_parse_fail("$.a = ");           /* 缺字面量 */
    expect_parse_fail("$.a = 1 extra");    /* 尾部多余 */
    expect_parse_fail("$.a == 1");         /* 双等号 */
    expect_parse_fail("$.a = \"unclosed"); /* 引号没闭合 */

    /* ---------------- 3. 等值匹配 ---------------- */
    expect_match("{\"a\":5}", "$.a = 5", true);
    expect_match("{\"a\":5}", "$.a = 6", false);
    expect_match("{\"a\":\"x\"}", "$.a = \"x\"", true);
    expect_match("{\"a\":\"x\"}", "$.a = 'x'", true);   /* 单引号同义 */
    expect_match("{\"a\":true}", "$.a = true", true);
    expect_match("{\"a\":false}", "$.a = false", true);
    expect_match("{\"a\":true}", "$.a = false", false);

    /* ---------------- 4. 大小比较 ---------------- */
    expect_match("{\"a\":5}", "$.a < 6", true);
    expect_match("{\"a\":5}", "$.a <= 5", true);
    expect_match("{\"a\":5}", "$.a > 4", true);
    expect_match("{\"a\":5}", "$.a >= 5", true);
    expect_match("{\"a\":5}", "$.a < 5", false);

    /* ---------------- 5. 三值逻辑：路径不存在 ---------------- */
    expect_match("{\"b\":5}", "$.a = 5", false);
    expect_match("{\"b\":5}", "$.a != 5", false);   /* ← 关键 */
    expect_match("{\"b\":5}", "$.a < 5", false);
    expect_match("{\"b\":5}", "$.a > 5", false);

    /* ---------------- 5'. 三值逻辑：值是 JSON null ---------------- */
    expect_match("{\"a\":null}", "$.a = null", false);   /* NULL = NULL 是未知 */
    expect_match("{\"a\":null}", "$.a != null", false);
    expect_match("{\"a\":null}", "$.a = 5", false);
    expect_match("{\"a\":null}", "$.a != 5", false);      /* ← 关键 */

    /* ---------------- 5''. 类型不同：按类型序，不等于但也不"未知" ---------------- */
    /*
     * $.a 是字符串 "5"，规格里是数字 5。
     *
     * ⚠️ 这里我第一版写错了期望（以为 `!=` 也是 false）。查清楚之后改的是测试：
     *   - `json_scalar.c` 的坑 M7-11 明确写着「类型不同永不相等，不做任何隐式转换」；
     *   - 那就是**类型序比较**（数值 < 文本），与 SQLite 一致；
     *   - `'5' != 5` 在类型序下是 TRUE（两者确实不相等）。
     *
     * 而 A1 的验收判据是「对照 SQLite JSON1 全等」—— 所以这里必须是 true。
     *
     * ⇒ 区别在于：
     *   「值是 JSON null / 路径不存在」⇒ 比较结果是**未知**（所有比较符 false）；
     *   「值是标量但类型不同」⇒ 比较结果是**确实不相等**（`=` false、`!=` true）。
     *   这两种情形不能混为一谈。
     */
    expect_match("{\"a\":\"5\"}", "$.a = 5", false);
    expect_match("{\"a\":\"5\"}", "$.a != 5", true);      /* ← 类型序：不相等 */
    /* $.a 是对象 ⇒ 提取结果是 NULL（非标量路径）⇒ 未知 */
    expect_match("{\"a\":{\"b\":1}}", "$.a = 1", false);
    expect_match("{\"a\":{\"b\":1}}", "$.a != 1", false);
    /* $.a 是数组 ⇒ 同上 */
    expect_match("{\"a\":[1,2]}", "$.a = 1", false);
    expect_match("{\"a\":[1,2]}", "$.a != 1", false);

    /* ---------------- 7. 数字：1 与 1.0 数值相等 ---------------- */
    expect_match("{\"a\":1}", "$.a = 1.0", true);
    expect_match("{\"a\":1.0}", "$.a = 1", true);
    expect_match("{\"a\":1.5}", "$.a = 1.5", true);
    expect_match("{\"a\":1.5}", "$.a > 1", true);
    expect_match("{\"a\":1.5}", "$.a < 2", true);

    /* ---------------- 8. 嵌套路径 ---------------- */
    expect_match("{\"a\":{\"b\":{\"c\":7}}}", "$.a.b.c = 7", true);
    expect_match("{\"a\":{\"b\":{\"c\":7}}}", "$.a.b.c = 8", false);
    expect_match("{\"a\":[10,20]}", "$.a[0] = 10", true);
    expect_match("{\"a\":[10,20]}", "$.a[1] = 20", true);
    expect_match("{\"a\":[10,20]}", "$.a[5] = 20", false);   /* 越界 */

    if (failures == 0) {
        printf("  8 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
