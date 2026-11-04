/*
 * test_ts_store.c —— 时序模态分区存储的单测（v1.5 步骤 2 · T4）
 *
 * 契约见 src/modalities/ts/tsstore.h。
 *
 * ==================== 这一步要证明什么 ====================
 *
 * 1. **路由算得对**：窗口左闭右开、负时间戳向下取整、边界不差一毫秒。
 *    路由错了 ⇒ 数据落错分区 ⇒ 按分区裁剪时**静默漏数据**（计划风险 R3）。
 * 2. **写得进读得出**：跨分区写入后，目录里的分区数 / 行数对得上，
 *    读回来的每一条与写进去的**逐个相等**（浮点比位模式）。
 * 3. **坏情况不装作没事**：找不到分区就明确报 E_NOTFOUND，
 *    不返回"最近的那个分区"糊弄过去。
 * 4. **重开还在**：关库再开，目录重建、行还在。
 *
 * ==================== 为什么浮点要比位模式 ====================
 *
 * 浮点的 `==` 看起来"能用"，但它有两个坑：
 *   - NaN != NaN（写进去 NaN 读出来一定"不等"）；
 *   - 编译器可能把 `a == b` 优化成"肯定相等/肯定不等"的假设。
 * 磁盘上存的是**位模式**（8 字节原样落盘），所以这里也比位模式：
 * 写什么位，读回来就是什么位，一字节不差。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ts_store_test.db"

/* 1 天 / 1 周 的毫秒数 */
#define MS_SEC 1000LL
#define MS_DAY (86400LL * MS_SEC)
#define MS_WEEK (7LL * MS_DAY)

/* 浮点位模式 */
static u64 dbits(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
}

/* t 是否落在 [start, end) 里（左闭右开） */
static bool in_win(i64 t, i64 start, i64 end)
{
    return t >= start && t < end;
}

/* ---------------- 扫描回调 ---------------- */

typedef struct {
    const TsReading *expect;
    u32 n;
    u32 i;
    int mismatch;
} ScanCtx;

static int scan_cb(const TsReading *r, void *ctx)
{
    ScanCtx *s = (ScanCtx *)ctx;

    if (s->i >= s->n) {
        s->mismatch++;             /* 读出来的比写进去的多 */
        s->i++;
        return 0;
    }
    if (r->time != s->expect[s->i].time || r->tag != s->expect[s->i].tag)
        s->mismatch++;
    if (dbits(r->value) != dbits(s->expect[s->i].value))
        s->mismatch++;
    s->i++;
    return 0;
}

/* ---------------- 1) 时间路由 ---------------- */

static void t1_route(void)
{
    printf("  1) 时间路由：左闭右开 / 负时间戳向下取整 / 多种窗口\n");

    /* 1 天窗口 */
    CHECK(ts_route_partition(0, MS_DAY) == 0);
    CHECK(ts_route_partition(1, MS_DAY) == 0);
    CHECK(ts_route_partition(MS_DAY - 1, MS_DAY) == 0);
    CHECK(ts_route_partition(MS_DAY, MS_DAY) == MS_DAY);          /* 边界归下一窗 */
    CHECK(ts_route_partition(2 * MS_DAY - 1, MS_DAY) == MS_DAY);

    /*
     * 负时间戳：除法向零截断会把 -1ms 算进第 0 个窗口，
     * 而第 0 个窗口是 [0, +∞) —— -1ms 根本不在里面 ⇒ 落错分区。
     */
    CHECK(ts_route_partition(-1, MS_DAY) == -MS_DAY);
    CHECK(ts_route_partition(-MS_DAY, MS_DAY) == -MS_DAY);
    CHECK(ts_route_partition(-MS_DAY - 1, MS_DAY) == -2 * MS_DAY);
    CHECK(ts_route_partition(-3 * MS_DAY, MS_DAY) == -3 * MS_DAY);

    /* 1 秒窗口 */
    CHECK(ts_route_partition(0, 1000) == 0);
    CHECK(ts_route_partition(999, 1000) == 0);
    CHECK(ts_route_partition(1000, 1000) == 1000);
    CHECK(ts_route_partition(1500, 1000) == 1000);
    CHECK(ts_route_partition(-1, 1000) == -1000);
    CHECK(ts_route_partition(-1000, 1000) == -1000);
    CHECK(ts_route_partition(-1001, 1000) == -2000);

    /* 7 天窗口 */
    CHECK(ts_route_partition(0, MS_WEEK) == 0);
    CHECK(ts_route_partition(MS_WEEK - 1, MS_WEEK) == 0);
    CHECK(ts_route_partition(MS_WEEK, MS_WEEK) == MS_WEEK);
    CHECK(ts_route_partition(-1, MS_WEEK) == -MS_WEEK);

    /*
     * 不变量自检（与实现内部断言同一件事，这里从外面再验一遍）：
     * 对任意 t，必须 part_start ≤ t < part_start + span。
     */
    {
        i64 ts[] = { 0, 1, -1, 999, 1000, -1000, 123456789, -123456789,
                     MS_DAY, MS_DAY - 1, MS_DAY + 1, -MS_DAY, -MS_DAY - 1 };
        u32 i;
        i64 spans[] = { 1, 1000, MS_SEC, MS_DAY, MS_WEEK };

        for (i = 0; i < sizeof(ts) / sizeof(ts[0]); i++) {
            u32 s;
            for (s = 0; s < sizeof(spans) / sizeof(spans[0]); s++) {
                i64 start = ts_route_partition(ts[i], spans[s]);

                CHECK(start <= ts[i]);
                CHECK(ts[i] < start + spans[s]);
                /* 路由的起点必须是"自己路由自己"（幂等） */
                CHECK(ts_route_partition(start, spans[s]) == start);
            }
        }
    }
}

/* ---------------- 2) 跨分区写入 + 目录 ---------------- */

static void t2_write(void)
{
    printf("  2) 跨分区写入：分区数 / 每分区行数 / 路由回去找得到\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading rs[6];
    u32 i;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);      /* 1 天窗口 */

    /* 三个时间窗，每窗两行；边界值故意踩在窗口起点上 */
    rs[0].time = 1000;         rs[0].tag = 1; rs[0].value = 1.5;
    rs[1].time = 2000;         rs[1].tag = 2; rs[1].value = 2.5;
    rs[2].time = MS_DAY;       rs[2].tag = 3; rs[2].value = 3.5;   /* 窗口 1 起点 */
    rs[3].time = MS_DAY + 1;   rs[3].tag = 4; rs[3].value = 4.5;
    rs[4].time = 2 * MS_DAY;   rs[4].tag = 5; rs[4].value = 5.5;   /* 窗口 2 起点 */
    rs[5].time = 2 * MS_DAY + 7; rs[5].tag = 6; rs[5].value = 6.5;

    for (i = 0; i < 6; i++)
        CHECK(ts_store_insert(st, &rs[i]) == OK);

    /* 分区数：3 个 */
    CHECK(ts_store_part_count(st) == 3);
    CHECK(ts_store_row_count(st) == 6);

    /* 每个分区 2 行，且按起始时间升序 */
    for (i = 0; i < 3; i++) {
        TsPartInfo pi;

        CHECK(ts_store_part_info(st, i, &pi) == OK);
        CHECK(pi.n_rows == 2);
        CHECK(pi.state == TS_PART_ACTIVE);
        CHECK(pi.start_ms == (i64)i * MS_DAY);
        CHECK(pi.end_ms == (i64)(i + 1) * MS_DAY);
        CHECK(pi.page_count == 1);
    }

    /* 越界取目录项 → E_INVAL */
    {
        TsPartInfo pi;

        CHECK(ts_store_part_info(st, 3, &pi) == E_INVAL);
    }

    /* 路由回去：每个时间戳都能找回自己的分区 */
    for (i = 0; i < 6; i++) {
        TsPartInfo pi;

        CHECK(ts_store_find_part(st, rs[i].time, &pi) == OK);
        CHECK(pi.start_ms <= rs[i].time);
        CHECK(rs[i].time < pi.end_ms);
    }

    /* 窗口边界：恰好在起点上归自己，恰好在终点上归下一个 */
    {
        TsPartInfo pi;

        CHECK(ts_store_find_part(st, 0, &pi) == OK);
        CHECK(pi.start_ms == 0);
        CHECK(ts_store_find_part(st, MS_DAY - 1, &pi) == OK);
        CHECK(pi.start_ms == 0);
        CHECK(ts_store_find_part(st, MS_DAY, &pi) == OK);
        CHECK(pi.start_ms == MS_DAY);
    }

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 3) 读回来的读数逐个相等 ---------------- */

static void t3_read(void)
{
    printf("  3) 读取：逐条相等（浮点比位模式）\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading rs[8];
    ScanCtx sc;
    u32 i;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);

    for (i = 0; i < 8; i++) {
        rs[i].time = (i64)i * 1000;
        rs[i].tag = (i64)(100 + i);
        rs[i].value = (double)i / 3.0;
    }
    /* 掺几个"难看"的浮点：-0.0、无穷、极小值 */
    rs[2].value = -0.0;
    rs[3].value = 1.0 / 3.0;
    rs[4].value = 1e-300;
    rs[5].value = 1e300;

    for (i = 0; i < 8; i++)
        CHECK(ts_store_insert(st, &rs[i]) == OK);

    sc.expect = rs;
    sc.n = 8;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == 8);
    CHECK(sc.mismatch == 0);
    CHECK(ts_store_row_count(st) == 8);

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 4) R3 负例：找不到就报 E_NOTFOUND ---------------- */

static void t4_notfound(void)
{
    printf("  4) R3 负例：不在任何分区里的时间戳 → E_NOTFOUND（不糊弄）\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading r;
    TsPartInfo pi;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);

    r.time = 1000;
    r.tag = 1;
    r.value = 1.0;
    CHECK(ts_store_insert(st, &r) == OK);

    /* 只有一个分区：[0, 1 天) */
    CHECK(ts_store_part_count(st) == 1);

    /* 别的窗口里没有分区 ⇒ 必须明确说"没有" */
    CHECK(ts_store_find_part(st, MS_DAY, &pi) == E_NOTFOUND);
    CHECK(ts_store_find_part(st, MS_DAY + 12345, &pi) == E_NOTFOUND);
    CHECK(ts_store_find_part(st, -1, &pi) == E_NOTFOUND);
    CHECK(ts_store_find_part(st, -MS_DAY, &pi) == E_NOTFOUND);

    /* 自己窗口里的能找到 */
    CHECK(ts_store_find_part(st, 1000, &pi) == OK);
    CHECK(ts_store_find_part(st, MS_DAY - 1, &pi) == OK);

    /*
     * 目录自洽性：每个分区都必须满足
     *   start = route(start)（起点的路由结果就是它自己）
     *   start ≤ start < end（窗口非空且左闭右开）
     * 这是"路由算错 ⇒ 静默漏数据"的外部体检。
     */
    {
        u32 i;

        for (i = 0; i < ts_store_part_count(st); i++) {
            TsPartInfo p;

            CHECK(ts_store_part_info(st, i, &p) == OK);
            CHECK(ts_route_partition(p.start_ms, (i64)ts_store_span_sec(st) * MS_SEC)
                  == p.start_ms);
            CHECK(p.start_ms < p.end_ms);
            CHECK(in_win(p.start_ms, p.start_ms, p.end_ms));
        }
    }

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 5) 关库重开 ---------------- */

static void t5_reopen(void)
{
    printf("  5) 关库重开：数据还在，目录也重建出来\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading rs[6];
    ScanCtx sc;
    u32 i;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);

    for (i = 0; i < 6; i++) {
        rs[i].time = (i64)i * MS_DAY;          /* 每个点一个窗口 */
        rs[i].tag = (i64)i;
        rs[i].value = (double)i * 1.25;
        CHECK(ts_store_insert(st, &rs[i]) == OK);
    }
    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);

    /* 重开 */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);
    CHECK(ts_store_part_count(st) == 6);
    CHECK(ts_store_row_count(st) == 6);
    CHECK(ts_store_span_sec(st) == 86400);

    for (i = 0; i < 6; i++) {
        TsPartInfo pi;

        CHECK(ts_store_part_info(st, i, &pi) == OK);
        CHECK(pi.start_ms == (i64)i * MS_DAY);
        CHECK(pi.n_rows == 1);
    }

    sc.expect = rs;
    sc.n = 6;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == 6);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 6) 页满 spill（续开新页，分区数不变） ---------------- */

static void t6_spill(void)
{
    printf("  6) spill：一页写满 → 续开新页（分区数不变、行数不变、读得全）\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading *rs;
    TsPartInfo pi;
    ScanCtx sc;
    u32 n = 400;                 /* 一页装 169 行 ⇒ 必然 spill */
    u32 i;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);

    rs = (TsReading *)malloc(sizeof(TsReading) * n);
    CHECK(rs != NULL);
    if (rs == NULL) {
        ts_store_destroy(st);
        pager_close(pg);
        return;
    }

    for (i = 0; i < n; i++) {
        rs[i].time = 1000 + (i64)i * 10;
        rs[i].tag = (i64)i;
        rs[i].value = (double)i * 0.5;
        CHECK(ts_store_insert(st, &rs[i]) == OK);
    }

    /* 全在同一个时间窗 ⇒ 只有一个分区 */
    CHECK(ts_store_part_count(st) == 1);
    CHECK(ts_store_row_count(st) == n);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.n_rows == n);
    CHECK(pi.page_count == 3);      /* 169 + 169 + 62 */
    printf("    %u 行落在 %u 页里\n", n, pi.page_count);

    sc.expect = rs;
    sc.n = n;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    /* 重开后再验一遍（spill 出来的页链也要能重建） */
    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);
    CHECK(ts_store_row_count(st) == n);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.n_rows == n);
    CHECK(pi.page_count == 3);

    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
    free(rs);
}

/* ---------------- 7) 参数与边界 ---------------- */

static void t7_params(void)
{
    printf("  7) 参数：非法窗口 / 空库开库 / 改窗口只影响之后的写入\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading r;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    /* 窗口为 0 → E_INVAL */
    CHECK(ts_store_create(pg, 0, &st) == E_INVAL);
    CHECK(st == NULL);

    CHECK(ts_store_create(pg, 86400, &st) == OK);
    CHECK(ts_store_span_sec(st) == 86400);

    /* 改窗口：只影响之后的写入 */
    CHECK(ts_store_set_span_sec(st, 7 * 86400) == OK);
    CHECK(ts_store_span_sec(st) == 7 * 86400);
    CHECK(ts_store_set_span_sec(st, 0) == E_INVAL);
    CHECK(ts_store_span_sec(st) == 7 * 86400);

    /* 空库：分区数 0、行数 0 */
    CHECK(ts_store_part_count(st) == 0);
    CHECK(ts_store_row_count(st) == 0);

    /* 写两条：都在第 0 个窗口（7 天窗口） */
    r.time = 1000; r.tag = 1; r.value = 1.0;
    CHECK(ts_store_insert(st, &r) == OK);
    r.time = 2 * MS_DAY; r.tag = 2; r.value = 2.0;
    CHECK(ts_store_insert(st, &r) == OK);
    CHECK(ts_store_part_count(st) == 1);
    CHECK(ts_store_row_count(st) == 2);

    ts_store_destroy(st);
    pager_close(pg);

    /* 空库（没建过时序存储）→ ts_store_open 报 E_NOTFOUND（不是错误） */
    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == E_NOTFOUND);
    CHECK(st == NULL);
    pager_close(pg);
}

/* ---------------- 8) 诊断输出 ---------------- */

static void t8_diag(void)
{
    printf("  8) 诊断：分区数 / 总行数 / 页数 / 页读数\n");
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading r;
    u32 i;
    u32 pages = 0;
    u64 reads_before;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 86400, &st) == OK);

    /* 3 个窗口，每窗 10 行 */
    for (i = 0; i < 30; i++) {
        r.time = (i64)(i / 10) * MS_DAY + (i64)(i % 10) * 100;
        r.tag = (i64)i;
        r.value = (double)i;
        CHECK(ts_store_insert(st, &r) == OK);
    }

    for (i = 0; i < ts_store_part_count(st); i++) {
        TsPartInfo pi;

        CHECK(ts_store_part_info(st, i, &pi) == OK);
        pages += pi.page_count;
    }

    reads_before = ts_store_page_reads(st);
    {
        ScanCtx sc;

        sc.expect = NULL;
        sc.n = 0;
        sc.i = 0;
        sc.mismatch = 0;
        (void)ts_store_scan(st, scan_cb, &sc);
    }

    printf("    分区数 = %u，总行数 = %u，数据页数 = %u\n",
           ts_store_part_count(st), ts_store_row_count(st), pages);
    printf("    累计逻辑页读：写完 %llu，扫描一遍后 %llu\n",
           (unsigned long long)reads_before,
           (unsigned long long)ts_store_page_reads(st));

    CHECK(ts_store_part_count(st) == 3);
    CHECK(ts_store_row_count(st) == 30);
    CHECK(pages == 3);
    CHECK(ts_store_page_reads(st) > reads_before);

    ts_store_destroy(st);
    pager_close(pg);
}

int main(void)
{
    printf("test_ts_store:\n");

    t1_route();
    t2_write();
    t3_read();
    t4_notfound();
    t5_reopen();
    t6_spill();
    t7_params();
    t8_diag();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  全部通过\n");
    return 0;
}
