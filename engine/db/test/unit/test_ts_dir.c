/*
 * test_ts_dir.c —— 时序分区目录的持久化验收（v1.5 步骤 2 · T5）
 *
 * 九组验收：① 基本往返 ② 50 分区逐字段相等 ③ 300 分区跨页一个不少
 * ④ 排序与查找（空档/已 DROP ⇒ E_NOTFOUND）⑤ 就地更新（封窗）
 * ⑥ 开库重建 == 内存 ⑦ 损坏检测（页类型/校验和/条数离谱/链截断/meta 版本/
 *    目录项不合法 ⇒ 一律 E_CORRUPT）⑧ 无目录 ⇒ E_NOTFOUND ⑨ 非法输入。
 *
 * 【为什么"损坏必须报错"而不是"尽力返回"】
 * 目录读坏只有两种可能：读到**一半**（链断在中间），或读到**假的**（字段被改坏）。
 * 都"尽力返回一个目录"的后果是**静默漏数据**：查询按分区裁剪，漏掉的分区里的数据
 * 就永远查不到，还没有任何报错。所以宁可**开库就报错**：报错会让人停下，错的数据会一路流下去。
 *
 * 【为什么"逐字段相等"而不是"查得到就行"】
 * 目录项六个字段每一个都有用（时间窗决定路由、状态决定读不读、行数决定计数、
 * 首页与页数决定去哪儿读）。少写或写错一个字段，症状都是"查出来的数据不对"，
 * 而且很难定位到目录头上。逐字段相等能在**写入侧**就把问题抓住。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/tsstore.h"
#include "storage/page.h"
#include "storage/pager.h"

#define PATH "test/tmp/g7_ts_dir.db"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 小工具 ---------------- */

/* 造一个分区项：六个字段都带上"个性"，便于逐字段比对 */
static TsPartInfo mk_part(int i, i64 start_ms, i64 span_ms, u32 rows)
{
    TsPartInfo p;

    p.start_ms = start_ms;
    p.end_ms = start_ms + span_ms;
    p.state = TS_PART_ACTIVE;
    p.n_rows = rows;
    p.first_page = (u32)(1000 + i);
    p.page_count = (u32)(1 + (i % 7));
    return p;
}

/* 两个分区项是否逐字段相等 */
static int part_eq(const TsPartInfo *a, const TsPartInfo *b)
{
    return a->start_ms == b->start_ms &&
           a->end_ms == b->end_ms &&
           a->state == b->state &&
           a->n_rows == b->n_rows &&
           a->first_page == b->first_page &&
           a->page_count == b->page_count;
}

/* 收集页空间里某类型的页号 */
static u32 collect_pages(Pager *pg, u8 type, u32 *ids, u32 max)
{
    u64 np = pager_npages(pg);
    u32 n = 0;
    u32 pid;

    for (pid = 0; (u64)pid < np && n < max; pid++) {
        PageFrame *fr = NULL;

        if (pager_get(pg, pid, &fr) != OK) {
            continue;
        }
        if (fr->data[H_TYPE] == type) {
            ids[n++] = pid;
        }
        (void)pager_unpin(pg, pid, false);
    }
    return n;
}

/* 改页里的一个 u32；fix_cksum=1 ⇒ 重算校验和（内容坏但页自洽），=0 ⇒ 不动（页被撕坏） */
static int patch_u32(Pager *pg, u32 pid, u32 off, u32 val, int fix_cksum)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK) {
        return rc;
    }
    put_u32(fr->data + off, val);
    if (fix_cksum) {
        page_set_checksum(fr->data);
    }
    return pager_unpin(pg, pid, true);
}

/* 改页类型（并重算校验和：页自洽，但类型不对） */
static int patch_type(Pager *pg, u32 pid, u8 type)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK) {
        return rc;
    }
    fr->data[H_TYPE] = type;
    page_set_checksum(fr->data);
    return pager_unpin(pg, pid, true);
}

/* ---------------- 1. 基本往返 ---------------- */

static void t_basic(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo p;
    TsPartInfo back;
    int rc;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    rc = ts_dir_create(pg, 86400u, &d);
    CHECK(rc == OK);
    CHECK(d != NULL);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }
    CHECK(ts_dir_count(d) == 0u);
    CHECK(ts_dir_span_sec(d) == 86400u);

    p = mk_part(0, 1000, 1000, 7);
    CHECK(ts_dir_append(d, &p) == OK);
    CHECK(ts_dir_count(d) == 1u);
    CHECK(ts_dir_get(d, 0u, &back) == OK);
    CHECK(part_eq(&p, &back));

    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);

    /* 重开：目录必须从磁盘回来 */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == OK);
    CHECK(d != NULL);
    if (d != NULL) {
        CHECK(ts_dir_count(d) == 1u);
        CHECK(ts_dir_get(d, 0u, &back) == OK);
        CHECK(part_eq(&p, &back));
        CHECK(ts_dir_span_sec(d) == 86400u);
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 2. 逐字段相等（50 个分区） ---------------- */

static void t_fields(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo parts[50];
    TsPartInfo back;
    int i;
    int rc;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 3600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }
    for (i = 0; i < 50; i++) {
        parts[i] = mk_part(i, (i64)i * 3600000LL, 3600000LL, (u32)(i * 3 + 1));
        /* 状态也变化一下，别全都是 ACTIVE */
        if (i % 3 == 0) {
            parts[i].state = TS_PART_SEALED;
        }
        if (i % 5 == 0) {
            parts[i].state = TS_PART_DROPPED;
        }
        /* 空分区（没有页）也要能存 */
        if (i % 7 == 0) {
            parts[i].first_page = PAGE_ID_NIL;
            parts[i].page_count = 0;
            parts[i].n_rows = 0;
        }
        rc = ts_dir_append(d, &parts[i]);
        CHECK(rc == OK);
    }
    CHECK(ts_dir_count(d) == 50u);
    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);

    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == OK);
    if (d != NULL) {
        CHECK(ts_dir_count(d) == 50u);
        for (i = 0; i < 50; i++) {
            CHECK(ts_dir_get(d, (u32)i, &back) == OK);
            if (!part_eq(&parts[i], &back)) {
                printf("    FAIL 第 %d 个分区不一致: (%lld,%lld,%u,%u,%u,%u) vs "
                       "(%lld,%lld,%u,%u,%u,%u)\n", i,
                       (long long)parts[i].start_ms, (long long)parts[i].end_ms,
                       (unsigned)parts[i].state, parts[i].n_rows,
                       parts[i].first_page, parts[i].page_count,
                       (long long)back.start_ms, (long long)back.end_ms,
                       (unsigned)back.state, back.n_rows, back.first_page, back.page_count);
                failures++;
            }
        }
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 3. 跨页（300 个分区 ⇒ 3 张目录页） ---------------- */

static void t_cross_page(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    int i;
    int rc;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 60u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }
    for (i = 0; i < 300; i++) {
        TsPartInfo p = mk_part(i, (i64)i * 60000LL, 60000LL, (u32)i);

        rc = ts_dir_append(d, &p);
        CHECK(rc == OK);
    }
    CHECK(ts_dir_count(d) == 300u);
    /* 300 条 ⇒ 至少 3 张目录页（每页 126 条） */
    CHECK(ts_dir_page_count(d) >= 3u);
    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);

    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == OK);
    if (d != NULL) {
        CHECK(ts_dir_count(d) == 300u);
        CHECK(ts_dir_page_count(d) >= 3u);
        /* 一个都不能少，顺序也不能乱 */
        for (i = 0; i < 300; i++) {
            TsPartInfo back;

            CHECK(ts_dir_get(d, (u32)i, &back) == OK);
            CHECK(back.start_ms == (i64)i * 60000LL);
            CHECK(back.n_rows == (u32)i);
        }
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 4. 排序与查找 ---------------- */

static void t_sorted_find(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo back;
    u32 idx = 0;
    int rc;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 100u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }

    /* 乱序追加：时间窗 0, 300, 100, 200（每窗 100ms） */
    {
        static const i64 starts[4] = { 0, 300, 100, 200 };
        int k;

        for (k = 0; k < 4; k++) {
            TsPartInfo p = mk_part(k, starts[k], 100, 1);

            rc = ts_dir_append(d, &p);
            CHECK(rc == OK);
        }
    }

    /* 按时间升序取 */
    CHECK(ts_dir_get_by_time(d, 0u, &back) == OK);
    CHECK(back.start_ms == 0);
    CHECK(ts_dir_get_by_time(d, 1u, &back) == OK);
    CHECK(back.start_ms == 100);
    CHECK(ts_dir_get_by_time(d, 2u, &back) == OK);
    CHECK(back.start_ms == 200);
    CHECK(ts_dir_get_by_time(d, 3u, &back) == OK);
    CHECK(back.start_ms == 300);
    /* 越界 */
    CHECK(ts_dir_get_by_time(d, 4u, &back) == E_INVAL);

    /* 查找：命中 */
    CHECK(ts_dir_find(d, 250, &idx, &back) == OK);
    CHECK(back.start_ms == 200);
    /* 落在空档（四个窗口盖住 [0,400)，400 之后没有分区） */
    CHECK(ts_dir_find(d, 400, &idx, &back) == E_NOTFOUND);
    /* 比所有分区都早 */
    CHECK(ts_dir_find(d, -10, &idx, &back) == E_NOTFOUND);

    /* 已 DROP 的分区不该再被查到 */
    {
        TsPartInfo p = mk_part(9, 1000, 100, 5);

        p.state = TS_PART_DROPPED;
        p.first_page = PAGE_ID_NIL;
        p.page_count = 0;
        CHECK(ts_dir_append(d, &p) == OK);
        CHECK(ts_dir_find(d, 1050, &idx, &back) == E_NOTFOUND);
    }

    /* 空目录里找 ⇒ E_NOTFOUND */
    {
        TsDir *e = NULL;

        CHECK(ts_dir_create(pg, 60u, &e) == OK);
        if (e != NULL) {
            CHECK(ts_dir_find(e, 0, &idx, &back) == E_NOTFOUND);
            ts_dir_destroy(e);
        }
    }

    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 5. 就地更新（封窗） ---------------- */

static void t_update(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo p;
    TsPartInfo back;
    int rc;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 3600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }
    p = mk_part(0, 0, 3600000LL, 0);
    p.first_page = PAGE_ID_NIL;
    p.page_count = 0;
    CHECK(ts_dir_append(d, &p) == OK);

    /* 封窗：状态改 SEALED，行数定了，页也有了 */
    p.state = TS_PART_SEALED;
    p.n_rows = 1234;
    p.first_page = 88;
    p.page_count = 3;
    rc = ts_dir_update(d, 0u, &p);
    CHECK(rc == OK);
    /* 越界 */
    CHECK(ts_dir_update(d, 5u, &p) == E_INVAL);
    /* 不合法的项：时间窗倒挂 */
    {
        TsPartInfo bad = p;

        bad.start_ms = 10;
        bad.end_ms = 10;
        CHECK(ts_dir_update(d, 0u, &bad) == E_INVAL);
    }
    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);

    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == OK);
    if (d != NULL) {
        CHECK(ts_dir_count(d) == 1u);
        CHECK(ts_dir_get(d, 0u, &back) == OK);
        CHECK(back.state == TS_PART_SEALED);
        CHECK(back.n_rows == 1234);
        CHECK(back.first_page == 88);
        CHECK(back.page_count == 3);
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 6. 开库重建 == 内存 ---------------- */

static void t_rebuild(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo snap[128];
    int i;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }
    for (i = 0; i < 128; i++) {
        TsPartInfo p = mk_part(i, (i64)i * 600000LL, 600000LL, (u32)(i + 1));

        CHECK(ts_dir_append(d, &p) == OK);
        snap[i] = p;
    }
    CHECK(ts_dir_total_rows(d) == 8256u);   /* 1+2+...+128 */
    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);

    /* 重开：读回来的必须与关库前**逐字段**一致 */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == OK);
    if (d != NULL) {
        TsPartInfo back;

        CHECK(ts_dir_count(d) == 128u);
        for (i = 0; i < 128; i++) {
            CHECK(ts_dir_get(d, (u32)i, &back) == OK);
            if (!part_eq(&snap[i], &back)) {
                printf("    FAIL 重建后第 %d 项不一致\n", i);
                failures++;
            }
        }
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 7. 损坏检测 ---------------- */

/* 造一个"有 N 个分区"的库，返回 meta 页号；文件保持打开 */
static u32 build_db(Pager **pg_out, u32 n_parts)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    u32 ids[64];
    u32 n;
    u32 i;

    remove(PATH);
    if (pager_open(PATH, 64, &pg) != OK) {
        return PAGE_ID_NIL;
    }
    if (ts_dir_create(pg, 3600u, &d) != OK || d == NULL) {
        (void)pager_close(pg);
        return PAGE_ID_NIL;
    }
    for (i = 0; i < n_parts; i++) {
        TsPartInfo p = mk_part((int)i, (i64)i * 3600000LL, 3600000LL, (u32)(i + 1));

        (void)ts_dir_append(d, &p);
    }
    ts_dir_destroy(d);
    (void)pager_close(pg);

    if (pager_open(PATH, 64, &pg) != OK) {
        return PAGE_ID_NIL;
    }
    *pg_out = pg;
    n = collect_pages(pg, (u8)PT_TS_META, ids, 64);
    return (n > 0u) ? ids[0] : PAGE_ID_NIL;
}

/* 开库必须报 E_CORRUPT，而且不得交出"半个目录" */
static int expect_corrupt(Pager *pg)
{
    TsDir *d = NULL;
    int rc = ts_dir_open(pg, &d);

    if (d != NULL) {
        ts_dir_destroy(d);
    }
    return (rc == E_CORRUPT && d == NULL);
}

static void t_corrupt(void)
{
    Pager *pg = NULL;
    u32 dir_ids[64];
    u32 ndir;
    u32 meta;

    /* 7a. 页类型错（页自洽，但类型不是目录页） */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            CHECK(patch_type(pg, dir_ids[0], (u8)PT_HEAP) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7b. 校验和错（页被撕坏） */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            /* 改一个字节，不重算校验和 */
            CHECK(patch_u32(pg, dir_ids[0], TSD_NENT, 3u, 0) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7c. 条数离谱（nent > 一页能放的条数） */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            CHECK(patch_u32(pg, dir_ids[0], TSD_NENT, 99999u, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7d. 链被截断（把某页的 next 改成 NIL ⇒ 链上条数少于水位线） */
    meta = build_db(&pg, 300);   /* 300 条 ⇒ 3 张目录页 */
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 3u);
        if (ndir >= 3u) {
            /* 截断中间那张 ⇒ 后面的分区读不到了 */
            CHECK(patch_u32(pg, dir_ids[1], TSD_NEXT, PAGE_ID_NIL, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7e. meta 页版本不认识 */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        CHECK(patch_u32(pg, meta, TSM_VER, 99u, 1) == OK);
        CHECK(expect_corrupt(pg));
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7f. meta 页校验和错 */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        CHECK(patch_u32(pg, meta, TSM_SPAN, 123u, 0) == OK);
        CHECK(expect_corrupt(pg));
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7g. 目录项不合法（时间窗倒挂）⇒ 假数据，必须报错 */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            /* 第一项的 start 改成 0xFFFFFFFF（比 end 大）⇒ 时间窗倒挂 ⇒ 假数据 */
            CHECK(patch_u32(pg, dir_ids[0],
                            (u32)(TSD_DATA + TSE_START), 0xFFFFFFFFu, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7h. 目录项：state 非法 */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            CHECK(patch_u32(pg, dir_ids[0],
                            (u32)(TSD_DATA + TSE_STATE), 77u, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7i. 链不连贯（某页的 prev 指向了别的页） */
    meta = build_db(&pg, 300);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 3u);
        if (ndir >= 3u) {
            /* 把中间那张的 prev 改成一个不相干的页号 */
            CHECK(patch_u32(pg, dir_ids[1], TSD_PREV, dir_ids[0] + 100u, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /* 7j. 目录页链的首页 prev 不该指向别的页 */
    meta = build_db(&pg, 10);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 1u);
        if (ndir >= 1u) {
            CHECK(patch_u32(pg, dir_ids[0], TSD_PREV, dir_ids[0] + 50u, 1) == OK);
            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    /*
     * 7k. 条数离谱（nent > 一页能放的条数）。
     *
     * 这一条要特别小心才测得准：光把 nent 改成 99999，"条数离谱"和"目录项不合法"
     * 都会报 E_CORRUPT ⇒ 测不出是哪一条在起作用。
     * 做法：先造一张放满 126 条的目录页，再把页尾空字节手工刻成一条**合法**目录项
     * （第 127 条前 28 字节在页内、最后 4 字节落在页外且恰好非零，整条仍满足不变量）。
     * 于是：有检查 ⇒ 127 > 126 ⇒ E_CORRUPT；没检查 ⇒ 127 条全合法 ⇒ 开库成功 ⇒ 断言变红。
     * 这条真正卡住的是"不许读越界"——否则解码器会顺着 nent 读到页外几 MB。
     */
    meta = build_db(&pg, 130);
    CHECK(meta != PAGE_ID_NIL);
    if (pg != NULL) {
        ndir = collect_pages(pg, (u8)PT_TS_DIR, dir_ids, 64);
        CHECK(ndir >= 2u);
        if (ndir >= 2u) {
            u32 full = dir_ids[0];   /* 第一张目录页：放满了 126 条 */
            u32 e126 = (u32)(TSD_DATA + 126u * TS_DIR_ENT_SIZE);

            /* 刻一条合法项：start=1000000, end=1000100, state=ACTIVE, first_page=5 */
            CHECK(patch_u32(pg, full, e126 + (u32)TSE_START, 1000000u, 1) == OK);
            CHECK(patch_u32(pg, full, e126 + (u32)TSE_END, 1000100u, 1) == OK);
            CHECK(patch_u32(pg, full, e126 + (u32)TSE_FIRST, 5u, 1) == OK);
            /* 条数改成 127（超过一页能放的 126 条） */
            CHECK(patch_u32(pg, full, TSD_NENT, 127u, 1) == OK);

            CHECK(expect_corrupt(pg));
        }
        (void)pager_close(pg);
        pg = NULL;
    }

    remove(PATH);
}

/* ---------------- 8. 没有目录的库 ⇒ E_NOTFOUND ---------------- */

static void t_no_dir(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    d = NULL;
    CHECK(ts_dir_open(pg, &d) == E_NOTFOUND);
    CHECK(d == NULL);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 9. 非法输入 ---------------- */

static void t_invalid(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo p;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    /* 窗口为 0 ⇒ E_INVAL */
    CHECK(ts_dir_create(pg, 0u, &d) == E_INVAL);
    CHECK(ts_dir_create(pg, 60u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }

    p = mk_part(0, 100, 100, 1);
    /* 时间窗倒挂 */
    {
        TsPartInfo bad = p;

        bad.start_ms = 200;
        bad.end_ms = 100;
        CHECK(ts_dir_append(d, &bad) == E_INVAL);
    }
    /* 空窗（start == end） */
    {
        TsPartInfo bad = p;

        bad.end_ms = bad.start_ms;
        CHECK(ts_dir_append(d, &bad) == E_INVAL);
    }
    /* 有页数却没有首页 */
    {
        TsPartInfo bad = p;

        bad.first_page = PAGE_ID_NIL;
        bad.page_count = 3;
        CHECK(ts_dir_append(d, &bad) == E_INVAL);
    }
    /* 没页数却指着首页 */
    {
        TsPartInfo bad = p;

        bad.first_page = 42;
        bad.page_count = 0;
        CHECK(ts_dir_append(d, &bad) == E_INVAL);
    }
    /* 状态非法 */
    {
        TsPartInfo bad = p;

        bad.state = (TsPartState)9;
        CHECK(ts_dir_append(d, &bad) == E_INVAL);
    }
    /* 上面这些都不该把分区写进去 */
    CHECK(ts_dir_count(d) == 0u);

    /* 改窗口 */
    CHECK(ts_dir_set_span_sec(d, 120u) == OK);
    CHECK(ts_dir_span_sec(d) == 120u);
    CHECK(ts_dir_set_span_sec(d, 0u) == E_INVAL);

    ts_dir_destroy(d);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

int main(void)
{
    printf("test_ts_dir ...\n");

    t_basic();
    t_fields();
    t_cross_page();
    t_sorted_find();
    t_update();
    t_rebuild();
    t_corrupt();
    t_no_dir();
    t_invalid();

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
