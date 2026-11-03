/*
 * btree_cursor.c — 范围扫描游标
 *
 * 游标沿着叶子链（`bt_right`）一页一页往后走。
 *
 * 【游标为什么必须持有树锁】
 * 游标要跨多次 `btree_cursor_next` 才能扫完，中途放锁的话，
 * 并发写可能把叶子劈开 —— 游标记住的"下一个叶子"是劈开**之前**的右兄弟，
 * 劈开后多出来的那半个叶子就被**跳过**了 → **静默少行**。
 *
 * 所以锁从 `btree_scan` 取，一直持有到 `btree_cursor_close`。
 */

#include "index/btree_internal.h"

#include <stdlib.h>
#include <string.h>

/* ---------------- 扫描器 ---------------- */

struct BtCursor {
    BTree *bt;
    u32    leaf;
    u16    pos;
    bool   has_hi;
    u8     hi[BT_MAX_KEY];
    u16    hilen;

    /* 下界。换了叶子要重新定位 —— 见 btree_cursor_next 的说明 */
    bool   has_lo;
    u8     lo[BT_MAX_KEY];
    u16    lolen;
    bool   reseek;
};

static int cursor_seek(BtCursor *c)
{
    /* 定位到叶子的第一个位置 */
    PageFrame *f;
    int rc = pager_get(c->bt->pager, c->leaf, &f);
    if (rc != OK)
        return rc;
    c->pos = 0;
    pager_unpin(c->bt->pager, c->leaf, false);
    return OK;
}

int btree_scan(BTree *bt, const void *lo, u16 lolen,
               const void *hi, u16 hilen, BtCursor **out)
{
    if (!bt || !out)
        return E_INVAL;
    if (lo && (lolen == 0 || lolen > BT_MAX_KEY))
        return E_INVAL;
    if (hi && (hilen == 0 || hilen > BT_MAX_KEY))
        return E_INVAL;

    BtCursor *c = calloc(1, sizeof(*c));
    if (!c)
        return E_IO;
    c->bt = bt;

    /*
     * 【树锁在这里取，并且交给游标一直持有】
     *
     * 游标要跨多次 `btree_cursor_next` 才能扫完。
     * 中途放锁的话，并发写可能把叶子劈开：
     * 游标记住的"下一个叶子"是劈开**之前**的右兄弟 ——
     * 劈开后多出来的那半个叶子就被**跳过**了 → **静默少行**。
     *
     * 所以锁要一直持有到 `btree_cursor_close`。
     * 代价：同一棵索引上的写要等这次扫描扫完 ——
     * 这正是大锁时代的行为，只是粒度从"整个库"缩到了"这一棵索引"。
     *
     * 【契约：必须 close】
     * 不 close 就等于把这棵树的锁永久占着（后面的写全堵住）。
     * 执行器与 SPI 适配层都在 `scan_end` 里 close ✓
     */
    bt_lock(bt);

    if (hi) {
        memcpy(c->hi, hi, hilen);
        c->hilen = hilen;
        c->has_hi = true;
    }

    u32 leaf;
    int rc;
    if (lo) {
        memcpy(c->lo, lo, lolen);
        c->lolen = lolen;
        c->has_lo = true;

        rc = descend(bt, (const u8 *)lo, lolen, &leaf);
        if (rc != OK) {
            free(c);
            bt_unlock(bt);
            return rc;
        }
        c->leaf = leaf;
        rc = cursor_seek(c);
        if (rc != OK) {
            free(c);
            bt_unlock(bt);
            return rc;
        }
        /* 跳到第一个 >= lo */
        PageFrame *f;
        rc = pager_get(bt->pager, c->leaf, &f);
        if (rc != OK) {
            free(c);
            bt_unlock(bt);
            return rc;
        }
        c->pos = lower_bound(f->data, (const u8 *)lo, lolen);
        /*
         * 【这里不能因为"这一页没有 lo"就放弃扫描】
         *
         * descend 找到的是"下界落在哪棵子树"，不是"答案在哪个叶子"。
         * 下界可能正好落在两棵子树的**缝隙**里 ——
         * 比如下界是「值 + 最小 RID」，而树里的分隔符是「值 + 实际最小 RID」，
         * 前者比后者小，于是 descend 走到了左子树，而答案在右子树。
         *
         * 这种情况以前是"扫不到就返回空集" —— 结果就是**静默少行**。
         * 现在交给游标：它会一路往后走，每换一个叶子就重新定位下界。
         */
        pager_unpin(bt->pager, c->leaf, false);
    } else {
        /* 无下界：从最左叶子开始 */
        u32 pid = bt->root;
        for (;;) {
            PageFrame *f;
            rc = pager_get(bt->pager, pid, &f);
            if (rc != OK) {
                free(c);
                bt_unlock(bt);
                return rc;
            }
            if (bt_type(f->data) == BT_LEAF) {
                pager_unpin(bt->pager, pid, false);
                break;
            }
            u32 child = bt_leftmost(f->data);
            pager_unpin(bt->pager, pid, false);
            pid = child;
        }
        c->leaf = pid;
        rc = cursor_seek(c);
        if (rc != OK) {
            free(c);
            bt_unlock(bt);
            return rc;
        }
    }

    *out = c;
    return OK;              /* 树锁由游标持有，close 时才放 */
}

int btree_cursor_next(BtCursor *c, Rid *rid)
{
    if (!c || !rid)
        return E_INVAL;

    /*
     * 这里**不取树锁** —— 锁由 `btree_scan` 取、由 `btree_cursor_close` 放，
     * 中间一直由这个游标持有（理由见 btree_scan 的注释）。
     *
     * 推论：游标只能在**创建它的那个线程**里用。
     * 换线程用就等于在别人的锁上解锁 —— 未定义行为。
     */
    for (;;) {
        PageFrame *f;
        int rc = pager_get(c->bt->pager, c->leaf, &f);
        if (rc != OK)
            return rc;

        /*
         * 换过叶子 → 重新定位下界。
         *
         * descend 定位的是"下界落在哪棵子树"，答案可能在后面的叶子里
         * （下界落在子树缝隙里时）。所以每换一个叶子，都要在这一页里
         * 重新找"第一个 >= 下界"的位置 —— 这一页整页都小于下界就跳过它。
         */
        if (c->reseek) {
            c->reseek = false;
            c->pos = c->has_lo ? lower_bound(f->data, c->lo, c->lolen) : 0;
        }

        if (c->pos >= bt_nitems(f->data)) {
            /* 本页扫完了 → 下一个叶子 */
            u32 nxt = bt_right(f->data);
            pager_unpin(c->bt->pager, c->leaf, false);
            if (nxt == PAGE_ID_NIL)
                return E_EOF;
            c->leaf = nxt;
            c->pos = 0;
            c->reseek = true;
            continue;
        }

        u16 kl;
        const u8 *k = item_key(f->data, c->pos, &kl);
        if (c->has_hi && key_cmp(k, kl, c->hi, c->hilen) > 0) {
            pager_unpin(c->bt->pager, c->leaf, false);
            return E_EOF;
        }

        *rid = rid_from_u64(item_val(f->data, c->pos));
        c->pos++;
        pager_unpin(c->bt->pager, c->leaf, false);
        return OK;
    }
}

void btree_cursor_close(BtCursor *c)
{
    if (!c)
        return;
    /* 树锁是在 btree_scan 里取的，放在这里 —— 必须成对 */
    bt_unlock(c->bt);
    free(c);
}
