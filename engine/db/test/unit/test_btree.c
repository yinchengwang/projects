/*
 * test_btree.c — B+tree 验收
 *
 * 8 项：基本查找 / 随机插入（树高+填充率）/ 顺序插入 / 重复 key /
 *       范围扫描 / 删除一半 / 删除全部 / 内部节点分裂正确性
 */

#include <stdio.h>
#include <string.h>

#include "index/btree.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define N 100000

typedef struct {
    Pager    *pg;
    FreeList *fl;
    BTree    *bt;
    u32       root;
} Env;

static int env_open(Env *e, const char *path, bool create)
{
    memset(e, 0, sizeof(*e));
    if (create)
        remove(path);

    int rc = pager_open(path, 64, &e->pg);
    if (rc != OK) return rc;
    rc = freelist_create(0, &e->fl);
    if (rc != OK) return rc;

    if (create) {
        rc = btree_create(e->pg, e->fl, &e->root);
        if (rc != OK) return rc;
    }
    return btree_open(e->pg, e->fl, e->root, &e->bt);
}

static void env_close(Env *e)
{
    if (e->bt) btree_destroy(e->bt);
    if (e->fl) freelist_destroy(e->fl);
    if (e->pg) pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

/*
 * key 必须大端 —— B+tree 用 memcmp 比较，小端会让 63 > 256。
 * 这个坑 DESIGN.md §3.2 写过，第一版测试还是踩了。
 */
static u8 kbuf[4];

static void mk_key(u32 k, u8 *b)
{
    b[0] = (u8)(k >> 24);
    b[1] = (u8)(k >> 16);
    b[2] = (u8)(k >> 8);
    b[3] = (u8)k;
}

#define KEY(x)  (mk_key((u32)(x), kbuf), (const void *)kbuf)
#define KLEN    4u

static Rid mk_rid(u32 k)
{
    return rid_make(k / 1000u + 1u, (u16)(k % 1000u));
}

static u64 rng_state = 0x243F6A8885A308D3ull;
static u64 rng_next(void)
{
    u64 x = rng_state;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    rng_state = x;
    return x;
}

static u32 scan_count(BTree *bt)
{
    BtCursor *c;
    if (btree_scan(bt, NULL, 0, NULL, 0, &c) != OK)
        return 0;
    Rid rid;
    u32 n = 0;
    while (btree_cursor_next(c, &rid) == OK)
        n++;
    btree_cursor_close(c);
    return n;
}

/* ---- 1) 基本插入与查找 ---- */
static void t1_basic(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt1.db", true) == OK);

    for (u32 k = 0; k < 100; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    for (u32 k = 0; k < 100; k++) {
        Rid out[4];
        int n = 0;
        CHECK(btree_search(e.bt, KEY(k), KLEN, out, 4, &n) == OK);
        CHECK(n == 1);
        if (n == 1) {
            CHECK(out[0].page == mk_rid(k).page);
            CHECK(out[0].slot == mk_rid(k).slot);
        }
    }
    /* 不存在的 key */
    {
        Rid out[4];
        int n = 0;
        CHECK(btree_search(e.bt, KEY(9999), KLEN, out, 4, &n) == E_NOTFOUND);
        CHECK(n == 0);
    }
    env_close(&e);
}

/* ---- 2) 随机插入 10 万：树高与填充率 ---- */
static void t2_random(void)
{
    static u32 perm[N];
    Env e;

    CHECK(env_open(&e, "test/tmp/bt2.db", true) == OK);

    for (u32 i = 0; i < N; i++)
        perm[i] = i;
    /* Fisher-Yates 打乱 */
    for (u32 i = N; i-- > 1; ) {
        u32 j = (u32)(rng_next() % (u64)(i + 1));
        u32 t = perm[i]; perm[i] = perm[j]; perm[j] = t;
    }

    for (u32 i = 0; i < N; i++) {
        CHECK(btree_insert(e.bt, KEY(perm[i]), KLEN, mk_rid(perm[i])) == OK);
        if (failures > 3) break;
    }

    BtStats st;
    CHECK(btree_stats(e.bt, &st) == OK);
    printf("    （随机 %d：高度 %u，%u 页（%u 叶 / %u 内），填充率 %u%%）\n",
           N, btree_height(e.bt), st.npages, st.nleaves, st.ninner, st.fill_pct);

    /* 100k 个 4 字节 key：叶扇出 254、内扇出 339 → 约 570 叶 → 高度 2 */
    CHECK(btree_height(e.bt) == 2);
    CHECK(st.fill_pct >= 60 && st.fill_pct <= 80);

    /* 全量查找 */
    u32 bad = 0;
    for (u32 k = 0; k < N; k++) {
        Rid out[2];
        int n = 0;
        if (btree_search(e.bt, KEY(k), KLEN, out, 2, &n) != OK || n != 1)
            bad++;
        if (bad > 3) break;
    }
    CHECK(bad == 0);
    CHECK(scan_count(e.bt) == N);

    env_close(&e);
}

/* ---- 3) 顺序插入：填充率应约 50% ---- */
static void t3_sequential(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt3.db", true) == OK);

    for (u32 k = 0; k < N; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    BtStats st;
    CHECK(btree_stats(e.bt, &st) == OK);
    printf("    （顺序 %d：高度 %u，%u 页（%u 叶 / %u 内），items=%u，填充率 %u%%）\n",
           N, btree_height(e.bt), st.npages, st.nleaves, st.ninner, st.items, st.fill_pct);

    /* 顺序插入总在最右叶分裂：左半留一半，右半接新值 → 稳态约 50% */
    CHECK(st.fill_pct >= 40 && st.fill_pct <= 60);
    CHECK(scan_count(e.bt) == N);
    env_close(&e);
}

/* ---- 4) 重复 key ---- */
static void t4_duplicate(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt4.db", true) == OK);

    for (u32 i = 0; i < 10; i++)
        CHECK(btree_insert(e.bt, KEY(42), KLEN, rid_make(7, (u16)i)) == OK);
    CHECK(btree_insert(e.bt, KEY(41), KLEN, rid_make(7, 100)) == OK);
    CHECK(btree_insert(e.bt, KEY(43), KLEN, rid_make(7, 200)) == OK);

    Rid out[16];
    int n = 0;
    CHECK(btree_search(e.bt, KEY(42), KLEN, out, 16, &n) == OK);
    CHECK(n == 10);

    /* 10 个 RID 互不相同 */
    for (int i = 0; i < n; i++)
        for (int j = i + 1; j < n; j++)
            CHECK(out[i].slot != out[j].slot);

    /* 删除其中一个，其余还在 */
    CHECK(btree_delete(e.bt, KEY(42), KLEN, rid_make(7, 5)) == OK);
    CHECK(btree_search(e.bt, KEY(42), KLEN, out, 16, &n) == OK);
    CHECK(n == 9);

    /* 删不存在的 RID */
    CHECK(btree_delete(e.bt, KEY(42), KLEN, rid_make(7, 99)) == E_NOTFOUND);
    env_close(&e);
}

/* ---- 5) 范围扫描 ---- */
static void t5_scan(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt5.db", true) == OK);

    for (u32 k = 0; k < 1000; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    BtCursor *c;
    u8 lo[4], hi[4];
    mk_key(100, lo);
    mk_key(200, hi);
    CHECK(btree_scan(e.bt, lo, KLEN, hi, KLEN, &c) == OK);

    u32 expect = 100;
    u32 got = 0;
    Rid rid;
    while (btree_cursor_next(c, &rid) == OK) {
        CHECK(rid.page == mk_rid(expect).page);
        CHECK(rid.slot == mk_rid(expect).slot);
        expect++;
        got++;
        if (got > 200) break;
    }
    btree_cursor_close(c);
    CHECK(got == 101);                 /* 闭区间 [100, 200] */

    /* 全范围 */
    CHECK(scan_count(e.bt) == 1000);

    /* 空区间 */
    u8 lo2[4], hi2[4];
    mk_key(500, lo2);
    mk_key(499, hi2);
    CHECK(btree_scan(e.bt, lo2, KLEN, hi2, KLEN, &c) == OK);
    CHECK(btree_cursor_next(c, &rid) == E_EOF);
    btree_cursor_close(c);
    env_close(&e);
}

/* ---- 6) 删除一半 ---- */
static void t6_delete_half(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt6.db", true) == OK);

    for (u32 k = 0; k < 10000; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    for (u32 k = 0; k < 10000; k += 2)
        CHECK(btree_delete(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    u32 bad = 0;
    for (u32 k = 0; k < 10000; k++) {
        Rid out[2];
        int n = 0;
        int rc = btree_search(e.bt, KEY(k), KLEN, out, 2, &n);
        if (k % 2 == 0) {
            if (rc != E_NOTFOUND) bad++;
        } else {
            if (rc != OK || n != 1) bad++;
        }
        if (bad > 3) break;
    }
    CHECK(bad == 0);
    CHECK(scan_count(e.bt) == 5000);
    env_close(&e);
}

/* ---- 7) 删除全部：树回到空 ---- */
static void t7_delete_all(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt7.db", true) == OK);

    for (u32 k = 0; k < 1000; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);
    CHECK(btree_height(e.bt) >= 1);

    for (u32 k = 0; k < 1000; k++)
        CHECK(btree_delete(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    CHECK(btree_height(e.bt) == 0);      /* 根降高到叶子 */
    CHECK(scan_count(e.bt) == 0);

    /* 还能继续用 */
    CHECK(btree_insert(e.bt, KEY(777), KLEN, mk_rid(777)) == OK);
    CHECK(scan_count(e.bt) == 1);
    env_close(&e);
}

/* ---- 8) 内部节点分裂：每个 key 只出现一次 ---- */
static void t8_inner_split(void)
{
    Env e;
    CHECK(env_open(&e, "test/tmp/bt8.db", true) == OK);

    /* 插入足够多，确保发生内部节点分裂 */
    /* 50000 个 key → 约 394 叶 > 根容量 339，必然发生内部节点分裂 */
    for (u32 k = 0; k < 50000; k++)
        CHECK(btree_insert(e.bt, KEY(k), KLEN, mk_rid(k)) == OK);

    BtStats st;
    CHECK(btree_stats(e.bt, &st) == OK);
    CHECK(st.ninner >= 2);               /* 至少有内部节点分裂过 */
    CHECK(btree_height(e.bt) >= 1);

    /* 扫描：必须严格递增，且每个 key 恰好一次 —— 
     * 内部节点分裂若把提升 key 留在某一边，这里会看到重复 */
    BtCursor *c;
    CHECK(btree_scan(e.bt, NULL, 0, NULL, 0, &c) == OK);

    Rid rid;
    u32 expect = 0;
    u32 bad = 0;
    while (btree_cursor_next(c, &rid) == OK) {
        if (rid.page != mk_rid(expect).page || rid.slot != mk_rid(expect).slot) {
            bad++;
            if (bad > 3) break;
        }
        expect++;
    }
    btree_cursor_close(c);
    CHECK(bad == 0);
    CHECK(expect == 50000);
    env_close(&e);
}

int main(void)
{
    printf("test_btree:\n");
    t1_basic();
    t2_random();
    t3_sequential();
    t4_duplicate();
    t5_scan();
    t6_delete_half();
    t7_delete_all();
    t8_inner_split();

    if (failures == 0) {
        printf("  8 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
