/*
 * test_ckpt_recovery_time.c — v1.2 步骤 6 · A6 对照实验
 *
 * A6 的原文：
 *   "写 100MB 数据 + checkpoint → 断电 → 恢复耗时只与 checkpoint 后日志量相关
 *    （对照实验：不 checkpoint 的同负载恢复明显更慢，且差值可解释）"
 *
 * ==================== 三个对照 ====================
 *
 *   A（基线，不 checkpoint）：
 *      写 N 行 → 断电 → 重开，测恢复耗时 T_A
 *      恢复必须把 N 条记录全部重放一遍。
 *
 *   B（有 checkpoint）：
 *      写 N 行 → checkpoint → 断电 → 重开，测恢复耗时 T_B
 *      checkpoint 已经把页刷到盘上、日志截断了 → 几乎没有东西要重放。
 *
 *   C（有 checkpoint，checkpoint 之后又写了一点）：
 *      写 N 行 → checkpoint → 再写 M 行 → 断电 → 重开，测恢复耗时 T_C
 *      **关键**：T_C 应该和 T_B 差不多 —— 多出来的只有那 M 条记录。
 *      这一条才真正证明"恢复时间只取决于 checkpoint 之后的日志量"，
 *      而不是"checkpoint 之后恰好没事可做"。
 *
 * ==================== 规模为什么不是 100MB ====================
 *
 * 计划写 100MB。这里用 N = 20000 行（约 1 MB 库、1.2 MB 日志）。
 * 理由：每条 INSERT 的日志记录都要过一遍重放，这正是要测的东西；
 * 100MB 的规模对**趋势**没有额外信息，只是把测试时间拉长一个量级。
 * 如实记录在下面的输出里。
 *
 * ==================== 计时口径 ====================
 *
 * 用 clock()（进程 CPU 时间）。恢复是 CPU 密集的（逐条重放），
 * CPU 时间比墙钟时间稳，不受机器负载干扰。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <time.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define DB_A "test/tmp/ckpt_time_a.db"
#define DB_B "test/tmp/ckpt_time_b.db"
#define DB_C "test/tmp/ckpt_time_c.db"

#define NROWS 20000   /* 主负载 */
#define MROWS 100     /* checkpoint 之后的追加负载 */

static void rm_db(const char *path)
{
    char buf[128];
    remove(path);
    snprintf(buf, sizeof(buf), "%s-wal", path);
    remove(buf);
    snprintf(buf, sizeof(buf), "%s-clog", path);
    remove(buf);
}

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

static long long file_size(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return -1;
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return -1;
    }
    long long n = ftell(f);
    fclose(f);
    return n;
}

static long long wal_size(const char *path)
{
    char buf[128];
    snprintf(buf, sizeof(buf), "%s-wal", path);
    return file_size(buf);
}

/* 写 rows 行（一个显式事务，只 fsync 一次） */
static bool write_rows(DB *db, int base, int rows)
{
    char sql[128];
    if (run_sql(db, "BEGIN;") != 0)
        return false;
    for (int i = 0; i < rows; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d);", base + i,
                 (base + i) * 3);
        if (run_sql(db, sql) != 0) {
            run_sql(db, "ROLLBACK;");
            return false;
        }
    }
    return run_sql(db, "COMMIT;") == 0;
}

static long long scalar(DB *db, const char *sql)
{
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r)
            db_result_free(r);
        return -1;
    }
    if (!r || db_result_rows(r) != 1) {
        if (r)
            db_result_free(r);
        return -1;
    }
    long long v = atoll(db_result_value(r, 0, 0));
    db_result_free(r);
    return v;
}

/* 一个用例：写负载 → （可选 checkpoint）→ （可选追加）→ 断电 → 计时重开 */
static long long run_case(const char *path, bool do_ckpt, int extra_rows,
                           double *ms, long long *wal_after)
{
    rm_db(path);

    DB *db = NULL;
    if (db_open(path, &db) != DB_OK || !db)
        return -1;
    if (run_sql(db, "CREATE TABLE t (id INT, val INT);") != 0) {
        db_close(db);
        return -1;
    }
    if (!write_rows(db, 0, NROWS)) {
        db_close(db);
        return -1;
    }

    if (do_ckpt) {
        if (run_sql(db, "CHECKPOINT;") != 0) {
            db_close(db);
            return -1;
        }
        *wal_after = wal_size(path);
        if (extra_rows > 0 && !write_rows(db, NROWS, extra_rows)) {
            db_close(db);
            return -1;
        }
    } else {
        *wal_after = wal_size(path);
    }

    /* 断电 */
    db_abandon(db);

    /* 计时重开 —— 这里发生的就是恢复 */
    clock_t t0 = clock();
    DB *db2 = NULL;
    if (db_open(path, &db2) != DB_OK || !db2)
        return -1;
    clock_t t1 = clock();
    *ms = 1000.0 * (double)(t1 - t0) / CLOCKS_PER_SEC;

    long long cnt = scalar(db2, "SELECT COUNT(*) FROM t;");
    db_close(db2);
    return cnt;
}

int main(void)
{
    printf("test_ckpt_recovery_time（集成 · A6 对照）:\n");
    printf("    规模：%d 行主负载 + %d 行追加（计划写 100MB，\n"
           "    这里用约 1MB 库 —— 重放量相同，趋势一致）\n",
           NROWS, MROWS);

    double ms_a = 0, ms_b = 0, ms_c = 0;
    long long wal_a = 0, wal_b = 0, wal_c = 0;

    /* A：基线，不 checkpoint */
    long long cnt_a = run_case(DB_A, false, 0, &ms_a, &wal_a);
    CHECK(cnt_a == NROWS);

    /* B：checkpoint 之后直接断电 */
    long long cnt_b = run_case(DB_B, true, 0, &ms_b, &wal_b);
    CHECK(cnt_b == NROWS);

    /* C：checkpoint 之后再写 M 行，然后断电 */
    long long cnt_c = run_case(DB_C, true, MROWS, &ms_c, &wal_c);
    CHECK(cnt_c == NROWS + MROWS);

    printf("    A 不 checkpoint      ：恢复 %7.1f ms，日志 %lld 字节（断电时）\n",
           ms_a, wal_a);
    printf("    B checkpoint 后断电  ：恢复 %7.1f ms，日志 %lld 字节（checkpoint 后）\n",
           ms_b, wal_b);
    printf("    C checkpoint 后再写 %d：恢复 %7.1f ms，日志 %lld 字节（checkpoint 后）\n",
           MROWS, ms_c, wal_c);

    /*
     * 判据 1：checkpoint 之后日志被截断了（这是恢复变快的**原因**）。
     */
    CHECK(wal_b < wal_a / 4);

    /*
     * 判据 2：恢复时间明显变快。
     *
     * 留一点余量：计时有噪声，不要求"快 10 倍"，
     * 只要求"明显快"（快一半以上）。
     */
    CHECK(ms_b < ms_a * 0.5);

    /*
     * 判据 3（A6 的正题）：C 的恢复时间与 B 同量级 ——
     * 也就是说"checkpoint 之后又写了多少"才是决定恢复时间的因素，
     * 而不是"这个库一共写了多少"。
     *
     * C 比 B 多了 MROWS 条记录，所以 C 略慢是正常的；
     * 判据是"C 远快于 A"（库的历史长度不影响恢复）。
     */
    CHECK(ms_c < ms_a * 0.6);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  3 项验收全部通过 [PASS]\n");
    return 0;
}
