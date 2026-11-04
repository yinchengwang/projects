/*
 * pager_free.c —— 全局页分配器的空闲回收（v1.5 步骤 4 · 计划判决 4）
 *
 * 【这一层解决什么】
 *
 * pager_alloc 只会往文件尾追加页 —— 它**没有**对侧：
 * 一个结构用完的页，没有地方可以"还回去"。
 *
 * 计划判决 4（v1.5 §1.2）要求时序 TTL 回收过期分区时，
 * 回收的页进入**全局页分配器的空闲**：
 *
 *   分区页区间 → freelist（页号空间共享，回收的页可被任何结构用）
 *
 * "全局"两个字是重点：它**不是**某张表的 freelist
 * （storage/freelist.h 那个是"哪一页能放下 N 字节的行"的空间索引，
 *  页在表里，不能跨表用）。
 *
 * 所以这个栈挂在 pager 上，**不属于任何表**：
 * 下一个调 pager_alloc 的**任何结构**都可能拿到这一页。
 *
 * 【为什么单独一个文件】
 *
 * pager.c 已经在 800 行的边上（v1.3 把它记在超限账上的是 heap.c / db.c，
 * pager.c 当时还没超）。这一层与"缓冲池"是两件事：
 * 帧的换入换出 vs 页号的分配回收。分开才不用把 pager.c 再顶过线。
 *
 * 【锁】
 *
 * 空闲栈只被**池级写锁**保护（与帧哈希表、Clock 手同一把锁）。
 * 取页/还页都是低频操作，不值得为它单开一个锁域。
 *
 * 【崩溃一致性：这里的欠账】（坑 M6-52）
 *
 * 空闲栈**只在内存里**，不落盘 ⇒ 断电后丢失 ⇒
 * 那些页变成"文件里的空洞"（永远不会被分配，也永远不会回收）。
 *
 * 这与"页被泄漏"相比是**安全**的方向：
 * 泄漏 = 空间浪费；错误的复用 = 读到别人的数据。
 * 落盘这个空闲栈需要 WAL 或 catalog 参与，超出本步骤范围。
 */

#include "storage/pager_internal.h"

#include <stdlib.h>

/*
 * 空闲栈扩容（调用方必须已持有池级写锁）。
 */
static int free_stack_grow(Pager *pg)
{
    u32 ncap = pg->free_cap ? pg->free_cap * 2u : 16u;
    u32 *np = (u32 *)realloc(pg->free_ids, (size_t)ncap * sizeof(u32));

    if (np == NULL)
        return E_NOSPACE;
    pg->free_ids = np;
    pg->free_cap = ncap;
    return OK;
}

/*
 * 从空闲栈里取一页（pager_alloc 的复用路径）。
 *
 * 没有空闲页 ⇒ 返回 false（调用方去文件尾追加）。
 */
bool pager_free_pop(Pager *pg, u32 *page_id)
{
    bool got = false;

    if (!pg || !page_id)
        return false;

    pager_pool_wrlock(pg);
    if (pg->free_count > 0u) {
        *page_id = pg->free_ids[--pg->free_count];
        got = true;
    }
    pager_pool_wrunlock(pg);
    return got;
}

/*
 * 把一页还给全局页分配器。
 *
 * 【只做两件事】
 *   1. 这页当前没被 pin 住？（被 pin 住说明还有人正在用它 ⇒ 拒绝）
 *   2. 把页号推进空闲栈。
 *
 * 【不做什么】
 * 不清页内容、不改页类型 —— pager 不解释页内结构。
 * 想让"下一个主人"看到一张干净页，由调用方先清零再归还
 * （时序 TTL 就是这么做的：先清零，再归还）。
 *
 * 【为什么"被 pin 住"要拒绝】
 *
 * pin_count > 0 ⇒ 有人正拿着这一页的内容（内容锁在他手里）。
 * 这时把它发给别人 ⇒ 两个人写同一页 ⇒ 数据互相踩，而且谁都不报错。
 * 宁可返回 E_INVAL 让调用方发现自己的时序错了。
 */
int pager_free(Pager *pg, u32 page_id)
{
    u32 fi;
    bool pinned = false;
    int rc;

    if (!pg)
        return E_INVAL;

    u64 np = atomic_load(&pg->npages);
    if (page_id >= (u32)np)
        return E_NOTFOUND;   /* 还没分配过的页，不该被归还 */

    /* 有人正拿着这一页 ⇒ 不能把它发给别人（双发 = 数据互相踩） */
    pager_pool_rdlock(pg);
    if (pager_hash_find(pg, page_id, &fi))
        pinned = atomic_load(&pg->frames[fi].pin_count) > 0;
    pager_pool_rdunlock(pg);
    if (pinned)
        return E_INVAL;

    pager_pool_wrlock(pg);
    if (pg->free_count == pg->free_cap) {
        rc = free_stack_grow(pg);
        if (rc != OK) {
            pager_pool_wrunlock(pg);
            return rc;
        }
    }
    pg->free_ids[pg->free_count++] = page_id;
    pager_pool_wrunlock(pg);
    return OK;
}

/* 当前空闲页数（诊断与测试用：断言"页确实还回去了"）。 */
u32 pager_free_count(const Pager *pg)
{
    if (!pg)
        return 0;
    return pg->free_count;
}
