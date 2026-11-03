/*
 * test_checkpoint.c — v1.2 步骤 6 集成验收
 *
 * 5 个阶段：
 *   A. CHECKPOINT 语句能执行（不报错、不破坏数据）
 *   B. checkpoint 之后**日志真的被截断了**（文件变小）
 *   C. checkpoint 之后断电 → 恢复正确（数据一条不少）
 *   D. clog 文件被落盘了（提交状态有地方可查）
 *   E. 20 轮「写 → checkpoint → 断电 → 恢复」全部正确，
 *      而且日志大小**不随轮数增长**（这才是步骤 6 的目的）
 *
 * 【为什么 B 是核心】
 *
 * 步骤 6 的全部意义就是"日志可以被扔掉"。
 * 如果 checkpoint 跑完日志还是那么大，那它只是刷了个页，
 * 恢复时间还是随库的历史长度线性增长 —— 什么都没改善。
 *
 * 【为什么 E 要跑 20 轮】
 *
 * 单轮对了说明不了"幂等"。每一轮都有：
 *   - 一批新提交的数据
 *   - 一次 checkpoint（刷页 + 写重放点 + 落 clog + 截日志）
 *   - 一次断电（脏页全丢、clog 不落盘）
 *   - 一次恢复（重放 + 回滚）
 * 20 轮下来，任何一处"状态没同步"都会在后面的轮次里暴露。
 *
 * 【断电怎么模拟】
 *
 * db_abandon()：**不刷脏页**、**clog 不落盘**、直接关掉。
 * 这才是真的断电 —— 用 db_close 的话，脏页刷了、clog 落盘了、
 * 日志还可能留下"干净关闭"的标记，那就测不出恢复路径了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH       "test/tmp/ckpt.db"
#define WAL_PATH   "test/tmp/ckpt.db-wal"
#define CLOG_PATH  "test/tmp/ckpt.db-clog"

/* 每轮写的行数 */
#define ROUND_ROWS 20
/* 轮数 */
#define ROUNDS     20

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
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

/* 文件大小；文件不存在返回 -1 */
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

static bool file_exists(const char *path)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return false;
    fclose(f);
    return true;
}

/* 插一批行：id 从 base 开始，val = id * 3 */
static void insert_rows(DB *db, int base, int n)
{
    char sql[128];
    for (int i = 0; i < n; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d);", base + i,
                 (base + i) * 3);
        if (run_sql(db, sql) != 0) {
            printf("    FAIL 插入第 %d 行失败\n", base + i);
            failures++;
            return;
        }
    }
}

/* ---- 阶段 A：CHECKPOINT 语句能执行 ---- */
static void phase_a_statement(DB *db)
{
    Result *r = NULL;
    db_err e = db_exec(db, "CHECKPOINT;", &r);
    CHECK(e == DB_OK);
    if (r)
        db_result_free(r);

    /* 数据没被动过 */
    CHECK(scalar(db, "SELECT COUNT(*) FROM t;") == 5);
}

/* ---- 阶段 B：日志真的被截断了 ---- */
static void phase_b_truncate(DB *db)
{
    /* 先写一批，让日志涨起来 */
    insert_rows(db, 100, 300);
    CHECK(scalar(db, "SELECT COUNT(*) FROM t;") == 305);

    long long before = file_size(WAL_PATH);
    printf("    checkpoint 前：日志 %lld 字节\n", before);
    CHECK(before > 0);

    CHECK(run_sql(db, "CHECKPOINT;") == 0);

    long long after = file_size(WAL_PATH);
    printf("    checkpoint 后：日志 %lld 字节\n", after);
    CHECK(after >= 0);

    /*
     * 截断之后日志里只剩"最后一条"（LSN 不能倒退），
     * 所以它应该远小于之前 —— 至少小一个数量级。
     */
    CHECK(after < before / 4);

    /* 数据一条不少 */
    CHECK(scalar(db, "SELECT COUNT(*) FROM t;") == 305);
}

/* ---- 阶段 C：checkpoint 之后断电 → 恢复正确 ---- */
static void phase_c_crash_after_ckpt(void)
{
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    /* checkpoint 之后又写了一批（这些数据只在日志里） */
    insert_rows(db, 1000, 50);
    CHECK(scalar(db, "SELECT COUNT(*) FROM t;") == 355);

    /* 断电：脏页全丢，clog 不落盘 */
    db_abandon(db);

    /* 重开：靠日志重放 + clog 重建可见性 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    if (!db2)
        return;

    long long cnt = scalar(db2, "SELECT COUNT(*) FROM t;");
    printf("    断电后重开：行数 = %lld（应为 355）\n", cnt);
    CHECK(cnt == 355);

    /* 抽查值：checkpoint 之前写的、之后写的，都要对 */
    Result *r = NULL;
    CHECK(db_exec(db2, "SELECT val FROM t WHERE id = 0;", &r) == DB_OK);
    if (r) {
        CHECK(db_result_rows(r) == 1);
        CHECK(atoll(db_result_value(r, 0, 0)) == 0);
        db_result_free(r);
    }
    r = NULL;
    CHECK(db_exec(db2, "SELECT val FROM t WHERE id = 1049;", &r) == DB_OK);
    if (r) {
        CHECK(db_result_rows(r) == 1);
        CHECK(atoll(db_result_value(r, 0, 0)) == 1049 * 3);
        db_result_free(r);
    }
    r = NULL;
    CHECK(db_exec(db2, "SELECT val FROM t WHERE id = 304;", &r) == DB_OK);
    if (r) {
        CHECK(db_result_rows(r) == 1);
        CHECK(atoll(db_result_value(r, 0, 0)) == 304 * 3);
        db_result_free(r);
    }

    db_close(db2);
}

/* ---- 阶段 D：clog 文件落盘了 ---- */
static void phase_d_clog_file(void)
{
    CHECK(file_exists(CLOG_PATH));
    long long n = file_size(CLOG_PATH);
    printf("    clog 文件 %lld 字节\n", n);
    CHECK(n > 0);
}

/* ---- 阶段 E：20 轮「写 → checkpoint → 断电 → 恢复」 ---- */
static void phase_e_rounds(void)
{
    long long wal_sizes[ROUNDS];

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    int total = 0;    /* 已经写进去的行数 */

    for (int round = 0; round < ROUNDS; round++) {
        /* 写一批 */
        insert_rows(db, 10000 + round * ROUND_ROWS, ROUND_ROWS);
        total += ROUND_ROWS;

        /* checkpoint */
        if (run_sql(db, "CHECKPOINT;") != 0) {
            printf("    FAIL 第 %d 轮 checkpoint 失败\n", round);
            failures++;
            return;
        }
        wal_sizes[round] = file_size(WAL_PATH);

        /* 断电 */
        db_abandon(db);

        /* 恢复 */
        DB *db2 = NULL;
        if (db_open(PATH, &db2) != DB_OK) {
            printf("    FAIL 第 %d 轮重开失败\n", round);
            failures++;
            return;
        }

        long long cnt = scalar(db2, "SELECT COUNT(*) FROM t;");
        if (cnt != total + 355) {
            printf("    FAIL 第 %d 轮：行数 = %lld，应为 %d\n",
                   round, cnt, total + 355);
            failures++;
            db_close(db2);
            return;
        }

        /* 抽查这一轮写的最后一行 */
        int last_id = 10000 + round * ROUND_ROWS + ROUND_ROWS - 1;
        char sql[64];
        snprintf(sql, sizeof(sql), "SELECT val FROM t WHERE id = %d;", last_id);
        Result *r = NULL;
        if (db_exec(db2, sql, &r) != DB_OK) {
            printf("    FAIL 第 %d 轮查询失败\n", round);
            failures++;
            db_close(db2);
            return;
        }
        if (r) {
            if (db_result_rows(r) != 1 ||
                atoll(db_result_value(r, 0, 0)) != last_id * 3) {
                printf("    FAIL 第 %d 轮：id=%d 的值不对\n", round, last_id);
                failures++;
            }
            db_result_free(r);
        }

        db_close(db2);

        /* 下一轮从新句柄继续 */
        db = NULL;
        if (db_open(PATH, &db) != DB_OK) {
            printf("    FAIL 第 %d 轮重新打开失败\n", round);
            failures++;
            return;
        }
    }

    db_close(db);

    /*
     * 日志大小不能随轮数增长。
     *
     * 如果截断没生效，每一轮都会把上一轮的日志留在文件里 →
     * 大小线性增长。
     */
    long long first = wal_sizes[0];
    long long last = wal_sizes[ROUNDS - 1];
    printf("    第 1 轮后日志 %lld 字节，第 %d 轮后 %lld 字节\n",
           first, ROUNDS, last);
    CHECK(last <= first * 2);
}

int main(void)
{
    printf("test_checkpoint（集成）:\n");

    remove(PATH);
    remove(WAL_PATH);
    remove(CLOG_PATH);

    /* 建库建表，插 5 行 */
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return 1;
    CHECK(run_sql(db, "CREATE TABLE t (id INT, val INT);") == 0);
    insert_rows(db, 0, 5);
    CHECK(scalar(db, "SELECT COUNT(*) FROM t;") == 5);

    phase_a_statement(db);
    phase_b_truncate(db);
    db_close(db);

    phase_c_crash_after_ckpt();
    phase_d_clog_file();
    phase_e_rounds();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  5 项验收全部通过 [PASS]\n");
    return 0;
}
