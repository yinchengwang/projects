/*
 * graph.c — 图存储：定长记录 + 双向邻接链（v0.11 步骤 2）
 *
 * 设计说明见 modalities/graph/graph.h。
 *
 * 【本文件的三块】
 *
 *   1. 建 / 拆：两个 gstore（节点页 + 边页）+ schema 计算
 *   2. 节点 / 边的增删
 *   3. 邻接链的增删摘（核心）
 *
 * 【邻接链的核心不变式】
 *
 * 每条边 e 同时在两个双向链表里：
 *   - e.src 的出链（e.out_prev / e.out_next 串）
 *   - e.dst 的入链（e.in_prev / e.in_next 串）
 *
 * 不变式（每条边都必须满足）：
 *   I1. 走 src 的出链能走到 e；走 dst 的入链也能走到 e
 *   I2. 出链里 e 的前驱指向 e，后继被 e 指向；入链同理
 *   I3. 链头（src.first_out / dst.first_in）指向的是链上第一条边，
 *       且这条边的 prev 是 GR_RID_NIL
 *
 * A1 验的是 I1 的一致性（出链集合 == 入链集合），
 * A2 验的是"删节点不动别人的链"。
 */

#include "modalities/graph/graph.h"

#include "common/crc.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 小工具 ---------------- */

/* 记录内字段的读写助手（gr_rid_get / gr_n_first_out / gr_e_src …）
 * 都在 graph.h 里 —— 定义必须只有一处，见那里的说明。
 */

/* ---------------- 建 / 拆 ---------------- */

/*
 * 算属性 schema：列宽 + 各列偏移 + 位图字节数 + 节点记录总长。
 *
 * 【为什么节点记录大小是算出来的，不是常量】
 *
 * 边记录是定长 52（字段固定）。节点记录多一个属性区，
 * 而属性区大小取决于建图时声明的列。
 * gstore 的 rec_size 是**建 gstore 时**定死的，
 * 所以 schema 必须先算清楚，再建节点 gstore。
 */
static int gr_calc_schema(Graph *g, const GraphCol *cols, int ncols)
{
    if (ncols < 0 || ncols > GR_MAX_COLS)
        return E_INVAL;

    g->ncols = ncols;
    u32 bitmap_bytes = (u32)((ncols + 7) / 8);

    /*
     * 记录布局（graph.h 的记录布局一节）：
     *
     *   [0]      gstore 公共头 4B
     *   [4]      first_out 8B
     *   [12]     first_in  8B
     *   [20]     NULL 位图 bitmap_bytes 字节
     *   [20+bb]  各列定长值
     *
     * 位图只在**开头占一次**。列偏移必须从"位图之后"开始递增，
     * 每列推进自己的宽度 —— 位图不参与每列的步进。
     */
    u16 off = (u16)(GR_N_BITMAP + bitmap_bytes);

    for (int i = 0; i < ncols; i++) {
        const GraphCol *c = &cols[i];
        u16 w = 0;

        switch (c->type) {
        case T_INT4: w = 4; break;
        case T_INT8: w = 8; break;
        case T_BOOL: w = 1; break;
        case T_TEXT:
            /* 定长文本必须声明宽度 —— 没有宽度就没法定长 */
            if (c->width == 0)
                return E_INVAL;
            /*
             * 宽度不能超过值层能存的上限。
             *
             * DbValue.s 是 256 字节，val_text 最长存 255 字节内容。
             * 声明一个 300 字节宽的 TEXT 列，等于承诺能存 300 字节 ——
             * 但值层根本装不下，最后一步截断在 255：
             * **存进去的和读出来的不是一回事**（静默数据错误）。
             * 所以在这里直接拒掉。
             */
            if (c->width > (u16)(VAL_TEXT_MAX - 1))
                return E_INVAL;
            w = c->width;
            break;
        default:
            return E_INVAL;
        }

        g->cols[i] = *c;
        g->cols[i].width = w;      /* 统一成实际宽度 */
        g->col_off[i] = off;
        off = (u16)(off + w);
    }

    /* 属性区（位图 + 各列）不能超过上限 */
    if (off > GR_PROP_MAX + GR_N_BITMAP)
        return E_NOSPACE;

    g->bitmap_bytes = (u16)bitmap_bytes;
    g->node_size = off;
    return OK;
}

/* ---------------- 建 / 拆 ---------------- */

/*
 * 建 schema gstore 并接管磁盘上已有的链。
 *
 * schema gstore 的页类型是 PT_GRAPH_SCHEMA，和节点页 / 边页都不同 ——
 * 所以扫全文件找链头时，三者互不干扰。
 */
static int gr_open_schema_store(Graph *g)
{
    int rc = gstore_create(g->pg, g->wal, g->wal_ctx,
                            GR_SCHEMA_REC_SIZE, PT_GRAPH_SCHEMA, &g->schema);
    if (rc != OK)
        return rc;
    return gstore_recover(g->schema);
}

/*
 * 建节点 / 边两个 gstore，并接管磁盘上已有的链。
 *
 * rec_size 必须和建图时算出来的记录长度一致 —— 节点记录的 rec_size
 * 来自 schema，所以这一步只能在 schema 定下来之后做。
 */
static int gr_build_stores(Graph *g)
{
    int rc = gstore_create(g->pg, g->wal, g->wal_ctx, g->node_size,
                           PT_GRAPH_NODE, &g->nodes);
    if (rc != OK)
        return rc;

    rc = gstore_create(g->pg, g->wal, g->wal_ctx, GR_EDGE_SIZE,
                       PT_GRAPH_EDGE, &g->edges);
    if (rc != OK) {
        gstore_destroy(g->nodes);
        g->nodes = NULL;
        return rc;
    }

    /* 接管磁盘上已有的图（重启场景） */
    rc = gstore_recover(g->nodes);
    if (rc == OK)
        rc = gstore_recover(g->edges);
    if (rc != OK) {
        gstore_destroy(g->nodes);
        gstore_destroy(g->edges);
        g->nodes = NULL;
        g->edges = NULL;
        return rc;
    }
    return OK;
}

/* 释放半成品（出错路径专用） */
static void gr_free_partial(Graph *g)
{
    if (!g)
        return;
    gstore_destroy(g->schema);
    gstore_destroy(g->nodes);
    gstore_destroy(g->edges);
    free(g);
}

int graph_create(Pager *pg, LogHookFn fn, void *ctx,
                 const GraphCol *cols, int ncols, Graph **out)
{
    if (!pg || !out)
        return E_INVAL;
    if (ncols > 0 && !cols)
        return E_INVAL;

    Graph *g = calloc(1, sizeof(*g));
    if (!g)
        return E_IO;

    g->pg = pg;
    g->wal = fn;
    g->wal_ctx = ctx;

    int rc = gr_open_schema_store(g);
    if (rc != OK) {
        gr_free_partial(g);
        return rc;
    }

    /* 磁盘上已经有 schema 了吗？ */
    GraphCol old[GR_MAX_COLS];
    int oldn = 0;
    u16 oldsize = 0;
    int rc_read = gr_schema_read(g->schema, old, GR_MAX_COLS, &oldn, &oldsize);
    bool has_old;
    if (rc_read == OK) {
        has_old = true;
    } else if (rc_read == E_NOTFOUND) {
        has_old = false;
    } else {
        /* schema 记录读不懂 = 磁盘上的东西坏了，不能当成"没有" */
        gr_free_partial(g);
        return rc_read;
    }

    rc = gr_calc_schema(g, cols, ncols);
    if (rc != OK) {
        gr_free_partial(g);
        return rc;
    }

    if (has_old) {
        /*
         * 已有 schema：必须**完全一致**。
         *
         * 不一致意味着磁盘上的节点记录和这次的布局对不上 ——
         * 用新布局去解释旧记录 = 读出垃圾，而且不报错。
         * 宁可开不了这个图，也不能让它带着错误的数据跑。
         */
        if (!gr_schema_equal(old, oldn, oldsize,
                             g->cols, g->ncols, g->node_size)) {
            gr_free_partial(g);
            return E_INVAL;
        }
    } else {
        /* 没有 schema → 新建，并把 schema 写进磁盘 */
        rc = gr_schema_write(g->schema, g->cols, g->ncols, g->node_size);
        if (rc != OK) {
            gr_free_partial(g);
            return rc;
        }
    }

    rc = gr_build_stores(g);
    if (rc != OK) {
        gr_free_partial(g);
        return rc;
    }

    g->ready = true;
    *out = g;
    return OK;
}

int graph_open(Pager *pg, LogHookFn fn, void *ctx, Graph **out)
{
    if (!pg || !out)
        return E_INVAL;

    Graph *g = calloc(1, sizeof(*g));
    if (!g)
        return E_IO;

    g->pg = pg;
    g->wal = fn;
    g->wal_ctx = ctx;

    int rc = gr_open_schema_store(g);
    if (rc != OK) {
        gr_free_partial(g);
        return rc;
    }

    /* schema 从磁盘读 —— 调用方不用（也不该）再提供 */
    GraphCol cols[GR_MAX_COLS];
    int ncols = 0;
    u16 node_size = 0;
    rc = gr_schema_read(g->schema, cols, GR_MAX_COLS, &ncols, &node_size);
    if (rc != OK) {
        /* E_NOTFOUND = 磁盘上还没建过图；E_CORRUPT = schema 坏了 */
        gr_free_partial(g);
        return rc;
    }

    rc = gr_calc_schema(g, cols, ncols);
    if (rc != OK) {
        /* 磁盘上的 schema 不合法 → 数据损坏（不是调用方的错） */
        gr_free_partial(g);
        return E_CORRUPT;
    }

    /* 交叉校验：列宽算出来的记录长度必须和当初存的一致 */
    if (g->node_size != node_size) {
        gr_free_partial(g);
        return E_CORRUPT;
    }

    rc = gr_build_stores(g);
    if (rc != OK) {
        gr_free_partial(g);
        return rc;
    }

    g->ready = true;
    *out = g;
    return OK;
}

void graph_destroy(Graph *g)
{
    if (!g)
        return;
    gstore_destroy(g->schema);
    gstore_destroy(g->nodes);
    gstore_destroy(g->edges);
    free(g);
}

bool graph_ready(const Graph *g)
{
    return g ? g->ready : false;
}

int graph_col_count(const Graph *g) { return g ? g->ncols : 0; }
const GraphCol *graph_cols(const Graph *g) { return g ? g->cols : NULL; }
u16 graph_node_record_size(const Graph *g) { return g ? g->node_size : 0; }

/* ---------------- 读记录 ---------------- */

/* gr_read_node / gr_read_edge / gr_write_node / gr_write_edge
 * 都在 graph.h 里（static inline）—— 定义只有一处，见那里的说明。
 */

/* ---------------- 节点 ---------------- */

int graph_create_node(Graph *g, u32 xid, const DbValue *props,
                      int nprops, Rid *out)
{
    if (!g || !out)
        return E_INVAL;
    if (nprops != g->ncols)
        return E_INVAL;

    u8 rec[512];
    if (g->node_size > sizeof(rec))
        return E_NOSPACE;
    memset(rec, 0, sizeof(rec));

    /*
     * 空指针必须是显式的 GR_RID_NIL。
     *
     * 为什么不能靠"清零 = 空"：清零后的 first_out = 0，
     * 而 0 是一个**看起来合法**的 RID（page 0）。
     * 图模态的"没有邻居"必须和"有个 page 0 的邻居"区分开。
     */
    gr_rid_set(rec, GR_N_FIRST_OUT, GR_RID_NIL);
    gr_rid_set(rec, GR_N_FIRST_IN, GR_RID_NIL);

    int rc = gr_prop_encode(g, props, nprops, rec);
    if (rc != OK)
        return rc;

    /* 分配即写全字段：内容写好才让它可见（PLAN §1.7 R3） */
    return gstore_alloc_with(g->nodes, xid, rec, g->node_size, out);
}

int graph_node_props(Graph *g, Rid node, DbValue *out, int cap)
{
    if (!g || !out)
        return E_INVAL;
    if (cap < g->ncols)
        return E_NOSPACE;

    u8 rec[512];
    if (g->node_size > sizeof(rec))
        return E_NOSPACE;

    int rc = gr_read_node(g, node, rec);
    if (rc != OK)
        return rc;

    return gr_prop_decode(g, rec, out, cap);
}

int graph_remove_node(Graph *g, u32 xid, Rid node)
{
    if (!g)
        return E_INVAL;

    /*
     * 删节点 = 删掉它的所有出边和入边，然后释放节点槽。
     *
     * 【为什么不能只删边不摘链】
     *
     * 别的节点的链里还挂着指向这个节点的边 → 那些节点一跳就跳到
     * 一个已释放的槽 → 读到垃圾。
     *
     * 【为什么要摘干净再释放】
     *
     * 顺序反了（先释放节点槽，后摘链）→ 摘链时要读节点记录，
     * 读到的已经是复用后的内容 → 链指针改到别处 → 断链。
     * 所以：先摘边（边都还活着，可以安全读），再释放节点。
     */
    u8 nbuf[512];
    if (g->node_size > sizeof(nbuf))
        return E_NOSPACE;

    /* 先摘出边（反复摘，直到出链空） */
    for (;;) {
        int rc = gr_read_node(g, node, nbuf);
        if (rc == E_NOTFOUND)
            return E_NOTFOUND;
        if (rc != OK)
            return rc;

        u64 first_out = gr_n_first_out(nbuf);
        if (first_out == GR_RID_NIL)
            break;

        rc = graph_remove_edge(g, xid, rid_from_u64(first_out));
        if (rc != OK)
            return rc;
    }

    /* 再摘入边 */
    for (;;) {
        int rc = gr_read_node(g, node, nbuf);
        if (rc == E_NOTFOUND)
            return E_NOTFOUND;
        if (rc != OK)
            return rc;

        u64 first_in = gr_n_first_in(nbuf);
        if (first_in == GR_RID_NIL)
            break;

        rc = graph_remove_edge(g, xid, rid_from_u64(first_in));
        if (rc != OK)
            return rc;
    }

    /* 边都摘干净了 → 释放节点槽 */
    return gstore_free(g->nodes, xid, node);
}

/* ---------------- 统计 / 枚举 ---------------- */

u64 graph_node_count(Graph *g)
{
    return g ? gstore_alive_count(g->nodes) : 0;
}

u64 graph_edge_count(Graph *g)
{
    return g ? gstore_alive_count(g->edges) : 0;
}

typedef struct {
    GraphEachFn fn;
    void *ctx;
} GrEachAdapter;

static int gr_each_adapter(void *ctx, Rid rid, const u8 *rec)
{
    GrEachAdapter *a = (GrEachAdapter *)ctx;
    (void)rec;
    return a->fn(a->ctx, rid);
}

int graph_each_node(Graph *g, GraphEachFn fn, void *ctx)
{
    if (!g || !fn)
        return E_INVAL;
    GrEachAdapter a = { fn, ctx };
    return gstore_foreach(g->nodes, gr_each_adapter, &a);
}

int graph_each_edge(Graph *g, GraphEachFn fn, void *ctx)
{
    if (!g || !fn)
        return E_INVAL;
    GrEachAdapter a = { fn, ctx };
    return gstore_foreach(g->edges, gr_each_adapter, &a);
}
