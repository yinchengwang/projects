/*
 * test_copyout.c —— v1.2 步骤 1：copy-out 重构的验收
 *
 * 3 项：
 *   1. **拷贝语义**：拿到的副本在 DDL（建表 / 建索引）之后**一字不差**
 *   2. **悬空是真的**：旧接口返回的内部指针，在 catalog 扩容后确实作废
 *   3. **并发**：一个连接狂建表，另一个连接狂查 —— 查到的列定义必须一直对
 *
 * ==================== 为什么这项重构有意义 ====================
 *
 * v0.10 的判断是"读接口返回共享指针，所以锁只能画粗"。
 * v1.2 步骤 3/4 要把一把大锁拆成按域的小锁，
 * 前提就是**读接口不再返回共享指针**。
 *
 * 而这三个接口原来返回的确实是**真悬空**：
 *
 *   catalog_table_meta   → &e->meta，e 在 cat->t 数组里，
 *                          建表时 cat->t 被 realloc → 整个数组搬家
 *   catalog_table_indexes→ e->idxs，建索引时 realloc → 数组搬家
 *   catalog_table_cols   → e->cols，每表一份 malloc，只在关库时 free
 *                          —— 它**恰好**稳定，但那是隐含前提，不是保证
 *
 * 第 2 项就是把这个"确实会悬空"**证明出来**（不解引用，只比较地址），
 * 第 1 项证明新接口的副本不受影响。
 *
 * ==================== 第 3 项现在为什么是绿的 ====================
 *
 * 现在 db_exec 整条语句持全局大锁 → DDL 和查询**不会交错**，
 * 所以第 3 项现在是靠大锁兜底的。
 * 步骤 4 拆掉大锁之后，它必须**继续绿** —— 那才是 copy-out 的真正价值。
 * 这就是计划里说的"先在大锁下把测试写好，确认测试本身有效"。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "catalog/catalog_priv.h"   /* 内部结构：为了证明"旧指针确实悬空" */
#include "db_internal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/copyout.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    if (e != DB_OK) {
        printf("    执行失败: %s\n      SQL: %.80s\n", db_last_error(db), sql);
        return -1;
    }
    return 0;
}

/* ---------------- 1) 拷贝语义 ---------------- */

/*
 * 拿三份副本，然后做一堆 DDL 把 catalog 的内部数组全部 realloc 一遍，
 * 最后检查三份副本**一字不差**。
 */
static void t1_copy_survives_ddl(DB *w)
{
    Catalog *cat = w->db.cat;

    u32 tid = 0;
    CHECK(catalog_find_table(cat, "users", &tid) == OK);

    /* 两段式：先问要多少 */
    int n = 0;
    CHECK(catalog_table_cols(cat, tid, NULL, 0, &n) == OK);
    CHECK(n == 2);

    ColDesc cols[16];
    CHECK(catalog_table_cols(cat, tid, cols, 16, &n) == OK);
    CHECK(n == 2);
    CHECK(strcmp(cols[0].name, "id") == 0);
    CHECK(cols[0].type == T_INT4);
    CHECK(strcmp(cols[1].name, "name") == 0);
    CHECK(cols[1].type == T_TEXT);

    TableMeta meta;
    memset(&meta, 0, sizeof(meta));
    CHECK(catalog_table_meta(cat, tid, &meta) == OK);
    CHECK(strcmp(meta.name, "users") == 0);
    CHECK(meta.table_id == tid);

    IndexInfo idxs[16];
    int nidx = 0;
    CHECK(catalog_table_indexes(cat, tid, idxs, 16, &nidx) == OK);
    CHECK(nidx == 0);

    /* 缓冲区不够 → 明确报错，不是静默截断 */
    ColDesc tiny[1];
    int n2 = 0;
    CHECK(catalog_table_cols(cat, tid, tiny, 1, &n2) == E_NOSPACE);
    CHECK(n2 == 2);                 /* 仍然报出实际需要的列数 */

    /* DDL：建 20 张表（逼 cat->t 反复 realloc）+ 给 users 建索引（逼 te->idxs realloc） */
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "CREATE TABLE tmp%d (a INT, b TEXT);", i);
        CHECK(run_sql(w, sql) == 0);
    }
    CHECK(run_sql(w, "CREATE INDEX ix_users_name ON users (name);") == 0);

    /* 三份副本必须一字不差 —— 这就是 copy-out 的全部意义 */
    CHECK(strcmp(cols[0].name, "id") == 0);
    CHECK(cols[0].type == T_INT4);
    CHECK(strcmp(cols[1].name, "name") == 0);
    CHECK(cols[1].type == T_TEXT);
    CHECK(strcmp(meta.name, "users") == 0);
    CHECK(meta.table_id == tid);
    CHECK(meta.ncols == 2);
    CHECK(nidx == 0);

    /* 重新取一次：索引数应该变成 1（说明 DDL 确实生效了，只是没影响旧副本） */
    IndexInfo idxs2[16];
    int nidx2 = 0;
    CHECK(catalog_table_indexes(cat, tid, idxs2, 16, &nidx2) == OK);
    CHECK(nidx2 == 1);
    CHECK(strcmp(idxs2[0].meta.name, "ix_users_name") == 0);
    CHECK(idxs2[0].meta.table_id == tid);
}

/* ---------------- 2) 悬空是真的 ---------------- */

/*
 * 证明旧接口返回的指针**确实**会作废。
 *
 * 【为什么不直接解引用那个旧指针】
 * 解引用已释放的内存是未定义行为：可能读到原值、可能读到垃圾、
 * 可能直接崩 —— 三种都可能出现，那样的测试是**不可靠**的。
 *
 * 所以这里只做**地址比较**：
 * 旧指针还落在当前数组的范围里吗？
 * 不在 → 它指向的是一块**已经被 realloc 释放**的内存 → 悬空，铁证。
 *
 * 而且要**等到数组真的搬了家**才下结论（realloc 可能原地扩展）。
 */
static void t2_old_pointer_dangles(DB *w)
{
    Catalog *cat = w->db.cat;

    u32 tid = 0;
    CHECK(catalog_find_table(cat, "users", &tid) == OK);

    /* 旧接口的做法：拿 catalog 内部的指针 */
    TableEnt *e = catalog_ent_by_id(cat, tid);
    CHECK(e != NULL);
    if (!e)
        return;
    const TableMeta *old_meta = &e->meta;
    TableEnt *old_base = cat->t;

    /* 建表直到 cat->t 搬家（realloc 可能原地扩展，那就还不算悬空） */
    int moved = 0;
    for (int i = 0; i < 40 && !moved; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "CREATE TABLE dangle%d (a INT);", i);
        if (run_sql(w, sql) != 0)
            break;
        if (cat->t != old_base)
            moved = 1;
    }
    CHECK(moved);                    /* 必须真的搬了家，否则这一项没意义 */

    if (!moved)
        return;

    /* 旧指针不再落在当前数组内 → 它指向的内存已经被 realloc 释放 */
    const void *lo = (const void *)cat->t;
    const void *hi = (const void *)(cat->t + cat->n);
    CHECK((const void *)old_meta < lo || (const void *)old_meta >= hi);

    /*
     * 而新的 copy-out 接口拿到的是**调用方自己的副本**：
     * 它既不在 catalog 的数组里，也不会因为 catalog 扩容而失效。
     */
    TableMeta meta;
    memset(&meta, 0, sizeof(meta));
    CHECK(catalog_table_meta(cat, tid, &meta) == OK);
    CHECK(strcmp(meta.name, "users") == 0);
    CHECK((const void *)&meta < lo || (const void *)&meta >= hi);
}

/* ---------------- 3) 并发：DDL 与查询交错 ---------------- */

typedef struct {
    DB  *db;
    int  rounds;
    int  bad;         /* 出错的次数 */
} Worker;

static void *ddl_worker(void *arg)
{
    Worker *w = (Worker *)arg;
    for (int i = 0; i < w->rounds; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "CREATE TABLE conc%d (a INT, b TEXT);", i);
        if (run_sql(w->db, sql) != 0)
            w->bad++;
    }
    return NULL;
}

/*
 * 查询线程：反复查 users 的两列。
 *
 * 判据不是"没崩" —— 悬空指针**可能**读出正确的值，看起来一切正常。
 * 判据是"每一列的名字和类型都对" ——
 * 读错了列定义，名字就会变成别的东西。
 */
static void *query_worker(void *arg)
{
    Worker *w = (Worker *)arg;
    for (int i = 0; i < w->rounds; i++) {
        Result *r = NULL;
        if (db_exec(w->db, "SELECT id, name FROM users;", &r) != DB_OK) {
            w->bad++;
            if (r)
                db_result_free(r);
            continue;
        }
        if (db_result_cols(r) != 2) {
            w->bad++;
        } else {
            const char *c0 = db_result_colname(r, 0);
            const char *c1 = db_result_colname(r, 1);
            if (!c0 || !c1 || strcmp(c0, "id") != 0 || strcmp(c1, "name") != 0)
                w->bad++;
        }
        if (db_result_rows(r) != 3)
            w->bad++;
        db_result_free(r);
    }
    return NULL;
}

static void t3_concurrent_ddl_and_query(DB *w)
{
    DB *r = NULL;
    CHECK(db_connect(w, &r) == DB_OK);
    if (!r)
        return;

    Worker d = { w, 30, 0 };
    Worker q = { r, 30, 0 };

    pthread_t td, tq;
    CHECK(pthread_create(&td, NULL, ddl_worker, &d) == 0);
    CHECK(pthread_create(&tq, NULL, query_worker, &q) == 0);
    pthread_join(td, NULL);
    pthread_join(tq, NULL);

    CHECK(d.bad == 0);
    CHECK(q.bad == 0);
    printf("    并发 %d 轮建表 + %d 轮查询，查询侧错误 %d 次\n",
           d.rounds, q.rounds, q.bad);

    db_conn_close(r);
}

/* ---------------- main ---------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能看到走到哪一步 */
    printf("test_copyout（集成）:\n");

    remove(PATH);
    remove("test/tmp/copyout.db-wal");
    remove("test/tmp/copyout.db-shm");

    DB *w = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    if (!w)
        return 1;

    /* users：两列，3 行 —— 后面并发查询要拿它当基准 */
    CHECK(run_sql(w, "CREATE TABLE users (id INT, name TEXT);") == 0);
    CHECK(run_sql(w, "INSERT INTO users VALUES (1,'alice');") == 0);
    CHECK(run_sql(w, "INSERT INTO users VALUES (2,'bob');") == 0);
    CHECK(run_sql(w, "INSERT INTO users VALUES (3,NULL);") == 0);

    printf("  1. 拷贝语义：DDL 之后副本一字不差\n");
    t1_copy_survives_ddl(w);

    printf("  2. 旧接口的内部指针确实会悬空\n");
    t2_old_pointer_dangles(w);

    printf("  3. 并发：DDL 与查询交错\n");
    t3_concurrent_ddl_and_query(w);

    db_close(w);

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
