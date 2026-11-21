/*
 * smoke_relational.c —— 关系模态的链接期独立证明
 *
 * 【这个文件证明什么】
 * 它只链接「底座 + 关系模态」。链接能过 ⇒ 关系模态没有引用别的模态。
 *
 * 【它不测什么】
 * 不测功能对不对（那是 test/unit/ 的事）。但也不是纯初始化就收工：
 * 建表 → 写入 → 读回 → 断言内容，走完一遍才叫「这个模态真的能用」。
 *
 * ⚠️ db_open 的真实签名是 db_err db_open(const char *path, DB **out)：
 *    返回值是错误码，句柄通过出参回填。写成 `DB *db = db_open(...)` 编不过。
 */
#include <stdio.h>
#include <string.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_relational: %s: ", (tag));                      \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    DB    *db = NULL;
    Result *r = NULL;
    db_err e;

    /* 上一轮留下的库会让 CREATE 报「表已存在」—— 每次从干净状态起跑 */
    remove("test/tmp/smoke_relational.db");
    remove("test/tmp/smoke_relational.db-wal");
    remove("test/tmp/smoke_relational.db-clog");

    e = db_open("test/tmp/smoke_relational.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    e = db_exec(db, "CREATE TABLE t(a INT)", &r);
    if (e != DB_OK) {
        db_close(db);
        FAIL("CREATE TABLE", "%s", db_last_error(db));
    }
    db_result_free(r);
    r = NULL;

    e = db_exec(db, "INSERT INTO t VALUES (42)", &r);
    if (e != DB_OK) {
        db_close(db);
        FAIL("INSERT", "%s", db_last_error(db));
    }
    db_result_free(r);
    r = NULL;

    /* 读回来断言内容 —— 写进去 42，读出来就得是 42 */
    e = db_exec(db, "SELECT a FROM t", &r);
    if (e != DB_OK) {
        db_close(db);
        FAIL("SELECT", "%s", db_last_error(db));
    }
    if (db_result_rows(r) != 1)
        FAIL("行数", "期望 1，实际 %d", db_result_rows(r));
    if (strcmp(db_result_value(r, 0, 0), "42") != 0)
        FAIL("读回值", "期望 42，实际 %s", db_result_value(r, 0, 0));
    db_result_free(r);

    printf("smoke_relational: OK\n");
    db_close(db);
    return 0;
}
