/*
 * test_geo_rtree.c —— R-tree 的验收（v1.4 步骤 2 · A2）
 *
 * 五项验收：
 *   1. **窗口全等**：1000 个确定性随机点 + 一批网格点，
 *      对若干窗口（随机窗口、卡在边界上的窗口、整片区域的窗口），
 *      R-tree 的结果与暴力基准 geo_brute_window **集合完全相同**。
 *   2. **结构自检**：逐节点断言内部条目记的 MBR **恰好**是它下面
 *      那棵子树所有点的包围盒（不多不少）；并断言树高不是链。
 *   3. **不丢点**：插入 N 个点后用整片平面当窗口 → 必须返回全部 N 个。
 *   4. **页读计数**：窗口查询的页读 > 0，且小窗口的页读明显少于全扫。
 *   5. **重开**：rtree_open 从根页打开 → 点数/树高/查询结果一致。
 *
 * 【为什么第 1 项必须"逐位全等"而不是"差不多"】
 *
 * 空间查询最容易出的错是**静默漏点**：剪枝条件写反一个符号，
 * 结果就少几个点，而且**不报错** —— 只是答案悄悄不对。
 * "看起来差不多"抓不住这类 bug，只有集合全等能抓住。
 *
 * 【为什么边界点要专门测】
 *
 * 本库统一**闭区间**：点落在窗口边界上算命中。
 * 剪枝用开区间、查询用闭区间，是最经典的漏点来源。
 * 所以网格点 + 卡在网格线上的窗口必须单独打一遍：
 * 边界上的点，**两边都必须有**。
 *
 * 【为什么随机数自己写 LCG】
 *
 * rand() 的实现跨平台/跨 libc 不一致，同一份测试在不同机器上
 * 跑出不同的数据集，bug 就没法复现。自己写 LCG：
 * 同样的种子 → 同样的数据 → 同样的结果。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_brute.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"
#include "storage/page.h"
#include "storage/pager.h"

#define PATH "test/tmp/g6_rtree.db"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 确定性伪随机 ---------------- */

static u32 lcg_state = 1u;

static void lcg_seed(u32 s)
{
    lcg_state = s;
}

static u32 lcg_next(void)
{
    lcg_state = lcg_state * 1664525u + 1013904223u;
    return lcg_state;
}

/* [lo, hi) 内的浮点数 */
static float lcg_range(float lo, float hi)
{
    /* 取高 24 位 → [0, 1)，避免低位相关性 */
    double u = (double)(lcg_next() >> 8) / 16777216.0;

    return (float)(lo + (hi - lo) * u);
}

/* ---------------- 测试用的缓冲（放文件级，避免大栈） ---------------- */

#define MAX_PTS  8192
#define MAX_OUT  8192

static GeoPoint g_pts[MAX_PTS];
static u32      g_a[MAX_OUT];
static u32      g_b[MAX_OUT];

/* ---------------- 独立重算子树包围盒 ---------------- */

/*
 * 【为什么要"独立"重算】
 *
 * 这一项查的是"记多了"：树里记的 MBR 可能是**旧的**（插入后没更新），
 * 也可能是**虚胖**的（把不相关的点裹进来）。
 * 虚胖不会漏点，但会让剪枝失效 —— 查询变慢，而且**不报错**。
 *
 * 所以这里绕开树里记的 MBR，从叶子往上重算：
 *   叶子 → 它的点的退化矩形的并集；
 *   内部节点 → 孩子们的包围盒的并集。
 * 两者必须**逐位相同**（min/max 是精确运算，与顺序无关）。
 */
static int subtree_bbox(Pager *pg, u32 pid, Mbr *out, u32 *npts, u32 depth)
{
    PageFrame *fr = NULL;
    const u8 *ents;
    u32 n;
    u32 i;
    bool leaf;

    if (depth > 64u)
        return -1;
    if (pager_get(pg, pid, &fr) != OK)
        return -1;

    n = get_u16(fr->data + RT_NENT_OFF);
    leaf = (fr->data[RT_FLAGS_OFF] & 1u) != 0;
    ents = fr->data + RT_ENT_OFF;

    if (leaf) {
        Mbr m;
        bool first = true;

        for (i = 0; i < n; i++) {
            const u8 *p = ents + (size_t)i * RT_ENT_SIZE;
            float x = rt_u2f(get_u32(p + 4));
            float y = rt_u2f(get_u32(p + 8));
            float mx = rt_u2f(get_u32(p + 12));
            float my = rt_u2f(get_u32(p + 16));

            if (first) {
                m.minx = x;
                m.miny = y;
                m.maxx = mx;
                m.maxy = my;
                first = false;
            } else {
                if (x < m.minx)
                    m.minx = x;
                if (y < m.miny)
                    m.miny = y;
                if (mx > m.maxx)
                    m.maxx = mx;
                if (my > m.maxy)
                    m.maxy = my;
            }
        }
        if (first)
            mbr_set_rect(&m, 0.0f, 0.0f, 0.0f, 0.0f);
        *out = m;
        *npts = n;
        pager_unpin(pg, pid, false);
        return 0;
    }

    {
        Mbr acc;
        bool first = true;
        u32 total = 0;

        for (i = 0; i < n; i++) {
            u32 child = get_u32(ents + (size_t)i * RT_ENT_SIZE);
            Mbr cm;
            u32 cn;

            if (subtree_bbox(pg, child, &cm, &cn, depth + 1) != 0) {
                pager_unpin(pg, pid, false);
                return -1;
            }
            if (first) {
                acc = cm;
                first = false;
            } else {
                mbr_union(&acc, &cm, &acc);
            }
            total += cn;
        }
        if (first)
            mbr_set_rect(&acc, 0.0f, 0.0f, 0.0f, 0.0f);
        *out = acc;
        *npts = total;
    }

    pager_unpin(pg, pid, false);
    return 0;
}

/* 逐节点校验内部条目记的 MBR 是否"恰好" */
static int check_node_mbrs(Pager *pg, u32 pid, int *bad, u32 depth)
{
    PageFrame *fr = NULL;
    const u8 *ents;
    u32 n;
    u32 i;
    bool leaf;

    if (depth > 64u) {
        (*bad)++;
        return -1;
    }
    if (pager_get(pg, pid, &fr) != OK) {
        (*bad)++;
        return -1;
    }

    n = get_u16(fr->data + RT_NENT_OFF);
    leaf = (fr->data[RT_FLAGS_OFF] & 1u) != 0;
    ents = fr->data + RT_ENT_OFF;

    if (!leaf) {
        for (i = 0; i < n; i++) {
            const u8 *p = ents + (size_t)i * RT_ENT_SIZE;
            u32 child = get_u32(p);
            float mnx = rt_u2f(get_u32(p + 4));
            float mny = rt_u2f(get_u32(p + 8));
            float mxx = rt_u2f(get_u32(p + 12));
            float mxy = rt_u2f(get_u32(p + 16));
            Mbr exact;
            u32 cn = 0;

            if (subtree_bbox(pg, child, &exact, &cn, depth + 1) != 0) {
                (*bad)++;
                continue;
            }
            if (mnx != exact.minx || mny != exact.miny ||
                mxx != exact.maxx || mxy != exact.maxy) {
                printf("    FAIL 内部条目 MBR 不精确：记 [%.9g,%.9g]-[%.9g,%.9g]"
                       " 实际 [%.9g,%.9g]-[%.9g,%.9g]\n",
                       mnx, mny, mxx, mxy,
                       exact.minx, exact.miny, exact.maxx, exact.maxy);
                (*bad)++;
            }
            check_node_mbrs(pg, child, bad, depth + 1);
        }
    }

    pager_unpin(pg, pid, false);
    return 0;
}

/* 树高的上限：h <= 2 + log_RT_MIN(n)（叶子数下界推出来的） */
static u32 max_height_for(u32 n)
{
    u32 h = 2;
    u32 v = n;

    while (v >= RT_MIN) {
        v /= RT_MIN;
        h++;
    }
    return h;
}

/* ---------------- 1. 窗口全等（随机点） ---------------- */

static void t_window_equiv_random(void)
{
    enum { N = 1000 };
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 i;
    int w;

    remove(PATH);
    CHECK(pager_open(PATH, 512, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    lcg_seed(20261026u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-1000.0f, 1000.0f);
        g_pts[i].y = lcg_range(-1000.0f, 1000.0f);
        CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
    }
    CHECK(rtree_npoints(tree) == N);

    for (w = 0; w < 60; w++) {
        Mbr win;
        u32 na = 0;
        u32 nb = 0;
        float cx;
        float cy;
        float rx;
        float ry;

        /* 窗口中心取在一个随机点上，保证大部分窗口有命中 */
        cx = g_pts[lcg_next() % N].x;
        cy = g_pts[lcg_next() % N].y;
        rx = lcg_range(0.0f, 300.0f);
        ry = lcg_range(0.0f, 300.0f);
        mbr_set_rect(&win, cx - rx, cy - ry, cx + rx, cy + ry);

        CHECK(geo_brute_window(g_pts, N, &win, g_b, MAX_OUT, &nb) == OK);
        CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &na) == OK);

        geo_ids_sort(g_a, na);
        geo_ids_sort(g_b, nb);

        if (na != nb) {
            printf("    FAIL 随机窗口 %d 命中数不同：树 %u，暴力 %u\n", w, na, nb);
            failures++;
        } else if (na > 0 && memcmp(g_a, g_b, (size_t)na * sizeof(u32)) != 0) {
            printf("    FAIL 随机窗口 %d 命中集合不同\n", w);
            failures++;
        }
    }

    /* 整片区域窗口 → 全部 N 个（不丢点） */
    {
        Mbr full;
        u32 na = 0;
        u32 nb = 0;

        mbr_set_rect(&full, -1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f);
        CHECK(geo_brute_window(g_pts, N, &full, g_b, MAX_OUT, &nb) == OK);
        CHECK(rtree_window(tree, &full, g_a, MAX_OUT, &na) == OK);
        geo_ids_sort(g_a, na);
        geo_ids_sort(g_b, nb);
        CHECK(na == N);
        CHECK(nb == N);
        CHECK(na == nb && memcmp(g_a, g_b, (size_t)na * sizeof(u32)) == 0);
    }

    /* 缓冲区装不下 → 报错，不静默截断 */
    {
        Mbr full;
        u32 na = 0;

        mbr_set_rect(&full, -1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f);
        CHECK(rtree_window(tree, &full, g_a, 10, &na) == E_NOSPACE);
    }

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 2. 窗口全等（网格点 + 卡边界的窗口） ---------------- */

static void t_window_equiv_grid(void)
{
    enum { G = 40, N = G * G };
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 i;
    u32 gx;
    u32 gy;

    remove(PATH);
    CHECK(pager_open(PATH, 512, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    /* 40x40 网格，整数坐标 —— 窗口边界可以正好卡在网格线上 */
    for (gy = 0; gy < G; gy++) {
        for (gx = 0; gx < G; gx++) {
            i = gy * G + gx;
            g_pts[i].row_id = i + 1u;
            g_pts[i].x = (float)gx;
            g_pts[i].y = (float)gy;
            CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
        }
    }
    CHECK(rtree_npoints(tree) == N);

    /* 窗口 [0,0]-[9,9]：x,y ∈ {0..9} 共 100 个点，边界上的 0 与 9 都在内 */
    {
        Mbr win;
        u32 na = 0;
        u32 nb = 0;

        mbr_set_rect(&win, 0.0f, 0.0f, 9.0f, 9.0f);
        CHECK(geo_brute_window(g_pts, N, &win, g_b, MAX_OUT, &nb) == OK);
        CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &na) == OK);
        geo_ids_sort(g_a, na);
        geo_ids_sort(g_b, nb);
        CHECK(na == 100);
        CHECK(nb == 100);
        CHECK(na == nb && memcmp(g_a, g_b, (size_t)na * sizeof(u32)) == 0);
    }

    /* 一批卡在网格线上的窗口 */
    for (i = 0; i < 40; i++) {
        Mbr win;
        u32 na = 0;
        u32 nb = 0;
        float x0 = (float)(lcg_next() % 30u);
        float y0 = (float)(lcg_next() % 30u);
        float x1 = x0 + (float)(1u + lcg_next() % 10u);
        float y1 = y0 + (float)(1u + lcg_next() % 10u);

        mbr_set_rect(&win, x0, y0, x1, y1);
        CHECK(geo_brute_window(g_pts, N, &win, g_b, MAX_OUT, &nb) == OK);
        CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &na) == OK);
        geo_ids_sort(g_a, na);
        geo_ids_sort(g_b, nb);
        if (na != nb) {
            printf("    FAIL 网格窗口 %u 命中数不同：树 %u，暴力 %u\n", i, na, nb);
            failures++;
        } else if (na > 0 && memcmp(g_a, g_b, (size_t)na * sizeof(u32)) != 0) {
            printf("    FAIL 网格窗口 %u 命中集合不同\n", i);
            failures++;
        }
    }

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 3. 边界点专项（闭区间） ---------------- */

static void t_boundary(void)
{
    RTree *tree = NULL;
    Pager *pg = NULL;
    Mbr win;
    u32 na = 0;
    u32 nb = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    /* 四个点分别落在窗口的四条边上（含角点）—— 全部都该命中 */
    g_pts[0].row_id = 1u; g_pts[0].x = 0.0f;  g_pts[0].y = 5.0f;   /* 左边 */
    g_pts[1].row_id = 2u; g_pts[1].x = 10.0f; g_pts[1].y = 5.0f;   /* 右边 */
    g_pts[2].row_id = 3u; g_pts[2].x = 5.0f;  g_pts[2].y = 0.0f;   /* 下边 */
    g_pts[3].row_id = 4u; g_pts[3].x = 5.0f;  g_pts[3].y = 10.0f;  /* 上边 */
    g_pts[4].row_id = 5u; g_pts[4].x = 0.0f;  g_pts[4].y = 0.0f;   /* 左下角 */
    g_pts[5].row_id = 6u; g_pts[5].x = 10.0f; g_pts[5].y = 10.0f;  /* 右上角 */

    for (na = 0; na < 6; na++) {
        CHECK(rtree_insert(tree, g_pts[na].row_id, g_pts[na].x, g_pts[na].y) == OK);
    }

    mbr_set_rect(&win, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(geo_brute_window(g_pts, 6, &win, g_b, MAX_OUT, &nb) == OK);
    CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &na) == OK);
    geo_ids_sort(g_a, na);
    geo_ids_sort(g_b, nb);
    CHECK(na == 6);          /* 边界上的点一个都不能少 */
    CHECK(nb == 6);
    CHECK(na == nb && memcmp(g_a, g_b, (size_t)na * sizeof(u32)) == 0);

    /*
     * 窗口边界往里挪一点点 → 原来贴着左边与左下角的两个点落到窗口外。
     *
     * 注意这里**不改点**，只改窗口：树里存的是当初插入的坐标，
     * 拿一份改过的数组去比树，是两个不同的数据集，比出来的差异没有意义。
     */
    mbr_set_rect(&win, 0.001f, 0.0f, 10.0f, 10.0f);
    CHECK(geo_brute_window(g_pts, 6, &win, g_b, MAX_OUT, &nb) == OK);
    CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &na) == OK);
    CHECK(na == 4);
    CHECK(nb == 4);

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 4. 结构自检（MBR 恰好 + 树高） ---------------- */

static void t_structure(void)
{
    enum { N = 1000 };
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 i;
    int bad = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 512, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    lcg_seed(777u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-500.0f, 500.0f);
        g_pts[i].y = lcg_range(-500.0f, 500.0f);
        CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
    }

    /* 逐节点：内部条目记的 MBR 必须恰好是子树的包围盒 */
    check_node_mbrs(pg, rtree_root_page(tree), &bad, 0);
    CHECK(bad == 0);

    /* 树高：1000 个点、每叶至少 102 个 → 高度只能是 2 */
    printf("    1000 点：树高 %u（上限 %u），点数 %u\n",
           rtree_height(tree), max_height_for(N), rtree_npoints(tree));
    CHECK(rtree_height(tree) >= 1);
    CHECK(rtree_height(tree) <= max_height_for(N));
    CHECK(rtree_height(tree) >= 2);      /* 1000 > 203，必须分裂过 */

    rtree_destroy(tree);
    pager_close(pg);
}

/* 小树：点数不超过一页容量 → 单叶，结构检查同样要过 */
static void t_structure_small(void)
{
    enum { N = 100 };
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 i;
    int bad = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 128, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    lcg_seed(4242u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-50.0f, 50.0f);
        g_pts[i].y = lcg_range(-50.0f, 50.0f);
        CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
    }

    check_node_mbrs(pg, rtree_root_page(tree), &bad, 0);
    CHECK(bad == 0);
    CHECK(rtree_height(tree) == 1);      /* 100 个点装得进一页 */

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 5. 页读计数 ---------------- */

static void t_page_reads(void)
{
    enum { N = 4000 };
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 i;
    u64 before;
    u64 full_reads;
    u64 small_reads;
    u32 n = 0;
    Mbr full;
    Mbr small;

    remove(PATH);
    CHECK(pager_open(PATH, 1024, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    lcg_seed(31337u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-1000.0f, 1000.0f);
        g_pts[i].y = lcg_range(-1000.0f, 1000.0f);
        CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
    }

    /* 全扫：整片平面 */
    mbr_set_rect(&full, -1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f);
    before = rtree_page_reads(tree);
    CHECK(rtree_window(tree, &full, g_a, MAX_OUT, &n) == OK);
    full_reads = rtree_page_reads(tree) - before;
    CHECK(n == N);

    /* 小窗：剪枝应该把大部分子树挡在门外 */
    mbr_set_rect(&small, 0.0f, 0.0f, 1.0f, 1.0f);
    before = rtree_page_reads(tree);
    CHECK(rtree_window(tree, &small, g_b, MAX_OUT, &n) == OK);
    small_reads = rtree_page_reads(tree) - before;

    printf("    页读：全扫 %llu，小窗 %llu（叶子约 %u 个）\n",
           (unsigned long long)full_reads,
           (unsigned long long)small_reads,
           (unsigned)(full_reads > 0 ? full_reads - 1u : 0u));

    CHECK(rtree_page_reads(tree) > 0);
    CHECK(full_reads > 0);
    CHECK(small_reads > 0);
    CHECK(small_reads < full_reads);     /* 小窗明显少于全扫 */

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 6. 重开（rtree_open） ---------------- */

static void t_reopen(void)
{
    enum { N = 500 };
    RTree *tree = NULL;
    RTree *tree2 = NULL;
    Pager *pg = NULL;
    u32 i;
    Mbr win;
    u32 n1 = 0;
    u32 n2 = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 512, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    lcg_seed(9001u);
    for (i = 0; i < N; i++) {
        g_pts[i].row_id = i + 1u;
        g_pts[i].x = lcg_range(-200.0f, 200.0f);
        g_pts[i].y = lcg_range(-200.0f, 200.0f);
        CHECK(rtree_insert(tree, g_pts[i].row_id, g_pts[i].x, g_pts[i].y) == OK);
    }

    /* 从根页重开 */
    CHECK(rtree_open(pg, rtree_root_page(tree), &tree2) == OK);
    CHECK(rtree_npoints(tree2) == N);
    CHECK(rtree_height(tree2) == rtree_height(tree));
    CHECK(rtree_root_page(tree2) == rtree_root_page(tree));

    mbr_set_rect(&win, -50.0f, -50.0f, 50.0f, 50.0f);
    CHECK(rtree_window(tree, &win, g_a, MAX_OUT, &n1) == OK);
    CHECK(rtree_window(tree2, &win, g_b, MAX_OUT, &n2) == OK);
    geo_ids_sort(g_a, n1);
    geo_ids_sort(g_b, n2);
    CHECK(n1 == n2);
    CHECK(n1 == 0 || memcmp(g_a, g_b, (size_t)n1 * sizeof(u32)) == 0);

    /* 空树：打开一个不存在的根 → 打开一棵空树 */
    {
        RTree *empty = NULL;

        CHECK(rtree_open(pg, PAGE_ID_NIL, &empty) == OK);
        CHECK(rtree_npoints(empty) == 0);
        CHECK(rtree_height(empty) == 0);
        CHECK(rtree_root_page(empty) == PAGE_ID_NIL);
        rtree_destroy(empty);
    }

    rtree_destroy(tree2);
    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 7. 参数校验 ---------------- */

static void t_invalid(void)
{
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 n = 0;
    Mbr win;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    CHECK(rtree_create(NULL, &tree) == E_INVAL);
    CHECK(rtree_create(pg, NULL) == E_INVAL);

    CHECK(rtree_create(pg, &tree) == OK);

    /* NaN / Inf 必须被拒 —— 它们会让剪枝静默失效 */
    CHECK(rtree_insert(tree, 1, 0.0f, 0.0f) == OK);
    CHECK(rtree_insert(tree, 2, INFINITY, 0.0f) == E_INVAL);
    CHECK(rtree_insert(tree, 3, 0.0f, INFINITY) == E_INVAL);
    CHECK(rtree_insert(tree, 4, NAN, 0.0f) == E_INVAL);
    CHECK(rtree_insert(NULL, 5, 0.0f, 0.0f) == E_INVAL);
    CHECK(rtree_npoints(tree) == 1);

    mbr_set_rect(&win, 0.0f, 0.0f, 1.0f, 1.0f);
    CHECK(rtree_window(NULL, &win, g_a, MAX_OUT, &n) == E_INVAL);
    CHECK(rtree_window(tree, NULL, g_a, MAX_OUT, &n) == E_INVAL);
    CHECK(rtree_window(tree, &win, NULL, MAX_OUT, &n) == E_INVAL);
    CHECK(rtree_window(tree, &win, g_a, MAX_OUT, NULL) == E_INVAL);

    rtree_destroy(tree);
    pager_close(pg);
}

int main(void)
{
    printf("test_geo_rtree ...\n");

    t_window_equiv_random();
    t_window_equiv_grid();
    t_boundary();
    t_structure();
    t_structure_small();
    t_page_reads();
    t_reopen();
    t_invalid();

    remove(PATH);

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
