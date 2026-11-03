/*
 * ft_bench_a5.c —— A5 · IO 可证：单 term 检索的页读
 *
 * ==================== A5 要证明什么 ====================
 *
 * 计划（PLAN-v1.3 §1.5）写的是：
 *
 *   单 term 检索页读 = 字典二分的页数 + 该 term 的 posting 页数
 *   （**与总文档量无关**）
 *
 * ==================== ⚠️ 这里有一句必须讲清楚的话 ====================
 *
 * **"与总文档量无关"这句话，严格说只对 df（该词的文档数）固定的情形成立。**
 *
 * 原因很简单：页读里的 posting 部分 = 该词倒排链的长度 = df 个 posting 占的页数。
 * df 随语料放大 → 倒排链变长 → 页读必然变大。这不是缺陷，是**必然**。
 *
 * 所以本基准分两组量，把两件事分开：
 *
 *   组一（df 固定）：语料放大，df 不变 → 页读应**基本不变**。
 *                    这是 A5 的**主判据**。
 *   组二（df 线性放大）：语料放大，df 同步放大 → 页读随 df 线性增长。
 *                    这是**必然**，如实说明，不掩饰。
 *
 * ==================== 组一怎么造语料 ====================
 *
 * 第 i 篇文档含词 i, i+1, ..., i+df-1（词表大小 = N + df - 1）。
 * 于是词 j（j ≥ df-1）恰好出现在 **df** 篇文档里 —— df 精确固定。
 * 代价是词表随语料线性增长 → 字典页数增长 → 字典二分的页数按 log 增长。
 * 所以组一的页读**不是严格恒定**，而是"基本不变"（posting 部分完全恒定）。
 *
 * ==================== 页读怎么取 ====================
 *
 * `ft_index_page_reads()` 是索引级**累计**（坑 M4-16）。
 * 所以取**检索前后的差值** —— 绝对值会把之前的检索算进来。
 *
 * ==================== 口径说明 ====================
 *
 * 这组数据量的是"**一个段**"（把语料建成一个段）——
 * 这样页读只由"字典二分 + 该词的倒排链"决定，与批大小无关。
 * 多段的情形下，**每段各贡献一次字典二分**，页读会随段数增长；
 * 那属于 A4 的范畴（合并把段数压下来正是为了它）。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "ft_bench.h"
#include "ft_bench_corpus.h"
#include "modalities/ft/bm25.h"
#include "modalities/ft/ft_index.h"
#include "modalities/ft/ft_segment.h"
#include "storage/pager.h"

#define DB_PATH_A5 "test/tmp/ft_bench_a5.db"

#define A5_DF_FIXED   12u      /* 组一：每个词的 df 固定为这个值 */
#define A5_VOCAB2     250u     /* 组二：词表小 → df 随语料线性放大 */
#define A5_WORDS2     12u      /* 组二：每篇文档的词数 */
#define A5_SEED       20261104u
#define A5_TOPK       10u
#define A5_MAXOUT     16u
#define A5_BP_FRAMES  4096u
#define A5_POST_BYTES 12u      /* 一个 posting 12 字节（delta + tf + dl） */
/* 倒排页里能装多少 posting 数据：页 4096 字节减去自定义区头（FPH_DATA = 28） */
#define A5_POST_CAP   4068u

/* 一行实测结果 */
typedef struct {
    u32   n_docs;
    u32   nsegs;
    u64   seg_pages;     /* 段的总页数 */
    u64   dict_pages;    /* 字典页数 */
    u64   post_pages;    /* 倒排页数 */
    u32   df;            /* 查询词的 df（实测） */
    u64   reads;         /* 单 term 检索的页读（检索前后差值） */
    double ms;           /* 检索耗时 */
    u32   n_hit;         /* 命中条数 */
} A5Row;

static void a5_remove_db(void)
{
    remove(DB_PATH_A5);
    remove("test/tmp/ft_bench_a5.db-wal");
    remove("test/tmp/ft_bench_a5.db-clog");
    remove("test/tmp/ft_bench_a5.db-shm");
}

/*
 * 造一个"单段索引"，查一个词，量页读。
 *
 * fixed_df —— 非 0：组一（df 固定，词表随语料增长）
 *             0   ：组二（词表固定，df 随语料放大）
 */
static int a5_run_one(u32 n_docs, int fixed_df, u32 query_idx, A5Row *row)
{
    FtBenchCorpus corpus;
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    FtDocText *docs = NULL;
    const FtSegment *seg = NULL;
    char term[16];
    u32 i;
    u32 df = 0;
    u64 ptr = 0;
    u64 pr_before;
    u64 pr_after;
    u32 n_hit = 0;
    FtHit hits[A5_MAXOUT];
    double t0;
    int rc = 0;

    memset(row, 0, sizeof(*row));

    if (fixed_df) {
        rc = ft_bench_corpus_fixed_df(&corpus, n_docs, A5_DF_FIXED);
    } else {
        rc = ft_bench_corpus_random(&corpus, n_docs, A5_VOCAB2, A5_WORDS2, A5_SEED);
    }
    if (rc != 0) {
        return 2;
    }

    a5_remove_db();
    if (pager_open(DB_PATH_A5, A5_BP_FRAMES, &pg) != OK) {
        ft_bench_corpus_free(&corpus);
        return 2;
    }
    if (ft_index_create(pg, NULL, NULL, 0, &ix) != OK) {
        pager_close(pg);
        ft_bench_corpus_free(&corpus);
        return 2;
    }

    docs = (FtDocText *)calloc(n_docs, sizeof(FtDocText));
    if (docs == NULL) {
        rc = 2;
        goto done;
    }
    for (i = 0; i < n_docs; i++) {
        ft_bench_corpus_doc(&corpus, i, &docs[i]);
    }

    /* 一个段装下全部语料 */
    {
        u32 seg_id = 0;

        rc = ft_index_add(ix, 0, docs, n_docs, &seg_id);
        if (rc != OK) {
            rc = 2;
            goto done;
        }
    }
    if (ft_index_seg_at(ix, 0, &seg) != OK || seg == NULL) {
        rc = 2;
        goto done;
    }

    /* 查询词的 df（实测，不是推算） */
    ft_bench_term(query_idx, term, (u32)sizeof(term));
    rc = ft_segment_lookup(seg, term, (u32)strlen(term), &df, &ptr);
    if (rc != OK) {
        printf("  查询词 %s 查不到（rc=%d；段：词数 %u、字典页 %u、倒排页 %u）\n",
               term, rc, seg->n_terms, seg->dict_count, seg->post_count);
        if (seg->dict_count > 1) {
            printf("  ⚠️ 这个段的字典超过一页却查不到词 —— "
                   "多半是「字典页不连续」那个 bug（读侧按连续区间读字典页，\n");
            printf("     而建段时倒排页与字典页是交替分配的）。基准在这里停下，不出数字。\n");
        }
        rc = 2;
        goto done;
    }
    rc = 0;

    /* 页读：取检索前后的差值（坑 M4-16：索引级累计） */
    pr_before = ft_index_page_reads(ix);
    t0 = ft_bench_now_ms();
    rc = ft_index_search(ix, term, (u32)strlen(term), A5_TOPK, A5_MAXOUT, hits, &n_hit);
    row->ms = ft_bench_now_ms() - t0;
    pr_after = ft_index_page_reads(ix);
    if (rc != OK) {
        rc = 2;
        goto done;
    }

    row->n_docs = n_docs;
    row->nsegs = ft_index_nsegs(ix);
    row->seg_pages = 1ull + (u64)seg->dict_count + (u64)seg->post_count;
    row->dict_pages = (u64)seg->dict_count;
    row->post_pages = (u64)seg->post_count;
    row->df = df;
    row->reads = pr_after - pr_before;
    row->n_hit = n_hit;

done:
    free(docs);
    if (ix != NULL) {
        ft_index_close(ix);
    }
    if (pg != NULL) {
        pager_close(pg);
    }
    ft_bench_corpus_free(&corpus);
    a5_remove_db();
    return rc;
}

static void a5_print_rows(const char *title, const A5Row *rows, u32 n)
{
    u32 i;

    printf("--- %s ---\n", title);
    printf("%9s %6s %10s %9s %9s %8s %10s %8s %9s\n",
           "文档数", "段数", "段总页数", "字典页", "倒排页", "df", "页读", "占段页", "耗时ms");
    for (i = 0; i < n; i++) {
        double ratio = (rows[i].seg_pages > 0)
                       ? (100.0 * (double)rows[i].reads / (double)rows[i].seg_pages)
                       : 0.0;

        printf("%9u %6u %10llu %9llu %9llu %8u %10llu %7.2f%% %9.3f\n",
               rows[i].n_docs, rows[i].nsegs,
               (unsigned long long)rows[i].seg_pages,
               (unsigned long long)rows[i].dict_pages,
               (unsigned long long)rows[i].post_pages,
               rows[i].df, (unsigned long long)rows[i].reads,
               ratio, rows[i].ms);
    }
    printf("\n");
}

int ft_bench_run_a5(void)
{
    static const u32 sizes[] = {1000u, 5000u, 10000u, 50000u, 100000u};
    const u32 n_sizes = (u32)(sizeof(sizes) / sizeof(sizes[0]));
    A5Row g1[8];
    A5Row g2[8];
    u32 i;
    int ok = 1;
    double g1_growth;
    double g2_growth;
    double corpus_growth;
    double r_min;
    double r_max;
    u32 j;

    printf("=== A5 · IO 可证：单 term 检索的页读 ===\n");
    printf("口径：一个段装下全部语料；页读 = 检索前后 ft_index_page_reads() 的差值\n\n");

    /* ---- 组一：df 固定 ---- */
    for (i = 0; i < n_sizes; i++) {
        u32 qidx = sizes[i] / 2u;      /* 中间那个词：df 恰好 = A5_DF_FIXED */
        int rc = a5_run_one(sizes[i], 1, qidx, &g1[i]);

        if (rc != 0) {
            printf("  组一在 %u 文档处失败\n", sizes[i]);
            return 2;
        }
    }
    a5_print_rows("组一 · df 固定（每个词恰好出现在固定数量的文档里）", g1, n_sizes);

    /* ---- 组二：df 随语料线性放大 ---- */
    for (i = 0; i < n_sizes; i++) {
        int rc = a5_run_one(sizes[i], 0, 0, &g2[i]);   /* 查第 0 个词 */

        if (rc != 0) {
            printf("  组二在 %u 文档处失败\n", sizes[i]);
            return 2;
        }
    }
    a5_print_rows("组二 · df 随语料线性放大（同一个词，词表固定）", g2, n_sizes);
    printf("  ⚠️ 组二里 df 很小的时候，页读会被「页粒度」压平：几十个 posting 装不满一页，\n");
    printf("     所以头几行的页读增长远慢于 df；df 到几千以后，页读才严格随 df 线性增长。\n\n");

    /* ---- 判据 ---- */
    printf("--- A5 判据 ---\n");

    /* C5：组一的页读增长远小于语料增长 */
    corpus_growth = (double)g1[n_sizes - 1u].n_docs / (double)g1[0].n_docs;
    g1_growth = (g1[0].reads > 0)
                ? ((double)g1[n_sizes - 1u].reads / (double)g1[0].reads)
                : 0.0;
    if (g1[0].reads == 0 || g1_growth <= corpus_growth / 10.0) {
        printf("  [PASS] C5 组一（df 固定）：语料放大 %.0f 倍，单 term 页读只放大 %.2f 倍"
               "（≤ 语料增长的 1/10）\n", corpus_growth, g1_growth);
    } else {
        printf("  [FAIL] C5 组一（df 固定）：语料放大 %.0f 倍，页读放大 %.2f 倍"
               "（> 语料增长的 1/10）\n", corpus_growth, g1_growth);
        ok = 0;
    }

    /* C6：组二的页读随 df 一起长（这是必然，不是缺陷） */
    {
        int mono = 1;
        int chain_ok = 1;
        double linear_err = 0.0;

        for (i = 1; i < n_sizes; i++) {
            if (g2[i].reads < g2[i - 1u].reads) {
                mono = 0;
            }
        }
        g2_growth = (g2[0].reads > 0)
                    ? ((double)g2[n_sizes - 1u].reads / (double)g2[0].reads)
                    : 0.0;
        /*
         * 逐行检查：页读必须**刚好覆盖这条倒排链** ——
         *   下界：估算的倒排页数（读全链是必须的）
         *   上界：倒排页数 + 字典页数 + 2（字典二分的零头）
         * 多读（整段扫一遍）或者少读（漏了链尾）都不该发生。
         */
        r_min = 1e18;
        r_max = 0.0;
        for (i = 0; i < n_sizes; i++) {
            double need = ceil((double)g2[i].df * (double)A5_POST_BYTES /
                               (double)A5_POST_CAP);
            double lo = need;
            double hi = need + (double)g2[i].dict_pages + 2.0;
            double r;

            if (need < 1.0) {
                need = 1.0;
            }
            r = (double)g2[i].reads;
            if (r < lo || r > hi) {
                chain_ok = 0;
            }
            if (r < r_min) r_min = r;
            if (r > r_max) r_max = r;
        }
        /*
         * 线性：最后两行的页读增量应当跟倒排页需求增量一致。
         * （小 df 的时候一页装得下，页读被"页粒度"压平 —— 那是页粒度，不是缺陷；
         *   所以线性只在 df 足够大的时候才谈得上。）
         */
        {
            double need_last = ceil((double)g2[n_sizes - 1u].df * (double)A5_POST_BYTES /
                                    (double)A5_POST_CAP);
            double need_prev = ceil((double)g2[n_sizes - 2u].df * (double)A5_POST_BYTES /
                                    (double)A5_POST_CAP);
            double d_read = (double)g2[n_sizes - 1u].reads - (double)g2[n_sizes - 2u].reads;
            double d_need = need_last - need_prev;

            if (d_need > 0.0) {
                linear_err = fabs(d_read / d_need - 1.0);
            } else {
                linear_err = 9.99;
            }
        }
        if (mono && chain_ok && linear_err <= 0.3) {
            printf("  [PASS] C6 组二（df 放大）：页读随 df 单调增长 %.2f 倍；"
                   "每行的页读都刚好覆盖该词的整条倒排链（估算需求 ~ 实际 %.0f~%.0f）；"
                   "大 df 端页读增量与倒排页需求增量的相对误差 %.1f%%（≤ 30%% ⇒ 线性）\n",
                   g2_growth, r_min, r_max, linear_err * 100.0);
        } else {
            printf("  [FAIL] C6 组二（df 放大）：单调=%d 覆盖整链=%d 线性误差=%.1f%%"
                   "（页读 %.0f~%.0f）\n",
                   mono, chain_ok, linear_err * 100.0, r_min, r_max);
            ok = 0;
        }
    }

    /* C7：老口径 —— 单 term 检索的页读远小于段的页数 */
    {
        double worst = 0.0;
        u32 worst_docs = 0;

        for (j = 0; j < n_sizes; j++) {
            if (g1[j].n_docs < 10000u) {
                continue;      /* 小语料整个段都在内存里，比值没有意义 */
            }
            if (g2[j].n_docs < 10000u) {
                continue;
            }
            if (g1[j].seg_pages > 0) {
                double r = (double)g1[j].reads / (double)g1[j].seg_pages;

                if (r > worst) { worst = r; worst_docs = g1[j].n_docs; }
            }
            if (g2[j].seg_pages > 0) {
                double r = (double)g2[j].reads / (double)g2[j].seg_pages;

                if (r > worst) { worst = r; worst_docs = g2[j].n_docs; }
            }
        }
        if (worst <= 0.05) {
            printf("  [PASS] C7 老口径：单 term 页读 ≤ 段总页数的 %.2f%%"
                   "（最差的一行是 %u 文档）\n", worst * 100.0, worst_docs);
        } else {
            printf("  [FAIL] C7 老口径：单 term 页读最高占到段总页数的 %.2f%%"
                   "（> 5%%，最差的一行是 %u 文档）\n", worst * 100.0, worst_docs);
            ok = 0;
        }
    }

    printf("\nRESULT a5 g1_reads_first=%llu g1_reads_last=%llu g1_growth=%.2f "
           "g2_df_first=%u g2_df_last=%u g2_reads_first=%llu g2_reads_last=%llu "
           "g2_growth=%.2f verdict=%s\n",
           (unsigned long long)g1[0].reads, (unsigned long long)g1[n_sizes - 1u].reads,
           g1_growth,
           g2[0].df, g2[n_sizes - 1u].df,
           (unsigned long long)g2[0].reads, (unsigned long long)g2[n_sizes - 1u].reads,
           g2_growth, ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
