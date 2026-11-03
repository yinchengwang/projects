/*
 * test_ft_bm25.c —— 评分公式必须和手算逐位一致（v1.3 步骤 1）
 *
 * 6 项：
 *   1. BM25 单词贡献 == 手算值（Lucene 式 idf，k1=1.2、b=0.75）
 *   2. BM25 的长度归一：同样命中，长文档分数更低
 *   3. TF-IDF 对照 == 手算值（教科书式 idf，**不做**长度归一）
 *   4. 线性扫描 BM25 == 按公式逐条累加
 *   5. 线性扫描 TF-IDF == 按公式逐条累加；且与 BM25 的排序**不同**
 *   6. top-k 截断、平局按 doc_id 升序、无命中、以及 avgdl=0 的防护
 *
 * 【为什么分数要写死】
 *
 * 评分公式是**索引格式的一部分**：同一个查询在写索引时和查索引时
 * 必须算出同一个分数，否则排序结果不可复现、缓存失效。
 * 而且改错 k1/b 或 idf 公式**不会报错** —— 只是排序悄悄变差。
 * 所以这里把分数写死成手算值，改公式就得改这里（并且想清楚为什么）。
 *
 * 【手算的来历】
 *
 *   idf  = ln(1 + (N - df + 0.5) / (df + 0.5))
 *   BM25 = idf * tf * (k1 + 1) / (tf + k1 * (1 - b + b * dl / avgdl))
 *   TFIDF = tf * ln((N - df + 0.5) / (df + 0.5))       ← 教科书式，没有长度归一
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ft/bm25.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 浮点比较：容差 1e-12 */
#define NEAR(a, b) (fabs((a) - (b)) < 1e-12)

/* ---------------- 1) BM25 单词贡献 ---------------- */

static void t1_bm25_values(void)
{
    printf("  1) BM25 单词贡献 == 手算值\n");

    FtCorpus cs;

    /*
     * N=10, df=3, tf=2, dl=20, avgdl=15
     *   idf  = ln(1 + 7.5/3.5) = ln(3.142857142857...) = 1.1451323043030026
     *   norm = 1.2 * (1 - 0.75 + 0.75 * 20/15) = 1.2 * 1.25 = 1.5
     *   分数 = 1.1451323043030026 * 4.4 / 3.5 = 1.4395948968380605
     */
    cs.n_docs = 10;
    cs.avgdl = 15.0;
    CHECK(NEAR(ft_bm25_score(&cs, 3, 2, 20), 1.4395948968380605));

    /*
     * N=1, df=1, tf=1, dl=1, avgdl=1
     *   idf = ln(1 + 0.5/1.5) = ln(4/3) = 0.2876820724517809
     *   norm = 1.2 * (0.25 + 0.75) = 1.2
     *   分数 = 0.2876820724517809 * 2.2 / 2.2 = 0.2876820724517809
     */
    cs.n_docs = 1;
    cs.avgdl = 1.0;
    CHECK(NEAR(ft_bm25_score(&cs, 1, 1, 1), 0.2876820724517809));

    /*
     * N=100, df=7, tf=4, dl=60, avgdl=50
     *   分数 = 4.2531438601173148
     */
    cs.n_docs = 100;
    cs.avgdl = 50.0;
    CHECK(NEAR(ft_bm25_score(&cs, 7, 4, 60), 4.2531438601173148));

    /* tf=0 → 贡献 0 */
    cs.n_docs = 10;
    cs.avgdl = 15.0;
    CHECK(NEAR(ft_bm25_score(&cs, 3, 0, 20), 0.0));
}

/* ---------------- 2) BM25 的长度归一 ---------------- */

static void t2_length_norm(void)
{
    printf("  2) BM25 的长度归一：长文档分数更低\n");

    FtCorpus cs;

    /*
     * 同样的 N / df / tf，只改文档长度：
     *   dl=15（正好等于 avgdl）→ 1.5745569184166286
     *   dl=30（两倍 avgdl）  → 1.2289224729105395
     *
     * 长文档分数更低 —— 这正是 BM25 相对 TF-IDF 的核心区别。
     */
    cs.n_docs = 10;
    cs.avgdl = 15.0;
    CHECK(NEAR(ft_bm25_score(&cs, 3, 2, 15), 1.5745569184166286));
    CHECK(NEAR(ft_bm25_score(&cs, 3, 2, 30), 1.2289224729105395));
    CHECK(ft_bm25_score(&cs, 3, 2, 30) < ft_bm25_score(&cs, 3, 2, 15));
}

/* ---------------- 3) TF-IDF 对照 ---------------- */

static void t3_tfidf_values(void)
{
    printf("  3) TF-IDF 对照 == 手算值\n");

    FtCorpus cs;

    /*
     * N=10, df=3, tf=2
     *   idf = ln(7.5/3.5) = ln(2.142857142857...) = 0.7621400520468967
     *   分数 = 2 * idf = 1.5242801040937934
     */
    cs.n_docs = 10;
    cs.avgdl = 15.0;
    CHECK(NEAR(ft_tfidf_score(&cs, 3, 2), 1.5242801040937934));

    /*
     * N=100, df=7, tf=4 → 10.092233663009507
     */
    cs.n_docs = 100;
    cs.avgdl = 50.0;
    CHECK(NEAR(ft_tfidf_score(&cs, 7, 4), 10.092233663009507));

    /*
     * 教科书式 idf 会**取到负数**：N=1, df=1 → ln(0.5/1.5) = ln(1/3)
     *   分数 = 1 * (-1.0986122886681098)
     *
     * 这不是 bug，是教科书式 idf 的本来面目：
     * 词在语料里越常见，idf 越低，低到极限就是负数。
     * Lucene 式的 ln(1 + ...) 修掉了这一点（idf 恒为正）。
     */
    cs.n_docs = 1;
    cs.avgdl = 1.0;
    CHECK(NEAR(ft_tfidf_score(&cs, 1, 1), -1.0986122886681098));

    /*
     * ln 的参数非正时（df 比 N 还大）没有定义 → 定义成 0。
     * N=1, df=2 → (1 - 2 + 0.5) / 2.5 = -0.2 → 不是正数 → 0。
     */
    CHECK(NEAR(ft_tfidf_score(&cs, 2, 1), 0.0));

    /*
     * TF-IDF **不做**长度归一 —— 这件事由函数签名保证：
     * ft_tfidf_score 压根没有 doc_len 参数。
     */
}

/* ---------------- 4) 线性扫描 BM25 ---------------- */

/*
 * 测试语料 A：
 *
 *   doc 0: "the quick brown fox"   （4 个词）
 *   doc 1: "the lazy dog the"      （4 个词）
 *   doc 2: "quick quick fox"       （3 个词）
 *
 * query: "quick fox"
 *
 *   N = 3, avgdl = 11/3
 *   df(quick) = 2, df(fox) = 2
 *   tf: doc0 → quick 1、fox 1；doc2 → quick 2、fox 1；doc1 一个都没命中
 *
 * 手算（逐条累加）：
 *   doc0 = bm25(3, df=2, tf=1, dl=4) + bm25(3, df=2, tf=1, dl=4)
 *        = 0.45315090947198411 * 2 = 0.9063018189439682
 *   doc2 = bm25(3, df=2, tf=2, dl=3) + bm25(3, df=2, tf=1, dl=3)
 *        = 0.68108310345789247 + 0.50777177802441087 = 1.1888548814823033
 */
static void t4_linear_bm25(void)
{
    printf("  4) 线性扫描 BM25 == 按公式逐条累加\n");

    FtDocText docs[3];
    FtToken query[2];
    FtHit hits[8];
    u32 n = 0;
    int rc;
    double expect0;
    double expect2;
    FtCorpus cs;

    docs[0].doc_id = 0;
    docs[0].text = "the quick brown fox";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 1;
    docs[1].text = "the lazy dog the";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 2;
    docs[2].text = "quick quick fox";
    docs[2].text_len = (u32)strlen(docs[2].text);

    query[0].len = 5;
    memcpy(query[0].text, "quick", 5);
    query[0].text[5] = '\0';
    query[1].len = 3;
    memcpy(query[1].text, "fox", 3);
    query[1].text[3] = '\0';

    rc = ft_score_linear_bm25(docs, 3, query, 2, 8, hits, &n);
    CHECK(rc == OK);
    /* doc1 没命中 → 不进结果 */
    CHECK(n == 2);

    /* 手算：语料统计量 N=3、avgdl=11/3，df=2 */
    cs.n_docs = 3;
    cs.avgdl = 11.0 / 3.0;
    expect0 = ft_bm25_score(&cs, 2, 1, 4) + ft_bm25_score(&cs, 2, 1, 4);
    expect2 = ft_bm25_score(&cs, 2, 2, 3) + ft_bm25_score(&cs, 2, 1, 3);
    CHECK(NEAR(expect0, 0.9063018189439682));
    CHECK(NEAR(expect2, 1.1888548814823033));

    if (n == 2) {
        /* 分数降序：doc2 在前 */
        CHECK(hits[0].doc_id == 2);
        CHECK(hits[1].doc_id == 0);
        CHECK(NEAR(hits[0].score, expect2));
        CHECK(NEAR(hits[1].score, expect0));
    }
}

/* ---------------- 5) 线性扫描 TF-IDF ---------------- */

/*
 * 同一份语料 A，换成 TF-IDF：
 *
 *   idf(quick) = idf(fox) = ln((3 - 2 + 0.5) / 2.5) = ln(0.6) = -0.5108256237659907
 *
 *   doc0 = 1 * idf + 1 * idf = -1.0216512475319814
 *   doc2 = 2 * idf + 1 * idf = -1.5324768712979722
 *
 * 注意排序**反过来了**：doc0 排在 doc2 前面。
 *
 * 为什么？因为教科书式 idf 是负的，tf 越大分数越低 ——
 * doc2 的 "quick" 出现了两次，负贡献翻倍，直接掉到后面。
 * BM25 因为 idf 恒正，tf 越大分数越高，doc2 排在前面。
 *
 * 这就是"为什么要用 Lucene 式 idf"的一个活生生的例子。
 */
static void t5_linear_tfidf(void)
{
    printf("  5) 线性扫描 TF-IDF == 按公式逐条累加\n");

    FtDocText docs[3];
    FtToken query[2];
    FtHit hits[8];
    u32 n = 0;
    int rc;
    FtCorpus cs;

    docs[0].doc_id = 0;
    docs[0].text = "the quick brown fox";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 1;
    docs[1].text = "the lazy dog the";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 2;
    docs[2].text = "quick quick fox";
    docs[2].text_len = (u32)strlen(docs[2].text);

    query[0].len = 5;
    memcpy(query[0].text, "quick", 5);
    query[0].text[5] = '\0';
    query[1].len = 3;
    memcpy(query[1].text, "fox", 3);
    query[1].text[3] = '\0';

    rc = ft_score_linear_tfidf(docs, 3, query, 2, 8, hits, &n);
    CHECK(rc == OK);
    CHECK(n == 2);

    cs.n_docs = 3;
    cs.avgdl = 11.0 / 3.0;
    if (n == 2) {
        /* doc0 排在前面（TF-IDF 的 idf 是负的，tf 越大越靠后） */
        CHECK(hits[0].doc_id == 0);
        CHECK(hits[1].doc_id == 2);
        CHECK(NEAR(hits[0].score, 2.0 * ft_tfidf_score(&cs, 2, 1)));
        CHECK(NEAR(hits[0].score, -1.0216512475319814));
        CHECK(NEAR(hits[1].score, 3.0 * ft_tfidf_score(&cs, 2, 1)));
        CHECK(NEAR(hits[1].score, -1.5324768712979722));
    }
}

/* ---------------- 6) top-k、平局、边界 ---------------- */

/*
 * 测试语料 B：两篇一模一样的文档，用来造一个"平局"。
 *
 *   doc 7: "alpha beta"
 *   doc 3: "alpha beta"
 *   doc 1: "gamma"
 *
 * query: "alpha" → df=2（doc7 和 doc3 都含 alpha）
 * N=3, avgdl=5/3
 *   BM25 分数 = 0.4344571362775708（两篇一样）
 *
 * 平局按 doc_id 升序 → doc 3 在前，doc 7 在后。
 */
static void t6_topk_and_edges(void)
{
    printf("  6) top-k 截断、平局顺序、边界\n");

    FtDocText docs[3];
    FtToken query[1];
    FtHit hits[8];
    u32 n = 0;
    int rc;

    docs[0].doc_id = 7;
    docs[0].text = "alpha beta";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 3;
    docs[1].text = "alpha beta";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 1;
    docs[2].text = "gamma";
    docs[2].text_len = (u32)strlen(docs[2].text);

    query[0].len = 5;
    memcpy(query[0].text, "alpha", 5);
    query[0].text[5] = '\0';

    /* 全部命中：2 条，平局按 doc_id 升序 */
    rc = ft_score_linear_bm25(docs, 3, query, 1, 8, hits, &n);
    CHECK(rc == OK);
    CHECK(n == 2);
    if (n == 2) {
        CHECK(hits[0].doc_id == 3);
        CHECK(hits[1].doc_id == 7);
        CHECK(NEAR(hits[0].score, 0.4344571362775708));
        CHECK(NEAR(hits[0].score, hits[1].score));
    }

    /* top_k=1 → 只留最好的那条（平局里 doc_id 小的赢） */
    rc = ft_score_linear_bm25(docs, 3, query, 1, 1, hits, &n);
    CHECK(rc == OK);
    CHECK(n == 1);
    if (n == 1) {
        CHECK(hits[0].doc_id == 3);
    }

    /* top_k=0 → 一条都不给 */
    rc = ft_score_linear_bm25(docs, 3, query, 1, 0, hits, &n);
    CHECK(rc == OK);
    CHECK(n == 0);

    /* 查询词一个都没命中 → 0 条 */
    {
        FtToken miss[1];
        miss[0].len = 3;
        memcpy(miss[0].text, "zzz", 3);
        miss[0].text[3] = '\0';
        rc = ft_score_linear_bm25(docs, 3, miss, 1, 8, hits, &n);
        CHECK(rc == OK);
        CHECK(n == 0);
    }

    /* 空查询 → 0 条 */
    rc = ft_score_linear_bm25(docs, 3, query, 0, 8, hits, &n);
    CHECK(rc == OK);
    CHECK(n == 0);

    /* 参数非法 */
    CHECK(ft_score_linear_bm25(docs, 3, query, 1, 8, hits, NULL) == E_INVAL);
    CHECK(ft_score_linear_bm25(NULL, 3, query, 1, 8, hits, &n) == E_INVAL);
    CHECK(ft_score_linear_bm25(docs, 3, NULL, 1, 8, hits, &n) == E_INVAL);
    /* top_k 非 0 但没有结果缓冲区 */
    CHECK(ft_score_linear_bm25(docs, 3, query, 1, 8, NULL, &n) == E_NOSPACE);

    /*
     * avgdl = 0 的防护。
     *
     * avgdl=0 意味着语料里全是空文档。这种语料不会有 tf>0 的词，
     * 但 dl/avgdl 依然会算出 NaN —— NaN 的比较全是假，
     * 会把排序彻底搞乱。所以这里把长度归一的比例定义成 0。
     */
    {
        FtCorpus cs;
        cs.n_docs = 0;
        cs.avgdl = 0.0;
        CHECK(!isnan(ft_bm25_score(&cs, 0, 1, 0)));
        CHECK(ft_bm25_score(&cs, 0, 1, 0) > 0.0);
    }
}

int main(void)
{
    printf("test_ft_bm25:\n");

    t1_bm25_values();
    t2_length_norm();
    t3_tfidf_values();
    t4_linear_bm25();
    t5_linear_tfidf();
    t6_topk_and_edges();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过\n");
    return 0;
}
