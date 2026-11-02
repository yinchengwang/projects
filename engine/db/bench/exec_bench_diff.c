/*
 * exec_bench_diff.c — 对拍驱动（v1.1 拆文件）
 *
 * ==================== 为什么拆出来 ====================
 *
 * 硬规则：单文件不超过 800 行。
 * 加了聚合对拍（25 条 SQL）之后 exec_bench.c 涨到 854 行 —— 超了。
 *
 * 拆分的界线是"**干什么**"：
 *   exec_bench.c      基准的主体（造数据、相位分解、索引门禁）
 *   exec_bench_diff.c 对拍（跑 SQL、逐格比、聚合对拍集）
 *
 * ==================== 对拍的是什么 ====================
 *
 *   当前执行器（列批 + hash 分组） vs 冻结的火山执行器（逐行）
 *   当前执行器（列批 + hash 分组） vs 逐行参照实现（线性分组）
 *
 * 两组对拍都在**同一个进程、同一个库**上跑 —— 没有进程启动、
 * WAL 恢复、页缓存冷启动这些中间变量（见 exec_volcano.c 的文件头）。
 */

#include "query/exec_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int exec_select_volcano(Db *db, Stmt *st, QueryResult **out,
                         char *err, int errcap);
int exec_select_agg_ref(Db *db, Stmt *st, QueryResult **out,
                         char *err, int errcap);

int bench_run_sql(Db *kdb, const char *sql, bool use_volcano, QueryResult **out)
{
    char err[256] = {0};
    Stmt st;
    memset(&st, 0, sizeof(st));

    int rc = parse_sql(sql, &st, err, sizeof(err));
    if (rc != OK) {
        printf("  解析失败: %s\n", err);
        return -1;
    }

    if (use_volcano)
        rc = exec_select_volcano(kdb, &st, out, err, sizeof(err));
    else
        rc = exec_stmt(kdb, &st, out, err, sizeof(err));

    if (rc != OK)
        printf("  执行失败(%s): %s\n", use_volcano ? "火山" : "当前", err);

    free_stmt(&st);
    return rc == OK ? 0 : -1;
}

/*
 * 逐格比对两个结果集。
 *
 * 【比什么】
 *   行数、行序、列名、每一格的内容 —— 全比。
 *   行序也要比：攒批不改变行的物理顺序，所以顺序变了就是真错了。
 */
int bench_diff_results(const QueryResult *a, const QueryResult *b,
                        const char *sql, int *fails)
{
    int bad = 0;

    if (a->ncols != b->ncols) {
        printf("    列数不同: %d vs %d\n", a->ncols, b->ncols);
        bad = 1;
    } else {
        for (int c = 0; c < a->ncols; c++) {
            const char *na = a->colnames[c] ? a->colnames[c] : "";
            const char *nb = b->colnames[c] ? b->colnames[c] : "";
            if (strcmp(na, nb) != 0) {
                printf("    列名不同[%d]: '%s' vs '%s'\n", c, na, nb);
                bad = 1;
            }
        }
    }

    if (a->nrows != b->nrows) {
        printf("    行数不同: %d vs %d\n", a->nrows, b->nrows);
        bad = 1;
    }

    int n = a->nrows < b->nrows ? a->nrows : b->nrows;
    int ncols = a->ncols < b->ncols ? a->ncols : b->ncols;
    for (int r = 0; r < n && !bad; r++) {
        for (int c = 0; c < ncols; c++) {
            const char *va = a->cells[r * a->ncols + c];
            const char *vb = b->cells[r * b->ncols + c];
            va = va ? va : "";
            vb = vb ? vb : "";
            if (strcmp(va, vb) != 0) {
                printf("    第 %d 行第 %d 列不同: '%s' vs '%s'\n", r, c, va, vb);
                bad = 1;
                break;
            }
        }
    }

    if (bad) {
        printf("  ✗ %s\n", sql);
        (*fails)++;
    } else {
        printf("  ✓ %s  (%d 行)\n", sql, a->nrows);
    }
    return bad ? -1 : 0;
}

/*
 * 对拍 SQL 集。
 *
 * 【覆盖面是刻意选的】
 *   - 单谓词 / 多谓词 / AND / OR / NOT
 *   - 三值逻辑（NULL 参与比较）
 *   - IS NULL / IS NOT NULL
 *   - 投影：星号 / 列 / 表达式
 *   - LIMIT（含 LIMIT 0 —— 投影前就停，批式最容易在这里分叉）
 *   - 索引路径（WHERE 命中索引 → 下推扫描）
 *   - 常量比较（值在左边：5 < val）
 *   - 空表（0 行）边界
 */
int bench_run_diff(Db *kdb, bool have_index)
{
    static const char *sqls[] = {
        "SELECT id, grp FROM bench_t WHERE val >= 900",
        "SELECT * FROM bench_t WHERE id = 12345",
        "SELECT id FROM bench_t WHERE val IS NULL",
        "SELECT id FROM bench_t WHERE val IS NOT NULL",
        "SELECT id FROM bench_t WHERE val > 100 AND grp < 5",
        "SELECT id FROM bench_t WHERE val = 1 OR val = 2",
        "SELECT id FROM bench_t WHERE NOT (val > 500)",
        "SELECT id FROM bench_t WHERE name = 'row000042'",
        "SELECT id FROM bench_t WHERE name != 'row000042'",
        "SELECT id, val + 1 FROM bench_t WHERE grp = 3",
        "SELECT id FROM bench_t LIMIT 10",
        "SELECT id FROM bench_t WHERE val >= 900 LIMIT 5",
        "SELECT id FROM bench_t LIMIT 0",
        "SELECT id FROM bench_t WHERE 900 <= val",
        "SELECT id FROM bench_t WHERE val > 100.5",
        "SELECT id FROM bench_t WHERE grp <> 3",
        "SELECT id FROM bench_t WHERE id = 42 AND val = 42 AND grp = 2",
        "SELECT id FROM bench_t WHERE id = 42 OR id = 43 OR id = 44",
        /* 含 NULL 的表：三值逻辑是批式求值最容易写错的地方 */
        "SELECT id, val FROM bench_nt WHERE val IS NULL",
        "SELECT id, val FROM bench_nt WHERE val IS NOT NULL",
        "SELECT id FROM bench_nt WHERE val = 1",
        "SELECT id FROM bench_nt WHERE val > 1",
        "SELECT id FROM bench_nt WHERE val IS NULL OR val > 1",
        "SELECT id FROM bench_nt WHERE NOT (val IS NULL)",
        "SELECT id, val FROM bench_nt LIMIT 3",
        "SELECT * FROM bench_nt WHERE id < 50",
    };
    const int nsql = (int)(sizeof(sqls) / sizeof(sqls[0]));

    int fails = 0;
    printf("\n--- 对拍（当前执行器 vs 冻结的火山执行器）---\n");

    for (int i = 0; i < nsql; i++) {
        /* 语法还没实现的语句直接跳过（parse 会失败，不是执行器的问题） */
        char err[256] = {0};
        Stmt probe;
        memset(&probe, 0, sizeof(probe));
        if (parse_sql(sqls[i], &probe, err, sizeof(err)) != OK) {
            printf("  – %s（语法未支持，跳过）\n", sqls[i]);
            free_stmt(&probe);
            continue;
        }
        free_stmt(&probe);

        QueryResult *qa = NULL;
        QueryResult *qb = NULL;
        if (bench_run_sql(kdb, sqls[i], false, &qa) != 0) {
            fails++;
            continue;
        }
        if (bench_run_sql(kdb, sqls[i], true, &qb) != 0) {
            qr_free(qa);
            fails++;
            continue;
        }
        bench_diff_results(qa, qb, sqls[i], &fails);
        qr_free(qa);
        qr_free(qb);
    }

    /* 空表也要对拍：批量边界最容易在 0 行时出错 */
    printf("\n--- 对拍（空表边界）---\n");
    {
        static const char *empty_sqls[] = {
            "SELECT id FROM bench_empty",
            "SELECT id FROM bench_empty WHERE val = 1",
            "SELECT * FROM bench_empty LIMIT 5",
        };
        for (int i = 0; i < (int)(sizeof(empty_sqls) / sizeof(empty_sqls[0])); i++) {
            QueryResult *qa = NULL;
            QueryResult *qb = NULL;
            if (bench_run_sql(kdb, empty_sqls[i], false, &qa) != 0 ||
                bench_run_sql(kdb, empty_sqls[i], true, &qb) != 0) {
                fails++;
                continue;
            }
            bench_diff_results(qa, qb, empty_sqls[i], &fails);
            qr_free(qa);
            qr_free(qb);
        }
    }

    if (have_index)
        printf("\n（索引已建，下推路径已包含在上面的对拍集里）\n");

    return fails;
}

/* ---------------- 对拍（聚合） ---------------- */

/*
 * 聚合的对拍：当前实现（列批 + hash 分组） vs 参照实现（逐行 + 线性分组）。
 *
 * 【为什么聚合要单独一套对拍】
 * 冻结的火山副本诞生于聚合之前 —— 它跑不了聚合查询。
 * 聚合这一块用**另一种走法**来对照（见 bench/exec_agg_ref.c 的文件头）。
 */
int bench_run_diff_agg(Db *kdb)
{
    static const char *sqls[] = {
        "SELECT COUNT(*) FROM bench_t",
        "SELECT COUNT(val) FROM bench_t",
        "SELECT SUM(val) FROM bench_t",
        "SELECT AVG(val) FROM bench_t",
        "SELECT MIN(val), MAX(val) FROM bench_t",
        "SELECT SUM(id), AVG(id) FROM bench_t",
        "SELECT COUNT(*) FROM bench_t WHERE val >= 900",
        "SELECT SUM(val), AVG(val) FROM bench_t WHERE val >= 900",
        "SELECT COUNT(*) FROM bench_nt",
        "SELECT COUNT(val) FROM bench_nt",
        "SELECT SUM(val) FROM bench_nt",
        "SELECT AVG(val) FROM bench_nt",
        "SELECT MIN(val), MAX(val) FROM bench_nt",
        "SELECT grp, COUNT(*) FROM bench_t GROUP BY grp",
        "SELECT grp, SUM(val), AVG(val) FROM bench_t GROUP BY grp",
        "SELECT grp, MIN(val), MAX(val) FROM bench_t GROUP BY grp",
        "SELECT grp, COUNT(*) FROM bench_t WHERE val >= 900 GROUP BY grp",
        "SELECT grp, SUM(val) FROM bench_t WHERE val >= 900 GROUP BY grp",
        "SELECT COUNT(*) FROM bench_t GROUP BY grp",
        /*
         * bench_nt 的 id 有 10000 个不同值 → 超过 MAX_GROUPS（4096），
         * 当前实现会**明确报错**（A4 的设计），参照实现没有上限。
         * 那是设计差异，不是 bug —— 所以不进对拍集（单测里验组数超限）。
         */
        "SELECT grp, COUNT(*), SUM(v), AVG(v), MIN(v), MAX(v) FROM bench_agg GROUP BY grp",
        "SELECT grp, COUNT(*) FROM bench_agg WHERE v > 100 GROUP BY grp",
        "SELECT grp, SUM(v) FROM bench_agg WHERE v > 100 GROUP BY grp",
        "SELECT COUNT(*), SUM(v), AVG(v) FROM bench_agg",
        "SELECT COUNT(*) FROM bench_empty",
        "SELECT SUM(val) FROM bench_empty",
        "SELECT id, COUNT(*) FROM bench_empty GROUP BY id",
    };
    const int nsql = (int)(sizeof(sqls) / sizeof(sqls[0]));

    int fails = 0;
    printf("\n--- 对拍（聚合：当前实现 vs 逐行参照实现）---\n");

    for (int i = 0; i < nsql; i++) {
        QueryResult *qa = NULL;
        QueryResult *qb = NULL;
        char err[256] = {0};

        Stmt st;
        memset(&st, 0, sizeof(st));
        if (parse_sql(sqls[i], &st, err, sizeof(err)) != OK) {
            printf("  – %s（语法未支持，跳过）\n", sqls[i]);
            free_stmt(&st);
            continue;
        }
        free_stmt(&st);

        /* 当前实现 */
        {
            Stmt s2;
            memset(&s2, 0, sizeof(s2));
            if (parse_sql(sqls[i], &s2, err, sizeof(err)) != OK) {
                free_stmt(&s2);
                fails++;
                continue;
            }
            int rc = exec_stmt(kdb, &s2, &qa, err, sizeof(err));
            free_stmt(&s2);
            if (rc != OK) {
                printf("    当前实现执行失败: %s\n", err);
                fails++;
                continue;
            }
        }

        /* 参照实现 */
        {
            Stmt s2;
            memset(&s2, 0, sizeof(s2));
            if (parse_sql(sqls[i], &s2, err, sizeof(err)) != OK) {
                free_stmt(&s2);
                qr_free(qa);
                fails++;
                continue;
            }
            int rc = exec_select_agg_ref(kdb, &s2, &qb, err, sizeof(err));
            free_stmt(&s2);
            if (rc != OK) {
                printf("    参照实现执行失败: %s\n", err);
                qr_free(qa);
                fails++;
                continue;
            }
        }

        bench_diff_results(qa, qb, sqls[i], &fails);
        qr_free(qa);
        qr_free(qb);
    }

    return fails;
}

