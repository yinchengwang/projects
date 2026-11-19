#include "modalities/ft/ft_writer.h"

#include "common/config.h"
#include "common/types.h"
#include "modalities/ft/ft_internal.h"

/*
 * ft_pagesim.c —— 页数模拟器（W-D 的 D-2）
 *
 * 契约与「为什么要有它」见 ft_writer.h 的 FtPageSim 那段。
 *
 * ⚠️ **本文件的规则必须与 ft_writer.c 的两处 put 逐字对应**：
 *    倒排 → ft_post_writer_put
 *    字典 → ft_dict_writer_put
 * 改了一边不改另一边，算出来的上界就会与实际不符（小于实际 ⇒ 合并失败）。
 * 一致性由 test/unit/test_ft_merge_pages.c 的四条边界测试盯着。
 *
 * 本文件**不碰 pager、不碰文件**：纯算术，所以能独立单测。
 */

void ft_pagesim_reset(FtPageSim *s)
{
    if (s == NULL) {
        return;
    }
    s->pages = 0u;
    s->used = 0u;
}

void ft_pagesim_add_postings(FtPageSim *s, u32 n)
{
    u32 i = 0;

    if (s == NULL) {
        return;
    }

    /* ft_post_writer_put：一个词都没写 ⇒ 一页都不开 ✓ */
    while (i < n) {
        u32 space;
        u32 cap;
        u32 take;

        if (s->pages == 0u) {
            /* 第一次写：先开第一页（对应 cur_pid == PAGE_ID_NIL 那个分支） */
            s->pages = 1u;
            s->used = 0u;
        }

        space = PAGE_SIZE - FPH_DATA - s->used;
        if (space < FT_RUN_HDR_BYTES + FT_POSTING_BYTES) {
            /* 本页连"一个 run 头 + 一条 posting"都放不下 ⇒ 换页 */
            s->pages++;
            s->used = 0u;
            continue;
        }

        cap = (space - FT_RUN_HDR_BYTES) / FT_POSTING_BYTES;
        take = (n - i < cap) ? (n - i) : cap;
        s->used += FT_RUN_HDR_BYTES + FT_POSTING_BYTES * take;
        i += take;

        if (i < n) {
            /*
             * 这个词还没写完 ⇒ 剩下的写进新开的一页。
             * 新页的第一个 run 是**续接段**，落在 FPH_DATA 处 ——
             * 但它照样要 12 字节 run 头（不是"0 开销"）。
             */
            s->pages++;
            s->used = 0u;
        }
    }
}

void ft_pagesim_add_dict_entry(FtPageSim *s, u32 term_len)
{
    u32 need;

    if (s == NULL) {
        return;
    }

    need = ft_dict_entry_bytes(term_len) + SLOT_SIZE;

    if (s->pages == 0u) {
        s->pages = 1u;
        s->used = 0u;
    } else if (s->used + need > FT_DICT_CAPACITY) {
        /* 本页装不下了 → 换页（对应 ft_dict_writer_put 的 else-if 分支） */
        s->pages++;
        s->used = 0u;
    }

    s->used += need;
}
