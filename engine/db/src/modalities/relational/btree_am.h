#ifndef DB_SPI_BTREE_AM_H
#define DB_SPI_BTREE_AM_H

#include "index/btree.h"
#include "spi/index_am.h"

/*
 * B+tree 的 Index AM 适配（v0.9）。
 *
 * 【为什么索引也要包一层】
 *
 * 和 heap_am 同一个理由：让"索引"成为接口而不是实现细节。
 * 将来 HNSW、倒排索引都实现同一套 Index AM，
 * 执行器 / 优化器不需要知道底下是哪种结构。
 *
 * 【knn 返回 E_NOTSUP】
 *
 * B+tree 没有近邻查询能力。这不是"实现得不好"，
 * 是"这个结构没有这个能力" —— 优化器看到 E_NOTSUP 就退回全表扫描。
 * **能力协商靠返回值，不靠类型判断**（DESIGN.md §5.2）。
 */

/*
 * 构造一个 B+tree 的 Index AM。
 *
 * am_out 由调用方持有。bt 不被拷贝 —— 它的生命周期由 catalog 管。
 */
int btree_am_build(BTree *bt, IndexAM *out);

#endif /* DB_SPI_BTREE_AM_H */
