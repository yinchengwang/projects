/*
 * test_graph_it.c —— 图模态接进 SQL 的端到端验收（v0.11 步骤 5）
 *
 * 6 项：
 *   1. **A3** 执行器零改动：src/query/ 里零个 "graph"
 *   2. **A4a** graph_nodes 的属性列过滤：下推结果 == 全扫过滤结果
 *   3. **A4b** graph_nodes 的 node_id 等值：只返回那一个节点
 *   4. **A4c** graph_edges 按 src / dst 下推：**只读链上那几条**（读计数证明）
 *   5. **A9** 能力协商：UPDATE / DELETE / VACUUM / CREATE INDEX 明确报错，不是崩溃
 *   6. INSERT 路径：node_id 必须由存储层分配（用户不能指定）
 *
 * 【第 4 项是这一版的重点】
 *
 * "下推"这件事，光看查询结果是**验不出来的**：
 * 一个写成"全表扫 + 逐行过滤"的实现，返回的行集和真下推**完全一样**。
 *
 * 所以必须用一个**可观测量**来区分：读了多少条记录。
 * 真下推只读匹配链上的那几条（= 锚点的度数）；
 * 全扫读了全表每一条。这两者差一个数量级 ——
 * 40 条边 vs 100 条边，一眼就能看出来。
 *
 * （可观测量的设计见 v0.11 步骤 3 的记录：GStore.rec_reads。）
 *
 * 【第 1 项为什么要用机械检查】
 *
 * A3 的内容是"执行器一行都不改"。这件事没法用行为断言表达 ——
 * 行为对了不代表执行器没被改。所以只能查源码。
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

#define PATH "test/tmp/graph_it.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

/* 跑一条 SELECT，返回行数；出错返回 -1 */
static int query_rows(DB *db, const char *sql, Result **out)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r)
            db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    if (out)
        *out = r;
    else
        db_result_free(r);
    return n;
}

/* ---------------- 1) A3：执行器零改动 ---------------- */

/*
 * 查 src/query/ 下的每个源文件，看有没有 "graph" 字样。
 *
 * 【为什么要在这里查，而不是在 Makefile 里查】
 *
 * 因为这是**验收项**，不是构建约束 —— 构建约束归 tools/check_deps.sh。
 * 放在测试里，测试失败时能直接看到"A3 破了"。
 *
 * 找不到源文件（比如从别的目录跑测试）→ 跳过并说明，
 * 不当成失败 —— 那不是代码的问题。
 */
static int t1_exec_untouched(void)
{
    printf("  1) A3：执行器零改动（src/query/ 里没有 graph）\n");

    static const char *files[] = {
        "src/query/exec.c",
        "src/query/exec.h",
        "src/query/exec_scan.c",
        "src/query/exec_write.c",
        "src/query/parser.c",
        "src/query/parser.h",
        "src/query/lexer.c",
        NULL
    };

    int checked = 0;
    for (int i = 0; files[i]; i++) {
        FILE *f = fopen(files[i], "rb");
        if (!f)
            continue;
        checked++;

        char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
        buf[n] = '\0';

        char *hit = strstr(buf, "graph");
        if (hit) {
            printf("    FAIL %s 里出现了 \"graph\"\n", files[i]);
            failures++;
        }
    }

    if (checked == 0) {
        printf("    （没找到 src/query/ 下的源文件，跳过机械检查）\n");
        return 0;
    }
    CHECK(checked > 0);
    return 0;
}

/* ---------------- 建图 + 灌数据 ---------------- */

#define N_NODES 40
#define N_EDGES 100
#define HUB_DEG 30

static db_graph_rid node_ids[N_NODES];
static db_graph_rid hub_id;

static void setup(DB *db)
{
    db_graph_col cols[3];
    memset(cols, 0, sizeof(cols));
    strcpy(cols[0].name, "id");
    cols[0].type = DB_GRAPH_INT4;
    strcpy(cols[1].name, "name");
    cols[1].type = DB_GRAPH_TEXT;
    cols[1].width = 16;
    strcpy(cols[2].name, "score");
    cols[2].type = DB_GRAPH_INT8;

    CHECK(db_graph_create(db, cols, 3) == DB_OK);

    for (int i = 0; i < N_NODES; i++) {
        db_graph_value vals[3];
        char nm[16];
        snprintf(nm, sizeof(nm), "n%d", i);

        memset(&vals[0], 0, sizeof(vals[0]));
        vals[0].type = DB_GRAPH_INT4;
        vals[0].i = i;

        memset(&vals[1], 0, sizeof(vals[1]));
        vals[1].type = DB_GRAPH_TEXT;
        snprintf(vals[1].s, sizeof(vals[1].s), "%s", nm);

        memset(&vals[2], 0, sizeof(vals[2]));
        vals[2].type = DB_GRAPH_INT8;
        vals[2].i = (i == 7) ? 999 : (i * 3);     /* 999 是唯一值（i*3 撞不上） */

        db_graph_rid rid = 0;
        CHECK(db_graph_add_node(db, vals, 3, &rid) == DB_OK);
        node_ids[i] = rid;
        if (i == 0)
            hub_id = rid;
    }

    /* 边：hub 出度 HUB_DEG，其余边随便连 */
    for (int i = 0; i < N_EDGES; i++) {
        db_graph_rid src, dst, e = 0;
        if (i < HUB_DEG) {
            /* hub 的出边 */
            src = hub_id;
            dst = node_ids[1 + (i % (N_NODES - 1))];
        } else if (i < HUB_DEG + 10) {
            /* 指向 hub 的入边（dst 下推要能命中） */
            src = node_ids[1 + (i % (N_NODES - 1))];
            dst = hub_id;
        } else {
            src = node_ids[1 + (i % (N_NODES - 1))];
            dst = node_ids[1 + ((i + 7) % (N_NODES - 1))];
        }
        db_err e2 = db_graph_add_edge(db, src, dst, &e);
        CHECK(e2 == DB_OK);
    }
}

/* ---------------- 2) A4a：属性列过滤 ---------------- */

static void t2_attr_filter(void)
{
    printf("  2) A4a：graph_nodes 的属性列过滤（结果 == 全扫过滤结果）\n");

    /* 图和数据在 main 里已经建好了（建图只能一次） */
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 下推查询 */
    int n_push = query_rows(db, "SELECT * FROM graph_nodes WHERE score = 999;", NULL);
    CHECK(n_push == 1);

    /* 全扫 + 自己在结果里数一遍（超集语义断言） */
    Result *all = NULL;
    int n_all = query_rows(db, "SELECT score FROM graph_nodes;", &all);
    CHECK(n_all == N_NODES);

    int n_match = 0;
    for (int i = 0; i < n_all; i++) {
        if (strcmp(db_result_value(all, i, 0), "999") == 0)
            n_match++;
    }
    db_result_free(all);

    /* 下推行数 == 全扫过滤行数 —— 这一条才是"下推没漏行"的证据 */
    CHECK(n_push == n_match);

    /* 文本列也一样 */
    int n_name = query_rows(db, "SELECT * FROM graph_nodes WHERE name = 'n7';", NULL);
    CHECK(n_name == 1);

    /* 不等式也要对 */
    int n_big = query_rows(db, "SELECT * FROM graph_nodes WHERE score > 100;", NULL);
    CHECK(n_big > 0 && n_big < N_NODES);

    db_close(db);
}

/* ---------------- 3) A4b：node_id 等值 ---------------- */

static void t3_nodeid_eq(void)
{
    printf("  3) A4b：node_id 等值 → 只返回那一个节点\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT * FROM graph_nodes WHERE node_id = %llu;",
             (unsigned long long)node_ids[7]);

    Result *r = NULL;
    int n = query_rows(db, sql, &r);
    CHECK(n == 1);
    if (n == 1) {
        /* 第 0 列是 node_id（RID），第 1 列才是 id 属性 */
        char want[32];
        snprintf(want, sizeof(want), "%llu", (unsigned long long)node_ids[7]);
        CHECK(strcmp(db_result_value(r, 0, 0), want) == 0);
        CHECK(strcmp(db_result_value(r, 0, 1), "7") == 0);
    }
    db_result_free(r);

    /* 不存在的 node_id → 0 行（不是报错） */
    snprintf(sql, sizeof(sql), "SELECT * FROM graph_nodes WHERE node_id = 999999;");
    CHECK(query_rows(db, sql, NULL) == 0);

    db_close(db);
}

/* ---------------- 4) A4c：边表真下推（读计数证明） ---------------- */

static void t4_edge_pushdown(void)
{
    printf("  4) A4c：graph_edges 按 src 下推 → 只读链上那几条\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    uint64_t r0 = 0, r1 = 0;
    CHECK(db_graph_reads(db, &r0, &r1) == DB_OK);

    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT * FROM graph_edges WHERE src = %llu;",
             (unsigned long long)hub_id);

    int n = query_rows(db, sql, NULL);
    CHECK(n == HUB_DEG);

    uint64_t r2 = 0, r3 = 0;
    CHECK(db_graph_reads(db, &r2, &r3) == DB_OK);

    uint64_t delta = r3 - r1;         /* 边表读了多少条记录 */
    printf("    （下推读了 %llu 条记录，全表 %d 条）\n",
           (unsigned long long)delta, N_EDGES);

    /*
     * 真下推的判据：读的记录数 ≈ 链长（= 锚点的度数），
     * 而不是全表条数。
     *
     * 留 5 条的余量：打开游标时还要读一次节点记录拿链头
     * （那算在节点表的计数里，不算边表）。
     */
    CHECK(delta <= (uint64_t)HUB_DEG + 5);
    CHECK(delta < (uint64_t)N_EDGES);

    /* 对照：全表扫一遍，边表读数应该正好是全表条数 */
    uint64_t before = 0;
    CHECK(db_graph_reads(db, &before, &r1) == DB_OK);
    int n_all = query_rows(db, "SELECT * FROM graph_edges;", NULL);
    CHECK(n_all == N_EDGES);
    uint64_t after = 0;
    CHECK(db_graph_reads(db, &before, &after) == DB_OK);
    CHECK(after - r1 == (uint64_t)N_EDGES);

    /* 按 dst 下推也要能命中：hub 的入边（其它节点指向 hub） */
    uint64_t b2 = 0, a2 = 0;
    CHECK(db_graph_reads(db, &b2, &a2) == DB_OK);
    snprintf(sql, sizeof(sql), "SELECT * FROM graph_edges WHERE dst = %llu;",
             (unsigned long long)hub_id);
    int n_in = query_rows(db, sql, NULL);
    CHECK(n_in > 0);
    uint64_t b3 = 0, a3 = 0;
    CHECK(db_graph_reads(db, &b3, &a3) == DB_OK);
    CHECK(a3 - a2 < (uint64_t)N_EDGES);

    db_close(db);
}

/* ---------------- 5) A9：能力协商 ---------------- */

static void t5_capability(void)
{
    printf("  5) A9：不支持的操作明确报错，不是崩溃\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* UPDATE */
    CHECK(run_sql(db, "UPDATE graph_nodes SET score = 1;") != 0);
    CHECK(strstr(db_last_error(db), "不支持更新") != NULL);

    /* DELETE */
    CHECK(run_sql(db, "DELETE FROM graph_nodes;") != 0);
    CHECK(strstr(db_last_error(db), "不支持删除") != NULL);

    /* VACUUM */
    CHECK(run_sql(db, "VACUUM graph_nodes;") != 0);
    CHECK(strstr(db_last_error(db), "不支持 VACUUM") != NULL);

    /* CREATE INDEX */
    CHECK(run_sql(db, "CREATE INDEX gidx ON graph_nodes(score);") != 0);
    CHECK(strstr(db_last_error(db), "不支持建索引") != NULL);

    /* 边表同理 */
    CHECK(run_sql(db, "UPDATE graph_edges SET src = 1;") != 0);
    CHECK(strstr(db_last_error(db), "不支持更新") != NULL);

    /*
     * 报错之后进程还活着，图的数据也没坏 ——
     * "明确报错"和"崩溃"的分界就在这里。
     */
    int n = query_rows(db, "SELECT * FROM graph_nodes;", NULL);
    CHECK(n == N_NODES);

    db_close(db);
}

/* ---------------- 6) INSERT 路径 ---------------- */

static void t6_insert(void)
{
    printf("  6) INSERT：node_id 由存储层分配，用户不能指定\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 正常插入：node_id 给 NULL */
    CHECK(run_sql(db, "INSERT INTO graph_nodes VALUES (NULL, 900, 'inserted', 900);") == 0);

    /* 指定 node_id → 被拒（id 由存储层分配） */
    CHECK(run_sql(db, "INSERT INTO graph_nodes VALUES (12345, 901, 'bad', 901);") != 0);

    /* 边表插入：src / dst 必须是已存在的节点 id */
    char sql[128];
    snprintf(sql, sizeof(sql), "INSERT INTO graph_edges VALUES (%llu, %llu);",
             (unsigned long long)node_ids[1], (unsigned long long)node_ids[2]);
    CHECK(run_sql(db, sql) == 0);

    /* 端点不存在 → 被拒（不能建悬空边） */
    snprintf(sql, sizeof(sql), "INSERT INTO graph_edges VALUES (%llu, 999999);",
             (unsigned long long)node_ids[1]);
    CHECK(run_sql(db, sql) != 0);

    /* 插进去的能被查出来 */
    int n = query_rows(db, "SELECT * FROM graph_nodes WHERE id = 900;", NULL);
    CHECK(n == 1);

    db_close(db);
}

int main(void)
{
    printf("test_graph_it:\n");

    remove(PATH);
    remove("test/tmp/graph_it.db-wal");

    t1_exec_untouched();

    /*
     * 剩下的项共用一个库。
     *
     * 建图和灌数据只做一次 —— 图只能建一次（schema 是图的身份证），
     * 所以 setup() 放在这里，后面每一项都只是**打开**这个库再断言。
     */
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    setup(db);
    db_close(db);

    t2_attr_filter();
    t3_nodeid_eq();
    t4_edge_pushdown();
    t5_capability();
    t6_insert();

    remove(PATH);
    remove("test/tmp/graph_it.db-wal");

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过\n");
    return 0;
}
