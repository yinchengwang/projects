/*
 * smoke_ft.c —— 全文检索模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + ft。链接能过 ⇒ ft 没引用别的模态。
 *
 * 五步：加文档 → 检索 → 断言命中 → 删一篇 → 断言不再命中 → 关闭。
 */
#include <stdio.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_ft: %s: ", (tag));                              \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

int main(void)
{
    DB        *db = NULL;
    db_err     e;
    db_ft_hit  hits[4];
    uint32_t   n_out = 0;

    remove("test/tmp/smoke_ft.db");
    remove("test/tmp/smoke_ft.db-wal");
    remove("test/tmp/smoke_ft.db-clog");

    e = db_open("test/tmp/smoke_ft.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    static const char text_a[] = "the quick brown fox jumps over the lazy dog";
    static const char text_b[] = "a slow green turtle sleeps in the sun";

    e = db_ft_add(db, 1, text_a, (uint32_t)(sizeof(text_a) - 1));
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ft_add(1)", "%s", db_last_error(db));
    }
    e = db_ft_add(db, 2, text_b, (uint32_t)(sizeof(text_b) - 1));
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ft_add(2)", "%s", db_last_error(db));
    }

    /* 查 quick —— 只有文档 1 有 */
    e = db_ft_search(db, "quick", 2, hits, 4, &n_out);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ft_search", "%s", db_last_error(db));
    }
    if (n_out != 1)
        FAIL("命中条数", "期望 1，实际 %u", n_out);
    if (hits[0].doc_id != 1)
        FAIL("命中文档", "期望 doc_id 1，实际 %u", hits[0].doc_id);

    /* 删掉文档 1，再查 quick 应该一条都没有 */
    e = db_ft_delete(db, 1);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ft_delete", "%s", db_last_error(db));
    }
    n_out = 0;
    e = db_ft_search(db, "quick", 2, hits, 4, &n_out);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_ft_search(删后)", "%s", db_last_error(db));
    }
    if (n_out != 0)
        FAIL("删后仍命中", "期望 0，实际 %u", n_out);

    printf("smoke_ft: OK\n");
    db_close(db);
    return 0;
}
