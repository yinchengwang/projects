/*
 * btree_am.c — B+tree 的 Index AM 适配（v0.9）
 *
 * 设计说明见 spi/btree_am.h。
 */

#include "spi/btree_am.h"

#include <string.h>

static BTree *bt_of(IndexAM *am)
{
    return (BTree *)am->impl;
}

static int bt_insert(IndexAM *am, const u8 *key, u16 klen, Rid rid)
{
    return btree_insert(bt_of(am), key, klen, rid);
}

static int bt_remove(IndexAM *am, const u8 *key, u16 klen, Rid rid)
{
    return btree_delete(bt_of(am), key, klen, rid);
}

static int bt_search(IndexAM *am, const u8 *key, u16 klen,
                     Rid *out, int max, int *n)
{
    return btree_search(bt_of(am), key, klen, out, max, n);
}

/*
 * 范围扫描。
 *
 * 【开区间的处理】
 * B+tree 只支持闭区间。这里返回闭区间 [lo, hi]，
 * 也就是**开区间请求的超集**。
 *
 * 【超集是可以接受的，子集不行】
 * SPI 的契约是：range 返回的是**候选行**，调用方必须自己确认边界。
 * 多给几行 = 调用方重新过滤掉；少给几行 = 数据静默丢失。
 * 所以保守（超集）是安全的，激进（子集）是危险的。
 */
static int bt_range(IndexAM *am,
                    const u8 *lo, u16 llen, bool lo_inc,
                    const u8 *hi, u16 hlen, bool hi_inc,
                    IndexCursor **out)
{
    (void)lo_inc;
    (void)hi_inc;

    BtCursor *cur = NULL;
    int rc = btree_scan(bt_of(am), lo, llen, hi, hlen, &cur);
    if (rc != OK)
        return rc;

    *out = (IndexCursor *)cur;
    return OK;
}

static int bt_cursor_next(IndexAM *am, IndexCursor *c, Rid *rid)
{
    (void)am;
    return btree_cursor_next((BtCursor *)c, rid);
}

static int bt_cursor_end(IndexAM *am, IndexCursor *c)
{
    (void)am;
    if (c)
        btree_cursor_close((BtCursor *)c);
    return OK;
}

/*
 * 近邻查询：B+tree 没有这个能力。
 *
 * 返回 E_NOTSUP —— 优化器据此退回全表扫描。
 * 这就是"能力协商靠返回值"的含义。
 */
static int bt_knn(IndexAM *am, const u8 *probe, u16 plen,
                   int k, Rid *out, float *dist, int *n)
{
    (void)am; (void)probe; (void)plen; (void)k; (void)out; (void)dist; (void)n;
    return E_NOTSUP;
}

int btree_am_build(BTree *bt, IndexAM *out)
{
    if (!bt || !out)
        return E_INVAL;

    out->name = "btree";
    out->caps = IX_CAP_SEARCH | IX_CAP_RANGE;
    out->insert = bt_insert;
    out->remove = bt_remove;
    out->search = bt_search;
    out->range = bt_range;
    out->cursor_next = bt_cursor_next;
    out->cursor_end = bt_cursor_end;
    out->knn = bt_knn;
    out->impl = bt;
    return OK;
}
