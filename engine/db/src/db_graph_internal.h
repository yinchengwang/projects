#ifndef DB_SRC_DB_GRAPH_INTERNAL_H
#define DB_SRC_DB_GRAPH_INTERNAL_H

/*
 * db_graph.c / db_graph_walk.c 共享的内部定义。
 *
 * 【为什么单独一个头】
 *
 * 两个文件共用两样东西：动态数组（RidVec）和"持锁状态下的跳一步"。
 * 共享的东西要有**唯一的一份定义** —— 两处各写一份，
 * 迟早会漂移（比如一个改了初始容量，另一个没改）。
 *
 * 这些不是公共 API —— 公共 API 只有 include/db.h。
 */

#include "db.h"

#include "common/types.h"
#include "modalities/graph/graph.h"

/*
 * 动态数组：图的 id 列表。
 *
 * 【为什么不是固定数组】
 *
 * 节点数 / 边数事先不知道（10 万节点 / 100 万边是验收规模），
 * 固定数组要么开小了溢出，要么开大了浪费。
 */
typedef struct {
    db_graph_rid *v;
    uint32_t      n;
    uint32_t      cap;
} RidVec;

/* 追加一个元素。满了就翻倍。失败返回 E_IO。 */
int  ridvec_push(RidVec *a, db_graph_rid x);

/* 释放数组（v 置 NULL，n / cap 归零）。可重复调用。 */
void ridvec_free(RidVec *a);

/*
 * 跳一步（**调用方必须已经持有全局 latch**）。
 *
 * out 是堆上分配的，调用方用 db_graph_free 释放。
 * 没有邻居时 *out = NULL、*n_out = 0（不是错误）。
 */
int dg_hop_locked(Graph *g, Rid node, bool in,
                  db_graph_rid **out, uint32_t *n_out);

#endif /* DB_SRC_DB_GRAPH_INTERNAL_H */
