/*
 * ft_bench_corpus.h —— 全文检索基准的语料生成（可复现）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * 语料生成是 A4 与 A5 共用的底座，而两边的**要求正好相反**：
 *
 *   A4（合并代价）：语料要**像真的一样** —— 词表固定、词在文档里随机分布；
 *   A5（IO 可证）：语料要**可控** —— 词的 df 必须能算出来，否则
 *                   "页读与文档量无关" 这句话就没法证伪。
 *
 * 放在一个文件里，两边可以各取所需，参数也都是显式的。
 *
 * ==================== 可复现 ====================
 *
 * 随机数用自己写的 LCG，**不用 rand()** ——
 * rand() 的序列是实现定义的，换个 libc 结果就不一样，
 * 那意味着"昨天过的基准今天数字不一样"。LCG 的序列钉死，语料可复现。
 */

#ifndef DB_BENCH_FT_BENCH_CORPUS_H
#define DB_BENCH_FT_BENCH_CORPUS_H

#include "common/types.h"
#include "modalities/ft/bm25.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 一份语料：所有文档的原文连续存放，另有每篇文档的偏移 / 长度 / doc_id */
typedef struct {
    char *text;      /* 所有文档的原文（连续存放） */
    u32   text_len;  /* 已用字节数 */
    u32   text_cap;  /* 缓冲容量 */
    u32  *off;       /* 每篇文档在 text 里的起点 */
    u32  *len;       /* 每篇文档的字节长度 */
    u32  *doc_id;    /* 每篇文档的 id（全局唯一、递增） */
    u32   n_docs;    /* 文档数 */
} FtBenchCorpus;

/* 释放语料（释放失败也不致命 —— 基准进程马上退出） */
void ft_bench_corpus_free(FtBenchCorpus *c);

/*
 * 语料一：**固定词表 + 随机取词**（A4 用）。
 *
 * vocab —— 词表大小（词编号 0..vocab-1）
 * words_per_doc —— 每篇文档多少个词
 *
 * 词在文档里随机出现 → 每个词的 df 随语料规模**线性放大**。
 */
int ft_bench_corpus_random(FtBenchCorpus *c, u32 n_docs, u32 vocab,
                           u32 words_per_doc, u32 seed);

/*
 * 语料二：**df 固定**（A5 组一用）。
 *
 * 第 i 篇文档含词 i, i+1, ..., i+df_target-1（词表大小 = n_docs + df_target - 1）。
 * 于是词 j（j ≥ df_target-1）恰好出现在 df_target 篇文档里 ——
 * **df 固定，与语料规模无关**。
 */
int ft_bench_corpus_fixed_df(FtBenchCorpus *c, u32 n_docs, u32 df_target);

/* 取第 i 篇文档（填进调用方的 FtDocText，语料内部指针，不拷贝） */
void ft_bench_corpus_doc(const FtBenchCorpus *c, u32 i, FtDocText *out);

/* 词编号 → 词文本（"w000123" 这种；写进调用方的 buf） */
const char *ft_bench_term(u32 idx, char *buf, u32 buf_len);

#ifdef __cplusplus
}
#endif

#endif /* DB_BENCH_FT_BENCH_CORPUS_H */
