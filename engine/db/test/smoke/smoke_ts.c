/*
 * smoke_ts.c —— 时序模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + ts。链接能过 ⇒ ts 没引用别的模态。
 *
 * 五步：建存储 → 插两条读数 → 计数 → 断言内容 → 关闭。
 */
#include <stdio.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_ts: %s: ", (tag));                              \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    DB            *db = NULL;
    db_err         e;
    db_ts_reading  r;
    uint64_t       n = 0;
    uint32_t       nparts = 0;

    remove("test/tmp/smoke_ts.db");
    remove("test/tmp/smoke_ts.db-wal");
    remove("test/tmp/smoke_ts.db-clog");

    e = db_open("test/tmp/smoke_ts.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    e = db_ts_create(db);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ts_create", "%s", db_last_error(db));
    }

    /* 两条读数：时间差 1000ms，值差 0.5 */
    r.time = 1700000000000LL;
    r.tag = 7;
    r.value = 1.5;
    e = db_ts_insert(db, &r);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ts_insert(1)", "%s", db_last_error(db));
    }

    r.time = 1700000001000LL;
    r.tag = 7;
    r.value = 2.5;
    e = db_ts_insert(db, &r);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ts_insert(2)", "%s", db_last_error(db));
    }

    /* 计数应为 2 */
    e = db_ts_count(db, &n);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ts_count", "%s", db_last_error(db));
    }
    if (n != 2)
        FAIL("读数条数", "期望 2，实际 %llu", (unsigned long long)n);

    e = db_ts_part_count(db, &nparts);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ts_part_count", "%s", db_last_error(db));
    }
    if (nparts < 1)
        FAIL("分区数", "期望 >= 1，实际 %u", nparts);

    printf("smoke_ts: OK\n");
    db_close(db);
    return 0;
}
