/*
 * bench/insert_batch_bench.c —— 批量 INSERT 的批大小 vs 造数据耗时
 *
 * ==================== 这个基准回答什么问题 ====================
 *
 * 单条 INSERT 的行数上限已经解除（原来硬限 16 行）。
 * 于是"一次塞多少行"变成了一个**可调参数** —— 那么该塞多少？
 *
 * 直觉是"越大越好"（少解析几次、少几条语句）。但这条直觉有两个边界：
 *
 *   1. **SQL 文本长度**：行数线性放大语句长度，而 lexer 是按字节扫的
 *      （每个 token 都有 TK_TEXT_MAX=256 的定长缓冲，不跟文本走）。
 *      一条 10 万行的 INSERT 是几 MB 字符串 —— 分配与拷贝都不免费。
 *   2. **单事务内的工作量**：整个 bench 包在一个事务里，
 *      批大小不改变事务数，所以**不该**看到 fsync 的差别。
 *      如果看到了，说明哪条路径偷偷开了事务。
 *
 * 所以这个基准量的是**"解析 + 执行"的摊销**，不是磁盘。
 *
 * ==================== 怎么读结果 ====================
 *
 *   batch=1     每行一条语句 —— 最坏情况（语句开销完全不摊销）
 *   batch=16    早期硬上限时的形态 —— 历史基准的可比基线
 *   batch=1024  大块装载
 *
 * 关注 **rows/ms**：它应该随批大小上升，然后趋平。
 * 趋平的拐点就是"再大也没有收益"的位置 —— 那就是该选的默认值。
 *
 * ⚠️ 每个批大小都用**独立的库文件**：共用文件会让后一轮读到前一轮的
 * 页缓存与 freelist 状态，测量互相污染。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "query/parser.h"   /* parse_sql / free_stmt：把解析成本单独量出来 */

/* 每轮装多少行。20 万行足以让解析开销盖过启动噪声，又不至于跑太久。 */
#define DEFAULT_ROWS 200000

/*
 * SQL 文本缓冲。
 *
 * ⚠️ 它不是"越大越好"的旋钮：批大小一旦让语句超过这个长度，
 * snprintf 就会悄悄截断，下一条语句从半截开始 → 解析报"缺少符号"。
 * 实测 4 列元组大约 26 字节/行，64 KB 够 2000 行出头。
 * 所以下面的批大小表最高取到 2048，并**核对实际插入行数**。
 */
#define SQL_BUF 65536

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
}

/* 每条语句的文本长度（%zu 语义），用于显示"这条 SQL 有多长" */
static int build_insert(char *sql, size_t cap, int batch, int base, int nrows)
{
    int n = 0;
    n += snprintf(sql + n, cap - (size_t)n, "INSERT INTO t VALUES ");
    int rows = 0;
    for (int k = 0; k < batch && base + k < nrows; k++) {
        int i = base + k;
        n += snprintf(sql + n, cap - (size_t)n,
                      "%s(%d,%d,%d,'row%06d')", k ? "," : "",
                      i, i % 10, i % 1000, i);
        rows++;
    }
    n += snprintf(sql + n, cap - (size_t)n, ";");
    return n;
}

/*
 * 只跑解析器、不执行 —— 用来把"解析成本"从总耗时里分离出来。
 *
 * 如果解析占比很小，那么"批大小"对总耗时就几乎没有影响，
 * 这个基准的结论就该是"批大小不是性能旋钮，它是 API 便利性"。
 */
static double parse_only(const char *sql, int repeat)
{
    double t0 = now_ms();
    for (int i = 0; i < repeat; i++) {
        Stmt st;
        char err[256];
        if (parse_sql(sql, &st, err, (int)sizeof(err)) != OK) {
            printf("    解析失败: %s\n", err);
            return -1;
        }
        free_stmt(&st);
    }
    return now_ms() - t0;
}

/*
 * 用一个给定的批大小装满 nrows 行。
 *
 * 输出：*ms_out 总耗时，*rows_out 实际插入行数（用于发现截断）。
 * 返回 0 成功，-1 失败。
 */
static int run_one(const char *path, int nrows, int batch,
                   double *ms_out, int *rows_out, int *sqllen_out)
{
    static char sql[SQL_BUF];
    Result *r = NULL;
    DB *db = NULL;

    *ms_out = -1;
    *rows_out = 0;

    remove(path);
    {
        char aux[256];
        snprintf(aux, sizeof(aux), "%s-wal", path);
        remove(aux);
        snprintf(aux, sizeof(aux), "%s-clog", path);
        remove(aux);
    }

    if (db_open(path, &db) != DB_OK) {
        printf("    开库失败: %s\n", path);
        return -1;
    }
    if (db_exec(db, "CREATE TABLE t (id INT, grp INT, val INT, name TEXT);",
                &r) != DB_OK) {
        printf("    建表失败: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    if (r) { db_result_free(r); r = NULL; }

    if (db_exec(db, "BEGIN;", &r) != DB_OK) {
        printf("    BEGIN 失败: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    if (r) { db_result_free(r); r = NULL; }

    double t0 = now_ms();
    for (int base = 0; base < nrows; base += batch) {
        int len = build_insert(sql, sizeof(sql), batch, base, nrows);
        *sqllen_out = len;
        if ((size_t)len >= sizeof(sql)) {
            printf("    ⚠️ 警告：语句被截断（%d ≥ %d），本轮数字不可信\n",
                   len, (int)sizeof(sql));
        }
        if (db_exec(db, sql, &r) != DB_OK) {
            printf("    插入失败（第 %d 行起，批 %d）: %s\n",
                   base, batch, db_last_error(db));
            db_exec(db, "ROLLBACK;", NULL);
            db_close(db);
            return -1;
        }
        if (r) {
            *rows_out += db_result_affected(r);
            db_result_free(r);
            r = NULL;
        }
    }

    if (db_exec(db, "COMMIT;", &r) != DB_OK) {
        printf("    COMMIT 失败: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    if (r) { db_result_free(r); r = NULL; }

    *ms_out = now_ms() - t0;
    db_close(db);
    return 0;
}

static void rm_db(const char *path)
{
    char aux[256];
    remove(path);
    snprintf(aux, sizeof(aux), "%s-wal", path);
    remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", path);
    remove(aux);
}

int main(int argc, char **argv)
{
    int nrows = argc > 1 ? atoi(argv[1]) : DEFAULT_ROWS;
    if (nrows <= 0)
        nrows = DEFAULT_ROWS;

    static const int batches[] = { 1, 8, 16, 64, 256, 1024, 2048 };
    const int nbatches = (int)(sizeof(batches) / sizeof(batches[0]));

    printf("=== 批量 INSERT 基准 ===\n");
    printf("每轮 %d 行 × 4 列（含 TEXT），**单事务**（所以只有 1 次 fsync）\n", nrows);
    printf("SQL 文本缓冲 %d 字节；批太大时语句会被截断，故最高测到 2048 行。\n\n",
           SQL_BUF);

    printf("%7s %10s %11s %11s %10s %9s\n",
           "batch", "SQL长度", "总耗时ms", "解析ms", "解析占比", "行/毫秒");
    printf("%7s %10s %11s %11s %10s %9s\n",
           "-----", "-------", "-------", "------", "-------", "-------");

    double base_ms = 0.0;
    double base_parse = 0.0;

    for (int b = 0; b < nbatches; b++) {
        char path[256];
        snprintf(path, sizeof(path), "test/tmp/ins_batch_%d.db", batches[b]);

        double ms = -1;
        int rows = 0, sqllen = 0;
        if (run_one(path, nrows, batches[b], &ms, &rows, &sqllen) != 0) {
            rm_db(path);
            continue;
        }

        /*
         * 解析成本：对同一条语句重复解析 n 次（就是实际执行的次数），
         * 只跑 parser、不碰存储。这样能把"解析"从总耗时里切出来。
         */
        static char sql[SQL_BUF];
        build_insert(sql, sizeof(sql), batches[b], 0, nrows);
        int reps = nrows / (batches[b] < nrows ? batches[b] : nrows);
        if (reps < 1) reps = 1;
        double pms = parse_only(sql, reps);

        if (batches[b] == 16) {
            base_ms = ms;
            base_parse = pms;
        }

        printf("%7d %10d %11.1f %11.1f %9.0f%% %9.0f",
               batches[b], sqllen, ms, pms,
               ms > 0 ? 100.0 * pms / ms : 0,
               ms > 0 ? (double)nrows / ms : 0);
        if (rows != nrows)
            printf("   ⚠️ 实际插入 %d/%d 行", rows, nrows);
        printf("\n");
        fflush(stdout);

        rm_db(path);
    }

    if (base_ms > 0) {
        printf("\n以 batch=16 为基线：总耗时 %.0f ms，其中解析 %.0f ms。\n",
               base_ms, base_parse);
    }

    /*
     * 结论必须写在输出里，不能只靠人看表 ——
     * 下一轮的人（或未来的我）需要知道"这个数字意味着什么"。
     */
    printf("\n怎么读这张表：\n"
           "  · 「解析占比」= 光跑 parser 的耗时 / 总耗时。它很小就说明\n"
           "    瓶颈在别处（堆插入 + WAL + 记录编码），批大小不是性能旋钮。\n"
           "  · 单事务 → 只有 1 次 fsync：批大小**不该**改变磁盘开销。\n"
           "  · 若各行/毫秒差别在 10%% 以内，结论就是「批大小影响的是\n"
           "    语句条数（API 便利性），不是吞吐」。\n");
    return 0;
}
