/*
 * smoke_vector.c —— 向量模态的链接期独立证明
 *
 * 链接集合 = 底座 + relational + vector。链接能过 ⇒ vector 没引用别的模态。
 *
 * 五步：建存储 → 插两条 → KNN 查最近 → 断言命中的 label → 关闭。
 */
#include <stdio.h>

#include "db.h"

#define FAIL(tag, ...)                                                         \
    do {                                                                       \
        fprintf(stderr, "smoke_vector: %s: ", (tag));                          \
        fprintf(stderr, __VA_ARGS__);                                          \
        fputc('\n', stderr);                                                   \
        return 1;                                                              \
    } while (0)

#define DIM 4u

int main(void)
{
    DB         *db = NULL;
    db_err      e;
    float       v1[DIM] = {1.0f, 0.0f, 0.0f, 0.0f};
    float       v2[DIM] = {0.0f, 1.0f, 0.0f, 0.0f};
    float       q[DIM]  = {0.9f, 0.1f, 0.0f, 0.0f};
    db_vec_hit  hits[4];
    uint32_t    n_out = 0;
    uint64_t    row_id = 0, n = 0;

    remove("test/tmp/smoke_vector.db");
    remove("test/tmp/smoke_vector.db-wal");
    remove("test/tmp/smoke_vector.db-clog");

    e = db_open("test/tmp/smoke_vector.db", &db);
    if (e != DB_OK)
        FAIL("db_open", "%s", db_strerror(e));

    /* dim=4，HNSW 的 m / efc 取常用值 */
    e = db_vec_create(db, DIM, 16, 200);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_vec_create", "%s", db_last_error(db));
    }

    /* 两条正交的单位向量，label 各自区分 */
    e = db_vec_insert(db, v1, 101, &row_id);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_vec_insert(1)", "%s", db_last_error(db));
    }
    e = db_vec_insert(db, v2, 202, &row_id);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_vec_insert(2)", "%s", db_last_error(db));
    }

    /* 查询向量贴近 v1 ⇒ 第一条命中必须来自 label 101 那一行 */
    e = db_vec_knn(db, q, 2, 64, hits, &n_out);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_vec_knn", "%s", db_last_error(db));
    }
    if (n_out != 2)
        FAIL("命中条数", "期望 2，实际 %u", n_out);
    if (hits[0].row_id != 0)
        FAIL("最近邻", "期望 row_id 0（第一条插入），实际 %llu",
             (unsigned long long)hits[0].row_id);

    /* 计数也得是 2 */
    e = db_vec_count(db, &n);
    if (e != DB_OK) {
        db_close(db);
        FAIL("db_vec_count", "%s", db_last_error(db));
    }
    if (n != 2)
        FAIL("向量总数", "期望 2，实际 %llu", (unsigned long long)n);

    printf("smoke_vector: OK\n");
    db_close(db);
    return 0;
}
