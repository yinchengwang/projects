/*
 * blob_am.c — blob 模态的 SPI 适配（v0.9）
 *
 * 设计说明见 spi/blob_am.h。
 */

#include "spi/blob_am.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 行格式（模态私有） ---------------- */

/*
 * blob_frames 的行：blob_id + frame_no（16 字节）
 * blob_objects 的行：blob_id + nframes + frame_size + total_len（28 字节）
 *
 * 【为什么不复用 record 格式】
 * record 格式是**关系模态**的行格式：xmin/xmax 头 + 变长偏移数组。
 * blob 的行没有版本（对象不可变），也不需要列式变长区。
 * 硬套 record 格式 = 逼所有模态长成关系模态。
 *
 * 所以模态有自己的行格式，**由自己解码**（SPI 的 decode 回调）。
 */

/* ---------------- 列定义 ---------------- */

static const ColDesc FRAME_COLS[5] = {
    { "blob_id",   T_INT8, false },
    { "frame_no",  T_INT8, false },
    { "data_len",  T_INT4, false },
    { "data_off",  T_INT8, false },
    { "data",      T_TEXT, false }
};

static const ColDesc OBJECT_COLS[4] = {
    { "blob_id",    T_INT8, false },
    { "nframes",    T_INT8, false },
    { "frame_size", T_INT4, false },
    { "total_len",  T_INT8, false }
};

/* ---------------- 扫描状态 ---------------- */

typedef struct {
    BlobStore *bs;
    bool       is_frames;

    u32      obj_index;      /* 目录里的下标 */
    BlobId   cur_id;
    u64      cur_nframes;
    u32      cur_frame_size;
    u64      cur_total_len;
    u32      cur_base;       /* 帧数据起始页号 */
    u64      frame_no;       /* 当前对象内的帧号 */

    /* 下推过滤：只枚举符合条件的对象 */
    bool    has_eq;
    BlobId  eq_id;
    bool    has_range;
    BlobId  lo_id;
    BlobId  hi_id;
} BlobScan;

/* ---------------- 工具 ---------------- */

static BlobAmCtx *ctx_of(StorageAM *am)
{
    return (BlobAmCtx *)am->impl;
}

static bool blob_match(const BlobScan *st, BlobId id)
{
    if (st->has_eq)
        return id == st->eq_id;
    if (st->has_range)
        return id >= st->lo_id && id <= st->hi_id;
    return true;
}

/*
 * 换到下一个符合条件的对象。
 *
 * 读它的头（帧数 / 帧大小 / 总长 / 起始页号）。
 * 头页会被缓冲池缓存 → 重复扫同一个对象不花 IO。
 */
static int blob_scan_next_object(BlobScan *st)
{
    u32 n = blob_count(st->bs);

    while (st->obj_index < n) {
        BlobId id = blob_id_at(st->bs, st->obj_index);
        st->obj_index++;

        if (!blob_match(st, id))
            continue;

        u64 nf = 0;
        u64 tl = 0;
        u32 fs = 0;
        u32 base = 0;
        int rc = blob_header(st->bs, id, &nf, &fs, &tl, &base);
        if (rc != OK)
            return rc;

        st->cur_id = id;
        st->cur_nframes = nf;
        st->cur_frame_size = fs;
        st->cur_total_len = tl;
        st->cur_base = base;
        st->frame_no = 0;
        return OK;
    }

    return E_EOF;
}

/* ---------------- 外部值的取回 ---------------- */

/*
 * 把一帧的内容读出来 —— 外部值的取回函数。
 *
 * 【和 val_detoast 的约定】
 *   cap == 0 → 只问长度：填 *len_out，返回 E_NOSPACE
 *   cap >  0 → 真正读：填 *len_out，返回 OK
 *
 * 为什么分两步：外部值的字节数事先不知道，
 * 调用方没法自己定缓冲大小。问一次长度，才知道要给多大的缓冲。
 */
static int blob_detoast(void *ctx, u64 key, u64 off,
                        u8 *buf, u16 cap, u16 *len_out)
{
    BlobStore *bs = (BlobStore *)ctx;
    if (!bs)
        return E_INVAL;

    BlobId id = (BlobId)key;
    u64 frame_no = off;

    if (!buf || cap == 0) {
        u64 file_off = 0;
        u32 l = 0;
        int rc = blob_frame_info(bs, id, frame_no, &file_off, &l);
        if (rc != OK)
            return rc;
        if (len_out)
            *len_out = (u16)l;
        return E_NOSPACE;          /* "长度问到了" */
    }

    u32 n = 0;
    int rc = blob_read_frame(bs, id, frame_no, buf, cap, &n);
    if (rc != OK)
        return rc;

    if (len_out)
        *len_out = (u16)n;
    return OK;
}

/* ---------------- 读接口 ---------------- */

static int blob_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c || !cols || !ncols)
        return E_INVAL;

    if (c->is_frames) {
        *cols = FRAME_COLS;
        *ncols = 5;
    } else {
        *cols = OBJECT_COLS;
        *ncols = 4;
    }
    return OK;
}

static int blob_am_scan_begin(StorageAM *am, AmScan **out)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c || !out)
        return E_INVAL;

    BlobScan *st = calloc(1, sizeof(*st));
    if (!st)
        return E_IO;

    st->bs = c->bs;
    st->is_frames = c->is_frames;

    *out = (AmScan *)st;
    return OK;
}

static int blob_am_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                             u8 *buf, u16 cap, u16 *len)
{
    (void)am;
    BlobScan *st = (BlobScan *)s;

    for (;;) {
        /* 当前对象还有帧吗 */
        if (st->cur_id == BLOB_ID_INVALID || st->frame_no >= st->cur_nframes) {
            int rc = blob_scan_next_object(st);
            if (rc != OK)
                return rc;              /* E_EOF */
            continue;
        }

        if (!st->is_frames) {
            /* 对象表：一行一个对象 */
            if (cap < 28)
                return E_NOSPACE;
            put_u64(buf, st->cur_id);
            put_u64(buf + 8, st->cur_nframes);
            put_u32(buf + 16, st->cur_frame_size);
            put_u64(buf + 20, st->cur_total_len);
            *len = 28;
            *rid = rid_make((u32)(st->cur_id - 1), 0);   /* 头页页号 */

            /* 标记这个对象已产出 → 下一轮换下一个 */
            st->cur_id = BLOB_ID_INVALID;
            return OK;
        }

        /* 帧表：一行一帧 */
        if (cap < 16)
            return E_NOSPACE;
        put_u64(buf, st->cur_id);
        put_u64(buf + 8, st->frame_no);
        *len = 16;
        *rid = rid_make(st->cur_base + (u32)st->frame_no, 0);

        st->frame_no++;
        return OK;
    }
}

static int blob_am_scan_end(StorageAM *am, AmScan *s)
{
    (void)am;
    free(s);
    return OK;
}

/*
 * 按 RID 取一行。
 *
 * 【RID → (对象, 帧号) 的反解】
 *
 * 帧的 RID.page 是帧页号。要反解出它属于哪个对象，
 * 得知道每个对象的帧页区间 —— 遍历 blob 自己的对象目录，
 * 对每个对象读头，看它的 [base, base+nframes) 是否包含这个页。
 *
 * 代价 O(对象数)。v0.9 接受，**记为已知限制**：
 * 将来的修法是 blob 自己维护一张"页号 → 对象"的反查表。
 *
 * 注意这里用的是 **blob 自己的目录**，不是 catalog ——
 * 模态之间不许互相依赖（硬规则 6）。
 */
static int blob_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;

    BlobStore *bs = c->bs;
    u32 n = blob_count(bs);

    for (u32 i = 0; i < n; i++) {
        BlobId id = blob_id_at(bs, i);

        u64 nf = 0;
        u64 tl = 0;
        u32 fs = 0;
        u32 base = 0;
        int rc = blob_header(bs, id, &nf, &fs, &tl, &base);
        if (rc != OK)
            return rc;

        if (!c->is_frames) {
            /* 对象表：RID.page = 头页页号 = blob_id - 1 */
            if (rid.page != (u32)(id - 1))
                continue;
            if (cap < 28)
                return E_NOSPACE;
            put_u64(buf, id);
            put_u64(buf + 8, nf);
            put_u32(buf + 16, fs);
            put_u64(buf + 20, tl);
            *len = 28;
            return OK;
        }

        /* 帧表：RID.page ∈ [base, base + nframes) */
        if (rid.page < base)
            continue;
        u64 frame_no = (u64)(rid.page - base);
        if (frame_no >= nf)
            continue;

        if (cap < 16)
            return E_NOSPACE;
        put_u64(buf, id);
        put_u64(buf + 8, frame_no);
        *len = 16;
        return OK;
    }

    return E_NOTFOUND;
}

/*
 * 把 blob 的行解成值。
 *
 * 帧表：blob_id / frame_no 直接从行里读；
 *       data_len / data_off 要问 blob（要读对象头）；
 *       data 是**外部值** —— 句柄 (blob_id, frame_no)，内容按需取。
 *
 * 对象表：四个字段都在行里。
 */
static int blob_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;

    if (!c->is_frames) {
        if (len < 28)
            return E_CORRUPT;
        val_int(&out[0], (i64)get_u64(buf));
        val_int(&out[1], (i64)get_u64(buf + 8));
        val_int(&out[2], (i64)get_u32(buf + 16));
        val_int(&out[3], (i64)get_u64(buf + 20));
        return OK;
    }

    if (len < 16)
        return E_CORRUPT;

    u64 blob_id = get_u64(buf);
    u64 frame_no = get_u64(buf + 8);

    u64 off = 0;
    u32 dlen = 0;
    int rc = blob_frame_info(c->bs, (BlobId)blob_id, frame_no, &off, &dlen);
    if (rc != OK)
        return rc;

    val_int(&out[0], (i64)blob_id);
    val_int(&out[1], (i64)frame_no);
    val_int(&out[2], (i64)dlen);
    val_int(&out[3], (i64)off);
    /*
     * data 是外部值：这里**不读帧内容**，只挂句柄。
     * 内容被需要时（输出到结果集 / 参与比较）才去读。
     */
    val_external_text(&out[4], blob_id, frame_no, blob_detoast, c->bs);
    return OK;
}

/*
 * 可见性：blob 的行**恒可见**。
 *
 * 对象不可变 → 没有版本 → 没有"这份快照看不看得见"的问题。
 *
 * 这正是"可见性归模态"的意义：执行器不需要知道 blob 有没有 xmin/xmax。
 */
static bool blob_am_visible(StorageAM *am, const u8 *buf, u16 len,
                            const AmVisCtx *vis)
{
    (void)am; (void)buf; (void)len; (void)vis;
    return true;
}

/* ---------------- 写接口：blob 是只读的 ---------------- */

/*
 * blob 对象不可变（创建 → 写 → 定稿），所以 SQL 侧不能改。
 *
 * 返回 E_NOTSUP —— 执行器据此报"这个表不支持写"。
 * 这就是**能力协商靠返回值**（DESIGN.md §5.2）。
 */
static int blob_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    (void)am; (void)xid; (void)vals; (void)out;
    return E_NOTSUP;
}

static int blob_am_update(StorageAM *am, u32 xid, Rid rid,
                          const DbValue *vals, Rid *new_rid)
{
    (void)am; (void)xid; (void)rid; (void)vals; (void)new_rid;
    return E_NOTSUP;
}

static int blob_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    (void)am; (void)xid; (void)rid;
    return E_NOTSUP;
}

/* ---------------- 维护操作：blob 没有版本，也没有索引 ---------------- */

/*
 * VACUUM：blob 没有版本 → 没有死版本 → 什么都不用做。
 *
 * 返回 E_NOTSUP 而不是 OK：
 * "这个模态不需要 vacuum"和"vacuum 成功了"是两回事，
 * 前者是能力问题，后者是执行结果。
 */
static int blob_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                          AmVacuumStats *out)
{
    (void)am; (void)txns; (void)xid; (void)out;
    return E_NOTSUP;
}

/* 建索引 / 重建索引：blob 的行不是关系行，没有索引这回事 */
static int blob_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    (void)am; (void)index_id; (void)rows_out;
    return E_NOTSUP;
}

/* ---------------- 谓词下推 ---------------- */

/*
 * blob 能下推什么？
 *
 * blob_id 上的比较。理由：
 *   - blob_id 是对象的主键（等值 = 定位一个对象）
 *   - 范围 = 一段对象
 *
 * 别的列（frame_no / data_len / data_off / data）**不能下推**：
 *   - frame_no 的范围可以下推，但下推后还是要逐帧判断，收益不大，
 *     而且容易写出"漏行"的 bug —— 保守起见不推。
 *   - data 是外部值，拿来比较要先物化 —— 那就不叫下推了。
 *
 * 【宁可不推，不可推错】
 * 下推错了会静默少数据。所以判据从紧。
 */
static int blob_am_can_push(StorageAM *am, const Predicate *p)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c || !p)
        return E_NOTSUP;

    /* 只有 blob_id（第 0 列）能推 */
    if (p->col_idx != 0)
        return E_NOTSUP;

    if (p->op != PD_EQ && p->op != PD_LT && p->op != PD_GT &&
        p->op != PD_LE && p->op != PD_GE)
        return E_NOTSUP;

    return OK;
}

/*
 * 下推：只枚举符合条件的对象。
 *
 * 【效果是可测的】
 * 全表扫要读**每个对象**的头页（要知道它有几帧）。
 * 下推后只读匹配的那个对象的头页。
 * 所以 "WHERE blob_id = 3" 的页读次数 << 全表扫。
 */
static int blob_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    BlobAmCtx *c = ctx_of(am);
    if (!c || !p || !out)
        return E_INVAL;

    BlobScan *st = calloc(1, sizeof(*st));
    if (!st)
        return E_IO;

    st->bs = c->bs;
    st->is_frames = c->is_frames;

    u64 v = (u64)p->val.i;

    switch (p->op) {
    case PD_EQ:
        st->has_eq = true;
        st->eq_id = (BlobId)v;
        break;
    case PD_GE:
        st->has_range = true;
        st->lo_id = (BlobId)v;
        st->hi_id = (BlobId)~0ull;
        break;
    case PD_LE:
        st->has_range = true;
        st->lo_id = 1;
        st->hi_id = (BlobId)v;
        break;
    case PD_GT:
        /*
         * 严格大于 → 闭区间 [v+1, ∞)
         *
         * blob_id 是整数，所以能精确转换（不像浮点数）。
         * 转换不了就退化成 [v, ∞)（超集，执行器会重算 WHERE）。
         */
        st->has_range = true;
        st->lo_id = (BlobId)(v + 1);
        st->hi_id = (BlobId)~0ull;
        break;
    case PD_LT:
        st->has_range = true;
        st->lo_id = 1;
        st->hi_id = (BlobId)(v > 0 ? v - 1 : 0);
        break;
    default:
        free(st);
        return E_NOTSUP;
    }

    *out = (AmScan *)st;
    return OK;
}

/* ---------------- 构造 ---------------- */

static int blob_am_build(BlobStore *bs, StorageAM *out, BlobAmCtx *ctx,
                         bool is_frames)
{
    if (!bs || !out || !ctx)
        return E_INVAL;

    ctx->bs = bs;
    ctx->is_frames = is_frames;

    out->name = is_frames ? "blob_frames" : "blob_objects";
    out->caps = AM_CAP_SCAN | AM_CAP_EXTERNAL | AM_CAP_PUSHDOWN;
    out->columns = blob_am_columns;
    out->scan_begin = blob_am_scan_begin;
    out->scan_next = blob_am_scan_next;
    out->scan_end = blob_am_scan_end;
    out->get = blob_am_get;
    out->decode = blob_am_decode;
    /* 批能力：blob 不支持（PLAN-v1.2 步骤 0：三个模态一行都不用改） */
    out->batch_scan = NULL;
    out->batch_decode = NULL;
    out->visible = blob_am_visible;
    out->insert = blob_am_insert;
    out->update = blob_am_update;
    out->remove = blob_am_remove;
    out->vacuum = blob_am_vacuum;
    out->reindex = blob_am_reindex;
    out->impl = ctx;
    out->can_push = blob_am_can_push;
    out->push = blob_am_push;
    return OK;
}

int blob_am_build_frames(BlobStore *bs, StorageAM *out, BlobAmCtx *ctx)
{
    return blob_am_build(bs, out, ctx, true);
}

int blob_am_build_objects(BlobStore *bs, StorageAM *out, BlobAmCtx *ctx)
{
    return blob_am_build(bs, out, ctx, false);
}
