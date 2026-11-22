/*
 * test_json_func.c —— 文档模态向值层注册的两个函数（v1.6 C5.7）
 *
 * 6 项：
 *   1. `db_json_init()` 把 `json_match` / `json_extract` 注册进值层函数表
 *   2. `json_extract` 取标量：字符串 / 整数 / 小数 / 布尔 / null / 路径不存在
 *   3. `json_match` 在真文档上求值：命中与不命中
 *   4. **外部 JSONB（256..2048B 借用记录缓冲）也能取路径** —— 实参是外部值
 *   5. 未注册的模态不会假装成功（`db_json_init` 前查表应为 NULL）
 *   6. 规格解析失败 ⇒ `json_match` 返回错误，不是静默 false
 *
 * 【为什么用值层函数表查，而不是走 SQL】
 * 走 SQL 需要"把 JSON 文本插进 JSON 列"的写入路径，那是 C5.3b 的范围
 * （JSON 文本 → JSONB 的编码发生在写入侧）。本步只验证
 * 「模态把函数注册进去 + 函数本身算得对」这两件事。
 *
 * 【第 4 项是核心】
 * 执行器拿到的文档既可能是内联值，也可能是外部值（256..2048B 的借用）。
 * 如果函数只处理内联，那大一点的文档就会静默查不到路径 ——
 * 症状是"小文档能查、大文档查不到"，极难定位。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/doc/json_func.h"
#include "modalities/doc/json_parse.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 把文本 JSON 编成 JSONB，并造成一个内联 DbValue（≤255B） */
static int make_inline_doc(const char *text, DbValue *out, u8 *buf, u32 cap)
{
    u32 len = 0;
    if (json_parse_to_jsonb(text, (u32)strlen(text), buf, cap, &len) != OK)
        return E_INVAL;
    if (len > VAL_INLINE_MAX)
        return E_NOSPACE;

    val_null(out);
    out->is_null = false;
    out->type = T_JSON;
    out->len = (u16)len;
    if (len)
        memcpy(out->s, buf, len);
    return OK;
}

/* 造成一个外部 JSONB 值（句柄指向 buf） */
static void make_ext_doc(const u8 *buf, u32 len, DbValue *out)
{
    val_external_json(out, (u64)(uintptr_t)buf, len, val_mem_detoast, (void *)buf);
}

int main(void)
{
    printf("test_json_func:\n");

    /* ---------------- 5. 注册前：查不到 ---------------- */
    CHECK(val_func_find("json_match") == NULL);
    CHECK(val_func_find("json_extract") == NULL);

    /* ---------------- 1. 注册后：查得到 ---------------- */
    CHECK(db_json_init() == OK);
    CHECK(val_func_find("json_match") != NULL);
    CHECK(val_func_find("json_extract") != NULL);
    /* 重复注册要能容忍（多连接场景会重复调 db_open） */
    CHECK(db_json_init() == OK);

    /* 渲染器也在同一个入口注册（C5.3 的钩子） */
    CHECK(val_json_renderer_is(NULL) == true);

    u8 jbuf[4096];

    /* ---------------- 2. json_extract 取标量 ---------------- */
    {
        DbValue doc, args[2], out;
        char path[] = "$.a";

        CHECK(make_inline_doc("{\"a\":42}", &doc, jbuf, sizeof(jbuf)) == OK);
        val_text(&args[1], path, (u16)strlen(path));
        args[0] = doc;

        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(!out.is_null);
        CHECK(out.i == 42);

        /* 字符串 */
        CHECK(make_inline_doc("{\"a\":\"hi\"}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(out.type == T_TEXT);
        CHECK(out.len == 2);
        CHECK(memcmp(out.s, "hi", 2) == 0);

        /* 小数 */
        CHECK(make_inline_doc("{\"a\":1.5}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(out.type == T_REAL);
        CHECK(out.d > 1.49 && out.d < 1.51);

        /* 布尔 → INT 1 */
        CHECK(make_inline_doc("{\"a\":true}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(out.i == 1);

        /* JSON null → DbValue(NULL)（不是错误） */
        CHECK(make_inline_doc("{\"a\":null}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(out.is_null);

        /* 路径不存在 → DbValue(NULL)，也是 OK */
        CHECK(make_inline_doc("{\"b\":1}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(out.is_null);
    }

    /* ---------------- 3. json_match 求值 ---------------- */
    {
        DbValue doc, args[2], out;

        CHECK(make_inline_doc("{\"a\":42}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;

        val_text(&args[1], "$.a = 42", 9);
        CHECK(val_func_find("json_match")(args, 2, &out) == OK);
        CHECK(out.i == 1);

        val_text(&args[1], "$.a = 43", 9);
        CHECK(val_func_find("json_match")(args, 2, &out) == OK);
        CHECK(out.i == 0);

        /* 三值逻辑：路径不存在 ⇒ false（含 !=） */
        CHECK(make_inline_doc("{\"b\":1}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        val_text(&args[1], "$.a != 42", 9);
        CHECK(val_func_find("json_match")(args, 2, &out) == OK);
        CHECK(out.i == 0);
    }

    /* ---------------- 4. 外部 JSONB（256..2048B）也能取路径 ---------------- */
    {
        /*
         * 造一个 >255 字节的文档：填一个大字符串撑长度。
         * 这样 json_extract 的实参必须是外部值路径，覆盖 detoast 分支。
         */
        char text[2048];
        char *p = text;
        p += sprintf(p, "{\"pad\":\"");
        for (int i = 0; i < 600; i++)
            p += sprintf(p, "x");
        sprintf(p, "\",\"a\":7}");

        u32 blen = 0;
        CHECK(json_parse_to_jsonb(text, (u32)strlen(text),
                                  jbuf, sizeof(jbuf), &blen) == OK);
        CHECK(blen > VAL_INLINE_MAX);

        DbValue doc, args[2], out;
        make_ext_doc(jbuf, blen, &doc);

        char path[] = "$.a";
        val_text(&args[1], path, (u16)strlen(path));
        args[0] = doc;

        CHECK(val_func_find("json_extract")(args, 2, &out) == OK);
        CHECK(!out.is_null);
        CHECK(out.i == 7);
    }

    /* ---------------- 6. 规格解析失败 ⇒ 错误，不是静默 false ---------------- */
    {
        DbValue doc, args[2], out;

        CHECK(make_inline_doc("{\"a\":1}", &doc, jbuf, sizeof(jbuf)) == OK);
        args[0] = doc;
        val_text(&args[1], "not-a-spec", 10);
        CHECK(val_func_find("json_match")(args, 2, &out) != OK);

        /* 实参个数不对也是错误 */
        CHECK(val_func_find("json_match")(args, 1, &out) != OK);
        CHECK(val_func_find("json_extract")(args, 1, &out) != OK);
    }

    if (failures == 0) {
        printf("  6 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
