/*
 * test_graph_am_push.c — 图模态下推的**效率**验收（v0.11 步骤 3 补充）
 *
 * 1 项：
 *   1. src / dst 等值下推是**真下推**：只读匹配链上的那几条记录，
 *      不是"全表读完再过滤"
 *
 * 【为什么这一项要单独一个文件】
 *
 * 因为它和 test_graph_am.c 里的其它项验的是**不同的东西**：
 *
 *   test_graph_am.c        验正确性：下推的结果 == 全扫过滤的结果
 *   test_graph_am_push.c   验效率性：下推读得比全扫少
 *
 * 【为什么"效率"必须单独验】
 *
 * "下推行数 == 全扫过滤行数"这条断言**验不出真下推**。
 *
 * 设想一个实现：下推的时候老老实实全表扫完，然后在模态内部过滤。
 * 它返回的行集和真下推**完全一样** —— 断言照样通过。
 * 但它一点 IO 都省不下，下推就白做了。
 *
 * 所以效率必须用另一个可观测量来验：**读了多少条记录**。
 * gstore_read_count 就是为此加的（见 gstore.h）。
 *
 * 少读 = 少 IO = 下推真的省了事。这才是"真下推"的定义。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/graph/graph_am.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 计数助手 ---------------- */

/* 全扫一行源，返回行数 */
static int count_scan(StorageAM *am)
{
    AmScan *scan = NULL;
    if (am->scan_begin(am, &scan) != OK)
        return -1;

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    int n = 0;

    while (am->scan_next(am, scan, &rid, buf, sizeof(buf), &len) == OK)
        n++;

    am->scan_end(am, scan);
    return n;
}

/* 下推行源，返回行数 */
static int count_push(StorageAM *am, const Predicate *p)
{
    AmScan *scan = NULL;
    if (am->push(am, p, &scan) != OK)
        return -1;

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    int n = 0;

    while (am->scan_next(am, scan, &rid, buf, sizeof(buf), &len) == OK)
        n++;

    am->scan_end(am, scan);
    return n;
}

/* ---------------- 建图 ---------------- */

static void make_cols(GraphCol *cols)
{
    memset(cols, 0, sizeof(GraphCol) * 3);
    strcpy(cols[0].name, "id");
    cols[0].type = T_INT4;
    strcpy(cols[1].name, "name");
    cols[1].type = T_TEXT;
    cols[1].width = 16;
    strcpy(cols[2].name, "score");
    cols[2].type = T_INT8;
}

/*
 * 造一个"锚点出度 1，但全表 39 条边"的图。
 *
 * 锚点是 ids[0]：它**刻意不在**那条长链上，
 * 所以它的出度只有后面单独加的那一条。
 */
static Graph *build_chain_graph(Pager *pg, Rid ids[40])
{
    GraphCol cols[3];
    make_cols(cols);

    Graph *g = NULL;
    if (graph_create(pg, NULL, NULL, cols, 3, &g) != OK)
        return NULL;

    /* 40 个节点 */
    DbValue vals[3];
    for (int i = 0; i < 40; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "x%d", i);
        val_int(&vals[0], i);
        val_text(&vals[1], nm, (u16)strlen(nm));
        val_int(&vals[2], i);
        if (graph_create_node(g, 0, vals, 3, &ids[i]) != OK)
            return NULL;
    }

    /* 链：ids[1] → ids[2] → ... → ids[39]（38 条边） */
    for (int i = 1; i + 1 < 40; i++) {
        Rid edge;
        if (graph_add_edge(g, 0, ids[i], ids[i + 1], &edge) != OK)
            return NULL;
    }

    /* 锚点唯一的出边：ids[0] → ids[39] */
    Rid anchor_edge;
    if (graph_add_edge(g, 0, ids[0], ids[39], &anchor_edge) != OK)
        return NULL;

    return g;
}

int main(void)
{
    printf("test_graph_am_push:\n");

    const char *path = "test/tmp/graph_am_push.db";
    remove(path);
    remove("test/tmp/graph_am_push.db-wal");

    Pager *pg = NULL;
    CHECK(pager_open(path, 64, &pg) == OK);

    Rid ids[40];
    Graph *g = build_chain_graph(pg, ids);
    CHECK(g != NULL);
    if (!g) {
        if (pg)
            pager_close(pg);
        return 1;
    }

    StorageAM am_n, am_e;
    GraphAmCtx ctx_n, ctx_e;
    CHECK(graph_am_build_nodes(g, &am_n, &ctx_n) == OK);
    CHECK(graph_am_build_edges(g, &am_e, &ctx_e) == OK);

    CHECK(count_scan(&am_e) == 39);

    /* --- src = 锚点 → 走锚点的出链 --- */
    Predicate p;
    memset(&p, 0, sizeof(p));
    p.col_idx = 0;                   /* src */
    p.op = PD_EQ;
    val_int(&p.val, (i64)rid_to_u64(ids[0]));

    CHECK(am_e.can_push(&am_e, &p) == OK);

    /* 【效率断言】下推只读了 1 条边记录，不是全表 39 条 */
    u64 reads_before = gstore_read_count(g->edges);
    int pushed = count_push(&am_e, &p);
    u64 reads_after = gstore_read_count(g->edges);

    CHECK(pushed == 1);
    CHECK(reads_after - reads_before == 1);   /* ← 真下推的证据 */

    /* 对照：全扫读了 39 条 */
    u64 full_before = gstore_read_count(g->edges);
    int full_rows = count_scan(&am_e);
    u64 full_after = gstore_read_count(g->edges);

    CHECK(full_rows == 39);
    CHECK(full_after - full_before == 39);

    /* --- 删掉锚点的出边 → 下推立刻变 0 行 --- */
    Rid anchor_edge;
    {
        /* 找出那条边：ids[0] 的出链上唯一一条 */
        GraphEdgeCursor ec;
        CHECK(graph_edge_cursor_open(g, ids[0], true, &ec) == OK);
        Rid e, s, d;
        CHECK(graph_edge_cursor_next(g, &ec, &e, &s, &d) == OK);
        anchor_edge = e;
    }
    CHECK(graph_remove_edge(g, 0, anchor_edge) == OK);
    CHECK(count_push(&am_e, &p) == 0);

    /* --- dst = 锚点 → 走锚点的入链 --- */
    Rid back_edge;
    CHECK(graph_add_edge(g, 0, ids[39], ids[0], &back_edge) == OK);

    Predicate p2d;
    memset(&p2d, 0, sizeof(p2d));
    p2d.col_idx = 1;                 /* dst */
    p2d.op = PD_EQ;
    val_int(&p2d.val, (i64)rid_to_u64(ids[0]));

    CHECK(am_e.can_push(&am_e, &p2d) == OK);

    /* 入链下推也只读 1 条 */
    u64 in_before = gstore_read_count(g->edges);
    int in_pushed = count_push(&am_e, &p2d);
    u64 in_after = gstore_read_count(g->edges);

    CHECK(in_pushed == 1);
    CHECK(in_after - in_before == 1);

    graph_destroy(g);
    pager_close(pg);

    if (failures) {
        printf("  %d 项断言失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  1 项验收全部通过 [PASS]\n");
    return 0;
}
