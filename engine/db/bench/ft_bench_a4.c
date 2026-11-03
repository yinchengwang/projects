/*
 * ft_bench_a4.c —— A4 · 合并代价（量级论证）
 *
 * ==================== 这个基准要回答两个问题 ====================
 *
 * A4 · **合并的代价是可解释的**
 *   计划（PLAN-v1.3 §1.5）的判据是"量级论证"：
 *   10 万文档分批插入，合并总 IO 呈 `c·N·log_f N` 量级。
 *   本基准量出：批大小 b、合并因子 f、合并轮数、每轮合并的页数、合并总 IO，
 *   再拟合出 c（单位：**页/文档**），并与"分层模型"对照 ——
 *   结论落在"量级对不对"，不是"精确等于"。
 *
 * A5（单 term 检索的页读）在 ft_bench_a5.c 里；
 * 页读与合并 IO 的口径说明在 ft_bench.h 顶部。
 *
 * ==================== 页读怎么取 ====================
 *
 * 见 ft_bench.h 顶部的口径说明（页读取差量、合并 IO 两套口径）。
 *
 * ==================== 合并 IO 的两套口径 ====================
 *
 * 1) **结构口径**（主口径）：读 = 参与合并的旧段的页数（假设每页只读一遍），
 *    写 = 新写出来的那个段的页数。
 * 2) **实测取页次数**（ft_index_merge_page_reads）：合并路径真正取了几次页。
 *    它跟结构口径差得很远 —— 倒排是**按 term 分散**存放的，合并时每个 term
 *    都要单独取一次页，同一页会被反复取 ⇒ 取页次数 ≈ 老段的**词数**之和
 *    （判据 C6）。真实的磁盘 IO 落在两者之间（取决于缓冲池命中率）。
 *
 * ==================== 退出码 ====================
 *
 *   0 = 全部判据达标；1 = 有判据不达标；2 = 基准自身出错（开库失败等）。
 *
 * 用法：
 *   ft_bench [a4|a5|all] [文档数] [批大小]
 *   默认 all / 100000 / 1000
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

/* ---------------- 参数 ---------------- */

#define DB_PATH_A4 "test/tmp/ft_bench_a4.db"

#define A4_VOCAB      5000u    /* 词表大小 */
#define A4_WORDS      10u      /* 每篇文档的词数 */
#define A4_SEED       20261103u
#define A4_QUERY      "w000000"
#define A4_TOPK       10u
#define A4_MAXOUT     16u
#define A4_BP_FRAMES  4096u

/* ---------------- 段的页数 ---------------- */

/* 一个段的页数 = 段头 1 页 + 字典页 + 倒排页 */
static u64 seg_pages_of(const FtSegment *s)
{
    return 1ull + (u64)s->dict_count + (u64)s->post_count;
}

/* 索引里所有"活着的"段的总页数 */
static u64 index_live_pages(const FtIndex *ix)
{
    u64 total = 0;
    u32 n = ft_index_nsegs(ix);
    u32 i;

    for (i = 0; i < n; i++) {
        const FtSegment *s = NULL;

        if (ft_index_seg_at(ix, i, &s) == OK && s != NULL) {
            total += seg_pages_of(s);
        }
    }
    return total;
}

/* 段的快照（用来做合并前后的差分） */
typedef struct {
    u32 head;    /* 段头页号（段的唯一标识） */
    u32 docs;    /* 段里的文档数 */
    u32 terms;   /* 段里的词数 */
    u64 pages;   /* 段的页数 */
    u32 dict;    /* 字典页数 */
    u32 post;    /* 倒排页数 */
} SegSnap;

static u32 snap_segs(const FtIndex *ix, SegSnap *buf, u32 cap)
{
    u32 n = ft_index_nsegs(ix);
    u32 i;

    if (n > cap) {
        n = cap;
    }
    for (i = 0; i < n; i++) {
        const FtSegment *s = NULL;

        if (ft_index_seg_at(ix, i, &s) != OK || s == NULL) {
            buf[i].head = 0xFFFFFFFFu;
            buf[i].docs = 0;
            buf[i].terms = 0;
            buf[i].pages = 0;
            buf[i].dict = 0;
            buf[i].post = 0;
            continue;
        }
        buf[i].head = s->head_page;
        buf[i].docs = s->doc_count;
        buf[i].terms = s->n_terms;
        buf[i].pages = seg_pages_of(s);
        buf[i].dict = s->dict_count;
        buf[i].post = s->post_count;
    }
    return n;
}

static int snap_has(const SegSnap *buf, u32 n, u32 head)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (buf[i].head == head) {
            return 1;
        }
    }
    return 0;
}

/*
 * 合并前后的差分：谁没了（被合并掉的旧段）、谁来了（新写的段）。
 * 差分按**段头页号**做 —— 段头页号是段的唯一标识。
 */
static void diff_snaps(const SegSnap *before, u32 nb, const SegSnap *after, u32 na,
                       u64 *removed_pages, u32 *removed_n, u64 *removed_docs,
                       u64 *removed_terms, u32 *removed_start,
                       u64 *added_pages, u32 *added_n, u32 *added_docs,
                       u32 *added_terms, u32 *max_removed_terms)
{
    u32 i;
    u32 first = 0xFFFFFFFFu;

    *removed_pages = 0;
    *removed_n = 0;
    *removed_docs = 0;
    *removed_terms = 0;
    *removed_start = 0;
    *added_pages = 0;
    *added_n = 0;
    *added_docs = 0;
    *added_terms = 0;
    *max_removed_terms = 0;

    for (i = 0; i < nb; i++) {
        if (!snap_has(after, na, before[i].head)) {
            *removed_pages += before[i].pages;
            *removed_docs += before[i].docs;
            *removed_terms += before[i].terms;
            (*removed_n)++;
            if (i < first) {
                first = i;
            }
            if (before[i].terms > *max_removed_terms) {
                *max_removed_terms = before[i].terms;
            }
        }
    }
    /*
     * 被合并掉的旧段是**相邻**的一段（合并窗口的定义就是相邻），
     * 所以 removed_start 就是窗口的起点下标。
     */
    *removed_start = (first == 0xFFFFFFFFu) ? 0 : first;
    for (i = 0; i < na; i++) {
        if (!snap_has(before, nb, after[i].head)) {
            *added_pages += after[i].pages;
            (*added_n)++;
            *added_docs += after[i].docs;
            *added_terms += after[i].terms;
        }
    }
}

/* ---------------- 一次检索（只测耗时） ---------------- */

typedef struct {
    double ms;
    u32   n_hit;
    int   rc;
} QueryStat;

static QueryStat time_search(const FtIndex *ix, const char *q, u32 qlen)
{
    QueryStat st;
    FtHit hits[A4_MAXOUT];
    u32 n = 0;
    double t0;

    memset(&st, 0, sizeof(st));
    t0 = ft_bench_now_ms();
    st.rc = ft_index_search(ix, q, qlen, A4_TOPK, A4_MAXOUT, hits, &n);
    st.ms = ft_bench_now_ms() - t0;
    st.n_hit = n;
    return st;
}

/* ================================================================
 * A4 · 合并代价
 * ================================================================ */

typedef struct {
    u32    w;            /* 这次合并吃掉了几个段 */
    u32    docs;         /* 新段的文档数 */
    u64    read_pages;   /* 读（参与合并的旧段的页数） */
    u64    write_pages;  /* 写（新段的页数） */
    double ms;           /* 合并耗时 */
    double q_before_ms;  /* 合并前的一次检索耗时 */
    double q_after_ms;   /* 合并后的一次检索耗时 */
    u32    start_idx;    /* 合并窗口的起点下标（0 = 包含最老的段） */
    u32    win_docs;     /* 窗口里参与合并的文档数之和 */
    u32    nsegs_before; /* 合并前的段数 */
    u32    n_small_after;/* 合并后仍是"一个批大小"的段数（从没被合并过的小段） */
    u64    mr_pages;     /* 本次合并的**实测**页读（ft_index_merge_page_reads 增量） */
} MergeRec;

static void remove_db(const char *path)
{
    remove(path);
    remove("test/tmp/ft_bench_a4.db-wal");
    remove("test/tmp/ft_bench_a4.db-clog");
    remove("test/tmp/ft_bench_a4.db-shm");
}

int ft_bench_run_a4(u32 n_docs, u32 batch)
{
    FtBenchCorpus corpus;
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    FtDocText *docs = NULL;
    MergeRec *recs = NULL;
    SegSnap *snap_a = NULL;
    SegSnap *snap_b = NULL;
    double *eff = NULL;
    double *qb = NULL;
    double *qa = NULL;
    double *ms_all = NULL;
    u32 n_rec = 0;
    u32 cap_rec;
    u64 total_read = 0;
    u64 total_write = 0;
    u64 sum_rm_docs = 0;   /* Σ 每次合并的窗口文档数 = 文档被重写的总次数 */
    u64 sum_rm_terms = 0;  /* Σ 每次合并的老段词数 = 合并路径要处理的词次 */
    u64 total_mr = 0;      /* Σ 每次合并的实测页读（ft_index_merge_page_reads） */
    u64 max_merged_docs = 0;
    u64 sum_w = 0;
    u32 n_lossy = 0;
    double total_merge_ms = 0;
    u32 base;
    int failed = 0;
    int ok = 1;
    u32 vocab = A4_VOCAB;
    u32 words = A4_WORDS;
    const char *ev;
    double f_avg;
    double log_f_n;
    double l_meas;
    double l_model;
    double l_gap_max;
    double ppd;
    double ppd_post;
    double dict_pages;
    double w_avg;
    double l_eff;
    double c_layer_fit;
    double c_fit;
    double c0;
    double c_model;
    double med_eff;
    double med_qb;
    double med_qa;
    double max_ms;

    printf("=== A4 · 合并代价（量级论证） ===\n");

    /*
     * 词表大小与每篇词数可以用环境变量调（诊断用）：
     *   FT_BENCH_VOCAB / FT_BENCH_WORDS
     * 调它们是因为"字典页有几页"直接决定段的大小（见下面的判据说明）。
     */
    ev = getenv("FT_BENCH_VOCAB");
    if (ev != NULL && atoi(ev) > 0) {
        vocab = (u32)atoi(ev);
    }
    ev = getenv("FT_BENCH_WORDS");
    if (ev != NULL && atoi(ev) > 0) {
        words = (u32)atoi(ev);
    }
    printf("配置：%u 篇文档，批大小 %u，词表 %u，每篇 %u 词\n\n",
           n_docs, batch, vocab, words);

    if (ft_bench_corpus_random(&corpus, n_docs, vocab, words, A4_SEED) != 0) {
        printf("  语料生成失败\n");
        return 2;
    }

    remove_db(DB_PATH_A4);
    if (pager_open(DB_PATH_A4, A4_BP_FRAMES, &pg) != OK) {
        printf("  开库失败\n");
        ft_bench_corpus_free(&corpus);
        return 2;
    }
    if (ft_index_create(pg, NULL, NULL, 0, &ix) != OK) {
        printf("  建索引失败\n");
        pager_close(pg);
        ft_bench_corpus_free(&corpus);
        return 2;
    }

    cap_rec = n_docs / batch + 64u;
    docs  = (FtDocText *)calloc(batch, sizeof(FtDocText));
    recs  = (MergeRec *)calloc(cap_rec, sizeof(MergeRec));
    snap_a = (SegSnap *)calloc(cap_rec + 16u, sizeof(SegSnap));
    snap_b = (SegSnap *)calloc(cap_rec + 16u, sizeof(SegSnap));
    eff = (double *)calloc(cap_rec + 16u, sizeof(double));
    qb  = (double *)calloc(cap_rec + 16u, sizeof(double));
    qa  = (double *)calloc(cap_rec + 16u, sizeof(double));
    ms_all = (double *)calloc(cap_rec + 16u, sizeof(double));
    if (docs == NULL || recs == NULL || snap_a == NULL || snap_b == NULL ||
        eff == NULL || qb == NULL || qa == NULL || ms_all == NULL) {
        printf("  内存不足\n");
        failed = 1;
        goto done;
    }

    /* ---- 按批插入 + 触发合并 ---- */
    for (base = 0; base < n_docs; base += batch) {
        u32 n = (n_docs - base < batch) ? (n_docs - base) : batch;
        u32 i;
        u32 seg_id = 0;
        int rc;

        for (i = 0; i < n; i++) {
            ft_bench_corpus_doc(&corpus, base + i, &docs[i]);
        }
        rc = ft_index_add(ix, 0, docs, n, &seg_id);
        if (rc != OK) {
            printf("  第 %u 批插入失败（rc=%d）\n", base / batch, rc);
            failed = 1;
            goto done;
        }

        /*
         * 段数超过阈值就合并。
         *
         * ⚠️ **合并的触发规则必须照实现来**：`ft_index_merge` 一次只合并
         * **一个**窗口（相邻若干段里"文档数之和最小"的那个），段数减少 w-1。
         * 这里循环到"段数 ≤ 阈值"为止 —— 每轮合并都让段数下降，循环必然终止。
         */
        while (ft_index_nsegs(ix) > FT_MERGE_THRESHOLD) {
            u32 na;
            u32 nb;
            u64 rm_pages = 0;
            u64 ad_pages = 0;
            u64 rm_docs = 0;
            u64 rm_terms = 0;
            u32 rm_n = 0;
            u32 rm_start = 0;
            u32 ad_n = 0;
            u32 ad_docs = 0;
            u32 ad_terms = 0;
            u32 max_rm_terms = 0;
            u32 merged = 0;
            u32 n_small = 0;
            u32 k;
            u64 mpr_before = 0;
            u64 mpr_after = 0;
            QueryStat q1;
            QueryStat q2;
            double t0;
            double t1;

            q1 = time_search(ix, A4_QUERY, (u32)strlen(A4_QUERY));

            na = snap_segs(ix, snap_a, cap_rec + 16u);
            mpr_before = ft_index_merge_page_reads(ix);
            t0 = ft_bench_now_ms();
            rc = ft_index_merge(ix, 0, &merged);
            t1 = ft_bench_now_ms();
            mpr_after = ft_index_merge_page_reads(ix);
            if (rc != OK) {
                printf("  合并失败（rc=%d）\n", rc);
                failed = 1;
                goto done;
            }
            nb = snap_segs(ix, snap_b, cap_rec + 16u);
            diff_snaps(snap_a, na, snap_b, nb,
                       &rm_pages, &rm_n, &rm_docs, &rm_terms, &rm_start,
                       &ad_pages, &ad_n, &ad_docs,
                       &ad_terms, &max_rm_terms);
            /* 合并后还有几个"一个批大小"的段（= 从没被合并过的新鲜小段） */
            for (k = 0; k < nb; k++) {
                if (snap_b[k].docs == batch) {
                    n_small++;
                }
            }

            /*
             * 合并**只可能增加词数**，不可能减少 ——
             * 新段的词集是参与合并的那些段的词集的并集。
             * 所以新段的词数 < 任何一个老段的词数 ⇒ 合并**丢了词**。
             * 这条检查是基准自带的"数据正确性哨兵"。
             */
            if (ad_terms < max_rm_terms) {
                n_lossy++;
                if (n_lossy <= 3u) {
                    printf("  ⚠️ 第 %u 次合并丢了词：新段 %u 词 < 老段最多 %u 词"
                           "（读 %llu 页 / 写 %llu 页）\n",
                           n_lossy, ad_terms, max_rm_terms,
                           (unsigned long long)rm_pages,
                           (unsigned long long)ad_pages);
                }
            }

            q2 = time_search(ix, A4_QUERY, (u32)strlen(A4_QUERY));

            if (n_rec < cap_rec) {
                recs[n_rec].w = merged;
                recs[n_rec].docs = ad_docs;
                recs[n_rec].read_pages = rm_pages;
                recs[n_rec].write_pages = ad_pages;
                recs[n_rec].ms = t1 - t0;
                recs[n_rec].q_before_ms = q1.ms;
                recs[n_rec].q_after_ms = q2.ms;
                recs[n_rec].start_idx = rm_start;
                recs[n_rec].win_docs = (u32)rm_docs;
                recs[n_rec].nsegs_before = na;
                recs[n_rec].n_small_after = n_small;
                recs[n_rec].mr_pages = mpr_after - mpr_before;
            }
            if (n_rec < cap_rec + 16u) {
                /* 效率 = 读掉的旧页数 ÷ 写出的新页数（密度相近时应 ≈ 1） */
                eff[n_rec] = (ad_pages > 0) ? ((double)rm_pages / (double)ad_pages) : 0.0;
                qb[n_rec] = q1.ms;
                qa[n_rec] = q2.ms;
                ms_all[n_rec] = t1 - t0;
            }
            total_read += rm_pages;
            total_write += ad_pages;
            sum_rm_docs += rm_docs;
            sum_rm_terms += rm_terms;
            total_mr += (mpr_after - mpr_before);
            total_merge_ms += (t1 - t0);
            sum_w += merged;
            if ((u64)ad_docs > max_merged_docs) {
                max_merged_docs = ad_docs;
            }
            n_rec++;
        }
    }

    /* ---- 统计 ---- */
    f_avg = (n_rec > 0) ? ((double)sum_w / (double)n_rec) : 2.0;
    if (f_avg < 2.0) {
        f_avg = 2.0;
    }
    log_f_n = log((double)n_docs) / log(f_avg);
    l_model = log((double)n_docs / (double)batch) / log(f_avg);
    /*
     * 实际做了几层：
     *   层的编号 = 该层的段比批大小大了多少倍（f^k）。
     *   最大的那个合并出来的段有 max_merged_docs 篇 ⇒ 最深做到第 k 层，
     *   从第 0 层算起一共 k+1 层。
     */
    l_meas = (max_merged_docs > 0)
             ? (1.0 + log((double)max_merged_docs / (double)batch) / log(f_avg))
             : 0.0;
    /*
     * 理想模型要合并到只剩一个段（log_f(N/b) 层）。
     * 但实现的阈值是 FT_MERGE_THRESHOLD = 8 —— 段数降到 8 就不合了，
     * 所以**最顶上那一层永远是残缺的**（最后剩 8 个段）。
     * 因此"实测层数比理想少"是正常的，少的量级就是 log_f(阈值)。
     */
    l_gap_max = log((double)FT_MERGE_THRESHOLD) / log(f_avg) + 1.0;
    ppd = (double)index_live_pages(ix) / (double)n_docs;
    /*
     * ---- 两套口径的 c ----
     *
     * (1) 题目要的口径：总 IO ≈ c·N·log_f N
     *     c_fit = 总IO/(N·log_f N) —— 经典 LSM 的写法，直接报告。
     *
     * (2) 分层口径（拿来跟"段里有多少页"对得上）：
     *     一次合并里，每个参与合并的文档都被**重写一遍**，
     *     所以"每文档每被重写一次"的 IO = 总IO / Σ(每次合并的窗口文档数)。
     *     这才是能和下面 c_model 对上的量。
     */
    l_eff = (n_docs > 0) ? ((double)sum_rm_docs / (double)n_docs) : 0.0;
    /* 题目要的口径：总 IO ≈ c·N·log_f N */
    c_fit = (log_f_n > 0.0)
              ? ((double)(total_read + total_write) / ((double)n_docs * log_f_n)) : 0.0;
    c_layer_fit = (sum_rm_docs > 0)
                    ? ((double)(total_read + total_write) / (double)sum_rm_docs) : 0.0;
    {
        u32 ns = ft_index_nsegs(ix);
        u32 i;
        u64 sum_post = 0;
        u64 sum_dict = 0;
        u64 sum_doc = 0;

        for (i = 0; i < ns; i++) {
            const FtSegment *s = NULL;

            if (ft_index_seg_at(ix, i, &s) == OK && s != NULL) {
                sum_post += (u64)s->post_count;
                sum_dict += (u64)s->dict_count;
                sum_doc += (u64)s->doc_count;
            }
        }
        ppd_post = (sum_doc > 0) ? ((double)sum_post / (double)sum_doc) : 0.0;
        dict_pages = (ns > 0) ? ((double)sum_dict / (double)ns) : 0.0;
    }
    w_avg = (n_rec > 0) ? ((double)sum_rm_docs / (double)n_rec) : 0.0;
    /*
     * 模型推导（一次合并，窗口 W 篇文档）：
     *   读：2 个老段的页数 ≈ 2·(字典页 + W/2·倒排页/文档)
     *   写：新段的页数       ≈ 1·(字典页 + W·倒排页/文档)
     *   摊到每篇文档 ≈ 3·倒排页/文档 + 3·字典页/W
     * （词表固定时，新段的字典页 ≈ 老段的字典页，所以直接用平均字典页数。）
     */
    c_model = 3.0 * ppd_post + ((w_avg > 0.0) ? (3.0 * dict_pages / w_avg) : 0.0);
    c0 = 2.0 * ppd;

    med_eff = ft_bench_median(eff, n_rec);
    med_qb = ft_bench_median(qb, n_rec);
    med_qa = ft_bench_median(qa, n_rec);
    max_ms = 0.0;
    for (base = 0; base < n_rec; base++) {
        if (recs[base].ms > max_ms) {
            max_ms = recs[base].ms;
        }
    }

    /* ---- 输出 ---- */
    printf("--- 每次合并 ---\n");
    printf("%4s %6s %6s %10s %10s %10s %10s %10s %9s %9s %9s\n",
           "#", "w", "起点", "窗口文档", "新段文档", "读(页)", "写(页)", "实测页读",
           "耗时ms", "查前ms", "查后ms");
    for (base = 0; base < n_rec; base++) {
        printf("%4u %6u %6u %10u %10u %10llu %10llu %10llu %9.2f %9.3f %9.3f\n",
               base + 1u, recs[base].w, recs[base].start_idx, recs[base].win_docs,
               recs[base].docs,
               (unsigned long long)recs[base].read_pages,
               (unsigned long long)recs[base].write_pages,
               (unsigned long long)recs[base].mr_pages,
               recs[base].ms, recs[base].q_before_ms, recs[base].q_after_ms);
    }

    /*
     * ---- 合并窗口的实际行为（如实量化） ----
     *
     * 合并规则（实现里的原话）：段数 > FT_MERGE_THRESHOLD 时，
     * 在**相邻**的若干段里挑"文档数之和最小"的那个窗口合并一次。
     * 后果是：最前面那几段"一直没被合并过的小段"文档数最小，
     * 它们的窗口和也最小 —— 所以合并会**优先吃掉最老的段**。
     * 同时段数降到阈值 8 就不合了 ⇒ **最顶上那一层永远是残缺的**。
     * 这两条都是真实行为（不是 bug），下面把它们量化出来。
     */
    {
        u32 n_start0 = 0;
        u64 sum_start = 0;
        u32 last_small = 0;
        u32 fin_n;
        u32 k;
        u64 fin_max_docs = 0;
        u64 fin_min_docs = 0;
        u32 fin_small = 0;
        u64 fin_small_docs = 0;

        for (base = 0; base < n_rec; base++) {
            if (recs[base].start_idx == 0) {
                n_start0++;
            }
            sum_start += recs[base].start_idx;
            last_small = recs[base].n_small_after;
        }
        fin_n = snap_segs(ix, snap_a, cap_rec + 16u);
        for (k = 0; k < fin_n; k++) {
            if (k == 0 || snap_a[k].docs > fin_max_docs) {
                fin_max_docs = snap_a[k].docs;
            }
            if (k == 0 || snap_a[k].docs < fin_min_docs) {
                fin_min_docs = snap_a[k].docs;
            }
            if (snap_a[k].docs == batch) {
                fin_small++;
                fin_small_docs += snap_a[k].docs;
            }
        }
        printf("\n--- 合并窗口的实际行为 ---\n");
        printf("  窗口起点（含最老那段，下标 0）的轮数：%u / %u；平均起点 %.2f\n",
               n_start0, n_rec,
               (n_rec > 0) ? ((double)sum_start / (double)n_rec) : 0.0);
        printf("  最后一轮合并后仍是「一个批大小」的段数：%u\n", last_small);
        printf("  结束时索引里的段（共 %u 个，下标 0 = 最老）：\n", fin_n);
        for (k = 0; k < fin_n; k++) {
            printf("    #%u  文档 %u  页 %llu  词 %u\n", k, snap_a[k].docs,
                   (unsigned long long)snap_a[k].pages, snap_a[k].terms);
        }
        printf("  最大段 %llu 篇（占全部 %u 篇的 %.1f%%），最小段 %llu 篇\n",
               (unsigned long long)fin_max_docs, n_docs,
               (n_docs > 0) ? ((double)fin_max_docs * 100.0 / (double)n_docs) : 0.0,
               (unsigned long long)fin_min_docs);
        printf("  结束时仍是「一个批大小」的小段：%u 个，共 %llu 篇\n",
               fin_small, (unsigned long long)fin_small_docs);
    }

    printf("\n--- 汇总 ---\n");
    printf("  文档数 N            : %u\n", n_docs);
    printf("  批大小 b            : %u\n", batch);
    printf("  合并次数            : %u\n", n_rec);
    printf("  实测合并因子 f      : %.2f（平均每次合并吃掉的段数）\n", f_avg);
    printf("  合并总耗时          : %.1f ms（单次最大 %.2f ms）\n", total_merge_ms, max_ms);
    printf("  合并总 IO（结构口径）: 读 %llu 页 + 写 %llu 页 = %llu 页\n",
           (unsigned long long)total_read, (unsigned long long)total_write,
           (unsigned long long)(total_read + total_write));
    printf("     结构口径的含义：读 = 参与合并的旧段的页数（合并把每个旧段的字典页与\n");
    printf("     倒排页各读一遍），写 = 新写出来的那个段的页数。\n");
    printf("  合并实测页读        : %llu 次取页（ft_index_merge_page_reads 的累计增量）\n",
           (unsigned long long)total_mr);
    if (total_read > 0) {
        printf("     实测取页次数 / 结构口径读页数 = %.2f\n",
               (double)total_mr / (double)total_read);
        printf("     ⚠️ 两者不是一回事：结构口径假设「每页只读一遍」，\n");
        printf("        实测是**取页次数** —— 倒排按 term 分散存放，合并时每个 term\n");
        printf("        都要单独取一次页，所以同一页会被反复取。\n");
        printf("        真实的磁盘 IO 取决于缓冲池命中率（缓冲池 %u 帧 ≈ %u KB，\n",
               A4_BP_FRAMES, A4_BP_FRAMES * 4u);
        printf("        而段的总页数只有 %llu 页 ≈ %llu KB），落在两者之间。\n",
               (unsigned long long)index_live_pages(ix),
               (unsigned long long)(index_live_pages(ix) * 4u));
    }
    printf("  取页次数模型        : Σ(每次合并的老段词数) = %llu 词次"
           "（每个 term 一次取页）\n",
           (unsigned long long)sum_rm_terms);
    printf("  段总页数（结束时）  : %llu（每文档 %.4f 页）\n",
           (unsigned long long)index_live_pages(ix), ppd);
    printf("  层数：实测 %.2f / 模型 log_f(N/b) = %.2f\n", l_meas, l_model);
    printf("  拟合 c（题目口径）  = 总IO/(N·log_f N) = %.4f 页/文档\n", c_fit);
    if (log_f_n > 0.0) {
        printf("  拟合 c（实测取页口径）= 实测取页/(N·log_f N) = %.4f 次取页/文档\n",
               (double)total_mr / ((double)n_docs * log_f_n));
    }
    printf("  实测重写深度 L_eff  = Σ(窗口文档数)/N = %.3f 层（每篇文档平均被重写几次）\n",
           l_eff);
    printf("  拟合 c（分层口径）  = 总IO/Σ(窗口文档数) = %.4f 页/文档·层\n",
           c_layer_fit);
    printf("  分层模型 c_model    = 3·倒排页/文档 + 3·字典页/窗口文档\n"
           "                       = 3·%.4f + 3·%.1f/%.0f = %.4f 页/文档·层\n",
           ppd_post, dict_pages, w_avg, c_model);
    printf("  （参考）c0 = 2·每文档页数 = %.4f\n", c0);
    printf("  单次合并效率中位数  : %.3f（读掉的旧页数 ÷ 写出的新页数）\n", med_eff);
    printf("  检索耗时：合并前中位数 %.3f ms / 合并后中位数 %.3f ms\n", med_qb, med_qa);
    printf("  合并丢词的次数：%u\n", n_lossy);

    /* ---- 判据 ---- */
    printf("\n--- A4 判据 ---\n");
    if (l_model - l_meas <= l_gap_max) {
        printf("  [PASS] C1 层数：实测做了 %.2f 层，理想模型 log_f(N/b) = %.2f 层，"
               "差 %.2f 层 ≤ 阈值带来的上限 %.2f 层（阈值 %u 段 ⇒ 顶层必然残缺）\n",
               l_meas, l_model, l_model - l_meas, l_gap_max, FT_MERGE_THRESHOLD);
    } else {
        printf("  [FAIL] C1 层数：实测 %.2f 层 vs 模型 %.2f 层，差 %.2f 层 > %.2f 层"
               "（合并没有收敛成层结构）\n",
               l_meas, l_model, l_model - l_meas, l_gap_max);
        ok = 0;
    }
    if (med_eff >= 0.5 && med_eff <= 2.0) {
        printf("  [PASS] C2 单次合并效率 %.3f ∈ [0.5, 2.0]\n", med_eff);
    } else {
        printf("  [FAIL] C2 单次合并效率 %.3f ∉ [0.5, 2.0]\n", med_eff);
        ok = 0;
    }
    if (c_model > 0.0 && fabs(c_layer_fit / c_model - 1.0) <= 0.5) {
        printf("  [PASS] C3 量级：实测 c（分层口径）= %.4f 页/文档·层，与模型 c_model = %.4f"
               " 的相对误差 %.1f%%（≤ 50%%）\n",
               c_layer_fit, c_model, fabs(c_layer_fit / c_model - 1.0) * 100.0);
    } else {
        printf("  [FAIL] C3 量级：实测 c（分层口径）= %.4f vs 模型 %.4f"
               "（相对误差 %.1f%% > 50%%）\n",
               c_layer_fit, c_model, (c_model > 0.0) ? fabs(c_layer_fit / c_model - 1.0) * 100.0 : 999.0);
        ok = 0;
    }
    if (med_qb <= 0.0 || med_qa <= med_qb * 3.0) {
        printf("  [PASS] C4 合并不拖垮检索：合并前 %.3f ms → 合并后 %.3f ms（≤ 3 倍）\n",
               med_qb, med_qa);
    } else {
        printf("  [FAIL] C4 合并不拖垮检索：合并前 %.3f ms → 合并后 %.3f ms（> 3 倍）\n",
               med_qb, med_qa);
        ok = 0;
    }
    if (n_lossy == 0) {
        printf("  [PASS] C5 合并没有丢词：%u 次合并，新段的词数都不小于任一老段\n", n_rec);
    } else {
        printf("  [FAIL] C5 合并没有丢词：有 %u 次合并的新段词数小于老段（合并丢词！"
               "下面的 IO 数字因此不可信）\n", n_lossy);
        ok = 0;
    }
    /*
     * C6：合并路径的**取页次数**模型。
     * 倒排是按 term 分散存的，合并时每个 term 都要单独取一次页 ——
     * 所以取页次数 ≈ 老段的词数之和，而不是老段的页数之和。
     * 这个模型能解释"为什么实测取页次数比结构口径大几十倍"。
     */
    if (sum_rm_terms > 0) {
        double mr_err = fabs((double)total_mr / (double)sum_rm_terms - 1.0);

        if (mr_err <= 0.3) {
            printf("  [PASS] C6 取页次数模型：实测 %llu 次取页 vs 模型"
                   "（老段词数之和 %llu 词次），相对误差 %.1f%%（≤ 30%%）\n",
                   (unsigned long long)total_mr, (unsigned long long)sum_rm_terms,
                   mr_err * 100.0);
        } else {
            printf("  [FAIL] C6 取页次数模型：实测 %llu 次取页 vs 模型 %llu 词次，"
                   "相对误差 %.1f%%（> 30%%）\n",
                   (unsigned long long)total_mr, (unsigned long long)sum_rm_terms,
                   mr_err * 100.0);
            ok = 0;
        }
    }

    printf("\nRESULT a4 n_docs=%u batch=%u merges=%u f=%.2f total_io_pages=%llu "
           "merge_reads_measured=%llu c_pages_per_doc=%.4f c_layer=%.4f c_model=%.4f "
           "c_measured=%.4f l_eff=%.3f l_meas=%.2f l_model=%.2f eff=%.3f "
           "q_before_ms=%.3f q_after_ms=%.3f verdict=%s\n",
           n_docs, batch, n_rec, f_avg,
           (unsigned long long)(total_read + total_write),
           (unsigned long long)total_mr,
           c_fit, c_layer_fit, c_model,
           (log_f_n > 0.0) ? ((double)total_mr / ((double)n_docs * log_f_n)) : 0.0,
           l_eff,
           l_meas, l_model, med_eff, med_qb, med_qa, ok ? "PASS" : "FAIL");

done:
    free(docs);
    free(recs);
    free(snap_a);
    free(snap_b);
    free(eff);
    free(qb);
    free(qa);
    free(ms_all);
    if (ix != NULL) {
        ft_index_close(ix);
    }
    if (pg != NULL) {
        pager_close(pg);
    }
    ft_bench_corpus_free(&corpus);
    remove_db(DB_PATH_A4);
    if (failed) {
        return 2;
    }
    return ok ? 0 : 1;
}

