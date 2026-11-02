/*
 * test_record.c — tuple 编解码验收
 *
 * 重点：混合类型布局、NULL bitmap、变长偏移链、与 page 的集成。
 */

#include <stdio.h>
#include <string.h>

#include "storage/page.h"
#include "storage/record.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static Value v_i4(i32 x)      { Value v; memset(&v, 0, sizeof v); v.u.i4 = x; return v; }
static Value v_i8(i64 x)      { Value v; memset(&v, 0, sizeof v); v.u.i8 = x; return v; }
static Value v_bool(bool b)   { Value v; memset(&v, 0, sizeof v); v.u.b = b; return v; }
static Value v_null(void)     { Value v; memset(&v, 0, sizeof v); v.is_null = true; return v; }

static Value v_text(const char *s)
{
    Value v;
    memset(&v, 0, sizeof v);
    v.u.text.p = (const u8 *)s;
    v.u.text.n = (u16)strlen(s);
    return v;
}

static bool text_eq(const Value *v, const char *s)
{
    size_t n = strlen(s);
    if (v->is_null || v->u.text.n != n)
        return false;
    return memcmp(v->u.text.p, s, n) == 0;
}

/* ---- 1) 单列 INT4 ---- */
static void t1_single(void)
{
    ColDesc cols[] = { { "id", T_INT4, false } };
    u8 buf[128];
    u16 len, fixed;

    CHECK(rec_fixed_size(cols, 1, &fixed) == OK);
    CHECK(fixed == REC_HDR_SIZE + 1 + 4);        /* header + bitmap + int4 = 25 */

    Value in[1] = { v_i4(-12345) };
    CHECK(rec_encode(cols, 1, in, buf, sizeof buf, &len) == OK);
    CHECK(len == fixed);                          /* 无变长数据 */

    Value out[1];
    CHECK(rec_decode(cols, 1, buf, len, out) == OK);
    CHECK(!out[0].is_null);
    CHECK(out[0].u.i4 == -12345);
}

/* ---- 2) 混合四列：布局与往返 ---- */
static void t2_mixed(void)
{
    ColDesc cols[] = {
        { "id",   T_INT4, false },
        { "name", T_TEXT, true  },
        { "big",  T_INT8, false },
        { "bio",  T_TEXT, true  },
    };
    u8 buf[256];
    u16 len, fixed;

    /* header 20 + bitmap 1 + (4+8) 定长 + 2 个变长偏移 4 = 37 */
    CHECK(rec_fixed_size(cols, 4, &fixed) == OK);
    CHECK(fixed == 37);

    Value in[4] = {
        v_i4(42), v_text("alice"), v_i8(-9000000000LL), v_text("hello world"),
    };
    CHECK(rec_encode(cols, 4, in, buf, sizeof buf, &len) == OK);
    CHECK(len == 37 + 5 + 11);

    Value out[4];
    CHECK(rec_decode(cols, 4, buf, len, out) == OK);
    CHECK(out[0].u.i4 == 42);
    CHECK(text_eq(&out[1], "alice"));
    CHECK(out[2].u.i8 == -9000000000LL);
    CHECK(text_eq(&out[3], "hello world"));
}

/* ---- 3) NULL：中间的变长列为 NULL，偏移链不能断 ---- */
static void t3_null(void)
{
    ColDesc cols[] = {
        { "a", T_TEXT, true },
        { "b", T_TEXT, true },
        { "c", T_TEXT, true },
    };
    u8 buf[128];
    u16 len;

    /* 只填 a 和 c，b 为 NULL —— 解码 c 时必须靠"下一个偏移"或总长，
     * 如果 b 的偏移没写，c 会被算成空串 */
    Value in[3] = { v_text("AAA"), v_null(), v_text("CCCCC") };
    CHECK(rec_encode(cols, 3, in, buf, sizeof buf, &len) == OK);
    CHECK(len == (u16)(REC_HDR_SIZE + 1 + 6) + 3 + 5);

    Value out[3];
    CHECK(rec_decode(cols, 3, buf, len, out) == OK);
    CHECK(text_eq(&out[0], "AAA"));
    CHECK(out[1].is_null);
    CHECK(text_eq(&out[2], "CCCCC"));
}

/* ---- 4) 非 nullable 列传 NULL 必须拒绝 ---- */
static void t4_notnull(void)
{
    ColDesc cols[] = { { "id", T_INT4, false } };
    u8 buf[64];
    u16 len;

    Value in[1] = { v_null() };
    CHECK(rec_encode(cols, 1, in, buf, sizeof buf, &len) == E_INVAL);
}

/* ---- 5) 空字符串 vs NULL 必须可区分 ---- */
static void t5_empty_vs_null(void)
{
    ColDesc cols[] = { { "a", T_TEXT, true }, { "b", T_TEXT, true } };
    u8 buf[64];
    u16 len;

    Value in[2] = { v_text(""), v_null() };
    CHECK(rec_encode(cols, 2, in, buf, sizeof buf, &len) == OK);

    Value out[2];
    CHECK(rec_decode(cols, 2, buf, len, out) == OK);
    CHECK(!out[0].is_null);
    CHECK(out[0].u.text.n == 0);
    CHECK(out[1].is_null);
}

/* ---- 6) 与 page 集成：存进去再读出来 ---- */
static void t6_with_page(void)
{
    static u8 pg[PAGE_SIZE];
    ColDesc cols[] = {
        { "id",   T_INT4, false },
        { "name", T_TEXT, true  },
        { "ok",   T_BOOL, false },
    };
    u8 buf[256];
    u16 len, slot;

    CHECK(page_init(pg, PT_HEAP) == OK);

    Value in[3] = { v_i4(7), v_text("张三"), v_bool(true) };
    CHECK(rec_encode(cols, 3, in, buf, sizeof buf, &len) == OK);
    CHECK(page_insert(pg, buf, len, &slot) == OK);

    const u8 *got;
    u16 got_len;
    CHECK(page_get(pg, slot, &got, &got_len) == OK);
    CHECK(got_len == len);

    Value out[3];
    CHECK(rec_decode(cols, 3, got, got_len, out) == OK);
    CHECK(out[0].u.i4 == 7);
    CHECK(text_eq(&out[1], "张三"));
    CHECK(out[2].u.b == true);
}

/* ---- 7) 损坏检测：长度不足 / 偏移越界 ---- */
static void t7_corrupt(void)
{
    ColDesc cols[] = { { "a", T_TEXT, true } };
    u8 buf[64];
    u16 len;

    Value in[1] = { v_text("xyz") };
    CHECK(rec_encode(cols, 1, in, buf, sizeof buf, &len) == OK);

    Value out[1];
    CHECK(rec_decode(cols, 1, buf, (u16)(len - 1), out) == OK);  /* 少 1 字节：内容变短但仍合法 */
    CHECK(rec_decode(cols, 1, buf, 10, out) == E_CORRUPT);       /* 比固定开销还短 */

    /* 人为把偏移改到页外 */
    put_u16(buf + REC_HDR_SIZE + 1, 60000);
    CHECK(rec_decode(cols, 1, buf, len, out) == E_CORRUPT);
}

/* ---- 8) 头部字段 ---- */
static void t8_header(void)
{
    ColDesc cols[] = { { "id", T_INT4, false } };
    u8 buf[64];
    u16 len;

    Value in[1] = { v_i4(1) };
    CHECK(rec_encode(cols, 1, in, buf, sizeof buf, &len) == OK);

    CHECK(rec_xmin(buf) == 0);
    CHECK(rec_xmax(buf) == 0);

    rec_set_xmin(buf, 1001);
    rec_set_xmax(buf, 1002);
    rec_set_flags(buf, REC_F_COMMITTED);
    CHECK(rec_xmin(buf) == 1001);
    CHECK(rec_xmax(buf) == 1002);
    CHECK(rec_flags(buf) == REC_F_COMMITTED);

    /* 改头部不影响列值 */
    Value out[1];
    CHECK(rec_decode(cols, 1, buf, len, out) == OK);
    CHECK(out[0].u.i4 == 1);
}

/* ---- 9) 单列投影 ---- */
static void t9_get_col(void)
{
    ColDesc cols[] = {
        { "id",  T_INT4, false },
        { "s",   T_TEXT, true  },
        { "big", T_INT8, false },
    };
    u8 buf[256];
    u16 len;

    Value in[3] = { v_i4(9), v_text("proj"), v_i8(77) };
    CHECK(rec_encode(cols, 3, in, buf, sizeof buf, &len) == OK);

    Value v;
    CHECK(rec_get_col(cols, 3, buf, len, 1, &v) == OK);
    CHECK(text_eq(&v, "proj"));
    CHECK(rec_get_col(cols, 3, buf, len, 2, &v) == OK);
    CHECK(v.u.i8 == 77);
    CHECK(rec_get_col(cols, 3, buf, len, 3, &v) == E_INVAL);
}

/* ---- 10) 全 NULL 行 ---- */
static void t10_all_null(void)
{
    ColDesc cols[] = { { "a", T_TEXT, true }, { "b", T_INT4, true } };
    u8 buf[64];
    u16 len;

    Value in[2] = { v_null(), v_null() };
    CHECK(rec_encode(cols, 2, in, buf, sizeof buf, &len) == OK);

    Value out[2];
    CHECK(rec_decode(cols, 2, buf, len, out) == OK);
    CHECK(out[0].is_null);
    CHECK(out[1].is_null);
}

int main(void)
{
    printf("test_record:\n");
    t1_single();
    t2_mixed();
    t3_null();
    t4_notnull();
    t5_empty_vs_null();
    t6_with_page();
    t7_corrupt();
    t8_header();
    t9_get_col();
    t10_all_null();

    if (failures == 0) {
        printf("  10 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
