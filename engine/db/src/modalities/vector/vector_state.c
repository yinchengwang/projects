/*
 * vector_state.c —— 向量模态的实例状态（v1.0 步骤 5）
 *
 * 设计说明见 modalities/vector/vector_state.h。
 *
 * 【重建索引为什么这么贵，又为什么可以接受】
 *
 * 重建 = 读全部向量页 + 重新跑一遍 HNSW 构建。10 万 × 128 维大约几秒。
 * 贵，但只在**索引过期时**发生：
 *
 *   - 正常关闭：关库时 flush 过 → 开库直接读回，不用重建
 *   - 崩溃：索引过期 → 重建一次
 *
 * 换个角度：索引本来就是**可以算出来**的东西。
 * 与其维护它的增量一致性（难且易错），不如让它能被重算。
 * 这是 B+tree 在这个项目里早就定下的策略（"索引不写日志，崩溃后重建"）。
 */

#include "modalities/vector/vector_state.h"

#include <stdlib.h>
#include <string.h>

/* 读出全部向量 */
static int vs_load_all(VectorState *st, float **out, u32 *n_out)
{
    *out = NULL;
    *n_out = 0;

    u64 n = vstore_count(st->vs);
    if (n == 0)
        return OK;

    float *buf = (float *)malloc((size_t)n * st->dim * sizeof(float));
    if (!buf)
        return E_IO;

    u64 got = 0;
    int rc = vstore_scan_all(st->vs, buf, &got);
    if (rc != OK) {
        free(buf);
        return rc;
    }
    if (got != n) {
        free(buf);
        return E_CORRUPT;
    }

    *out = buf;
    *n_out = (u32)n;
    return OK;
}

int vec_state_reindex(VectorState *st, u64 *rows_out)
{
    if (!st || !st->vs)
        return E_INVAL;

    float *vecs = NULL;
    u32 n = 0;
    int rc = vs_load_all(st, &vecs, &n);
    if (rc != OK)
        return rc;

    Hnsw *h = hnsw_create(st->dim, HNSW_L2, st->m, st->efc, 0x5EEDu);
    if (!h) {
        free(vecs);
        return E_IO;
    }

    for (u32 i = 0; i < n; i++) {
        rc = hnsw_add(h, vecs + (size_t)i * st->dim, NULL);
        if (rc != OK) {
            hnsw_destroy(h);
            free(vecs);
            return rc;
        }
    }
    free(vecs);

    if (st->hnsw)
        hnsw_destroy(st->hnsw);
    st->hnsw = h;

    if (rows_out)
        *rows_out = n;

    /* 顺手落盘 —— 免得下次开库又重建一遍 */
    if (st->vindex) {
        rc = vindex_flush(st->vindex, 0, h);
        if (rc != OK)
            return rc;
    }
    return OK;
}

int vec_state_restore(VectorState *st)
{
    if (!st || !st->vs)
        return E_INVAL;

    float *vecs = NULL;
    u32 n = 0;
    int rc = vs_load_all(st, &vecs, &n);
    if (rc != OK)
        return rc;

    Hnsw *h = NULL;
    if (st->vindex && vindex_load(st->vindex, n, vecs, &h) == OK) {
        st->hnsw = h;
        free(vecs);
        return OK;
    }

    /*
     * 磁盘上的索引过期或损坏 → 重建。
     *
     * 不用旧索引：它和数据对不上，拿它去搜会给出
     * **看着合理但全错**的结果 —— 这比"慢一点"坏得多。
     */
    free(vecs);
    return vec_state_reindex(st, NULL);
}
