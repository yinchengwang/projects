/*
 * vindex.c — HNSW 索引的持久化（v1.0 步骤 4）
 *
 * 设计说明见 modalities/vector/vindex.h。
 *
 * 两个页类型：
 *   PT_VEC_INDEX  头页（dim / m / efc / metric / entry / max_level / count）
 *   PT_VEC_LINK   邻接表（定长槽页，每节点一条记录）—— 走 gstore
 *
 * 复用 gstore 而不是自己写页格式：定长槽页 + 空闲链 + 页链 + 恢复
 * 这套东西 v0.11 已经写对并测透了。索引的"每节点一条定长记录"
 * 正好是它的适用面（v1.0 步骤 1 把它下沉到 storage/ 就是为了这个）。
 */

#include "modalities/vector/vindex.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/vector/hnsw_internal.h"
#include "storage/page.h"

/* 头页字段偏移（12..17 留给 LSN / 校验和） */
enum {
    VIX_H_TYPE   = 0,    /* u8  PT_VEC_INDEX */
    VIX_H_VER    = 1,    /* u8  版本 */
    VIX_H_DIM    = 2,    /* u16 */
    VIX_H_M      = 4,    /* u16 */
    VIX_H_EFC    = 6,    /* u16 */
    VIX_H_METRIC = 8,    /* u8 */
    VIX_H_MAXLV  = 9,    /* u8  入口点的最高层 */
    VIX_H_ENTRY  = 20,   /* u32 入口点 */
    VIX_H_COUNT  = 24    /* u32 已落盘的节点数 */
};

#define VIX_HDR_VER 1u

/* 记录载荷里的字段偏移 */
enum {
    VIR_ID    = 0,   /* u32 节点号 */
    VIR_LEVEL = 4,   /* u8  最高层 */
    VIR_CNT   = 6    /* u16 cnt[0..HNSW_MAX_LEVEL] */
};

/* 载荷里邻居数组的起点 */
#define VIR_LINKS (VIR_CNT + ((u32)HNSW_MAX_LEVEL + 1u) * 2u)

/* 栈上记录缓冲的上限（m 最大 32 → 918 字节） */
#define VIX_REC_MAX 1024u

struct VIndex {
    Pager     *pg;
    LogHookFn  wal;
    void      *wal_ctx;

    u32 hdr_page;
    u16 dim;
    u16 m;
    u16 efc;
    u8  metric;

    GStore *links;      /* 每节点一条定长记录 */
    u32     count;      /* 已落盘的节点数 */
    u32     cap_rids;
    Rid    *rids;       /* 节点号 → 记录 RID */
};

/* ---------------- 头页 ---------------- */

static int vix_hdr_find(Pager *pg, u32 *out)
{
    *out = PAGE_ID_NIL;

    u64 np = pager_npages(pg);
    for (u64 i = 0; i < np; i++) {
        PageFrame *fr = NULL;
        int rc = pager_get(pg, (u32)i, &fr);
        if (rc != OK)
            return rc;

        bool hit = (fr->data[VIX_H_TYPE] == (u8)PT_VEC_INDEX);
        pager_unpin(pg, (u32)i, false);
        if (hit) {
            *out = (u32)i;
            return OK;
        }
    }
    return OK;
}

/* 头页里除了 entry/max_level/count 之外的身份字段 */
struct vix_id {
    u16 dim;
    u16 m;
    u16 efc;
    u8  metric;
};

static int vix_hdr_read(Pager *pg, u32 page, struct vix_id *id,
                        u32 *entry, u8 *maxlv, u32 *count)
{
    PageFrame *fr = NULL;
    int rc = pager_get(pg, page, &fr);
    if (rc != OK)
        return rc;

    if (fr->data[VIX_H_TYPE] != (u8)PT_VEC_INDEX) {
        pager_unpin(pg, page, false);
        return E_CORRUPT;
    }
    if (fr->data[VIX_H_VER] != (u8)VIX_HDR_VER) {
        pager_unpin(pg, page, false);
        return E_CORRUPT;
    }

    if (id) {
        id->dim = get_u16(fr->data + VIX_H_DIM);
        id->m = get_u16(fr->data + VIX_H_M);
        id->efc = get_u16(fr->data + VIX_H_EFC);
        id->metric = fr->data[VIX_H_METRIC];
    }
    if (entry)
        *entry = get_u32(fr->data + VIX_H_ENTRY);
    if (maxlv)
        *maxlv = fr->data[VIX_H_MAXLV];
    if (count)
        *count = get_u32(fr->data + VIX_H_COUNT);

    pager_unpin(pg, page, false);
    return OK;
}

static int vix_hdr_create(VIndex *ix, u32 xid, u32 *out)
{
    u32 pid = 0;
    PageFrame *fr = NULL;
    int rc = pager_alloc(ix->pg, &pid, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    memset(fr->data, 0, PAGE_SIZE);
    fr->data[VIX_H_TYPE] = (u8)PT_VEC_INDEX;
    fr->data[VIX_H_VER] = (u8)VIX_HDR_VER;
    put_u16(fr->data + VIX_H_DIM, ix->dim);
    put_u16(fr->data + VIX_H_M, ix->m);
    put_u16(fr->data + VIX_H_EFC, ix->efc);
    fr->data[VIX_H_METRIC] = ix->metric;
    put_u32(fr->data + VIX_H_ENTRY, 0);
    put_u32(fr->data + VIX_H_COUNT, 0);

    u32 lsn = 0;
    if (ix->wal) {
        rc = ix->wal(ix->wal_ctx, xid, LOG_PAGEDATA, pid,
                     PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                     before, (u16)PAGE_SIZE, fr->data, (u16)PAGE_SIZE, &lsn);
        if (rc != OK) {
            memcpy(fr->data, before, PAGE_SIZE);
            pager_unpin(ix->pg, pid, false);
            return rc;
        }
    }
    put_u32(fr->data + H_LSN, lsn);
    page_set_checksum(fr->data);
    pager_unpin(ix->pg, pid, true);

    *out = pid;
    return OK;
}

struct vix_hdr_arg {
    u32 entry;
    u8  maxlv;
    u32 count;
};

static int vix_hdr_mut(void *ctx, u8 *pg)
{
    const struct vix_hdr_arg *a = (const struct vix_hdr_arg *)ctx;
    if (pg[VIX_H_TYPE] != (u8)PT_VEC_INDEX)
        return E_CORRUPT;

    put_u32(pg + VIX_H_ENTRY, a->entry);
    pg[VIX_H_MAXLV] = a->maxlv;
    put_u32(pg + VIX_H_COUNT, a->count);
    return OK;
}

static int vix_hdr_write(VIndex *ix, u32 xid, u32 entry, u8 maxlv, u32 count)
{
    PageFrame *fr = NULL;
    int rc = pager_get(ix->pg, ix->hdr_page, &fr);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, fr->data, PAGE_SIZE);

    struct vix_hdr_arg a;
    a.entry = entry;
    a.maxlv = maxlv;
    a.count = count;

    rc = vix_hdr_mut(&a, fr->data);
    if (rc != OK) {
        pager_unpin(ix->pg, ix->hdr_page, false);
        return rc;
    }

    u32 lsn = 0;
    if (ix->wal) {
        rc = ix->wal(ix->wal_ctx, xid, LOG_PAGEDATA, ix->hdr_page,
                     PAGE_SLOT_NONE, LOG_NO_SLOT_OFF,
                     before, (u16)PAGE_SIZE, fr->data, (u16)PAGE_SIZE, &lsn);
        if (rc != OK) {
            memcpy(fr->data, before, PAGE_SIZE);
            pager_unpin(ix->pg, ix->hdr_page, false);
            return rc;
        }
    }
    put_u32(fr->data + H_LSN, lsn);
    page_set_checksum(fr->data);
    pager_unpin(ix->pg, ix->hdr_page, true);
    return OK;
}

/* ---------------- 建 / 开 / 拆 ---------------- */

/*
 * 追加一条 RID 映射，并**自己维护 count**。
 *
 * 【为什么 count 必须在这里加】
 *
 * 第一版让调用方自己 `ix->count++`。重建路径（vix_rebuild_rids）
 * 忘了加 —— 于是 2000 次 push 全写在 rids[0] 上，容量也永远停在 64。
 * 表现是：打开时一切正常（读回走的是游标，不碰这张表），
 * 直到下一次 flush 拿 rids[id] 去原地改写时才崩/报错，
 * 而那时离出错点已经很远了。
 *
 * 教训：**"谁分配谁维护"要贯彻到底** —— 数组和它的长度放在一起管，
 * 调用方只负责给值。
 */
static int vix_rid_push(VIndex *ix, Rid rid)
{
    u32 need = ix->count + 1;

    if (need > ix->cap_rids) {
        u32 nc = ix->cap_rids ? ix->cap_rids * 2u : 64u;
        while (nc < need)
            nc *= 2u;
        Rid *p = (Rid *)realloc(ix->rids, (size_t)nc * sizeof(Rid));
        if (!p)
            return E_IO;
        ix->rids = p;
        ix->cap_rids = nc;
    }

    ix->rids[ix->count] = rid;
    ix->count++;
    return OK;
}

/*
 * 重建"节点号 → 记录 RID"表。
 *
 * 【为什么非重建不可】
 *
 * flush 是"已存在的记录原地改写"，所以它必须知道每个节点那条记录在哪
 * （Rid）。第一版没重建这张表，重开之后 flush 直接解引用空指针 ——
 * 崩溃点就在 `ix->rids[id]`。
 *
 * 顺带做列对齐的断言：第 i 条记录的 id 必须是 i。
 * 错位说明记录被写坏了，返回 E_CORRUPT 让调用方重建，
 * **不要**带着一个错位的图去搜。
 */
static int vix_rebuild_rids(VIndex *ix, u16 rec_size)
{
    GStoreCursor c;
    gstore_cursor_init(ix->links, &c);

    u8 rec[VIX_REC_MAX];
    Rid rid;
    u16 len = 0;
    u32 i = 0;
    int rc;

    while ((rc = gstore_cursor_next(ix->links, &c, &rid, rec, rec_size,
                                    &len)) == OK) {
        if (len != rec_size)
            return E_CORRUPT;

        if (get_u32(rec + GS_REC_HDR_SIZE + VIR_ID) != i)
            return E_CORRUPT;

        rc = vix_rid_push(ix, rid);
        if (rc != OK)
            return rc;
        i++;
    }
    if (rc != E_EOF)
        return rc;

    /* push 自己维护 count，所以这里只做一致性断言 */
    if (ix->count != i)
        return E_CORRUPT;
    return OK;
}

/* 读头页拿 m，然后按 m 算出的记录长度建 gstore */
static int vix_attach(VIndex *ix)
{
    struct vix_id id;
    u32 hdr_count = 0;
    int rc = vix_hdr_read(ix->pg, ix->hdr_page, &id, NULL, NULL, &hdr_count);
    if (rc != OK)
        return rc;

    if (id.dim == 0 || id.m == 0 || id.m > HNSW_MAX_M)
        return E_CORRUPT;

    ix->dim = id.dim;
    ix->m = id.m;
    ix->efc = id.efc;
    ix->metric = id.metric;

    u16 rec_size = (u16)VINDEX_REC(ix->m);

    rc = gstore_create(ix->pg, ix->wal, ix->wal_ctx, rec_size,
                       PT_VEC_LINK, &ix->links);
    if (rc != OK)
        return rc;
    rc = gstore_recover(ix->links);
    if (rc != OK)
        return rc;

    ix->count = 0;
    rc = vix_rebuild_rids(ix, rec_size);
    if (rc != OK)
        return rc;

    /*
     * 记录数必须和头页里的 count 一致。
     *
     * 不一致说明 flush 崩在中间（写了记录没写头页，或者反过来）——
     * 这时索引不可信，但**不是致命错误**：调用方会走重建路径。
     * 所以这里只把 count 归零，让 vindex_load 去报 E_NOTFOUND。
     */
    if (ix->count != hdr_count)
        ix->count = 0;

    return OK;
}

int vindex_create(Pager *pg, LogHookFn fn, void *ctx,
                  u16 dim, u16 m, u16 efc, u8 metric, VIndex **out)
{
    if (!pg || !out || dim == 0 || m == 0 || m > HNSW_MAX_M)
        return E_INVAL;

    VIndex *ix = (VIndex *)calloc(1, sizeof(VIndex));
    if (!ix)
        return E_IO;

    ix->pg = pg;
    ix->wal = fn;
    ix->wal_ctx = ctx;
    ix->dim = dim;
    ix->m = m;
    ix->efc = efc;
    ix->metric = metric;

    u32 hdr = PAGE_ID_NIL;
    int rc = vix_hdr_find(pg, &hdr);
    if (rc != OK) {
        free(ix);
        return rc;
    }

    if (hdr != PAGE_ID_NIL) {
        /* 已有索引：身份必须一致 */
        struct vix_id id;
        rc = vix_hdr_read(pg, hdr, &id, NULL, NULL, NULL);
        if (rc != OK) {
            free(ix);
            return rc;
        }
        if (id.dim != dim || id.m != m || id.metric != metric) {
            free(ix);
            return E_INVAL;
        }
        ix->hdr_page = hdr;
    } else {
        rc = vix_hdr_create(ix, 0, &ix->hdr_page);
        if (rc != OK) {
            free(ix);
            return rc;
        }
    }

    rc = vix_attach(ix);
    if (rc != OK) {
        vindex_destroy(ix);
        return rc;
    }

    *out = ix;
    return OK;
}

int vindex_open(Pager *pg, LogHookFn fn, void *ctx, VIndex **out)
{
    if (!pg || !out)
        return E_INVAL;

    u32 hdr = PAGE_ID_NIL;
    int rc = vix_hdr_find(pg, &hdr);
    if (rc != OK)
        return rc;
    if (hdr == PAGE_ID_NIL)
        return E_NOTFOUND;

    VIndex *ix = (VIndex *)calloc(1, sizeof(VIndex));
    if (!ix)
        return E_IO;

    ix->pg = pg;
    ix->wal = fn;
    ix->wal_ctx = ctx;
    ix->hdr_page = hdr;

    rc = vix_attach(ix);
    if (rc != OK) {
        vindex_destroy(ix);
        return rc;
    }

    *out = ix;
    return OK;
}

void vindex_destroy(VIndex *ix)
{
    if (!ix)
        return;
    gstore_destroy(ix->links);
    free(ix->rids);
    free(ix);
}

/* ---------------- 落盘 ---------------- */

int vindex_flush(VIndex *ix, u32 xid, const Hnsw *h)
{
    if (!ix || !h)
        return E_INVAL;
    if (hnsw_dim(h) != ix->dim || hnsw_m(h) != ix->m)
        return E_INVAL;

    u32 n = hnsw_count(h);
    u16 rec_size = (u16)VINDEX_REC(ix->m);

    u8 rec[VIX_REC_MAX];
    u32 nb[HNSW_MAX_M * 2];

    if (rec_size > sizeof(rec))
        return E_NOSPACE;

    /*
     * 逐节点写。
     *
     * 已存在的节点**原地改写**（Rid 不变），新的追加 ——
     * 所以反复 flush 不会让记录越堆越多，也不会泄漏页。
     */
    for (u32 id = 0; id < n; id++) {
        u16 lvl = hnsw_level_of(h, id);
        if (lvl > HNSW_MAX_LEVEL)
            return E_CORRUPT;

        memset(rec, 0, rec_size);
        u8 *pay = rec + GS_REC_HDR_SIZE;

        put_u32(pay + VIR_ID, id);
        pay[VIR_LEVEL] = (u8)lvl;

        u32 loff = 0;      /* 邻居区内的写指针（按层顺序） */
        for (u16 lc = 0; lc <= HNSW_MAX_LEVEL; lc++) {
            u16 cnt = 0;

            if (lc <= lvl) {
                u32 got = hnsw_neighbors(h, id, lc, nb, HNSW_MAX_M * 2);
                cnt = (u16)got;
                if (got)
                    memcpy(pay + VIR_LINKS + loff, nb,
                           (size_t)got * sizeof(u32));
            }

            put_u16(pay + VIR_CNT + (u32)lc * 2u, cnt);
            loff += max_links(h, lc) * sizeof(u32);
        }

        if (id < ix->count) {
            int rc = gstore_put(ix->links, xid, ix->rids[id], rec, rec_size);
            if (rc != OK)
                return rc;
        } else {
            Rid rid;
            int rc = gstore_alloc_with(ix->links, xid, rec, rec_size, &rid);
            if (rc != OK)
                return rc;
            rc = vix_rid_push(ix, rid);
            if (rc != OK)
                return rc;
        }
    }

    if (n < ix->count)
        return E_CORRUPT;

    /*
     * 头页最后写。
     *
     * 顺序很重要：先写记录再写头页，所以"头页说 count=N"
     * 一定意味着 N 条记录已经在盘上了。反过来（先写头页）
     * 崩在中间就会留下"头页说有 N 条、实际只有一半"的索引 ——
     * 那正是 vix_attach 里那条校验要拦的，但**不制造它**更好。
     */
    return vix_hdr_write(ix, xid, hnsw_entry(h),
                         (u8)hnsw_max_level(h), n);
}

/* ---------------- 读回 ---------------- */

int vindex_load(VIndex *ix, u32 expect_count, const float *vecs, Hnsw **out)
{
    if (!ix || !vecs || !out)
        return E_INVAL;


    /* 数量对不上 = 索引过期或缺失 → 让调用方重建 */
    if (ix->count != expect_count)
        return E_NOTFOUND;

    Hnsw *h = hnsw_create(ix->dim, (HnswMetric)ix->metric, ix->m, ix->efc,
                          0x5EEDu);
    if (!h)
        return E_IO;

    u16 rec_size = (u16)VINDEX_REC(ix->m);
    u8 rec[VIX_REC_MAX];
    u32 nb[HNSW_MAX_M * 2];
    u16 cnts[HNSW_MAX_LEVEL + 1];

    GStoreCursor c;
    gstore_cursor_init(ix->links, &c);

    Rid rid;
    u16 len = 0;
    u32 i = 0;
    int rc;

    while ((rc = gstore_cursor_next(ix->links, &c, &rid, rec, rec_size,
                                    &len)) == OK) {
        if (len != rec_size) {
            hnsw_destroy(h);
            return E_CORRUPT;
        }

        const u8 *pay = rec + GS_REC_HDR_SIZE;

        /* 列对齐的防线：第 i 条记录必须是第 i 个节点 */
        if (get_u32(pay + VIR_ID) != i) {
            hnsw_destroy(h);
            return E_CORRUPT;
        }

        u16 lvl = pay[VIR_LEVEL];
        if (lvl > HNSW_MAX_LEVEL) {
            hnsw_destroy(h);
            return E_CORRUPT;
        }

        memset(nb, 0, sizeof(nb));
        u32 loff = 0;
        for (u16 lc = 0; lc <= HNSW_MAX_LEVEL; lc++) {
            cnts[lc] = get_u16(pay + VIR_CNT + (u32)lc * 2u);
            if (cnts[lc] > max_links(h, lc)) {
                hnsw_destroy(h);
                return E_CORRUPT;
            }
            if (cnts[lc])
                memcpy(nb + loff / sizeof(u32),
                       pay + VIR_LINKS + loff,
                       (size_t)cnts[lc] * sizeof(u32));
            loff += max_links(h, lc) * sizeof(u32);
        }

        rc = hnsw_append_raw(h, vecs + (size_t)i * ix->dim, lvl, cnts, nb);
        if (rc != OK) {
            hnsw_destroy(h);
            return rc;
        }
        i++;
    }

    if (rc != E_EOF) {
        hnsw_destroy(h);
        return rc;
    }
    if (i != expect_count) {
        hnsw_destroy(h);
        return E_CORRUPT;
    }

    /* 入口点与最高层 */
    u32 entry = 0;
    u8 maxlv = 0;
    rc = vix_hdr_read(ix->pg, ix->hdr_page, NULL, &entry, &maxlv, NULL);
    if (rc != OK) {
        hnsw_destroy(h);
        return rc;
    }
    rc = hnsw_set_entry(h, entry, (u16)maxlv);
    if (rc != OK) {
        hnsw_destroy(h);
        return rc;
    }

    *out = h;
    return OK;
}

/* ---------------- 只读访问 ---------------- */

u32 vindex_count(const VIndex *ix) { return ix ? ix->count : 0; }
u16 vindex_m(const VIndex *ix)     { return ix ? ix->m : 0; }
u16 vindex_dim(const VIndex *ix)   { return ix ? ix->dim : 0; }
