/*
 * zorder.c —— Z-order（Morton 码 + 固定网格桶）索引
 *
 * 【这一层在 v1.4 里的角色】
 *
 * 它**不是**要取代 R-tree，而是给 A5 验收当**对照**：
 * 同样一批点、同样的窗口，两条路各要读多少 IO 单位？
 * R-tree 的 MBR 是**跟着数据长**的（聚簇友好、代价是要分裂要维护）；
 * Z-order 的格子是**固定的**（结构极简、代价是不管数据聚在哪里）。
 * 谁省 IO 取决于分布 —— 所以必须实测，不能靠"听说"。
 *
 * 【结构】
 *
 *   点 → 量化成 u32 坐标 → 落到固定网格的一个格子 → 格子按 Morton 序排 →
 *   每 GEO_Z_BUCKET_CAP 个点凑一个桶（桶 = 一个页的 IO 单位）。
 *
 *   格子坐标 = (qx >> shift, qy >> shift)，shift = 32 - cell_bits；
 *   格子的 Morton 序 = geo_zorder_encode(格子坐标)。
 *
 * 【为什么"桶"能代表 IO】
 *
 * 一个页 4096 字节，一个桶条目 12 字节（u64 码 + u32 行号）⇒ 一页装 341 条。
 * 所以"扫一个桶"≈"读一个页"。这是**模型**，不是实测的磁盘 IO ——
 * 与 R-tree 那边用的 `rtree_page_reads()`（逻辑页读）同一量纲，可以对照。
 *
 * 【查询为什么是"超集"】
 *
 * 窗口落在哪些格子里，就把那些格子**整桶**扫一遍，再按矩形逐点过滤。
 * 格子只被部分覆盖时，桶里会带进一些**不在**窗口里的点 ⇒ 候选集 ⊇ 命中集。
 * 好处是**绝不漏点**（R4 的超集断言就是查这一条）；
 * 代价是格子越粗、超集越大 ⇒ 格子层级（cell_bits）是个需要调的旋钮。
 */

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/zorder.h"

/* ---------------- 桶条目与桶 ---------------- */

typedef struct {
    u64 code;    /* 点的 Morton 码（量化坐标的交错） */
    u32 row_id;  /* 行号 */
} GeoZEntry;

typedef struct {
    u32 cell;    /* 格子坐标的 Morton 码（格子在 Z 序里的位置） */
    u32 first;   /* 桶内第一条在 entries[] 里的下标 */
    u32 count;   /* 桶内条数（1..GEO_Z_BUCKET_CAP） */
} GeoZBucket;

struct GeoZIndex {
    GeoZEntry  *entries;    /* 按桶序排好（格子 Morton 序，桶内按点的码） */
    u32         n_entries;
    GeoZBucket *buckets;
    u32         n_buckets;
    u32         cell_bits;
    u32         shift;      /* 32 - cell_bits；cell_bits==0 时为 32 */
};

/* ---------------- 位操作（Morton 用的经典 "split by 2"） ---------------- */

static u64 split_by_2(u32 v)
{
    u64 x = (u64)v;

    x = (x | (x << 16)) & 0x0000FFFF0000FFFFull;
    x = (x | (x << 8)) & 0x00FF00FF00FF00FFull;
    x = (x | (x << 4)) & 0x0F0F0F0F0F0F0F0Full;
    x = (x | (x << 2)) & 0x3333333333333333ull;
    x = (x | (x << 1)) & 0x5555555555555555ull;

    return x;
}

static u32 compact_by_2(u64 x)
{
    x &= 0x5555555555555555ull;
    x = (x ^ (x >> 1)) & 0x3333333333333333ull;
    x = (x ^ (x >> 2)) & 0x0F0F0F0F0F0F0F0Full;
    x = (x ^ (x >> 4)) & 0x00FF00FF00FF00FFull;
    x = (x ^ (x >> 8)) & 0x0000FFFF0000FFFFull;
    x = (x ^ (x >> 16)) & 0x00000000FFFFFFFFull;

    return (u32)x;
}

/* ---------------- 量化 ---------------- */

u32 geo_zorder_quantize(double v, double lo, double hi)
{
    double t;
    double q;

    /* 退化区间：全部落进同一个格子（坐标 0） */
    if (!(hi > lo)) {
        return 0u;
    }

    /* 越界 ⇒ 截断到两端（量化是投影，投影到边界之外就落在边界上） */
    if (!(v > lo)) {
        return 0u;
    }
    if (v >= hi) {
        return 0xFFFFFFFFu;
    }

    t = (v - lo) / (hi - lo);
    q = t * 4294967295.0;
    if (q < 0.0) {
        q = 0.0;
    }
    if (q > 4294967295.0) {
        q = 4294967295.0;
    }

    return (u32)q;
}

double geo_zorder_dequantize(u32 q, double lo, double hi)
{
    if (!(hi > lo)) {
        return lo;
    }

    return lo + (hi - lo) * ((double)q / 4294967295.0);
}

/* ---------------- Morton 编码 ---------------- */

/*
 * 约定：x 的第 i 位 → 码的第 2i 位；y 的第 i 位 → 码的第 2i+1 位。
 */
u64 geo_zorder_encode(u32 x, u32 y)
{
    return split_by_2(x) | (split_by_2(y) << 1);
}

void geo_zorder_decode(u64 code, u32 *x, u32 *y)
{
    if (x != NULL) {
        *x = compact_by_2(code);
    }
    if (y != NULL) {
        *y = compact_by_2(code >> 1);
    }
}

/* ---------------- 建索引 ---------------- */

/* 排序用的键：格子码 → 点的码 → 行号（逐级兜底，保证结果确定） */
typedef struct {
    u64 cell_code;
    u64 code;
    u32 row_id;
} GeoZKey;

static int cmp_key(const void *a, const void *b)
{
    const GeoZKey *ka = (const GeoZKey *)a;
    const GeoZKey *kb = (const GeoZKey *)b;

    if (ka->cell_code != kb->cell_code) {
        return (ka->cell_code < kb->cell_code) ? -1 : 1;
    }
    if (ka->code != kb->code) {
        return (ka->code < kb->code) ? -1 : 1;
    }
    if (ka->row_id != kb->row_id) {
        return (ka->row_id < kb->row_id) ? -1 : 1;
    }

    return 0;
}

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

/* 格子坐标（cell_bits == 0 时全部落在格子 0） */
static u32 cell_of(u32 q, u32 shift)
{
    if (shift >= 32u) {
        return 0u;
    }

    return q >> shift;
}

GeoZIndex *geo_zorder_build(const u32 *qx, const u32 *qy, const u32 *row_ids,
                            u32 n, u32 cell_bits)
{
    GeoZIndex *ix;
    GeoZKey *keys;
    u32 i;

    if (cell_bits > 16u) {
        return NULL;
    }
    if (n > 0u && (qx == NULL || qy == NULL || row_ids == NULL)) {
        return NULL;
    }

    ix = (GeoZIndex *)calloc(1, sizeof(*ix));
    if (ix == NULL) {
        return NULL;
    }

    ix->cell_bits = cell_bits;
    ix->shift = 32u - cell_bits;
    ix->n_entries = n;

    if (n == 0u) {
        return ix;       /* 空索引：没有条目，也没有桶 */
    }

    ix->entries = (GeoZEntry *)calloc(n, sizeof(GeoZEntry));
    ix->buckets = (GeoZBucket *)calloc(n, sizeof(GeoZBucket));
    keys = (GeoZKey *)calloc(n, sizeof(GeoZKey));
    if (ix->entries == NULL || ix->buckets == NULL || keys == NULL) {
        free(keys);
        free(ix->buckets);
        free(ix->entries);
        free(ix);
        return NULL;
    }

    for (i = 0; i < n; i++) {
        u32 cx = cell_of(qx[i], ix->shift);
        u32 cy = cell_of(qy[i], ix->shift);

        keys[i].cell_code = geo_zorder_encode(cx, cy);
        keys[i].code = geo_zorder_encode(qx[i], qy[i]);
        keys[i].row_id = row_ids[i];
    }

    qsort(keys, n, sizeof(GeoZKey), cmp_key);

    /*
     * 分桶：同格子的条目在数组里是连续的；
     * 一个格子点数超过容量就拆成多个桶（桶仍然是同格子 ⇒ 查询时整桶看）。
     *
     * ⚠️ **坑 M5-8（2026-11-04 修）**：装桶计数**必须**用局部变量 `fill` 记，
     * 不能读 `ix->buckets[b].count`。
     *
     * 第一版是这么写的：
     *     if (... || ix->buckets[b].count >= GEO_Z_BUCKET_CAP) { ... ix->buckets[b].count = 0; }
     * 而 `count` **只在开下一个桶时才被结算**（`count = i - first`），
     * 也就是说在判据那一刻它永远是 0 ⇒ "装满了就拆桶"这条判据**从来没生效过**：
     * 一个格子塞 11111 个点也只开一个桶，"一桶 ≈ 一页"这个模型直接作废
     * （实测桶数 == 非空格数，一桶装了 33 倍于容量的点）。
     *
     * 这类 bug 之所以能溜过去，是因为它**不产生任何错误结果**：
     * 命中集照样对（超集还是超集），只是 IO 口径被低估了 33 倍。
     * —— "结果对"不等于"度量对"，度量本身也要单独验。
     */
    {
        u32 b = 0;
        u32 fill = 0;      /* 当前桶已经装了多少条（**局部**计数，见上面的坑） */

        for (i = 0; i < n; i++) {
            ix->entries[i].code = keys[i].code;
            ix->entries[i].row_id = keys[i].row_id;

            if (i == 0u || keys[i].cell_code != keys[i - 1u].cell_code ||
                fill >= GEO_Z_BUCKET_CAP) {
                if (i > 0u) {
                    ix->buckets[b].count = fill;
                    b++;
                }
                ix->buckets[b].cell = (u32)keys[i].cell_code;
                ix->buckets[b].first = i;
                fill = 0;
            }
            fill++;
        }
        ix->buckets[b].count = fill;
        b++;
        ix->n_buckets = b;
    }

    free(keys);

    return ix;
}

void geo_zorder_free(GeoZIndex *ix)
{
    if (ix == NULL) {
        return;
    }
    free(ix->buckets);
    free(ix->entries);
    free(ix);
}

/* ---------------- 只读接口 ---------------- */

u32 geo_zorder_n_points(const GeoZIndex *ix)
{
    return (ix == NULL) ? 0u : ix->n_entries;
}

u32 geo_zorder_n_buckets(const GeoZIndex *ix)
{
    return (ix == NULL) ? 0u : ix->n_buckets;
}

u32 geo_zorder_cell_bits(const GeoZIndex *ix)
{
    return (ix == NULL) ? 0u : ix->cell_bits;
}

u32 geo_zorder_n_cells(const GeoZIndex *ix)
{
    u32 n = 0;
    u32 i;

    if (ix == NULL) {
        return 0u;
    }
    for (i = 0; i < ix->n_buckets; i++) {
        /* 同一个格子可能被拆成多个桶 ⇒ 按桶数算格子数会多算，
         * 这里按"桶首条目所在格子"去重：桶是按格子连续排的，
         * 相邻两个桶格子不同才算一个新格子。 */
        if (i == 0u || ix->buckets[i].cell != ix->buckets[i - 1u].cell) {
            n++;
        }
    }

    return n;
}

/* ---------------- 查询 ---------------- */

/*
 * 扫一遍：命中集 / 候选集。
 *
 * want_hits_only = 1 ⇒ 只留落在窗口里的点（命中集）；
 * want_hits_only = 0 ⇒ 留下扫到的**全部**点（候选集，R4 的超集断言用）。
 */
static int zorder_scan(const GeoZIndex *ix, u32 x0, u32 x1, u32 y0, u32 y1,
                        u32 *out, u32 max_out, u32 *n_out,
                        int want_hits_only,
                        u64 *buckets_touched, u64 *points_scanned)
{
    u32 *tmp;
    u32 cnt = 0;
    u32 i;
    u32 cx0, cx1, cy0, cy1;

    if (ix == NULL || n_out == NULL) {
        return E_INVAL;
    }
    if (x1 < x0 || y1 < y0) {
        *n_out = 0;
        if (buckets_touched != NULL) {
            *buckets_touched = 0;
        }
        if (points_scanned != NULL) {
            *points_scanned = 0;
        }
        return E_INVAL;
    }

    cx0 = cell_of(x0, ix->shift);
    cx1 = cell_of(x1, ix->shift);
    cy0 = cell_of(y0, ix->shift);
    cy1 = cell_of(y1, ix->shift);

    tmp = (u32 *)calloc(ix->n_entries ? ix->n_entries : 1u, sizeof(u32));
    if (tmp == NULL) {
        return E_NOSPACE;
    }

    for (i = 0; i < ix->n_buckets; i++) {
        const GeoZBucket *bk = &ix->buckets[i];
        u32 bx, by;
        u32 k;

        geo_zorder_decode((u64)bk->cell, &bx, &by);
        if (bx < cx0 || bx > cx1 || by < cy0 || by > cy1) {
            continue;       /* 格子与窗口不相交 ⇒ 整个桶都不用看 */
        }

        if (buckets_touched != NULL) {
            (*buckets_touched)++;
        }

        for (k = 0; k < bk->count; k++) {
            const GeoZEntry *e = &ix->entries[bk->first + k];
            u32 px, py;

            if (points_scanned != NULL) {
                (*points_scanned)++;
            }

            if (want_hits_only) {
                geo_zorder_decode(e->code, &px, &py);
                if (px < x0 || px > x1 || py < y0 || py > y1) {
                    continue;       /* 格子只是部分覆盖 ⇒ 这一点不在窗口里 */
                }
            }
            tmp[cnt++] = e->row_id;
        }
    }

    if (cnt > max_out) {
        free(tmp);
        *n_out = cnt;
        return E_NOSPACE;       /* 不静默截断 */
    }

    /* 输出按 row_id 升序（与暴力基准逐位对照时两边顺序一致） */
    if (cnt > 1u) {
        qsort(tmp, cnt, sizeof(u32), cmp_u32);
    }
    if (out != NULL && cnt > 0u) {
        memcpy(out, tmp, (size_t)cnt * sizeof(u32));
    }
    free(tmp);

    *n_out = cnt;

    return OK;
}

int geo_zorder_window(const GeoZIndex *ix,
                      u32 x0, u32 x1, u32 y0, u32 y1,
                      u32 *out_row_ids, u32 max_out, u32 *n_out,
                      u64 *buckets_touched, u64 *points_scanned)
{
    return zorder_scan(ix, x0, x1, y0, y1, out_row_ids, max_out, n_out, 1,
                        buckets_touched, points_scanned);
}

int geo_zorder_window_io(const GeoZIndex *ix,
                          u32 x0, u32 x1, u32 y0, u32 y1,
                          u64 *buckets_touched, u64 *points_scanned)
{
    u32 n = 0;

    /* 这里只量 IO，不要结果 ⇒ 容量给到"全部点"，
     * 否则候选集比 max_out 大时会误报 E_NOSPACE。 */
    return zorder_scan(ix, x0, x1, y0, y1, NULL,
                        (ix == NULL) ? 0u : ix->n_entries, &n, 0,
                        buckets_touched, points_scanned);
}

int geo_zorder_window_candidates(const GeoZIndex *ix,
                                  u32 x0, u32 x1, u32 y0, u32 y1,
                                  u32 *out_row_ids, u32 max_out, u32 *n_out,
                                  u64 *buckets_touched, u64 *points_scanned)
{
    return zorder_scan(ix, x0, x1, y0, y1, out_row_ids, max_out, n_out, 0,
                        buckets_touched, points_scanned);
}
