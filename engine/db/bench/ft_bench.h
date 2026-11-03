/*
 * ft_bench.h —— 全文检索基准（A4 / A5）的公共小工具
 *
 * 基准分三个源文件（ft_bench.c 是入口，ft_bench_a4.c 跑 A4，ft_bench_a5.c 跑 A5），
 * 公共的东西（计时、中位数）放这里 —— 写成 static inline，
 * 省得再开一个 .c 文件（基准文件越散越难读）。
 *
 * ==================== 口径说明（两个基准共用） ====================
 *
 * 1) **检索页读**：`ft_index_page_reads()` 是索引级**累计**（坑 M4-16），
 *    量"一次检索读了几页"必须取**检索前后的差值**。
 *
 * 2) **合并页读**：不在那个计数器里（它只统计检索），
 *    现在有两个口径 ——
 *    - 结构口径：读 = 参与合并的旧段页数（假设每页只读一遍），
 *                写 = 新段的页数；
 *    - 实测取页次数：`ft_index_merge_page_reads()`，合并路径真正取了几次页。
 *    两者差得很远（倒排按 term 分散，合并时每个 term 单独取页），
 *    真实的磁盘 IO 落在两者之间（取决于缓冲池命中率）。
 */

#ifndef DB_BENCH_FT_BENCH_H
#define DB_BENCH_FT_BENCH_H

#include <stdlib.h>
#include <time.h>

#include "common/types.h"

#ifdef __cplusplus
extern "C" {
#endif

/* 单调时钟（毫秒） */
static inline double ft_bench_now_ms(void)
{
    struct timespec ts;

    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static int ft_bench_cmp_double(const void *a, const void *b)
{
    double x = *(const double *)a;
    double y = *(const double *)b;

    if (x < y) return -1;
    if (x > y) return 1;
    return 0;
}

/* 中位数（就地排序后取中间那个；偶数个取靠前的） */
static inline double ft_bench_median(double *v, u32 n)
{
    if (n == 0) {
        return 0.0;
    }
    qsort(v, (size_t)n, sizeof(double), ft_bench_cmp_double);
    return v[n / 2u];
}

/* A4（合并代价）在 ft_bench_a4.c 里 */
int ft_bench_run_a4(u32 n_docs, u32 batch);

/* A5（单 term 检索的页读）在另一个源文件里 */
int ft_bench_run_a5(void);

#ifdef __cplusplus
}
#endif

#endif /* DB_BENCH_FT_BENCH_H */
