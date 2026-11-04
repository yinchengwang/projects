/*
 * test_geo_dead.c —— R-tree 的死条目标记与全树枚举（v1.4 步骤 5）
 *
 * 八项验收：
 *   1. 标死一个点后，窗口查询**不再返回**它（其余照常返回）。
 *   2. 标死不存在的 row_id → E_NOTFOUND。
 *   3. 重复标死同一个 row_id → OK（幂等）。
 *   4. rtree_scan_all 枚举到的点数 == N，被删的那个 dead == true、
 *      row_id 与坐标都与插入时一致。
 *   5. rtree_count_live：n_live == N-1、n_total == N。
 *   6. KNN 不返回死点（把死点放成离查询点**最近**的那个）。
 *   7. rtree_insert 传 row_id = 0x80000000u → E_INVAL。
 *   8. 空树（root_page == PAGE_ID_NIL）：枚举 0 条、标死 E_NOTFOUND、
 *      计数都是 0。
 *
 * 【为什么"死条目"要用最高位而不是加字段】
 *
 * 条目是定长 20 字节，一页塞 203 条。加一个字节就要重排布局、
 * 破坏 MBR 的 4 字节对齐；用 id 的最高位当标志，一个字节都不用加。
 * 代价是 row_id 只有 31 位 —— 所以第 7 项专门测越界被拒。
 *
 * 【为什么删除的代价是 O(n)】
 *
 * 树里没有"按 row_id 查坐标"的索引，标死只能全树扫一遍。
 * 这一项在第 2 项里顺带量了页读，如实记录。
 *
 * 【pin/unpin 纪律】
 *
 * 每次 pager_get 都要配一次 pager_unpin。泄漏的内容锁会让后面的
 * latch_order_acquire(LATCH_TXN) 断言崩 —— 所以本测试跑完还能正常
 * 关闭 pager（pager_close 会检查 pin 配平）。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_knn.h"
#include "modalities/geo/rtree_scan.h"
#include "storage/pager.h"

#define PATH "test/tmp/g7_dead.db"

/* 测试规模：300 个点 > 一页容量 203 ⇒ 一定会分裂出内部节点 */
#define N_PTS  300

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 测试用的点集 ---------------- */

static float g_x[N_PTS];
static float g_y[N_PTS];

/* 网格点：20 × 15，坐标互不相同，便于逐点核对 */
static void make_grid(void)
{
    u32 i;

    for (i = 0; i < N_PTS; i++) {
        g_x[i] = (float)(i % 20u) * 10.0f;
        g_y[i] = (float)(i / 20u) * 10.0f;
    }
}

/* ---------------- 枚举回调 ---------------- */

typedef struct {
    u32  n;            /* 回调收到的点数 */
    u32  n_dead;       /* 其中 dead == true 的个数 */
    u32  dead_row;     /* 找到的"死点"的 row_id */
    float dead_x;      /* 死点的坐标 */
    float dead_y;
    bool found_dead;
    bool coord_ok;     /* 坐标是否与插入时逐位相同 */
} ScanCtx;

static int scan_cb(const RTreePoint *p, void *ctx)
{
    ScanCtx *c = (ScanCtx *)ctx;

    c->n++;
    if (p->dead) {
        c->n_dead++;
        c->dead_row = p->row_id;
        c->dead_x = p->x;
        c->dead_y = p->y;
        c->found_dead = true;
    } else if (p->row_id >= 1u && p->row_id <= (u32)N_PTS) {
        u32 k = p->row_id - 1u;

        if (p->x != g_x[k] || p->y != g_y[k])
            c->coord_ok = false;
    }
    return OK;
}

/* ---------------- 1~5：标死 + 枚举 + 计数 ---------------- */

/* 被删的点：row_id = 42 ⇒ 下标 41 */
#define VICTIM_ROW 42u

static void t_dead_flow(void)
{
    RTree *tree = NULL;
    Pager *pg = NULL;
    Mbr full;
    u32 ids[N_PTS];
    u32 n = 0;
    u32 i;
    u64 reads = 0;
    ScanCtx sc;
    u32 live = 0;
    u32 total = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 512, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    make_grid();
    for (i = 0; i < N_PTS; i++) {
        CHECK(rtree_insert(tree, i + 1u, g_x[i], g_y[i]) == OK);
    }
    CHECK(rtree_npoints(tree) == N_PTS);

    /* ---- 2. 标死不存在的 row_id → E_NOTFOUND ---- */
    CHECK(rtree_mark_dead(pg, rtree_root_page(tree), 0x12345678u, &reads) == E_NOTFOUND);
    printf("    标死不存在的 row_id：E_NOTFOUND，扫了 %llu 页\n",
           (unsigned long long)reads);

    /* ---- 1. 标死一个真实存在的点 ---- */
    CHECK(rtree_mark_dead(pg, rtree_root_page(tree), VICTIM_ROW, &reads) == OK);
    CHECK(reads > 0);

    /* ---- 3. 重复标死 → 幂等（OK） ---- */
    CHECK(rtree_mark_dead(pg, rtree_root_page(tree), VICTIM_ROW, &reads) == OK);

    /* ---- 1. 窗口查询不再返回死点，其余照常 ---- */
    mbr_set_rect(&full, -1.0e9f, -1.0e9f, 1.0e9f, 1.0e9f);
    CHECK(rtree_window(tree, &full, ids, N_PTS, &n) == OK);
    if (n != (u32)(N_PTS - 1)) {
        printf("    FAIL 窗口命中数不对：期望 %u，实际 %u\n",
               (u32)(N_PTS - 1), n);
        failures++;
    }
    for (i = 0; i < n; i++) {
        if (ids[i] == VICTIM_ROW) {
            printf("    FAIL 窗口查询仍然返回了死点 %u\n", VICTIM_ROW);
            failures++;
        }
        if (ids[i] == 0u || ids[i] > (u32)N_PTS) {
            printf("    FAIL 窗口返回了非法 row_id %u\n", ids[i]);
            failures++;
        }
    }

    /* ---- 4. 全树枚举：点数、死点标记、坐标都要对 ---- */
    memset(&sc, 0, sizeof(sc));
    sc.coord_ok = true;
    CHECK(rtree_scan_all(pg, rtree_root_page(tree), scan_cb, &sc, &reads) == OK);
    if (sc.n != (u32)N_PTS) {
        printf("    FAIL 枚举点数不对：期望 %u，实际 %u\n", (u32)N_PTS, sc.n);
        failures++;
    }
    if (sc.n_dead != 1u) {
        printf("    FAIL 枚举到的死点数不对：期望 1，实际 %u\n", sc.n_dead);
        failures++;
    }
    if (!sc.found_dead || sc.dead_row != VICTIM_ROW) {
        printf("    FAIL 枚举到的死点 row_id 不对：期望 %u，实际 %u\n",
               VICTIM_ROW, sc.found_dead ? sc.dead_row : 0u);
        failures++;
    }
    if (sc.found_dead) {
        if (sc.dead_x != g_x[VICTIM_ROW - 1u] || sc.dead_y != g_y[VICTIM_ROW - 1u]) {
            printf("    FAIL 枚举到的死点坐标不对：期望 (%g,%g)，实际 (%g,%g)\n",
                   (double)g_x[VICTIM_ROW - 1u], (double)g_y[VICTIM_ROW - 1u],
                   (double)sc.dead_x, (double)sc.dead_y);
            failures++;
        }
    }
    if (!sc.coord_ok) {
        printf("    FAIL 枚举到的活点坐标与插入时不一致\n");
        failures++;
    }

    /* ---- 5. 活点计数 ---- */
    CHECK(rtree_count_live(pg, rtree_root_page(tree), &live, &total, &reads) == OK);
    if (live != (u32)(N_PTS - 1) || total != (u32)N_PTS) {
        printf("    FAIL 计数不对：n_live 期望 %u 实际 %u；n_total 期望 %u 实际 %u\n",
               (u32)(N_PTS - 1), live, (u32)N_PTS, total);
        failures++;
    }
    /* 两个指针为 NULL 也不能崩 */
    CHECK(rtree_count_live(pg, rtree_root_page(tree), NULL, NULL, NULL) == OK);

    printf("    %u 点里标死 %u：窗口命中 %u，枚举 %u 条（死 %u），活点 %u / 总 %u\n",
           (u32)N_PTS, VICTIM_ROW, n, sc.n, sc.n_dead, live, total);

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 6：KNN 不返回死点 ---------------- */

static void t_knn_skips_dead(void)
{
    RTree *tree = NULL;
    Pager *pg = NULL;
    u32 ids[8];
    u32 n = 0;
    u32 i;
    u64 reads = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    /*
     * 10 个点排在一条直线上：row_id = i+1，坐标 (i, 0)。
     * 查询点取 (0,0) ⇒ 最近的是 row_id = 1 —— 把它标死。
     */
    for (i = 0; i < 10u; i++) {
        CHECK(rtree_insert(tree, i + 1u, (float)i, 0.0f) == OK);
    }

    CHECK(rtree_mark_dead(pg, rtree_root_page(tree), 1u, &reads) == OK);

    CHECK(rtree_knn(pg, rtree_root_page(tree), 0.0f, 0.0f, 3u,
                    ids, 8, &n, &reads) == OK);
    if (n != 3u) {
        printf("    FAIL KNN 结果个数不对：期望 3，实际 %u\n", n);
        failures++;
    }
    for (i = 0; i < n; i++) {
        if (ids[i] == 1u) {
            printf("    FAIL KNN 返回了死点 1\n");
            failures++;
        }
    }
    /* 死点不在 ⇒ 结果应当是 2、3、4（按距离升序） */
    if (n == 3u && (ids[0] != 2u || ids[1] != 3u || ids[2] != 4u)) {
        printf("    FAIL KNN 结果不对：期望 2,3,4，实际 %u,%u,%u\n",
               ids[0], ids[1], ids[2]);
        failures++;
    }
    printf("    KNN（k=3，最近的那个已被标死）：返回 %u 个：%u %u %u\n",
           n, n > 0 ? ids[0] : 0u, n > 1 ? ids[1] : 0u, n > 2 ? ids[2] : 0u);

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 7：row_id 越界被拒 ---------------- */

static void t_rowid_limit(void)
{
    RTree *tree = NULL;
    Pager *pg = NULL;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(rtree_create(pg, &tree) == OK);

    /* 最高位被"已删除"标志占了 ⇒ 越界 */
    CHECK(rtree_insert(tree, 0x80000000u, 1.0f, 1.0f) == E_INVAL);
    CHECK(rtree_insert(tree, 0xFFFFFFFFu, 1.0f, 1.0f) == E_INVAL);
    /* 上限本身合法 */
    CHECK(rtree_insert(tree, RT_ROWID_MAX, 1.0f, 1.0f) == OK);
    CHECK(rtree_npoints(tree) == 1u);

    printf("    row_id 越界被拒（E_INVAL）；RT_ROWID_MAX 可以插入\n");

    rtree_destroy(tree);
    pager_close(pg);
}

/* ---------------- 8：空树 ---------------- */

static void t_empty_tree(void)
{
    Pager *pg = NULL;
    ScanCtx sc;
    u32 live = 123u;
    u32 total = 456u;
    u64 reads = 0;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    memset(&sc, 0, sizeof(sc));
    CHECK(rtree_scan_all(pg, PAGE_ID_NIL, scan_cb, &sc, &reads) == OK);
    if (sc.n != 0u) {
        printf("    FAIL 空树枚举出了 %u 个点\n", sc.n);
        failures++;
    }
    CHECK(rtree_mark_dead(pg, PAGE_ID_NIL, 1u, &reads) == E_NOTFOUND);
    CHECK(rtree_count_live(pg, PAGE_ID_NIL, &live, &total, &reads) == OK);
    if (live != 0u || total != 0u) {
        printf("    FAIL 空树计数不对：live %u，total %u\n", live, total);
        failures++;
    }

    printf("    空树：枚举 0 条、标死 E_NOTFOUND、计数 0/0\n");

    pager_close(pg);
}

int main(void)
{
    printf("test_geo_dead ...\n");

    t_dead_flow();
    t_knn_skips_dead();
    t_rowid_limit();
    t_empty_tree();

    remove(PATH);

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
