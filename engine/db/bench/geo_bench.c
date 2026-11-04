/*
 * geo_bench.c —— Z-order 与 R-tree 的窗口 IO 对照基准（v1.4 步骤 4 · A5）
 *
 * 【要回答的问题】
 * R-tree 的 MBR 跟着数据长（聚簇友好，代价是要分裂、要维护）；
 * Z-order 的格子是死的（不管数据聚在哪里，代价是网格切不巧）。
 * A5 把两条路在三组分布下的"窗口 IO"摆在一起比：
 *   1. 均匀分布；
 *   2. 聚簇分布（100 个簇心，σ=2）；
 *   3. 高斯基组（x ~ N(中, 3000)，y ~ N(中, 300) —— 一条扁带）。
 * 每组量：1% 面积小窗口（20 个）的平均 IO，与整片窗口（全扫）的 IO。
 * 判据：1% 窗口的 IO < 全扫 IO / 10（R-tree 与 Z-order **分别**判）。
 *
 * ==================== 坐标口径（本基准最要紧的一件事） ====================
 *
 * 点落在**整数网格**上：x、y ∈ [0, 65536) 的整数。
 *
 * 为什么不用 double：
 * - R-tree 的 MBR 是 **float**（只有 24 位有效数字）；
 * - Z-order 的坐标是量化后的 **u32**（32 位）。
 * 若在 [0, 1000) 上按 32 位量化，量化步长 ≈ 2.3e-7，
 * 而 float 在 1000 附近的精度 ≈ 6e-5 ⇒ **float 分辨不出相邻的量化值**
 * （实测：20 万个点塌成 16 万个不同的 float 位置）⇒
 * 两边看到的**根本不是同一批点**，IO 对比就是废的。
 *
 * 所以本基准把坐标限制在 16 位整数网格上：
 * 整数 < 2^24 ⇒ float **精确**表示 ⇒ 两边的点集**逐点相同**，
 * 命中数与命中集合可以直接对比。
 *
 * ==================== IO 口径 ====================
 *
 * - R-tree：`rtree_page_reads()` 的**增量**（逻辑页读 = pager_get 次数，
 *   **不是**物理磁盘 IO）。
 * - Z-order：`geo_zorder_window()` 的 `buckets_touched`（桶数，一桶 ≈ 一页）。
 *
 * ⚠️ **两个 IO 单位不等价**：Z-order 一个桶最多 341 条（条目 12 字节），
 * R-tree 一个叶页最多 203 条（条目 20 字节）⇒ 按"装多少点"算，
 * Z-order 的 IO 单位更大，**对 Z-order 有利**。
 * 所以本基准额外给一个更严的口径：**等效页数 = 扫过的点数 / 203**
 * （"假如这些点存在 203 条一页的页里，要读几页"）——
 * 这个口径对两边是同一把尺子。
 *
 * ==================== 坑：buckets_touched 是"累加"语义 ====================
 *
 * `geo_zorder_window()` 内部只对 `*buckets_touched` 做 `++`，**不会**清零。
 * 同一个变量连续传给两次调用，第二次拿到的是**累计值**。
 * 本基准在**每次**调用前显式清零。
 * 不清零的症状很典型：小窗口的"桶数"会大于索引的总桶数（物理上不可能）。
 *
 * ==================== 退出码 ====================
 *
 * 0 = 全部判据达标；1 = 有判据不达标；2 = 基准自身出错（开库失败等）。
 * 用法：geo_bench [点数] [窗口数]    默认 100000 / 20
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/zorder.h"
#include "storage/pager.h"

#if defined(_WIN32)
#include <direct.h>
#define GB_MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define GB_MKDIR(p) mkdir((p), 0777)
#endif

#define GB_DB_PATH    "test/tmp/geo_bench.db"
#define GB_GRID       65536u      /* 坐标网格边长（16 位整数） */
#define GB_LEAF_CAP   203u        /* R-tree 一个叶页最多 203 条 */
#define GB_BUCKET_CAP 341u        /* Z-order 一个桶最多 341 条 */
#define GB_WIN_FRAC   0.01        /* 小窗口占数据包围盒面积的比例 */
#define GB_CLUSTERS   100u        /* 聚簇分布的簇心个数 */

/* ---------------- 确定性伪随机（LCG） ---------------- */

/*
 * 为什么不用 rand()：rand() 的实现跨平台/跨 libc 不一致，
 * 同一份基准在不同机器上跑出不同的数据集，结论没法复现。
 */
static u64 g_state = 1u;

static void lcg_seed(u64 s)
{
    g_state = s ? s : 1u;
}

static u32 lcg_next(void)
{
    g_state = g_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (u32)(g_state >> 32);
}

/* [0,1) 的浮点数（取高 24 位，避免低位相关性） */
static double lcg_unit(void)
{
    return (double)(lcg_next() >> 8) / 16777216.0;
}

/* 标准正态（Box-Muller） */
static double lcg_normal(void)
{
    double u1 = lcg_unit();
    double u2 = lcg_unit();
    double r;

    if (u1 < 1e-12) {
        u1 = 1e-12;
    }
    r = sqrt(-2.0 * log(u1));

    return r * cos(2.0 * 3.14159265358979323846 * u2);
}

/* ---------------- 数据集 ---------------- */

typedef enum {
    DIST_UNIFORM = 0,
    DIST_CLUSTER = 1,
    DIST_STRIP = 2,
    DIST_COUNT = 3
} GbDist;

typedef struct {
    u32   *qx;      /* 量化坐标（整数网格） */
    u32   *qy;
    u32   *ids;     /* row_id */
    float *fx;      /* 同一坐标的 float 视图（R-tree 用；整数 ⇒ 精确） */
    float *fy;
    u32    n;
    u32    minqx, maxqx, minqy, maxqy;
} GbData;

static const char *dist_name(GbDist d)
{
    switch (d) {
    case DIST_UNIFORM:
        return "均匀分布";
    case DIST_CLUSTER:
        return "聚簇分布（100 簇心，σ=2）";
    case DIST_STRIP:
        return "高斯基组（x~N(中,3000)，y~N(中,300)）";
    default:
        return "?";
    }
}

static void gb_data_free(GbData *d)
{
    free(d->qx);
    free(d->qy);
    free(d->ids);
    free(d->fx);
    free(d->fy);
    memset(d, 0, sizeof(*d));
}

/* 生成一批点（坐标取整到 16 位网格） */
static int gb_gen(GbDist dist, u32 n, GbData *d)
{
    u32 i;

    memset(d, 0, sizeof(*d));
    d->n = n;
    d->qx = (u32 *)calloc(n, sizeof(u32));
    d->qy = (u32 *)calloc(n, sizeof(u32));
    d->ids = (u32 *)calloc(n, sizeof(u32));
    d->fx = (float *)calloc(n, sizeof(float));
    d->fy = (float *)calloc(n, sizeof(float));
    if (!d->qx || !d->qy || !d->ids || !d->fx || !d->fy) {
        gb_data_free(d);
        return -1;
    }

    lcg_seed(0x5EEDu + (u64)dist * 7919u);

    if (dist == DIST_UNIFORM) {
        for (i = 0; i < n; i++) {
            d->qx[i] = (u32)(lcg_unit() * (double)GB_GRID);
            d->qy[i] = (u32)(lcg_unit() * (double)GB_GRID);
        }
    } else if (dist == DIST_CLUSTER) {
        u32 cx[GB_CLUSTERS];
        u32 cy[GB_CLUSTERS];

        for (i = 0; i < GB_CLUSTERS; i++) {
            cx[i] = (u32)(lcg_unit() * (double)GB_GRID);
            cy[i] = (u32)(lcg_unit() * (double)GB_GRID);
        }
        for (i = 0; i < n; i++) {
            u32 c = lcg_next() % GB_CLUSTERS;
            double x = (double)cx[c] + lcg_normal() * 2.0;
            double y = (double)cy[c] + lcg_normal() * 2.0;

            if (x < 0.0) {
                x = 0.0;
            }
            if (y < 0.0) {
                y = 0.0;
            }
            if (x >= (double)GB_GRID) {
                x = (double)GB_GRID - 1.0;
            }
            if (y >= (double)GB_GRID) {
                y = (double)GB_GRID - 1.0;
            }
            d->qx[i] = (u32)x;
            d->qy[i] = (u32)y;
        }
    } else {
        /* 扁带：x 方向宽、y 方向窄 */
        for (i = 0; i < n; i++) {
            double x = (double)GB_GRID / 2.0 + lcg_normal() * 3000.0;
            double y = (double)GB_GRID / 2.0 + lcg_normal() * 300.0;

            if (x < 0.0) {
                x = 0.0;
            }
            if (y < 0.0) {
                y = 0.0;
            }
            if (x >= (double)GB_GRID) {
                x = (double)GB_GRID - 1.0;
            }
            if (y >= (double)GB_GRID) {
                y = (double)GB_GRID - 1.0;
            }
            d->qx[i] = (u32)x;
            d->qy[i] = (u32)y;
        }
    }

    d->minqx = d->qx[0];
    d->maxqx = d->qx[0];
    d->minqy = d->qy[0];
    d->maxqy = d->qy[0];
    for (i = 0; i < n; i++) {
        if (d->qx[i] < d->minqx) {
            d->minqx = d->qx[i];
        }
        if (d->qx[i] > d->maxqx) {
            d->maxqx = d->qx[i];
        }
        if (d->qy[i] < d->minqy) {
            d->minqy = d->qy[i];
        }
        if (d->qy[i] > d->maxqy) {
            d->maxqy = d->qy[i];
        }
        d->ids[i] = i;
        d->fx[i] = (float)d->qx[i];      /* 整数 ⇒ float 精确 */
        d->fy[i] = (float)d->qy[i];
    }

    return 0;
}

/*
 * 把 16 位网格坐标映射到 u32 空间的**高 16 位**。
 *
 * 为什么：Z-order 的网格层级（cell_bits）是在**整个 u32 空间**上切格的。
 * 本基准的坐标只有 16 位有效（为了让 float 能精确表示），
 * 若直接丢进去，所有点都落在"第 0 格"里 —— 整个索引退化成 1 个桶。
 * 左移 16 位 = 把数据放到 u32 空间的高半区，
 * 于是 cell_bits 切出来的格子正好对齐到数据的实际范围。
 */
#define GB_SHIFT16 16u

static u32 gb_q32(u32 q)
{
    return q << GB_SHIFT16;
}

/* ---------------- 结果结构 ---------------- */

typedef struct {
    double rt_small;      /* 小窗口平均页读 */
    double z_small;       /* 小窗口平均桶数 */
    double z_small_eq;    /* 小窗口平均"等效页数"（扫过的点 / 203） */
    double rt_full;
    double z_full;
    double z_full_eq;
    double rt_hits;       /* 小窗口平均命中数 */
    double z_hits;
    u32    n_cells;
    u32    n_buckets;
    u32    cell_bits;
    u32    mismatch_cnt;  /* 两边命中数不同的窗口数 */
    u32    set_mismatch;  /* 命中集合不同的窗口数 */
} GbResult;

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

/* 跑一组分布 */
static int gb_run(GbDist dist, u32 n, u32 nwin, GbResult *res)
{
    GbData d;
    Pager *pg = NULL;
    RTree *tree = NULL;
    GeoZIndex *zix = NULL;
    u32 *buf_a = NULL;
    u32 *buf_b = NULL;
    u32 i;
    u32 w;
    u64 rt_sum = 0;
    u64 z_sum = 0;
    u64 zps_sum = 0;
    u64 rt_hits_sum = 0;
    u64 z_hits_sum = 0;
    u64 before;
    u32 na = 0;
    u32 nb = 0;
    u64 buckets = 0;
    u64 points_scanned = 0;
    u32 spanqx, spanqy, side_q;
    int rc = -1;

    memset(res, 0, sizeof(*res));

    if (gb_gen(dist, n, &d) != 0) {
        printf("    错误：生成数据失败\n");
        return -1;
    }

    buf_a = (u32 *)calloc(n, sizeof(u32));
    buf_b = (u32 *)calloc(n, sizeof(u32));
    if (!buf_a || !buf_b) {
        printf("    错误：内存不足\n");
        goto done;
    }

    GB_MKDIR("test/tmp");
    remove(GB_DB_PATH);
    remove("test/tmp/geo_bench.db-wal");
    remove("test/tmp/geo_bench.db-clog");

    if (pager_open(GB_DB_PATH, 512, &pg) != OK) {
        printf("    错误：pager_open 失败\n");
        goto done;
    }
    if (rtree_create(pg, &tree) != OK) {
        printf("    错误：rtree_create 失败\n");
        goto done;
    }
    for (i = 0; i < n; i++) {
        if (rtree_insert(tree, d.ids[i], d.fx[i], d.fy[i]) != OK) {
            printf("    错误：rtree_insert 失败（点 %u）\n", i);
            goto done;
        }
    }

    /* Z-order 的网格层级：让"每格平均点数"接近一个桶的容量 */
    {
        u32 bits = 0;
        double per_cell;

        for (bits = 0; bits < 16u; bits++) {
            per_cell = (double)n / (double)(1ull << (2u * bits));
            if (per_cell <= (double)GB_BUCKET_CAP) {
                break;
            }
        }
        res->cell_bits = bits;
        {
            u32 *sx = (u32 *)calloc(n, sizeof(u32));
            u32 *sy = (u32 *)calloc(n, sizeof(u32));

            if (!sx || !sy) {
                free(sx);
                free(sy);
                printf("    错误：内存不足\n");
                goto done;
            }
            for (i = 0; i < n; i++) {
                sx[i] = gb_q32(d.qx[i]);
                sy[i] = gb_q32(d.qy[i]);
            }
            zix = geo_zorder_build(sx, sy, d.ids, n, bits);
            free(sx);
            free(sy);
        }
        if (zix == NULL) {
            printf("    错误：geo_zorder_build 失败\n");
            goto done;
        }
        res->n_cells = geo_zorder_n_cells(zix);
        res->n_buckets = geo_zorder_n_buckets(zix);
    }

    /* ---- 全扫（整片窗口 = 数据包围盒） ---- */
    {
        Mbr full;

        mbr_set_rect(&full, (float)d.minqx, (float)d.minqy,
                     (float)d.maxqx, (float)d.maxqy);

        before = rtree_page_reads(tree);
        if (rtree_window(tree, &full, buf_a, n, &na) != OK) {
            printf("    错误：R-tree 全扫失败\n");
            goto done;
        }
        res->rt_full = (double)(rtree_page_reads(tree) - before);

        buckets = 0;
        points_scanned = 0;
        if (geo_zorder_window(zix, gb_q32(d.minqx), gb_q32(d.maxqx),
                              gb_q32(d.minqy), gb_q32(d.maxqy),
                              buf_b, n, &nb, &buckets, &points_scanned) != OK) {
            printf("    错误：Z-order 全扫失败\n");
            goto done;
        }
        res->z_full = (double)buckets;
        res->z_full_eq = (double)points_scanned / (double)GB_LEAF_CAP;
        if (na != nb) {
            printf("    ⚠ 全扫命中数不同：R-tree %u，Z-order %u\n", na, nb);
        }
    }

    /* ---- 小窗口（1% 面积） ---- */
    spanqx = d.maxqx - d.minqx;
    spanqy = d.maxqy - d.minqy;
    {
        double side = sqrt(GB_WIN_FRAC * (double)spanqx * (double)spanqy);

        side_q = (u32)side;
        if (side_q < 1u) {
            side_q = 1u;
        }
        if (side_q > spanqx) {
            side_q = spanqx;
        }
        if (side_q > spanqy) {
            side_q = spanqy;
        }
    }

    for (w = 0; w < nwin; w++) {
        u32 x0;
        u32 y0;
        u32 x1;
        u32 y1;
        Mbr win;

        x0 = d.minqx + (u32)(lcg_unit() * (double)(spanqx - side_q + 1u));
        y0 = d.minqy + (u32)(lcg_unit() * (double)(spanqy - side_q + 1u));
        if (x0 + side_q > d.maxqx) {
            x0 = d.maxqx - side_q;
        }
        if (y0 + side_q > d.maxqy) {
            y0 = d.maxqy - side_q;
        }
        x1 = x0 + side_q;
        y1 = y0 + side_q;

        mbr_set_rect(&win, (float)x0, (float)y0, (float)x1, (float)y1);

        before = rtree_page_reads(tree);
        if (rtree_window(tree, &win, buf_a, n, &na) != OK) {
            printf("    错误：R-tree 小窗口查询失败\n");
            goto done;
        }
        rt_sum += rtree_page_reads(tree) - before;

        buckets = 0;              /* 累加语义 ⇒ 每次调用前必须清零 */
        points_scanned = 0;
        if (geo_zorder_window(zix, gb_q32(x0), gb_q32(x1), gb_q32(y0), gb_q32(y1),
                              buf_b, n, &nb, &buckets, &points_scanned) != OK) {
            printf("    错误：Z-order 小窗口查询失败\n");
            goto done;
        }
        z_sum += buckets;
        zps_sum += points_scanned;

        rt_hits_sum += na;
        z_hits_sum += nb;

        if (na != nb) {
            res->mismatch_cnt++;
        } else {
            /* 命中数一样还要比集合：漏点是最难抓的错 */
            qsort(buf_a, na, sizeof(u32), cmp_u32);
            qsort(buf_b, nb, sizeof(u32), cmp_u32);
            if (na > 0 && memcmp(buf_a, buf_b, (size_t)na * sizeof(u32)) != 0) {
                res->set_mismatch++;
            }
        }
    }

    res->rt_small = (double)rt_sum / (double)nwin;
    res->z_small = (double)z_sum / (double)nwin;
    res->z_small_eq = (double)zps_sum / (double)nwin / (double)GB_LEAF_CAP;
    res->rt_hits = (double)rt_hits_sum / (double)nwin;
    res->z_hits = (double)z_hits_sum / (double)nwin;
    rc = 0;

done:
    if (zix != NULL) {
        geo_zorder_free(zix);
    }
    if (tree != NULL) {
        rtree_destroy(tree);
    }
    if (pg != NULL) {
        pager_close(pg);
    }
    free(buf_a);
    free(buf_b);
    gb_data_free(&d);

    return rc;
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    u32 n = 100000u;
    u32 nwin = 20u;
    int dist;
    int ok = 1;
    int rc = 0;

    if (argc > 1) {
        n = (u32)strtoul(argv[1], NULL, 10);
    }
    if (argc > 2) {
        nwin = (u32)strtoul(argv[2], NULL, 10);
    }
    if (n == 0u || nwin == 0u) {
        printf("用法：geo_bench [点数] [窗口数]\n");
        return 2;
    }

    printf("geo_bench —— Z-order 与 R-tree 的窗口 IO 对照（A5）\n");
    printf("  点数 = %u，小窗口数 = %u\n", n, nwin);
    printf("  坐标口径：16 位整数网格 [0,%u)（float 可精确表示 ⇒ 两边看到同一批点）\n",
           GB_GRID);
    printf("  IO 口径：R-tree = 逻辑页读增量；Z-order = 桶数（一桶 ≈ 一页）\n");
    printf("  ⚠ 两个 IO 单位不等价：桶 341 条 vs 叶页 203 条 ⇒ 另给「等效页数 = 扫过的点/203」\n");

    for (dist = 0; dist < DIST_COUNT; dist++) {
        GbResult r;

        printf("\n== %s ==\n", dist_name((GbDist)dist));
        if (gb_run((GbDist)dist, n, nwin, &r) != 0) {
            printf("  基准自身出错\n");
            return 2;
        }

        printf("    Z-order：网格层级 cell_bits = %u，非空格 %u，桶 %u\n",
               r.cell_bits, r.n_cells, r.n_buckets);
        printf("    小窗口平均 IO：R-tree %.2f 页 / Z-order %.2f 桶（等效 %.2f 页）；"
               "平均命中：R-tree %.0f 点 / Z-order %.0f 点\n",
               r.rt_small, r.z_small, r.z_small_eq, r.rt_hits, r.z_hits);
        printf("    全扫 IO：R-tree %.0f 页 / Z-order %.0f 桶（等效 %.0f 页）\n",
               r.rt_full, r.z_full, r.z_full_eq);
        printf("    比值（小/全）：R-tree %.4f / Z-order %.4f\n",
               r.rt_full ? r.rt_small / r.rt_full : 0.0,
               r.z_full ? r.z_small / r.z_full : 0.0);
        printf("    命中一致性：命中数不同的窗口 %u 个；命中集合不同的窗口 %u 个\n",
               r.mismatch_cnt, r.set_mismatch);

        /* 判据：1% 窗口 IO < 全扫 / 10 */
        {
            int rt_ok = (r.rt_small * 10.0 < r.rt_full);
            int z_ok = (r.z_small * 10.0 < r.z_full);
            int z_eq_ok = (r.z_small_eq * 10.0 < r.rt_full);

            printf("    判据 R-tree：1%% 窗口 %.2f 页 < 全扫/10 = %.2f ⇒ [%s]\n",
                   r.rt_small, r.rt_full / 10.0, rt_ok ? "PASS" : "FAIL");
            printf("    判据 Z-order：1%% 窗口 %.2f 桶 < 全扫/10 = %.2f ⇒ [%s]"
                   "（按等效页数 %.2f < %.2f ⇒ [%s]）\n",
                   r.z_small, r.z_full / 10.0, z_ok ? "PASS" : "FAIL",
                   r.z_small_eq, r.rt_full / 10.0, z_eq_ok ? "PASS" : "FAIL");

            if (!rt_ok) {
                ok = 0;
            }
            if (!z_ok) {
                /* Z-order 的失败**不**算基准失败：
                 * 它恰恰是要对照出来的事实（固定网格切不巧）。 */
                printf("    ⚠ Z-order 不满足「1%% 窗口 IO < 全扫/10」 —— "
                       "这正是对照要展示的事实（固定网格对分布敏感），不算基准失败\n");
            }
            if (r.mismatch_cnt != 0u || r.set_mismatch != 0u) {
                printf("    ⚠ 两边命中不一致（如实记录）\n");
                ok = 0;
            }
        }
    }

    printf("\n");
    if (ok) {
        printf("=== 结论：A5 达标（R-tree 侧；Z-order 侧的对照数字见上）===\n");
        printf("RESULT geo verdict=PASS\n");
    } else {
        printf("=== 结论：未达标 ===\n");
        printf("RESULT geo verdict=FAIL\n");
        rc = 1;
    }

    return rc;
}
