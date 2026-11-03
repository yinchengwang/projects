/*
 * btree_delete.c — 删除路径
 *
 * 删除是"递归下降 + 必要时摘掉空孩子"：
 *
 *   delete_rec：在叶子找到 (key, rid) 并删掉；
 *              叶子空了 → 父节点把它的条目摘掉，页还给 freelist
 *   根降高：根是内部节点且只剩 leftmost 时，把唯一的孩子提上来
 *
 * 【注意】本文件**不做**节点合并/借位 —— 空了就摘掉，页回收。
 */

#include "index/btree_internal.h"

/* ---------------- 删除 ---------------- */

static int delete_rec(BTree *bt, u32 pid, const u8 *key, u16 klen, u64 rid, bool *empty)
{
    PageFrame *f;
    int rc = pager_get(bt->pager, pid, &f);
    if (rc != OK)
        return rc;

    if (bt_type(f->data) == BT_LEAF) {
        u16 pos = lower_bound(f->data, key, klen);
        bool found = false;
        u16 nit = bt_nitems(f->data);
        while (pos < nit) {
            u16 kl;
            const u8 *k = item_key(f->data, pos, &kl);
            if (key_cmp(k, kl, key, klen) != 0)
                break;
            if (item_val(f->data, pos) == rid) {
                found = true;
                break;
            }
            pos++;
        }
        if (!found) {
            pager_unpin(bt->pager, pid, false);
            return E_NOTFOUND;
        }
        rc = remove_at(f->data, pos);
        if (rc != OK) {
            pager_unpin(bt->pager, pid, false);
            return rc;
        }
        *empty = (bt_nitems(f->data) == 0);
        pager_unpin(bt->pager, pid, true);
        return OK;
    }

    u16 pos = upper_bound(f->data, key, klen);
    u32 child = (pos == 0) ? bt_leftmost(f->data)
                           : (u32)item_val(f->data, (u16)(pos - 1));
    pager_unpin(bt->pager, pid, false);

    bool child_empty = false;
    rc = delete_rec(bt, child, key, klen, rid, &child_empty);
    if (rc != OK)
        return rc;
    if (!child_empty) {
        *empty = false;
        return OK;
    }

    /* 孩子空了：从本节点摘掉，页还给 freelist */
    rc = pager_get(bt->pager, pid, &f);
    if (rc != OK)
        return rc;

    if (pos == 0) {
        /* 空的是 leftmost：把第一个 item 的孩子提为 leftmost，删掉 item 0 */
        if (bt_nitems(f->data) == 0) {
            *empty = true;
            pager_unpin(bt->pager, pid, false);
            freelist_set(bt->fl, child, FL_USABLE);
            return OK;
        }
        set_leftmost(f->data, (u32)item_val(f->data, 0));
        remove_at(f->data, 0);
    } else {
        remove_at(f->data, (u16)(pos - 1));
    }
    freelist_set(bt->fl, child, FL_USABLE);

    *empty = (bt_nitems(f->data) == 0);
    pager_unpin(bt->pager, pid, true);
    return OK;
}

int btree_delete(BTree *bt, const void *key, u16 klen, Rid rid)
{
    if (!bt || !key || klen == 0 || klen > BT_MAX_KEY)
        return E_INVAL;

    bt_lock(bt);

    bool empty = false;
    int rc = delete_rec(bt, bt->root, (const u8 *)key, klen, rid_to_u64(rid), &empty);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    /* 根降高：根是内部节点且只剩 leftmost 时，把唯一的孩子提上来 */
    for (;;) {
        PageFrame *f;
        rc = pager_get(bt->pager, bt->root, &f);
        if (rc != OK) {
            bt_unlock(bt);
            return rc;
        }
        bool inner = (bt_type(f->data) == BT_INNER);
        u16 nit = bt_nitems(f->data);
        u32 lm = bt_leftmost(f->data);
        pager_unpin(bt->pager, bt->root, false);

        if (!inner || nit > 0)
            break;

        /* 根的 nitems == 0，唯一孩子 lm 上位 */
        u32 old = bt->root;
        bt->root = lm;
        bt->height--;
        freelist_set(bt->fl, old, FL_USABLE);
        if (bt->height == 0)
            break;
    }

    bt_unlock(bt);
    return OK;
}
