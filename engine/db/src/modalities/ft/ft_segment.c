/*
 * ft_segment.c —— 全文检索段的"段对象"（v1.3 步骤 2）
 *
 * 一个段 = 一批文档构建出来的**不可变**倒排索引：
 *
 *   PT_FT_HEAD（1 页，段头）
 *   PT_FT_DICT（若干页，字典：有序的 term 数组）
 *   PT_FT_POST（若干页，倒排：posting 数组）
 *
 * 磁盘格式的契约全在 modalities/ft/ft_internal.h —— 本文件只负责"读段头"，
 * 建段（切词、聚合、写页）在 ft_segment_build.c 里。
 *
 * 【段在内存里只有段头】
 *
 * 字典页和倒排页都是**按需从页里读**的：
 * 段对象里只有段头的字段（文档数、词数、页区间……），
 * 真正的 posting 一个都不缓存 —— 这正是"IO 可证"（验收 A5）的前提：
 * 查一个词的页读 = 字典二分的页数 + 该词的 posting 页数，与总文档量无关。
 *
 * 【段是不可变的】
 *
 * 建完之后一个字都不改。所以段对象只有"读"的方法，
 * 唯一的写字段是 page_reads（页读计数器，纯统计）。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>

#include "common/error.h"
#include "storage/page.h"

/*
 * 从段头页里把字段读出来。
 *
 * 段头页的格式（ft_internal.h 的 FTH_*）：
 *
 *   20 FTH_VER         u8   版本
 *   24 FTH_SEG_ID      u32  段号
 *   28 FTH_DOC_COUNT   u32  文档数 N
 *   32 FTH_MAX_DOC_ID  u32  段内最大 doc_id
 *   36 FTH_DICT_START  u32  字典页起始页号
 *   40 FTH_DICT_COUNT  u32  字典页数
 *   44 FTH_POST_START  u32  倒排页起始页号
 *   48 FTH_POST_COUNT  u32  倒排页数
 *   52 FTH_NTERMS      u32  段的词数
 *   56 FTH_TOTAL_LEN   u64  所有文档的词数之和（avgdl = 它 / 文档数）
 *   64 FTH_CREATE_LSN  u32  建段时的 LSN
 */
static void ft_segment_load_head(FtSegment *seg, const u8 *head)
{
    seg->seg_id = get_u32(head + FTH_SEG_ID);
    seg->doc_count = get_u32(head + FTH_DOC_COUNT);
    seg->total_len = get_u64(head + FTH_TOTAL_LEN);
    seg->max_doc_id = get_u32(head + FTH_MAX_DOC_ID);
    seg->n_terms = get_u32(head + FTH_NTERMS);
    seg->dict_start = get_u32(head + FTH_DICT_START);
    seg->dict_count = get_u32(head + FTH_DICT_COUNT);
    seg->post_start = get_u32(head + FTH_POST_START);
    seg->post_count = get_u32(head + FTH_POST_COUNT);
    /*
     * page_reads 记的是**检索**引起的页读（ft_segment.h 的措辞）。
     * 打开段头页不算检索 —— 所以这里不计数。
     */
    seg->page_reads = 0;
}

int ft_segment_open(Pager *pg, u32 head_page, FtSegment **out)
{
    PageFrame *fr = NULL;
    FtSegment *seg;
    int rc;

    if (!pg || !out || head_page == PAGE_ID_NIL)
        return E_INVAL;

    rc = pager_get(pg, head_page, &fr);
    if (rc != OK)
        return rc;

    /*
     * 页类型或版本不对 → E_CORRUPT。
     *
     * 宁可打不开，也不要带着错数据跑 ——
     * 段里的 posting 会直接决定 BM25 的分数，
     * 页型不对还硬读的话，出来的是"看起来像结果"的错分数。
     */
    if (fr->data[H_TYPE] != (u8)PT_FT_HEAD ||
        fr->data[FTH_VER] != (u8)FT_HEAD_VER) {
        pager_unpin(pg, head_page, false);
        return E_CORRUPT;
    }

    seg = (FtSegment *)calloc(1, sizeof(*seg));
    if (!seg) {
        pager_unpin(pg, head_page, false);
        return E_IO;
    }

    seg->pg = pg;
    seg->head_page = head_page;
    ft_segment_load_head(seg, fr->data);

    pager_unpin(pg, head_page, false);
    *out = seg;
    return OK;
}

void ft_segment_close(FtSegment *seg)
{
    free(seg);
}

u64 ft_segment_page_reads(const FtSegment *seg)
{
    return seg ? seg->page_reads : 0;
}

u32 ft_segment_nterms(const FtSegment *seg)
{
    return seg ? seg->n_terms : 0;
}
