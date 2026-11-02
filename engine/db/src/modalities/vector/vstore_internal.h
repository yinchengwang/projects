#ifndef DB_MODALITIES_VECTOR_VSTORE_INTERNAL_H
#define DB_MODALITIES_VECTOR_VSTORE_INTERNAL_H

#include "modalities/vector/vstore.h"

/*
 * vstore 的内部头 —— 只给 vstore.c / vstore_read.c 用，不对外。
 *
 * 为什么要有它：结构体和几个页操作助手被**两个** .c 共用
 * （写路径在 vstore.c，读路径在 vstore_read.c）。
 * C 没有"同模块私有头"，那就开一个 internal 头，
 * 而不是把字段摊进公开的 vstore.h。
 */

/*
 * VStore 的内部结构。
 *
 * 放在 .c 里而不是头文件里 —— 和 gstore 的做法相反，理由也不同：
 * gstore 的结构体必须进头文件，是因为它有**两个** .c 文件要碰它
 * （gstore.c 和 gstore_cursor.c）。vstore 目前只有一个 .c，
 * 所以保持不透明，外面只能通过 vstore_*.h 的函数访问。
 *
 * 将来如果 hnsw / vector_am 确实需要直接读页，再开一个
 * vstore_internal.h —— 而不是现在就把字段摊开。
 */
struct VStore {
    Pager     *pg;
    LogHookFn  wal;
    void      *wal_ctx;

    u16  dim;             /* 维度 */
    u16  k;               /* 每页向量数 = VEC_K(dim) */
    u32  hdr_page;        /* 头页页号 */

    u32  head;            /* 第一个数据页 */
    u32  tail;            /* 最后一个数据页 */
    u32  npages;          /* 数据页数 */
    u32  cap_pages;
    u32 *pages;           /* 内存目录：序号 → 页号 */

    GStore *meta;         /* 标量表（定长槽页） */
    u64  count;           /* 向量总数 */
    u32  cap_rids;
    Rid *meta_rids;       /* row_id → 标量记录的 RID */

    u64  chain_sum;       /* 重建时各页 count 之和（交叉校验用） */
    u64  page_reads;      /* 查询引起的页读次数（诊断用） */
    u64  meta_reads;      /* 累计读出的标量记录条数 */
};

/* 动态数组扩容（两个 .c 共用） */
int vec_grow(void **arr, u32 *cap, u32 need, size_t elem);

/* 页读（读路径用：带 page_reads 计数） */
int vec_read_page(VStore *vs, u32 page_id, PageFrame **out);

/* 页写（WAL 整页镜像） */
typedef int (*VecMut)(void *ctx, u8 *pg);
int vec_write_page(VStore *vs, u32 xid, u32 page_id, VecMut mut, void *mut_ctx);

#endif /* DB_MODALITIES_VECTOR_VSTORE_INTERNAL_H */
