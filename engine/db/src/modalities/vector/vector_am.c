/*
 * vector_am.c —— 向量模态的 SPI 适配（v1.0 步骤 5）
 *
 * 设计说明见 modalities/vector/vector_am.h。
 *
 * 【行的字节串格式】
 *
 * AM 的行是**模态自己的格式**，不是 tuple 格式（storage_am.h 的原话：
 * "强行统一等于逼所有模态都长成关系模态"）。
 *
 *   [0] u64 row_id
 *   [8] i32 label
 *
 * 只有 12 字节 —— **向量本体不在行里**。
 * 它是外部值，句柄 = row_id，要读才去 vstore 取。
 *
 * 这是"外部值惰性"的关键：行扫描只碰标量表，
 * 向量页要等到真的 SELECT vec 才读。
 */

#include "modalities/vector/vector_am.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/vector/vector_state.h"

#include "spi/pushdown.h"
#include "storage/gstore.h"

/* 行字节数 */
#define VEC_ROW_BYTES 12u

/* 列号 */
enum {
    VEC_COL_ROW_ID = 0,
    VEC_COL_LABEL  = 1,
    VEC_COL_VEC    = 2
};

static const ColDesc ITEM_COLS[3] = {
    { "row_id", T_INT8, false },
    { "label",  T_INT4, true  },
    { "vec",    T_TEXT, true  }
};

/* ---------------- 扫描状态 ---------------- */

typedef struct {
    u64 row_id;
    i32 label;
} Hit;

typedef struct {
    VStore      *vs;
    GStoreCursor mc;      /* 标量表的顺序游标（全扫用） */
    u64          next_row;/* 全扫时下一个该给的 row_id */
    bool         full;    /* true = 全扫；false = 按命中集走 */

    Hit         *hits;    /* 命中集（下推用） */
    u32          nhit;
    u32          cur;
} VecScan;

/* ---------------- 外部值：读向量本体 ---------------- */

/*
 * 把 row_id 对应的向量读出来。
 *
 * 句柄：key = row_id，off = 0（向量是一个整体，没有"偏移"可言）。
 *
 * 第一次调用（buf = NULL, cap = 0）只报长度 —— 这是值层的约定
 * （storage/value.h 的 val_detoast 说明）。
 */
static int vec_detoast(void *ctx, u64 key, u64 off,
                       u8 *buf, u16 cap, u16 *len_out)
{
    (void)off;
    VectorAmCtx *c = (VectorAmCtx *)ctx;
    if (!c || !c->st || !c->st->vs)
        return E_CORRUPT;

    u16 vlen = (u16)((u32)c->st->dim * 4u);

    if (cap < vlen) {
        if (len_out)
            *len_out = vlen;
        return E_NOSPACE;
    }

    int rc = vstore_get(c->st->vs, key, (float *)buf);
    if (rc != OK)
        return rc;

    if (len_out)
        *len_out = vlen;
    return OK;
}

/* ---------------- schema ---------------- */

static int vec_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    (void)am;
    if (cols)
        *cols = ITEM_COLS;
    if (ncols)
        *ncols = 3;
    return OK;
}

/* ---------------- 扫描 ---------------- */

static int vec_scan_begin(StorageAM *am, AmScan **out)
{
    VectorAmCtx *c = (VectorAmCtx *)am->impl;

    VecScan *st = (VecScan *)calloc(1, sizeof(VecScan));
    if (!st)
        return E_IO;

    st->vs = c->st->vs;
    st->full = true;
    st->next_row = 0;
    gstore_cursor_init(vstore_meta(c->st->vs), &st->mc);

    *out = (AmScan *)st;
    return OK;
}

static int vec_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                         u8 *buf, u16 cap, u16 *len)
{
    VectorAmCtx *c = (VectorAmCtx *)am->impl;
    VecScan *st = (VecScan *)s;

    if (cap < VEC_ROW_BYTES)
        return E_NOSPACE;

    u64 row_id;
    i32 label;

    if (st->full) {
        /*
         * 全扫：拉标量表。
         *
         * 用**顺序游标**而不是"按 row_id 逐个 gstore_get" ——
         * 后者是随机读，每个向量一次页读。
         *
         * 顺带做列对齐的防线：第 i 条标量记录必须是第 i 个向量。
         * 不成立说明标量表和向量表错位了 —— 返回 E_CORRUPT，
         * 不要带着错位的对应关系去出结果。
         */
        u8 rec[VEC_META_REC];
        Rid mrid;
        u16 mlen = 0;
        int rc = gstore_cursor_next(vstore_meta(c->st->vs), &st->mc, &mrid, rec,
                                    sizeof(rec), &mlen);
        if (rc == E_EOF)
            return E_EOF;
        if (rc != OK)
            return rc;

        vstore_meta_decode(rec, &row_id, &label);
        if (row_id != st->next_row)
            return E_CORRUPT;
        st->next_row++;
    } else {
        if (st->cur >= st->nhit)
            return E_EOF;
        row_id = st->hits[st->cur].row_id;
        label = st->hits[st->cur].label;
        st->cur++;
    }

    put_u64(buf, row_id);
    put_u32(buf + 8, (u32)label);
    if (len)
        *len = VEC_ROW_BYTES;

    if (rid) {
        u32 ord = 0;
        u16 slot = 0;
        u16 off = 0;
        vstore_slot_of(c->st->vs, row_id, &ord, &slot, &off);
        u32 pid = vstore_page_id(c->st->vs, ord);
        *rid = rid_make(pid, slot);
    }
    return OK;
}

static int vec_scan_end(StorageAM *am, AmScan *s)
{
    (void)am;
    VecScan *st = (VecScan *)s;
    if (!st)
        return OK;

    free(st->hits);
    free(st);
    return OK;
}

/* ---------------- 按 RID 取一行 ---------------- */

static int vec_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    VectorAmCtx *c = (VectorAmCtx *)am->impl;

    if (cap < VEC_ROW_BYTES)
        return E_NOSPACE;

    /* RID → row_id：页号反查序号 + 槽号 */
    u32 ord = 0;
    int rc = vstore_ordinal_of_page(c->st->vs, rid.page, &ord);
    if (rc != OK)
        return rc;

    u16 k = vstore_k(c->st->vs);
    u64 row_id = (u64)ord * (u64)k + (u64)rid.slot;

    if (row_id >= vstore_count(c->st->vs))
        return E_NOTFOUND;

    i32 label = 0;
    rc = vstore_label(c->st->vs, row_id, &label);
    if (rc != OK)
        return rc;

    put_u64(buf, row_id);
    put_u32(buf + 8, (u32)label);
    if (len)
        *len = VEC_ROW_BYTES;
    return OK;
}

/* ---------------- 解码 ---------------- */

static int vec_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    VectorAmCtx *c = (VectorAmCtx *)am->impl;

    if (len != VEC_ROW_BYTES)
        return E_CORRUPT;

    u64 row_id = get_u64(buf);
    i32 label = (i32)get_u32(buf + 8);

    val_int(&out[VEC_COL_ROW_ID], (i64)row_id);
    out[VEC_COL_ROW_ID].type = T_INT8;

    val_int(&out[VEC_COL_LABEL], (i64)label);
    out[VEC_COL_LABEL].type = T_INT4;

    /*
     * vec 是外部值：句柄 = row_id，内容要读才取。
     *
     * 这样 `SELECT row_id, label FROM vec_items` 一个向量页都不读。
     */
    val_external_text(&out[VEC_COL_VEC], row_id, 0, vec_detoast, c);

    return OK;
}

/* ---------------- 可见性 ---------------- */

/*
 * 向量没有版本（PLAN §1.4：锁 + WAL，不做 MVCC）→ 恒可见。
 */
static bool vec_am_visible(StorageAM *am, const u8 *buf, u16 len,
                           const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写路径（本版只读） ---------------- */

static int vec_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)am; (void)xid; (void)vals; (void)out;
    return E_NOTSUP;
}

static int vec_am_update(StorageAM *am, u32 xid, Rid rid,
                          const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int vec_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

static int vec_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                         AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;   /* 没有版本 → 没有可回收的东西 */
}

/* ---------------- 重建索引 ---------------- */

/*
 * reindex：把已有向量灌进 HNSW。
 *
 * 什么时候需要：
 *   - 崩溃恢复之后（索引不写日志）
 *   - 索引与数据不一致时（load 返回 E_NOTFOUND）
 *
 * 本版没有 SQL 的 CREATE INDEX 入口，所以这个回调是**给公开 API 用的**：
 * db_vec_reindex 直接走它。
 */
static int vec_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)index_id;

    VectorAmCtx *c = (VectorAmCtx *)am->impl;

    /*
     * 真正的重建：读全部向量 → 新建 HNSW → 逐个插入 → 落盘。
     *
     * 这一步要动实例里的 Hnsw 对象，所以必须能拿到完整的模态状态 ——
     * 这就是 VectorState 存在的理由（AM 只看得到 VStore 不够）。
     */
    return vec_state_reindex(c->st, rows_out);
}

/* ---------------- 谓词下推 ---------------- */

/*
 * 只支持 label 的等值比较。
 *
 * 【为什么只有等值】
 *
 * label 是 INT4。范围比较（label > 3）当然能做，但那种谓词
 * 在"命中集"模型里没有优势 —— 命中集要么是一批行，要么是全部行。
 * 等值是语义上最自然的一种（"这一类向量"）。
 *
 * 【下推省的是什么，不是 IO】
 *
 * 要诚实地说明：命中集的**构造**必须扫一遍标量表
 *（没有标量索引的话，找出 label = X 的行就是 O(N)），
 * 所以**IO 和全扫一样**。
 *
 * 下推省的是**交给执行器的行数**：全扫给 N 行、执行器再过滤；
 * 下推只给命中行数。执行器拿到行**仍然要重算一遍 WHERE**，
 * 所以结果一定相同。
 *
 * 真正的 IO 优势在** ANN 路径**上（索引 vs 暴力扫）——
 * 那是 A4 量的，不是这里量的。别把两件事混在一起说。
 */
static int vec_am_can_push(StorageAM *am, const Predicate *p)
{
    (void)am;

    if (!p)
        return E_NOTSUP;
    if (p->col_idx != VEC_COL_LABEL)
        return E_NOTSUP;
    if (p->op != PD_EQ)
        return E_NOTSUP;
    if (p->val.is_null)
        return E_NOTSUP;
    if (p->val.type != T_INT4 && p->val.type != T_INT8)
        return E_NOTSUP;

    return OK;
}

static int vec_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    VectorAmCtx *c = (VectorAmCtx *)am->impl;

    int rc = vec_am_can_push(am, p);
    if (rc != OK)
        return rc;

    i32 want = (i32)p->val.i;

    /*
     * 第一遍：数出命中行数。
     *
     * 为什么要两遍：命中集是变长的数组，而 AM 的 push 只能失败一次。
     * 估一个上限（全部行数）再按实际收缩，多一次分配但不用反复 realloc。
     */
    u64 total = vstore_count(c->st->vs);

    Hit *hits = NULL;
    u32 nhit = 0;
    if (total > 0) {
        hits = (Hit *)calloc((size_t)total, sizeof(Hit));
        if (!hits)
            return E_IO;
    }

    GStoreCursor mc;
    gstore_cursor_init(vstore_meta(c->st->vs), &mc);

    u8 rec[VEC_META_REC];
    Rid mrid;
    u16 mlen = 0;
    u64 expect = 0;

    while ((rc = gstore_cursor_next(vstore_meta(c->st->vs), &mc, &mrid, rec,
                                    sizeof(rec), &mlen)) == OK) {
        u64 row_id = 0;
        i32 label = 0;
        vstore_meta_decode(rec, &row_id, &label);

        if (row_id != expect) {
            free(hits);
            return E_CORRUPT;
        }
        expect++;

        if (label == want && nhit < total) {
            hits[nhit].row_id = row_id;
            hits[nhit].label = label;
            nhit++;
        }
    }
    if (rc != E_EOF) {
        free(hits);
        return rc;
    }

    VecScan *st = (VecScan *)calloc(1, sizeof(VecScan));
    if (!st) {
        free(hits);
        return E_IO;
    }

    st->vs = c->st->vs;
    st->full = false;
    st->hits = hits;
    st->nhit = nhit;
    st->cur = 0;

    *out = (AmScan *)st;
    return OK;
}

/* ---------------- 构造 ---------------- */

int vector_am_build_items(VectorState *st, StorageAM *out, VectorAmCtx *ctx)
{
    if (!st || !st->vs || !out || !ctx || st->dim == 0)
        return E_INVAL;

    ctx->st = st;

    out->name = "vector";
    out->caps = AM_CAP_SCAN | AM_CAP_EXTERNAL | AM_CAP_PUSHDOWN;

    out->columns = vec_am_columns;
    out->scan_begin = vec_scan_begin;
    out->scan_next = vec_scan_next;
    out->scan_end = vec_scan_end;
    out->get = vec_am_get;
    out->decode = vec_am_decode;
    /* 批能力：向量模态不支持（PLAN-v1.2 步骤 0：三个模态一行都不用改） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = vec_am_visible;
    out->insert = vec_am_insert;
    out->update = vec_am_update;
    out->remove = vec_am_remove;
    out->vacuum = vec_am_vacuum;
    out->reindex = vec_am_reindex;
    out->impl = ctx;
    out->can_push = vec_am_can_push;
    out->push = vec_am_push;

    return OK;
}
