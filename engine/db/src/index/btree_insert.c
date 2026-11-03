/*
 * btree_insert.c — 插入路径（含分裂）
 *
 * 插入是"递归下降 + 必要时分裂"：
 *
 *   insert_rec：从根往下走，途中每一层用 upper_bound 选孩子
 *              叶子插不下 → split_leaf；内部节点插不下 → split_inner
 *   孩子分裂了 → 把提升的分隔键插回父节点；父节点也满了 → 继续往上裂
 *   根分裂 → 新建一个更高的根
 *
 * 两条最容易写错的规矩（见 btree_internal.h）：
 *   1. 叶子分裂：提升的 key **两边都保留**
 *      内部节点分裂：提升的 key **不在任何一边**（被移走）
 *   2. 内部节点查找用 upper_bound（第一个 sep > key），走 pos-1 的孩子
 */

#include "index/btree_internal.h"

#include <string.h>

/* ---------------- 分裂 ---------------- */

/* 叶子分裂：提升的 key 两边都保留 */
static int split_leaf(BTree *bt, u8 *pg, u32 pid, u16 pos,
                      const u8 *key, u16 klen, u64 val, Split *sp)
{
    u32 new_pid;
    PageFrame *nf;
    int rc = bt_alloc_page(bt, BT_LEAF, 0, &new_pid, &nf);
    if (rc != OK)
        return rc;

    u16 n = bt_nitems(pg);
    u16 mid = (u16)(n / 2);
    u8 tmpk[BT_MAX_KEY];

    /*
     * 把 [mid, n) 搬到新页，**必须保持升序**。
     *
     * 这里反复读下标 mid：每删掉一个，后面的项自动补位，
     * 所以读到的永远是"当前第 mid 个"，也就是原 mid, mid+1, ...
     * 顺序追加到新页 → 新页也是升序。
     *
     * 千万别倒序遍历 + 追加 —— 那样新页是降序，
     * 叶子区间会互相重叠，提升的 key 也全错。
     */
    for (u16 i = mid; i < n; i++) {
        u16 kl;
        const u8 *k = item_key(pg, mid, &kl);
        u64 v = item_val(pg, mid);
        memcpy(tmpk, k, kl);
        rc = insert_at(bt, nf->data, bt_nitems(nf->data), tmpk, kl, v);
        if (rc != OK)
            goto fail;
        remove_at(pg, mid);
    }

    /* 新项插到合适的一页 */
    if (pos <= mid)
        rc = insert_at(bt, pg, pos, key, klen, val);
    else
        rc = insert_at(bt, nf->data, (u16)(pos - mid), key, klen, val);
    if (rc != OK)
        goto fail;

    /* 提升新页的第一个 key（两边都保留副本） */
    {
        u16 kl;
        const u8 *k = item_key(nf->data, 0, &kl);
        memcpy(sp->sep, k, kl);
        sp->sep_len = kl;
    }
    /* 搬完必须整理：remove_at 留下的是洞，不计入 free_bytes，
     * 不整理的话这一页看起来"很满"，后续插入会误判需要再分裂 */
    bt_compact(bt, pg);

    sp->right = new_pid;
    sp->split = true;

    /* 叶子链：原页 → 新页 → 原右兄弟 */
    set_right(nf->data, bt_right(pg));
    set_right(pg, new_pid);
    bt_checksum(nf->data);

    pager_unpin(bt->pager, new_pid, true);
    (void)pid;
    return OK;

fail:
    pager_unpin(bt->pager, new_pid, true);
    return rc;
}

/* 内部节点分裂：提升的 key 不在任何一边 */
static int split_inner(BTree *bt, u8 *pg, u32 pid, u16 pos,
                       const u8 *key, u16 klen, u32 child, Split *sp)
{
    u32 new_pid;
    PageFrame *nf;
    int rc = bt_alloc_page(bt, BT_INNER, bt_level(pg), &new_pid, &nf);
    if (rc != OK)
        return rc;

    u16 n = bt_nitems(pg);
    u16 total = (u16)(n + 1);       /* 含新项 */
    u16 mid = (u16)(total / 2);     /* 提升项在"合并序列"里的下标 */

    u8 tmpk[BT_MAX_KEY];
    bool promoted = false;

    /* 从后往前扫原页：ci 是该项在合并序列里的下标
     *   ci >  mid → 搬到新页
     *   ci == mid → 提升（不留在任何一边）
     *   ci <  mid → 留在原页
     * 倒序处理，remove_at 不会影响还没处理的更小下标 */
    for (u16 i = n; i-- > 0; ) {
        u16 ci = (i < pos) ? i : (u16)(i + 1);
        if (ci > mid) {
            u16 kl;
            const u8 *k = item_key(pg, i, &kl);
            u32 ch = (u32)item_val(pg, i);
            memcpy(tmpk, k, kl);
            rc = insert_at(bt, nf->data, 0, tmpk, kl, (u64)ch);
            if (rc != OK)
                goto fail;
            remove_at(pg, i);
        } else if (ci == mid) {
            u16 kl;
            const u8 *k = item_key(pg, i, &kl);
            u32 ch = (u32)item_val(pg, i);
            memcpy(sp->sep, k, kl);
            sp->sep_len = kl;
            set_leftmost(nf->data, ch);   /* 新页的最左孩子 = 提升项的右孩子 */
            remove_at(pg, i);
            promoted = true;
        }
    }

    /* 新项自己怎么安排 */
    if (pos == mid) {
        memcpy(sp->sep, key, klen);
        sp->sep_len = klen;
        set_leftmost(nf->data, child);
        promoted = true;
    } else if (pos < mid) {
        rc = insert_at(bt, pg, pos, key, klen, (u64)child);
        if (rc != OK)
            goto fail;
    } else {
        rc = insert_at(bt, nf->data, (u16)(pos - mid - 1), key, klen, (u64)child);
        if (rc != OK)
            goto fail;
    }

    if (!promoted) {
        rc = E_CORRUPT;
        goto fail;
    }

    bt_compact(bt, pg);
    bt_checksum(nf->data);
    sp->right = new_pid;
    sp->split = true;
    pager_unpin(bt->pager, new_pid, true);
    (void)pid;
    return OK;

fail:
    pager_unpin(bt->pager, new_pid, true);
    return rc;
}

/* ---------------- 插入 ---------------- */

static int insert_rec(BTree *bt, u32 pid, const u8 *key, u16 klen, u64 val, Split *sp)
{
    PageFrame *f;
    int rc = pager_get(bt->pager, pid, &f);
    if (rc != OK)
        return rc;

    if (bt_type(f->data) == BT_LEAF) {
        u16 pos = upper_bound(f->data, key, klen);   /* 插到相同 key 之后 */
        rc = insert_at(bt, f->data, pos, key, klen, val);
        if (rc == OK) {
            pager_unpin(bt->pager, pid, true);
            sp->split = false;
            return OK;
        }
        if (rc != E_NOSPACE) {
            pager_unpin(bt->pager, pid, false);
            return rc;
        }
        rc = split_leaf(bt, f->data, pid, pos, key, klen, val, sp);
        pager_unpin(bt->pager, pid, true);
        return rc;
    }

    /* 内部节点 */
    u16 pos = upper_bound(f->data, key, klen);
    u32 child = (pos == 0) ? bt_leftmost(f->data)
                           : (u32)item_val(f->data, (u16)(pos - 1));
    pager_unpin(bt->pager, pid, false);

    Split cs;
    rc = insert_rec(bt, child, key, klen, val, &cs);
    if (rc != OK)
        return rc;
    if (!cs.split) {
        sp->split = false;
        return OK;
    }

    rc = pager_get(bt->pager, pid, &f);
    if (rc != OK)
        return rc;

    rc = insert_at(bt, f->data, pos, cs.sep, cs.sep_len, (u64)cs.right);
    if (rc == OK) {
        pager_unpin(bt->pager, pid, true);
        sp->split = false;
        return OK;
    }
    if (rc != E_NOSPACE) {
        pager_unpin(bt->pager, pid, false);
        return rc;
    }

    rc = split_inner(bt, f->data, pid, pos, cs.sep, cs.sep_len, cs.right, sp);
    pager_unpin(bt->pager, pid, true);
    return rc;
}

int btree_insert(BTree *bt, const void *key, u16 klen, Rid rid)
{
    if (!bt || !key || klen == 0 || klen > BT_MAX_KEY)
        return E_INVAL;

    bt_lock(bt);

    Split sp;
    memset(&sp, 0, sizeof(sp));

    int rc = insert_rec(bt, bt->root, (const u8 *)key, klen, rid_to_u64(rid), &sp);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }
    if (!sp.split) {
        bt_unlock(bt);
        return OK;
    }

    /* 根分裂：新建根 */
    u32 new_root;
    PageFrame *f;
    rc = bt_alloc_page(bt, BT_INNER, bt->height + 1, &new_root, &f);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    set_leftmost(f->data, bt->root);
    rc = insert_at(bt, f->data, 0, sp.sep, sp.sep_len, (u64)sp.right);
    pager_unpin(bt->pager, new_root, true);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    bt->root = new_root;
    bt->height++;
    bt_unlock(bt);
    return OK;
}
