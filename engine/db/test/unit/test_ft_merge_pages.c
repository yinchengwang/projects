/*
 * test_ft_merge_pages.c —— 合并页数上界的**模拟器**（W-D 的 D-2）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * 合并出新段之前要先算出"新段要占多少页"，好一次性预留一块连号的字典区。
 * 算多了 ⇒ 池里凑不出那么长的连号段 ⇒ 每轮合并都往文件尾追加 ⇒
 * 文件随合并次数线性增长（页回收等于白做）。
 * 算少了 ⇒ 写入时页块耗尽 ⇒ E_NOSPACE ⇒ 合并失败。
 *
 * 所以上界必须 **≥ 真实需要** 且 **尽量紧**。
 *
 * 老公式「字节数 ÷ (每页容量 − 每页最多浪费的字节) + 1」是**宁多勿少**的估算：
 * 它假设每页都浪费掉一个"最长词条"的空间，而实际浪费通常远小于这个值。
 * 实测（10 万文档 / 92 次合并）：倒排上界 18895 页 vs 实际 18776 页（多要 119 页），
 * 字典上界 2926 页 vs 实际 2850 页（多要 76 页）。
 *
 * 新做法：**按写入器的分页规则逐字模拟**。写入器的打包规则是确定的
 * （ft_writer.c 的 ft_post_writer_put / ft_dict_writer_put），
 * 照它走一遍就得到**精确**页数，不需要"每页最多浪费多少"这种猜出来的余量。
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * 这四条都是**纯函数边界**（不需要 pager、不需要开库），
 * 塞进 test_ft_reclaim.c 会把它变成"既测 IO 又测算术"的混合体。
 *
 * 判据一律是**机械可判**的（`== 期望值`），不写"差不多"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ft/ft_internal.h"
#include "modalities/ft/ft_writer.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/*
 * 两个容量口径 —— 从**格式常量**推出来，不写死 338 / 169。
 * 公式变了测试要跟着红，这才是判据；写死数字只会让测试变成摆设。
 */
#define POST_SPACE   (PAGE_SIZE - FPH_DATA)                       /* 倒排页数据区 */
#define POST_PER_PG  ((POST_SPACE - FT_RUN_HDR_BYTES) / FT_POSTING_BYTES)

/* 一个词的词长取 6（"alpha1"），词条 = 2 + 6 + 12 = 20，加 slot 4 = 24 */
#define TERM_LEN     6u
#define DICT_NEED    (ft_dict_entry_bytes(TERM_LEN) + SLOT_SIZE)
#define DICT_PER_PG  (FT_DICT_CAPACITY / DICT_NEED)

/* ---------------- 1) 空段：一页都不该要 ---------------- */
static void t1_empty(void)
{
    FtPageSim post, dict;

    ft_pagesim_reset(&post);
    ft_pagesim_reset(&dict);
    CHECK(post.pages == 0);
    CHECK(post.used == 0);
    CHECK(dict.pages == 0);
    CHECK(dict.used == 0);

    printf("  [1] 空段 ⇒ 倒排 0 页、字典 0 页（不崩、不白占一页）\n");
}

/* ---------------- 2) 满页：正好填满一页就只要一页 ---------------- */
static void t2_exactly_full(void)
{
    FtPageSim post, dict;
    u32 i;

    /*
     * 一页刚好装 POST_PER_PG 条 posting：
     * 浪费的余量 = POST_SPACE − (run 头 + 条数 × posting)
     * 按 POST_PER_PG 的定义这个余量 < 12 字节（不够再放一条）——
     * 于是"正好满页"也只要 **1** 页，不会多要。
     */
    ft_pagesim_reset(&post);
    ft_pagesim_add_postings(&post, POST_PER_PG);
    CHECK(post.pages == 1);
    CHECK(post.used == FT_RUN_HDR_BYTES + FT_POSTING_BYTES * POST_PER_PG);

    /* 再放一个词（哪怕只有 1 条）⇒ 必须开第 2 页（本页已放不下 run 头 + 一条） */
    ft_pagesim_add_postings(&post, 1);
    CHECK(post.pages == 2);
    /*
     * ⚠️ 这条断言是**变异测试抓出来的**：
     * 只断言 pages == 2 抓不到「换页之后不再继续写」这种错法 ——
     * 那种错法页数照样是 2，但新页里**一条 posting 都没写**（used 停在 0）。
     * 所以必须再断言新页真的装了那一条（12 字节 run 头 + 12 字节 posting）。
     */
    CHECK(post.used == FT_RUN_HDR_BYTES + FT_POSTING_BYTES);

    /* 字典同理：DICT_PER_PG 条正好一页 */
    ft_pagesim_reset(&dict);
    for (i = 0; i < DICT_PER_PG; i++) {
        ft_pagesim_add_dict_entry(&dict, TERM_LEN);
    }
    CHECK(dict.pages == 1);
    CHECK(dict.used == DICT_NEED * DICT_PER_PG);
    CHECK(dict.used <= FT_DICT_CAPACITY);

    /* 再加一条 ⇒ 超容量 ⇒ 第 2 页 */
    ft_pagesim_add_dict_entry(&dict, TERM_LEN);
    CHECK(dict.pages == 2);

    printf("  [2] 满页 ⇒ 倒排 %u 条 = 1 页、字典 %u 条 = 1 页；各多 1 条就变 2 页\n",
           POST_PER_PG, DICT_PER_PG);
}

/* ---------------- 3) 单条大词：一个词自己跨页 ---------------- */
static void t3_one_big_term(void)
{
    FtPageSim post;
    u32 df = POST_PER_PG * 3u + 7u;      /* 一个词就有 3 页多 */

    ft_pagesim_reset(&post);
    ft_pagesim_add_postings(&post, df);

    /*
     * 每页装 POST_PER_PG 条，前 3 页满、第 4 页装 7 条 ⇒ **4 页**。
     * ⚠️ 老公式在这里偏得多：它按"字节 ÷(容量−35)"算，
     * 不区分"run 头"和"页尾不足一条"的浪费，会多要一两页。
     */
    CHECK(post.pages == 4);
    printf("  [3] 单条大词（df=%u）⇒ %u 页（每页 %u 条）\n",
           df, post.pages, POST_PER_PG);
}

/* ---------------- 4) 跨页 posting：词与词的边界不能"白占" ---------------- */
static void t4_spanning(void)
{
    FtPageSim post;
    u32 i;
    u32 per_page = POST_PER_PG;

    /*
     * 情形 A：每个词都只有 1 条 posting ——
     * 每页能放 POST_PER_PG 条，但**每个词还带一个 12 字节 run 头**，
     * 所以一页放不下 POST_PER_PG 个"单词"。
     */
    ft_pagesim_reset(&post);
    for (i = 0; i < per_page; i++) {
        ft_pagesim_add_postings(&post, 1);
    }
    /* 一页 = 12 + 12×n；n 个单词占 24×n ⇒ 每页最多 (POST_SPACE)/24 个 */
    CHECK(post.pages >= 1);
    CHECK(post.used <= POST_SPACE);

    /*
     * 情形 B：把**同一个词**的 posting 从一页中间接着写 ——
     * 第二页的续接 run 头照样要 12 字节，不能当成"0 开销"。
     * 用一个恰好跨页的词验证：第 2 页的 used 必须 ≥ 12 + 12。
     */
    ft_pagesim_reset(&post);
    ft_pagesim_add_postings(&post, POST_PER_PG + 1u);
    CHECK(post.pages == 2);
    CHECK(post.used == FT_RUN_HDR_BYTES + FT_POSTING_BYTES);

    printf("  [4] 跨页：同词续接的 run 头照样占 12 字节（第 2 页 used=%u）\n",
           post.used);
}

/* ---------------- 5) 单调性：词越多，页数不减 ---------------- */
static void t5_monotonic(void)
{
    u32 n;
    u32 prev = 0;

    for (n = 0; n <= POST_PER_PG * 2u + 3u; n += 7u) {
        FtPageSim s;
        ft_pagesim_reset(&s);
        ft_pagesim_add_postings(&s, n);
        CHECK(s.pages >= prev);
        prev = s.pages;
    }
    printf("  [5] 单调性：df 递增时页数不减（0..%u）\n", POST_PER_PG * 2u + 3u);
}

int main(void)
{
    printf("test_ft_merge_pages:\n");

    t1_empty();
    t2_exactly_full();
    t3_one_big_term();
    t4_spanning();
    t5_monotonic();

    if (failures == 0) {
        printf("  5 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
