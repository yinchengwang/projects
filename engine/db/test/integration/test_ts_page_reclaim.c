/*
 * test_ts_page_reclaim.c —— 封窗旧裸写页真回收（W-D 的 D-6.2）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * 封窗（`ts_seal_write_wal`）把裸写页链换成列式页之后，那批裸写页就没人引用了。
 * 改之前它们只是"标记不再使用"（`TSC_OLDFIRST` / `TSC_OLDPAGES` 记在
 * 列式头部页里），**本轮不真回收** —— 坑 M6-42 如实记账：
 * 文件只涨不缩。
 *
 * 而 TTL 也收不了它们：TTL 走 `release_column_part()`，只回收
 * "目录项指着的那些页"（对封窗分区来说就是列式页），永远碰不到旧裸写页。
 * ⇒ 那批页在文件里是**永久空洞**。
 *
 * D-6.2 就是把它们收进全局空闲栈（`pager_free()`）。
 *
 * ==================== 判据为什么是这五条 ====================
 *
 * 1. **页真的还回去了**：空闲栈计数必须**精确**增加"封窗前裸写页数"那么多。
 *    只断言"变多了"抓不住"多收/少收"。
 * 2. **幂等**：再调一次回收 ⇒ 空闲栈计数**一页都不再涨**。
 *    重复回收会把同一页发给两个主人 ⇒ 数据互相踩，而且不报错。
 * 3. **标记落盘**：关库重开之后，头部页里的 `OLDFIRST` 必须是 `PAGE_ID_NIL`。
 *    只在内存里清 ⇒ 重开后又看到它指着一批已属于别人的页 ⇒ 下次回收
 *    会把别人的页清零（静默损坏）。
 * 4. **复用前清零**：把回收的页分配出来，内容区必须全零。
 *    （`pager_alloc` 会 memset 整页 —— 这里验证这条对复用页也成立。）
 * 5. **数据不受影响**：回收之后，封窗分区的行数与内容都还读得对。
 *    这条是"回收没有踩到自己的列式页"的证据。
 *
 * ==================== 为什么不用 SQL / TsStore 层 ====================
 *
 * 本测试直接操作 Pager + TsStore + ts_ttl_release_seal_old_wal：
 * 要精确控制"封窗前有几页裸写页"，走 SQL 就拿不到这个数字了。
 * 且 `TS_SEAL_ROW_THRESHOLD` 是 10000 行 —— 造 10000 行再封窗
 * 会让测试变慢；这里直接用 `ts_column_write` 手工造一个"看起来像封过窗"
 * 的头部页，把关注点收在回收逻辑本身。
 * 自动封窗那条链由 test_ts_seal.c 覆盖。
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

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ts_page_reclaim.db"

/* 造一条裸写页链：n_pages 页，每页 1 行；返回首页，*pages_out 是页数 */
static u32 make_raw_chain(Pager *pg, u32 n_pages, i64 t0, u32 *pages_out)
{
    u32 first = PAGE_ID_NIL;
    u32 prev = PAGE_ID_NIL;
    u32 i;

    for (i = 0; i < n_pages; i++) {
        PageFrame *fr = NULL;
        u32 pid = 0;
        u8 *row;
        u64 bits;
        double v = 1.5;

        CHECK(pager_alloc(pg, &pid, &fr) == OK);
        if (fr == NULL)
            return PAGE_ID_NIL;

        page_init(fr->data, (u8)PT_TS_PART);
        put_u32(fr->data + TSSP_NROWS, 1);
        put_u32(fr->data + TSSP_NEXT, PAGE_ID_NIL);
        row = fr->data + TSSP_DATA;
        put_u64(row + 0, (u64)(t0 + (i64)i));
        put_u64(row + 8, (u64)7);
        memcpy(&bits, &v, sizeof(bits));
        put_u64(row + 16, bits);
        page_set_checksum(fr->data);

        if (prev != PAGE_ID_NIL) {
            /* 把上一页的 NEXT 指过来 —— 这里直接重取上一页改它 */
            PageFrame *pf = NULL;

            CHECK(pager_get(pg, prev, &pf) == OK);
            if (pf != NULL) {
                put_u32(pf->data + TSSP_NEXT, pid);
                page_set_checksum(pf->data);
                CHECK(pager_unpin(pg, prev, true) == OK);
            }
        } else {
            first = pid;
        }
        CHECK(pager_unpin(pg, pid, true) == OK);
        prev = pid;
    }

    *pages_out = n_pages;
    return first;
}

/* 读列式头部页里的"旧裸写页指针"（返回 first；pages 从 *pages_out 拿） */
static u32 read_old_ptr(Pager *pg, u32 col_head, u32 *pages_out)
{
    PageFrame *fr = NULL;
    u32 first;

    if (pager_get(pg, col_head, &fr) != OK)
        return PAGE_ID_NIL;

    first = get_u32(fr->data + TSC_OLDFIRST);
    *pages_out = get_u32(fr->data + TSC_OLDPAGES);
    (void)pager_unpin(pg, col_head, false);
    return first;
}

/* 一页的内容区是否全零（页头会被 LSN / 校验和写过，不能查整页） */
static int page_body_is_zero(Pager *pg, u32 pid)
{
    PageFrame *fr = NULL;
    int i;
    int zero = 1;

    if (pager_get(pg, pid, &fr) != OK)
        return 0;

    for (i = (int)TSSP_DATA; i < (int)PAGE_SIZE; i++) {
        if (fr->data[i] != 0) {
            zero = 0;
            break;
        }
    }
    (void)pager_unpin(pg, pid, false);
    return zero;
}

int main(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    u32 raw_first;
    u32 raw_pages = 0;
    u32 head = 0;
    u32 col_pages = 0;
    u32 freed = 0;
    u32 free_before;
    u32 got_page = 0;
    u32 old_first = 0;
    u32 old_pages = 0;
    TsDir *dir = NULL;
    TsPartInfo info;

    printf("test_ts_page_reclaim:\n");

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_create(pg, 60, &st) == OK);

    /*
     * ---- 0. 造一个"封过窗"的分区 ----
     *
     * 先造 3 页裸写页链（那批页就是"封窗时被换掉的"），
     * 再用 ts_column_write 写列式页并把 old_first / old_pages 记进头部页
     * —— 与 ts_seal_write_wal 做的事完全一致，只是手工造、规模小。
     */
    raw_first = make_raw_chain(pg, 3u, 1000000LL, &raw_pages);
    CHECK(raw_first != PAGE_ID_NIL);
    CHECK(raw_pages == 3u);

    {
        i64 ts[2] = { 1000000LL, 1000001LL };
        i64 tag[2] = { 7, 7 };
        double val[2] = { 1.5, 2.5 };

        CHECK(ts_column_write(pg, ts, tag, val, 2u, raw_first, raw_pages, 1u,
                              &head, &col_pages) == OK);
    }
    CHECK(head != 0u);

    /* 手工登记这个分区（模拟封窗第二步：目录项 = SEALED，指向列式头部页） */
    memset(&info, 0, sizeof(info));
    info.start_ms = 1000000LL;
    info.end_ms = 1000000LL + 60LL * 1000LL;
    info.state = TS_PART_SEALED;
    info.n_rows = 2u;
    info.first_page = head;
    info.page_count = col_pages;
    dir = (TsDir *)ts_store_dir(st);
    CHECK(ts_dir_append(dir, &info) == OK);

    /* 头部页里确实记着旧裸写页 */
    old_first = read_old_ptr(pg, head, &old_pages);
    CHECK(old_first == raw_first);
    CHECK(old_pages == raw_pages);

    /*
     * ---- 1. 回收：空闲栈必须**精确**多出 raw_pages 页 ----
     */
    free_before = pager_free_count(pg);
    CHECK(ts_ttl_release_seal_old(pg, head, &freed) == OK);
    CHECK(freed == raw_pages);
    CHECK(pager_free_count(pg) == free_before + raw_pages);
    printf("  [1] 回收旧裸写页：还回 %u 页，空闲栈 %u → %u\n",
           freed, free_before, pager_free_count(pg));

    /*
     * ---- 2. 幂等：再收一次，一页都不许多 ----
     */
    {
        u32 free_mid = pager_free_count(pg);
        u32 freed2 = 0;

        CHECK(ts_ttl_release_seal_old(pg, head, &freed2) == OK);
        CHECK(freed2 == 0u);
        CHECK(pager_free_count(pg) == free_mid);
        printf("  [2] 幂等：再收一次还回 %u 页（空闲栈停在 %u）\n",
               freed2, pager_free_count(pg));
    }

    /* 头部页里的标记必须已经清掉 */
    CHECK(read_old_ptr(pg, head, &old_pages) == PAGE_ID_NIL);
    CHECK(old_pages == 0u);

    /*
     * ---- 3. 数据不受影响：封窗分区还读得对 ----
     *
     * ⚠️ 这里用 `ts_column_read` 直读列式页，**不**用 ts_store_scan：
     * 本测试是手工登记目录项的（绕过了 TsStore 的运行时账本），
     * 而 ts_store_scan 会去读 `st->rt[k]`（那本账）⇒ 会踩到空指针。
     * 直读列式页正好把关注点收在"回收有没有踩坏列式页"上。
     */
    {
        i64 rts[2];
        i64 rtag[2];
        double rval[2];

        CHECK(ts_column_read(pg, head, rts, rtag, rval, 2u) == OK);
        CHECK(rts[0] == 1000000LL && rts[1] == 1000001LL);
        CHECK(rtag[0] == 7 && rtag[1] == 7);
        CHECK(rval[0] == 1.5 && rval[1] == 2.5);
        printf("  [3] 回收之后读列式页：2 行、内容与写入时全等\n");
    }

    /*
     * ---- 4. 复用前清零：从空闲栈分配一页，内容区必须全零 ----
     *
     * pager_alloc 会 memset 整页 ⇒ 这条验证"复用页交出去时是干净的"。
     */
    {
        PageFrame *fr = NULL;

        CHECK(pager_alloc(pg, &got_page, &fr) == OK);
        CHECK(fr != NULL);
        CHECK(got_page >= raw_first && got_page < raw_first + raw_pages);
        CHECK(page_body_is_zero(pg, got_page) == 1);
        CHECK(pager_unpin(pg, got_page, false) == OK);
        printf("  [4] 复用回收页 %u：内容区全零\n", got_page);
    }

    /*
     * ---- 5. 标记落盘：关库重开之后 OLDFIRST 仍是 NIL ----
     */
    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);

    pg = NULL;
    st = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(ts_store_open(pg, &st) == OK);

    {
        TsDir *nd = (TsDir *)ts_store_dir(st);
        TsPartInfo pi;

        CHECK(ts_dir_get(nd, 0, &pi) == OK);
        CHECK(pi.state == TS_PART_SEALED);
        CHECK(read_old_ptr(pg, pi.first_page, &old_pages) == PAGE_ID_NIL);
        CHECK(old_pages == 0u);
        printf("  [5] 重开库：OLDFIRST 仍是 NIL（标记落盘了）\n");
    }

    ts_store_destroy(st);
    CHECK(pager_close(pg) == OK);
    remove(PATH);

    if (failures == 0) {
        printf("  5 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
