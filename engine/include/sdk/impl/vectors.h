/**
 * @file vectors.h
 * @brief 向量模型内部接口
 */
#ifndef SDK_IMPL_VECTORS_H
#define SDK_IMPL_VECTORS_H

#include "sdk/mmdb.h"
#include "sdk/impl/mmdb_internal.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 在 collection 首次写入时创建数据表（id BLOB PRIMARY KEY, vector BLOB,
 * metadata TEXT, text TEXT, created_at INTEGER） */
int mmdb_vectors_ensure_table(mmdb_collection_t* coll);

/* 懒创建 HNSW 索引：N >= 阈值时自动构建（启动时 / 首次写入后调用） */
int mmdb_vectors_index_ensure(mmdb_collection_t* coll);

/* 从 SQLite 全表扫描重建 HNSW 索引（启动时调用，coll_lock 已持有） */
int mmdb_vectors_index_rebuild(mmdb_collection_t* coll);

/* 释放 collection 的 HNSW 索引内存 */
void mmdb_vectors_index_free(mmdb_collection_t* coll);

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_VECTORS_H */
