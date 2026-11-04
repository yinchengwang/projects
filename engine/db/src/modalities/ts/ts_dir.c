/*
 * ts_dir.c —— 时序模态的**分区目录持久化**（v1.5 步骤 2 · T5）
 *
 * 契约（磁盘格式 + API + 选型理由 + 坑）见 ts_dir.h；这里只补实现说明。
 * 分工：本层只管目录的**存与取**，路由与写分区是 tsstore.c 的事。
 *
 * 内存镜像：items[]（目录项，磁盘序）+ item_page[]/item_slot[]（每项在
 * 哪张页的第几个槽位 —— 为了就地改写时 O(1) 定位）+ sorted[]（按时间升序）。
 *
 * 写顺序（硬要求）：**先数据后水位线** —— 先写尾目录页，再写 meta 页。
 * 断电只可能停在两步之间 ⇒ 链上条数比 meta 记的多 ⇒ 开库以链为准（M6-24）。
 *
 * 扩页：尾页满了 ⇒ 新分配一张目录页接在链尾（新页 TSD_PREV = 老尾页号，
 * 老尾页 TSD_NEXT = 新页号 —— **老页被改了一次**）。⚠️ 老页被改 ⇒ 断电
 * 可能把它写坏 ⇒ E_CORRUPT。这是"没有 WAL"的代价（M6-23）。
 */
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_dir.h"
#include "storage/page.h"
#include "storage/pager.h"

/* 目录链的长度上限（防环）：一百万张目录页 = 三千多万个分区 */
#define TS_DIR_MAX_PAGES 1048576u

/* 目录项总数上限（"条数离谱"的判据） */
#define TS_DIR_MAX_PARTS 16777216u

/* 内存数组的初始容量 */
#define TS_DIR_INIT_CAP 64u

struct TsDir {
    Pager *pg;

    u32 meta_page;   /* meta 页页号 */
    u32 dir_head;    /* 目录链首页 */
    u32 dir_tail;    /* 目录链尾页 */
    u32 span_sec;    /* 分区窗口（秒） */

    u32 n_parts;      /* 目录项个数 */
    u32 n_dir_pages;  /* 目录页数 */
    u32 meta_nparts;  /* meta 页里的水位线 */

    TsPartInfo *items;   /* 目录项（磁盘序） */
    u32 *item_page;      /* 每项所在目录页 */
    u16 *item_slot;      /* 每项在页内的槽位号 */
    u32 *sorted;         /* 按 start_ms 升序的下标 */
    u32 cap;             /* 上面四个数组的容量 */
    u64 page_reads;      /* 累计逻辑页读 */
};

/*
 * 位模式互转（memcpy，避开有符号↔无符号转换的实现定义行为）。
 */
static i64 tsdir_u64_to_i64(u64 v)
{
    i64 r;

    memcpy(&r, &v, sizeof(r));
    return r;
}

static void ent_encode(u8 *p, const TsPartInfo *it)
{
    put_u64(p + TSE_START, (u64)it->start_ms);
    put_u64(p + TSE_END, (u64)it->end_ms);
    put_u32(p + TSE_STATE, (u32)it->state);
    put_u32(p + TSE_ROWS, it->n_rows);
    put_u32(p + TSE_FIRST, it->first_page);
    put_u32(p + TSE_PAGES, it->page_count);
}

static void ent_decode(const u8 *p, TsPartInfo *it)
{
    it->start_ms = tsdir_u64_to_i64(get_u64(p + TSE_START));
    it->end_ms = tsdir_u64_to_i64(get_u64(p + TSE_END));
    it->state = (TsPartState)get_u32(p + TSE_STATE);
    it->n_rows = get_u32(p + TSE_ROWS);
    it->first_page = get_u32(p + TSE_FIRST);
    it->page_count = get_u32(p + TSE_PAGES);
}

/*
 * 目录项的不变量（坑 M6-25）：写入前查一遍（挡调用方的错），
 * 读出后再查一遍（挡坏数据）—— 两条路径用**同一份**判据。
 */static int part_valid(const TsPartInfo *it)
{
    if (it->start_ms >= it->end_ms) {
        return 0;   /* 时间窗必须非空且不倒挂 */
    }
    if ((u32)it->state > (u32)TS_PART_DROPPED) {
        return 0;   /* 状态只能是那三个 */
    }
    if (it->page_count == 0u) {
        return (it->first_page == PAGE_ID_NIL) ? 1 : 0;
    }
    return (it->first_page != PAGE_ID_NIL) ? 1 : 0;
}

/* ---------------- 内存数组 ---------------- */

/* 四个数组同步扩容（realloc 失败 ⇒ E_NOSPACE） */
#define GROW(field, type) do { \
    p = realloc(d->field, (size_t)ncap * sizeof(type)); \
    if (p == NULL) { \
        return E_NOSPACE; \
    } \
    d->field = (type *)p; \
} while (0)

static int dir_reserve(TsDir *d, u32 need)
{
    u32 ncap;
    void *p;

    if (need <= d->cap) {
        return OK;
    }
    ncap = (d->cap > 0u) ? d->cap : TS_DIR_INIT_CAP;
    while (ncap < need) {
        ncap *= 2u;
    }

    GROW(items, TsPartInfo);
    GROW(item_page, u32);
    GROW(item_slot, u16);
    GROW(sorted, u32);

    d->cap = ncap;
    return OK;
}

/*
 * 把下标 idx 插进排序数组（按 start_ms 升序）。
 *
 * n_existing —— 排序数组里**已经排好**的项数（不含 idx 自己）。
 * 一个时间窗只有一个分区 ⇒ start_ms 不会重复；真重复就按"新的排后面"。
 */
static void sorted_insert(TsDir *d, u32 idx, u32 n_existing)
{
    u32 lo = 0;
    u32 hi = n_existing;
    i64 key = d->items[idx].start_ms;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        if (d->items[d->sorted[mid]].start_ms <= key) {
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (lo < n_existing) {
        memmove(&d->sorted[lo + 1u], &d->sorted[lo],
                (size_t)(n_existing - lo) * sizeof(*d->sorted));
    }
    d->sorted[lo] = idx;
}

/* ---------------- meta 页 ---------------- */

/*
 * 把 meta 页写回磁盘（校验和要重算：不重算这页以后就被当成坏页）。
 */
static int meta_store(TsDir *d)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(d->pg, d->meta_page, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;

    put_u32(fr->data + TSM_VER, TS_DIR_META_VER);
    put_u32(fr->data + TSM_SPAN, d->span_sec);
    put_u32(fr->data + TSM_HEAD, d->dir_head);
    put_u32(fr->data + TSM_TAIL, d->dir_tail);
    put_u32(fr->data + TSM_NPARTS, d->n_parts);
    page_set_checksum(fr->data);

    rc = pager_unpin(d->pg, d->meta_page, true);
    if (rc != OK) {
        return rc;
    }
    d->meta_nparts = d->n_parts;
    return OK;
}

/* ---------------- 目录页 ---------------- */

/*
 * 分配一张新页，**避开页 0**（坑 M5-10：页 0 归 catalog 的元数据页，
 * 模态占了它，catalog 之后 bootstrap 会报 E_CORRUPT —— 整库开不起来）。
 *
 * 返回的帧**已经 pin 过**（pager_alloc 的约定），调用方负责 unpin。
 */
static int alloc_page(Pager *pg, u32 *pid_out, PageFrame **fr_out)
{
    PageFrame *fr = NULL;
    u32 pid = 0;
    int rc;

    rc = pager_alloc(pg, &pid, &fr);
    if (rc != OK) {
        return rc;
    }
    if (pid == PAGE_ID_META) {
        u32 pid2 = 0;
        PageFrame *fr2 = NULL;

        (void)pager_unpin(pg, pid, false);
        rc = pager_alloc(pg, &pid2, &fr2);
        if (rc != OK) {
            return rc;
        }
        pid = pid2;
        fr = fr2;
    }
    *pid_out = pid;
    *fr_out = fr;
    return OK;
}

/* 新建一张空的目录页（prev = 上一张目录页，链首时传 PAGE_ID_NIL） */
static int dir_new_page(TsDir *d, u32 prev, u32 *pid_out)
{
    PageFrame *fr = NULL;
    u32 pid = 0;
    int rc;

    rc = alloc_page(d->pg, &pid, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;

    page_init(fr->data, (u8)PT_TS_DIR);
    put_u32(fr->data + TSD_VER, TS_DIR_PAGE_VER);
    put_u32(fr->data + TSD_NENT, 0);
    put_u32(fr->data + TSD_NEXT, PAGE_ID_NIL);
    put_u32(fr->data + TSD_PREV, prev);
    page_set_checksum(fr->data);

    rc = pager_unpin(d->pg, pid, true);
    if (rc != OK) {
        return rc;
    }
    *pid_out = pid;
    return OK;
}

/* 把老尾页的 TSD_NEXT 指到新页，并把它接成新的尾页 */
static int dir_link_new_page(TsDir *d, u32 *new_pid)
{
    PageFrame *fr = NULL;
    u32 pid = 0;
    int rc;

    rc = dir_new_page(d, d->dir_tail, &pid);
    if (rc != OK) {
        return rc;
    }

    /* 老尾页改一行：next 指向新页 */
    rc = pager_get(d->pg, d->dir_tail, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;
    put_u32(fr->data + TSD_NEXT, pid);
    page_set_checksum(fr->data);
    rc = pager_unpin(d->pg, d->dir_tail, true);
    if (rc != OK) {
        return rc;
    }

    d->dir_tail = pid;
    d->n_dir_pages++;
    *new_pid = pid;
    return OK;
}

/* ---------------- 建 / 开 ---------------- */

int ts_dir_create(Pager *pg, u32 span_sec, TsDir **out)
{
    TsDir *d;
    PageFrame *fr = NULL;
    u32 pid = 0;
    int rc;

    if (!pg || !out) {
        return E_INVAL;
    }
    *out = NULL;
    if (span_sec == 0u) {
        return E_INVAL;   /* 窗口为 0 ⇒ 路由无从谈起 */
    }

    rc = alloc_page(pg, &pid, &fr);
    if (rc != OK) {
        return rc;
    }

    page_init(fr->data, (u8)PT_TS_META);
    put_u32(fr->data + TSM_VER, TS_DIR_META_VER);
    put_u32(fr->data + TSM_SPAN, span_sec);
    put_u32(fr->data + TSM_HEAD, PAGE_ID_NIL);
    put_u32(fr->data + TSM_TAIL, PAGE_ID_NIL);
    put_u32(fr->data + TSM_NPARTS, 0);
    page_set_checksum(fr->data);
    rc = pager_unpin(pg, pid, true);
    if (rc != OK) {
        return rc;
    }

    d = (TsDir *)calloc(1, sizeof(TsDir));
    if (d == NULL) {
        return E_NOSPACE;
    }
    d->pg = pg;
    d->meta_page = pid;
    d->dir_head = PAGE_ID_NIL;
    d->dir_tail = PAGE_ID_NIL;
    d->span_sec = span_sec;
    d->page_reads = 1;   /* 刚才分配并写了一页 */

    *out = d;
    return OK;
}

/*
 * 扫页空间找 meta 页（坑 M6-22）。
 * 找到就停 ⇒ 通常只扫几页（meta 页在文件靠前处）；最坏才 O(npages)。
 */
static int find_meta_page(Pager *pg, u32 *pid_out, u64 *reads)
{
    u64 npages = pager_npages(pg);
    u32 pid;
    int rc;

    *pid_out = PAGE_ID_NIL;
    for (pid = 0; (u64)pid < npages; pid++) {
        PageFrame *fr = NULL;

        rc = pager_get(pg, pid, &fr);
        if (rc != OK) {
            return rc;
        }
        (*reads)++;
        if (fr->data[H_TYPE] == (u8)PT_TS_META) {
            (void)pager_unpin(pg, pid, false);
            *pid_out = pid;
            return OK;
        }
        (void)pager_unpin(pg, pid, false);
    }
    return OK;
}

/*
 * 读一遍目录链，把条目装进内存。
 *
 * 【坏在哪都算坏：一条都不给】页类型不对 / 校验和不过 / 版本不认识 /
 * 条数离谱 / 链不连贯 —— 任何一种都返回 E_CORRUPT，**不返回读了一半的
 * 目录**（半截目录会让"查这段时间"漏数据，且看不出来）。
 */
static int load_chain(TsDir *d)
{
    u32 pid = d->dir_head;
    u32 prev = PAGE_ID_NIL;
    u32 total = 0;
    u32 npages = 0;
    int rc;

    while (pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 nent;
        u32 next;
        u32 prv;
        u32 i;

        rc = pager_get(d->pg, pid, &fr);
        if (rc != OK) {
            return rc;
        }
        d->page_reads++;

        nent = get_u32(fr->data + TSD_NENT);
        next = get_u32(fr->data + TSD_NEXT);
        prv = get_u32(fr->data + TSD_PREV);

        if (fr->data[H_TYPE] != (u8)PT_TS_DIR ||
            !page_verify_checksum(fr->data) ||
            get_u32(fr->data + TSD_VER) != TS_DIR_PAGE_VER ||
            nent > TS_DIR_MAX_ENT ||            /* 条数离谱 */
            prv != prev) {                      /* 链不连贯 */
            (void)pager_unpin(d->pg, pid, false);
            return E_CORRUPT;
        }

        rc = dir_reserve(d, d->n_parts + nent);
        if (rc != OK) {
            (void)pager_unpin(d->pg, pid, false);
            return rc;
        }
        for (i = 0; i < nent; i++) {
            TsPartInfo it;

            ent_decode(fr->data + TSD_DATA + (u32)i * TS_DIR_ENT_SIZE, &it);
            if (!part_valid(&it)) {
                (void)pager_unpin(d->pg, pid, false);
                return E_CORRUPT;   /* 坏数据：假分区 */
            }
            d->items[d->n_parts] = it;
            d->item_page[d->n_parts] = pid;
            d->item_slot[d->n_parts] = (u16)i;
            d->n_parts++;
            sorted_insert(d, d->n_parts - 1u, d->n_parts - 1u);
        }
        (void)pager_unpin(d->pg, pid, false);

        total += nent;
        prev = pid;
        pid = next;
        npages++;
        if (npages > TS_DIR_MAX_PAGES) {
            return E_CORRUPT;   /* 链成环了 */
        }
        if (d->n_parts > TS_DIR_MAX_PARTS) {
            return E_CORRUPT;   /* 条数离谱 */
        }
    }

    d->dir_tail = prev;
    d->n_dir_pages = npages;

    /*
     * 坑 M6-24：水位线校验。写顺序是"先目录页、后 meta 页"，
     * 所以链上条数只会 **≥** 水位线；反过来（链上条数 < 水位线）
     * 说明**目录页丢了** ⇒ E_CORRUPT。没有这一条，链断在半路时
     * 开库会"成功地"返回一个少了几个分区的目录 —— 那是静默漏数据。
     */
    if (total < d->meta_nparts) {
        return E_CORRUPT;
    }
    return OK;
}

int ts_dir_open_hint(Pager *pg, u32 meta_page, TsDir **out)
{
    TsDir *d;
    PageFrame *fr = NULL;
    int rc;

    if (!pg || !out) {
        return E_INVAL;
    }
    *out = NULL;

    rc = pager_get(pg, meta_page, &fr);
    if (rc != OK) {
        return rc;   /* 页号不对 → E_NOTFOUND */
    }
    if (fr->data[H_TYPE] != (u8)PT_TS_META ||
        !page_verify_checksum(fr->data) ||
        get_u32(fr->data + TSM_VER) != TS_DIR_META_VER) {
        /* 类型不对 / 校验和不过 / 版本不认识 ⇒ 不能猜着读 */
        (void)pager_unpin(pg, meta_page, false);
        return E_CORRUPT;
    }

    d = (TsDir *)calloc(1, sizeof(TsDir));
    if (d == NULL) {
        (void)pager_unpin(pg, meta_page, false);
        return E_NOSPACE;
    }
    d->pg = pg;
    d->meta_page = meta_page;
    d->span_sec = get_u32(fr->data + TSM_SPAN);
    d->dir_head = get_u32(fr->data + TSM_HEAD);
    d->dir_tail = get_u32(fr->data + TSM_TAIL);
    d->meta_nparts = get_u32(fr->data + TSM_NPARTS);
    d->page_reads = 1;
    (void)pager_unpin(pg, meta_page, false);

    rc = load_chain(d);
    if (rc != OK) {
        ts_dir_destroy(d);
        return rc;
    }

    *out = d;
    return OK;
}

int ts_dir_open(Pager *pg, TsDir **out)
{
    u32 meta = PAGE_ID_NIL;
    u64 reads = 0;
    int rc;

    if (!pg || !out) {
        return E_INVAL;
    }
    *out = NULL;

    rc = find_meta_page(pg, &meta, &reads);
    if (rc != OK) {
        return rc;
    }
    if (meta == PAGE_ID_NIL) {
        /*
         * 磁盘上没有时序目录 ⇒ 这个库还没建过时序存储。
         * 这**不是错误**（与 geo / graph 同一个口径）。
         */
        return E_NOTFOUND;
    }

    rc = ts_dir_open_hint(pg, meta, out);
    if (rc == OK) {
        (*out)->page_reads += reads;
    }
    return rc;
}

void ts_dir_destroy(TsDir *d)
{
    if (!d) {
        return;
    }
    free(d->items);
    free(d->item_page);
    free(d->item_slot);
    free(d->sorted);
    free(d);
}

/* ---------------- 追加 / 更新 ---------------- */

int ts_dir_append(TsDir *d, const TsPartInfo *info)
{
    PageFrame *fr = NULL;
    u32 cur;
    u32 nent;
    int rc;

    if (!d || !info) {
        return E_INVAL;
    }
    if (!part_valid(info)) {
        return E_INVAL;
    }

    rc = dir_reserve(d, d->n_parts + 1u);
    if (rc != OK) {
        return rc;
    }

    /* 还没有任何目录页 ⇒ 先建一张 */
    if (d->dir_tail == PAGE_ID_NIL) {
        u32 pid = 0;

        rc = dir_new_page(d, PAGE_ID_NIL, &pid);
        if (rc != OK) {
            return rc;
        }
        d->dir_head = pid;
        d->dir_tail = pid;
        d->n_dir_pages++;
    }

    cur = d->dir_tail;
    rc = pager_get(d->pg, cur, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;
    nent = get_u32(fr->data + TSD_NENT);
    (void)pager_unpin(d->pg, cur, false);

    if (nent >= TS_DIR_MAX_ENT) {
        /* 尾页满了 ⇒ 自动扩页（绝不静默丢分区） */
        u32 pid = 0;

        rc = dir_link_new_page(d, &pid);
        if (rc != OK) {
            return rc;
        }
        cur = pid;
        nent = 0;
    }

    /* 写条目 */
    rc = pager_get(d->pg, cur, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;
    ent_encode(fr->data + TSD_DATA + nent * TS_DIR_ENT_SIZE, info);
    put_u32(fr->data + TSD_NENT, nent + 1u);
    page_set_checksum(fr->data);
    rc = pager_unpin(d->pg, cur, true);
    if (rc != OK) {
        return rc;
    }

    d->items[d->n_parts] = *info;
    d->item_page[d->n_parts] = cur;
    d->item_slot[d->n_parts] = (u16)nent;
    d->n_parts++;
    sorted_insert(d, d->n_parts - 1u, d->n_parts - 1u);

    /* 最后更新 meta（先数据后水位线，坑 M6-24） */
    return meta_store(d);
}

int ts_dir_update(TsDir *d, u32 idx, const TsPartInfo *info)
{
    PageFrame *fr = NULL;
    u32 pid;
    u16 slot;
    int rc;

    if (!d || !info) {
        return E_INVAL;
    }
    if (idx >= d->n_parts) {
        return E_INVAL;
    }
    if (!part_valid(info)) {
        return E_INVAL;
    }

    pid = d->item_page[idx];
    slot = d->item_slot[idx];

    rc = pager_get(d->pg, pid, &fr);
    if (rc != OK) {
        return rc;
    }
    d->page_reads++;
    ent_encode(fr->data + TSD_DATA + (u32)slot * TS_DIR_ENT_SIZE, info);
    page_set_checksum(fr->data);
    rc = pager_unpin(d->pg, pid, true);
    if (rc != OK) {
        return rc;
    }

    if (d->items[idx].start_ms != info->start_ms) {
        /* 时间窗变了 ⇒ 排序位置也要变（罕见路径：重建整个排序数组） */
        u32 i;

        d->items[idx] = *info;
        for (i = 0; i < d->n_parts; i++) {
            d->sorted[i] = i;
        }
        for (i = 1; i < d->n_parts; i++) {
            u32 key = d->sorted[i];
            u32 j = i;

            while (j > 0 &&
                   d->items[d->sorted[j - 1u]].start_ms >
                   d->items[key].start_ms) {
                d->sorted[j] = d->sorted[j - 1u];
                j--;
            }
            d->sorted[j] = key;
        }
    } else {
        d->items[idx] = *info;
    }
    return OK;
}

/* ---------------- 查询 ---------------- */

int ts_dir_get(const TsDir *d, u32 idx, TsPartInfo *out)
{
    if (!d || !out) {
        return E_INVAL;
    }
    if (idx >= d->n_parts) {
        return E_INVAL;
    }
    *out = d->items[idx];
    return OK;
}

int ts_dir_get_by_time(const TsDir *d, u32 i, TsPartInfo *out)
{
    if (!d || !out) {
        return E_INVAL;
    }
    if (i >= d->n_parts) {
        return E_INVAL;
    }
    *out = d->items[d->sorted[i]];
    return OK;
}

int ts_dir_find(const TsDir *d, i64 t_ms, u32 *idx_out, TsPartInfo *out)
{
    u32 lo;
    u32 hi;
    u32 cand;

    if (!d) {
        return E_INVAL;
    }
    if (d->n_parts == 0u) {
        return E_NOTFOUND;
    }

    /* 二分找"起始时间 ≤ t_ms 的最后一项"；没有候选项 ⇒ t_ms 早于所有分区 */
    lo = 0;
    hi = d->n_parts;
    cand = PAGE_ID_NIL;
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2u;
        u32 idx = d->sorted[mid];

        if (d->items[idx].start_ms <= t_ms) {
            cand = idx;
            lo = mid + 1u;
        } else {
            hi = mid;
        }
    }
    if (cand == PAGE_ID_NIL) {
        return E_NOTFOUND;
    }
    if (t_ms >= d->items[cand].end_ms) {
        return E_NOTFOUND;   /* 落在两个分区之间的空档 */
    }
    if (d->items[cand].state == TS_PART_DROPPED) {
        /* 已回收的分区不该再被查到 */
        return E_NOTFOUND;
    }
    if (idx_out) {
        *idx_out = cand;
    }
    if (out) {
        *out = d->items[cand];
    }
    return OK;
}

u32 ts_dir_count(const TsDir *d)
{
    return d ? d->n_parts : 0u;
}

u64 ts_dir_total_rows(const TsDir *d)
{
    u64 total = 0;
    u32 i;

    if (!d) {
        return 0;
    }
    for (i = 0; i < d->n_parts; i++) {
        total += d->items[i].n_rows;
    }
    return total;
}

u32 ts_dir_span_sec(const TsDir *d)
{
    return d ? d->span_sec : 0u;
}

int ts_dir_set_span_sec(TsDir *d, u32 span_sec)
{
    if (!d) {
        return E_INVAL;
    }
    if (span_sec == 0u) {
        return E_INVAL;
    }
    d->span_sec = span_sec;
    return meta_store(d);
}

u32 ts_dir_page_count(const TsDir *d)
{
    return d ? d->n_dir_pages : 0u;
}

u64 ts_dir_page_reads(const TsDir *d)
{
    return d ? d->page_reads : 0;
}
