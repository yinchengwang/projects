/*
 * test_geo_zorder.c —— Z-order 对照索引的验收（v1.4 步骤 4）
 *
 * 六项验收：
 *   1. **量化**：越界截断到两端、退化区间返回 0、反量化往返一致。
 *   2. **Morton 编解码**：往返一致（边界 + 随机），且不同的点码不同（双射）。
 *   3. **窗口全等**：2000 个确定性随机点 + 10 个随机窗口 + 整片窗口，
 *      Z-order 的结果与**暴力**（同一套量化坐标）集合完全相同。
 *   4. **R4 超集断言**：候选集（扫到的点，**未过滤**）必须**包含**窗口内所有点
 *      —— 这一条查的是"会不会漏点"，与第 3 项查的"点找对没有"是两件事。
 *   5. **IO 口径**：1% 面积窗口的桶数明显少于全扫；桶数 > 0；扫过的点数 ≥ 命中数。
 *   6. **边界**：空索引不崩；结果装不下时返回 E_NOSPACE（不静默截断）；
 *      退化成一条线/一个点的窗口也能查。
 *
 * 【为什么随机数自己写 LCG】
 * rand() 的实现跨平台/跨 libc 不一致，同一份测试在不同机器上跑出不同的数据，
 * bug 就没法复现。自己写 LCG：同样的种子 → 同样的数据 → 同样的结果。
 *
 * 【为什么暴力基准要用"同一套量化坐标"】
 * Z-order 的坐标是量化后的 u32（32 位），而暴力那侧若用原始 double 过滤，
 * 就会在量化边界上差 1 ulp —— 那是**口径不同**，不是 bug。
 * 所以两边的过滤判据必须用**同一批量化坐标**。
 *
 * 【为什么不跟 R-tree 比】
 * 这一层是**对照**用的：R-tree 是页式结构，IO 口径是逻辑页读；
 * Z-order 这里是内存里的桶，IO 口径是"桶数"（一桶 ≈ 一页）。
 * 两者在 bench/geo_bench.c 里对照；本测试只管**自己的正确性**。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/zorder.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 确定性伪随机 ---------------- */

static u32 lcg_state = 1u;

static void lcg_seed(u32 s)
{
    lcg_state = s;
}

static u32 lcg_next(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

/* [0, 1) */
static double lcg_u01(void)
{
    return (double)(lcg_next() >> 8) / 16777216.0;
}

/* ---------------- 测试数据 ---------------- */

#define N_PTS   2000
#define MAX_OUT 8192

static u32 g_qx[N_PTS];
static u32 g_qy[N_PTS];
static u32 g_rid[N_PTS];
static u32 g_out[MAX_OUT];
static u32 g_cand[MAX_OUT];

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a;
    u32 y = *(const u32 *)b;

    if (x < y) {
        return -1;
    }
    if (x > y) {
        return 1;
    }
    return 0;
}

/* 暴力窗口：同一套量化坐标，闭区间 */
static u32 brute_window(u32 x0, u32 x1, u32 y0, u32 y1, u32 *out)
{
    u32 n = 0;
    u32 i;

    for (i = 0; i < N_PTS; i++) {
        if (g_qx[i] >= x0 && g_qx[i] <= x1 && g_qy[i] >= y0 && g_qy[i] <= y1) {
            out[n++] = g_rid[i];
        }
    }
    if (n > 1u) {
        qsort(out, n, sizeof(u32), cmp_u32);
    }
    return n;
}

/* ---------------- 1. 量化 ---------------- */

static void test_quantize(void)
{
    const double lo = 0.0;
    const double hi = 1000.0;
    u32 q;

    printf("  1. 量化\n");

    /* 两端 */
    CHECK(geo_zorder_quantize(lo, lo, hi) == 0u);
    CHECK(geo_zorder_quantize(hi, lo, hi) == 0xFFFFFFFFu);

    /* 越界 ⇒ 截断到两端（不报错） */
    CHECK(geo_zorder_quantize(-5.0, lo, hi) == 0u);
    CHECK(geo_zorder_quantize(5000.0, lo, hi) == 0xFFFFFFFFu);

    /* 退化区间（hi <= lo）⇒ 一律 0 */
    CHECK(geo_zorder_quantize(10.0, 5.0, 5.0) == 0u);
    CHECK(geo_zorder_quantize(10.0, 5.0, 1.0) == 0u);

    /* 中点：量化后再反量化，应当仍在原值附近（1e-6 相对误差内） */
    q = geo_zorder_quantize(500.0, lo, hi);
    {
        double back = geo_zorder_dequantize(q, lo, hi);
        double rel = (back - 500.0) / 500.0;
        if (rel < 0) {
            rel = -rel;
        }
        CHECK(rel < 1e-6);
    }

    /* 反量化：两端必须精确落回 lo / hi */
    CHECK(geo_zorder_dequantize(0u, lo, hi) == lo);
    CHECK(geo_zorder_dequantize(0xFFFFFFFFu, lo, hi) == hi);

    /* 退化区间的反量化 ⇒ 返回 lo */
    CHECK(geo_zorder_dequantize(12345u, 5.0, 5.0) == 5.0);
}

/* ---------------- 2. Morton 编解码 ---------------- */

static void test_morton(void)
{
    u32 i;
    u32 x, y;

    printf("  2. Morton 编解码\n");

    /* 边界 */
    geo_zorder_decode(geo_zorder_encode(0u, 0u), &x, &y);
    CHECK(x == 0u && y == 0u);
    geo_zorder_decode(geo_zorder_encode(0xFFFFFFFFu, 0xFFFFFFFFu), &x, &y);
    CHECK(x == 0xFFFFFFFFu && y == 0xFFFFFFFFu);

    /* 随机往返 */
    lcg_seed(20261104u);
    for (i = 0; i < 1000u; i++) {
        u32 ax = lcg_next();
        u32 ay = lcg_next();

        geo_zorder_decode(geo_zorder_encode(ax, ay), &x, &y);
        if (x != ax || y != ay) {
            printf("    FAIL 往返不一致：(%u,%u) → (%u,%u)\n", ax, ay, x, y);
            failures++;
            break;
        }
    }

    /* 双射：不同的点 ⇒ 不同的码（抽查 2000 组） */
    {
        u64 seen_ok = 1;

        lcg_seed(777u);
        for (i = 0; i < 2000u; i++) {
            u64 c1 = geo_zorder_encode(lcg_next(), lcg_next());
            u64 c2 = geo_zorder_encode(lcg_next(), lcg_next());

            if (c1 == c2) {
                seen_ok = 0;
                break;
            }
        }
        CHECK(seen_ok == 1u);
    }
}

/* ---------------- 3 + 4 + 5. 窗口全等 / 超集 / IO ---------------- */

static void test_window(void)
{
    GeoZIndex *ix;
    u32 k;
    u32 brute[MAX_OUT];
    u64 total_buckets = 0;
    u64 total_points = 0;

    printf("  3/4/5. 窗口全等 + R4 超集断言 + IO 口径\n");

    lcg_seed(424242u);
    {
        u32 i;

        for (i = 0; i < N_PTS; i++) {
            g_qx[i] = geo_zorder_quantize(lcg_u01() * 1000.0, 0.0, 1000.0);
            g_qy[i] = geo_zorder_quantize(lcg_u01() * 1000.0, 0.0, 1000.0);
            g_rid[i] = i;
        }
    }

    ix = geo_zorder_build(g_qx, g_qy, g_rid, N_PTS, 5u);
    CHECK(ix != NULL);
    if (ix == NULL) {
        return;
    }

    CHECK(geo_zorder_n_points(ix) == N_PTS);
    CHECK(geo_zorder_cell_bits(ix) == 5u);
    CHECK(geo_zorder_n_buckets(ix) > 0u);

    /* 全扫基线 */
    {
        u64 bt = 0, ps = 0;

        CHECK(geo_zorder_window_io(ix, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, &bt, &ps) == OK);
        total_buckets = bt;
        total_points = ps;
        printf("    全扫：%llu 桶 / %llu 点（总桶数 %u）\n",
               (unsigned long long)total_buckets, (unsigned long long)total_points,
               geo_zorder_n_buckets(ix));
        CHECK(total_buckets > 0u);
        CHECK(total_points == N_PTS);
    }

    lcg_seed(987654u);
    for (k = 0; k < 10u; k++) {
        /* 随机窗口：在量化空间里取一段
         * ⚠️ 上界要留出余量：x0 + 200000000 可能溢出 u32 ⇒ x1 < x0
         *    ⇒ 查询返回 E_INVAL（那是**参数非法**，不是实现 bug）。 */
        u32 x0 = lcg_next() % 4000000000u;
        u32 x1 = x0 + (lcg_next() % 200000000u);
        u32 y0 = lcg_next() % 4000000000u;
        u32 y1 = y0 + (lcg_next() % 200000000u);
        u32 nb;
        u32 n_out = 0;
        u32 n_cand = 0;
        u64 bt = 0, ps = 0;
        u32 i;

        nb = brute_window(x0, x1, y0, y1, brute);
        CHECK(geo_zorder_window(ix, x0, x1, y0, y1, g_out, MAX_OUT, &n_out, &bt, &ps) == OK);

        /* 命中数一致 + 逐位一致 */
        CHECK(n_out == nb);
        if (n_out == nb) {
            for (i = 0; i < nb; i++) {
                if (g_out[i] != brute[i]) {
                    printf("    FAIL 窗口 %u：第 %u 个结果不同（%u vs %u）\n",
                           k, i, g_out[i], brute[i]);
                    failures++;
                    break;
                }
            }
        }

        /* R4 超集断言：候选集必须包含全部暴力命中点 */
        CHECK(geo_zorder_window_candidates(ix, x0, x1, y0, y1, g_cand, MAX_OUT,
                                            &n_cand, &bt, &ps) == OK);
        CHECK(n_cand >= n_out);
        for (i = 0; i < nb; i++) {
            u32 j;
            int found = 0;

            for (j = 0; j < n_cand; j++) {
                if (g_cand[j] == brute[i]) {
                    found = 1;
                    break;
                }
            }
            if (!found) {
                printf("    FAIL 超集断言：窗口 %u 里暴力算出的点 %u 不在候选集中\n",
                       k, brute[i]);
                failures++;
                break;
            }
        }

        /* IO 口径：桶数 > 0；扫过的点数 ≥ 命中数 */
        CHECK(bt > 0u);
        CHECK(ps >= (u64)n_out);
    }

    /* 整片窗口 ⇒ 全部点都要命中 */
    {
        u32 n_out = 0;
        u64 bt = 0, ps = 0;
        u32 nb = brute_window(0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, brute);

        CHECK(geo_zorder_window(ix, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, g_out, MAX_OUT,
                                &n_out, &bt, &ps) == OK);
        CHECK(n_out == N_PTS);
        CHECK(nb == N_PTS);
    }

    /* 1% 面积的小窗口：桶数应明显少于全扫 */
    {
        u32 n_out = 0;
        u64 bt = 0, ps = 0;
        /* 量化空间 2^32，取边长 2^32/100 ≈ 42949673（≈ 1% 面积） */
        u32 side = 42949673u;

        CHECK(geo_zorder_window(ix, 0u, side, 0u, side, g_out, MAX_OUT, &n_out,
                                &bt, &ps) == OK);
        printf("    1%% 窗口：%llu 桶（全扫 %llu 桶，比值 %.3f）\n",
               (unsigned long long)bt, (unsigned long long)total_buckets,
               total_buckets ? (double)bt / (double)total_buckets : 0.0);
        CHECK(bt > 0u);
        CHECK(bt * 10u < total_buckets);
    }

    geo_zorder_free(ix);
}

/* ---------------- 6. 边界 ---------------- */

static void test_edges(void)
{
    GeoZIndex *ix;
    u32 n_out = 0;
    u32 i;

    printf("  6. 边界\n");

    /* 空索引：建得出来、查不崩、命中 0 个 */
    ix = geo_zorder_build(NULL, NULL, NULL, 0u, 4u);
    CHECK(ix != NULL);
    if (ix != NULL) {
        CHECK(geo_zorder_n_points(ix) == 0u);
        CHECK(geo_zorder_n_buckets(ix) == 0u);
        CHECK(geo_zorder_window(ix, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, g_out, MAX_OUT,
                                &n_out, NULL, NULL) == OK);
        CHECK(n_out == 0u);
        geo_zorder_free(ix);
    }

    /* 非法参数 */
    CHECK(geo_zorder_build(g_qx, g_qy, g_rid, N_PTS, 17u) == NULL);
    CHECK(geo_zorder_build(NULL, g_qy, g_rid, N_PTS, 4u) == NULL);

    /* 装不下 ⇒ E_NOSPACE（不静默截断） */
    ix = geo_zorder_build(g_qx, g_qy, g_rid, N_PTS, 5u);
    CHECK(ix != NULL);
    if (ix != NULL) {
        n_out = 0;
        CHECK(geo_zorder_window(ix, 0u, 0xFFFFFFFFu, 0u, 0xFFFFFFFFu, g_out, 1u,
                                &n_out, NULL, NULL) == E_NOSPACE);
        CHECK(n_out == N_PTS);      /* 报"装不下"时把真实个数说出来 */

        /* 退化成一条线（y 只有一个量化值）与一个点 */
        {
            u32 yq = geo_zorder_quantize(500.0, 0.0, 1000.0);

            n_out = 0;
            CHECK(geo_zorder_window(ix, 0u, 0xFFFFFFFFu, yq, yq, g_out, MAX_OUT,
                                    &n_out, NULL, NULL) == OK);
            for (i = 0; i < n_out; i++) {
                CHECK(g_qy[g_out[i]] == yq);
            }
        }
        {
            u32 xq = geo_zorder_quantize(123.0, 0.0, 1000.0);
            u32 yq = geo_zorder_quantize(456.0, 0.0, 1000.0);

            n_out = 0;
            CHECK(geo_zorder_window(ix, xq, xq, yq, yq, g_out, MAX_OUT, &n_out,
                                    NULL, NULL) == OK);
            for (i = 0; i < n_out; i++) {
                CHECK(g_qx[g_out[i]] == xq && g_qy[g_out[i]] == yq);
            }
        }

        /* 窗口反了 ⇒ E_INVAL（不是悄悄返回 0 个） */
        n_out = 0;
        CHECK(geo_zorder_window(ix, 100u, 50u, 0u, 100u, g_out, MAX_OUT, &n_out,
                                NULL, NULL) == E_INVAL);

        geo_zorder_free(ix);
    }
}

int main(void)
{
    printf("=== test_geo_zorder：Z-order（Morton 码 + 固定网格桶）===\n");

    test_quantize();
    test_morton();
    test_window();
    test_edges();

    if (failures == 0) {
        printf("=== 全部通过 ===\n");
        printf("[PASS]\n");
        return 0;
    }

    printf("=== %d 项验收失败 ===\n", failures);
    printf("[FAIL]\n");
    return 1;
}
