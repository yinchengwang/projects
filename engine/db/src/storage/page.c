/*
 * page.c — slotted page
 *
 * 布局见 DESIGN.md §3：
 *   [header 20B][slot 数组 ↓][──── free ────][记录区 ↑]
 *
 * 两条铁律（写错任何一条，索引就会指向错数据，而且不报错）：
 *   1. slot 编号永不复用
 *   2. compact 必须从后往前压
 */

#include "storage/page.h"

#include "common/crc.h"

#include <string.h>

/* ---------------- header 读写 ---------------- */

static inline u16 hdr_slots(const u8 *pg)      { return get_u16(pg + H_SLOTS); }
static inline u16 hdr_free_off(const u8 *pg)   { return get_u16(pg + H_FREE_OFF); }
static inline u16 hdr_free_bytes(const u8 *pg) { return get_u16(pg + H_FREE_BYTES); }

static inline void set_slots(u8 *pg, u16 v)      { put_u16(pg + H_SLOTS, v); }
static inline void set_free_off(u8 *pg, u16 v)   { put_u16(pg + H_FREE_OFF, v); }
static inline void set_free_bytes(u8 *pg, u16 v) { put_u16(pg + H_FREE_BYTES, v); }

/* ---------------- slot 读写 ---------------- */

#define SLOT_POS(i) ((u32)HDR_SIZE + (u32)(i) * SLOT_SIZE)

static inline u16 slot_off(const u8 *pg, u16 i) { return get_u16(pg + SLOT_POS(i)); }
static inline u16 slot_len(const u8 *pg, u16 i) { return get_u16(pg + SLOT_POS(i) + 2); }

static inline void slot_set(u8 *pg, u16 i, u16 off, u16 len)
{
    put_u16(pg + SLOT_POS(i), off);
    put_u16(pg + SLOT_POS(i) + 2, len);
}

/* ---------------- 校验 ---------------- */

void page_set_checksum(u8 *pg)
{
    put_u16(pg + H_CKSUM, 0);
    put_u16(pg + H_CKSUM, crc16(pg, PAGE_SIZE));
}

bool page_verify_checksum(const u8 *pg)
{
    static const u8 zero2[2] = { 0, 0 };
    u16 stored = get_u16(pg + H_CKSUM);
    u16 crc = 0xFFFFu;
    crc = crc16_update(crc, pg, H_CKSUM);
    crc = crc16_update(crc, zero2, 2);                 /* 把 cksum 字段当 0 */
    crc = crc16_update(crc, pg + H_CKSUM + 2, PAGE_SIZE - H_CKSUM - 2);
    return crc == stored;
}

/* ---------------- 初始化 ---------------- */

int page_init(u8 *pg, u8 page_type)
{
    if (!pg)
        return E_INVAL;

    memset(pg, 0, PAGE_SIZE);
    pg[H_TYPE] = page_type;
    pg[H_FLAGS] = 0;
    set_slots(pg, 0);
    set_free_off(pg, (u16)PAGE_SIZE);
    set_free_bytes(pg, (u16)(PAGE_SIZE - HDR_SIZE));
    put_u32(pg + H_NEXT, PAGE_ID_NIL);
    put_u32(pg + H_LSN, 0);
    page_set_checksum(pg);
    return OK;
}

/* ---------------- 插入 ---------------- */

int page_insert(u8 *pg, const u8 *rec, u16 len, u16 *slot_out)
{
    if (!pg || !rec || !slot_out)
        return E_INVAL;
    if (len == 0)
        return E_INVAL;          /* 零长记录会和"已删除"混淆 */

    u32 need = (u32)len + SLOT_SIZE;
    if (need > PAGE_SIZE - HDR_SIZE)
        return E_NOSPACE;        /* 单条就塞不下 */

    if (hdr_free_bytes(pg) < need) {
        /* 有碎片就先整理，再试一次（决策 B：只在必要时 compact） */
        if (page_fragmentation(pg) > 0)
            page_compact(pg);
        if (hdr_free_bytes(pg) < need)
            return E_NOSPACE;
    }

    u16 slots = hdr_slots(pg);
    u16 off = (u16)(hdr_free_off(pg) - len);
    memcpy(pg + off, rec, len);

    set_free_off(pg, off);
    slot_set(pg, slots, off, len);
    set_slots(pg, (u16)(slots + 1));
    set_free_bytes(pg, (u16)(hdr_free_bytes(pg) - need));
    page_set_checksum(pg);

    *slot_out = slots;
    return OK;
}

/* ---------------- 读取 ---------------- */

int page_get(const u8 *pg, u16 slot, const u8 **rec, u16 *len)
{
    if (!pg || !rec || !len)
        return E_INVAL;
    if (slot >= hdr_slots(pg))
        return E_NOTFOUND;

    u16 off = slot_off(pg, slot);
    u16 l = slot_len(pg, slot);
    if (l == 0)
        return E_NOTFOUND;                       /* 已删除 */
    if (off < HDR_SIZE || (u32)off + l > PAGE_SIZE)
        return E_CORRUPT;                        /* 页被外部破坏 */

    *rec = pg + off;
    *len = l;
    return OK;
}

/* ---------------- 改写 ---------------- */

int page_update(u8 *pg, u16 slot, const u8 *rec, u16 len)
{
    if (!pg || !rec)
        return E_INVAL;
    if (slot >= hdr_slots(pg))
        return E_NOTFOUND;

    u16 off = slot_off(pg, slot);
    u16 old = slot_len(pg, slot);
    if (old == 0)
        return E_NOTFOUND;
    if (len == 0)
        return E_INVAL;
    if (len > old)
        return E_NOSPACE;    /* 变长更新请走 delete + insert（RID 会变） */

    memmove(pg + off, rec, len);
    slot_set(pg, slot, off, len);    /* 缩短留下的尾部空洞计入碎片 */
    page_set_checksum(pg);
    return OK;
}

/* ---------------- 删除 ---------------- */

int page_delete(u8 *pg, u16 slot)
{
    if (!pg)
        return E_INVAL;
    if (slot >= hdr_slots(pg))
        return E_NOTFOUND;

    u16 off = slot_off(pg, slot);
    if (slot_len(pg, slot) == 0)
        return E_NOTFOUND;

    /* 保留 offset 便于调试，长度置 0 表示删除。
     * slot 编号永久保留 —— 铁律 1。 */
    slot_set(pg, slot, off, 0);
    page_set_checksum(pg);
    return OK;
}

/* ---------------- 碎片整理 ---------------- */

void page_compact(u8 *pg)
{
    if (!pg)
        return;

    u16 slots = hdr_slots(pg);
    u32 dst = PAGE_SIZE;

    /*
     * 按 slot 升序处理 —— 这一点反直觉，但必须这样：
     *
     * 记录总是从页尾往下长，所以「slot 号越大，offset 越小」。
     * 打包到页尾时要按 offset 从大到小处理（高地址的先归位），
     * 也就是 slot 升序。
     *
     * 这样每条记录的目的地址 dst 都 >= 原地址 off，只向上移动；
     * 而未处理的记录全在更低的地址，绝不会被覆盖。
     *
     * 反例（slot 倒序会踩）：
     *   slot0 R1@3996(100)  slot1 R3@3796(100)  slot2 R4@3596(200)
     *   倒序先搬 R4 → dst=3896，搬到 3896..4095，正好盖掉还没搬的 R1。
     */
    for (u16 i = 0; i < slots; i++) {
        u16 l = slot_len(pg, i);
        if (l == 0)
            continue;
        u16 off = slot_off(pg, i);
        dst -= l;
        if (dst != off)
            memmove(pg + dst, pg + off, l);
        slot_set(pg, i, (u16)dst, l);
    }

    set_free_off(pg, (u16)dst);
    set_free_bytes(pg, (u16)(dst - HDR_SIZE - (u32)slots * SLOT_SIZE));
    page_set_checksum(pg);
}

/* ---------------- 统计 ---------------- */

u16 page_free_bytes(const u8 *pg)
{
    return pg ? hdr_free_bytes(pg) : 0;
}

u16 page_used_bytes(const u8 *pg)
{
    return pg ? (u16)(PAGE_SIZE - hdr_free_off(pg)) : 0;
}

u16 page_live_bytes(const u8 *pg)
{
    if (!pg)
        return 0;
    u16 slots = hdr_slots(pg);
    u32 sum = 0;
    for (u16 i = 0; i < slots; i++)
        sum += slot_len(pg, i);
    return (u16)sum;
}

u16 page_fragmentation(const u8 *pg)
{
    u16 used = page_used_bytes(pg);
    if (used == 0)
        return 0;
    u16 live = page_live_bytes(pg);
    return (u16)(((u32)(used - live) * 100u) / used);
}

u16 page_slot_count(const u8 *pg)
{
    return pg ? hdr_slots(pg) : 0;
}

bool page_slot_alive(const u8 *pg, u16 slot)
{
    if (!pg || slot >= hdr_slots(pg))
        return false;
    return slot_len(pg, slot) != 0;
}

/* ---------------- 页归属 ---------------- */

u16 page_table_id(const u8 *pg)
{
    return pg ? get_u16(pg + H_TID) : 0;
}

void page_set_table_id(u8 *pg, u16 tid)
{
    if (!pg)
        return;
    put_u16(pg + H_TID, tid);
    page_set_checksum(pg);
}

/* ---------------- WAL 用 ---------------- */

u32 page_lsn(const u8 *pg)
{
    return pg ? get_u32(pg + H_LSN) : 0;
}

void page_set_lsn(u8 *pg, u32 lsn)
{
    if (!pg)
        return;
    put_u32(pg + H_LSN, lsn);
    page_set_checksum(pg);
}

u16 page_slot_offset(const u8 *pg, u16 slot)
{
    if (!pg || slot >= hdr_slots(pg))
        return PAGE_SLOT_NONE;
    if (slot_len(pg, slot) == 0)
        return PAGE_SLOT_NONE;      /* 已删除 */
    return slot_off(pg, slot);
}

int page_insert_at(u8 *pg, u16 slot, const u8 *rec, u16 len)
{
    if (!pg || !rec || len == 0)
        return E_INVAL;

    u16 slots = hdr_slots(pg);

    /* slot 必须正好是"下一个" —— 不能跳过，否则中间的 slot 全是空洞 */
    if (slot < slots)
        return E_INVAL;

    /* slot 可以比 slots 大一（跳过空洞），但不能跳更多 */
    if (slot > slots + 1)
        return E_INVAL;

    if (slot > slots) {
        /* 中间跳过了一个 slot：那个 slot 是空洞，永远不会有记录 */
        slots = slot;
    }

    u32 need = (u32)len + SLOT_SIZE;
    if (need > PAGE_SIZE - HDR_SIZE)
        return E_NOSPACE;

    if (hdr_free_bytes(pg) < need) {
        if (page_fragmentation(pg) > 0)
            page_compact(pg);
        if (hdr_free_bytes(pg) < need)
            return E_NOSPACE;
    }

    u16 off = (u16)(hdr_free_off(pg) - len);
    memcpy(pg + off, rec, len);

    set_free_off(pg, off);
    slot_set(pg, slot, off, len);
    set_slots(pg, (u16)(slot + 1));
    set_free_bytes(pg, (u16)(hdr_free_bytes(pg) - need));
    page_set_checksum(pg);
    return OK;
}

int page_restore(u8 *pg, u16 slot, const u8 *rec, u16 len)
{
    if (!pg || !rec || len == 0)
        return E_INVAL;

    u16 slots = hdr_slots(pg);
    if (slot > slots)
        return E_INVAL;

    if (slot == slots) {
        u16 s;
        return page_insert(pg, rec, len, &s);
    }

    /* 覆盖已有 slot：必须本来是空的 */
    if (slot_len(pg, slot) != 0)
        return E_INVAL;

    if (hdr_free_bytes(pg) < len) {
        if (page_fragmentation(pg) > 0)
            page_compact(pg);
        if (hdr_free_bytes(pg) < len)
            return E_NOSPACE;
    }

    u16 off = (u16)(hdr_free_off(pg) - len);
    memcpy(pg + off, rec, len);
    set_free_off(pg, off);
    slot_set(pg, slot, off, len);
    set_free_bytes(pg, (u16)(hdr_free_bytes(pg) - len));
    page_set_checksum(pg);
    return OK;
}

int page_restore_at(u8 *pg, u16 slot, u16 expect_off, const u8 *rec, u16 len)
{
    if (!pg || !rec || len == 0)
        return E_INVAL;

    u16 slots = hdr_slots(pg);
    if (slot > slots)
        return E_INVAL;

    /* 追加位：直接走 page_restore */
    if (slot == slots)
        return page_restore(pg, slot, rec, len);

    u16 cur_len = slot_len(pg, slot);

    if (cur_len != 0) {
        /*
         * slot 里有活记录 —— 这是一次原地 UPDATE 的 undo。
         * 必须确认记录还在原来的偏移上：
         * 页被 compact 过的话记录挪了位置，原地回写会写坏别的 slot。
         */
        if (expect_off == PAGE_SLOT_NONE)
            return E_NOTFOUND;      /* 不知道原来在哪，不敢动 */
        if (slot_off(pg, slot) != expect_off)
            return E_NOTFOUND;      /* 挪过位置了，放弃这次 undo */

        if (len > cur_len)
            return E_NOSPACE;       /* 回写不进去：原记录比镜像长 */

        memmove(pg + expect_off, rec, len);
        slot_set(pg, slot, expect_off, len);
        /* 缩短留下的洞由 compact 回收 */
        page_set_checksum(pg);
        return OK;
    }

    /* slot 是空的：撤销删除 —— 把 before 镜像填回原 slot */
    return page_restore(pg, slot, rec, len);
}

/* ---------------- 叶子链 ---------------- */

u32 page_next(const u8 *pg)
{
    return pg ? get_u32(pg + H_NEXT) : PAGE_ID_NIL;
}

void page_set_next(u8 *pg, u32 next_page)
{
    if (!pg)
        return;
    put_u32(pg + H_NEXT, next_page);
    page_set_checksum(pg);
}
