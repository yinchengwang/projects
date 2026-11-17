/*
 * query_test_common.h —— 查询层验收系列的共用件
 *
 * 三个测试文件共用这一套小工具：
 *   test_query.c       核心 9 项：建表 / 插入 / 查询 / WHERE / LIMIT / 更新 / 删除 / 错误
 *   test_query_null.c  结果集 NULL 语义（db_result_is_null）
 *   test_query_bulk.c  批量 INSERT 的行数上限
 *
 * 拆开的原因：原来全在一个文件里，加起来 640 多行，而且三块关注的东西
 * 完全不同 —— 核心语义 / 结果集表示 / 解析器容量。一块出问题时，
 * 另外两块的输出会把失败信息淹掉。
 *
 * ⚠️ 三个文件**各自是独立的可执行文件**（run.sh 逐个编译运行），
 * 所以这里的 static 变量与函数在每个二进制里各有一份，不会重复定义。
 *
 * ⚠️ 下方的 `db` / `failures` 由**本头文件**定义（不是各 .c 定义）：
 * 头文件里的辅助函数要用它们，而 C 的文本包含顺序决定了
 * 头文件必须先看到声明。包含本头文件之后，.c 里直接写 `db` 和
 * `failures` 即可，不要再自己声明一遍（否则重复定义）。
 */

#ifndef DB_TEST_QUERY_COMMON_H
#define DB_TEST_QUERY_COMMON_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

/* 连接 + 失败计数：每个测试二进制一份 */
static DB *db = NULL;
static int failures = 0;

/*
 * 断言：失败就记一笔，继续跑完（不中断，一次看到全部失败）。
 */
#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/*
 * 静态函数在三个文件里用到的**子集不同** ——
 * 只用到一部分的文件会报"定义了但没用到"（-Werror 当错误）。
 */
#if defined(__GNUC__)
#define QT_UNUSED __attribute__((unused))
#else
#define QT_UNUSED
#endif

/* 执行不关心结果的语句 */
static QT_UNUSED void exec_ok(const char *sql)
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

/*
 * 执行查询，把结果压成 "r1c1,r1c2|r2c1,r2c2" 便于断言。
 *
 * 返回静态缓冲，**下一次调用会覆盖** —— 断言里不要同时调两次。
 */
static QT_UNUSED char *qt_dump(const char *sql)
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

/* 结果行数；出错返回 -1 */
static QT_UNUSED int count_rows(const char *sql)
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

/* 返回受影响行数；出错返回 -1 */
static QT_UNUSED int affected(const char *sql)
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

/* 只关心错误码 */
static QT_UNUSED db_err err_of(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e;
}

/*
 * 收尾：打印结论并给出退出码。
 *
 * ok_msg 是"全部通过"时的描述，让输出一眼能看出这个二进制验了什么。
 */
static QT_UNUSED int qt_finish(const char *ok_msg)
{
    if (failures == 0) {
        printf("  %s", ok_msg);
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}

#endif /* DB_TEST_QUERY_COMMON_H */
