/*
 * test_expr_func.c —— 通用函数调用表达式 EX_FUNC（v1.6 C5.4 + C5.6）
 *
 * 6 项：
 *   1. 投影里的函数调用能求值（`SELECT fn_double(21) FROM f_small`）
 *   2. 函数实参里的**列**能绑定上（不是空值、不是错列）
 *   3. 函数可以出现在 WHERE 里（小表 → 逐行路径）
 *   4. 函数可以出现在 WHERE 里（**大表 → 列式路径**）
 *   5. 未注册的函数名返回错误，不是崩溃也不是静默 0
 *   6. 嵌套函数调用（函数套函数）能求值
 *
 * 【第 3 与第 4 项为什么必须都测】
 * 执行器有**两条平行的求值实现**：逐行（`exec_scan.c::eval_expr`）
 * 与列式（`exec_eval.c::eval_where`）。小表走逐行、大表走列式 ——
 * 只加一条的症状是「同样的查询在数据量变大后答案变了」，极难定位。
 * 所以本测试建**两张表**（3 行 / 500 行），同一组断言跑两遍。
 *
 * 【为什么用假函数而不直接用 json_match】
 * 值层是 L0，它不许认识任何模态。这里测的是**通用函数调用机制**，
 * 用 `fn_double` / `fn_add` 这种纯假名字才能保证结论与文档模态无关。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 测试用函数 ---------------- */

/* 把实参的整数翻倍 */
static int fn_double(const DbValue *args, int nargs, DbValue *out)
{
    if (nargs != 1)
        return E_INVAL;
    if (args[0].is_null) {
        val_null(out);
        return OK;
    }
    val_int(out, args[0].i * 2);
    return OK;
}

/* 两个实参相加 */
static int fn_add(const DbValue *args, int nargs, DbValue *out)
{
    if (nargs != 2)
        return E_INVAL;
    val_int(out, args[0].i + args[1].i);
    return OK;
}

/* ---------------- 环境 ---------------- */

#define PATH "test/tmp/expr_func.db"

static DB *g_db;

static void exec_ok(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(g_db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 执行失败: %s -> %s (%s)\n", sql, db_strerror(e),
               db_last_error(g_db));
        failures++;
    }
    if (r)
        db_result_free(r);
}

/*
 * 断言一条查询返回的单行单列是期望整数。
 */
static void expect_int(const char *sql, long long want, const char *what)
{
    Result *r = NULL;
    db_err e = db_exec(g_db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL %s: 查询失败 %s (%s)\n", what, db_strerror(e),
               db_last_error(g_db));
        failures++;
        if (r)
            db_result_free(r);
        return;
    }
    if (db_result_rows(r) != 1 || db_result_cols(r) != 1) {
        printf("    FAIL %s: 形状不对 rows=%d cols=%d\n", what,
               db_result_rows(r), db_result_cols(r));
        failures++;
        db_result_free(r);
        return;
    }
    const char *got = db_result_value(r, 0, 0);
    long long v = got ? strtoll(got, NULL, 10) : -1;
    if (v != want) {
        printf("    FAIL %s: 期望 %lld 实际 %lld ('%s')\n", what, want, v,
               got ? got : "(null)");
        failures++;
    }
    db_result_free(r);
}

/*
 * 建两张表并灌数据：
 *   f_small  3 行   （0 / 10 / 20）        → 走逐行路径
 *   f_big    500 行 （0 / 10 / ... / 4990）→ 走列式路径
 */
static void setup_tables(void)
{
    exec_ok("CREATE TABLE f_small (id INT)");
    exec_ok("CREATE TABLE f_big (id INT)");

    char sql[256];
    for (int i = 0; i < 3; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO f_small (id) VALUES (%d)", i * 10);
        exec_ok(sql);
    }
    for (int i = 0; i < 500; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO f_big (id) VALUES (%d)", i * 10);
        exec_ok(sql);
    }
}

int main(void)
{
    printf("test_expr_func:\n");

    remove(PATH);
    remove(PATH "-wal");

    db_err e = db_open(PATH, &g_db);
    if (e != DB_OK) {
        printf("    FAIL 打不开库: %s\n", db_strerror(e));
        return 1;
    }

    /* 注册两个测试函数（值层函数表是进程级全局，注册一次即可） */
    CHECK(val_func_register("fn_double", fn_double) == OK);
    CHECK(val_func_register("fn_add", fn_add) == OK);

    setup_tables();

    /* ================= 1) 投影里的函数调用（常量实参） ================= */
    {
        Result *r = NULL;
        db_err rc = db_exec(g_db, "SELECT fn_double(21) FROM f_small", &r);
        CHECK(rc == DB_OK);
        if (rc == DB_OK && r) {
            CHECK(db_result_rows(r) == 3);
            CHECK(db_result_cols(r) == 1);
            /* 常量实参：每一行都是 42 */
            CHECK(strcmp(db_result_value(r, 0, 0), "42") == 0);
            CHECK(strcmp(db_result_value(r, 2, 0), "42") == 0);
        }
        if (r)
            db_result_free(r);
    }

    /* ================= 2) 实参是**列** —— 必须绑定进 args ================= */
    {
        Result *r = NULL;
        db_err rc = db_exec(g_db, "SELECT fn_double(id) FROM f_small", &r);
        CHECK(rc == DB_OK);
        if (rc == DB_OK && r) {
            CHECK(db_result_rows(r) == 3);
            /* id = 0 / 10 / 20 → 0 / 20 / 40 */
            CHECK(strcmp(db_result_value(r, 0, 0), "0") == 0);
            CHECK(strcmp(db_result_value(r, 1, 0), "20") == 0);
            CHECK(strcmp(db_result_value(r, 2, 0), "40") == 0);
        }
        if (r)
            db_result_free(r);
    }

    /* ================= 3) WHERE 里的函数（小表 → 逐行） ================= */
    expect_int("SELECT COUNT(*) FROM f_small WHERE fn_double(id) = 20", 1,
               "WHERE fn(id) 逐行");

    /* ================= 6) 嵌套函数调用 ================= */
    {
        Result *r = NULL;
        db_err rc = db_exec(g_db, "SELECT fn_add(fn_double(id), 1) FROM f_small", &r);
        CHECK(rc == DB_OK);
        if (rc == DB_OK && r) {
            CHECK(db_result_rows(r) == 3);
            /* id = 0/10/20 → 翻倍 0/20/40 → 加 1 → 1/21/41 */
            CHECK(strcmp(db_result_value(r, 0, 0), "1") == 0);
            CHECK(strcmp(db_result_value(r, 1, 0), "21") == 0);
            CHECK(strcmp(db_result_value(r, 2, 0), "41") == 0);
        }
        if (r)
            db_result_free(r);
    }

    /* ================= 5) 未注册的函数名 —— 必须报错 ================= */
    {
        Result *r = NULL;
        db_err rc = db_exec(g_db, "SELECT no_such_fn(id) FROM f_small", &r);
        CHECK(rc != DB_OK);
        if (r)
            db_result_free(r);
    }

    /* ================= 4) WHERE + 投影在大表上重跑（列式路径） ================= */
    expect_int("SELECT COUNT(*) FROM f_big WHERE fn_double(id) = 20", 1,
               "WHERE fn(id)=20 列式");
    /*
     * f_big 的 id = 0,10,...,4990 ⇒ 翻倍后 0,20,...,9980。
     * 满足 > 800 的是 820,840,...,9980 —— 共 (9980-820)/20 + 1 = 459 行。
     */
    expect_int("SELECT COUNT(*) FROM f_big WHERE fn_double(id) > 800", 459,
               "WHERE fn(id)>800 列式");

    {
        Result *r = NULL;
        db_err rc = db_exec(g_db, "SELECT fn_add(id, 5) FROM f_big WHERE id = 30", &r);
        CHECK(rc == DB_OK);
        if (rc == DB_OK && r) {
            CHECK(db_result_rows(r) == 1);
            CHECK(strcmp(db_result_value(r, 0, 0), "35") == 0);
        }
        if (r)
            db_result_free(r);
    }

    db_close(g_db);

    if (failures == 0) {
        printf("  6 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
