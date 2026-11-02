/*
 * test_gstore.c — 定长槽页原语单元验收（v0.11 步骤 1）
 *
 * 7 项：
 *   1. 分配槽 → RID 有效，槽数按 rec_size 算术排布
 *   2. 写槽 / 读槽 → 内容逐字节相同
 *   3. 读一个不存在的槽 → E_NOTFOUND（不返回垃圾）
 *   4. 释放槽 → 再读返回 E_NOTFOUND；再分配拿到同一个槽（空闲链复用）
 *   5. 槽位算术：第 i 槽的偏移 = 页头 + i × rec_size（**纯算术，无查找**）
 *   6. 页满 → 自动开新页，两页的槽互不干扰
 *   7. 遍历：gstore_foreach 走遍所有在用槽，跳过已释放的
 *
 * 【第 5 项是核心】
 *
 * 定长槽页的全部价值在于「位置纯算术可得」。
 * 如果这条不成立，用 slotted page 反而更省空间 —— 那就没有理由做这一层。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "storage/gstore.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/gstore_test.db"

#define REC_SIZE 40u
#define NSLOTS   ((PAGE_SIZE - GS_HDR_SIZE) / REC_SIZE)

/* ---------------- WAL 钩子（统计整页镜像条数） ---------------- */

typedef struct {
    int n_pagedata;
} WalCount;

static int count_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                      u16 slot_off,
                      const u8 *before, u16 blen,
                      const u8 *after, u16 alen, u32 *lsn_out)
{
    (void)xid; (void)page_id; (void)slot; (void)slot_off;
    (void)before; (void)blen; (void)after; (void)alen;
    WalCount *c = (WalCount *)ctx;
    if (logtype == LOG_PAGEDATA)
        c->n_pagedata++;
    if (lsn_out)
        *lsn_out = (u32)(c->n_pagedata + 1);
    return OK;
}

/* ---------------- 遍历回调 ---------------- */

typedef struct {
    int  count;
    u64  sum;
} EachCtx;

static int each_cb(void *ctx, Rid rid, const u8 *rec)
{
    (void)rid;
    EachCtx *e = (EachCtx *)ctx;
    e->count++;
    /* 记录首 8 字节当载荷，统计和 */
    e->sum += get_u64(rec + 4);
    return OK;
}

/* ---------------- 各项验收 ---------------- */

static void test_alloc_and_rw(void)
{
    printf("  1) 分配槽 + 写读\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);
    CHECK(gstore_rec_size(gs) == REC_SIZE);
    CHECK(gstore_page_type(gs) == PT_GRAPH_NODE);

    Rid r;
    CHECK(gstore_alloc(gs, 0, &r) == OK);
    CHECK(r.page != PAGE_ID_NIL);
    CHECK(r.slot == 0);

    u8 buf[REC_SIZE];
    memset(buf, 0, REC_SIZE);
    put_u64(buf + 4, 12345);
    CHECK(gstore_put(gs, 0, r, buf, REC_SIZE) == OK);

    u8 out[REC_SIZE];
    u16 len = 0;
    CHECK(gstore_get(gs, r, out, REC_SIZE, &len) == OK);
    CHECK(len == REC_SIZE);
    CHECK(get_u64(out + 4) == 12345);

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_notfound(void)
{
    printf("  2) 不存在的槽 → E_NOTFOUND\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    u8 buf[REC_SIZE];
    u16 len = 0;
    /* 页号是垃圾 */
    CHECK(gstore_get(gs, rid_make(9999, 0), buf, REC_SIZE, &len) == E_NOTFOUND);

    Rid r;
    CHECK(gstore_alloc(gs, 0, &r) == OK);
    /* 槽号越界 */
    CHECK(gstore_get(gs, rid_make(r.page, 60000), buf, REC_SIZE, &len) == E_NOTFOUND);

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_free_and_reuse(void)
{
    printf("  3) 释放槽 + 空闲链复用\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    Rid a, b, c;
    CHECK(gstore_alloc(gs, 0, &a) == OK);
    CHECK(gstore_alloc(gs, 0, &b) == OK);
    CHECK(gstore_alloc(gs, 0, &c) == OK);
    CHECK(a.slot == 0 && b.slot == 1 && c.slot == 2);

    CHECK(gstore_free(gs, 0, b) == OK);

    u8 buf[REC_SIZE];
    u16 len = 0;
    CHECK(gstore_get(gs, b, buf, REC_SIZE, &len) == E_NOTFOUND);
    CHECK(gstore_slot_alive(gs, b) == false);
    CHECK(gstore_slot_alive(gs, a) == true);

    /* 重复释放同一个槽 → E_NOTFOUND（槽已经不在用了） */
    CHECK(gstore_free(gs, 0, b) == E_NOTFOUND);

    /* 释放后应复用同一个槽（空闲链头） */
    Rid d;
    CHECK(gstore_alloc(gs, 0, &d) == OK);
    CHECK(d.page == b.page);
    CHECK(d.slot == b.slot);
    /* 复用后槽又变成在用状态 */
    CHECK(gstore_slot_alive(gs, d) == true);

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_slot_arithmetic(void)
{
    printf("  4) 槽位算术（纯算术，无查找）\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    Rid r;
    CHECK(gstore_alloc(gs, 0, &r) == OK);

    /* 读整页，验证第 0 槽在偏移 GS_HDR_SIZE 处，第 1 槽在其后 rec_size 处 */
    u8 page[PAGE_SIZE];
    CHECK(gstore_read_page(gs, r.page, page, PAGE_SIZE) == OK);

    CHECK(page[GS_H_TYPE] == PT_GRAPH_NODE);
    CHECK(get_u16(page + GS_H_REC_SIZE) == REC_SIZE);
    CHECK(get_u16(page + GS_H_NSLOTS) == NSLOTS);

    /* 槽 0 的 in_use 在偏移 GS_HDR_SIZE */
    CHECK(page[GS_HDR_SIZE + GS_REC_IN_USE] == 1);
    /* 槽 1 的 in_use 在偏移 GS_HDR_SIZE + rec_size */
    CHECK(page[GS_HDR_SIZE + REC_SIZE + GS_REC_IN_USE] == 0);

    /* 页校验和落在页头内（16），不能压到槽区 —— 这是曾经踩过的坑 */
    CHECK(page_verify_checksum(page) == true);

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_page_full_newpage(void)
{
    printf("  5) 页满 → 自动开新页\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    Rid first, last;
    CHECK(gstore_alloc(gs, 0, &first) == OK);

    /* 分配超过一页的槽数 */
    for (u32 i = 1; i < NSLOTS + 5; i++) {
        Rid r;
        CHECK(gstore_alloc(gs, 0, &r) == OK);
        last = r;
    }

    CHECK(gstore_page_count(gs) >= 2);
    CHECK(last.page != first.page);
    CHECK(last.slot == 4);   /* 第二页的第 5 个槽 */

    /* 两页内容互不干扰 */
    u8 buf[REC_SIZE], out[REC_SIZE];
    u16 len = 0;
    memset(buf, 0, REC_SIZE);
    put_u64(buf + 4, 777);
    CHECK(gstore_put(gs, 0, last, buf, REC_SIZE) == OK);

    CHECK(gstore_get(gs, first, out, REC_SIZE, &len) == OK);
    CHECK(get_u64(out + 4) != 777);       /* 第一页没被改到 */

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_foreach(void)
{
    printf("  6) 遍历：走遍在用槽，跳过已释放的\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    GStore *gs = NULL;
    CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    Rid rids[10];
    for (int i = 0; i < 10; i++) {
        CHECK(gstore_alloc(gs, 0, &rids[i]) == OK);
        u8 buf[REC_SIZE];
        memset(buf, 0, REC_SIZE);
        put_u64(buf + 4, (u64)(i + 1));
        CHECK(gstore_put(gs, 0, rids[i], buf, REC_SIZE) == OK);
    }

    /* 释放 3 个 */
    CHECK(gstore_free(gs, 0, rids[2]) == OK);
    CHECK(gstore_free(gs, 0, rids[5]) == OK);
    CHECK(gstore_free(gs, 0, rids[8]) == OK);

    EachCtx ec = { 0, 0 };
    CHECK(gstore_foreach(gs, each_cb, &ec) == OK);
    CHECK(ec.count == 7);

    /* 和 = 1+2+...+10 去掉 3、6、9 = 55 - 18 = 37 */
    CHECK(ec.sum == 37);
    CHECK(gstore_alive_count(gs) == 7);

    gstore_destroy(gs);
    pager_close(pg);
}

static void test_wal(void)
{
    printf("  7) WAL：写槽产生整页镜像日志\n");
    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    WalCount wc = { 0 };
    GStore *gs = NULL;
    CHECK(gstore_create(pg, count_hook, &wc, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

    Rid r;
    CHECK(gstore_alloc(gs, 0, &r) == OK);
    int after_alloc = wc.n_pagedata;
    CHECK(after_alloc > 0);

    u8 buf[REC_SIZE];
    memset(buf, 0, REC_SIZE);
    CHECK(gstore_put(gs, 0, r, buf, REC_SIZE) == OK);
    CHECK(wc.n_pagedata > after_alloc);

    gstore_destroy(gs);
    pager_close(pg);
}

/*
 * 8) 重启接管
 *
 * 【这一项在验证什么】
 *
 * gstore_create 只在内存里记 head/tail/npages/alive。
 * 重启后这些全没了 —— 但页还在磁盘上。
 *
 * 如果不管，重启后 gstore 认为自己有 0 页 → 遍历看不到任何记录 →
 * 整张图"消失"。这是静默的数据丢失，比报错更糟。
 *
 * 解法：链头页带 GS_FLAG_DIR 标志 → gstore_recover 扫全文件找链头，
 * 顺 next 重建 head/tail/npages/alive。
 *
 * 这一项模拟"关库再开库"，验证：
 *   - 重启后 alive 数对
 *   - 重启后页数对
 *   - 重启后能读到之前写的数据
 *   - 重启后能继续分配（新槽不与旧槽冲突）
 */
#define RPATH "test/tmp/gstore_recover.db"

static void test_recover(void)
{
    printf("  8) 重启接管（链头标志 + 扫全文件重建）\n");

    remove(RPATH);

    Rid saved[5];
    u32 pages_before = 0;

    /* --- 第一段：写 --- */
    {
        Pager *pg = NULL;
        CHECK(pager_open(RPATH, 64, &pg) == OK);

        GStore *gs = NULL;
        CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

        for (int i = 0; i < 5; i++) {
            CHECK(gstore_alloc(gs, 0, &saved[i]) == OK);
            u8 buf[REC_SIZE];
            memset(buf, 0, REC_SIZE);
            put_u64(buf + 4, (u64)(100 + i));
            CHECK(gstore_put(gs, 0, saved[i], buf, REC_SIZE) == OK);
        }
        /* 多占一页，验证多页链也能重建 */
        for (u32 i = 5; i < NSLOTS + 3; i++) {
            Rid r;
            CHECK(gstore_alloc(gs, 0, &r) == OK);
        }
        CHECK(gstore_alive_count(gs) == 5 + (NSLOTS + 3 - 5));
        pages_before = gstore_page_count(gs);

        gstore_destroy(gs);
        pager_close(pg);   /* 正常关闭 → 刷盘 */
    }

    /* --- 第二段：重开，接管 --- */
    {
        Pager *pg = NULL;
        CHECK(pager_open(RPATH, 64, &pg) == OK);

        GStore *gs = NULL;
        CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);

        /* 新建的 gstore 什么都不知道 */
        CHECK(gstore_alive_count(gs) == 0);
        CHECK(gstore_page_count(gs) == 0);

        /* 接管 */
        CHECK(gstore_recover(gs) == OK);

        CHECK(gstore_page_count(gs) == pages_before);
        CHECK(gstore_alive_count(gs) == 5 + (NSLOTS + 3 - 5));
        CHECK(gstore_head_page(gs) != PAGE_ID_NIL);

        /* 之前写的数据还在 */
        u8 out[REC_SIZE];
        u16 len = 0;
        for (int i = 0; i < 5; i++) {
            CHECK(gstore_get(gs, saved[i], out, REC_SIZE, &len) == OK);
            CHECK(get_u64(out + 4) == (u64)(100 + i));
        }

        /* 接管后还能继续分配（新槽不与旧槽冲突） */
        Rid nr;
        CHECK(gstore_alloc(gs, 0, &nr) == OK);
        CHECK(gstore_slot_alive(gs, nr) == true);
        CHECK(gstore_alive_count(gs) == 5 + (NSLOTS + 3 - 5) + 1);

        gstore_destroy(gs);
        pager_close(pg);
    }

    /* --- 第三段：空库接管不是错误 --- */
    {
        remove("test/tmp/gstore_empty.db");
        Pager *pg = NULL;
        CHECK(pager_open("test/tmp/gstore_empty.db", 64, &pg) == OK);

        GStore *gs = NULL;
        CHECK(gstore_create(pg, NULL, NULL, REC_SIZE, PT_GRAPH_NODE, &gs) == OK);
        CHECK(gstore_recover(gs) == OK);
        CHECK(gstore_page_count(gs) == 0);
        CHECK(gstore_alive_count(gs) == 0);
        CHECK(gstore_head_page(gs) == PAGE_ID_NIL);

        gstore_destroy(gs);
        pager_close(pg);
    }
}

int main(void)
{
    printf("test_gstore:\n");

    remove(PATH);
    remove("test/tmp/gstore_test.db-wal");

    test_alloc_and_rw();
    test_notfound();
    test_free_and_reuse();
    test_slot_arithmetic();
    test_page_full_newpage();
    test_foreach();
    test_wal();
    test_recover();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  8 项验收全部通过\n");
    return 0;
}
