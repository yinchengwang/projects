/*
 * db_graph_walk.c —— 图的遍历原语：跳一步 / BFS 多跳（v0.11 步骤 4）
 *
 * 设计说明见 include/db.h 和 src/db_graph_internal.h。
 *
 * 【为什么从 db_graph.c 里拆出来】
 *
 * db_graph.c 一度到 793 行，贴着 800 行的红线。
 * 拆分的边界是**"会不会改图"**：
 *   - db_graph.c：建图、加节点、加边、删 —— 全部会改图
 *   - db_graph_walk.c：只读 —— 一行都不改
 *
 * 拆完之后，"遍历逻辑会不会不小心把图改坏"这种担心不存在了：
 * 改图的代码根本不在这个文件里。
 *
 * 【BFS 为什么必须去重】
 *
 * 同一个节点可以从多条路径到达（菱形图：A→B、A→C、B→D、C→D）。
 * 不去重的话，"3 跳"会把 D 数两遍 ——
 * 跳的次数被算错，A6 的性能基准也就失去意义。
 *
 * 【为什么自己写哈希集】
 *
 * 库里没有哈希集合。用排序数组去重是 O(n²)：
 * 10 万节点、3 跳展开到几千个节点时，n² 就是几百万次比较 ——
 * 比遍历本身还慢，测出来的 p99 就不是图的 p99 了。
 * 开放寻址哈希集是几十行的事。
 */

#include "db_graph_internal.h"
#include "db_internal.h"

#include "common/latch.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 动态数组 ---------------- */

int ridvec_push(RidVec *a, db_graph_rid x)
{
    if (a->n == a->cap) {
        uint32_t ncap = a->cap ? a->cap * 2 : 16;
        db_graph_rid *nv = (db_graph_rid *)realloc(a->v,
                                                   (size_t)ncap * sizeof(db_graph_rid));
        if (!nv)
            return E_IO;
        a->v = nv;
        a->cap = ncap;
    }
    a->v[a->n++] = x;
    return OK;
}

void ridvec_free(RidVec *a)
{
    if (!a)
        return;
    free(a->v);
    a->v = NULL;
    a->n = 0;
    a->cap = 0;
}

/* ---------------- 跳一步 ---------------- */

/*
 * 出度未知 → 先给一个小缓冲，装不下就翻倍重试。
 * 最多重试 log2(度数) 次，通常一次就够。
 *
 * 【为什么不在图层加一个"数度数"的接口】
 *
 * 数一遍再取一遍 = 链被走两遍，IO 翻倍。
 * 而图遍历是 IO 密集的（每次跳都读边页）—— 能少读一遍就少读一遍。
 */
int dg_hop_locked(Graph *g, Rid node, bool in,
                  db_graph_rid **out, uint32_t *n_out)
{
    *out = NULL;
    *n_out = 0;

    uint32_t cap = 16;
    for (;;) {
        Rid *buf = (Rid *)malloc((size_t)cap * sizeof(Rid));
        if (!buf)
            return E_IO;

        uint32_t n = 0;
        int rc = in ? graph_in_neighbors(g, node, buf, cap, &n)
                    : graph_out_neighbors(g, node, buf, cap, &n);
        if (rc == E_NOSPACE) {
            /* 装不下 → 翻倍重来 */
            free(buf);
            cap *= 2;
            continue;
        }
        if (rc != OK) {
            free(buf);
            return rc;
        }

        if (n == 0) {
            free(buf);
            return OK;          /* 没有邻居：*out 保持 NULL */
        }

        db_graph_rid *res = (db_graph_rid *)malloc((size_t)n * sizeof(db_graph_rid));
        if (!res) {
            free(buf);
            return E_IO;
        }
        for (uint32_t i = 0; i < n; i++)
            res[i] = rid_to_u64(buf[i]);

        free(buf);
        *out = res;
        *n_out = n;
        return OK;
    }
}

/* ---------------- BFS 多跳 ---------------- */

/*
 * 开放寻址的 u64 集合（BFS 去重用）。
 *
 * 负载因子控制在 0.7 以下 —— 开放寻址在接近满载时探测长度会急剧上升，
 * 0.7 是经验上的甜点。
 */
typedef struct {
    u64 *keys;
    u8  *used;
    uint32_t cap;      /* 2 的幂 */
    uint32_t n;
} U64Set;

static u64 us_hash(u64 x)
{
    /* splitmix64：把连续 id 搅开，避免低位聚集 */
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

static int us_init(U64Set *s, uint32_t cap)
{
    s->keys = (u64 *)calloc(cap, sizeof(u64));
    s->used = (u8 *)calloc(cap, 1);
    if (!s->keys || !s->used) {
        free(s->keys);
        free(s->used);
        s->keys = NULL;
        s->used = NULL;
        return E_IO;
    }
    s->cap = cap;
    s->n = 0;
    return OK;
}

static void us_free(U64Set *s)
{
    free(s->keys);
    free(s->used);
    s->keys = NULL;
    s->used = NULL;
    s->cap = 0;
    s->n = 0;
}

/* 集合已存在 k → 返回 1；不存在 → 插入并返回 0。 */
static int us_add(U64Set *s, u64 k)
{
    if ((s->n + 1) * 10 >= s->cap * 7) {
        /* 负载因子 > 0.7 → 扩容（2 倍） */
        uint32_t ncap = s->cap * 2;
        u64 *nkeys = (u64 *)calloc(ncap, sizeof(u64));
        u8 *nused = (u8 *)calloc(ncap, 1);
        if (!nkeys || !nused) {
            free(nkeys);
            free(nused);
            return E_IO;
        }
        for (uint32_t i = 0; i < s->cap; i++) {
            if (!s->used[i])
                continue;
            uint32_t j = (uint32_t)(us_hash(s->keys[i]) & (ncap - 1));
            while (nused[j])
                j = (j + 1) & (ncap - 1);
            nkeys[j] = s->keys[i];
            nused[j] = 1;
        }
        free(s->keys);
        free(s->used);
        s->keys = nkeys;
        s->used = nused;
        s->cap = ncap;
    }

    uint32_t i = (uint32_t)(us_hash(k) & (s->cap - 1));
    while (s->used[i]) {
        if (s->keys[i] == k)
            return 1;            /* 已经有了 */
        i = (i + 1) & (s->cap - 1);
    }
    s->used[i] = 1;
    s->keys[i] = k;
    s->n++;
    return 0;
}

db_err db_graph_walk(DB *db, db_graph_rid node, uint32_t depth,
                     db_graph_rid **out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;

    Graph *g = db->inst->graph;
    if (!g) {
        snprintf(db->errmsg, sizeof(db->errmsg), "图还不存在");
        return DB_E_NOTFOUND;
    }

    *out = NULL;
    *n_out = 0;

    latch_lock();

    U64Set seen;
    RidVec res = { NULL, 0, 0 };
    int rc = us_init(&seen, 1024);
    if (rc != OK) {
        latch_unlock();
        return map_rc(rc);
    }

    /* 起点一定在结果里（depth = 0 时结果就是起点自己） */
    us_add(&seen, node);
    rc = ridvec_push(&res, node);
    if (rc != OK) {
        us_free(&seen);
        latch_unlock();
        return map_rc(rc);
    }

    db_graph_rid *level = (db_graph_rid *)malloc(sizeof(db_graph_rid));
    if (!level) {
        us_free(&seen);
        ridvec_free(&res);
        latch_unlock();
        return DB_E_IO;
    }
    level[0] = node;
    uint32_t nlevel = 1;

    for (uint32_t d = 0; d < depth && nlevel > 0; d++) {
        db_graph_rid *next = NULL;
        uint32_t nnext = 0;

        for (uint32_t i = 0; i < nlevel; i++) {
            db_graph_rid *nb = NULL;
            uint32_t nnb = 0;
            rc = dg_hop_locked(g, rid_from_u64(level[i]), false, &nb, &nnb);
            if (rc != OK) {
                free(next);
                free(level);
                ridvec_free(&res);
                us_free(&seen);
                latch_unlock();
                snprintf(db->errmsg, sizeof(db->errmsg), "遍历失败");
                return map_rc(rc);
            }

            for (uint32_t j = 0; j < nnb; j++) {
                if (us_add(&seen, nb[j]))
                    continue;       /* 已经到过这个节点了 */
                rc = ridvec_push(&res, nb[j]);
                if (rc == OK) {
                    db_graph_rid *nn = (db_graph_rid *)realloc(
                        next, (size_t)(nnext + 1) * sizeof(db_graph_rid));
                    if (!nn) {
                        rc = E_IO;
                    } else {
                        next = nn;
                        next[nnext++] = nb[j];
                    }
                }
                if (rc != OK) {
                    free(nb);
                    free(next);
                    free(level);
                    ridvec_free(&res);
                    us_free(&seen);
                    latch_unlock();
                    return map_rc(rc);
                }
            }
            free(nb);
        }

        free(level);
        level = next;
        nlevel = nnext;
    }

    free(level);
    us_free(&seen);

    *out = res.v;
    *n_out = res.n;

    latch_unlock();
    return DB_OK;
}
