/*
 * freelist.c — 页空间索引
 *
 * 档位换算的单调性是这个模块的正确性基础：
 *   level = floor(free * 255 / USABLE)
 *   need_level = ceil(need * 255 / USABLE)
 * 于是 level >= need_level  ⟹  free >= need
 *
 * 反过来不成立（level 只保证"至少"，不保证"最多"），
 * 所以 freelist_find 返回的页一定要再用 page_free_bytes 复核一次。
 */

#include "storage/freelist.h"

#include <stdlib.h>
#include <string.h>

struct FreeList {
    u32  npages;
    u8  *levels;              /* 每页一个字节 */
    u32  bucket[FL_MAX_LEVEL + 1];   /* 每个档位有多少页 */
};

static u8 level_of(u16 free_bytes)
{
    if (free_bytes >= FL_USABLE)
        return (u8)FL_MAX_LEVEL;
    return (u8)(((u32)free_bytes * FL_MAX_LEVEL) / FL_USABLE);
}

static u8 need_level_of(u16 need)
{
    if (need >= FL_USABLE)
        return (u8)FL_MAX_LEVEL;
    return (u8)(((u32)need * FL_MAX_LEVEL + FL_USABLE - 1) / FL_USABLE);
}

int freelist_create(u32 npages, FreeList **out)
{
    if (!out)
        return E_INVAL;

    FreeList *fl = calloc(1, sizeof(*fl));
    if (!fl)
        return E_IO;

    fl->npages = npages;
    if (npages) {
        fl->levels = malloc(npages);
        if (!fl->levels) {
            free(fl);
            return E_IO;
        }
        /* 新页默认「不可用」（档位 0）。
         * 必须由调用方用 freelist_set 显式登记自己的页 ——
         * 否则多张表共用一个 pager 时，A 表会把 B 表的页当成空闲页。 */
        memset(fl->levels, 0, npages);
    }
    fl->bucket[0] = npages;

    *out = fl;
    return OK;
}

void freelist_destroy(FreeList *fl)
{
    if (!fl)
        return;
    free(fl->levels);
    free(fl);
}

int freelist_grow(FreeList *fl, u32 npages)
{
    if (!fl)
        return E_INVAL;
    if (npages <= fl->npages)
        return OK;

    u8 *nl = realloc(fl->levels, npages);
    if (!nl)
        return E_IO;

    /* 新增页同样默认不可用，等调用方登记 */
    memset(nl + fl->npages, 0, npages - fl->npages);
    fl->levels = nl;
    fl->bucket[0] += npages - fl->npages;
    fl->npages = npages;
    return OK;
}

u8 freelist_level(const FreeList *fl, u32 page_id)
{
    if (!fl || page_id >= fl->npages)
        return 0;
    return fl->levels[page_id];
}

void freelist_set(FreeList *fl, u32 page_id, u16 free_bytes)
{
    if (!fl || page_id >= fl->npages)
        return;

    u8 old = fl->levels[page_id];
    u8 nw = level_of(free_bytes);
    if (old == nw)
        return;

    if (fl->bucket[old])
        fl->bucket[old]--;
    fl->bucket[nw]++;
    fl->levels[page_id] = nw;
}

void freelist_set_full(FreeList *fl, u32 page_id)
{
    freelist_set(fl, page_id, 0);
}

u32 freelist_find(const FreeList *fl, u16 need)
{
    if (!fl || need > FL_USABLE)
        return PAGE_ID_NIL;

    u8 start = need_level_of(need);

    /* 从"刚好够"的档位往上找第一个非空桶 —— best-fit */
    for (u32 lv = start; lv <= FL_MAX_LEVEL; lv++) {
        if (fl->bucket[lv] == 0)
            continue;
        for (u32 p = 0; p < fl->npages; p++)
            if (fl->levels[p] == (u8)lv)
                return p;
    }
    return PAGE_ID_NIL;
}

u32 freelist_npages(const FreeList *fl)
{
    return fl ? fl->npages : 0;
}

u32 freelist_count_at(const FreeList *fl, u8 level)
{
    /* level 是 u8，天然落在 0..255，不需要上界检查 */
    if (!fl)
        return 0;
    return fl->bucket[level];
}
