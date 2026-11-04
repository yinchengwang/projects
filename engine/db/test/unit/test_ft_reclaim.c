/*
 * test_ft_reclaim.c —— 段级空闲页表 + 合并"同页复用"（v1.3 欠账 2 / 欠账 4）
 *
 * 4 项：
 *   1. 合并**不再让文件增长**：合并前后 pager_npages() 的增量 ≈ 0
 *      （不回收时，合并一次就要多占"新段那么大的"空间）；
 *      合并前后检索结果（doc_id 序列）逐条一致。
 *   2. 空闲页池：合并提交之后，老段的页一次性进池（ft_index_free_pages > 0）。
 *   3. 回滚安全：合并失败（写日志失败）→ 老段仍然可用、检索结果不变、
 *      空闲页池**一个页都不许多**（老段的页不能提前进池）。
 *   4. 合并的逻辑页读（欠账 4）："逻辑页读次数 / 段页数"的前后比值。
 *
 * 【为什么"文件不增长"是这一轮的核心指标】
 *
 * 合并把 f 个旧段合成一个新段：新段要占"新段那么大的"空间，
 * 而旧段的页**没人引用了**却还占着位置 ——
 * 不回收的话，磁盘文件就随合并次数**线性增长**（每合并一轮翻一倍）。
 * 回收之后，新段直接**用旧段腾出来的页**，文件就不再增长。
 *
 * 【为什么复用的粒度是"一段连号的页"】
 *
 * 坑 M4-23：段的页必须**连号**（段头只记连续区间，读侧按连续区间读页）。
 * 空闲页池里的页是零散的 —— 一页一页发出去，段就不连号了，
 * 读侧会把倒排页当成字典页（查不到 / 命中 0 / 静默丢词）。
 * 所以复用的粒度是"一段连号的页"（FtBlock）。
 *
 * 【欠账 4 的度量口径】
 *
 * ⚠️ **逻辑页读（pager_get 的次数），不是物理磁盘 IO。**
 * buffer pool 会把页缓存起来，同一页反复取只是多次**逻辑**读；
 * 真正的物理 IO 由 buffer pool 的换入换出决定。
 * 这个口径与 A4/A5 用的页读口径**完全一致**（都是 pager_get 次数），所以前后可比。
 *
 * 【第 4 项怎么测"前后"】
 *
 * "前后"用的是**同一批段、同一批词**：
 *   - 前 = 用 ft_segment_postings（老路径：每个词重新 pager_get 一次它那一页）；
 *   - 后 = 用 ft_segment_postings_cached（新路径：相邻词复用同一页）。
 * 两者的差别只在"取页的方式"，读的词与 posting 完全一样 → 可比。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_merge.h"
#include "modalities/ft/ft_segment.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/b2_reclaim.db"

/* ---------------- 确定性语料 ---------------- */

#define BATCHES     20u      /* 批次数（= 段数）：要能合够多轮 */
#define PER_BATCH   100u     /* 每批篇数 */
#define DOC_N       (BATCHES * PER_BATCH)   /* 2000 篇 */
#define RATIO_SEGS  10u      /* 测页读比值时只用前 10 段（= 1000 篇） */
#define WORDS_PER   20u      /* 每篇的词数 */
#define VOCAB       2000u    /* 词表大小（词多 → 词数远大于段页数） */
#define WORD_BYTES  7u       /* "wNNNN " —— 词长 5 + 1 个分隔符 */

static char      g_text[DOC_N][WORDS_PER * WORD_BYTES];
static FtDocText g_docs[DOC_N];

/*
 * 语料：第 i 篇的第 j 个词 = (i * WORDS_PER + j) % VOCAB。
 *
 * 词表大（2000）而每篇只有 20 个词 → 一个段里大约 2000 个词，
 * 而这些词的 posting 只占十几个页 ——
 * 这正是"逻辑页读 O(词数) vs 段页数 O(页数)"的量级差（欠账 4）。
 */
static void build_corpus(void)
{
    u32 i;

    for (i = 0; i < DOC_N; i++) {
        u32 off = 0;

        for (u32 j = 0; j < WORDS_PER; j++) {
            u32 w = (i * WORDS_PER + j) % VOCAB;
            u32 v = w;

            g_text[i][off++] = 'w';
            g_text[i][off++] = (char)('0' + (v / 1000u) % 10u);
            v %= 1000u;
            g_text[i][off++] = (char)('0' + (v / 100u) % 10u);
            v %= 100u;
            g_text[i][off++] = (char)('0' + (v / 10u) % 10u);
            v %= 10u;
            g_text[i][off++] = (char)('0' + v % 10u);
            g_text[i][off++] = ' ';
        }
        g_docs[i].doc_id = 5000u + i;
        g_docs[i].text = g_text[i];
        g_docs[i].text_len = off;
    }
}

static void fresh(void)
{
    remove(PATH);
    remove(PATH "-wal");
}

/* 建一个装了 BATCHES 个段的索引（每段 PER_BATCH 篇） */
static int build_index(Pager *pg, LogHookFn wal, FtIndex **out)
{
    FtIndex *ix = NULL;
    int rc;

    rc = ft_index_create(pg, wal, NULL, 0, &ix);
    if (rc != OK) {
        return rc;
    }
    for (u32 b = 0; b < BATCHES; b++) {
        u32 seg_id = 0;

        rc = ft_index_add(ix, 0, &g_docs[b * PER_BATCH], PER_BATCH, &seg_id);
        if (rc != OK) {
            ft_index_close(ix);
            return rc;
        }
    }
    *out = ix;
    return OK;
}

/* 两条结果的 doc_id 序列必须一模一样（顺序也要一样） */
static int same_doc_seq(const FtHit *a, u32 n_a, const FtHit *b, u32 n_b)
{
    if (n_a != n_b) {
        return 0;
    }
    for (u32 i = 0; i < n_a; i++) {
        if (a[i].doc_id != b[i].doc_id) {
            return 0;
        }
    }
    return 1;
}

/* ---------------- 1 + 2：合并不增长文件 + 结果一致 + 页进了池 ---------------- */

/*
 * 合并的窗口是"相邻窗口里文档数之和最小"的那个 ——
 * 段的大小一样时，窗口宽度就是 2（两个 100 篇的段），
 * 每一轮合并把段数减 1（10 → 9 → 8 …）。
 */
static void test_growth_and_consistency(void)
{
    static FtHit before[64];
    static FtHit after[64];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    u32 n_before = 0;
    u32 n_after = 0;
    u32 merged = 0;
    u32 pages_pre = 0;
    u32 pages_post = 0;
    u32 new_seg_pages = 0;
    u32 free_after = 0;
    u32 round = 0;
    u32 grown_rounds = 0;     /* 让文件变大的合并轮数 */
    u64 baseline = 0;         /* 不回收的话，文件要多占多少页 */
    const FtSegment *seg = NULL;
    int rc;

    printf("  1. 合并不再让文件增长 + 合并前后结果一致\n");
    fresh();

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, NULL, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }
    CHECK(ft_index_nsegs(ix) == BATCHES);

    /* 打印前两个段的页构成与"合并它们需要的页数上界"（复用能不能成的关键） */
    {
        FtSegment *pair[2];
        FtSegBound bound;
        u64 freed = 0;

        for (u32 i = 0; i < 2; i++) {
            const FtSegment *s = NULL;

            CHECK(ft_index_seg_at(ix, i, &s) == OK);
            pair[i] = (FtSegment *)s;
            if (s != NULL) {
                freed += 1u + s->post_count + s->dict_count;
            }
        }
        ft_seg_bound_merge(pair, 2, &bound);
        printf("    前两段共占 %llu 页；合并它们的上界是 %u 页（段头 %u + 倒排 %u + 字典 %u）\n",
               (unsigned long long)freed, bound.total, bound.head_pages,
               bound.post_pages, bound.dict_pages);
        for (u32 i = 0; i < 2; i++) {
            const FtSegment *s = NULL;

            CHECK(ft_index_seg_at(ix, i, &s) == OK);
            if (s != NULL) {
                printf("      第 %u 段：倒排 %u 页 + 字典 %u 页（词数 %u）\n",
                       i, s->post_count, s->dict_count, s->n_terms);
            }
        }
    }

    /* 合并前：检索一次 + 记下文件页数 */
    rc = ft_index_search(ix, "w0007 w0100", 10, 32, 64, before, &n_before);
    CHECK(rc == OK);
    pages_pre = pager_npages(pg);

    /*
     * 一轮一轮地合并（段数降到阈值 8 就不合并了）。
     *
     * 第 1 轮：空闲页池是**空的**（还没合过）→ 只能向文件尾追加，
     * 文件会长大"新段那么大"—— 这是必然的，没有旧页可复用。
     * 第 2 轮起：老段腾出来的页已经在池里了 → 新段直接用它们，
     * 文件**不再增长**。这一轮才是"回收"真正生效的地方。
     */
    while (ft_index_nsegs(ix) > FT_MERGE_THRESHOLD) {
        u32 p0 = pager_npages(pg);
        u32 seg_pages_now = 0;

        rc = ft_index_merge(ix, 0, &merged);
        CHECK(rc == OK);
        if (rc != OK) {
            break;
        }
        round++;
        if (pager_npages(pg) > p0) {
            grown_rounds++;
        }
        /* 本轮新段占了几页（不回收的话，文件就要多占这么多） */
        for (u32 i = 0; i < ft_index_nsegs(ix); i++) {
            const FtSegment *s = NULL;

            if (ft_index_seg_at(ix, i, &s) != OK || s == NULL) {
                continue;
            }
            if (s->doc_count == 2u * PER_BATCH) {
                seg_pages_now = 1u + s->post_count + s->dict_count;
                break;
            }
        }
        baseline += seg_pages_now;
        printf("    第 %u 轮合并：文件页数 %llu → %llu（增量 %llu），新段 %u 页，空闲页池 %u 页\n",
               round, (unsigned long long)p0, (unsigned long long)pager_npages(pg),
               (unsigned long long)(pager_npages(pg) - p0), seg_pages_now,
               ft_index_free_pages(ix));
    }
    pages_post = pager_npages(pg);

    /* 合并后：检索一次，结果必须逐条一致 */
    rc = ft_index_search(ix, "w0007 w0100", 10, 32, 64, after, &n_after);
    CHECK(rc == OK);
    CHECK(same_doc_seq(before, n_before, after, n_after));

    /* 新段占了几页（段头 + 倒排 + 字典）：不回收时合并就要多占这么多页 */
    CHECK(ft_index_seg_at(ix, 0, &seg) == OK);
    if (seg != NULL) {
        new_seg_pages = 1u + seg->post_count + seg->dict_count;
        printf("    合并后的段占 %u 页（不回收的话，每轮合并都要多占这么多）\n",
               new_seg_pages);
    }

    /* 老段的页进了空闲页池（合并提交之后才进） */
    free_after = ft_index_free_pages(ix);
    CHECK(free_after > 0u);
    CHECK(ft_index_recycled_pages(ix) > 0u);

    printf("    共 %u 轮合并；文件页数 %llu → %llu（其中 %u 轮让文件变大了）\n",
           round, (unsigned long long)pages_pre, (unsigned long long)pages_post, grown_rounds);
    printf("    不回收的话这 %u 轮合并要多占 %llu 页；实际只多了 %llu 页\n",
           round, (unsigned long long)baseline,
           (unsigned long long)(pages_post - pages_pre));
    printf("    空闲页池：%u 页（累计回收 %llu 页）\n",
           free_after, (unsigned long long)ft_index_recycled_pages(ix));

    /*
     * 核心断言：
     *   - 只有**头两轮**会让文件变大（第 1 轮池是空的，第 2 轮池里的页
     *     比需要的少一页 —— 池子要先"攒"够）；之后每轮都是 0 增长；
     *   - 总增长远小于"不回收"的基线。
     */
    CHECK(round >= 2u);
    CHECK(grown_rounds <= 2u);
    CHECK((u64)(pages_post - pages_pre) * 2u < baseline);

    ft_index_close(ix);
    pager_close(pg);
}

/* ---------------- 3：回滚安全（合并失败 → 老段仍然可用） ---------------- */

/*
 * 写日志钩子：平时正常返回；"armed"之后第 fail_at 次调用开始返回错误。
 *
 * 用它把合并**打断**在写新段的中途 —— 这是最真实的失败点
 * （页已经写了一部分，新段还没写完）。
 */
static int g_wal_calls = 0;
static int g_wal_armed = 0;
static int g_wal_fail_at = 0;

static int wal_fail_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                         u16 slot_off, const u8 *before, u16 before_len,
                         const u8 *after, u16 after_len, u32 *lsn_out)
{
    (void)ctx; (void)xid; (void)logtype; (void)page_id; (void)slot;
    (void)slot_off; (void)before; (void)before_len; (void)after; (void)after_len;

    g_wal_calls++;
    if (g_wal_armed && g_wal_calls >= g_wal_fail_at) {
        return E_IO;      /* 日志写不进去 → 这一页不许改（ft_page_write 会还原） */
    }
    if (lsn_out) {
        *lsn_out = 0;     /* 没有真日志：LSN 取 0 */
    }
    return OK;
}

static void test_rollback(void)
{
    static FtHit before[64];
    static FtHit after[64];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    u32 n_before = 0;
    u32 n_after = 0;
    u32 merged = 0;
    u32 free_before = 0;
    u32 free_after = 0;
    u32 segs_before = 0;
    int rc;

    printf("  3. 回滚安全：合并失败 → 老段仍然可用、页不提前进池\n");
    fresh();

    g_wal_calls = 0;
    g_wal_armed = 0;
    g_wal_fail_at = 0;

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, wal_fail_hook, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }
    segs_before = ft_index_nsegs(ix);
    free_before = ft_index_free_pages(ix);
    CHECK(segs_before == BATCHES);
    CHECK(free_before == 0u);

    rc = ft_index_search(ix, "w0007 w0100", 10, 32, 64, before, &n_before);
    CHECK(rc == OK);

    /*
     * 让合并失败：标 MERGING 那一步之后、写新段的中途失败。
     *
     * fail_at 取"当前调用数 + 3"：前两次是标 MERGING / 写段头之类，
     * 第三次开始就是写新段的内容页 —— 那里失败，老段已经被标过 MERGING，
     * 回滚（abort_merge）必须把它改回 LIVE。
     */
    g_wal_armed = 1;
    g_wal_fail_at = g_wal_calls + 3;
    rc = ft_index_merge(ix, 0, &merged);
    g_wal_armed = 0;
    CHECK(rc != OK);

    /* 老段必须还在（回滚成功） */
    CHECK(ft_index_nsegs(ix) == segs_before);
    CHECK(merged == 0u);

    /* 老段仍然可用：检索结果与失败前逐条一致 */
    rc = ft_index_search(ix, "w0007 w0100", 10, 32, 64, after, &n_after);
    CHECK(rc == OK);
    CHECK(same_doc_seq(before, n_before, after, n_after));

    /* 空闲页池一个页都不许多（老段的页不能提前进池） */
    free_after = ft_index_free_pages(ix);
    CHECK(free_after == free_before);
    printf("    合并失败后：段数 %u（不变），空闲页池 %u → %u（不变）\n",
           ft_index_nsegs(ix), free_before, free_after);

    ft_index_close(ix);
    pager_close(pg);
}

/* ---------------- 4：合并的逻辑页读（欠账 4：同页复用） ---------------- */

/* 收集一个段的全部词条（df + 倒排链头），用来喂"老路径"的读法 */
typedef struct {
    u64 *ptrs;
    u32 *dfs;
    u32  n;
    u32  cap;
} TermList;

static int term_collect(void *ctx, const char *term, u32 term_len, u32 df, u64 post_ptr)
{
    TermList *t = (TermList *)ctx;

    (void)term;
    (void)term_len;
    if (t->n == t->cap) {
        u32 ncap = (t->cap == 0) ? 256u : t->cap * 2u;
        u64 *np = (u64 *)realloc(t->ptrs, sizeof(u64) * (size_t)ncap);
        u32 *nd = (u32 *)realloc(t->dfs, sizeof(u32) * (size_t)ncap);

        if (np == NULL || nd == NULL) {
            free(np);
            free(nd);
            return E_NOSPACE;
        }
        t->ptrs = np;
        t->dfs = nd;
        t->cap = ncap;
    }
    t->ptrs[t->n] = post_ptr;
    t->dfs[t->n] = df;
    t->n++;
    return OK;
}

static void test_page_read_ratio(void)
{
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    FtSegment *segs[BATCHES];
    const FtSegment *cseg = NULL;
    u64 before_reads = 0;
    u64 after_reads = 0;
    u64 seg_pages = 0;
    u64 terms = 0;
    u32 n_segs = 0;
    FtMergeResult res;
    int rc;

    printf("  4. 合并的逻辑页读：同页复用前后（口径：pager_get 次数，不是物理 IO）\n");
    fresh();

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, NULL, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }
    n_segs = ft_index_nsegs(ix);
    CHECK(n_segs == BATCHES);
    n_segs = RATIO_SEGS;    /* 只用前 10 段（1000 篇）做这个度量 */

    /* ---- "前"：老路径（每个词重新 pager_get 一次它那一页） ---- */
    for (u32 i = 0; i < n_segs; i++) {
        TermList tl;
        FtPosting *buf = NULL;
        u32 cap = 0;
        u64 base;

        memset(&tl, 0, sizeof(tl));
        CHECK(ft_index_seg_at(ix, i, &cseg) == OK);
        if (cseg == NULL) {
            continue;
        }
        segs[i] = (FtSegment *)cseg;

        /* 先把词表收出来（这一步的页读不算进去：它与合并无关） */
        CHECK(ft_segment_scan_terms(cseg, term_collect, &tl) == OK);
        terms += tl.n;

        base = ft_segment_page_reads(cseg);
        for (u32 t = 0; t < tl.n; t++) {
            u32 df = tl.dfs[t];
            u32 got = 0;

            if (df > cap) {
                FtPosting *nb = (FtPosting *)realloc(buf, sizeof(FtPosting) * (size_t)df);

                if (nb == NULL) {
                    break;
                }
                buf = nb;
                cap = df;
            }
            rc = ft_segment_postings(cseg, tl.ptrs[t], df, buf, cap, &got);
            CHECK(rc == OK);
        }
        before_reads += ft_segment_page_reads(cseg) - base;

        /* 段占了几页（段头 + 倒排 + 字典） */
        seg_pages += 1u + cseg->post_count + cseg->dict_count;

        free(buf);
        free(tl.ptrs);
        free(tl.dfs);
    }

    /* ---- "后"：新路径（相邻词复用同一页）—— 直接调合并 ---- */
    {
        u64 base[BATCHES];

        for (u32 i = 0; i < n_segs; i++) {
            base[i] = ft_segment_page_reads(segs[i]);
        }
        memset(&res, 0, sizeof(res));
        rc = ft_merge_segments(pg, NULL, NULL, NULL, 0, segs, n_segs,
                               NULL, 0, 9999u, &res);
        CHECK(rc == OK);
        for (u32 i = 0; i < n_segs; i++) {
            after_reads += ft_segment_page_reads(segs[i]) - base[i];
        }
    }

    printf("    词数 %llu，段页数 %llu（%u 段）\n",
           (unsigned long long)terms, (unsigned long long)seg_pages, n_segs);
    printf("    前（每词一次 pager_get）：逻辑页读 %llu → 每段页 %.2f 次\n",
           (unsigned long long)before_reads, (double)before_reads / (double)seg_pages);
    printf("    后（同页复用）：        逻辑页读 %llu → 每段页 %.2f 次\n",
           (unsigned long long)after_reads, (double)after_reads / (double)seg_pages);
    printf("    前后比值：%.1fx\n",
           (after_reads > 0) ? (double)before_reads / (double)after_reads : 0.0);

    /* 同页复用之后，逻辑页读应当接近"段页数"的量级（而不是"词数"的量级） */
    CHECK(after_reads < before_reads);
    CHECK(after_reads <= seg_pages);

    ft_index_close(ix);
    pager_close(pg);
}

int main(void)
{
    printf("=== test_ft_reclaim：段级空闲页表 + 合并同页复用 ===\n");
    build_corpus();

    test_growth_and_consistency();
    test_rollback();
    test_page_read_ratio();

    if (failures == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 处失败 ===\n", failures);
    return 1;
}
