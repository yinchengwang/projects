/*
 * test_vindex.c — HNSW 索引落盘与恢复验收（v1.0 步骤 4 · A6 / A7）
 *
 * 5 项：
 *   1. **重开一致（A6）**：落盘 → 重开 → 读回，查询结果与内存版**逐条相同**
 *   2. 索引过期：插了新向量但没 flush → 重开时 load 返回 E_NOTFOUND
 *   3. **重建（A7）**：索引不可用时从向量数据重放，recall 回到内存版的水平
 *   4. 反复 flush 不膨胀：同一份索引写两次，记录数不变
 *   5. 记录错位 → E_CORRUPT（不静默返回一个错位的图）
 *
 * 【第 1 项为什么要求"逐条相同"而不是"recall 相同"】
 *
 * recall 是个统计量，两个不同的图也可能碰巧给出同样的 recall。
 * 而落盘/读回是**同一张图的两个副本** —— 它必须逐条相同。
 * 只有这个强度的断言才能证明"序列化是无损的"。
 *
 * 【第 3 项是崩溃恢复的核心】
 *
 * 索引是 checkpoint 式的（不写日志），所以崩在两次 flush 之间时，
 * 磁盘上的索引一定过期。这时**不能**拿它去搜 ——
 * 要么重建，要么报错。本项验的是重建这条路能走通、且 recall 恢复。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/vector/hnsw.h"
#include "modalities/vector/vindex.h"
#include "modalities/vector/vstore.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH  "test/tmp/vindex_test.db"
#define DIM   16u
#define M     16u
#define N     2000u

static u64 g_rng = 0xC0FFEE123456789ull;

static u64 rng_next(void)
{
    u64 z = (g_rng += 0x9E3779B97F4A7C15ull);
    z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
    z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
    return z ^ (z >> 31);
}

static float rng_f(void)
{
    return (float)((rng_next() >> 40) & 0xFFFFFFu) / 16777216.0f;
}

static void fresh(void)
{
    remove(PATH);
    remove(PATH "-wal");
}

/* ---------------- 读回 vstore 里所有向量 ---------------- */

typedef struct {
    float *buf;
    u16    dim;
    u32    n;
} Collect;

static int collect_cb(void *ctx, u64 row_id, const float *vec, i32 label)
{
    (void)label;
    Collect *c = (Collect *)ctx;
    memcpy(c->buf + (size_t)row_id * c->dim, vec,
           (size_t)c->dim * sizeof(float));
    c->n++;
    return 0;
}

static float *read_all_vecs(VStore *vs)
{
    u32 n = (u32)vstore_count(vs);
    if (n == 0)
        return NULL;

    float *buf = (float *)malloc((size_t)n * DIM * sizeof(float));
    if (!buf)
        return NULL;

    Collect c;
    c.buf = buf;
    c.dim = DIM;
    c.n = 0;
    if (vstore_scan(vs, collect_cb, &c) != OK) {
        free(buf);
        return NULL;
    }
    if (c.n != n) {
        free(buf);
        return NULL;
    }
    return buf;
}

/* 从向量数据重建索引（A7 的 reindex 路径） */
static Hnsw *rebuild(VStore *vs, const float *vecs, u32 n)
{
    (void)vs;
    Hnsw *h = hnsw_create(DIM, HNSW_L2, M, 200, 777);
    if (!h)
        return NULL;
    for (u32 i = 0; i < n; i++) {
        if (hnsw_add(h, vecs + (size_t)i * DIM, NULL) != OK) {
            hnsw_destroy(h);
            return NULL;
        }
    }
    return h;
}

/* 两个索引对同一批查询的结果必须逐条相同 */
static int same_results(Hnsw *a, Hnsw *b, const float *queries, u32 nq,
                        u32 k, u16 ef)
{
    HnswHit ra[16];
    HnswHit rb[16];
    u32 na = 0;
    u32 nb = 0;

    for (u32 i = 0; i < nq; i++) {
        const float *q = queries + (size_t)i * DIM;
        if (hnsw_search(a, q, k, ef, ra, &na) != OK)
            return 0;
        if (hnsw_search(b, q, k, ef, rb, &nb) != OK)
            return 0;
        if (na != nb)
            return 0;
        for (u32 x = 0; x < na; x++) {
            if (ra[x].id != rb[x].id)
                return 0;
            if (ra[x].dist != rb[x].dist)
                return 0;
        }
    }
    return 1;
}

static double recall(Hnsw *h, const float *queries, u32 nq, u32 k, u16 ef)
{
    HnswHit a[16];
    HnswHit g[16];
    u32 na = 0;
    u32 ng = 0;
    u32 hit = 0;
    u32 total = 0;

    for (u32 i = 0; i < nq; i++) {
        const float *q = queries + (size_t)i * DIM;
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

/* ---------------- 建库：vstore + hnsw + vindex ---------------- */

typedef struct {
    Pager  *pg;
    VStore *vs;
    VIndex *ix;
    Hnsw   *h;
} Bundle;

static int bundle_open(Bundle *b, int create)
{
    memset(b, 0, sizeof(*b));

    Pager *pg = NULL;
    if (pager_open(PATH, 1024, &pg) != OK)
        return 0;
    b->pg = pg;

    if (create) {
        if (vstore_create(pg, NULL, NULL, DIM, &b->vs) != OK)
            return 0;
        if (vindex_create(pg, NULL, NULL, DIM, M, 200, (u8)HNSW_L2,
                          &b->ix) != OK)
            return 0;
    } else {
        if (vstore_open(pg, NULL, NULL, &b->vs) != OK)
            return 0;
        if (vindex_open(pg, NULL, NULL, &b->ix) != OK)
            return 0;
    }
    return 1;
}

/*
 * 关库。**必须先关 Pager** —— 脏页是 pager_close 时落盘的。
 * 第一版忘了这一步，结果"落盘"什么都没写，
 * 重开全是空库（这个坑在测试脚手架里，不在被测代码里）。
 */
static void bundle_close(Bundle *b)
{
    if (b->h)
        hnsw_destroy(b->h);
    b->h = NULL;
    vindex_destroy(b->ix);
    b->ix = NULL;
    vstore_destroy(b->vs);
    b->vs = NULL;
    if (b->pg)
        pager_close(b->pg);
    b->pg = NULL;
}

/* ---------------- 1. 重开一致（A6） ---------------- */

static float *g_queries = NULL;
static Hnsw  *g_mem = NULL;      /* 内存版（flush 之后保留） */

static void test_roundtrip(void)
{
    printf("  1. 重开一致（A6）：查询结果逐条相同\n");
    fresh();

    Bundle b;
    CHECK(bundle_open(&b, 1));

    b.h = hnsw_create(DIM, HNSW_L2, M, 200, 4242);
    CHECK(b.h != NULL);

    float v[DIM];
    for (u32 i = 0; i < N; i++) {
        for (u16 d = 0; d < DIM; d++)
            v[d] = rng_f();

        u64 rid = 0;
        CHECK(vstore_insert(b.vs, 0, v, (i32)(i % 5), &rid) == OK);
        CHECK(rid == i);
        CHECK(hnsw_add(b.h, v, NULL) == OK);
    }
    CHECK(vstore_count(b.vs) == N);
    CHECK(hnsw_count(b.h) == N);

    /* 落盘 */
    CHECK(vindex_flush(b.ix, 0, b.h) == OK);
    CHECK(vindex_count(b.ix) == N);

    /* 留一份内存版做对照（关库前不能销毁） */
    g_mem = b.h;
    b.h = NULL;
    bundle_close(&b);

    /* 重开：读回索引 */
    Bundle c;
    CHECK(bundle_open(&c, 0));

    float *vecs = read_all_vecs(c.vs);
    CHECK(vecs != NULL);

    CHECK(vindex_load(c.ix, N, vecs, &c.h) == OK);
    CHECK(c.h != NULL);
    CHECK(hnsw_count(c.h) == N);
    CHECK(hnsw_entry(c.h) == hnsw_entry(g_mem));
    CHECK(hnsw_max_level(c.h) == hnsw_max_level(g_mem));

    /* 查询：读回的索引与内存版必须逐条相同 */
    u32 nq = 50;
    g_queries = (float *)malloc((size_t)nq * DIM * sizeof(float));
    CHECK(g_queries != NULL);
    for (u32 i = 0; i < nq * DIM; i++)
        g_queries[i] = rng_f();

    CHECK(same_results(g_mem, c.h, g_queries, nq, 10, 64));

    double r1 = recall(g_mem, g_queries, nq, 10, 64);
    double r2 = recall(c.h, g_queries, nq, 10, 64);
    printf("    （内存版 recall@10 = %.4f，读回版 = %.4f）\n", r1, r2);
    CHECK(r1 == r2);

    free(vecs);
    bundle_close(&c);
}

/* ---------------- 2. 索引过期 ---------------- */

static void test_stale(void)
{
    printf("  2. 索引过期：没 flush 就关库 → load 返回 E_NOTFOUND\n");

    Bundle b;
    CHECK(bundle_open(&b, 0));

    float *vecs = read_all_vecs(b.vs);
    CHECK(vecs != NULL);

    /* 磁盘上索引还是 N（第 1 项 flush 的），现在有 N+5 个向量 */
    CHECK(vindex_count(b.ix) == N);

    float v[DIM];
    for (u32 i = 0; i < 5; i++) {
        for (u16 d = 0; d < DIM; d++)
            v[d] = rng_f();
        CHECK(vstore_insert(b.vs, 0, v, 0, NULL) == OK);
    }
    free(vecs);

    vecs = read_all_vecs(b.vs);
    CHECK(vecs != NULL);
    CHECK(vstore_count(b.vs) == N + 5);

    Hnsw *h = NULL;
    CHECK(vindex_load(b.ix, N + 5, vecs, &h) == E_NOTFOUND);
    CHECK(h == NULL);

    /* 按磁盘上的数量去要，还是能读回来（只是它已经旧了） */
    CHECK(vindex_load(b.ix, N, vecs, &h) == OK);
    CHECK(h != NULL);
    CHECK(hnsw_count(h) == N);
    hnsw_destroy(h);

    free(vecs);
    bundle_close(&b);
}

/* ---------------- 3. 重建（A7） ---------------- */

static void test_rebuild(void)
{
    printf("  3. 重建（A7）：索引不可用时从向量重放\n");

    Bundle b;
    CHECK(bundle_open(&b, 0));

    /* 上一项插了 5 个向量但没 flush → 索引过期 */
    u32 n = (u32)vstore_count(b.vs);
    CHECK(n == N + 5);

    float *vecs = read_all_vecs(b.vs);
    CHECK(vecs != NULL);

    Hnsw *h = NULL;
    CHECK(vindex_load(b.ix, n, vecs, &h) == E_NOTFOUND);

    /* reindex：从向量数据重建 */
    h = rebuild(b.vs, vecs, n);
    CHECK(h != NULL);
    CHECK(hnsw_count(h) == n);

    double r = recall(h, g_queries, 50, 10, 64);
    printf("    （重建后 recall@10 = %.4f）\n", r);
    CHECK(r >= 0.95);

    /* 重建后落盘，下次就不用再建 */
    /* 重建后落盘（写回 2005 条：前 2000 条原地改写，后 5 条追加） */
    CHECK(vindex_flush(b.ix, 0, h) == OK);
    CHECK(vindex_count(b.ix) == n);

    hnsw_destroy(h);
    free(vecs);
    bundle_close(&b);
}

/* ---------------- 4. 反复 flush 不膨胀 ---------------- */

static void test_flush_twice(void)
{
    printf("  4. 反复 flush：记录数不变（原地改写）\n");

    Bundle b;
    CHECK(bundle_open(&b, 0));

    float *vecs = read_all_vecs(b.vs);
    CHECK(vecs != NULL);
    u32 n = (u32)vstore_count(b.vs);

    Hnsw *h = NULL;
    CHECK(vindex_load(b.ix, n, vecs, &h) == OK);
    CHECK(h != NULL);

    u32 before = vindex_count(b.ix);
    CHECK(vindex_flush(b.ix, 0, h) == OK);
    CHECK(vindex_flush(b.ix, 0, h) == OK);
    CHECK(vindex_count(b.ix) == before);
    CHECK(before == n);

    /* 页数不应该因为重复 flush 增长 */
    hnsw_destroy(h);
    free(vecs);
    bundle_close(&b);
}

/* ---------------- 5. 记录错位 ---------------- */

static void test_corrupt(void)
{
    printf("  5. 记录错位 → E_CORRUPT\n");

    /*
     * 直接改第 1 条记录的 id（从 1 改成 99），再打开。
     *
     * 不校验的话，读回来的图会把节点 1 的邻居装到节点 0 的位置上 ——
     * 图还是"合法"的（邻居数没超限、入口点也在最高层），
     * 只是**整个索引都错位了**。搜出来的结果看着像模像样，全是错的。
     */
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 1024, &pg) == OK);

    int fixed = 0;
    u64 np = pager_npages(pg);
    for (u64 i = 0; i < np && !fixed; i++) {
        PageFrame *fr = NULL;
        CHECK(pager_get(pg, (u32)i, &fr) == OK);

        if (fr->data[0] == (u8)PT_VEC_LINK) {
            u16 nslots = get_u16(fr->data + 4);
            if (nslots > 1) {
                u16 rsz = get_u16(fr->data + 2);
                /* 槽 1 = 节点 1；载荷第 0 个字段是 id */
                u8 *rec = fr->data + 20 + (u32)rsz;
                put_u32(rec + 4, 99);
                page_set_checksum(fr->data);
                fixed = 1;
            }
        }
        pager_unpin(pg, (u32)i, fixed ? true : false);
    }
    CHECK(fixed == 1);
    pager_close(pg);

    /* 重新打开：必须报 E_CORRUPT，而不是安静地读出一张错位的图 */
    CHECK(pager_open(PATH, 1024, &pg) == OK);
    VIndex *ix = NULL;
    CHECK(vindex_open(pg, NULL, NULL, &ix) == E_CORRUPT);
    CHECK(ix == NULL);
    pager_close(pg);
}

int main(void)
{
    printf("test_vindex:\n");

    fresh();
    test_roundtrip();
    test_stale();
    test_rebuild();
    test_flush_twice();
    test_corrupt();

    if (g_mem)
        hnsw_destroy(g_mem);
    free(g_queries);

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  5 项验收全部通过\n");
    return 0;
}
