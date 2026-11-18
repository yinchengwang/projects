/*
 * ft_merge_iter.c —— 合并的迭代器层（从 ft_merge.c 拆出）
 *
 * 原 ft_merge.c 876 行，超了 800 行硬规则。拆出来的是**"怎么走词表、怎么归并 posting"**：
 *
 *   FtTermCursor          每个段一个词条游标（顺序走字典）
 *   ft_cursor_next/init/free
 *   FtPostBuf             posting 的归并输出缓冲
 *   ft_postbuf_reserve/free
 *   ft_merge_postings     两路归并倒排链（按 doc_id 严格递增）
 *   ft_tomb_find          tombstone 升序数组的二分
 *   ft_cursors_pick/advance/reset   多路词表的"选最小词"
 *
 * 留在 ft_merge.c 的是**"合并的策略与流程"**：
 *   ft_merge_count_pages  先数页数（坑 M4-51）
 *   ft_merge_pick_window  Tiered 窗口挑选
 *   ft_merge_segments      合并主流程（建新段 → 提交 → 归还旧页）
 *
 * 界线是"数据流 vs 控制流"：本文件只搬运与归并，不决定合谁、不合谁。
 * 跨文件符号（游标类型与六个原语）的声明在 ft_merge_iter.h。
 */

#include "modalities/ft/ft_merge.h"
#include "modalities/ft/ft_merge_iter.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/ft_internal.h"

/* ---------------- 词条游标 ---------------- */

/*
 * 一个段上的"当前词"游标。
 *
 * 它按字典序逐个吐出词条：字典页按页号顺序、页内按 slot 顺序，
 * 而"页号 + slot"就是字典序（建段时词是排好序写进去的）。
 */

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
            c->page_reads++;      /* 合并读了一页字典 */
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
int ft_cursor_init(FtTermCursor *c, const FtSegment *seg)
{
    memset(c, 0, sizeof(*c));
    c->seg = seg;
    c->page = seg->dict_start;
    return ft_cursor_next(c);
}

/* 放开游标持有的页 */
void ft_cursor_free(FtTermCursor *c)
{
    if (c->fr != NULL) {
        pager_unpin(c->seg->pg, c->page, false);
        c->fr = NULL;
    }
}

/* ---------------- posting 缓冲 ---------------- */

int ft_postbuf_reserve(FtPostBuf *b, u32 need)
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

void ft_postbuf_free(FtPostBuf *b)
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
int ft_merge_postings(FtPostBuf *dst, u32 *dst_n, const FtPosting *src, u32 src_n)
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
bool ft_tomb_find(const u32 *tombs, u32 n, u32 doc_id, u32 *idx_out)
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

/* ---------------- 词序遍历（两趟共用） ---------------- */

/*
 * 找出各段当前词里的最小者。
 *
 * found = false 表示所有段都读完了。
 * 词被拷进 term（不能指向游标里的缓冲：游标一推进就没了）。
 */
bool ft_cursors_pick(FtTermCursor *cursors, u32 n_segs,
                             char *term, u32 *term_len)
{
    u32 best = 0;
    bool found = false;

    for (u32 i = 0; i < n_segs; i++) {
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
        return false;
    }
    *term_len = cursors[best].term_len;
    memcpy(term, cursors[best].term, cursors[best].term_len);
    return true;
}

/*
 * 把"当前词就是这个词"的那些段的游标各推进一步。
 */
int ft_cursors_advance(FtTermCursor *cursors, u32 n_segs,
                               const char *term, u32 term_len)
{
    for (u32 i = 0; i < n_segs; i++) {
        int rc;

        if (!cursors[i].have) {
            continue;
        }
        if (ft_term_cmp(cursors[i].term, cursors[i].term_len, term, term_len) != 0) {
            continue;
        }
        rc = ft_cursor_next(&cursors[i]);
        if (rc != OK) {
            return rc;
        }
    }
    return OK;
}

/* 游标复位：放开当前页，从头再来一遍（第二趟要重走同一个词序） */
int ft_cursors_reset(FtTermCursor *cursors, u32 n_segs, FtSegment *const *segs)
{
    for (u32 i = 0; i < n_segs; i++) {
        int rc;

        ft_cursor_free(&cursors[i]);
        rc = ft_cursor_init(&cursors[i], segs[i]);
        if (rc != OK) {
            return rc;
        }
    }
    return OK;
}

