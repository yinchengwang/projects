/*
 * @file index_flat.c
 * @brief 精确扁平向量索引（SDK 自持，脱离冻结内核的 HNSW）。
 *
 * 【为什么是扁平索引】
 * 见 index_flat.h 的说明：精确最近邻，召回率 100%，代价 O(N)。
 *
 * 【为什么用最大堆而不是全排序】
 * 只需前 k 条。维护一个大小 k 的最大堆（堆顶是当前最远者），
 * 遍历一遍 O(N log k) 就够；全排序是 O(N log N)。
 * 嵌入式场景 k 通常 ≤ 20，log k 很小。
 */
#include "sdk/impl/index_flat.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>

struct mmdb_flat_index {
    int32_t dims;   /* 向量维度 */
    int32_t n;      /* 已索引条数 */
    int32_t cap;    /* 容量（可容纳的条数） */
    float  *data;   /* cap * dims，行主序 */
};

mmdb_flat_index_t *mmdb_flat_index_create(int32_t dims)
{
    if (dims <= 0)
        return NULL;

    mmdb_flat_index_t *idx = (mmdb_flat_index_t *)calloc(1, sizeof(*idx));
    if (!idx)
        return NULL;

    idx->dims = dims;
    return idx;
}

void mmdb_flat_index_drop(mmdb_flat_index_t *idx)
{
    if (!idx)
        return;
    free(idx->data);
    free(idx);
}

int32_t mmdb_flat_index_reserve(mmdb_flat_index_t *idx, int32_t n)
{
    if (!idx || n < 0)
        return -1;
    if (n <= idx->cap)
        return 0;   /* 已有容量够用，不缩容 */

    /*
     * 溢出检查：n * dims 必须能被 size_t 表示。
     * 直接乘会先溢出再判断 ⇒ 必须在乘之前比较。
     */
    size_t need = (size_t)n * (size_t)idx->dims;
    if (idx->dims > 0 && (size_t)n > (SIZE_MAX / sizeof(float)) / (size_t)idx->dims)
        return -1;

    float *p = (float *)realloc(idx->data, need * sizeof(float));
    if (!p)
        return -1;

    /*
     * 新容量部分清零 —— 避免读到旧内存。
     * （search 只读 [0, n) 区间，清零主要是为了调试时看到确定的值。）
     */
    if (n > idx->cap)
        memset(p + (size_t)idx->cap * (size_t)idx->dims, 0,
               (size_t)(n - idx->cap) * (size_t)idx->dims * sizeof(float));

    idx->data = p;
    idx->cap = n;
    return 0;
}

int32_t mmdb_flat_index_add(mmdb_flat_index_t *idx, int32_t n, const float *vectors)
{
    if (!idx || n <= 0 || !vectors)
        return -1;

    int32_t want = idx->n + n;
    if (want < idx->n)          /* 整数溢出 */
        return -1;
    if (want > idx->cap) {
        /* 容量翻倍增长，避免每次 add 都 realloc */
        int32_t grow = idx->cap > 0 ? idx->cap : 16;
        while (grow < want) {
            if (grow > (INT32_MAX / 2)) {
                grow = want;
                break;
            }
            grow *= 2;
        }
        if (mmdb_flat_index_reserve(idx, grow) != 0)
            return -1;
    }

    memcpy(idx->data + (size_t)idx->n * (size_t)idx->dims,
           vectors,
           (size_t)n * (size_t)idx->dims * sizeof(float));
    idx->n = want;
    return n;
}

int32_t mmdb_flat_index_size(const mmdb_flat_index_t *idx)
{
    return idx ? idx->n : 0;
}

/* ---------------- 内部：最大堆（堆顶 = 当前最远的候选） ---------------- */

typedef struct {
    int32_t id;
    float   dist;
} flat_cand_t;

static void heap_siftup(flat_cand_t *h, int32_t i)
{
    while (i > 0) {
        int32_t parent = (i - 1) / 2;
        if (h[parent].dist >= h[i].dist)
            break;
        flat_cand_t tmp = h[parent];
        h[parent] = h[i];
        h[i] = tmp;
        i = parent;
    }
}

static void heap_siftdown(flat_cand_t *h, int32_t size, int32_t i)
{
    for (;;) {
        int32_t largest = i;
        int32_t l = 2 * i + 1;
        int32_t r = 2 * i + 2;
        if (l < size && h[l].dist > h[largest].dist)
            largest = l;
        if (r < size && h[r].dist > h[largest].dist)
            largest = r;
        if (largest == i)
            break;
        flat_cand_t tmp = h[largest];
        h[largest] = h[i];
        h[i] = tmp;
        i = largest;
    }
}

static int cand_cmp_asc(const void *a, const void *b)
{
    const flat_cand_t *x = (const flat_cand_t *)a;
    const flat_cand_t *y = (const flat_cand_t *)b;
    if (x->dist < y->dist)
        return -1;
    if (x->dist > y->dist)
        return 1;
    /* 距离并列时按 id 升序 —— 否则"第 k 名"不唯一，结果无法逐位对照 */
    return (x->id < y->id) ? -1 : (x->id > y->id) ? 1 : 0;
}

int32_t mmdb_flat_search_filtered(mmdb_flat_index_t *idx,
                                  const float *query,
                                  int32_t k,
                                  mmdb_flat_filter_fn filter,
                                  void *user_data,
                                  int32_t *out_ids,
                                  float *out_distances)
{
    if (!idx || !query || k <= 0 || !out_ids || !out_distances)
        return -1;
    if (idx->n <= 0)
        return 0;

    int32_t cap = (k < idx->n) ? k : idx->n;

    flat_cand_t *heap = (flat_cand_t *)calloc((size_t)cap, sizeof(flat_cand_t));
    if (!heap)
        return -1;

    int32_t heap_size = 0;

    for (int32_t i = 0; i < idx->n; i++) {
        if (filter && !filter(i, user_data))
            continue;

        const float *v = idx->data + (size_t)i * (size_t)idx->dims;
        float dist = 0.0f;
        for (int32_t d = 0; d < idx->dims; d++) {
            float diff = v[d] - query[d];
            dist += diff * diff;
        }

        if (heap_size < cap) {
            heap[heap_size].id = i;
            heap[heap_size].dist = dist;
            heap_siftup(heap, heap_size);
            heap_size++;
        } else if (dist < heap[0].dist) {
            /* 比当前最远者更近 ⇒ 替换堆顶 */
            heap[0].id = i;
            heap[0].dist = dist;
            heap_siftdown(heap, heap_size, 0);
        }
    }

    qsort(heap, (size_t)heap_size, sizeof(flat_cand_t), cand_cmp_asc);

    for (int32_t i = 0; i < heap_size; i++) {
        out_ids[i] = heap[i].id;
        out_distances[i] = heap[i].dist;
    }

    free(heap);
    return heap_size;
}
