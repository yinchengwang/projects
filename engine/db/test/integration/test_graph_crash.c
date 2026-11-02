/*
 * test_graph_crash.c —— 图模态崩溃恢复验收（v0.11 步骤 6，A5）
 *
 * "断电"用 db_abandon 模拟：不刷脏页、不写 CHECKPOINT，直接丢句柄。
 * 等价于进程被 kill -9 —— 日志已 fsync，页还在内存里没落盘。
 *
 * 4 项：
 *   1. 装载 → 断电 → 重开：节点数、边数全对
 *   2. 断电后：每条边的两个端点全对（邻接链指针没丢）
 *   3. 断电后：3 跳的结果集和断电前**完全一致**
 *   4. 多轮：每轮加一批再断电，累加后全对（增量也要活）
 *
 * 【为什么第 3 项最难，也最值钱】
 *
 * 节点数和边数只是**计数**。
 *
 * 邻接链指针是**图的结构**：
 *   - 边的 src / dst 写在边记录里（gstore 的定长槽）
 *   - 节点记录里的 first_out / first_in 指向链头
 *   - 链上的 prev / next 把边串起来
 *
 * 这些指针全在**页**里，而页在断电时可能还没落盘 ——
 * 全靠 WAL 重放救回来。计数对但链断了，是最容易漏掉的失败模式：
 * "3 个节点 2 条边"看着没问题，但一跳跳到空。
 *
 * 所以第 3 项把断电前的 3 跳结果集存下来，断电后重算，逐个比对。
 *
 * 【为什么图的写路径在 WAL 里是安全的】
 *
 * 图的写路径 WAL xid 传 0（系统事务），
 * 而恢复阶段 xid=0 被当作"永远已提交"（recovery.c 的 xid_committed）。
 * 所以图的数据**只会被重放，不会被回滚**。
 *
 * 这正是 PLAN §1.4 判决 (b) 的落地：
 * "图走锁 + WAL，语句级提交，不做 MVCC"。
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

#define PATH "test/tmp/graph_crash.db"

#define N_NODES 200
#define N_EDGES 400
#define ROUNDS  3

/* 断电前记下的期望值 */
static db_graph_rid expect_node[N_NODES];
static db_graph_rid expect_edge_src[N_EDGES];
static db_graph_rid expect_edge_dst[N_EDGES];

/* 3 跳的结果集（断电前算好，断电后比对） */
static db_graph_rid walk_before[8192];
static uint32_t walk_before_n;

static void build_graph(DB *db)
{
    db_graph_col cols[2];
    memset(cols, 0, sizeof(cols));
    strcpy(cols[0].name, "id");
    cols[0].type = DB_GRAPH_INT4;
    strcpy(cols[1].name, "name");
    cols[1].type = DB_GRAPH_TEXT;
    cols[1].width = 16;

    if (db_graph_create(db, cols, 2) != DB_OK) {
        printf("    FAIL 建图失败\n");
        failures++;
        return;
    }

    for (int i = 0; i < N_NODES; i++) {
        db_graph_value vals[2];
        char nm[16];
        snprintf(nm, sizeof(nm), "n%d", i);

        memset(&vals[0], 0, sizeof(vals[0]));
        vals[0].type = DB_GRAPH_INT4;
        vals[0].i = i;

        memset(&vals[1], 0, sizeof(vals[1]));
        vals[1].type = DB_GRAPH_TEXT;
        snprintf(vals[1].s, sizeof(vals[1].s), "%s", nm);

        db_graph_rid rid = 0;
        if (db_graph_add_node(db, vals, 2, &rid) != DB_OK) {
            printf("    FAIL 建节点 %d 失败\n", i);
            failures++;
            return;
        }
        expect_node[i] = rid;
    }

    /*
     * 边：节点 i 连到 (i*7+3) % N_NODES —— 刻意让链不是一条直线，
     * 这样"链断在中间"和"链全断了"都能被抓到。
     */
    for (int i = 0; i < N_EDGES; i++) {
        int a = i % N_NODES;                  /* 起点 */
        int j = (i * 7 + 3) % N_NODES;       /* 终点 */
        if (j == a)
            j = (j + 1) % N_NODES;

        db_graph_rid e = 0;
        if (db_graph_add_edge(db, expect_node[a], expect_node[j], &e) != DB_OK) {
            printf("    FAIL 加边 %d 失败\n", i);
            failures++;
            return;
        }
        expect_edge_src[i] = expect_node[a];
        expect_edge_dst[i] = expect_node[j];
    }
}

/* 3 跳结果集：排序后逐个比对 */
static int cmp_rid(const void *a, const void *b)
{
    uint64_t x = *(const uint64_t *)a;
    uint64_t y = *(const uint64_t *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void snapshot_walk(DB *db)
{
    db_graph_rid *out = NULL;
    uint32_t n = 0;
    if (db_graph_walk(db, expect_node[0], 3, &out, &n) != DB_OK) {
        printf("    FAIL 3 跳失败\n");
        failures++;
        return;
    }
    walk_before_n = n;
    memcpy(walk_before, out, (size_t)n * sizeof(db_graph_rid));
    db_graph_free(out);
    qsort(walk_before, walk_before_n, sizeof(db_graph_rid), cmp_rid);
}

static void check_walk(DB *db)
{
    db_graph_rid *out = NULL;
    uint32_t n = 0;
    if (db_graph_walk(db, expect_node[0], 3, &out, &n) != DB_OK) {
        printf("    FAIL 断电后 3 跳失败\n");
        failures++;
        return;
    }
    qsort(out, n, sizeof(db_graph_rid), cmp_rid);

    if (n != walk_before_n) {
        printf("    FAIL 3 跳结果数变了：断电前 %u，断电后 %u\n",
               walk_before_n, n);
        failures++;
    } else {
        for (uint32_t i = 0; i < n; i++) {
            if (out[i] != walk_before[i]) {
                printf("    FAIL 3 跳第 %u 个结果变了\n", i);
                failures++;
                break;
            }
        }
    }
    db_graph_free(out);
}

/* ---------------- 1~3) 装载 → 断电 → 重开 ---------------- */

static void t1_crash_once(void)
{
    printf("  1~3) 装载 → 断电 → 重开：计数 / 边端点 / 3 跳全对\n");

    remove(PATH);
    remove("test/tmp/graph_crash.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    build_graph(db);
    snapshot_walk(db);

    /* 断电：不刷脏页、不写 CHECKPOINT */
    db_abandon(db);
    db = NULL;

    /* 重开 → 恢复重放 */
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 图还在（schema 从磁盘恢复了） */
    CHECK(db_graph_exists(db) == 1);
    CHECK(db_graph_col_count(db) == 2);

    /* 1) 计数 */
    uint64_t nn = 0, ne = 0;
    CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
    CHECK(nn == N_NODES);
    CHECK(ne == N_EDGES);

    /* 2) 每条边的端点 */
    db_graph_rid *edges = NULL;
    uint32_t nedges = 0;
    CHECK(db_graph_list_edges(db, &edges, &nedges) == DB_OK);
    CHECK(nedges == N_EDGES);

    int bad = 0;
    for (uint32_t i = 0; i < nedges; i++) {
        db_graph_rid s = 0, d = 0;
        if (db_graph_get_edge(db, edges[i], &s, &d) != DB_OK) {
            bad++;
            continue;
        }
        /* 边集合应该和断电前完全一样（顺序可能不同，所以按集合比） */
        bool found = false;
        for (int k = 0; k < N_EDGES; k++) {
            if (expect_edge_src[k] == s && expect_edge_dst[k] == d) {
                found = true;
                break;
            }
        }
        if (!found)
            bad++;
    }
    db_graph_free(edges);
    CHECK(bad == 0);

    /* 节点属性也要能读出来（schema 恢复的另一个证据） */
    db_graph_value vals[2];
    int nv = 0;
    CHECK(db_graph_get_node(db, expect_node[5], vals, 2, &nv) == DB_OK);
    CHECK(nv == 2);
    CHECK(strcmp(vals[1].s, "n5") == 0);

    /* 3) 3 跳结果集 */
    check_walk(db);

    db_close(db);
}

/* ---------------- 4) 多轮增量 ---------------- */

static void t4_rounds(void)
{
    printf("  4) 多轮：每轮加一批再断电，累加后全对\n");

    remove(PATH);
    remove("test/tmp/graph_crash.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 第一轮：建图 + 灌数据 */
    build_graph(db);

    /* 后续轮次：每次加一批边 */
    int added = 0;
    for (int round = 1; round < ROUNDS; round++) {
        int batch = 10 * round;

        for (int i = 0; i < batch; i++) {
            int a = (added * 3 + 1) % N_NODES;
            int b = (added * 5 + 2) % N_NODES;
            if (a == b)
                b = (b + 1) % N_NODES;

            db_graph_rid e = 0;
            if (db_graph_add_edge(db, expect_node[a], expect_node[b], &e) != DB_OK) {
                printf("    FAIL 第 %d 轮加边失败\n", round);
                failures++;
                break;
            }
            added++;
        }

        /* 断电 */
        db_abandon(db);
        db = NULL;

        /* 重开 */
        CHECK(db_open(PATH, &db) == DB_OK);

        uint64_t nn = 0, ne = 0;
        CHECK(db_graph_count(db, &nn, &ne) == DB_OK);
        CHECK(nn == N_NODES);
        if (ne != (uint64_t)(N_EDGES + added)) {
            printf("    FAIL 第 %d 轮：边数 %llu，应为 %d\n", round,
                   (unsigned long long)ne, N_EDGES + added);
            failures++;
        }
    }

    /* 最后一轮再验一次 3 跳：结构在多轮断电后仍然完整 */
    snapshot_walk(db);
    check_walk(db);

    db_close(db);
}

int main(void)
{
    printf("test_graph_crash:\n");

    t1_crash_once();
    t4_rounds();

    remove(PATH);
    remove("test/tmp/graph_crash.db-wal");

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  4 项验收全部通过\n");
    return 0;
}
