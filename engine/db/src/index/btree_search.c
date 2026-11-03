/*
 * btree_search.c — 树的下降与精确查找
 *
 * 本文件负责"从根走到叶子"这条路径，以及在叶子上的精确查找。
 *
 * descend 是这一层唯一的入口：
 *   内部节点用 upper_bound（第一个 sep > key），走 pos-1 的孩子；
 *   pos == 0 时走 leftmost 指向的最左孩子。
 */

#include "index/btree_internal.h"

/* ---------------- 查找 ---------------- */

int descend(BTree *bt, const u8 *key, u16 klen, u32 *leaf_out)
{
    u32 pid = bt->root;
    for (;;) {
        PageFrame *f;
        int rc = pager_get(bt->pager, pid, &f);
        if (rc != OK)
            return rc;

        if (bt_type(f->data) == BT_LEAF) {
            pager_unpin(bt->pager, pid, false);
            *leaf_out = pid;
            return OK;
        }

        /* upper_bound：第一个 sep > key 的位置，走 pos-1 的孩子 */
        u16 pos = upper_bound(f->data, key, klen);
        u32 child = (pos == 0) ? bt_leftmost(f->data)
                               : (u32)item_val(f->data, (u16)(pos - 1));
        pager_unpin(bt->pager, pid, false);
        pid = child;
    }
}

int btree_search(BTree *bt, const void *key, u16 klen, Rid *out, int max, int *n)
{
    if (!bt || !key || !out || !n || klen == 0 || klen > BT_MAX_KEY)
        return E_INVAL;

    bt_lock(bt);

    u32 leaf;
    int rc = descend(bt, (const u8 *)key, klen, &leaf);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    PageFrame *f;
    rc = pager_get(bt->pager, leaf, &f);
    if (rc != OK) {
        bt_unlock(bt);
        return rc;
    }

    u16 pos = lower_bound(f->data, (const u8 *)key, klen);
    int cnt = 0;
    u16 nit = bt_nitems(f->data);
    while (pos < nit) {
        u16 kl;
        const u8 *k = item_key(f->data, pos, &kl);
        if (key_cmp(k, kl, (const u8 *)key, klen) != 0)
            break;
        if (cnt < max)
            out[cnt] = rid_from_u64(item_val(f->data, pos));
        cnt++;
        pos++;
    }
    pager_unpin(bt->pager, leaf, false);

    *n = cnt;
    bt_unlock(bt);
    return cnt > 0 ? OK : E_NOTFOUND;
}
