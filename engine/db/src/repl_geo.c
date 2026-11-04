/*
 * repl_geo.c — REPL 的 GEO 命令（v1.4 步骤 5）
 *
 * 设计说明见 repl_geo.h。
 *
 * 【参数怎么切】
 *
 *   GEO INSERT <x> <y>               两个浮点
 *   GEO WINDOW <minx> <miny> <maxx> <maxy>   四个浮点
 *   GEO KNN <x> <y> <k>              两个浮点 + 一个整数
 *
 * 与 FT 的"正文是自由文本"不同，GEO 的参数全是定长数值 ——
 * 所以可以一个一个 strtof 往下切，不需要"从行尾倒着找最后一个 token"那套。
 *
 * 【输出缓冲区怎么定】
 *
 * 窗口和 KNN 的命中数事先不知道（窗口要看数据分布，KNN 还要扣掉死条目）。
 * 公开 API 在装不下时返回 DB_E_NOSPACE，**不静默截断** ——
 * 所以这里的做法是：撞到 NOSPACE 就把缓冲区翻倍重试。
 *
 * 【为什么不给"给个上限"这个选项】
 *
 * 真给上限就得截断，截断了用户就以为"就这些点"——
 * 那比报错更坏。翻倍重试的代价只是多几次拷贝，
 * 而空间查询的返回量本来就是数据决定的。
 */

#include "repl_geo.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* 缓冲区初始大小 */
#define GEO_BUF_INIT 64

/* 翻倍的上限：最多扩到 1<<20 个元素（4MB / 8MB 量级），再大就报错 */
#define GEO_BUF_MAX (1u << 20)

/* 输出流（默认 stdout，测试里可重定向到临时文件） */
static FILE *g_out = NULL;

static FILE *repl_out(void)
{
    return g_out ? g_out : stdout;
}

FILE *repl_geo_set_output(FILE *f)
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
 * 切一个浮点数。
 *
 * 成功 → 返回 1，*out 存值，*rest 指向后面的空白。
 * 失败 → 返回 0（调用方打印用法）。
 */
static int take_float(const char **p, float *out)
{
    const char *s = skip_ws(*p);
    char *endp = NULL;
    float v;

    v = strtof(s, &endp);
    if (endp == s) {
        return 0;
    }
    *out = v;
    *p = endp;
    return 1;
}

/*
 * 切一个非负整数（row_id / k）。
 *
 * 用 strtoull 不用 strtol：row_id 是 uint64_t，
 * 用 long 在 32 位平台上有溢出风险，且负数没有意义。
 */
static int take_u64(const char **p, uint64_t *out)
{
    const char *s = skip_ws(*p);
    char *endp = NULL;
    unsigned long long v;

    if (*s == '-') {
        return 0;      /* 负数不接受 */
    }
    v = strtoull(s, &endp, 10);
    if (endp == s) {
        return 0;
    }
    *out = (uint64_t)v;
    *p = endp;
    return 1;
}

/* ---------------- GEO WINDOW ---------------- */

static int geo_cmd_window(DB *db, const char *line)
{
    float minx, miny, maxx, maxy;
    uint32_t *buf = NULL;
    uint32_t cap = GEO_BUF_INIT;
    uint32_t n = 0;
    db_err e;

    if (!take_float(&line, &minx) || !take_float(&line, &miny) ||
        !take_float(&line, &maxx) || !take_float(&line, &maxy)) {
        fprintf(repl_out(), "用法: GEO WINDOW <minx> <miny> <maxx> <maxy>\n");
        return 1;
    }

    for (;;) {
        uint32_t *nb = (uint32_t *)realloc(buf, (size_t)cap * sizeof(uint32_t));
        if (!nb) {
            fprintf(repl_out(), "内存不足\n");
            free(buf);
            return 1;
        }
        buf = nb;

        n = 0;
        e = db_geo_window(db, minx, miny, maxx, maxy, buf, cap, &n);
        if (e == DB_OK) {
            break;
        }
        if (e != DB_E_NOSPACE) {
            fprintf(repl_out(), "窗口查询失败: %s\n", db_strerror(e));
            free(buf);
            return 1;
        }
        if (cap >= GEO_BUF_MAX) {
            fprintf(repl_out(), "命中数超过上限 %u，放弃\n", GEO_BUF_MAX);
            free(buf);
            return 1;
        }
        cap *= 2;
    }

    for (uint32_t i = 0; i < n; i++) {
        fprintf(repl_out(), "  row_id=%u\n", buf[i]);
    }
    fprintf(repl_out(), "(%u 个)\n", n);
    free(buf);
    return 1;
}

/* ---------------- GEO KNN ---------------- */

static int geo_cmd_knn(DB *db, const char *line)
{
    float qx, qy;
    uint64_t k64;
    uint32_t k;
    db_geo_hit *buf = NULL;
    uint32_t cap = GEO_BUF_INIT;
    uint32_t n = 0;
    db_err e;

    if (!take_float(&line, &qx) || !take_float(&line, &qy) ||
        !take_u64(&line, &k64)) {
        fprintf(repl_out(), "用法: GEO KNN <x> <y> <k>\n");
        return 1;
    }
    if (k64 == 0 || k64 > 0xFFFFFFFFULL) {
        fprintf(repl_out(), "用法: GEO KNN <x> <y> <k>（k 必须在 1..4294967295 之间）\n");
        return 1;
    }
    k = (uint32_t)k64;

    for (;;) {
        db_geo_hit *nb = (db_geo_hit *)realloc(buf, (size_t)cap * sizeof(db_geo_hit));
        if (!nb) {
            fprintf(repl_out(), "内存不足\n");
            free(buf);
            return 1;
        }
        buf = nb;

        n = 0;
        e = db_geo_knn(db, qx, qy, k, buf, cap, &n);
        if (e == DB_OK) {
            break;
        }
        if (e != DB_E_NOSPACE) {
            fprintf(repl_out(), "近邻查询失败: %s\n", db_strerror(e));
            free(buf);
            return 1;
        }
        if (cap >= GEO_BUF_MAX) {
            fprintf(repl_out(), "命中数超过上限 %u，放弃\n", GEO_BUF_MAX);
            free(buf);
            return 1;
        }
        cap *= 2;
    }

    for (uint32_t i = 0; i < n; i++) {
        fprintf(repl_out(), "  row_id=%llu  dist=%.6f\n",
               (unsigned long long)buf[i].row_id, (double)buf[i].dist);
    }
    fprintf(repl_out(), "(%u 个)\n", n);
    free(buf);
    return 1;
}

/* ---------------- 入口 ---------------- */

int handle_geo(DB *db, char *line)
{
    line = (char *)skip_ws(line);
    if (strncmp(line, "GEO", 3) != 0) {
        return 0;
    }
    line += 3;
    line = (char *)skip_ws(line);

    /* ---------------- GEO CREATE ---------------- */
    if (strncmp(line, "CREATE", 6) == 0) {
        db_err e = db_geo_create(db);

        if (e != DB_OK) {
            fprintf(repl_out(), "建几何存储失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "几何存储已建\n");
        }
        return 1;
    }

    /* ---------------- GEO EXISTS ---------------- */
    if (strncmp(line, "EXISTS", 6) == 0) {
        fprintf(repl_out(), "%s\n", db_geo_exists(db) ? "有几何存储" : "没有几何存储");
        return 1;
    }

    /* ---------------- GEO INSERT <x> <y> ---------------- */
    if (strncmp(line, "INSERT", 6) == 0) {
        const char *p = line + 6;
        float x, y;
        uint64_t rid = 0;
        db_err e;

        if (!take_float(&p, &x) || !take_float(&p, &y)) {
            fprintf(repl_out(), "用法: GEO INSERT <x> <y>\n");
            return 1;
        }
        e = db_geo_insert(db, x, y, &rid);
        if (e != DB_OK) {
            fprintf(repl_out(), "插入失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "已插入 row_id=%llu\n", (unsigned long long)rid);
        }
        return 1;
    }

    /* ---------------- GEO WINDOW ---------------- */
    if (strncmp(line, "WINDOW", 6) == 0) {
        line += 6;
        return geo_cmd_window(db, line);
    }

    /* ---------------- GEO KNN ---------------- */
    if (strncmp(line, "KNN", 3) == 0) {
        line += 3;
        return geo_cmd_knn(db, line);
    }

    /* ---------------- GEO DELETE <row_id> ---------------- */
    if (strncmp(line, "DELETE", 6) == 0) {
        const char *p = line + 6;
        uint64_t rid;
        db_err e;

        if (!take_u64(&p, &rid)) {
            fprintf(repl_out(), "用法: GEO DELETE <row_id>\n");
            return 1;
        }
        e = db_geo_delete(db, rid);
        if (e != DB_OK) {
            fprintf(repl_out(), "删除失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "已删除 row_id=%llu\n", (unsigned long long)rid);
        }
        return 1;
    }

    /* ---------------- GEO COUNT ---------------- */
    if (strncmp(line, "COUNT", 5) == 0) {
        uint64_t n = 0;
        db_err e = db_geo_count(db, &n);

        if (e != DB_OK) {
            fprintf(repl_out(), "计数失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "%llu\n", (unsigned long long)n);
        }
        return 1;
    }

    /* ---------------- GEO REINDEX ---------------- */
    if (strncmp(line, "REINDEX", 7) == 0) {
        db_err e = db_geo_reindex(db);

        if (e != DB_OK) {
            fprintf(repl_out(), "重建索引失败: %s\n", db_strerror(e));
        } else {
            fprintf(repl_out(), "索引已重建\n");
        }
        return 1;
    }

    /* ---------------- GEO READS ---------------- */
    if (strncmp(line, "READS", 5) == 0) {
        fprintf(repl_out(), "%llu\n", (unsigned long long)db_geo_page_reads(db));
        return 1;
    }

    fprintf(repl_out(), "用法: GEO CREATE / EXISTS / INSERT / WINDOW / KNN / DELETE / COUNT / REINDEX / READS\n");
    return 1;
}
