/*
 * test_ts_prune.c —— 时间谓词下推 → 分区裁剪的验收（v1.5 步骤 4 · T7）
 *
 * 六组验收：
 *   1. **相交判据的边界**：窗口 [start,end) 与范围 [lo,hi] 的各种端点组合。
 *   2. **不可解析 ⇒ 全部分区**：没有谓词 / 范围反了 ⇒ 一个不少地全给。
 *   3. **超集裁剪**：范围命中一部分分区 ⇒ 返回的集合是**超集**（含边界）。
 *   4. **store 级裁剪**：真实分区目录上的裁剪。
 *   5. **裁剪后读到的数据不缺**：范围里的行一条不少地读回来。
 *   6. **无谓词 ⇒ 与全扫全等**：一条不多、一条不少、顺序一致。
 *
 * 【为什么"缺一条"是最严重的错】（计划坑 M6-5）
 *
 * 裁剪多给：只是慢一点。
 * 裁剪少给：**静默漏数据** —— 查询按分区裁剪，漏掉的分区里的数据
 * 就永远查不到，而且没有任何报错。
 *
 * 所以本测试最看重的不是"裁得准"，而是"**不缺**"。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_prune.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

#define PATH "test/tmp/g8_ts_prune.db"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 小工具 ---------------- */

/* 造一个窗口 */
static TsPruneWindow win(i64 s, i64 e)
{
    TsPruneWindow w;

    w.start_ms = s;
    w.end_ms = e;
    w.state = (u32)TS_PART_ACTIVE;
    return w;
}

/* 读数收集器 */
typedef struct {
    TsReading rows[256];
    u32       n;
} Collector;

static int collect(const TsReading *r, void *ctx)
{
    Collector *c = (Collector *)ctx;

    if (c->n < 256u)
        c->rows[c->n] = *r;
    c->n++;
    return OK;
}

static int has_row(const Collector *c, i64 t)
{
    u32 i;

    for (i = 0; i < c->n && i < 256u; i++) {
        if (c->rows[i].time == t)
            return 1;
    }
    return 0;
}

/* 按时间排序（裁剪扫描按时间序遍历，全扫按创建序 ⇒ 顺序可能不同） */
static void sort_rows(Collector *c)
{
    u32 i;
    u32 j;

    for (i = 1; i < c->n && i < 256u; i++) {
        TsReading key = c->rows[i];

        j = i;
        while (j > 0 && c->rows[j - 1].time > key.time) {
            c->rows[j] = c->rows[j - 1];
            j--;
        }
        c->rows[j] = key;
    }
}

/* ---------------- 1. 相交判据的边界 ---------------- */

static void t_window_hits(void)
{
    printf("  1. 相交判据的边界\n");

    /* 完全重合 */
    CHECK(ts_prune_window_hits(0, 100, 0, 100) == true);
    /* 范围在窗口内部 */
    CHECK(ts_prune_window_hits(0, 100, 50, 60) == true);
    /* 范围在窗口之前 */
    CHECK(ts_prune_window_hits(100, 200, 0, 99) == false);
    /* 范围在窗口之后 */
    CHECK(ts_prune_window_hits(0, 100, 101, 200) == false);

    /*
     * t_lo 恰好等于窗口终点 ⇒ **不**命中
     * （窗口左闭右开，t_lo 不在这个窗口里，它属于下一个窗口）
     */
    CHECK(ts_prune_window_hits(0, 100, 100, 200) == false);
    /*
     * t_hi 恰好等于窗口起点 ⇒ **命中**
     * （范围两端都含，t_hi 这个点就在这个窗口里 ⇒ 必须读，宁可多给）
     */
    CHECK(ts_prune_window_hits(100, 200, 0, 100) == true);

    /* 单点范围：正好落在窗口里 */
    CHECK(ts_prune_window_hits(0, 100, 50, 50) == true);
    /* 单点范围：落在窗口外 */
    CHECK(ts_prune_window_hits(0, 100, 100, 100) == false);

    /* 坏窗口（倒挂）不命中，但也不能崩 */
    CHECK(ts_prune_window_hits(100, 100, 0, 200) == false);
    CHECK(ts_prune_window_hits(200, 100, 0, 200) == false);
}

/* ---------------- 2. 不可解析 ⇒ 全部分区 ---------------- */

static void t_unparseable(void)
{
    TsPruneWindow wins[3];
    TsPruneSet set;
    int rc;

    printf("  2. 不可解析 ⇒ 全部分区\n");

    wins[0] = win(0, 100);
    wins[1] = win(100, 200);
    wins[2] = win(200, 300);

    /* 没有谓词 ⇒ 全给 */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_select(&set, wins, 3, 0, 100, false);
    CHECK(rc == OK);
    CHECK(ts_prune_all(&set) == true);
    CHECK(ts_prune_count(&set) == 3u);
    ts_prune_set_destroy(&set);

    /* 范围反了 ⇒ 全给（不能猜） */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_select(&set, wins, 3, 500, 100, true);
    CHECK(rc == OK);
    CHECK(ts_prune_all(&set) == true);
    CHECK(ts_prune_count(&set) == 3u);
    /* 全给 ⇒ 任何时间点都在集合里 */
    CHECK(ts_prune_contains_time(&set, 0) == true);
    CHECK(ts_prune_contains_time(&set, 999999) == true);
    ts_prune_set_destroy(&set);

    /* 空目录也不该出错 */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_select(&set, NULL, 0, 0, 100, false);
    CHECK(rc == OK);
    CHECK(ts_prune_count(&set) == 0u);
    ts_prune_set_destroy(&set);
}

/* ---------------- 3. 超集裁剪 ---------------- */

static void t_select(void)
{
    TsPruneWindow wins[3];
    TsPruneSet set;

    printf("  3. 超集裁剪\n");

    wins[0] = win(0, 100);
    wins[1] = win(100, 200);
    wins[2] = win(200, 300);

    /* 范围 [100,199]：只命中中间那个 */
    CHECK(ts_prune_set_init(&set) == OK);
    CHECK(ts_prune_select(&set, wins, 3, 100, 199, true) == OK);
    CHECK(ts_prune_all(&set) == false);
    CHECK(ts_prune_count(&set) == 1u);
    CHECK(ts_prune_contains_time(&set, 150) == true);
    CHECK(ts_prune_contains_time(&set, 50) == false);
    CHECK(ts_prune_contains_time(&set, 250) == false);
    ts_prune_set_destroy(&set);

    /*
     * 范围 [100,200]：t_hi 恰好是第三个窗口的起点 ⇒ **也**命中
     *（超集：宁可多给，不可少给）
     */
    CHECK(ts_prune_set_init(&set) == OK);
    CHECK(ts_prune_select(&set, wins, 3, 100, 200, true) == OK);
    CHECK(ts_prune_count(&set) == 2u);
    CHECK(ts_prune_contains_time(&set, 250) == true);
    ts_prune_set_destroy(&set);

    /* 范围 [0,299]：三个全中 */
    CHECK(ts_prune_set_init(&set) == OK);
    CHECK(ts_prune_select(&set, wins, 3, 0, 299, true) == OK);
    CHECK(ts_prune_count(&set) == 3u);
    ts_prune_set_destroy(&set);

    /* 范围落在所有窗口之外 ⇒ 一个都不命中（空集合，不是全给） */
    CHECK(ts_prune_set_init(&set) == OK);
    CHECK(ts_prune_select(&set, wins, 3, 1000, 2000, true) == OK);
    CHECK(ts_prune_count(&set) == 0u);
    ts_prune_set_destroy(&set);

    /* 窗口下标对应关系：allows_window 按窗口本身判 */
    CHECK(ts_prune_set_init(&set) == OK);
    CHECK(ts_prune_select(&set, wins, 3, 100, 199, true) == OK);
    CHECK(ts_prune_allows_window(&set, 100, 200) == true);
    CHECK(ts_prune_allows_window(&set, 0, 100) == false);
    ts_prune_set_destroy(&set);
}

/* ---------------- 4~6. store 级 ---------------- */

/*
 * 造一个库：三个时间窗，每窗两行。
 *
 * 窗口（span = 1 小时 = 3600000 ms）：
 *   W0 = [0, 3600000)        行 t = 0, 1000
 *   W1 = [3600000, 7200000)  行 t = 3600000, 3601000
 *   W2 = [7200000, 10800000) 行 t = 7200000, 7201000
 */
static void build_store(Pager *pg, TsStore **st_out)
{
    TsStore *st = NULL;
    TsReading r;
    u32 i;

    CHECK(ts_store_create(pg, 3600u, &st) == OK);
    if (st == NULL)
        return;

    for (i = 0; i < 3u; i++) {
        r.time = (i64)i * 3600000LL;
        r.tag = (i64)i;
        r.value = (double)i;
        CHECK(ts_store_insert(st, &r) == OK);

        r.time = (i64)i * 3600000LL + 1000;
        r.tag = (i64)i;
        r.value = (double)i + 0.5;
        CHECK(ts_store_insert(st, &r) == OK);
    }

    *st_out = st;
}

static void t_store_prune(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsPruneSet set;
    TsPruneWindow *wins = NULL;
    u32 n = 0;
    int rc;

    printf("  4. store 级裁剪\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    build_store(pg, &st);
    if (st == NULL) {
        (void)pager_close(pg);
        return;
    }

    /* 取窗口：三个分区 */
    rc = ts_prune_windows_of_store(st, &wins, &n);
    CHECK(rc == OK);
    CHECK(n == 3u);
    if (wins != NULL && n == 3u) {
        CHECK(wins[0].start_ms == 0);
        CHECK(wins[0].end_ms == 3600000);
        CHECK(wins[1].start_ms == 3600000);
        CHECK(wins[2].end_ms == 10800000);
    }
    free(wins);
    wins = NULL;

    /* 范围落在 W1 里 ⇒ 只命中 W1 */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_windows_of_store(st, &wins, &n);
    CHECK(rc == OK);
    CHECK(ts_prune_select(&set, wins, n, 3600000, 3601000, true) == OK);
    CHECK(ts_prune_count(&set) == 1u);
    CHECK(ts_prune_contains_time(&set, 3600500) == true);
    CHECK(ts_prune_contains_time(&set, 100) == false);
    ts_prune_set_destroy(&set);
    free(wins);
    wins = NULL;

    /* 范围 [0, 3600000]：命中 W0（t_hi == W1 起点 ⇒ W1 也命中 ⇒ 超集 2 个） */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_windows_of_store(st, &wins, &n);
    CHECK(rc == OK);
    CHECK(ts_prune_select(&set, wins, n, 0, 3600000, true) == OK);
    CHECK(ts_prune_count(&set) == 2u);
    ts_prune_set_destroy(&set);
    free(wins);
    wins = NULL;

    /* 没有谓词 ⇒ 全部分区 */
    CHECK(ts_prune_set_init(&set) == OK);
    rc = ts_prune_windows_of_store(st, &wins, &n);
    CHECK(rc == OK);
    CHECK(ts_prune_select(&set, wins, n, 0, 0, false) == OK);
    CHECK(ts_prune_count(&set) == 3u);
    CHECK(ts_prune_all(&set) == true);
    ts_prune_set_destroy(&set);
    free(wins);

    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

static void t_scan_equivalence(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    Collector all;
    Collector cut;
    Collector nocut;
    u64 reads_cut = 0;
    u64 reads_all = 0;
    u32 i;

    printf("  5. 裁剪后读到的数据不缺\n");
    printf("  6. 无谓词 ⇒ 与全扫全等\n");

    memset(&all, 0, sizeof(all));
    memset(&cut, 0, sizeof(cut));
    memset(&nocut, 0, sizeof(nocut));

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    build_store(pg, &st);
    if (st == NULL) {
        (void)pager_close(pg);
        return;
    }

    /* 全扫 */
    CHECK(ts_store_scan(st, collect, &all) == OK);
    CHECK(all.n == 6u);
    sort_rows(&all);

    /*
     * 裁剪扫描：范围 = W1 的两个行时间点。
     *
     * 关键断言：**范围里的行一条不少**。
     */
    CHECK(ts_prune_scan(pg, st, 3600000, 3601000, true, collect, &cut,
                        &reads_cut) == OK);
    CHECK(cut.n == 2u);
    CHECK(has_row(&cut, 3600000));
    CHECK(has_row(&cut, 3601000));
    /* 范围外的行不该出现 */
    CHECK(!has_row(&cut, 0));
    CHECK(!has_row(&cut, 1000));

    /* 范围里的每一行，全扫里有的，裁剪扫描里也必须有 */
    for (i = 0; i < all.n && i < 256u; i++) {
        if (all.rows[i].time >= 3600000 && all.rows[i].time <= 3601000) {
            CHECK(has_row(&cut, all.rows[i].time));
        }
    }

    /* 没有谓词 ⇒ 与全扫**完全一致**（条数与每一行） */
    CHECK(ts_prune_scan(pg, st, 0, 0, false, collect, &nocut, &reads_all) == OK);
    CHECK(nocut.n == all.n);
    sort_rows(&nocut);
    for (i = 0; i < all.n && i < 256u; i++) {
        CHECK(nocut.rows[i].time == all.rows[i].time);
        CHECK(nocut.rows[i].tag == all.rows[i].tag);
    }
    /* 没裁剪 ⇒ 读的页数与全扫一样多（3 个分区各 1 页） */
    CHECK(reads_all == 3u);

    /* 裁剪 ⇒ **少读了页**（只读命中分区的那 1 页） */
    CHECK(reads_cut == 1u);
    CHECK(reads_cut < reads_all);

    /* 不可解析的范围（反了）⇒ 与全扫一致 */
    memset(&nocut, 0, sizeof(nocut));
    CHECK(ts_prune_scan(pg, st, 999, 1, true, collect, &nocut, NULL) == OK);
    CHECK(nocut.n == all.n);

    /* 非法参数 */
    CHECK(ts_prune_scan(pg, st, 0, 10, true, NULL, NULL, NULL) == E_INVAL);
    CHECK(ts_prune_scan(NULL, st, 0, 10, true, collect, &nocut, NULL) == E_INVAL);

    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

int main(void)
{
    printf("test_ts_prune —— 分区裁剪\n");

    t_window_hits();
    t_unparseable();
    t_select();
    t_store_prune();
    t_scan_equivalence();

    if (failures == 0) {
        printf("  6 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
