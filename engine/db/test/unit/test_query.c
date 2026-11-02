/*
 * test_query.c — 查询层验收（走公开 API）
 *
 * 9 项：建表/插入/查询/WHERE/三值逻辑/LIMIT/更新/删除/错误处理
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

#define PATH "test/tmp/query.db"

static DB *db;

/* 执行不关心结果的语句 */
static void exec_ok(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 执行失败: %s → %s\n", sql, db_strerror(e));
        failures++;
    }
    if (r)
        db_result_free(r);
}

/* 执行查询，把结果压成 "r1c1,r1c2|r2c1,r2c2" 便于断言 */
static char *q(const char *sql)
{
    static char buf[8192];
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);

    buf[0] = '\0';
    if (e != DB_OK) {
        snprintf(buf, sizeof(buf), "<ERR:%d>", (int)e);
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

/* 返回受影响行数；出错返回 -1 */
/* 结果行数 */
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

static int affected(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_affected(r);
    db_result_free(r);
    return n;
}

static db_err err_of(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e;
}

int main(void)
{
    remove(PATH);
    printf("test_query:\n");

    CHECK(db_open(PATH, &db) == DB_OK);

    /* ---- 1) 建表 ---- */
    exec_ok("CREATE TABLE t (id INT NOT NULL, name TEXT, age INT);");
    exec_ok("CREATE TABLE empty_t (a INT);");

    /* ---- 2) 插入 ---- */
    CHECK(affected("INSERT INTO t VALUES (1, 'alice', 30);") == 1);
    CHECK(affected("INSERT INTO t VALUES (2, 'bob', 25), (3, NULL, 40);") == 2);
    CHECK(affected("INSERT INTO t VALUES (4, 'dave', 50), (5, 'eve', 60), (6, 'frank', 70);") == 3);

    /* ---- 3) 查询 ---- */
    CHECK(strcmp(q("SELECT * FROM t WHERE id = 1;"), "1,alice,30") == 0);
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 3;"), "NULL") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age > 45;"), "4|5|6") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age >= 50 AND age < 70;"), "4|5") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 1 OR id = 2;"), "1|2") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE NOT (age > 45);"), "1|2|3") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE name = 'bob';"), "2") == 0);

    /* ---- 4) NULL 与三值逻辑 ---- */
    CHECK(strcmp(q("SELECT id FROM t WHERE name IS NULL;"), "3") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE name IS NOT NULL;"), "1|2|4|5|6") == 0);
    /* NULL = 'x' 结果是 NULL，不匹配 */
    CHECK(strcmp(q("SELECT id FROM t WHERE name = 'zzz';"), "") == 0);
    /* false AND NULL = false；true OR NULL = true */
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 3 AND name = 'zzz';"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 3 OR name = 'zzz';"), "3") == 0);

    /*
     * ---- 4b) 真 AND 空 = 空（v1.1 步骤 6 反向验证补的） ----
     *
     * 【为什么原来抓不到】
     * 顶层 WHERE 里"空"和"假"都是**不匹配**，所以 AND 真值表里
     * (真, 空) 被写错成"假"也看不出任何差别。
     * 只有 NOT（或 IS NULL）能把两者区分开 —— 而原来的测试没有这两条。
     *
     * 这是反向验证的价值：注入一个 bug，测试不红，
     * 说明**测试有盲区**，不是"bug 不严重"。
     *
     * 只有 id=3 那行的 name 是 NULL（其余行是"假"不是"空"）：
     *
     *   id=3   : (id>0)=真, (name='zzz')=空 → 真 AND 空 = 空 → NOT 空 = 空 → **不匹配**
     *   其余行 : (id>0)=真, (name='zzz')=假 → 真 AND 假 = 假 → NOT 假 = 真 → 匹配
     *
     * 所以正确结果是 1,2,4,5,6（少了 id=3）。
     * 若把"真 AND 空"错写成"假"，id=3 会冒出来 → 结果变成 6 行 → 测试红 ✓
     */
    CHECK(strcmp(q("SELECT id FROM t WHERE NOT (id > 0 AND name = 'zzz');"),
                 "1|2|4|5|6") == 0);

    /*
     * 同一件事的另一条路：把"空"显式判出来。
     * 这条走的是**逐行路径**（IS NULL 的左边不是列），
     * 上一条走的是**列式路径** —— 两条路都要被覆盖。
     *
     * 只有 id=3 的表达式结果是"空"，其余行是"假"（IS NULL 为假）。
     * 若把"真 AND 空"错写成"假"，id=3 就不见了 → 结果变成空集 → 测试红 ✓
     */
    CHECK(strcmp(q("SELECT id FROM t WHERE (id > 0 AND name = 'zzz') IS NULL;"),
                 "3") == 0);

    /*
     * ---- 4c) 列式路径的"真 AND 空" ----
     *
     * 【为什么还要单独一条】
     * 上面两条的右操作数是 `name`（TEXT）—— 列式求值不支持文本比较，
     * 整条语句退回**逐行路径**。而列式路径的 AND 真值表是**另一份实现**
     * （exec_eval.c 的 AND3 表），不测它就等于没测。
     *
     * 列式要求**两边都是数值列**，所以另建一张带数值 NULL 的小表。
     *
     *   (1, NULL): a>0 真, b=99 空 → 真 AND 空 = 空 → NOT 空 = 空 → 不匹配
     *   (1, 5)   : a>0 真, b=99 假 → 真 AND 假 = 假 → NOT 假 = 真 → 匹配
     *
     * 若把"真 AND 空"错写成"假"，第一行会冒出来（2 行）→ 测试红 ✓
     */
    exec_ok("CREATE TABLE tri (a INT, b INT);");
    exec_ok("INSERT INTO tri VALUES (1, NULL), (1, 5);");
    CHECK(count_rows("SELECT a FROM tri WHERE NOT (a > 0 AND b = 99);") == 1);
    /* 同一份语义再验一次正向的（真 AND 空 在顶层 = 不匹配，两条路径都一样） */
    CHECK(count_rows("SELECT a FROM tri WHERE a > 0 AND b = 99;") == 0);

    /* ---- 5) LIMIT ---- */
    CHECK(strcmp(q("SELECT id FROM t LIMIT 2;"), "1|2") == 0);
    CHECK(strcmp(q("SELECT id FROM t LIMIT 0;"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age > 0 LIMIT 3;"), "1|2|3") == 0);

    /* ---- 6) 更新 ---- */
    CHECK(affected("UPDATE t SET age = 99 WHERE id = 2;") == 1);
    CHECK(strcmp(q("SELECT age FROM t WHERE id = 2;"), "99") == 0);
    CHECK(affected("UPDATE t SET name = 'ALL' WHERE age > 45;") == 4);  /* 99/50/60/70 */
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 4;"), "ALL") == 0);

    /* ---- 7) 删除 ---- */
    CHECK(affected("DELETE FROM t WHERE age = 99;") == 1);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 2;"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t;"), "1|3|4|5|6") == 0);

    /* ---- 8) 错误处理 ---- */
    CHECK(err_of("SELECT * FROM nope;") == DB_E_NOTFOUND);
    CHECK(err_of("SELECT nope FROM t;") == DB_E_NOTFOUND);
    CHECK(err_of("SELEC * FROM t;") == DB_E_PARSE);
    CHECK(err_of("SELECT * FROM t WHERE;") == DB_E_PARSE);
    CHECK(err_of("INSERT INTO t VALUES (1);") == DB_E_INVAL);        /* 列数不匹配 */
    CHECK(err_of("INSERT INTO t VALUES (NULL, 'x', 1);") == DB_E_INVAL);  /* id NOT NULL */

    /* ---- 9) 重启后数据还在 ---- */
    db_close(db);
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(strcmp(q("SELECT id FROM t;"), "1|3|4|5|6") == 0);
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 1;"), "alice") == 0);
    CHECK(strcmp(q("SELECT a FROM empty_t;"), "") == 0);

    db_close(db);

    if (failures == 0) {
        printf("  9 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
