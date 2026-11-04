/*
 * test_ft_root.c —— 根页（双页交替）+ 段目录页的验收（v1.3 欠账 3 / 7）
 *
 * 本轮只做**内核件**，不接线：不碰 ft_tomb.c / ft_catalog.c，
 * 也不依赖 struct FtIndex —— 目录机制自己带一套状态结构。
 *
 * 5 项验收：
 *   1. 写第 1 版目录 → 读回**逐条一致**；
 *   2. 写第 2 版（段更多，还带 tombstone 页的位置）→ 读回第 2 版，
 *      且 generation 更大；
 *   3. **人为损坏较新的那份**（根页）→ 读**回退到旧版**，不报错、
 *      没有脏数据；重开库（模拟断电）后仍然回退；
 *   4. 空目录 → 0 条；
 *   5. 目录跨多页 → 链能读完，次序不乱。
 *
 * 另外顺带验两条硬要求：
 *   - **WAL 先行**：页上带的 LSN 必须就是 WAL 记录给的那个 LSN
 *     （说明日志先写、页后改，页头的 LSN 不是编出来的）；
 *   - **损坏后能自愈**：回退之后继续写目录，写到坏掉的那一张上，
 *     读回来又是新目录。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>

#include "common/error.h"
#include "common/loghook.h"
#include "common/types.h"
#include "modalities/ft/ft_root.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define DB_PATH "test/tmp/c5_ft_root.db"

#define XID 7u

/* ---------------- 假的 WAL 钩子 ---------------- */

/*
 * 记录每一次写页的日志调用，用来验"WAL 先行"。
 *
 * 【为什么能拿它证明 WAL 先行】
 *
 * ft_page_write 的顺序是：写日志 → 把日志给的 LSN 填进页头 → 算校验和。
 * 所以"页头 LSN == 日志记录里的 LSN"这件事，
 * 只有在**日志先写**时才可能成立
 * （反过来先改页再写日志，页头上的 LSN 会是上一条日志的）。
 */
typedef struct {
    u32 n;
    u32 page_id[512];
    u32 lsn[512];
    u32 counter;
    bool all_full_page;   /* 每条记录都是整页镜像（LOG_PAGEDATA） */
} FakeWal;

static int fake_wal(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                    u16 slot_off, const u8 *before, u16 before_len,
                    const u8 *after, u16 after_len, u32 *lsn_out)
{
    FakeWal *w = (FakeWal *)ctx;
    u32 lsn;

    (void)xid;
    (void)slot;
    (void)slot_off;
    (void)before;
    (void)before_len;
    (void)after;

    if (logtype != LOG_PAGEDATA || after_len != (u16)PAGE_SIZE)
        w->all_full_page = false;

    lsn = ++w->counter;
    if (w->n < 512) {
        w->page_id[w->n] = page_id;
        w->lsn[w->n] = lsn;
        w->n++;
    }
    *lsn_out = lsn;
    return OK;
}

/* 取某个页最后一次写日志的 LSN（0 = 没写过） */
static u32 fake_wal_last_lsn(const FakeWal *w, u32 page_id)
{
    u32 lsn = 0;

    for (u32 i = 0; i < w->n; i++) {
        if (w->page_id[i] == page_id)
            lsn = w->lsn[i];
    }
    return lsn;
}

/* ---------------- 工具 ---------------- */

/* 把一段页的内容直接改坏（模拟写了一半 / 位翻转） */
static void corrupt_page(Pager *pg, u32 pid, u8 new_type)
{
    PageFrame *fr = NULL;

    if (pager_get(pg, pid, &fr) != OK)
        return;
    fr->data[H_TYPE] = new_type;      /* 页类型不对 —— 校验和也跟着失效 */
    pager_unpin(pg, pid, true);
}

static int pager_open_fresh(Pager **out)
{
    remove(DB_PATH);
    return pager_open(DB_PATH, 64, out);
}

/* ---------------- 各项验收 ---------------- */

/* 1 + 2：写第 1 版、写第 2 版，读回一致且 generation 更大 */
static void test_write_read(FakeWal *wal, Pager *pg, FtRootState *st)
{
    FtRootSeg segs1[2];
    FtRootSeg segs2[5];
    FtRootTomb tombs[2];
    FtRootCatalog cat;
    int rc;

    printf("  1. 写第 1 版目录 → 读回一致\n");

    segs1[0].seg_id = 0;  segs1[0].head_page = 100; segs1[0].state = 0;
    segs1[1].seg_id = 1;  segs1[1].head_page = 101; segs1[1].state = 1;

    rc = ft_root_write(pg, fake_wal, wal, XID, st, segs1, 2, NULL, 0);
    CHECK(rc == OK);
    CHECK(st->generation == 2);          /* 建库是 1，写一版 +1 */

    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == 2);
    CHECK(cat.generation == 2);
    CHECK(cat.segs[0].seg_id == 0 && cat.segs[0].head_page == 100 && cat.segs[0].state == 0);
    CHECK(cat.segs[1].seg_id == 1 && cat.segs[1].head_page == 101 && cat.segs[1].state == 1);
    ft_root_catalog_free(&cat);

    printf("  2. 写第 2 版（段更多 + tombstone 页的位置）→ 读回第 2 版\n");

    for (u32 i = 0; i < 5; i++) {
        segs2[i].seg_id = 10 + i;
        segs2[i].head_page = 200 + i;
        segs2[i].state = 0;
    }
    tombs[0].page_id = 900; tombs[0].ordinal = 0;
    tombs[1].page_id = 901; tombs[1].ordinal = 1;

    rc = ft_root_write(pg, fake_wal, wal, XID, st, segs2, 5, tombs, 2);
    CHECK(rc == OK);
    CHECK(st->generation == 3);          /* 比第 1 版大 */

    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == 5);
    CHECK(cat.n_tombs == 2);
    CHECK(cat.generation == 3);
    CHECK(cat.generation > 2);
    for (u32 i = 0; i < 5; i++) {
        CHECK(cat.segs[i].seg_id == 10 + i);
        CHECK(cat.segs[i].head_page == 200 + i);
    }
    CHECK(cat.tombs[0].page_id == 900 && cat.tombs[0].ordinal == 0);
    CHECK(cat.tombs[1].page_id == 901 && cat.tombs[1].ordinal == 1);
    ft_root_catalog_free(&cat);
}

/* 3：损坏较新的根页 → 回退到旧版（不报错、无脏数据），且能自愈 */
static void test_corrupt_fallback(FakeWal *wal, Pager **pgp, FtRootState *st)
{
    Pager *pg = *pgp;
    FtRootCatalog cat;
    u32 bad_pid;
    u32 good_pid;
    int rc;

    printf("  3. 人为损坏较新的那份 → 回退到旧版\n");

    /*
     * 3a：先坏**目录页**（根页本身还好，只是它指的链读不通）——
     * 读的时候要能跳过这一代，退到上一代去。
     */
    CHECK(st->cat_head != PAGE_ID_NIL);
    corrupt_page(pg, st->cat_head, 0xEE);
    if (pager_flush_all(pg) != OK)
        CHECK(false);

    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == 2);              /* 旧版是 2 个段，不是新版的 5 个 */
    CHECK(cat.generation == 2);
    CHECK(cat.segs[0].seg_id == 0 && cat.segs[0].head_page == 100);
    CHECK(cat.segs[1].seg_id == 1 && cat.segs[1].head_page == 101);
    ft_root_catalog_free(&cat);

    /* 3b：再坏**根页**本身 → 仍然回退到旧版 */
    bad_pid = (st->cur_slot == 0) ? st->root_a : st->root_b;
    good_pid = (st->cur_slot == 0) ? st->root_b : st->root_a;

    corrupt_page(pg, bad_pid, 0xEE);
    if (pager_flush_all(pg) != OK)
        CHECK(false);

    /* 读：必须**不报错**，而且拿回旧的那一版（第 1 版：2 个段） */
    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == 2);
    CHECK(cat.generation == 2);
    CHECK(cat.segs[0].seg_id == 0 && cat.segs[0].head_page == 100);
    CHECK(cat.segs[1].seg_id == 1 && cat.segs[1].head_page == 101);
    ft_root_catalog_free(&cat);

    /* 定位当前根也要能落在旧的那张上 */
    rc = ft_root_load(pg, st);
    CHECK(rc == OK);
    CHECK(st->generation == 2);
    CHECK(st->cur_slot == (good_pid == st->root_a ? 0u : 1u));

    /* 模拟断电：关库重开，回退依然成立 */
    if (pager_close(pg) != OK)
        CHECK(false);
    *pgp = NULL;
    rc = pager_open(DB_PATH, 64, &pg);
    CHECK(rc == OK);
    *pgp = pg;

    {
        FtRootState st2;

        memset(&st2, 0, sizeof(st2));
        CHECK(ft_root_bind(&st2, st->root_a, st->root_b) == OK);
        rc = ft_root_read(pg, &st2, &cat);
        CHECK(rc == OK);
        CHECK(cat.n_segs == 2);
        CHECK(cat.generation == 2);
        ft_root_catalog_free(&cat);
    }

    /* 自愈：坏掉的那张正好是下一版要写的位置 → 写完就恢复 */
    {
        FtRootSeg segs[1];

        segs[0].seg_id = 42;
        segs[0].head_page = 777;
        segs[0].state = 0;

        rc = ft_root_write(pg, fake_wal, wal, XID, st, segs, 1, NULL, 0);
        CHECK(rc == OK);
        CHECK(st->generation == 3);

        rc = ft_root_read(pg, st, &cat);
        CHECK(rc == OK);
        CHECK(cat.n_segs == 1);
        CHECK(cat.generation == 3);
        CHECK(cat.segs[0].seg_id == 42 && cat.segs[0].head_page == 777);
        ft_root_catalog_free(&cat);
    }
}

/* 4：空目录 → 0 条 */
static void test_empty(Pager *pg, FtRootState *st, FakeWal *wal)
{
    FtRootCatalog cat;
    int rc;

    printf("  4. 空目录 → 0 条\n");

    rc = ft_root_write(pg, fake_wal, wal, XID, st, NULL, 0, NULL, 0);
    CHECK(rc == OK);
    CHECK(st->cat_pages == 0);
    CHECK(st->cat_head == PAGE_ID_NIL);

    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == 0);
    CHECK(cat.n_tombs == 0);
    CHECK(cat.segs == NULL);
    CHECK(cat.tombs == NULL);
    ft_root_catalog_free(&cat);
}

/* 5：目录跨多页 → 链能读完，次序不乱 */
static void test_multi_page(Pager *pg, FtRootState *st, FakeWal *wal)
{
    const u32 N = 1000;
    FtRootSeg *segs = NULL;
    FtRootTomb *tombs = NULL;
    FtRootCatalog cat;
    int rc;

    printf("  5. 目录跨多页 → 链能读完\n");

    segs = (FtRootSeg *)malloc(sizeof(FtRootSeg) * N);
    tombs = (FtRootTomb *)malloc(sizeof(FtRootTomb) * 200);
    CHECK(segs != NULL && tombs != NULL);
    if (segs == NULL || tombs == NULL)
        return;

    for (u32 i = 0; i < N; i++) {
        segs[i].seg_id = i;
        segs[i].head_page = 1000 + i;
        segs[i].state = (u32)(i % 3);
    }
    for (u32 i = 0; i < 200; i++) {
        tombs[i].page_id = 5000 + i;
        tombs[i].ordinal = i;
    }

    rc = ft_root_write(pg, fake_wal, wal, XID, st, segs, N, tombs, 200);
    CHECK(rc == OK);
    CHECK(st->cat_pages >= 3);            /* 1000 个段 + 200 个 tombstone → 至少 3 页 */

    rc = ft_root_read(pg, st, &cat);
    CHECK(rc == OK);
    CHECK(cat.n_segs == N);
    CHECK(cat.n_tombs == 200);
    CHECK(cat.n_cat_pages == st->cat_pages);
    for (u32 i = 0; i < N; i++) {
        if (cat.segs[i].seg_id != i || cat.segs[i].head_page != 1000 + i ||
            cat.segs[i].state != (u32)(i % 3)) {
            CHECK(false);
            break;
        }
    }
    for (u32 i = 0; i < 200; i++) {
        if (cat.tombs[i].page_id != 5000 + i || cat.tombs[i].ordinal != i) {
            CHECK(false);
            break;
        }
    }
    ft_root_catalog_free(&cat);

    free(segs);
    free(tombs);
}

/* 硬要求：WAL 先行 —— 页上的 LSN 就是 WAL 记录给的 LSN */
static void test_wal_first(Pager *pg, const FakeWal *wal)
{
    printf("  +. WAL 先行：页头 LSN == 日志记录的 LSN\n");

    CHECK(wal->n > 0);
    CHECK(wal->all_full_page);

    for (u32 i = 0; i < wal->n; i++) {
        u32 pid = wal->page_id[i];
        u32 lsn = fake_wal_last_lsn(wal, pid);
        PageFrame *fr = NULL;

        if (pager_get(pg, pid, &fr) != OK) {
            CHECK(false);
            continue;
        }
        CHECK(page_lsn(fr->data) == lsn);
        pager_unpin(pg, pid, false);
    }
}

/* ---------------- main ---------------- */

int main(void)
{
    FakeWal wal;
    Pager *pg = NULL;
    FtRootState st;
    int rc;

    memset(&wal, 0, sizeof(wal));
    wal.all_full_page = true;
    memset(&st, 0, sizeof(st));

    printf("=== 根页双页交替 + 段目录页 ===\n");

    rc = pager_open_fresh(&pg);
    if (rc != OK) {
        printf("    FAIL: pager_open 失败 rc=%d\n", rc);
        return 1;
    }

    /* 建库：分配两张固定的根页 */
    rc = ft_root_create(pg, fake_wal, &wal, XID, &st);
    CHECK(rc == OK);
    CHECK(st.root_a != PAGE_ID_NIL);
    CHECK(st.root_b != PAGE_ID_NIL);
    CHECK(st.root_a != st.root_b);
    CHECK(st.generation == 1);

    /* 初始目录是空的 */
    rc = ft_root_load(pg, &st);
    CHECK(rc == OK);
    CHECK(st.generation == 1);
    CHECK(st.cat_pages == 0);

    test_write_read(&wal, pg, &st);
    test_corrupt_fallback(&wal, &pg, &st);
    test_empty(pg, &st, &wal);
    test_multi_page(pg, &st, &wal);
    test_wal_first(pg, &wal);

    if (pg != NULL)
        pager_close(pg);
    remove(DB_PATH);

    if (failures == 0) {
        printf("=== 全部通过 ===\n");
        return 0;
    }
    printf("=== %d 处失败 ===\n", failures);
    return 1;
}
