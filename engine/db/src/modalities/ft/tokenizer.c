/*
 * tokenizer.c —— 全文检索模态的分词器（v1.3 步骤 1）
 *
 * 规则见 tokenizer.h 的头注释。这里只实现，不解释。
 */

#include "modalities/ft/tokenizer.h"

#include <string.h>

#include "common/error.h"

/*
 * 词内容的最大字节数。
 *
 * text[] 的容量是 FT_MAX_TOKEN_LEN，但里面还要放一个结尾的 '\0'，
 * 所以词本身最长 FT_MAX_TOKEN_LEN-1 = 63 字节。
 *
 * 【为什么必须留 '\0'】
 *
 * FtToken 是会被 memcpy 进倒排表的定长结构，下游很可能直接
 * printf("%s", tok.text)。如果 text 不保证以 '\0' 结尾，
 * 一个 63 字节的词会把后面的内存一起打印出来 —— 越界读。
 * 所以约定：text 永远以 '\0' 结尾，超长词截到 63 字节。
 */
#define FT_TOKEN_TEXT_MAX (FT_MAX_TOKEN_LEN - 1)

/*
 * 是否是 ASCII 字母或数字。
 *
 * 坑 M4-6：>= 0x80 的字节（UTF-8 的中文等）**不算**词字符，
 * 一律当分隔符。本版不做中文分词 —— 把一串 UTF-8 当成一个词，
 * 得到的既不是词也不是字，只是乱码。
 */
static int ft_is_alnum(unsigned char c)
{
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

/*
 * 扫描一遍文本。
 *
 * out == NULL：只数词数（*n_total = 总词数）。
 * out != NULL：写出前 max_out 个词（不足则全写），*n_total 仍然是总词数。
 *
 * 同一个扫描器跑两遍是故意的：
 * 第一遍数词数，够就第二遍写，**不够就一个都不写**（坑 M4-8）。
 * 半截结果比没有结果更难查 —— 调用方忘了看返回值，
 * 半截词表会被当成完整词表用下去。
 */
static void ft_scan_pass(const char *text, u32 text_len, FtToken *out, u32 max_out, u32 *n_total)
{
    u32 i = 0;
    u32 produced = 0;
    u32 total = 0;

    while (i < text_len) {
        unsigned char c = (unsigned char)text[i];
        u32 start;
        u32 len;
        u32 k;

        if (!ft_is_alnum(c)) {
            i++;
            continue;
        }

        /* 一串连续的字母数字 = 一个词 */
        start = i;
        while (i < text_len && ft_is_alnum((unsigned char)text[i])) {
            i++;
        }

        /*
         * 坑 M4-7：超长词截断。
         *
         * 策略：**保留前 FT_TOKEN_TEXT_MAX 字节，剩余部分整段丢弃**，
         * 不在截断处再切一刀。
         *
         * 为什么不在截断处再切：
         *   "abc...(100 字节)" 如果切成 63 + 37 两个词，
         *   就凭空造出了一个词边界 —— 37 字节那段根本不是词，
         *   拿它当 term 去匹配，等于允许"命中任意后缀"。
         *   整段丢弃的话，这个串只对应一个 term（它的前 63 字节），
         *   语义上还是"一个超长的怪词"，可控。
         *
         * 已知代价：两个不同的超长词可能截成同一个 term（词形碰撞）。
         * 本版接受这个代价 —— 真出现 base64 长串那种场景，
         * 正确做法是给超长词加哈希后缀（留到有真实需求时再做）。
         */
        len = i - start;
        if (len > FT_TOKEN_TEXT_MAX) {
            len = FT_TOKEN_TEXT_MAX;
        }
        total++;

        if (out != NULL && produced < max_out) {
            FtToken *t = &out[produced];

            for (k = 0; k < len; k++) {
                unsigned char ch = (unsigned char)text[start + k];

                if (ch >= 'A' && ch <= 'Z') {
                    ch = (unsigned char)(ch - 'A' + 'a');
                }
                t->text[k] = (char)ch;
            }
            t->text[len] = '\0';
            t->len = (u16)len;
            produced++;
        }
    }

    *n_total = total;
}

int ft_tokenize(const char *text, u32 text_len, FtToken *out, u32 max_out, u32 *n_out)
{
    u32 total = 0;

    if (n_out == NULL) {
        return E_INVAL;
    }
    if (text == NULL && text_len != 0) {
        return E_INVAL;
    }
    if (out == NULL && max_out != 0) {
        return E_INVAL;
    }

    *n_out = 0;

    if (text == NULL || text_len == 0) {
        return OK;
    }

    /* 第一遍：只数词数 */
    ft_scan_pass(text, text_len, NULL, 0, &total);

    if (total > max_out) {
        /*
         * 装不下：报告需要的数量，**一个词都不写**。
         * 调用方按 *n_out 重新给足缓冲再来一次即可。
         */
        *n_out = total;
        return E_NOSPACE;
    }

    /* 第二遍：写词（此时一定装得下） */
    ft_scan_pass(text, text_len, out, max_out, &total);
    *n_out = total;

    return OK;
}
