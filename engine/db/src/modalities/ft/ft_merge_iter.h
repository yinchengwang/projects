#ifndef DB_MODALITIES_FT_MERGE_ITER_H
#define DB_MODALITIES_FT_MERGE_ITER_H

/*
 * ft_merge_iter.h —— 合并迭代器层的内部契约
 *
 * 只在 ft_merge.c（用）与 ft_merge_iter.c（定义）之间共享。
 * 不开到公共契约 ft_merge.h：游标与 posting 缓冲是实现细节，
 * 对外只需要 ft_merge_pick_window / ft_merge_segments。
 *
 * 从 ft_merge.c 拆出 ft_merge_iter.c 时（原文件 876 行超限）新建本头。
 */

#include "modalities/ft/ft_merge.h"
#include "storage/page.h"

#include <stdbool.h>

/* 一个段上的"当前词"游标（按字典序逐个吐出词条） */
typedef struct {
    const FtSegment *seg;
    u32        page;      /* 当前字典页；>= dict_start+dict_count 表示读完 */
    PageFrame *fr;        /* 当前页（已 pin） */
    u16        slot;      /* 当前页的下一个 slot */
    bool       have;      /* 当前词有效 */
    char       term[FT_MAX_TOKEN_LEN];   /* 当前词的副本（不能指向页里的记录） */
    u32        term_len;
    u32        df;
    u64        ptr;
    u64        page_reads;   /* 这个游标读过的字典页数 */
} FtTermCursor;

/* 一个可增长的 posting 缓冲（每个源段一个） */
typedef struct {
    FtPosting *buf;
    u32        cap;
} FtPostBuf;

/* ---------------- 跨文件的六个原语（原 static） ---------------- */

int  ft_cursor_init(FtTermCursor *c, const FtSegment *seg);
void ft_cursor_free(FtTermCursor *c);
int  ft_cursors_reset(FtTermCursor *cursors, u32 n_segs, FtSegment *const *segs);
bool ft_cursors_pick(FtTermCursor *cursors, u32 n_segs, char *term, u32 *term_len);
int  ft_cursors_advance(FtTermCursor *cursors, u32 n_segs,
                        const char *term, u32 term_len);
int  ft_merge_postings(FtPostBuf *dst, u32 *dst_n, const FtPosting *src, u32 src_n);
bool ft_tomb_find(const u32 *tombs, u32 n, u32 doc_id, u32 *idx_out);
int  ft_postbuf_reserve(FtPostBuf *b, u32 need);
void ft_postbuf_free(FtPostBuf *b);

#endif /* DB_MODALITIES_FT_MERGE_ITER_H */
