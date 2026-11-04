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
 *
 * 【v1.3 欠账 4 修复之后，第 2 项与第 1 项已经基本对齐】
 *
 * 修复前：倒排**按 term 分散**存放，合并时每个 term 都要单独取一次页，
 * 同一页被反复取 ⇒ 取页次数 ≈ 老段的**词数**之和（O(词数)）。
 * 修复后：合并按字典序遍历词条，相邻词落在同一页时**复用已读入的页**
 * ⇒ 取页次数 ≈ 老段的**页数**之和（O(页数)）。
 *
 * ⚠️ 注意还有第三个口径：**pager 的"逻辑页读"**（pager_get 与 pager_alloc 各算一次），
 * 它比上面的"实测取页次数"多两笔 —— 合并前的**数页那一趟**
 * （ft_merge_count_pages 读一遍源段字典页）和**新段页的分配**。
 * 判据 C6 的模型统一用这个口径：**2 × 老段页数 + 新写页数**（详见判据处注释）。
 *
 * 另外还有两个 pager 层的口径（逻辑页读 / 物理读盘），见 ft_bench.h 顶部。
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
/*
 * ft_bench_seg_pages_of / ft_bench_index_live_pages（段的页数）
 * ft_bench_snap_segs / ft_bench_snap_has / ft_bench_diff_snaps（段快照与差分）
 * ft_bench_time_search（一次检索只测耗时）
 * —— 全在 ft_bench.h 里：A4 / A5 共用，也为了让本文件守住 800 行上限。
 */

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
    u64 sum_pg_logical = 0;   /* Σ 每次合并的逻辑页读（pager 级） */
    u64 sum_pg_physical = 0;  /* Σ 每次合并的物理读盘（pager 级） */
    u64 sum_pg_unique = 0;    /* Σ 每次合并的去重页数（同一页跨合并会重复计） */
    u64 pg_unique_total = 0;  /* 全程去重的物理页数（数据足迹，跨合并累计） */
    /* 每次合并的 pager 层 IO 明细（打印用；与 MergeRec 并行存一份） */
    FtBenchMergeIo *mio = NULL;
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

    ft_bench_remove_db(DB_PATH_A4);
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
    mio   = (FtBenchMergeIo *)calloc(cap_rec, sizeof(FtBenchMergeIo));
    snap_a = (SegSnap *)calloc(cap_rec + 16u, sizeof(SegSnap));
    snap_b = (SegSnap *)calloc(cap_rec + 16u, sizeof(SegSnap));
    eff = (double *)calloc(cap_rec + 16u, sizeof(double));
    qb  = (double *)calloc(cap_rec + 16u, sizeof(double));
    qa  = (double *)calloc(cap_rec + 16u, sizeof(double));
    ms_all = (double *)calloc(cap_rec + 16u, sizeof(double));
    if (docs == NULL || recs == NULL || mio == NULL || snap_a == NULL ||
        snap_b == NULL || eff == NULL || qb == NULL || qa == NULL || ms_all == NULL) {
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
            u64 pg_logical = 0;
            u64 pg_physical = 0;
            u64 pg_unique = 0;
            FtBenchQueryStat q1;
            FtBenchQueryStat q2;
            double t0;
            double t1;

            q1 = ft_bench_time_search(ix, A4_QUERY, (u32)strlen(A4_QUERY), A4_TOPK, A4_MAXOUT);

            na = ft_bench_snap_segs(ix, snap_a, cap_rec + 16u);
            mpr_before = ft_index_merge_page_reads(ix);
            /*
             * 开一个新的测量窗口（v1.3 欠账 6）。
             *
             * 之后到 merge 返回之间，pager 的三个计数器只统计**这次合并**。
             * 位置很讲究：必须在 snap_segs（它也读页）之后开窗口，
             * 而且取数必须在 merge 返回后**立刻**取 ——
             * 后面的 ft_bench_snap_segs( snap_b ) 也会读页，夹进来就脏了。
             */
            pager_stats_reset(pg);
            t0 = ft_bench_now_ms();
            rc = ft_index_merge(ix, 0, &merged);
            t1 = ft_bench_now_ms();
            /* 立刻取窗口内的三个数字（此时还没有别的读页操作） */
            pg_logical = pager_logical_reads(pg);
            pg_physical = pager_physical_reads(pg);
            pg_unique = pager_unique_pages(pg);
            mpr_after = ft_index_merge_page_reads(ix);
            if (rc != OK) {
                printf("  合并失败（rc=%d）\n", rc);
                failed = 1;
                goto done;
            }
            nb = ft_bench_snap_segs(ix, snap_b, cap_rec + 16u);
            ft_bench_diff_snaps(snap_a, na, snap_b, nb,
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

            q2 = ft_bench_time_search(ix, A4_QUERY, (u32)strlen(A4_QUERY), A4_TOPK, A4_MAXOUT);

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
                /* 这次合并的 pager 层 IO（打印用） */
                mio[n_rec].old_pages = rm_pages;
                mio[n_rec].logical = pg_logical;
                mio[n_rec].physical = pg_physical;
                mio[n_rec].unique = pg_unique;
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
            sum_pg_logical += pg_logical;
            sum_pg_physical += pg_physical;
            sum_pg_unique += pg_unique;
            total_merge_ms += (t1 - t0);
            sum_w += merged;
            if ((u64)ad_docs > max_merged_docs) {
                max_merged_docs = ad_docs;
            }
            n_rec++;
        }
    }

    /* ---- 统计 ---- */
    /*
     * 全程去重的物理页数（数据足迹）——
     * 它**不随测量窗口重置**，所以这里读到的就是"整个 A4 跑下来
     * 真正从盘上读回过多少个不重复的页"。
     * 必须在 pager_close 之前读（pg 在 done: 段才关）。
     */
    pg_unique_total = pager_unique_pages_total(pg);

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
    ppd = (double)ft_bench_index_live_pages(ix) / (double)n_docs;
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
     * ---- 每次合并的 IO 口径（pager 层，v1.3 欠账 6） ----
     *
     * 四个口径各说各的，**不能互相换算**：
     *   逻辑页读 —— 取页次数，缓存命中也计数（逻辑访问量）
     *   物理读盘 —— 真正从文件读回一页的次数（真实磁盘 IO）
     *   唯一页数 —— 本次窗口里读盘过的不重复页号（帧池无限大时的下界）
     *   段页数   —— 参与合并的旧段的页数之和（结构口径）
     *
     * 打印在 ft_bench.h 的 ft_bench_print_merge_io 里（A4 / A5 共用）。
     */
    ft_bench_print_merge_io(mio, n_rec);

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
        fin_n = ft_bench_snap_segs(ix, snap_a, cap_rec + 16u);
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
        printf("     同页复用之后这个比值接近 1：合并时按字典序遍历词条，\n");
        printf("        相邻词落在同一页时复用已读入的页 —— 代价从 O(词数)\n");
        printf("        降到了 O(页数)（两趟写 ⇒ 约 2 倍）。\n");
        printf("        真实的磁盘 IO 见下面 pager 层的「物理读盘」一行。\n");
    }

    /*
     * ---- 四个口径（v1.3 欠账 6） ----
     *
     * 这是本次要定死的度量口径，每个数字后面一句话说清它是什么。
     * 打印在 ft_bench.h 的 ft_bench_print_io 里（A4 / A5 共用）。
     */
    {
        FtBenchIo m;

        m.logical = sum_pg_logical;
        m.physical = sum_pg_physical;
        m.unique = sum_pg_unique;
        m.unique_total = pg_unique_total;
        m.seg_pages = ft_bench_index_live_pages(ix);
        if (ft_bench_print_io("A4 合并阶段", &m) != 0) {
            ok = 0;
        }
    }
    printf("  Σ 各次合并窗口内的去重页数 = %llu 页（同一页被多次合并触及会重复计，"
           "是上界）\n", (unsigned long long)sum_pg_unique);
    printf("  参与合并的旧段页数之和   = %llu 页（结构口径）\n",
           (unsigned long long)total_read);
    if (sum_pg_logical > 0) {
        printf("  --- 口径之间的关系 ---\n");
        printf("  逻辑页读 / 物理读盘 = %.2f（**真实磁盘 IO 比逻辑页读小 %.2f 倍**）\n",
               (double)sum_pg_logical / (double)sum_pg_physical,
               (double)sum_pg_logical / (double)sum_pg_physical);
        printf("  物理读盘 / 唯一页数 = %.2f（> 1 说明同一页被逐出后又读回了 —— "
               "帧池不够大）\n",
               (pg_unique_total > 0)
                   ? ((double)sum_pg_physical / (double)pg_unique_total) : 0.0);
        printf("  物理读盘 / 旧段页数 = %.2f（**全部合并**的平均 —— 大部分合并"
               "根本没碰盘，平均进去就压小了；\n",
               (total_read > 0)
                   ? ((double)sum_pg_physical / (double)total_read) : 0.0);
        printf("     只看真正碰了盘的那几次，每个旧页**最多**真正读盘一次"
               "⇒ 真实 IO 是 O(页数)）\n");
    }
    /*
     * 【只看真正碰了盘的那几次合并】
     *
     * 大部分合并的物理读盘是 0 —— 因为被合并的段是刚写的，还留在帧池里。
     * 只有少数几次（合并的是很老、早已被逐出并落盘的段）真的要回盘。
     *
     * 对这几次单独看：**物理读盘 ≈ 旧段页数**（每个旧页只真正读盘一次）
     * ⇒ 真实 IO 的代价是 **O(页数)**；
     * 而逻辑页读在同页复用之后也是 **O(页数)**（约 2 倍 —— 两趟写）。
     * 这两个数必须分开报 —— 混在一起会得出"物理 IO 远大于实际"的错觉。
     *
     * （逐次明细与这个小结在 ft_bench.h 的 ft_bench_print_merge_io 里。）
     */
    printf("  旧口径（优化前）    : Σ(每次合并的老段词数) = %llu 词次"
           "（那时每个 term 一次取页；现在是**上界**，不再是模型）\n",
           (unsigned long long)sum_rm_terms);
    printf("  段总页数（结束时）  : %llu（每文档 %.4f 页）\n",
           (unsigned long long)ft_bench_index_live_pages(ix), ppd);
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
     * C6：合并路径的**页读模型**（v1.3 欠账 4 修好之后重新定义）。
     *
     * 【旧判据为什么废了 —— 这一条必须写进代码里】
     *
     * 旧判据是"取页次数 ≈ 老段**词数**之和"：
     * 它描述的是**同页复用之前**的实现 —— 倒排按 term 分散存放，
     * 合并时每个 term 单独取一次页，同一页被反复取，所以取页次数 ∝ 词数。
     *
     * 修复之后（合并时按字典序遍历、相邻词落在同一页就复用已读入的页），
     * 取页次数从 O(词数) 降到 **O(页数)** ——
     * 旧判据自然不再成立。
     * **判据失效是优化生效的证据，不是回归**：
     * 旧模型对应的实测值（老段词数之和）现在只是**上界**，不再是模型。
     *
     * 【中间版本（"2.0 × 老段页数 ± 0.6"）为什么也废了 —— 2026-11-04 实测发现】
     *
     * 换成 pager 的"逻辑页读"口径之后，判据曾写成
     * "逻辑页读 ≈ 2 × 老段页数（容差 ±0.6）"。实测 2.95 倍，**打红**。
     * 查下来是**判据自己写错了**，不是实现退化：
     *
     *   ① **口径混用**：模型 2.0 是照着"合并实测取页次数"
     *      （ft_index_merge_page_reads，实测 ≈ 1.0 × 老段页数）推出来的，
     *      判定用的却是 pager 的**逻辑页读**
     *      （pager_get 与 pager_alloc **各算一次**，见 pager.h 顶部口径说明）。
     *      分母相同、分子差一笔"写入也算取页"，
     *      拿一个口径的模型去卡另一个口径，必然对不上。
     *   ② **模型漏了两笔**：
     *      - 合并前还有**数页那一趟**（ft_merge_count_pages：
     *        读一遍源段的字典页，把新段的字节数数清楚）
     *        ⇒ 老段的页被读**两趟**，不是一趟；
     *      - 新段的页是 pager_alloc 出来的，而 **pager_alloc 也计一次逻辑页读**。
     *
     * 【新判据】（口径统一到 pager 的"逻辑页读"）
     *
     *   模型：逻辑页读 ≈ 2 × 老段页数 + 新写页数
     *        （老段：数页一趟 + 合并一趟；新段：alloc 算一次取页）
     *   容差：每次合并 ±64 页（每轮固定开销：根页双写 + 目录页 + 墓碑页）
     *
     * 实测（100000 文档 / 批 1000 / 92 次合并）：
     *   逻辑页读 75135 次，模型 2×25483 + 21718 = 72684 次，偏差 +2451
     *   （每次合并 +26.6 页，在 ±64 的容差内），
     *   比值 75135 / 25483 = **2.95 倍**，且与段大小无关（92 次合并都在 2.49~3.01 之间）。
     *
     * 【这个判据能打红】
     * 若哪天同页复用坏掉、退回"每词一次取页"，
     * 老段词数之和是 849596 词次 ⇒ 逻辑页读会变成老段页数的 **33 倍**，
     * 远超模型 —— 直接变红。
     */
    if (total_read > 0) {
        double model = (double)(2u * total_read + total_write);
        double diff = (double)sum_pg_logical - model;
        double tol = 64.0 * (double)n_rec;
        double ratio = (double)sum_pg_logical / (double)total_read;

        if (fabs(diff) <= tol) {
            printf("  [PASS] C6 页读模型：合并的逻辑页读 %llu 次，模型"
                   " 2×老段页数 + 新写页数 = %.0f 次，偏差 %+.0f（容差 ±%.0f）；"
                   "比值 = 老段页数的 %.2f 倍（与段大小无关 ⇒ 代价是 O(页数)，"
                   "不是 O(词数)）\n",
                   (unsigned long long)sum_pg_logical, model, diff, tol, ratio);
        } else {
            printf("  [FAIL] C6 页读模型：合并的逻辑页读 %llu 次，模型"
                   " 2×老段页数 + 新写页数 = %.0f 次，偏差 %+.0f（超出容差 ±%.0f）；"
                   "比值 = 老段页数的 %.2f 倍\n",
                   (unsigned long long)sum_pg_logical, model, diff, tol, ratio);
            ok = 0;
        }
    }

    /*
     * C7：真实磁盘 IO 是 **O(页数)**，不是 O(词数)。
     *
     * 物理读盘次数 ≤ 被读盘过的唯一页数 —— 即"每个真正读过的页，
     * 最多真正读盘一次"（超过就说明帧池不够、同一页被逐出后又读回）。
     */
    if (pg_unique_total > 0) {
        if (sum_pg_physical <= pg_unique_total) {
            printf("  [PASS] C7 真实 IO 是 O(页数)：物理读盘 %llu 次 ≤ 唯一页数 %llu"
                   "（每页最多读盘一次）；同期老段词数之和为 %llu 词次 —— "
                   "两者差 %.1f 倍\n",
                   (unsigned long long)sum_pg_physical,
                   (unsigned long long)pg_unique_total,
                   (unsigned long long)sum_rm_terms,
                   (sum_rm_terms > 0)
                       ? ((double)sum_rm_terms / (double)sum_pg_physical) : 0.0);
        } else {
            printf("  [FAIL] C7 真实 IO 是 O(页数)：物理读盘 %llu 次 > 唯一页数 %llu"
                   "（同一页被逐出后又读回了 —— 帧池不够）\n",
                   (unsigned long long)sum_pg_physical,
                   (unsigned long long)pg_unique_total);
            ok = 0;
        }
    } else {
        /*
         * ⚠️ C7 本次**测不出差别**，如实记录而不是当成通过：
         * 全程一次物理读盘都没有发生（帧池 4096 帧装得下整个库，
         * 合并时要的页全在内存里）⇒ "物理读盘 ≤ 唯一页数"这个判据
         * 在当前规模下是**空判据**（两边都是 0）。
         * 想让这个口径有区分力，必须把帧池调小到装不下库 —— 见 PLAN-v1.3.md
         * 「仍然欠着的」第 5 条。
         */
        printf("  [SKIP] C7 真实 IO 是 O(页数)：本次全程物理读盘 %llu 次、"
               "唯一页数 %llu —— 帧池装得下整个库，这个口径**测不出差别**"
               "（空判据，不计入达标）\n",
               (unsigned long long)sum_pg_physical,
               (unsigned long long)pg_unique_total);
    }

    printf("\nRESULT a4 n_docs=%u batch=%u merges=%u f=%.2f total_io_pages=%llu "
           "merge_reads_measured=%llu pg_logical_reads=%llu pg_physical_reads=%llu "
           "pg_unique_pages=%llu c_pages_per_doc=%.4f c_layer=%.4f c_model=%.4f "
           "c_measured=%.4f l_eff=%.3f l_meas=%.2f l_model=%.2f eff=%.3f "
           "q_before_ms=%.3f q_after_ms=%.3f verdict=%s\n",
           n_docs, batch, n_rec, f_avg,
           (unsigned long long)(total_read + total_write),
           (unsigned long long)total_mr,
           (unsigned long long)sum_pg_logical,
           (unsigned long long)sum_pg_physical,
           (unsigned long long)pg_unique_total,
           c_fit, c_layer_fit, c_model,
           (log_f_n > 0.0) ? ((double)total_mr / ((double)n_docs * log_f_n)) : 0.0,
           l_eff,
           l_meas, l_model, med_eff, med_qb, med_qa, ok ? "PASS" : "FAIL");

done:
    free(docs);
    free(recs);
    free(mio);
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
    ft_bench_remove_db(DB_PATH_A4);
    if (failed) {
        return 2;
    }
    return ok ? 0 : 1;
}

