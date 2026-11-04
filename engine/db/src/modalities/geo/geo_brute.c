#include "modalities/geo/geo_brute.h"

#include <stdlib.h>

/*
 * 暴力基准实现（v1.4 步骤 1）。
 *
 * 【为什么不写成"随手一算"的函数】
 *
 * 基准实现是**验收尺子**，尺子本身必须没有争议：
 *   - 距离一律走 geo_dist2（与 R-tree 叶子条目同一套算术）；
 *   - 窗口判定一律走 mbr_contains_point（闭区间，与剪枝同一套语义）；
 *   - 并列一律按 row_id 升序。
 * 这样"两边不一样"就一定是**树错了**，而不是"尺子歪了"。
 */

/* ---------------- 窗口 ---------------- */

int geo_brute_window(const GeoPoint *pts, u32 n, const Mbr *win,
                     u32 *out_ids, u32 max_out, u32 *n_out)
{
    u32 n_hit = 0;
    u32 i;

    if (pts == NULL || win == NULL || out_ids == NULL || n_out == NULL)
        return E_INVAL;
    if (n > 0 && max_out == 0)
        return E_NOSPACE;

    for (i = 0; i < n; i++) {
        if (mbr_contains_point(win, pts[i].x, pts[i].y)) {
            if (n_hit >= max_out)
                return E_NOSPACE;   /* 静默截断会掩盖"命中数算错" */
            out_ids[n_hit++] = pts[i].row_id;
        }
    }
    *n_out = n_hit;
    return OK;
}

/* ---------------- KNN ---------------- */

/*
 * 排序用的中间记录：距离 + row_id。
 *
 * 【为什么不直接排点数组】
 *
 * 排点数组会把坐标也搬来搬去，而且排序后调用方拿到的顺序
 * 与"距离顺序"绑死。这里单独排 (d2, row_id)，输出仍然只给 row_id。
 */
typedef struct {
    float d2;
    u32   row_id;
} GeoHit;

/*
 * 比较器：距离升序，并列时 row_id 升序。
 *
 * 用平方距离比较：sqrt 单调，排序结果与真实距离完全一致，
 * 但省掉每次比较一次 sqrt。
 */
static int geo_hit_cmp(const void *pa, const void *pb)
{
    const GeoHit *a = (const GeoHit *)pa;
    const GeoHit *b = (const GeoHit *)pb;

    if (a->d2 < b->d2)
        return -1;
    if (a->d2 > b->d2)
        return 1;
    if (a->row_id < b->row_id)
        return -1;
    if (a->row_id > b->row_id)
        return 1;
    return 0;
}

int geo_brute_knn(const GeoPoint *pts, u32 n, float qx, float qy, u32 k,
                  u32 *out_ids, u32 max_out, u32 *n_out)
{
    GeoHit *hits = NULL;
    u32 i;
    u32 nres;

    if (pts == NULL || out_ids == NULL || n_out == NULL)
        return E_INVAL;
    if (k == 0 || n == 0) {
        *n_out = 0;
        return OK;
    }

    /*
     * 结果个数 = min(k, 点数)。
     *
     * 【为什么不拿 k 去比 max_out】
     *
     * "要 k 个"是**请求**，"只有 n 个点"是**事实**。
     * 问 10 个而库里只有 2 个点，返回 2 个是对的，不是错误。
     * 真正的容量问题是"要装下的结果数 > 缓冲区" —— 那才是 E_NOSPACE。
     */
    nres = (k < n) ? k : n;
    if (nres > max_out)
        return E_NOSPACE;           /* 装不下 → 报错，不静默截断 */

    hits = (GeoHit *)malloc(sizeof(GeoHit) * (size_t)n);
    if (hits == NULL)
        return E_NOSPACE;

    for (i = 0; i < n; i++) {
        hits[i].d2 = geo_dist2(qx, qy, pts[i].x, pts[i].y);
        hits[i].row_id = pts[i].row_id;
    }
    qsort(hits, (size_t)n, sizeof(GeoHit), geo_hit_cmp);

    for (i = 0; i < nres; i++)
        out_ids[i] = hits[i].row_id;

    free(hits);
    *n_out = nres;
    return OK;
}

/* ---------------- 排序工具 ---------------- */

static int geo_u32_cmp(const void *pa, const void *pb)
{
    u32 a = *(const u32 *)pa;
    u32 b = *(const u32 *)pb;

    if (a < b)
        return -1;
    if (a > b)
        return 1;
    return 0;
}

void geo_ids_sort(u32 *ids, u32 n)
{
    if (ids == NULL || n < 2)
        return;
    qsort(ids, (size_t)n, sizeof(u32), geo_u32_cmp);
}
