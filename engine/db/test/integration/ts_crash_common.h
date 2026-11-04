/*
 * ts_crash_common.h —— 时序崩溃恢复验收的共用件（v1.5 步骤 6 · T10 · A7）
 *
 * 两个测试文件共用：
 *   test_ts_crash.c      —— db 层：真 WAL、真断电（db_abandon）
 *   test_ts_crash_mid.c  —— 模态层：崩在**多页操作的中间**
 *
 * 共用的只有"读数"这一小块东西：一条读数的形状、造数据的规则、
 * 以及"从 SQL 里读回来逐条对照"的比较函数。
 *
 * ⚠️ 为什么浮点比**位模式**而不是比 ==：
 * NaN != NaN，而且编译器可能按"肯定相等"优化。
 * 磁盘上存的是位模式 ⇒ 这里也比位模式：写什么位，读回什么位。
 *
 * ⚠️ 为什么造的值都是"二进制精确"的：
 * 1/8 的整数倍（0.125 / 0.5 / 1.5 …）在 double 里**精确可表示**，
 * 不存在"四舍五成另一个数"的干扰（坑 M6-38）。
 */

#ifndef DB_TEST_TS_CRASH_COMMON_H
#define DB_TEST_TS_CRASH_COMMON_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"

#include "common/types.h"

/*
 * 静态函数在两个测试文件里用到的**子集不同** ——
 * 只用 db 层的那一份会报"定义了但没用到"（-Werror 会当错误）。
 * 所以统一标成"可能不用"。
 */
#if defined(__GNUC__)
#define TS_CRASH_UNUSED __attribute__((unused))
#else
#define TS_CRASH_UNUSED
#endif

/* 一天（毫秒） */
#define DAY_MS 86400000LL

/* 今天这个时间窗里的基准点（今天 + 1 小时） */
#define BASE_OFFSET_MS 3600000LL

/* ---------------- 一条读数 ---------------- */

typedef struct {
    int64_t time;
    int64_t tag;
    double value;
} Row;

/* 浮点位模式 */
static TS_CRASH_UNUSED u64 ts_crash_dbits(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
}

/* 排序用：先按时间，再按标签 */
static TS_CRASH_UNUSED int ts_crash_row_cmp(const void *a, const void *b)
{
    const Row *x = (const Row *)a;
    const Row *y = (const Row *)b;

    if (x->time != y->time) {
        return (x->time < y->time) ? -1 : 1;
    }
    if (x->tag != y->tag) {
        return (x->tag < y->tag) ? -1 : 1;
    }
    return 0;
}

/* 造一批读数：时间等间隔、标签有重复、值是二进制精确的分数 */
static TS_CRASH_UNUSED void ts_crash_fill(Row *rs, int n, int64_t t0, int64_t step)
{
    int i;

    for (i = 0; i < n; i++) {
        rs[i].time = t0 + (int64_t)i * step;
        rs[i].tag = (int64_t)(i % 7);
        rs[i].value = (double)(i % 977) * 0.125;
    }
}

/* 现在的毫秒时间戳 */
static TS_CRASH_UNUSED int64_t ts_crash_now_ms(void)
{
    return (int64_t)time(NULL) * 1000;
}

/*
 * 今天这个时间窗里的基准点。
 *
 * 取"今天 + 1 小时"：不管现在几点，它都落在**今天这一个分区**里
 * （除非正好在跨日的那一秒，最多多分出一个分区）。
 */
static TS_CRASH_UNUSED int64_t ts_crash_today_base(void)
{
    return (ts_crash_now_ms() / DAY_MS) * DAY_MS + BASE_OFFSET_MS;
}

/* ---------------- SQL 读回 ---------------- */

/*
 * 查 ts_readings，把结果收进行数组。返回 0 成功；-1 失败（SQL 报错）。
 */
static TS_CRASH_UNUSED int ts_crash_fetch(DB *db, const char *sql, Row *out,
                                          int cap, int *nout)
{
    Result *r = NULL;
    db_err e;
    int nrow, i;

    *nout = 0;
    e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) {
            db_result_free(r);
        }
        return -1;
    }

    nrow = db_result_rows(r);
    if (nrow > cap) {
        db_result_free(r);
        return -1;
    }
    for (i = 0; i < nrow; i++) {
        out[i].time = (int64_t)strtoll(db_result_value(r, i, 0), NULL, 10);
        out[i].tag = (int64_t)strtoll(db_result_value(r, i, 1), NULL, 10);
        out[i].value = strtod(db_result_value(r, i, 2), NULL);
    }
    db_result_free(r);
    *nout = nrow;
    return 0;
}

/*
 * 两组读数是否逐条相等（先排序再比；浮点比位模式）。
 *
 * ⚠️ **条数对不代表数据对**：
 * 时序最容易出的错是**静默漏读**或者**读串行** ——
 * 分区页链断了一截、目录项的行数与实际不符、列式页少了几行 ——
 * 结果就是少几条、或者某条的值对不上，**而且不报错**。
 * 所以判据只能是"逐条全等"。
 */
static TS_CRASH_UNUSED int ts_crash_rows_equal(Row *a, int na, Row *b, int nb)
{
    int i;

    if (na != nb) {
        return 0;
    }
    qsort(a, (size_t)na, sizeof(Row), ts_crash_row_cmp);
    qsort(b, (size_t)nb, sizeof(Row), ts_crash_row_cmp);
    for (i = 0; i < na; i++) {
        if (a[i].time != b[i].time || a[i].tag != b[i].tag) {
            return 0;
        }
        if (ts_crash_dbits(a[i].value) != ts_crash_dbits(b[i].value)) {
            return 0;
        }
    }
    return 1;
}

#endif /* DB_TEST_TS_CRASH_COMMON_H */
