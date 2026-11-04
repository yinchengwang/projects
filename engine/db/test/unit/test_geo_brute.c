/*
 * test_geo_brute.c —— 暴力基准实现的验收（v1.4 步骤 1）
 *
 * 【为什么暴力实现自己也要有测试】
 *
 * 暴力基准是 R-tree 的**验收尺子**。
 * 尺子歪了，"两边全等"就什么也证明不了 —— 两边一起错、恰好相等，
 * 这种"通过"比不通过更危险。
 *
 * 所以这里用**手算的小数据集**把暴力实现钉死：
 *   - 窗口：命中哪些点、边界上的点算不算命中、超容量会不会报错；
 *   - KNN：顺序对不对、并列时 row_id 升序对不对、k 超过点数怎么办。
 *
 * 【tie-break 为什么必须在这里钉】
 *
 * 网格点与对称分布是空间数据的常态 —— 距离并列是**规则发生**的，
 * 不是罕见例外。若暴力与 R-tree 各自"随便挑一个"，
 * 全等验收会被浮点噪声随机击穿。所以两边都按 row_id 升序，
 * 这条规则在本文件里被断言成可执行的期望。
 */

#include <stdio.h>
#include <stdlib.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_brute.h"
#include "modalities/geo/mbr.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 1. 窗口查询 ---------------- */

static void test_window(void)
{
    GeoPoint pts[5];
    Mbr win;
    u32 ids[8];
    u32 n = 0;
    int rc;

    pts[0].row_id = 10; pts[0].x = 1.0f;  pts[0].y = 1.0f;
    pts[1].row_id = 20; pts[1].x = 5.0f;  pts[1].y = 5.0f;
    pts[2].row_id = 30; pts[2].x = 9.0f;  pts[2].y = 9.0f;
    pts[3].row_id = 40; pts[3].x = 11.0f; pts[3].y = 11.0f;
    pts[4].row_id = 50; pts[4].x = -1.0f; pts[4].y = -1.0f;

    mbr_set_rect(&win, 0.0f, 0.0f, 10.0f, 10.0f);
    rc = geo_brute_window(pts, 5, &win, ids, 8, &n);
    CHECK(rc == OK);
    CHECK(n == 3);
    CHECK(ids[0] == 10 && ids[1] == 20 && ids[2] == 30);
}

static void test_window_boundary(void)
{
    GeoPoint pts[4];
    Mbr win;
    u32 ids[8];
    u32 n = 0;

    /* 四个点分别在窗口的四条边上（含角点）—— 全部都该命中 */
    pts[0].row_id = 1; pts[0].x = 0.0f;   pts[0].y = 5.0f;   /* 左边 */
    pts[1].row_id = 2; pts[1].x = 10.0f;  pts[1].y = 5.0f;   /* 右边 */
    pts[2].row_id = 3; pts[2].x = 5.0f;   pts[2].y = 0.0f;   /* 下边 */
    pts[3].row_id = 4; pts[3].x = 5.0f;   pts[3].y = 10.0f;  /* 上边 */

    mbr_set_rect(&win, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(geo_brute_window(pts, 4, &win, ids, 8, &n) == OK);
    CHECK(n == 4);

    /* 刚好在边界外一点点 → 不命中 */
    pts[0].x = -0.001f;
    CHECK(geo_brute_window(pts, 4, &win, ids, 8, &n) == OK);
    CHECK(n == 3);
}

static void test_window_too_many(void)
{
    GeoPoint pts[3];
    Mbr win;
    u32 ids[2];
    u32 n = 0;

    pts[0].row_id = 1; pts[0].x = 1.0f; pts[0].y = 1.0f;
    pts[1].row_id = 2; pts[1].x = 2.0f; pts[1].y = 2.0f;
    pts[2].row_id = 3; pts[2].x = 3.0f; pts[3].y = 3.0f;

    mbr_set_rect(&win, 0.0f, 0.0f, 10.0f, 10.0f);

    /* 命中 3 个但只给 2 个位置 → 报错，不静默截断 */
    CHECK(geo_brute_window(pts, 3, &win, ids, 2, &n) == E_NOSPACE);

    /* 空窗口 → 0 个 */
    mbr_set_rect(&win, 100.0f, 100.0f, 110.0f, 110.0f);
    CHECK(geo_brute_window(pts, 3, &win, ids, 8, &n) == OK);
    CHECK(n == 0);
}

/* ---------------- 2. KNN ---------------- */

static void test_knn(void)
{
    GeoPoint pts[4];
    u32 ids[8];
    u32 n = 0;

    pts[0].row_id = 1; pts[0].x = 3.0f;  pts[0].y = 0.0f;   /* d2 = 9  */
    pts[1].row_id = 2; pts[1].x = 0.0f;  pts[1].y = 4.0f;   /* d2 = 16 */
    pts[2].row_id = 3; pts[2].x = -1.0f; pts[2].y = 0.0f;   /* d2 = 1  */
    pts[3].row_id = 4; pts[3].x = 10.0f; pts[3].y = 10.0f;  /* d2 = 200*/

    CHECK(geo_brute_knn(pts, 4, 0.0f, 0.0f, 3, ids, 8, &n) == OK);
    CHECK(n == 3);
    CHECK(ids[0] == 3);   /* d2 = 1  */
    CHECK(ids[1] == 1);   /* d2 = 9  */
    CHECK(ids[2] == 2);   /* d2 = 16 */
}

static void test_knn_tiebreak(void)
{
    GeoPoint pts[2];
    u32 ids[8];
    u32 n = 0;

    /* 两个点与查询点等距（对称）→ 必须按 row_id 升序 */
    pts[0].row_id = 5; pts[0].x = 1.0f;  pts[0].y = 0.0f;
    pts[1].row_id = 4; pts[1].x = -1.0f; pts[1].y = 0.0f;

    CHECK(geo_brute_knn(pts, 2, 0.0f, 0.0f, 2, ids, 8, &n) == OK);
    CHECK(n == 2);
    CHECK(ids[0] == 4);   /* 距离并列 → row_id 小的在前 */
    CHECK(ids[1] == 5);
}

static void test_knn_grid_tie(void)
{
    GeoPoint pts[4];
    u32 ids[8];
    u32 n = 0;

    /* 网格点：四个角到中心的距离完全相等 */
    pts[0].row_id = 1; pts[0].x = 1.0f;  pts[0].y = 1.0f;
    pts[1].row_id = 2; pts[1].x = 1.0f;  pts[1].y = -1.0f;
    pts[2].row_id = 3; pts[2].x = -1.0f; pts[2].y = 1.0f;
    pts[3].row_id = 4; pts[3].x = -1.0f; pts[3].y = -1.0f;

    CHECK(geo_brute_knn(pts, 4, 0.0f, 0.0f, 4, ids, 8, &n) == OK);
    CHECK(n == 4);
    CHECK(ids[0] == 1 && ids[1] == 2 && ids[2] == 3 && ids[3] == 4);
}

static void test_knn_edges(void)
{
    GeoPoint pts[3];
    u32 ids[8];
    u32 n = 0;

    pts[0].row_id = 1; pts[0].x = 0.0f; pts[0].y = 0.0f;
    pts[1].row_id = 2; pts[1].x = 1.0f; pts[1].y = 0.0f;
    pts[2].row_id = 3; pts[2].x = 2.0f; pts[2].y = 0.0f;

    /* 结果装不下（3 个结果，缓冲区只给 2 个）→ 报错，不静默截断 */
    CHECK(geo_brute_knn(pts, 3, 0.0f, 0.0f, 3, ids, 2, &n) == E_NOSPACE);

    /* k 超过点数 → 返回点数那么多个（"要 10 个但只有 3 个"不是错误） */
    CHECK(geo_brute_knn(pts, 3, 0.0f, 0.0f, 10, ids, 8, &n) == OK);
    CHECK(n == 3);

    /* k = 0 */
    CHECK(geo_brute_knn(pts, 3, 0.0f, 0.0f, 0, ids, 8, &n) == OK);
    CHECK(n == 0);

    /* 空点集 */
    CHECK(geo_brute_knn(pts, 0, 0.0f, 0.0f, 3, ids, 8, &n) == OK);
    CHECK(n == 0);
}

/* ---------------- 3. 排序工具 ---------------- */

static void test_ids_sort(void)
{
    u32 ids[5];

    ids[0] = 30; ids[1] = 10; ids[2] = 50; ids[3] = 20; ids[4] = 40;
    geo_ids_sort(ids, 5);
    CHECK(ids[0] == 10 && ids[1] == 20 && ids[2] == 30 && ids[3] == 40 && ids[4] == 50);
}

int main(void)
{
    printf("test_geo_brute ...\n");

    test_window();
    test_window_boundary();
    test_window_too_many();
    test_knn();
    test_knn_tiebreak();
    test_knn_grid_tie();
    test_knn_edges();
    test_ids_sort();

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
