/*
 * graph_am.c — 图模态的 SPI 适配（v0.11 步骤 3）
 *
 * 设计说明见 modalities/graph/graph_am.h。
 *
 * 【本文件的三块】
 *
 *   1. 构造：把 Graph 包装成 StorageAM（两张虚拟表）
 *   2. 读路径：columns / scan / get / decode / visible
 *   3. 写路径：insert（节点 / 边）；update / remove / vacuum / reindex 不支持
 *
 * 下推的实现见 graph_am_push.c。
 *
 * 【行格式】
 *
 *   graph_nodes 一行 = [u64 node_id][节点记录原样]
 *   graph_edges 一行 = [u64 src][u64 dst]
 *
 * 两张表的行格式**互不相同** —— 这正是 SPI 想要的：
 * 模态有自己的行格式，由自己解码（storage_am.h 的"最小公共表示"）。
 * 强行统一成一种格式，就等于逼所有模态长成关系模态。
 *
 * 【为什么 node_id 要放进行字节里，而不能从 RID 现算】
 *
 * decode(am, buf, len, out) 只拿到行字节，拿不到 RID。
 * 所以凡是"这一行的身份"，都必须编码在行字节里。
 *
 * 【为什么 src / dst 存的是"节点 id"而不是别的】
 *
 * 边记录里存的就是 rid_to_u64(端点 RID)（graph.c 的 gr_link_edge）。
 * 节点表暴露的 node_id 也是 rid_to_u64(节点 RID)。
 * 两边编码一致 → 用户可以拿 graph_nodes.node_id 直接当 graph_edges.src 用，
 * 而且"按 src = X 真下推"才成立（X → 反解出节点 RID → 走它的出链）。
 */

#include "modalities/graph/graph_am.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 列定义 ---------------- */

/*
 * 边表的两列。
 *
 * src / dst 都是**节点 id**（RID 的 u64 编码），不是节点槽号、不是自增序号。
 * 编码一致才能让 WHERE src = X 变成"沿 X 的出链走一圈"这种真下推。
 */
static const ColDesc EDGE_COLS[2] = {
    { "src", T_INT8, false },
    { "dst", T_INT8, false }
};

/* ---------------- 工具 ---------------- */

static GraphAmCtx *ctx_of(StorageAM *am)
{
    return ga_ctx_of(am);
}

/* ---------------- 前向声明 ---------------- */

/*
 * 构造函数在前面，回调定义在后面 —— 先声明。
 *
 * 这些回调的类型在 storage_am.h 里已经定死了，
 * 这里只是让 C 编译器知道它们长什么样。
 */
static int ga_columns(StorageAM *am, const ColDesc **cols, int *ncols);
static int ga_scan_begin(StorageAM *am, AmScan **out);
static int ga_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                        u8 *buf, u16 cap, u16 *len);
static int ga_scan_end(StorageAM *am, AmScan *s);
static int ga_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len);
static int ga_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out);
static bool ga_visible(StorageAM *am, const u8 *buf, u16 len,
                       const AmVisCtx *vis);
static int ga_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out);
static int ga_update(StorageAM *am, u32 xid, Rid rid,
                     const DbValue *vals, Rid *new_rid);
static int ga_remove(StorageAM *am, u32 xid, Rid rid);
static int ga_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                     AmVacuumStats *out);
static int ga_reindex(StorageAM *am, u32 index_id, u64 *rows_out);

/* ---------------- 构造 ---------------- */

/*
 * 节点表的列是**动态**的：属性列来自建图时声明的 schema。
 *
 * 所以列定义不能是静态数组，得由 AM 持有（GraphAmCtx.cols）。
 * ctx 归调用方所有（像 BlobAmCtx 一样），生命周期跟着 AM 走。
 */
static int ga_build_node_cols(GraphAmCtx *ctx)
{
    Graph *g = ctx->g;
    int np = graph_col_count(g);

    if (np + 1 > GA_MAX_COLS)
        return E_INVAL;

    memset(ctx->cols, 0, sizeof(ctx->cols));

    /* 第 0 列：node_id —— 由存储层分配，所以可空 */
    strcpy(ctx->cols[0].name, "node_id");
    ctx->cols[0].type = T_INT8;
    ctx->cols[0].nullable = true;

    /* 其余：图的属性列 */
    const GraphCol *gc = graph_cols(g);
    for (int i = 0; i < np; i++) {
        /*
         * 属性列不能和 node_id 重名。
         *
         * 重名的话绑定阶段 bind_expr 按名字找列，
         * 两个同名列到底绑哪个就说不清了 —— 宁可建表失败。
         */
        if (strcmp(gc[i].name, "node_id") == 0)
            return E_INVAL;

        strncpy(ctx->cols[i + 1].name, gc[i].name, COL_NAME_MAX - 1);
        ctx->cols[i + 1].name[COL_NAME_MAX - 1] = '\0';
        ctx->cols[i + 1].type = gc[i].type;
        ctx->cols[i + 1].nullable = gc[i].nullable;
    }

    ctx->ncols = np + 1;
    return OK;
}

static int ga_build(Graph *g, StorageAM *out, GraphAmCtx *ctx, bool is_nodes)
{
    if (!g || !out || !ctx)
        return E_INVAL;

    memset(ctx, 0, sizeof(*ctx));
    ctx->g = g;
    ctx->is_nodes = is_nodes;

    if (is_nodes) {
        int rc = ga_build_node_cols(ctx);
        if (rc != OK)
            return rc;
    } else {
        ctx->ncols = 2;
    }

    out->name = is_nodes ? "graph_nodes" : "graph_edges";

    /*
     * 能力位。
     *
     * 有 INSERT：节点和边都能插。
     * 没有 UPDATE / REMOVE / VACUUM / INDEX：
     *   执行器在调用之前就按能力位拒掉，报"表 %s 不支持更新"——
     *   这是 A9 要的"明确错误，不是崩溃"。
     */
    out->caps = AM_CAP_SCAN | AM_CAP_INSERT | AM_CAP_PUSHDOWN;

    out->columns = ga_columns;
    out->scan_begin = ga_scan_begin;
    out->scan_next = ga_scan_next;
    out->scan_end = ga_scan_end;
    out->get = ga_get;
    out->decode = ga_decode;
    /* 批能力：图模态不支持（PLAN-v1.2 步骤 0：三个模态一行都不用改） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = ga_visible;
    out->insert = ga_insert;
    out->update = ga_update;
    out->remove = ga_remove;
    out->vacuum = ga_vacuum;
    out->reindex = ga_reindex;
    out->impl = ctx;

    out->can_push = graph_am_can_push;
    out->push = graph_am_push;

    return OK;
}

int graph_am_build_nodes(Graph *g, StorageAM *out, GraphAmCtx *ctx)
{
    return ga_build(g, out, ctx, true);
}

int graph_am_build_edges(Graph *g, StorageAM *out, GraphAmCtx *ctx)
{
    return ga_build(g, out, ctx, false);
}

/* ---------------- schema ---------------- */

static int ga_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    GraphAmCtx *c = ctx_of(am);
    if (!c || !cols || !ncols)
        return E_INVAL;

    if (c->is_nodes) {
        *cols = c->cols;
        *ncols = c->ncols;
    } else {
        *cols = EDGE_COLS;
        *ncols = 2;
    }
    return OK;
}

/* ---------------- 扫描 ---------------- */

static int ga_scan_begin(StorageAM *am, AmScan **out)
{
    GraphAmCtx *c = ctx_of(am);
    if (!c || !out)
        return E_INVAL;

    GraphScan *st = calloc(1, sizeof(*st));
    if (!st)
        return E_IO;

    st->ctx = c;
    st->mode = c->is_nodes ? GA_MODE_SCAN_NODES : GA_MODE_SCAN_EDGES;

    /* 游标从页链头开始（全扫模式用） */
    if (c->is_nodes)
        gstore_cursor_init(c->g->nodes, &st->cur);
    else
        gstore_cursor_init(c->g->edges, &st->cur);

    *out = (AmScan *)st;
    return OK;
}

static int ga_scan_end(StorageAM *am, AmScan *s)
{
    (void)am;
    free(s);
    return OK;
}

/*
 * 节点行：[u64 node_id][节点记录原样]
 */
static int ga_row_node(Graph *g, Rid rid, const u8 *rec, u8 *buf,
                       u16 cap, u16 *len)
{
    u16 need = (u16)(GA_NODE_ID_BYTES + g->node_size);
    if (cap < need)
        return E_NOSPACE;

    put_u64(buf, rid_to_u64(rid));
    memcpy(buf + GA_NODE_ID_BYTES, rec, g->node_size);
    *len = need;
    return OK;
}

/*
 * 边行：[u64 src][u64 dst]
 */
static int ga_row_edge(const u8 *rec, u8 *buf, u16 cap, u16 *len)
{
    if (cap < GA_EDGE_ROW_BYTES)
        return E_NOSPACE;

    put_u64(buf, get_u64(rec + GR_E_SRC));
    put_u64(buf + 8, get_u64(rec + GR_E_DST));
    *len = GA_EDGE_ROW_BYTES;
    return OK;
}

/* 扫节点（可带属性列自滤） */
static int ga_next_node(GraphScan *st, Rid *rid, u8 *buf, u16 cap, u16 *len)
{
    Graph *g = st->ctx->g;

    for (;;) {
        Rid r;
        u16 rl = 0;
        int rc = gstore_cursor_next(g->nodes, &st->cur, &r,
                                    st->rec, (u16)sizeof(st->rec), &rl);
        if (rc != OK)
            return rc;               /* E_EOF = 扫完了 */

        /* 自滤：按属性列比较，语义与执行器逐字一致 */
        if (st->mode == GA_MODE_PUSH_NODE_FILTER) {
            DbValue props[GR_MAX_COLS];
            rc = gr_prop_decode(g, st->rec, props, g->ncols);
            if (rc != OK)
                return rc;

            bool matched = false;
            rc = ga_match(st->fop, &props[st->fcol], &st->fval, &matched);
            if (rc != OK)
                return rc;
            if (!matched)
                continue;            /* 不匹配 → 跳过（这一行不给上层） */
        }

        rc = ga_row_node(g, r, st->rec, buf, cap, len);
        if (rc != OK)
            return rc;
        *rid = r;
        return OK;
    }
}

/* 扫边（全扫） */
static int ga_next_edge(GraphScan *st, Rid *rid, u8 *buf, u16 cap, u16 *len)
{
    Graph *g = st->ctx->g;

    for (;;) {
        Rid r;
        u16 rl = 0;
        int rc = gstore_cursor_next(g->edges, &st->cur, &r,
                                    st->rec, (u16)sizeof(st->rec), &rl);
        if (rc != OK)
            return rc;

        rc = ga_row_edge(st->rec, buf, cap, len);
        if (rc != OK)
            return rc;
        *rid = r;
        return OK;
    }
}

/* 按 node_id 直接定位一个节点（等值下推） */
static int ga_next_node_one(GraphScan *st, Rid *rid, u8 *buf, u16 cap, u16 *len)
{
    Graph *g = st->ctx->g;

    if (st->one_done)
        return E_EOF;
    st->one_done = true;

    int rc = gstore_get(g->nodes, st->one, st->rec, g->node_size, NULL);
    if (rc == E_NOTFOUND)
        return E_EOF;                /* 锚点不存在 → 0 行 */
    if (rc != OK)
        return rc;

    rc = ga_row_node(g, st->one, st->rec, buf, cap, len);
    if (rc != OK)
        return rc;
    *rid = st->one;
    return OK;
}

/* 沿邻接链枚举边（src / dst 等值下推） */
static int ga_next_edge_chain(GraphScan *st, Rid *rid, u8 *buf, u16 cap, u16 *len)
{
    Rid e, s, d;

    int rc = graph_edge_cursor_next(st->ctx->g, &st->ec, &e, &s, &d);
    if (rc != OK)
        return rc;

    /*
     * 链上的每条边，端点就是锚点节点。
     * 端点 id = rid_to_u64(端点 RID)。
     *
     * 链游标给的是 Rid，这里转回 id —— 因为行里存的是 id。
     * 反解是精确的：记录里存的 u64 本来就是 rid_to_u64 的产物，
     * 而 rid_to_u64 只用低 48 位（页号 u32 << 16 | 槽号 u16），
     * 所以 u64 → Rid → u64 往返不丢信息。
     */
    if (cap < GA_EDGE_ROW_BYTES)
        return E_NOSPACE;
    put_u64(buf, rid_to_u64(s));
    put_u64(buf + 8, rid_to_u64(d));
    *len = GA_EDGE_ROW_BYTES;
    *rid = e;
    return OK;
}

static int ga_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                        u8 *buf, u16 cap, u16 *len)
{
    (void)am;
    GraphScan *st = (GraphScan *)s;
    if (!st || !rid || !buf || !len)
        return E_INVAL;

    switch (st->mode) {
    case GA_MODE_SCAN_NODES:
    case GA_MODE_PUSH_NODE_FILTER:
        return ga_next_node(st, rid, buf, cap, len);
    case GA_MODE_SCAN_EDGES:
        return ga_next_edge(st, rid, buf, cap, len);
    case GA_MODE_PUSH_NODE_ONE:
        return ga_next_node_one(st, rid, buf, cap, len);
    case GA_MODE_PUSH_EDGE_CHAIN:
        return ga_next_edge_chain(st, rid, buf, cap, len);
    case GA_MODE_EMPTY:
        return E_EOF;
    default:
        return E_INVAL;
    }
}

/* ---------------- 按 RID 取一行 ---------------- */

/*
 * 每张表有自己的 RID 空间：
 *   graph_nodes 的 RID = 节点槽的 (页, 槽)
 *   graph_edges 的 RID = 边槽的 (页, 槽)
 *
 * 拿错表的 RID 过来 → gstore_get 校验页类型 → E_NOTFOUND。
 */

static int ga_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    GraphAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;

    Graph *g = c->g;

    if (c->is_nodes) {
        u8 rec[GA_REC_MAX];
        if (g->node_size > sizeof(rec))
            return E_NOSPACE;

        int rc = gstore_get(g->nodes, rid, rec, g->node_size, NULL);
        if (rc != OK)
            return rc;

        return ga_row_node(g, rid, rec, buf, cap, len);
    }

    u8 rec[GR_EDGE_SIZE];
    int rc = gstore_get(g->edges, rid, rec, GR_EDGE_SIZE, NULL);
    if (rc != OK)
        return rc;

    return ga_row_edge(rec, buf, cap, len);
}

/* ---------------- 解码 ---------------- */

/*
 * 把行字节解成值。
 *
 * graph_nodes：
 *   out[0]      = node_id
 *   out[1..n]   = 属性列（用图层自己的定长解码）
 *
 * graph_edges：
 *   out[0] = src
 *   out[1] = dst
 */
static int ga_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    GraphAmCtx *c = ctx_of(am);
    if (!c || !buf || !out)
        return E_INVAL;

    Graph *g = c->g;

    if (c->is_nodes) {
        if (len < (u16)(GA_NODE_ID_BYTES + g->node_size))
            return E_CORRUPT;

        val_int(&out[0], (i64)get_u64(buf));
        return gr_prop_decode(g, buf + GA_NODE_ID_BYTES, out + 1, g->ncols);
    }

    if (len < GA_EDGE_ROW_BYTES)
        return E_CORRUPT;

    val_int(&out[0], (i64)get_u64(buf));
    val_int(&out[1], (i64)get_u64(buf + 8));
    return OK;
}

/* ---------------- 可见性 ---------------- */

/*
 * 图的行恒可见。
 *
 * 图没有版本链：记录要么在用（in_use = 1）要么在空闲链上（in_use = 0），
 * 没有"这个版本对当前快照可不可见"这回事。
 *
 * 这正是"可见性归模态"的意义：执行器不需要知道图有没有 xmin/xmax。
 */
static bool ga_visible(StorageAM *am, const u8 *buf, u16 len,
                       const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写路径 ---------------- */

/*
 * 插一个节点。
 *
 * vals[0]     = node_id —— 由存储层分配，必须是 NULL
 * vals[1..n]  = 属性值
 *
 * xid 传 0（系统事务）：图走语句级提交，不参与 BEGIN/COMMIT，
 * 详见 graph_am.h 的"写入语义"。
 */
static int ga_insert_node(GraphAmCtx *c, const DbValue *vals, Rid *out)
{
    Graph *g = c->g;

    if (!vals[0].is_null)
        return E_INVAL;      /* node_id 由存储层分配，用户不能指定 */

    return graph_create_node(g, 0, vals + 1, g->ncols, out);
}

/*
 * 插一条边。
 *
 * vals[0] = src（节点 id）
 * vals[1] = dst（节点 id）
 *
 * 端点必须存在 —— graph_add_edge 内部会检查，
 * 不存在返回 E_NOTFOUND（而不是建出一条悬空边）。
 */
static int ga_insert_edge(GraphAmCtx *c, const DbValue *vals, Rid *out)
{
    Graph *g = c->g;

    Rid src, dst;
    int rc = ga_nodeid_to_rid(&vals[0], &src);
    if (rc != OK)
        return rc;
    rc = ga_nodeid_to_rid(&vals[1], &dst);
    if (rc != OK)
        return rc;

    return graph_add_edge(g, 0, src, dst, out);
}

static int ga_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)xid;                 /* 图的 WAL 用系统事务 xid = 0 */
    GraphAmCtx *c = ctx_of(am);
    if (!c || !vals || !out)
        return E_INVAL;

    return c->is_nodes ? ga_insert_node(c, vals, out)
                        : ga_insert_edge(c, vals, out);
}

/*
 * 图不支持的写操作。
 *
 * caps 里没声明这些能力位，执行器在调用之前就拒掉了（"表 %s 不支持更新"）。
 * 这里返回 E_NOTSUP 是双保险 —— 万一有人绕过 caps 直接调。
 */
static int ga_update(StorageAM *am, u32 xid, Rid rid,
                     const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int ga_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

/* 图没有版本 → 没有死版本 → 没有可回收的东西 */
static int ga_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                     AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;
}

/* 图的行不是关系行，v0.11 没有图索引 */
static int ga_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)am; (void)index_id; (void)rows_out;
    return E_NOTSUP;
}
