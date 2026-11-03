/*
 * test_ft_index.c —— 全文检索**索引**（多段 + tombstone + 合并）的验收（v1.3 步骤 3）
 *
 * 4 项：
 *   1. A1 多段一致性：跨段检索与"整个语料的线性扫描"逐位一致（BM25 与 TF-IDF 各一遍）
 *      —— 外加严格口径（top_k > max_out → E_NOSPACE，不静默截断）
 *   2. A3 删除生效：删掉的文档检索不到；合并之后依然检索不到，且段数下降
 *   3. A6 合并前后一致：merge 之前的结果与 merge 之后的结果逐位一致
 *      —— 外加页读累计（坑 M4-16）：合并不会让索引级累计页读缩水
 *   4. 字典跨页（坑 M4-23）：字典页**连号**、词一个都不丢、检索与线性扫描逐位一致
 *
 * 【这一整个文件在证明什么】
 *
 * 步骤 2 已经证明"单段检索 = 线性扫描"。这一步证明的是**跨段**的那一层：
 * 一个索引里有很多段，而 BM25 的 idf 依赖语料级的 N / df / avgdl ——
 * 分段之后它们被摊在各段的段头里，必须在检索时**实时聚合成全局量**。
 * 聚合口径只要有一处不对（用段自己的 df、avgdl 先除后加、累加顺序变了），
 * 结果就与线性扫描对不上，而且**不会报错** —— 只是排序悄悄变了。
 * 所以 A1 仍然是"与基准逐位对齐"，只是基准的对象从"整篇语料"换成了"同一批语料"。
 *
 * 【为什么用 9 段】
 *
 * 合并的触发阈值是 FT_MERGE_THRESHOLD = 8：段数 ≤ 8 时合并什么都不做。
 * 要真的走到"合并"这条路径，段数必须 > 8 —— 所以这里用 9 段。
 * 每段 60 篇、总共 540 篇：段数够触发合并，语料又够大到"跨段统计量"有意义。
 *
 * 【第 4 项为什么单列：字典跨页（坑 M4-23）】
 *
 * 段头只记**连续区间**：dict_start + dict_count。
 * 读侧（跨页二分、词条游标）都按"连续区间"读页 ——
 * 所以字典页必须**连号**，否则读侧会把倒排页当成字典页。
 *
 * 单页字典时这个 bug **测不出来**（词表小的时候字典正好一页），
 * 所以这里专门造一个"词表大到字典跨好几页"的语料：
 * 每个段 600 篇文档、601 个词 → 字典 4 页。
 *
 * 症状是三种：查不到（E_NOTFOUND）、命中 0 篇、**静默丢词**。
 * 最坏的是第三种 —— 不报错，只是词悄悄没了，
 * 所以这一项把"每一个词都还能查到"逐个验一遍。
 *
 * 【删除的语义（别把它当成 bug）】
 *
 * 被删的文档**不进结果**，但**仍然计入 N 与 df**（直到合并把它物理丢掉）——
 * 与 Lucene 一致。所以"删除后 idf 略有偏差"是**已知**行为。
 * 合并时被删的文档才真正从段里消失 → ndocs 会少 1。
 *
 * 【分数为什么用 == 精确比】
 *
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
#include "modalities/ft/ft_index.h"
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

#define PATH "test/tmp/ft_index_test.db"

/* ---------------- 确定性语料 ---------------- */

#define SEG_N     9u       /* 段数：必须 > FT_MERGE_THRESHOLD(8) 才触发合并 */
#define PER_SEG   60u      /* 每段篇数 */
#define DOC_N     (SEG_N * PER_SEG)   /* 540 篇 */
#define VOCAB     30u      /* 词表 w00..w29 */
#define MAX_WORDS 40u      /* 每篇最多 40 个词 */
#define WORD_BYTES 4u      /* "wNN " —— 词长 3 + 1 个分隔符 */

/* 静态数组：540 × 40 × 4 = 86 KB，放 BSS 里，不占栈 */
static char      g_text[DOC_N][MAX_WORDS * WORD_BYTES];
static FtDocText g_docs[DOC_N];
static u8        g_has_w05[DOC_N];   /* 这篇文档里出现过 w05 吗 */

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
 * 造 540 篇文档：词表固定 30 个词（w00..w29），长度 3~40 词**有变化**
 * （长度不变的话 BM25 的长度归一项是常数，测不出东西来）；
 * doc_id 用 1000+i 且**互不相同**（索引内 doc_id 必须唯一，否则 A1 对照不成立）。
 */
static void build_corpus(void)
{
    u32 i;

    g_rng = 20260913u;      /* 固定种子 */
    for (i = 0; i < DOC_N; i++) {
        u32 len = 3u + rnd() % (MAX_WORDS - 2u);   /* 3..40 个词 */
        u32 j;
        u32 off = 0;

        g_has_w05[i] = 0;
        for (j = 0; j < len; j++) {
            u32 w = rnd() % VOCAB;

            if (w == 5u) {
                g_has_w05[i] = 1;
            }
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
static void cmp_hits(const char *what, const FtHit *a, u32 n_a, const FtHit *b, u32 n_b)
{
    u32 i;

    if (n_a != n_b) {
        printf("    FAIL %s:%d: %s 条数不等（%u，%u）\n", __FILE__, __LINE__, what, n_a, n_b);
        failures++;
        return;
    }
    for (i = 0; i < n_a; i++) {
        if (a[i].doc_id != b[i].doc_id) {
            printf("    FAIL %s:%d: %s 第 %u 条 doc_id 不等（%u，%u）\n",
                   __FILE__, __LINE__, what, i, a[i].doc_id, b[i].doc_id);
            failures++;
            return;
        }
        if (a[i].score != b[i].score) {
            printf("    FAIL %s:%d: %s 第 %u 条分数不等（%.17g，%.17g）\n",
                   __FILE__, __LINE__, what, i, a[i].score, b[i].score);
            failures++;
            return;
        }
    }
}

/* 结果里有没有这个 doc_id */
static int has_hit(const FtHit *hits, u32 n, u32 doc_id)
{
    u32 i;

    for (i = 0; i < n; i++) {
        if (hits[i].doc_id == doc_id) {
            return 1;
        }
    }
    return 0;
}

static void fresh(void)
{
    remove(PATH);
    remove(PATH "-wal");
}

/*
 * 建一个装了 9 段的索引。
 *
 * 每段 60 篇：段的大小一致 → 合并窗口的"文档数之和最小"必然落在最前面两个段上
 * （宽度 2 的窗口和 = 120，比宽度 3 的 180 小），选择是确定的，测试才可复现。
 */
static int build_index(Pager *pg, FtIndex **out)
{
    u32 s;
    int rc;

    rc = ft_index_create(pg, NULL, NULL, 0, out);
    if (rc != OK) {
        return rc;
    }
    for (s = 0; s < SEG_N; s++) {
        u32 seg_id = 0;

        rc = ft_index_add(*out, 0, &g_docs[s * PER_SEG], PER_SEG, &seg_id);
        if (rc != OK) {
            return rc;
        }
    }
    return OK;
}

/* ---------------- 1. A1：多段一致性 ---------------- */

static void test_a1_multisegment(void)
{
    static const char *const queries[] = {"w05 w07", "w03 w11 w19", "w22", "w05 w05 w07"};
    static FtHit idx_hits[64];
    static FtHit lin_hits[64];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    u32 qi;

    printf("  1. A1 多段一致性：跨段检索 vs 整个语料的线性扫描（逐位一致）\n");
    fresh();

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }

    /* 9 个段、540 篇 —— 跨段统计量（N / df / avgdl）必须聚合自全部段 */
    CHECK(ft_index_nsegs(ix) == SEG_N);
    CHECK(ft_index_ndocs(ix) == DOC_N);
    CHECK(ft_index_ntombstones(ix) == 0);

    for (qi = 0; qi < 4; qi++) {
        const char *q = queries[qi];
        u32 qlen = (u32)strlen(q);
        FtToken qt[8];
        u32 nq = tokenize_query(q, qt, 8);
        u32 n_idx = 0;
        u32 n_lin = 0;
        const u32 top_k = 32;
        const u32 max_out = 64;

        CHECK(nq > 0);

        /* BM25：索引检索 vs 线性扫描 */
        CHECK(ft_index_search(ix, q, qlen, top_k, max_out, idx_hits, &n_idx) == OK);
        CHECK(ft_score_linear_bm25(g_docs, DOC_N, qt, nq, top_k, max_out, lin_hits, &n_lin) == OK);
        cmp_hits("A1 BM25", idx_hits, n_idx, lin_hits, n_lin);

        /* TF-IDF 对照组 */
        n_idx = 0;
        n_lin = 0;
        CHECK(ft_index_search_tfidf(ix, q, qlen, top_k, max_out, idx_hits, &n_idx) == OK);
        CHECK(ft_score_linear_tfidf(g_docs, DOC_N, qt, nq, top_k, max_out, lin_hits, &n_lin) == OK);
        cmp_hits("A1 TF-IDF", idx_hits, n_idx, lin_hits, n_lin);
    }

    /* 严格口径：top_k > max_out 一律 E_NOSPACE（不静默截断） */
    {
        u32 n = 12345u;

        CHECK(ft_index_search(ix, "w05", 3, 10, 8, idx_hits, &n) == E_NOSPACE);
        CHECK(n == 0);
        CHECK(ft_index_search(ix, "w05", 3, 10, 8, NULL, &n) == E_NOSPACE);
        CHECK(n == 0);
        CHECK(ft_index_search(ix, "w05", 3, 0, 8, NULL, &n) == E_INVAL);
        CHECK(n == 0);
    }

    ft_index_close(ix);
    pager_close(pg);
}

/* ---------------- 2. A3：删除生效 ---------------- */

static void test_a3_delete(void)
{
    static FtHit hits[512];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    u32 victim = 0;
    u32 i;
    u32 n = 0;
    u32 nsegs_before;
    u32 ndocs_before;
    u32 merged = 0;

    printf("  2. A3 删除生效：删掉的文档检索不到；合并之后依然检索不到\n");
    fresh();

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }

    /*
     * 找一篇命中 w05 的文档，且必须在**段 0** 里 ——
     * 段 0 正是合并窗口要收进去的两个段之一，这样合并时它会被**物理丢掉**。
     */
    for (i = 0; i < PER_SEG; i++) {
        if (g_has_w05[i]) {
            victim = g_docs[i].doc_id;
            break;
        }
    }
    CHECK(victim != 0);
    if (victim == 0) {
        ft_index_close(ix);
        pager_close(pg);
        return;
    }

    /* 删除之前：它必须在结果里（否则这个用例就是空的，测不出任何东西） */
    CHECK(ft_index_search(ix, "w05", 3, 512, 512, hits, &n) == OK);
    CHECK(has_hit(hits, n, victim) == 1);

    /* 打 tombstone：立刻生效 */
    CHECK(ft_index_delete(ix, 0, victim) == OK);
    CHECK(ft_index_ntombstones(ix) == 1);

    n = 0;
    CHECK(ft_index_search(ix, "w05", 3, 512, 512, hits, &n) == OK);
    CHECK(has_hit(hits, n, victim) == 0);

    /* 再删一次同一个 doc_id：合法的空操作（不多记 tombstone） */
    CHECK(ft_index_delete(ix, 0, victim) == OK);
    CHECK(ft_index_ntombstones(ix) == 1);

    /* 删一个不存在的 doc_id：不报错（索引不知道哪些 doc_id 存在，只白占一个 tombstone 位） */
    CHECK(ft_index_delete(ix, 0, 999999u) == OK);
    CHECK(ft_index_ntombstones(ix) >= 1u);

    /* 合并：段数必须下降，被删的文档被物理丢掉 → ndocs 少 1 */
    nsegs_before = ft_index_nsegs(ix);
    ndocs_before = ft_index_ndocs(ix);
    CHECK(ft_index_merge(ix, 0, &merged) == OK);
    CHECK(merged >= 2u);
    CHECK(ft_index_nsegs(ix) == nsegs_before - (merged - 1u));
    CHECK(ft_index_ndocs(ix) == ndocs_before - 1u);

    /* 合并之后依然检索不到 */
    n = 0;
    CHECK(ft_index_search(ix, "w05", 3, 512, 512, hits, &n) == OK);
    CHECK(has_hit(hits, n, victim) == 0);

    ft_index_close(ix);
    pager_close(pg);
}

/* ---------------- 3. A6：合并前后一致 ---------------- */

static void test_a6_merge_consistency(void)
{
    static const char *const queries[] = {"w05 w07", "w03 w11 w19"};
    static FtHit before[2][64];
    static FtHit after[2][64];
    static FtHit lin_hits[64];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    u64 pr_before;
    u64 pr_after;
    u32 nsegs_before;
    u32 merged = 0;
    u32 n_before[2];
    u32 n_after[2];
    u32 qi;

    printf("  3. A6 合并前后一致：merge 前后结果逐位一致\n");
    fresh();

    CHECK(pager_open(PATH, 1024, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(build_index(pg, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }

    /* ---- 合并之前：先把两个查询的结果都取下来 ---- */
    for (qi = 0; qi < 2; qi++) {
        const char *q = queries[qi];
        u32 qlen = (u32)strlen(q);

        CHECK(ft_index_search(ix, q, qlen, 32, 64, before[qi], &n_before[qi]) == OK);
    }

    /*
     * 页读累计（坑 M4-16）：先检索一次，让各段都攒下页读计数；
     * 合并会释放老段 —— 若是"各段求和"，这个数会**缩水**。
     */
    pr_before = ft_index_page_reads(ix);
    CHECK(pr_before > 0);

    nsegs_before = ft_index_nsegs(ix);
    CHECK(ft_index_merge(ix, 0, &merged) == OK);
    CHECK(merged >= 2u);
    CHECK(ft_index_nsegs(ix) == nsegs_before - (merged - 1u));

    pr_after = ft_index_page_reads(ix);
    CHECK(pr_after >= pr_before);
    CHECK(pr_after > 0);

    /* ---- 合并之后：逐位一致 ---- */
    for (qi = 0; qi < 2; qi++) {
        const char *q = queries[qi];
        u32 qlen = (u32)strlen(q);
        FtToken qt[8];
        u32 nq = tokenize_query(q, qt, 8);
        u32 n_lin = 0;

        CHECK(nq > 0);
        CHECK(ft_index_search(ix, q, qlen, 32, 64, after[qi], &n_after[qi]) == OK);
        cmp_hits("A6 合并前后", before[qi], n_before[qi], after[qi], n_after[qi]);

        /*
         * 合并不该改变检索语义：合并后的结果仍要与线性扫描逐位一致
         * （文档集合没变 → N / df / avgdl 都没变 → 分数必须一模一样）。
         */
        CHECK(ft_score_linear_bm25(g_docs, DOC_N, qt, nq, 32, 64, lin_hits, &n_lin) == OK);
        cmp_hits("A6 合并后 vs 线性", after[qi], n_after[qi], lin_hits, n_lin);
    }

    ft_index_close(ix);
    pager_close(pg);
}

/* ---------------- 4. 字典跨页（坑 M4-23） ---------------- */

/*
 * 大词表语料：每个段 600 篇文档。
 *
 * 每篇文档 = 一个**私有词** tNNNN + 一个**公共词** common：
 *   - 私有词 → 每个段各有 600 个不同的词，加上 common 共 601 个词；
 *   - 公共词 → 每个词在每个段里都出现一次 → 跨段 df = 9（9 个段）。
 *
 * 601 个词 × 每条约 21 字节 ≈ 12.6 KB，而一页字典只能装 4076 字节
 * → 字典要 4 页（≥ 3 页才算"跨页"）。
 */
#define BIG_SEGS  9u
#define BIG_PER   600u
#define BIG_TOTAL (BIG_SEGS * BIG_PER)
#define BIG_TEXT  16u

static char      g_big_text[BIG_TOTAL][BIG_TEXT];
static FtDocText g_big_docs[BIG_TOTAL];

static void build_big_corpus(void)
{
    u32 s, i;

    for (s = 0; s < BIG_SEGS; s++) {
        for (i = 0; i < BIG_PER; i++) {
            u32 k = s * BIG_PER + i;

            snprintf(g_big_text[k], BIG_TEXT, "t%04u common ", i);
            g_big_docs[k].doc_id = s * 10000u + i;
            g_big_docs[k].text = g_big_text[k];
            g_big_docs[k].text_len = (u32)strlen(g_big_text[k]);
        }
    }
}

/*
 * 字典页必须**连号**，而且区间里的每一页都得是 PT_FT_DICT。
 *
 * 这是坑 M4-23 的回归防线：
 * 曾经的写法是"一个词先写倒排、再写字典"，两个写入器各自 pager_alloc
 * → 倒排页与字典页交替分配，字典页**不连号**。
 * 读侧按连续区间读页 → 读到的是倒排页 → 查不到 / 命中 0 / 静默丢词。
 */
static void check_dict_pages(Pager *pg, const FtSegment *seg, const char *tag)
{
    u32 i;

    for (i = 0; i < seg->dict_count; i++) {
        u32 pid = seg->dict_start + i;
        PageFrame *fr = NULL;

        if (pager_get(pg, pid, &fr) != OK) {
            printf("    FAIL %s：字典页 %u（页号 %u）读不出来\n", tag, i, pid);
            failures++;
            continue;
        }
        if (fr->data[H_TYPE] != (u8)PT_FT_DICT) {
            printf("    FAIL %s：字典页 %u（页号 %u）页类型是 0x%02x，不是 PT_FT_DICT(0x%02x)\n",
                   tag, i, pid, fr->data[H_TYPE], (unsigned)PT_FT_DICT);
            failures++;
        }
        pager_unpin(pg, pid, false);
    }
}

/* 这个词还在段里吗？（df 与链头都要拿到 —— 拿不到就是"词丢了"） */
static int term_present(const FtSegment *seg, const char *term, u32 len)
{
    u32 df = 0;
    u64 ptr = 0;

    return ft_segment_lookup(seg, term, len, &df, &ptr) == OK && df > 0;
}

static void test_dict_multipage(void)
{
    static FtHit idx_hits[64];
    static FtHit lin_hits[64];
    static FtHit before[64];
    static FtHit after[64];
    Pager *pg = NULL;
    FtIndex *ix = NULL;
    const FtSegment *seg = NULL;
    const char *q = "t0000 t0300 t0599 common";
    u32 qlen = (u32)strlen(q);
    u32 n_idx = 0, n_lin = 0, n_before = 0, n_after = 0;
    u32 df = 0;
    u64 ptr = 0;
    u32 merged = 0;
    u32 s, i;

    printf("  4. 字典跨页（坑 M4-23）：字典页连号 / 词不丢 / 与线性扫描逐位一致\n");
    fresh();

    CHECK(pager_open(PATH, 4096, &pg) == OK);
    if (pg == NULL) {
        return;
    }
    CHECK(ft_index_create(pg, NULL, NULL, 0, &ix) == OK);
    if (ix == NULL) {
        pager_close(pg);
        return;
    }
    for (s = 0; s < BIG_SEGS; s++) {
        u32 seg_id = 0;

        CHECK(ft_index_add(ix, 0, &g_big_docs[s * BIG_PER], BIG_PER, &seg_id) == OK);
    }

    /* ---- 建出来的段：字典必须跨页（否则这个用例测不到 bug） ---- */
    CHECK(ft_index_seg_at(ix, 0, &seg) == OK);
    if (seg == NULL) {
        ft_index_close(ix);
        pager_close(pg);
        return;
    }
    CHECK(seg->dict_count >= 3u);
    CHECK(ft_segment_nterms(seg) == BIG_PER + 1u);
    check_dict_pages(pg, seg, "建段后");

    /* ---- 词表的首 / 中 / 尾都要查得到（它们落在不同的字典页上） ---- */
    CHECK(ft_segment_lookup(seg, "common", 6, &df, &ptr) == OK);
    CHECK(ft_segment_lookup(seg, "t0000", 5, &df, &ptr) == OK);
    CHECK(ft_segment_lookup(seg, "t0300", 5, &df, &ptr) == OK);
    CHECK(ft_segment_lookup(seg, "t0599", 5, &df, &ptr) == OK);
    /* 查不到的词是 E_NOTFOUND（"词不在这个段里"是正常结果，不是"段坏了"） */
    CHECK(ft_segment_lookup(seg, "nosuchword", 10, &df, &ptr) == E_NOTFOUND);

    /* ---- 检索与线性扫描逐位一致 ---- */
    {
        FtToken qt[8];
        u32 nq = tokenize_query(q, qt, 8);

        CHECK(nq == 4);
        CHECK(ft_index_search(ix, q, qlen, 32, 64, idx_hits, &n_idx) == OK);
        CHECK(ft_score_linear_bm25(g_big_docs, BIG_TOTAL, qt, nq, 32, 64,
                                   lin_hits, &n_lin) == OK);
        cmp_hits("字典跨页 BM25", idx_hits, n_idx, lin_hits, n_lin);

        n_idx = 0;
        n_lin = 0;
        CHECK(ft_index_search_tfidf(ix, q, qlen, 32, 64, idx_hits, &n_idx) == OK);
        CHECK(ft_score_linear_tfidf(g_big_docs, BIG_TOTAL, qt, nq, 32, 64,
                                    lin_hits, &n_lin) == OK);
        cmp_hits("字典跨页 TF-IDF", idx_hits, n_idx, lin_hits, n_lin);
    }

    /* ---- 合并：合并写出来的段，字典页也必须连号、词也不能丢 ---- */
    CHECK(ft_index_search(ix, q, qlen, 32, 64, before, &n_before) == OK);
    CHECK(ft_index_merge(ix, 0, &merged) == OK);
    CHECK(merged >= 2u);
    CHECK(ft_index_search(ix, q, qlen, 32, 64, after, &n_after) == OK);
    cmp_hits("字典跨页 合并前后", before, n_before, after, n_after);

    {
        const FtSegment *mseg = NULL;

        CHECK(ft_index_seg_at(ix, 0, &mseg) == OK);
        if (mseg != NULL) {
            CHECK(mseg->dict_count >= 3u);
            CHECK(ft_segment_nterms(mseg) == BIG_PER + 1u);
            check_dict_pages(pg, mseg, "合并后");

            /* 每一个词都还在（静默丢词是这里最该防的） */
            for (i = 0; i < BIG_PER; i++) {
                char term[8];

                snprintf(term, sizeof(term), "t%04u", i);
                if (!term_present(mseg, term, 5)) {
                    printf("    FAIL 合并后词 %s 丢了\n", term);
                    failures++;
                    break;
                }
            }
            CHECK(term_present(mseg, "common", 6));
        }
    }

    ft_index_close(ix);
    pager_close(pg);
}

int main(void)
{
    printf("test_ft_index:\n");

    build_corpus();
    build_big_corpus();

    test_a1_multisegment();
    test_a3_delete();
    test_a6_merge_consistency();
    test_dict_multipage();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  4 项验收全部通过\n");
    return 0;
}
