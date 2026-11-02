/*
 * test_agg.c — 聚合 + GROUP BY 验收（v1.1 步骤 3）
 *
 * 覆盖 PLAN-v1.1 §1.5 的 A3（聚合语义）与 A4（组数超限明确报错）。
 *
 * 【为什么期望值内嵌在这里，而不是只靠对照 SQLite】
 * 对照 SQLite 是"和另一个实现一致"，内嵌期望值是"和手算一致"。
 * 两者都要：SQLite 也会错（它的分组顺序是未定义的），
 * 而手算期望值把"输出顺序确定"这条也钉住了。
 * SQLite 对照在 test/integration/test_agg_sqlite.c。
 *
 * 语义基线（对齐 SQLite）：
 *   COUNT(*)     → 行数
 *   COUNT(col)   → 非 NULL 的个数
 *   SUM(col)     → 全 NULL 出 NULL；整数列出整数，溢出转浮点
 *   AVG(col)     → 总是浮点；全 NULL 出 NULL
 *   MIN/MAX(col) → 全 NULL 出 NULL
 *   无 GROUP BY  → 恒 1 行（表空也有）
 *   有 GROUP BY  → 每组 1 行；表空 → 0 行
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

#define PATH "test/tmp/agg.db"

static DB *db;

/* 执行一条语句，失败就记一笔 */
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

/* 把结果集拍平成 "a,b|c,d" 的字符串 */
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

/* 期望值相等 */
static void expect_eq(const char *sql, const char *want)
{
    char *got = q(sql);
    if (strcmp(got, want) != 0) {
        printf("    FAIL %s\n      得到: %s\n      期望: %s\n", sql, got, want);
        failures++;
    }
}

/* 期望行数 */
static void expect_rows(const char *sql, int want)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL %s → %s (%s)\n", sql, db_strerror(e), db_last_error(db));
        failures++;
        if (r)
            db_result_free(r);
        return;
    }
    int got = db_result_rows(r);
    if (got != want) {
        printf("    FAIL %s\n      行数: %d，期望 %d\n", sql, got, want);
        failures++;
    }
    db_result_free(r);
}

/* 期望报错，且错误信息里含某个关键字 */
static void expect_err(const char *sql, const char *kw)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);

    if (e == DB_OK) {
        printf("    FAIL %s\n      本该报错，却成功了\n", sql);
        failures++;
        return;
    }
    const char *msg = db_last_error(db);
    if (!msg || !strstr(msg, kw)) {
        printf("    FAIL %s\n      错误信息里没有 \"%s\"，实际是: %s\n",
               sql, kw, msg ? msg : "(空)");
        failures++;
    }
}

/* ---------------- 1. 无 GROUP BY 聚合 ---------------- */

static void test_basic_agg(void)
{
    printf("  [1] 无 GROUP BY 聚合\n");

    exec_ok("CREATE TABLE t (id INT, grp TEXT, val INT)");
    exec_ok("INSERT INTO t (id, grp, val) VALUES "
            "(1, 'a', 10), (2, 'b', 20), (3, 'a', 30), "
            "(4, 'b', 40), (5, NULL, 50)");

    expect_eq("SELECT COUNT(*) FROM t", "5");
    expect_eq("SELECT SUM(val) FROM t", "150");
    expect_eq("SELECT AVG(val) FROM t", "30.0");
    expect_eq("SELECT MIN(val) FROM t", "10");
    expect_eq("SELECT MAX(val) FROM t", "50");

    /* 多个聚合并列 */
    expect_eq("SELECT COUNT(*), MIN(val), MAX(val) FROM t", "5,10,50");

    /* SUM 整数列出整数 */
    expect_eq("SELECT SUM(id) FROM t", "15");
}

/* ---------------- 2. NULL 语义 ---------------- */

static void test_null_semantics(void)
{
    printf("  [2] NULL 语义\n");

    exec_ok("CREATE TABLE n (v INT)");
    exec_ok("INSERT INTO n (v) VALUES (1), (NULL), (3), (NULL)");

    expect_eq("SELECT COUNT(*) FROM n", "4");        /* 数行 */
    expect_eq("SELECT COUNT(v) FROM n", "2");        /* 数非 NULL */
    expect_eq("SELECT SUM(v) FROM n", "4");          /* NULL 不参与 */
    expect_eq("SELECT AVG(v) FROM n", "2.0");
    expect_eq("SELECT MIN(v) FROM n", "1");
    expect_eq("SELECT MAX(v) FROM n", "3");

    /* 全 NULL */
    exec_ok("CREATE TABLE allnull (v INT)");
    exec_ok("INSERT INTO allnull (v) VALUES (NULL), (NULL)");

    expect_eq("SELECT COUNT(*) FROM allnull", "2");
    expect_eq("SELECT COUNT(v) FROM allnull", "0");
    expect_eq("SELECT SUM(v) FROM allnull", "NULL");
    expect_eq("SELECT AVG(v) FROM allnull", "NULL");
    expect_eq("SELECT MIN(v) FROM allnull", "NULL");
    expect_eq("SELECT MAX(v) FROM allnull", "NULL");
}

/* ---------------- 3. 空表 ---------------- */

static void test_empty_table(void)
{
    printf("  [3] 空表\n");

    exec_ok("CREATE TABLE e (g INT, v INT)");

    /* 无 GROUP BY：恒 1 行 */
    expect_eq("SELECT COUNT(*) FROM e", "0");
    expect_eq("SELECT SUM(v) FROM e", "NULL");
    expect_eq("SELECT AVG(v) FROM e", "NULL");

    /* 有 GROUP BY：0 行 */
    expect_eq("SELECT g, COUNT(*) FROM e GROUP BY g", "");
}

/* ---------------- 4. GROUP BY ---------------- */

static void test_group_by(void)
{
    printf("  [4] GROUP BY\n");

    exec_ok("CREATE TABLE g (k INT, v INT)");
    exec_ok("INSERT INTO g (k, v) VALUES "
            "(2, 20), (1, 10), (2, 21), (3, 30), (1, 11), (NULL, 99)");

    /* 输出按组键排序：NULL 在最前（与 SQLite 一致），然后按数值 */
    expect_eq("SELECT k, COUNT(*), SUM(v) FROM g GROUP BY k",
              "NULL,1,99|1,2,21|2,2,41|3,1,30");

    /* 只要聚合，不带分组列 */
    expect_eq("SELECT COUNT(*) FROM g GROUP BY k", "1|2|2|1");

    /* 多列分组 */
    exec_ok("CREATE TABLE g2 (a INT, b TEXT, v INT)");
    exec_ok("INSERT INTO g2 (a, b, v) VALUES "
            "(1, 'x', 10), (1, 'y', 20), (1, 'x', 30), (2, 'x', 40)");

    expect_eq("SELECT a, b, COUNT(*), SUM(v) FROM g2 GROUP BY a, b",
              "1,x,2,40|1,y,1,20|2,x,1,40");
}

/* ---------------- 5. GROUP BY + WHERE ---------------- */

static void test_group_by_where(void)
{
    printf("  [5] GROUP BY + WHERE\n");

    exec_ok("CREATE TABLE w (k INT, v INT)");
    exec_ok("INSERT INTO w (k, v) VALUES "
            "(1, 10), (1, 20), (2, 30), (2, 40), (3, 50)");

    expect_eq("SELECT k, COUNT(*), SUM(v) FROM w WHERE v >= 20 GROUP BY k",
              "1,1,20|2,2,70|3,1,50");

    /* WHERE 命中空集 → 无分组 → 0 行 */
    expect_eq("SELECT k, COUNT(*) FROM w WHERE v > 1000 GROUP BY k", "");

    /* 无 GROUP BY + WHERE 命中空集 → 仍是 1 行 */
    expect_eq("SELECT COUNT(*) FROM w WHERE v > 1000", "0");
    expect_eq("SELECT SUM(v) FROM w WHERE v > 1000", "NULL");
}

/* ---------------- 6. 文本分组的键编码 ---------------- */

static void test_text_group_key(void)
{
    printf("  [6] 文本分组的键编码\n");

    exec_ok("CREATE TABLE s (t TEXT)");
    exec_ok("INSERT INTO s (t) VALUES ('a'), ('ab'), ('a'), ('b'), (NULL)");

    /*
     * 键编码的两个坑：
     *   1. "a" 与 "ab" 是前缀关系 —— 不加终止符的话键会一样 → 两组合一
     *   2. NULL 与文本 "NULL" 必须分开 —— 靠类型字节区分
     * 这里 'a' 出现两次 → 2 行一组。
     */
    expect_eq("SELECT t, COUNT(*) FROM s GROUP BY t", "NULL,1|a,2|ab,1|b,1");
}

/* ---------------- 7. 哈希表扩容（组数 > 64） ---------------- */

static void test_hash_growth(void)
{
    printf("  [7] 哈希表扩容\n");

    exec_ok("CREATE TABLE many (k INT)");
    exec_ok("BEGIN");
    for (int base = 0; base < 200; base += 16) {
        char sql[512];
        int n = snprintf(sql, sizeof(sql), "INSERT INTO many (k) VALUES ");
        for (int i = 0; i < 16; i++)
            n += snprintf(sql + n, sizeof(sql) - (size_t)n, "%s(%d)",
                          i ? "," : "", base + i);
        exec_ok(sql);
    }
    exec_ok("COMMIT");

    /* 13 轮 × 16 行 = 208 组 → 表从 64 一路扩到 512，扩容路径必须正确 */
    expect_rows("SELECT k FROM many GROUP BY k", 208);
    expect_eq("SELECT COUNT(*) FROM many", "208");
}

/* ---------------- 8. SUM 整数溢出报错 ---------------- */

static void test_sum_overflow(void)
{
    printf("  [8] SUM 整数溢出报错\n");

    /* BIGINT（8 字节）才装得下 INT64_MAX */
    exec_ok("CREATE TABLE big (v BIGINT)");
    exec_ok("INSERT INTO big (v) VALUES "
            "(9223372036854775807), (1), (0)");

    /*
     * 整数和溢出 → 报错（对齐 SQLite 的 "integer overflow"）。
     * 不能转浮点 —— 那是静默丢精度：
     *   9223372036854775807 + 2 = 9223372036854775809
     * 转 double 是 ...808.0，差 1 但"看起来对"。
     */
    expect_err("SELECT SUM(v) FROM big", "溢出");

    /* 不溢出就正常出整数 */
    exec_ok("CREATE TABLE ok (v INT)");
    exec_ok("INSERT INTO ok (v) VALUES (10), (20)");
    expect_eq("SELECT SUM(v) FROM ok", "30");
}

/* ---------------- 9. 形状错误必须明确报错 ---------------- */

static void test_shape_errors(void)
{
    printf("  [9] 形状错误明确报错\n");

    /* 投影非分组列 → 报错（SQLite 允许但不确定，本版明确拒绝） */
    expect_err("SELECT v, COUNT(*) FROM g GROUP BY k", "非分组列");

    /* 无 GROUP BY 时投影普通列 + 聚合 → 报错 */
    expect_err("SELECT v, COUNT(*) FROM g", "只能有聚合函数");

    /* 无 GROUP BY、无聚合的普通查询是合法的（不走聚合路径） */
    expect_rows("SELECT v FROM g", 6);

    /* SELECT * 与聚合同现 → 报错 */
    expect_err("SELECT * FROM g GROUP BY k", "SELECT *");

    /* 不支持的函数 */
    expect_err("SELECT UPPER(k) FROM g", "不支持的函数");

    /* SUM(*) 不合法 */
    expect_err("SELECT SUM(*) FROM g", "不合法");
}

/* ---------------- 10. A4：组数超限明确报错 ---------------- */

static void test_group_limit(void)
{
    printf("  [10] 组数超限明确报错（A4）\n");

    exec_ok("CREATE TABLE lim (k INT)");
    exec_ok("BEGIN");
    for (int base = 0; base < 4200; base += 16) {
        char sql[512];
        int n = snprintf(sql, sizeof(sql), "INSERT INTO lim (k) VALUES ");
        for (int i = 0; i < 16; i++)
            n += snprintf(sql + n, sizeof(sql) - (size_t)n, "%s(%d)",
                          i ? "," : "", base + i);
        exec_ok(sql);
    }
    exec_ok("COMMIT");

    /* 4200 组 > 上限（4096）→ 必须明确报错，不能静默截断 */
    Result *r = NULL;
    db_err e = db_exec(db, "SELECT k, COUNT(*) FROM lim GROUP BY k", &r);
    if (r)
        db_result_free(r);

    if (e == DB_OK) {
        printf("    FAIL 组数超限没有报错\n");
        failures++;
    } else {
        const char *msg = db_last_error(db);
        if (!msg || !strstr(msg, "分组数超上限")) {
            printf("    FAIL 错误信息不是\"分组数超上限\"，实际: %s\n",
                   msg ? msg : "(空)");
            failures++;
        } else if (!strstr(msg, "4096")) {
            /* 上限要写进错误信息 —— 让人知道该缩到多少 */
            printf("    FAIL 错误信息里没有上限数字: %s\n", msg);
            failures++;
        }
    }

    /* 不超限的查询仍然正常（超限不能把表弄坏） */
    expect_eq("SELECT COUNT(*) FROM lim", "4208");
}

/* ---------------- 11. 多连接/事务可见性 ---------------- */

static void test_txn_visibility(void)
{
    printf("  [11] 聚合看的是快照里的行\n");

    exec_ok("CREATE TABLE mv (k INT, v INT)");
    exec_ok("BEGIN");
    exec_ok("INSERT INTO mv (k, v) VALUES (1, 10)");
    exec_ok("COMMIT");

    expect_eq("SELECT COUNT(*), SUM(v) FROM mv", "1,10");
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    remove(PATH);
    remove("test/tmp/agg.db-wal");

    db_err e = db_open(PATH, &db);
    if (e != DB_OK) {
        printf("打开失败: %s\n", db_strerror(e));
        return 1;
    }

    test_basic_agg();
    test_null_semantics();
    test_empty_table();
    test_group_by();
    test_group_by_where();
    test_text_group_key();
    test_hash_growth();
    test_sum_overflow();
    test_shape_errors();
    test_group_limit();
    test_txn_visibility();

    db_close(db);

    if (failures == 0)
        printf("  11 项验收全部通过\n");
    else
        printf("  %d 项失败\n", failures);
    return failures ? 1 : 0;
}
