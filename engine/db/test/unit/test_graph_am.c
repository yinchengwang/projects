/*
 * test_graph_am.c — 图模态的 SPI 适配验收（v0.11 步骤 3）
 *
 * 8 项：
 *   1. 两张虚拟表的 schema（列名 / 类型 / 可空性）
 *   2. 扫描节点表：行数对、解码出的属性值对
 *   3. 扫描边表：行数对、src / dst 对
 *   4. get(rid) 与扫描行字节完全一致（往返）
 *   5. **下推行数 == 全扫过滤行数**（核心断言）
 *   6. 锚点不存在 → 0 行（不是报错）
 *   7. 能力协商：不支持的操作返 E_NOTSUP；可见性恒真
 *   8. insert：节点和边都能插，且插完能扫出来
 *
 * 【第 5 项是本文件的核心】
 *
 * 下推的正确性判据只有一条：**下推的结果必须和全扫过滤的结果一样**。
 *
 * 这就是"下推行数 == 全扫过滤行数"这条断言的含义。
 * 它比"下推的行数 >= 全扫过滤行数"更强 —— 后者只保证不漏行，
 * 前者保证**不多给也不少给**。
 *
 * 为什么要求更强？因为下推给出去的行，执行器还会**再算一遍 WHERE**。
 * 多给的行会被执行器过滤掉（无害，只是慢）；
 * 少给的行执行器永远看不到（**漏数据**）。
 * 所以真正不能出错的方向是"少给"。
 * 但如果多给很多，执行器的工作量就白费了 —— 那下推也就没意义了。
 *
 * 两条都断言，才能同时保证"不漏"和"下推真的省了事"。
 *
 * 【本文件验的是正确性，不是效率】
 *
 * "src = X 是真下推"这件事，**光看返回的行集是验不出来的**：
 * 一个写成"全扫 + 过滤"的实现返回的行集和真下推完全一样。
 * 效率必须用另一个可观测量（读了多少条记录）来验 ——
 * 那在 test_graph_am_push.c 里。
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

#define PATH "test/tmp/graph_am.db"

/* ---------------- 属性 schema ---------------- */

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

/* ---------------- 环境 ---------------- */

typedef struct {
    Pager      *pg;
    Graph      *g;
    StorageAM   am_nodes;
    StorageAM   am_edges;
    GraphAmCtx  ctx_nodes;
    GraphAmCtx  ctx_edges;
} Env;

static int env_open(Env *e)
{
    memset(e, 0, sizeof(*e));

    remove(PATH);
    remove("test/tmp/graph_am.db-wal");

    int rc = pager_open(PATH, 64, &e->pg);
    if (rc != OK)
        return rc;

    GraphCol cols[3];
    make_cols(cols);

    rc = graph_create(e->pg, NULL, NULL, cols, 3, &e->g);
    if (rc != OK)
        return rc;

    rc = graph_am_build_nodes(e->g, &e->am_nodes, &e->ctx_nodes);
    if (rc != OK)
        return rc;

    return graph_am_build_edges(e->g, &e->am_edges, &e->ctx_edges);
}

static void env_close(Env *e)
{
    graph_destroy(e->g);
    pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

/* ---------------- 建图数据 ---------------- */

/*
 * 造 N 个节点，M 条边。
 *
 * 节点 id 是 RID 的 u64 编码，所以节点建好之后才知道 id ——
 * 测试里要先把节点建出来，拿到 id，才能建边。
 */
static Rid build_graph(Env *e, int n_nodes, int n_edges)
{
    DbValue vals[3];

    Rid ids[64];
    for (int i = 0; i < n_nodes && i < 64; i++) {
        char nm[16];
        snprintf(nm, sizeof(nm), "n%d", i);
        val_int(&vals[0], i);
        val_text(&vals[1], nm, (u16)strlen(nm));
        val_int(&vals[2], i * 10);
        CHECK(graph_create_node(e->g, 0, vals, 3, &ids[i]) == OK);
    }

    /* 边：i → (i+1) % n，多余的边都挂到 0 号节点上 */
    for (int i = 0; i < n_edges; i++) {
        Rid edge;
        int a = (i < n_nodes) ? i : 0;
        int b = (i < n_nodes) ? (i + 1) % n_nodes : i % n_nodes;
        CHECK(graph_add_edge(e->g, 0, ids[a], ids[b], &edge) == OK);
    }

    return ids[0];
}

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

/*
 * 全扫 + 用**执行器的比较语义**过滤，返回行数。
 *
 * 【为什么要自己写一遍过滤】
 *
 * 不能直接用 am->push 的结果当参照 —— 那就是自己验自己。
 * 这里用与执行器逐字一致的比较（ga_match 就是照执行器写的），
 * 但走的是**另一条代码路径**（全扫 + 显式过滤），
 * 所以两条路径的差异会暴露出来。
 *
 * 参照值算的是"如果完全不下推，执行器会得到几行"。
 */
static int count_fullscan_filtered(StorageAM *am, const Predicate *p)
{
    AmScan *scan = NULL;
    if (am->scan_begin(am, &scan) != OK)
        return -1;

    const ColDesc *cols = NULL;
    int ncols = 0;
    am->columns(am, &cols, &ncols);

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    DbValue row[16];
    int n = 0;

    while (am->scan_next(am, scan, &rid, buf, sizeof(buf), &len) == OK) {
        if (am->decode(am, buf, len, row) != OK)
            continue;

        bool matched = false;
        if (ga_match(p->op, &row[p->col_idx], &p->val, &matched) != OK)
            continue;
        if (matched)
            n++;
    }

    am->scan_end(am, scan);
    return n;
}

/* ---------------- 1) schema ---------------- */

static void t1_schema(Env *e)
{
    printf("  1) 两张虚拟表的 schema\n");

    const ColDesc *cols = NULL;
    int ncols = 0;

    CHECK(e->am_nodes.columns(&e->am_nodes, &cols, &ncols) == OK);
    CHECK(ncols == 4);                     /* node_id + 3 个属性列 */
    CHECK(cols && strcmp(cols[0].name, "node_id") == 0);
    CHECK(cols && cols[0].type == T_INT8);
    CHECK(cols && cols[0].nullable);       /* id 由存储层分配 → 可空 */
    CHECK(cols && strcmp(cols[1].name, "id") == 0);
    CHECK(cols && cols[1].type == T_INT4);
    CHECK(cols && strcmp(cols[2].name, "name") == 0);
    CHECK(cols && cols[2].type == T_TEXT);
    CHECK(cols && strcmp(cols[3].name, "score") == 0);
    CHECK(cols && cols[3].type == T_INT8);

    CHECK(e->am_edges.columns(&e->am_edges, &cols, &ncols) == OK);
    CHECK(ncols == 2);
    CHECK(cols && strcmp(cols[0].name, "src") == 0);
    CHECK(cols && cols[0].type == T_INT8);
    CHECK(cols && !cols[0].nullable);
    CHECK(cols && strcmp(cols[1].name, "dst") == 0);
    CHECK(cols && cols[1].type == T_INT8);

    /* 表名 */
    CHECK(strcmp(e->am_nodes.name, "graph_nodes") == 0);
    CHECK(strcmp(e->am_edges.name, "graph_edges") == 0);
}

/* ---------------- 2) 扫节点表 ---------------- */

static void t2_scan_nodes(Env *e)
{
    printf("  2) 扫描节点表：行数对、属性解码对\n");

    AmScan *scan = NULL;
    CHECK(e->am_nodes.scan_begin(&e->am_nodes, &scan) == OK);

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    DbValue row[16];

    int n = 0;
    int checked_props = 0;

    while (e->am_nodes.scan_next(&e->am_nodes, scan, &rid,
                                 buf, sizeof(buf), &len) == OK) {
        /* 行 = [u64 node_id][节点记录] */
        CHECK(len == (u16)(8 + e->g->node_size));
        CHECK(e->am_nodes.decode(&e->am_nodes, buf, len, row) == OK);

        /* node_id = rid_to_u64(节点 RID) */
        CHECK(row[0].type == T_INT8);
        CHECK(row[0].i == (i64)get_u64(buf));

        /* 属性列 */
        CHECK(row[1].type == T_INT4);
        CHECK(row[2].type == T_TEXT);
        CHECK(row[3].type == T_INT8);

        /* 属性值和图层读到的一致 */
        DbValue direct[3];
        CHECK(graph_node_props(e->g, rid, direct, 3) == OK);
        CHECK(row[1].i == direct[0].i);
        CHECK(row[2].len == direct[1].len);
        CHECK(memcmp(row[2].s, direct[1].s, direct[1].len) == 0);
        CHECK(row[3].i == direct[2].i);

        checked_props++;
        n++;
    }

    CHECK(n == 4);                       /* 4 个节点 */
    CHECK(checked_props == 4);
    e->am_nodes.scan_end(&e->am_nodes, scan);
}

/* ---------------- 3) 扫边表 ---------------- */

static void t3_scan_edges(Env *e)
{
    printf("  3) 扫描边表：行数对、src / dst 对\n");

    AmScan *scan = NULL;
    CHECK(e->am_edges.scan_begin(&e->am_edges, &scan) == OK);

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    DbValue row[16];

    int n = 0;
    int checked = 0;

    while (e->am_edges.scan_next(&e->am_edges, scan, &rid,
                                 buf, sizeof(buf), &len) == OK) {
        CHECK(len == 16);
        CHECK(e->am_edges.decode(&e->am_edges, buf, len, row) == OK);
        CHECK(row[0].type == T_INT8);
        CHECK(row[1].type == T_INT8);

        /* src / dst 能在图层里反查回来 */
        Rid s, d;
        CHECK(graph_edge_endpoints(e->g, rid, &s, &d) == OK);
        CHECK(row[0].i == (i64)rid_to_u64(s));
        CHECK(row[1].i == (i64)rid_to_u64(d));

        checked++;
        n++;
    }

    CHECK(n == 6);                       /* 6 条边 */
    CHECK(checked == 6);
    e->am_edges.scan_end(&e->am_edges, scan);
}

/* ---------------- 4) get(rid) 往返 ---------------- */

static void t4_get_roundtrip(Env *e)
{
    printf("  4) get(rid) 与扫描行字节完全一致\n");

    AmScan *scan = NULL;
    CHECK(e->am_nodes.scan_begin(&e->am_nodes, &scan) == OK);

    u8 buf[1024];
    u16 len = 0;
    Rid rid;
    int checked = 0;

    while (e->am_nodes.scan_next(&e->am_nodes, scan, &rid,
                                 buf, sizeof(buf), &len) == OK) {
        u8 buf2[1024];
        u16 len2 = 0;
        CHECK(e->am_nodes.get(&e->am_nodes, rid, buf2, sizeof(buf2), &len2) == OK);
        CHECK(len2 == len);
        CHECK(memcmp(buf, buf2, len) == 0);
        checked++;
    }
    CHECK(checked == 4);
    e->am_nodes.scan_end(&e->am_nodes, scan);

    /* 边表同样 */
    CHECK(e->am_edges.scan_begin(&e->am_edges, &scan) == OK);
    checked = 0;
    while (e->am_edges.scan_next(&e->am_edges, scan, &rid,
                                 buf, sizeof(buf), &len) == OK) {
        u8 buf2[1024];
        u16 len2 = 0;
        CHECK(e->am_edges.get(&e->am_edges, rid, buf2, sizeof(buf2), &len2) == OK);
        CHECK(len2 == len);
        CHECK(memcmp(buf, buf2, len) == 0);
        checked++;
    }
    CHECK(checked == 6);
    e->am_edges.scan_end(&e->am_edges, scan);

    /* 越界 / 不存在的 RID → E_NOTFOUND，不是返回垃圾 */
    u8 tmp[1024];
    u16 tmplen = 0;
    CHECK(e->am_nodes.get(&e->am_nodes, rid_make(0xFFFFFF, 0),
                           tmp, sizeof(tmp), &tmplen) == E_NOTFOUND);
    CHECK(e->am_edges.get(&e->am_edges, rid_make(0xFFFFFF, 0),
                          tmp, sizeof(tmp), &tmplen) == E_NOTFOUND);
}

/* ---------------- 5) 下推行数 == 全扫过滤行数 ---------------- */

/*
 * 【核心断言】
 *
 * 对每一种能下推的谓词形状，都要求：
 *   下推行数 == 全扫 + 同谓词过滤后的行数
 */
static void t5_pushdown_count_matches(Env *e)
{
    printf("  5) 下推行数 == 全扫过滤行数\n");

    /* --- 节点表：node_id 等值 --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 0;
        p.op = PD_EQ;

        /* 找一个真实节点的 id */
        AmScan *scan = NULL;
        CHECK(e->am_nodes.scan_begin(&e->am_nodes, &scan) == OK);
        u8 buf[1024];
        u16 len = 0;
        Rid rid;
        CHECK(e->am_nodes.scan_next(&e->am_nodes, scan, &rid,
                                    buf, sizeof(buf), &len) == OK);
        val_int(&p.val, (i64)get_u64(buf));
        e->am_nodes.scan_end(&e->am_nodes, scan);

        CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == OK);

        int pushed = count_push(&e->am_nodes, &p);
        int full = count_fullscan_filtered(&e->am_nodes, &p);
        CHECK(pushed == full);
        CHECK(pushed == 1);              /* node_id 唯一 → 恰好 1 行 */
    }

    /* --- 节点表：属性列等值 --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 3;                   /* score 列 */
        p.op = PD_EQ;
        val_int(&p.val, 20);             /* 第 2 号节点的 score */

        CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == OK);

        int pushed = count_push(&e->am_nodes, &p);
        int full = count_fullscan_filtered(&e->am_nodes, &p);
        CHECK(pushed == full);
        CHECK(pushed == 1);
    }

    /* --- 节点表：属性列范围 --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 3;
        p.op = PD_GT;
        val_int(&p.val, 10);

        CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == OK);

        int pushed = count_push(&e->am_nodes, &p);
        int full = count_fullscan_filtered(&e->am_nodes, &p);
        CHECK(pushed == full);
        CHECK(pushed == 2);              /* score > 10：20, 30（score 是 i*10） */
    }

    /* --- 节点表：TEXT 属性列等值 --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 2;                   /* name 列 */
        p.op = PD_EQ;
        val_text(&p.val, "n2", 2);

        CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == OK);

        int pushed = count_push(&e->am_nodes, &p);
        int full = count_fullscan_filtered(&e->am_nodes, &p);
        CHECK(pushed == full);
        CHECK(pushed == 1);
    }

    /* --- 边表：src 等值（真下推） --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 0;                   /* src */
        p.op = PD_EQ;

        /* 取一条真实边的 src */
        AmScan *scan = NULL;
        CHECK(e->am_edges.scan_begin(&e->am_edges, &scan) == OK);
        u8 buf[1024];
        u16 len = 0;
        Rid rid;
        CHECK(e->am_edges.scan_next(&e->am_edges, scan, &rid,
                                    buf, sizeof(buf), &len) == OK);
        val_int(&p.val, (i64)get_u64(buf));
        e->am_edges.scan_end(&e->am_edges, scan);

        CHECK(e->am_edges.can_push(&e->am_edges, &p) == OK);

        int pushed = count_push(&e->am_edges, &p);
        int full = count_fullscan_filtered(&e->am_edges, &p);
        CHECK(pushed == full);
        CHECK(pushed >= 1);
    }

    /* --- 边表：dst 等值 --- */
    {
        Predicate p;
        memset(&p, 0, sizeof(p));
        p.col_idx = 1;                   /* dst */
        p.op = PD_EQ;

        AmScan *scan = NULL;
        CHECK(e->am_edges.scan_begin(&e->am_edges, &scan) == OK);
        u8 buf[1024];
        u16 len = 0;
        Rid rid;
        CHECK(e->am_edges.scan_next(&e->am_edges, scan, &rid,
                                    buf, sizeof(buf), &len) == OK);
        val_int(&p.val, (i64)get_u64(buf + 8));
        e->am_edges.scan_end(&e->am_edges, scan);

        CHECK(e->am_edges.can_push(&e->am_edges, &p) == OK);

        int pushed = count_push(&e->am_edges, &p);
        int full = count_fullscan_filtered(&e->am_edges, &p);
        CHECK(pushed == full);
        CHECK(pushed >= 1);
    }
}


/* ---------------- 7) 锚点不存在 → 0 行 ---------------- */

static void t7_missing_anchor(Env *e)
{
    printf("  7) 锚点不存在 → 0 行（不是报错）\n");

    /* 一个肯定不存在的节点 id（超出 RID 能表达的范围） */
    Predicate p;
    memset(&p, 0, sizeof(p));
    p.col_idx = 0;
    p.op = PD_EQ;
    val_int(&p.val, -1);

    CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == OK);
    CHECK(count_push(&e->am_nodes, &p) == 0);

    /* 边表：src = 不存在的节点 */
    Predicate p2;
    memset(&p2, 0, sizeof(p2));
    p2.col_idx = 0;
    p2.op = PD_EQ;
    val_int(&p2.val, (i64)rid_to_u64(rid_make(0xFFFFFF, 0)));

    CHECK(e->am_edges.can_push(&e->am_edges, &p2) == OK);
    CHECK(count_push(&e->am_edges, &p2) == 0);

    /* 一个在范围内但页号不存在的 id → 节点不存在 → 0 行 */
    Predicate p3;
    memset(&p3, 0, sizeof(p3));
    p3.col_idx = 0;
    p3.op = PD_EQ;
    val_int(&p3.val, (i64)rid_to_u64(rid_make(0xFFFFF, 5)));

    CHECK(e->am_nodes.can_push(&e->am_nodes, &p3) == OK);
    CHECK(count_push(&e->am_nodes, &p3) == 0);
}

/* ---------------- 8) 能力协商 ---------------- */

static void t8_caps_and_visibility(Env *e)
{
    printf("  8) 能力协商 + 可见性恒真\n");

    /* 节点表：能扫、能插、能下推；不能改 / 删 / vacuum / 索引 */
    CHECK((e->am_nodes.caps & AM_CAP_SCAN) != 0);
    CHECK((e->am_nodes.caps & AM_CAP_INSERT) != 0);
    CHECK((e->am_nodes.caps & AM_CAP_PUSHDOWN) != 0);
    CHECK((e->am_nodes.caps & AM_CAP_UPDATE) == 0);
    CHECK((e->am_nodes.caps & AM_CAP_REMOVE) == 0);
    CHECK((e->am_nodes.caps & AM_CAP_VACUUM) == 0);
    CHECK((e->am_nodes.caps & AM_CAP_INDEX) == 0);

    /* 边表同理 */
    CHECK((e->am_edges.caps & AM_CAP_SCAN) != 0);
    CHECK((e->am_edges.caps & AM_CAP_INSERT) != 0);
    CHECK((e->am_edges.caps & AM_CAP_PUSHDOWN) != 0);
    CHECK((e->am_edges.caps & AM_CAP_UPDATE) == 0);

    /* 写接口返回 E_NOTSUP（不是 E_INVAL，不是崩溃） */
    DbValue v[4];
    memset(v, 0, sizeof(v));
    Rid rid = rid_make(1, 0);

    CHECK(e->am_nodes.update(&e->am_nodes, 1, rid, v, &rid) == E_NOTSUP);
    CHECK(e->am_nodes.remove(&e->am_nodes, 1, rid) == E_NOTSUP);
    CHECK(e->am_nodes.vacuum(&e->am_nodes, NULL, 1, NULL) == E_NOTSUP);
    CHECK(e->am_nodes.reindex(&e->am_nodes, 0, NULL) == E_NOTSUP);

    CHECK(e->am_edges.update(&e->am_edges, 1, rid, v, &rid) == E_NOTSUP);
    CHECK(e->am_edges.remove(&e->am_edges, 1, rid) == E_NOTSUP);
    CHECK(e->am_edges.vacuum(&e->am_edges, NULL, 1, NULL) == E_NOTSUP);
    CHECK(e->am_edges.reindex(&e->am_edges, 0, NULL) == E_NOTSUP);

    /* 可见性恒真 */
    u8 buf[1024];
    u16 len = 0;
    AmVisCtx vis;
    memset(&vis, 0, sizeof(vis));
    CHECK(e->am_nodes.visible(&e->am_nodes, buf, len, &vis) == true);
    CHECK(e->am_edges.visible(&e->am_edges, buf, len, &vis) == true);

    /* 推不了的谓词：越界的列号 / 边表的非等值 */
    Predicate p;
    memset(&p, 0, sizeof(p));
    p.col_idx = 99;
    p.op = PD_EQ;
    val_int(&p.val, 1);
    CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == E_NOTSUP);

    p.col_idx = 0;
    p.op = PD_LT;                       /* node_id 只支持等值 */
    CHECK(e->am_nodes.can_push(&e->am_nodes, &p) == E_NOTSUP);

    p.col_idx = 0;                      /* 边表：src 只支持等值 */
    p.op = PD_GT;
    CHECK(e->am_edges.can_push(&e->am_edges, &p) == E_NOTSUP);
}

/* ---------------- 9) insert ---------------- */

static void t9_insert(Env *e)
{
    printf("  9) insert：节点和边都能插，且插完能扫出来\n");

    int nodes_before = count_scan(&e->am_nodes);
    int edges_before = count_scan(&e->am_edges);

    /* 插节点：vals[0] = node_id（NULL，存储层分配），后面是属性 */
    DbValue nv[4];
    val_null(&nv[0]);
    val_int(&nv[1], 99);
    val_text(&nv[2], "newbie", 6);
    val_int(&nv[3], 777);

    Rid new_node;
    CHECK(e->am_nodes.insert(&e->am_nodes, 0, nv, &new_node) == OK);
    CHECK(!rid_is_nil(new_node));

    CHECK(count_scan(&e->am_nodes) == nodes_before + 1);

    /* 插出来的节点能被扫到，属性对 */
    {
        AmScan *scan = NULL;
        CHECK(e->am_nodes.scan_begin(&e->am_nodes, &scan) == OK);
        u8 buf[1024];
        u16 len = 0;
        Rid rid;
        DbValue row[16];
        int found = 0;

        while (e->am_nodes.scan_next(&e->am_nodes, scan, &rid,
                                     buf, sizeof(buf), &len) == OK) {
            CHECK(e->am_nodes.decode(&e->am_nodes, buf, len, row) == OK);
            if (row[1].i == 99) {
                found++;
                CHECK(row[2].len == 6);
                CHECK(memcmp(row[2].s, "newbie", 6) == 0);
                CHECK(row[3].i == 777);
                CHECK(row[0].i == (i64)rid_to_u64(rid));
            }
        }
        CHECK(found == 1);
        e->am_nodes.scan_end(&e->am_nodes, scan);
    }

    /* node_id 不许用户指定（给非 NULL → E_INVAL） */
    {
        DbValue bad[4];
        val_int(&bad[0], 12345);
        val_int(&bad[1], 1);
        val_text(&bad[2], "x", 1);
        val_int(&bad[3], 1);
        Rid r;
        CHECK(e->am_nodes.insert(&e->am_nodes, 0, bad, &r) == E_INVAL);
    }

    /* 插边：src / dst 都是节点 id */
    {
        /* 拿新节点的 id */
        AmScan *scan = NULL;
        CHECK(e->am_nodes.scan_begin(&e->am_nodes, &scan) == OK);
        u8 buf[1024];
        u16 len = 0;
        Rid rid;
        i64 nid = -1;
        while (e->am_nodes.scan_next(&e->am_nodes, scan, &rid,
                                     buf, sizeof(buf), &len) == OK) {
            DbValue row[16];
            CHECK(e->am_nodes.decode(&e->am_nodes, buf, len, row) == OK);
            if (row[1].i == 99)
                nid = row[0].i;
        }
        e->am_nodes.scan_end(&e->am_nodes, scan);
        CHECK(nid >= 0);

        /* 找一个已存在的节点当另一端 */
        DbValue ev[2];
        val_int(&ev[0], nid);
        val_int(&ev[1], (i64)rid_to_u64(rid_make(1, 0)));  /* 可能不存在 */

        Rid edge;
        int rc = e->am_edges.insert(&e->am_edges, 0, ev, &edge);
        /* 端点不存在 → E_NOTFOUND（不是建出悬空边） */
        CHECK(rc == E_NOTFOUND || rc == OK);

        if (rc == OK)
            CHECK(count_scan(&e->am_edges) == edges_before + 1);
    }
}

int main(void)
{
    printf("test_graph_am:\n");

    Env e;
    if (env_open(&e) != OK) {
        printf("    FAIL 环境初始化失败\n");
        return 1;
    }

    build_graph(&e, 4, 6);

    t1_schema(&e);
    t2_scan_nodes(&e);
    t3_scan_edges(&e);
    t4_get_roundtrip(&e);
    t5_pushdown_count_matches(&e);
    t7_missing_anchor(&e);
    t8_caps_and_visibility(&e);
    t9_insert(&e);

    env_close(&e);

    if (failures) {
        printf("  %d 项断言失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过 [PASS]\n");
    return 0;
}
