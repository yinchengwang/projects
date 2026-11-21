/*
 * @file index_flat.h
 * @brief SDK 自持的内存向量索引（精确扁平索引）。
 *
 * 【为什么是"扁平"而不是 HNSW】
 * 原实现用冻结内核的 HNSW，近似最近邻，有召回率损失。
 * 扁平索引是**精确**最近邻：召回率 100%，代价是 O(N) 扫描。
 * 嵌入式 SDK 的典型规模（万级向量）可接受；真要更大规模，
 * 应当引入量化（PQ/SQ），而不是回到近似。
 *
 * 【与原 HNSW 接口的对应关系】（便于对照评审与机械替换）
 *   创建 → mmdb_flat_index_create(dims)      （原接口还需 M / ef / 距离口径 / 量化参数）
 *   预留 → mmdb_flat_index_reserve(idx, n)
 *   追加 → mmdb_flat_index_add(idx, n, vectors)
 *   条数 → mmdb_flat_index_size(idx)
 *   释放 → mmdb_flat_index_drop(idx)
 *   检索 → mmdb_flat_search_filtered(...)
 * 原接口的 M / ef / 量化参数在本实现里**没有对应物** —— 精确检索不需要它们。
 */
#ifndef SDK_IMPL_INDEX_FLAT_H
#define SDK_IMPL_INDEX_FLAT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct mmdb_flat_index mmdb_flat_index_t;

/* 过滤谓词：返回非 0 表示该候选通过。 */
typedef int (*mmdb_flat_filter_fn)(int32_t vec_id, void *user_data);

/* 建索引。dims = 向量维度。失败返回 NULL。 */
mmdb_flat_index_t *mmdb_flat_index_create(int32_t dims);

/* 释放索引。可传 NULL。 */
void mmdb_flat_index_drop(mmdb_flat_index_t *idx);

/* 预分配 n 条的容量（避免逐条 realloc）。0 成功 / -1 失败。 */
int32_t mmdb_flat_index_reserve(mmdb_flat_index_t *idx, int32_t n);

/* 批量追加 n 条向量（行主序，长度 n*dims）。返回实际追加条数；失败返回负数。 */
int32_t mmdb_flat_index_add(mmdb_flat_index_t *idx, int32_t n, const float *vectors);

/* 已索引的向量条数。idx 为 NULL 返回 0。 */
int32_t mmdb_flat_index_size(const mmdb_flat_index_t *idx);

/*
 * 精确 KNN 搜索（带过滤）。
 *
 * 距离口径：**L2 平方距离** —— 与 vectors.c 里 l2_distance_* 以及原 HNSW 的
 * DISTANCE_METRIC_L2_SQUARED 完全一致（不开根号）。口径不一致 = 静默给错答案。
 *
 * filter 为 NULL 表示不过滤；filter 返回 0 的候选被丢弃。
 * out_ids / out_distances 由调用方提供（各 k 项），输出按距离**升序**。
 *
 * 返回实际命中条数（<= k）；-1 表示参数非法。
 */
int32_t mmdb_flat_search_filtered(mmdb_flat_index_t *idx,
                                  const float *query,
                                  int32_t k,
                                  mmdb_flat_filter_fn filter,
                                  void *user_data,
                                  int32_t *out_ids,
                                  float *out_distances);

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_INDEX_FLAT_H */
