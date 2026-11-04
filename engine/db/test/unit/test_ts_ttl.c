/*
 * test_ts_ttl.c —— TTL 整分区过期回收的验收（v1.5 步骤 4 · T8）
 *
 * 七组验收：
 *   1. **只过期该过期的**：过了保留期的被标 DROPPED，其余原封不动。
 *   2. **顺序**：目录先改（落盘），页才回收 —— 本测试验证"落盘"这件事
 *      真的发生了（重开库看到的是 DROPPED）。
 *   3. **查不到**：过期之后查那个时间窗 ⇒ E_NOTFOUND。
 *   4. **页真的还回去了**：全局空闲页计数增加，且下一个分配页拿到了它。
 *   5. **幂等**：TTL 跑两遍，第二次不重复回收、不报错。
 *   6. **新写入不受影响**：TTL 之后照常写、照常读。
 *   7. **边界**：结束时间**正好等于** now − TTL 的分区**不**过期。
 *
 * 【为什么要单测"顺序"】
 *
 * 顺序反了（先回收页、后改目录）崩在中间 ⇒
 * 目录说"这个分区还在"，但页已经被别人拿去用了 ⇒
 * 查询时读到**别人的数据**，而且不报错。
 *
 * 这是本步骤最危险的一条，所以测试盯着它。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/ts/ts_column.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/ts_ttl.h"
#include "modalities/ts/tsstore.h"
#include "storage/page.h"
#include "storage/pager.h"

#define PATH "test/tmp/g8_ts_ttl.db"

/* 当前时间与保留期（毫秒 / 秒） */
#define NOW_MS   100000000LL
#define TTL_SEC  3600u
#define THRESH   (NOW_MS - (i64)TTL_SEC * 1000LL)   /* 96400000 */

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 小工具 ---------------- */

/*
 * 造一个分区：一页裸写页（1 行）+ 一条目录项。
 *
 * 返回首页页号。
 */
static u32 make_part(Pager *pg, TsDir *d, i64 start, i64 end, i64 row_time)
{
    PageFrame *fr = NULL;
    TsPartInfo info;
    u32 pid = 0;
    u8 *row;
    u64 bits;
    double v = 1.5;

    CHECK(pager_alloc(pg, &pid, &fr) == OK);
    if (fr == NULL)
        return PAGE_ID_NIL;

    page_init(fr->data, (u8)PT_TS_PART);
    put_u32(fr->data + TSSP_NROWS, 0);
    put_u32(fr->data + TSSP_NEXT, PAGE_ID_NIL);

    row = fr->data + TSSP_DATA;
    put_u64(row + 0, (u64)row_time);
    put_u64(row + 8, (u64)7);
    memcpy(&bits, &v, sizeof(bits));
    put_u64(row + 16, bits);
    put_u32(fr->data + TSSP_NROWS, 1);
    page_set_checksum(fr->data);
    CHECK(pager_unpin(pg, pid, true) == OK);

    memset(&info, 0, sizeof(info));
    info.start_ms = start;
    info.end_ms = end;
    info.state = TS_PART_ACTIVE;
    info.n_rows = 1;
    info.first_page = pid;
    info.page_count = 1;
    CHECK(ts_dir_append(d, &info) == OK);

    return pid;
}

/* 按磁盘序取第 i 项 */
static int get_part(const TsDir *d, u32 i, TsPartInfo *out)
{
    return ts_dir_get(d, i, out);
}

/* 这一页是不是已经被清零了（回收前先清零） */
static int page_is_zero(Pager *pg, u32 pid)
{
    PageFrame *fr = NULL;
    u32 i;
    int zero = 1;

    if (pager_get(pg, pid, &fr) != OK)
        return 0;
    for (i = 0; i < PAGE_SIZE; i++) {
        if (fr->data[i] != 0u) {
            zero = 0;
            break;
        }
    }
    (void)pager_unpin(pg, pid, false);
    return zero;
}

/* ---------------- 1~5. 主流程 ---------------- */

static void t_expire(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsTtlStats st;
    TsPartInfo p;
    u32 page_old;
    u32 page_mid;
    u32 page_keep;
    u32 page_now;
    u32 free_before;
    int rc;

    printf("  1. 只过期该过期的\n");
    printf("  2. 目录先改、页后回收\n");
    printf("  3. 过期之后查那个时间窗 ⇒ E_NOTFOUND\n");
    printf("  4. 页真的还回去了\n");
    printf("  5. 幂等\n");
    printf("  7. 边界：end == now − TTL 不过期\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 3600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }

    /*
     * 四个分区：
     *   P0 [0, 3600000)            早就过期了
     *   P1 [3600000, 7200000)      早就过期了
     *   P2 [93600000, 96400000)    end **正好等于**阈值 ⇒ 不过期
     *   P3 [97200000, 100800000)   覆盖"现在" ⇒ 不过期
     */
    page_old = make_part(pg, d, 0, 3600000, 1000);
    page_mid = make_part(pg, d, 3600000, 7200000, 3601000);
    page_keep = make_part(pg, d, 93600000, 96400000, 95000000);
    page_now = make_part(pg, d, 97200000, 100800000, 100000000);

    free_before = pager_free_count(pg);
    CHECK(free_before == 0u);

    /* ---- 跑 TTL ---- */
    memset(&st, 0, sizeof(st));
    rc = ts_ttl_expire(pg, d, NOW_MS, TTL_SEC, &st);
    CHECK(rc == OK);
    CHECK(st.expired_parts == 2u);
    CHECK(st.freed_pages == 2u);
    CHECK(st.kept_parts == 2u);
    CHECK(st.already_dropped == 0u);

    /* 页确实还回去了（全局空闲计数 +2） */
    CHECK(pager_free_count(pg) == free_before + 2u);

    /* 回收前先清零：下一个主人拿到的是一张空页，不是别人的残留 */
    CHECK(page_is_zero(pg, page_old));
    CHECK(page_is_zero(pg, page_mid));

    /* P0：过期了 */
    CHECK(get_part(d, 0u, &p) == OK);
    CHECK(p.state == TS_PART_DROPPED);
    CHECK(p.first_page == PAGE_ID_NIL);
    CHECK(p.page_count == 0u);

    /* P1：过期了 */
    CHECK(get_part(d, 1u, &p) == OK);
    CHECK(p.state == TS_PART_DROPPED);
    CHECK(p.first_page == PAGE_ID_NIL);
    CHECK(p.page_count == 0u);

    /* P2：end == 阈值 ⇒ **不**过期，且原封不动 */
    CHECK(get_part(d, 2u, &p) == OK);
    CHECK(p.state == TS_PART_ACTIVE);
    CHECK(p.n_rows == 1u);
    CHECK(p.page_count == 1u);
    CHECK(p.first_page == page_keep);

    /* P3：没到期，原封不动 */
    CHECK(get_part(d, 3u, &p) == OK);
    CHECK(p.state == TS_PART_ACTIVE);
    CHECK(p.n_rows == 1u);
    CHECK(p.page_count == 1u);
    CHECK(p.first_page == page_now);

    /* 查那个时间窗 ⇒ E_NOTFOUND */
    {
        TsPartInfo tmp;

        CHECK(ts_dir_find(d, 1000, NULL, &tmp) == E_NOTFOUND);
        CHECK(ts_dir_find(d, 3601000, NULL, &tmp) == E_NOTFOUND);
        /* 没过期的时间窗照样查得到 */
        CHECK(ts_dir_find(d, 95000000, NULL, &tmp) == OK);
    }

    /* ---- 再跑一遍：幂等 ---- */
    memset(&st, 0, sizeof(st));
    rc = ts_ttl_expire(pg, d, NOW_MS, TTL_SEC, &st);
    CHECK(rc == OK);
    CHECK(st.expired_parts == 0u);
    CHECK(st.freed_pages == 0u);
    CHECK(st.already_dropped == 2u);
    CHECK(st.kept_parts == 2u);
    /* 空闲页数没有再涨（没有重复回收） */
    CHECK(pager_free_count(pg) == free_before + 2u);

    ts_dir_destroy(d);
    d = NULL;
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 2. 重开库看落盘 ---------------- */

static void t_reopen(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsPartInfo p;
    u32 page_old;
    u32 page_keep;
    u32 page_now;

    printf("  2. 重开库看到的是 DROPPED（说明目录真的落盘了）\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 3600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }

    page_old = make_part(pg, d, 0, 3600000, 1000);
    page_keep = make_part(pg, d, 93600000, 96400000, 95000000);
    page_now = make_part(pg, d, 97200000, 100800000, 100000000);
    (void)page_old;
    (void)page_keep;
    (void)page_now;

    CHECK(ts_ttl_expire(pg, d, NOW_MS, TTL_SEC, NULL) == OK);
    CHECK(pager_close(pg) == OK);

    /* 重开 */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_open(pg, &d) == OK);
    if (d != NULL) {
        CHECK(ts_dir_count(d) == 3u);
        CHECK(get_part(d, 0u, &p) == OK);
        CHECK(p.state == TS_PART_DROPPED);
        CHECK(p.first_page == PAGE_ID_NIL);
        CHECK(get_part(d, 1u, &p) == OK);
        CHECK(p.state == TS_PART_ACTIVE);
        CHECK(get_part(d, 2u, &p) == OK);
        CHECK(p.state == TS_PART_ACTIVE);
        ts_dir_destroy(d);
    }
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

/* ---------------- 4/6. 复用与新写入 ---------------- */

static void t_reuse_and_write(void)
{
    Pager *pg = NULL;
    TsDir *d = NULL;
    TsStore *st = NULL;
    TsPartInfo p;
    TsReading r;
    u32 page_old;
    u32 page_mid;
    u32 page_new;
    u32 free_after_ttl;
    int rc;

    printf("  4. 回收的页能被下一个分配页拿到\n");
    printf("  6. TTL 之后新写入正常\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_dir_create(pg, 3600u, &d) == OK);
    if (d == NULL) {
        (void)pager_close(pg);
        return;
    }

    page_old = make_part(pg, d, 0, 3600000, 1000);
    page_mid = make_part(pg, d, 3600000, 7200000, 3601000);
    make_part(pg, d, 97200000, 100800000, 100000000);

    CHECK(ts_ttl_expire(pg, d, NOW_MS, TTL_SEC, NULL) == OK);
    free_after_ttl = pager_free_count(pg);
    CHECK(free_after_ttl == 2u);

    /*
     * 页真的回到了全局池子：**同一个会话**里再分配页，
     * 拿到的就是刚回收的那两个之一。
     *
     * ⚠️ 必须同会话：空闲栈只在内存里（关库即丢，见坑 M6-52），
     * 重开库之后那些页就变成了"文件里的空洞" —— 不会被复用，也不会被回收。
     */
    {
        PageFrame *fr = NULL;

        CHECK(pager_alloc(pg, &page_new, &fr) == OK);
        CHECK(page_new == page_old || page_new == page_mid);
        if (fr != NULL)
            CHECK(pager_unpin(pg, page_new, true) == OK);
        /* 空闲页少了一个 */
        CHECK(pager_free_count(pg) == free_after_ttl - 1u);
    }

    /* 重开库：走的是"目录里已经是 DROPPED"的磁盘状态 */
    ts_dir_destroy(d);
    d = NULL;
    CHECK(pager_close(pg) == OK);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    rc = ts_store_open(pg, &st);
    CHECK(rc == OK);
    if (st == NULL) {
        (void)pager_close(pg);
        return;
    }

    /* 过期的时间窗查不到 */
    {
        TsPartInfo tmp;

        CHECK(ts_store_find_part(st, 1000, &tmp) == E_NOTFOUND);
    }

    /* 活着的那个分区里的行还在（过期的两个分区一个都不算） */
    CHECK(ts_store_row_count(st) == 1u);

    /* 新写入：当前时间附近 ⇒ 落到还活着的分区 */
    r.time = NOW_MS;
    r.tag = 1;
    r.value = 2.5;
    CHECK(ts_store_insert(st, &r) == OK);
    CHECK(ts_store_row_count(st) == 2u);

    /* 再写一条到**新的**时间窗 ⇒ 新开一个分区 ⇒ 正常 */
    r.time = 200000000LL;
    r.tag = 2;
    r.value = 3.5;
    CHECK(ts_store_insert(st, &r) == OK);
    CHECK(ts_store_row_count(st) == 3u);

    /* 新分区是时间序最后一个 */
    CHECK(ts_store_part_count(st) == 4u);
    CHECK(ts_store_part_info(st, 3u, &p) == OK);
    CHECK(p.start_ms == 198000000LL);
    CHECK(p.page_count == 1u);
    CHECK(p.n_rows == 1u);

    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    remove(PATH);
}

int main(void)
{
    printf("test_ts_ttl —— TTL 整分区过期回收\n");

    t_expire();
    t_reopen();
    t_reuse_and_write();

    if (failures == 0) {
        printf("  7 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
