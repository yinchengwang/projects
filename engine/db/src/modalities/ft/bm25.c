/*
 * bm25.c —— 全文检索模态的评分器 + 线性扫描基准实现（v1.3 步骤 1）
 *
 * 公式与语义见 bm25.h 的头注释。这里只实现，不解释。
 */

#include "modalities/ft/bm25.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"

/* ---------------- 单个词的评分 ---------------- */

double ft_bm25_score(const FtCorpus *cs, u32 df, u32 tf, u32 doc_len)
{
    double idf;
    double dl_norm;
    double norm;
    double denom;
    double n;
    double d;

    /* 参数检查必须**在解引用之前** —— 顺序反了就是空指针解引用 */
    if (cs == NULL) {
        return 0.0;
    }
    n = (double)cs->n_docs;
    d = (double)df;

    /*
     * Lucene 式 idf：idf(t) = ln(1 + (N - df + 0.5) / (df + 0.5))。
     *
     * 那个 "+1" 不是随手加的：没有它的时候，
     * df 接近 N 时括号里的东西会 <= 0，ln 直接取不到实数。
     * 加了 1 之后括号里的值恒 > 0，idf 恒为正。
     */
    idf = log(1.0 + (n - d + 0.5) / (d + 0.5));

    /*
     * 坑 M4-9：avgdl 为 0 时的除零防护。
     *
     * avgdl = 0 意味着语料里每篇文档都是空的（一个词都没有）。
     * 这种语料里不会有 tf > 0 的词，分数本来就是 0，
     * 但 dl / avgdl 依然会算出 NaN —— NaN 会污染排序（NaN 的比较全是假）。
     * 所以这里把长度归一的比例定义成 0（= "这篇文档不长不短"），
     * 保证结果是个确定的数。
     */
    dl_norm = (cs->avgdl > 0.0) ? ((double)doc_len / cs->avgdl) : 0.0;

    /* 长度归一：k1 * (1 - b + b * dl / avgdl) */
    norm = FT_BM25_K1 * (1.0 - FT_BM25_B + FT_BM25_B * dl_norm);

    denom = (double)tf + norm;
    if (denom <= 0.0) {
        return 0.0;
    }

    return idf * ((double)tf * (FT_BM25_K1 + 1.0)) / denom;
}

double ft_tfidf_score(const FtCorpus *cs, u32 df, u32 tf)
{
    double arg;
    double idf;
    double n;
    double d;

    /* 参数检查必须**在解引用之前** —— 顺序反了就是空指针解引用 */
    if (cs == NULL) {
        return 0.0;
    }
    n = (double)cs->n_docs;
    d = (double)df;

    /*
     * 教科书式 idf：ln((N - df + 0.5) / (df + 0.5))。
     *
     * 坑 M4-10：ln 的参数可能 <= 0（df 比 N 还大时），
     * 这时 ln 没有定义。这里把这种情形定义为 idf = 0 ——
     * 那个词在这个语料里没有区分度，贡献 0 是合理的。
     *
     * 注意：参数为正时 idf **可以是负数**（df > N/2 时），
     * 这正是教科书式 idf 的毛病，也是 Lucene 式要修掉的东西。
     */
    arg = (n - d + 0.5) / (d + 0.5);
    if (arg <= 0.0) {
        return 0.0;
    }
    idf = log(arg);

    /* 教科书式：score = tf * idf，**不做**文档长度归一 */
    return (double)tf * idf;
}

/* ---------------- 线性扫描 ---------------- */

/*
 * 把一篇文档切成词，词表放在 *buf 里（不够就自动长）。
 *
 * *buf / *cap 是"跨文档复用的缓冲区"：第一篇文档把它撑大，
 * 后面几篇就直接用同一块内存 —— 扫描 1000 篇文档只分配一两次。
 */
static int ft_collect_tokens(const char *text, u32 text_len, FtToken **buf, u32 *cap, u32 *n_out)
{
    u32 need = 0;
    int rc;

    if (*buf == NULL) {
        *cap = 256;
        *buf = (FtToken *)malloc(sizeof(FtToken) * (size_t)*cap);
        if (*buf == NULL) {
            return E_NOSPACE;
        }
    }

    rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    if (rc == E_NOSPACE) {
        /* need = 这篇文档的总词数，按它重新分配再来一次 */
        FtToken *nb = (FtToken *)realloc(*buf, sizeof(FtToken) * (size_t)(need ? need : 1));
        if (nb == NULL) {
            return E_NOSPACE;
        }
        *buf = nb;
        *cap = need;
        rc = ft_tokenize(text, text_len, *buf, *cap, &need);
    }
    if (rc != OK) {
        return rc;
    }

    *n_out = need;
    return OK;
}

/* 两个词是否相同 */
static int ft_token_eq(const FtToken *a, const FtToken *b)
{
    if (a->len != b->len) {
        return 0;
    }
    return memcmp(a->text, b->text, (size_t)a->len) == 0;
}

/*
 * 新命中项是不是比已有项"更好"（分数降序、doc_id 升序）。
 *
 * 坑 M4-11：平局必须按 doc_id 升序，不能靠"先来后到"。
 * 扫描顺序一变结果顺序就变，测试没法复现，缓存也没法命中。
 */
static int ft_hit_better(double score, u32 doc_id, const FtHit *old)
{
    if (score != old->score) {
        return score > old->score;
    }
    return doc_id < old->doc_id;
}

/*
 * 线性扫描的主流程。use_bm25 != 0 走 BM25，否则走 TF-IDF。
 *
 * 两遍：
 *   第一遍：切词，统计每篇文档的长度、每个 query 词的 tf；
 *   第二遍：按公式打分，插入 top-k。
 *
 * 为什么不能一遍搞定：idf 依赖语料级的 df，而 df 要等所有文档都切过才知道。
 */
static int ft_linear_scan(const FtDocText *docs, u32 n_docs, const FtToken *query, u32 n_query,
                           u32 top_k, FtHit *out, u32 *n_out, int use_bm25)
{
    FtToken *buf = NULL;
    u32 buf_cap = 0;
    u32 *tf = NULL;     /* n_docs × n_query：每个 query 词在每篇文档里的次数 */
    u32 *doc_len = NULL;/* n_docs：每篇文档的词数 */
    u32 *df = NULL;     /* n_query：每个 query 词的文档频率 */
    u64 total_len = 0;
    u32 n_hit = 0;
    u32 d;
    u32 q;
    u32 i;
    int rc = OK;
    FtCorpus cs;

    if (n_out == NULL) {
        return E_INVAL;
    }
    if (docs == NULL && n_docs > 0) {
        return E_INVAL;
    }
    if (query == NULL && n_query > 0) {
        return E_INVAL;
    }
    if (out == NULL && top_k > 0) {
        /* 约定 out 的容量 >= top_k；连 out 都没有就无处可写 */
        return E_NOSPACE;
    }

    *n_out = 0;

    if (n_docs == 0 || n_query == 0 || top_k == 0) {
        return OK;
    }

    /* 别让 n_docs * n_query 溢出 */
    if ((size_t)n_docs > ((size_t)-1) / sizeof(u32) / (size_t)n_query) {
        return E_INVAL;
    }

    tf = (u32 *)calloc((size_t)n_docs * (size_t)n_query, sizeof(u32));
    doc_len = (u32 *)calloc((size_t)n_docs, sizeof(u32));
    df = (u32 *)calloc((size_t)n_query, sizeof(u32));
    if (tf == NULL || doc_len == NULL || df == NULL) {
        rc = E_NOSPACE;
        goto done;
    }

    /* 第一遍：切词、统计 tf 和文档长度 */
    for (d = 0; d < n_docs; d++) {
        u32 nt = 0;

        rc = ft_collect_tokens(docs[d].text, docs[d].text_len, &buf, &buf_cap, &nt);
        if (rc != OK) {
            goto done;
        }
        doc_len[d] = nt;
        total_len += (u64)nt;

        for (i = 0; i < nt; i++) {
            for (q = 0; q < n_query; q++) {
                if (ft_token_eq(&buf[i], &query[q])) {
                    /*
                     * 坑 M4-12：query 里的重复词**不去重** ——
                     * 每个位置各记一次 tf，评分时也各贡献一次。
                     *
                     * 这么定是因为线性扫描是**基准**：
                     * 将来倒排索引的实现必须和它逐位一致。
                     * 倒排那边遍历 query 词表、查一次倒排链、加一次分，
                     * 天然就是"不去重"的语义。这里跟着它走。
                     */
                    tf[(size_t)d * (size_t)n_query + (size_t)q]++;
                }
            }
        }
    }

    /* df 是**文档**频率：含有该词的文档数，不是出现总次数 */
    for (d = 0; d < n_docs; d++) {
        for (q = 0; q < n_query; q++) {
            if (tf[(size_t)d * (size_t)n_query + (size_t)q] > 0) {
                df[q]++;
            }
        }
    }

    cs.n_docs = n_docs;
    cs.avgdl = (n_docs > 0) ? ((double)total_len / (double)n_docs) : 0.0;

    /* 第二遍：打分 + 插入 top-k */
    for (d = 0; d < n_docs; d++) {
        double score = 0.0;
        int matched = 0;
        u32 pos;

        for (q = 0; q < n_query; q++) {
            u32 tfq = tf[(size_t)d * (size_t)n_query + (size_t)q];

            if (tfq == 0) {
                continue;
            }
            matched = 1;
            if (use_bm25) {
                score += ft_bm25_score(&cs, df[q], tfq, doc_len[d]);
            } else {
                score += ft_tfidf_score(&cs, df[q], tfq);
            }
        }

        /* 一个 query 词都没命中 → 不进结果 */
        if (!matched) {
            continue;
        }

        /*
         * 插入 top-k：out[0..n_hit-1] 一直保持有序
         * （分数降序、doc_id 升序）。
         */
        pos = n_hit;
        for (i = 0; i < n_hit; i++) {
            if (ft_hit_better(score, docs[d].doc_id, &out[i])) {
                pos = i;
                break;
            }
        }
        if (pos >= top_k) {
            /* 挤不进前 top_k */
            continue;
        }
        if (n_hit < top_k) {
            n_hit++;
        }
        for (i = n_hit - 1; i > pos; i--) {
            out[i] = out[i - 1];
        }
        out[pos].doc_id = docs[d].doc_id;
        out[pos].score = score;
    }

    *n_out = n_hit;

done:
    free(buf);
    free(tf);
    free(doc_len);
    free(df);
    return rc;
}

int ft_score_linear_bm25(const FtDocText *docs, u32 n_docs, const FtToken *query, u32 n_query,
                         u32 top_k, FtHit *out, u32 *n_out)
{
    return ft_linear_scan(docs, n_docs, query, n_query, top_k, out, n_out, 1);
}

int ft_score_linear_tfidf(const FtDocText *docs, u32 n_docs, const FtToken *query, u32 n_query,
                          u32 top_k, FtHit *out, u32 *n_out)
{
    return ft_linear_scan(docs, n_docs, query, n_query, top_k, out, n_out, 0);
}
