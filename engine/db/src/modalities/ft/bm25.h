/*
 * bm25.h —— 全文检索模态的评分器 + 线性扫描基准实现（v1.3 步骤 1）
 *
 * 本文件提供三样东西：
 *
 *   1. BM25 评分（Lucene 式 idf，k1=1.2、b=0.75）
 *   2. TF-IDF 对照评分（教科书式 idf，**不做**文档长度归一）
 *   3. 线性扫描评分（纯内存、无索引）—— A1 的基准实现
 *
 * 【为什么要有一个"教科书式"的 TF-IDF】
 *
 * 教科书式 idf 是 idf(t) = ln((N - df + 0.5) / (df + 0.5))，
 * 它在 df 大的时候**会取到负值甚至负无穷** —— 因为 ln 的参数可能 <= 0。
 * Lucene 式 idf = ln(1 + (N - df + 0.5) / (df + 0.5)) 修掉了这一点：
 * 加 1 之后参数恒 > 0，idf 恒为正。
 *
 * 所以这里保留 TF-IDF 作为**对照组**，用来说明两件事：
 *
 *   - Lucene 式 idf 为什么是那样写的（它不是"随便加个 1"）；
 *   - BM25 和 TF-IDF 的排序差异到底来自哪里（长度归一）。
 *
 * TF-IDF 版**故意不做**文档长度归一 —— 教科书就是那样。
 * 于是"同样命中的两篇文档，短的那篇分数更高"，
 * 这正是 BM25 的长度归一要修掉的问题（见 PLAN-v1.3 §1.4）。
 */

#ifndef DB_MODALITIES_FT_BM25_H
#define DB_MODALITIES_FT_BM25_H

#include "common/types.h"
#include "modalities/ft/tokenizer.h"

#ifdef __cplusplus
extern "C" {
#endif

/* BM25 的两个参数（钉死，不做成可配置 —— v1.3 不需要调参） */
#define FT_BM25_K1 1.2
#define FT_BM25_B  0.75

/*
 * 语料统计量。
 *
 * n_docs —— 语料里的文档数 N。
 * avgdl  —— 平均文档长度（**词数**，不是字节数）。
 *
 * 只带这两个量是刻意的：df / tf / 文档长度都是"某一个词的某一次查询"才有的量，
 * 它们由调用方（线性扫描或将来的倒排索引）按查询现算。
 */
typedef struct {
    u32   n_docs;
    double avgdl;
} FtCorpus;

/*
 * BM25 单个词的贡献：
 *
 *   idf(t) = ln(1 + (N - df + 0.5) / (df + 0.5))          ← Lucene 式
 *   score  = idf(t) * tf * (k1 + 1)
 *                    / (tf + k1 * (1 - b + b * dl / avgdl))
 *
 * df —— 该词在语料里出现的**文档数**。
 * tf —— 该词在当前文档里出现的**次数**。
 * dl —— 当前文档的长度（词数）。
 */
double ft_bm25_score(const FtCorpus *cs, u32 df, u32 tf, u32 doc_len);

/*
 * TF-IDF 对照：score = tf * idf(t)，idf(t) = ln((N - df + 0.5) / (df + 0.5))。
 *
 * 教科书式 idf **不做**文档长度归一。
 */
double ft_tfidf_score(const FtCorpus *cs, u32 df, u32 tf);

/*
 * 线性扫描的输入：一篇文档的原文。
 */
typedef struct {
    u32         doc_id;    /* 调用方给的文档标识，原样带回结果 */
    const char *text;      /* 原文 */
    u32         text_len;  /* 原文字节数 */
} FtDocText;

/*
 * 线性扫描的输出：一个命中的文档。
 */
typedef struct {
    u32   doc_id;
    double score;
} FtHit;

/*
 * 线性扫描评分（纯内存、无索引 —— A1 的基准实现）。
 *
 * 做法：把每篇文档都切一遍词，统计 query 词在其中的 tf，
 * 语料级 df 和 avgdl 也是现算的，然后按 BM25 / TF-IDF 公式打分。
 *
 * 语义（钉死，后续的倒排索引实现必须和它完全一致）：
 *
 *   - 只有**至少命中一个 query 词**的文档才进结果（不按分数正负过滤：
 *     TF-IDF 的教科书式 idf 可能为负，命中了就是命中了）。
 *   - query 里的重复词**不去重**，各贡献一次（坑 M4-12）。
 *   - 排序：分数降序；分数相同按 doc_id 升序（坑 M4-11）。
 *   - 返回最多 top_k 条，且**绝不写出超过 max_out 条**。
 *
 * 返回值：
 *   OK        —— 成功，*n_out = 写进 out 的条数（<= top_k，且 <= max_out）。
 *   E_NOSPACE —— top_k > max_out（结果缓冲区装不下 top_k 条）；
 *                或 top_k 非 0 但 out 为 NULL（没地方放结果）。
 *   E_INVAL   —— 参数非法（n_out 为 NULL，或 out 为 NULL 而 top_k 为 0）。
 *
 * 【out 的容量：由 max_out 显式给出】
 *
 * out 的容量不再靠"约定 out 的容量 >= top_k"，而是由 max_out 这个
 * 参数**显式给出**。top_k > max_out 直接报 E_NOSPACE，**不做静默截断** ——
 * 截断出来的结果"看起来对其实错"（调用方以为拿到了 top_k 条，
 * 实际只有 max_out 条，且少的那些往往是排名靠后的），比报错坏得多。
 * 这条方针和 config.h 里 MAX_GROUPS 那条"不能静默截断"是同一条。
 *
 * 这个参数是 v1.3 步骤 2 与倒排检索接口（ft_segment_search）对齐时补上的：
 * 那边的签名本来就带"显式容量"，两边形状不一致会让接线的人一直踩坑。
 */
int ft_score_linear_bm25(const FtDocText *docs, u32 n_docs, const FtToken *query, u32 n_query,
                         u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

int ft_score_linear_tfidf(const FtDocText *docs, u32 n_docs, const FtToken *query, u32 n_query,
                          u32 top_k, u32 max_out, FtHit *out, u32 *n_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_BM25_H */
