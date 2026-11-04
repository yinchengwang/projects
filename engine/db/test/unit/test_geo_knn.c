/*
 * test_geo_knn.c —— R-tree 精确 KNN 的验收（v1.4 步骤 3 · A3）
 *
 * 五项验收：
 *   1. **结果全等（含顺序）**：随机点集 + 网格点集，
 *      对若干查询点、若干 k，R-tree 的 KNN 结果与暴力基准
 *      geo_brute_knn **逐位相同**（不仅集合相同，顺序也相同）。
 *   2. **并列 tie-break**：等距点（圆环、对称网格）下，
 *      顺序一律 row_id 升序 —— 与暴力基准一致。
 *   3. **第 k 名并列的终止条件**：构造"第 k 近的距离 == 某棵子树的 mindist"
 *      的场景，验证剪枝用的是**严格大于**（坑 M4-44）。
 *   4. **边界与退化**：k > 点数、max_out 装不下、空树、k == 0。
 *   5. **页读计数**：KNN 的页读 > 0，且 k 小的时候页读明显少于全扫。
 *
 * 【为什么必须"连顺序一起全等"】
 *
 * KNN 返回的是**排名**，不是集合。
 * 集合全等但顺序不同，说明 tie-break 规则两边不一致 ——
 * 而"第 3 名是谁"恰恰是 KNN 的语义本身，顺序错了就是答案错了。
 *
 * 【为什么并列必须专门测】
 *
 * 浮点距离并列在空间数据里是**常态**（网格点、圆环、对称分布），
 * 不是边角情况。而"第 k 名"在并列时本来就不唯一 ——
 * 所以必须定死一条规则（本库：row_id 升序），并且两边都用同一条。
 * 否则全等验收会被浮点噪声随机击穿（今天是"碰巧一致"，明天换台机器就炸）。
 *
 * 【第 3 项为什么是本文件最重要的一项】
 *
 * 提前终止的判据写成 `堆顶 mindist >= 第 k 近距离` 会**漏点**：
 * 被剪掉的子树里可能正好有一个距离等于 d_k、但 row_id 更小的点，
 * 它按 tie-break 本该排在第 k 名**之前**。
 * 而漏点**不报错** —— 结果只是悄悄少了一个/换了一个。
 * 唯一能抓住它的办法，就是专门构造并列场景并与暴力基准逐位对照。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_brute.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_knn.h"
#include "storage/pager.h"

#define PATH "test/tmp/g7_knn.db"

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

static float lcg_range(float lo, float hi)
{
    double u = (double)(lcg_next() >> 8) / 16777216.0;

    return (float)(lo + (hi - lo) * u);
}

/* ---------------- 测试用缓冲 ---------------- */

#define MAX_PTS  8192
#define MAX_OUT  8192

static GeoPoint g_pts[MAX_PTS];
static u32      g_a[MAX_OUT];
static u32      g_b[MAX_OUT];

/* ---------------- 通用：建树 ---------------- */

typedef struct {
    Pager *pg;
    RTree *tree;
    u32    n;
} Fixture;

static void fx_open(Fixture *fx)
{
    remove(PATH);
    fx->pg = NULL;
    fx->tree = NULL;
    fx->n = 0;
    CHECK(pager_open(PATH, 512, &fx->pg) == OK);
    CHECK(rtree_create(fx->pg, &fx->tree) == OK);
}

static void fx_close(Fixture *fx)
{
    rtree_destroy(fx->tree);
    pager_close(fx->pg);
    fx->tree = NULL;
    fx->pg = NULL;
}

/* 把 g_pts[0..n-1] 插进树 */
static void fx_fill(Fixture *fx, u32 n)
{
    u32 i;

    fx->n = n;
    for (i = 0; i < n; i++)
        CHECK(rtree_insert(fx->tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
}

/* ---------------- 通用：一次 KNN 对照 ---------------- */

/*
 * 跑一次"树 vs 暴力"的逐位对照。
 * 返回 1 = 全等；返回 0 = 有差异（并打印差异）。
 */
static int knn_compare(Fixture *fx, float qx, float qy, u32 k, const char *tag)
{
    u32 na = 0;
    u32 nb = 0;
    u32 i;
    int rc_a;
    int rc_b;

    rc_a = rtree_knn(fx->pg, rtree_root_page(fx->tree), qx, qy, k,
                     g_a, MAX_OUT, &na, NULL);
    rc_b = geo_brute_knn(g_pts, fx->n, qx, qy, k, g_b, MAX_OUT, &nb);

    if (rc_a != OK || rc_b != OK) {
        printf("    FAIL %s 返回码不同：树 %d，暴力 %d\n", tag, rc_a, rc_b);
        failures++;
        return 0;
    }
    if (na != nb) {
        printf("    FAIL %s 结果个数不同：树 %u，暴力 %u\n", tag, na, nb);
        failures++;
        return 0;
    }
    for (i = 0; i < na; i++) {
        if (g_a[i] != g_b[i]) {
            printf("    FAIL %s 第 %u 名不同：树 %u，暴力 %u"
                   "（查询 (%.9g,%.9g) k=%u）\n",
                   tag, i, g_a[i], g_b[i], qx, qy, k);
            failures++;
            return 0;
        }
    }
    return 1;
}

/* ---------------- 1. 随机点集：逐位全等 ---------------- */

static void t_knn_equiv_random(void)
{
    enum { N = 1000 };
    static const u32 ks[] = { 1u, 2u, 3u, 5u, 10u, 37u };
    Fixture fx;
    u32 i;
    int q;

    fx_open(&fx);
    lcg_seed(20261103u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-1000.0f, 1000.0f);
        g_pts[i].y = lcg_range(-1000.0f, 1000.0f);
    }
    fx_fill(&fx, N);
    CHECK(rtree_npoints(fx.tree) == N);
    CHECK(rtree_knn_npoints(fx.pg, rtree_root_page(fx.tree)) == N);

    for (q = 0; q < 40; q++) {
        float qx = lcg_range(-1200.0f, 1200.0f);
        float qy = lcg_range(-1200.0f, 1200.0f);
        u32 ki;

        for (ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
            char tag[64];

            snprintf(tag, sizeof(tag), "随机查询 %d k=%u", q, ks[ki]);
            knn_compare(&fx, qx, qy, ks[ki], tag);
        }
    }
    fx_close(&fx);
}

/* ---------------- 2. 网格点集：并列极多 ---------------- */

static void t_knn_equiv_grid(void)
{
    enum { GX = 40, GY = 40, N = GX * GY };
    static const u32 ks[] = { 1u, 2u, 3u, 4u, 5u, 10u, 50u };
    Fixture fx;
    u32 i;
    u32 gx;
    u32 gy;
    int q;

    fx_open(&fx);
    i = 0;
    for (gy = 0; gy < GY; gy++) {
        for (gx = 0; gx < GX; gx++) {
            g_pts[i].row_id = i + 1u;
            g_pts[i].x = (float)gx * 10.0f;
            g_pts[i].y = (float)gy * 10.0f;
            i++;
        }
    }
    fx_fill(&fx, N);

    /*
     * 查询点分两类：
     *  - 打在**格点**上：会有大量等距点（上下左右四个距离相同）；
     *  - 打在**格子中心**：也会等距（对角方向）。
     * 两类都必须与暴力基准逐位一致。
     */
    for (q = 0; q < 30; q++) {
        float qx = (float)(lcg_next() % GX) * 10.0f;
        float qy = (float)(lcg_next() % GY) * 10.0f;
        u32 ki;

        for (ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
            char tag[64];

            snprintf(tag, sizeof(tag), "网格查询 %d k=%u", q, ks[ki]);
            knn_compare(&fx, qx, qy, ks[ki], tag);
        }

        qx += 5.0f;
        qy += 5.0f;
        for (ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
            char tag[64];

            snprintf(tag, sizeof(tag), "网格中心查询 %d k=%u", q, ks[ki]);
            knn_compare(&fx, qx, qy, ks[ki], tag);
        }
    }
    fx_close(&fx);
}

/* ---------------- 3. 等距圆环 + 远处填充：并列 tie-break ---------------- */

/*
 * 12 个整数坐标的点在半径 5 的圆上（x²+y² = 25）：
 *   (±5,0) (0,±5) (±3,±4) (±4,±3)
 * 全部**精确**等距（浮点表示精确），因此第 1 名到第 12 名同距，
 * 顺序完全由 row_id 决定 —— 这是最严格的 tie-break 检验。
 *
 * 后面再塞 600 个远处点，逼出树的分裂（单叶最多 203 条），
 * 让"某个子树恰好装着一颗等距点"成为可能 ——
 * 那正是 `>=` 剪枝会漏点的场景。
 */
static void t_knn_tie_ring(void)
{
    enum { NRING = 12, NFAR = 600, N = NRING + NFAR };
    static const float ring_x[NRING] = {
        5.0f, -5.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, -3.0f, 4.0f, -4.0f, 4.0f, -4.0f
    };
    static const float ring_y[NRING] = {
        0.0f, 0.0f, 5.0f, -5.0f, 4.0f, 4.0f, -4.0f, -4.0f, 3.0f, 3.0f, -3.0f, -3.0f
    };
    static const u32 ks[] = { 1u, 2u, 3u, 5u, 11u, 12u, 13u };
    Fixture fx;
    u32 i;
    u32 ki;

    fx_open(&fx);
    for (i = 0; i < NRING; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = ring_x[i];
        g_pts[i].y = ring_y[i];
    }
    lcg_seed(777u);
    for (i = NRING; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(5000.0f, 9000.0f);
        g_pts[i].y = lcg_range(5000.0f, 9000.0f);
    }
    fx_fill(&fx, N);

    for (ki = 0; ki < sizeof(ks) / sizeof(ks[0]); ki++) {
        char tag[64];

        snprintf(tag, sizeof(tag), "圆环查询 k=%u", ks[ki]);
        knn_compare(&fx, 0.0f, 0.0f, ks[ki], tag);
    }
    fx_close(&fx);
}

/*
 * 第 3 项的"加强版"：k 正好卡在并列组的边界上。
 *
 * 圆环 12 点同距、远处 600 点更远：
 *   - k = 12 → 前 12 名全是圆环点，顺序 row_id 升序；
 *   - k = 13 → 第 13 名是最近的远处点。
 * 此时"第 k 近的距离" == 圆环距离，而圆环点所在子树的 mindist
 * 恰好可能等于这个距离 —— `>=` 会在此处剪掉整棵子树，
 * 丢掉 row_id 更小、但本该排在前面的并列点。
 */
static void t_knn_tie_at_kth_boundary(void)
{
    enum { NRING = 12, NFAR = 600, N = NRING + NFAR };
    static const float ring_x[NRING] = {
        5.0f, -5.0f, 0.0f, 0.0f, 3.0f, -3.0f, 3.0f, -3.0f, 4.0f, -4.0f, 4.0f, -4.0f
    };
    static const float ring_y[NRING] = {
        0.0f, 0.0f, 5.0f, -5.0f, 4.0f, 4.0f, -4.0f, -4.0f, 3.0f, 3.0f, -3.0f, -3.0f
    };
    Fixture fx;
    u32 i;
    u32 k;

    fx_open(&fx);
    for (i = 0; i < NRING; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = ring_x[i];
        g_pts[i].y = ring_y[i];
    }
    lcg_seed(778u);
    for (i = NRING; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(5000.0f, 9000.0f);
        g_pts[i].y = lcg_range(5000.0f, 9000.0f);
    }
    fx_fill(&fx, N);

    /* k 从 1 扫到 20：每一个 k 都与暴力基准逐位对照 */
    for (k = 1; k <= 20u; k++) {
        char tag[64];

        snprintf(tag, sizeof(tag), "并列临界 k=%u", k);
        knn_compare(&fx, 0.0f, 0.0f, k, tag);
    }

    /* 前 12 名必须是圆环点，且 row_id 严格升序 */
    {
        u32 na = 0;

        CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 12u,
                        g_a, MAX_OUT, &na, NULL) == OK);
        CHECK(na == 12u);
        for (i = 0; i < na && i < 12u; i++) {
            if (g_a[i] != i + 1u) {
                printf("    FAIL 并列组第 %u 名应为 row_id %u，实际 %u\n",
                       i, i + 1u, g_a[i]);
                failures++;
                break;
            }
        }
    }
    fx_close(&fx);
}

/* ---------------- 4. 边界与退化 ---------------- */

static void t_knn_edge_cases(void)
{
    enum { N = 300 };
    Fixture fx;
    u32 i;
    u32 n = 0;

    fx_open(&fx);
    lcg_seed(4242u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-100.0f, 100.0f);
        g_pts[i].y = lcg_range(-100.0f, 100.0f);
    }
    fx_fill(&fx, N);

    /* k > 点数 → 返回点数（不是 k 个，也不是报错） */
    CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 1000u,
                    g_a, MAX_OUT, &n, NULL) == OK);
    CHECK(n == N);

    /* max_out 装不下 → E_NOSPACE（不静默截断） */
    n = 0;
    CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 10u,
                    g_a, 3u, &n, NULL) == E_NOSPACE);

    /* k == 0 → 0 条，OK */
    n = 12345u;
    CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 0u,
                    g_a, MAX_OUT, &n, NULL) == OK);
    CHECK(n == 0);

    /* 空树（根页不存在） → 0 条，OK */
    n = 12345u;
    CHECK(rtree_knn(fx.pg, PAGE_ID_NIL, 0.0f, 0.0f, 5u,
                    g_a, MAX_OUT, &n, NULL) == OK);
    CHECK(n == 0);
    CHECK(rtree_knn_npoints(fx.pg, PAGE_ID_NIL) == 0);

    fx_close(&fx);
}

/* ---------------- 5. 页读计数 ---------------- */

static void t_knn_page_reads(void)
{
    enum { N = 4000 };
    Fixture fx;
    u64 r_small = 0;
    u64 r_big = 0;
    u64 r_scan = 0;
    u32 n = 0;
    u32 i;
    Mbr full;

    fx_open(&fx);
    lcg_seed(99991u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-10000.0f, 10000.0f);
        g_pts[i].y = lcg_range(-10000.0f, 10000.0f);
    }
    fx_fill(&fx, N);

    /* k = 1：只碰少数几页 */
    CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 1u,
                    g_a, MAX_OUT, &n, &r_small) == OK);
    CHECK(n == 1u);
    CHECK(r_small > 0);

    /* k = 1000：要展开更多子树 */
    CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 1000u,
                    g_a, MAX_OUT, &n, &r_big) == OK);
    CHECK(n == 1000u);
    CHECK(r_big > r_small);

    /* 全扫（整片平面的窗口查询）作参照：KNN 的 k=1 必须明显少于它 */
    mbr_set_rect(&full, -1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f);
    CHECK(rtree_window(fx.tree, &full, g_a, MAX_OUT, &n) == OK);
    CHECK(n == N);
    r_scan = rtree_page_reads(fx.tree);
    CHECK(r_scan > 0);
    CHECK(r_small < r_scan);

    printf("    [页读] k=1: %llu，k=1000: %llu，全扫窗口: %llu\n",
           (unsigned long long)r_small, (unsigned long long)r_big,
           (unsigned long long)r_scan);

    fx_close(&fx);
}

/* ---------------- main ---------------- */

int main(void)
{
    t_knn_equiv_random();
    t_knn_equiv_grid();
    t_knn_tie_ring();
    t_knn_tie_at_kth_boundary();
    t_knn_edge_cases();
    t_knn_page_reads();

    if (failures == 0) {
        printf("test_geo_knn: 全部通过\n");
        return 0;
    }
    printf("test_geo_knn: %d 处失败\n", failures);
    return 1;
}
