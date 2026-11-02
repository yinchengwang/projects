/*
 * test_hnsw.c — HNSW 构建与查询单元验收（v1.0 步骤 3）
 *
 * 7 项：
 *   1. 小规模（200 点）：recall@10 应该接近满分 —— 图连得对不对的第一道关
 *   2. 结构不变量：邻居数不超上限、层级分布符合 1/M、入口点确实在最高层
 *   3. 中等规模（1 万 × 16 维）：**recall@10 ≥ 0.9**（PLAN 的烟雾测试线）
 *   4. ef 单调性：ef 越大召回越高（这是 HNSW 的核心旋钮）
 *   5. 暴力扫 == 朴素实现（连基准本身都要验，否则 recall 无从谈起）
 *   6. 增量插入：边插边查，recall 不塌
 *   7. 边界：空索引 / k > n / 内积模式
 *
 * 【为什么第 5 项不能省】
 *
 * recall 是"ANN 结果 ∩ 真值 / k"。如果**真值本身算错了**，
 * recall 会给出一个看起来合理的数字（比如 0.93），
 * 然后所有人都在调 HNSW 的参数 —— 而问题在基准。
 *
 * 所以基准要用一份**完全不同写法**的朴素实现来验：
 * 一个用堆，一个用全排序，两种实现结果一致才算数。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/vector/hnsw.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 随机数（固定种子：结果可复现） ---------------- */

static u64 g_rng = 0x1234567890ABCDEFull;

static u64 rng_next(void)
{
    u64 z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

/* [0,1) 均匀 */
static float rng_f(void)
{
    return (float)((rng_next() >> 40) & 0xFFFFFFu) / 16777216.0f;
}

static void gen_vecs(float *v, u32 n, u16 dim)
{
    for (u32 i = 0; i < n; i++)
        for (u16 d = 0; d < dim; d++)
            v[(size_t)i * dim + d] = rng_f();
}

/* ---------------- recall ---------------- */

static double recall_at_k(Hnsw *h, const float *queries, u32 nq, u16 dim,
                          u32 k, u16 ef)
{
    HnswHit a[64];
    HnswHit g[64];
    u32 na = 0;
    u32 ng = 0;
    u32 hit = 0;
    u32 total = 0;

    for (u32 i = 0; i < nq; i++) {
        const float *q = queries + (size_t)i * dim;
        CHECK(hnsw_search(h, q, k, ef, a, &na) == OK);
        CHECK(hnsw_bruteforce(h, q, k, g, &ng) == OK);

        for (u32 x = 0; x < na; x++) {
            for (u32 y = 0; y < ng; y++) {
                if (a[x].id == g[y].id) {
                    hit++;
                    break;
                }
            }
        }
        total += ng;
    }

    return total ? (double)hit / (double)total : 0.0;
}

/* ---------------- 1. 小规模：图连对了吗 ---------------- */

static void test_small(void)
{
    printf("  1. 小规模 200 点：recall@10 应接近满分\n");

    const u16 dim = 8;
    const u32 n = 200;

    float *v = (float *)malloc((size_t)n * dim * sizeof(float));
    CHECK(v != NULL);
    gen_vecs(v, n, dim);

    Hnsw *h = hnsw_create(dim, HNSW_L2, 16, 200, 42);
    CHECK(h != NULL);

    for (u32 i = 0; i < n; i++) {
        u32 id = 999;
        CHECK(hnsw_add(h, v + (size_t)i * dim, &id) == OK);
        CHECK(id == i);          /* id 必须等于插入顺序 */
    }
    CHECK(hnsw_count(h) == n);

    /* 用点集里的一部分当查询 */
    double r = recall_at_k(h, v, 40, dim, 10, 64);
    printf("    （recall@10 = %.3f）\n", r);
    CHECK(r >= 0.99);

    free(v);
    hnsw_destroy(h);
}

/* ---------------- 2. 结构不变量 ---------------- */

static void test_structure(void)
{
    printf("  2. 结构不变量（邻居上限 / 层级分布 / 入口点）\n");

    const u16 dim = 8;
    const u32 n = 2000;
    const u16 m = 16;

    float *v = (float *)malloc((size_t)n * dim * sizeof(float));
    CHECK(v != NULL);
    gen_vecs(v, n, dim);

    Hnsw *h = hnsw_create(dim, HNSW_L2, m, 100, 7);
    CHECK(h != NULL);
    for (u32 i = 0; i < n; i++)
        CHECK(hnsw_add(h, v + (size_t)i * dim, NULL) == OK);

    /* 邻居数不超上限 */
    u32 buf[64];
    u32 bad = 0;
    u32 n_upper = 0;
    for (u32 i = 0; i < n; i++) {
        u16 lv = hnsw_level_of(h, i);
        if (lv > 0)
            n_upper++;

        for (u16 lc = 0; lc <= lv; lc++) {
            u32 cnt = hnsw_neighbors(h, i, lc, buf, 64);
            u16 lim = (lc == 0) ? (u16)(m * 2) : m;
            if (cnt > lim)
                bad++;
        }
    }
    CHECK(bad == 0);

    /*
     * 层级分布：P(level >= 1) = 1/M = 1/16。
     * 2000 个点 → 期望 ~125 个有上层。给足余量（±3 倍标准差）。
     */
    printf("    （有上层的节点 %u / %u，期望约 %u）\n",
           n_upper, n, n / m);
    CHECK(n_upper > n / m / 3);
    CHECK(n_upper < n / m * 3 + 20);

    /* 入口点必须真的在最高层 */
    u32 ep = hnsw_entry(h);
    CHECK(hnsw_level_of(h, ep) == hnsw_max_level(h));

    /* 每个节点的第 0 层至少要有 1 个邻居（除只有一个点时） */
    u32 isolated = 0;
    for (u32 i = 0; i < n; i++)
        if (hnsw_neighbors(h, i, 0, buf, 64) == 0)
            isolated++;
    CHECK(isolated == 0);

    free(v);
    hnsw_destroy(h);
}

/* ---------------- 3. 中等规模：recall 硬指标 ---------------- */

static Hnsw *g_big = NULL;
static float *g_bigv = NULL;
static const u16 BIG_DIM = 16;
static const u32 BIG_N = 10000;

static void test_recall_10k(void)
{
    printf("  3. 1 万 × 16 维：recall@10 ≥ 0.9\n");

    g_bigv = (float *)malloc((size_t)BIG_N * BIG_DIM * sizeof(float));
    CHECK(g_bigv != NULL);
    gen_vecs(g_bigv, BIG_N, BIG_DIM);

    g_big = hnsw_create(BIG_DIM, HNSW_L2, 16, 200, 2024);
    CHECK(g_big != NULL);

    for (u32 i = 0; i < BIG_N; i++)
        CHECK(hnsw_add(g_big, g_bigv + (size_t)i * BIG_DIM, NULL) == OK);

    CHECK(hnsw_count(g_big) == BIG_N);

    /* 用独立生成的 100 个查询（不是训练点，更接近真实用法） */
    float *q = (float *)malloc(100 * BIG_DIM * sizeof(float));
    CHECK(q != NULL);
    gen_vecs(q, 100, BIG_DIM);

    double r = recall_at_k(g_big, q, 100, BIG_DIM, 10, 64);
    printf("    （recall@10 = %.4f，M=16 efC=200 ef=64）\n", r);
    CHECK(r >= 0.90);

    /* 4. ef 单调性 */
    printf("  4. ef 单调性（ef 越大召回越高）\n");
    double r16 = recall_at_k(g_big, q, 30, BIG_DIM, 10, 16);
    double r64 = recall_at_k(g_big, q, 30, BIG_DIM, 10, 64);
    double r200 = recall_at_k(g_big, q, 30, BIG_DIM, 10, 200);
    printf("    （ef=16 → %.3f，ef=64 → %.3f，ef=200 → %.3f）\n",
           r16, r64, r200);

    CHECK(r64 >= r16);
    CHECK(r200 >= r64 - 1e-9);
    CHECK(r200 >= 0.95);

    free(q);
}

/* ---------------- 5. 暴力扫 == 朴素实现 ---------------- */

typedef struct {
    u32 id;
    float d;
} Pair;

static int cmp_pair(const void *a, const void *b)
{
    const Pair *x = (const Pair *)a;
    const Pair *y = (const Pair *)b;
    if (x->d < y->d)
        return -1;
    if (x->d > y->d)
        return 1;
    return 0;
}

static void test_bruteforce_truth(void)
{
    printf("  5. 暴力扫 == 朴素实现（连基准本身都要验）\n");

    /* 承接第 3 项建好的索引 */
    CHECK(g_big != NULL);

    const u32 k = 10;
    float q[16];
    for (u16 d = 0; d < BIG_DIM; d++)
        q[d] = rng_f();

    HnswHit g[16];
    u32 ng = 0;
    CHECK(hnsw_bruteforce(g_big, q, k, g, &ng) == OK);
    CHECK(ng == k);

    /*
     * 朴素实现：全部算完再全排序。
     * 和 hnsw_bruteforce 的"容量 k 的最大堆"是完全不同的写法，
     * 两种结果一致才说明基准可信。
     */
    Pair *all = (Pair *)malloc((size_t)BIG_N * sizeof(Pair));
    CHECK(all != NULL);
    for (u32 i = 0; i < BIG_N; i++) {
        all[i].id = i;
        const float *p = g_bigv + (size_t)i * BIG_DIM;
        float s = 0.0f;
        for (u16 d = 0; d < BIG_DIM; d++) {
            float t = p[d] - q[d];
            s += t * t;
        }
        all[i].d = s;
    }
    qsort(all, BIG_N, sizeof(Pair), cmp_pair);

    for (u32 i = 0; i < k; i++) {
        CHECK(all[i].id == g[i].id);
        CHECK(all[i].d == g[i].dist);
    }

    free(all);
}

/* ---------------- 6. 增量插入 ---------------- */

static void test_incremental(void)
{
    printf("  6. 增量插入：边插边查，recall 不塌\n");

    const u16 dim = 16;
    const u32 chunk = 2000;
    const u32 rounds = 4;

    float *v = (float *)malloc((size_t)chunk * rounds * dim * sizeof(float));
    CHECK(v != NULL);
    gen_vecs(v, chunk * rounds, dim);

    Hnsw *h = hnsw_create(dim, HNSW_L2, 16, 200, 555);
    CHECK(h != NULL);

    u32 inserted = 0;
    double last = 0.0;

    for (u32 r = 0; r < rounds; r++) {
        for (u32 i = 0; i < chunk; i++) {
            CHECK(hnsw_add(h, v + (size_t)(inserted + i) * dim, NULL) == OK);
        }
        inserted += chunk;

        /* 每个检查点都用**当前已插入的点**做查询 */
        double rec = recall_at_k(h, v, 20, dim, 10, 64);
        printf("    （插到 %u 个时 recall@10 = %.3f）\n", inserted, rec);
        CHECK(rec >= 0.93);

        last = rec;
    }
    CHECK(last >= 0.93);

    free(v);
    hnsw_destroy(h);
}

/* ---------------- 7. 边界 ---------------- */

static void test_edges(void)
{
    printf("  7. 边界（空索引 / k > n / 内积模式）\n");

    Hnsw *h = hnsw_create(4, HNSW_L2, 16, 200, 1);
    CHECK(h != NULL);

    float q[4] = { 0.1f, 0.2f, 0.3f, 0.4f };
    HnswHit out[8];
    u32 n = 123;

    /* 空索引：返回 0 个，不是错误 */
    CHECK(hnsw_search(h, q, 5, 16, out, &n) == OK);
    CHECK(n == 0);
    CHECK(hnsw_bruteforce(h, q, 5, out, &n) == OK);
    CHECK(n == 0);

    /* 只放 3 个点，k 要 8 个 → 返回 3 个 */
    float v[4];
    for (u32 i = 0; i < 3; i++) {
        for (u16 d = 0; d < 4; d++)
            v[d] = (float)i + (float)d * 0.1f;
        CHECK(hnsw_add(h, v, NULL) == OK);
    }
    CHECK(hnsw_search(h, q, 8, 16, out, &n) == OK);
    CHECK(n == 3);
    CHECK(hnsw_bruteforce(h, q, 8, out, &n) == OK);
    CHECK(n == 3);

    /* k = 0 */
    CHECK(hnsw_search(h, q, 0, 16, out, &n) == OK);
    CHECK(n == 0);

    hnsw_destroy(h);

    /*
     * 内积模式：方向相反的向量应该离得远。
     *
     * 距离 = 1 - dot，所以 dot 越大距离越小。
     * 查询 (1,0,0,0) 时，(2,0,0,0) 最近，(-1,0,0,0) 最远。
     */
    Hnsw *hi = hnsw_create(4, HNSW_IP, 16, 200, 2);
    CHECK(hi != NULL);

    float a[4] = { 1, 0, 0, 0 };
    float b[4] = { 2, 0, 0, 0 };
    float c[4] = { -1, 0, 0, 0 };
    CHECK(hnsw_add(hi, a, NULL) == OK);
    CHECK(hnsw_add(hi, b, NULL) == OK);
    CHECK(hnsw_add(hi, c, NULL) == OK);

    float iq[4] = { 1, 0, 0, 0 };
    CHECK(hnsw_bruteforce(hi, iq, 3, out, &n) == OK);
    CHECK(n == 3);
    CHECK(out[0].id == 1);            /* (2,0,0,0)：dot 最大 */
    CHECK(out[2].id == 2);            /* (-1,0,0,0)：dot 最小 */

    hnsw_destroy(hi);
}

/* ---------------- 8. 上层导航必须真的在起作用 ---------------- */

/*
 * 【这一项是步骤 7 反向验证时补的】
 *
 * 注入"贪心搜索只搜第 0 层"之后，**前面 7 项全绿** ——
 * recall 在 1 万点上从 0.999 变成 0.999，几乎没变。
 *
 * 【为什么 recall 抓不到】
 *
 * 我原以为"少了一层导航肯定更差"。实测才发现不是：
 *
 *   5 万 × 128 维均匀数据，逐档对比（走上层 / 不走上层）：
 *     ef= 16 → 0.218 / 0.216
 *     ef= 32 → 0.344 / 0.364
 *     ef= 64 → 0.506 / 0.504
 *     ef=128 → 0.658 / 0.668
 *
 * 在 10 万点以内、ef ≥ 16 的规模上，上层对 recall **几乎没有影响**；
 * 工作量也只差 0.5~4%。
 *
 * 原因是：第 0 层的图已经足够密，第 0 层的 beam 搜索
 * （ef 宽）自己就能摸到近邻。上层的收益（论文里的 10~100 倍加速）
 * 要到更大的规模、更小的 ef 才显现。
 *
 * 所以这一项断言的是**机制**，不是**收益**：
 *
 *   上层下降之后，第 0 层的实际起点必须和索引的入口点不同 ——
 *   起点没变 = 上层是死代码 = 搜索退化成了单层贪心。
 *
 * 注入把上层关掉时，起点恒等于入口点，这一项立刻变红。
 */
static void test_upper_layers_used(void)
{
    printf("  8. 上层导航必须真的在起作用（断言机制，不是收益）\n");

    /* 承接第 3 项建好的索引（1 万 × 16 维） */
    CHECK(g_big != NULL);
    if (!g_big)
        return;

    float *q = (float *)malloc(50 * BIG_DIM * sizeof(float));
    CHECK(q != NULL);
    gen_vecs(q, 50, BIG_DIM);

    u32 raw_entry = hnsw_entry(g_big);
    u32 nq = 50;
    u32 moved = 0;
    u64 total_normal = 0;
    u64 total_raw = 0;

    for (u32 i = 0; i < nq; i++) {
        HnswHit hits[10];
        u32 nh = 0;
        const float *qi = q + (size_t)i * BIG_DIM;

        CHECK(hnsw_search(g_big, qi, 10, 64, hits, &nh) == OK);

        /* 第 0 层的实际起点 —— 上层下降之后的位置 */
        if (hnsw_last_entry(g_big) != raw_entry)
            moved++;

        /* 工作量对比（只作记录：这个规模上上层省得不多） */
        total_normal += hnsw_last_visited(g_big);
        total_raw += hnsw_visited_from(g_big, qi, raw_entry, 64);
    }

    printf("    （50 个查询：%u 个的起点被上层挪过（入口点 %u）；"
           "走上层访问 %llu 个节点，不走上层 %llu 个）\n",
           moved, raw_entry,
           (unsigned long long)total_normal,
           (unsigned long long)total_raw);

    /*
     * 上层必须**真的**参与了导航。
     *
     * 门槛取 90%：入口点停在最高层，查询是随机的 ——
     * 绝大多数查询的下降都会挪动起点。
     * 取 100% 太紧（万一某个查询恰好落在入口点的邻域里就不挪了）。
     */
    CHECK(moved >= nq * 9 / 10);

    free(q);
}

int main(void)
{
    printf("test_hnsw:\n");

    test_small();
    test_structure();
    test_recall_10k();
    test_bruteforce_truth();
    test_incremental();
    test_edges();
    test_upper_layers_used();

    if (g_big) {
        hnsw_destroy(g_big);
        g_big = NULL;
    }
    free(g_bigv);

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
