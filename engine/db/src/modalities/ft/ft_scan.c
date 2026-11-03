/*
 * ft_scan.c —— 段的扫描接口（读侧，v1.3 步骤 3）
 *
 * ==================== 这一层在做什么 ====================
 *
 * 步骤 2 的检索（ft_search.c）只需要"查一个词"；
 * 步骤 3 的合并（ft_merge.c）需要"把整个段摊开看"：
 * 遍历全部词条、读出某个词的全部 posting。
 *
 * 所以这一层是**读侧的通用出口**：三个函数都只读段，不写段，
 * 也不改段里的任何内容（段是不可变的）。
 *
 * ==================== 三个函数 ====================
 *
 *   1. ft_segment_lookup     —— 查一个词：df + 倒排链头
 *   2. ft_segment_scan_terms —— 按字典序遍历全部词条（回调式）
 *   3. ft_segment_postings   —— 读一个词的**全部** posting
 *
 * ==================== 为什么 ft_segment_lookup 与 ft_search.c 里的是两份 ====================
 *
 * ft_search.c 里那份是 static 的（检索侧内部用），这里是公开的一份（合并侧用）。
 * 两份的算法完全一样：
 *
 *   - 跨页二分：用 **slot 0 的记录**当"本页第一个词"（坑 M4-13：
 *     字典页是 slotted page，slot 目录从偏移 20 开始往上长，
 *     那里放不下自定义字段，所以"本页第一个词"只能取 slot 0）；
 *   - 页内二分：slot i 就是第 i 个词（插入顺序 = 字典序）。
 *
 * 【将来应该合并到一处】：把 ft_search.c 里那份改成非静态、声明在 ft_segment.h，
 * 检索侧与合并侧共用同一个实现 —— 这样"字典的序"和"二分的序"永远是一套，
 * 不会出现"检索能找到、合并找不到"这种漂移。
 * （ft_segment.h 是只读契约，本轮没有改动它，所以先各写一份。）
 *
 * ==================== 页读统计 ====================
 *
 * 这一层**把页读记进** seg->page_reads：索引层的检索就是靠这三个函数读页的，
 * 不记的话，索引级的页读计数永远是 0（坑 M4-16：索引级计数是"各段计数的
 * 差量累加"，段级不记，差量就恒为 0）。
 *
 * 段里 page_reads 的语义仍是"该段自打开以来读过的页数"（累计，段内不清零）。
 * 合并要读的页比检索多得多（整个字典 + 整个倒排），混进来会把这个指标搅浑 ——
 * 但**看不出来**：合并掉的段随后就被释放了，新段的计数从 0 开始，
 * 所以被搅浑的只是"已经被扔掉的段"的数字，活着的段仍然只反映检索。
 */

#include "modalities/ft/ft_internal.h"

#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"

/* 一个词的倒排链最多允许串多少段（防御：坏数据导致的死循环） */
#define FT_MAX_RUN_CHAIN 65536u

/*
 * 页读计数（与 ft_search.c 里那份同味）。
 *
 * seg 是 const —— 但 page_reads 只是**统计字段**，我们只改它，
 * 不改段里的任何内容（段是不可变的）。
 */
static void ft_scan_count_page_read(const FtSegment *seg)
{
    FtSegment *m = (FtSegment *)seg;

    m->page_reads++;
}

/* ---------------- 字典页的读取 ---------------- */

/*
 * 取字典页的"第一个词"（跨页二分查找的支点）。
 *
 * 坑 M4-13：字典页**没有自定义区** —— slotted page 的 slot 目录
 * 从偏移 HDR_SIZE(=20) 开始往上长，写自定义区会被 page_insert 覆盖。
 * 所以"本页第一个词"就是 **slot 0 的记录**（插入顺序 = 字典序）。
 *
 * 返回 false 表示"这一页读不出第一个词"（页是空的 / 页坏了）。
 */
static bool ft_dict_page_first_term(const u8 *page, const char **term_out, u32 *len_out)
{
    const u8 *rec = NULL;
    u16 rlen = 0;
    u32 len;

    if (page_slot_count(page) == 0) {
        /* 空页：第一个词当成空串（比任何词都小），二分不会跳过它 */
        *term_out = "";
        *len_out = 0;
        return true;
    }

    if (page_get(page, 0, &rec, &rlen) != OK || rlen < FT_DICT_HDR_BYTES) {
        return false;
    }

    len = ft_dict_entry_term_len(rec);
    if (len > FT_MAX_TOKEN_LEN || (u32)rlen < FT_DICT_HDR_BYTES + len) {
        return false;      /* 词条长度不自洽：页坏了 */
    }

    *term_out = (const char *)(rec + FT_DICT_HDR_BYTES);
    *len_out = len;
    return true;
}

/*
 * 从字典页的第 slot 个记录里取出词条。
 *
 * 词条布局（ft_internal.h）：u16 term_len + 词字节 + u32 df + u64 post_ptr。
 *
 * 取出的是**页缓冲里的指针** —— 调用方只能在页被放开之前用
 * （所以扫描回调里要 memcpy 走，不能存着以后再用）。
 */
static int ft_dict_entry_at(const u8 *page, u16 slot,
                            const char **term_out, u32 *len_out,
                            u32 *df_out, u64 *post_ptr_out)
{
    const u8 *rec = NULL;
    u16 rlen = 0;
    u32 len;

    if (page_get(page, slot, &rec, &rlen) != OK) {
        /* 字典页的记录只增不删，不该有空洞 */
        return E_CORRUPT;
    }
    if (rlen < FT_DICT_HDR_BYTES) {
        return E_CORRUPT;
    }

    len = ft_dict_entry_term_len(rec);
    if (len > FT_MAX_TOKEN_LEN || (u32)rlen < FT_DICT_HDR_BYTES + len) {
        return E_CORRUPT;
    }

    *term_out = (const char *)(rec + FT_DICT_HDR_BYTES);
    *len_out = len;
    *df_out = ft_dict_entry_df(rec, len);
    *post_ptr_out = ft_dict_entry_post_ptr(rec, len);
    return OK;
}

/* ---------------- 1）查字典 ---------------- */

/*
 * 查字典：找 term 的 df 与倒排链头。
 *
 * 两级二分：
 *   跨页：字典页按"本页第一个词"递增 → 二分找**最后一个首词 <= term 的页**；
 *   页内：slot 就是词，词在页内也是递增的 → 二分找 term 本身。
 *
 * 找不到返回 E_NOTFOUND —— 这是**正常结果**（词不在这个段里），不是错误。
 */
int ft_segment_lookup(const FtSegment *seg, const char *term, u32 term_len,
                      u32 *df_out, u64 *post_ptr_out)
{
    u32 lo;
    u32 hi;
    u32 cand = 0;
    bool found_page = false;
    PageFrame *fr = NULL;
    u32 cand_page;
    int rc;

    if (seg == NULL || term == NULL || df_out == NULL || post_ptr_out == NULL) {
        return E_INVAL;
    }
    if (seg->dict_count == 0) {
        return E_NOTFOUND;
    }

    /* ---- 跨页二分 ---- */
    lo = 0;
    hi = seg->dict_count - 1;
    while (lo <= hi) {
        u32 mid = lo + (hi - lo) / 2;
        u32 pid = seg->dict_start + mid;
        const char *ft = NULL;
        u32 ftlen = 0;
        int cmp;

        rc = pager_get(seg->pg, pid, &fr);
        if (rc != OK) {
            return rc;
        }
        ft_scan_count_page_read(seg);
        if (!ft_dict_page_first_term(fr->data, &ft, &ftlen)) {
            /* 这一页读不出"第一个词" → 字典结构不可信，宁可当没找到 */
            pager_unpin(seg->pg, pid, false);
            return E_NOTFOUND;
        }
        cmp = ft_term_cmp(ft, ftlen, term, term_len);
        pager_unpin(seg->pg, pid, false);
        fr = NULL;

        if (cmp <= 0) {
            /* 这一页的第一个词 <= term → term 可能在这一页或后面的页里 */
            cand = mid;
            found_page = true;
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }

    if (!found_page) {
        /* term 比所有页的第一个词都小 → 字典里没有 */
        return E_NOTFOUND;
    }

    /* ---- 页内二分 ---- */
    cand_page = seg->dict_start + cand;
    rc = pager_get(seg->pg, cand_page, &fr);
    if (rc != OK) {
        return rc;
    }
    ft_scan_count_page_read(seg);

    lo = 0;
    hi = page_slot_count(fr->data);
    while (lo < hi) {
        u32 mid = lo + (hi - lo) / 2;
        const char *t = NULL;
        u32 tlen = 0;
        u32 df = 0;
        u64 ptr = 0;
        int cmp;

        rc = ft_dict_entry_at(fr->data, (u16)mid, &t, &tlen, &df, &ptr);
        if (rc != OK) {
            break;
        }
        cmp = ft_term_cmp(t, tlen, term, term_len);
        if (cmp == 0) {
            *df_out = df;
            *post_ptr_out = ptr;
            pager_unpin(seg->pg, cand_page, false);
            return OK;
        }
        if (cmp < 0) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }

    pager_unpin(seg->pg, cand_page, false);
    return E_NOTFOUND;
}

/* ---------------- 2）遍历全部词条 ---------------- */

/*
 * 按字典序遍历段里的全部词条。
 *
 * 字典页是按页号顺序、页内按 slot 顺序存的，
 * 而"页号顺序 + slot 顺序"就是字典序（建段时词是排好序写进去的），
 * 所以顺着页号走一遍就是全局字典序 —— 合并端的 k 路归并靠的就是这个。
 *
 * 回调收到的 term 指针**指向页缓冲**：回调返回前一直有效，
 * 回调里可以 memcpy 走，但不能存着以后再用（页会被放开）。
 *
 * 回调返回非 OK → 立刻停止遍历，并把该值**原样**返回。
 */
int ft_segment_scan_terms(const FtSegment *seg, FtTermFn fn, void *ctx)
{
    u32 pid;
    u32 end;

    if (seg == NULL || fn == NULL) {
        return E_INVAL;
    }

    end = seg->dict_start + seg->dict_count;
    for (pid = seg->dict_start; pid < end; pid++) {
        PageFrame *fr = NULL;
        u16 nslots;
        u16 s;
        int rc;

        rc = pager_get(seg->pg, pid, &fr);
        if (rc != OK) {
            return rc;
        }

        nslots = page_slot_count(fr->data);
        rc = OK;
        for (s = 0; s < nslots; s++) {
            const char *t = NULL;
            u32 tlen = 0;
            u32 df = 0;
            u64 ptr = 0;

            rc = ft_dict_entry_at(fr->data, s, &t, &tlen, &df, &ptr);
            if (rc != OK) {
                break;
            }
            /* 回调期间 fr 还被 pin 着 → t 指向的页缓冲有效 */
            rc = fn(ctx, t, tlen, df, ptr);
            if (rc != OK) {
                break;
            }
        }

        pager_unpin(seg->pg, pid, false);
        if (rc != OK) {
            return rc;      /* 回调的错误原样返回 */
        }
    }

    return OK;
}

/* ---------------- 3）读一个词的全部 posting ---------------- */

/*
 * 这个位置看起来像一个合法的 run 头吗？
 *
 * 判据（三条都要满足）：
 *   1. run 头落在页内、且在数据区里；
 *   2. npost > 0；
 *   3. run 头 + 它带的 posting 整段都在页内。
 *
 * 段格式只定死了一种布局，校验不通过就是段坏了（E_CORRUPT），
 * 而不是"换个位置再猜一次"。
 */
static bool ft_run_looks_valid(const u8 *page, u32 off)
{
    u32 npost;

    if (off < FPH_DATA || off + FT_RUN_HDR_BYTES > PAGE_SIZE) {
        return false;
    }
    npost = ft_run_npost(page + off);
    if (npost == 0) {
        return false;
    }
    if (off + FT_RUN_HDR_BYTES + npost * FT_POSTING_BYTES > PAGE_SIZE) {
        return false;
    }
    return true;
}

/*
 * 读一个词的**全部** posting（按 doc_id 递增）。
 *
 * 【链的走法：只有一条路】
 *
 *   FTR_NEXT_RUN = 下一个 run 所在**页号**（0 = 链尾）；
 *   续接的 run 一定落在那一页的 FPH_DATA 处。
 *
 * 【doc 的解码】
 *
 * 每个 run 都从**自己的** FTR_FIRST_DOC 起步，再逐条 doc += FTP_DELTA。
 * FTR_FIRST_DOC 是"本段第一个 posting 的**绝对** doc_id"，
 * 而本段第一个 posting 的 delta 恒为 0 —— 所以每段都要重新起步。
 *
 * 【装不下 → E_NOSPACE】
 *
 * df 就是这个词的 posting 条数，所以"装不下"在读之前就知道：
 * df > max_out 直接报错，**不静默截断**（截断出来的 posting 列表
 * 看起来对其实错 —— 少掉的往往是后面那几篇，比报错坏得多）。
 */
int ft_segment_postings(const FtSegment *seg, u64 post_ptr, u32 df,
                        FtPosting *out, u32 max_out, u32 *n_out)
{
    u32 page = ft_ptr_page(post_ptr);
    u32 off = ft_ptr_off(post_ptr);
    u32 n = 0;
    u32 last_doc = 0;
    bool have_last = false;
    u32 guard = 0;
    PageFrame *fr = NULL;
    u32 cur_page = 0;
    bool have_frame = false;
    int rc = OK;

    if (seg == NULL || n_out == NULL) {
        return E_INVAL;
    }
    *n_out = 0;

    if (out == NULL) {
        /* 没有地方放结果：df 非 0 就是装不下（E_NOSPACE），否则这个参数没意义 */
        return (df > 0) ? E_NOSPACE : E_INVAL;
    }
    if (df > max_out) {
        /* 装不下 → 明确报错，不静默截断 */
        return E_NOSPACE;
    }

    for (;;) {
        const u8 *data;
        u32 npost;
        u32 next_run;
        u32 first_doc;
        u32 doc;

        if (++guard > FT_MAX_RUN_CHAIN) {
            rc = E_CORRUPT;     /* 链太长：段坏了 */
            goto done;
        }

        /* 换页才重新 pin */
        if (!have_frame || page != cur_page) {
            if (have_frame) {
                pager_unpin(seg->pg, cur_page, false);
                have_frame = false;
            }
            rc = pager_get(seg->pg, page, &fr);
            if (rc != OK) {
                goto done;
            }
            ft_scan_count_page_read(seg);
            cur_page = page;
            have_frame = true;
        }

        data = fr->data;
        if (!ft_run_looks_valid(data, off)) {
            rc = E_CORRUPT;
            goto done;
        }
        npost = ft_run_npost(data + off);
        next_run = ft_run_next_run(data + off);
        first_doc = ft_run_first_doc(data + off);
        if (have_last && first_doc <= last_doc) {
            /*
             * 倒排是按 doc_id 递增的：本段首文档必须大于上一段最后一个。
             * 不满足 = 段坏了 —— 宁可报错，也不要返回错的 doc_id。
             */
            rc = E_CORRUPT;
            goto done;
        }

        doc = first_doc;
        for (u32 i = 0; i < npost; i++) {
            const u8 *p = data + off + FTR_POSTS + i * FT_POSTING_BYTES;

            doc += ft_posting_delta(p);
            if (n >= max_out) {
                /* 理论上 df <= max_out 时不会走到这里（格式坏了） */
                rc = E_NOSPACE;
                goto done;
            }
            out[n].doc_id = doc;
            out[n].tf = ft_posting_tf(p);
            out[n].dl = ft_posting_dl(p);
            n++;
        }

        last_doc = doc;
        have_last = true;

        /* 这个词的 posting 读完了 */
        if (df != 0 && n >= df) {
            break;
        }

        if (next_run == 0) {
            break;      /* 链尾 */
        }
        page = next_run;        /* 下一段所在页的页号 */
        off = FPH_DATA;         /* 续接的 run 一定落在那一页的 FPH_DATA 处 */
    }

    *n_out = n;
    rc = OK;

done:
    if (have_frame) {
        pager_unpin(seg->pg, cur_page, false);
    }
    return rc;
}
