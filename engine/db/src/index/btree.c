/*
 * btree.c — B+tree 生命周期与观察接口
 *
 * 本文件负责"树"这一层的生命周期与观察：
 *
 *   - create / open / destroy：建树、打开一棵树、销毁
 *   - reset：把整棵树重置成空树（索引重建用）
 *   - height / npages / root / format_ok：观察树的形状与格式版本
 *   - stats：遍历全树统计（页数、条目数、填充率）
 *   - 树锁 bt_lock / bt_unlock：域锁 + latch 记账
 *
 * 查找在 btree_search.c，插入在 btree_insert.c，删除在 btree_delete.c，
 * 扫描在 btree_cursor.c，页内原语在 btree_page.c。
 * 共享的宏与类型在 index/btree_internal.h。
 */

#include "index/btree_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/latch.h"

/* ---------------- 树锁 ---------------- */

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → btree → frame → wal → freelist"这条全序
 * 从步骤 2 起就是**机器检查**的。
 */
void bt_lock(BTree *bt)
{
    pthread_mutex_lock(&bt->mu);
    latch_order_acquire(LATCH_BTREE);
}

void bt_unlock(BTree *bt)
{
    latch_order_release(LATCH_BTREE);
    pthread_mutex_unlock(&bt->mu);
}

/* ---------------- 生命周期 ---------------- */

int btree_create(Pager *pg, FreeList *fl, u32 *root_out)
{
    if (!pg || !fl || !root_out)
        return E_INVAL;

    PageFrame *f;
    u32 pid;
    int rc = pager_alloc(pg, &pid, &f);
    if (rc != OK)
        return rc;

    bt_init(f->data, BT_LEAF, 0);

    rc = freelist_grow(fl, pid + 1);
    if (rc != OK) {
        pager_unpin(pg, pid, false);
        return rc;
    }
    freelist_set_full(fl, pid);

    rc = pager_unpin(pg, pid, true);
    if (rc != OK)
        return rc;

    *root_out = pid;
    return OK;
}

int btree_open(Pager *pg, FreeList *fl, u32 root, BTree **out)
{
    if (!pg || !fl || !out || root == PAGE_ID_NIL)
        return E_INVAL;

    BTree *bt = calloc(1, sizeof(*bt));
    if (!bt)
        return E_IO;
    if (pthread_mutex_init(&bt->mu, NULL) != 0) {
        free(bt);
        return E_IO;
    }
    bt->pager = pg;
    bt->fl = fl;
    bt->root = root;
    bt->npages = 1;

    PageFrame *f;
    int rc = pager_get(pg, root, &f);
    if (rc != OK) {
        pthread_mutex_destroy(&bt->mu);
        free(bt);
        return rc;
    }
    bt->height = bt_level(f->data);
    pager_unpin(pg, root, false);

    *out = bt;
    return OK;
}

void btree_destroy(BTree *bt)
{
    if (!bt)
        return;
    pthread_mutex_destroy(&bt->mu);
    free(bt);
}

static void reset_walk(BTree *bt, u32 pid, bool is_root)
{
    PageFrame *f;
    if (pager_get(bt->pager, pid, &f) != OK)
        return;

    u16 n = bt_nitems(f->data);
    u32 lm = bt_leftmost(f->data);
    bool inner = (bt_type(f->data) == BT_INNER);

    u32 kids[512];
    u16 nk = 0;
    if (inner) {
        kids[nk++] = lm;
        for (u16 i = 0; i < n && nk < 512; i++)
            kids[nk++] = (u32)item_val(f->data, i);
    }
    pager_unpin(bt->pager, pid, false);

    for (u16 i = 0; i < nk; i++)
        reset_walk(bt, kids[i], false);

    if (is_root) {
        pager_get(bt->pager, pid, &f);
        bt_init(f->data, BT_LEAF, 0);
        pager_unpin(bt->pager, pid, true);
    } else {
        freelist_set(bt->fl, pid, FL_USABLE);
    }
}

int btree_reset(BTree *bt)
{
    if (!bt)
        return E_INVAL;
    bt_lock(bt);
    reset_walk(bt, bt->root, true);
    bt->height = 0;
    bt->npages = 1;
    bt_unlock(bt);
    return OK;
}

/* ---------------- 观察接口 ---------------- */

/*
 * 【观察接口为什么要取锁】
 *
 * `root` / `height` / `npages` 是树结构的可变状态，
 * 写线程会改它们（根分裂、根降高）。不加锁读就是数据竞争。
 *
 * `const BTree *` 的意思是"调用方不许改这棵树"，
 * 不是"这个对象里的每个字节都不可变"——
 * 锁的状态当然要变。所以取锁时显式去掉 const
 * （与 freelist.c 的同类说明一致）。
 */
BTree *bt_mut(const BTree *bt)
{
    return (BTree *)bt;
}

u32 btree_height(const BTree *bt)
{
    if (!bt)
        return 0;
    BTree *m = bt_mut(bt);
    bt_lock(m);
    u32 h = m->height;
    bt_unlock(m);
    return h;
}

u32 btree_npages(const BTree *bt)
{
    if (!bt)
        return 0;
    BTree *m = bt_mut(bt);
    bt_lock(m);
    u32 n = m->npages;
    bt_unlock(m);
    return n;
}

u32 btree_root(const BTree *bt)
{
    if (!bt)
        return PAGE_ID_NIL;
    BTree *m = bt_mut(bt);
    bt_lock(m);
    u32 r = m->root;
    bt_unlock(m);
    return r;
}

/*
 * 索引 key 的格式版本是不是当前代码认的那个？
 *
 * 对不上 → 这个索引是用旧编码建的，拿新代码读会读出错的 key
 *        → 必须重建（索引是派生物，重建没有语义风险）
 *
 * 读根页失败也算"对不上" —— 宁可重建一次，也不要拿着半残的索引去查。
 */
int btree_format_ok(const BTree *bt)
{
    if (!bt)
        return 0;

    BTree *m = bt_mut(bt);
    bt_lock(m);
    if (m->root == PAGE_ID_NIL) {
        bt_unlock(m);
        return 0;
    }
    u32 root = m->root;

    PageFrame *f;
    if (pager_get(m->pager, root, &f) != OK) {
        bt_unlock(m);
        return 0;
    }

    u8 ver = f->data[BT_FLAGS];
    pager_unpin(m->pager, root, false);

    bt_unlock(m);
    return ver == BT_FMTVER;
}

/* ---------------- 统计 ---------------- */

typedef struct {
    u32 npages;
    u32 nleaves;
    u32 ninner;
    u32 items;
    u32 leafitems;   /* 只数叶子页里的条目（真正存着 key→rid 的那些） */
    u64 used;
    u64 cap;
} StatsAcc;

static int stats_walk(BTree *bt, u32 pid, StatsAcc *acc)
{
    PageFrame *f;
    int rc = pager_get(bt->pager, pid, &f);
    if (rc != OK)
        return rc;

    u32 cap = PAGE_SIZE - BT_HDR;
    u16 nit = bt_nitems(f->data);
    acc->npages++;
    acc->items += nit;
    /* 按条目实际字节算，不能只看 free_bytes —— 分裂/删除留下的洞
     * 不在 free_bytes 里，会把填充率算高 */
    for (u16 i = 0; i < nit; i++)
        acc->used += item_len(f->data, i) + 2u;
    acc->cap += cap;

    if (bt_type(f->data) == BT_LEAF) {
        acc->nleaves++;
        acc->leafitems += nit;
        pager_unpin(bt->pager, pid, false);
        return OK;
    }

    acc->ninner++;
    u16 n = bt_nitems(f->data);
    u32 lm = bt_leftmost(f->data);

    /* 先把孩子页号全拷出来，再 unpin —— 递归时不能还 pin 着父页 */
    u32 *kids = malloc((size_t)(n + 1) * sizeof(u32));
    if (!kids) {
        pager_unpin(bt->pager, pid, false);
        return E_IO;
    }
    kids[0] = lm;
    for (u16 i = 0; i < n; i++)
        kids[i + 1] = (u32)item_val(f->data, i);
    pager_unpin(bt->pager, pid, false);

    for (u16 i = 0; i <= n; i++) {
        rc = stats_walk(bt, kids[i], acc);
        if (rc != OK)
            break;
    }
    free(kids);
    return rc;
}

int btree_stats(BTree *bt, BtStats *out)
{
    if (!bt || !out)
        return E_INVAL;

    bt_lock(bt);

    StatsAcc acc;
    memset(&acc, 0, sizeof(acc));

    int rc = stats_walk(bt, bt->root, &acc);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    out->npages = acc.npages;
    out->nleaves = acc.nleaves;
    out->ninner = acc.ninner;
    out->items = acc.items;
    out->leafitems = acc.leafitems;
    out->fill_pct = acc.cap ? (u32)(acc.used * 100u / acc.cap) : 0;
    bt_unlock(bt);
    return OK;
}
