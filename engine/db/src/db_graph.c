/*
 * db_graph.c —— 图模态的公开 API（v0.11 步骤 4）
 *
 * 设计说明见 include/db.h 和 src/modalities/graph/graph.h。
 *
 * 【这一层解决什么】
 *
 * 图有两套入口，职责不同：
 *
 *   1. SQL 侧：两张虚拟表 graph_nodes / graph_edges
 *      → 标量查询、谓词下推（WHERE src = X 走真下推）
 *
 *   2. C API 侧（本文件）：遍历原语
 *      → SQL 表达不了递归，"3 跳"只能走这里
 *
 * 两套入口操作的是**同一张图**（同一个 Graph 对象），
 * 所以 SQL 插进去的节点，C API 一样能跳到。
 *
 * 【为什么公开 API 全部持锁】
 *
 * 硬规则 8：新入口路径必须加锁。
 * 图的页分配、邻接链指针、空闲链全是**实例级共享状态** ——
 * 不持锁的话，并发写会互相踩链。
 *
 * 全局 latch 是可重入的，所以 db_graph_walk 内部调 db_graph_hop
 * 不会把自己锁死。
 *
 * 【事务语义】
 *
 * 图走语句级提交（PLAN §1.4 判决 b）：写路径的 WAL xid 传 0，
 * 不参与 BEGIN/COMMIT。所以本文件里所有写操作都传 xid = 0，
 * 而且**不碰连接的事务状态**（不申请 xid、不写 clog）。
 *
 * 这一点和关系模态正好相反：那边一次 db_exec = 一个事务，
 * 那边失败了能回滚；这边一条 db_graph_add_node 立刻生效，
 * 没有"回滚到建图之前"这回事。
 */

#include "db_internal.h"
#include "db_graph_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"

/* ---------------- 小工具 ---------------- */

/* 写一条详细错误（连接私有）。 */
static void dg_err(DB *db, const char *msg)
{
    if (db)
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
}

/* 取实例里的图。没建图 → NULL。 */
static Graph *dg_graph(DB *db)
{
    return db ? db->inst->graph : NULL;
}

/*
 * 公共类型 → 内部类型。
 *
 * 【为什么不用数字直接赋值】
 *
 * db_graph_type 的取值（1/2/3/4）碰巧和内部的 ColType 一样，
 * 但"碰巧一样"不是约定 —— 内部枚举一改，公开 API 就静默错位。
 * 所以这里逐个 switch 映射，改一处只需改这一处。
 */
static int dg_to_dbvalue(const db_graph_value *v, DbValue *out)
{
    if (!v || !out)
        return E_INVAL;

    memset(out, 0, sizeof(*out));

    switch (v->type) {
    case DB_GRAPH_INT4:
        val_int(out, (i32)v->i);
        out->type = T_INT4;
        break;
    case DB_GRAPH_INT8:
        val_int(out, v->i);
        out->type = T_INT8;
        break;
    case DB_GRAPH_BOOL:
        val_bool(out, v->i != 0);
        out->type = T_BOOL;
        break;
    case DB_GRAPH_TEXT: {
        /* 定长文本：按实际长度存，剩余补 0（写进记录时是定长槽） */
        size_t n = strnlen(v->s, DB_GRAPH_TEXT_MAX);
        val_text(out, v->s, (u16)n);
        out->type = T_TEXT;
        break;
    }
    default:
        return E_INVAL;
    }

    if (v->is_null) {
        val_null(out);
        out->type = v->type;    /* NULL 也得带类型，否则解出来是"无类型 NULL" */
    }
    return OK;
}

/* 内部类型 → 公共类型 */
static void dg_from_dbvalue(const DbValue *v, db_graph_value *out)
{
    memset(out, 0, sizeof(*out));

    switch (v->type) {
    case T_INT4:
        out->type = DB_GRAPH_INT4;
        out->i = (i32)v->i;
        break;
    case T_INT8:
        out->type = DB_GRAPH_INT8;
        out->i = v->i;
        break;
    case T_BOOL:
        out->type = DB_GRAPH_BOOL;
        out->i = v->i ? 1 : 0;
        break;
    case T_TEXT:
    default:
        out->type = DB_GRAPH_TEXT;
        {
            u16 n = v->len;
            if (n > DB_GRAPH_TEXT_MAX - 1)
                n = DB_GRAPH_TEXT_MAX - 1;
            memcpy(out->s, v->s, n);
            out->s[n] = '\0';
        }
        break;
    }

    out->is_null = v->is_null ? 1 : 0;
}

/* ---------------- 注册虚拟表 ---------------- */

/*
 * 把图的两张虚拟表接进注册表。
 *
 * 和 blob 的接法同款：建两个 StorageAM，把名字注册进去。
 * 执行器只认 StorageAM，不知道图的存在（这正是 SPI 要证明的事）。
 */
int db_graph_register(DbInstance *inst)
{
    if (!inst || !inst->graph || !inst->reg)
        return E_INVAL;

    int rc = graph_am_build_nodes(inst->graph, &inst->graph_am_nodes,
                                  &inst->graph_ctx_nodes);
    if (rc != OK)
        return rc;

    rc = graph_am_build_edges(inst->graph, &inst->graph_am_edges,
                               &inst->graph_ctx_edges);
    if (rc != OK)
        return rc;

    rc = spi_registry_add(inst->reg, "graph_nodes", &inst->graph_am_nodes);
    if (rc != OK)
        return rc;

    return spi_registry_add(inst->reg, "graph_edges", &inst->graph_am_edges);
}

/* ---------------- 建图 ---------------- */

db_err db_graph_create(DB *db, const db_graph_col *cols, int ncols)
{
    if (!db)
        return DB_E_INVAL;
    if (ncols < 0 || ncols > GR_MAX_COLS) {
        dg_err(db, "属性列数超出范围");
        return DB_E_INVAL;
    }
    if (ncols > 0 && !cols) {
        dg_err(db, "属性列定义为空");
        return DB_E_INVAL;
    }

    /* 公共列定义 → 内部列定义 */
    GraphCol gc[GR_MAX_COLS];
    memset(gc, 0, sizeof(gc));
    for (int i = 0; i < ncols; i++) {
        strncpy(gc[i].name, cols[i].name, COL_NAME_MAX - 1);
        gc[i].name[COL_NAME_MAX - 1] = '\0';
        gc[i].type = (u8)cols[i].type;
        gc[i].width = (u16)cols[i].width;
        gc[i].nullable = cols[i].nullable ? true : false;
    }

    DbInstance *inst = db->inst;

    latch_lock();

    if (inst->graph_ready) {
        latch_unlock();
        dg_err(db, "图已经存在，属性 schema 不可更改");
        return DB_E_INVAL;
    }

    Graph *g = NULL;
    int rc = graph_create(inst->pager, db_wal_hook, inst, gc, ncols, &g);
    if (rc == OK) {
        inst->graph = g;
        inst->graph_ready = true;

        rc = db_graph_register(inst);
        if (rc != OK) {
            /*
             * 注册失败（只可能是名字被占）。
             * 回滚内存状态 —— 但 schema 已经落盘了，
             * 所以下次打开时这张图会被接管（那是对的行为：
             * 图确实建出来了，只是这次没接进 SQL）。
             */
            inst->graph = NULL;
            inst->graph_ready = false;
            graph_destroy(g);
        }
    }

    latch_unlock();

    if (rc != OK) {
        dg_err(db, "建图失败");
        return map_rc(rc);
    }
    return DB_OK;
}

int db_graph_exists(DB *db)
{
    if (!db)
        return 0;
    return db->inst->graph_ready ? 1 : 0;
}

/* ---------------- 节点 ---------------- */

db_err db_graph_add_node(DB *db, const db_graph_value *props, int nprops,
                         db_graph_rid *out)
{
    if (!db || !out)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在，请先建图");
        return DB_E_NOTFOUND;
    }

    if (nprops != graph_col_count(g)) {
        dg_err(db, "属性值个数与建图时声明的列数不一致");
        return DB_E_INVAL;
    }

    DbValue dv[GR_MAX_COLS];
    for (int i = 0; i < nprops; i++) {
        int rc = dg_to_dbvalue(&props[i], &dv[i]);
        if (rc != OK) {
            dg_err(db, "属性值类型不合法");
            return map_rc(rc);
        }
    }

    latch_lock();
    Rid rid;
    int rc = graph_create_node(g, 0, dv, nprops, &rid);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "建节点失败");
        return map_rc(rc);
    }

    *out = rid_to_u64(rid);
    return DB_OK;
}

db_err db_graph_get_node(DB *db, db_graph_rid node,
                         db_graph_value *out, int cap, int *n_out)
{
    if (!db || !out)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    int ncols = graph_col_count(g);
    if (cap < ncols) {
        dg_err(db, "输出缓冲太小");
        return DB_E_NOSPACE;
    }

    DbValue dv[GR_MAX_COLS];
    latch_lock();
    int rc = graph_node_props(g, rid_from_u64(node), dv, ncols);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "节点不存在");
        return map_rc(rc);
    }

    for (int i = 0; i < ncols; i++)
        dg_from_dbvalue(&dv[i], &out[i]);

    if (n_out)
        *n_out = ncols;
    return DB_OK;
}

db_err db_graph_remove_node(DB *db, db_graph_rid node)
{
    if (!db)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    int rc = graph_remove_node(g, 0, rid_from_u64(node));
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "删节点失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/* ---------------- 边 ---------------- */

db_err db_graph_add_edge(DB *db, db_graph_rid src, db_graph_rid dst,
                         db_graph_rid *out)
{
    if (!db || !out)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    Rid edge;
    int rc = graph_add_edge(g, 0, rid_from_u64(src), rid_from_u64(dst), &edge);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "加边失败（端点必须存在）");
        return map_rc(rc);
    }

    *out = rid_to_u64(edge);
    return DB_OK;
}

db_err db_graph_remove_edge(DB *db, db_graph_rid edge)
{
    if (!db)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    int rc = graph_remove_edge(g, 0, rid_from_u64(edge));
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "删边失败");
        return map_rc(rc);
    }
    return DB_OK;
}

db_err db_graph_get_edge(DB *db, db_graph_rid edge,
                         db_graph_rid *src, db_graph_rid *dst)
{
    if (!db || !src || !dst)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    Rid s, d;
    latch_lock();
    int rc = graph_edge_endpoints(g, rid_from_u64(edge), &s, &d);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "边不存在");
        return map_rc(rc);
    }

    *src = rid_to_u64(s);
    *dst = rid_to_u64(d);
    return DB_OK;
}

/* ---------------- 遍历 ---------------- */

db_err db_graph_hop(DB *db, db_graph_rid node, db_graph_rid **out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    int rc = dg_hop_locked(g, rid_from_u64(node), false, out, n_out);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "跳一步失败");
        return map_rc(rc);
    }
    return DB_OK;
}

db_err db_graph_hop_in(DB *db, db_graph_rid node, db_graph_rid **out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    int rc = dg_hop_locked(g, rid_from_u64(node), true, out, n_out);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "跳一步失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/* ---------------- schema ---------------- */

int db_graph_col_count(DB *db)
{
    Graph *g = dg_graph(db);
    return g ? graph_col_count(g) : 0;
}

db_err db_graph_schema(DB *db, db_graph_col *out, int cap, int *n_out)
{
    if (!db)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    int n = graph_col_count(g);
    if (n_out)
        *n_out = n;

    if (n == 0)
        return DB_OK;
    if (!out || cap < n) {
        dg_err(db, "输出缓冲太小");
        return DB_E_NOSPACE;
    }

    const GraphCol *gc = graph_cols(g);
    for (int i = 0; i < n; i++) {
        strncpy(out[i].name, gc[i].name, DB_GRAPH_NAME_MAX - 1);
        out[i].name[DB_GRAPH_NAME_MAX - 1] = '\0';
        switch (gc[i].type) {
        case T_INT4: out[i].type = DB_GRAPH_INT4; break;
        case T_INT8: out[i].type = DB_GRAPH_INT8; break;
        case T_BOOL: out[i].type = DB_GRAPH_BOOL; break;
        default:     out[i].type = DB_GRAPH_TEXT; break;
        }
        out[i].width = gc[i].width;
        out[i].nullable = gc[i].nullable ? 1 : 0;
    }
    return DB_OK;
}

/* ---------------- 枚举 / 统计 ---------------- */

typedef struct {
    RidVec v;
} CollectCtx;

static int dg_collect(void *ctx, Rid rid)
{
    CollectCtx *c = (CollectCtx *)ctx;
    return ridvec_push(&c->v, rid_to_u64(rid));
}

static db_err dg_list(DB *db, bool nodes, db_graph_rid **out, uint32_t *n_out)
{
    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    CollectCtx c = { { NULL, 0, 0 } };

    latch_lock();
    int rc = nodes ? graph_each_node(g, dg_collect, &c)
                   : graph_each_edge(g, dg_collect, &c);
    latch_unlock();

    if (rc != OK) {
        free(c.v.v);
        dg_err(db, "枚举失败");
        return map_rc(rc);
    }

    *out = c.v.v;
    *n_out = c.v.n;
    return DB_OK;
}

db_err db_graph_list_nodes(DB *db, db_graph_rid **out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;
    return dg_list(db, true, out, n_out);
}

db_err db_graph_list_edges(DB *db, db_graph_rid **out, uint32_t *n_out)
{
    if (!db || !out || !n_out)
        return DB_E_INVAL;
    return dg_list(db, false, out, n_out);
}

db_err db_graph_count(DB *db, uint64_t *nodes, uint64_t *edges)
{
    if (!db)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    u64 nn = graph_node_count(g);
    u64 ne = graph_edge_count(g);
    latch_unlock();

    if (nodes)
        *nodes = nn;
    if (edges)
        *edges = ne;
    return DB_OK;
}

void db_graph_free(void *p)
{
    free(p);
}

db_err db_graph_reads(DB *db, uint64_t *node_reads, uint64_t *edge_reads)
{
    if (!db)
        return DB_E_INVAL;

    Graph *g = dg_graph(db);
    if (!g) {
        dg_err(db, "图还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    u64 nn = gstore_read_count(g->nodes);
    u64 ne = gstore_read_count(g->edges);
    latch_unlock();

    if (node_reads)
        *node_reads = nn;
    if (edge_reads)
        *edge_reads = ne;
    return DB_OK;
}
