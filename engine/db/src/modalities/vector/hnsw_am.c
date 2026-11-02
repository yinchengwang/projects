/*
 * hnsw_am.c —— HNSW 的 Index AM 适配（v1.0 步骤 5）
 *
 * 设计说明见 modalities/vector/hnsw_am.h。
 *
 * 【knn 的实现】
 *
 * probe 是查询向量的字节串（dim × 4 字节的 fp32）。
 * 流程：
 *   1. 校验长度（长度必须正好是 dim × 4 —— 维度不对就是用错了索引）
 *   2. HNSW 查 top-k（ef 由调用方给，这是召回/延迟的旋钮）
 *   3. 把内部的 id（= row_id）换算成表的 RID
 *
 * 【ef 从哪来】
 *
 * IndexAM 的 knn 签名里**没有 ef** —— 只有 k。
 * 这是契约的缺口：ef 是 HNSW 的核心旋钮，缺了它就没法调召回。
 *
 * 处理办法：HnswAmCtx 里放一个默认 ef（64，hnswlib 默认值），
 * 需要调的时候由**上层**（公开 API）直接调 hnsw_search，
 * 绕过这个接口。
 *
 * 那为什么还要这个接口？因为它是**契约** ——
 * 将来优化器要支持 `ORDER BY vec <-> q LIMIT k` 时，
 * 走的正是这条路。现在把接口填上，等于把那条路先修通。
 */

#include "modalities/vector/hnsw_am.h"

#include <stdlib.h>
#include <string.h>

/* 默认的 ef（hnswlib 默认 64） */
#define HNSW_AM_DEFAULT_EF 64u

/* ---------------- 不支持的接口 ---------------- */

/*
 * B+tree 能做而 HNSW 不能做的事，一律 E_NOTSUP。
 *
 * 这不是"实现得不好"，是"这个结构没有这个能力"。
 * 优化器看到 E_NOTSUP 就退回别的做法 ——
 * 能力协商靠返回值，不靠类型判断。
 */
static int ha_insert(IndexAM *am, const u8 *key, u16 klen, Rid rid)
{
    (void)am; (void)key; (void)klen; (void)rid;
    return E_NOTSUP;
}

static int ha_remove(IndexAM *am, const u8 *key, u16 klen, Rid rid)
{
    (void)am; (void)key; (void)klen; (void)rid;
    return E_NOTSUP;
}

static int ha_search(IndexAM *am, const u8 *key, u16 klen,
                     Rid *out, int max, int *n)
{
    (void)am; (void)key; (void)klen; (void)out; (void)max; (void)n;
    return E_NOTSUP;
}

static int ha_range(IndexAM *am,
                    const u8 *lo, u16 llen, bool lo_inc,
                    const u8 *hi, u16 hlen, bool hi_inc,
                    IndexCursor **out)
{
    (void)am; (void)lo; (void)llen; (void)lo_inc;
    (void)hi; (void)hlen; (void)hi_inc; (void)out;
    return E_NOTSUP;
}

static int ha_cursor_next(IndexAM *am, IndexCursor *c, Rid *rid)
{
    (void)am; (void)c; (void)rid;
    return E_NOTSUP;
}

static int ha_cursor_end(IndexAM *am, IndexCursor *c)
{
    (void)am; (void)c;
    return E_NOTSUP;
}

/* ---------------- knn：真实现 ---------------- */

static int ha_knn(IndexAM *am, const u8 *probe, u16 plen,
                  int k, Rid *out, float *dist, int *n)
{
    HnswAmCtx *c = (HnswAmCtx *)am->impl;

    if (!c || !c->h || !probe)
        return E_INVAL;
    if (k <= 0) {
        if (n)
            *n = 0;
        return OK;
    }

    u16 vlen = (u16)((u32)c->dim * 4u);
    if (plen != vlen)
        return E_INVAL;     /* 维度对不上 = 用错了索引 */

    if (!out || !dist || !n)
        return E_INVAL;

    HnswHit *hits = (HnswHit *)calloc((size_t)k, sizeof(HnswHit));
    if (!hits)
        return E_IO;

    u16 ef = c->ef ? c->ef : (u16)HNSW_AM_DEFAULT_EF;

    u32 nh = 0;
    int rc = hnsw_search(c->h, (const float *)probe, (u32)k, ef, hits, &nh);
    if (rc != OK) {
        free(hits);
        return rc;
    }

    /*
     * 把内部 id（= row_id）换算成表的 RID。
     *
     * RID = (数据页号, 槽号)，槽号 = row_id % K，页号靠序号反查。
     */
    u16 kk = vstore_k(c->vs);
    for (u32 i = 0; i < nh; i++) {
        u64 row_id = hits[i].id;

        u32 ord = (u32)(row_id / kk);
        u16 slot = (u16)(row_id % kk);
        u32 pid = vstore_page_id(c->vs, ord);

        if (pid == PAGE_ID_NIL) {
            /* 索引里有、数据里没有 —— 索引过期了。 */
            free(hits);
            if (n)
                *n = 0;
            return E_NOTFOUND;
        }

        out[i] = rid_make(pid, slot);
        dist[i] = hits[i].dist;
    }

    free(hits);
    *n = (int)nh;
    return OK;
}

/* ---------------- 构造 ---------------- */

int hnsw_am_build(VStore *vs, Hnsw *h, u16 dim, IndexAM *out, HnswAmCtx *ctx)
{
    if (!vs || !h || !out || !ctx || dim == 0)
        return E_INVAL;

    ctx->vs = vs;
    ctx->h = h;
    ctx->dim = dim;
    ctx->ef = (u16)HNSW_AM_DEFAULT_EF;

    out->name = "hnsw";
    out->caps = IX_CAP_KNN;

    out->insert = ha_insert;
    out->remove = ha_remove;
    out->search = ha_search;
    out->range = ha_range;
    out->cursor_next = ha_cursor_next;
    out->cursor_end = ha_cursor_end;
    out->knn = ha_knn;
    out->impl = ctx;

    return OK;
}
