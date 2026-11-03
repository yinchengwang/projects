/*
 * ft_merge.c —— 段合并（v1.3 步骤 3）
 *
 * ==================== 合并在做什么 ====================
 *
 * 把若干个段合并成一个新段：
 *
 *   1. **k 路字典归并**：各段的词表都是有序的 → 归并出全局有序的词表；
 *   2. **posting 归并**：同一个词在几个段里都有 posting →
 *      几段各自的 posting 都按 doc_id 递增 → 归并（**不是拼接**）；
 *   3. **丢弃 tombstone 的文档**：被删的文档在这里**物理丢弃**；
 *   4. **重算统计量**：新段的 doc_count / total_len / max_doc_id 都要重算。
 *
 * delta 的重算**不在这一层**（坑 M4-2）：
 * 写入器收的是**绝对 doc_id**，delta 由 ft_writer.c 编码 ——
 * 只拼接不重算，解出来的 doc_id 全错，而且**不报错**。
 *
 * ==================== 内存策略：逐词推进，不缓存整张词表 ====================
 *
 * 归并的经典做法是"把每个段的词表读进内存，再 k 路归并"。
 * 但段可能有上百万个词，全部缓存就是几兆字节 —— 合并本来就是
 * "后台慢慢做"的事，不该因为内存爆掉。
 *
 * 所以这里用**流式游标**：每个段维护一个"当前词"，
 * 游标只持有**一页**字典（pin 着）+ 当前词的副本：
 *
 *   - 内存占用 = O(k) 页 + O(k) 个 posting 缓冲，与段的大小无关；
 *   - 归并的每一轮取"各段当前词的最小者"作为本次要处理的词，
 *     把所有段里这个词的 posting 归并成一条，再写进新段；
 *   - 处理完就把这些段的游标各推进一步。
 *
 * 词的**字节**必须拷进游标自己的缓冲（不能直接指向页里的记录）：
 * 推进游标会换页，指向旧页的指针就失效了。
 *
 * ==================== 为什么重复的 doc_id 报错，而不是挑一条留下 ====================
 *
 * 同一个 doc_id 出现在两个段里 = 这篇文档被索引了两次。
 * 挑一条留下 → 另一段里的 tf / dl 就丢了，分数与线性扫描对不上；
 * 两条都留 → 分数重复累加，也对不上。
 * 两种"凑合"都会**静默**产生错结果，所以直接报 E_CORRUPT。
 */

#include "modalities/ft/ft_merge.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"

/* ---------------- 词条游标 ---------------- */

/*
 * 一个段上的"当前词"游标。
 *
 * 它按字典序逐个吐出词条：字典页按页号顺序、页内按 slot 顺序，
 * 而"页号 + slot"就是字典序（建段时词是排好序写进去的）。
 */
typedef struct {
    const FtSegment *seg;
    u32        page;      /* 当前字典页；>= dict_start+dict_count 表示读完了 */
    PageFrame *fr;       /* 当前页（已 pin） */
    u16        slot;      /* 当前页的下一个 slot */
    bool       have;      /* 当前词有效 */
    char       term[FT_MAX_TOKEN_LEN];   /* 当前词的副本（不能指向页里的记录） */
    u32        term_len;
    u32        df;
    u64        ptr;
} FtTermCursor;

/* 取下一个词条；读完返回 OK 且 have = false */
static int ft_cursor_next(FtTermCursor *c)
{
    u32 end = c->seg->dict_start + c->seg->dict_count;

    for (;;) {
        const u8 *rec = NULL;
        u16 rlen = 0;
        u32 len;

        if (c->fr == NULL) {
            int rc;

            if (c->page >= end) {
                c->have = false;
                return OK;
            }
            rc = pager_get(c->seg->pg, c->page, &c->fr);
            if (rc != OK) {
                return rc;
            }
            c->slot = 0;
        }

        if (c->slot < page_slot_count(c->fr->data)) {
            if (page_get(c->fr->data, c->slot, &rec, &rlen) != OK) {
                return E_CORRUPT;     /* 字典页不该有空洞 */
            }
            if (rlen < FT_DICT_HDR_BYTES) {
                return E_CORRUPT;
            }
            len = ft_dict_entry_term_len(rec);
            if (len > FT_MAX_TOKEN_LEN || (u32)rlen < FT_DICT_HDR_BYTES + len) {
                return E_CORRUPT;
            }
            memcpy(c->term, rec + FT_DICT_HDR_BYTES, len);
            c->term_len = len;
            c->df = ft_dict_entry_df(rec, len);
            c->ptr = ft_dict_entry_post_ptr(rec, len);
            c->slot++;
            c->have = true;
            return OK;
        }

        /* 本页读完了 → 换下一页 */
        pager_unpin(c->seg->pg, c->page, false);
        c->fr = NULL;
        c->page++;
    }
}

/* 初始化游标并定位到第一个词条 */
static int ft_cursor_init(FtTermCursor *c, const FtSegment *seg)
{
    memset(c, 0, sizeof(*c));
    c->seg = seg;
    c->page = seg->dict_start;
    return ft_cursor_next(c);
}

/* 放开游标持有的页 */
static void ft_cursor_free(FtTermCursor *c)
{
    if (c->fr != NULL) {
        pager_unpin(c->seg->pg, c->page, false);
        c->fr = NULL;
    }
}

/* ---------------- posting 缓冲 ---------------- */

/*
 * 一个可增长的 posting 缓冲。
 *
 * 每个源段一个：读那个段的 posting 时用它（读进来的 posting 是临时的，
 * 归并进结果数组后就没用了）。
 */
typedef struct {
    FtPosting *buf;
    u32        cap;
} FtPostBuf;

static int ft_postbuf_reserve(FtPostBuf *b, u32 need)
{
    u32 ncap;
    FtPosting *nb;

    if (need <= b->cap) {
        return OK;
    }
    ncap = (b->cap == 0) ? 64u : b->cap;
    while (ncap < need) {
        if (ncap > 0x3FFFFFFFu) {
            return E_NOSPACE;         /* 翻倍溢出 */
        }
        ncap *= 2;
    }
    nb = (FtPosting *)realloc(b->buf, sizeof(FtPosting) * (size_t)ncap);
    if (nb == NULL) {
        return E_NOSPACE;
    }
    b->buf = nb;
    b->cap = ncap;
    return OK;
}

static void ft_postbuf_free(FtPostBuf *b)
{
    free(b->buf);
    b->buf = NULL;
    b->cap = 0;
}

/* ---------------- 两路归并 ---------------- */

/*
 * 把 src（按 doc_id 递增）归并进 dst（按 doc_id 递增），结果留在 dst。
 *
 * 发现**同一个 doc_id 在两边都出现** → E_CORRUPT：
 * 这说明这篇文档被索引了两次（doc_id 在索引内必须唯一）。
 * 挑一条留下或两条都留都会**静默**产生错分数，所以直接报错。
 */
static int ft_merge_postings(FtPostBuf *dst, u32 *dst_n, const FtPosting *src, u32 src_n)
{
    u32 dn = *dst_n;
    u32 i = 0;
    u32 j = 0;
    u32 w = 0;
    FtPosting *d;
    int rc;

    if (src_n == 0) {
        return OK;
    }

    rc = ft_postbuf_reserve(dst, dn + src_n);
    if (rc != OK) {
        return rc;
    }
    d = dst->buf;

    while (i < dn && j < src_n) {
        if (d[i].doc_id == src[j].doc_id) {
            return E_CORRUPT;
        }
        if (d[i].doc_id < src[j].doc_id) {
            d[w++] = d[i++];
        } else {
            d[w++] = src[j++];
        }
    }
    while (i < dn) {
        d[w++] = d[i++];
    }
    while (j < src_n) {
        d[w++] = src[j++];
    }

    *dst_n = w;
    return OK;
}

/* ---------------- tombstone ---------------- */

/*
 * tomb_doc_ids 按 doc_id 升序 → 二分判断成员。
 *
 * 命中时把下标写进 *idx_out：调用方用它记住"这个 doc_id 已经扣过统计量了"。
 */
static bool ft_tomb_find(const u32 *tombs, u32 n, u32 doc_id, u32 *idx_out)
{
    u32 lo = 0;
    u32 hi = n;

    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;

        if (tombs[mid] == doc_id) {
            *idx_out = mid;
            return true;
        }
        if (tombs[mid] < doc_id) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return false;
}

/* ---------------- 合并主流程 ---------------- */

/*
 * 把 segs[0..n_segs) 合并成一个新段（新段直接写到 pg 里）。
 */
int ft_merge_segments(Pager *pg, LogHookFn wal, void *wal_ctx, u32 xid,
                      FtSegment *const *segs, u32 n_segs,
                      const u32 *tomb_doc_ids, u32 n_tombs,
                      u32 new_seg_id, FtMergeResult *out)
{
    FtTermCursor *cursors = NULL;
    FtPostBuf *src_bufs = NULL;
    FtPostBuf merged;
    FtPostWriter *pw = NULL;
    FtDictWriter *dw = NULL;
    u8 *tomb_done = NULL;
    u32 head_pid = PAGE_ID_NIL;
    PageFrame *head_fr = NULL;
    u32 doc_count = 0;
    u64 total_len = 0;
    u32 max_doc_id = 0;
    u32 n_terms = 0;
    u32 create_lsn = 0;
    char term[FT_MAX_TOKEN_LEN];
    u32 term_len = 0;
    u32 i;
    int rc = OK;

    memset(&merged, 0, sizeof(merged));

    /* ---- 参数检查 ---- */
    if (pg == NULL || out == NULL || segs == NULL || n_segs == 0) {
        return E_INVAL;
    }
    if (tomb_doc_ids == NULL && n_tombs > 0) {
        return E_INVAL;
    }
    for (i = 0; i < n_segs; i++) {
        if (segs[i] == NULL) {
            return E_INVAL;
        }
    }

    cursors = (FtTermCursor *)calloc((size_t)n_segs, sizeof(FtTermCursor));
    src_bufs = (FtPostBuf *)calloc((size_t)n_segs, sizeof(FtPostBuf));
    if (cursors == NULL || src_bufs == NULL) {
        rc = E_NOSPACE;
        goto done;
    }
    if (n_tombs > 0) {
        tomb_done = (u8 *)calloc((size_t)n_tombs, 1);
        if (tomb_done == NULL) {
            rc = E_NOSPACE;
            goto done;
        }
    }

    /*
     * 段头页**先分配**（页号最小、最好记），内容最后写：
     * 段头要记字典/倒排的页区间，那些要等页都分配完才知道。
     *
     * 这里分配完就立刻放开：ft_write_segment_head 的签名里没有帧参数，
     * 它自己会 pager_get 这一页来写。
     */
    rc = pager_alloc(pg, &head_pid, &head_fr);
    if (rc != OK) {
        goto done;
    }
    pager_unpin(pg, head_pid, false);
    head_fr = NULL;

    /* ---- 初始化各段的游标与统计量 ---- */
    for (i = 0; i < n_segs; i++) {
        rc = ft_cursor_init(&cursors[i], segs[i]);
        if (rc != OK) {
            goto done;
        }
        doc_count += segs[i]->doc_count;
        total_len += segs[i]->total_len;
    }

    /* ---- 写入器 ---- */
    pw = ft_post_writer_new(pg, wal, wal_ctx, xid);
    dw = ft_dict_writer_new(pg, wal, wal_ctx, xid);
    if (pw == NULL || dw == NULL) {
        rc = E_NOSPACE;
        goto done;
    }

    /* ---- k 路归并主循环 ---- */
    for (;;) {
        u32 best = 0;
        bool found = false;
        u32 merged_n = 0;
        u64 ptr = FT_PTR_NIL;

        /* 找各段当前词里的最小者 */
        for (i = 0; i < n_segs; i++) {
            if (!cursors[i].have) {
                continue;
            }
            if (!found ||
                ft_term_cmp(cursors[i].term, cursors[i].term_len,
                            cursors[best].term, cursors[best].term_len) < 0) {
                best = i;
                found = true;
            }
        }
        if (!found) {
            break;      /* 所有段都读完了 */
        }

        /* 拷一份词出来：游标一推进，cursors[best].term 就变了 */
        term_len = cursors[best].term_len;
        memcpy(term, cursors[best].term, term_len);

        /* 把这个词在**所有**段里的 posting 归并成一条 */
        for (i = 0; i < n_segs; i++) {
            u32 pn = 0;

            if (!cursors[i].have) {
                continue;
            }
            if (ft_term_cmp(cursors[i].term, cursors[i].term_len, term, term_len) != 0) {
                continue;
            }

            /* 读这个段里该词的 posting（缓冲按 df 预留，装不下就是格式坏了） */
            rc = ft_postbuf_reserve(&src_bufs[i], cursors[i].df);
            if (rc != OK) {
                goto done;
            }
            rc = ft_segment_postings(segs[i], cursors[i].ptr, cursors[i].df,
                                     src_bufs[i].buf, src_bufs[i].cap, &pn);
            if (rc != OK) {
                goto done;
            }

            rc = ft_merge_postings(&merged, &merged_n, src_bufs[i].buf, pn);
            if (rc != OK) {
                goto done;
            }

            /* 这个段的游标推进一步 */
            rc = ft_cursor_next(&cursors[i]);
            if (rc != OK) {
                goto done;
            }
        }

        /*
         * 丢弃 tombstone 的文档（物理丢弃，不写进新段），
         * 同时把统计量扣掉。
         *
         * 扣法（ft_merge.h 里写死的口径）：
         *   doc_count 扣"**不同的** doc_id 数"；
         *   total_len 扣"被丢弃文档的 dl 之和"，同一 doc_id 只扣一次。
         *
         * "只扣一次"靠 tomb_done[]：二分找到的**下标**就是身份，
         * 第一次丢弃时记一笔，之后同一篇文档的其它 posting 就不再扣。
         */
        {
            u32 r;
            u32 w = 0;

            for (r = 0; r < merged_n; r++) {
                u32 idx = 0;

                if (n_tombs > 0 &&
                    ft_tomb_find(tomb_doc_ids, n_tombs, merged.buf[r].doc_id, &idx)) {
                    if (!tomb_done[idx]) {
                        tomb_done[idx] = 1;
                        doc_count--;
                        total_len -= (u64)merged.buf[r].dl;
                    }
                    continue;       /* 物理丢弃 */
                }
                if (merged.buf[r].doc_id > max_doc_id) {
                    max_doc_id = merged.buf[r].doc_id;
                }
                merged.buf[w++] = merged.buf[r];
            }
            merged_n = w;
        }

        /* 全部 posting 都被删掉了 → 这个词不进新段 */
        if (merged_n == 0) {
            continue;
        }

        /*
         * 写新段：先写 posting（拿到链头），再写字典词条。
         *
         * 写入器收的是**绝对 doc_id**，delta 由它算（坑 M4-2）。
         */
        rc = ft_post_writer_put(pw, merged.buf, merged_n, &ptr);
        if (rc != OK) {
            goto done;
        }
        rc = ft_dict_writer_put(dw, term, term_len, merged_n, ptr);
        if (rc != OK) {
            goto done;
        }
        n_terms++;
    }

    /* ---- 收尾：两个写入器都要 flush（漏一个最后一页就不落盘） ---- */
    rc = ft_post_writer_flush(pw);
    if (rc != OK) {
        goto done;
    }
    rc = ft_dict_writer_flush(dw);
    if (rc != OK) {
        goto done;
    }

    /* 段头的 create_lsn 取两个写入器里较大的那个 */
    create_lsn = ft_post_writer_last_lsn(pw);
    if (ft_dict_writer_last_lsn(dw) > create_lsn) {
        create_lsn = ft_dict_writer_last_lsn(dw);
    }

    /* ---- 写段头页 ---- */
    rc = ft_write_segment_head(pg, wal, wal_ctx, xid, head_pid, new_seg_id,
                               doc_count, max_doc_id,
                               ft_dict_writer_start(dw), ft_dict_writer_count(dw),
                               ft_post_writer_start(pw), ft_post_writer_count(pw),
                               n_terms, total_len, create_lsn);
    if (rc != OK) {
        goto done;
    }

    /* ---- 填结果 ---- */
    out->head_page = head_pid;
    out->doc_count = doc_count;
    out->total_len = total_len;
    out->n_terms = n_terms;
    out->max_doc_id = max_doc_id;

done:
    if (pw != NULL) {
        ft_post_writer_free(pw);
    }
    if (dw != NULL) {
        ft_dict_writer_free(dw);
    }
    if (cursors != NULL) {
        for (i = 0; i < n_segs; i++) {
            ft_cursor_free(&cursors[i]);
        }
        free(cursors);
    }
    if (src_bufs != NULL) {
        for (i = 0; i < n_segs; i++) {
            ft_postbuf_free(&src_bufs[i]);
        }
        free(src_bufs);
    }
    ft_postbuf_free(&merged);
    free(tomb_done);
    if (head_fr != NULL) {
        pager_unpin(pg, head_pid, false);
    }
    return rc;
}
