/*
 * test_ts_rle.c —— delta + RLE 整数列编码的验收（v1.5 步骤 1 · A1）
 *
 * 六组验收：
 *   1. **往返全等**：多种形态的数组 —— 随机长度 / 随机值 / 全同值 /
 *      每次 +1 的递增序列 / 负数 / i64 极值 / 值变了但游程很多的混合 ——
 *      编码→解码后必须**逐位全等**。
 *   2. **fuzz**：固定种子的 LCG 跑 2000+ 轮随机数组，每一轮都往返全等。
 *   3. **截断**：把编码结果截短 1 字节再解码 ⇒ 必须 E_CORRUPT，
 *      不许返回"看起来对"的结果。
 *   4. **容量不足** ⇒ E_NOSPACE。
 *   5. **畸形输入**：count = 0、count 过大、尾部多余字节、varint 组数超限
 *      ⇒ 一律 E_CORRUPT。
 *   6. **压缩比**（A3 bench 的参考数）：打印典型数据集的字节/点。
 *
 * 【为什么必须逐位全等】
 *
 * 编码器的唯一价值就是"解出来的和原来一样"。
 * 差一位、一个符号位错一次，结果就是**静默的错误数据** ——
 * 数值类的错不会像崩溃那样提醒你，它只会让报表对不上。
 *
 * 【为什么极值要单独测】
 *
 * delta = 本段段首值 - 上一段段首值。INT64_MIN 与 INT64_MAX 之间的差
 * 在 i64 里**根本表达不出来** —— 直接相减是溢出（UB）。
 * 本实现用模 2^64 的回绕语义（见 rle.c 的坑 M6-10），
 * 所以极值序列必须能**原样**还原。
 *
 * 【为什么截断必须报错而不是"尽力解出 n 个"】
 *
 * 少一个字节的流**必然**是坏的。解码端如果"读够 n 个就收工"，
 * 就会把坏数据变成"看起来对"的数据 —— 这个库里最不能忍的一类错误。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/rle.h"
#include "modalities/ts/tsbits.h"

#define MAXN 4096

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

static i64 g_v[MAXN];
static i64 g_out[MAXN];

/* ---------------- 确定性伪随机（不用 rand()：跨平台实现不一致） ---------------- */

static u64 lcg_state = 1u;

static void lcg_seed(u64 s)
{
    lcg_state = s;
}

static u64 lcg_next(void)
{
    lcg_state = lcg_state * 6364136223846793005ull + 1442695040888963407ull;
    return lcg_state;
}

/* 任意 i64（把两次 32 位抽样拼起来） */
static i64 lcg_i64(void)
{
    u64 hi = lcg_next() >> 32;
    u64 lo = lcg_next() >> 32;

    return (i64)((hi << 32) | lo);
}

/* [lo, hi] 内的 i64 */
static i64 lcg_range_i64(i64 lo, i64 hi)
{
    u64 span = (u64)(hi - lo) + 1ull;

    if (span == 0ull) {
        return lcg_i64();   /* 整个 i64 空间 */
    }
    return lo + (i64)(lcg_next() % span);
}

/* ---------------- 通用：编码 → 解码 → 逐位比对 ---------------- */

/*
 * 返回编码后的字节数（失败返回 -1）。
 */
static int roundtrip(const i64 *v, u32 n, const char *tag, u32 *bytes_out)
{
    u8 *buf;
    u32 cap;
    int rc;
    int drc;
    u32 i;

    cap = rle_encoded_bound(n);
    buf = (u8 *)malloc(cap > 0u ? (size_t)cap : 1u);
    if (buf == NULL) {
        printf("    FAIL %s: 内存不足\n", tag);
        failures++;
        return -1;
    }

    rc = rle_encode(v, n, buf, cap);
    if (rc < 0) {
        printf("    FAIL %s: 编码失败 rc=%d\n", tag, rc);
        failures++;
        free(buf);
        return -1;
    }
    if ((u32)rc > cap) {
        printf("    FAIL %s: 写入 %u 字节，超出上限 %u\n", tag, (u32)rc, cap);
        failures++;
        free(buf);
        return -1;
    }
    if (bytes_out) {
        *bytes_out = (u32)rc;
    }

    memset(g_out, 0, sizeof(g_out));
    drc = rle_decode(buf, (u32)rc, g_out, n);
    if (drc < 0) {
        printf("    FAIL %s: 解码失败 rc=%d（编码 %u 字节）\n", tag, drc, (u32)rc);
        failures++;
        free(buf);
        return -1;
    }
    if ((u32)drc != n) {
        printf("    FAIL %s: 解出 %d 条，应为 %u\n", tag, drc, n);
        failures++;
        free(buf);
        return -1;
    }
    for (i = 0; i < n; i++) {
        if (g_out[i] != v[i]) {
            printf("    FAIL %s: 第 %u 个值不同：应为 %lld，实际 %lld\n",
                   tag, i, (long long)v[i], (long long)g_out[i]);
            failures++;
            free(buf);
            return -1;
        }
    }

    free(buf);
    return (int)rc;
}

/* ---------------- 1. 往返全等：各种形态 ---------------- */

/* 1a. 随机长度 + 随机值（含长度 0 / 1 / 2 的边界） */
static void t_roundtrip_random(void)
{
    u32 n;

    lcg_seed(20261105u);
    for (n = 0; n <= 1000u; n++) {
        u32 i;

        for (i = 0; i < n; i++) {
            g_v[i] = lcg_i64();
        }
        roundtrip(g_v, n, "随机值", NULL);
    }
}

/* 1b. 全同值：RLE 收益最大（整列压成一段） */
static void t_roundtrip_same(void)
{
    u32 n;

    lcg_seed(20261106u);
    for (n = 0; n <= 1000u; n += 37u) {
        u32 i;
        i64 val = lcg_i64();

        for (i = 0; i < n; i++) {
            g_v[i] = val;
        }
        roundtrip(g_v, n, "全同值", NULL);
    }
}

/* 1c. 每次 +1 的递增序列：delta 收益最大 */
static void t_roundtrip_incr(void)
{
    u32 n;

    for (n = 0; n <= 1000u; n += 13u) {
        u32 i;

        for (i = 0; i < n; i++) {
            g_v[i] = (i64)i * 3 + 1000;
        }
        roundtrip(g_v, n, "递增 +3", NULL);
    }
}

/* 1d. 负数 */
static void t_roundtrip_negative(void)
{
    u32 n;

    lcg_seed(20261107u);
    for (n = 0; n <= 1000u; n += 29u) {
        u32 i;

        for (i = 0; i < n; i++) {
            g_v[i] = -(i64)(lcg_next() % 100000u) - 1;
        }
        roundtrip(g_v, n, "负数", NULL);
    }
}

/*
 * 1e. i64 极值：差值在 i64 里表达不出来 ⇒ 只能靠模 2^64 回绕（坑 M6-10）。
 */
static void t_roundtrip_extreme(void)
{
    static const i64 ext[] = {
        0,
        INT64_MAX, INT64_MIN,
        INT64_MAX - 1, INT64_MIN + 1,
        -1, 1,
        0, INT64_MAX, INT64_MIN
    };
    u32 n;

    for (n = 0; n <= 1000u; n += 7u) {
        u32 i;

        for (i = 0; i < n; i++) {
            g_v[i] = ext[i % (sizeof(ext) / sizeof(ext[0]))];
        }
        roundtrip(g_v, n, "i64 极值", NULL);
    }
}

/* 1f. 混合：值在变，但游程很多（1~5 个一段） */
static void t_roundtrip_mixed(void)
{
    u32 n;

    lcg_seed(20261108u);
    for (n = 0; n <= 1000u; n += 17u) {
        u32 i = 0;

        while (i < n) {
            u32 run = (u32)(lcg_next() % 5u) + 1u;
            i64 val = lcg_range_i64(-100000, 100000);
            u32 k;

            if (run > n - i) {
                run = n - i;
            }
            for (k = 0; k < run; k++) {
                g_v[i++] = val;
            }
        }
        roundtrip(g_v, n, "混合游程", NULL);
    }
}

/* ---------------- 2. fuzz ---------------- */

/*
 * 2000+ 轮随机数组：长度、值域、游程长度都随机。
 *
 * 一半用小值域（游程多 ⇒ 走 RLE 路径），
 * 一半用整个 i64 空间（delta 很大 ⇒ 走 varint 长路径）。
 */
static void t_fuzz(void)
{
    enum { ROUNDS = 2500 };
    int round;

    lcg_seed(20261109u);
    for (round = 0; round < ROUNDS; round++) {
        u32 n = (u32)(lcg_next() % 65u);
        u32 i = 0;
        int small = (int)(lcg_next() & 1u);

        while (i < n) {
            u32 run = (u32)(lcg_next() % 4u) + 1u;
            i64 val;
            u32 k;

            if (small) {
                val = (i64)(lcg_next() % 17u) - 8;
            } else {
                val = lcg_i64();
            }
            if (run > n - i) {
                run = n - i;
            }
            for (k = 0; k < run; k++) {
                g_v[i++] = val;
            }
        }
        if (roundtrip(g_v, n, "fuzz", NULL) < 0) {
            printf("    FAIL fuzz 第 %d 轮失败（n=%u）\n", round, n);
            failures++;
            break;
        }
    }
}

/* ---------------- 3. 截断 ⇒ E_CORRUPT ---------------- */

static void t_truncated(void)
{
    static const char *tags[] = { "同值", "递增", "随机", "极值" };
    u32 k;

    for (k = 0; k < 4u; k++) {
        u32 n = 100u;
        u32 i;
        u32 bytes = 0;
        u8 *buf;
        int rc;

        for (i = 0; i < n; i++) {
            if (k == 0u) {
                g_v[i] = 7;
            } else if (k == 1u) {
                g_v[i] = (i64)i;
            } else if (k == 2u) {
                g_v[i] = lcg_i64();
            } else {
                g_v[i] = (i % 2u == 0u) ? INT64_MIN : INT64_MAX;
            }
        }
        if (roundtrip(g_v, n, tags[k], &bytes) < 0) {
            continue;
        }
        if (bytes < 1u) {
            continue;
        }

        buf = (u8 *)malloc(bytes);
        if (buf == NULL) {
            continue;
        }
        {
            u32 cap = rle_encoded_bound(n);

            rc = rle_encode(g_v, n, buf, cap);
            if (rc < 0) {
                free(buf);
                continue;
            }
        }

        /* 截短 1 字节 ⇒ 必须报 E_CORRUPT，不许"看起来对" */
        memset(g_out, 0, sizeof(g_out));
        rc = rle_decode(buf, bytes - 1u, g_out, n);
        if (rc != -E_CORRUPT) {
            printf("    FAIL %s: 截短 1 字节后应报 E_CORRUPT，实际 rc=%d\n",
                   tags[k], rc);
            failures++;
        }

        free(buf);
    }
}

/* ---------------- 4. 容量不足 ⇒ E_NOSPACE ---------------- */

static void t_no_space(void)
{
    u8 buf[64];
    u32 i;
    int rc;

    for (i = 0; i < 20u; i++) {
        g_v[i] = (i64)i;
    }

    /* 容量 0（但有数据） ⇒ 装不下 */
    rc = rle_encode(g_v, 20u, buf, 0u);
    CHECK(rc == -E_NOSPACE);

    /* 容量 1 字节（至少要写 count + delta） ⇒ 装不下 */
    rc = rle_encode(g_v, 20u, buf, 1u);
    CHECK(rc == -E_NOSPACE);

    /* 容量刚好够 ⇒ 成功 */
    {
        u32 need = 0;

        CHECK(roundtrip(g_v, 20u, "容量刚好", &need) >= 0);
        rc = rle_encode(g_v, 20u, buf, need);
        CHECK(rc == (int)need);
    }
}

/* ---------------- 5. 畸形输入 ⇒ E_CORRUPT ---------------- */

/* 用 tsbits 手搓一段畸形流 */
static u32 craft(u8 *buf, u32 cap, const u64 *groups, u32 ngroups)
{
    TsBitWriter w;
    u32 i;
    u32 bytes = 0;

    tsbw_init(&w, buf, cap);
    for (i = 0; i < ngroups; i++) {
        tsbw_write_varint(&w, groups[i]);
    }
    tsbw_finish(&w, &bytes);
    return bytes;
}

static void t_malformed(void)
{
    u8 buf[64];
    u32 bytes;
    int rc;

    /*
     * 5a. count = 0（编码器永不写出）。
     *
     * 注意：这里在 count=0 之后**再补一段合法数据**，
     * 否则解码端"跳过空段继续读"也能碰巧撞到 E_CORRUPT ——
     * 那样这条断言就分不清"因为 count=0 而报错"和"因为读到末尾而报错"。
     * 补上合法尾巴后，若不检查 count=0，解码会**成功**返回 1 条 ⇒ 断言变红。
     */
    {
        u64 g[4];

        g[0] = 0;      /* 畸形：count = 0 */
        g[1] = 0;      /* 它的 delta（不会用到） */
        g[2] = 1;      /* 合法段：count = 1 */
        g[3] = 5;      /* 合法段：delta = 5 */
        bytes = craft(buf, sizeof(buf), g, 4u);
        rc = rle_decode(buf, bytes, g_out, 1u);
        CHECK(rc == -E_CORRUPT);
    }

    /* 5b. count 超过"还差多少个值" */
    {
        u64 g[2];

        g[0] = 5;
        g[1] = 0;
        bytes = craft(buf, sizeof(buf), g, 2u);
        rc = rle_decode(buf, bytes, g_out, 2u);
        CHECK(rc == -E_CORRUPT);
    }

    /* 5c. varint 的组数超过 64 位 ⇒ 格式错 */
    {
        u64 g[2];

        g[0] = 0xFFFFFFFFFFFFFFFFull;   /* 需要 10 组 */
        g[1] = 0;
        bytes = craft(buf, sizeof(buf), g, 2u);
        rc = rle_decode(buf, bytes, g_out, 1u);
        CHECK(rc == -E_CORRUPT);
    }

    /* 5d. 尾部多余字节（坑 M6-12：必须吃干净整条流） */
    {
        u32 i;

        for (i = 0; i < 10u; i++) {
            g_v[i] = (i64)i;
        }
        bytes = 0;
        CHECK(roundtrip(g_v, 10u, "尾部多余", &bytes) >= 0);
        {
            u32 cap = rle_encoded_bound(10u);

            rc = rle_encode(g_v, 10u, buf, cap);
            CHECK(rc >= 0);
            if (rc >= 0) {
                /* 后面再塞一个字节 */
                buf[rc] = 0x5Au;
                rc = rle_decode(buf, (u32)rc + 1u, g_out, 10u);
                CHECK(rc == -E_CORRUPT);
            }
        }
    }

    /* 5e. 空数据但 n > 0 ⇒ 读不到任何一段 ⇒ E_CORRUPT */
    {
        rc = rle_decode(buf, 0u, g_out, 1u);
        CHECK(rc == -E_CORRUPT);
    }
}

/* ---------------- 6. 压缩比（A3 bench 的参考数） ---------------- */

static void t_ratio(void)
{
    enum { N = 1000 };
    u32 i;
    u32 bytes = 0;

    /* 全同值：RLE 收益最大 */
    for (i = 0; i < N; i++) {
        g_v[i] = 42;
    }
    if (roundtrip(g_v, N, "比率-同值", &bytes) >= 0) {
        printf("    压缩比 · 全同值    ：%6u 字节 / %u 点 = %.3f 字节/点"
               "（原始 8 字节/点）\n", bytes, N, (double)bytes / (double)N);
    }

    /* 每次 +1：delta 收益最大 */
    for (i = 0; i < N; i++) {
        g_v[i] = (i64)i;
    }
    if (roundtrip(g_v, N, "比率-递增", &bytes) >= 0) {
        printf("    压缩比 · 递增 +1   ：%6u 字节 / %u 点 = %.3f 字节/点\n",
               bytes, N, (double)bytes / (double)N);
    }

    /* 小值域随机：游程多 */
    lcg_seed(20261110u);
    for (i = 0; i < N; i++) {
        g_v[i] = (i64)(lcg_next() % 16u);
    }
    if (roundtrip(g_v, N, "比率-小值域", &bytes) >= 0) {
        printf("    压缩比 · 小值域随机：%6u 字节 / %u 点 = %.3f 字节/点\n",
               bytes, N, (double)bytes / (double)N);
    }

    /* 全随机 i64：最坏情况（坑 M6-11：可能比原始还大） */
    lcg_seed(20261111u);
    for (i = 0; i < N; i++) {
        g_v[i] = lcg_i64();
    }
    if (roundtrip(g_v, N, "比率-全随机", &bytes) >= 0) {
        printf("    压缩比 · 全随机 i64：%6u 字节 / %u 点 = %.3f 字节/点"
               "（⚠ 位级 varint 每组占满一字节 ⇒ 最坏 10 字节/段）\n",
               bytes, N, (double)bytes / (double)N);
    }
}

/* ---------------- 7. 上限函数 ---------------- */

static void t_bound(void)
{
    CHECK(rle_encoded_bound(0u) == 0u);
    CHECK(rle_encoded_bound(1u) == 20u);
    CHECK(rle_encoded_bound(1000u) == 20000u);
}

int main(void)
{
    printf("test_ts_rle ...\n");

    t_roundtrip_random();
    t_roundtrip_same();
    t_roundtrip_incr();
    t_roundtrip_negative();
    t_roundtrip_extreme();
    t_roundtrip_mixed();
    t_fuzz();
    t_truncated();
    t_no_space();
    t_malformed();
    t_ratio();
    t_bound();

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
