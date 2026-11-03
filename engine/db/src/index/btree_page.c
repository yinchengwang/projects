/*
 * btree_page.c — 页内原语与页操作
 *
 * 本文件负责"页"这一层的全部原语：
 *
 *   - 页头 / slot 数组的读写
 *   - 条目（key + value）的读取与比较
 *   - 二分查找（lower_bound / upper_bound）
 *   - 页的初始化、compact、插入、删除
 *   - 从 freelist 分配一个新页
 *
 * 这一层**不认识树**（不知道根在哪、有几层）——
 * 树结构的事在 btree_search.c / btree_insert.c / btree_delete.c 里。
 *
 * 共享的宏与类型在 index/btree_internal.h。
 */

#include "index/btree_internal.h"

#include <string.h>

#include "common/crc.h"

/* ---------------- header / slot 访问 ---------------- */

u8   bt_type(const u8 *pg)      { return pg[BT_TYPE]; }
u16  bt_nitems(const u8 *pg)    { return get_u16(pg + BT_NITEMS); }
u16  bt_free_off(const u8 *pg)  { return get_u16(pg + BT_FREEOFF); }
u16  bt_free_bytes(const u8 *pg){ return get_u16(pg + BT_FREEBYTES); }
u32  bt_right(const u8 *pg)     { return get_u32(pg + BT_RIGHT); }
u32  bt_leftmost(const u8 *pg)  { return get_u32(pg + BT_LEFTMOST); }
u32  bt_level(const u8 *pg)     { return get_u32(pg + BT_LEVEL); }

void set_nitems(u8 *pg, u16 v)  { put_u16(pg + BT_NITEMS, v); }
void set_right(u8 *pg, u32 v)   { put_u32(pg + BT_RIGHT, v); }
void set_leftmost(u8 *pg, u32 v){ put_u32(pg + BT_LEFTMOST, v); }
void set_level(u8 *pg, u32 v)   { put_u32(pg + BT_LEVEL, v); }

u16  bt_slot(const u8 *pg, u16 i)      { return get_u16(pg + BT_HDR + 2u * i); }
void set_slot(u8 *pg, u16 i, u16 off) { put_u16(pg + BT_HDR + 2u * i, off); }

/* free_bytes 只算 slot 区和条目区之间的连续空隙，不含删除留下的洞 */
void recalc_free(u8 *pg)
{
    u16 fo = bt_free_off(pg);
    put_u16(pg + BT_FREEOFF, fo);
    put_u16(pg + BT_FREEBYTES, (u16)(fo - BT_HDR - 2u * bt_nitems(pg)));
}

void bt_checksum(u8 *pg)
{
    put_u16(pg + BT_CKSUM, 0);
    put_u16(pg + BT_CKSUM, crc16(pg, PAGE_SIZE));
}

/* ---------------- 条目访问 ---------------- */

u16 item_len(const u8 *pg, u16 i)
{
    u16 off = bt_slot(pg, i);
    u16 klen = get_u16(pg + off);
    return (u16)(2u + klen + (bt_type(pg) == BT_LEAF ? 8u : 4u));
}

const u8 *item_key(const u8 *pg, u16 i, u16 *klen)
{
    u16 off = bt_slot(pg, i);
    u16 kl = get_u16(pg + off);
    *klen = kl;
    return pg + off + 2;
}

u64 item_val(const u8 *pg, u16 i)
{
    u16 off = bt_slot(pg, i);
    u16 kl = get_u16(pg + off);
    if (bt_type(pg) == BT_LEAF)
        return get_u64(pg + off + 2 + kl);
    return (u64)get_u32(pg + off + 2 + kl);
}

int key_cmp(const u8 *a, u16 alen, const u8 *b, u16 blen)
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
u16 lower_bound(const u8 *pg, const u8 *key, u16 klen)
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
u16 upper_bound(const u8 *pg, const u8 *key, u16 klen)
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

int bt_init(u8 *pg, u8 type, u32 level)
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
void bt_compact(BTree *bt, u8 *pg)
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
int insert_at(BTree *bt, u8 *pg, u16 pos,
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

int remove_at(u8 *pg, u16 pos)
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

int bt_alloc_page(BTree *bt, u8 type, u32 level,
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
