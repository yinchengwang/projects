/*
 * btree.c — B+tree
 *
 * 页格式（docs/PLAN-v0.3.md §1.2）：
 *   [header 28B][slot 数组 u16 ...][── free ──][条目区 ←]
 *
 * 三条最容易写错的规矩：
 *   1. 叶子分裂：提升的 key **两边都保留**
 *      内部节点分裂：提升的 key **不在任何一边**（被移走）
 *   2. 内部节点查找用 upper_bound（第一个 sep > key），走 pos-1 的孩子
 *   3. compact 必须借 scratch buffer 重排，不能原地搬
 */

#include "index/btree.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "common/latch.h"

/* ---------------- 页格式 ---------------- */

#define BT_HDR       28u
#define BT_TYPE      0u
#define BT_FLAGS     1u
#define BT_NITEMS    2u
#define BT_FREEOFF   4u
#define BT_FREEBYTES 6u
#define BT_RIGHT     8u
#define BT_LEFTMOST  12u
#define BT_LEVEL     16u
#define BT_LSN       20u
#define BT_CKSUM     24u

#define BT_INNER 0x03u
#define BT_LEAF  0x04u

/*
 * 索引 key 的格式版本（存在页头的 BT_FLAGS 字节里）。
 *
 * 【为什么需要它】
 * 索引 key 的编码是可以变的（v1.1 就把「只有列值」改成了「列值 + RID」）。
 * 变了之后，磁盘上**已有的**索引页里存的是旧格式的 key ——
 * 拿新代码去读，结果是错的，而且不报错。
 *
 * 所以页头里记一个版本号：版本对不上就重建这个索引。
 * 索引本来就是派生物（能从堆重建），重建一次没有语义风险。
 *
 * 版本 1 = 旧格式（key 只有列值，有重复值丢行的 bug）
 * 版本 2 = 复合键（列值 + RID）
 */
#define BT_FMTVER 2u

struct BTree {
    Pager    *pager;
    FreeList *fl;
    u32       root;
    u32       height;
    u32       npages;
    u8        scratch[PAGE_SIZE];   /* compact 用，避免原地搬 */

    /*
     * ==================== v1.2 步骤 4d：btree 域的锁 ====================
     *
     * 【为什么 B+tree 必须有自己的锁】
     *
     * 树结构本身就是**共享可变状态**，而且缺一不可：
     *
     *   - `root` / `height`：根分裂要换根，根降高要换根
     *   - `scratch[]`：**compact 用的整页暂存区** —— 两个线程同时用它
     *     就是两个人往同一块内存里搬数据 → 页内容直接乱掉
     *   - **分裂本身不是原子的**：`insert_rec` 先在孩子页插，
     *     再回父亲页插分隔键 —— 这两步之间如果另一个线程也在分裂，
     *     父亲页的条目就可能重复或丢失
     *
     * 大锁在的时候，这些全被顺带护住了。
     * 大锁一撤（步骤 4d），实测**并发写索引会丢索引项**：
     * 1200 行并发插入，堆里 1200 行全在，点查却能漏掉 200 多行 ——
     * **静默的数据不一致**（行在，索引找不到）。
     *
     * 【为什么是"每棵树一把"】
     *
     * 锁的粒度 = 索引的粒度。不同索引互不阻塞，
     * 同一索引上的写才排队 —— 这正是索引该有的并发度。
     *
     * 【锁的全序】
     *
     * btree 在全序里排第 3（catalog → txn → btree → frame → wal → freelist）。
     * 因为树的操作要拿页（`pager_get` → frame 域），
     * 所以 btree 比 frame 更外层 ✓
     *
     * 反过来没有边：**没有任何路径是"持有页锁再去取树锁"的** ——
     * 堆的写路径是 `heap_insert`（页锁在里面取好又放掉）
     * 然后才 `index_insert_row`（树锁），两步是**先后**不是嵌套 ✓
     */
    pthread_mutex_t mu;
};

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → btree → frame → wal → freelist"这条全序
 * 从步骤 2 起就是**机器检查**的。
 */
static void bt_lock(BTree *bt)
{
    pthread_mutex_lock(&bt->mu);
    latch_order_acquire(LATCH_BTREE);
}

static void bt_unlock(BTree *bt)
{
    latch_order_release(LATCH_BTREE);
    pthread_mutex_unlock(&bt->mu);
}

/* ---------------- header / slot 访问 ---------------- */

static u8   bt_type(const u8 *pg)      { return pg[BT_TYPE]; }
static u16  bt_nitems(const u8 *pg)    { return get_u16(pg + BT_NITEMS); }
static u16  bt_free_off(const u8 *pg)  { return get_u16(pg + BT_FREEOFF); }
static u16  bt_free_bytes(const u8 *pg){ return get_u16(pg + BT_FREEBYTES); }
static u32  bt_right(const u8 *pg)     { return get_u32(pg + BT_RIGHT); }
static u32  bt_leftmost(const u8 *pg)  { return get_u32(pg + BT_LEFTMOST); }
static u32  bt_level(const u8 *pg)     { return get_u32(pg + BT_LEVEL); }

static void set_nitems(u8 *pg, u16 v)  { put_u16(pg + BT_NITEMS, v); }
static void set_right(u8 *pg, u32 v)   { put_u32(pg + BT_RIGHT, v); }
static void set_leftmost(u8 *pg, u32 v){ put_u32(pg + BT_LEFTMOST, v); }
static void set_level(u8 *pg, u32 v)   { put_u32(pg + BT_LEVEL, v); }

static u16 bt_slot(const u8 *pg, u16 i)      { return get_u16(pg + BT_HDR + 2u * i); }
static void set_slot(u8 *pg, u16 i, u16 off) { put_u16(pg + BT_HDR + 2u * i, off); }

/* free_bytes 只算 slot 区和条目区之间的连续空隙，不含删除留下的洞 */
static void recalc_free(u8 *pg)
{
    u16 fo = bt_free_off(pg);
    put_u16(pg + BT_FREEOFF, fo);
    put_u16(pg + BT_FREEBYTES, (u16)(fo - BT_HDR - 2u * bt_nitems(pg)));
}

static void bt_checksum(u8 *pg)
{
    put_u16(pg + BT_CKSUM, 0);
    put_u16(pg + BT_CKSUM, crc16(pg, PAGE_SIZE));
}

/* ---------------- 条目访问 ---------------- */

static u16 item_len(const u8 *pg, u16 i)
{
    u16 off = bt_slot(pg, i);
    u16 klen = get_u16(pg + off);
    return (u16)(2u + klen + (bt_type(pg) == BT_LEAF ? 8u : 4u));
}

static const u8 *item_key(const u8 *pg, u16 i, u16 *klen)
{
    u16 off = bt_slot(pg, i);
    u16 kl = get_u16(pg + off);
    *klen = kl;
    return pg + off + 2;
}

static u64 item_val(const u8 *pg, u16 i)
{
    u16 off = bt_slot(pg, i);
    u16 kl = get_u16(pg + off);
    if (bt_type(pg) == BT_LEAF)
        return get_u64(pg + off + 2 + kl);
    return (u64)get_u32(pg + off + 2 + kl);
}

static int key_cmp(const u8 *a, u16 alen, const u8 *b, u16 blen)
{
    u16 n = alen < blen ? alen : blen;
    int c = memcmp(a, b, n);
    if (c != 0)
        return c;
    if (alen == blen)
        return 0;
    return alen < blen ? -1 : 1;
}

/* 第一个 >= key 的位置 */
static u16 lower_bound(const u8 *pg, const u8 *key, u16 klen)
{
    u16 lo = 0, hi = bt_nitems(pg);
    while (lo < hi) {
        u16 mid = (u16)((lo + hi) / 2);
        u16 kl;
        const u8 *k = item_key(pg, mid, &kl);
        if (key_cmp(k, kl, key, klen) < 0)
            lo = (u16)(mid + 1);
        else
            hi = mid;
    }
    return lo;
}

/* 第一个 > key 的位置 */
static u16 upper_bound(const u8 *pg, const u8 *key, u16 klen)
{
    u16 lo = 0, hi = bt_nitems(pg);
    while (lo < hi) {
        u16 mid = (u16)((lo + hi) / 2);
        u16 kl;
        const u8 *k = item_key(pg, mid, &kl);
        if (key_cmp(k, kl, key, klen) <= 0)
            lo = (u16)(mid + 1);
        else
            hi = mid;
    }
    return lo;
}

/* ---------------- 页初始化 / 整理 ---------------- */

static int bt_init(u8 *pg, u8 type, u32 level)
{
    memset(pg, 0, PAGE_SIZE);
    pg[BT_TYPE] = type;
    pg[BT_FLAGS] = BT_FMTVER;      /* 索引 key 的格式版本 */
    set_nitems(pg, 0);
    put_u16(pg + BT_FREEOFF, (u16)PAGE_SIZE);
    put_u16(pg + BT_FREEBYTES, (u16)(PAGE_SIZE - BT_HDR));
    set_right(pg, PAGE_ID_NIL);
    set_leftmost(pg, PAGE_ID_NIL);
    set_level(pg, level);
    bt_checksum(pg);
    return OK;
}

/*
 * 整理：按 slot 顺序把条目重新排到页尾。
 * 借 scratch 中转 —— 原地搬会出现"覆盖还没搬的条目"。
 */
static void bt_compact(BTree *bt, u8 *pg)
{
    u16 n = bt_nitems(pg);
    if (n == 0) {
        put_u16(pg + BT_FREEOFF, (u16)PAGE_SIZE);
        recalc_free(pg);
        bt_checksum(pg);
        return;
    }

    u16 fo = bt_free_off(pg);
    u8 *tmp = bt->scratch;
    memcpy(tmp, pg + fo, (size_t)(PAGE_SIZE - fo));

    u32 dst = PAGE_SIZE;
    for (u16 i = 0; i < n; i++) {
        u16 old_off = bt_slot(pg, i);
        u16 len = item_len(pg, i);
        dst -= len;
        memcpy(pg + dst, tmp + (old_off - fo), len);
        set_slot(pg, i, (u16)dst);
    }

    put_u16(pg + BT_FREEOFF, (u16)dst);
    recalc_free(pg);
    bt_checksum(pg);
}

/* 在第 pos 个位置插入一项 */
static int insert_at(BTree *bt, u8 *pg, u16 pos,
                     const u8 *key, u16 klen, u64 val)
{
    u16 n = bt_nitems(pg);
    if (pos > n)
        return E_INVAL;
    if (klen == 0 || klen > BT_MAX_KEY)
        return E_INVAL;

    bool leaf = (bt_type(pg) == BT_LEAF);
    u16 len = (u16)(2u + klen + (leaf ? 8u : 4u));
    u16 need = (u16)(len + 2u);

    if (bt_free_bytes(pg) < need) {
        bt_compact(bt, pg);
        if (bt_free_bytes(pg) < need)
            return E_NOSPACE;
    }

    /* slot 数组右移腾位置 */
    memmove(pg + BT_HDR + 2u * (pos + 1),
            pg + BT_HDR + 2u * pos,
            (size_t)(2u * (n - pos)));

    u16 off = (u16)(bt_free_off(pg) - len);
    put_u16(pg + off, klen);
    memcpy(pg + off + 2, key, klen);
    if (leaf)
        put_u64(pg + off + 2 + klen, val);
    else
        put_u32(pg + off + 2 + klen, (u32)val);

    set_slot(pg, pos, off);
    set_nitems(pg, (u16)(n + 1));
    put_u16(pg + BT_FREEOFF, off);
    recalc_free(pg);
    bt_checksum(pg);
    return OK;
}

static int remove_at(u8 *pg, u16 pos)
{
    u16 n = bt_nitems(pg);
    if (pos >= n)
        return E_INVAL;

    memmove(pg + BT_HDR + 2u * pos,
            pg + BT_HDR + 2u * (pos + 1),
            (size_t)(2u * (n - pos - 1)));

    set_nitems(pg, (u16)(n - 1));
    recalc_free(pg);       /* 洞不计入 free_bytes，等 compact 回收 */
    bt_checksum(pg);
    return OK;
}

/* ---------------- 页分配 / 回收 ---------------- */

static int bt_alloc_page(BTree *bt, u8 type, u32 level,
                         u32 *pid_out, PageFrame **f_out)
{
    u32 pid = freelist_find(bt->fl, FL_USABLE);
    PageFrame *f = NULL;
    int rc;

    if (pid != PAGE_ID_NIL) {
        rc = pager_get(bt->pager, pid, &f);
        if (rc != OK)
            return rc;
    } else {
        rc = pager_alloc(bt->pager, &pid, &f);
        if (rc != OK)
            return rc;
        rc = freelist_grow(bt->fl, pid + 1);
        if (rc != OK) {
            pager_unpin(bt->pager, pid, false);
            return rc;
        }
        bt->npages++;          /* 只统计从文件新扩的页 */
    }

    bt_init(f->data, type, level);
    freelist_set_full(bt->fl, pid);

    *pid_out = pid;
    *f_out = f;
    return OK;
}

/* ---------------- 查找 ---------------- */

static int descend(BTree *bt, const u8 *key, u16 klen, u32 *leaf_out)
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

/* ---------------- 分裂 ---------------- */

typedef struct {
    u8  sep[BT_MAX_KEY];
    u16 sep_len;
    u32 right;
    bool split;
} Split;

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
static BTree *bt_mut(const BTree *bt)
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
