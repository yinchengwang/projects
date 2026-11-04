/*
 * repl_ts.c —— REPL 的 TS 命令（v1.5 步骤 5）
 *
 * 设计说明见 repl_ts.h。
 *
 * 【参数怎么切】
 *
 *   TS INSERT <time_ms> <tag> <value>   两个整数 + 一个浮点
 *   TS RETENTION <seconds>              一个非负整数
 *
 * 与 GEO 一样是定长数值参数 ⇒ 可以一个一个往下切，
 * 不需要 FT 那种"从行尾倒着找最后一个 token"的写法。
 *
 * 【为什么 TS PARTS 走 SQL 而不是专用 API】
 *
 * 公开 API 里只有"分区个数"，没有"分区列表"——
 * 逐个分区取信息是**内部调用**，暴露到公开层就得承诺内存生命周期
 * （调用者得负责 free 什么？）。
 * 而分区元数据本来就是视图（ts_partitions 虚拟表），
 * REPL 这一层直接查视图最自然，顺带也验证了 SPI 那条路是通的。
 */

#include "repl_ts.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 输出流（默认 stdout，测试里可重定向到临时文件） */
static FILE *g_out = NULL;

static FILE *repl_out(void)
{
    return g_out ? g_out : stdout;
}

FILE *repl_ts_set_output(FILE *f)
{
    FILE *old = g_out;

    g_out = f;
    return old;
}

/* 跳过行首的空白 */
static const char *skip_ws(const char *s)
{
    while (*s == ' ' || *s == '\t') {
        s++;
    }
    return s;
}

/*
 * 切一个整数（time_ms / tag / seconds）。
 *
 * 用 strtoll：time_ms 是 i64 毫秒时间戳，秒级 × 1000 会超过 int32，
 * 用 int 会溢出。负数允许（时间戳可以是 1970 年之前）。
 */
static int take_i64(const char **p, int64_t *out)
{
    const char *s = skip_ws(*p);
    char *endp = NULL;
    long long v;

    v = strtoll(s, &endp, 10);
    if (endp == s) {
        return 0;
    }
    *out = (int64_t)v;
    *p = endp;
    return 1;
}

/* 切一个非负整数（retention 秒数） */
static int take_u32(const char **p, uint32_t *out)
{
    const char *s = skip_ws(*p);
    char *endp = NULL;
    unsigned long long v;

    if (*s == '-') {
        return 0;
    }
    v = strtoull(s, &endp, 10);
    if (endp == s) {
        return 0;
    }
    if (v > 0xFFFFFFFFULL) {
        return 0;
    }
    *out = (uint32_t)v;
    *p = endp;
    return 1;
}

/* 切一个浮点数（读数 value） */
static int take_double(const char **p, double *out)
{
    const char *s = skip_ws(*p);
    char *endp = NULL;
    double v;

    v = strtod(s, &endp);
    if (endp == s) {
        return 0;
    }
    *out = v;
    *p = endp;
    return 1;
}

/*
 * 分区状态的名字。
 *
 * 目录里的 state 是 u32 枚举（ACTIVE / SEALED / DROPPED），
 * 直接打数字也能用，但人看的是名字 —— 打印时两样都给。
 * 未知值原样打数字，不猜。
 */
static const char *part_state_name(const char *num)
{
    if (strcmp(num, "0") == 0) {
        return "ACTIVE";
    }
    if (strcmp(num, "1") == 0) {
        return "SEALED";
    }
    if (strcmp(num, "2") == 0) {
        return "DROPPED";
    }
    return "UNKNOWN";
}

/* ---------------- TS PARTS ---------------- */

static int ts_cmd_parts(DB *db)
{
    Result *r = NULL;
    db_err e;
    int nrow, ncol, i;

    e = db_exec(db,
                "SELECT start_ms, end_ms, state, n_rows, page_count "
                "FROM ts_partitions",
                &r);
    if (e != DB_OK) {
        fprintf(repl_out(), "查分区失败: %s\n", db_strerror(e));
        return 1;
    }

    nrow = db_result_rows(r);
    ncol = db_result_cols(r);

    for (i = 0; i < nrow; i++) {
        const char *start = db_result_value(r, i, 0);
        const char *end = db_result_value(r, i, 1);
        const char *state = db_result_value(r, i, 2);
        const char *rows = (ncol > 3) ? db_result_value(r, i, 3) : "?";
        const char *pages = (ncol > 4) ? db_result_value(r, i, 4) : "?";

        fprintf(repl_out(),
                "  start_ms=%s  end_ms=%s  state=%s(%s)  rows=%s  pages=%s\n",
                start, end, state, part_state_name(state), rows, pages);
    }
    fprintf(repl_out(), "(%d 个分区)\n", nrow);

    db_result_free(r);
    return 1;
}

/* ---------------- 入口 ---------------- */

int handle_ts(DB *db, char *line)
{
    line = (char *)skip_ws(line);
    if (strncmp(line, "TS", 2) != 0) {
        return 0;
    }
    line += 2;
    line = (char *)skip_ws(line);

    /* ---------------- TS CREATE ---------------- */
    if (strncmp(line, "CREATE", 6) == 0) {
        db_err e = db_ts_create(db);

        if (e != DB_OK) {
            fprintf(repl_out(), "建时序存储失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "时序存储已建\n");
        }
        return 1;
    }

    /* ---------------- TS EXISTS ---------------- */
    if (strncmp(line, "EXISTS", 6) == 0) {
        fprintf(repl_out(), "%s\n", db_ts_exists(db) ? "有时序存储" : "没有时序存储");
        return 1;
    }

    /* ---------------- TS INSERT <time_ms> <tag> <value> ---------------- */
    if (strncmp(line, "INSERT", 6) == 0) {
        const char *p = line + 6;
        db_ts_reading rd;
        db_err e;

        if (!take_i64(&p, &rd.time) || !take_i64(&p, &rd.tag) ||
            !take_double(&p, &rd.value)) {
            fprintf(repl_out(), "用法: TS INSERT <time_ms> <tag> <value>\n");
            return 1;
        }

        e = db_ts_insert(db, &rd);
        if (e != DB_OK) {
            fprintf(repl_out(), "插入失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "已插入 time=%lld tag=%lld value=%g\n",
                    (long long)rd.time, (long long)rd.tag, rd.value);
        }
        return 1;
    }

    /* ---------------- TS COUNT ---------------- */
    if (strncmp(line, "COUNT", 5) == 0) {
        uint64_t n = 0;
        db_err e = db_ts_count(db, &n);

        if (e != DB_OK) {
            fprintf(repl_out(), "计数失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "%llu\n", (unsigned long long)n);
        }
        return 1;
    }

    /* ---------------- TS PARTS ---------------- */
    if (strncmp(line, "PARTS", 5) == 0) {
        return ts_cmd_parts(db);
    }

    /* ---------------- TS RETENTION <seconds> ---------------- */
    if (strncmp(line, "RETENTION", 9) == 0) {
        const char *p = line + 9;
        uint32_t sec;
        db_err e;

        if (!take_u32(&p, &sec)) {
            fprintf(repl_out(), "用法: TS RETENTION <seconds>\n");
            return 1;
        }
        e = db_ts_retention(db, sec);
        if (e != DB_OK) {
            fprintf(repl_out(), "TTL 回收失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "TTL 回收完成（保留 %u 秒）\n", (unsigned)sec);
        }
        return 1;
    }

    /* ---------------- TS READS ---------------- */
    if (strncmp(line, "READS", 5) == 0) {
        fprintf(repl_out(), "%llu\n", (unsigned long long)db_ts_page_reads(db));
        return 1;
    }

    fprintf(repl_out(),
            "用法: TS CREATE / EXISTS / INSERT / COUNT / PARTS / RETENTION / READS\n");
    return 1;
}
