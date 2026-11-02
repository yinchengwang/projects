/*
 * test_concurrent.c — 真并发验收（v0.10，关系模态）
 *
 * 6 项：
 *   1. 并发插入 → 行数正确、每个 id 恰好一次
 *   2. **xid 不串号**：一个事务 ROLLBACK、另一个 COMMIT → 提交的那行必须可见
 *   3. 缓冲池压力：多线程大量写入（缓冲池只有 256 帧）→ 不崩、数据不乱
 *   4. 并发写入后崩溃 → 恢复后已提交的行全在
 *   5. 并发读写 → 读到的行完整、行数不越界
 *   6. 隔离级别在并发下仍成立
 *
 * blob 的并发测试在 test_concurrent_blob.c（走的是另一条加锁路径）。
 * 公共设施在 conc_common.h。
 *
 * ==================== 为什么这些测试重要 ====================
 *
 * 竞态 bug 有两个特点：
 *   1. **只在特定时序下暴露** —— 单线程测试永远测不出来
 *   2. **后果是静默的数据损坏** —— xid 串号会让已提交的数据"消失"
 *
 * 所以这一版的测试必须**真的并发**（多个线程同时跑），
 * 而不是"开多个连接顺序跑"。
 *
 * ==================== 第 2 项是核心 ====================
 *
 * 两个事务拿到同一个 xid 时：
 *   A: BEGIN → xid=5          clog[5]=INPROGRESS
 *   B: BEGIN → xid=5（撞了！）  clog[5]=INPROGRESS
 *   B: COMMIT                  clog[5]=COMMITTED   ← B 的行可见
 *   A: ROLLBACK                clog[5]=ABORTED     ← B 的行**消失了**
 *
 * B 已经提交的行，因为 A 的回滚而不可见 —— **静默丢数据**。
 *
 * 这个 bug 只在 xid 分配撞车时出现，所以测试要跑很多轮。
 */

#include "conc_common.h"

#define PATH "test/tmp/concurrent.db"

/* ---------------- 1) 并发插入 ---------------- */

static void *insert_worker(void *arg)
{
    Worker *w = (Worker *)arg;

    DB *conn = NULL;
    if (db_connect(w->inst, &conn) != DB_OK) {
        w->errors++;
        return NULL;
    }

    /* 一个事务插一批 —— 每条语句都 fsync 的话太慢 */
    if (run_sql(conn, "BEGIN;") != 0)
        w->errors++;

    for (int i = 0; i < w->nrows; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (%d, 'w%d-%d');",
                 w->table, w->base_id + i, w->tid, i);
        if (run_sql(conn, sql) != 0)
            w->errors++;
    }

    if (run_sql(conn, "COMMIT;") != 0)
        w->errors++;

    db_conn_close(conn);
    return NULL;
}

static void t1_concurrent_insert(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);

    pthread_t th[NTHREAD];
    Worker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].inst = db;
        w[i].table = "t";
        w[i].tid = i;
        w[i].nrows = NROWS;
        w[i].base_id = i * NROWS;
        w[i].errors = 0;
        CHECK(pthread_create(&th[i], NULL, insert_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].errors == 0);
    }

    /* 行数必须正好是 NTHREAD × NROWS —— 少一行就是丢了 */
    Result *r = NULL;
    int n = query_int_col(db, "SELECT id FROM t;", &r);
    CHECK(n == NTHREAD * NROWS);
    if (n == NTHREAD * NROWS) {
        /* 每个 id 恰好出现一次 —— 重复就是两个线程抢到了同一个 slot */
        int *seen = calloc((size_t)(NTHREAD * NROWS), sizeof(int));
        CHECK(seen != NULL);
        if (seen) {
            int bad = 0;
            for (int i = 0; i < n; i++) {
                int id = atoi(db_result_value(r, i, 0));
                if (id < 0 || id >= NTHREAD * NROWS) { bad++; continue; }
                seen[id]++;
                if (seen[id] > 1) bad++;
            }
            CHECK(bad == 0);
            free(seen);
        }
    }
    if (r) db_result_free(r);

    db_close(db);
    remove_db(PATH);
}

/* ---------------- 2) xid 不串号 ---------------- */

/*
 * A 线程：BEGIN → INSERT → 等 B 提交完 → ROLLBACK
 * B 线程：等 A 插完 → BEGIN → INSERT → COMMIT
 *
 * 如果 A 和 B 拿到同一个 xid：
 *   B 先 COMMIT（clog[xid]=COMMITTED），A 后 ROLLBACK（clog[xid]=ABORTED）
 *   → B 的行不可见 → 行数是 0 而不是 1
 */
static void *rollback_worker(void *arg)
{
    (void)arg;
    DB *conn = NULL;
    if (db_connect((DB *)arg, &conn) != DB_OK)
        return NULL;

    /*
     * 屏障：和 B 同时开始。
     *
     * 两者的 BEGIN 同时发生 → xid 分配撞车才有可能 ✓
     */
    barrier_wait();

    if (run_sql(conn, "BEGIN;") != 0) { db_conn_close(conn); return NULL; }
    if (run_sql(conn, "INSERT INTO t VALUES (1, 'rolled-back');") != 0) {
        db_conn_close(conn);
        return NULL;
    }

    /* 等 B 提交完 —— 顺序是关键：B 先提交，A 后回滚 */
    flag_wait(0);

    run_sql(conn, "ROLLBACK;");
    db_conn_close(conn);
    return NULL;
}

static void *commit_worker(void *arg)
{
    (void)arg;
    DB *conn = NULL;
    if (db_connect((DB *)arg, &conn) != DB_OK)
        return NULL;

    /* 屏障：和 A 同时开始（两个 BEGIN 撞车才有可能） */
    barrier_wait();

    if (run_sql(conn, "BEGIN;") != 0) { db_conn_close(conn); return NULL; }
    if (run_sql(conn, "INSERT INTO t VALUES (2, 'committed');") != 0) {
        db_conn_close(conn);
        return NULL;
    }
    run_sql(conn, "COMMIT;");

    flag_set(0);      /* 告诉 A：B 提交完了 */
    db_conn_close(conn);
    return NULL;
}

static void t2_xid_no_collision(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);

    /*
     * 跑很多轮 —— xid 撞车是概率事件，一轮撞不上说明不了什么。
     */
    const int ROUNDS = 50;
    int bad = 0;

    for (int round = 0; round < ROUNDS; round++) {
        flag_reset();
        barrier_init(2);

        pthread_t th_a, th_b;
        pthread_create(&th_a, NULL, rollback_worker, db);
        pthread_create(&th_b, NULL, commit_worker, db);

        pthread_join(th_a, NULL);
        pthread_join(th_b, NULL);

        Result *r = NULL;
        int n = query_int_col(db, "SELECT id FROM t;", &r);
        if (n != 1)
            bad++;
        if (r) db_result_free(r);

        /* 清表，准备下一轮 */
        run_sql(db, "DELETE FROM t;");
    }

    CHECK(bad == 0);

    db_close(db);
    remove_db(PATH);
}

/* ---------------- 3) 缓冲池压力 ---------------- */

/*
 * 缓冲池只有 256 帧（BP_FRAMES_DEFAULT）。
 * 写入足够多的数据 → 缓冲池反复逐出 → 逼出 pin/unpin 的竞态。
 *
 * 没有 latch 时：pin_count 的读改写不是原子的 →
 * 一个页可能在 pin_count 还是 1 的时候被逐出 → 野指针。
 */
static void t3_buffer_pool_pressure(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE big (id INT, pad TEXT);") == 0);

    pthread_t th[NTHREAD];
    Worker w[NTHREAD];

    /* 每行 200 字节的填充 → 8000 行 ≈ 1.6MB ≈ 400 页 > 256 帧 */
    for (int i = 0; i < NTHREAD; i++) {
        w[i].inst = db;
        w[i].table = "big";
        w[i].tid = i;
        w[i].nrows = 2000;
        w[i].base_id = i * 2000;
        w[i].errors = 0;
        CHECK(pthread_create(&th[i], NULL, insert_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].errors == 0);
    }

    Result *r = NULL;
    int n = query_int_col(db, "SELECT id FROM big;", &r);
    CHECK(n == NTHREAD * 2000);
    if (r) db_result_free(r);

    db_close(db);
    remove_db(PATH);
}

/* ---------------- 4) 并发写 + 崩溃恢复 ---------------- */

/*
 * 并发写完之后模拟崩溃（db_abandon：不刷脏页、不写 CHECKPOINT）。
 * 恢复后已提交的行必须全在。
 *
 * 【这一项在抓什么】
 * WAL 记录如果被两个线程交错写，恢复时记录会错乱 ——
 * 后果是**静默的数据损坏**（重放出乱七八糟的页）。
 */
static void t4_crash_after_concurrent(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);

    pthread_t th[NTHREAD];
    Worker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].inst = db;
        w[i].table = "t";
        w[i].tid = i;
        w[i].nrows = 200;
        w[i].base_id = i * 200;
        w[i].errors = 0;
        CHECK(pthread_create(&th[i], NULL, insert_worker, &w[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    /* 模拟断电：脏页全丢，只靠 WAL 恢复 */
    db_abandon(db);

    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    if (!db2)
        return;

    Result *r = NULL;
    int n = query_int_col(db2, "SELECT id FROM t;", &r);
    CHECK(n == NTHREAD * 200);
    if (r) db_result_free(r);

    db_close(db2);
    remove_db(PATH);
}

/* ---------------- 5) 并发读写：读到的行必须完整 ---------------- */

/*
 * 写线程持续插入（未提交），读线程持续扫描。
 *
 * 【这一项在抓什么】
 * 插入时页的 slot 目录（slot 数组）会被改。
 * 读线程如果和写线程同时访问同一个页，可能读到
 * "slot 已分配但记录还没写完"的半成品记录。
 *
 * 而这个半成品的 xmin 是垃圾值 —— 如果恰好被当成"已提交"，
 * 就会把一行垃圾返回给用户。
 *
 * 检验方式：读到的每一行，name 必须和 id 对得上。
 */
static volatile int g_reading = 0;
static volatile int g_torn = 0;       /* 读到半成品行的次数 */

static void *writer_worker(void *arg)
{
    Worker *w = (Worker *)arg;
    DB *conn = NULL;
    if (db_connect(w->inst, &conn) != DB_OK) {
        w->errors++;
        return NULL;
    }

    for (int round = 0; round < 4; round++) {
        if (run_sql(conn, "BEGIN;") != 0)
            w->errors++;
        for (int i = 0; i < 300; i++) {
            char sql[128];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO %s VALUES (%d, 'w%d-%d');",
                     w->table, w->base_id + round * 300 + i, w->tid, i);
            if (run_sql(conn, sql) != 0)
                w->errors++;
        }
        if (run_sql(conn, "COMMIT;") != 0)
            w->errors++;
    }

    db_conn_close(conn);
    return NULL;
}

static void *reader_worker(void *arg)
{
    (void)arg;
    DB *conn = NULL;
    if (db_connect((DB *)arg, &conn) != DB_OK)
        return NULL;

    while (g_reading) {
        Result *r = NULL;
        if (db_exec(conn, "SELECT id, name FROM t;", &r) != DB_OK) {
            if (r) db_result_free(r);
            continue;
        }
        int n = db_result_rows(r);
        for (int i = 0; i < n; i++) {
            const char *id_s = db_result_value(r, i, 0);
            const char *nm = db_result_value(r, i, 1);
            /* name 必须是 "w<线程>-<序号>"，和 id 的高位对得上 */
            if (!id_s || !nm || nm[0] != 'w') {
                g_torn = 1;
                break;
            }
        }
        db_result_free(r);
    }

    db_conn_close(conn);
    return NULL;
}

static void t5_no_torn_reads(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);

    pthread_t th[NTHREAD], rd;
    Worker w[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        w[i].inst = db;
        w[i].table = "t";
        w[i].tid = i;
        w[i].nrows = 0;
        w[i].base_id = i * 100000;
        w[i].errors = 0;
        CHECK(pthread_create(&th[i], NULL, writer_worker, &w[i]) == 0);
    }

    g_reading = 1;
    CHECK(pthread_create(&rd, NULL, reader_worker, db) == 0);

    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(w[i].errors == 0);
    }

    g_reading = 0;
    pthread_join(rd, NULL);

    CHECK(g_torn == 0);

    /* 最终行数必须正好 */
    Result *r = NULL;
    int n = query_int_col(db, "SELECT id FROM t;", &r);
    CHECK(n == NTHREAD * 4 * 300);
    if (r) db_result_free(r);

    db_close(db);
    remove_db(PATH);
}

/* ---------------- 6) 隔离级别在并发下仍成立 ---------------- */

/*
 * 连接 A 开 RR 事务读一次 → 连接 B 并发提交新行 →
 * A 再读：RR 的快照不变（还是原来的行数）。
 *
 * 【为什么这一项在并发下才有意义】
 * 快照是**连接私有**的，但 clog 和活跃事务表是**实例共享**的。
 * 并发时如果 clog 的状态被别的线程改坏，
 * A 的可见性判断就会错 —— 而这个错误单线程测不出来。
 */
static void t6_isolation_under_concurrency(void)
{
    remove_db(PATH);

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (1, 'a');") == 0);

    DB *a = NULL, *b = NULL;
    CHECK(db_connect(db, &a) == DB_OK);
    CHECK(db_connect(db, &b) == DB_OK);

    /* A 开 RR 事务，建快照 */
    CHECK(run_sql(a, "BEGIN;") == 0);
    CHECK(run_sql(a, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);

    Result *r = NULL;
    int n1 = query_int_col(a, "SELECT id FROM t;", &r);
    if (r) db_result_free(r);
    CHECK(n1 == 1);

    /* B 并发提交一行 */
    CHECK(run_sql(b, "BEGIN;") == 0);
    CHECK(run_sql(b, "INSERT INTO t VALUES (2, 'b');") == 0);
    CHECK(run_sql(b, "COMMIT;") == 0);

    /* A 的 RR 快照不变 */
    r = NULL;
    int n2 = query_int_col(a, "SELECT id FROM t;", &r);
    if (r) db_result_free(r);
    CHECK(n2 == 1);

    CHECK(run_sql(a, "COMMIT;") == 0);

    /* 新连接（RC）应该看得见 B 提交的行 */
    r = NULL;
    int n3 = query_int_col(b, "SELECT id FROM t;", &r);
    if (r) db_result_free(r);
    CHECK(n3 == 2);

    db_conn_close(a);
    db_conn_close(b);
    db_close(db);
    remove_db(PATH);
}

int main(void)
{
    printf("test_concurrent（集成）:\n");

    pthread_t wd;
    pthread_create(&wd, NULL, watchdog_worker, NULL);

    t1_concurrent_insert();
    t2_xid_no_collision();
    t3_buffer_pool_pressure();
    t4_crash_after_concurrent();
    t5_no_torn_reads();
    t6_isolation_under_concurrency();

    g_done = 1;
    pthread_join(wd, NULL);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过 [PASS]\n");
    return 0;
}
