/*
 * test_stmt_alloc.c —— INSERT 值矩阵（Stmt.vals）的分配/释放账目
 *
 * ==================== 这个测试解决什么问题 ====================
 *
 * 单条 INSERT 的行数从"定长 16"改成了"按需增长"（见 parser.h 的
 * Stmt.vals 注释），于是多了三类资源：
 *
 *   vals        行指针数组本身
 *   vals[r]     每一行的值槽（calloc(SQL_MAX_COLS)）
 *   vals[r][c]  行内每个 Expr（p_expr_new）
 *
 * 这三层都必须由 free_stmt 收干净。**泄漏是断言测不出来的**：
 * 少 free 一层，所有 SQL 语义断言照样全绿，只有内存慢慢涨。
 * 我把三个"漏 free"的变异分别注入过，test_query 全部存活；
 * 换成本文件的分配器计数后才全部变红。
 *
 * ==================== 为什么用链接器 --wrap ====================
 *
 * 不改生产代码、不加全局钩子：链接时把 malloc/calloc/realloc/free
 * 重定向到本文件的计数版本，__real_* 转发给真正的 libc 实现。
 * 因此本文件需要 run.sh 为它单独加链接参数（见 run.sh 的
 * test_stmt_alloc 分支）。
 *
 * ==================== 三条场景 ====================
 *
 *   1. 成功解析大批量 → free_stmt 之后净分配回到 0
 *   2. 解析中途失败（各行列数不一致）→ 半成品也要清干净
 *   3. 列数超限失败 → 同上，且这一条会**放弃整行**
 *
 * 失败路径是最容易漏的：那时 nrows 还没 ++，若 free_stmt 按 nrows
 * 遍历就会漏掉当前这一行已解析出的 Expr（实测漏 20 / 85 处）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "query/parser.h"

/* ---------------- 分配器计数（--wrap 重定向到这些） ---------------- */

static long g_live = 0;       /* 当前净分配块数 */
static long g_peak = 0;

void *__real_malloc(size_t n);
void *__real_calloc(size_t n, size_t s);
void *__real_realloc(void *p, size_t n);
void  __real_free(void *p);

void *__wrap_malloc(size_t n)
{
    void *p = __real_malloc(n);
    if (p && ++g_live > g_peak)
        g_peak = g_live;
    return p;
}

void *__wrap_calloc(size_t n, size_t s)
{
    void *p = __real_calloc(n, s);
    if (p && ++g_live > g_peak)
        g_peak = g_live;
    return p;
}

/*
 * realloc 的账：非 NULL→NULL 不改变活跃块数（原地或搬家都还是 1 块）；
 * NULL→非 NULL 是新增 1 块；任何失败都返回 NULL 且不动旧块。
 */
void *__wrap_realloc(void *old, size_t n)
{
    void *p = __real_realloc(old, n);
    if (p && !old && ++g_live > g_peak)
        g_peak = g_live;
    return p;
}

void __wrap_free(void *p)
{
    if (p)
        g_live--;
    __real_free(p);
}

/* ---------------- 断言框架 ---------------- */

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 跑一次解析+释放，返回净泄漏块数（应为 0） */
static long roundtrip(const char *sql, int expect_rows)
{
    Stmt st;
    char err[256];
    long before = g_live;

    int rc = parse_sql(sql, &st, err, (int)sizeof(err));
    if (rc != OK) {
        printf("    FAIL 解析应当成功: %s → %s\n", sql, err);
        failures++;
        return 0;               /* parse_sql 内部已 free_stmt(out) */
    }
    if (expect_rows >= 0 && st.nrows != expect_rows) {
        printf("    FAIL nrows=%d，期望 %d\n", st.nrows, expect_rows);
        failures++;
    }
    free_stmt(&st);
    return g_live - before;
}

/* 跑一次"注定失败"的解析，返回净泄漏块数 */
static long roundtrip_fail(const char *sql, const char *why)
{
    Stmt st;
    char err[256];
    long before = g_live;

    int rc = parse_sql(sql, &st, err, (int)sizeof(err));
    if (rc == OK) {
        printf("    FAIL 解析应当失败（%s）：%s\n", why, sql);
        failures++;
        free_stmt(&st);
        return 0;
    }
    /* parse_sql 在失败分支里已经 free_stmt(out)，这里不再重复释放 */
    return g_live - before;
}

int main(void)
{
    /*
     * 基线不是 0：stdio 在 main 之前就分配过缓冲（实测 2 块）。
     * 所以所有账目都记成"相对基线"的增量，最后比对回到基线。
     */
    long base = g_live;

    printf("test_stmt_alloc:\n");
    printf("  （基线活跃块 %ld —— stdio 等在 main 之前分配的）\n", base);

    /*
     * ---- 1) 成功路径：18 行批量（> 原来的 16 行上限）----
     *
     * 18 行会触发一次 vals 扩容（初值 SQL_ROWS_INIT=16 → 32）。
     * 重复多轮，确保每轮都清干净（漏一次就会累计）。
     */
    {
        const char *sql =
            "INSERT INTO t VALUES (1,2),(3,4),(5,6),(7,8),(9,10),(11,12),"
            "(13,14),(15,16),(17,18),(19,20),(21,22),(23,24),(25,26),(27,28),"
            "(29,30),(31,32),(33,34),(35,36);";
        for (int i = 0; i < 5; i++)
            CHECK(roundtrip(sql, 18) == 0);
        printf("  [1] 成功路径 18 行 × 5 轮：无泄漏\n");
    }

    /*
     * ---- 2) 成功路径：单行最小批量（边界）----
     */
    {
        for (int i = 0; i < 3; i++)
            CHECK(roundtrip("INSERT INTO t VALUES (1);", 1) == 0);
        printf("  [2] 单行批量 × 3 轮：无泄漏\n");
    }

    /*
     * ---- 3) 失败路径：第三行值个数不一致 ----
     *
     * 前两行各 2 个值已经解析并放进 vals[0]、vals[1]，
     * 第三行解析到第 3 个值时才发现不一致 → 放弃。
     * 此时 nrows == 2，但 vals[2] 已经 calloc 过、里面也有 Expr。
     * free_stmt 若按 nrows 遍历，vals[2] 整行都会漏（实测漏 20 处/5 轮）。
     */
    {
        for (int i = 0; i < 5; i++)
            CHECK(roundtrip_fail("INSERT INTO t VALUES (1,2),(3,4),(5,6,7);",
                                 "各行列数不一致") == 0);
        printf("  [3] 失败路径（行列数不一致）× 5 轮：无泄漏\n");
    }

    /*
     * ---- 4) 失败路径：列数超限 ----
     *
     * 17 列 > SQL_MAX_COLS。解析到第 17 个值时报错，
     * 该行前 16 个槽已经填了 Expr。free_stmt 必须扫**全宽**
     * （SQL_MAX_COLS）而不是 ncols —— 此时 ncols 还是 0（首行失败），
     * 按 ncols 扫会一个都不 free（实测漏 80 处/5 轮）。
     */
    {
        for (int i = 0; i < 5; i++)
            CHECK(roundtrip_fail(
                "INSERT INTO t VALUES (1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17);",
                "列数超限") == 0);
        printf("  [4] 失败路径（17 列超限）× 5 轮：无泄漏\n");
    }

    /*
     * ---- 5) 失败路径：VALUES 后缺左括号 ----
     *
     * 连第一行都没进去 —— vals 可能已扩容但一次行都没分配。
     */
    {
        for (int i = 0; i < 3; i++)
            CHECK(roundtrip_fail("INSERT INTO t VALUES 1;", "缺左括号") == 0);
        printf("  [5] 失败路径（缺左括号）× 3 轮：无泄漏\n");
    }

    /*
     * ---- 6) 全流程结束后回到基线 ----
     *
     * 上面每一项都比了 before/after，这里再兜一次底：
     * 若某处有跨轮次的累积，逐项相等也可能因为错位而看起来正常。
     *
     * 基线不是 0 —— stdio 在 main 之前就分配过缓冲（实测 2 块），
     * 所以比的是"是否回到进入 main 时的活跃块数"。
     */
    CHECK(g_live == base);
    if (g_live != base)
        printf("    → 结束后净增 %ld 个块未释放\n", g_live - base);

    if (failures == 0) {
        printf("  6 项验收全部通过（峰值 %ld 块）", g_peak);
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
