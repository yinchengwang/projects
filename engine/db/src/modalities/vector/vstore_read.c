/*
 * vstore_read.c — 向量模态的读路径（v1.0）
 *
 * 和 vstore.c（写路径 + 生命周期）分开，是因为两者关心的事完全不同：
 *   写：分配页、挂链、写日志、维护内存目录
 *   读：地址算术、页定位、顺序扫描
 *
 * 读路径上没有任何写操作 —— 它连 xid 都不需要。
 */

#include "modalities/vector/vstore_internal.h"

#include <string.h>

/* ---------------- 读 ---------------- */

void vstore_slot_of(const VStore *vs, u64 row_id,
                    u32 *ordinal_out, u16 *slot_out, u16 *off_out)
{
    if (!vs) {
        if (ordinal_out) *ordinal_out = 0;
        if (slot_out)    *slot_out = 0;
        if (off_out)     *off_out = 0;
        return;
    }

    u32 ordinal = (u32)(row_id / vs->k);
    u16 slot = (u16)(row_id % vs->k);
    u16 off = (u16)(VEC_PAGE_HDR + (u32)slot * (u32)vs->dim * 4u);

    if (ordinal_out) *ordinal_out = ordinal;
    if (slot_out)    *slot_out = slot;
    if (off_out)     *off_out = off;
}

int vstore_get(VStore *vs, u64 row_id, float *out)
{
    if (!vs || !out)
        return E_INVAL;
    if (row_id >= vs->count)
        return E_NOTFOUND;

    u32 ordinal = 0;
    u16 slot = 0;
    u16 off = 0;
    vstore_slot_of(vs, row_id, &ordinal, &slot, &off);

    if (ordinal >= vs->npages)
        return E_NOTFOUND;

    PageFrame *fr = NULL;
    int rc = vec_read_page(vs, vs->pages[ordinal], &fr);
    if (rc != OK)
        return rc;

    if (fr->data[VEC_H_TYPE] != (u8)PT_VEC_DATA ||
        get_u16(fr->data + VEC_H_COUNT) <= slot) {
        pager_unpin(vs->pg, vs->pages[ordinal], false);
        return E_NOTFOUND;
    }

    memcpy(out, fr->data + off, (size_t)vs->dim * 4u);
    pager_unpin(vs->pg, vs->pages[ordinal], false);
    return OK;
}

int vstore_label(VStore *vs, u64 row_id, i32 *out)
{
    if (!vs || !out)
        return E_INVAL;
    if (row_id >= vs->count)
        return E_NOTFOUND;

    u8 rec[VEC_META_REC];
    u16 len = 0;
    int rc = gstore_get(vs->meta, vs->meta_rids[row_id], rec, sizeof(rec), &len);
    if (rc != OK)
        return rc;
    if (len != VEC_META_REC)
        return E_CORRUPT;

    vs->meta_reads++;

    *out = (i32)get_u32(rec + GS_REC_HDR_SIZE + 8);
    return OK;
}

/*
 * 把全部向量读进一块连续内存。
 *
 * 顺序扫数据页，一页一次 IO —— 列式布局在这里兑现。
 */
int vstore_scan_all(VStore *vs, float *buf, u64 *got_out)
{
    if (!vs || !buf || !got_out)
        return E_INVAL;

    u16 dim = vs->dim;
    u64 row = 0;

    for (u32 ord = 0; ord < vs->npages; ord++) {
        u32 pid = vs->pages[ord];
        PageFrame *fr = NULL;
        int rc = vec_read_page(vs, pid, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[VEC_H_TYPE] != (u8)PT_VEC_DATA) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }

        u16 cnt = get_u16(fr->data + VEC_H_COUNT);
        if ((u32)cnt > vs->k) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }

        memcpy(buf + (size_t)row * dim,
               fr->data + VEC_PAGE_HDR,
               (size_t)cnt * dim * sizeof(float));
        row += cnt;

        pager_unpin(vs->pg, pid, false);
    }

    *got_out = row;
    return OK;
}

int vstore_scan(VStore *vs, VecScanFn fn, void *ctx)
{
    if (!vs || !fn)
        return E_INVAL;

    /*
     * 两个顺序游标对齐着走。
     *
     * 反面做法：每取一个向量就去 gstore_get 一次标量行 ——
     * 那是**随机读**，每个向量一次页读，列式布局省下来的 IO 全还回去了。
     *
     * 正确做法：标量表也是按 row_id 顺序填的，
     * 所以它的一条**顺序游标**就能和向量页的顺序一一对上。
     * 两次顺序扫描，总 IO = 数据页数 + 标量页数。
     */
    GStoreCursor mc;
    gstore_cursor_init(vs->meta, &mc);

    u64 row = 0;

    for (u32 ord = 0; ord < vs->npages; ord++) {
        u32 pid = vs->pages[ord];
        PageFrame *fr = NULL;
        int rc = vec_read_page(vs, pid, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[VEC_H_TYPE] != (u8)PT_VEC_DATA) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }

        u16 cnt = get_u16(fr->data + VEC_H_COUNT);
        if ((u32)cnt > vs->k) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }

        for (u16 s = 0; s < cnt; s++) {
            u8 rec[VEC_META_REC];
            Rid rid;
            u16 len = 0;
            rc = gstore_cursor_next(vs->meta, &mc, &rid, rec, sizeof(rec), &len);
            if (rc == E_EOF) {
                pager_unpin(vs->pg, pid, false);
                return E_CORRUPT;      /* 数据页比标量表长 */
            }
            if (rc != OK) {
                pager_unpin(vs->pg, pid, false);
                return rc;
            }
            if (len != VEC_META_REC ||
                get_u64(rec + GS_REC_HDR_SIZE) != row) {
                pager_unpin(vs->pg, pid, false);
                return E_CORRUPT;
            }

            const float *v = (const float *)(fr->data + VEC_PAGE_HDR +
                                             (u32)s * (u32)vs->dim * 4u);
            int stop = fn(ctx, row, v,
                          (i32)get_u32(rec + GS_REC_HDR_SIZE + 8));
            if (stop) {
                pager_unpin(vs->pg, pid, false);
                return stop;
            }
            row++;
        }

        pager_unpin(vs->pg, pid, false);
    }

    return OK;
}

/* ---------------- 只读访问 ---------------- */

u16 vstore_dim(const VStore *vs)  { return vs ? vs->dim : 0; }
u16 vstore_k(const VStore *vs)    { return vs ? vs->k : 0; }
u64 vstore_count(const VStore *vs) { return vs ? vs->count : 0; }
u32 vstore_npages(const VStore *vs) { return vs ? vs->npages : 0; }

u32 vstore_page_id(const VStore *vs, u32 ordinal)
{
    if (!vs || ordinal >= vs->npages)
        return PAGE_ID_NIL;
    return vs->pages[ordinal];
}

u64 vstore_page_reads(const VStore *vs)
{
    return vs ? vs->page_reads : 0;
}

u64 vstore_meta_reads(const VStore *vs)
{
    return vs ? vs->meta_reads : 0;
}

/*
 * 页号 → 序号。二分。
 *
 * 前提：数据页按分配顺序追加，页号严格递增。
 * 这个前提由 vec_new_page 保证（每次 pager_alloc 都拿到更大的页号），
 * 而 pages[] 是按序号顺序填的 —— 所以 pages[] 有序。
 */
int vstore_ordinal_of_page(const VStore *vs, u32 page_id, u32 *ordinal_out)
{
    if (!vs || !ordinal_out)
        return E_INVAL;

    u32 lo = 0;
    u32 hi = vs->npages;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        if (vs->pages[mid] == page_id) {
            *ordinal_out = mid;
            return OK;
        }
        if (vs->pages[mid] < page_id)
            lo = mid + 1;
        else
            hi = mid;
    }
    return E_NOTFOUND;
}

GStore *vstore_meta(VStore *vs)
{
    return vs ? vs->meta : NULL;
}

void vstore_meta_decode(const u8 *rec, u64 *row_id, i32 *label)
{
    if (!rec)
        return;
    const u8 *pay = rec + GS_REC_HDR_SIZE;
    if (row_id)
        *row_id = get_u64(pay);
    if (label)
        *label = (i32)get_u32(pay + 8);
}
