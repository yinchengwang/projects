/*
 * catalog.c — 元数据：生命周期 + 公共入口
 *
 * 自举的两处硬编码（都不可省略）：
 *   1. 系统表 id：1..4，写死在宏里，绝不查表
 *   2. 系统表 schema：写死（在 catalog_load.c 里，因为 sys_column 的行要靠 schema 才能解码）
 *
 * 用户表的 schema 存在 sys_column 里，catalog_open 时读出来缓存。
 *
 * ==================== v1.2 步骤 7：按职责拆分 ====================
 *
 * 原来这个文件 1036 行，超过"单文件不超过 800 行"的硬规则。
 * 按职责拆成三个 .c，行为**完全不变**（纯搬家，不改逻辑）：
 *
 *   catalog.c       —— 本文件：生命周期 + 公共入口 + 一堆 *_locked 包装
 *   catalog_write.c —— 写路径：meta 页镜像、写系统表行
 *   catalog_load.c  —— 加载路径：读系统表，把元数据读回内存
 *
 * 跨文件共享的声明放在 catalog_internal.h。
 */

#include "catalog/catalog_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- meta 页布局在 catalog.h（公开） ----------------
 *
 * 原来这些宏是本文件私有的。v1.2 步骤 6 改了它们的可见性：
 * db 层要在**恢复之前**读出 checkpoint_lsn（恢复的起点），
 * 那时 catalog 还没打开 —— 所以布局常量必须公开。
 */

/* ---------------- 内存结构 ---------------- */

/* TableEnt / struct Catalog 在 catalog_priv.h（与 catalog_stats.c 共享） */

TableEnt *catalog_ent_by_id(Catalog *cat, u32 table_id)
{
    for (int i = 0; i < cat->n; i++)
        if (cat->t[i].meta.table_id == table_id)
            return &cat->t[i];
    return NULL;
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

/* ---------------- checkpoint 的 LSN（v1.2 步骤 6） ---------------- */

/*
 * 把 checkpoint_lsn 写进 meta 页。
 *
 * meta 页的整页镜像会进日志，所以这个值本身是崩溃安全的：
 * 崩溃后要么是旧值（多重放一遍，无害），要么是新值（正是我们要的）。
 *
 * xid 用 0（系统事务）—— checkpoint 不是用户事务。
 */
int catalog_set_checkpoint_lsn_locked(Catalog *cat, u32 xid, u32 lsn)
{
    if (!cat)
        return E_INVAL;
    cat->checkpoint_lsn = lsn;
    return meta_write(cat, xid);
}

u32 catalog_checkpoint_lsn_locked(Catalog *cat)
{
    return cat ? cat->checkpoint_lsn : 0;
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

    /* catalog 域的读写锁（v1.2 步骤 3）。此时还没有别的线程能用它 */
    if (pthread_rwlock_init(&cat->rw, NULL) != 0) {
        free(cat);
        return E_IO;
    }

    int rc;
    if (pager_npages(pg) == 0) {
        rc = bootstrap(cat);
    } else {
        PageFrame *f;
        rc = pager_get(pg, PAGE_ID_META, &f);
        if (rc != OK) {
            pthread_rwlock_destroy(&cat->rw);
            free(cat);
            return rc;
        }
        u32 magic = get_u32(f->data + M_MAGIC);
        u32 psz = get_u32(f->data + M_PAGESZ);
        cat->catalog_root = get_u32(f->data + M_CATROOT);
        cat->next_table_id = get_u32(f->data + M_NEXTTID);
        cat->checkpoint_lsn = get_u32(f->data + M_CKPT_LSN);
        pager_unpin(pg, PAGE_ID_META, false);

        if (magic != CAT_MAGIC || psz != PAGE_SIZE) {
            pthread_rwlock_destroy(&cat->rw);
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

/*
 * 关闭 catalog。
 *
 * 【不取锁】
 * 销毁时已经没有别的线程在用这个实例（引用计数归零才走到这里），
 * 而且 catalog_open 的错误路径也会调它 —— 取锁会自锁。
 */
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
    pthread_rwlock_destroy(&cat->rw);
    free(cat);
}

int catalog_create_table_locked(Catalog *cat, u32 xid, const char *name,
                                const ColDesc *cols, int ncols, u32 *table_id_out)
{
    if (!cat || !name || !cols || ncols <= 0 || !table_id_out)
        return E_INVAL;
    if (strlen(name) == 0 || strlen(name) >= TABLE_NAME_MAX)
        return E_INVAL;

    u32 exist;
    /* 内部调用走 _locked 版本 —— 外面已经持写锁了，不能再取一次 */
    if (catalog_find_table_locked(cat, name, &exist) == OK)
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

int catalog_find_table_locked(Catalog *cat, const char *name, u32 *table_id_out)
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
int catalog_table_meta_locked(Catalog *cat, u32 table_id, TableMeta *out)
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
int catalog_table_cols_locked(Catalog *cat, u32 table_id, ColDesc *buf, int cap, int *ncols)
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

Heap *catalog_heap_locked(Catalog *cat, u32 table_id)
{
    if (!cat)
        return NULL;
    TableEnt *e = catalog_ent_by_id(cat, table_id);
    return e ? e->heap : NULL;
}

void catalog_set_wal_locked(Catalog *cat, HeapWalFn fn, void *ctx)
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

u32 catalog_table_id_at_locked(const Catalog *cat, int i)
{
    if (!cat || i < 0 || i >= cat->n)
        return 0;
    return cat->t[i].meta.table_id;
}

u32 catalog_ntables_locked(const Catalog *cat)
{
    return cat ? (u32)cat->n : 0;
}

int catalog_create_index_locked(Catalog *cat, u32 xid, const char *name, u32 table_id,
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
int catalog_table_indexes_locked(Catalog *cat, u32 table_id,
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

u32 catalog_next_table_id_locked(const Catalog *cat)
{
    return cat ? cat->next_table_id : 0;
}
