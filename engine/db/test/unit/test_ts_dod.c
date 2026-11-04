/*
 * test_ts_dod.c —— delta-of-delta 编码的单测（v1.5 步骤 1 · A1）
 *
 * 编码格式见 src/modalities/ts/dod.h，位流基元见 src/modalities/ts/tsbits.h。
 *
 * ==================== 这一步要证明什么 ====================
 *
 * 1. **往返全等**：任意输入都能**逐位**还原（不是"差不多"，是每一位都一样）。
 * 2. **坏数据必须报错**：截短、装不下、乱码 ⇒ 明确错误，
 *    绝不"读不出来就填 0"（那样会把坏数据变成看着正常的数据）。
 * 3. **压缩比**：恒定间隔的时间列应该压到**一个 bit 一个点**。
 *
 * ==================== 为什么"逐位全等"是唯一站得住的判据 ====================
 *
 * 时间列的编码最危险的失败模式是**静默地错**：
 * 少解一个点、delta 算错一次，序列就整体漂移了 —— 而时间戳漂移
 * 在监控场景里往往**看不出来**（曲线还是"像模像样"）。
 * 所以这里每一条都用 == 逐位比，不用容差。
 *
 * ==================== 测试组 ====================
 *
 *   1) 基本往返：长度 0/1/2/3/7/100/1000，随机时间戳
 *   2) 恒定间隔（最常见场景）：往返 + 压缩比应接近 1 bit/点
 *   3) 乱序（delta 为负）：往返全等
 *   4) 极值（i64 边界附近）：往返全等，**不许有溢出**（有符号溢出是 UB）
 *   5) 重复值：往返全等
 *   6) fuzz：固定种子 LCG，≥ 2000 轮随机数组
 *   7) 截断：砍掉 1 字节（以及每一个更短的长度）⇒ 必须 E_CORRUPT
 *   8) 容量不足：cap 给小 ⇒ E_NOSPACE
 *   9) 上界：dod_encoded_bound(n) ≥ 实际编码长度
 *  10) 乱码不崩：随机字节 ⇒ 不崩、要么报错要么解出条数
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <stdint.h>

#include "modalities/ts/dod.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define MAX_N 1200

/* ---------------- 随机数（自己写，不用 rand()） ---------------- */

/*
 * xorshift64：固定种子 ⇒ 结果可复现。
 *
 * 为什么不用 rand()：rand() 的实现与种子在不同 libc 上不一样，
 * 同一份测试在不同机器上会跑出不同的随机序列 ——
 * "随机"测试一旦不可复现，失败就没法复盘。
 */
static u64 rng_state = 0x2545F4914F6CDD1DULL;

static u64 rng_next(void)
{
    rng_state ^= rng_state << 13;
    rng_state ^= rng_state >> 7;
    rng_state ^= rng_state << 17;
    return rng_state;
}

/* 取一个任意 i64（含负数与极值） */
static i64 rng_i64(void)
{
    u64 v = rng_next();

    /* 一半的次数落在"小范围"（更接近真实时间戳），一半是全 64 位 */
    if (rng_next() & 1u)
        v &= 0xFFFFULL;
    return (i64)v;
}

/* ---------------- 往返助手 ---------------- */

/*
 * 编码 → 解码 → 逐位比对。
 *
 * 返回编码后的字节数（失败返回 -1）。
 */
static long roundtrip(const i64 *ts, u32 n, const char *tag)
{
    u32 cap = dod_encoded_bound(n);
    u8 *buf = (u8 *)malloc(cap > 0 ? cap : 1);
    i64 *back = (i64 *)malloc(sizeof(i64) * (n > 0 ? n : 1));
    long bytes;
    int enc;
    int dec;
    u32 i;

    if (!buf || !back) {
        printf("    FAIL %s：内存不够\n", tag);
        failures++;
        free(buf);
        free(back);
        return -1;
    }

    enc = dod_encode(ts, n, buf, cap);
    if (enc < 0) {
        printf("    FAIL %s：编码失败（%d），上界 %u 不够\n", tag, enc, cap);
        failures++;
        free(buf);
        free(back);
        return -1;
    }
    bytes = enc;

    /* 上界必须够用 */
    if ((u32)bytes > dod_encoded_bound(n)) {
        printf("    FAIL %s：编码长度 %ld 超过上界 %u\n", tag, bytes,
               dod_encoded_bound(n));
        failures++;
    }

    dec = dod_decode(buf, (u32)bytes, back, n);
    if (dec != (int)n) {
        printf("    FAIL %s：解码返回 %d（应为 %u）\n", tag, dec, n);
        failures++;
        free(buf);
        free(back);
        return -1;
    }

    for (i = 0; i < n; i++) {
        if (back[i] != ts[i]) {
            printf("    FAIL %s：第 %u 个不一样（原 %lld，解 %lld）\n",
                   tag, i, (long long)ts[i], (long long)back[i]);
            failures++;
            break;
        }
    }

    free(buf);
    free(back);
    return bytes;
}

/* ---------------- 1) 基本往返 ---------------- */

static void t1_basic(void)
{
    i64 ts[MAX_N];
    u32 lens[] = { 0, 1, 2, 3, 7, 100, 1000 };
    u32 k;

    printf("  1) 基本往返：长度 0/1/2/3/7/100/1000，随机时间戳\n");

    for (k = 0; k < sizeof(lens) / sizeof(lens[0]); k++) {
        u32 n = lens[k];
        u32 i;
        char tag[64];

        for (i = 0; i < n; i++)
            ts[i] = rng_i64();
        snprintf(tag, sizeof(tag), "长度 %u", n);
        (void)roundtrip(ts, n, tag);
    }

    /* 空数组：编码 0 字节，解码 0 条 */
    {
        u8 buf[8];
        i64 out[1];

        memset(buf, 0xFF, sizeof(buf));
        CHECK(dod_encode(ts, 0, buf, sizeof(buf)) == 0);
        CHECK(dod_decode(buf, sizeof(buf), out, 0) == 0);
    }

    /* 参数非法 */
    CHECK(dod_encode(NULL, 3, (u8 *)lens, 16) == -E_INVAL);
    CHECK(dod_encode(ts, 3, NULL, 16) == -E_INVAL);
    CHECK(dod_decode(NULL, 8, ts, 3) == -E_INVAL);
}

/* ---------------- 2) 恒定间隔（最常见场景） ---------------- */

static void t2_constant_step(void)
{
    i64 ts[MAX_N];
    u32 n = 1000;
    u32 i;
    long bytes;

    printf("  2) 恒定间隔（每秒一个点）：往返全等 + 压缩比接近 1 bit/点\n");

    for (i = 0; i < n; i++)
        ts[i] = 1700000000000LL + (i64)i * 1000LL;

    bytes = roundtrip(ts, n, "恒定间隔");
    if (bytes > 0) {
        double per = (double)bytes / (double)n;

        printf("    %u 个点：%ld 字节 ⇒ %.4f 字节/点（%.2f bit/点）\n",
               n, bytes, per, per * 8.0);
        /*
         * 恒定间隔 ⇒ 每个 dod 都是 0 ⇒ 每点 1 bit。
         * 头尾两个 varint 约 6~10 字节，所以 1000 点的总量应是
         * "1000/8 + 十几" 字节 —— 明显小于"一个点 8 字节"（8000 字节）。
         */
        CHECK(bytes < 200);
    }
}

/* ---------------- 3) 乱序（delta 为负） ---------------- */

static void t3_unordered(void)
{
    i64 ts[64];
    u32 i;
    u32 j;

    printf("  3) 乱序（delta 为负）：往返全等\n");

    for (i = 0; i < 64; i++)
        ts[i] = (i64)i * 1000LL;

    /* 随机交换若干次 ⇒ 制造出负的 delta */
    for (i = 0; i < 200; i++) {
        j = (u32)(rng_next() % 64u);
        {
            i64 tmp = ts[i % 64];

            ts[i % 64] = ts[j];
            ts[j] = tmp;
        }
    }

    (void)roundtrip(ts, 64, "乱序");

    /* 完全逆序：delta 恒为负 */
    for (i = 0; i < 64; i++)
        ts[i] = (i64)(63 - i) * 1000LL;
    (void)roundtrip(ts, 64, "逆序");
}

/* ---------------- 4) 极值（i64 边界） ---------------- */

static void t4_extremes(void)
{
    i64 ts[16];
    u32 n = 0;

    printf("  4) 极值（i64 边界附近）：往返全等，不许溢出\n");

    ts[n++] = INT64_MIN;
    ts[n++] = INT64_MAX;
    ts[n++] = 0;
    ts[n++] = -1;
    ts[n++] = 1;
    ts[n++] = INT64_MIN + 1;
    ts[n++] = INT64_MAX - 1;
    ts[n++] = INT64_MIN / 2;
    ts[n++] = INT64_MAX / 2;
    (void)roundtrip(ts, n, "极值混合");

    /* 两端直接相邻：差值是 2^64-1，根本不是 i64 */
    ts[0] = INT64_MIN;
    ts[1] = INT64_MAX;
    (void)roundtrip(ts, 2, "MIN→MAX");

    /* 恒定极值 */
    for (n = 0; n < 8; n++)
        ts[n] = INT64_MIN;
    (void)roundtrip(ts, 8, "恒定 INT64_MIN");

    for (n = 0; n < 8; n++)
        ts[n] = INT64_MAX;
    (void)roundtrip(ts, 8, "恒定 INT64_MAX");
}

/* ---------------- 5) 重复值 ---------------- */

static void t5_duplicates(void)
{
    i64 ts[128];
    u32 i;

    printf("  5) 重复值：往返全等\n");

    for (i = 0; i < 128; i++)
        ts[i] = 42;                     /* 全一样 ⇒ dod 恒为 0 */
    (void)roundtrip(ts, 128, "全重复");

    for (i = 0; i < 128; i++)
        ts[i] = (i64)(i / 4) * 10;       /* 每四个一个值 */
    (void)roundtrip(ts, 128, "分段重复");
}

/* ---------------- 6) fuzz ---------------- */

static void t6_fuzz(void)
{
    i64 ts[MAX_N];
    int round;

    printf("  6) fuzz：固定种子 LCG，2000 轮随机数组\n");

    for (round = 0; round < 2000; round++) {
        u32 n = (u32)(rng_next() % 65u);     /* 0..64 */
        u32 i;

        for (i = 0; i < n; i++) {
            if ((round & 3) == 0) {
                /* 四分之一是"接近恒定间隔 + 抖动"（贴近真实数据） */
                ts[i] = 1700000000000LL + (i64)i * 1000LL
                        + (i64)(rng_next() % 3u) - 1;
            } else {
                ts[i] = rng_i64();
            }
        }
        if (roundtrip(ts, n, "fuzz") < 0) {
            printf("    FAIL fuzz 第 %d 轮失败（长度 %u）\n", round, n);
            break;
        }
    }
}

/* ---------------- 7) 截断 ⇒ 必须报错 ---------------- */

static void t7_truncated(void)
{
    i64 ts[100];
    u8 *buf;
    u32 cap;
    long bytes;
    u32 cut;
    u32 i;

    printf("  7) 截断：砍掉每一个字节都必须报 E_CORRUPT（不许看起来对）\n");

    for (i = 0; i < 100; i++)
        ts[i] = 1700000000000LL + (i64)i * 1000LL;

    cap = dod_encoded_bound(100);
    buf = (u8 *)malloc(cap);
    if (!buf) {
        printf("    FAIL 内存不够\n");
        failures++;
        return;
    }

    bytes = dod_encode(ts, 100, buf, cap);
    CHECK(bytes > 0);
    if (bytes <= 0) {
        free(buf);
        return;
    }

    for (cut = 0; cut < (u32)bytes; cut++) {
        i64 back[100];
        int dec = dod_decode(buf, cut, back, 100);

        if (dec != -E_CORRUPT) {
            printf("    FAIL 截短到 %u 字节时返回 %d（应为 E_CORRUPT）\n",
                   cut, dec);
            failures++;
            break;
        }
    }
    free(buf);
}

/* ---------------- 8) 容量不足 ⇒ E_NOSPACE ---------------- */

static void t8_no_space(void)
{
    i64 ts[64];
    u8 buf[64];
    u32 i;

    printf("  8) 容量不足：cap 给小 ⇒ E_NOSPACE\n");

    for (i = 0; i < 64; i++)
        ts[i] = rng_i64();             /* 随机 ⇒ 接近最坏情况 */

    CHECK(dod_encode(ts, 64, buf, 0) == -E_NOSPACE);
    CHECK(dod_encode(ts, 64, buf, 1) == -E_NOSPACE);
    CHECK(dod_encode(ts, 64, buf, 8) == -E_NOSPACE);
    CHECK(dod_encode(ts, 64, buf, sizeof(buf)) == -E_NOSPACE);

    /* 恒定间隔时 64 字节绰绰有余 —— 说明 E_NOSPACE 不是"一律失败" */
    for (i = 0; i < 64; i++)
        ts[i] = (i64)i * 1000LL;
    CHECK(dod_encode(ts, 64, buf, sizeof(buf)) > 0);
}

/* ---------------- 9) 上界 ---------------- */

static void t9_bound(void)
{
    i64 ts[300];
    u32 n;

    printf("  9) 上界：dod_encoded_bound(n) ≥ 实际编码长度\n");

    CHECK(dod_encoded_bound(0) == 20);
    CHECK(dod_encoded_bound(1) == 31);
    CHECK(dod_encoded_bound(100) == 1120);

    for (n = 1; n <= 300; n++) {
        u32 i;
        long bytes;

        for (i = 0; i < n; i++)
            ts[i] = rng_i64();
        bytes = roundtrip(ts, n, "上界");
        if (bytes < 0)
            break;
        CHECK((u32)bytes <= dod_encoded_bound(n));
    }
}

/* ---------------- 10) 乱码不崩 ---------------- */

static void t10_garbage(void)
{
    u8 buf[64];
    i64 out[64];
    int round;
    int crash = 0;

    printf("  10) 乱码不崩：随机字节 ⇒ 不崩，要么报错要么解出条数\n");

    for (round = 0; round < 500; round++) {
        u32 len = (u32)(rng_next() % 64u) + 1u;
        u32 n = (u32)(rng_next() % 33u) + 1u;
        u32 i;
        int dec;

        for (i = 0; i < len; i++)
            buf[i] = (u8)(rng_next() & 0xFFu);

        dec = dod_decode(buf, len, out, n);
        if (dec != -E_CORRUPT && dec != (int)n) {
            printf("    FAIL 乱码解码返回了 %d（只能是 E_CORRUPT 或条数）\n", dec);
            failures++;
            break;
        }
        crash++;
    }
    CHECK(crash == 500);
}

/* ---------------- 压缩比参考数 ---------------- */

static void report_ratio(void)
{
    static const char *names[] = { "恒定间隔(1s)", "抖动(±1s)", "随机时间戳" };
    u32 n = 1000;
    i64 ts[MAX_N];
    u32 kind;
    long bytes;

    printf("\n  压缩比参考（%u 个点，供 A3 bench 对照）：\n", n);

    for (kind = 0; kind < 3; kind++) {
        u32 i;

        for (i = 0; i < n; i++) {
            if (kind == 0)
                ts[i] = 1700000000000LL + (i64)i * 1000LL;
            else if (kind == 1)
                ts[i] = 1700000000000LL + (i64)i * 1000LL
                        + (i64)(rng_next() % 3u) - 1;
            else
                ts[i] = rng_i64();
        }
        bytes = roundtrip(ts, n, "压缩比");
        if (bytes > 0) {
            printf("    %-14s：%5ld 字节 ⇒ %.4f 字节/点\n",
                   names[kind], bytes, (double)bytes / (double)n);
        }
    }
}

int main(void)
{
    printf("test_ts_dod:\n");

    t1_basic();
    t2_constant_step();
    t3_unordered();
    t4_extremes();
    t5_duplicates();
    t6_fuzz();
    t7_truncated();
    t8_no_space();
    t9_bound();
    t10_garbage();
    report_ratio();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  全部通过\n");
    return 0;
}
