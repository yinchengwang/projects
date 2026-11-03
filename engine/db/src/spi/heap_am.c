/*
 * heap_am.c — 关系模态的 SPI 适配（v0.9）
 *
 * 设计说明见 spi/heap_am.h。这里只讲实现上的关键点。
 *
 * 【这个文件从 exec.c 里搬走了什么】
 *
 *   1. 索引维护（index_insert_row / index_delete_row）
 *   2. 索引选择（leaf_indexable / pick_index）→ 变成"下推"
 *   3. 版本号打标（stamp_version / stamp_deleted）
 *   4. MVCC 更新的两段式（打 xmax + 插新版本）
 *
 * 搬走之后，exec.c 里**再也没有"堆表"的知识**了 ——
 * 它只知道"一张表有一个 StorageAM"。
 */

#include "spi/heap_am.h"

#include <stdlib.h>
#include <string.h>

#include "index/btree.h"
#include "index/index_key.h"
#include "spi/heap_am_internal.h"
#include "txn/mvcc.h"
#include "txn/vacuum.h"

/* ---------------- 工具 ---------------- */

static HeapAmCtx *ctx_of(StorageAM *am)
{
    return (HeapAmCtx *)am->impl;
}

/* 找列上的索引。找不到返回 NULL。 */
static const IndexInfo *index_for_col(const HeapAmCtx *c, int col_idx)
{
    for (int i = 0; i < c->nidx; i++) {
        if ((int)c->idxs[i].meta.col_no == col_idx)
            return &c->idxs[i];
    }
    return NULL;
}

/*
 * 维护索引：把一行写进（或从）各索引。
 *
 * NULL 值不入索引（索引只存非 NULL 的行）。
 */
static int index_insert_row(const HeapAmCtx *c, const DbValue *row, Rid rid)
{
    for (int i = 0; i < c->nidx; i++) {
        const IndexInfo *ix = &c->idxs[i];
        u32 col = ix->meta.col_no;
        if ((int)col >= c->ncols)
            continue;
        u8 kb[BT_MAX_KEY];
        u16 kl;
        if (index_key_encode(&c->cols[col], &row[col], rid_to_u64(rid),
                             kb, sizeof(kb), &kl) != OK)
            continue;                    /* NULL 不入索引 */
        int rc = btree_insert(ix->bt, kb, kl, rid);
        if (rc != OK)
            return rc;
    }
    return OK;
}

static void index_delete_row(const HeapAmCtx *c, const DbValue *row, Rid rid)
{
    for (int i = 0; i < c->nidx; i++) {
        const IndexInfo *ix = &c->idxs[i];
        u32 col = ix->meta.col_no;
        if ((int)col >= c->ncols)
            continue;
        u8 kb[BT_MAX_KEY];
        u16 kl;
        if (index_key_encode(&c->cols[col], &row[col], rid_to_u64(rid),
                             kb, sizeof(kb), &kl) != OK)
            continue;
        (void)btree_delete(ix->bt, kb, kl, rid);    /* 不在也无所谓 */
    }
}

/* ---------------- 读接口 ---------------- */

static int heap_am_columns(StorageAM *am, const ColDesc **cols, int *ncols)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !cols || !ncols)
        return E_INVAL;
    *cols = c->cols;
    *ncols = c->ncols;
    return OK;
}

static int heap_am_scan_begin(StorageAM *am, AmScan **out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !out)
        return E_INVAL;

    HeapScanState *st = calloc(1, sizeof(*st));
    if (!st)
        return E_IO;

    st->kind = SCAN_FULL;
    int rc = heap_scan_begin(c->heap, &st->full);
    if (rc != OK) {
        free(st);
        return rc;
    }

    *out = (AmScan *)st;
    return OK;
}

/*
 * 取下一行。逐行路径与批路径共用这一份逻辑。
 *
 * 【为什么要共用】
 * 批路径如果自己写一遍"全表扫 / 下推扫"的分支，两份逻辑迟早走偏 ——
 * 而这种偏差只在特定数据下才显形，极难查。
 */
int heap_am_scan_next_row(HeapAmCtx *c, AmScan *s, Rid *rid,
                          u8 *buf, u16 cap, u16 *len)
{
    (void)c;
    HeapScanState *st = (HeapScanState *)s;

    if (st->kind == SCAN_FULL)
        return heap_scan_next(st->full, rid, buf, cap, len);

    /* 下推扫描：游标给 RID，回表拿行 */
    for (;;) {
        int rc = btree_cursor_next(st->cursor, rid);
        if (rc != OK)
            return rc;                 /* E_EOF 或真错误 */

        rc = heap_get(st->heap, *rid, buf, cap, len);
        if (rc == OK)
            return OK;

        /* 索引里有、堆里没了：跳过 */
    }
}

static int heap_am_scan_next(StorageAM *am, AmScan *s, Rid *rid,
                             u8 *buf, u16 cap, u16 *len)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;
    return heap_am_scan_next_row(c, s, rid, buf, cap, len);
}

static int heap_am_scan_end(StorageAM *am, AmScan *s)
{
    (void)am;
    HeapScanState *st = (HeapScanState *)s;
    if (!st)
        return OK;

    if (st->kind == SCAN_FULL)
        heap_scan_end(st->full);
    else if (st->cursor)
        btree_cursor_close(st->cursor);

    free(st->arena);
    free(st);
    return OK;
}

static int heap_am_get(StorageAM *am, Rid rid, u8 *buf, u16 cap, u16 *len)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;
    return heap_get(c->heap, rid, buf, cap, len);
}

static int heap_am_decode(StorageAM *am, const u8 *buf, u16 len, DbValue *out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;
    /* 用预计算的布局 —— 不每行重算（见 record.h 的 RecLayout 说明） */
    return val_from_record_layout(&c->layout, c->cols, c->ncols, buf, len, out);
}

static bool heap_am_visible(StorageAM *am, const u8 *buf, u16 len,
                            const AmVisCtx *vis)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !vis || !vis->snap || !vis->txns)
        return false;
    return mvcc_visible(vis->snap, vis->txns, buf, len);
}

/* ---------------- 写接口 ---------------- */

/*
 * 插入。
 *
 * xid 显式传进来（硬规则 5）—— AM 是共享的，xid 不能存在里面。
 */
static int heap_am_insert(StorageAM *am, u32 xid, const DbValue *vals, Rid *out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !vals || !out)
        return E_INVAL;

    u8 recbuf[PAGE_SIZE];
    u16 reclen = 0;

    int rc = val_to_record(c->cols, c->ncols, vals, recbuf, sizeof(recbuf), &reclen);
    if (rc != OK)
        return rc;

    /* 打版本号：这一行是 xid 这个事务写的 */
    rec_set_xmin(recbuf, (u64)xid);
    rec_set_xmax(recbuf, 0);

    rc = heap_insert(c->heap, xid, recbuf, reclen, out);
    if (rc != OK)
        return rc;

    return index_insert_row(c, vals, *out);
}

/*
 * MVCC 更新 = 两个原语的组合：
 *   1. 给旧版本打 xmax 标记（不抹掉，旧快照还要看它）
 *   2. 插入新版本（RID 会变）
 *
 * 【为什么不直接用 heap_update】
 * heap_update 做的是"原地覆盖"。而原地覆盖会让
 * "读者看到旧值还是新值"无法回答 —— 旧值被覆盖后就不存在于任何地方了。
 */
static int heap_am_update(StorageAM *am, u32 xid, Rid rid,
                          const DbValue *vals, Rid *new_rid)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !vals || !new_rid)
        return E_INVAL;
    if (c->ncols <= 0)
        return E_INVAL;

    u8 oldbuf[PAGE_SIZE];
    u16 oldlen = 0;
    int rc = heap_get(c->heap, rid, oldbuf, sizeof(oldbuf), &oldlen);
    if (rc != OK)
        return rc;

    /*
     * 旧行的值：用来算"索引里该删哪个 key"。
     *
     * 用堆分配而不是定长数组 —— 列数是表的属性，
     * 不该在这里假设一个上限（定长数组会引入一个假的限制）。
     */
    DbValue *oldvals = calloc((size_t)c->ncols, sizeof(DbValue));
    if (!oldvals)
        return E_IO;

    rc = val_from_record(c->cols, c->ncols, oldbuf, oldlen, oldvals);
    if (rc != OK) {
        free(oldvals);
        return rc;
    }

    u8 newbuf[PAGE_SIZE];
    u16 newlen = 0;
    rc = val_to_record(c->cols, c->ncols, vals, newbuf, sizeof(newbuf), &newlen);
    if (rc != OK) {
        free(oldvals);
        return rc;
    }

    /* 旧版本打上"被删除"的标记 */
    rec_set_xmax(oldbuf, (u64)xid);
    rc = heap_write_ver(c->heap, xid, rid, oldbuf, oldlen);
    if (rc != OK) {
        free(oldvals);
        return rc;
    }

    /* 旧索引项按旧值删 */
    index_delete_row(c, oldvals, rid);
    free(oldvals);

    /* 新版本 */
    rec_set_xmin(newbuf, (u64)xid);
    rec_set_xmax(newbuf, 0);
    rc = heap_insert(c->heap, xid, newbuf, newlen, new_rid);
    if (rc != OK) {
        /* 插入失败：把旧索引项补回去（保持索引和堆一致） */
        DbValue *back = calloc((size_t)c->ncols, sizeof(DbValue));
        if (back) {
            u8 tmp[PAGE_SIZE];
            u16 tmplen = 0;
            if (heap_get(c->heap, rid, tmp, sizeof(tmp), &tmplen) == OK &&
                val_from_record(c->cols, c->ncols, tmp, tmplen, back) == OK)
                index_insert_row(c, back, rid);
            free(back);
        }
        return rc;
    }

    return index_insert_row(c, vals, *new_rid);
}

/*
 * 删除 = 打 xmax 标记，不是抹掉记录。
 *
 * 旧版本要留给还需要它的快照看。真正抹除是 vacuum 的活。
 */
static int heap_am_remove(StorageAM *am, u32 xid, Rid rid)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;

    u8 recbuf[PAGE_SIZE];
    u16 reclen = 0;
    int rc = heap_get(c->heap, rid, recbuf, sizeof(recbuf), &reclen);
    if (rc != OK)
        return rc;

    /* 删之前要拿到旧行，才知道索引里该删哪个 key */
    DbValue *oldvals = calloc((size_t)c->ncols, sizeof(DbValue));
    if (oldvals) {
        if (val_from_record(c->cols, c->ncols, recbuf, reclen, oldvals) == OK)
            index_delete_row(c, oldvals, rid);
        free(oldvals);
    }

    rec_set_xmax(recbuf, (u64)xid);
    return heap_write_ver(c->heap, xid, rid, recbuf, reclen);
}

/* ---------------- 维护操作 ---------------- */

/*
 * 版本回收（VACUUM）。
 *
 * 只有"有版本"的模态需要。blob 的对象不可变 → 没有版本。
 */
static int heap_am_vacuum(StorageAM *am, TxnTable *txns, u32 xid,
                          AmVacuumStats *out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !txns)
        return E_INVAL;

    VacuumStats st;
    int rc = vacuum_table(c->heap, txns, xid, &st);
    if (rc != OK)
        return rc;

    if (out) {
        out->scanned = st.scanned;
        out->removed = st.removed;
        out->kept = st.kept;
    }
    return OK;
}

/*
 * 重建索引：把已有行灌进索引。
 *
 * index_id = 0 → 重建全部索引。
 *
 * 【为什么索引不写日志，所以要重建】
 * v0.6 的决定：索引不写 WAL —— 索引是**派生物**，可以从表数据重建。
 * 崩溃恢复后索引可能是旧的（甚至半成品），必须重建。
 *
 * 【重建前必须先清空】
 * btree_reset 把整棵树清空（页还给 freelist）。
 * 不清空就往里灌 → 旧项还在 → 同一个 RID 出现两次 → 扫描时重复返回。
 */
static int heap_am_reindex(StorageAM *am, u32 index_id, u64 *rows_out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c)
        return E_INVAL;
    if (rows_out)
        *rows_out = 0;

    for (int i = 0; i < c->nidx; i++) {
        if (index_id != 0 && c->idxs[i].meta.index_id != index_id)
            continue;
        (void)btree_reset(c->idxs[i].bt);
    }

    if (c->ncols <= 0)
        return OK;

    DbValue *row = calloc((size_t)c->ncols, sizeof(DbValue));
    if (!row)
        return E_IO;

    HeapScan *scan = NULL;
    int rc = heap_scan_begin(c->heap, &scan);
    if (rc != OK) {
        free(row);
        return rc;
    }

    u8 recbuf[PAGE_SIZE];
    u16 reclen = 0;
    Rid rid;
    u64 nrows = 0;

    while (heap_scan_next(scan, &rid, recbuf, sizeof(recbuf), &reclen) == OK) {
        if (val_from_record(c->cols, c->ncols, recbuf, reclen, row) != OK)
            continue;

        nrows++;

        /*
         * 【先把页放开，再灌索引】
         *
         * 扫描器会把当前页一直 pin 着（少一次哈希查找），
         * 于是这时候手里捏着 frame 域的锁。
         * 而 B+tree 的操作是 "btree → frame"（树的操作要拿页）。
         * 如果这里直接灌索引，就成了 "frame → btree" —— **逆序**，
         * 全序断言会当场 abort。
         *
         * 行已经拷进 recbuf 了（heap 的规矩：返回值一律拷贝），
         * 页已经没有用途 → 先放开，锁序就干净了 ✓
         */
        heap_scan_release_page(scan);

        for (int i = 0; i < c->nidx; i++) {
            const IndexInfo *ix = &c->idxs[i];
            if (index_id != 0 && ix->meta.index_id != index_id)
                continue;
            u32 col = ix->meta.col_no;
            if ((int)col >= c->ncols)
                continue;
            u8 kb[BT_MAX_KEY];
            u16 kl = 0;
            if (index_key_encode(&c->cols[col], &row[col], rid_to_u64(rid),
                                 kb, sizeof(kb), &kl) != OK)
                continue;                    /* NULL 不入索引 */
            (void)btree_insert(ix->bt, kb, kl, rid);
        }
    }

    heap_scan_end(scan);
    free(row);
    if (rows_out)
        *rows_out = nrows;
    return OK;
}

/* ---------------- 谓词下推 ---------------- */

/*
 * 这个谓词我能处理吗？
 *
 * 判据（和 v0.5 的索引选择逻辑一致）：
 *   1. 是"列 vs 常量"的比较
 *   2. 比较符是 B+tree 能用的那几种（= < > <= >=）
 *   3. 这一列上有索引
 *
 * 【为什么判据在 AM 里，不在执行器里】
 * 执行器不知道"哪些列有索引"—— 那是表和模态的知识。
 * 它只负责把表达式翻译成 Predicate。
 */
static int heap_am_can_push(StorageAM *am, const Predicate *p)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !p)
        return E_NOTSUP;

    if (p->op != PD_EQ && p->op != PD_LT && p->op != PD_GT &&
        p->op != PD_LE && p->op != PD_GE)
        return E_NOTSUP;

    if (p->col_idx < 0 || p->col_idx >= c->ncols)
        return E_NOTSUP;

    if (!index_for_col(c, p->col_idx))
        return E_NOTSUP;

    return OK;
}

/*
 * 下推：打开索引游标，当作行源交给执行器。
 *
 * 【边界一律用闭区间】
 * B+tree 的扫描是闭区间。严格小于 / 大于会多带一个端点行 ——
 * 那是**超集**，执行器会重新求值 WHERE 把它滤掉。
 * （多给行 = 慢一点；少给行 = 数据错。）
 */
static int heap_am_push(StorageAM *am, const Predicate *p, AmScan **out)
{
    HeapAmCtx *c = ctx_of(am);
    if (!c || !p || !out)
        return E_INVAL;

    const IndexInfo *ix = index_for_col(c, p->col_idx);
    if (!ix)
        return E_NOTSUP;

    /*
     * 边界用复合键的「值部分 + RID 极值」，
     * 这样同一个值的所有行都落在区间里（详见 index_key.h）。
     *
     * 【为什么 > 和 >= 用同一个下界】
     * 下推只是"少读一些行"，精确过滤由执行器复检 WHERE 完成。
     * 用闭区间会多读一点，但绝不会少读 —— 宁可多读，不可漏读。
     */
    u8 lobuf[BT_MAX_KEY];
    u8 hibuf[BT_MAX_KEY];
    u16 lolen = 0;
    u16 hilen = 0;

    const ColDesc *col = &c->cols[p->col_idx];

    switch (p->op) {
    case PD_EQ:
        if (index_key_bound_lo(col, &p->val, lobuf, sizeof(lobuf), &lolen) != OK)
            return E_NOTSUP;
        if (index_key_bound_hi(col, &p->val, hibuf, sizeof(hibuf), &hilen) != OK)
            return E_NOTSUP;
        break;
    case PD_GT:
    case PD_GE:
        if (index_key_bound_lo(col, &p->val, lobuf, sizeof(lobuf), &lolen) != OK)
            return E_NOTSUP;
        break;
    case PD_LT:
    case PD_LE:
        if (index_key_bound_hi(col, &p->val, hibuf, sizeof(hibuf), &hilen) != OK)
            return E_NOTSUP;
        break;
    default:
        return E_NOTSUP;
    }

    const u8 *lo = (lolen > 0) ? lobuf : NULL;
    const u8 *hi = (hilen > 0) ? hibuf : NULL;

    BtCursor *cur = NULL;
    int rc = btree_scan(ix->bt, lo, lolen, hi, hilen, &cur);
    if (rc != OK)
        return rc;

    HeapScanState *st = calloc(1, sizeof(*st));
    if (!st) {
        btree_cursor_close(cur);
        return E_IO;
    }
    st->kind = SCAN_PUSH;
    st->cursor = cur;
    st->heap = c->heap;

    *out = (AmScan *)st;
    return OK;
}

/* ---------------- 构造 ---------------- */

int heap_am_build(Heap *heap, const ColDesc *cols, int ncols,
                  const IndexInfo *idxs, int nidx,
                  StorageAM *am_out, HeapAmCtx *ctx_out)
{
    if (!heap || !cols || !am_out || !ctx_out)
        return E_INVAL;

    ctx_out->heap = heap;
    ctx_out->cols = cols;
    ctx_out->ncols = ncols;
    ctx_out->idxs = idxs;
    ctx_out->nidx = nidx;

    /* 记录布局：算一次，整条查询都用它 */
    int lrc = rec_layout(cols, ncols, &ctx_out->layout);
    if (lrc != OK)
        return lrc;

    am_out->name = "heap";
    am_out->caps = AM_CAP_SCAN | AM_CAP_INSERT | AM_CAP_UPDATE |
                   AM_CAP_REMOVE | AM_CAP_VACUUM | AM_CAP_INDEX |
                   AM_CAP_PUSHDOWN |
                   AM_CAP_BATCH_SCAN | AM_CAP_BATCH_DECODE;
    am_out->columns = heap_am_columns;
    am_out->scan_begin = heap_am_scan_begin;
    am_out->scan_next = heap_am_scan_next;
    am_out->scan_end = heap_am_scan_end;
    am_out->get = heap_am_get;
    am_out->decode = heap_am_decode;
    am_out->batch_scan = heap_am_batch_scan;
    am_out->batch_decode = heap_am_batch_decode;
    am_out->visible = heap_am_visible;
    am_out->insert = heap_am_insert;
    am_out->update = heap_am_update;
    am_out->remove = heap_am_remove;
    am_out->vacuum = heap_am_vacuum;
    am_out->reindex = heap_am_reindex;
    am_out->impl = ctx_out;
    am_out->can_push = heap_am_can_push;
    am_out->push = heap_am_push;

    return OK;
}
