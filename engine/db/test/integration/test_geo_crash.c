/*
 * test_geo_crash.c —— 地理空间模态崩溃恢复验收（v1.4 步骤 6 · A7）
 *
 * "断电"用 db_abandon 模拟：不刷脏页、不写 CHECKPOINT，直接丢句柄。
 * 等价于进程被 kill -9 —— 日志已 fsync，页还在内存里没落盘。
 *
 * 4 项：
 *   1. 灌点 → 断电 → 重开：点数对、窗口与暴力基准**逐位全等**、KNN 顺序全等
 *   2. 删点 → 断电 → 重开：被删的点**没有复活**，其余点与暴力基准全等
 *   3. reindex → 断电 → 重开：meta 页指向新根，点数与结果都对
 *   4. 断电且**日志也没了** → 重开：点**没了**（坑 M5-16：没有第二份行存）
 *
 * ==================== 为什么第 1 项要"逐位全等" ====================
 *
 * 点数对**不代表**数据对。
 *
 * 空间查询最容易出的错是**静默漏点**：树页只恢复了一半、某个 MBR
 * 没更新对，窗口查询就会少几个点 —— 而且**不报错**，
 * 只是答案悄悄不对。"点数差不多"这种验收抓不住这类 bug。
 *
 * 所以这里把暴力基准（geo_brute.c，与树**同一套几何算术**）的答案
 * 算出来，两边各自按 row_id 排序后**逐位用 == 比**。
 *
 * ==================== 第 2 项是本步骤的核心命题 ====================
 *
 * 删除 = 给叶子条目的 id 打最高位（死条目）。
 *
 * 死标记改的是**叶子页** —— 如果这个写点不进日志，
 * 崩溃重放后这一页会回到"没标记"的样子 ⇒ **被删的点诈尸**。
 *
 * 这就是"写日志的覆盖面"要盯的地方：不是"树页在不在日志里"，
 * 而是"**每一个改页的写点**在不在日志里"。
 *
 * ==================== 第 4 项为什么要测"点没了" ====================
 *
 * 坑 M5-16：地理模态的点**只存在树里**（没有第二份行存）。
 * 所以 WAL 是**唯一**的恢复路径 —— 日志没了，这批点就是没了。
 *
 * 这一项把这个事实**钉成断言**：哪天有人给 geo 加了行存兜底，
 * 这一项会失败（那时该改的是文档，不是代码）。
 * 它同时提醒读者：第 1~3 项的"没丢"是**日志的功劳**，不是别的原因。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

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

#define PATH  "test/tmp/geo_crash.db"
#define PATH2 "test/tmp/geo_crash2.db"
#define PATH3 "test/tmp/geo_crash3.db"

/*
 * 300 个点：叶子一页装 203 条 ⇒ 必然触发一次**叶子分裂**，
 * 而单叶子的树分裂后要**新建根**（根页号变、meta 页要改）。
 * 这条路径不覆盖的话，根页号/meta 页的写日志就是没测过。
 */
#define N_PTS   300
#define DEL_ROW 17          /* 第 2 项里被删掉的点 */
#define DEL_ROW2 233        /* 第 3 项里被删掉的另一个点 */
#define K      10           /* KNN 取几个 */

/* 窗口与 KNN 的查询点（固定值 ⇒ 结果可复现） */
#define WIN_MINX 10.0f
#define WIN_MINY 20.0f
#define WIN_MAXX 60.0f
#define WIN_MAXY 80.0f
#define QX       33.0f
#define QY       47.0f

/* ---------------- 点集（确定性生成） ---------------- */

static GeoPoint pts[N_PTS];
static bool     dead[N_PTS];

/*
 * 用固定种子的 LCG 生成点。
 *
 * 为什么要自己生成而不是随机数：**结果必须可复现** ——
 * 断电前后两次查询要比对，点的集合必须完全一样。
 */
static void gen_points(void)
{
    u32 s = 20241103u;

    for (int i = 0; i < N_PTS; i++) {
        s = s * 1103515245u + 12345u;
        pts[i].x = (float)((s >> 16) & 0x7FFFu) / 32768.0f * 100.0f;
        s = s * 1103515245u + 12345u;
        pts[i].y = (float)((s >> 16) & 0x7FFFu) / 32768.0f * 100.0f;
        pts[i].row_id = (u32)i;
        dead[i] = false;
    }
}

/* 把活点摊平成数组（row_id 升序 —— 暴力基准的输出约定） */
static u32 build_live(GeoPoint *out)
{
    u32 n = 0;

    for (int i = 0; i < N_PTS; i++) {
        if (dead[i])
            continue;
        out[n++] = pts[i];
    }
    return n;
}

/* ---------------- 结果比对 ---------------- */

/*
 * 窗口查询：树的结果 vs 暴力基准，逐位全等。
 *
 * 两边都要先按 row_id 排序 —— 树的遍历顺序是"树序"，
 * 集合全等才是判据，顺序只是对照前的整理手段。
 */
static void check_window(DB *db, const char *tag)
{
    Mbr win;
    GeoPoint live[N_PTS];
    u32 brute[N_PTS];
    u32 got[N_PTS];
    u32 n_live, n_brute = 0, n_got = 0;

    n_live = build_live(live);
    mbr_set_rect(&win, WIN_MINX, WIN_MINY, WIN_MAXX, WIN_MAXY);

    CHECK(geo_brute_window(live, n_live, &win, brute, N_PTS, &n_brute) == OK);
    CHECK(db_geo_window(db, WIN_MINX, WIN_MINY, WIN_MAXX, WIN_MAXY,
                        got, N_PTS, &n_got) == DB_OK);

    geo_ids_sort(brute, n_brute);
    geo_ids_sort(got, n_got);

    if (n_brute != n_got) {
        printf("    FAIL %s：窗口命中数不一样（暴力 %u，树 %u）\n",
               tag, n_brute, n_got);
        failures++;
        return;
    }
    for (u32 i = 0; i < n_brute; i++) {
        if (brute[i] != got[i]) {
            printf("    FAIL %s：窗口第 %u 个不一样（暴力 %u，树 %u）\n",
                   tag, i, brute[i], got[i]);
            failures++;
            return;
        }
    }
    printf("    %s：窗口 %u 个命中，与暴力基准逐位全等\n", tag, n_got);
}

/*
 * KNN：顺序与距离都逐位全等。
 *
 * 距离用 == 比（不用容差）：db 层算距离走的是 geo_dist2 ——
 * 与暴力基准**同一套算术**（mbr.c），同样的 float 输入就该有同样的
 * float 输出。容差会把"算术走偏了"这种真问题也放过。
 */
static void check_knn(DB *db, const char *tag)
{
    GeoPoint live[N_PTS];
    u32 brute[K];
    db_geo_hit got[K];
    u32 n_live, n_brute = 0, n_got = 0;

    n_live = build_live(live);

    CHECK(geo_brute_knn(live, n_live, QX, QY, K, brute, K, &n_brute) == OK);
    CHECK(db_geo_knn(db, QX, QY, K, got, K, &n_got) == DB_OK);

    if (n_brute != n_got) {
        printf("    FAIL %s：KNN 个数不一样（暴力 %u，树 %u）\n",
               tag, n_brute, n_got);
        failures++;
        return;
    }
    for (u32 i = 0; i < n_brute; i++) {
        if (brute[i] != got[i].row_id) {
            printf("    FAIL %s：KNN 第 %u 名不一样（暴力 %u，树 %llu）\n",
                   tag, i, brute[i], (unsigned long long)got[i].row_id);
            failures++;
            return;
        }
        {
            const GeoPoint *p = &pts[brute[i]];
            float d = geo_dist2(QX, QY, p->x, p->y);

            if (got[i].dist != d) {
                printf("    FAIL %s：KNN 第 %u 名的距离不一样（暴力 %f，树 %f）\n",
                       tag, i, d, got[i].dist);
                failures++;
                return;
            }
        }
    }
    printf("    %s：KNN 前 %u 名与暴力基准逐位全等\n", tag, n_got);
}

/* 灌点：返回是否成功 */
static bool fill_points(DB *db, u32 n)
{
    for (u32 i = 0; i < n; i++) {
        uint64_t rid = 0;

        if (db_geo_insert(db, pts[i].x, pts[i].y, &rid) != DB_OK) {
            printf("    FAIL 灌点 %u 失败\n", i);
            failures++;
            return false;
        }
        if (rid != (uint64_t)i) {
            printf("    FAIL 第 %u 个点的行号是 %llu（应为 %u）\n",
                   i, (unsigned long long)rid, i);
            failures++;
            return false;
        }
    }
    return true;
}

/* ---------------- 1) 灌点 → 断电 → 重开 ---------------- */

static void t1_crash_insert(void)
{
    uint64_t n = 0;

    printf("  1) 灌 %u 个点 → 断电 → 重开：点数 / 窗口 / KNN 都要对\n", N_PTS);

    remove(PATH);
    remove("test/tmp/geo_crash.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_create(db) == DB_OK);
    if (!fill_points(db, N_PTS)) {
        db_abandon(db);
        return;
    }
    CHECK(db_geo_count(db, &n) == DB_OK);
    CHECK(n == N_PTS);

    /* 断电：不刷脏页、不写 CHECKPOINT（日志已经 fsync 过） */
    db_abandon(db);
    db = NULL;

    /* 重开 → 恢复重放 → 树页与 meta 页都被补回来 */
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_exists(db) == 1);
    CHECK(db_geo_count(db, &n) == DB_OK);
    if (n != N_PTS) {
        printf("    FAIL 断电后点数变了：断电前 %u，断电后 %llu\n",
               N_PTS, (unsigned long long)n);
        failures++;
    }

    check_window(db, "断电后");
    check_knn(db, "断电后");

    /* 再断一次电、再重开：重放是幂等的，结果不能变 */
    db_abandon(db);
    db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;
    CHECK(db_geo_count(db, &n) == DB_OK);
    CHECK(n == N_PTS);
    check_window(db, "断电两次后");
    check_knn(db, "断电两次后");

    db_close(db);
    remove(PATH);
    remove("test/tmp/geo_crash.db-wal");
}

/* ---------------- 2) 删点 → 断电 → 重开 ---------------- */

static void t2_crash_delete(void)
{
    uint64_t n = 0;
    uint32_t ids[N_PTS];
    uint32_t n_ids = 0;

    printf("  2) 删一个点 → 断电 → 重开：被删的点不能诈尸\n");

    remove(PATH2);
    remove("test/tmp/geo_crash2.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_create(db) == DB_OK);
    if (!fill_points(db, N_PTS)) {
        db_abandon(db);
        return;
    }

    CHECK(db_geo_delete(db, DEL_ROW) == DB_OK);
    dead[DEL_ROW] = true;
    CHECK(db_geo_count(db, &n) == DB_OK);
    CHECK(n == N_PTS - 1);

    db_abandon(db);
    db = NULL;

    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_count(db, &n) == DB_OK);
    if (n != N_PTS - 1) {
        printf("    FAIL 断电后点数变了：断电前 %u，断电后 %llu\n",
               N_PTS - 1, (unsigned long long)n);
        failures++;
    }

    /* 被删的点不能出现在窗口结果里（诈尸 = 死标记没进日志） */
    CHECK(db_geo_window(db, 0.0f, 0.0f, 100.0f, 100.0f, ids, N_PTS, &n_ids) == DB_OK);
    for (uint32_t i = 0; i < n_ids; i++) {
        if (ids[i] == DEL_ROW) {
            printf("    FAIL 被删的点 %u 复活了\n", DEL_ROW);
            failures++;
            break;
        }
    }

    check_window(db, "删点后断电");
    check_knn(db, "删点后断电");

    db_close(db);
    remove(PATH2);
    remove("test/tmp/geo_crash2.db-wal");
}

/* ---------------- 3) reindex → 断电 → 重开 ---------------- */

static void t3_crash_reindex(void)
{
    uint64_t n = 0;

    printf("  3) reindex → 断电 → 重开：meta 页要指向新根\n");

    remove(PATH2);
    remove("test/tmp/geo_crash2.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_create(db) == DB_OK);
    if (!fill_points(db, N_PTS)) {
        db_abandon(db);
        return;
    }

    CHECK(db_geo_delete(db, DEL_ROW) == DB_OK);
    dead[DEL_ROW] = true;
    CHECK(db_geo_delete(db, DEL_ROW2) == DB_OK);
    dead[DEL_ROW2] = true;

    /* 重建索引：换一棵新树，根页号变 ⇒ meta 页要改 */
    CHECK(db_geo_reindex(db) == DB_OK);
    CHECK(db_geo_count(db, &n) == DB_OK);
    CHECK(n == N_PTS - 2);

    db_abandon(db);
    db = NULL;

    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_count(db, &n) == DB_OK);
    if (n != N_PTS - 2) {
        printf("    FAIL reindex 后断电，点数变了：断电前 %u，断电后 %llu\n",
               N_PTS - 2, (unsigned long long)n);
        failures++;
    }

    check_window(db, "reindex 后断电");
    check_knn(db, "reindex 后断电");

    db_close(db);
    remove(PATH2);
    remove("test/tmp/geo_crash2.db-wal");
}

/* ---------------- 4) 断电且日志也没了 → 点没了（坑 M5-16） ---------------- */

/*
 * 这一项断言的是"**没有兜底**"这个事实。
 *
 * geo 的点只存在树里（没有第二份行存），所以：
 *   - 树页没落盘（断电） + 日志没了 ⇒ 这批点就是没了。
 *
 * 之前三项的"没丢"是**日志的功劳**，不是别的原因 ——
 * 这一项把这件事钉成断言，免得以后有人误以为"还有别的副本"。
 */
static void t4_no_wal_no_data(void)
{
    printf("  4) 断电且日志也没了 → 重开：点没了（坑 M5-16：没有第二份行存）\n");

    remove(PATH3);
    remove("test/tmp/geo_crash3.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH3, &db) == DB_OK);
    if (!db)
        return;

    CHECK(db_geo_create(db) == DB_OK);
    if (!fill_points(db, N_PTS)) {
        db_abandon(db);
        return;
    }

    /* 断电，然后**把日志也删掉**（模拟"日志没落盘"的极端情况） */
    db_abandon(db);
    db = NULL;
    remove("test/tmp/geo_crash3.db-wal");

    CHECK(db_open(PATH3, &db) == DB_OK);
    if (!db)
        return;

    /*
     * 树页从来没落盘 ⇒ 磁盘上根本没有几何存储 ⇒ 点没了。
     *
     * 这里断言的是"几何存储不存在"（db_geo_exists == 0）。
     * 如果哪天 geo 有了行存兜底，这里会失败 —— 那时要改的是文档。
     */
    if (db_geo_exists(db) != 0) {
        printf("    FAIL 日志没了之后几何存储还在（说明有第二份行存？）\n");
        failures++;
    } else {
        printf("    日志没了 ⇒ 几何存储不存在 ⇒ 这批点没了（符合坑 M5-16）\n");
    }

    db_close(db);
    remove(PATH3);
    remove("test/tmp/geo_crash3.db-wal");
}

int main(void)
{
    printf("test_geo_crash:\n");

    gen_points();

    t1_crash_insert();
    t2_crash_delete();
    t3_crash_reindex();
    t4_no_wal_no_data();

    remove(PATH);
    remove("test/tmp/geo_crash.db-wal");
    remove(PATH2);
    remove("test/tmp/geo_crash2.db-wal");
    remove(PATH3);
    remove("test/tmp/geo_crash3.db-wal");

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  4 项验收全部通过\n");
    return 0;
}
