/*
 * test_graph.c — 图记录与邻接链单元验收（v0.11 步骤 2）
 *
 * 8 项：
 *   1. 建节点 → RID 有效；节点数对
 *   2. 属性读写往返（定长列）
 *   3. 加边 → 出邻居/入邻居对
 *   4. **A1 双向一致性**：出邻居 ⟺ 入邻居（逐对节点验）
 *   5. **A2 指针稳定性**：删中间节点后，其余节点的邻接链仍完整
 *   6. 删边 → 两个方向的链都摘干净
 *   7. 删节点 → 连带删掉它的所有出边和入边
 *   8. 重启后接管：节点数、边数、边的端点全对
 *
 * 【第 4 项是核心】
 *
 * 一条边同时挂在两个双向链表里（src 的出链、dst 的入链）。
 * 只要有一个方向漏摘，一个方向查得到、另一个方向查不到 ——
 * 这是图模态最容易出的 bug，而且**加边时看不出来**，
 * 只有删边之后才暴露。所以测试必须"加完删、再加、再删"反复验。
 *
 * 【第 5 项是 PLAN §1.6 的 A2】
 *
 * "删中间节点后，其余节点的邻接链仍完整（删除只摘链 + 挂空闲链，不动别的边）"
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/graph/graph.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/graph_test.db"

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

/* 造一组属性值 */
static void make_vals(DbValue *vals, i64 id, const char *name, i64 score)
{
    val_int(&vals[0], id);
    val_text(&vals[1], name, (u16)strlen(name));
    val_int(&vals[2], score);
}

/* ---------------- WAL 钩子 ---------------- */

typedef struct { int n; } WalCount;

static int count_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                      u16 slot_off, const u8 *before, u16 blen,
                      const u8 *after, u16 alen, u32 *lsn_out)
{
    (void)xid; (void)page_id; (void)slot; (void)slot_off;
    (void)before; (void)blen; (void)after; (void)alen;
    WalCount *c = (WalCount *)ctx;
    if (logtype == LOG_PAGEDATA)
        c->n++;
    if (lsn_out)
        *lsn_out = (u32)(c->n + 1);
    return OK;
}

/* ---------------- 小集合（用于比对） ---------------- */

#define MAXN 128

typedef struct {
    Rid ids[MAXN];
    int n;
} RidSet;

static void set_add(RidSet *s, Rid r)
{
    for (int i = 0; i < s->n; i++)
        if (s->ids[i].page == r.page && s->ids[i].slot == r.slot)
            return;
    if (s->n < MAXN)
        s->ids[s->n++] = r;
}

static bool set_has(const RidSet *s, Rid r)
{
    for (int i = 0; i < s->n; i++)
        if (s->ids[i].page == r.page && s->ids[i].slot == r.slot)
            return true;
    return false;
}

typedef struct {
    RidSet *out;
} NodeCollect;

static int collect_node(void *ctx, Rid rid)
{
    set_add(((NodeCollect *)ctx)->out, rid);
    return OK;
}

typedef struct {
    RidSet *out;
} EdgeCollect;

static int collect_edge(void *ctx, Rid rid)
{
    set_add(((EdgeCollect *)ctx)->out, rid);
    return OK;
}

/* 两个集合是否相同（不看顺序） */
static bool set_same(const RidSet *a, const RidSet *b)
{
    if (a->n != b->n)
        return false;
    for (int i = 0; i < a->n; i++)
        if (!set_has(b, a->ids[i]))
            return false;
    return true;
}

/* ---------------- 各项验收 ---------------- */

static void test_create_node(Graph *g, Rid *a, Rid *b, Rid *c)
{
    printf("  1) 建节点 → RID 有效，节点数对\n");

    DbValue vals[3];

    make_vals(vals, 1, "alpha", 100);
    CHECK(graph_create_node(g, 0, vals, 3, a) == OK);
    CHECK(!rid_is_nil(*a));

    make_vals(vals, 2, "beta", 200);
    CHECK(graph_create_node(g, 0, vals, 3, b) == OK);

    make_vals(vals, 3, "gamma", 300);
    CHECK(graph_create_node(g, 0, vals, 3, c) == OK);

    CHECK(graph_node_count(g) == 3);
}

static void test_props(Graph *g, Rid node)
{
    printf("  2) 属性读写往返（定长列）\n");

    DbValue out[3];
    CHECK(graph_node_props(g, node, out, 3) == OK);

    CHECK(out[0].type == T_INT4);
    CHECK(out[0].i == 1);
    CHECK(out[1].type == T_TEXT);
    CHECK(out[1].len == 5);
    CHECK(memcmp(out[1].s, "alpha", 5) == 0);
    CHECK(out[2].type == T_INT8);
    CHECK(out[2].i == 100);

    /* 不存在的节点 → E_NOTFOUND（不返回垃圾） */
    CHECK(graph_node_props(g, rid_make(9999, 0), out, 3) == E_NOTFOUND);
}

static void test_add_edge(Graph *g, Rid a, Rid b)
{
    printf("  3) 加边 → 出邻居 / 入邻居对\n");

    Rid e;
    CHECK(graph_add_edge(g, 0, a, b, &e) == OK);
    CHECK(graph_edge_count(g) == 1);

    Rid nbr[MAXN];
    u32 n = 0;

    CHECK(graph_out_neighbors(g, a, nbr, MAXN, &n) == OK);
    CHECK(n == 1);
    CHECK(nbr[0].page == b.page && nbr[0].slot == b.slot);

    CHECK(graph_in_neighbors(g, b, nbr, MAXN, &n) == OK);
    CHECK(n == 1);
    CHECK(nbr[0].page == a.page && nbr[0].slot == a.slot);

    /* 反方向应该是空的 */
    CHECK(graph_in_neighbors(g, a, nbr, MAXN, &n) == OK);
    CHECK(n == 0);
    CHECK(graph_out_neighbors(g, b, nbr, MAXN, &n) == OK);
    CHECK(n == 0);

    /* 端点读回 */
    Rid s, d;
    CHECK(graph_edge_endpoints(g, e, &s, &d) == OK);
    CHECK(s.page == a.page && s.slot == a.slot);
    CHECK(d.page == b.page && d.slot == b.slot);

    /* 端点不存在 → 加边失败（不能建出悬空边） */
    Rid bad;
    CHECK(graph_add_edge(g, 0, a, rid_make(9999, 0), &bad) == E_NOTFOUND);
}

/*
 * A1：双向链一致性。
 *
 * 对每一对节点 (u, v)：
 *   "v 是 u 的出邻居" ⟺ "u 是 v 的入邻居"
 *
 * 这比"沿出链走的集合 == 沿入链走的集合"更容易定位问题：
 * 哪一对节点对不上，直接能看出来。
 */
static void test_a1_bidirectional(Graph *g)
{
    printf("  4) A1 双向一致性：出邻居 ⟺ 入邻居\n");

    RidSet nodes = { { {0,0} }, 0 };
    NodeCollect nc = { &nodes };
    CHECK(graph_each_node(g, collect_node, &nc) == OK);
    CHECK(nodes.n >= 3);

    /* 建一个全连接图：每个节点出度 n-1、入度 n-1 */
    u64 edges_before = graph_edge_count(g);
    for (int i = 0; i < nodes.n; i++) {
        for (int j = 0; j < nodes.n; j++) {
            if (i == j)
                continue;
            Rid e;
            CHECK(graph_add_edge(g, 0, nodes.ids[i], nodes.ids[j], &e) == OK);
        }
    }
    CHECK(graph_edge_count(g) == edges_before + (u64)nodes.n * (u64)(nodes.n - 1));

    /* 逐个节点验：出邻居集合 == 入邻居集合 */
    for (int i = 0; i < nodes.n; i++) {
        RidSet outn = { { {0,0} }, 0 };
        RidSet inn = { { {0,0} }, 0 };

        Rid buf[MAXN];
        u32 n = 0;
        CHECK(graph_out_neighbors(g, nodes.ids[i], buf, MAXN, &n) == OK);
        for (u32 k = 0; k < n; k++)
            set_add(&outn, buf[k]);

        CHECK(graph_in_neighbors(g, nodes.ids[i], buf, MAXN, &n) == OK);
        for (u32 k = 0; k < n; k++)
            set_add(&inn, buf[k]);

        CHECK(outn.n == nodes.n - 1);
        CHECK(set_same(&outn, &inn));
    }

    /* 边枚举：每条边的端点必须能互相走到 */
    RidSet edges = { { {0,0} }, 0 };
    EdgeCollect ec = { &edges };
    CHECK(graph_each_edge(g, collect_edge, &ec) == OK);
    CHECK((u64)edges.n == graph_edge_count(g));

    for (int i = 0; i < edges.n; i++) {
        Rid src, dst;
        CHECK(graph_edge_endpoints(g, edges.ids[i], &src, &dst) == OK);

        Rid buf[MAXN];
        u32 n = 0;

        CHECK(graph_out_neighbors(g, src, buf, MAXN, &n) == OK);
        bool found = false;
        for (u32 k = 0; k < n; k++)
            if (buf[k].page == dst.page && buf[k].slot == dst.slot)
                found = true;
        CHECK(found);

        CHECK(graph_in_neighbors(g, dst, buf, MAXN, &n) == OK);
        found = false;
        for (u32 k = 0; k < n; k++)
            if (buf[k].page == src.page && buf[k].slot == src.slot)
                found = true;
        CHECK(found);
    }
}

/*
 * A2：指针稳定性。
 *
 * 场景：a → b → c → d 是一条链。删掉中间的 b。
 * 期望：
 *   - b 的两条边都被摘掉
 *   - a 的出链空了（唯一邻居 b 没了）
 *   - c 的出链里还有 d（**c 的链没被动过**）
 *   - c 的入链空了（唯一入边来自 b）
 *   - d 的入链里还有 c（**d 的链没被动过**）
 *
 * 「其余节点的邻接链仍完整」就是最后两条。
 */
static void test_a2_pointer_stability(Graph *g)
{
    printf("  5) A2 指针稳定性：删中间节点，其余节点的链仍完整\n");

    DbValue vals[3];
    Rid a, b, c, d;
    Rid eab, ebc, ecd;

    make_vals(vals, 10, "a", 1);
    CHECK(graph_create_node(g, 0, vals, 3, &a) == OK);
    make_vals(vals, 20, "b", 2);
    CHECK(graph_create_node(g, 0, vals, 3, &b) == OK);
    make_vals(vals, 30, "c", 3);
    CHECK(graph_create_node(g, 0, vals, 3, &c) == OK);
    make_vals(vals, 40, "d", 4);
    CHECK(graph_create_node(g, 0, vals, 3, &d) == OK);

    CHECK(graph_add_edge(g, 0, a, b, &eab) == OK);
    CHECK(graph_add_edge(g, 0, b, c, &ebc) == OK);
    CHECK(graph_add_edge(g, 0, c, d, &ecd) == OK);

    /* 删中间节点 b */
    CHECK(graph_remove_node(g, 0, b) == OK);

    Rid buf[MAXN];
    u32 n = 0;

    /* a 的出链空了 */
    CHECK(graph_out_neighbors(g, a, buf, MAXN, &n) == OK);
    CHECK(n == 0);

    /* a 的入链本来就没有 */
    CHECK(graph_in_neighbors(g, a, buf, MAXN, &n) == OK);
    CHECK(n == 0);

    /* c 的出链里还有 d —— c 的链没被动过 */
    CHECK(graph_out_neighbors(g, c, buf, MAXN, &n) == OK);
    CHECK(n == 1);
    CHECK(buf[0].page == d.page && buf[0].slot == d.slot);

    /* c 的入链空了（唯一入边来自 b） */
    CHECK(graph_in_neighbors(g, c, buf, MAXN, &n) == OK);
    CHECK(n == 0);

    /* d 的入链里还有 c —— d 的链没被动过 */
    CHECK(graph_in_neighbors(g, d, buf, MAXN, &n) == OK);
    CHECK(n == 1);
    CHECK(buf[0].page == c.page && buf[0].slot == c.slot);

    /* 删掉的节点读不到 */
    DbValue out[3];
    CHECK(graph_node_props(g, b, out, 3) == E_NOTFOUND);
}

static void test_remove_edge(Graph *g)
{
    printf("  6) 删边 → 两个方向的链都摘干净\n");

    DbValue vals[3];
    Rid a, b;
    make_vals(vals, 1, "x", 1);
    CHECK(graph_create_node(g, 0, vals, 3, &a) == OK);
    make_vals(vals, 2, "y", 2);
    CHECK(graph_create_node(g, 0, vals, 3, &b) == OK);

    u64 edges_before = graph_edge_count(g);

    Rid e1, e2;
    CHECK(graph_add_edge(g, 0, a, b, &e1) == OK);
    CHECK(graph_add_edge(g, 0, a, b, &e2) == OK);

    Rid buf[MAXN];
    u32 n = 0;
    CHECK(graph_out_neighbors(g, a, buf, MAXN, &n) == OK);
    CHECK(n == 2);
    CHECK(graph_in_neighbors(g, b, buf, MAXN, &n) == OK);
    CHECK(n == 2);

    /* 删头插的那条（e2 是链头，e1 是链尾） */
    CHECK(graph_remove_edge(g, 0, e2) == OK);
    CHECK(graph_edge_count(g) == edges_before + 1);

    /* 出链还剩 1 条 */
    CHECK(graph_out_neighbors(g, a, buf, MAXN, &n) == OK);
    CHECK(n == 1);
    CHECK(graph_in_neighbors(g, b, buf, MAXN, &n) == OK);
    CHECK(n == 1);

    /* 删最后一条 → 两个方向都空 */
    CHECK(graph_remove_edge(g, 0, e1) == OK);
    CHECK(graph_out_neighbors(g, a, buf, MAXN, &n) == OK);
    CHECK(n == 0);
    CHECK(graph_in_neighbors(g, b, buf, MAXN, &n) == OK);
    CHECK(n == 0);
    CHECK(graph_edge_count(g) == edges_before);
}

static void test_remove_node(Graph *g)
{
    printf("  7) 删节点 → 连带删掉它的所有出边和入边\n");

    DbValue vals[3];
    Rid hub, x, y;
    make_vals(vals, 1, "h", 1);
    CHECK(graph_create_node(g, 0, vals, 3, &hub) == OK);
    make_vals(vals, 2, "x", 2);
    CHECK(graph_create_node(g, 0, vals, 3, &x) == OK);
    make_vals(vals, 3, "y", 3);
    CHECK(graph_create_node(g, 0, vals, 3, &y) == OK);

    Rid e1, e2, e3;
    CHECK(graph_add_edge(g, 0, hub, x, &e1) == OK);
    CHECK(graph_add_edge(g, 0, y, hub, &e2) == OK);
    CHECK(graph_add_edge(g, 0, hub, y, &e3) == OK);

    u64 edges_before = graph_edge_count(g);
    CHECK(edges_before >= 3);

    CHECK(graph_remove_node(g, 0, hub) == OK);
    CHECK(graph_edge_count(g) == edges_before - 3);

    /* x 的入链空了（hub → x 那条被摘了） */
    Rid buf[MAXN];
    u32 n = 0;
    CHECK(graph_in_neighbors(g, x, buf, MAXN, &n) == OK);
    CHECK(n == 0);

    /* y 的出链空了（y → hub 那条被摘了） */
    CHECK(graph_out_neighbors(g, y, buf, MAXN, &n) == OK);
    CHECK(n == 0);

    /* y 的出链里也不该再有 hub 的边 */
    CHECK(graph_out_neighbors(g, y, buf, MAXN, &n) == OK);
    CHECK(n == 0);
}

/*
 * 8) 重启后接管。
 *
 * 节点数、边数、每条边的端点全对。
 * 这是 A5（崩溃恢复）的地基 —— 步骤 6 会在这上面加"断电"。
 */
static void test_recover(void)
{
    printf("  8) 重启接管：节点数 / 边数 / 边的端点全对\n");

    GraphCol cols[3];
    make_cols(cols);

    RidSet nodes = { { {0,0} }, 0 };
    RidSet edges = { { {0,0} }, 0 };
    Rid saved_src[64], saved_dst[64];
    int nsaved = 0;
    u64 nodes_before = 0, edges_before = 0;

    {
        Pager *pg = NULL;
        CHECK(pager_open(PATH, 64, &pg) == OK);

        Graph *g = NULL;
        CHECK(graph_create(pg, NULL, NULL, cols, 3, &g) == OK);

        /* 造一个有结构的图 */
        DbValue vals[3];
        Rid n[4];
        for (int i = 0; i < 4; i++) {
            char nm[8];
            snprintf(nm, sizeof(nm), "n%d", i);
            make_vals(vals, i, nm, i * 10);
            CHECK(graph_create_node(g, 0, vals, 3, &n[i]) == OK);
        }
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                if (i != j) {
                    Rid e;
                    CHECK(graph_add_edge(g, 0, n[i], n[j], &e) == OK);
                }

        nodes_before = graph_node_count(g);
        edges_before = graph_edge_count(g);
        CHECK(nodes_before == 4);
        CHECK(edges_before == 12);

        NodeCollect nc = { &nodes };
        CHECK(graph_each_node(g, collect_node, &nc) == OK);
        EdgeCollect ec = { &edges };
        CHECK(graph_each_edge(g, collect_edge, &ec) == OK);

        for (int i = 0; i < edges.n && i < 64; i++) {
            CHECK(graph_edge_endpoints(g, edges.ids[i],
                                       &saved_src[i], &saved_dst[i]) == OK);
            nsaved++;
        }

        graph_destroy(g);
        pager_close(pg);   /* 正常关闭 → 刷盘 */
    }

    /* 重开 */
    {
        Pager *pg = NULL;
        CHECK(pager_open(PATH, 64, &pg) == OK);

        Graph *g = NULL;
        CHECK(graph_create(pg, NULL, NULL, cols, 3, &g) == OK);

        CHECK(graph_node_count(g) == nodes_before);
        CHECK(graph_edge_count(g) == edges_before);

        RidSet edges2 = { { {0,0} }, 0 };
        EdgeCollect ec2 = { &edges2 };
        CHECK(graph_each_edge(g, collect_edge, &ec2) == OK);
        CHECK(edges2.n == nsaved);

        /* 每条边的端点逐条比对 */
        for (int i = 0; i < nsaved; i++) {
            Rid s, d;
            CHECK(graph_edge_endpoints(g, edges2.ids[i], &s, &d) == OK);
            CHECK(s.page == saved_src[i].page && s.slot == saved_src[i].slot);
            CHECK(d.page == saved_dst[i].page && d.slot == saved_dst[i].slot);
        }

        /* 邻接链结构也还在：每个节点的出邻居数 == 入邻居数 == 3 */
        RidSet nodes2 = { { {0,0} }, 0 };
        NodeCollect nc2 = { &nodes2 };
        CHECK(graph_each_node(g, collect_node, &nc2) == OK);

        for (int i = 0; i < nodes2.n; i++) {
            Rid buf[MAXN];
            u32 n = 0;
            CHECK(graph_out_neighbors(g, nodes2.ids[i], buf, MAXN, &n) == OK);
            CHECK(n == 3);
            CHECK(graph_in_neighbors(g, nodes2.ids[i], buf, MAXN, &n) == OK);
            CHECK(n == 3);
        }

        graph_destroy(g);
        pager_close(pg);
    }
}

int main(void)
{
    printf("test_graph:\n");

    remove(PATH);

    GraphCol cols[3];
    make_cols(cols);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    WalCount wc = { 0 };
    Graph *g = NULL;
    CHECK(graph_create(pg, count_hook, &wc, cols, 3, &g) == OK);
    CHECK(graph_col_count(g) == 3);
    CHECK(graph_node_record_size(g) > GR_N_BITMAP);

    Rid a, b, c;
    test_create_node(g, &a, &b, &c);

    /* 写操作必须产生整页镜像日志（邻接链指针是数据，日志里没有就重放不回来） */
    CHECK(wc.n > 0);
    test_props(g, a);
    test_add_edge(g, a, b);
    test_a1_bidirectional(g);
    test_a2_pointer_stability(g);
    test_remove_edge(g);
    test_remove_node(g);

    graph_destroy(g);
    pager_close(pg);

    /* 第 8 项自己开关库，所以要重新 remove */
    remove(PATH);
    test_recover();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
