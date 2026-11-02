/*
 * hnsw_search.c — HNSW 的查询路径（v1.0 步骤 3）
 *
 * 和 hnsw.c（构建）分开：构建关心"图连得好不好"，
 * 查询关心"走得多快、召回多准"。两者的旋钮也不同
 * （构建用 ef_construction，查询用 ef）。
 *
 * 这里还放着**暴力扫**（hnsw_bruteforce）——
 * 它是 recall 的唯一可信基准，也是将来 db_vec_knn_exact 的实现。
 */

#include "modalities/vector/hnsw_internal.h"

/* ---------------- 查询 ---------------- */

int hnsw_search(Hnsw *h, const float *q, u32 k, u16 ef,
                HnswHit *out, u32 *n_out)
{
    if (!h || !q || !out)
        return E_INVAL;

    if (n_out)
        *n_out = 0;
    if (h->n == 0 || k == 0)
        return OK;

    /* ef 小于 k 时最多只能返回 ef 个 —— 悄悄抬到 k，别让调用方拿到空结果 */
    if (ef < k)
        ef = (u16)k;

    u32 ep = h->entry;

    /* 上层：单点贪心下降 */
    for (int lc = (int)h->max_level; lc > 0; lc--)
        ep = hnsw_greedy_step(h, q, ep, (u16)lc);

    /* 记下第 0 层的实际起点（诊断用；见 hnsw_internal.h 的说明） */
    h->last_entry = ep;

    /* 第 0 层：ef 宽搜索 */
    int rc = hnsw_search_layer(h, q, ep, ef, 0, &h->res);
    if (rc != OK)
        return rc;

    u32 ncand = h->res.n;
    rc = scratch_reserve(h, ncand);
    if (rc != OK)
        return rc;

    for (u32 i = 0; i < ncand; i++)
        h->scratch[i] = h->res.a[i];
    qsort(h->scratch, ncand, sizeof(HItem), cmp_asc);

    u32 n = (ncand < k) ? ncand : k;
    for (u32 i = 0; i < n; i++) {
        out[i].id = h->scratch[i].id;
        out[i].dist = h->scratch[i].d;
    }

    if (n_out)
        *n_out = n;
    return OK;
}

/* ---------------- 诊断口：搜索成本 ---------------- */

u32 hnsw_last_visited(const Hnsw *h)
{
    return h ? h->visits : 0;
}

/*
 * 最近一次搜索在第 0 层的实际起点（诊断用）。
 *
 * 【怎么用它判断"上层有没有参与"】
 *
 *     u32 raw = hnsw_entry(h);        索引的入口点
 *     hnsw_search(h, q, 10, 64, hits, &nh);
 *     u32 used = hnsw_last_entry(h);  下降之后的起点
 *     used != raw → 上层确实把起点挪了
 *
 * 上层被关掉时 used == raw（起点就是入口点）—— 一眼可辨。
 */
u32 hnsw_last_entry(const Hnsw *h)
{
    return h ? h->last_entry : 0;
}

/*
 * 从指定起点做一次第 0 层搜索，返回访问的节点数。
 *
 * 【它和正常搜索的区别】
 *
 * 正常搜索：上层贪心下降 → 第 0 层搜索。
 * 这个函数：**跳过上层**，直接从 start 搜第 0 层。
 *
 * 两者的差值就是上层的贡献 —— 不是在 recall 上，是在**工作量**上。
 */
u32 hnsw_visited_from(Hnsw *h, const float *q, u32 start, u16 ef)
{
    if (!h || !q || h->n == 0 || start >= h->n)
        return 0;
    if (ef == 0)
        ef = 1;

    /* 用独立的工作区，别把上一次的候选集搅乱 */
    Heap res;
    memset(&res, 0, sizeof(res));
    res.max = true;      /* 结果集是最大堆（堆顶 = 最差的那个） */
    /*
     * 【这个标志必须置位】
     *
     * 置错了，h_top() 返回的是"最好的"而不是"最差的" ——
     * 搜索的剪枝条件整个反了，访问的节点数也会变。
     * 第一版忘了置，测出来的两个数字对不上（58912 vs 52335），
     * 而同一份数据同一段代码本该给出**完全相同**的结果。
     */

    int rc = hnsw_search_layer(h, q, start, ef, 0, &res);
    u32 n = h->visits;

    free(res.a);
    return (rc == OK) ? n : 0;
}

int hnsw_bruteforce(const Hnsw *h, const float *q, u32 k,
                    HnswHit *out, u32 *n_out)
{
    if (!h || !q || !out)
        return E_INVAL;

    if (n_out)
        *n_out = 0;
    if (h->n == 0 || k == 0)
        return OK;

    /*
     * 用一个容量 k 的最大堆维护"当前最好的 k 个"。
     *
     * 不用"全算完再排序"：10 万点的距离数组要 800KB，
     * 而堆只要 k 个元素（k=10 就是 80 字节）。
     * 顺便这也是精确查询该有的写法 —— 它将来要当 db_vec_knn_exact 用。
     */
    Heap top;
    memset(&top, 0, sizeof(top));
    top.max = true;

    for (u32 i = 0; i < h->n; i++) {
        float d = vec_dist(h, q, hnsw_vec(h, i));
        if (top.n < k) {
            if (h_push(&top, d, i) != OK) {
                free(top.a);
                return E_IO;
            }
        } else if (d < h_top(&top).d) {
            h_pop(&top);
            if (h_push(&top, d, i) != OK) {
                free(top.a);
                return E_IO;
            }
        }
    }

    /* 最大堆全弹出来是降序，倒着放就是升序 */
    u32 n = top.n;
    for (u32 i = n; i > 0; i--) {
        HItem it = h_pop(&top);
        out[i - 1].id = it.id;
        out[i - 1].dist = it.d;
    }
    free(top.a);

    if (n_out)
        *n_out = n;
    return OK;
}

