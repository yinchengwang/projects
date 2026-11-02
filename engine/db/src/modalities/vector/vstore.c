/*
 * vstore.c — 向量模态的列式存储（v1.0）
 *
 * 设计说明见 modalities/vector/vstore.h。
 *
 * 【写路径的日志策略】
 *
 * 与 gstore / blob 同款：WAL_PAGEDATA 整页镜像。
 * 向量页是只追加的（写了不改），本来可以更省（只记新增的字节），
 * 但"一种页类型两套日志格式"会让恢复逻辑分叉 ——
 * 不值得为了省日志把恢复路径变复杂。整页镜像一条路走到底。
 *
 * 【读路径的页计数】
 *
 * vstore_get / vstore_scan 每次真正取一页就 +1（page_reads）。
 * 这是 A4 的证据：ANN 查询的页读必须远小于暴力扫（暴力扫要读完所有页）。
 */

#include "modalities/vector/vstore_internal.h"

#include <stdlib.h>
#include <string.h>

#include "storage/page.h"


/* ---------------- 内存目录：两个动态数组 ---------------- */

/*
 * 为什么用"手写动态数组"而不是 realloc 一个数组了事：
 * 两个数组（序号→页号、row_id→标量 RID）增长节奏不同 ——
 * 数据页每 k 个向量长一次，标量 RID 每个向量长一次。
 * 分开管各自的容量，比共用一个结构清楚。
 */

int vec_grow(void **arr, u32 *cap, u32 need, size_t elem)
{
    if (need <= *cap)
        return OK;

    u32 ncap = (*cap == 0) ? 16u : *cap;
    while (ncap < need) {
        if (ncap > 0x40000000u)
            return E_NOSPACE;
        ncap *= 2u;
    }

    void *p = realloc(*arr, (size_t)ncap * elem);
    if (!p)
        return E_IO;
    *arr = p;
    *cap = ncap;
    return OK;
}

/* ---------------- 页读（带计数） ---------------- */

int vec_read_page(VStore *vs, u32 page_id, PageFrame **out)
{
    int rc = pager_get(vs->pg, page_id, out);
    if (rc == OK)
        vs->page_reads++;
    return rc;
}

/* ---------------- 页写（WAL 整页镜像） ---------------- */

static int vec_log(VStore *vs, u32 xid, u32 page_id,
                   const u8 *before, const u8 *after, u32 *lsn_out)
{
    u32 lsn = 0;
    int rc = OK;

    if (vs->wal) {
        rc = vs->wal(vs->wal_ctx, xid, LOG_PAGEDATA, page_id,
                     PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                     before, (u16)PAGE_SIZE,
                     after, (u16)PAGE_SIZE,
                     &lsn);
        if (rc != OK)
            return rc;
    }
    if (lsn_out)
        *lsn_out = lsn;
    return OK;
}

int vec_write_page(VStore *vs, u32 xid, u32 page_id,
                          VecMut mut, void *mut_ctx)
{
    PageFrame *fr = NULL;
    int rc = pager_get(vs->pg, page_id, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    if (mut) {
        rc = mut(mut_ctx, fr->data);
        if (rc != OK) {
            pager_unpin(vs->pg, page_id, false);
            return rc;
        }
    }

    u32 lsn = 0;
    rc = vec_log(vs, xid, page_id, before, fr->data, &lsn);
    if (rc != OK) {
        memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(vs->pg, page_id, false);
        return rc;
    }

    put_u32(fr->data + VEC_H_LSN, lsn);
    page_set_checksum(fr->data);

    pager_unpin(vs->pg, page_id, true);
    return OK;
}

/* ---------------- 头页 ---------------- */

static int vec_hdr_find(Pager *pg, u32 *out)
{
    *out = PAGE_ID_NIL;

    u64 np = pager_npages(pg);
    for (u64 i = 0; i < np; i++) {
        PageFrame *fr = NULL;
        int rc = pager_get(pg, (u32)i, &fr);
        if (rc != OK)
            return rc;

        bool hit = (fr->data[VECH_H_TYPE] == (u8)PT_VEC_HDR);
        pager_unpin(pg, (u32)i, false);
        if (hit) {
            *out = (u32)i;
            return OK;
        }
    }
    return OK;
}

/* 头页的写：head / tail / npages 三个字段 */
struct hdr_arg {
    u32 head;
    u32 tail;
    u32 npages;
};

static int vec_hdr_mut(void *ctx, u8 *pg)
{
    const struct hdr_arg *a = (const struct hdr_arg *)ctx;
    if (pg[VECH_H_TYPE] != (u8)PT_VEC_HDR)
        return E_CORRUPT;

    put_u32(pg + VECH_H_HEAD, a->head);
    put_u32(pg + VECH_H_TAIL, a->tail);
    put_u32(pg + VECH_H_NPAGES, a->npages);
    return OK;
}

static int vec_hdr_sync(VStore *vs, u32 xid)
{
    struct hdr_arg a;
    a.head = vs->head;
    a.tail = vs->tail;
    a.npages = vs->npages;
    return vec_write_page(vs, xid, vs->hdr_page, vec_hdr_mut, &a);
}

/* 新建头页（分配 + 写日志 + 落盘） */
static int vec_hdr_create(VStore *vs, u32 xid, u16 dim, u16 k, u32 *out)
{
    u32 pid = 0;
    PageFrame *fr = NULL;
    int rc = pager_alloc(vs->pg, &pid, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);   /* pager_alloc 已清零 */

    memset(fr->data, 0, PAGE_SIZE);
    fr->data[VECH_H_TYPE] = (u8)PT_VEC_HDR;
    fr->data[VECH_H_VER] = (u8)VEC_HDR_VER;
    put_u16(fr->data + VECH_H_DIM, dim);
    put_u16(fr->data + VECH_H_K, k);
    put_u32(fr->data + VECH_H_HEAD, PAGE_ID_NIL);
    put_u32(fr->data + VECH_H_TAIL, PAGE_ID_NIL);
    put_u32(fr->data + VECH_H_NPAGES, 0);

    u32 lsn = 0;
    rc = vec_log(vs, xid, pid, before, fr->data, &lsn);
    if (rc != OK) {
        memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(vs->pg, pid, false);
        return rc;
    }
    put_u32(fr->data + VEC_H_LSN, lsn);
    page_set_checksum(fr->data);
    pager_unpin(vs->pg, pid, true);

    *out = pid;
    return OK;
}

/* ---------------- 数据页 ---------------- */

struct link_arg {
    u32 next;
};

static int vec_link_mut(void *ctx, u8 *pg)
{
    const struct link_arg *a = (const struct link_arg *)ctx;
    if (pg[VEC_H_TYPE] != (u8)PT_VEC_DATA)
        return E_CORRUPT;
    put_u32(pg + VEC_H_NEXT, a->next);
    return OK;
}

/*
 * 分配一个新的数据页，挂到链尾，并同步头页。
 *
 * 【为什么新页要立刻写日志】
 *
 * pager_alloc 只是把页标脏，崩溃时可能还没落盘。
 * 而"尾页的 next 指向它"这件事是**写了日志**的 ——
 * 恢复后链会指到一个内容还没落盘的页。
 *
 * 所以新页的内容本身也必须进日志（before = 全零，after = 初始化后）。
 * 恢复时按 LSN 顺序重放，链和内容一起回来。
 */
static int vec_new_page(VStore *vs, u32 xid)
{
    u32 pid = 0;
    PageFrame *fr = NULL;
    int rc = pager_alloc(vs->pg, &pid, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    memset(fr->data, 0, PAGE_SIZE);
    fr->data[VEC_H_TYPE] = (u8)PT_VEC_DATA;
    /* 链头标志只加在第一个数据页上（重启后靠它找到链的起点） */
    fr->data[VEC_H_FLAGS] = (vs->npages == 0) ? (u8)VEC_FLAG_HEAD : 0u;
    put_u16(fr->data + VEC_H_COUNT, 0);
    put_u32(fr->data + VEC_H_NEXT, PAGE_ID_NIL);
    put_u32(fr->data + VEC_H_ORDINAL, vs->npages);

    u32 lsn = 0;
    rc = vec_log(vs, xid, pid, before, fr->data, &lsn);
    if (rc != OK) {
        memcpy(fr->data, before, PAGE_SIZE);
        pager_unpin(vs->pg, pid, false);
        return rc;
    }
    put_u32(fr->data + VEC_H_LSN, lsn);
    page_set_checksum(fr->data);
    pager_unpin(vs->pg, pid, true);

    /* 挂到链尾 */
    if (vs->tail != PAGE_ID_NIL) {
        struct link_arg a;
        a.next = pid;
        rc = vec_write_page(vs, xid, vs->tail, vec_link_mut, &a);
        if (rc != OK)
            return rc;
    } else {
        vs->head = pid;
    }
    vs->tail = pid;

    rc = vec_grow((void **)&vs->pages, &vs->cap_pages, vs->npages + 1,
                  sizeof(u32));
    if (rc != OK)
        return rc;
    vs->pages[vs->npages] = pid;
    vs->npages++;

    return vec_hdr_sync(vs, xid);
}

/* 往数据页里写一个向量 */
struct put_arg {
    const float *vec;
    u16         slot;
    u16         dim;
};

static int vec_put_mut(void *ctx, u8 *pg)
{
    const struct put_arg *a = (const struct put_arg *)ctx;

    if (pg[VEC_H_TYPE] != (u8)PT_VEC_DATA)
        return E_CORRUPT;

    u16 cnt = get_u16(pg + VEC_H_COUNT);

    /* 只能顺序填：槽号必须落在 [0, count] 里 */
    if (a->slot > cnt)
        return E_CORRUPT;

    u32 vlen = (u32)a->dim * 4u;
    u32 off = VEC_PAGE_HDR + (u32)a->slot * vlen;

    /*
     * 坑 M3-1 的防线：越界写在这里拦住。
     *
     * 如果 K 算大了（比如 (4096-20)/512 被写成四舍五入的 8），
     * 第 8 个向量就会写到 20 + 8*512 = 4116 > 4096 —— 越界。
     * 越界写不报错，只会踩坏下一页（或者别的模块的数据）。
     */
    if (off + vlen > PAGE_SIZE)
        return E_NOSPACE;

    memcpy(pg + off, a->vec, vlen);
    if (a->slot == cnt)
        put_u16(pg + VEC_H_COUNT, (u16)(cnt + 1));
    return OK;
}

/* ---------------- 重建（打开时） ---------------- */

/*
 * 顺数据页链重建内存目录。
 *
 * 三个断言一起做（对应 MODALITIES.md 坑 M3-2）：
 *   1. 链上的页必须都是 PT_VEC_DATA
 *   2. 页头里记的序号必须等于它在链上的位置
 *   3. 各页 count 之和必须等于标量表的行数
 *
 * 这三条任何一条不成立，说明"第 i 个向量 ↔ 第 i 行标量"这个对应关系已经断了 ——
 * 那是最难查的一类错：不报错，只是查出来的向量配错了 label。
 * 与其带着错位的数据跑，不如打开就失败。
 */
static int vec_rebuild_chain(VStore *vs)
{
    u64 np = pager_npages(vs->pg);
    u32 pid = vs->head;
    u32 ordinal = 0;
    u64 sum = 0;

    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        int rc = pager_get(vs->pg, pid, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[VEC_H_TYPE] != (u8)PT_VEC_DATA) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }
        if (get_u32(fr->data + VEC_H_ORDINAL) != ordinal) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }

        u16 cnt = get_u16(fr->data + VEC_H_COUNT);
        if ((u32)cnt > vs->k) {
            pager_unpin(vs->pg, pid, false);
            return E_CORRUPT;
        }
        u32 next = get_u32(fr->data + VEC_H_NEXT);
        pager_unpin(vs->pg, pid, false);

        rc = vec_grow((void **)&vs->pages, &vs->cap_pages, ordinal + 1,
                      sizeof(u32));
        if (rc != OK)
            return rc;
        vs->pages[ordinal] = pid;

        sum += cnt;
        ordinal++;
        pid = next;

        /* 环检测：链长不可能超过文件页数 */
        if ((u64)ordinal > np)
            return E_CORRUPT;
    }

    if (ordinal != vs->npages)
        return E_CORRUPT;

    vs->chain_sum = sum;
    return OK;
}

/* 重建 row_id → 标量 RID 的表，并断言序号一致 */
static int vec_rebuild_meta(VStore *vs)
{
    GStoreCursor c;
    gstore_cursor_init(vs->meta, &c);

    u8 rec[VEC_META_REC];
    Rid rid;
    u16 len = 0;
    u64 i = 0;
    int rc;

    while ((rc = gstore_cursor_next(vs->meta, &c, &rid, rec, sizeof(rec),
                                    &len)) == OK) {
        if (len != VEC_META_REC)
            return E_CORRUPT;

        /* 列对齐的核心断言：第 i 条标量记录的 row_id 必须是 i */
        if (get_u64(rec + GS_REC_HDR_SIZE) != i)
            return E_CORRUPT;

        rc = vec_grow((void **)&vs->meta_rids, &vs->cap_rids, (u32)(i + 1),
                      sizeof(Rid));
        if (rc != OK)
            return rc;
        vs->meta_rids[i] = rid;
        i++;
    }
    if (rc != E_EOF)
        return rc;

    vs->count = i;
    return OK;
}

/* 读头页 + 建两个 store + 重建内存结构 */
static int vec_attach(VStore *vs)
{
    PageFrame *fr = NULL;
    int rc = pager_get(vs->pg, vs->hdr_page, &fr);
    if (rc != OK)
        return rc;

    if (fr->data[VECH_H_TYPE] != (u8)PT_VEC_HDR) {
        pager_unpin(vs->pg, vs->hdr_page, false);
        return E_CORRUPT;
    }

    u16 dim = get_u16(fr->data + VECH_H_DIM);
    u16 k = get_u16(fr->data + VECH_H_K);
    u32 head = get_u32(fr->data + VECH_H_HEAD);
    u32 npages = get_u32(fr->data + VECH_H_NPAGES);
    pager_unpin(vs->pg, vs->hdr_page, false);

    if (dim == 0 || dim * 4u > PAGE_SIZE - VEC_PAGE_HDR)
        return E_CORRUPT;
    /* k 必须能由 dim 重算出来 —— 对不上说明头页被写坏了 */
    if (k != (u16)VEC_K(dim) || k == 0)
        return E_CORRUPT;

    vs->dim = dim;
    vs->k = k;
    vs->head = head;
    vs->tail = PAGE_ID_NIL;
    vs->npages = npages;

    rc = vec_rebuild_chain(vs);
    if (rc != OK)
        return rc;

    /* 标量表：定长槽页，一个向量一行 */
    rc = gstore_create(vs->pg, vs->wal, vs->wal_ctx, VEC_META_REC,
                       PT_VEC_META, &vs->meta);
    if (rc != OK)
        return rc;
    rc = gstore_recover(vs->meta);
    if (rc != OK)
        return rc;

    rc = vec_rebuild_meta(vs);
    if (rc != OK)
        return rc;

    /* 交叉校验：数据页里的向量总数必须等于标量表的行数 */
    if (vs->chain_sum != vs->count || vs->count != vs->meta->alive)
        return E_CORRUPT;

    if (vs->npages > 0)
        vs->tail = vs->pages[vs->npages - 1];

    return OK;
}

/* ---------------- 建 / 开 / 拆 ---------------- */

static VStore *vec_alloc(void)
{
    return (VStore *)calloc(1, sizeof(VStore));
}

int vstore_create(Pager *pg, LogHookFn fn, void *ctx, u16 dim, VStore **out)
{
    if (!pg || !out)
        return E_INVAL;
    if (dim == 0 || (u32)dim * 4u > PAGE_SIZE - VEC_PAGE_HDR)
        return E_INVAL;

    VStore *vs = vec_alloc();
    if (!vs)
        return E_IO;

    vs->pg = pg;
    vs->wal = fn;
    vs->wal_ctx = ctx;

    u32 hdr = PAGE_ID_NIL;
    int rc = vec_hdr_find(pg, &hdr);
    if (rc != OK) {
        free(vs);
        return rc;
    }

    if (hdr != PAGE_ID_NIL) {
        /*
         * 磁盘上已经有向量存储了。
         *
         * 维度是它的身份：改了维度，每页放几个向量、每个向量多长都变了，
         * 旧数据没法解释。所以维度不同直接拒绝，不覆盖。
         */
        PageFrame *fr = NULL;
        rc = pager_get(pg, hdr, &fr);
        if (rc != OK) {
            free(vs);
            return rc;
        }
        u16 old_dim = get_u16(fr->data + VECH_H_DIM);
        pager_unpin(pg, hdr, false);

        if (old_dim != dim) {
            free(vs);
            return E_INVAL;
        }
        vs->hdr_page = hdr;
    } else {
        rc = vec_hdr_create(vs, 0, dim, (u16)VEC_K(dim), &vs->hdr_page);
        if (rc != OK) {
            free(vs);
            return rc;
        }
    }

    rc = vec_attach(vs);
    if (rc != OK) {
        vstore_destroy(vs);
        return rc;
    }

    *out = vs;
    return OK;
}

int vstore_open(Pager *pg, LogHookFn fn, void *ctx, VStore **out)
{
    if (!pg || !out)
        return E_INVAL;

    u32 hdr = PAGE_ID_NIL;
    int rc = vec_hdr_find(pg, &hdr);
    if (rc != OK)
        return rc;
    if (hdr == PAGE_ID_NIL)
        return E_NOTFOUND;      /* 这个库还没有向量存储 */

    VStore *vs = vec_alloc();
    if (!vs)
        return E_IO;

    vs->pg = pg;
    vs->wal = fn;
    vs->wal_ctx = ctx;
    vs->hdr_page = hdr;

    rc = vec_attach(vs);
    if (rc != OK) {
        vstore_destroy(vs);
        return rc;
    }

    *out = vs;
    return OK;
}

void vstore_destroy(VStore *vs)
{
    if (!vs)
        return;
    gstore_destroy(vs->meta);
    free(vs->pages);
    free(vs->meta_rids);
    free(vs);
}

/* ---------------- 写 ---------------- */

int vstore_insert(VStore *vs, u32 xid, const float *vec, i32 label,
                  u64 *row_id_out)
{
    if (!vs || !vec)
        return E_INVAL;

    u64 row_id = vs->count;
    u32 ordinal = (u32)(row_id / vs->k);
    u16 slot = (u16)(row_id % vs->k);

    /* 序号超出现有页数 → 开新页 */
    if (ordinal >= vs->npages) {
        if (ordinal != vs->npages)
            return E_CORRUPT;    /* 只能一页一页往后长 */
        int rc = vec_new_page(vs, xid);
        if (rc != OK)
            return rc;
    }

    /* 1) 写向量 */
    struct put_arg a;
    a.vec = vec;
    a.slot = slot;
    a.dim = vs->dim;

    int rc = vec_write_page(vs, xid, vs->pages[ordinal], vec_put_mut, &a);
    if (rc != OK)
        return rc;

    /*
     * 2) 写标量行。
     *
     * 先写向量后写标量：如果崩在中间，数据页多一个向量、标量表少一行 ——
     * 打开时 vec_rebuild_chain 的 sum 与 count 对不上 → E_CORRUPT。
     * 反过来（先写标量）也是同样的结果。
     *
     * 两个都不会"静默错位"：这正是 §"列对齐"要的。
     */
    u8 rec[VEC_META_REC];
    memset(rec, 0, sizeof(rec));
    put_u64(rec + GS_REC_HDR_SIZE, row_id);
    put_u32(rec + GS_REC_HDR_SIZE + 8, (u32)label);   /* i32 按补码存 */

    Rid rid;
    rc = gstore_alloc_with(vs->meta, xid, rec, VEC_META_REC, &rid);
    if (rc != OK)
        return rc;

    /* 3) 更新内存 */
    rc = vec_grow((void **)&vs->meta_rids, &vs->cap_rids,
                  (u32)(row_id + 1), sizeof(Rid));
    if (rc != OK)
        return rc;
    vs->meta_rids[row_id] = rid;

    vs->chain_sum++;
    vs->count++;

    if (row_id_out)
        *row_id_out = row_id;
    return OK;
}
