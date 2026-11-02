#ifndef DB_SPI_HEAP_AM_INTERNAL_H
#define DB_SPI_HEAP_AM_INTERNAL_H

#include "index/btree.h"
#include "spi/heap_am.h"

/*
 * heap AM 的私有状态（v1.2 步骤 0 拆出来）。
 *
 * 为什么要单独一个头文件：
 * heap_am.c 已经 594 行，批接口再加进来会逼近 800 行的红线。
 * 而批接口（heap_am_batch.c）需要用到这里定义的扫描状态，
 * 所以把它挪到这个私有头里 —— 两个 .c 共享一份定义，不复制逻辑。
 *
 * 和 catalog/catalog_priv.h、query/exec_internal.h 是同一个套路。
 */

/*
 * 两种扫描共用一个结构。
 *
 * 为什么要合一：SPI 的 scan_next / scan_end 是**固定的函数指针**，
 * 没法按扫描类型分派。所以状态里带一个 kind，取行时看它走哪条路。
 */
typedef enum {
    SCAN_FULL = 0,      /* 全表扫 */
    SCAN_PUSH           /* 索引下推：游标 + 回表 */
} ScanKind;

typedef struct {
    ScanKind  kind;
    HeapScan *full;         /* SCAN_FULL */
    BtCursor *cursor;       /* SCAN_PUSH */
    Heap     *heap;         /* SCAN_PUSH：回表用 */

    /*
     * 批扫描的行字节竞技场（v1.2 步骤 0）。
     *
     * 行的长度是变的，AM 没法凭空造出一块装着 N 行的连续内存 ——
     * 除非它自己有一块。竞技场就是这块缓冲。
     *
     * 惰性分配：只有真的走批路径的查询才会付这 256 KB。
     */
    u8   *arena;
    u32   arena_cap;
    u32   arena_used;
} HeapScanState;

/*
 * 取下一行。逐行路径（heap_am.c）与批路径（heap_am_batch.c）共用。
 *
 * 共用的理由：批路径如果自己写一遍"全表扫 / 下推扫"的分支，
 * 两份逻辑迟早会走偏 —— 而这种偏差只在特定数据下才显形，极难查。
 */
int heap_am_scan_next_row(HeapAmCtx *c, AmScan *s, Rid *rid,
                          u8 *buf, u16 cap, u16 *len);

/* 批接口（heap_am_batch.c 实现） */
int heap_am_batch_scan(StorageAM *am, AmScan *s, AmRowRef *rows, int cap, int *n_out);
int heap_am_batch_decode(StorageAM *am, const AmRowRef *rows, int nrows, AmBatch *out);

#endif /* DB_SPI_HEAP_AM_INTERNAL_H */
