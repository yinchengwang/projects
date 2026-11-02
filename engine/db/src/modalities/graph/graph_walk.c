/*
 * graph_walk.c —— 沿邻接链走一步（v0.11 步骤 4 拆分）
 *
 * 设计说明见 modalities/graph/graph.h。
 *
 * 【这个文件只读，不改结构】
 *
 * 挂链 / 摘链在 graph_edge.c。这里只做一件事：
 * 从某个节点出发，沿一条链把邻居逐个读出来。
 *
 * 拆开的理由不是"文件太长"，而是**不变式归属**：
 * 写链指针的代码和读链指针的代码物理隔离，
 * "改遍历逻辑会不会不小心改坏链"这种担心就不存在了。
 *
 * 【为什么一次跳只读 K 个边页】
 *
 * 邻接链的指针在边记录里 → 每跳一个邻居要读一条边记录。
 * 出度为 K 的节点 = 1 次节点页读 + K 次边页读。
 * 边记录定长 → 边页内的定位是纯算术（PLAN §1.2）。
 */

#include "modalities/graph/graph.h"

/*
 * 沿一条链走一步。
 *
 * dir = OUT：走 src 的出链，每条边的 dst 是邻居
 * dir = IN ：走 dst 的入链，每条边的 src 是邻居
 */
typedef enum { GR_DIR_OUT, GR_DIR_IN } GrDir;

static int gr_walk(Graph *g, Rid node, GrDir dir, Rid *out, u32 cap, u32 *n)
{
    if (!g || !out || !n)
        return E_INVAL;

    *n = 0;

    u8 nbuf[512];
    if (g->node_size > sizeof(nbuf))
        return E_NOSPACE;

    int rc = gr_read_node(g, node, nbuf);
    if (rc != OK)
        return rc;

    u64 cur = (dir == GR_DIR_OUT) ? gr_n_first_out(nbuf) : gr_n_first_in(nbuf);

    /* 环检测：一条链最多走"边总数"步（每步换一个边槽） */
    u64 guard = gstore_alive_count(g->edges) + 1;

    while (cur != GR_RID_NIL) {
        if (guard-- == 0)
            return E_CORRUPT;      /* 链成环了 */

        u8 ebuf[GR_EDGE_SIZE];
        rc = gr_read_edge(g, rid_from_u64(cur), ebuf);
        if (rc != OK)
            return rc;

        if (*n < cap) {
            u64 nb = (dir == GR_DIR_OUT) ? gr_e_dst(ebuf) : gr_e_src(ebuf);
            out[(*n)++] = rid_from_u64(nb);
        } else {
            /* 缓冲区满了还要继续走吗？
             * 不走 —— 但调用方拿到的数量会少于真实出度。
             * 所以约定：返回 E_NOSPACE 让调用方知道要重试。 */
            return E_NOSPACE;
        }

        cur = (dir == GR_DIR_OUT) ? gr_e_out_next(ebuf) : gr_e_in_next(ebuf);
    }

    return OK;
}

int graph_out_neighbors(Graph *g, Rid node, Rid *out, u32 cap, u32 *n)
{
    return gr_walk(g, node, GR_DIR_OUT, out, cap, n);
}

int graph_in_neighbors(Graph *g, Rid node, Rid *out, u32 cap, u32 *n)
{
    return gr_walk(g, node, GR_DIR_IN, out, cap, n);
}
