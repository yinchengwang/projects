#ifndef DB_MODALITIES_VECTOR_VECTOR_STATE_H
#define DB_MODALITIES_VECTOR_VECTOR_STATE_H

#include "common/types.h"
#include "modalities/vector/hnsw.h"
#include "modalities/vector/vindex.h"
#include "modalities/vector/vstore.h"

/*
 * vector_state —— 向量模态的实例状态（v1.0 步骤 5）
 *
 * 【为什么要有这个结构】
 *
 * 向量模态有三样东西：
 *
 *   vstore  向量数据 + 标量表      —— 真相来源，只追加
 *   hnsw    内存里的 HNSW 索引       —— 可重建
 *   vindex  磁盘上的索引（checkpoint）—— 可重建
 *
 * 它们必须**一起**动：重建索引要新建一个 Hnsw 并替换掉旧的；
 * 而 AM 的 reindex 回调只拿到 VStore，它看不见实例里的 Hnsw。
 *
 * 所以把这三样绑在一个结构里，谁都能拿到全部。
 *
 * 【谁持有它】
 *
 * 实例（DbInstance）持有一个。公开 API 层（db_vec.c）和 AM 适配层
 * （vector_am.c）都通过指针访问它 —— 两边看到的是**同一份**状态。
 *
 * 这不是"共享可变状态"的老问题：它由全局 latch 保护（硬规则 8）。
 */

/* HNSW 的默认参数（hnswlib 同款） */
#define VEC_DEFAULT_M    16u
#define VEC_DEFAULT_EFC  200u
#define VEC_DEFAULT_EF   64u

typedef struct {
    VStore *vs;       /* 向量数据 + 标量表 */
    VIndex *vindex;   /* 磁盘上的索引（checkpoint） */
    Hnsw   *hnsw;     /* 内存里的索引 */

    u16 dim;
    u16 m;            /* 每层邻居数 */
    u16 efc;          /* 构建期候选池 */
} VectorState;

/*
 * 从向量数据重建 HNSW 索引。
 *
 * 这是**自愈**路径：崩溃后索引过期 → 重建 → 继续用。
 * 重建完顺手落盘，下次开库就不用重建了。
 *
 * rows_out 报告重建了多少个节点（可为 NULL）。
 */
int vec_state_reindex(VectorState *st, u64 *rows_out);

/*
 * 恢复索引：先试磁盘上的，读不出来就重建。
 *
 * 开库时走这条。返回 OK 时 st->hnsw 一定可用。
 */
int vec_state_restore(VectorState *st);

#endif /* DB_MODALITIES_VECTOR_VECTOR_STATE_H */
