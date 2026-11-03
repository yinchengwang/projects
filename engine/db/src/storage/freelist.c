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
 *
 * ==================== v1.2 步骤 4c：freelist 域的锁 ====================
 *
 * freelist 域保护三样东西：`levels[]`（每页的档位）、`bucket[]`（每档多少页）、
 * 以及 `npages`。
 *
 * 【为什么必须有锁：两个线程会挑到同一页】
 *
 * 插入路径是：
 *
 *     pid = freelist_find(fl, need);   ← 找一页放得下的
 *     pager_get(pager, pid, &f);       ← 把它读进来
 *     page_insert(f->data, ...);       ← 往里写
 *
 * 两个线程同时 find，**可能拿到同一个页**（档位在 find 之后才被更新）。
 * 没有锁的时候，一个线程的 `freelist_set` 会把档位算成
 * "扣掉它自己那一份" —— 另一个线程写进去的那条就被**漏算**了。
 * 后果不是"多占一点空间"，而是**同一页被塞进两行**：
 * 页内的 slot 目录是并发写的 → 页损坏 → 那一页的数据全丢。
 *
 * 【读路径也要加锁：levels 是 realloc 出来的】
 *
 * `freelist_grow` 用 `realloc` 扩 `levels` —— **指针会变**。
 * 读者如果在外面缓存了旧指针，扩容之后就是**悬空指针**。
 * 所以本模块的读接口（level / find / npages / count_at）一律加锁，
 * 临界区里只用 `fl->levels` 这个**当下**的指针。
 *
 * 【为什么读接口的签名带 const，却还是要改 mutex 的状态】
 *
 * `const FreeList *` 的意思是"调用方不许改这个 FSM"，
 * 不是"这个对象里的每个字节都不可变"——
 * mutex 的状态当然要变。所以取锁时显式去掉 const，
 * （mutex 本身不是 const 对象，去掉的是调用方的视角约束）。
 *
 * 【域锁的获取全序】
 *
 * freelist 在全序里排**最后**（catalog → txn → frame → wal → freelist）。
 * 因为 `freelist_set(fl, pid, page_free_bytes(f->data))` 要读页内容，
 * 调用它的时候**页还捏在手里**（frame 域的锁还持着）→ frame → freelist ✓
 *
 * 反过来没有边：freelist 锁里不会去碰页
 * （`freelist_find` 先返回，`freelist_grow` 在页被捏住之后才调）。
 */

#include "storage/freelist.h"

#include <pthread.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"

struct FreeList {
    u32  npages;
    u8  *levels;              /* 每页一个字节 */
    u32  bucket[FL_MAX_LEVEL + 1];   /* 每个档位有多少页 */
    pthread_mutex_t mu;       /* freelist 域锁 */
};

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → frame → wal → freelist"这条全序
 * 从步骤 2 起就是**机器检查**的。
 */
static void fl_lock(const FreeList *fl)
{
    /* 见文件头：const 是调用方视角的约束，mutex 自己的状态必须能变 */
    pthread_mutex_lock((pthread_mutex_t *)&fl->mu);
    latch_order_acquire(LATCH_FREELIST);
}

static void fl_unlock(const FreeList *fl)
{
    latch_order_release(LATCH_FREELIST);
    pthread_mutex_unlock((pthread_mutex_t *)&fl->mu);
}

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

    if (pthread_mutex_init(&fl->mu, NULL) != 0) {
        free(fl);
        return E_IO;
    }

    fl->npages = npages;
    if (npages) {
        fl->levels = malloc(npages);
        if (!fl->levels) {
            pthread_mutex_destroy(&fl->mu);
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
    pthread_mutex_destroy(&fl->mu);
    free(fl->levels);
    free(fl);
}

int freelist_grow(FreeList *fl, u32 npages)
{
    if (!fl)
        return E_INVAL;

    fl_lock(fl);
    if (npages <= fl->npages) {
        fl_unlock(fl);
        return OK;
    }

    u8 *nl = realloc(fl->levels, npages);
    if (!nl) {
        fl_unlock(fl);
        return E_IO;
    }

    /* 新增页同样默认不可用，等调用方登记 */
    memset(nl + fl->npages, 0, npages - fl->npages);
    fl->levels = nl;
    fl->bucket[0] += npages - fl->npages;
    fl->npages = npages;
    fl_unlock(fl);
    return OK;
}

u8 freelist_level(const FreeList *fl, u32 page_id)
{
    if (!fl)
        return 0;
    fl_lock(fl);
    u8 lv = (page_id < fl->npages) ? fl->levels[page_id] : 0;
    fl_unlock(fl);
    return lv;
}

void freelist_set(FreeList *fl, u32 page_id, u16 free_bytes)
{
    if (!fl)
        return;

    fl_lock(fl);
    if (page_id >= fl->npages) {
        fl_unlock(fl);
        return;
    }

    u8 old = fl->levels[page_id];
    u8 nw = level_of(free_bytes);
    if (old == nw) {
        fl_unlock(fl);
        return;
    }

    /*
     * 档位与桶计数必须**一起**改。
     *
     * 少了这一步的锁，两个线程会各自算"扣掉自己那一份"，
     * 桶里的计数和实际档位对不上 → find 会返回已经被别人占满的页。
     */
    if (fl->bucket[old])
        fl->bucket[old]--;
    fl->bucket[nw]++;
    fl->levels[page_id] = nw;
    fl_unlock(fl);
}

void freelist_set_full(FreeList *fl, u32 page_id)
{
    freelist_set(fl, page_id, 0);
}

u32 freelist_find(const FreeList *fl, u16 need)
{
    if (!fl || need > FL_USABLE)
        return PAGE_ID_NIL;

    fl_lock(fl);
    u8 start = need_level_of(need);
    u32 found = PAGE_ID_NIL;

    /* 从"刚好够"的档位往上找第一个非空桶 —— best-fit */
    for (u32 lv = start; lv <= FL_MAX_LEVEL && found == PAGE_ID_NIL; lv++) {
        if (fl->bucket[lv] == 0)
            continue;
        for (u32 p = 0; p < fl->npages; p++) {
            if (fl->levels[p] == (u8)lv) {
                found = p;
                break;
            }
        }
    }
    fl_unlock(fl);
    return found;
}

u32 freelist_npages(const FreeList *fl)
{
    if (!fl)
        return 0;
    fl_lock(fl);
    u32 n = fl->npages;
    fl_unlock(fl);
    return n;
}

u32 freelist_count_at(const FreeList *fl, u8 level)
{
    /* level 是 u8，天然落在 0..255，不需要上界检查 */
    if (!fl)
        return 0;
    fl_lock(fl);
    u32 n = fl->bucket[level];
    fl_unlock(fl);
    return n;
}
