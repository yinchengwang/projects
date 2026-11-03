/*
 * ft_catalog.c —— 段目录的**落盘形态**与恢复（v1.3 步骤 5）
 *
 * 【段目录是什么】
 *
 * 磁盘上有很多段，其中一部分已经被合并掉了（dead）。
 * 重开库时必须知道"哪些段还在" —— 否则：
 *
 *   - 把 dead 的段也算进去 → 同一篇文档被数两遍（老段一份、新段一份）；
 *   - 把 dead 的段漏掉 → 文档凭空消失。
 *
 * 【为什么没有"目录页"】（坑 M4-20）
 *
 * 本版把状态**放进段头页自己**（FTH_STATE / FTH_MERGE_TO，
 * 格式见 ft_internal.h）。理由：段头页已经存在了，
 * 状态跟着段走，不会出现"目录页说有、段头页却找不到"的分裂状态。
 *
 * 代价：恢复时必须**扫整个页空间**（坑 M4-22）——
 * 因为没有"根页"可以指路。接受这个代价：
 * 换来的是"没有根页就不会有根页写丢"这种简单到不会错的恢复逻辑。
 *
 * 【恢复的三步】
 *
 *   1. 扫页空间：认领 PT_FT_HEAD（段）与 PT_FT_TOMB（删除标记）两类页；
 *   2. 判定每个段死活（DEAD / MERGING 的判据见下）；
 *   3. 按段号递增把活着的段打开，装进索引。
 *
 * 【MERGING 的判据】（坑 M4-21）
 *
 *   状态是 MERGING 的段：**目标段存在 → 算 dead；目标段不存在 → 算 live**。
 *
 * 合并的顺序是"标 MERGING → 写新段 → 标 DEAD"，所以：
 *   - 崩在写新段之前 → 目标段不存在 → 老段还活着（文档还在）✓
 *   - 崩在标 DEAD 之前 → 目标段存在 → 老段算死（文档在新段里）✓
 *
 * 两种情况都不丢文档、也不重复计数。
 */

#include "modalities/ft/ft_index_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"
#include "storage/page.h"

/* ---------------- 段头页的信息（扫描时收集） ---------------- */

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

/* ---------------- 扫页空间 ---------------- */

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

/* ---------------- 恢复 ---------------- */

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
 * 恢复段目录：扫页空间 → 判定死活 → 打开活着的段。
 */
int ft_catalog_recover(FtIndex *ix)
{
    FtHeadInfo *heads = NULL;
    u32 n_heads = 0;
    u32 max_seg_id = 0;
    u32 max_merge_to = 0;
    int rc;

    if (ix == NULL)
        return E_INVAL;

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

        if (!live)
            continue;

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

/* ---------------- 合并时的状态标记 ---------------- */

/*
 * 把几个段标成 MERGING（指向新段的段号）。
 *
 * 必须在**写新段之前**调用 —— 否则"新段已存在、老段还没标死"的
 * 窗口里崩溃，重开库就会把两批文档都算进去（坑 M4-21）。
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
