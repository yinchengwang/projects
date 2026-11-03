/*
 * catalog_write.c — 元数据写路径
 *
 * ==================== 这一层为什么存在 ====================
 *
 * v1.2 步骤 7：catalog.c 原来 1036 行，超过"单文件不超过 800 行"的硬规则。
 * 把"往系统表里写"这件事单独拆出来：
 *
 *   meta_write               meta 页的整页镜像进日志（根指针，必须崩溃安全）
 *   ent_add                  往内存里的表项数组追加一项
 *   catalog_encode_table_row sys_table 一行的编码（公开 API，写路径与统计共用）
 *   write_table_row          写 sys_table 的一行
 *   write_col_rows           写 sys_column 的若干行
 *   write_index_row          写 sys_index 的一行
 *   write_indexcol_row       写 sys_index_col 的一行
 *
 * ==================== 纪律 ====================
 *
 * 本文件是**纯搬家**：代码逻辑与拆分前逐字一致，
 * 只是从 catalog.c 里搬出来，方便以后各改各的。
 */

#include "catalog/catalog_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 内存结构 ---------------- */

/* TableEnt / struct Catalog 在 catalog_priv.h（与 catalog_stats.c 共享） */

int ent_add(Catalog *cat, const TableMeta *m, Heap *h, FreeList *fl,
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
int meta_write(Catalog *cat, u32 xid)
{
    PageFrame *f;
    int rc = pager_get(cat->pager, PAGE_ID_META, &f);
    if (rc != OK)
        return rc;

    u8 before[PAGE_SIZE];
    memcpy(before, f->data, PAGE_SIZE);

    put_u32(f->data + M_CATROOT, cat->catalog_root);
    put_u32(f->data + M_NEXTTID, cat->next_table_id);
    put_u32(f->data + M_CKPT_LSN, cat->checkpoint_lsn);
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

int write_table_row(Catalog *cat, u32 xid, const TableMeta *m,
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

int write_col_rows(Catalog *cat, u32 xid, u32 table_id,
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

int write_index_row(Catalog *cat, u32 xid, const IndexMeta *m)
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

int write_indexcol_row(Catalog *cat, u32 xid, u32 index_id, u32 seq, u32 col_no)
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
