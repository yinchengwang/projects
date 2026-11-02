/*
 * test_v02.c — v0.2 端到端：catalog + heap 合起来
 *
 * 场景：建两张表 → 各插几千行 → 关闭 → 重开 → 按表名找回 → 逐行校验。
 *
 * 这是 v0.2 的真正验收：证明「表」这个概念在重启后依然成立，
 * 而且两张表的行不会串。
 */

#include <stdio.h>
#include <string.h>

#include "catalog/catalog.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH        "test/tmp/v02.db"
#define N_USERS     2000
#define N_EVENTS    500

static ColDesc USERS_COLS[] = {
    { "id",    T_INT4, false },
    { "name",  T_TEXT, true  },
    { "score", T_INT8, false },
};
#define N_UCOLS 3

static ColDesc EVENTS_COLS[] = {
    { "ts",   T_INT8, false },
    { "kind", T_TEXT, true  },
    { "val",  T_INT4, false },
};
#define N_ECOLS 3

static int ins_user(Heap *h, u32 id)
{
    char name[32];
    Value v[N_UCOLS];
    u8 buf[256];
    u16 len;
    Rid rid;

    memset(v, 0, sizeof(v));
    v[0].u.i4 = (i32)id;
    if (id % 7 == 0) {
        v[1].is_null = true;
    } else {
        snprintf(name, sizeof(name), "u%u", id);
        v[1].u.text.p = (const u8 *)name;
        v[1].u.text.n = (u16)strlen(name);
    }
    v[2].u.i8 = (i64)id * 7;

    int rc = rec_encode(USERS_COLS, N_UCOLS, v, buf, sizeof(buf), &len);
    if (rc != OK) return rc;
    return heap_insert(h, 0, buf, len, &rid);
}

static int ins_event(Heap *h, u32 i)
{
    Value v[N_ECOLS];
    u8 buf[256];
    u16 len;
    Rid rid;

    memset(v, 0, sizeof(v));
    v[0].u.i8 = 1700000000LL + (i64)i;
    v[1].is_null = (i % 4 == 0);
    v[1].u.text.p = (const u8 *)"click";
    v[1].u.text.n = 5;
    v[2].u.i4 = (i32)(i * 3);

    int rc = rec_encode(EVENTS_COLS, N_ECOLS, v, buf, sizeof(buf), &len);
    if (rc != OK) return rc;
    return heap_insert(h, 0, buf, len, &rid);
}

/* 扫 users，校验每行内容；返回行数 */
static u32 verify_users(Catalog *cat, u32 tid)
{
    Heap *h = catalog_heap(cat, tid);
    if (!h) { CHECK(h != NULL); return 0; }

    HeapScan *s;
    u8 buf[256];
    u16 len;
    Rid rid;
    u32 n = 0;
    u32 bad = 0;

    if (heap_scan_begin(h, &s) != OK) { CHECK(0); return 0; }
    while (heap_scan_next(s, &rid, buf, sizeof(buf), &len) == OK) {
        Value v[N_UCOLS];
        if (rec_decode(USERS_COLS, N_UCOLS, buf, len, v) != OK) { bad++; continue; }

        u32 id = (u32)v[0].u.i4;
        if (id >= N_USERS) { bad++; continue; }
        if (v[2].u.i8 != (i64)id * 7) { bad++; continue; }

        if (id % 7 == 0) {
            if (!v[1].is_null) bad++;
        } else {
            char name[32];
            snprintf(name, sizeof(name), "u%u", id);
            if (v[1].is_null || v[1].u.text.n != strlen(name) ||
                memcmp(v[1].u.text.p, name, strlen(name)) != 0)
                bad++;
        }
        n++;
    }
    heap_scan_end(s);

    CHECK(bad == 0);
    return n;
}

static u32 count_events(Catalog *cat, u32 tid)
{
    Heap *h = catalog_heap(cat, tid);
    if (!h) return 0;

    HeapScan *s;
    u8 buf[256];
    u16 len;
    Rid rid;
    u32 n = 0;

    if (heap_scan_begin(h, &s) != OK) return 0;
    while (heap_scan_next(s, &rid, buf, sizeof(buf), &len) == OK) {
        Value v[N_ECOLS];
        if (rec_decode(EVENTS_COLS, N_ECOLS, buf, len, v) != OK) continue;
        /* 事件表的 val 一定是 i*3，抽查几个 */
        if (v[2].u.i4 % 3 == 0) n++;
    }
    heap_scan_end(s);
    return n;
}

int main(void)
{
    Pager *pg = NULL;
    Catalog *cat = NULL;
    u32 users_tid = 0, events_tid = 0, tmp = 0;

    remove(PATH);
    printf("test_v02:\n");

    /* ---------------- 建表 + 灌数据 ---------------- */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);
    CHECK(catalog_ntables(cat) == 4);

    CHECK(catalog_create_table(cat, 0, "users", USERS_COLS, N_UCOLS, &users_tid) == OK);
    CHECK(catalog_create_table(cat, 0, "events", EVENTS_COLS, N_ECOLS, &events_tid) == OK);
    CHECK(users_tid != events_tid);
    CHECK(catalog_ntables(cat) == 6);

    Heap *hu = catalog_heap(cat, users_tid);
    Heap *he = catalog_heap(cat, events_tid);
    CHECK(hu != NULL && he != NULL);

    for (u32 i = 0; i < N_USERS; i++) {
        CHECK(ins_user(hu, i) == OK);
        if (failures > 3) break;
    }
    for (u32 i = 0; i < N_EVENTS; i++) {
        CHECK(ins_event(he, i) == OK);
        if (failures > 3) break;
    }

    CHECK(verify_users(cat, users_tid) == N_USERS);
    CHECK(count_events(cat, events_tid) == N_EVENTS);

    u32 users_pages = heap_npages(hu);
    u32 events_pages = heap_npages(he);
    CHECK(users_pages > events_pages);      /* 行多的表页更多 */

    /* ---------------- 重启 ---------------- */
    catalog_close(cat);
    pager_close(pg);
    cat = NULL;
    pg = NULL;

    CHECK(pager_open(PATH, 16, &pg) == OK);     /* 小池子强制逐出 */
    CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);

    CHECK(catalog_ntables(cat) == 6);
    CHECK(catalog_find_table(cat, "users", &tmp) == OK);
    CHECK(tmp == users_tid);
    CHECK(catalog_find_table(cat, "events", &tmp) == OK);
    CHECK(tmp == events_tid);

    /* 列定义还在 */
    {
        const ColDesc *cols = NULL;
        int n = 0;
        CHECK(catalog_table_cols(cat, users_tid, &cols, &n) == OK);
        CHECK(n == N_UCOLS);
        if (n == N_UCOLS) {
            CHECK(strcmp(cols[1].name, "name") == 0);
            CHECK(cols[1].type == T_TEXT && cols[1].nullable);
        }
    }

    /* 行也在，且两张表不串 */
    CHECK(verify_users(cat, users_tid) == N_USERS);
    CHECK(count_events(cat, events_tid) == N_EVENTS);

    printf("    （%u 页 users + %u 页 events；重启后 %u 行 / %u 行全部校验通过）\n",
           users_pages, events_pages, N_USERS, N_EVENTS);

    catalog_close(cat);
    pager_close(pg);

    if (failures == 0) {
        printf("  v0.2 端到端通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
