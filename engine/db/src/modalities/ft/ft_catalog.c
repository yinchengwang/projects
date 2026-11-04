/*
 * ft_catalog.c —— 段目录（根页 + 目录页）与恢复（v1.3 欠账 3 / 7）
 *
 * 【段目录是什么】
 *
 * 磁盘上有很多段，其中一部分已经被合并掉了（dead）。
 * 重开库时必须知道"哪些段还在" —— 否则：
 *
 *   - 把 dead 的段也算进去 → 同一篇文档被数两遍（老段一份、新段一份）；
 *   - 把 dead 的段漏掉 → 文档凭空消失。
 *
 * 【本版：目录优先，扫页空间只是回退】
 *
 * 以前（v1.3 步骤 5）段的清单散落在**段头页**的状态字段里，
 * 恢复时只能**扫整个页空间**（坑 M4-22）—— 开库时间随库大小线性增长。
 *
 * 现在多了一层显式的目录：
 *
 *   根页（两张，交替写）  —— 指向"当前这一版目录"；
 *   段目录页（可跨多页）  —— 段条目（段号 / 段头页 / 状态）
 *                          + tombstone 页条目（页号 / 序号）。
 *
 * 恢复时：**有根页 → 读目录**（读两张根页 + 一条目录链）；
 * **没有根页（老库）→ 才扫页空间**（一次性回退，之后就建根页）。
 *
 * 【目录是"已提交视图"，段头页是"过程状态"】
 *
 *   - 目录里只有两种状态：LIVE / DEAD。
 *     它记的是"**已经提交**的段集合"—— 合并没提交之前，目录不动。
 *   - 段头页上还有第三种：MERGING（合并进行中）。
 *     它是**过程状态**，只给"没有目录的老路径"用。
 *
 * 为什么要分这么清：目录是**权威**（恢复时按它找段），
 * 段头页上的状态是**辅助**（老路径按它判死活）。
 * 两边不一致时，以目录为准 —— 目录写不进去 = 这次操作没提交。
 *
 * 【"谁在什么时候写目录"（完整清单）】
 *
 *   1. 建段之后（ft_index_add 成功）—— 新段进目录。
 *      这一写是**加文档的提交点**：写不进去 → 加文档报错。
 *   2. 合并提交之后（ft_index_merge 换完内存里的段）——
 *      老段标 DEAD、新段进目录。**这一写是合并的提交点。**
 *   3. 合并**开始**时（标 MERGING）—— **不写目录**。
 *      合并还没提交，目录不动（目录记的是已提交视图）。
 *      段头页上的 MERGING 状态照旧写 —— 那是给老路径用的过程状态。
 *   4. 合并**失败**时（abort）—— **不写目录**。
 *      目录里老段本来就是 LIVE（没提交过），不用改。
 *   5. 新建 tombstone 页时（ft_tomb_append）—— 页号列表变了。
 *      页**内**追加不用写目录（条数存在页里，目录只记页号）。
 *   6. 老库首次打开（回退路径扫完页空间之后）——
 *      建两张根页 + 把扫到的段与 tombstone 页写进目录。
 *
 * 【崩溃时的一致性】
 *
 *   - 目录里说有的段，磁盘上可能还没有（建段的目录写在段写完之后）：
 *     恢复时**跳过打不开的段**（那一段的内容没写完，文档没进去过）。
 *   - 磁盘上有、目录里没有的段（建段的目录还没写）：
 *     恢复时**看不见**它 —— 但那一次建段**还没返回成功**，
 *     等于"没提交过的写入"，丢掉是对的（不是丢数据）。
 *   - 合并的提交点就是目录那一写：目录写了 = 老段死了、新段活了；
 *     目录没写 = 合并没发生，老段还是活的（新段成了孤儿页，白占地方）。
 *
 * 所以**任何时刻**恢复出来的段集合都是对的：不丢文档，也不重复计数。
 */

#include "modalities/ft/ft_index_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_root.h"
#include "modalities/ft/ft_writer.h"
#include "storage/page.h"

/* ---------------- 根页的接缝（db 层 ↔ FT 层） ---------------- */

/*
 * 索引上是不是绑着一对根页？
 *
 * "没有"有两种表示：0（还没绑，见 ft_index_set_root 的约定）
 * 与 PAGE_ID_NIL（calloc 出来的默认值）。两种都算"没有"。
 */
static bool ft_root_bound(const FtIndex *ix)
{
    if (ix->root.root_a == 0 || ix->root.root_b == 0)
        return false;
    if (ix->root.root_a == PAGE_ID_NIL || ix->root.root_b == PAGE_ID_NIL)
        return false;
    return true;
}

int ft_index_set_root(FtIndex *ix, u32 root_a, u32 root_b)
{
    if (ix == NULL)
        return E_INVAL;

    /*
     * 页号 0 = "没设"（meta 页自己就是第 0 页，根页永远不会是 0）。
     * 两个都传 0 → 老库（没有根页）→ 恢复走回退路径。
     */
    if (root_a == 0 || root_b == 0) {
        memset(&ix->root, 0, sizeof(ix->root));
        ix->root.root_a = PAGE_ID_NIL;
        ix->root.root_b = PAGE_ID_NIL;
        return OK;
    }
    return ft_root_bind(&ix->root, root_a, root_b);
}

int ft_index_root_pages(const FtIndex *ix, u32 *root_a, u32 *root_b)
{
    if (ix == NULL || root_a == NULL || root_b == NULL)
        return E_INVAL;

    /*
     * 没建过根页 → 报 0（"没有"）。
     * 用 0 而不是 PAGE_ID_NIL：meta 页的槽位用 0 当"空"的哨兵
     * （第 0 页是 meta 页自己，根页不可能是第 0 页）。
     */
    if (ix->root.root_a == PAGE_ID_NIL || ix->root.root_b == PAGE_ID_NIL) {
        *root_a = 0;
        *root_b = 0;
        return OK;
    }
    if (!ft_root_bound(ix)) {
        *root_a = 0;
        *root_b = 0;
        return OK;
    }
    *root_a = ix->root.root_a;
    *root_b = ix->root.root_b;
    return OK;
}

/* ---------------- 段头页的信息（扫页空间时收集） ---------------- */

typedef struct {
    u32 head_page;
    u32 seg_id;
    u32 state;
    u32 merge_to;
} FtHeadInfo;

/* 按段号升序比较 */
static int ft_catalog_head_cmp(const void *pa, const void *pb)
{
    const FtHeadInfo *a = (const FtHeadInfo *)pa;
    const FtHeadInfo *b = (const FtHeadInfo *)pb;

    if (a->seg_id != b->seg_id)
        return (a->seg_id < b->seg_id) ? -1 : 1;
    /* 段号相同时按页号排（保证结果确定） */
    if (a->head_page != b->head_page)
        return (a->head_page < b->head_page) ? -1 : 1;
    return 0;
}

/* 目录条目按段号升序比较 */
static int ft_catalog_seg_cmp(const void *pa, const void *pb)
{
    const FtRootSeg *a = (const FtRootSeg *)pa;
    const FtRootSeg *b = (const FtRootSeg *)pb;

    if (a->seg_id != b->seg_id)
        return (a->seg_id < b->seg_id) ? -1 : 1;
    if (a->head_page != b->head_page)
        return (a->head_page < b->head_page) ? -1 : 1;
    return 0;
}

/* tombstone 条目按序号升序比较 */
static int ft_catalog_tomb_cmp(const void *pa, const void *pb)
{
    const FtRootTomb *a = (const FtRootTomb *)pa;
    const FtRootTomb *b = (const FtRootTomb *)pb;

    if (a->ordinal != b->ordinal)
        return (a->ordinal < b->ordinal) ? -1 : 1;
    if (a->page_id != b->page_id)
        return (a->page_id < b->page_id) ? -1 : 1;
    return 0;
}

/* ---------------- 死段登记 ---------------- */

int ft_catalog_add_dead(FtIndex *ix, u32 seg_id, u32 head_page)
{
    if (ix == NULL)
        return E_INVAL;

    if (ix->n_dead == ix->cap_dead) {
        u32 ncap = (ix->cap_dead == 0) ? 8u : ix->cap_dead * 2u;
        FtDeadSeg *nd = (FtDeadSeg *)realloc(ix->dead, sizeof(FtDeadSeg) * (size_t)ncap);

        if (nd == NULL)
            return E_NOSPACE;
        ix->dead = nd;
        ix->cap_dead = ncap;
    }
    ix->dead[ix->n_dead].seg_id = seg_id;
    ix->dead[ix->n_dead].head_page = head_page;
    ix->n_dead++;
    return OK;
}

/* 清掉"半拉子"的恢复状态（目录读不通、回退扫页空间之前用） */
void ft_catalog_reset(FtIndex *ix)
{
    u32 i;

    if (ix == NULL)
        return;

    for (i = 0; i < ix->n_segs; i++) {
        ft_segment_close(ix->segs[i]);
    }
    free(ix->segs);
    ix->segs = NULL;
    ix->n_segs = 0;
    ix->cap_segs = 0;

    free(ix->dead);
    ix->dead = NULL;
    ix->n_dead = 0;
    ix->cap_dead = 0;
}

/* ---------------- 目录链的页从哪儿来（目录专用空闲页池） ---------------- */

/*
 * 目录链的页也从**空闲页池**里取，旧链的页还回池里（坑 M4-53）。
 *
 * 不这么做的话：每写一次目录就要多占一条链的页（1 页起），
 * 而合并每轮都要写一次目录 → 文件每轮 +1 页 → 合并做不回本。
 *
 * 目录链的页**不必连号**（读侧顺着页链 H_NEXT 走）→
 * 所以可以直接从池里一页一页取，不用管池里的页连不连号 ✓。
 *
 * 【用的是目录专用池，不是段池】
 *
 * 段池（FtIndex.pool）的语义是"这里放的都是**段**的页"：
 * 合并失败回滚时老段的页不能提前进池（老段还要用），
 * 测试也按这个口径断言"合并失败 → 空闲页池一个页都不许多"。
 * 把目录链的页混进段池会破坏这条不变式
 * （目录链换代会把旧链页还进段池，合并失败时新写的目录页又从段池取）。
 *
 * 单独一个池之后：目录链页只在目录池里流转（换代时旧链还池、
 * 新链从池里取），段池语义保持干净，文件也不随合并次数线性增长。
 */
static int ft_catalog_page_alloc(void *ctx, u32 *pid_out, PageFrame **fr_out)
{
    FtIndex *ix = (FtIndex *)ctx;

    return ft_pool_alloc(ix->cat_pool, ix->pg, pid_out, fr_out);
}

static void ft_catalog_page_release(void *ctx, u32 pid)
{
    FtIndex *ix = (FtIndex *)ctx;

    (void)ft_pool_add(ix->cat_pool, pid);
}

/* ---------------- 写目录 ---------------- */

/*
 * 把索引当前的状态写进目录。
 *
 * 写的是：**一整条新的目录链 + 翻根页**（ft_root_write 干的活）。
 *
 * 【为什么要整条重写，而不是"改一页"】
 *
 * 目录链是不可变的：翻根页是**唯一的提交点**。
 * 如果就地改链上的某一页，断电就可能停在"改了一半" ——
 * 而根页指着这条链，链是断的 → 目录读不出来 → 回退到上一代目录。
 * 整条重写 + 翻根页的写法，任何时刻磁盘上都**至少有一份完整目录**
 * （见 ft_root.c 的坑 M4-33）。
 *
 * 【代价：写放大】
 *
 * 每写一次目录 = 一条新链。旧链的页在**换代成功之后**还进目录池
 * （坑 M4-53：见上面 ft_catalog_page_release），
 * 所以文件不会随合并次数线性增长；但链本身仍是"整条重写"，
 * 段数越多，每次写的链越长 —— 这是本轮设计**已知**的代价。
 */
int ft_catalog_sync(FtIndex *ix, u32 xid)
{
    FtRootSeg *segs = NULL;
    FtRootTomb *tombs = NULL;
    u32 n_segs = 0;
    u32 n_tombs = 0;
    u32 k = 0;
    int rc;

    if (ix == NULL)
        return E_INVAL;

    /*
     * 还没有根页 → 先建（两张，generation 1 / 0）。
     *
     * 这一步在**第一次写目录**时发生：可能是老库首次打开时的回退路径，
     * 也可能是第一次加文档（那时候还没有任何目录）。
     */
    if (!ft_root_bound(ix)) {
        rc = ft_root_create(ix->pg, ix->wal, ix->wal_ctx, xid, &ix->root);
        if (rc != OK)
            return rc;
    }

    n_segs = ix->n_segs + ix->n_dead;
    if (n_segs > 0) {
        segs = (FtRootSeg *)malloc(sizeof(FtRootSeg) * (size_t)n_segs);
        if (segs == NULL)
            return E_NOSPACE;
    }
    n_tombs = ix->n_tomb_pages;
    if (n_tombs > 0) {
        tombs = (FtRootTomb *)malloc(sizeof(FtRootTomb) * (size_t)n_tombs);
        if (tombs == NULL) {
            free(segs);
            return E_NOSPACE;
        }
    }

    /* 活段在前（ix->segs 本来就是段号递增），死段在后 */
    for (u32 i = 0; i < ix->n_segs; i++) {
        segs[k].seg_id = ix->segs[i]->seg_id;
        segs[k].head_page = ix->segs[i]->head_page;
        segs[k].state = FT_SEG_LIVE;
        k++;
    }
    for (u32 i = 0; i < ix->n_dead; i++) {
        segs[k].seg_id = ix->dead[i].seg_id;
        segs[k].head_page = ix->dead[i].head_page;
        segs[k].state = FT_SEG_DEAD;
        k++;
    }

    /* tombstone 页：序号就是它在页序列里的位置（追加时要靠它找"当前页"） */
    for (u32 i = 0; i < n_tombs; i++) {
        tombs[i].page_id = ix->tomb_pages[i];
        tombs[i].ordinal = i;
    }

    /*
     * 写目录链 + 翻根页。
     *
     * ops 里的 alloc/release 让根页模块知道"目录链的页从哪儿来"：
     * 从目录池取（没有就向文件尾追加），换代成功后把旧链的页还回目录池。
     */
    {
        FtRootPageOps ops;

        ops.alloc = ft_catalog_page_alloc;
        ops.release = ft_catalog_page_release;
        ops.ctx = ix;
        rc = ft_root_write_ex(ix->pg, ix->wal, ix->wal_ctx, xid, &ix->root,
                              segs, n_segs, tombs, n_tombs, &ops);
    }

    free(segs);
    free(tombs);
    return rc;
}

/*
 * 建根页（还没有根页时）+ 把当前状态写进目录。
 *
 * 老库首次打开走的就是这条路径：扫完页空间 → 建两张根页 →
 * 把扫到的段与 tombstone 页写进目录 → 之后开库就不用扫了。
 */
int ft_catalog_root_init(FtIndex *ix, u32 xid)
{
    if (ix == NULL)
        return E_INVAL;

    return ft_catalog_sync(ix, xid);
}

/* ---------------- 恢复：读目录 ---------------- */

/*
 * 按目录恢复：读两张根页 → 读目录链 → 打开活着的段。
 *
 * 【目录是权威：段的状态按目录说的算】
 *
 * 段头页上的 MERGING 是**过程状态**（合并进行中），
 * 目录里只有 LIVE / DEAD（已提交视图）。
 * 崩溃时两者可能不一致（合并的目录写在最后）——
 * 这时以**目录**为准：目录说 LIVE 就是 LIVE。
 *
 * 【打不开的段：跳过】
 *
 * 目录说有、磁盘上没有（建段的目录写在段写完之后，
 * 断电停在中间）→ 那一段的内容压根没写完 → 跳过。
 * 不跳过的话整个开库就失败了 —— 而那只是一次**没提交**的写入。
 */
static int ft_catalog_recover_dir(FtIndex *ix)
{
    FtRootCatalog cat;
    u32 max_seg_id = 0;
    int rc;

    rc = ft_root_load(ix->pg, &ix->root);
    if (rc != OK)
        return rc;          /* E_NOTFOUND：没有可用的根页 → 调用方回退 */

    rc = ft_root_read(ix->pg, &ix->root, &cat);
    if (rc != OK)
        return rc;          /* E_CORRUPT：目录链读不通 → 调用方回退 */

    if (cat.n_segs > 1)
        qsort(cat.segs, cat.n_segs, sizeof(FtRootSeg), ft_catalog_seg_cmp);
    if (cat.n_tombs > 1)
        qsort(cat.tombs, cat.n_tombs, sizeof(FtRootTomb), ft_catalog_tomb_cmp);

    /* 段 */
    for (u32 i = 0; i < cat.n_segs; i++) {
        FtSegment *seg = NULL;

        if (cat.segs[i].seg_id > max_seg_id)
            max_seg_id = cat.segs[i].seg_id;

        if (cat.segs[i].state == FT_SEG_DEAD) {
            /* 死段：不打开，但要记住（段号不复用） */
            rc = ft_catalog_add_dead(ix, cat.segs[i].seg_id, cat.segs[i].head_page);
            if (rc != OK)
                goto fail;
            continue;
        }

        rc = ft_segment_open(ix->pg, cat.segs[i].head_page, &seg);
        if (rc != OK) {
            /* 目录说有、段没有 → 跳过（见上面的说明） */
            continue;
        }
        rc = ft_index_push_seg(ix, seg);
        if (rc != OK) {
            ft_segment_close(seg);
            goto fail;
        }
    }

    /*
     * 下一个段号 = 目录里最大段号 + 1。
     *
     * 死段也在目录里 → 它们的段号被"占着" → 不会复用 ✓。
     */
    ix->next_seg_id = max_seg_id + 1u;

    /* tombstone 页：登记进来（条数在页里，assemble 的时候再读） */
    for (u32 i = 0; i < cat.n_tombs; i++) {
        rc = ft_tomb_add_page(ix, cat.tombs[i].page_id, 0);
        if (rc != OK)
            goto fail;
    }
    /* 最后一页的条数 = "当前页还能写多少"（追加时要接着往后写） */
    if (ix->n_tomb_pages > 0) {
        u32 last = ix->tomb_pages[ix->n_tomb_pages - 1];
        PageFrame *fr = NULL;

        if (pager_get(ix->pg, last, &fr) == OK) {
            u32 c = get_u32(fr->data + FTM_COUNT);

            if (c > FT_TOMB_CAP)
                c = FT_TOMB_CAP;
            ix->tomb_fill = c;
            pager_unpin(ix->pg, last, false);
        }
    }

    ft_root_catalog_free(&cat);
    return OK;

fail:
    ft_root_catalog_free(&cat);
    ft_catalog_reset(ix);
    return rc;
}

/* ---------------- 恢复：扫页空间（老库回退） ---------------- */

/*
 * 把页空间里属于 FT 的页找出来。
 *
 * heads —— 段头页的信息（调用方给空间或传 NULL 让本函数分配）。
 *          本函数负责在返回前把数组的归属交给调用方（free 由调用方做）。
 *
 * 【认领的依据只有页类型（H_TYPE）】
 *
 * 别的模态的页、堆页、目录页的类型都不一样 → 不会认错。
 * 认领错页的后果是"把别人的页当段打开" → 读出垃圾分数，
 * 所以宁可不认领也不认错。
 */
static int ft_catalog_scan(FtIndex *ix, FtHeadInfo **heads_out, u32 *n_heads_out)
{
    FtHeadInfo *heads = NULL;
    u32 n_heads = 0;
    u32 cap = 0;
    u64 npages = pager_npages(ix->pg);
    int rc;

    for (u64 pid = 0; pid < npages; pid++) {
        PageFrame *fr = NULL;

        rc = pager_get(ix->pg, (u32)pid, &fr);
        if (rc != OK) {
            free(heads);
            return rc;
        }

        if (fr->data[H_TYPE] == (u8)PT_FT_HEAD) {
            if (n_heads == cap) {
                u32 ncap = (cap == 0) ? 8u : cap * 2u;
                FtHeadInfo *nh = (FtHeadInfo *)realloc(heads,
                                                       sizeof(FtHeadInfo) * (size_t)ncap);

                if (nh == NULL) {
                    pager_unpin(ix->pg, (u32)pid, false);
                    free(heads);
                    return E_NOSPACE;
                }
                heads = nh;
                cap = ncap;
            }
            heads[n_heads].head_page = (u32)pid;
            heads[n_heads].seg_id = get_u32(fr->data + FTH_SEG_ID);
            heads[n_heads].state = get_u32(fr->data + FTH_STATE);
            heads[n_heads].merge_to = get_u32(fr->data + FTH_MERGE_TO);
            n_heads++;
        } else if (fr->data[H_TYPE] == (u8)PT_FT_TOMB) {
            u32 count = get_u32(fr->data + FTM_COUNT);

            if (count > FT_TOMB_CAP)
                count = FT_TOMB_CAP;
            rc = ft_tomb_add_page(ix, (u32)pid, count);
            if (rc != OK) {
                pager_unpin(ix->pg, (u32)pid, false);
                free(heads);
                return rc;
            }
        }

        pager_unpin(ix->pg, (u32)pid, false);
    }

    *heads_out = heads;
    *n_heads_out = n_heads;
    return OK;
}

/*
 * 页空间里有没有段号等于 seg_id 的段？
 *
 * 用来判"MERGING 的老段还算不算活"：
 * 合并的目标段存在 → 合并已经写完了 → 老段算死。
 */
static bool ft_catalog_seg_exists(const FtHeadInfo *heads, u32 n_heads, u32 seg_id)
{
    for (u32 i = 0; i < n_heads; i++) {
        if (heads[i].seg_id == seg_id)
            return true;
    }
    return false;
}

/*
 * 回退路径：扫页空间 → 判定死活 → 打开活着的段。
 *
 * 【这是"没有根页的老库"的路】
 *
 * 一次性代价：开库时间与页数成正比。扫完就把结果写进目录
 * （ft_catalog_root_init），之后开库就走目录了。
 */
static int ft_catalog_recover_scan(FtIndex *ix)
{
    FtHeadInfo *heads = NULL;
    u32 n_heads = 0;
    u32 max_seg_id = 0;
    u32 max_merge_to = 0;
    int rc;

    rc = ft_catalog_scan(ix, &heads, &n_heads);
    if (rc != OK)
        return rc;

    if (n_heads > 1)
        qsort(heads, n_heads, sizeof(FtHeadInfo), ft_catalog_head_cmp);

    for (u32 i = 0; i < n_heads; i++) {
        if (heads[i].seg_id > max_seg_id)
            max_seg_id = heads[i].seg_id;
        if (heads[i].merge_to > max_merge_to)
            max_merge_to = heads[i].merge_to;
    }

    /*
     * 下一个段号 = 所有段号与所有"合并目标段号"的最大值 + 1。
     *
     * 【为什么要把 merge_to 也算进去】
     *
     * 崩掉的合并会留下"MERGING 状态、指向一个不存在的段号"。
     * 那个段号如果被后来的合并复用，老段就会因为"目标段存在"
     * 而被误判成 dead → 文档凭空消失。
     *
     * 所以段号**永不复用**：把 merge_to 也算进上界，
     * 保证后来的段号一定大于所有历史目标号。
     */
    ix->next_seg_id = ((max_seg_id > max_merge_to) ? max_seg_id : max_merge_to) + 1u;

    /* 逐个判定死活并打开 */
    for (u32 i = 0; i < n_heads; i++) {
        FtSegment *seg = NULL;
        bool live;

        /* 段号重复（不该发生）：只认第一个，避免同一篇文档被数两遍 */
        if (i > 0 && heads[i].seg_id == heads[i - 1].seg_id)
            continue;

        if (heads[i].state == FT_SEG_DEAD) {
            live = false;
        } else if (heads[i].state == FT_SEG_MERGING) {
            /* 目标段存在 → 合并已完成 → 老段算死；否则老段还活着 */
            live = !ft_catalog_seg_exists(heads, n_heads, heads[i].merge_to);
        } else {
            live = true;
        }

        if (!live) {
            /* 死段：记下来（段号不复用），不打开 */
            rc = ft_catalog_add_dead(ix, heads[i].seg_id, heads[i].head_page);
            if (rc != OK) {
                free(heads);
                return rc;
            }
            continue;
        }

        rc = ft_segment_open(ix->pg, heads[i].head_page, &seg);
        if (rc != OK) {
            free(heads);
            return rc;
        }
        rc = ft_index_push_seg(ix, seg);
        if (rc != OK) {
            ft_segment_close(seg);
            free(heads);
            return rc;
        }
    }

    free(heads);
    return OK;
}

/* ---------------- 恢复：总入口 ---------------- */

/*
 * 恢复段目录。
 *
 *   1. 有根页 → 先读目录（读两张根页 + 一条目录链）；
 *   2. 目录读不通 → 回退扫页空间；
 *   3. 扫完（老库）→ 建根页 + 把结果写进目录。
 *
 * 【为什么"目录读不通"要回退而不是报错】
 *
 * 目录坏掉的概率低，但**后果很重**（整个库开不了）。
 * 回退扫页空间慢一点，但**一定能开** —— 慢是可接受的代价，
 * 开不了不是。
 */
int ft_catalog_recover(FtIndex *ix)
{
    int rc;

    if (ix == NULL)
        return E_INVAL;

    /* 1) 有根页 → 目录优先 */
    if (ft_root_bound(ix)) {
        rc = ft_catalog_recover_dir(ix);
        if (rc == OK)
            return OK;
        /* 目录不可用 → 清掉半拉子的状态，走回退 */
        ft_catalog_reset(ix);
    }

    /* 2) 回退：扫页空间 */
    rc = ft_catalog_recover_scan(ix);
    if (rc != OK)
        return rc;

    /* 3) 老库首次打开 → 建根页 + 写目录（下次开库就不用扫了） */
    return ft_catalog_root_init(ix, 0u);
}

/* ---------------- 合并时的状态标记 ---------------- */

/*
 * 把几个段标成 MERGING（指向新段的段号）。
 *
 * 必须在**写新段之前**调用 —— 否则"新段已存在、老段还没标死"的
 * 窗口里崩溃，重开库就会把两批文档都算进去（坑 M4-21）。
 *
 * 【为什么不写目录】
 *
 * 目录记的是"已提交的段集合"。合并还没提交，目录不动 ——
 * 目录一写就是**提交点**（见文件头的清单）。
 * 段头页上的 MERGING 照旧写：那是给"没有目录的老路径"用的过程状态。
 */
int ft_catalog_mark_merge(FtIndex *ix, u32 xid, u32 new_seg_id,
                           const u32 *head_pages, u32 n)
{
    if (ix == NULL || (head_pages == NULL && n > 0))
        return E_INVAL;

    for (u32 i = 0; i < n; i++) {
        int rc = ft_write_segment_state(ix->pg, ix->wal, ix->wal_ctx, xid,
                                        head_pages[i], FT_SEG_MERGING, new_seg_id);

        if (rc != OK)
            return rc;
    }
    return OK;
}

/* 合并成功：老段标死 */
int ft_catalog_end_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n)
{
    if (ix == NULL || (head_pages == NULL && n > 0))
        return E_INVAL;

    for (u32 i = 0; i < n; i++) {
        int rc = ft_write_segment_state(ix->pg, ix->wal, ix->wal_ctx, xid,
                                        head_pages[i], FT_SEG_DEAD, 0);

        if (rc != OK)
            return rc;
    }
    return OK;
}

/* 合并失败：老段回滚成 LIVE（状态回滚，索引状态不变） */
int ft_catalog_abort_merge(FtIndex *ix, u32 xid, const u32 *head_pages, u32 n)
{
    if (ix == NULL || (head_pages == NULL && n > 0))
        return E_INVAL;

    for (u32 i = 0; i < n; i++) {
        int rc = ft_write_segment_state(ix->pg, ix->wal, ix->wal_ctx, xid,
                                        head_pages[i], FT_SEG_LIVE, 0);

        if (rc != OK)
            return rc;
    }
    return OK;
}
