#ifndef DB_SPI_HEAP_AM_H
#define DB_SPI_HEAP_AM_H

#include "catalog/catalog.h"
#include "spi/storage_am.h"
#include "storage/heap.h"

/*
 * 关系模态的 SPI 适配（v0.9）。
 *
 * 【为什么关系模态也要包一层】
 *
 * 因为 SPI 的目标是"执行器只认接口，不认模态"。
 * 如果关系模态直接被执行器特殊处理（现在是这个状态），
 * 那"模态"就只是"blob 那一块"，SPI 是假的。
 *
 * 包一层之后：
 *   - 执行器对 heap 和 blob 一视同仁
 *   - heap 的索引维护、MVCC 打版本号都收进 AM 里
 *   - 加新模态 = 多一个 .c 文件，执行器不动
 *
 * 【AM 的生命周期】
 *
 * heap 的 AM 是**每条查询现构造**的（不是实例级共享）：
 * 它要绑定到某张具体的表（堆句柄 + 列定义 + 索引列表），
 * 而表是运行期动态创建的。
 *
 * 所以 am 和 ctx 都由调用方（执行器的 TableCtx）持有，
 * 栈上就行 —— AM 本身不持有任何资源。
 *
 * blob 的 AM 相反：它是实例级的（所有连接共享同一片对象空间），
 * 挂在 DbInstance 上。
 */

/* AM 的私有状态：这张表是什么 */
typedef struct {
    Heap            *heap;
    const ColDesc   *cols;      /* 归 catalog 所有 */
    int              ncols;
    const IndexInfo *idxs;      /* 归 catalog 所有 */
    int              nidx;

    /*
     * 预计算的记录布局。
     *
     * 布局只跟列定义有关，跟行无关 —— 每行重算一遍是纯浪费。
     * AM 每条查询构造一次，所以在这里算一次就够整条查询用了。
     */
    RecLayout        layout;
} HeapAmCtx;

/*
 * 构造一个关系模态的 StorageAM。
 *
 * am_out 由调用方持有（通常是 TableCtx 里的一个字段）。
 * ctx_out 由调用方持有（通常是 TableCtx 里的一个字段）。
 * 两者都必须活到这次查询结束。
 *
 * heap / cols / idxs 都不被拷贝 —— 它们由 catalog 拥有，生命周期更长。
 */
int heap_am_build(Heap *heap, const ColDesc *cols, int ncols,
                  const IndexInfo *idxs, int nidx,
                  StorageAM *am_out, HeapAmCtx *ctx_out);

#endif /* DB_SPI_HEAP_AM_H */
