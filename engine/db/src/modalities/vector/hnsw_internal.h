#ifndef DB_MODALITIES_VECTOR_HNSW_INTERNAL_H
#define DB_MODALITIES_VECTOR_HNSW_INTERNAL_H

#include <stdlib.h>
#include <string.h>

#include "modalities/vector/hnsw.h"

/*
 * hnsw 的内部头 —— 只给 hnsw.c（构建）/ hnsw_search.c（查询）用。
 *
 * 放这里的东西有两个共同点：**小、且被两个 .c 同时需要**。
 * 做成 static inline 而不是普通函数，是为了不引入外部符号 ——
 * 否则 h_push 这种名字会污染整个链接单元。
 */

/* ---------------- 堆：候选集 / 结果集 ---------------- */

typedef struct {
    float d;
    u32   id;
} HItem;

typedef struct {
    HItem *a;
    u32    n;
    u32    cap;
    bool   max;    /* true = 最大堆（堆顶最远），false = 最小堆 */
} Heap;

/* a 是否该排在 b 前面（即更靠近堆顶） */
static inline bool h_before(const Heap *h, HItem x, HItem y)
{
    return h->max ? (x.d > y.d) : (x.d < y.d);
}

static inline int h_reserve(Heap *h, u32 need)
{
    if (need <= h->cap)
        return OK;

    u32 nc = h->cap ? h->cap : 32u;
    while (nc < need)
        nc *= 2u;

    HItem *p = (HItem *)realloc(h->a, (size_t)nc * sizeof(HItem));
    if (!p)
        return E_IO;
    h->a = p;
    h->cap = nc;
    return OK;
}

static inline int h_push(Heap *h, float d, u32 id)
{
    int rc = h_reserve(h, h->n + 1);
    if (rc != OK)
        return rc;

    u32 i = h->n++;
    h->a[i].d = d;
    h->a[i].id = id;

    while (i > 0) {
        u32 p = (i - 1) / 2;
        if (!h_before(h, h->a[i], h->a[p]))
            break;
        HItem t = h->a[i];
        h->a[i] = h->a[p];
        h->a[p] = t;
        i = p;
    }
    return OK;
}

static inline HItem h_pop(Heap *h)
{
    HItem top = h->a[0];
    h->n--;
    if (h->n > 0) {
        h->a[0] = h->a[h->n];
        u32 i = 0;
        for (;;) {
            u32 l = 2 * i + 1;
            u32 r = l + 1;
            u32 best = i;
            if (l < h->n && h_before(h, h->a[l], h->a[best]))
                best = l;
            if (r < h->n && h_before(h, h->a[r], h->a[best]))
                best = r;
            if (best == i)
                break;
            HItem t = h->a[i];
            h->a[i] = h->a[best];
            h->a[best] = t;
            i = best;
        }
    }
    return top;
}

static inline HItem h_top(const Heap *h)
{
    return h->a[0];
}

static inline void h_clear(Heap *h)
{
    h->n = 0;
}

/* ---------------- 索引结构 ---------------- */

struct Hnsw {
    u16 dim;
    u16 m;        /* 上层每节点邻居上限 */
    u16 m0;       /* 第 0 层：2m */
    u16 efc;
    u8  metric;

    u32 n;
    u32 cap;
    float *vecs;      /* n × dim */

    u8  *level;       /* 每个节点的最高层号 */
    u32 *link_off;    /* 每个节点在 pool 里的起始下标 */
    u16 *cnt;         /* n × (MAX_LEVEL+1)：每层邻居数 */
    u32 *pool;        /* 邻接表池 */
    u32  pool_used;
    u32  pool_cap;

    u32 entry;
    u16 max_level;

    /*
     * 最近一次第 0 层搜索访问的节点数（诊断用）。
     *
     * 【为什么需要它】
     *
     * 上层（贪心下降）的唯一作用是"把起点挪近查询，
     * 让第 0 层少做距离计算"。但这件事**在 recall 上几乎看不出来** ——
     * 实测 1 万点：跳过上层，recall 从 0.999 变成 0.999。
     *
     * 也就是说：上层被关掉这种错误，用 recall 类断言是抓不到的。
     * 只有量"做了多少距离计算"才看得见。
     *
     * 这和 vstore 的 `page_reads` 是同一个用途：
     * 给一个平时看不见的性能性质开一个观测口。
     */
    u32 visits;

    /*
     * 最近一次搜索在第 0 层的**实际起点**（上层下降之后的那个点）。
     *
     * 【为什么需要它】
     *
     * 上层（贪心下降）的作用是"把起点从入口点挪到更靠近查询的地方"。
     * 这个作用在**收益**上很难断言（实测 5 万点：
     * 走上层与不走上层的 recall 几乎一样，工作量只差 0.5%），
     * 但在**机制**上一眼可辨：起点变了没有。
     *
     * 起点没变 = 上层是死代码 = 搜索退化成了单层贪心。
     */
    u32 last_entry;

    u64 rng;

    /* 查询工作区（避免每次查询都 malloc） */
    u32  *visited;
    u32   vcap;
    u32   epoch;

    Heap  cand;
    Heap  res;

    HItem *scratch;
    u32    scratch_cap;
    u32   *disc;        /* 修剪时暂存"被丢弃的"邻居 */
    u32    disc_cap;
};

/* 每层邻居上限：第 0 层 2m，其余 m */
static inline u16 max_links(const Hnsw *h, u16 lc)
{
    return (lc == 0) ? h->m0 : h->m;
}

/*
 * 某节点某层的邻居数组起点。
 *
 * 布局：每节点 [第0层 2m][第1层 m][第2层 m]...
 * 池子里按插入顺序排 —— 节点一旦建好，它的层数就不再变，
 * 所以偏移是固定的，不需要间接层。
 */
static inline u32 *links_at(const Hnsw *h, u32 id, u16 lc)
{
    u32 off = h->link_off[id];
    if (lc == 0)
        return h->pool + off;
    return h->pool + off + h->m0 + (u32)(lc - 1) * h->m;
}

static inline u16 *cnt_at(Hnsw *h, u32 id, u16 lc)
{
    return &h->cnt[(size_t)id * (HNSW_MAX_LEVEL + 1) + lc];
}

/* ---------------- 距离 ---------------- */

static inline float vec_dist(const Hnsw *h, const float *a, const float *b)
{
    const u16 d = h->dim;
    float s = 0.0f;

    if (h->metric == (u8)HNSW_L2) {
        /*
         * 用平方距离，不开方。
         *
         * 排序只需要"谁更近"，平方是单调的 —— 开方纯属浪费。
         * 一次查询要算几千次距离，这一条省下来的很实在。
         */
        for (u16 i = 0; i < d; i++) {
            float t = a[i] - b[i];
            s += t * t;
        }
        return s;
    }

    /* 内积：距离 = 1 - dot。这样"越小越近"的统一语义就不用特判 */
    for (u16 i = 0; i < d; i++)
        s += a[i] * b[i];
    return 1.0f - s;
}

/* 按距离升序（qsort 用） */
static inline int cmp_asc(const void *a, const void *b)
{
    const HItem *x = (const HItem *)a;
    const HItem *y = (const HItem *)b;
    if (x->d < y->d)
        return -1;
    if (x->d > y->d)
        return 1;
    return 0;
}

/* ---------------- 两个 .c 共用的搜索原语 ---------------- */

/*
 * 在第 lc 层做宽度为 ef 的贪心广度搜索，结果填进 res（最大堆，堆顶最远）。
 * 定义在 hnsw.c，查询路径（hnsw_search.c）也要用。
 */
int  hnsw_search_layer(Hnsw *h, const float *q, u32 ep, u16 ef, u16 lc,
                       Heap *res);

/* 上层用的单点贪心下降（ef=1 的特例）：走到局部最近为止 */
u32  hnsw_greedy_step(Hnsw *h, const float *q, u32 ep, u16 lc);

/* ---------------- 反序列化专用（vindex 用） ---------------- */

/*
 * 按磁盘上记的层数与邻居表**直接追加**一个节点。
 *
 * 不走 hnsw_add 的搜索与建边 —— 图是从磁盘读回来的，不是重新算的。
 * 这两个接口绕过了算法的不变量，只该被 vindex 调用。
 *
 * links 的排布必须和 links_at 一致：[第0层 2m][第1层 m]...[第level层 m]。
 */
int hnsw_append_raw(Hnsw *h, const float *vec, u16 level,
                    const u16 *cnts, const u32 *links);

/* 恢复入口点与最高层（读完全部节点后调用一次） */
int hnsw_set_entry(Hnsw *h, u32 entry, u16 max_level);

/* ---------------- 临时缓冲（两个 .c 都要用） ---------------- */

static inline int scratch_reserve(Hnsw *h, u32 need)
{
    if (need <= h->scratch_cap)
        return OK;

    u32 nc = h->scratch_cap ? h->scratch_cap : 64u;
    while (nc < need)
        nc *= 2u;

    HItem *p = (HItem *)realloc(h->scratch, (size_t)nc * sizeof(HItem));
    if (!p)
        return E_IO;
    h->scratch = p;
    h->scratch_cap = nc;
    return OK;
}

#endif /* DB_MODALITIES_VECTOR_HNSW_INTERNAL_H */
