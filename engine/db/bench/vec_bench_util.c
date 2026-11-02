/*
 * vec_bench_util.c — 向量模态基准的公共部件（v1.0 步骤 6）
 *
 * 见 vec_bench.h 的说明：这一份放"几乎不变"的东西。
 *
 * 【随机数为什么要固定种子】
 *
 * 基准的可比性要求"同样的数据"上比较。
 * 随机数据每次都不一样，recall 的波动就分不清是代码变了还是数据变了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "vec_bench.h"

/* ---------------- 随机数 ---------------- */

u64 g_rng = 0x243F6A8885A308D3ull;

u64 rng_next(void)
{
    u64 z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

float rng_f(void)
{
    return (float)((rng_next() >> 40) & 0xFFFFFFu) / 16777216.0f;
}

/* ---------------- 数据生成 ---------------- */

/*
 * 两种数据形态。
 *
 * 【为什么要有两种】
 *
 * **均匀随机**（每个分量独立 U(0,1)）是图索引的**最坏情况**：
 * 高维下所有向量的距离都差不多（"维度灾难"），
 * "近邻"这件事本身就没有明确的结构可言 ——
 * 论文里 ANN 的 recall 数字几乎都是在**有聚簇结构**的真实数据上测的。
 *
 * **聚簇**（K 个中心 + 扰动）接近真实 embedding 的形态：
 * 同类的向量挨在一起，图索引正好靠这个导航。
 *
 * 只报一种都会失真：
 *   - 只报均匀 → 显得 HNSW 不行，其实是数据没有结构
 *   - 只报聚簇 → 显得 HNSW 很好，掩盖了"高维均匀数据"这个真实难点
 */
void gen_uniform(float *v, u32 n, u16 dim)
{
    for (u32 i = 0; i < n * dim; i++)
        v[i] = rng_f();
}

/*
 * 聚簇数据：K 个中心，每个向量 = 中心 + 扰动。
 *
 * 扰动用两个均匀随机数相减（近似三角分布）——
 * 比均匀噪声更接近真实 embedding 的分布（中心附近更密）。
 */
void gen_centers(float *centers, u32 ncenters, u16 dim)
{
    gen_uniform(centers, ncenters, dim);
}

void gen_clustered_around(const float *centers, u32 ncenters,
                          float *v, u32 n, u16 dim)
{
    for (u32 i = 0; i < n; i++) {
        u32 c = (u32)(rng_next() % ncenters);
        for (u16 d = 0; d < dim; d++) {
            float a = rng_f();
            float b = rng_f();
            float noise = (a + b - 1.0f) * 0.12f;
            v[(size_t)i * dim + d] = centers[(size_t)c * dim + d] + noise;
        }
    }
}

/* ---------------- 计时 / 文件 ---------------- */

double now_us(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1e6 + (double)ts.tv_nsec / 1e3;
}

u64 file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fclose(f);
    return (n > 0) ? (u64)n : 0;
}

/* ---------------- 延迟样本 ---------------- */

void samples_init(Samples *s, u32 cap)
{
    s->us = (double *)calloc(cap, sizeof(double));
    s->n = 0;
    s->cap = cap;
}

void samples_free(Samples *s)
{
    free(s->us);
    s->us = NULL;
    s->n = 0;
    s->cap = 0;
}

static int cmp_d(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;
    if (x < y)
        return -1;
    if (x > y)
        return 1;
    return 0;
}

double pct(Samples *s, double p)
{
    if (s->n == 0)
        return 0.0;
    qsort(s->us, s->n, sizeof(double), cmp_d);
    u32 idx = (u32)(p * (double)(s->n - 1) + 0.5);
    if (idx >= s->n)
        idx = s->n - 1;
    return s->us[idx];
}

/* ---------------- 精确 top-k（ground truth） ---------------- */

typedef struct {
    u32   id;
    float d;
} Cand;

/* 最大堆：堆顶是当前最远的那个 */
static void heap_push_max(Cand *h, u32 *hn, u32 cap, u32 id, float d)
{
    if (*hn < cap) {
        h[*hn].id = id;
        h[*hn].d = d;
        (*hn)++;
        u32 j = *hn - 1;
        while (j > 0) {
            u32 p = (j - 1) / 2;
            if (h[p].d >= h[j].d)
                break;
            Cand t = h[p];
            h[p] = h[j];
            h[j] = t;
            j = p;
        }
    } else if (d < h[0].d) {
        h[0].id = id;
        h[0].d = d;
        u32 j = 0;
        for (;;) {
            u32 l = 2 * j + 1;
            u32 r = l + 1;
            u32 best = j;
            if (l < *hn && h[l].d > h[best].d)
                best = l;
            if (r < *hn && h[r].d > h[best].d)
                best = r;
            if (best == j)
                break;
            Cand t = h[j];
            h[j] = h[best];
            h[best] = t;
            j = best;
        }
    }
}

/*
 * 堆排序：最大堆 → **升序**。
 *
 * 【别搞反】
 *
 * 最大堆做堆排序（每轮把堆顶的最大换到末尾、堆大小减一），
 * 结果是升序 —— 最大的被换到了数组末尾，最小的留在开头。
 * 所以直接按顺序抄就行，不需要再倒一次。
 *
 * （v1.0 步骤 5 的 db_vec_knn_exact 在这里踩过一次：
 *  先忘了排、再倒着抄，两处都错 —— 结果是乱的。
 *  集合对、顺序错，普通的断言验不出来。）
 */
static void heap_sort_asc(Cand *h, u32 n)
{
    for (u32 i = n; i > 0; i--) {
        Cand t = h[0];
        h[0] = h[i - 1];
        h[i - 1] = t;

        u32 size = i - 1;
        u32 j = 0;
        while (j < size) {
            u32 l = 2 * j + 1;
            u32 r = l + 1;
            u32 best = j;
            if (l < size && h[l].d > h[best].d)
                best = l;
            if (r < size && h[r].d > h[best].d)
                best = r;
            if (best == j)
                break;
            Cand t2 = h[j];
            h[j] = h[best];
            h[best] = t2;
            j = best;
        }
    }
}

/*
 * 精确 top-k。out_ids / out_d 各要能放 k 个。
 *
 * 复杂度 O(n × dim + n × log k) —— 距离只算一遍。
 * （第一版在比较时重算距离，是 O(n × k × dim)，慢 10 倍。）
 */
void topk_exact(const float *vecs, u32 n, u16 dim,
                const float *q, u32 k, u32 *out_ids, float *out_d)
{
    Cand *heap = (Cand *)calloc(k, sizeof(Cand));
    if (!heap)
        return;

    u32 hn = 0;
    for (u32 i = 0; i < n; i++) {
        const float *v = vecs + (size_t)i * dim;
        float d = 0.0f;
        for (u16 x = 0; x < dim; x++) {
            float t = v[x] - q[x];
            d += t * t;
        }
        heap_push_max(heap, &hn, k, i, d);
    }

    heap_sort_asc(heap, hn);

    for (u32 i = 0; i < hn; i++) {
        out_ids[i] = heap[i].id;
        out_d[i] = heap[i].d;
    }
    free(heap);
}

/* ---------------- 数据落盘 ---------------- */

int dump_data(const char *path, u16 dim, u32 nvec, u32 nq, u32 k,
              const float *vecs, const float *queries)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    u32 hdr[4] = { dim, nvec, nq, k };
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(vecs, sizeof(float), (size_t)nvec * dim, f);
    fwrite(queries, sizeof(float), (size_t)nq * dim, f);
    fclose(f);
    return 0;
}

int dump_result(const char *path, u32 nq, u32 k, u16 ef, float c_recall,
                const u16 *curve_efs, const float *curve_recalls, u32 n_curve,
                const u32 *gt_ids, const u32 *ann_ids,
                const float *ann_dists)
{
    FILE *f = fopen(path, "wb");
    if (!f)
        return -1;

    u32 hdr[3] = { nq, k, (u32)ef };
    fwrite(hdr, sizeof(hdr), 1, f);
    fwrite(&c_recall, sizeof(float), 1, f);
    fwrite(&n_curve, sizeof(u32), 1, f);
    if (n_curve) {
        fwrite(curve_efs, sizeof(u16), n_curve, f);
        fwrite(curve_recalls, sizeof(float), n_curve, f);
    }
    fwrite(gt_ids, sizeof(u32), (size_t)nq * k, f);
    fwrite(ann_ids, sizeof(u32), (size_t)nq * k, f);
    fwrite(ann_dists, sizeof(float), (size_t)nq * k, f);
    fclose(f);
    return 0;
}
