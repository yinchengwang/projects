/*
 * graph_cursor.c — 邻接链的增量游标（v0.11 步骤 3）
 *
 * 设计说明见 modalities/graph/graph.h 的"边链的增量游标"一节。
 *
 * 【为什么单独一个文件】
 *
 * graph.c 已经 683 行，再塞进游标实现会逼近 800 行的红线。
 * 而且游标是**只读**的 —— graph.c 管的是写路径（建/删/摘链）。
 * 分开之后"改写路径不会碰到只读遍历"是物理隔离的。
 *
 * 【游标的链尾表示】
 *
 * 头文件里的注释写的是"cur = GR_RID_NIL 表示链走完"，
 * 但 GR_RID_NIL 是 u64，而 cur 是 Rid（page + slot）。
 * 两者能对上：
 *
 *   rid_from_u64(GR_RID_NIL) → page = (u32)(GR_RID_NIL >> 16) = 0xFFFFFFFF
 *                           → 正好是 PAGE_ID_NIL
 *                           → rid_is_nil() 为真
 *
 * 所以链尾统一用 rid_is_nil(cur) 判断，和全工程其他地方一致。
 *
 * 【每跳一次读什么】
 *
 * 一跳 = 读一条边记录（52 字节，定长）。
 * 出度为 K 的节点 = 1 次节点页读（open 时）+ K 次边页读。
 * 边记录定长 → 边页内定位是纯算术。
 */

#include "modalities/graph/graph.h"

#include <string.h>

/* 读节点记录里的链头 */
static int gr_cursor_head(Graph *g, Rid node, bool out_dir, u64 *head)
{
    u8 rec[512];
    if (g->node_size > sizeof(rec))
        return E_NOSPACE;

    int rc = gstore_get(g->nodes, node, rec, g->node_size, NULL);
    if (rc != OK)
        return rc;          /* 节点不存在 → E_NOTFOUND */

    u32 off = out_dir ? GR_N_FIRST_OUT : GR_N_FIRST_IN;
    *head = get_u64(rec + off);
    return OK;
}

int graph_edge_cursor_open(Graph *g, Rid node, bool out_dir, GraphEdgeCursor *c)
{
    if (!g || !c)
        return E_INVAL;

    u64 head = GR_RID_NIL;
    int rc = gr_cursor_head(g, node, out_dir, &head);
    if (rc != OK)
        return rc;

    c->cur = rid_from_u64(head);
    c->out_dir = out_dir;
    c->steps = 0;
    return OK;
}

int graph_edge_cursor_next(Graph *g, GraphEdgeCursor *c,
                           Rid *edge, Rid *src, Rid *dst)
{
    if (!g || !c)
        return E_INVAL;

    /* 链走完 */
    if (rid_is_nil(c->cur))
        return E_EOF;

    /*
     * 环检测。
     *
     * 一条链最多走"边总数"步（每步换一个边槽）。
     * 走超了说明链成环（写路径出 bug 或数据损坏）→ E_CORRUPT。
     * 同样的守卫在 graph.c 的 gr_walk 里也有 —— 两处都要有，
     * 因为一个防的是"链指针写错"，一个防的是"边记录被复用后指回自己"。
     */
    u64 guard = gstore_alive_count(g->edges) + 1;
    if (c->steps >= guard)
        return E_CORRUPT;
    c->steps++;

    u8 rec[GR_EDGE_SIZE];
    int rc = gstore_get(g->edges, c->cur, rec, GR_EDGE_SIZE, NULL);
    if (rc != OK)
        return rc;

    if (edge)
        *edge = c->cur;
    if (src)
        *src = rid_from_u64(get_u64(rec + GR_E_SRC));
    if (dst)
        *dst = rid_from_u64(get_u64(rec + GR_E_DST));

    /* 前进到链上的下一条边 */
    u32 noff = c->out_dir ? GR_E_OUT_NEXT : GR_E_IN_NEXT;
    c->cur = rid_from_u64(get_u64(rec + noff));

    return OK;
}
