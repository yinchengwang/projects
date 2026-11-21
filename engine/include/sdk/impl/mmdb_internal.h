/**
 * @file mmdb_internal.h
 * @brief SDK 内部共享定义（仅供 src/sdk/ 下的 .c 文件使用，不对外暴露）
 */
#ifndef SDK_IMPL_MMDB_INTERNAL_H
#define SDK_IMPL_MMDB_INTERNAL_H

#include "sdk/mmdb.h"
#include "sdk/mmdb_types.h"
#include "sdk/mmdb_error.h"
#include "sdk/mmdb_embedding.h"  /* P4-T4.1：mmdb_collection_s 新增 embedding 字段 */
#include "sdk/impl/mmdb_lock.h"
#include "sdk/impl/mmdb_memctx.h"  /* Task 8：SDK 兼容层内存上下文 */

#include <stdint.h>
#include <sqlite3.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明类型真实结构（仅实现可见） */
struct mmdb_s {
    sqlite3*                 db;             /* SQLite 后端句柄 */
    char*                   path;           /* 数据库文件路径 */
    mmdb_options_t          options;        /* 配置（拷贝） */
    int                     last_err;       /* 线程局部错误码副本（仅供非持有线程读） */
    char*                   last_err_msg;   /* 错误信息字符串 */
    mmdb_rwlock_t         lock;           /* 写操作全局锁（支持并发读） */
    mmdb_collection_t**     collections;    /* 已打开的 collection 缓存 */
    size_t                  collection_count;

    /* Task 8：内存上下文层次（末尾追加，不破坏现有 ABI） */
    MemoryContext          memory_context;     /* DatabaseContext：数据库级根上下文 */
    MemoryContext          connection_context; /* ConnectionContext：连接级上下文 */
    MemoryContext          cache_context;      /* CacheContext：缓存级上下文 */
};

struct mmdb_collection_s {
    mmdb_t*                 db;             /* 归属数据库 */
    char*                   name;           /* Collection 名 */
    mmdb_model_t            model;          /* 模型类型 */
    mmdb_schema_t           schema;         /* Schema 拷贝（含 vector_dim） */
    sqlite3*                sdb;            /* SQLite 句柄（db->db 的别名，加速访问） */
    mmdb_rwlock_t*        coll_lock;      /* 指向 db->lock（读写锁，支持并发读） */
    void*                   vec_index;      /* 内存向量索引指针（精确扁平索引） */
    mmdb_embedding_t*       embedding;      /* P4-T4.1 新增：collection-level embedding；
                                            *   非 NULL 时 mmdb_rag_retrieve 默认用它。
                                            *   所有权归调用方，collection 不接管释放。 */
    /* P5-6：双模同集合能力标志（末尾 append，不破坏 ABI） */
    int                     has_text;       /* 1 = 具备 FTS5 文本检索能力 */
    int                     has_vector;     /* 1 = 具备向量检索能力 */
};

/* 错误信息缓冲最大长度（含末尾 \0） */
#define MMDB_ERR_MSG_MAX 512

/* 设置当前线程错误码与信息（内部使用） */
void mmdb_set_error(mmdb_t* db, int code, const char* msg);

/* 分配并复制字符串（失败时返回 NULL；使用指定内存上下文，Task 11 升级版） */
char* mmdb_strdup_in_ctx(MemoryContext ctx, const char* s);

/*
 * 分配并复制字符串（失败时返回 NULL；旧接口，Task 11 之后仅供未迁移模块使用）。
 * 新代码请使用 mmdb_strdup_in_ctx()，由 ctx 统一管理生命周期。
 */
char* mmdb_strdup_internal(const char* s);

/* 释放 mmdb_result_t 内部所有堆内存 */
void mmdb_result_release(mmdb_result_t* result);

/* 释放 mmdb_path_t 内部所有堆内存 */
void mmdb_path_release(mmdb_path_t* path);

#ifdef __cplusplus
}
#endif

#endif /* SDK_IMPL_MMDB_INTERNAL_H */
