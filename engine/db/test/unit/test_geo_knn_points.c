/*
 * test_geo_knn_points.c —— 带坐标与距离的 KNN（W-D 的 D-5）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * D-5 之前，db_geo_knn 为了填 db_geo_hit.dist 要**再摊平整棵树**
 * 把坐标捞回来。实测 10 万点 k=10：rtree_knn 自己只要 11 次页读，
 * 而 db_geo_knn 一共 739 次 —— 728 次全是那第二遍。
 *
 * 修法是让 KNN 把**它本来就算出来的东西**（距离、坐标）带回去。
 * 于是这个测试要证明三件事：
 *
 *  ① **结果与旧的 rtree_knn 逐位一致**（row_id 序列相同）——
 *     不能因为换了个函数就换了答案。
 *  ② **距离与坐标是对的**（与暴力基准同一套算术，逐位相等，不是"接近"）。
 *  ③ **没有多读一页**（页读数必须 ≤ 旧函数）——
 *     否则"省掉第二遍"就成了空话。
 *
 * ==================== 为什么"逐位相等"是唯一站得住的判据 ====================
 *
 * 空间查询最容易出的错是**静默漏点**：剪枝条件写反一个符号、少算一条边界，
 * 结果就少几个点，而且不报错。`geo_brute_knn` 存在的唯一理由就是这个
 *（见 geo_brute.h 的说明）。所以这里也用它做标尺。
 *
 * ==================== 死条目必须覆盖 ====================
 *
 * 现在的 db_geo_knn 之所以要"多要 k + 死条目数 + 16 个候选"，
 * 就是因为旧实现不知道哪些条目是死的。新函数**内部已经过滤**死条目，
 * 所以"要 k 个就是 k 个活点"。这条不测，等于没测 D-5.3 的核心改动。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_brute.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_knn.h"
#include "modalities/geo/rtree_scan.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH  "test/tmp/geo_knn_points.db"
#define MAX_OUT 2048u

static u64 g_lcg = 0;

static void lcg_seed(u64 s)
{
    g_lcg = s;
}

static u64 lcg_next(void)
{
    g_lcg = g_lcg * 6364136223846793005ull + 1442695040888963407ull;
    return g_lcg >> 11;
}

/* [lo, hi) 上的浮点（100 万分之一粒度，便于两个实现看到同一批数） */
static float lcg_range(float lo, float hi)
{
    float t = (float)(lcg_next() % 1000000ull) / 1000000.0f;

    return lo + (hi - lo) * t;
}

/* ---------------- 建树 ---------------- */

typedef struct {
    Pager *pg;
    RTree *tree;
    u32    n;        /* 建了几个点 */
} Fx;

static void fx_open(Fx *fx)
{
    remove(PATH);

    fx->pg = NULL;
    fx->tree = NULL;
    fx->n = 0;

    CHECK(pager_open(PATH, 256, &fx->pg) == OK);
    CHECK(rtree_create(fx->pg, &fx->tree) == OK);
}

static void fx_close(Fx *fx)
{
    if (fx->tree != NULL) {
        rtree_destroy(fx->tree);
        fx->tree = NULL;
    }
    if (fx->pg != NULL) {
        pager_close(fx->pg);
        fx->pg = NULL;
    }
}

/*
 * ==================== 第 1 项：与旧 rtree_knn 逐位一致 ====================
 *
 * 不掺死条目，纯"两个函数给出同一串 row_id"。
 */
static void t1_same_as_rtree_knn(void)
{
    enum { N = 2000 };
    Fx fx;
    u32 old_ids[MAX_OUT];
    KnPoint pts[MAX_OUT];
    u32 n_old = 0;
    u32 n_new = 0;
    u64 r_old = 0;
    u64 r_new = 0;
    u32 i;
    int bad = 0;

    fx_open(&fx);
    lcg_seed(20260101u);
    for (i = 0; i < N; i++) {
        CHECK(rtree_insert(fx.tree, i, lcg_range(-1000.0f, 1000.0f),
                           lcg_range(-1000.0f, 1000.0f)) == OK);
    }
    fx.n = N;

    /* 20 个查询点、从小到大 3 档 k */
    for (i = 0; i < 20u && !bad; i++) {
        float qx = lcg_range(-1000.0f, 1000.0f);
        float qy = lcg_range(-1000.0f, 1000.0f);
        u32 k = (i % 3u == 0u) ? 1u : ((i % 3u == 1u) ? 10u : 100u);

        CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), qx, qy, k,
                        old_ids, MAX_OUT, &n_old, &r_old) == OK);
        CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), qx, qy, k,
                               pts, MAX_OUT, &n_new, &r_new) == OK);

        if (n_old != n_new) {
            printf("    FAIL 第 %u 次查询结果个数不同：旧 %u，新 %u\n", i, n_old, n_new);
            failures++;
            bad = 1;
            break;
        }
        for (u32 j = 0; j < n_old; j++) {
            if (pts[j].row_id != old_ids[j]) {
                printf("    FAIL 第 %u 次查询第 %u 名不同：旧 %u，新 %u\n",
                       i, j, old_ids[j], pts[j].row_id);
                failures++;
                bad = 1;
                break;
            }
        }
    }

    printf("  [1] 20 次查询：新函数与旧 rtree_knn 的 row_id 序列逐位一致\n");
    fx_close(&fx);
}

/*
 * ==================== 第 2 项：距离与坐标与暴力基准逐位一致 ====================
 *
 * 用 geo_brute_knn 当标尺（它按定义扫全部点）。
 * 距离用**逐位相等**（不是"接近"）——两个实现走同一个 geo_dist2，
 * 若这里有差，说明某个地方换了算术。
 */
static void t2_dist_and_coords_match_brute(void)
{
    enum { N = 1000 };
    Fx fx;
    GeoPoint *gp = NULL;
    KnPoint pts[MAX_OUT];
    u32 brute[MAX_OUT];
    u32 n_new = 0;
    u32 n_brute = 0;
    u64 r = 0;
    u32 i;
    int bad = 0;

    gp = (GeoPoint *)calloc(N, sizeof(*gp));

    fx_open(&fx);
    lcg_seed(777u);
    for (i = 0; i < N; i++) {
        float x = lcg_range(-500.0f, 500.0f);
        float y = lcg_range(-500.0f, 500.0f);

        CHECK(rtree_insert(fx.tree, i, x, y) == OK);
        gp[i].row_id = i;
        gp[i].x = x;
        gp[i].y = y;
    }
    fx.n = N;

    for (i = 0; i < 10u && !bad; i++) {
        float qx = lcg_range(-500.0f, 500.0f);
        float qy = lcg_range(-500.0f, 500.0f);
        u32 k = 50u;

        CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), qx, qy, k,
                               pts, MAX_OUT, &n_new, &r) == OK);
        CHECK(geo_brute_knn(gp, N, qx, qy, k, brute, MAX_OUT, &n_brute) == OK);

        if (n_new != n_brute) {
            printf("    FAIL 第 %u 次查询：新 %u 条，暴力 %u 条\n", i, n_new, n_brute);
            failures++;
            bad = 1;
            break;
        }
        for (u32 j = 0; j < n_brute; j++) {
            if (pts[j].row_id != brute[j]) {
                printf("    FAIL 第 %u 次查询第 %u 名：新 row_id %u，暴力 %u\n",
                       i, j, pts[j].row_id, brute[j]);
                failures++;
                bad = 1;
                break;
            }
            /*
             * 距离与坐标：直接从 KnPoint 里读到的必须与暴力基准
             * 用同一套算术算出来的**逐位相同**。
             */
            {
                float want_d = geo_dist2(qx, qy, gp[brute[j]].x, gp[brute[j]].y);

                if (pts[j].dist != want_d) {
                    printf("    FAIL 第 %u 次查询第 %u 名距离：%.9g vs 暴力 %.9g\n",
                           i, j, (double)pts[j].dist, (double)want_d);
                    failures++;
                    bad = 1;
                    break;
                }
                if (pts[j].x != gp[brute[j]].x || pts[j].y != gp[brute[j]].y) {
                    printf("    FAIL 第 %u 次查询第 %u 名坐标："
                           "(%.9g,%.9g) vs 暴力 (%.9g,%.9g)\n",
                           i, j, (double)pts[j].x, (double)pts[j].y,
                           (double)gp[brute[j]].x, (double)gp[brute[j]].y);
                    failures++;
                    bad = 1;
                    break;
                }
            }
        }
    }

    printf("  [2] 10 次查询 × k=50：row_id / 距离 / 坐标与暴力基准逐位一致\n");
    free(gp);
    fx_close(&fx);
}

/*
 * ==================== 第 3 项：死条目（D-5.3 的核心） ====================
 *
 * 旧调用方"多要 k+dead+16 个候选"就是因为旧实现不知道谁是死的。
 * 新函数内部过滤，所以：k=3 就是要 3 个**活点**，
 * 哪怕离查询点最近的 4 个里有 2 个是死的。
 */
static void t3_dead_entries(void)
{
    Fx fx;
    KnPoint pts[MAX_OUT];
    u32 n_new = 0;
    u64 r = 0;
    u64 dead_reads = 0;
    u32 i;

    fx_open(&fx);

    /* 20 个点排成一条直线：row_id = i，坐标 (i, 0) */
    for (i = 0; i < 20u; i++) {
        CHECK(rtree_insert(fx.tree, i, (float)i, 0.0f) == OK);
    }
    fx.n = 20u;

    /* 把离 (0,0) 最近的 3 个（row_id 0、1、2）标死 */
    for (i = 0; i < 3u; i++) {
        CHECK(rtree_mark_dead(fx.pg, rtree_root_page(fx.tree), i, &dead_reads) == OK);
    }

    /*
     * k = 3 就要 3 条 —— 而且必须是活的 3、4、5。
     * ⚠️ 若新函数没过滤死条目，这里会拿到 0、1、2（死点）。
     */
    CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 3u,
                           pts, MAX_OUT, &n_new, &r) == OK);
    CHECK(n_new == 3u);
    CHECK(pts[0].row_id == 3u);
    CHECK(pts[1].row_id == 4u);
    CHECK(pts[2].row_id == 5u);
    /* 距离也要对：row_id 3 在 (3,0)，到 (0,0) 的平方距离 = 9 */
    CHECK(pts[0].dist == geo_dist2(0.0f, 0.0f, 3.0f, 0.0f));
    CHECK(pts[0].x == 3.0f);
    CHECK(pts[0].y == 0.0f);

    /*
     * 活点只剩 17 个 ⇒ k=100 时结果只有 17 条（不是 20 条，
     * 也不能把死点算进来）。
     */
    CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 100u,
                           pts, MAX_OUT, &n_new, &r) == OK);
    CHECK(n_new == 17u);

    /*
     * 装不下：min(k, 活点数) = 17 > max_out = 5 ⇒ E_NOSPACE（事后精确判）。
     */
    CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), 0.0f, 0.0f, 100u,
                           pts, 5u, &n_new, &r) == E_NOSPACE);

    printf("  [3] 3 个死条目：k=3 返回活点 3/4/5，k=100 只返回 17 条，"
           "max_out=5 精确报 NOSPACE\n");
    fx_close(&fx);
}

/*
 * ==================== 第 4 项：不多读一页（D-5 的目的本身） ====================
 *
 * 页读数必须 ≤ 旧函数 —— 同一份数据、同一批查询。
 * 若新函数多读了，那"省掉第二遍"就等于没做。
 */
static void t4_page_reads_not_worse(void)
{
    enum { N = 5000 };
    Fx fx;
    KnPoint pts[MAX_OUT];
    u32 old_ids[MAX_OUT];
    u32 n_old = 0;
    u32 n_new = 0;
    u64 r_old = 0;
    u64 r_new = 0;
    u32 i;

    fx_open(&fx);
    lcg_seed(31337u);
    for (i = 0; i < N; i++) {
        CHECK(rtree_insert(fx.tree, i, lcg_range(-2000.0f, 2000.0f),
                           lcg_range(-2000.0f, 2000.0f)) == OK);
    }
    fx.n = N;

    for (i = 0; i < 10u; i++) {
        float qx = lcg_range(-2000.0f, 2000.0f);
        float qy = lcg_range(-2000.0f, 2000.0f);
        u32 k = 20u;

        CHECK(rtree_knn(fx.pg, rtree_root_page(fx.tree), qx, qy, k,
                        old_ids, MAX_OUT, &n_old, &r_old) == OK);
        CHECK(rtree_knn_points(fx.pg, rtree_root_page(fx.tree), qx, qy, k,
                               pts, MAX_OUT, &n_new, &r_new) == OK);

        CHECK(r_new <= r_old);          /* 一页都不许多 */
        CHECK(n_new == n_old);
    }

    printf("  [4] 10 次查询：新函数的页读 ≤ 旧函数（一次都没多读）\n");
    fx_close(&fx);
}

int main(void)
{
    printf("test_geo_knn_points:\n");

    t1_same_as_rtree_knn();
    t2_dist_and_coords_match_brute();
    t3_dead_entries();
    t4_page_reads_not_worse();

    remove(PATH);

    if (failures == 0) {
        printf("  4 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
