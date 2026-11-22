/*
 * test_value_json.c —— `T_JSON` 在值层 / 记录层往返（v1.6 C5.3）
 *
 * 这不是 JSON parser 的测试（parser 已由 test_jsonb.c 覆盖），
 * 测的是**值层契约**：给定一段 JSONB 字节，能不能按 T_JSON 原样存进 heap 行、
 * 再原样读回来；读出来的 DbValue 是否仍然分得清这是 T_JSON 而不是 T_TEXT。
 *
 * 三个边界（按用户裁决）：
 *   ≤ 255B       → 记录内联；DbValue 也内联（T_JSON）
 *   256..2048B   → 记录内联；DbValue 是指向本次 tuple buffer 的外部值
 *   > 2048B      → 属于 C5.3b，本 Task 明确不测 / 不实现
 *
 * 若 record.c / value.c 把 T_JSON 当 T_TEXT 编解码，或把 256..2048B
 * 截断塞进 `DbValue.s[256]`，这里的字节级往返会失败（RED）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 造一个内联 T_JSON 值（≤255 字节，直接放 s[]） */
static void make_inline_json(DbValue *v, const u8 *bytes, u16 len)
{
    memset(v, 0, sizeof(*v));
    v->is_null = false;
    v->type = T_JSON;
    v->len = len;
    if (len)
        memcpy(v->s, bytes, len);
}

/* 造一个外部 T_JSON 值（256..2048 字节，句柄指向给定的内存块） */
static void make_ext_json(DbValue *v, const u8 *bytes, u16 len)
{
    val_external_json(v, (u64)(uintptr_t)bytes, len, val_mem_detoast, (void *)bytes);
}

/* 假 JSONB 渲染器：输出 "<rendered:N>"，只用来证明值层会调注册进来的实现 */
static int fake_json_renderer(const u8 *buf, u32 len, char *out, u32 cap,
                              u32 *out_len)
{
    (void)buf;
    char tmp[32];
    int n = snprintf(tmp, sizeof(tmp), "<rendered:%u>", len);
    if (n <= 0)
        return E_INVAL;
    if (!out) {
        *out_len = (u32)n;
        return E_NOSPACE;
    }
    if ((u32)n > cap)
        return E_NOSPACE;
    memcpy(out, tmp, (size_t)n);
    *out_len = (u32)n;
    return OK;
}

int main(void)
{
    printf("test_value_json:\n");

    ColDesc cols[1] = {{"doc", T_JSON, true}};
    RecLayout layout;
    CHECK(rec_layout(cols, 1, &layout) == OK);

    /* ---------------- 1. 小 JSON：≤255B，record 与 DbValue 都内联 ---------------- */
    {
        const u8 jsonb[] = { 0x08, 0x01, 0x02, 0x03, 0x04 };
        DbValue in[1], out[1];
        make_inline_json(&in[0], jsonb, (u16)sizeof(jsonb));
        memset(out, 0, sizeof(out));

        u8 rec[256];
        u16 rec_len = 0;
        CHECK(val_to_record(cols, 1, in, rec, sizeof(rec), &rec_len) == OK);
        CHECK(rec_len > 0);
        CHECK(val_from_record_layout(&layout, cols, 1, rec, rec_len, out) == OK);
        CHECK(!out[0].is_null);
        CHECK(out[0].type == T_JSON);
        CHECK(!out[0].external);
        CHECK(out[0].len == sizeof(jsonb));
        CHECK(memcmp(out[0].s, jsonb, sizeof(jsonb)) == 0);
    }

    /* ---------------- 2. 边界：255B，必须仍可内联 ---------------- */
    {
        u8 jsonb[255];
        for (u32 i = 0; i < sizeof(jsonb); i++)
            jsonb[i] = (u8)(i % 251u);

        DbValue in[1], out[1];
        make_inline_json(&in[0], jsonb, (u16)sizeof(jsonb));
        memset(out, 0, sizeof(out));

        u8 rec[512];
        u16 rec_len = 0;
        CHECK(val_to_record(cols, 1, in, rec, sizeof(rec), &rec_len) == OK);
        CHECK(val_from_record_layout(&layout, cols, 1, rec, rec_len, out) == OK);
        CHECK(out[0].type == T_JSON);
        CHECK(!out[0].external);
        CHECK(out[0].len == sizeof(jsonb));
        CHECK(memcmp(out[0].s, jsonb, sizeof(jsonb)) == 0);
    }

    /* ---------------- 3. 中 JSON：256B，记录内联、DbValue 外部借用 ---------------- */
    {
        u8 jsonb[256];
        for (u32 i = 0; i < sizeof(jsonb); i++)
            jsonb[i] = (u8)((i * 17u) % 251u);

        DbValue in[1], out[1];
        make_ext_json(&in[0], jsonb, (u16)sizeof(jsonb));
        memset(out, 0, sizeof(out));

        u8 rec[512];
        u16 rec_len = 0;
        CHECK(val_to_record(cols, 1, in, rec, sizeof(rec), &rec_len) == OK);
        CHECK(rec_len > 256);
        CHECK(val_from_record_layout(&layout, cols, 1, rec, rec_len, out) == OK);
        CHECK(out[0].type == T_JSON);
        CHECK(out[0].external);

        u8 *copy = NULL;
        u32 copy_len = 0;
        CHECK(val_detoast(&out[0], &copy, &copy_len) == OK);
        CHECK(copy_len == sizeof(jsonb));
        CHECK(memcmp(copy, jsonb, sizeof(jsonb)) == 0);
        free(copy);
    }

    /* ---------------- 4. 既有 TEXT 列行为不变 ---------------- */
    {
        ColDesc text_cols[1] = {{"txt", T_TEXT, true}};
        RecLayout text_layout;
        CHECK(rec_layout(text_cols, 1, &text_layout) == OK);

        DbValue in[1], out[1];
        val_text(&in[0], "hello", 5);
        memset(out, 0, sizeof(out));

        u8 rec[256];
        u16 rec_len = 0;
        CHECK(val_to_record(text_cols, 1, in, rec, sizeof(rec), &rec_len) == OK);
        CHECK(val_from_record_layout(&text_layout, text_cols, 1,
                                     rec, rec_len, out) == OK);
        CHECK(out[0].type == T_TEXT);
        CHECK(!out[0].external);
        CHECK(out[0].len == 5);
        CHECK(memcmp(out[0].s, "hello", 5) == 0);
    }

    /* ---------------- 5. 没有渲染器时不许吐原始字节 ---------------- */
    {
        /*
         * 值层（L1）不许 include 文档模态（L4）。所以 "JSONB → JSON 文本"
         * 这套渲染能力必须**由模态自己注册**进来，值层只持一个函数指针。
         * 没有注册时，val_to_str 必须给明确标记，不能把二进制当文本输出
         * （Rid 里的二进制会在内嵌 NUL 处截断，看起来像合法 JSON）。
         */
        DbValue v;
        make_inline_json(&v, (const u8 *)"\x08\x01", 2);
        char buf[64];
        int n = val_to_str(&v, buf, (int)sizeof(buf));
        CHECK(n > 0);
        CHECK(strstr(buf, "JSONB") != NULL);
    }

    /* ---------------- 6. 注册渲染器之后能出合法 JSON 文本 ---------------- */
    {
        DbValue v;
        char buf[64];
        int n;

        /* 假渲染器：只证明"值层会用注册进来的实现" */
        CHECK(val_json_renderer_is(NULL) == false);  /* 此刻还没有任何渲染器 */
        CHECK(val_set_json_renderer(fake_json_renderer) == OK);
        CHECK(val_json_renderer_is(fake_json_renderer) == true);

        make_inline_json(&v, (const u8 *)"\x08\x01", 2);
        n = val_to_str(&v, buf, (int)sizeof(buf));
        CHECK(n > 0);
        CHECK(v.type == T_JSON);
        CHECK(strcmp(buf, "<rendered:2>") == 0);

        /* 注册后再注册 ⇒ E_INVAL（不静默覆盖） */
        CHECK(val_set_json_renderer(fake_json_renderer) == E_INVAL);
    }

    if (failures == 0) {
        printf("  T_JSON 值层 / 记录层 6 组验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
