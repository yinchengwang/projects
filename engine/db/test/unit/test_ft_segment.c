/*
 * test_ft_segment.c —— 全文检索段（segment）的单元验收（v1.3 步骤 2 · A1 单段版）
 *
 * 13 项：
 *   1. 空语料：建段 OK，检索 0 条
 *   2. 单文档单词：命中数 / doc_id / 分数与线性扫描全等
 *   3. 千文档对照（BM25）：条数 / 每条 doc_id / 每条分数全等
 *   4. 千文档对照（TF-IDF）：同上
 *   5. 查不到：语料里没有的词 → 0 条
 *   6. top_k 截断；top_k > max_out → E_NOSPACE（不静默截断）
 *   7. query 里的重复词（坑 M4-12）：与线性扫描全等
 *   8. 分词边界：连字符 / 大写 / 字母数字混合 / 中文
 *   9. 重开段：段头的字段要能从磁盘原样读回来
 *  10. 段的大小与文档量无关（是"倒排"而不是"逐文档存页"）
 *  11. 页读可证：查一个词的页读次数 < 段总页数
 *  12. 重复 doc_id 被拒 → E_INVAL
 *  13. 段头被改坏 → 打不开（E_CORRUPT），不带着错数据跑
 *
 * 【口径】top_k > max_out 一律 E_NOSPACE（段与线性扫描同口径）：本文件里所有"期望 OK"的调用都给足 max_out。
 *
 * 【这一整个文件在证明什么】
 *
 * A1 的验收标准是"**段检索的结果必须和线性扫描逐位一致**"。
 * 线性扫描（bm25.c）是**基准实现**：没有索引，一篇一篇切词、按公式算分 ——
 * 慢，但行为是定义好的、可以直接手算验证的。
 * 段是"真检索"：有字典、有倒排，只读该词的那几条 posting ——
 * 快，但行为是**推出来的**，推错了不报错，只是排序悄悄变了。
 *
 * 所以这里每一项都在做同一件事：**把段的结果和基准逐位对齐**。
 * 一旦出现偏差（分数差一个 ulp、排序差一名、多一条少一条），都会在这里被抓住；
 * 而且因为基准是手算可验证的，抓住之后能立刻判断"是段错了还是基准错了"。
 *
 * 【分数为什么用 == 精确比】
 * 两边的 idf / tf / dl / 累加顺序都一样，算出来的 double 应该**逐位相同**。
 * 若实测出现末位差异，那是"累加顺序或公式细节不一致"——是真 bug，不是浮点噪声。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ft/bm25.h"
#include "modalities/ft/ft_segment.h"
#include "modalities/ft/tokenizer.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ft_segment_test.db"

/* ---------------- 确定性语料 ---------------- */

#define CORPUS_N   1000u
#define VOCAB      30u
#define MAX_WORDS  40u
#define WORD_BYTES 4u    /* "wNN " —— 词长 3 + 1 个分隔符 */

/* 静态数组：1000 × 40 × 4 = 160 KB，放 BSS 里，不占栈 */
static char      g_text[CORPUS_N][MAX_WORDS * WORD_BYTES];
static FtDocText g_docs[CORPUS_N];

static u32 g_rng;

/*
 * 自己写的 LCG（不用 rand()）。
 *
 * rand() 的序列是**实现定义**的，换个 libc 结果就不一样了 ——
 * 那意味着"昨天过的测试今天可能挂"。自己写 LCG，序列钉死，语料可复现。
 */
static u32 rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) & 0x7FFFu;
}

/*
 * 造 1000 篇文档：词表固定 30 个词（w00..w29），长度 3~40 词**有变化**
 * （长度不变的话 BM25 的长度归一项是常数，测不出东西来）；
 * doc_id 用 1000+i 且**互不相同**（重复 doc_id 会被建段拒绝，见第 12 项）。
 */
static void build_corpus(void)
{
    u32 i;

    g_rng = 20260901u;      /* 固定种子 */
    for (i = 0; i < CORPUS_N; i++) {
        u32 len = 3u + rnd() % (MAX_WORDS - 2u);   /* 3..40 个词 */
        u32 j;
        u32 off = 0;

        for (j = 0; j < len; j++) {
            u32 w = rnd() % VOCAB;

            g_text[i][off++] = 'w';
            g_text[i][off++] = (char)('0' + (w / 10u) % 10u);
            g_text[i][off++] = (char)('0' + w % 10u);
            g_text[i][off++] = ' ';
        }
        g_docs[i].doc_id = 1000u + i;
        g_docs[i].text = g_text[i];
        g_docs[i].text_len = off;
    }
}

/*
 * 把查询串切成词。
 *
 * 线性扫描只收 FtToken 数组、不收原文 —— 所以对照时必须**手工切词**，
 * 而且必须用**同一个**分词器（ft_tokenize），否则两边语义不同，对照就没意义了。
 */
static u32 tokenize_query(const char *q, FtToken *out, u32 max)
{
    u32 n = 0;

    if (ft_tokenize(q, (u32)strlen(q), out, max, &n) != OK) {
        return 0;
    }
    return n;
}

/* 两份结果全等：条数、每条的 doc_id、每条的 score（分数用 == 精确比，见文件头） */
static void cmp_hits(const char *what, const FtHit *seg, u32 n_seg,
                     const FtHit *lin, u32 n_lin)
{
    u32 i;

    if (n_seg != n_lin) {
        printf("    FAIL %s:%d: %s 条数不等（段 %u，线性 %u）\n",
               __FILE__, __LINE__, what, n_seg, n_lin);
        failures++;
        return;
    }
    for (i = 0; i < n_seg; i++) {
        if (seg[i].doc_id != lin[i].doc_id) {
            printf("    FAIL %s:%d: %s 第 %u 条 doc_id 不等（段 %u，线性 %u）\n",
                   __FILE__, __LINE__, what, i, seg[i].doc_id, lin[i].doc_id);
            failures++;
            return;
        }
        if (seg[i].score != lin[i].score) {
            printf("    FAIL %s:%d: %s 第 %u 条分数不等（段 %.17g，线性 %.17g）\n",
                   __FILE__, __LINE__, what, i, seg[i].score, lin[i].score);
            failures++;
            return;
        }
    }
}

static void fresh(void)
{
    remove(PATH);
    remove(PATH "-wal");
}

/* ---------------- 1. 空语料 ---------------- */

static void test_empty_corpus(void)
{
    printf("  1. 空语料：建段 OK，检索 0 条\n");
    fresh();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, NULL, 0, 0, &head) == OK);
    CHECK(head != PAGE_ID_NIL);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    CHECK(seg != NULL);
    if (seg != NULL) {
        CHECK(seg->doc_count == 0);
        CHECK(ft_segment_nterms(seg) == 0);

        /* 空语料里查什么都该是 0 条，而且必须是 OK（不是"段坏了"） */
        FtHit hits[16];
        u32 n = 0;
        CHECK(ft_segment_search(seg, "hello", 5, 10, 16, hits, &n) == OK);
        CHECK(n == 0);
        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 2. 单文档单词 ---------------- */

static void test_single_doc(void)
{
    printf("  2. 单文档单词：命中数 / doc_id / 分数与线性扫描全等\n");
    fresh();

    FtDocText docs[1];
    docs[0].doc_id = 7;
    docs[0].text = "hello world";
    docs[0].text_len = 11;

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 1, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        FtHit seg_hits[16];
        u32 n_seg = 0;

        CHECK(ft_segment_search(seg, "hello", 5, 10, 16, seg_hits, &n_seg) == OK);
        CHECK(n_seg == 1);
        if (n_seg == 1) {
            CHECK(seg_hits[0].doc_id == 7);

            /* 基准：线性扫描 */
            FtToken qt[4];
            u32 nq = tokenize_query("hello", qt, 4);
            FtHit lin_hits[16];
            u32 n_lin = 0;

            CHECK(nq == 1);
            CHECK(ft_score_linear_bm25(docs, 1, qt, nq, 10, 16, lin_hits, &n_lin) == OK);
            CHECK(n_lin == 1);
            if (n_lin == 1) {
                /* 分数必须逐位相同 */
                CHECK(seg_hits[0].score == lin_hits[0].score);
            }
        }
        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 3. 千文档对照（BM25） ---------------- */

static void test_thousand_bm25(void)
{
    printf("  3. 千文档对照（BM25）：条数 / doc_id / 分数全等\n");
    fresh();
    build_corpus();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, g_docs, CORPUS_N, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        const char *q = "w05 w17 w29";
        FtHit seg_hits[16];
        u32 n_seg = 0;

        CHECK(ft_segment_search(seg, q, (u32)strlen(q), 10, 16, seg_hits, &n_seg) == OK);
        CHECK(n_seg == 10);

        /* 基准：同样 1000 篇文档，线性扫描 top-10 */
        FtToken qt[8];
        u32 nq = tokenize_query(q, qt, 8);
        FtHit lin_hits[16];
        u32 n_lin = 0;

        CHECK(nq == 3);
        CHECK(ft_score_linear_bm25(g_docs, CORPUS_N, qt, nq, 10, 16, lin_hits, &n_lin) == OK);
        cmp_hits("BM25 千文档", seg_hits, n_seg, lin_hits, n_lin);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 4. 千文档对照（TF-IDF） ---------------- */

static void test_thousand_tfidf(void)
{
    printf("  4. 千文档对照（TF-IDF）：条数 / doc_id / 分数全等\n");
    fresh();
    build_corpus();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, g_docs, CORPUS_N, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        const char *q = "w05 w17 w29";
        FtHit seg_hits[16];
        u32 n_seg = 0;

        CHECK(ft_segment_search_tfidf(seg, q, (u32)strlen(q), 10, 16, seg_hits, &n_seg) == OK);
        CHECK(n_seg == 10);

        FtToken qt[8];
        u32 nq = tokenize_query(q, qt, 8);
        FtHit lin_hits[16];
        u32 n_lin = 0;

        CHECK(nq == 3);
        CHECK(ft_score_linear_tfidf(g_docs, CORPUS_N, qt, nq, 10, 16, lin_hits, &n_lin) == OK);
        cmp_hits("TF-IDF 千文档", seg_hits, n_seg, lin_hits, n_lin);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 5. 查不到 ---------------- */

static void test_no_match(void)
{
    printf("  5. 查不到：语料里没有的词 → 0 条\n");
    fresh();

    FtDocText docs[3];
    docs[0].doc_id = 1;
    docs[0].text = "alpha beta gamma";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 2;
    docs[1].text = "delta epsilon";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 3;
    docs[2].text = "zeta";
    docs[2].text_len = (u32)strlen(docs[2].text);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 3, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        FtHit hits[16];
        u32 n = 0;

        /* 词在字典里根本不存在 → 0 条，且必须返回 OK */
        CHECK(ft_segment_search(seg, "nosuchword", 10, 10, 16, hits, &n) == OK);
        CHECK(n == 0);

        /* TF-IDF 版同样 */
        CHECK(ft_segment_search_tfidf(seg, "nosuchword", 10, 10, 16, hits, &n) == OK);
        CHECK(n == 0);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 6. top_k 截断 / max_out ---------------- */

static void test_topk_and_maxout(void)
{
    printf("  6. top_k 截断；top_k > max_out → E_NOSPACE\n");
    fresh();

    FtDocText docs[8];
    static char bufs[8][64];
    u32 i;

    /* 8 篇都含 "alpha"，长度各不相同（长度会影响 BM25 的分数） */
    for (i = 0; i < 8; i++) {
        u32 j;
        u32 off = 0;

        for (j = 0; j <= i; j++) {
            bufs[i][off++] = 'a';
            bufs[i][off++] = 'l';
            bufs[i][off++] = 'p';
            bufs[i][off++] = 'h';
            bufs[i][off++] = 'a';
            bufs[i][off++] = ' ';
        }
        docs[i].doc_id = 100 + i;
        docs[i].text = bufs[i];
        docs[i].text_len = off;
    }

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 8, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        FtHit hits[16];
        u32 n = 0;

        /* top_k = 3 → 给 3 条 */
        CHECK(ft_segment_search(seg, "alpha", 5, 3, 16, hits, &n) == OK);
        CHECK(n == 3);

        /*
         * top_k > max_out → E_NOSPACE。
         *
         * 重点是**不静默截断**：截断出来的结果"看起来对其实错"，
         * 比报错坏得多 —— 报错会让人停下，错的结果会一路流下去。
         */
        CHECK(ft_segment_search(seg, "alpha", 5, 10, 5, hits, &n) == E_NOSPACE);
        CHECK(ft_segment_search_tfidf(seg, "alpha", 5, 10, 5, hits, &n) == E_NOSPACE);

        /* top_k == max_out → 正常 */
        CHECK(ft_segment_search(seg, "alpha", 5, 5, 5, hits, &n) == OK);
        CHECK(n == 5);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 7. query 里的重复词（坑 M4-12） ---------------- */

static void test_dup_query_term(void)
{
    printf("  7. query 重复词：不去重，与线性扫描全等\n");
    fresh();

    FtDocText docs[3];
    docs[0].doc_id = 1;
    docs[0].text = "alpha beta gamma";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 2;
    docs[1].text = "alpha alpha delta";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 3;
    docs[2].text = "alpha zeta";
    docs[2].text_len = (u32)strlen(docs[2].text);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 3, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        FtHit seg_hits[16];
        u32 n_seg = 0;

        /* "alpha alpha" → 两个词，各自贡献一次（不去重） */
        CHECK(ft_segment_search(seg, "alpha alpha", 11, 10, 16, seg_hits, &n_seg) == OK);
        CHECK(n_seg == 3);

        /* 基准：同样手工切词，同样传两个 token */
        FtToken qt[4];
        u32 nq = tokenize_query("alpha alpha", qt, 4);
        FtHit lin_hits[16];
        u32 n_lin = 0;

        CHECK(nq == 2);
        CHECK(ft_score_linear_bm25(docs, 3, qt, nq, 10, 16, lin_hits, &n_lin) == OK);
        cmp_hits("重复 query 词", seg_hits, n_seg, lin_hits, n_lin);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 8. 分词边界 ---------------- */

/*
 * 语料里故意放四种"边界"：
 *   "state-of-the-art" —— 连字符是分隔符 → 4 个词
 *   "Hello"            —— 大写要小写化
 *   "a1b2"             —— 字母数字混合是一个词
 *   "检索"              —— 非 ASCII 字节当分隔符（本版不做中文分词）
 *
 * 查询侧给 "state" / "hello" / "a1b2"，两边用同一个分词器，
 * 语义应该完全一致。
 */
static void test_tokenizer_boundary(void)
{
    printf("  8. 分词边界：连字符 / 大写 / 字母数字混合 / 中文\n");
    fresh();

    FtDocText docs[4];
    docs[0].doc_id = 1;
    docs[0].text = "state-of-the-art";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 2;
    docs[1].text = "Hello World";
    docs[1].text_len = (u32)strlen(docs[1].text);
    docs[2].doc_id = 3;
    docs[2].text = "a1b2 c3";
    docs[2].text_len = (u32)strlen(docs[2].text);
    docs[3].doc_id = 4;
    docs[3].text = "\xE6\xA3\x80\xE7\xB4\xA2 hello";
    docs[3].text_len = (u32)strlen(docs[3].text);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 4, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        const char *queries[3];
        u32 qi;

        queries[0] = "state";
        queries[1] = "hello";
        queries[2] = "a1b2";

        for (qi = 0; qi < 3; qi++) {
            FtHit seg_hits[16];
            u32 n_seg = 0;
            FtToken qt[4];
            u32 nq = tokenize_query(queries[qi], qt, 4);
            FtHit lin_hits[16];
            u32 n_lin = 0;

            CHECK(ft_segment_search(seg, queries[qi], (u32)strlen(queries[qi]),
                                    10, 16, seg_hits, &n_seg) == OK);
            CHECK(ft_score_linear_bm25(docs, 4, qt, nq, 10, 16, lin_hits, &n_lin) == OK);
            cmp_hits("分词边界", seg_hits, n_seg, lin_hits, n_lin);
        }

        /* "state" 只能命中第 1 篇（不能把 "of" "the" "art" 也算进来） */
        {
            FtHit seg_hits[16];
            u32 n_seg = 0;

            CHECK(ft_segment_search(seg, "state", 5, 10, 16, seg_hits, &n_seg) == OK);
            CHECK(n_seg == 1);
            if (n_seg == 1) {
                CHECK(seg_hits[0].doc_id == 1);
            }
        }

        /* 大写要小写化："Hello" 命中的是 doc 2 和 doc 4 */
        {
            FtHit seg_hits[16];
            u32 n_seg = 0;

            CHECK(ft_segment_search(seg, "hello", 5, 10, 16, seg_hits, &n_seg) == OK);
            CHECK(n_seg == 2);
            if (n_seg == 2) {
                /*
                 * 只断言"命中了哪两篇"，不断言谁在前。
                 *
                 * 谁在前是 BM25 长度归一说了算（doc4 只有 1 个词，
                 * 比 doc2 的 2 个词更短 → 分数更高 → 排前面），
                 * 那是第 3/4 项（与线性扫描逐位对照）该管的事，
                 * 这里不重复它的结论，免得改一处公式要改两处断言。
                 */
                CHECK((seg_hits[0].doc_id == 2 && seg_hits[1].doc_id == 4) ||
                      (seg_hits[0].doc_id == 4 && seg_hits[1].doc_id == 2));
            }
        }

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 9. 重开段 ---------------- */

static void test_reopen(void)
{
    printf("  9. 重开段：段头字段能从磁盘原样读回来\n");
    fresh();
    build_corpus();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, g_docs, 100, 7, &head) == OK);

    /* 第一次打开：记下段头的字段 */
    FtSegment *seg = NULL;
    u32 doc_count = 0;
    u32 n_terms = 0;
    u32 dict_count = 0;
    u32 post_count = 0;
    u64 total_len = 0;

    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        doc_count = seg->doc_count;
        n_terms = seg->n_terms;
        dict_count = seg->dict_count;
        post_count = seg->post_count;
        total_len = seg->total_len;

        CHECK(doc_count == 100);
        CHECK(n_terms == VOCAB);            /* 词表就 30 个词 */
        CHECK(ft_segment_nterms(seg) == n_terms);
        CHECK(total_len > 0);

        ft_segment_close(seg);
    }

    /* 第二次打开：字段必须一模一样（段头是落盘的，不是在内存里现编的） */
    FtSegment *seg2 = NULL;
    CHECK(ft_segment_open(pg, head, &seg2) == OK);
    if (seg2 != NULL) {
        CHECK(seg2->doc_count == doc_count);
        CHECK(seg2->n_terms == n_terms);
        CHECK(seg2->dict_count == dict_count);
        CHECK(seg2->post_count == post_count);
        CHECK(seg2->total_len == total_len);
        CHECK(seg2->seg_id == 7);
        ft_segment_close(seg2);
    }

    pager_close(pg);
}

/* ---------------- 10. 段的大小与文档量无关 ---------------- */

static void test_segment_size(void)
{
    printf("  10. 段的大小与文档量无关（倒排，不是逐文档存页）\n");
    fresh();
    build_corpus();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, g_docs, CORPUS_N, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        u32 total_pages = 1u + seg->dict_count + seg->post_count;

        /*
         * 1000 篇文档，段却只有几十页。
         *
         * 如果段是"逐文档存页"的，1000 篇至少要 1000 页。
         * 倒排把"每篇文档的一行"变成"每个词的一条 posting 链"，
         * 段的大小由**词数**决定，与文档数无关 —— 这就是倒排的意义。
         */
        CHECK(total_pages < 200u);
        printf("    （%u 篇文档，段共 %u 页：段头 1 + 字典 %u + 倒排 %u）\n",
               CORPUS_N, total_pages, seg->dict_count, seg->post_count);
        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 11. 页读可证 ---------------- */

static void test_page_reads(void)
{
    printf("  11. 页读可证：查一个词的页读 < 段总页数\n");
    fresh();
    build_corpus();

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 256, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, g_docs, CORPUS_N, 0, &head) == OK);

    FtSegment *seg = NULL;
    CHECK(ft_segment_open(pg, head, &seg) == OK);
    if (seg != NULL) {
        u32 total_pages = 1u + seg->dict_count + seg->post_count;
        u64 before = ft_segment_page_reads(seg);
        FtHit hits[16];
        u32 n = 0;

        CHECK(ft_segment_search(seg, "w05", 3, 10, 16, hits, &n) == OK);
        CHECK(n > 0);

        u64 reads = ft_segment_page_reads(seg) - before;

        /*
         * 查一个词：字典二分（几页）+ 该词的 posting（几页）。
         * **不该**把整个段都读一遍 —— 那就是"没建索引，只是把倒排当摆设"。
         */
        CHECK(reads < total_pages);
        printf("    （查一个词读了 %llu 页，段共 %u 页）\n",
               (unsigned long long)reads, total_pages);

        ft_segment_close(seg);
    }
    pager_close(pg);
}

/* ---------------- 12. 重复 doc_id 被拒 ---------------- */

static void test_dup_doc_id(void)
{
    printf("  12. 重复 doc_id → E_INVAL\n");
    fresh();

    FtDocText docs[2];
    docs[0].doc_id = 5;
    docs[0].text = "alpha beta";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 5;               /* 和上一篇同一个 id */
    docs[1].text = "gamma delta";
    docs[1].text_len = (u32)strlen(docs[1].text);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    /*
     * 重复 doc_id 必须被拒：同一个 id 的两篇文档在段里会合并成一条 posting，
     * 而线性扫描那边还是两条 —— A1 的对照就不成立，而且这种偏差**不会报错**。
     */
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 2, 0, &head) == E_INVAL);

    pager_close(pg);
}

/* ---------------- 13. 段头被改坏 → 打不开 ---------------- */

static void test_corrupt_head(void)
{
    printf("  13. 段头被改坏 → 打不开（E_CORRUPT）\n");
    fresh();

    FtDocText docs[2];
    docs[0].doc_id = 1;
    docs[0].text = "alpha beta";
    docs[0].text_len = (u32)strlen(docs[0].text);
    docs[1].doc_id = 2;
    docs[1].text = "gamma delta";
    docs[1].text_len = (u32)strlen(docs[1].text);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    u32 head = PAGE_ID_NIL;
    CHECK(ft_segment_build(pg, NULL, NULL, 0, docs, 2, 0, &head) == OK);

    /*
     * 把段头页的页类型改成 PT_HEAP（不是 PT_FT_HEAD 了）。
     *
     * 改完要重算校验和，否则 pager 读页时先因校验和失败而拒 ——
     * 那测的就不是"页类型不对"，是"校验和不对"，两码事。
     */
    PageFrame *fr = NULL;
    CHECK(pager_get(pg, head, &fr) == OK);
    fr->data[H_TYPE] = (u8)PT_HEAP;
    page_set_checksum(fr->data);
    CHECK(pager_unpin(pg, head, true) == OK);

    /* 打开必须失败：宁可打不开，也不要跑出错误的答案 */
    FtSegment *bad = NULL;
    CHECK(ft_segment_open(pg, head, &bad) == E_CORRUPT);
    CHECK(bad == NULL);

    pager_close(pg);
}

int main(void)
{
    printf("test_ft_segment:\n");

    fresh();
    test_empty_corpus();
    test_single_doc();
    test_thousand_bm25();
    test_thousand_tfidf();
    test_no_match();
    test_topk_and_maxout();
    test_dup_query_term();
    test_tokenizer_boundary();
    test_reopen();
    test_segment_size();
    test_page_reads();
    test_dup_doc_id();
    test_corrupt_head();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  13 项验收全部通过\n");
    return 0;
}
