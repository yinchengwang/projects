/*
 * ts_column.c —— 时序分区的页格式与读路径（v1.5 步骤 3）
 *
 * 契约见 ts_column.h。这里补实现层面的说明与踩过的坑。
 *
 * 【这一层做两件事】
 *   1. 裸写分区页（活跃分区）的读路径：走页链、逐行回调、整段读出。
 *   2. 列式页（封窗分区）的写与读：三列各自编码进数据页，头部页记元数据。
 *
 * 【为什么裸写页的读也放在这里】
 *
 * 封窗要把一个裸写分区整体读出来再编码 —— 那就是"读裸写页"的活。
 * 读路径与写路径必须对页内布局有**完全一致的理解**，所以两者放一层，
 * 格式常量也只在这里定义一份（两处各写一份 ⇒ 迟早有一处偏移写错）。
 *
 * 【坑 M6-39：段头必须记条数】
 *
 * dod / rle / gorilla 三个编码的流里**没有条数字段**。
 * 解码时我们按"头部页记的条数"去要数据：
 *   - 流比要的短 ⇒ 解码器读越界 ⇒ E_CORRUPT（dod/rle/gorilla 都会报错）；
 *   - 流比要的長 ⇒ rle / gorilla 会发现"流没读完" ⇒ E_CORRUPT；
 *     （dod 的契约是忽略尾部，dod 这一列单独发现不了 —— 但三列里
 *      只要有一列发现不了就等于没发现，所以另外两列会抓住。）
 * 所以"头部页记的条数"是**唯一的条数权威**，编码器不许自己数。
 *
 * 【坑 M6-40：写页的顺序是"先写数据页、后写头部页"】
 *
 * 头部页是整个列式分区的**入口**：读路径先读它，再按它记的页号去读数据页。
 * 如果先写头部页再写数据页，崩在中间 ⇒ 头部页指着还没写的数据页 ⇒
 * 读出来是**半截**（而且不报错 —— 那些页的校验和恰好过，因为是零页）。
 * 反过来（先写完数据页，最后写头部页）崩在中间 ⇒ 头部页还是旧的 ⇒
 * 读路径根本不会去读那些页 ⇒ 看到的是**完整的旧分区**。
 *
 * 这就是封窗原子性的全部秘密：列式页要么整体可见，要么整体不可见。
 *
 * 【坑 M6-41：页链的 next 要在页写完之后再补】
 *
 * 链的尾页号是"下一页分配出来才知道"的。所以分两步：
 * 先把一页页的数据写好（各自 unpin），再回头把链串起来。
 * 中间崩了 ⇒ 留下一堆孤儿页（浪费空间，不丢数据）——
 * 这是可以接受的失败模式（与坑 M6-29 同一个取舍）。
 *
 * 【坑 M6-42：列式页的页数不能靠"链有多长"来定】
 *
 * 头部页里必须记每列的**字节数**和**页数**：
 * 读的时候按字节数往缓冲里填，填超了 ⇒ E_CORRUPT。
 * 只记页数的话，"最后一页只有 3 个字节"这种信息就丢了，
 * 解码会拿到多余的垃圾字节（可能刚好解出"看起来对"的值）。
 */

#include "modalities/ts/ts_column.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/ts/dod.h"
#include "modalities/ts/gorilla.h"
#include "modalities/ts/rle.h"
#include "storage/page.h"

/* ---------------- 位模式互转（坑 M6-33） ---------------- */

static i64 tsc_i64_of(u64 v)
{
    i64 r;

    memcpy(&r, &v, sizeof(r));
    return r;
}

static double tsc_double_of(u64 bits)
{
    double d;

    memcpy(&d, &bits, sizeof(d));
    return d;
}

/* ---------------- 裸写分区页 ---------------- */

int ts_part_walk(Pager *pg, u32 first_page, u32 *rows_out, u32 *pages_out,
                 u32 *last_out, u64 *reads)
{
    u32 page = first_page;
    u32 rows = 0;
    u32 pages = 0;
    u32 last = PAGE_ID_NIL;

    if (!pg || !rows_out || !pages_out || !last_out)
        return E_INVAL;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 n;
        u32 next;
        int rc;

        rc = pager_get(pg, page, &fr);
        if (rc != OK)
            return rc;
        if (reads)
            (*reads)++;

        if (fr->data[H_TYPE] != (u8)PT_TS_PART ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }
        n = get_u32(fr->data + TSSP_NROWS);
        if (n > TS_ROWS_PER_PAGE) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }

        rows += n;
        pages++;
        last = page;
        next = get_u32(fr->data + TSSP_NEXT);
        rc = pager_unpin(pg, page, false);
        if (rc != OK)
            return rc;
        page = next;
    }

    *rows_out = rows;
    *pages_out = pages;
    *last_out = last;
    return OK;
}

int ts_part_scan(Pager *pg, u32 first_page, TsReadFn fn, void *ctx, u64 *reads)
{
    u32 page = first_page;

    if (!pg || !fn)
        return E_INVAL;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 n;
        u32 next;
        u32 j;
        int rc;

        rc = pager_get(pg, page, &fr);
        if (rc != OK)
            return rc;
        if (reads)
            (*reads)++;

        if (fr->data[H_TYPE] != (u8)PT_TS_PART ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }
        n = get_u32(fr->data + TSSP_NROWS);
        if (n > TS_ROWS_PER_PAGE) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }

        for (j = 0; j < n; j++) {
            TsReading rr;
            const u8 *row = fr->data + TSSP_DATA + (size_t)j * TS_ROW_SIZE;

            rr.time = tsc_i64_of(get_u64(row + 0));
            rr.tag = tsc_i64_of(get_u64(row + 8));
            rr.value = tsc_double_of(get_u64(row + 16));

            rc = fn(&rr, ctx);
            if (rc != OK) {
                (void)pager_unpin(pg, page, false);
                return rc;  /* 回调要求停止：原样返回它的值 */
            }
        }

        next = get_u32(fr->data + TSSP_NEXT);
        rc = pager_unpin(pg, page, false);
        if (rc != OK)
            return rc;
        page = next;
    }
    return OK;
}

int ts_part_read_rows(Pager *pg, u32 first_page, u32 max_rows,
                      i64 *ts, i64 *tag, double *val, u32 *rows_out, u64 *reads)
{
    u32 rows = 0;
    u32 pages = 0;
    u32 last = PAGE_ID_NIL;
    u32 page = first_page;
    u32 idx = 0;
    int rc;

    if (!pg || !rows_out)
        return E_INVAL;
    if (max_rows > 0 && (!ts || !tag || !val))
        return E_INVAL;

    /* 先数一遍：分区实际有多少行 */
    rc = ts_part_walk(pg, first_page, &rows, &pages, &last, reads);
    if (rc != OK)
        return rc;

    /*
     * 调用方给的 max_rows 必须装得下 —— 装不下说明它手里的行数不对，
     * 与"段头必须记条数"是同一个原则（坑 M6-39）。
     */
    if (rows > max_rows)
        return E_CORRUPT;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 n;
        u32 next;
        u32 j;
        int rc2;

        rc2 = pager_get(pg, page, &fr);
        if (rc2 != OK)
            return rc2;
        if (reads)
            (*reads)++;

        if (fr->data[H_TYPE] != (u8)PT_TS_PART ||
            !page_verify_checksum(fr->data)) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }
        n = get_u32(fr->data + TSSP_NROWS);
        if (n > TS_ROWS_PER_PAGE) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }

        for (j = 0; j < n; j++) {
            const u8 *row = fr->data + TSSP_DATA + (size_t)j * TS_ROW_SIZE;

            ts[idx] = tsc_i64_of(get_u64(row + 0));
            tag[idx] = tsc_i64_of(get_u64(row + 8));
            val[idx] = tsc_double_of(get_u64(row + 16));
            idx++;
        }

        next = get_u32(fr->data + TSSP_NEXT);
        rc2 = pager_unpin(pg, page, false);
        if (rc2 != OK)
            return rc2;
        page = next;
    }

    *rows_out = idx;
    return OK;
}

/* ---------------- 列式页：头部 ---------------- */

/* 头部页里记的东西 */
typedef struct {
    u32 n_rows;    /* 分区行数 */
    u32 npages;    /* 数据页总数（不含头部页） */
    u32 len[TSCOL_NCOL];    /* 每列的编码字节数 */
    u32 pages[TSCOL_NCOL];  /* 每列的页数 */
    u32 first[TSCOL_NCOL];  /* 每列的首页页号 */
    u32 count[TSCOL_NCOL];  /* 每列的条数（坑 M6-39） */
    u32 old_first; /* 被换掉的旧裸写页首页 */
    u32 old_pages; /* 被换掉的旧裸写页页数 */
    u32 seal_seq;  /* 封窗序号 */
} ColHdr;

static int col_read_header(Pager *pg, u32 head_page, ColHdr *h, u64 *reads)
{
    PageFrame *fr = NULL;
    int rc;

    if (!pg || !h)
        return E_INVAL;

    rc = pager_get(pg, head_page, &fr);
    if (rc != OK)
        return rc;
    if (reads)
        (*reads)++;

    if (fr->data[H_TYPE] != (u8)PT_TS_COL ||
        !page_verify_checksum(fr->data) ||
        get_u32(fr->data + TSC_FLAGS) != TS_COL_FLAG_HEADER ||
        get_u32(fr->data + TSC_VER) != TS_COL_VER) {
        (void)pager_unpin(pg, head_page, false);
        return E_CORRUPT;
    }

    h->n_rows = get_u32(fr->data + TSC_NROWS);
    h->npages = get_u32(fr->data + TSC_NPAGES);
    h->len[TSCOL_TIME] = get_u32(fr->data + TSC_TLEN);
    h->pages[TSCOL_TIME] = get_u32(fr->data + TSC_TPAGES);
    h->len[TSCOL_TAG] = get_u32(fr->data + TSC_GLEN);
    h->pages[TSCOL_TAG] = get_u32(fr->data + TSC_GPAGES);
    h->len[TSCOL_VALUE] = get_u32(fr->data + TSC_VLEN);
    h->pages[TSCOL_VALUE] = get_u32(fr->data + TSC_VPAGES);
    h->first[TSCOL_TIME] = get_u32(fr->data + TSC_TFIRST);
    h->first[TSCOL_TAG] = get_u32(fr->data + TSC_GFIRST);
    h->first[TSCOL_VALUE] = get_u32(fr->data + TSC_VFIRST);
    h->count[TSCOL_TIME] = get_u32(fr->data + TSC_TCOUNT);
    h->count[TSCOL_TAG] = get_u32(fr->data + TSC_GCOUNT);
    h->count[TSCOL_VALUE] = get_u32(fr->data + TSC_VCOUNT);
    h->old_first = get_u32(fr->data + TSC_OLDFIRST);
    h->old_pages = get_u32(fr->data + TSC_OLDPAGES);
    h->seal_seq = get_u32(fr->data + TSC_SEALSEQ);

    rc = pager_unpin(pg, head_page, false);
    if (rc != OK)
        return rc;

    /* 不变式：三列的条数必须都等于分区行数（坑 M6-39） */
    if (h->count[TSCOL_TIME] != h->n_rows ||
        h->count[TSCOL_TAG] != h->n_rows ||
        h->count[TSCOL_VALUE] != h->n_rows)
        return E_CORRUPT;

    return OK;
}

/* ---------------- 列式页：数据页链 ---------------- */

/*
 * 把一列的编码字节写成一串数据页。
 *
 * 坑 M6-41：链要等页号都拿到了才串得上 ⇒ 写两遍（先写内容，再接链）。
 *
 * 步骤 6：两遍都进 WAL —— 第一遍是新页（整页镜像），
 * 第二遍是就地改写（把 H_NEXT 指到下一页）。
 */
static int col_write_chain(Pager *pg, u32 col, const u8 *buf, u32 len,
                           const TsWal *w, u32 *first_out, u32 *pages_out)
{
    u32 npages = 0;
    u32 *ids = NULL;
    u32 i;
    int rc = OK;

    if (!pg || !first_out || !pages_out)
        return E_INVAL;
    if (len > 0 && !buf)
        return E_INVAL;

    if (len > 0)
        npages = (len + TSC_CAP - 1u) / TSC_CAP;

    if (npages > 0) {
        ids = (u32 *)malloc((size_t)npages * sizeof(u32));
        if (ids == NULL)
            return E_NOSPACE;
    }

    for (i = 0; i < npages; i++) {
        PageFrame *fr = NULL;
        u32 pid = 0;
        u32 off = i * TSC_CAP;
        u32 chunk = len - off;
        u8 before[PAGE_SIZE];

        if (chunk > TSC_CAP)
            chunk = TSC_CAP;

        rc = pager_alloc(pg, &pid, &fr);
        if (rc != OK)
            goto out;

        ts_wal_snap(fr->data, before);   /* 新页的前像（此刻还是全零） */
        page_init(fr->data, (u8)PT_TS_COL);
        put_u32(fr->data + TSC_VER, TS_COL_VER);
        put_u32(fr->data + TSC_FLAGS, TS_COL_FLAG_DATA);
        put_u32(fr->data + TSC_COL, col);
        put_u32(fr->data + TSC_CLEN, chunk);
        put_u32(fr->data + TSC_CCAP, TSC_CAP);
        memcpy(fr->data + TSC_DATA, buf + off, chunk);
        page_set_checksum(fr->data);

        rc = ts_pg_commit(pg, pid, fr, w, before);
        if (rc != OK)
            goto out;

        ids[i] = pid;
    }

    /* 接链：坑 M6-41 */
    for (i = 0; i < npages; i++) {
        PageFrame *fr = NULL;
        u32 next = (i + 1u < npages) ? ids[i + 1u] : PAGE_ID_NIL;
        u8 before[PAGE_SIZE];

        rc = pager_get(pg, ids[i], &fr);
        if (rc != OK)
            goto out;

        ts_wal_snap(fr->data, before);
        put_u32(fr->data + H_NEXT, next);
        page_set_checksum(fr->data);

        rc = ts_pg_commit(pg, ids[i], fr, w, before);
        if (rc != OK)
            goto out;
    }

    *first_out = npages ? ids[0] : PAGE_ID_NIL;
    *pages_out = npages;

out:
    free(ids);
    return rc;
}

/* 把一列的页链读进 buf（长度必须是 h->len[col]） */
static int col_load_chain(Pager *pg, u32 col, const ColHdr *h, u8 *buf, u64 *reads)
{
    u32 page = h->first[col];
    u32 got = 0;
    u32 pages = 0;

    while (page != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 clen;
        u32 next;
        int rc;

        rc = pager_get(pg, page, &fr);
        if (rc != OK)
            return rc;
        if (reads)
            (*reads)++;

        if (fr->data[H_TYPE] != (u8)PT_TS_COL ||
            !page_verify_checksum(fr->data) ||
            get_u32(fr->data + TSC_FLAGS) != TS_COL_FLAG_DATA ||
            get_u32(fr->data + TSC_COL) != col) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }

        clen = get_u32(fr->data + TSC_CLEN);
        if (clen > TSC_CAP || got + clen > h->len[col] ||
            pages + 1u > h->pages[col]) {
            (void)pager_unpin(pg, page, false);
            return E_CORRUPT;
        }

        memcpy(buf + got, fr->data + TSC_DATA, clen);
        got += clen;
        pages++;

        next = get_u32(fr->data + H_NEXT);
        rc = pager_unpin(pg, page, false);
        if (rc != OK)
            return rc;
        page = next;
    }

    /* 页数与字节数都要对上（坑 M6-42） */
    if (pages != h->pages[col] || got != h->len[col])
        return E_CORRUPT;

    return OK;
}

/* ---------------- 列式页：写 ---------------- */

int ts_column_write_wal(Pager *pg, const i64 *ts, const i64 *tag, const double *val,
                         u32 n, u32 old_first, u32 old_pages, u32 seal_seq,
                         const TsWal *w, u32 *head_out, u32 *pages_out)
{
    u8 *bt = NULL;
    u8 *bg = NULL;
    u8 *bv = NULL;
    u32 lt = 0;
    u32 lg = 0;
    u32 lv = 0;
    u32 tf = PAGE_ID_NIL;
    u32 gf = PAGE_ID_NIL;
    u32 vf = PAGE_ID_NIL;
    u32 tp = 0;
    u32 gp = 0;
    u32 vp = 0;
    u32 head = 0;
    PageFrame *fr = NULL;
    u8 before[PAGE_SIZE];
    int rc;

    if (!pg || !head_out || !pages_out)
        return E_INVAL;
    if (n > 0 && (!ts || !tag || !val))
        return E_INVAL;

    /* ---- 编码三列 ---- */

    bt = (u8 *)malloc((size_t)dod_encoded_bound(n) + 1u);
    bg = (u8 *)malloc((size_t)rle_encoded_bound(n) + 1u);
    bv = (u8 *)malloc((size_t)gorilla_encoded_bound(n) + 1u);
    if (bt == NULL || bg == NULL || bv == NULL) {
        rc = E_NOSPACE;
        goto out;
    }

    rc = dod_encode(ts, n, bt, dod_encoded_bound(n));
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    lt = (u32)rc;

    rc = rle_encode(tag, n, bg, rle_encoded_bound(n));
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    lg = (u32)rc;

    rc = gorilla_encode(val, n, bv, gorilla_encoded_bound(n));
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    lv = (u32)rc;

    /* ---- 写数据页 ---- */

    rc = col_write_chain(pg, TSCOL_TIME, bt, lt, w, &tf, &tp);
    if (rc != OK)
        goto out;
    rc = col_write_chain(pg, TSCOL_TAG, bg, lg, w, &gf, &gp);
    if (rc != OK)
        goto out;
    rc = col_write_chain(pg, TSCOL_VALUE, bv, lv, w, &vf, &vp);
    if (rc != OK)
        goto out;

    /* ---- 最后写头部页（坑 M6-40：数据页先写完） ---- */

    rc = pager_alloc(pg, &head, &fr);
    if (rc != OK)
        goto out;

    ts_wal_snap(fr->data, before);   /* 新页的前像（此刻还是全零） */
    page_init(fr->data, (u8)PT_TS_COL);
    put_u32(fr->data + TSC_VER, TS_COL_VER);
    put_u32(fr->data + TSC_FLAGS, TS_COL_FLAG_HEADER);
    put_u32(fr->data + TSC_NROWS, n);
    put_u32(fr->data + TSC_NPAGES, tp + gp + vp);
    put_u32(fr->data + TSC_TLEN, lt);
    put_u32(fr->data + TSC_TPAGES, tp);
    put_u32(fr->data + TSC_GLEN, lg);
    put_u32(fr->data + TSC_GPAGES, gp);
    put_u32(fr->data + TSC_VLEN, lv);
    put_u32(fr->data + TSC_VPAGES, vp);
    put_u32(fr->data + TSC_TFIRST, tf);
    put_u32(fr->data + TSC_GFIRST, gf);
    put_u32(fr->data + TSC_VFIRST, vf);
    put_u32(fr->data + TSC_OLDFIRST, old_first);
    put_u32(fr->data + TSC_OLDPAGES, old_pages);
    put_u32(fr->data + TSC_SEALSEQ, seal_seq);
    put_u32(fr->data + TSC_TCOUNT, n);
    put_u32(fr->data + TSC_GCOUNT, n);
    put_u32(fr->data + TSC_VCOUNT, n);
    page_set_checksum(fr->data);

    rc = ts_pg_commit(pg, head, fr, w, before);
    if (rc != OK)
        goto out;

    *head_out = head;
    *pages_out = 1u + tp + gp + vp;

out:
    free(bt);
    free(bg);
    free(bv);
    return rc;
}

/* 不写日志的版本（单元测试直接用模态时走这条路）。 */
int ts_column_write(Pager *pg, const i64 *ts, const i64 *tag, const double *val,
                    u32 n, u32 old_first, u32 old_pages, u32 seal_seq,
                    u32 *head_out, u32 *pages_out)
{
    return ts_column_write_wal(pg, ts, tag, val, n, old_first, old_pages,
                                seal_seq, NULL, head_out, pages_out);
}

/* ---------------- 列式页：读 ---------------- */

static int col_read_all(Pager *pg, u32 head_page, i64 *ts, i64 *tag, double *val,
                        u32 n, u64 *reads)
{
    ColHdr h;
    u8 *bt = NULL;
    u8 *bg = NULL;
    u8 *bv = NULL;
    int rc;

    rc = col_read_header(pg, head_page, &h, reads);
    if (rc != OK)
        return rc;

    /* 头部页记的条数与调用方要的不一致 ⇒ 坏数据（坑 M6-39） */
    if (h.n_rows != n)
        return E_CORRUPT;

    if (n == 0)
        return OK;

    bt = (u8 *)malloc((size_t)h.len[TSCOL_TIME] + 1u);
    bg = (u8 *)malloc((size_t)h.len[TSCOL_TAG] + 1u);
    bv = (u8 *)malloc((size_t)h.len[TSCOL_VALUE] + 1u);
    if (bt == NULL || bg == NULL || bv == NULL) {
        rc = E_NOSPACE;
        goto out;
    }

    rc = col_load_chain(pg, TSCOL_TIME, &h, bt, reads);
    if (rc != OK)
        goto out;
    rc = col_load_chain(pg, TSCOL_TAG, &h, bg, reads);
    if (rc != OK)
        goto out;
    rc = col_load_chain(pg, TSCOL_VALUE, &h, bv, reads);
    if (rc != OK)
        goto out;

    /*
     * 解码：**按头部页记的条数**去要数据。
     * 解码器返回负数 ⇒ 读坏了；解出来的条数与要的条数不符 ⇒ 也是坏数据。
     */
    rc = dod_decode(bt, h.len[TSCOL_TIME], ts, n);
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    if ((u32)rc != n) {
        rc = E_CORRUPT;
        goto out;
    }

    rc = rle_decode(bg, h.len[TSCOL_TAG], tag, n);
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    if ((u32)rc != n) {
        rc = E_CORRUPT;
        goto out;
    }

    rc = gorilla_decode(bv, h.len[TSCOL_VALUE], val, n);
    if (rc < 0) {
        rc = -rc;
        goto out;
    }
    if ((u32)rc != n) {
        rc = E_CORRUPT;
        goto out;
    }

    rc = OK;

out:
    free(bt);
    free(bg);
    free(bv);
    return rc;
}

int ts_column_read(Pager *pg, u32 head_page, i64 *ts, i64 *tag, double *val, u32 n)
{
    if (!pg)
        return E_INVAL;
    if (n > 0 && (!ts || !tag || !val))
        return E_INVAL;

    return col_read_all(pg, head_page, ts, tag, val, n, NULL);
}

int ts_column_scan(Pager *pg, u32 head_page, u32 n_rows, TsReadFn fn, void *ctx,
                   u64 *reads)
{
    i64 *ts = NULL;
    i64 *tag = NULL;
    double *val = NULL;
    u32 i;
    int rc;

    if (!pg || !fn)
        return E_INVAL;
    if (n_rows == 0)
        return OK;

    ts = (i64 *)malloc((size_t)n_rows * sizeof(i64));
    tag = (i64 *)malloc((size_t)n_rows * sizeof(i64));
    val = (double *)malloc((size_t)n_rows * sizeof(double));
    if (ts == NULL || tag == NULL || val == NULL) {
        rc = E_NOSPACE;
        goto out;
    }

    rc = col_read_all(pg, head_page, ts, tag, val, n_rows, reads);
    if (rc != OK)
        goto out;

    for (i = 0; i < n_rows; i++) {
        TsReading r;

        r.time = ts[i];
        r.tag = tag[i];
        r.value = val[i];

        rc = fn(&r, ctx);
        if (rc != OK)
            goto out;  /* 回调要求停止：原样返回它的值 */
    }

    rc = OK;

out:
    free(ts);
    free(tag);
    free(val);
    return rc;
}

int ts_column_info(Pager *pg, u32 head_page, u32 *n_rows, u32 *n_pages)
{
    ColHdr h;
    int rc;

    if (!pg || !n_rows || !n_pages)
        return E_INVAL;

    rc = col_read_header(pg, head_page, &h, NULL);
    if (rc != OK)
        return rc;

    *n_rows = h.n_rows;
    *n_pages = 1u + h.npages;   /* 头部页也算进去 */
    return OK;
}
