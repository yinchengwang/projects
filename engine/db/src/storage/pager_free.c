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
#include <string.h>

#include "storage/page.h"

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
 * 把"这一页空闲"这件事**写到页里**（W-D 的 D-6.3）。
 *
 * 不写日志：标记丢了的两个方向都安全（见 common/config.h 里 PT_FREE 的说明）。
 *
 * 【为什么必须走 pager_get / pager_unpin 而不是直接 write_page】
 *
 * 这一页可能正躺在缓冲池里（脏的、或被别人 pin 着）。
 * 绕过缓冲池直接写文件 ⇒ 内存里那份旧内容以后被刷出去时会把标记抹掉，
 * 或者反过来把新主人的数据盖掉。所以走正常的取帧路径。
 */
static int free_mark_page(Pager *pg, u32 page_id)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(pg, page_id, &fr);
    if (rc != OK)
        return rc;

    /*
     * 直接走 page_init：它做的正是"整页清零 + 置页类型 + 重置槽位区 + 算校验和"。
     * 空闲页不需要保留任何旧字节（下一个主人拿到的必须是干净页）——
     * 这比手工逐字段设置更不容易漏，也和 pager_alloc 的交接口径一致。
     */
    rc = page_init(fr->data, (u8)PT_FREE);
    if (rc != OK) {
        (void)pager_unpin(pg, page_id, false);
        return rc;
    }

    return pager_unpin(pg, page_id, true);
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

    /*
     * 先把"空闲"这件事**写进页里**（D-6.3 的落盘）。
     *
     * 顺序：标记页 → 进栈。反过来（先进栈、后标记）在崩溃窗口里
     * 会得到"栈里有它、页上没标记" ⇒ 重开后栈重建不出来 ⇒ 只是空洞（安全）；
     * 而现在是"页上有标记、栈里还没它" ⇒ 重开后会被扫出来 ⇒ 更好。
     * 两种顺序都不会"重复发放"，因为发放的前提是页上**没有**标记。
     */
    rc = free_mark_page(pg, page_id);
    if (rc != OK)
        return rc;

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

/*
 * 开库时重建空闲栈（W-D 的 D-6.3）。
 *
 * 扫一遍页空间，凡是页类型是 PT_FREE 的都收进栈。
 *
 * 【必须在 WAL 重放**之后**调用】
 *
 * 重放会把"已提交的页镜像"盖回去 —— 那些页是**有主**的。
 * 在重放之前扫 ⇒ 会把它们当空闲页收进栈 ⇒ 下次分配发出去 ⇒ 数据互相踩。
 * 所以唯一正确的调用点是 db_open 里 recovery_run 之后。
 *
 * 【为什么可以线性扫】
 *
 * 开库时本来就要付这次代价：geo / ts / ft 都在开库时扫页空间找自己的页
 * （坑 M5-9 / M6-32 已经如实记账"开库代价随库大小线性增长"）。
 * 这一趟额外扫描与它们同量级，不改变量级。
 *
 * 【不重不漏】
 *
 * 扫描是幂等的：同一个页扫两遍只会被 push 两次 —— 所以这里先清空栈。
 * 清空是安全的：调用点在开库时，此前的栈里只可能有
 * "本次会话回收过的页"，而那些页在盘上**也有标记** ⇒ 扫得到。
 */
int pager_free_rebuild(Pager *pg, u32 *found_out)
{
    u64 np;
    u32 pid;
    u32 n = 0;
    int rc = OK;

    if (!pg)
        return E_INVAL;
    if (found_out)
        *found_out = 0;

    pager_pool_wrlock(pg);
    pg->free_count = 0;
    pager_pool_wrunlock(pg);

    np = atomic_load(&pg->npages);
    for (pid = 0; (u64)pid < np; pid++) {
        PageFrame *fr = NULL;

        rc = pager_get(pg, pid, &fr);
        if (rc != OK)
            return rc;

        if (fr->data[H_TYPE] == (u8)PT_FREE) {
            pager_pool_wrlock(pg);
            if (pg->free_count == pg->free_cap) {
                rc = free_stack_grow(pg);
                if (rc != OK) {
                    pager_pool_wrunlock(pg);
                    (void)pager_unpin(pg, pid, false);
                    return rc;
                }
            }
            pg->free_ids[pg->free_count++] = pid;
            pager_pool_wrunlock(pg);
            n++;
        }
        (void)pager_unpin(pg, pid, false);
    }

    if (found_out)
        *found_out = n;
    return OK;
}

/* 当前空闲页数（诊断与测试用：断言"页确实还回去了"）。 */
u32 pager_free_count(const Pager *pg)
{
    if (!pg)
        return 0;
    return pg->free_count;
}
