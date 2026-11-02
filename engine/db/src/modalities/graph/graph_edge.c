/*
 * graph_edge.c —— 边与邻接链的维护（v0.11 步骤 4 拆分）
 *
 * 设计说明见 modalities/graph/graph.h。
 *
 * 【为什么从 graph.c 里拆出来】
 *
 * graph.c 一度到 852 行，超过项目 800 行的红线。
 * 但"为什么拆"不能只是"文件太长"—— 那只说明需要拆，
 * 不说明该按什么边界拆。
 *
 * 这里的边界是**不变式归属**：
 *   - graph_edge.c：双向链的挂链 / 摘链（A1 守的就是这里的性质）
 *   - graph_walk.c：沿链读一遍（只读，不改结构）
 *   - graph.c：图的建立 / 打开 / 销毁 + 节点的增删 + 统计
 *
 * 拆完之后，"改链会不会碰到读链"这件事是**物理隔离**的，
 * 不是靠自觉。往链里写指针的代码，永远不会出现在读链的代码里。
 *
 * 【这个文件的核心不变式】
 *
 * 每条边 e 同时在两个双向链表里：
 *   - e.src 的出链（e.out_prev / e.out_next 串）
 *   - e.dst 的入链（e.in_prev / e.in_next 串）
 *
 * I1. 走 src 的出链能走到 e；走 dst 的入链也能走到 e
 * I2. 出链里 e 的前驱指向 e，后继被 e 指向；入链同理
 * I3. 链头（src.first_out / dst.first_in）指向的是链上第一条边，
 *     且这条边的 prev 是 GR_RID_NIL
 *
 * A1 验 I1，R1（PLAN §1.7）防的就是"摘链时漏改一边"。
 */

#include "modalities/graph/graph.h"

#include <string.h>

/* ---------------- 挂链 ---------------- */

/*
 * 把一条边挂进 src 的出链**和** dst 的入链。
 *
 * 两条链都是**头插**：
 *   新边的 next = 旧的链头
 *   旧链头的 prev = 新边
 *   节点的 first = 新边
 *
 * 为什么头插而不是尾插：
 *   头插不需要找尾（找尾要遍历整条链，O(度)）→ 加边是 O(1)。
 *   而且图遍历的常见模式是"最近的边最热"（LSI 模型里
 *   节点属性常挂在最近创建的边上），头插让热点在最前面。
 *
 * 代价：链的顺序 = 插入顺序的逆序。不影响正确性。
 *
 * 【为什么必须一次改两个节点】
 *
 * 边的"可见性"是两边的：只挂 src 的出链，dst 就查不到这条入边。
 * 这是 A1 要守的。
 */
static int gr_link_edge(Graph *g, u32 xid, Rid edge,
                        Rid src, Rid dst,
                        u64 old_src_first_out, u64 old_dst_first_in)
{
    u8 ebuf[GR_EDGE_SIZE];
    u8 nbuf[512];

    /* --- 边记录 --- */
    memset(ebuf, 0, sizeof(ebuf));
    gr_rid_set(ebuf, GR_E_SRC, rid_to_u64(src));
    gr_rid_set(ebuf, GR_E_DST, rid_to_u64(dst));
    gr_rid_set(ebuf, GR_E_OUT_PREV, GR_RID_NIL);
    gr_rid_set(ebuf, GR_E_OUT_NEXT, old_src_first_out);
    gr_rid_set(ebuf, GR_E_IN_PREV, GR_RID_NIL);
    gr_rid_set(ebuf, GR_E_IN_NEXT, old_dst_first_in);

    int rc = gr_write_edge(g, xid, edge, ebuf);
    if (rc != OK)
        return rc;

    /* --- 原链头的前驱指向新边 --- */
    if (old_src_first_out != GR_RID_NIL) {
        Rid old_head = rid_from_u64(old_src_first_out);
        u8 obuf[GR_EDGE_SIZE];
        rc = gr_read_edge(g, old_head, obuf);
        if (rc != OK)
            return rc;
        gr_rid_set(obuf, GR_E_OUT_PREV, rid_to_u64(edge));
        rc = gr_write_edge(g, xid, old_head, obuf);
        if (rc != OK)
            return rc;
    }

    if (old_dst_first_in != GR_RID_NIL) {
        Rid old_head = rid_from_u64(old_dst_first_in);
        u8 obuf[GR_EDGE_SIZE];
        rc = gr_read_edge(g, old_head, obuf);
        if (rc != OK)
            return rc;
        gr_rid_set(obuf, GR_E_IN_PREV, rid_to_u64(edge));
        rc = gr_write_edge(g, xid, old_head, obuf);
        if (rc != OK)
            return rc;
    }

    /* --- 两个节点的链头指向新边 --- */
    rc = gr_read_node(g, src, nbuf);
    if (rc != OK)
        return rc;
    gr_rid_set(nbuf, GR_N_FIRST_OUT, rid_to_u64(edge));
    rc = gr_write_node(g, xid, src, nbuf);
    if (rc != OK)
        return rc;

    rc = gr_read_node(g, dst, nbuf);
    if (rc != OK)
        return rc;
    gr_rid_set(nbuf, GR_N_FIRST_IN, rid_to_u64(edge));
    rc = gr_write_node(g, xid, dst, nbuf);
    if (rc != OK)
        return rc;

    return OK;
}

int graph_add_edge(Graph *g, u32 xid, Rid src, Rid dst, Rid *out)
{
    if (!g || !out)
        return E_INVAL;

    u8 nbuf[512];
    if (g->node_size > sizeof(nbuf))
        return E_NOSPACE;

    /* 端点必须存在 */
    int rc = gr_read_node(g, src, nbuf);
    if (rc != OK)
        return rc;
    u64 src_first_out = gr_n_first_out(nbuf);

    rc = gr_read_node(g, dst, nbuf);
    if (rc != OK)
        return rc;
    u64 dst_first_in = gr_n_first_in(nbuf);

    /* 先分配边槽（内容稍后一次性写全，见 gr_link_edge） */
    Rid edge;
    rc = gstore_alloc(g->edges, xid, &edge);
    if (rc != OK)
        return rc;

    rc = gr_link_edge(g, xid, edge, src, dst, src_first_out, dst_first_in);
    if (rc != OK)
        return rc;

    *out = edge;
    return OK;
}

/* ---------------- 摘链 ---------------- */

/*
 * 把一条边从两个链里摘掉（但不释放槽）。
 *
 * 【摘链的四种情况】
 *
 * 边 e 在 src 出链里的位置：
 *   - e 是链头（out_prev == NIL）→ src.first_out = e.out_next
 *   - e 是链尾（out_next == NIL）→ e.out_prev.out_next = NIL
 *   - e 在中间                  → e.out_prev.out_next = e.out_next
 *                                  e.out_next.out_prev = e.out_prev
 *
 * 入链同理（用 in_prev / in_next 和 dst.first_in）。
 *
 * 注意链头那一支和"前驱不存在"是同一件事：
 * 头插的链里，链头的 prev 永远是 NIL。
 * 所以"e.out_prev == NIL" ⟺ "e 是链头" —— 一个判断覆盖两支。
 *
 * 【这是 R1 最容易出错的地方】
 *
 * 出链改四处（链头 / 前驱 / 后继 / 节点链头），
 * 入链也要改四处。漏改一处 → 一个方向查得到、另一个方向查不到。
 * A1 的"加完删、再加、再删"就是专门抓这个的。
 */
static int gr_unlink_edge(Graph *g, u32 xid, Rid edge)
{
    u8 ebuf[GR_EDGE_SIZE];
    int rc = gr_read_edge(g, edge, ebuf);
    if (rc != OK)
        return rc;

    Rid src = rid_from_u64(gr_e_src(ebuf));
    Rid dst = rid_from_u64(gr_e_dst(ebuf));

    u64 out_prev = gr_e_out_prev(ebuf);
    u64 out_next = gr_e_out_next(ebuf);
    u64 in_prev  = gr_e_in_prev(ebuf);
    u64 in_next  = gr_e_in_next(ebuf);

    u8 nbuf[512];

    /* ---------- 出链 ---------- */
    if (out_prev == GR_RID_NIL) {
        /* e 是链头 → 改 src.first_out */
        rc = gr_read_node(g, src, nbuf);
        if (rc != OK)
            return rc;
        gr_rid_set(nbuf, GR_N_FIRST_OUT, out_next);
        rc = gr_write_node(g, xid, src, nbuf);
        if (rc != OK)
            return rc;
    } else {
        /* e 有前驱 → 前驱的 next 改指 e 的 next */
        Rid prev = rid_from_u64(out_prev);
        u8 pbuf[GR_EDGE_SIZE];
        rc = gr_read_edge(g, prev, pbuf);
        if (rc != OK)
            return rc;
        gr_rid_set(pbuf, GR_E_OUT_NEXT, out_next);
        rc = gr_write_edge(g, xid, prev, pbuf);
        if (rc != OK)
            return rc;
    }

    if (out_next != GR_RID_NIL) {
        /* e 的后继的 prev 改指 e 的前驱 */
        Rid next = rid_from_u64(out_next);
        u8 nbuf2[GR_EDGE_SIZE];
        rc = gr_read_edge(g, next, nbuf2);
        if (rc != OK)
            return rc;
        gr_rid_set(nbuf2, GR_E_OUT_PREV, out_prev);
        rc = gr_write_edge(g, xid, next, nbuf2);
        if (rc != OK)
            return rc;
    }

    /* ---------- 入链 ---------- */
    if (in_prev == GR_RID_NIL) {
        /* e 是入链链头 → 改 dst.first_in */
        rc = gr_read_node(g, dst, nbuf);
        if (rc != OK)
            return rc;
        gr_rid_set(nbuf, GR_N_FIRST_IN, in_next);
        rc = gr_write_node(g, xid, dst, nbuf);
        if (rc != OK)
            return rc;
    } else {
        Rid prev = rid_from_u64(in_prev);
        u8 pbuf[GR_EDGE_SIZE];
        rc = gr_read_edge(g, prev, pbuf);
        if (rc != OK)
            return rc;
        gr_rid_set(pbuf, GR_E_IN_NEXT, in_next);
        rc = gr_write_edge(g, xid, prev, pbuf);
        if (rc != OK)
            return rc;
    }

    if (in_next != GR_RID_NIL) {
        Rid next = rid_from_u64(in_next);
        u8 nbuf2[GR_EDGE_SIZE];
        rc = gr_read_edge(g, next, nbuf2);
        if (rc != OK)
            return rc;
        gr_rid_set(nbuf2, GR_E_IN_PREV, in_prev);
        rc = gr_write_edge(g, xid, next, nbuf2);
        if (rc != OK)
            return rc;
    }

    return OK;
}

int graph_remove_edge(Graph *g, u32 xid, Rid edge)
{
    if (!g)
        return E_INVAL;

    int rc = gr_unlink_edge(g, xid, edge);
    if (rc != OK)
        return rc;

    /* 链摘干净了才释放槽 —— 顺序反了会出现"悬空指针指向已释放槽" */
    return gstore_free(g->edges, xid, edge);
}

int graph_edge_endpoints(Graph *g, Rid edge, Rid *src, Rid *dst)
{
    if (!g || !src || !dst)
        return E_INVAL;

    u8 ebuf[GR_EDGE_SIZE];
    int rc = gr_read_edge(g, edge, ebuf);
    if (rc != OK)
        return rc;

    *src = rid_from_u64(gr_e_src(ebuf));
    *dst = rid_from_u64(gr_e_dst(ebuf));
    return OK;
}
