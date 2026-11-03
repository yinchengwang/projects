/*
 * catalog_load.c — 元数据加载路径
 *
 * ==================== 这一层为什么存在 ====================
 *
 * v1.2 步骤 7：catalog.c 原来 1036 行，超过"单文件不超过 800 行"的硬规则。
 * 把"从磁盘把元数据读回内存"这件事单独拆出来：
 *
 *   sys_cols_of     系统表的硬编码 schema
 *   load_user_cols  用户表的列定义从 sys_column 读
 *   load_indexes    索引从 sys_index / sys_index_col 读，并打开各自的 btree
 *   load_all        按 catalog_root 把磁盘上的元数据读进内存
 *
 * 系统表 schema 也放在这里 —— sys_cols_of 就在这儿，
 * 而 schema 必须硬编码：sys_column 的行要靠 schema 才能解码，
 * 而 schema 又存在 sys_column 里，不硬编码就是自举死锁。
 *
 * ==================== 纪律 ====================
 *
 * 本文件是**纯搬家**：代码逻辑与拆分前逐字一致，
 * 只是从 catalog.c 里搬出来，方便以后各改各的。
 */

#include "catalog/catalog_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 系统表 schema（硬编码） ---------------- */

const ColDesc SYS_TABLE_COLS[N_SYSTABLE_COLS] = {
    { "table_id",  T_INT4, false },
    { "name",      T_TEXT, false },
    { "root_page", T_INT4, false },
    { "ncols",     T_INT4, false },
    { "nrows_est", T_INT8, true  },
    { "flags",     T_INT4, false },
};

const ColDesc SYS_COLUMN_COLS[N_SYSCOLUMN_COLS] = {
    { "table_id", T_INT4, false },
    { "col_no",   T_INT4, false },
    { "name",     T_TEXT, false },
    { "type",     T_INT4, false },
    { "flags",    T_INT4, false },
};

const ColDesc SYS_INDEX_COLS[N_SYSINDEX_COLS] = {
    { "index_id",  T_INT4, false },
    { "name",      T_TEXT, false },
    { "table_id",  T_INT4, false },
    { "root_page", T_INT4, false },
    { "nkeys",     T_INT4, false },
    { "is_unique", T_INT4, false },
    { "flags",     T_INT4, false },
};

const ColDesc SYS_INDEXCOL_COLS[N_SYSINDEXCOL_COLS] = {
    { "index_id", T_INT4, false },
    { "seq",      T_INT4, false },
    { "col_no",   T_INT4, false },
};

const ColDesc *sys_cols_of(u32 table_id, int *ncols)
{
    switch (table_id) {
    case SYS_TABLE_ID:    *ncols = N_SYSTABLE_COLS;    return SYS_TABLE_COLS;
    case SYS_COLUMN_ID:   *ncols = N_SYSCOLUMN_COLS;   return SYS_COLUMN_COLS;
    case SYS_INDEX_ID:    *ncols = N_SYSINDEX_COLS;    return SYS_INDEX_COLS;
    case SYS_INDEXCOL_ID: *ncols = N_SYSINDEXCOL_COLS; return SYS_INDEXCOL_COLS;
    default:              *ncols = 0;                  return NULL;
    }
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

int load_all(Catalog *cat)
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
