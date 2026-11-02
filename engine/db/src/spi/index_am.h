#ifndef DB_SPI_INDEX_AM_H
#define DB_SPI_INDEX_AM_H

#include "common/error.h"
#include "common/types.h"

/*
 * Index AM —— 加速结构（v0.9 定稿）。
 *
 * 【和 DESIGN.md §5.2 草图的两处调整】
 *
 * 1. **去掉 create**。
 *    索引的创建要写元数据（catalog），那是 catalog 层的事，
 *    不是索引 AM 的事。Index AM 只负责"一个已经存在的索引"的读写与查询。
 *
 * 2. **写接口不带 xid**。
 *    因为**索引不写日志**（v0.6 的决定：索引崩溃后重建）。
 *    索引里没有"属于哪个事务"的概念，自然不需要 xid。
 *    —— 这一条必须写下来，否则后人一定会以为漏了。
 *
 * 【knn 是给向量/地理留的口子】
 *
 * B+tree 实现返回 E_NOTSUP。优化器看到 E_NOTSUP 就退回全表扫描 ——
 * **能力协商靠返回值，不靠类型判断**（DESIGN.md §5.2）。
 * 这样加新模态不需要改优化器。
 */

typedef struct IndexAM     IndexAM;
typedef struct IndexCursor IndexCursor;

/* 能力位 */
#define IX_CAP_SEARCH  0x01u   /* 支持等值查找 */
#define IX_CAP_RANGE   0x02u   /* 支持范围扫描 */
#define IX_CAP_KNN     0x04u   /* 支持近邻查询 */
#define IX_CAP_UNIQUE  0x08u   /* 唯一索引 */

struct IndexAM {
    const char *name;          /* "btree" / 将来的 "hnsw" / "inverted" */
    u32         caps;

    /* 插入 / 删除一个 (key, rid) */
    int (*insert)(IndexAM *am, const u8 *key, u16 klen, Rid rid);
    int (*remove)(IndexAM *am, const u8 *key, u16 klen, Rid rid);

    /*
     * 精确查找。同一 key 可能对应多个 RID（非唯一索引）。
     * out 是调用方的缓冲，最多写 max 个；*n 返回实际个数
     * （可能 > max —— 调用方据此知道被截断了）。
     */
    int (*search)(IndexAM *am, const u8 *key, u16 klen,
                  Rid *out, int max, int *n);

    /*
     * 范围扫描。
     *
     * lo / hi 传 NULL 表示该侧无界。
     * lo_inc / hi_inc 为 false 表示**开区间**（不含端点）。
     *
     * 【B+tree 只支持闭区间，怎么办】
     * B+tree 的扫描是闭区间 [lo, hi]。开区间由适配层在游标里
     * **跳过端点条目**实现 —— 宁可多给行（执行器会重新求值 WHERE），
     * 不能少给行。
     */
    int (*range)(IndexAM *am,
                 const u8 *lo, u16 llen, bool lo_inc,
                 const u8 *hi, u16 hlen, bool hi_inc,
                 IndexCursor **out);

    int (*cursor_next)(IndexAM *am, IndexCursor *c, Rid *rid);
    int (*cursor_end)(IndexAM *am, IndexCursor *c);

    /*
     * 近邻查询（向量 / 地理用）。
     *
     * B+tree 返回 E_NOTSUP —— 这不是"实现得不好"，
     * 是"这个结构没有这个能力"。优化器据此退回全表扫描。
     */
    int (*knn)(IndexAM *am, const u8 *probe, u16 plen,
               int k, Rid *out, float *dist, int *n);

    void *impl;                /* 模态私有状态 */
};

#endif /* DB_SPI_INDEX_AM_H */
