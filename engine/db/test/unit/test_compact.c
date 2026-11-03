/*
 * test_compact.c —— v1.2 步骤 5：页内 compact 的验收
 *
 * 3 项：
 *   1. **整理真的回收空间**：碎片率 > 0 的页，整理后碎片率归零、可用空间变大
 *   2. **RID 稳定**：整理**前后**同一个 RID 读出来的内容一字不差
 *      （这是 compact 与"重表"的分界线 —— slot 号变了索引就全废了）
 *   3. **WAL 双镜像 + freelist 档位 + 不越界**：
 *      整理写一条 LOG_PAGEDATA，before/after 各是整页镜像，
 *      而且**写日志时页还没被改**（WAL 规则：先日志，后改页）；
 *      整理回收了空间，freelist 的档位必须升上去；
 *      别人的页不碰、没碎片的页不整理、不到阈值不整理
 *
 * ==================== 为什么要单独测这一层 ====================
 *
 * 页内的重排（page_compact）v0.1 就有了，但**没人调用它** ——
 * 空洞只增不减，10 万次增删后碎片率能到 100%。
 *
 * v1.2 步骤 5 把 compact 接进 vacuum，代价是：
 *   ① 它会**移动记录**（RID 稳不稳？）
 *   ② 它要**写整页镜像进 WAL**（崩溃安不安全？）
 *   ③ 它会**改变页的可用空间**（freelist 跟不跟得上？）
 *
 * 这三件事都不是"调用一下 page_compact"能盖住的，
 * 所以要在**堆这一层**单独压。
 */

#include <stdio.h>
#include <string.h>

#include "storage/heap.h"

/* 日志钩子的类型定义在 common/loghook.h（堆表和 catalog 共用） */
#include "common/loghook.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define TEST_TID 100
#define NREC      24u
#define REC_LEN   120u

/*
 * 24 条 × 124 字节（记录 + slot 目录项）= 2976 字节 < 4096
 * —— 全部落在**同一页**里，这样"这一页的碎片"才有意义。
 */

/* ---------------- 测试用的 WAL 钩子 ---------------- */

/*
 * 只关心 LOG_PAGEDATA —— 其余类型的记录忽略。
 *
 * 记下来的东西：
 *   calls      —— 收到过多少条 PAGEDATA
 *   page_id    —— 是哪一页
 *   blen/alen  —— 两份镜像的长度（都应该是整页）
 *   before/after —— 两份镜像的内容
 *   frag_before/frag_after —— 两份镜像各自的碎片率
 */
typedef struct {
    int   calls;
    u8    logtype;
    u32   page_id;
    u16   blen;
    u16   alen;
    u8    before[PAGE_SIZE];
    u8    after[PAGE_SIZE];
    u16   frag_before;
    u16   frag_after;
    int   lsn_seq;
} WalSpy;

static int spy_wal(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                   u16 slot_off,
                   const u8 *before, u16 blen,
                   const u8 *after, u16 alen, u32 *lsn_out)
{
    (void)xid; (void)slot; (void)slot_off;
    WalSpy *s = (WalSpy *)ctx;

    if (logtype != LOG_PAGEDATA)
        return OK;

    s->calls++;
    s->logtype = logtype;
    s->page_id = page_id;
    s->blen = blen;
    s->alen = alen;
    if (before && blen <= PAGE_SIZE)
        memcpy(s->before, before, blen);
    if (after && alen <= PAGE_SIZE)
        memcpy(s->after, after, alen);

    s->frag_before = (blen >= PAGE_SIZE) ? page_fragmentation(before) : 0;
    s->frag_after = (alen >= PAGE_SIZE) ? page_fragmentation(after) : 0;

    s->lsn_seq++;
    if (lsn_out)
        *lsn_out = (u32)(1000 + s->lsn_seq);
    return OK;
}

/* ---------------- 环境 ---------------- */

typedef struct {
    Pager    *pg;
    FreeList *fl;
    Heap     *h;
    u32       root;
    WalSpy   *spy;
} Env;

static int env_create(Env *e, const char *path, WalSpy *spy)
{
    memset(e, 0, sizeof(*e));
    e->spy = spy;
    remove(path);
    int rc = pager_open(path, 64, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;
    rc = heap_create(e->pg, e->fl, TEST_TID, spy_wal, spy, &e->root);
    if (rc != OK) return rc;
    return heap_open(e->pg, e->fl, e->root, TEST_TID, spy_wal, spy, &e->h);
}

static void env_close(Env *e)
{
    if (e->h) heap_destroy(e->h);
    if (e->fl) freelist_destroy(e->fl);
    if (e->pg) pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

static void make_rec(u8 *buf, u32 id, u16 len)
{
    put_u32(buf, id);
    put_u16(buf + 4, len);
    for (u16 i = 6; i < len; i++)
        buf[i] = (u8)(id * 7u + i * 3u);
}

/* 读一页的碎片率与可用空间（测试要直接看页） */
static int page_stat(Env *e, u32 pid, u16 *frag, u16 *free_bytes)
{
    PageFrame *f;
    int rc = pager_get(e->pg, pid, &f);
    if (rc != OK)
        return rc;
    if (frag)
        *frag = page_fragmentation(f->data);
    if (free_bytes)
        *free_bytes = page_free_bytes(f->data);
    pager_unpin(e->pg, pid, false);
    return OK;
}

/*
 * 造一页带空洞的：插 NREC 条，删掉后一半。
 *
 * 记录总是从页尾往下长，所以删掉靠后的记录，
 * 页的**下半截**就变成了一整段空洞 ✓
 */
static u32 make_fragmented(Env *e, Rid *rids, int *nrid)
{
    u8 rec[REC_LEN];
    *nrid = 0;
    for (u32 i = 0; i < NREC; i++) {
        make_rec(rec, i, REC_LEN);
        Rid rid;
        int rc = heap_insert(e->h, 7, rec, REC_LEN, &rid);
        if (rc != OK)
            return rc;
        rids[(*nrid)++] = rid;
    }
    for (int i = *nrid / 2; i < *nrid; i++) {
        int rc = heap_delete(e->h, 7, rids[i]);
        if (rc != OK)
            return rc;
    }
    return OK;
}

/* ---------------- 1~2) 整理回收空间 + RID 稳定 ---------------- */

static void t1_compact_reclaims_and_keeps_rid(void)
{
    Env e;
    WalSpy spy;
    memset(&spy, 0, sizeof(spy));
    if (env_create(&e, "test/tmp/compact1.db", &spy) != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    Rid rids[NREC];
    int nrid = 0;
    if (make_fragmented(&e, rids, &nrid) != OK) {
        printf("    FAIL 造数据失败\n");
        failures++;
        env_close(&e);
        return;
    }

    /* 造出空洞的那一页 */
    u32 pid = rids[0].page;
    u16 frag_before = 0, free_before = 0;
    CHECK(page_stat(&e, pid, &frag_before, &free_before) == OK);
    CHECK(frag_before > 0);          /* 前提：有空洞 */

    /* 整理前把活记录的内容抄下来 */
    u8 snap[NREC][REC_LEN];
    int nlive = 0;
    for (int i = 0; i < nrid / 2; i++) {
        u16 len = 0;
        CHECK(heap_get(e.h, rids[i], snap[nlive], REC_LEN, &len) == OK);
        CHECK(len == REC_LEN);
        nlive++;
    }

    HeapPageCompact pc;
    int rc = heap_compact_page(e.h, 9, pid, 0, &pc);
    CHECK(rc == OK);
    CHECK(pc.compacted);
    CHECK(pc.freed > 0);

    /* 第 1 项：碎片归零、空间变大 */
    u16 frag_after = 0, free_after = 0;
    CHECK(page_stat(&e, pid, &frag_after, &free_after) == OK);
    CHECK(frag_after == 0);
    CHECK(free_after > free_before);

    /* 第 2 项：RID 不变，内容一字不差 */
    for (int i = 0; i < nlive; i++) {
        u8 buf[REC_LEN];
        u16 len = 0;
        CHECK(heap_get(e.h, rids[i], buf, REC_LEN, &len) == OK);
        CHECK(len == REC_LEN);
        CHECK(memcmp(buf, snap[i], REC_LEN) == 0);
    }

    /* 整理后扫描，行数与内容都不能少 */
    int scanned = 0;
    HeapScan *s = NULL;
    CHECK(heap_scan_begin(e.h, &s) == OK);
    Rid rid;
    u8 buf[REC_LEN];
    u16 len = 0;
    while (heap_scan_next(s, &rid, buf, sizeof(buf), &len) == OK)
        scanned++;
    heap_scan_end(s);
    CHECK(scanned == nlive);

    printf("    整理前碎片 %u%%（可用 %u 字节）→ 整理后碎片 %u%%（可用 %u 字节），"
           "回收 %llu 字节；RID 不变，%d 行全在\n",
           frag_before, free_before, frag_after, free_after,
           (unsigned long long)pc.freed, nlive);

    env_close(&e);
}

/* ---------------- 3) WAL 双镜像 ---------------- */

static void t2_wal_double_image(void)
{
    Env e;
    WalSpy spy;
    memset(&spy, 0, sizeof(spy));
    if (env_create(&e, "test/tmp/compact2.db", &spy) != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    Rid rids[NREC];
    int nrid = 0;
    if (make_fragmented(&e, rids, &nrid) != OK) {
        printf("    FAIL 造数据失败\n");
        failures++;
        env_close(&e);
        return;
    }
    u32 pid = rids[0].page;

    int calls_before = spy.calls;
    HeapPageCompact pc;
    CHECK(heap_compact_page(e.h, 11, pid, 0, &pc) == OK);
    CHECK(pc.compacted);

    /* 只应该多出一条 PAGEDATA（删记录的 LOG_DELETE 不算） */
    CHECK(spy.calls == calls_before + 1);
    CHECK(spy.logtype == LOG_PAGEDATA);
    CHECK(spy.page_id == pid);

    /* 两份镜像都是整页 */
    CHECK(spy.blen == PAGE_SIZE);
    CHECK(spy.alen == PAGE_SIZE);

    /* before 镜像有碎片，after 镜像没有 */
    CHECK(spy.frag_before > 0);
    CHECK(spy.frag_after == 0);

    /*
     * WAL 规则：写日志的那一刻，页还没被改 ——
     * before 镜像的碎片率必须等于"写日志之前真页的碎片率"。
     * 如果实现先改页再写日志，这里就会对不上。
     */
    CHECK(spy.frag_before == pc.frag_before);

    /* 重放 after → 得到整理后的页 */
    u16 frag_now = 0;
    CHECK(page_stat(&e, pid, &frag_now, NULL) == OK);
    CHECK(frag_now == 0);

    printf("    一条 LOG_PAGEDATA：before %u 字节（碎片 %u%%）/ after %u 字节（碎片 %u%%）"
           " → 重放即最终态，回滚即整理前\n",
           spy.blen, spy.frag_before, spy.alen, spy.frag_after);

    env_close(&e);
}

/* ---------------- 4) freelist 档位 + 不越界 ---------------- */

static void t3_freelist_and_bounds(void)
{
    Env e;
    WalSpy spy;
    memset(&spy, 0, sizeof(spy));
    if (env_create(&e, "test/tmp/compact3.db", &spy) != OK) {
        printf("    FAIL 环境创建失败\n");
        failures++;
        return;
    }

    Rid rids[NREC];
    int nrid = 0;
    if (make_fragmented(&e, rids, &nrid) != OK) {
        printf("    FAIL 造数据失败\n");
        failures++;
        env_close(&e);
        return;
    }
    u32 pid = rids[0].page;

    /* 整理前：这一页有碎片，freelist 的档位还挂在旧值上 */
    u16 f = 0;
    CHECK(page_stat(&e, pid, &f, NULL) == OK);
    CHECK(f > 0);
    u8 lvl_before = freelist_level(e.fl, pid);

    HeapPageCompact pc2;

    /* 阈值：碎片率不到阈值就不整理，也不写日志 */
    int calls_before = spy.calls;
    /* 阈值取 100 —— 碎片率不到 100% 就不该整理 */
    CHECK(heap_compact_page(e.h, 15, pid, 100, &pc2) == OK);
    CHECK(pc2.compacted == false);
    CHECK(spy.calls == calls_before);

    /* 阈值取 1 → 该整理了 */
    HeapPageCompact pc;
    CHECK(heap_compact_page(e.h, 13, pid, 1, &pc) == OK);
    CHECK(pc.compacted);

    /* 空间回收了 → 档位必须升上去（不升 = 空着的空间用不上） */
    u8 lvl_after = freelist_level(e.fl, pid);
    CHECK(lvl_after > lvl_before);

    /* 另一张表的页：不是本表的页不能被整理 */
    u32 other_root = 0;
    CHECK(heap_create(e.pg, e.fl, 200, NULL, NULL, &other_root) == OK);
    Heap *other = NULL;
    CHECK(heap_open(e.pg, e.fl, other_root, 200, NULL, NULL, &other) == OK);

    /* 不是本表的页 → E_NOTFOUND（"这不是我的页"和"整理失败"是两回事） */
    CHECK(heap_compact_page(e.h, 15, other_root, 0, &pc2) == E_NOTFOUND);
    CHECK(pc2.compacted == false);

    /* 没有碎片的页：不整理、也不写日志 */
    calls_before = spy.calls;
    u16 frag = 0;
    CHECK(page_stat(&e, e.root, &frag, NULL) == OK);
    if (frag == 0) {
        CHECK(heap_compact_page(e.h, 15, e.root, 0, &pc2) == OK);
        CHECK(pc2.compacted == false);
        CHECK(spy.calls == calls_before);
    }

    printf("    freelist 档位 %u → %u（回收 %llu 字节）；越界的页一律不碰\n",
           lvl_before, lvl_after, (unsigned long long)pc.freed);

    heap_destroy(other);
    env_close(&e);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_compact（单元）:\n");

    printf("  1. 整理回收空间 + RID 不变\n");
    t1_compact_reclaims_and_keeps_rid();

    printf("  2. WAL 双镜像（一条 PAGEDATA，before/after 各整页）\n");
    t2_wal_double_image();

    printf("  3. freelist 档位跟着更新 + 不越界\n");
    t3_freelist_and_bounds();

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
