/*
 * test_catalog.c — 元数据验收
 *
 * 重点：自举、建表、列定义往返、重启后元信息还在、损坏检测。
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

#define PATH "test/tmp/catalog.db"

static ColDesc USERS_COLS[] = {
    { "id",   T_INT4, false },
    { "name", T_TEXT, true  },
    { "age",  T_INT4, true  },
};
#define N_USERS 3

static ColDesc LOGS_COLS[] = {
    { "ts",   T_INT8, false },
    { "msg",  T_TEXT, false },
};
#define N_LOGS 2

static void make_user_row(u32 id, const char *name, i32 age,
                          u8 *buf, u16 bufsize, u16 *len)
{
    Value v[N_USERS];
    memset(v, 0, sizeof(v));
    v[0].u.i4 = (i32)id;
    if (name) {
        v[1].u.text.p = (const u8 *)name;
        v[1].u.text.n = (u16)strlen(name);
    } else {
        v[1].is_null = true;
    }
    v[2].u.i4 = age;
    if (age < 0)
        v[2].is_null = true;

    int rc = rec_encode(USERS_COLS, N_USERS, v, buf, bufsize, len);
    CHECK(rc == OK);
}

int main(void)
{
    Pager *pg = NULL;
    Catalog *cat = NULL;
    u32 users_tid = 0, logs_tid = 0, tmp = 0;

    remove(PATH);

    printf("test_catalog:\n");

    /* ---- 1) 自举：空文件 → 4 张系统表 ---- */
    CHECK(pager_open(PATH, 64, &pg) == OK);
    CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);
    CHECK(catalog_ntables(cat) == 4);
    CHECK(catalog_find_table(cat, "sys_table", &tmp) == OK);
    CHECK(tmp == SYS_TABLE_ID);
    CHECK(catalog_find_table(cat, "sys_column", &tmp) == OK);
    CHECK(tmp == SYS_COLUMN_ID);
    CHECK(catalog_next_table_id(cat) == CAT_FIRST_USER_ID);

    /* ---- 2) 建表 ---- */
    CHECK(catalog_create_table(cat, 0, "users", USERS_COLS, N_USERS, &users_tid) == OK);
    CHECK(users_tid == CAT_FIRST_USER_ID);
    CHECK(catalog_ntables(cat) == 5);
    CHECK(catalog_find_table(cat, "users", &tmp) == OK);
    CHECK(tmp == users_tid);

    /* ---- 3) 列定义往返（copy-out：拷进自己的缓冲区） ---- */
    {
        ColDesc cols[8];
        int n = 0;
        CHECK(catalog_table_cols(cat, users_tid, cols, 8, &n) == OK);
        CHECK(n == N_USERS);
        if (n == N_USERS) {
            CHECK(strcmp(cols[0].name, "id") == 0);
            CHECK(cols[0].type == T_INT4);
            CHECK(!cols[0].nullable);
            CHECK(strcmp(cols[1].name, "name") == 0);
            CHECK(cols[1].type == T_TEXT);
            CHECK(cols[1].nullable);
            CHECK(strcmp(cols[2].name, "age") == 0);
            CHECK(cols[2].nullable);
        }
    }

    /* ---- 4) 通过 catalog_heap 存行 ---- */
    {
        Heap *h = catalog_heap(cat, users_tid);
        CHECK(h != NULL);

        u8 rec[256], out[256];
        u16 len, olen;
        Rid rid;
        const char *names[] = { "alice", "bob", NULL };

        for (u32 i = 0; i < 3; i++) {
            make_user_row(i, names[i], (i32)(20 + i), rec, sizeof(rec), &len);
            CHECK(heap_insert(h, 0, rec, len, &rid) == OK);
            CHECK(heap_get(h, rid, out, sizeof(out), &olen) == OK);
            CHECK(olen == len);
            CHECK(memcmp(out, rec, len) == 0);
        }
    }

    /* ---- 5) 第二张表：互不干扰 ---- */
    CHECK(catalog_create_table(cat, 0, "logs", LOGS_COLS, N_LOGS, &logs_tid) == OK);
    CHECK(logs_tid == users_tid + 1);
    {
        Heap *hu = catalog_heap(cat, users_tid);
        Heap *hl = catalog_heap(cat, logs_tid);
        CHECK(hu != NULL && hl != NULL && hu != hl);

        u8 rec[256];
        u16 len;
        Rid rid;
        Value v[N_LOGS];
        memset(v, 0, sizeof(v));
        v[0].u.i8 = 1700000000LL;
        v[1].u.text.p = (const u8 *)"hello";
        v[1].u.text.n = 5;
        CHECK(rec_encode(LOGS_COLS, N_LOGS, v, rec, sizeof(rec), &len) == OK);
        CHECK(heap_insert(hl, 0, rec, len, &rid) == OK);
    }

    /* ---- 6) 重名拒绝 ---- */
    CHECK(catalog_create_table(cat, 0, "users", USERS_COLS, N_USERS, &tmp) == E_INVAL);

    /* ---- 7) 重启后元信息还在 ---- */
    catalog_close(cat);
    pager_close(pg);
    cat = NULL;
    pg = NULL;

    CHECK(pager_open(PATH, 16, &pg) == OK);      /* 小池子 */
    CHECK(catalog_open(pg, NULL, NULL, &cat) == OK);
    CHECK(catalog_ntables(cat) == 6);            /* 4 系统 + users + logs */
    CHECK(catalog_find_table(cat, "users", &tmp) == OK);
    CHECK(tmp == users_tid);
    CHECK(catalog_find_table(cat, "logs", &tmp) == OK);
    CHECK(tmp == logs_tid);
    CHECK(catalog_next_table_id(cat) == logs_tid + 1);

    {
        ColDesc cols[8];
        int n = 0;
        CHECK(catalog_table_cols(cat, users_tid, cols, 8, &n) == OK);
        CHECK(n == N_USERS);
        if (n == N_USERS) {
            CHECK(strcmp(cols[1].name, "name") == 0);
            CHECK(cols[1].type == T_TEXT);
            CHECK(cols[1].nullable);
        }

        /* 行也还在 */
        Heap *h = catalog_heap(cat, users_tid);
        HeapScan *s;
        u32 rows = 0;
        u8 out[256];
        u16 olen;
        Rid rid;
        CHECK(heap_scan_begin(h, &s) == OK);
        while (heap_scan_next(s, &rid, out, sizeof(out), &olen) == OK)
            rows++;
        heap_scan_end(s);
        CHECK(rows == 3);
    }

    /* ---- 8) 页大小或 magic 不对 → E_CORRUPT ---- */
    catalog_close(cat);
    pager_close(pg);
    cat = NULL;
    pg = NULL;

    CHECK(pager_open(PATH, 16, &pg) == OK);
    {
        PageFrame *f;
        CHECK(pager_get(pg, PAGE_ID_META, &f) == OK);
        put_u32(f->data + HDR_SIZE, 0xDEADBEEFu);   /* 破坏 magic */
        CHECK(pager_unpin(pg, PAGE_ID_META, true) == OK);
    }
    pager_close(pg);
    pg = NULL;

    CHECK(pager_open(PATH, 16, &pg) == OK);
    CHECK(catalog_open(pg, NULL, NULL, &cat) == E_CORRUPT);
    pager_close(pg);

    if (failures == 0) {
        printf("  8 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
