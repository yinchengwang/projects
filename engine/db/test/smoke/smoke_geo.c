/*
 * smoke_geo.c —— 地理空间模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + geo。链接能过 ⇒ geo 没引用别的模态。
 *
 * 五步：建存储 → 插两点 → 窗口查询 → 断言命中与内容 → 关闭。
 */
#include <stdio.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_geo: %s: ", (tag));                             \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    DB        *db = NULL;
    db_err     e;
    uint64_t   id0 = 0, id1 = 0, n = 0;
    uint32_t   ids[8];
    uint32_t   n_out = 0;

    remove("test/tmp/smoke_geo.db");
    remove("test/tmp/smoke_geo.db-wal");
    remove("test/tmp/smoke_geo.db-clog");

    e = db_open("test/tmp/smoke_geo.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    e = db_geo_create(db);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_geo_create", "%s", db_last_error(db));
    }

    /* 一个在窗口内，一个在窗口外 */
    e = db_geo_insert(db, 1.0f, 1.0f, &id0);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_geo_insert(内)", "%s", db_last_error(db));
    }
    e = db_geo_insert(db, 100.0f, 100.0f, &id1);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_geo_insert(外)", "%s", db_last_error(db));
    }

    /* 窗口 [0,2] × [0,2] 只该命中第一个点 */
    n_out = 0;
    e = db_geo_window(db, 0.0f, 0.0f, 2.0f, 2.0f, ids, 8, &n_out);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_geo_window", "%s", db_last_error(db));
    }
    if (n_out != 1)
        FAIL("窗口命中数", "期望 1，实际 %u", n_out);
    if ((uint64_t)ids[0] != id0)
        FAIL("窗口命中 id", "期望 %llu，实际 %u",
             (unsigned long long)id0, ids[0]);

    /* 计数应为 2 */
    e = db_geo_count(db, &n);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_geo_count", "%s", db_last_error(db));
    }
    if (n != 2)
        FAIL("点数", "期望 2，实际 %llu", (unsigned long long)n);

    printf("smoke_geo: OK\n");
    db_close(db);
    return 0;
}
