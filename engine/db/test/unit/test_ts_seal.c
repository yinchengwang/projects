/*
 * test_ts_seal.c —— 时序模态封窗与列式读路径的单测（v1.5 步骤 3 · T7）
 *
 * 契约见 src/modalities/ts/ts_seal.h 与 ts_column.h。
 *
 * ==================== 这一步要证明什么 ====================
 *
 * 1. **列式往返全等**：多种规模（0 / 1 / 2 / 17 / 1000 / 50000），
 *    写进去的每一行读回来都**逐个相等**（浮点比位模式）。
 * 2. **封窗后数据不变**：达到阈值 ⇒ 自动封窗 ⇒ 扫描结果与写进去的一模一样。
 * 3. **压缩效果可证**：封窗后占的页数**明显少于**封窗前。
 * 4. **迟到写入被明确拒绝**：封窗后再往同一个时间窗写 ⇒ E_NOTSUP。
 * 5. **封窗中途失败不产生半截**：只写了列式页、没翻目录 ⇒
 *    读到的必须是**完整的旧分区**（关库重开也一样）。
 * 6. **关库重开仍读得对**：封窗分区重开后行数与内容都对。
 * 7. **混读全等**：有的分区封了、有的没封 ⇒ 整体扫描结果仍然全等。
 *
 * ==================== 为什么浮点要比位模式 ====================
 *
 * 浮点的 `==` 看起来能用，但 NaN != NaN，而且编译器可能按"肯定相等"
 * 优化。磁盘上存的是**位模式** ⇒ 这里也比位模式：写什么位，读回什么位。
 *
 * ==================== 测试数据都是"二进制精确"的 ====================
 *
 * 造的值都是 1/8 的整数倍（0.125 / 0.5 / 1.5 …）⇒ 在 double 里**精确可表示**，
 * 不存在"四舍五入成另一个数"的干扰（坑 M6-38）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_column.h"
#include "modalities/ts/ts_seal.h"
#include "modalities/ts/tsstore.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ts_seal_test.db"

/* 浮点位模式 */
static u64 dbits(double d)
{
    u64 b;

    memcpy(&b, &d, sizeof(b));
    return b;
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

/* 造一批读数：时间等间隔、标签有重复、值是二进制精确的分数 */
static void fill_readings(TsReading *rs, u32 n, i64 t0, i64 step)
{
    u32 i;

    for (i = 0; i < n; i++) {
        rs[i].time = t0 + (i64)i * step;
        rs[i].tag = (i64)(i % 7u);
        rs[i].value = (double)(i % 977u) * 0.125;
    }
}

/* ---------------- 1) 列式页往返 ---------------- */

static void check_column_roundtrip(Pager *pg, u32 n, i64 step)
{
    i64 *ts = NULL;
    i64 *tag = NULL;
    double *val = NULL;
    i64 *ts2 = NULL;
    i64 *tag2 = NULL;
    double *val2 = NULL;
    u32 head = 0;
    u32 pages = 0;
    u32 i;
    int bad = 0;
    int rc;

    ts = (i64 *)malloc(sizeof(i64) * (n ? n : 1u));
    tag = (i64 *)malloc(sizeof(i64) * (n ? n : 1u));
    val = (double *)malloc(sizeof(double) * (n ? n : 1u));
    ts2 = (i64 *)malloc(sizeof(i64) * (n ? n : 1u));
    tag2 = (i64 *)malloc(sizeof(i64) * (n ? n : 1u));
    val2 = (double *)malloc(sizeof(double) * (n ? n : 1u));
    if (!ts || !tag || !val || !ts2 || !tag2 || !val2) {
        CHECK(0);
        goto out;
    }

    for (i = 0; i < n; i++) {
        ts[i] = 1000000LL + (i64)i * step;
        tag[i] = (i64)(i % 5u);
        val[i] = (double)(i % 101u) * 0.5;
    }

    rc = ts_column_write(pg, ts, tag, val, n, PAGE_ID_NIL, 0u, 7u, &head, &pages);
    CHECK(rc == OK);
    if (rc != OK)
        goto out;

    CHECK(pages >= 1u);            /* 至少一张头部页 */

    memset(ts2, 0, sizeof(i64) * (n ? n : 1u));
    memset(tag2, 0, sizeof(i64) * (n ? n : 1u));
    memset(val2, 0, sizeof(double) * (n ? n : 1u));

    rc = ts_column_read(pg, head, ts2, tag2, val2, n);
    CHECK(rc == OK);
    if (rc != OK)
        goto out;

    for (i = 0; i < n; i++) {
        if (ts2[i] != ts[i] || tag2[i] != tag[i] || dbits(val2[i]) != dbits(val[i]))
            bad++;
    }
    CHECK(bad == 0);

    /* 头部页记的行数必须与实际一致（段头必须记条数，坑 M6-39） */
    {
        u32 rows = 0;
        u32 cpages = 0;

        CHECK(ts_column_info(pg, head, &rows, &cpages) == OK);
        CHECK(rows == n);
        CHECK(cpages == pages);
    }

    /* 要的行数与头部页记的不一致 ⇒ 必须报错，不许"读出半截" */
    if (n > 0) {
        i64 junk = 0;
        double jd = 0.0;

        CHECK(ts_column_read(pg, head, &junk, &junk, &jd, n + 1u) == E_CORRUPT);
    }

out:
    free(ts);
    free(tag);
    free(val);
    free(ts2);
    free(tag2);
    free(val2);
}

static void t1_column_roundtrip(void)
{
    Pager *pg = NULL;

    printf("  1) 列式页往返：0 / 1 / 2 / 17 / 1000 / 50000 行\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    if (pg == NULL)
        return;

    check_column_roundtrip(pg, 0u, 1000);        /* 空分区 */
    check_column_roundtrip(pg, 1u, 1000);
    check_column_roundtrip(pg, 2u, 1000);
    check_column_roundtrip(pg, 17u, 1000);
    check_column_roundtrip(pg, 1000u, 1000);
    check_column_roundtrip(pg, 50000u, 1000);     /* 很大 */
    check_column_roundtrip(pg, 1000u, 37);        /* 间隔不规整（dod 的 dod ≠ 0） */

    pager_close(pg);
}

/* ---------------- 2) 封窗往返（自动封窗） ---------------- */

static void t2_seal_roundtrip(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading *rs = NULL;
    TsPartInfo pi;
    ScanCtx sc;
    u32 n = TS_SEAL_ROW_THRESHOLD;   /* 恰好到阈值 ⇒ 自动封窗 */
    u32 i;
    int rc;

    printf("  2) 封窗往返：%u 行 ⇒ 触发自动封窗 ⇒ 读回来全等\n", n);

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 60, &st) == OK);    /* 60 秒窗口 ⇒ 容得下 10000 行 */

    rs = (TsReading *)malloc(sizeof(TsReading) * n);
    CHECK(rs != NULL);
    if (rs == NULL) {
        ts_store_destroy(st);
        pager_close(pg);
        return;
    }

    /* 全落在同一个 1 秒窗口里（[1000000, 1001000)）⇒ 一个分区 */
    fill_readings(rs, n, 1000000LL, 1);

    for (i = 0; i < n; i++) {
        rc = ts_store_insert(st, &rs[i]);
        CHECK(rc == OK);
        if (rc != OK)
            break;
    }

    CHECK(ts_store_part_count(st) == 1);
    CHECK(ts_store_row_count(st) == n);

    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);          /* 已经封窗了 */
    CHECK(pi.n_rows == n);
    printf("    封窗后：行数 = %u，占 %u 页\n", pi.n_rows, pi.page_count);

    sc.expect = rs;
    sc.n = n;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
    free(rs);
}

/* ---------------- 3) 压缩效果可证 ---------------- */

static void t3_compression(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading r;
    TsPartInfo pi;
    u32 i;
    u32 raw_pages;
    u32 col_pages;

    printf("  3) 压缩效果：封窗后的页数明显少于封窗前\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 60, &st) == OK);    /* 60 秒窗口 */

    /* 先写阈值 - 1 行：不封窗 ⇒ 看到裸写的页数 */
    for (i = 0; i < TS_SEAL_ROW_THRESHOLD - 1u; i++) {
        r.time = 1000000LL + (i64)i;
        r.tag = 7;
        r.value = 42.5;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    raw_pages = pi.page_count;
    printf("    封窗前（裸写 %u 行）：%u 页\n", TS_SEAL_ROW_THRESHOLD - 1u, raw_pages);

    /* 再写一行 ⇒ 到阈值 ⇒ 自动封窗 */
    r.time = 1000000LL + (i64)(TS_SEAL_ROW_THRESHOLD - 1u);
    CHECK(ts_store_insert(st, &r) == OK);

    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);
    col_pages = pi.page_count;
    printf("    封窗后（列式 %u 行）：%u 页\n", TS_SEAL_ROW_THRESHOLD, col_pages);

    /* 裸写一行 24 字节 ⇒ 一页 169 行；列式三列各自压缩 ⇒ 页数应少一个量级 */
    CHECK(raw_pages >= 50u);
    CHECK(col_pages < raw_pages / 4u);
    CHECK(col_pages <= 8u);

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 4) 封窗后再写 ⇒ 明确失败 ---------------- */

static void t4_late_write_rejected(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading r;
    TsPartInfo pi;
    u32 i;

    printf("  4) 迟到写入：封窗后再往同一个时间窗写 ⇒ E_NOTSUP\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 60, &st) == OK);    /* 60 秒窗口 */

    for (i = 0; i < TS_SEAL_ROW_THRESHOLD; i++) {
        r.time = 1000000LL + (i64)i;
        r.tag = 1;
        r.value = 1.0;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);

    /* 迟到的那一条：必须明确失败，不许悄悄跑到别处 */
    r.time = 1000000LL + (i64)TS_SEAL_ROW_THRESHOLD;   /* 同一个 1 秒窗口 */
    r.tag = 2;
    r.value = 2.0;
    CHECK(ts_store_insert(st, &r) == E_NOTSUP);

    /* 行数不变（没有偷偷写进去） */
    CHECK(ts_store_row_count(st) == TS_SEAL_ROW_THRESHOLD);
    CHECK(ts_store_part_count(st) == 1);      /* 也没有偷偷开新分区 */

    /* 已经封窗的分区不能重复封窗 */
    CHECK(ts_store_seal(st, 0) == E_NOTSUP);

    ts_store_destroy(st);
    pager_close(pg);
}

/* ---------------- 5) 封窗中途失败 ⇒ 不产生半截 ---------------- */

static void t5_seal_interrupted(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading *rs = NULL;
    TsPartInfo pi;
    ScanCtx sc;
    u32 n = 50;
    u32 i;
    u32 head = 0;
    u32 pages = 0;
    u32 rows = 0;
    u64 reads = 0;

    printf("  5) 封窗中途失败：只写列式页、不翻目录 ⇒ 读到完整的旧分区\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 1, &st) == OK);

    rs = (TsReading *)malloc(sizeof(TsReading) * n);
    CHECK(rs != NULL);
    if (rs == NULL) {
        ts_store_destroy(st);
        pager_close(pg);
        return;
    }
    fill_readings(rs, n, 1000000LL, 10);

    for (i = 0; i < n; i++)
        CHECK(ts_store_insert(st, &rs[i]) == OK);

    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);

    /* 封窗的第一步：写列式页（**故意不翻目录** = 模拟"写了一半"） */
    CHECK(ts_seal_write(pg, pi.first_page, 1u, &reads, &head, &pages, &rows) == OK);
    CHECK(rows == n);
    CHECK(pages >= 1u);

    /* 目录还是 ACTIVE ⇒ 读到的必须是**完整的旧分区**，不是半截 */
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);

    sc.expect = rs;
    sc.n = n;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    /* 关库重开：仍然是完整的旧分区（孤儿列式页只是浪费空间，不丢数据） */
    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);

    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);

    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    /* 现在补上第二步：翻目录 ⇒ 变成完整的**新**分区 */
    CHECK(ts_store_seal_commit(st, 0, head, pages, rows) == OK);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);

    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
    free(rs);
}

/* ---------------- 6) 关库重开：封窗分区仍读得对 ---------------- */

static void t6_reopen_sealed(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading *rs = NULL;
    TsPartInfo pi;
    ScanCtx sc;
    u32 n = 120;
    u32 i;

    printf("  6) 关库重开：封窗分区重开后行数与内容都对\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 1, &st) == OK);

    rs = (TsReading *)malloc(sizeof(TsReading) * n);
    CHECK(rs != NULL);
    if (rs == NULL) {
        ts_store_destroy(st);
        pager_close(pg);
        return;
    }
    fill_readings(rs, n, 1000000LL, 1);

    for (i = 0; i < n; i++)
        CHECK(ts_store_insert(st, &rs[i]) == OK);

    /* 手动封窗（不靠阈值） */
    CHECK(ts_store_seal(st, 0) == OK);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);

    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);

    CHECK(ts_store_part_count(st) == 1);
    CHECK(ts_store_row_count(st) == n);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);
    CHECK(pi.n_rows == n);

    sc.expect = rs;
    sc.n = n;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
    free(rs);
}

/* ---------------- 7) 混读：有的封了有的没封 ---------------- */

static void t7_mixed(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    TsReading *rs = NULL;
    TsPartInfo pi;
    ScanCtx sc;
    u32 per = 40;      /* 每个窗口 40 行 */
    u32 windows = 3;
    u32 total = per * windows;
    u32 i;
    u32 sealed_pages = 0;

    printf("  7) 混读：一个分区封了、两个没封 ⇒ 整体全等\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 1, &st) == OK);

    rs = (TsReading *)malloc(sizeof(TsReading) * total);
    CHECK(rs != NULL);
    if (rs == NULL) {
        ts_store_destroy(st);
        pager_close(pg);
        return;
    }

    /* 三个 1 秒窗口：[1000000,1001000) / [1001000,1002000) / [1002000,1003000) */
    for (i = 0; i < total; i++) {
        rs[i].time = 1000000LL + (i64)(i / per) * 1000LL + (i64)(i % per);
        rs[i].tag = (i64)(i % 3u);
        rs[i].value = (double)(i % 13u) * 0.25;
        CHECK(ts_store_insert(st, &rs[i]) == OK);
    }
    CHECK(ts_store_part_count(st) == 3);

    /* 只封第一个分区 */
    CHECK(ts_store_seal(st, 0) == OK);
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);
    sealed_pages = pi.page_count;
    CHECK(ts_store_part_info(st, 1, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    CHECK(ts_store_part_info(st, 2, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    printf("    封窗分区占 %u 页，另两个分区仍是裸写\n", sealed_pages);

    /* 整体扫描：三条路径（列式 / 裸写 / 裸写）拼起来的结果必须全等 */
    sc.expect = rs;
    sc.n = total;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == total);
    CHECK(sc.mismatch == 0);
    CHECK(ts_store_row_count(st) == total);

    /* 重开后再混读一遍 */
    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);

    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == total);
    CHECK(sc.mismatch == 0);

    ts_store_destroy(st);
    pager_close(pg);
    free(rs);
}

int main(void)
{
    printf("test_ts_seal:\n");

    t1_column_roundtrip();
    t2_seal_roundtrip();
    t3_compression();
    t4_late_write_rejected();
    t5_seal_interrupted();
    t6_reopen_sealed();
    t7_mixed();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  全部通过\n");
    return 0;
}
