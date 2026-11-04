/*
 * test_geo_api.c —— 地理空间模态的公开 API 验收（v1.4 步骤 5）
 *
 * 8 项：
 *   1. **建之前的行为**：没有几何存储时，窗口 / 计数 / 删除 / 插入一律 DB_E_NOTFOUND；
 *      第二次建 → DB_E_INVAL；NaN 坐标 → DB_E_INVAL。
 *   2. **插入**：行号从 0 连续、单调递增、不复用；活点数 == 插入数。
 *   3. **窗口 vs 暴力基准**：集合全等（含 min>max 自动归一化、边界闭区间）。
 *   4. **KNN vs 暴力基准**：顺序全等（距离升序、并列按 row_id 升序），距离逐位相等。
 *   5. **删除**：窗口 / KNN 立刻排除它、活点数减一、删不存在的 → DB_E_NOTFOUND、
 *      重复删幂等（返回 DB_OK）。
 *   6. **reindex**：结果仍与暴力基准全等，活点数不变。
 *   7. **关库重开**：数据还在（活点数 + 窗口结果 + KNN 结果）。
 *   8. **geo_points 虚拟表**：SPI 接线通了（行数 = 总条目数，含死条目）。
 *
 * 【为什么一定要跟暴力基准对照】
 *
 * 空间查询最容易出的错是**静默漏点**：剪枝条件写反一个符号、少算一条边界，
 * 结果就少几个点，而且**不报错**。所以判据只能是"逐点全等"。
 *
 * 【为什么删除之后还要重算暴力基准】
 *
 * 死条目在树里还占着位置（只是被打了标记），
 * 暴力基准那边必须同步把它去掉 —— 两边的"活点集合"必须一致，
 * 否则对照的是两批不同的点，比出来的差异没有意义。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "modalities/geo/geo_brute.h"
#include "modalities/geo/mbr.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/geo_api.db"
#define N    500
#define CAP  1024

/* 多留一个槽：第 6 项要补插一个点，用来验证"行号不复用" */
#define MAXP (N + 1)

/* 补插之后应有的活点数：N 个原始点 − 10 个删掉的 + 1 个补插的 */
#define LIVE_AFTER_REINDEX (N - 10 + 1)

/* ---------------- 测试数据 ---------------- */

static float px[MAXP];
static float py[MAXP];
static bool  pdead[MAXP];

static GeoPoint ref[N];      /* 暴力基准的点（只含活点，按 row_id 升序） */
static u32     nref;

/* 补插的那个点（第 6 项）有没有真的插进去 */
static bool extra_present = false;

/* 确定性随机（自己写的 LCG，不用 rand()，免得受平台实现影响） */
static unsigned long long rng_state = 88172645463325252ULL;

static float rnd01(void)
{
    rng_state = rng_state * 6364136223846793005ULL + 1442695040888963407ULL;
    return (float)((rng_state >> 40) % 1000000ULL) / 1000000.0f;
}

/* 重建暴力基准的点集：只含活点，按 row_id 升序（暴力基准的输出顺序 = 输入顺序） */
static void build_ref(void)
{
    u32 limit = extra_present ? (u32)MAXP : (u32)N;

    nref = 0;
    for (u32 i = 0; i < limit; i++) {
        if (pdead[i])
            continue;
        ref[nref].row_id = i;
        ref[nref].x = px[i];
        ref[nref].y = py[i];
        nref++;
    }
}

/* ---------------- 对照：窗口 ---------------- */

static void check_window(DB *db, float minx, float miny,
                         float maxx, float maxy, const char *tag)
{
    Mbr win;
    mbr_set_rect(&win, minx, miny, maxx, maxy);

    u32 refids[CAP];
    u32 nref_hit = 0;
    int rc = geo_brute_window(ref, nref, &win, refids, CAP, &nref_hit);
    CHECK(rc == OK);
    if (rc != OK)
        return;

    u32 ids[CAP];
    u32 n = 0;
    db_err e = db_geo_window(db, minx, miny, maxx, maxy, ids, CAP, &n);
    CHECK(e == DB_OK);
    if (e != DB_OK)
        return;

    geo_ids_sort(ids, n);          /* 树序 → row_id 序，两边才好逐位对照 */

    if (n != nref_hit) {
        printf("    FAIL 窗口 %s: 命中数不一致 树=%u 暴力=%u\n", tag, n, nref_hit);
        failures++;
        return;
    }

    for (u32 i = 0; i < n; i++) {
        if (ids[i] != refids[i]) {
            printf("    FAIL 窗口 %s: 第 %u 个不一致 树=%u 暴力=%u\n",
                   tag, i, ids[i], refids[i]);
            failures++;
            return;
        }
    }
}

/* ---------------- 对照：KNN ---------------- */

static void check_knn(DB *db, float qx, float qy, u32 k, const char *tag)
{
    u32 refids[64];
    u32 nref_hit = 0;
    int rc = geo_brute_knn(ref, nref, qx, qy, k, refids, 64, &nref_hit);
    CHECK(rc == OK);
    if (rc != OK)
        return;

    db_geo_hit hits[64];
    u32 n = 0;
    db_err e = db_geo_knn(db, qx, qy, k, hits, 64, &n);
    CHECK(e == DB_OK);
    if (e != DB_OK)
        return;

    if (n != nref_hit) {
        printf("    FAIL KNN %s: 条数不一致 树=%u 暴力=%u\n", tag, n, nref_hit);
        failures++;
        return;
    }

    for (u32 i = 0; i < n; i++) {
        if (hits[i].row_id != refids[i]) {
            printf("    FAIL KNN %s: 第 %u 个不一致 树=%llu 暴力=%u\n",
                   tag, i, (unsigned long long)hits[i].row_id, refids[i]);
            failures++;
            return;
        }
        /*
         * 距离逐位相等：两边用的是同一套算术（geo_dist2）。
         * 差一个末位都算不一致 —— 那种差会被"排序并列"放大成顺序错乱。
         */
        float d = geo_dist2(qx, qy, px[refids[i]], py[refids[i]]);
        if (hits[i].dist != d) {
            printf("    FAIL KNN %s: 第 %u 个距离不一致 树=%f 暴力=%f\n",
                   tag, i, (double)hits[i].dist, (double)d);
            failures++;
            return;
        }
    }
}

/* ---------------- geo_points 虚拟表 ---------------- */

/* 查 geo_points 的行数（失败返回 -1） */
static int count_points_rows(DB *db)
{
    Result *res = NULL;

    if (db_exec(db, "SELECT row_id, x, y, deleted FROM geo_points", &res) != DB_OK) {
        if (res)
            db_result_free(res);
        return -1;
    }
    int rows = db_result_rows(res);
    db_result_free(res);
    return rows;
}

/* ---------------- 1. 建之前的行为 ---------------- */

static void t1_before_create(void)
{
    printf("  1. 没有几何存储时的行为\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_geo_exists(db) == 0);

    uint32_t ids[8];
    uint32_t n = 0;
    CHECK(db_geo_window(db, 0, 0, 10, 10, ids, 8, &n) == DB_E_NOTFOUND);

    uint64_t c = 0;
    CHECK(db_geo_count(db, &c) == DB_E_NOTFOUND);

    uint64_t rid = 0;
    CHECK(db_geo_insert(db, 1.0f, 2.0f, &rid) == DB_E_NOTFOUND);
    CHECK(db_geo_delete(db, 0) == DB_E_NOTFOUND);
    CHECK(db_geo_reindex(db) == DB_E_NOTFOUND);

    /* 建：一次成功，第二次不允许 */
    CHECK(db_geo_create(db) == DB_OK);
    CHECK(db_geo_exists(db) == 1);
    CHECK(db_geo_create(db) == DB_E_INVAL);

    /* NaN / Inf 必须被拒：否则剪枝会静默失效 */
    CHECK(db_geo_insert(db, (float)NAN, 1.0f, NULL) == DB_E_INVAL);
    CHECK(db_geo_insert(db, 1.0f, (float)NAN, NULL) == DB_E_INVAL);
    CHECK(db_geo_insert(db, (float)INFINITY, 1.0f, NULL) == DB_E_INVAL);

    db_close(db);
}

/* ---------------- 2. 插入 ---------------- */

static void t2_insert(DB *db)
{
    printf("  2. 插入：行号单调、不复用\n");

    for (int i = 0; i < N; i++) {
        uint64_t rid = 0;
        CHECK(db_geo_insert(db, px[i], py[i], &rid) == DB_OK);
        if (rid != (uint64_t)i) {
            printf("    FAIL 第 %d 个点的行号不对：%llu（应为 %d）\n",
                   i, (unsigned long long)rid, i);
            failures++;
        }
    }

    uint64_t c = 0;
    CHECK(db_geo_count(db, &c) == DB_OK);
    CHECK(c == (uint64_t)N);
}

/* ---------------- 3. 窗口 vs 暴力 ---------------- */

static void t3_window(DB *db)
{
    printf("  3. 窗口查询 vs 暴力基准\n");

    build_ref();

    check_window(db, 0.0f, 0.0f, 10.0f, 10.0f, "小窗口");
    check_window(db, 20.0f, 30.0f, 60.0f, 70.0f, "中窗口");
    check_window(db, 0.0f, 0.0f, 100.0f, 100.0f, "全覆盖");
    check_window(db, 50.0f, 50.0f, 50.0f, 50.0f, "退化窗口（一个点）");

    /*
     * min > max 要被自动归一化（mbr_set_rect 的约定）：
     * 传反了与传正了必须得到**同一个**结果。
     */
    check_window(db, 60.0f, 60.0f, 40.0f, 40.0f, "传反的窗口（应与 40,40,60,60 相同）");

    /* 装不下要报错，不能静默截断 */
    uint32_t small[2];
    uint32_t nn = 0;
    CHECK(db_geo_window(db, 0.0f, 0.0f, 100.0f, 100.0f, small, 2, &nn) == DB_E_NOSPACE);

    /* 页读计数：查过就该 > 0 */
    CHECK(db_geo_page_reads(db) > 0);
}

/* ---------------- 4. KNN vs 暴力 ---------------- */

static void t4_knn(DB *db)
{
    printf("  4. KNN vs 暴力基准\n");

    check_knn(db, 0.0f, 0.0f, 10, "原点");
    check_knn(db, 50.0f, 50.0f, 10, "中心");
    check_knn(db, 99.0f, 1.0f, 25, "角落");
    check_knn(db, px[7], py[7], 1, "就查某个点自己");

    /* 装不下要报错，不能静默截断 */
    db_geo_hit small[3];
    uint32_t nn = 0;
    CHECK(db_geo_knn(db, 0.0f, 0.0f, 20, small, 3, &nn) == DB_E_NOSPACE);

    /* k == 0 → 空结果，不是错误 */
    CHECK(db_geo_knn(db, 0.0f, 0.0f, 0, small, 3, &nn) == DB_OK);
    CHECK(nn == 0);
}

/* ---------------- 5. 删除 ---------------- */

static void t5_delete(DB *db)
{
    printf("  5. 删除\n");

    /* 删 10 个点（隔一个删一个，覆盖各处） */
    for (int i = 0; i < 10; i++) {
        u32 id = (u32)(i * 37);
        pdead[id] = true;
        CHECK(db_geo_delete(db, (uint64_t)id) == DB_OK);
    }

    uint64_t c = 0;
    CHECK(db_geo_count(db, &c) == DB_OK);
    CHECK(c == (uint64_t)(N - 10));

    build_ref();

    /* 被删的点不能再出现在窗口 / KNN 里 */
    check_window(db, 0.0f, 0.0f, 100.0f, 100.0f, "删除后的全覆盖窗口");
    check_knn(db, 0.0f, 0.0f, 20, "删除后的 KNN");

    /* 重复删 → 幂等（OK） */
    CHECK(db_geo_delete(db, 0) == DB_OK);

    /* 删一个不存在的 → DB_E_NOTFOUND */
    CHECK(db_geo_delete(db, (uint64_t)N + 100) == DB_E_NOTFOUND);

    /*
     * geo_points 的行数 = **总条目数**（含死条目）。
     *
     * 删除语义是"打标记"，状态由**行内容**表达，不由可见性表达
     * （与 ft_docs 的 sealed 列同一个口径）——
     * 所以被删的点仍然在表里，只是 deleted = 1。
     */
    CHECK(count_points_rows(db) == N);
}

/* ---------------- 6. reindex ---------------- */

static void t6_reindex(DB *db)
{
    printf("  6. reindex\n");

    CHECK(db_geo_reindex(db) == DB_OK);

    uint64_t c = 0;
    CHECK(db_geo_count(db, &c) == DB_OK);
    CHECK(c == (uint64_t)(N - 10));      /* 活点数不变 */

    /* 结果仍与暴力基准全等 */
    build_ref();
    check_window(db, 0.0f, 0.0f, 100.0f, 100.0f, "reindex 后的窗口");
    check_knn(db, 50.0f, 50.0f, 15, "reindex 后的 KNN");

    /*
     * 行号不复用：reindex 之后新插入的点，
     * 行号必须落在"用过的最大行号"之后，不能顶替一个用过的号。
     */
    px[N] = 1.0f;
    py[N] = 1.0f;
    pdead[N] = false;

    uint64_t rid = 0;
    CHECK(db_geo_insert(db, px[N], py[N], &rid) == DB_OK);
    if (rid != (uint64_t)N) {
        printf("    FAIL reindex 后行号被复用了：%llu（应为 %d）\n",
               (unsigned long long)rid, N);
        failures++;
    }
    extra_present = true;
}

/* ---------------- 7. 关库重开 ---------------- */

static void t7_reopen(void)
{
    printf("  7. 关库重开：数据还在\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_geo_exists(db) == 1);

    uint64_t c = 0;
    CHECK(db_geo_count(db, &c) == DB_OK);
    CHECK(c == (uint64_t)LIVE_AFTER_REINDEX);

    build_ref();
    check_window(db, 0.0f, 0.0f, 100.0f, 100.0f, "重开后的窗口");
    check_knn(db, 10.0f, 90.0f, 12, "重开后的 KNN");

    db_close(db);
}

/* ---------------- 8. geo_points 虚拟表 ---------------- */

static void t8_points_table(void)
{
    printf("  8. geo_points 虚拟表（SPI 接线）\n");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    Result *res = NULL;
    db_err e = db_exec(db, "SELECT row_id, x, y, deleted FROM geo_points", &res);
    CHECK(e == DB_OK);
    if (e == DB_OK && res) {
        /*
         * 行数 = 树里的条目数。
         *
         * reindex 之后死条目被**物理清掉**了，所以这时的行数
         * 等于活点数（reindex 之前是"含死条目的总数"，见第 5 项）。
         */
        int rows = db_result_rows(res);
        if (rows != LIVE_AFTER_REINDEX) {
            printf("    FAIL geo_points 行数不对：%d（应为 %d）\n",
                   rows, LIVE_AFTER_REINDEX);
            failures++;
        }
        db_result_free(res);
    } else if (res) {
        db_result_free(res);
    }

    db_close(db);
}

int main(void)
{
    printf("test_geo_api:\n");

    remove(PATH);
    remove(PATH "-wal");
    remove(PATH "-clog");

    /* 造数据（确定性） */
    rng_state = 88172645463325252ULL;
    for (int i = 0; i < N; i++) {
        px[i] = rnd01() * 100.0f;
        py[i] = rnd01() * 100.0f;
        pdead[i] = false;
    }

    t1_before_create();

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_geo_exists(db) == 1);      /* 第 1 项建过了 */

    t2_insert(db);
    t3_window(db);
    t4_knn(db);
    t5_delete(db);
    t6_reindex(db);

    db_close(db);

    t7_reopen();
    t8_points_table();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
