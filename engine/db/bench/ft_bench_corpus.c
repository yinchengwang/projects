/*
 * ft_bench_corpus.c —— 全文检索基准的语料生成（见 ft_bench_corpus.h 的说明）
 *
 * 两种语料：
 *
 *   random()    固定词表 + 随机取词 —— df 随语料线性放大（A4 / A5 组二）
 *   fixed_df()  词 j 恰好出现在 df_target 篇文档里 —— df 固定（A5 组一）
 *
 * 【词文本的格式】
 *
 * 统一用 "w%06u"（字母 + 6 位十进制）——
 * 只含字母数字（分词器只把字母数字当词字符）、长度固定（7 字节）、
 * 字典序与编号序一致（"w000123" < "w000124"）。
 * 最后一点有用：**词的字典序 = 编号序**，
 * 合并时多路归并的顺序可以预期，出问题好查。
 */

#include "ft_bench_corpus.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- LCG（可复现） ---------------- */

static u32 g_rng;

static void rng_seed(u32 seed)
{
    g_rng = seed;
}

static u32 rnd(void)
{
    g_rng = g_rng * 1103515245u + 12345u;
    return (g_rng >> 16) & 0x7FFFu;
}

/* ---------------- 词的文本 ---------------- */

const char *ft_bench_term(u32 idx, char *buf, u32 buf_len)
{
    snprintf(buf, (size_t)buf_len, "w%06u", idx);
    return buf;
}

/* ---------------- 语料的骨架 ---------------- */

static int corpus_init(FtBenchCorpus *c, u32 n_docs)
{
    memset(c, 0, sizeof(*c));

    c->off    = (u32 *)calloc(n_docs, sizeof(u32));
    c->len    = (u32 *)calloc(n_docs, sizeof(u32));
    c->doc_id = (u32 *)calloc(n_docs, sizeof(u32));
    if (c->off == NULL || c->len == NULL || c->doc_id == NULL) {
        ft_bench_corpus_free(c);
        return -1;
    }
    c->n_docs = n_docs;
    return 0;
}

void ft_bench_corpus_free(FtBenchCorpus *c)
{
    if (c == NULL) {
        return;
    }
    free(c->text);
    free(c->off);
    free(c->len);
    free(c->doc_id);
    memset(c, 0, sizeof(*c));
}

/* 往文本缓冲尾部追加 n 个字节 */
static int text_reserve(FtBenchCorpus *c, u32 extra)
{
    u32 need = c->text_len + extra;
    char *nb;
    u32 ncap;

    if (need <= c->text_cap) {
        return 0;
    }
    ncap = (c->text_cap == 0) ? 65536u : c->text_cap;
    while (ncap < need) {
        ncap *= 2;
    }
    nb = (char *)realloc(c->text, (size_t)ncap);
    if (nb == NULL) {
        return -1;
    }
    c->text = nb;
    c->text_cap = ncap;
    return 0;
}

/* 追加一个词（后面跟一个空格当分隔符） */
static int text_append_term(FtBenchCorpus *c, u32 idx)
{
    char buf[16];

    ft_bench_term(idx, buf, (u32)sizeof(buf));
    if (text_reserve(c, (u32)strlen(buf) + 1u) != 0) {
        return -1;
    }
    memcpy(c->text + c->text_len, buf, strlen(buf));
    c->text_len += (u32)strlen(buf);
    c->text[c->text_len++] = ' ';
    return 0;
}

/* ---------------- 语料一：固定词表 + 随机取词 ---------------- */

int ft_bench_corpus_random(FtBenchCorpus *c, u32 n_docs, u32 vocab,
                           u32 words_per_doc, u32 seed)
{
    u32 i;

    if (c == NULL || vocab == 0 || words_per_doc == 0) {
        return -1;
    }
    if (corpus_init(c, n_docs) != 0) {
        return -1;
    }
    rng_seed(seed);

    for (i = 0; i < n_docs; i++) {
        u32 j;

        c->off[i] = c->text_len;
        for (j = 0; j < words_per_doc; j++) {
            if (text_append_term(c, rnd() % vocab) != 0) {
                ft_bench_corpus_free(c);
                return -1;
            }
        }
        c->len[i] = c->text_len - c->off[i];
        c->doc_id[i] = i + 1u;      /* 全局唯一、递增 */
    }
    return 0;
}

/* ---------------- 语料二：df 固定 ---------------- */

int ft_bench_corpus_fixed_df(FtBenchCorpus *c, u32 n_docs, u32 df_target)
{
    u32 i;

    if (c == NULL || df_target == 0) {
        return -1;
    }
    if (corpus_init(c, n_docs) != 0) {
        return -1;
    }
    /* 不用随机数：df 要精确，不能有概率成分 */
    rng_seed(0);

    for (i = 0; i < n_docs; i++) {
        u32 j;

        c->off[i] = c->text_len;
        for (j = 0; j < df_target; j++) {
            if (text_append_term(c, i + j) != 0) {
                ft_bench_corpus_free(c);
                return -1;
            }
        }
        c->len[i] = c->text_len - c->off[i];
        c->doc_id[i] = i + 1u;
    }
    return 0;
}

/* ---------------- 取文档 ---------------- */

void ft_bench_corpus_doc(const FtBenchCorpus *c, u32 i, FtDocText *out)
{
    out->doc_id = c->doc_id[i];
    out->text = c->text + c->off[i];
    out->text_len = c->len[i];
}
