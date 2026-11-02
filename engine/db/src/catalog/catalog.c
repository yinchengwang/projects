/*
 * catalog.c — 元数据
 *
 * 自举的两处硬编码（都不可省略）：
 *   1. 系统表 id：1..4，写死在宏里，绝不查表
 *   2. 系统表 schema：写死在本文件里，因为 sys_column 的行要靠 schema 才能解码
 *
 * 用户表的 schema 存在 sys_column 里，catalog_open 时读出来缓存。
 */

#include "catalog/catalog_priv.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- meta 页布局（从 HDR_SIZE 之后开始） ---------------- */

#define M_MAGIC    (HDR_SIZE + 0u)    /* u32 */
#define M_VERSION  (HDR_SIZE + 4u)    /* u32 */
#define M_CATROOT  (HDR_SIZE + 8u)    /* u32 */
#define M_NEXTTID  (HDR_SIZE + 12u)   /* u32 */
#define M_PAGESZ   (HDR_SIZE + 16u)   /* u32 */
#define M_CRC      (HDR_SIZE + 20u)   /* u32 */

/* ---------------- 系统表 schema（硬编码） ---------------- */

#define N_SYSTABLE_COLS 6
static const ColDesc SYS_TABLE_COLS[N_SYSTABLE_COLS] = {
    { "table_id",  T_INT4, false },
    { "name",      T_TEXT, false },
    { "root_page", T_INT4, false },
    { "ncols",     T_INT4, false },
    { "nrows_est", T_INT8, true  },
    { "flags",     T_INT4, false },
};

#define N_SYSCOLUMN_COLS 5
static const ColDesc SYS_COLUMN_COLS[N_SYSCOLUMN_COLS] = {
    { "table_id", T_INT4, false },
    { "col_no",   T_INT4, false },
    { "name",     T_TEXT, false },
    { "type",     T_INT4, false },
    { "flags",    T_INT4, false },
};

#define N_SYSINDEX_COLS 7
static const ColDesc SYS_INDEX_COLS[N_SYSINDEX_COLS] = {
    { "index_id",  T_INT4, false },
    { "name",      T_TEXT, false },
    { "table_id",  T_INT4, false },
    { "root_page", T_INT4, false },
    { "nkeys",     T_INT4, false },
    { "is_unique", T_INT4, false },
    { "flags",     T_INT4, false },
};

#define N_SYSINDEXCOL_COLS 3
static const ColDesc SYS_INDEXCOL_COLS[N_SYSINDEXCOL_COLS] = {
    { "index_id", T_INT4, false },
    { "seq",      T_INT4, false },
    { "col_no",   T_INT4, false },
};

static const ColDesc *sys_cols_of(u32 table_id, int *ncols)
{
    switch (table_id) {
    case SYS_TABLE_ID:    *ncols = N_SYSTABLE_COLS;    return SYS_TABLE_COLS;
    case SYS_COLUMN_ID:   *ncols = N_SYSCOLUMN_COLS;   return SYS_COLUMN_COLS;
    case SYS_INDEX_ID:    *ncols = N_SYSINDEX_COLS;    return SYS_INDEX_COLS;
    case SYS_INDEXCOL_ID: *ncols = N_SYSINDEXCOL_COLS; return SYS_INDEXCOL_COLS;
    default:              *ncols = 0;                  return NULL;
    }
}

/* ---------------- 内存结构 ---------------- */

/* TableEnt / struct Catalog 在 catalog_priv.h（与 catalog_stats.c 共享） */

TableEnt *catalog_ent_by_id(Catalog *cat, u32 table_id)
{
    for (int i = 0; i < cat->n; i++)
        if (cat->t[i].meta.table_id == table_id)
            return &cat->t[i];
    return NULL;
}

static int ent_add(Catalog *cat, const TableMeta *m, Heap *h, FreeList *fl,
                   const ColDesc *cols, int ncols, Rid meta_rid)
{
    if (cat->n == cat->cap) {
        int ncap = cat->cap ? cat->cap * 2 : 8;
        TableEnt *nt = realloc(cat->t, (size_t)ncap * sizeof(*nt));
        if (!nt)
            return E_IO;
        cat->t = nt;
        cat->cap = ncap;
    }

    TableEnt *e = &cat->t[cat->n];
    memset(e, 0, sizeof(*e));
    e->meta = *m;
    e->heap = h;
    e->meta_rid = meta_rid;
    e->has_rid = !rid_is_nil(meta_rid);
    e->fl = fl;
    e->ncols = ncols;
    if (cols && ncols > 0) {
        e->cols = malloc((size_t)ncols * sizeof(ColDesc));
        if (!e->cols)
            return E_IO;
        memcpy(e->cols, cols, (size_t)ncols * sizeof(ColDesc));
    }
    cat->n++;
    return OK;
}

/* ---------------- meta 页 ---------------- */

/*
 * meta 页写日志。
 *
 * 为什么 meta 页必须写日志：它是整个库的"根指针"。
 * 崩溃恢复后如果 meta 页的内容丢了（比如页头被回滚成初始值），
 * catalog_root 就变成了 NIL，sys_table 找不到 —— 整个库的数据还在页里，
 * 但没人知道它们在哪，等于全丢。
 *
 * 做法：把 meta 页的**整页镜像**写进日志（before/after 各 4KB）。
 * meta 页只有一页、修改不频繁，4KB 的代价可以接受。
 *
 * before 镜像是怎么拿的：先把当前页内容拷出来，再改，再拷一次。
 */
static int meta_write(Catalog *cat, u32 xid)
{
    PageFrame *f;
    int rc = pager_get(cat->pager, PAGE_ID_META, &f);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, f->data, PAGE_SIZE);

    put_u32(f->data + M_CATROOT, cat->catalog_root);
    put_u32(f->data + M_NEXTTID, cat->next_table_id);
    page_set_checksum(f->data);

    u8 after[PAGE_SIZE];
    memcpy(after, f->data, PAGE_SIZE);

    u32 lsn = 0;
    if (cat->wal_fn) {
        rc = cat->wal_fn(cat->wal_ctx, xid, LOG_PAGEDATA, PAGE_ID_META, 0,
                         LOG_NO_SLOT_OFF,
                         before, (u16)PAGE_SIZE, after, (u16)PAGE_SIZE, &lsn);
        if (rc != OK) {
            pager_unpin(cat->pager, PAGE_ID_META, false);
            return rc;
        }
    }

    if (lsn)
        page_set_lsn(f->data, lsn);

    return pager_unpin(cat->pager, PAGE_ID_META, true);
}

/* ---------------- 写系统表行 ---------------- */

/*
 * 把 TableMeta 编码成 sys_table 的一行。
 *
 * 【为什么单独抽出来】
 * 两处要用同一份编码：
 *   1. 建表时写新行（write_table_row → heap_insert）
 *   2. 更新统计时原地重写那一行（catalog_stats.c → heap_write_ver）
 * 编码写两遍的话，字段顺序一变就是"读出来全是错值"。
 */
int catalog_encode_table_row(const TableMeta *m, u8 *buf, u16 cap, u16 *len_out)
{
    Value v[N_SYSTABLE_COLS];
    memset(v, 0, sizeof(v));
    v[0].u.i4 = (i32)m->table_id;
    v[1].u.text.p = (const u8 *)m->name;
    v[1].u.text.n = (u16)strlen(m->name);
    v[2].u.i4 = (i32)m->root_page;
    v[3].u.i4 = (i32)m->ncols;
    /*
     * nrows_est（v1.1 步骤 4 起真正维护）。
     *
     * 原来这里写的是 NULL —— 也就是说这一列**从来没有过值**，
     * 代价模型拿到的永远是 0。留空的原因大概是"还没到用它的版本"，
     * 但代价模型一上来就会读到它，所以必须有人写。
     */
    v[4].u.i8 = m->nrows_est;
    v[5].u.i4 = (i32)m->flags;

    return rec_encode(SYS_TABLE_COLS, N_SYSTABLE_COLS, v, buf, cap, len_out);
}

static int write_table_row(Catalog *cat, u32 xid, const TableMeta *m,
                           Rid *rid_out)
{
    TableEnt *e = catalog_ent_by_id(cat, SYS_TABLE_ID);
    if (!e)
        return E_CORRUPT;

    u8 buf[256];
    u16 len;
    int rc = catalog_encode_table_row(m, buf, sizeof(buf), &len);
    if (rc != OK)
        return rc;

    Rid rid;
    rc = heap_insert(e->heap, xid, buf, len, &rid);
    if (rc != OK)
        return rc;

    if (rid_out)
        *rid_out = rid;
    return OK;
}

static int write_col_rows(Catalog *cat, u32 xid, u32 table_id,
                          const ColDesc *cols, int ncols)
{
    TableEnt *e = catalog_ent_by_id(cat, SYS_COLUMN_ID);
    if (!e)
        return E_CORRUPT;

    for (int i = 0; i < ncols; i++) {
        Value v[N_SYSCOLUMN_COLS];
        memset(v, 0, sizeof(v));
        v[0].u.i4 = (i32)table_id;
        v[1].u.i4 = (i32)i;
        v[2].u.text.p = (const u8 *)cols[i].name;
        v[2].u.text.n = (u16)strlen(cols[i].name);
        v[3].u.i4 = (i32)cols[i].type;
        v[4].u.i4 = cols[i].nullable ? 1 : 0;

        u8 buf[256];
        u16 len;
        int rc = rec_encode(SYS_COLUMN_COLS, N_SYSCOLUMN_COLS, v, buf, sizeof(buf), &len);
        if (rc != OK)
            return rc;

        Rid rid;
        rc = heap_insert(e->heap, xid, buf, len, &rid);
        if (rc != OK)
            return rc;
    }
    return OK;
}

static int write_index_row(Catalog *cat, u32 xid, const IndexMeta *m)
{
    TableEnt *e = catalog_ent_by_id(cat, SYS_INDEX_ID);
    if (!e)
        return E_CORRUPT;

    Value v[N_SYSINDEX_COLS];
    memset(v, 0, sizeof(v));
    v[0].u.i4 = (i32)m->index_id;
    v[1].u.text.p = (const u8 *)m->name;
    v[1].u.text.n = (u16)strlen(m->name);
    v[2].u.i4 = (i32)m->table_id;
    v[3].u.i4 = (i32)m->root_page;
    v[4].u.i4 = 1;                     /* nkeys：单列 */
    v[5].u.i4 = m->is_unique ? 1 : 0;
    v[6].u.i4 = 0;

    u8 buf[256];
    u16 len;
    int rc = rec_encode(SYS_INDEX_COLS, N_SYSINDEX_COLS, v, buf, sizeof(buf), &len);
    if (rc != OK)
        return rc;

    Rid rid;
    return heap_insert(e->heap, xid, buf, len, &rid);
}

static int write_indexcol_row(Catalog *cat, u32 xid, u32 index_id, u32 seq, u32 col_no)
{
    TableEnt *e = catalog_ent_by_id(cat, SYS_INDEXCOL_ID);
    if (!e)
        return E_CORRUPT;

    Value v[N_SYSINDEXCOL_COLS];
    memset(v, 0, sizeof(v));
    v[0].u.i4 = (i32)index_id;
    v[1].u.i4 = (i32)seq;
    v[2].u.i4 = (i32)col_no;

    u8 buf[128];
    u16 len;
    int rc = rec_encode(SYS_INDEXCOL_COLS, N_SYSINDEXCOL_COLS, v, buf, sizeof(buf), &len);
    if (rc != OK)
        return rc;

    Rid rid;
    return heap_insert(e->heap, xid, buf, len, &rid);
}

/* ---------------- 首次创建 ---------------- */

static int bootstrap(Catalog *cat)
{
    Pager *pg = cat->pager;

    /* 空文件：pager_alloc 一定给出第 0 页 */
    PageFrame *f;
    u32 pid;
    int rc = pager_alloc(pg, &pid, &f);
    if (rc != OK)
        return rc;
    if (pid != PAGE_ID_META) {
        pager_unpin(pg, pid, false);
        return E_CORRUPT;      /* 文件不空却要走 bootstrap */
    }

    page_init(f->data, PT_META);
    put_u32(f->data + M_MAGIC, CAT_MAGIC);
    put_u32(f->data + M_VERSION, CAT_VERSION);
    put_u32(f->data + M_CATROOT, PAGE_ID_NIL);
    put_u32(f->data + M_NEXTTID, CAT_FIRST_USER_ID);
    put_u32(f->data + M_PAGESZ, PAGE_SIZE);
    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    static const struct {
        u32 id;
        const char *name;
    } sys[4] = {
        { SYS_TABLE_ID,    "sys_table"     },
        { SYS_COLUMN_ID,   "sys_column"    },
        { SYS_INDEX_ID,    "sys_index"     },
        { SYS_INDEXCOL_ID, "sys_index_col" },
    };

    /* 1) 先把 4 张系统表的堆建出来 */
    for (int i = 0; i < 4; i++) {
        int nc = 0;
        const ColDesc *cd = sys_cols_of(sys[i].id, &nc);

        FreeList *fl = NULL;
        rc = freelist_create(0, &fl);
        if (rc != OK)
            return rc;

        u32 root;
        rc = heap_create(pg, fl, (u16)sys[i].id, cat->wal_fn, cat->wal_ctx, &root);
        if (rc != OK) {
            freelist_destroy(fl);
            return rc;
        }

        Heap *h;
        rc = heap_open(pg, fl, root, (u16)sys[i].id, cat->wal_fn, cat->wal_ctx, &h);
        if (rc != OK) {
            freelist_destroy(fl);
            return rc;
        }

        TableMeta m;
        memset(&m, 0, sizeof(m));
        m.table_id = sys[i].id;
        snprintf(m.name, sizeof(m.name), "%s", sys[i].name);
        m.root_page = root;
        m.ncols = (u32)nc;
        m.flags = 1;                 /* 系统表 */

        rc = ent_add(cat, &m, h, fl, cd, nc, rid_make(PAGE_ID_NIL, 0));
        if (rc != OK)
            return rc;

        if (sys[i].id == SYS_TABLE_ID)
            cat->catalog_root = root;   /* 后面写进 meta */
    }

    cat->next_table_id = CAT_FIRST_USER_ID;

    /* 2) catalog_root 落盘 —— 从此 sys_table 可被找到 */
    rc = meta_write(cat, 0);
    if (rc != OK)
        return rc;

    /* 3) 把 4 张系统表自己登记进 sys_table，并写各自的列 */
    for (int i = 0; i < 4; i++) {
        TableEnt *e = catalog_ent_by_id(cat, sys[i].id);
        if (!e)
            return E_CORRUPT;
        Rid mrid;
        rc = write_table_row(cat, 0, &e->meta, &mrid);
        if (rc != OK)
            return rc;
        e->meta_rid = mrid;
        e->has_rid = true;
        rc = write_col_rows(cat, 0, e->meta.table_id, e->cols, e->ncols);
        if (rc != OK)
            return rc;
    }
    return OK;
}

/* ---------------- 加载 ---------------- */

/* 用户表的列定义从 sys_column 读 */
static int load_user_cols(Catalog *cat)
{
    TableEnt *ce = catalog_ent_by_id(cat, SYS_COLUMN_ID);
    if (!ce)
        return E_CORRUPT;

    for (int i = 0; i < cat->n; i++) {
        TableEnt *e = &cat->t[i];
        if (e->meta.table_id < CAT_FIRST_USER_ID || e->cols)
            continue;
        if (e->meta.ncols == 0)
            continue;

        ColDesc *cols = calloc(e->meta.ncols, sizeof(ColDesc));
        if (!cols)
            return E_IO;

        HeapScan *s;
        int rc = heap_scan_begin(ce->heap, &s);
        if (rc != OK) {
            free(cols);
            return rc;
        }

        int got = 0;
        u8 buf[512];
        u16 len;
        Rid rid;
        while ((rc = heap_scan_next(s, &rid, buf, sizeof(buf), &len)) == OK) {
            Value v[N_SYSCOLUMN_COLS];
            if (rec_decode(SYS_COLUMN_COLS, N_SYSCOLUMN_COLS, buf, len, v) != OK)
                continue;
            if ((u32)v[0].u.i4 != e->meta.table_id)
                continue;
            int cno = (int)v[1].u.i4;
            if (cno < 0 || cno >= (int)e->meta.ncols)
                continue;
            snprintf(cols[cno].name, COL_NAME_MAX, "%.*s",
                     (int)v[2].u.text.n, (const char *)v[2].u.text.p);
            cols[cno].type = (u8)v[3].u.i4;
            cols[cno].nullable = (v[4].u.i4 != 0);
            got++;
        }
        heap_scan_end(s);
        if (rc != E_EOF) {
            free(cols);
            return rc;
        }
        if (got != (int)e->meta.ncols) {
            free(cols);
            return E_CORRUPT;      /* 元数据不一致 */
        }

        e->cols = cols;
        e->ncols = (int)e->meta.ncols;
    }
    return OK;
}

/* 从 sys_index / sys_index_col 加载索引，并打开各自的 btree */
static int load_indexes(Catalog *cat)
{
    TableEnt *ie = catalog_ent_by_id(cat, SYS_INDEX_ID);
    TableEnt *ce = catalog_ent_by_id(cat, SYS_INDEXCOL_ID);
    if (!ie || !ce)
        return E_CORRUPT;

    HeapScan *s;
    int rc = heap_scan_begin(ie->heap, &s);
    if (rc != OK)
        return rc;

    u8 buf[512];
    u16 len;
    Rid rid;
    while ((rc = heap_scan_next(s, &rid, buf, sizeof(buf), &len)) == OK) {
        Value v[N_SYSINDEX_COLS];
        if (rec_decode(SYS_INDEX_COLS, N_SYSINDEX_COLS, buf, len, v) != OK)
            continue;

        IndexMeta m;
        memset(&m, 0, sizeof(m));
        m.index_id = (u32)v[0].u.i4;
        snprintf(m.name, sizeof(m.name), "%.*s",
                 (int)v[1].u.text.n, (const char *)v[1].u.text.p);
        m.table_id = (u32)v[2].u.i4;
        m.root_page = (u32)v[3].u.i4;
        m.is_unique = (u8)(v[5].u.i4 != 0);

        TableEnt *te = catalog_ent_by_id(cat, m.table_id);
        if (!te)
            continue;

        /* 单列索引：取 seq=0 的那一列 */
        bool got = false;
        u32 col_no = 0;
        HeapScan *s2;
        if (heap_scan_begin(ce->heap, &s2) == OK) {
            u8 b2[256];
            u16 l2;
            Rid r2;
            while (heap_scan_next(s2, &r2, b2, sizeof(b2), &l2) == OK) {
                Value v2[N_SYSINDEXCOL_COLS];
                if (rec_decode(SYS_INDEXCOL_COLS, N_SYSINDEXCOL_COLS, b2, l2, v2) != OK)
                    continue;
                if ((u32)v2[0].u.i4 != m.index_id || v2[1].u.i4 != 0)
                    continue;
                col_no = (u32)v2[2].u.i4;
                got = true;
                break;
            }
            heap_scan_end(s2);
        }
        if (!got)
            continue;
        m.col_no = col_no;

        FreeList *fl = NULL;
        if (freelist_create(0, &fl) != OK)
            continue;
        BTree *bt = NULL;
        if (btree_open(cat->pager, fl, m.root_page, &bt) != OK) {
            freelist_destroy(fl);
            continue;
        }

        IndexInfo *ni = realloc(te->idxs, (size_t)(te->nidx + 1) * sizeof(IndexInfo));
        if (!ni) {
            btree_destroy(bt);
            freelist_destroy(fl);
            continue;
        }
        te->idxs = ni;
        te->idxs[te->nidx].meta = m;
        te->idxs[te->nidx].bt = bt;
        te->idxs[te->nidx].fl = fl;
        te->nidx++;

        if (m.index_id >= cat->next_index_id)
            cat->next_index_id = m.index_id + 1;
    }
    heap_scan_end(s);
    return (rc == E_EOF) ? OK : rc;
}

static int load_all(Catalog *cat)
{
    /* 1) 按 catalog_root 打开 sys_table */
    FreeList *fl = NULL;
    int rc = freelist_create(0, &fl);
    if (rc != OK)
        return rc;

    Heap *h = NULL;
    rc = heap_open(cat->pager, fl, cat->catalog_root, (u16)SYS_TABLE_ID,
                   cat->wal_fn, cat->wal_ctx, &h);
    if (rc != OK) {
        freelist_destroy(fl);
        return rc;
    }

    /* 2) 扫 sys_table，把每张表都登记进来 */
    HeapScan *s;
    rc = heap_scan_begin(h, &s);
    if (rc != OK) {
        heap_destroy(h);
        freelist_destroy(fl);
        return rc;
    }

    u8 buf[512];
    u16 len;
    Rid rid;
    while ((rc = heap_scan_next(s, &rid, buf, sizeof(buf), &len)) == OK) {
        Value v[N_SYSTABLE_COLS];
        rc = rec_decode(SYS_TABLE_COLS, N_SYSTABLE_COLS, buf, len, v);
        if (rc != OK)
            break;

        TableMeta m;
        memset(&m, 0, sizeof(m));
        m.table_id = (u32)v[0].u.i4;
        snprintf(m.name, sizeof(m.name), "%.*s",
                 (int)v[1].u.text.n, (const char *)v[1].u.text.p);
        m.root_page = (u32)v[2].u.i4;
        m.ncols = (u32)v[3].u.i4;
        m.nrows_est = v[4].is_null ? 0 : v[4].u.i8;
        m.flags = (u32)v[5].u.i4;

        int nc = 0;
        const ColDesc *cd = sys_cols_of(m.table_id, &nc);

        if (cd) {
            /* 系统表：schema 硬编码；sys_table 复用已打开的 heap */
            if (m.table_id == SYS_TABLE_ID)
                rc = ent_add(cat, &m, h, fl, cd, nc, rid);
            else {
                FreeList *tfl = NULL;
                rc = freelist_create(0, &tfl);
                if (rc != OK)
                    break;
                Heap *th = NULL;
                rc = heap_open(cat->pager, tfl, m.root_page, (u16)m.table_id,
                               cat->wal_fn, cat->wal_ctx, &th);
                if (rc != OK) {
                    freelist_destroy(tfl);
                    break;
                }
                rc = ent_add(cat, &m, th, tfl, cd, nc, rid);
            }
        } else {
            FreeList *tfl = NULL;
            rc = freelist_create(0, &tfl);
            if (rc != OK)
                break;
            Heap *th = NULL;
            rc = heap_open(cat->pager, tfl, m.root_page, (u16)m.table_id,
                           cat->wal_fn, cat->wal_ctx, &th);
            if (rc != OK) {
                freelist_destroy(tfl);
                break;
            }
            rc = ent_add(cat, &m, th, tfl, NULL, 0, rid);
        }
        if (rc != OK)
            break;
    }
    heap_scan_end(s);
    if (rc != E_EOF)
        return rc;

    /* 3) 用户表的列定义 */
    rc = load_user_cols(cat);
    if (rc != OK)
        return rc;

    /* 4) 索引 */
    return load_indexes(cat);
}

/* ---------------- 公开 API ---------------- */

int catalog_open(Pager *pg, HeapWalFn fn, void *ctx, Catalog **out)
{
    if (!pg || !out)
        return E_INVAL;

    Catalog *cat = calloc(1, sizeof(*cat));
    if (!cat)
        return E_IO;
    cat->pager = pg;
    cat->wal_fn = fn;
    cat->wal_ctx = ctx;

    int rc;
    if (pager_npages(pg) == 0) {
        rc = bootstrap(cat);
    } else {
        PageFrame *f;
        rc = pager_get(pg, PAGE_ID_META, &f);
        if (rc != OK) {
            free(cat);
            return rc;
        }
        u32 magic = get_u32(f->data + M_MAGIC);
        u32 psz = get_u32(f->data + M_PAGESZ);
        cat->catalog_root = get_u32(f->data + M_CATROOT);
        cat->next_table_id = get_u32(f->data + M_NEXTTID);
        pager_unpin(pg, PAGE_ID_META, false);

        if (magic != CAT_MAGIC || psz != PAGE_SIZE) {
            free(cat);
            return E_CORRUPT;      /* 不是我们的文件，或页大小变了 */
        }
        rc = load_all(cat);
    }

    if (rc != OK) {
        catalog_close(cat);
        return rc;
    }
    *out = cat;
    return OK;
}

void catalog_close(Catalog *cat)
{
    if (!cat)
        return;
    for (int i = 0; i < cat->n; i++) {
        for (int j = 0; j < cat->t[i].nidx; j++) {
            btree_destroy(cat->t[i].idxs[j].bt);
            freelist_destroy(cat->t[i].idxs[j].fl);
        }
        free(cat->t[i].idxs);
        free(cat->t[i].cols);
        heap_destroy(cat->t[i].heap);
        freelist_destroy(cat->t[i].fl);
    }
    free(cat->t);
    free(cat);
}

int catalog_create_table(Catalog *cat, u32 xid, const char *name,
                         const ColDesc *cols, int ncols, u32 *table_id_out)
{
    if (!cat || !name || !cols || ncols <= 0 || !table_id_out)
        return E_INVAL;
    if (strlen(name) == 0 || strlen(name) >= TABLE_NAME_MAX)
        return E_INVAL;

    u32 exist;
    if (catalog_find_table(cat, name, &exist) == OK)
        return E_INVAL;            /* 重名 */

    FreeList *fl = NULL;
    int rc = freelist_create(0, &fl);
    if (rc != OK)
        return rc;

    u32 root;
    rc = heap_create(cat->pager, fl, (u16)cat->next_table_id,
                     cat->wal_fn, cat->wal_ctx, &root);
    if (rc != OK) {
        freelist_destroy(fl);
        return rc;
    }

    Heap *h = NULL;
    rc = heap_open(cat->pager, fl, root, (u16)cat->next_table_id,
                   cat->wal_fn, cat->wal_ctx, &h);
    if (rc != OK) {
        freelist_destroy(fl);
        return rc;
    }

    TableMeta m;
    memset(&m, 0, sizeof(m));
    m.table_id = cat->next_table_id;
    snprintf(m.name, sizeof(m.name), "%s", name);
    m.root_page = root;
    m.ncols = (u32)ncols;
    m.flags = 0;

    rc = ent_add(cat, &m, h, fl, cols, ncols, rid_make(PAGE_ID_NIL, 0));
    if (rc != OK)
        return rc;

    Rid mrid;
    rc = write_table_row(cat, xid, &m, &mrid);
    if (rc != OK)
        return rc;

    TableEnt *me = catalog_ent_by_id(cat, m.table_id);
    if (me) {
        me->meta_rid = mrid;
        me->has_rid = true;
    }
    rc = write_col_rows(cat, xid, m.table_id, cols, ncols);
    if (rc != OK)
        return rc;

    cat->next_table_id++;
    rc = meta_write(cat, xid);
    if (rc != OK)
        return rc;

    *table_id_out = m.table_id;
    return OK;
}

int catalog_find_table(Catalog *cat, const char *name, u32 *table_id_out)
{
    if (!cat || !name || !table_id_out)
        return E_INVAL;
    for (int i = 0; i < cat->n; i++) {
        if (strcmp(cat->t[i].meta.name, name) == 0) {
            *table_id_out = cat->t[i].meta.table_id;
            return OK;
        }
    }
    return E_NOTFOUND;
}

/*
 * 取表元数据 —— **拷贝出参**（v1.2 步骤 1）。
 *
 * 原来返回的是 &e->meta —— 那是 TableEnt 数组里的一个元素，
 * 而 cat->t 会在建表时 realloc（见 ent_add）→ 指针悬空。
 */
int catalog_table_meta(Catalog *cat, u32 table_id, TableMeta *out)
{
    if (!cat || !out)
        return E_INVAL;
    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return E_NOTFOUND;
    *out = e->meta;
    return OK;
}

/*
 * 取列定义 —— **拷贝出参**（v1.2 步骤 1）。
 *
 * e->cols 本身是"每表一份 malloc"，扩容时不会被 realloc，
 * 所以它**恰好**是稳定的 —— 但那是个隐含前提：
 * "只要没人 free 它"（只有 catalog_close 会 free）。
 * 步骤 3/4 之后 catalog 的锁会放开，这个前提就不再由锁兜底了，
 * 所以统一改成拷贝：调用方拿到的是**自己的**副本。
 *
 * 两段式：buf = NULL 且 cap = 0 → 只报需要多少列。
 */
int catalog_table_cols(Catalog *cat, u32 table_id, ColDesc *buf, int cap, int *ncols)
{
    if (!cat || !ncols)
        return E_INVAL;
    if (!buf && cap > 0)
        return E_INVAL;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e || !e->cols)
        return E_NOTFOUND;

    *ncols = e->ncols;
    if (!buf)
        return OK;                     /* 只是问长度 */

    if (cap < e->ncols)
        return E_NOSPACE;              /* 缓冲区不够 */

    memcpy(buf, e->cols, (size_t)e->ncols * sizeof(ColDesc));
    return OK;
}

Heap *catalog_heap(Catalog *cat, u32 table_id)
{
    if (!cat)
        return NULL;
    TableEnt *e = catalog_ent_by_id(cat, table_id);
    return e ? e->heap : NULL;
}

void catalog_set_wal(Catalog *cat, HeapWalFn fn, void *ctx)
{
    if (!cat)
        return;
    for (int i = 0; i < cat->n; i++)
        heap_set_wal(cat->t[i].heap, fn, ctx);
}

/*
 * 【为什么没有 catalog_set_xid】
 *
 * v0.7 最初有个 catalog_set_xid(cat, xid)：给所有表的堆设置"当前事务号"。
 * 看起来很自然 —— 每次写入都要在 tuple 头打 xmin/xmax，堆总得知道是谁在写。
 *
 * 但这是**共享状态**，在多连接下必然出错：
 *   catalog 是实例级共享的（所有连接一份）。
 *   连接 A 开事务设 xid=5，连接 B 开事务设 xid=6，
 *   A 的写就被打上了 B 的 xid —— 可见性判断立刻失效。
 *
 * 教训和 v0.6 的"xid 跨会话混淆"同源：
 *   **"当前是谁"这类信息不能藏在共享对象里，必须随操作显式传递。**
 *
 * 所以现在 xid 一路从 db 层传到 heap 层，作为参数传下去。
 */

u32 catalog_table_id_at(const Catalog *cat, int i)
{
    if (!cat || i < 0 || i >= cat->n)
        return 0;
    return cat->t[i].meta.table_id;
}

u32 catalog_ntables(const Catalog *cat)
{
    return cat ? (u32)cat->n : 0;
}

int catalog_create_index(Catalog *cat, u32 xid, const char *name, u32 table_id,
                         u32 col_no, u32 *index_id_out)
{
    if (!cat || !name || !index_id_out)
        return E_INVAL;

    TableEnt *te = catalog_ent_by_id(cat, table_id);
    if (!te)
        return E_NOTFOUND;
    if (col_no >= (u32)te->ncols)
        return E_INVAL;

    /* 索引名全局唯一 */
    for (int i = 0; i < cat->n; i++)
        for (int j = 0; j < cat->t[i].nidx; j++)
            if (strcmp(cat->t[i].idxs[j].meta.name, name) == 0)
                return E_INVAL;

    FreeList *fl = NULL;
    int rc = freelist_create(0, &fl);
    if (rc != OK)
        return rc;

    u32 root;
    rc = btree_create(cat->pager, fl, &root);
    if (rc != OK) {
        freelist_destroy(fl);
        return rc;
    }

    BTree *bt = NULL;
    rc = btree_open(cat->pager, fl, root, &bt);
    if (rc != OK) {
        freelist_destroy(fl);
        return rc;
    }

    IndexMeta m;
    memset(&m, 0, sizeof(m));
    m.index_id = cat->next_index_id++;
    snprintf(m.name, sizeof(m.name), "%s", name);
    m.table_id = table_id;
    m.root_page = root;
    m.col_no = col_no;
    m.is_unique = 0;

    rc = write_index_row(cat, xid, &m);
    if (rc == OK)
        rc = write_indexcol_row(cat, xid, m.index_id, 0, col_no);
    if (rc != OK) {
        btree_destroy(bt);
        freelist_destroy(fl);
        return rc;
    }

    IndexInfo *ni = realloc(te->idxs, (size_t)(te->nidx + 1) * sizeof(IndexInfo));
    if (!ni) {
        btree_destroy(bt);
        freelist_destroy(fl);
        return E_IO;
    }
    te->idxs = ni;
    te->idxs[te->nidx].meta = m;
    te->idxs[te->nidx].bt = bt;
    te->idxs[te->nidx].fl = fl;
    te->nidx++;

    *index_id_out = m.index_id;
    return OK;
}

/*
 * 取索引列表 —— **拷贝出参**（v1.2 步骤 1）。
 *
 * te->idxs 会在建索引时 realloc（见 catalog_create_index），
 * 原来返回的 e->idxs 因此是**真悬空**：另一个连接建个索引，
 * 这里拿着的指针就指向 freed 内存。
 *
 * 拷贝的是 IndexInfo 结构本身（浅拷贝）——
 * 里面的 BTree* / FreeList* 指向的对象归 catalog 所有，
 * 索引不会被删，所以那些指针照样有效。
 */
int catalog_table_indexes(Catalog *cat, u32 table_id,
                          IndexInfo *buf, int cap, int *n_out)
{
    if (!cat || !n_out)
        return E_INVAL;
    if (!buf && cap > 0)
        return E_INVAL;

    TableEnt *e = catalog_ent_by_id(cat, table_id);
    if (!e)
        return E_NOTFOUND;

    *n_out = e->nidx;
    if (!buf)
        return OK;

    if (cap < e->nidx)
        return E_NOSPACE;

    if (e->nidx > 0)
        memcpy(buf, e->idxs, (size_t)e->nidx * sizeof(IndexInfo));
    return OK;
}

u32 catalog_next_table_id(const Catalog *cat)
{
    return cat ? cat->next_table_id : 0;
}
