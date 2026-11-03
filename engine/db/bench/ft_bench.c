/*
 * ft_bench.c —— 全文检索基准的入口
 *
 * ==================== 这个基准要回答两个问题 ====================
 *
 * A4 · **合并的代价是可解释的**（在 ft_bench_a4.c 里）
 *   计划（PLAN-v1.3 §1.5）的判据是"量级论证"：
 *   10 万文档分批插入，合并总 IO 呈 `c·N·log_f N` 量级。
 *   基准量出：批大小 b、合并因子 f、合并轮数、每轮合并的页数、合并总 IO，
 *   再拟合出 c（单位：**页/文档**），并与"分层模型"对照 ——
 *   结论落在"量级对不对"，不是"精确等于"。
 *
 * A5 · **单 term 检索的页读是可证的**（在 ft_bench_a5.c 里）
 *   单 term 检索的页读 = 字典二分的页数 + 该 term 的 posting 页数。
 *
 * ==================== 退出码 ====================
 *
 *   0 = 全部判据达标；1 = 有判据不达标；2 = 基准自身出错（开库失败等）。
 *
 * 用法：
 *   ft_bench [a4|a5|all] [文档数] [批大小]
 *   默认 all / 100000 / 1000
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/types.h"
#include "ft_bench.h"

/* ================================================================
 * 入口
 * ================================================================ */

int main(int argc, char **argv)
{
    const char *mode = (argc > 1) ? argv[1] : "all";
    u32 n_docs = (argc > 2) ? (u32)atoi(argv[2]) : 100000u;
    u32 batch = (argc > 3) ? (u32)atoi(argv[3]) : 1000u;
    int rc = 0;
    int rc_a4 = 0;
    int rc_a5 = 0;

    if (batch == 0) {
        batch = 1000u;
    }
    if (n_docs == 0) {
        n_docs = 100000u;
    }

    if (strcmp(mode, "a4") == 0 || strcmp(mode, "all") == 0) {
        rc_a4 = ft_bench_run_a4(n_docs, batch);
        if (rc_a4 == 2) {
            return 2;
        }
    }
    if (strcmp(mode, "a5") == 0 || strcmp(mode, "all") == 0) {
        rc_a5 = ft_bench_run_a5();
        if (rc_a5 == 2) {
            return 2;
        }
    }
    if (strcmp(mode, "a4") != 0 && strcmp(mode, "a5") != 0 && strcmp(mode, "all") != 0) {
        printf("用法：ft_bench [a4|a5|all] [文档数] [批大小]\n");
        return 2;
    }

    rc = (rc_a4 == 0 && rc_a5 == 0) ? 0 : 1;
    printf("\n基准退出码：%d（0 = 全部判据达标）\n", rc);
    return rc;
}
