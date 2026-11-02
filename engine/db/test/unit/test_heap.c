/*
 * test_heap.c — 堆表 AM 验收
 *
 * 9 项：插入/跨页/删除/缩小更新/变大更新/扫描/超页拒绝/WAL 钩子/重启。
 */

#include <stdio.h>
#include <string.h>

#include "storage/heap.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define BUF 512

typedef struct {
    Pager    *pg;
    FreeList *fl;
    Heap     *h;
    u32       root;
    u16       tid;
} Env;

/* 测试用的 table_id，固定 100 以免和系统表混淆 */
#define TEST_TID 100

static int env_create(Env *e, const char *path, u32 nframes)
{
    memset(e, 0, sizeof(*e));
    remove(path);
    e->tid = TEST_TID;
    int rc = pager_open(path, nframes, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;
    rc = heap_create(e->pg, e->fl, e->tid, NULL, NULL, &e->root);
    if (rc != OK) return rc;
    return heap_open(e->pg, e->fl, e->root, e->tid, NULL, NULL, &e->h);
}

static int env_reopen(Env *e, const char *path, u32 nframes)
{
    u32 root = e->root;
    u16 tid = e->tid;
    if (e->h) heap_destroy(e->h);
    if (e->fl) freelist_destroy(e->fl);
    if (e->pg) pager_close(e->pg);

    memset(e, 0, sizeof(*e));
    e->root = root;
    e->tid = tid;
    int rc = pager_open(path, nframes, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;
    return heap_open(e->pg, e->fl, root, tid, NULL, NULL, &e->h);
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
        buf[i] = (u8)(id * 13u + i);
}

static bool rec_ok(const u8 *buf, u16 len, u32 id)
{
    if (len < 6 || get_u32(buf) != id || get_u16(buf + 4) != len)
        return false;
    for (u16 i = 6; i < len; i++)
        if (buf[i] != (u8)(id * 13u + i))
            return false;
    return true;
}

/* ---- WAL 钩子记账 ---- */
typedef struct {
    u32 ins, del, upd, pageinit, total;
} WalStats;

static int wal_count(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                     u16 slot_off,
                     const u8 *before, u16 blen,
                     const u8 *after, u16 alen, u32 *lsn_out)
{
    (void)xid;
    (void)page_id; (void)slot; (void)slot_off;
    (void)before; (void)blen; (void)after; (void)alen;
    *lsn_out = 0;
    WalStats *s = (WalStats *)ctx;
    s->total++;
    switch (logtype) {
    case LOG_INSERT:   s->ins++; break;
    case LOG_DELETE:   s->del++; break;
    case LOG_UPDATE:   s->upd++; break;
    case LOG_PAGEINIT: s->pageinit++; break;
    default: break;
    }
    return OK;
}

/* ---- 1) 基本插入与读取 ---- */
static void t1_basic(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rid;

    CHECK(env_create(&e, "test/tmp/heap1.db", 16) == OK);

    for (u32 id = 0; id < 100; id++) {
        u16 n = (u16)(16 + (id * 7) % 100);
        make_rec(rec, id, n);
        CHECK(heap_insert(e.h, 0, rec, n, &rid) == OK);
        CHECK(!rid_is_nil(rid));

        CHECK(heap_get(e.h, rid, out, sizeof out, &len) == OK);
        CHECK(rec_ok(out, len, id));
    }
    env_close(&e);
}

/* ---- 2) 跨页：确认页链真的在长，且每行都能读回 ---- */
static void t2_cross_page(void)
{
    static Rid rids[2000];
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;

    CHECK(env_create(&e, "test/tmp/heap2.db", 32) == OK);
    CHECK(heap_npages(e.h) == 1);

    for (u32 id = 0; id < 2000; id++) {
        u16 n = (u16)(100 + (id % 100));
        make_rec(rec, id, n);
        CHECK(heap_insert(e.h, 0, rec, n, &rids[id]) == OK);
        if (failures > 3)
            break;
    }
    CHECK(heap_npages(e.h) > 1);

    for (u32 id = 0; id < 2000; id++) {
        CHECK(heap_get(e.h, rids[id], out, sizeof out, &len) == OK);
        CHECK(rec_ok(out, len, id));
        if (failures > 3)
            break;
    }
    printf("    （2000 行 / %u 页）\n", heap_npages(e.h));
    env_close(&e);
}

/* ---- 3) 删除 ---- */
static void t3_delete(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rids[50];

    CHECK(env_create(&e, "test/tmp/heap3.db", 16) == OK);

    for (u32 i = 0; i < 50; i++) {
        make_rec(rec, i, 60);
        CHECK(heap_insert(e.h, 0, rec, 60, &rids[i]) == OK);
    }
    for (u32 i = 0; i < 50; i += 5)
        CHECK(heap_delete(e.h, 0, rids[i]) == OK);

    for (u32 i = 0; i < 50; i++) {
        int rc = heap_get(e.h, rids[i], out, sizeof out, &len);
        if (i % 5 == 0) {
            CHECK(rc == E_NOTFOUND);
        } else {
            CHECK(rc == OK);
            CHECK(rec_ok(out, len, i));
        }
    }
    /* 重复删除 */
    CHECK(heap_delete(e.h, 0, rids[0]) == E_NOTFOUND);
    env_close(&e);
}

/* ---- 4) 更新（缩小）：RID 不变 ---- */
static void t4_update_shrink(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rid, nrid;

    CHECK(env_create(&e, "test/tmp/heap4.db", 16) == OK);

    make_rec(rec, 1, 200);
    CHECK(heap_insert(e.h, 0, rec, 200, &rid) == OK);

    make_rec(rec, 2, 80);
    CHECK(heap_update(e.h, 0, rid, rec, 80, &nrid) == OK);
    CHECK(nrid.page == rid.page && nrid.slot == rid.slot);   /* RID 不变 */

    CHECK(heap_get(e.h, rid, out, sizeof out, &len) == OK);
    CHECK(rec_ok(out, len, 2));
    CHECK(len == 80);
    env_close(&e);
}

/* ---- 5) 更新（变大）：RID 变，旧 RID 失效 ---- */
static void t5_update_grow(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rid, nrid;

    CHECK(env_create(&e, "test/tmp/heap5.db", 16) == OK);

    make_rec(rec, 3, 60);
    CHECK(heap_insert(e.h, 0, rec, 60, &rid) == OK);

    make_rec(rec, 4, 300);
    CHECK(heap_update(e.h, 0, rid, rec, 300, &nrid) == OK);

    /* 旧 RID 必须失效 */
    CHECK(heap_get(e.h, rid, out, sizeof out, &len) == E_NOTFOUND);
    /* 新 RID 可读且内容正确 */
    CHECK(heap_get(e.h, nrid, out, sizeof out, &len) == OK);
    CHECK(rec_ok(out, len, 4));
    env_close(&e);
}

/* ---- 6) 扫描：行数正确、集合一致 ---- */
static void t6_scan(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rids[200];
    bool seen[200];
    HeapScan *s;
    u32 n = 0;

    memset(seen, 0, sizeof seen);
    CHECK(env_create(&e, "test/tmp/heap6.db", 32) == OK);

    for (u32 i = 0; i < 200; i++) {
        u16 l = (u16)(40 + (i % 80));
        make_rec(rec, i, l);
        CHECK(heap_insert(e.h, 0, rec, l, &rids[i]) == OK);
    }
    for (u32 i = 0; i < 200; i += 4)
        CHECK(heap_delete(e.h, 0, rids[i]) == OK);

    CHECK(heap_scan_begin(e.h, &s) == OK);
    for (;;) {
        Rid rid;
        int rc = heap_scan_next(s, &rid, out, sizeof out, &len);
        if (rc == E_EOF)
            break;
        CHECK(rc == OK);
        if (rc != OK)
            break;
        u32 id = get_u32(out);
        CHECK(id < 200);
        CHECK(!seen[id]);              /* 不能重复吐同一行 */
        seen[id] = true;
        n++;
        if (failures > 5)
            break;
    }
    heap_scan_end(s);

    CHECK(n == 150);                   /* 200 - 50 */
    for (u32 i = 0; i < 200; i++)
        CHECK(seen[i] == (i % 4 != 0));
    env_close(&e);
}

/* ---- 7) 超页行拒绝 ---- */
static void t7_oversize(void)
{
    Env e;
    static u8 rec[PAGE_SIZE];
    Rid rid;

    CHECK(env_create(&e, "test/tmp/heap7.db", 16) == OK);

    /* 4072 + 4(slot) == PAGE_SIZE - HDR，刚好放得下 */
    memset(rec, 0x5A, sizeof rec);
    CHECK(heap_insert(e.h, 0, rec, (u16)(PAGE_SIZE - HDR_SIZE - SLOT_SIZE), &rid) == OK);

    /* 再多 1 字节就不行 */
    CHECK(heap_insert(e.h, 0, rec, (u16)(PAGE_SIZE - HDR_SIZE - SLOT_SIZE + 1), &rid) == E_NOSPACE);
    env_close(&e);
}

/* ---- 8) WAL 钩子覆盖三条路径 ---- */
static void t8_wal(void)
{
    Env e;
    u8 rec[BUF];
    Rid rids[10];
    WalStats st;
    Rid nrid;

    memset(&st, 0, sizeof st);
    CHECK(env_create(&e, "test/tmp/heap8.db", 16) == OK);
    heap_set_wal(e.h, wal_count, &st);

    for (u32 i = 0; i < 10; i++) {
        make_rec(rec, i, 80);
        CHECK(heap_insert(e.h, 0, rec, 80, &rids[i]) == OK);
    }
    CHECK(st.ins == 10);

    /* 缩小更新：一次 upd */
    make_rec(rec, 100, 40);
    CHECK(heap_update(e.h, 0, rids[0], rec, 40, &nrid) == OK);
    CHECK(st.upd == 1);

    /* 变大更新：一次 del + 一次 ins */
    make_rec(rec, 101, 200);
    CHECK(heap_update(e.h, 0, rids[1], rec, 200, &nrid) == OK);
    CHECK(st.del == 1);
    CHECK(st.ins == 11);

    /* 删除 5 行 */
    for (u32 i = 2; i < 7; i++)
        CHECK(heap_delete(e.h, 0, rids[i]) == OK);
    CHECK(st.del == 6);

    CHECK(st.total == 10 + 1 + 2 + 5);
    CHECK(heap_wal_calls(e.h) == st.total);
    env_close(&e);
}

/* ---- 9) 重启 ---- */
static void t9_reopen(void)
{
    Env e;
    u8 rec[BUF], out[BUF];
    u16 len;
    Rid rids[500];
    u32 n = 0;
    HeapScan *s;

    CHECK(env_create(&e, "test/tmp/heap9.db", 64) == OK);
    for (u32 i = 0; i < 500; i++) {
        u16 l = (u16)(50 + (i % 120));
        make_rec(rec, i, l);
        CHECK(heap_insert(e.h, 0, rec, l, &rids[i]) == OK);
    }
    for (u32 i = 0; i < 500; i += 3)
        CHECK(heap_delete(e.h, 0, rids[i]) == OK);

    CHECK(env_reopen(&e, "test/tmp/heap9.db", 16) == OK);   /* 小池子，强制逐出 */

    for (u32 i = 0; i < 500; i++) {
        int rc = heap_get(e.h, rids[i], out, sizeof out, &len);
        if (i % 3 == 0) {
            CHECK(rc == E_NOTFOUND);
        } else {
            CHECK(rc == OK);
            CHECK(rec_ok(out, len, i));
        }
        if (failures > 5)
            break;
    }

    CHECK(heap_scan_begin(e.h, &s) == OK);
    for (;;) {
        Rid rid;
        int rc = heap_scan_next(s, &rid, out, sizeof out, &len);
        if (rc == E_EOF) break;
        CHECK(rc == OK);
        if (rc != OK) break;
        n++;
    }
    heap_scan_end(s);
    CHECK(n == 500 - 167);   /* 删掉 i%3==0 的 167 行 */

    printf("    （重启后 %u 页 / 存活 %u 行）\n", heap_npages(e.h), n);
    env_close(&e);
}

int main(void)
{
    printf("test_heap:\n");
    t1_basic();
    t2_cross_page();
    t3_delete();
    t4_update_shrink();
    t5_update_grow();
    t6_scan();
    t7_oversize();
    t8_wal();
    t9_reopen();

    if (failures == 0) {
        printf("  9 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
