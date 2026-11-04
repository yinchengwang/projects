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
    u64        page_reads;   /* 这个游标读过的字典页数（合并路径的页读） */
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

/* ---------------- 词序遍历（两趟共用） ---------------- */

/*
 * 找出各段当前词里的最小者。
 *
 * found = false 表示所有段都读完了。
 * 词被拷进 term（不能指向游标里的缓冲：游标一推进就没了）。
 */
static bool ft_cursors_pick(FtTermCursor *cursors, u32 n_segs,
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
static int ft_cursors_advance(FtTermCursor *cursors, u32 n_segs,
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
static int ft_cursors_reset(FtTermCursor *cursors, u32 n_segs, FtSegment *const *segs)
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

/* ---------------- 页数上界：先数清楚（坑 M4-51） ---------------- */

/*
 * 走一遍源段的字典，把新段要写的**字节数**数清楚，换算成页数。
 *
 * 【为什么要专门数一遍】
 *
 * 老的做法（ft_seg_bound_merge）：上界 = Σ 源段的页数 × 每页容量。
 * 它把"源段的总容量"当成了"新段的总字节" —— 只有在**两个段的词完全不重叠**
 * 时才准。词表重叠时（比如两段都是那 2000 个常用词），新段其实只比
 * 一个段大一点点，上界却翻了一倍。
 *
 * 上界翻倍的直接后果：空闲页池里根本没有那么长的连号段可借 →
 * 每次合并都向文件尾追加 → 文件**每轮都在长**（页回收等于白做）。
 *
 * 【这一趟读什么】
 *
 * 只读源段的**字典页**（不读 posting）：字典条目里有 df，
 * df 加上"词长"就够算出新段的字节数了。
 *
 * ⚠️ 数出来的 df 是**归并前**的（还没扣 tombstone 删掉的文档），
 * 所以算出来的字节数是上界（可能偏大，不会偏小）✓ 安全。
 *
 * 【页数公式】
 *
 * 和建段那条路（ft_build_page_bound）**同一个公式**：
 *   倒排页 = 倒排字节 ÷ (每页容量 − 每页最多浪费的字节) + 1
 *   字典页 = 字典字节 ÷ (每页容量 − 每页最多浪费的字节) + 1
 */
static int ft_merge_count_pages(FtTermCursor *cursors, u32 n_segs,
                                u32 *post_pages_out, u32 *dict_pages_out)
{
    u64 post_bytes = 0;
    u64 dict_bytes = 0;
    u32 post_cap = PAGE_SIZE - FPH_DATA;
    u32 dict_cap = FT_DICT_CAPACITY;
    u32 dict_max_need = ft_dict_entry_bytes(FT_MAX_TOKEN_LEN) + SLOT_SIZE;
    char term[FT_MAX_TOKEN_LEN];
    u32 term_len = 0;

    for (;;) {
        u32 merged_df = 0;
        int rc;

        /* 找各段当前词里的最小者（拷一份词出来：游标一推进就没了） */
        if (!ft_cursors_pick(cursors, n_segs, term, &term_len)) {
            break;      /* 所有段都读完了 */
        }

        /* 这个词在**所有**段里的 df 之和 = 归并后这个词的 posting 数 */
        for (u32 i = 0; i < n_segs; i++) {
            if (!cursors[i].have) {
                continue;
            }
            if (ft_term_cmp(cursors[i].term, cursors[i].term_len, term, term_len) != 0) {
                continue;
            }
            merged_df += cursors[i].df;
        }

        /* 倒排：一个 run 头 + df 条 posting */
        post_bytes += (u64)FT_RUN_HDR_BYTES + (u64)merged_df * (u64)FT_POSTING_BYTES;
        /* 字典：一个词条 + 一个 slot */
        dict_bytes += (u64)ft_dict_entry_bytes(term_len) + (u64)SLOT_SIZE;

        rc = ft_cursors_advance(cursors, n_segs, term, term_len);
        if (rc != OK) {
            return rc;
        }
    }

    *post_pages_out = (u32)(post_bytes / (u64)(post_cap - 35u)) + 1u;
    *dict_pages_out = (u32)(dict_bytes / (u64)(dict_cap - dict_max_need)) + 1u;
    return OK;
}

/* ---------------- 合并策略：挑窗口 ---------------- */

/*
 * 挑合并窗口：在所有长度为 2..n_segs 的**相邻窗口**里，挑"文档数之和最小"的那个。
 * 并列时的取法（必须确定性）：文档数之和更小的胜 → 并列取**窗口更大**的
 * → 还并列取**起点更小**的。
 *
 * 【为什么挑文档数之和最小的】合并要读这些段的字典与倒排、再写出一个新段，代价
 * 正比于参与的文档数；而且 Tiered 的思路就是"小段先合"。
 * 【为什么并列时选更大的窗口】窗口更大 = 一次把更多的段收成一段 → 段数降得更多
 * → 后续合并触发得更晚；同样的代价下少合并几轮更划算。
 *
 * 窗口个数 O(n²)、每个窗口求和 O(n) → 总共 O(n³)；段数被阈值卡在十几以内，
 * 这个代价可以忽略（真要担心，把前缀和存下来即可）。
 */
void ft_merge_pick_window(u32 n_segs, FtSegment *const *segs,
                           u32 *start_out, u32 *width_out)
{
    u32 best_start = 0;
    u32 best_w = 0;
    u64 best_sum = 0;
    bool have = false;

    for (u32 w = 2; w <= n_segs; w++) {
        for (u32 st = 0; st + w <= n_segs; st++) {
            u64 sum = 0;
            bool better;

            for (u32 k = 0; k < w; k++) {
                sum += segs[st + k]->doc_count;
            }
            better = (!have) ||
                     (sum < best_sum) ||
                     (sum == best_sum && w > best_w) ||
                     (sum == best_sum && w == best_w && st < best_start);
            if (better) {
                have = true;
                best_sum = sum;
                best_w = w;
                best_start = st;
            }
        }
    }
    *start_out = best_start;
    *width_out = best_w;
}

/* ---------------- 合并主流程 ---------------- */

/*
 * 把 segs[0..n_segs) 合并成一个新段（新段直接写到 pg 里）。
 */
int ft_merge_segments(Pager *pg, FtPagePool *pool,
                      LogHookFn wal, void *wal_ctx, u32 xid,
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
    u32 post_pages = 0;   /* 倒排页数上界（数页数那一趟算出来的） */
    char term[FT_MAX_TOKEN_LEN];
    u32 term_len = 0;
    u32 i;
    int rc = OK;
    FtBlock head_blk;
    FtBlock post_blk;
    FtBlock dict_blk;
    FtPostCache *pcache = NULL;   /* 每段一个 posting 页缓存（v1.3 欠账 4） */

    memset(&merged, 0, sizeof(merged));
    memset(&head_blk, 0, sizeof(head_blk));
    memset(&post_blk, 0, sizeof(post_blk));
    memset(&dict_blk, 0, sizeof(dict_blk));

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

    /* ---- 初始化各段的游标与统计量（游标要先定位到第一个词条） ---- */
    for (i = 0; i < n_segs; i++) {
        rc = ft_cursor_init(&cursors[i], segs[i]);
        if (rc != OK) {
            goto done;
        }
        doc_count += segs[i]->doc_count;
        total_len += segs[i]->total_len;
    }

    /*
     * ---- 页数上界：**先数清楚**再要页（坑 M4-51）----
     *
     * 老的做法（ft_seg_bound_merge）：上界 = Σ 源段的页数 × 每页容量。
     * 它把"源段的总容量"当成了"新段的总字节" —— 只有在**两个段的词完全不重叠**
     * 时才准。词表重叠时（比如两段都是那 2000 个常用词），新段其实只比
     * 一个段大一点点，上界却翻了一倍。
     *
     * 上界翻倍的直接后果：空闲页池里根本没有那么长的连号段可借 →
     * 每次合并都向文件尾追加 → 文件**每轮都在长**（页回收等于白做）。
     *
     * 这里改成：先走一遍字典（只数不写），把新段要写的字节数数清楚，
     * 再换算成页数 —— 和建段那条路（ft_build_page_bound）同一个公式，
     * 上界贴得很紧（词表完全重叠时也只多算一点点）。
     *
     * ⚠️ 这一趟数出来的 df 是**归并前**的（还没扣 tombstone 删掉的文档），
     * 所以算出来的字节数是上界（可能偏大，不会偏小）✓ 安全。
     *
     * 代价：字典页多读一遍？—— **没有**。数完这一趟之后，写那一趟
     * 把倒排和字典**交替写**（两个写入器各用各的页块，页各自连号，
     * 坑 M4-23 的约束依然满足），不用再单独走第二趟写字典 → 总页读不增加。
     */
    {
        u32 dict_pages = 0;

        rc = ft_merge_count_pages(cursors, n_segs, &post_pages, &dict_pages);
        if (rc != OK) {
            goto done;
        }
        /* 数完要复位：写那一趟要重走同一个词序 */
        rc = ft_cursors_reset(cursors, n_segs, segs);
        if (rc != OK) {
            goto done;
        }

        /*
         * ---- 页块：字典区**必须连号**，倒排区**不必**（坑 M4-52）----
         *
         * 字典区：读侧按"连续区间"（dict_start + i）二分查词 →
         *   页号不连号就会读到别的页（查不到 / 命中 0 / 静默丢词）。
         *   所以字典区**先占好一整段**（池里有就借池里的，
         *   池里凑不出就在文件尾预留一段）→ 之后两个写入器交替写也顶不掉它。
         *
         * 倒排区：读侧顺着 posting 的链走，页与页之间靠页链（H_NEXT）串着 →
         *   页号**不必连号** → 用"池模式"：从池里按整段连号一段一段取，
         *   段与段之间由写入器用页链串起来。
         *
         * ---- 坑 M4-50：一次要三块，不要一次要一整块 ----
         *
         * 段的**格式**只要求字典区自己连号（倒排区现在也不要求了）。
         * 以前这里一次要 bound.total（头+倒排+字典）页的**一整段连号**，
         * 池里凑不齐 → 每轮合并都往文件尾追加，文件**每轮都在长**
         * （这就是 test_ft_reclaim 红了的原因）。
         */
        rc = ft_seg_block_reserve_strict(pool, pg, dict_pages, &dict_blk);
        if (rc != OK) {
            goto done;
        }
        ft_seg_block_reserve(pool, pg, 1u, &head_blk);
        ft_block_init_pool(&post_blk, pg, pool);
    }

    /*
     * 段头页**先取**（内容最后写：段头要记字典/倒排的页区间，
     * 那些要等页都分配完才知道）。
     *
     * 有界块时这一页是**旧段腾出来的页**：取用时整页清零
     * （旧段的页头 / 私有区残留会被读侧当成新段的格式 —— 坑 M4-31）。
     */
    rc = ft_block_alloc(&head_blk, &head_pid, &head_fr);
    if (rc != OK) {
        goto done;
    }
    pager_unpin(pg, head_pid, true);
    head_fr = NULL;

    /*
     * ---- 写入器（倒排一块页、字典另一块页 → 两个区各自连号） ----
     *
     * 两个写入器**不能**共用一块页：交替分配会让页号交错，
     * 读侧按连续区间读页就会读到另一种页（坑 M4-23）。
     *
     * 各自一块页之后，写入就可以**交替**进行了：
     * 倒排页从 post_blk 里连续取、字典页从 dict_blk 里连续取，
     * 两个区各自连号 ✓ —— 坑 M4-23 的约束依然满足。
     */
    pw = ft_post_writer_new(pg, &post_blk, wal, wal_ctx, xid);
    dw = ft_dict_writer_new(pg, &dict_blk, wal, wal_ctx, xid);
    if (pw == NULL || dw == NULL) {
        rc = E_NOSPACE;
        goto done;
    }

    /*
     * 每个源段一个 posting 页缓存（v1.3 欠账 4）：
     * 合并按**字典序**读 posting，倒排页也是按这个词序写的 ——
     * 相邻两个词的 posting 往往在同一张页上，命中缓存就省一次逻辑页读。
     */
    pcache = (FtPostCache *)calloc((size_t)n_segs, sizeof(FtPostCache));
    if (pcache == NULL) {
        rc = E_NOSPACE;
        goto done;
    }
    for (i = 0; i < n_segs; i++) {
        ft_post_cache_init(&pcache[i], pg);
    }

    /*
     * ---- 写入这一趟：k 路归并，倒排与字典**交替**写 ----
     *
     * 字典页必须连号（段头只记连续区间）—— 但两个写入器**各用各的页块**
     * （post_blk / dict_blk），所以交替写不会让页号交错：
     * 倒排页从 post_blk 连续取、字典页从 dict_blk 连续取，两个区各自连号 ✓。
     *
     * 【为什么可以省掉"第二趟写字典"】
     * 以前要分两趟，是因为两个写入器共用一块页（交替分配 → 页号交错）。
     * 现在各用各的块，一趟就够 —— 字典页读的总次数不变（省一趟）✓。
     *
     * */
    for (;;) {
        u32 merged_n = 0;
        u64 ptr = FT_PTR_NIL;

        /* 找各段当前词里的最小者（拷一份词出来：游标一推进就没了） */
        if (!ft_cursors_pick(cursors, n_segs, term, &term_len)) {
            break;      /* 所有段都读完了 */
        }

        /* 把这个词在**所有**段里的 posting 归并成一条 */
        for (i = 0; i < n_segs; i++) {
            u32 pn = 0;

            if (!cursors[i].have) {
                continue;
            }
            if (ft_term_cmp(cursors[i].term, cursors[i].term_len, term, term_len) != 0) {
                continue;
            }

            /*
             * 读这个段里该词的 posting（缓冲按 df 预留，装不下就是格式坏了）。
             * 用**带缓存**的那一版：相邻词的 posting 多半在同一张页上，
             * 命中缓存就省一次逻辑页读（v1.3 欠账 4）。
             * ⚠️ 口径：逻辑页读（pager_get 次数），不是物理磁盘 IO。
             */
            rc = ft_postbuf_reserve(&src_bufs[i], cursors[i].df);
            if (rc != OK) {
                goto done;
            }
            rc = ft_segment_postings_cached(segs[i], &pcache[i],
                                            cursors[i].ptr, cursors[i].df,
                                            src_bufs[i].buf, src_bufs[i].cap, &pn);
            if (rc != OK) {
                goto done;
            }

            rc = ft_merge_postings(&merged, &merged_n, src_bufs[i].buf, pn);
            if (rc != OK) {
                goto done;
            }
        }

        /*
         * 这个词的游标推进一步。
         *
         * 第二趟用**同样的推进方式** → 同样的词序 → 才能按顺序取 recs[]。
     * ⚠️ 现在的写法：数页数那一趟与写入那一趟用的是**同一个**推进方式，
     * 词序一致 ✓（数出来的字节数与实际写出来的对得上）。
         */
        rc = ft_cursors_advance(cursors, n_segs, term, term_len);
        if (rc != OK) {
            goto done;
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

        /* 全部 posting 都被删光了 → 这个词不进新段（字典也不写） */
        if (merged_n == 0) {
            continue;
        }

        /*
         * 先写倒排（拿到链头），紧接着写这个词的字典条目。
         *
         * 写入器收的是**绝对 doc_id**，delta 由它算（坑 M4-2）。
         *
         * ⚠️ 顺序有讲究：字典条目里记的是倒排链头，所以必须**先写倒排**。
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

    /* ---- 倒排页收尾（最后半页落盘） ---- */
    rc = ft_post_writer_flush(pw);
    if (rc != OK) {
        goto done;
    }

    /*
     * 倒排页数不该超过上界（数页数那一趟算出来的）。
     *
     * 超过就说明那一趟算错了（页数预算与实际写入对不上）——
     * 宁可报错，也不要写出一个"与预算不符"的段（页数对不上时，
     * 后面按页数算的东西（回收、回收判定）全会偏）。
     */
    if (ft_post_writer_count(pw) > post_pages) {
        rc = E_CORRUPT;
        goto done;
    }

    /* ---- 收尾：字典页也要 flush（漏一个最后一页就不落盘） ---- */
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

    /*
     * 块里没用完的尾巴**还给空闲页池**（那些页一个字节都没写过）。
     * ⚠️ 只有**成功**时才还：失败时新段的页是半成品，发出去就是发垃圾。
     *
     * 三块（头/倒排/字典）各自的尾巴都要还 —— 少还一块，
     * 那块就等于从池里消失了（页被占着又不写内容，白白浪费）。
     */
    if (pool != NULL) {
        if (head_blk.end != PAGE_ID_NIL && head_blk.next < head_blk.end) {
            (void)ft_pool_add_range(pool, head_blk.next, head_blk.end - head_blk.next);
        }
        /*
         * 倒排块是**池模式**：它的"尾巴"是"最后取的那一段没用完的部分" ——
         * 那一段是连号的，还回去池里仍然是一段连号页 ✓（坑 M4-52）。
         */
        (void)ft_block_pool_release_tail(&post_blk, pool);
        if (dict_blk.end != PAGE_ID_NIL && dict_blk.next < dict_blk.end) {
            (void)ft_pool_add_range(pool, dict_blk.next, dict_blk.end - dict_blk.next);
        }
    }
    /* ---- 填结果 ---- */
    out->head_page = head_pid;
    out->doc_count = doc_count;
    out->total_len = total_len;
    out->n_terms = n_terms;
    out->max_doc_id = max_doc_id;
    /*
     * 合并读的字典页数（游标读的）。
     *
     * 两趟（坑 M4-23）意味着字典页被读了**两遍** ——
     * 这是"字典页必须连号"换来的代价，如实计入。
     */
    out->page_reads = 0;
    for (i = 0; i < n_segs; i++) {
        out->page_reads += cursors[i].page_reads;
    }

done:
    if (pw != NULL) {
        ft_post_writer_free(pw);
    }
    if (dw != NULL) {
        ft_dict_writer_free(dw);
    }
    if (pcache != NULL) {
        for (i = 0; i < n_segs; i++) {
            ft_post_cache_release(&pcache[i]);
        }
        free(pcache);
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
