/*
 * geo_am.c —— 地理空间模态的 SPI 适配：geo_points 虚拟表（v1.4 步骤 5）
 *
 * 契约见 geo_am.h（列定义、列号、构造函数的签名都在那里）。
 *
 * ==================== 这一层在干什么 ====================
 *
 * geo_points 是一张**只读的元数据视图**：把 R-tree 里的每个点
 * 摊平成一行。
 *
 *   row_id   INT8   点的标识（插入顺序分配，单调递增）
 *   x        REAL   横坐标
 *   y        REAL   纵坐标
 *   deleted  BOOL   是否已被标记删除
 *
 * ==================== 行是怎么来的 ====================
 *
 * R-tree 里没有"行"：叶子条目是 (row_id, 退化矩形)。
 * 所以行是**现算**出来的：扫一遍树，把每个叶子条目摊成一行。
 *
 * 与 ft_docs 同款取舍：这是**元数据路径**，不是热路径，
 * 物化一次换掉"每行都去翻一遍树"是对的。
 *
 * ==================== 行的字节格式 ====================
 *
 * 固定 16 字节（小端）：
 *
 *   偏移 0   u32   row_id
 *   偏移 4   u32   x 的**位模式**（float）
 *   偏移 8   u32   y 的位模式（float）
 *   偏移 12  u32   dead（0 / 1）
 *
 * 【为什么浮点存位模式】
 *
 * 磁盘格式只允许 u8/u16/u32/u64（common/types.h 铁律 1），
 * 而坐标是 float —— 所以存的是 float 的**位模式**。
 * 位模式的互转走 rt_f2u / rt_u2f（rtree.h 里的 memcpy 版本），
 * 不用 union、**不用指针双关**（双关只在 -fno-strict-aliasing 下安全）。
 *
 * ==================== 为什么 deleted 是一列 ====================
 *
 * 与 ft_docs 的 sealed 列同一个理由：删除语义是"打标记"，
 * 状态由**行内容**表达，不由可见性表达。
 * 再用可见性藏一次，就是同一件事说两遍。
 *
 * ==================== 写入不走这条路 ====================
 *
 * geo_points 只读：insert / update / remove / vacuum / reindex
 * 一律返回 E_NOTSUP。写入走专用 API（步骤 6 的 db_geo_insert /
 * db_geo_delete）—— 写路径不只是"加一行"，它还要**更新索引**。
 *
 * ==================== 为什么没有窗口/KNN 的下推 ====================
 *
 * 窗口查询要的是一个**空间谓词**，而 SQL 谓词系统里没有这个算子
 * （计划 §1.1 明确把空间谓词划在范围外）。
 * 窗口/KNN 走专用 API。
 */

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_am.h"
#include "modalities/geo/geo_state.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_scan.h"
#include "spi/storage_am.h"
#include "storage/record.h"
#include "storage/value.h"

/* 一行的字节数 */
#define GEO_ROW_BYTES 16u

/* 行内各字段的偏移 */
#define GEO_ROW_ID_OFF   0u
#define GEO_ROW_X_OFF    4u
#define GEO_ROW_Y_OFF    8u
#define GEO_ROW_DEAD_OFF 12u

/*
 * 列定义。
 *
 * 名字与 geo_am.h 的说明一一对应：
 *   row_id / x / y / deleted
 */
static const ColDesc GEO_COLS[GEO_POINTS_NCOLS] = {
    { "row_id",  T_INT8, false },
    { "x",       T_REAL, false },
    { "y",       T_REAL, false },
    { "deleted", T_BOOL, false }
};

/* ---------------- 物化 ---------------- */

/* 一行（解码后的形式） */
typedef struct {
    u32   row_id;
    float x;
    float y;
    u32   dead;
} GeoRow;

/* 收集用的上下文（回调里只往里塞） */
typedef struct {
    GeoRow *rows;
    u32     n;
    u32     cap;
} GeoCollect;

/* 枚举回调：把一个叶子条目收成一行 */
static int geo_am_collect_cb(const RTreePoint *p, void *vctx)
{
    GeoCollect *c = (GeoCollect *)vctx;

    if (c->n == c->cap) {
        u32 ncap = (c->cap == 0u) ? 64u : c->cap * 2u;
        GeoRow *nr = (GeoRow *)realloc(c->rows, sizeof(GeoRow) * (size_t)ncap);

        if (nr == NULL) {
            return E_NOSPACE;
        }
        c->rows = nr;
        c->cap = ncap;
    }
    c->rows[c->n].row_id = p->row_id;
    c->rows[c->n].x = p->x;
    c->rows[c->n].y = p->y;
    c->rows[c->n].dead = p->dead ? 1u : 0u;
    c->n++;
    return OK;
}

/*
 * 把整棵树摊成行。
 *
 * 【为什么每次扫描都重新物化】
 *
 * 这与 ft_docs 不同：ft 有一层带签名校验的物化缓存，
 * 这里是**元数据路径**，而且树在扫描期间也可能在变
 * （另一个连接在插入）——缓存反而会给出"新旧混在一起"的行。
 * 宁可每次重扫，也不给混着两个时刻的结果。
 *
 * 顺序是"树序"（深度优先），**不是 row_id 序**。
 * 需要 row_id 序的调用方自己排。
 */
static int geo_am_materialize(GeoState *st, GeoRow **out, u32 *n_out)
{
    GeoCollect c;
    u64 reads = 0;
    int rc;

    memset(&c, 0, sizeof(c));
    rc = rtree_scan_all(st->pg, rtree_root_page(st->tree),
                         geo_am_collect_cb, &c, &reads);
    /* 页读计入模态的累计计数（诊断用） */
    st->page_reads += reads;
    if (rc != OK) {
        free(c.rows);
        return rc;
    }
    *out = c.rows;
    *n_out = c.n;
    return OK;
}

/* ---------------- 扫描状态 ---------------- */

typedef struct {
    GeoRow *rows;
    u32     nrows;
    u32     cur;
} GeoScan;

/* ---------------- schema ---------------- */

static int geo_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    (void)am;
    if (!cols || !ncols) {
        return E_INVAL;
    }
    *cols = GEO_COLS;
    *ncols = GEO_POINTS_NCOLS;
    return OK;
}

/* ---------------- 扫描 ---------------- */

static int geo_am_scan_begin(StorageAM *am, AmScan **out)
{
    GeoAmCtx *c = (GeoAmCtx *)am->impl;
    GeoScan *s;
    int rc;

    if (!c || !c->st || !out) {
        return E_INVAL;
    }
    s = (GeoScan *)calloc(1, sizeof(GeoScan));
    if (s == NULL) {
        return E_IO;
    }
    rc = geo_am_materialize(c->st, &s->rows, &s->nrows);
    if (rc != OK) {
        free(s);
        return rc;
    }
    *out = (AmScan *)s;
    return OK;
}

static int geo_am_scan_next(StorageAM *am, AmScan *sc, Rid *rid,
                            u8 *buf, u16 cap, u16 *len)
{
    GeoScan *s = (GeoScan *)sc;
    const GeoRow *r;

    (void)am;
    if (!s) {
        return E_INVAL;
    }
    if (cap < GEO_ROW_BYTES) {
        return E_NOSPACE;
    }
    if (s->cur >= s->nrows) {
        return E_EOF;
    }
    r = &s->rows[s->cur];

    put_u32(buf + GEO_ROW_ID_OFF, r->row_id);
    put_u32(buf + GEO_ROW_X_OFF, rt_f2u(r->x));
    put_u32(buf + GEO_ROW_Y_OFF, rt_f2u(r->y));
    put_u32(buf + GEO_ROW_DEAD_OFF, r->dead);
    if (len) {
        *len = GEO_ROW_BYTES;
    }
    if (rid) {
        /*
         * RID = (行序号, 0)。
         *
         * geo_points 没有物理页 —— 行是"现算"出来的，
         * 所以 RID 用**输出序号**定位（与 ft_docs 同一口径：
         * 同一批物化结果内稳定）。
         */
        *rid = rid_make(s->cur, 0);
    }
    s->cur++;
    return OK;
}

static int geo_am_scan_end(StorageAM *am, AmScan *sc)
{
    GeoScan *s = (GeoScan *)sc;

    (void)am;
    if (!s) {
        return OK;
    }
    free(s->rows);
    free(s);
    return OK;
}

/* ---------------- 按 RID 取一行 ---------------- */

static int geo_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    GeoAmCtx *c = (GeoAmCtx *)am->impl;
    GeoRow *rows = NULL;
    u32 n = 0;
    int rc;

    if (!c || !c->st) {
        return E_INVAL;
    }
    if (cap < GEO_ROW_BYTES) {
        return E_NOSPACE;
    }
    /*
     * get 与 scan 走的是**同一份物化**（同样的树序），
     * 所以 scan 时记下的 RID 能原样取回来。
     */
    rc = geo_am_materialize(c->st, &rows, &n);
    if (rc != OK) {
        return rc;
    }
    if (rid.page >= n) {
        free(rows);
        return E_NOTFOUND;
    }
    put_u32(buf + GEO_ROW_ID_OFF, rows[rid.page].row_id);
    put_u32(buf + GEO_ROW_X_OFF, rt_f2u(rows[rid.page].x));
    put_u32(buf + GEO_ROW_Y_OFF, rt_f2u(rows[rid.page].y));
    put_u32(buf + GEO_ROW_DEAD_OFF, rows[rid.page].dead);
    free(rows);
    if (len) {
        *len = GEO_ROW_BYTES;
    }
    return OK;
}

/* ---------------- 解码 ---------------- */

static int geo_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    (void)am;
    if (!buf || !out) {
        return E_INVAL;
    }
    if (len < GEO_ROW_BYTES) {
        return E_CORRUPT;
    }
    val_int(&out[GEO_COL_ID], (i64)get_u32(buf + GEO_ROW_ID_OFF));
    out[GEO_COL_ID].type = T_INT8;

    val_real(&out[GEO_COL_X], (double)rt_u2f(get_u32(buf + GEO_ROW_X_OFF)));
    out[GEO_COL_X].type = T_REAL;

    val_real(&out[GEO_COL_Y], (double)rt_u2f(get_u32(buf + GEO_ROW_Y_OFF)));
    out[GEO_COL_Y].type = T_REAL;

    val_bool(&out[GEO_COL_DEAD], get_u32(buf + GEO_ROW_DEAD_OFF) != 0);
    return OK;
}

/* ---------------- 可见性 ---------------- */

/*
 * geo_points 的行**恒可见**。
 *
 * 理由与 blob / graph / vector / ft_docs 一致：
 * 删除语义是"打删除标记"，状态由**行内容**（deleted 列）表达，
 * 不由可见性表达。
 */
static bool geo_am_visible(StorageAM *am, const u8 *buf, u16 len,
                           const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写路径：只读 ---------------- */

/*
 * geo_points 是只读元数据视图：写入走专用 API。
 *
 * 返回 E_NOTSUP —— 执行器据此报"这个表不支持写"。
 * 能力位里本来也没有 INSERT / UPDATE / REMOVE / VACUUM / INDEX。
 */
static int geo_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)am; (void)xid; (void)vals; (void)out;
    return E_NOTSUP;
}

static int geo_am_update(StorageAM *am, u32 xid, Rid rid,
                         const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int geo_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

static int geo_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                         AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;   /* 没有版本 → 没有可回收的东西 */
}

/* 建索引 / 重建索引：geo_points 是 R-tree 的投影，不是它的索引 */
static int geo_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)am; (void)index_id; (void)rows_out;
    return E_NOTSUP;
}

/* ---------------- 构造 ---------------- */

int geo_am_build_points(GeoState *st, StorageAM *out, GeoAmCtx *ctx)
{
    if (!st || !out || !ctx) {
        return E_INVAL;
    }

    ctx->st = st;

    out->name = "geo_points";
    /*
     * 能力位：**只读**。
     *
     * 没有 INSERT / UPDATE / REMOVE / VACUUM / INDEX ——
     * geo_points 是元数据视图，写入走专用 API。
     * 也没有 PUSHDOWN（窗口/KNN 要的是空间谓词，不在 SQL 谓词范围内），
     * 所以 can_push / push 都留 NULL。
     */
    out->caps = AM_CAP_SCAN;

    out->columns = geo_am_columns;
    out->scan_begin = geo_am_scan_begin;
    out->scan_next = geo_am_scan_next;
    out->scan_end = geo_am_scan_end;
    out->get = geo_am_get;
    out->decode = geo_am_decode;
    /* 批能力：geo 模态不支持（与 blob / vector / ft 同款） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = geo_am_visible;
    out->insert = geo_am_insert;
    out->update = geo_am_update;
    out->remove = geo_am_remove;
    out->vacuum = geo_am_vacuum;
    out->reindex = geo_am_reindex;
    out->impl = ctx;
    out->can_push = NULL;
    out->push = NULL;

    return OK;
}
