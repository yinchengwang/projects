/*
 * ft_am.c —— 全文检索模态的 SPI 适配：ft_docs 虚拟表（v1.3 步骤 4）
 *
 * 设计说明见 modalities/ft/ft_am.h。
 *
 * 【这一层在干什么】
 *
 * 段的物理格式里**没有文档表**：文档长度（dl）跟着 posting 走。
 * 所以 ft_docs 的行不是"读一张表"，而是**从倒排链里现算**：
 *
 *   逐段 → 逐词 → 逐 posting → 按 doc_id 聚合
 *
 * 聚合出三件事：
 *   doc_len  = posting 里的 dl（该文档的词数）
 *   nterms   = 该 doc_id 命中的 posting 条数（= 命中的不同词数）
 *   sealed   = 该 doc_id 是否已被 tombstone
 *
 * 【为什么 doc_len 取 posting 的 dl，而不是"整篇文档的词数"再存一份】
 *
 * 因为 dl 本来就跟着 posting 走（ft_internal.h 的设计）：
 * 每个词的 posting 都带一遍该文档的 dl。直接用它，
 * 就**不用**为 ft_docs 引入第四种页类型（文档表）。
 *
 * 【坑 M4-18：同一 doc_id 的 dl 不一致 = 数据坏了】
 *
 * 同一篇文档的所有 posting 里的 dl 必须相等（dl 是文档级属性）。
 * 不相等说明段被写坏了 —— 这时候**不能**"取一个凑合"，
 * 必须报 E_CORRUPT：带着错数据出结果，比报错坏得多。
 *
 * 【坑 M4-19：同一个 doc_id 出现在多个段里】
 *
 * 正常情况下一个 doc_id 只在一个段里（写入即建新段，索引内 doc_id 唯一）。
 * 但"删了又用同一个 doc_id 重新加"会让它出现在两个段里 ——
 * 这**不是**数据损坏，所以不报 E_CORRUPT。
 * 口径：**段号大者胜**（后加入的段是更新的那份），
 * 旧段里同一个 doc_id 的 posting 直接忽略。
 */

#include "modalities/ft/ft_am.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "spi/pushdown.h"
#include "storage/value.h"

/* 一行的字节数：doc_id / doc_len / nterms / sealed，各 u32 */
#define FT_DOC_ROW_BYTES 16u

/* 列号 */
enum {
    FT_DOC_COL_ID     = 0,
    FT_DOC_COL_LEN    = 1,
    FT_DOC_COL_NTERMS = 2,
    FT_DOC_COL_SEALED = 3
};

static const ColDesc DOC_COLS[4] = {
    { "doc_id",  T_INT8, false },
    { "doc_len", T_INT4, false },
    { "nterms",  T_INT4, false },
    { "sealed",  T_BOOL, false }
};

/* ---------------- 一行 ---------------- */

typedef struct {
    u32 doc_id;
    u32 doc_len;   /* 词数 */
    u32 nterms;    /* 命中的不同词数 */
    u32 sealed;    /* 1 = 已被 tombstone */
} FtDocRow;

/* ---------------- 聚合用的哈希表 ---------------- */

/*
 * doc_id → (doc_len, nterms)。
 *
 * doc_id 由调用方给、**不连续**，数量事先未知：
 * 用数组浪费，用线性查找就是 O(n²)。所以用开放寻址哈希
 * （装填因子 <= 0.5，Knuth 乘法散列 —— 与 ft_index.c 的做法一致）。
 */
typedef struct {
    u32 doc_id;
    u32 doc_len;
    u32 nterms;
    u32 seg_id;    /* 权威来源段（段号大者胜，坑 M4-19） */
    u8  used;
} FtDocSlot;

typedef struct {
    FtDocSlot *slots;
    u32        cap;      /* 槽数（2 的幂） */
    u32        count;    /* 已占槽数 */
} FtDocMap;

static u32 ft_docmap_hash(u32 doc_id)
{
    return doc_id * 2654435761u;
}

static void ft_docmap_init(FtDocMap *m)
{
    memset(m, 0, sizeof(*m));
}

static void ft_docmap_free(FtDocMap *m)
{
    free(m->slots);
    memset(m, 0, sizeof(*m));
}

/* 容量翻倍并把老条目重插进去 */
static int ft_docmap_grow(FtDocMap *m)
{
    u32 ncap = m->cap * 2;
    FtDocSlot *ns = (FtDocSlot *)calloc(ncap, sizeof(FtDocSlot));
    u32 i;

    if (ns == NULL) {
        return E_NOSPACE;
    }
    for (i = 0; i < m->cap; i++) {
        u32 j;

        if (!m->slots[i].used) {
            continue;
        }
        j = ft_docmap_hash(m->slots[i].doc_id) & (ncap - 1);
        while (ns[j].used) {
            j = (j + 1) & (ncap - 1);
        }
        ns[j] = m->slots[i];
    }
    free(m->slots);
    m->slots = ns;
    m->cap = ncap;
    return OK;
}

/*
 * 记一次 (doc_id, dl) 命中。
 *
 * dl —— 该文档的词数（来自 posting）
 * seg_id —— 这次命中来自哪个段
 */
static int ft_docmap_add(FtDocMap *m, u32 doc_id, u32 dl, u32 seg_id)
{
    u32 i;

    if (m->cap == 0) {
        m->slots = (FtDocSlot *)calloc(64, sizeof(FtDocSlot));
        if (m->slots == NULL) {
            return E_NOSPACE;
        }
        m->cap = 64;
    }
    /* 装填因子 <= 0.5：满了先翻倍 */
    if ((u64)m->count * 2 >= (u64)m->cap) {
        if (ft_docmap_grow(m) != OK) {
            return E_NOSPACE;
        }
    }

    i = ft_docmap_hash(doc_id) & (m->cap - 1);
    for (;;) {
        FtDocSlot *s = &m->slots[i];

        if (!s->used) {
            s->used = 1;
            s->doc_id = doc_id;
            s->doc_len = dl;
            s->nterms = 1;
            s->seg_id = seg_id;
            m->count++;
            return OK;
        }
        if (s->doc_id == doc_id) {
            if (seg_id < s->seg_id) {
                /* 旧段的命中：更新的段已经赢了，忽略（坑 M4-19） */
                return OK;
            }
            if (seg_id > s->seg_id) {
                /* 更新的段：这份文档以它为准，旧的统计作废 */
                s->seg_id = seg_id;
                s->doc_len = dl;
                s->nterms = 0;
            }
            /* 同一个 doc_id 的 dl 必须一致（坑 M4-18） */
            if (s->doc_len != dl) {
                return E_CORRUPT;
            }
            s->nterms++;
            return OK;
        }
        i = (i + 1) & (m->cap - 1);
    }
}

/* ---------------- 物化 ---------------- */

/*
 * 逐词回调：把这个词的 posting 全部读出来，聚合进表。
 */
typedef struct {
    const FtSegment *seg;
    FtDocMap       *map;
    u32             seg_id;
    FtPosting      *buf;
    u32             cap;
} FtTermCtx;

static int ft_am_term_cb(void *vctx, const char *term, u32 term_len,
                         u32 df, u64 post_ptr)
{
    FtTermCtx *c = (FtTermCtx *)vctx;
    u32 np = 0;
    u32 i;
    int rc;

    (void)term;
    (void)term_len;

    if (df == 0) {
        return OK;
    }
    /* posting 缓冲：装下这个词在这个段里的全部 posting */
    if (df > c->cap) {
        FtPosting *nb = (FtPosting *)realloc(c->buf, sizeof(FtPosting) * (size_t)df);

        if (nb == NULL) {
            return E_NOSPACE;
        }
        c->buf = nb;
        c->cap = df;
    }
    rc = ft_segment_postings(c->seg, post_ptr, df, c->buf, c->cap, &np);
    if (rc != OK) {
        return rc;
    }
    for (i = 0; i < np; i++) {
        rc = ft_docmap_add(c->map, c->buf[i].doc_id, c->buf[i].dl, c->seg_id);
        if (rc != OK) {
            return rc;
        }
    }
    return OK;
}

/* 行按 doc_id 升序（输出顺序稳定，方便人看和测试断言） */
static int ft_doc_row_cmp(const void *a, const void *b)
{
    const FtDocRow *x = (const FtDocRow *)a;
    const FtDocRow *y = (const FtDocRow *)b;

    if (x->doc_id != y->doc_id) {
        return (x->doc_id < y->doc_id) ? -1 : 1;
    }
    return 0;
}

/*
 * 把整个索引物化成 ft_docs 的行。
 *
 * 内存开销 O(文档数) —— 这是**元数据路径**，不是热路径，
 * 物化一次换掉"每行都去翻倒排链"是对的取舍。
 */
static int ft_am_materialize(FtIndex *ix, FtDocRow **rows_out, u32 *n_out)
{
    FtDocMap map;
    FtTermCtx ctx;
    u32 nsegs;
    u32 s;
    int rc = OK;

    *rows_out = NULL;
    *n_out = 0;

    ft_docmap_init(&map);
    memset(&ctx, 0, sizeof(ctx));

    nsegs = ft_index_nsegs(ix);
    for (s = 0; s < nsegs; s++) {
        const FtSegment *seg = NULL;

        rc = ft_index_seg_at(ix, s, &seg);
        if (rc != OK) {
            goto done;
        }
        ctx.seg = seg;
        ctx.map = &map;
        ctx.seg_id = seg->seg_id;
        ctx.buf = NULL;
        ctx.cap = 0;

        rc = ft_segment_scan_terms(seg, ft_am_term_cb, &ctx);

        free(ctx.buf);
        ctx.buf = NULL;
        ctx.cap = 0;
        if (rc != OK) {
            goto done;
        }
    }

    if (map.count > 0) {
        FtDocRow *rows = (FtDocRow *)malloc(sizeof(FtDocRow) * (size_t)map.count);
        u32 n = 0;

        if (rows == NULL) {
            rc = E_NOSPACE;
            goto done;
        }
        for (s = 0; s < map.cap; s++) {
            if (!map.slots[s].used) {
                continue;
            }
            rows[n].doc_id = map.slots[s].doc_id;
            rows[n].doc_len = map.slots[s].doc_len;
            rows[n].nterms = map.slots[s].nterms;
            rows[n].sealed = (u32)(ft_index_is_tomb(ix, map.slots[s].doc_id) ? 1 : 0);
            n++;
        }
        qsort(rows, n, sizeof(FtDocRow), ft_doc_row_cmp);
        *rows_out = rows;
        *n_out = n;
    }

done:
    ft_docmap_free(&map);
    return rc;
}

/* ---------------- 扫描状态 ---------------- */

typedef struct {
    FtDocRow *rows;
    u32       nrows;
    u32       cur;

    /* 下推过滤：只枚举符合条件的文档 */
    bool  none;       /* 谓词不可能命中任何行 */
    bool  has_eq;
    u32   eq_id;
    bool  has_range;
    u32   lo_id;
    u32   hi_id;
} FtDocScan;

static bool ft_doc_match(const FtDocScan *st, const FtDocRow *r)
{
    if (st->none) {
        return false;
    }
    if (st->has_eq) {
        return r->doc_id == st->eq_id;
    }
    if (st->has_range) {
        return r->doc_id >= st->lo_id && r->doc_id <= st->hi_id;
    }
    return true;
}

/* ---------------- schema ---------------- */

static int ft_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    (void)am;
    if (!cols || !ncols) {
        return E_INVAL;
    }
    *cols = DOC_COLS;
    *ncols = 4;
    return OK;
}

/* ---------------- 扫描 ---------------- */

static int ft_am_scan_begin(StorageAM *am, AmScan **out)
{
    FtAmCtx *c = (FtAmCtx *)am->impl;
    FtDocScan *st;
    int rc;

    if (!c || !c->ix || !out) {
        return E_INVAL;
    }
    st = (FtDocScan *)calloc(1, sizeof(FtDocScan));
    if (!st) {
        return E_IO;
    }
    rc = ft_am_materialize(c->ix, &st->rows, &st->nrows);
    if (rc != OK) {
        free(st);
        return rc;
    }
    *out = (AmScan *)st;
    return OK;
}

static int ft_am_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                           u8 *buf, u16 cap, u16 *len)
{
    FtDocScan *st = (FtDocScan *)s;

    (void)am;
    if (!st) {
        return E_INVAL;
    }
    if (cap < FT_DOC_ROW_BYTES) {
        return E_NOSPACE;
    }
    while (st->cur < st->nrows) {
        const FtDocRow *r = &st->rows[st->cur];
        u32 ord = st->cur;

        st->cur++;
        if (!ft_doc_match(st, r)) {
            continue;
        }
        put_u32(buf, r->doc_id);
        put_u32(buf + 4, r->doc_len);
        put_u32(buf + 8, r->nterms);
        put_u32(buf + 12, r->sealed);
        if (len) {
            *len = FT_DOC_ROW_BYTES;
        }
        if (rid) {
            /*
             * RID = (行序号, 0)。
             *
             * ft_docs 没有物理页 —— 行是"现算"出来的，
             * 所以 RID 用**输出序号**定位（同一批物化结果内稳定）。
             */
            *rid = rid_make(ord, 0);
        }
        return OK;
    }
    return E_EOF;
}

static int ft_am_scan_end(StorageAM *am, AmScan *s)
{
    FtDocScan *st = (FtDocScan *)s;

    (void)am;
    if (!st) {
        return OK;
    }
    free(st->rows);
    free(st);
    return OK;
}

/* ---------------- 按 RID 取一行 ---------------- */

static int ft_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    FtAmCtx *c = (FtAmCtx *)am->impl;
    FtDocRow *rows = NULL;
    u32 n = 0;
    int rc;

    if (!c || !c->ix) {
        return E_INVAL;
    }
    if (cap < FT_DOC_ROW_BYTES) {
        return E_NOSPACE;
    }
    rc = ft_am_materialize(c->ix, &rows, &n);
    if (rc != OK) {
        return rc;
    }
    if (rid.page >= n) {
        free(rows);
        return E_NOTFOUND;
    }
    put_u32(buf, rows[rid.page].doc_id);
    put_u32(buf + 4, rows[rid.page].doc_len);
    put_u32(buf + 8, rows[rid.page].nterms);
    put_u32(buf + 12, rows[rid.page].sealed);
    if (len) {
        *len = FT_DOC_ROW_BYTES;
    }
    free(rows);
    return OK;
}

/* ---------------- 解码 ---------------- */

static int ft_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    (void)am;
    if (len < FT_DOC_ROW_BYTES) {
        return E_CORRUPT;
    }
    val_int(&out[FT_DOC_COL_ID], (i64)get_u32(buf));
    out[FT_DOC_COL_ID].type = T_INT8;

    val_int(&out[FT_DOC_COL_LEN], (i64)get_u32(buf + 4));
    out[FT_DOC_COL_LEN].type = T_INT4;

    val_int(&out[FT_DOC_COL_NTERMS], (i64)get_u32(buf + 8));
    out[FT_DOC_COL_NTERMS].type = T_INT4;

    val_bool(&out[FT_DOC_COL_SEALED], get_u32(buf + 12) != 0);
    return OK;
}

/* ---------------- 可见性 ---------------- */

/*
 * ft_docs 的行**恒可见**。
 *
 * 理由与 blob / graph / vector 一致：ft 的删除语义是"打删除标记"，
 * 不是"版本可见性" —— 被删的文档仍然在段里（行还在，sealed = 1），
 * 由**行内容**表达状态，不由可见性表达。
 *
 * 换句话说：sealed 列已经回答了"这篇文档删了没"，
 * 再用可见性藏一次就是同一件事说两遍。
 */
static bool ft_am_visible(StorageAM *am, const u8 *buf, u16 len,
                          const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写路径：只读 ---------------- */

/*
 * ft_docs 是只读元数据视图：写入走专用 API（db_ft_add / db_ft_delete）。
 *
 * 返回 E_NOTSUP —— 执行器据此报"这个表不支持写"。
 * 能力位里本来也没有 INSERT / UPDATE / REMOVE / VACUUM。
 */
static int ft_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)am; (void)xid; (void)vals; (void)out;
    return E_NOTSUP;
}

static int ft_am_update(StorageAM *am, u32 xid, Rid rid,
                         const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int ft_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

static int ft_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                         AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;   /* 没有版本 → 没有可回收的东西 */
}

/* 建索引 / 重建索引：ft_docs 是倒排索引的**投影**，不是它的索引 */
static int ft_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)am; (void)index_id; (void)rows_out;
    return E_NOTSUP;
}

/* ---------------- 谓词下推 ---------------- */

/*
 * 只能下推 **doc_id 的等值 / 范围**。
 *
 * 【为什么只推 doc_id】
 *
 *   - doc_id 是这张表的**主键**：等值 = 定位一篇文档，范围 = 一段文档。
 *   - doc_len / nterms / sealed 是**派生属性**：推了也没法少读 posting
 *     （行本身就是从 posting 聚合出来的），推了只是白过滤一遍。
 *
 * 【下推省的是什么】
 *
 * 省的是"交给执行器的行数"，不是 IO —— 物化已经把整个索引读了一遍。
 * 这与向量模态的诚实说明是同一个口径（见 vector_am.c）。
 *
 * 【宁可不推，不可推错】
 *
 * 下推错了会静默少数据。所以判据从紧：只推 doc_id 的五种比较，
 * 其余一律 E_NOTSUP。
 */
static int ft_am_can_push(StorageAM *am, const Predicate *p)
{
    (void)am;

    if (!p) {
        return E_NOTSUP;
    }
    if (p->col_idx != FT_DOC_COL_ID) {
        return E_NOTSUP;
    }
    if (p->op != PD_EQ && p->op != PD_LT && p->op != PD_GT &&
        p->op != PD_LE && p->op != PD_GE) {
        return E_NOTSUP;
    }
    if (p->val.is_null) {
        return E_NOTSUP;
    }
    if (p->val.type != T_INT4 && p->val.type != T_INT8) {
        return E_NOTSUP;
    }
    return OK;
}

static int ft_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    FtAmCtx *c = (FtAmCtx *)am->impl;
    FtDocScan *st;
    i64 v;
    u32 uv;
    int rc;

    rc = ft_am_can_push(am, p);
    if (rc != OK) {
        return rc;
    }
    if (!c || !c->ix || !out) {
        return E_INVAL;
    }
    st = (FtDocScan *)calloc(1, sizeof(FtDocScan));
    if (!st) {
        return E_IO;
    }
    rc = ft_am_materialize(c->ix, &st->rows, &st->nrows);
    if (rc != OK) {
        free(st);
        return rc;
    }

    /*
     * doc_id 是 u32，但列类型是 INT8（有符号）——
     * 所以常量要**夹到** [0, 2^32-1]，负数一律夹成 0。
     * 不夹的话，负常量会变成巨大的无符号数，"doc_id < -1" 反而匹配一堆行。
     */
    v = p->val.i;
    if (v < 0) {
        uv = 0;
    } else if ((u64)v > 0xFFFFFFFFull) {
        uv = 0xFFFFFFFFu;
    } else {
        uv = (u32)v;
    }

    switch (p->op) {
    case PD_EQ:
        st->has_eq = true;
        st->eq_id = uv;
        break;
    case PD_GE:
        st->has_range = true;
        st->lo_id = uv;
        st->hi_id = 0xFFFFFFFFu;
        break;
    case PD_LE:
        st->has_range = true;
        st->lo_id = 0;
        st->hi_id = uv;
        break;
    case PD_GT:
        if (uv == 0xFFFFFFFFu) {
            st->none = true;      /* 没有比 2^32-1 更大的 doc_id */
        } else {
            st->has_range = true;
            st->lo_id = uv + 1;
            st->hi_id = 0xFFFFFFFFu;
        }
        break;
    case PD_LT:
        if (uv == 0) {
            st->none = true;      /* doc_id 是无符号的，没有比 0 更小的 */
        } else {
            st->has_range = true;
            st->lo_id = 0;
            st->hi_id = uv - 1;
        }
        break;
    default:
        free(st->rows);
        free(st);
        return E_NOTSUP;
    }

    *out = (AmScan *)st;
    return OK;
}

/* ---------------- 构造 ---------------- */

int ft_am_build_docs(FtIndex *ix, StorageAM *out, FtAmCtx *ctx)
{
    if (!ix || !out || !ctx) {
        return E_INVAL;
    }

    ctx->ix = ix;

    out->name = "ft_docs";
    /*
     * 能力位：**只读 + 可下推**。
     *
     * 没有 INSERT / UPDATE / REMOVE / VACUUM ——
     * ft_docs 是元数据视图，写入走专用 API。
     * 也没有 EXTERNAL（没有外部值：四列都是标量）。
     */
    out->caps = AM_CAP_SCAN | AM_CAP_PUSHDOWN;

    out->columns = ft_am_columns;
    out->scan_begin = ft_am_scan_begin;
    out->scan_next = ft_am_scan_next;
    out->scan_end = ft_am_scan_end;
    out->get = ft_am_get;
    out->decode = ft_am_decode;
    /* 批能力：ft 模态不支持（与 blob / vector 同款：三个模态一行都不用改） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = ft_am_visible;
    out->insert = ft_am_insert;
    out->update = ft_am_update;
    out->remove = ft_am_remove;
    out->vacuum = ft_am_vacuum;
    out->reindex = ft_am_reindex;
    out->impl = ctx;
    out->can_push = ft_am_can_push;
    out->push = ft_am_push;

    return OK;
}
