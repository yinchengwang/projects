/*
 * tokenizer.h —— 全文检索模态的分词器（v1.3 步骤 1）
 *
 * 本版只做**英文**分词：小写化 + 按非字母数字切分。
 * 中文分词、FST 前缀压缩、位置索引都不在 v1.3 步骤 1 的范围内。
 *
 * 【分词规则（钉死）】
 *
 *   1. 一串连续的 [A-Za-z0-9] 是一个词（token）。
 *   2. 词内的小写化：'A'..'Z' → 'a'..'z'（只处理 ASCII）。
 *   3. 数字算词的一部分："abc123" 是一个词；"3.14" 切成 "3" 和 "14"。
 *   4. 非 [A-Za-z0-9] 的字节一律是分隔符（空格、标点、换行……）。
 *   5. 非 ASCII 字节（>= 0x80，UTF-8 的中文等）**当作分隔符**（坑 M4-6）：
 *      本版不做中文分词，把 UTF-8 序列整体当一个词只会得到一堆乱码 term。
 *   6. 超长词（超过 FT_MAX_TOKEN_LEN-1 字节）**截断**保留前段，剩余部分丢弃，
 *      且**不**在截断处再切一刀（坑 M4-7）。
 *
 * 【为什么要钉这些】
 *
 * 分词规则是**索引格式的一部分**：同一个词在写入端和查询端必须切成同一个样子，
 * 否则写进去的 term 和查出来的 term 对不上 —— 检索结果永远是空的，
 * 而且不报错。所以这里的每一条规则都在 test_ft_tokenizer.c 里有对应用例。
 */

#ifndef DB_MODALITIES_FT_TOKENIZER_H
#define DB_MODALITIES_FT_TOKENIZER_H

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* text[] 的字节容量（含结尾的 '\0'）。 */
#define FT_MAX_TOKEN_LEN 64

/*
 * 一个词。
 *
 * text 一定是**以 '\0' 结尾**的 C 字符串（当 len < FT_MAX_TOKEN_LEN 时；
 * 词内容最长 FT_MAX_TOKEN_LEN-1 = 63 字节）。长度以 len 为准，
 * 不要用 strlen 反推 —— 词里不可能有 '\0'，两者等价，但 len 是权威值。
 */
typedef struct {
    u16 len;                        /* 词的字节长度（不含 '\0'） */
    char text[FT_MAX_TOKEN_LEN];    /* 小写化后的词，'\0' 结尾 */
} FtToken;

/*
 * 把 text 切成词，写进 out。
 *
 * 返回值：
 *   OK        —— 成功，*n_out = 产出的词数（<= max_out）。
 *   E_NOSPACE —— 词数超过 max_out：*n_out = **需要的**词数，
 *                此时 out 里**一个词都没写**（不留半截结果，坑 M4-8）。
 *   E_INVAL   —— 参数非法（n_out 为 NULL；text 为 NULL 但 text_len 非 0；
 *                out 为 NULL 但 max_out 非 0）。
 *
 * 另有一条**只数不切**的用法：out = NULL 且 max_out = 0 时，
 * 只统计词数并写进 *n_out（词数 > 0 时返回 E_NOSPACE）。
 * 这是"先问要多大、再给多大"的省事写法。
 */
int ft_tokenize(const char *text, u32 text_len, FtToken *out, u32 max_out, u32 *n_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_FT_TOKENIZER_H */
