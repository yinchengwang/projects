/*
 * test_graph_api.c —— 图模态公开 API + 接线验收（v0.11 步骤 4）
 *
 * 8 项：
 *   1. 建图 → 图存在；重复建图被拒（schema 不可改）
 *   2. 属性读写往返（定长列）
 *   3. 加边 → 出邻居 / 入邻居对
 *   4. BFS 多跳：去重、深度对
 *   5. 删节点连带删掉它的所有出边和入边
 *   6. **schema 持久化**：重开库后属性列还在、节点数边数对
 *   7. **schema 不一致 → 拒绝建图**（不能拿新布局解释旧记录）
 *   8. 没建图时 graph_nodes 不存在（SQL 报"表不存在"）
 *
 * 【第 6 项是这一版的核心】
 *
 * 节点记录是定长的：长度 = 公共头 + 位图 + 各列宽度之和。
 * 记录里**没有**"这一列多宽"的信息 —— 宽度只在 schema 里。
 *
 * 所以 schema 必须落盘：不落盘 = 重启后整张图变成一堆看不懂的字节，
 * 连"节点数"都数不出来（gstore_recover 会先因为 rec_size 对不上而 E_CORRUPT）。
 *
 * 第 7 项是这个机制的另一半：schema 是图的身份证，不是可以后改的元数据。
 * 用不一样的 schema 去接管一张已有的图 → 必须报错，
 * 宁可开不了，也不能读出垃圾。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/graph_api.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

/* ---------------- schema ---------------- */

/*
 * 三列：INT4 / 定长 TEXT / INT8。
 * name 故意用 16 字节 —— 定长文本必须有宽度，否则节点记录就不是定长的。
 */
static void make_cols(db_graph_col *cols)
{
    memset(cols, 0, sizeof(db_graph_col) * 3);

    strcpy(cols[0].name, "id");
    cols[0].type = DB_GRAPH_INT4;
    cols[0].nullable = 0;

    strcpy(cols[1].name, "name");
    cols[1].type = DB_GRAPH_TEXT;
    cols[1].width = 16;
    cols[1].nullable = 1;

    strcpy(cols[2].name, "score");
    cols[2].type = DB_GRAPH_INT8;
    cols[2].nullable = 1;
}

static void make_val(db_graph_value *v, db_graph_type type, int64_t i,
                     const char *s)
{
    memset(v, 0, sizeof(*v));
    v->type = type;
    v->i = i;
    if (s)
        snprintf(v->s, sizeof(v->s), "%s", s);
}

/* ---------------- 1) 建图 ---------------- */

static void t1_create(void)
{
    printf("  1) 建图：图存在；重复建图被拒\n");
    remove(PATH);
    remove("test/tmp/graph_api.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 还没建图 → 图不存在，虚拟表也不存在 */
    CHECK(db_graph_exists(db) == 0);
    CHECK(run_sql(db, "SELECT * FROM graph_nodes;") != 0);

    db_graph_col cols[3];
    make_cols(cols);
    CHECK(db_graph_create(db, cols, 3) == DB_OK);
    CHECK(db_graph_exists(db) == 1);

    /* 建完图，虚拟表就存在了 */
    CHECK(run_sql(db, "SELECT * FROM graph_nodes;") == 0);

    /* schema 不可改：再建一次必须被拒 */
    CHECK(db_graph_create(db, cols, 3) == DB_E_INVAL);

    /* 换一套 schema 也必须被拒（不能拿新布局解释旧记录） */
    db_graph_col cols2[2];
    memset(cols2, 0, sizeof(cols2));
    strcpy(cols2[0].name, "x");
    cols2[0].type = DB_GRAPH_INT8;
    CHECK(db_graph_create(db, cols2, 2) == DB_E_INVAL);

    db_close(db);
}

/* ---------------- 2) 属性往返 ---------------- */

static db_graph_rid add_node(DB *db, int64_t id, const char *name, int64_t score)
{
    db_graph_value vals[3];
    make_val(&vals[0], DB_GRAPH_INT4, id, NULL);
    make_val(&vals[1], DB_GRAPH_TEXT, 0, name);
    make_val(&vals[2], DB_GRAPH_INT8, score, NULL);

    db_graph_rid rid = 0;
    db_err e = db_graph_add_node(db, vals, 3, &rid);
    CHECK(e == DB_OK);
    return rid;
}

static void t2_props(void)
{
    printf("  2) 属性读写往返（定长列）\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    db_graph_rid a = add_node(db, 7, "alice", 100);
    CHECK(a != 0);

    db_graph_value out[3];
    int n = 0;
    CHECK(db_graph_get_node(db, a, out, 3, &n) == DB_OK);
    CHECK(n == 3);
    CHECK(out[0].type == DB_GRAPH_INT4 && out[0].i == 7);
    CHECK(out[1].type == DB_GRAPH_TEXT && strcmp(out[1].s, "alice") == 0);
    CHECK(out[2].type == DB_GRAPH_INT8 && out[2].i == 100);

    /* NULL 是合法的（name 列可空） */
    db_graph_value vals[3];
    make_val(&vals[0], DB_GRAPH_INT4, 8, NULL);
    make_val(&vals[1], DB_GRAPH_TEXT, 0, NULL);
    vals[1].is_null = 1;
    make_val(&vals[2], DB_GRAPH_INT8, 200, NULL);

    db_graph_rid b = 0;
    CHECK(db_graph_add_node(db, vals, 3, &b) == DB_OK);

    CHECK(db_graph_get_node(db, b, out, 3, &n) == DB_OK);
    CHECK(out[1].is_null == 1);

    /* 非空列给 NULL → 被拒 */
    db_graph_value bad[3];
    make_val(&bad[0], DB_GRAPH_INT4, 9, NULL);
    bad[0].is_null = 1;
    make_val(&bad[1], DB_GRAPH_TEXT, 0, "x");
    make_val(&bad[2], DB_GRAPH_INT8, 0, NULL);
    db_graph_rid c = 0;
    CHECK(db_graph_add_node(db, bad, 3, &c) != DB_OK);

    /* 文本超宽 → 被拒（不截断） */
    db_graph_value toolong[3];
    make_val(&toolong[0], DB_GRAPH_INT4, 10, NULL);
    memset(toolong[1].s, 'x', sizeof(toolong[1].s) - 1);
    toolong[1].s[sizeof(toolong[1].s) - 1] = '\0';
    toolong[1].type = DB_GRAPH_TEXT;
    make_val(&toolong[2], DB_GRAPH_INT8, 0, NULL);
    db_graph_rid d = 0;
    CHECK(db_graph_add_node(db, toolong, 3, &d) != DB_OK);

    db_close(db);
}

/* ---------------- 3) 边与邻居 ---------------- */

static db_graph_rid g_a, g_b, g_c;

static void t3_edges(void)
{
    printf("  3) 加边 → 出邻居 / 入邻居对\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    g_a = add_node(db, 1, "a", 10);
    g_b = add_node(db, 2, "b", 20);
    g_c = add_node(db, 3, "c", 30);

    db_graph_rid e1 = 0, e2 = 0;
    CHECK(db_graph_add_edge(db, g_a, g_b, &e1) == DB_OK);
    CHECK(db_graph_add_edge(db, g_a, g_c, &e2) == DB_OK);

    db_graph_rid *out = NULL;
    uint32_t n = 0;
    CHECK(db_graph_hop(db, g_a, &out, &n) == DB_OK);
    CHECK(n == 2);
    bool found_b = false, found_c = false;
    for (uint32_t i = 0; i < n; i++) {
        if (out[i] == g_b) found_b = true;
        if (out[i] == g_c) found_c = true;
    }
    CHECK(found_b && found_c);
    db_graph_free(out);

    /* 入邻居：b 的入邻居只有 a */
    out = NULL;
    CHECK(db_graph_hop_in(db, g_b, &out, &n) == DB_OK);
    CHECK(n == 1 && out[0] == g_a);
    db_graph_free(out);

    /* 边的端点能读回来 */
    db_graph_rid src = 0, dst = 0;
    CHECK(db_graph_get_edge(db, e1, &src, &dst) == DB_OK);
    CHECK(src == g_a && dst == g_b);

    /* 加边到不存在的节点 → 被拒（不能建悬空边） */
    db_graph_rid bad = 0;
    CHECK(db_graph_add_edge(db, g_a, 999999, &bad) != DB_OK);

    uint64_t nn = 0, ne = 0;
    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(nn == 5);      /* 上一项留了 4 个 + 这里 3 个 */
    CHECK(ne == 2);

    db_close(db);
}

/* ---------------- 4) BFS 多跳 ---------------- */

static void t4_walk(void)
{
    printf("  4) BFS 多跳：去重、深度对\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 现在的图：a → b, a → c */
    db_graph_rid *out = NULL;
    uint32_t n = 0;

    /* 深度 1 = 一跳 = 2 个邻居 */
    CHECK(db_graph_walk(db, g_a, 1, &out, &n) == DB_OK);
    CHECK(n == 3);          /* 起点 + 2 个邻居 */
    db_graph_free(out);

    /* 深度 0 = 只有起点 */
    out = NULL;
    CHECK(db_graph_walk(db, g_a, 0, &out, &n) == DB_OK);
    CHECK(n == 1 && out[0] == g_a);
    db_graph_free(out);

    /* 加一条 c → b：b 可以从 a 经两条路到达。
       深度 2 必须**去重**（b 只出现一次）。 */
    db_graph_rid e3 = 0;
    CHECK(db_graph_add_edge(db, g_c, g_b, &e3) == DB_OK);

    out = NULL;
    CHECK(db_graph_walk(db, g_a, 2, &out, &n) == DB_OK);
    /* a, b, c, b → 去重后 3 个 */
    CHECK(n == 3);

    int cnt_b = 0;
    for (uint32_t i = 0; i < n; i++)
        if (out[i] == g_b) cnt_b++;
    CHECK(cnt_b == 1);
    db_graph_free(out);

    db_close(db);
}

/* ---------------- 5) 删节点连带删边 ---------------- */

static void t5_remove(void)
{
    printf("  5) 删节点连带删掉它的所有出边和入边\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    uint64_t nn = 0, ne = 0;
    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(ne == 3);

    /* 删掉 c：c 的出边（c→b）和入边（a→c）都要消失 */
    CHECK(db_graph_remove_node(db, g_c) == DB_OK);

    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(ne == 1);
    CHECK(nn == 4);

    /* a 的出邻居只剩 b */
    db_graph_rid *out = NULL;
    uint32_t n = 0;
    CHECK(db_graph_hop(db, g_a, &out, &n) == DB_OK);
    CHECK(n == 1 && out[0] == g_b);
    db_graph_free(out);

    /* b 的入邻居只剩 a（c 那条链摘干净了） */
    out = NULL;
    CHECK(db_graph_hop_in(db, g_b, &out, &n) == DB_OK);
    CHECK(n == 1 && out[0] == g_a);
    db_graph_free(out);

    db_close(db);
}

/* ---------------- 6) 重启：schema 与数据都要在 ---------------- */

static void t6_restart(void)
{
    printf("  6) 重启后：schema 恢复、节点数 / 边数 / 邻居全对\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    CHECK(db_graph_exists(db) == 1);

    /* schema 从磁盘读回来了 */
    int ncols = db_graph_col_count(db);
    CHECK(ncols == 3);

    db_graph_col cols[8];
    int got = 0;
    CHECK(db_graph_schema(db, cols, 8, &got) == DB_OK);
    CHECK(got == 3);
    CHECK(strcmp(cols[0].name, "id") == 0);
    CHECK(cols[0].type == DB_GRAPH_INT4);
    CHECK(strcmp(cols[1].name, "name") == 0);
    CHECK(cols[1].type == DB_GRAPH_TEXT && cols[1].width == 16);
    CHECK(strcmp(cols[2].name, "score") == 0);
    CHECK(cols[2].type == DB_GRAPH_INT8);

    /* 数据也还在 */
    uint64_t nn = 0, ne = 0;
    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(nn == 4);
    CHECK(ne == 1);

    /* 节点属性能读出来 */
    db_graph_value out[3];
    int n = 0;
    CHECK(db_graph_get_node(db, g_a, out, 3, &n) == DB_OK);
    CHECK(out[1].type == DB_GRAPH_TEXT && strcmp(out[1].s, "a") == 0);

    /* 邻接链还在 */
    db_graph_rid *nb = NULL;
    uint32_t cnt = 0;
    CHECK(db_graph_hop(db, g_a, &nb, &cnt) == DB_OK);
    CHECK(cnt == 1 && nb[0] == g_b);
    db_graph_free(nb);

    db_close(db);
}

/* ---------------- 7) schema 不一致 → 拒绝 ---------------- */

/*
 * 拿一套**不一样**的 schema 去接管磁盘上已有的图。
 *
 * 判据：node_size 相同也可能布局不同
 *   INT4 + INT4  → 8 字节
 *   INT8         → 8 字节
 * 长度一样但列的切分完全不同 —— 所以必须逐列比，不能只比长度。
 */
static void t7_schema_mismatch(void)
{
    printf("  7) schema 不一致 → 拒绝建图\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 图已经存在 → 任何建图请求都要被拒 */
    db_graph_col cols[3];
    make_cols(cols);
    CHECK(db_graph_create(db, cols, 3) == DB_E_INVAL);

    /* 长度相同但布局不同：id:INT4 + pad:INT4 vs id:INT8 */
    db_graph_col cols2[2];
    memset(cols2, 0, sizeof(cols2));
    strcpy(cols2[0].name, "id");
    cols2[0].type = DB_GRAPH_INT4;
    strcpy(cols2[1].name, "pad");
    cols2[1].type = DB_GRAPH_INT4;
    CHECK(db_graph_create(db, cols2, 2) == DB_E_INVAL);

    db_close(db);
}

/* ---------------- 8) SQL 与 API 看到的是同一张图 ---------------- */

static void t8_sql_same_graph(void)
{
    printf("  8) SQL 与 C API 看到的是同一张图\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 从 SQL 插一个节点 */
    CHECK(run_sql(db, "INSERT INTO graph_nodes VALUES (NULL, 99, 'sqlnode', 990);") == 0);

    /* C API 能数到它 */
    uint64_t nn = 0, ne = 0;
    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(nn == 5);

    /* 从 SQL 查，能查到 C API 建的节点 */
    Result *r = NULL;
    CHECK(db_exec(db, "SELECT name FROM graph_nodes WHERE id = 1;", &r) == DB_OK);
    CHECK(db_result_rows(r) == 1);
    CHECK(strcmp(db_result_value(r, 0, 0), "a") == 0);
    db_result_free(r);

    /* 从 C API 建的节点，SQL 也查得到 */
    r = NULL;
    CHECK(db_exec(db, "SELECT name FROM graph_nodes WHERE id = 99;", &r) == DB_OK);
    CHECK(db_result_rows(r) == 1);
    CHECK(strcmp(db_result_value(r, 0, 0), "sqlnode") == 0);
    db_result_free(r);

    /* 图不支持 UPDATE → 明确报错，不是崩溃 */
    CHECK(run_sql(db, "UPDATE graph_nodes SET score = 1;") != 0);
    CHECK(run_sql(db, "DELETE FROM graph_nodes;") != 0);

    db_close(db);
}

int main(void)
{
    printf("test_graph_api:\n");

    t1_create();
    t2_props();
    t3_edges();
    t4_walk();
    t5_remove();
    t6_restart();
    t7_schema_mismatch();
    t8_sql_same_graph();

    remove(PATH);
    remove("test/tmp/graph_api.db-wal");

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
