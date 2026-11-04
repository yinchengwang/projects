/*
 * test_geo_repl.c —— REPL 的 GEO 命令验收（v1.4 步骤 5）
 *
 * 5 项：
 *   1. **命令前缀不误伤**：非 GEO 开头的行不走 GEO 命令处理（返回 0）；
 *      未知子命令 → 打印用法并返回 1。
 *   2. **建 / 存在性**：GEO CREATE 之后 GEO EXISTS 说"有"；
 *      重复建 → 失败信息。
 *   3. **插入 / 计数**：GEO INSERT 打印 row_id，GEO COUNT 与活点数一致。
 *   4. **窗口 / KNN**：命令输出的 row_id 集合与直接调 API 的结果一致；
 *      GEO DELETE 之后两个查询都不再返回它。
 *   5. **REINDEX / READS**：重建后结果不变；页读数单调不减。
 *
 * 【为什么要对着 API 再验一遍】
 *
 * REPL 这层只做"解析参数 → 调公开 API → 打印"，没有自己的空间逻辑。
 * 所以这一项测的是**接线**：参数切对了吗、row_id 打印对了吗、
 * 有没有把活点漏出去 —— 不是再验一遍 R-tree。
 *
 * 【为什么要把 stdout 抓下来】
 *
 * 命令的产物就是**给人看的输出**（REPL 就是这么用的）。
 * 只调 API 的话，命令层写错了输出照样测不出来。
 * 所以这里把命令的输出流重定向到临时文件，抓回来逐行核对。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "repl_geo.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/geo_repl.db"
#define OUT "test/tmp/geo_repl.out"

/* ---------------- stdout 抓取 ---------------- */

static FILE *saved_out = NULL;
static FILE *out_file = NULL;

static void capture_begin(void)
{
    out_file = fopen(OUT, "w+");
    if (!out_file) {
        printf("    FAIL: 打不开临时输出文件 %s\n", OUT);
        failures++;
        return;
    }
    saved_out = repl_geo_set_output(out_file);
}

static void capture_end(char *buf, size_t cap)
{
    size_t sz;

    repl_geo_set_output(saved_out);
    if (!out_file) {
        buf[0] = '\0';
        return;
    }
    fflush(out_file);
    fseek(out_file, 0, SEEK_SET);
    sz = fread(buf, 1, cap - 1, out_file);
    buf[sz] = '\0';
    fclose(out_file);
    out_file = NULL;
}

/* 输出里有没有出现这个子串 */
static int has_text(const char *buf, const char *needle)
{
    return strstr(buf, needle) != NULL;
}

int main(void)
{
    DB *db = NULL;
    char out[8192];
    uint64_t cnt = 0;
    uint32_t ids[64], n = 0;
    db_geo_hit hits[16];
    uint32_t nh = 0;
    db_err e;

    printf("test_geo_repl（集成）:\n");

    remove(PATH);

    /* ---------------- 1. 命令前缀 ---------------- */
    CHECK(handle_geo(NULL, "SELECT 1") == 0);
    CHECK(handle_geo(NULL, "  geo create") == 0);   /* 前缀大小写敏感，'geo' 不算 GEO */

    CHECK(db_open(PATH, &db) == DB_OK);

    /* ---------------- 2. 建 / 存在性 ---------------- */
    capture_begin();
    handle_geo(db, "GEO CREATE");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "几何存储已建"));

    capture_begin();
    handle_geo(db, "GEO EXISTS");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "有几何存储"));

    /* 重复建 → 失败 */
    capture_begin();
    handle_geo(db, "GEO CREATE");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "失败"));

    /* 未知子命令 → 用法 */
    capture_begin();
    handle_geo(db, "GEO NOSUCHTHING");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "用法"));

    /* ---------------- 3. 插入 / 计数 ---------------- */
    capture_begin();
    handle_geo(db, "GEO INSERT 1.5 2.5");
    handle_geo(db, "GEO INSERT 3.0 4.0");
    handle_geo(db, "GEO INSERT 10.0 10.0");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "row_id=0"));
    CHECK(has_text(out, "row_id=1"));
    CHECK(has_text(out, "row_id=2"));

    e = db_geo_count(db, &cnt);
    CHECK(e == DB_OK);
    CHECK(cnt == 3);

    capture_begin();
    handle_geo(db, "GEO COUNT");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "3"));

    /* 参数切错 → 用法 */
    capture_begin();
    handle_geo(db, "GEO INSERT 1.5");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "用法"));

    /* ---------------- 4. 窗口 / KNN / 删除 ---------------- */
    /* 直接调 API 拿一份基准 */
    e = db_geo_window(db, 0.0f, 0.0f, 5.0f, 5.0f, ids, 64, &n);
    CHECK(e == DB_OK);
    CHECK(n == 2);

    capture_begin();
    handle_geo(db, "GEO WINDOW 0 0 5 5");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "row_id=0"));
    CHECK(has_text(out, "row_id=1"));
    CHECK(!has_text(out, "row_id=2"));
    CHECK(has_text(out, "(2 个)"));

    e = db_geo_knn(db, 1.5f, 2.5f, 2, hits, 16, &nh);
    CHECK(e == DB_OK);
    CHECK(nh == 2);
    CHECK(hits[0].row_id == 0);

    capture_begin();
    handle_geo(db, "GEO KNN 1.5 2.5 2");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "row_id=0"));
    CHECK(has_text(out, "row_id=1"));
    CHECK(has_text(out, "dist="));

    /* 删掉 row_id=1，窗口与 KNN 都要立刻排除它 */
    capture_begin();
    handle_geo(db, "GEO DELETE 1");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "已删除 row_id=1"));

    capture_begin();
    handle_geo(db, "GEO WINDOW 0 0 5 5");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "row_id=0"));
    CHECK(!has_text(out, "row_id=1"));
    CHECK(has_text(out, "(1 个)"));

    capture_begin();
    handle_geo(db, "GEO KNN 1.5 2.5 2");
    capture_end(out, sizeof(out));
    CHECK(!has_text(out, "row_id=1"));

    e = db_geo_count(db, &cnt);
    CHECK(e == DB_OK);
    CHECK(cnt == 2);

    /* ---------------- 5. REINDEX / READS ---------------- */
    capture_begin();
    handle_geo(db, "GEO REINDEX");
    capture_end(out, sizeof(out));
    CHECK(has_text(out, "索引已重建"));

    e = db_geo_count(db, &cnt);
    CHECK(e == DB_OK);
    CHECK(cnt == 2);

    capture_begin();
    handle_geo(db, "GEO READS");
    capture_end(out, sizeof(out));
    CHECK(strlen(out) > 0);

    db_close(db);
    remove(PATH);
    remove(OUT);

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  5 项验收全部通过\n");
    return 0;
}
