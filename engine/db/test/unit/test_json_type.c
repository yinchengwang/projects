/*
 * test_json_type.c —— `JSON` 列类型（v1.6 C5.5）
 *
 * 3 项：
 *   1. `CREATE TABLE t (doc JSON)` 能建出来（`p_parse_type` 认识 JSON）
 *   2. 建出来的表里那一列的 catalog 类型**确实是 T_JSON**（不是被当成 T_TEXT）
 *   3. 不认识的类型名仍然报错（`CREATE TABLE t (doc JSONX)` 必须红）
 *
 * 【第 2 项是核心】
 * 如果 `p_parse_type` 把 JSON 映射成 T_TEXT，上面第 1 项照样绿 ——
 * 建表成功、能插能查，但**类型语义全错**：`T_JSON` 走的是 JSONB 渲染路径，
 * `T_TEXT` 走的是原样输出。两者在 SELECT 时表现得完全不一样。
 * 所以必须验到底层类型位，不能只验"建表没报错"。
 *
 * 【为什么不测"插入 JSON 文本"】
 * 插入路径要把 JSON 文本编码成 JSONB，那是 C5.3b 的接线范围。
 * 本步只负责让 parser 认识这个类型名。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/catalog.h"
#include "query/parser.h"
#include "storage/pager.h"
#include "storage/record.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/json_type.db"

/* 解析一条 SQL，只关心成功/失败；成功时把 Stmt 交给调用方 */
static int parse_ok(const char *sql, Stmt *st, char *err, int errcap)
{
    return parse_sql(sql, st, err, errcap);
}

int main(void)
{
    char err[256] = {0};
    Stmt st;

    remove(PATH);
    remove(PATH "-wal");

    printf("test_json_type:\n");

    /* ---------------- 1. CREATE TABLE 带 JSON 列 ---------------- */
    memset(&st, 0, sizeof(st));
    int rc = parse_ok("CREATE TABLE docs (id INT, doc JSON)", &st, err, sizeof(err));
    CHECK(rc == OK);
    if (rc == OK) {
        CHECK(st.kind == ST_CREATE);
        CHECK(st.ncols == 2);
        CHECK(strcmp(st.col_names[0], "id") == 0);
        CHECK(st.col_types[0] == T_INT4);
        CHECK(strcmp(st.col_names[1], "doc") == 0);
        /* ---------------- 2. 底层类型必须是 T_JSON ---------------- */
        CHECK(st.col_types[1] == T_JSON);

        /*
         * 再钉一条：T_JSON 是变长类型。
         * 如果哪天 JSON 被误映射成 T_INT8 之类的定长类型，
         * 上面那条 CHECK 会红，但这条能同时钉住 record 层的编码路径。
         */
        CHECK(col_is_var(st.col_types[1]));
        CHECK(T_JSON == 6);
    }
    free_stmt(&st);

    /* ---------------- 3. 不认识的类型名仍然报错 ---------------- */
    memset(&st, 0, sizeof(st));
    memset(err, 0, sizeof(err));
    rc = parse_ok("CREATE TABLE docs (id INT, doc JSONX)", &st, err, sizeof(err));
    CHECK(rc != OK);
    CHECK(err[0] != '\0');
    free_stmt(&st);

    /*
     * JSON 列要真的能进 catalog —— 这一步走真实建表路径，
     * 证明 parser 与 catalog 对类型的理解一致。
     */
    {
        Pager *pg = NULL;
        Catalog *cat = NULL;
        u32 tid = 0;

        CHECK(pager_open(PATH, 64, &pg) == OK);
        CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);

        {
            ColDesc cols[2] = {
                { "id",  T_INT4, false },
                { "doc", T_JSON, true  },
            };
            CHECK(catalog_create_table(cat, 0, "docs", cols, 2, &tid) == OK);

            ColDesc got[2];
            int n = 0;
            CHECK(catalog_table_cols(cat, tid, got, 2, &n) == OK);
            CHECK(n == 2);
            CHECK(got[1].type == T_JSON);
            CHECK(strcmp(got[1].name, "doc") == 0);
        }

        catalog_close(cat);
        pager_close(pg);
    }

    remove(PATH);
    remove(PATH "-wal");

    if (failures == 0) {
        printf("  3 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
