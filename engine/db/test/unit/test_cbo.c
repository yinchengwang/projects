/*
 * test_cbo.c — 规则 CBO 验收（v1.1 步骤 4 · A5）
 *
 * 覆盖 PLAN-v1.1 §1.5 的 A5：
 *   「CBO：大表等值 → 走索引；小表（<64 行）等值 → 全扫；
 *     EXPLAIN 输出两个代价」
 *
 * 另外把"统计从哪来"也一并验了 —— 没有 nrows_est 就没有 CBO，
 * 所以 nrows_est 的维护（写路径增量、VACUUM 校准并落盘）是这一项的前提。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/cbo.db"

static DB *db;

static void exec_ok(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 执行失败: %s → %s (%s)\n", sql, db_strerror(e),
               db_last_error(db));
        failures++;
    }
    if (r)
        db_result_free(r);
}

/* 把结果集拍平（多行用 | 连接） */
static char *q(const char *sql)
{
    static char buf[8192];
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);

    buf[0] = '\0';
    if (e != DB_OK) {
        snprintf(buf, sizeof(buf), "<ERR:%s>", db_last_error(db));
        if (r)
            db_result_free(r);
        return buf;
    }

    int nrows = db_result_rows(r);
    int ncols = db_result_cols(r);
    size_t off = 0;

    for (int i = 0; i < nrows; i++) {
        if (i && off < sizeof(buf) - 1)
            buf[off++] = '|';
        for (int c = 0; c < ncols; c++) {
            if (c && off < sizeof(buf) - 1)
                buf[off++] = ',';
            const char *v = db_result_value(r, i, c);
            size_t n = strlen(v);
            if (off + n >= sizeof(buf) - 1)
                n = sizeof(buf) - 2 - off;
            memcpy(buf + off, v, n);
            off += n;
        }
    }
    buf[off] = '\0';
    db_result_free(r);
    return buf;
}

static int count_rows(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

/* 批量插入 n 行到 t(a,b) */
static void fill(const char *table, int n, int start)
{
    exec_ok("BEGIN");
    for (int base = 0; base < n; base += 16) {
        char sql[1024];
        int k = snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES ", table);
        for (int i = 0; i < 16 && base + i < n; i++) {
            int v = start + base + i;
            k += snprintf(sql + k, sizeof(sql) - (size_t)k,
                          "%s(%d,%d)", i ? "," : "", v, v % 7);
        }
        exec_ok(sql);
    }
    exec_ok("COMMIT");
}

/* ---------------- 1. nrows_est 的维护 ---------------- */

static void test_nrows_maintenance(void)
{
    printf("  [1] nrows_est 的维护\n");

    exec_ok("CREATE TABLE t (a INT, b INT)");

    /* 空表 */
    CHECK(strcmp(q("EXPLAIN SELECT a FROM t WHERE a = 1"), "") != 0);
    {
        char *s = q("EXPLAIN SELECT a FROM t WHERE a = 1");
        CHECK(strstr(s, "rows_est = 0") != NULL);
    }

    /* 插入 100 行 → 100 */
    fill("t", 100, 0);
    {
        char *s = q("EXPLAIN SELECT a FROM t WHERE a = 1");
        CHECK(strstr(s, "rows_est = 100") != NULL);
    }

    /* 删 10 行 → 90 */
    exec_ok("DELETE FROM t WHERE a < 10");
    {
        char *s = q("EXPLAIN SELECT a FROM t WHERE a = 50");
        CHECK(strstr(s, "rows_est = 90") != NULL);
    }

    /*
     * VACUUM 校准：删掉的是**死版本**，kept 数的是活行。
     * 这里只验"VACUUM 之后估算仍然等于活行数"。
     */
    exec_ok("VACUUM t");
    {
        char *s = q("EXPLAIN SELECT a FROM t WHERE a = 50");
        CHECK(strstr(s, "rows_est = 90") != NULL);
    }
    CHECK(count_rows("SELECT a FROM t") == 90);
}

/* ---------------- 2. 小表恒全扫 ---------------- */

static void test_small_table(void)
{
    printf("  [2] 小表（< 64 行）恒全扫\n");

    exec_ok("CREATE TABLE small (a INT, b INT)");
    exec_ok("INSERT INTO small VALUES (1,10),(2,20),(3,30)");
    exec_ok("CREATE INDEX ix_small ON small (a)");

    char *s = q("EXPLAIN SELECT a FROM small WHERE a = 2");
    CHECK(strstr(s, "Seq Scan") != NULL);
    CHECK(strstr(s, "小表") != NULL);
    /* 即使算出来的 idx 更便宜，规则仍然压过公式 —— 理由要写清楚 */
    CHECK(strstr(s, "rows_est = 3") != NULL);

    /* 结果本身仍然正确 */
    CHECK(strcmp(q("SELECT a FROM small WHERE a = 2"), "2") == 0);
}

/* ---------------- 3. 大表走索引 ---------------- */

static void test_large_table(void)
{
    printf("  [3] 大表（>= 64 行）走索引\n");

    exec_ok("CREATE TABLE big (a INT, b INT)");
    fill("big", 200, 0);
    exec_ok("CREATE INDEX ix_big ON big (a)");

    /* 等值 → 走索引 */
    char *s = q("EXPLAIN SELECT a FROM big WHERE a = 20");
    CHECK(strstr(s, "Index Scan") != NULL);
    CHECK(strstr(s, "Index Cond: (a = 20)") != NULL);
    CHECK(strstr(s, "cost_idx < cost_seq") != NULL);

    /* 范围 → 也走索引（sel = 1/3，cost_idx 仍小于 cost_seq） */
    s = q("EXPLAIN SELECT a FROM big WHERE a >= 190");
    CHECK(strstr(s, "Index Scan") != NULL);
    CHECK(strstr(s, "sel = 0.3333") != NULL);

    /* 没有索引的列 → 全扫，但 rows_est 仍然显示 */
    s = q("EXPLAIN SELECT a FROM big WHERE b = 3");
    CHECK(strstr(s, "Seq Scan") != NULL);
    CHECK(strstr(s, "rows_est = 200") != NULL);
    CHECK(strstr(s, "无可用谓词") != NULL);

    /* 结果正确性（CBO 只影响路径，不影响结果） */
    CHECK(count_rows("SELECT a FROM big WHERE a = 20") == 1);
    CHECK(count_rows("SELECT a FROM big WHERE a >= 190") == 10);
}

/* ---------------- 4. EXPLAIN 输出两个代价 ---------------- */

static void test_explain_output(void)
{
    printf("  [4] EXPLAIN 输出两个代价\n");

    char *s = q("EXPLAIN SELECT a FROM big WHERE a = 20");
    CHECK(strstr(s, "Cost: seq =") != NULL);
    CHECK(strstr(s, "idx =") != NULL);
    CHECK(strstr(s, "Decision:") != NULL);

    /* 列名是 QUERY PLAN（与 PG 一致） */
    {
        Result *r = NULL;
        db_err e = db_exec(db, "EXPLAIN SELECT a FROM big WHERE a = 20", &r);
        CHECK(e == DB_OK);
        if (e == DB_OK) {
            CHECK(db_result_cols(r) == 1);
            CHECK(strcmp(db_result_colname(r, 0), "QUERY PLAN") == 0);
            CHECK(db_result_rows(r) >= 4);
            db_result_free(r);
        }
    }

    /* 聚合也要交代 */
    s = q("EXPLAIN SELECT b, COUNT(*) FROM big GROUP BY b");
    CHECK(strstr(s, "Aggregate: yes") != NULL);
    CHECK(strstr(s, "GROUP BY 1 列") != NULL);
}

/* ---------------- 5. 多个 AND 谓词：挑最窄的 ---------------- */

static void test_pick_narrowest(void)
{
    printf("  [5] 多个 AND 谓词时挑选择率最小的\n");

    exec_ok("CREATE TABLE m (a INT, b INT)");
    fill("m", 300, 0);
    exec_ok("CREATE INDEX ix_m_a ON m (a)");
    exec_ok("CREATE INDEX ix_m_b ON m (b)");

    /*
     * a = 5   → sel = 1/300 ≈ 0.0033
     * b >= 5  → sel = 1/3 ≈ 0.3333
     * 应该挑 a（窄得多）。推 b 会白读 100 行。
     */
    char *s = q("EXPLAIN SELECT a FROM m WHERE a = 5 AND b >= 5");
    CHECK(strstr(s, "Index Cond: (a = 5)") != NULL);
    CHECK(strstr(s, "sel = 0.0033") != NULL);

    /* 换个顺序写也一样（不依赖谓词在 AND 树里的位置） */
    s = q("EXPLAIN SELECT a FROM m WHERE b >= 5 AND a = 5");
    CHECK(strstr(s, "Index Cond: (a = 5)") != NULL);

    /* 结果正确（只推一个谓词，剩下的由执行器复检） */
    CHECK(count_rows("SELECT a FROM m WHERE a = 5 AND b >= 5") == 1);
}

/* ---------------- 6. VACUUM 之后落盘：重启还在 ---------------- */

static void test_persist_across_restart(void)
{
    printf("  [6] VACUUM 落盘 → 重启后估算还在\n");

    /*
     * 建一张独立的表，灌 120 行，VACUUM（校准 + 落盘），
     * 然后关库重开 —— nrows_est 应该从磁盘读回来。
     */
    exec_ok("CREATE TABLE p (a INT, b INT)");
    fill("p", 120, 0);
    exec_ok("VACUUM p");

    db_close(db);
    CHECK(db_open(PATH, &db) == DB_OK);

    char *s = q("EXPLAIN SELECT a FROM p WHERE a = 5");
    CHECK(strstr(s, "rows_est = 120") != NULL);
    CHECK(count_rows("SELECT a FROM p") == 120);
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    remove(PATH);
    remove(PATH "-wal");

    db_err e = db_open(PATH, &db);
    if (e != DB_OK) {
        printf("打开失败: %s\n", db_strerror(e));
        return 1;
    }

    test_nrows_maintenance();
    test_small_table();
    test_large_table();
    test_explain_output();
    test_pick_narrowest();
    test_persist_across_restart();

    db_close(db);

    if (failures == 0)
        printf("  6 项验收全部通过\n");
    else
        printf("  %d 项失败\n", failures);
    return failures ? 1 : 0;
}
