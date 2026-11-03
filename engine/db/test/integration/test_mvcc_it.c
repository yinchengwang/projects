/*
 * test_mvcc.c — MVCC 端到端验收（两个连接）
 *
 * 7 项：
 *   1. 写方未提交 → 读方看不见（不脏读）
 *   2. 写方提交 → 读方（RC）看得见
 *   3. 读方（RR）事务中途重新读 → 仍看不见事务开始后提交的数据
 *   4. ROLLBACK → 撤销本事务的所有修改
 *   5. UPDATE 产生新版本 → 旧快照看旧值，新快照看新值
 *   6. DELETE → 旧快照仍可见，新快照不可见
 *   7. 重启后可见性一致（clog 从日志重建）
 *
 * 【为什么用 db_connect 而不是再 db_open 一次】
 *
 * 这是 v0.7 踩过的一个大坑，值得记下来：
 *
 * 我最初用 db_open(path) 开两个句柄来模拟两个连接。
 * 结果读方看不到写方建的表（SELECT 报"表不存在"）。
 *
 * 原因不是 bug，是架构的必然：
 *   每个 db_open 都会**独立地**打开缓冲池、加载 catalog 副本、
 *   重建 freelist。两个句柄其实是两个互不相通的数据库实例。
 *
 * 正确的模型是 PG 的 postmaster/backend：
 *   一个实例（共享缓冲池 + catalog + WAL + 事务表）
 *   多个连接（各自的事务号、快照、事务状态）
 *
 * 所以 v0.7 加了 db_connect：共享实例资源，只分离连接级状态。
 *
 * 【为什么不用两个线程】
 *
 * v0.7 的并发模型是「全局 latch」但代码仍是单线程 ——
 * 先把隔离级别的**语义**（谁能看到什么）测对，
 * 再测实现（并发下的实际表现）。语义对了，实现才可能对。
 * 线程级的测试留给 v0.8 加了真 latch 之后。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/mvcc_it.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

static int count_rows(DB *db)
{
    Result *r = NULL;
    if (db_exec(db, "SELECT id FROM t;", &r) != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

static int has_id(DB *db, int id)
{
    char sql[128];
    snprintf(sql, sizeof(sql), "SELECT id FROM t WHERE id = %d;", id);
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

/* ---- 1 & 2) 未提交不可见，提交后可见 ---- */
static void t1_dirty_read(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *r = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &r) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 'a');") == 0);

    /* 写方开事务，插入但不提交 */
    CHECK(run_sql(w, "BEGIN;") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (2, 'b');") == 0);

    /* 读方看不见未提交的数据 —— 这就是"不脏读" */
    CHECK(count_rows(r) == 1);
    CHECK(has_id(r, 2) == 0);

    /* 写方提交 */
    CHECK(run_sql(w, "COMMIT;") == 0);

    /* 读方现在看得见 */
    CHECK(count_rows(r) == 2);
    CHECK(has_id(r, 2) == 1);

    db_conn_close(r);
    db_close(w);
}

/* ---- 3) RR：快照固定 ---- */
static void t2_repeatable_read(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *r = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &r) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 'a');") == 0);

    /* 读方开事务并显式声明 RR（默认是 RC，会看到别人刚提交的数据）*/
    CHECK(run_sql(r, "BEGIN;") == 0);
    CHECK(run_sql(r, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);
    CHECK(count_rows(r) == 1);

    /* 写方插入并提交 */
    CHECK(run_sql(w, "INSERT INTO t VALUES (2, 'b');") == 0);

    /* 读方在同一个事务里重新读 —— 应该还是 1 行（快照没变） */
    CHECK(count_rows(r) == 1);

    /* 读方提交后（结束 RR 事务），新快照才看得到 */
    CHECK(run_sql(r, "COMMIT;") == 0);
    CHECK(count_rows(r) == 2);

    db_conn_close(r);
    db_close(w);
}

/* ---- 4) ROLLBACK 撤销修改 ---- */
static void t3_rollback(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (1, 'a');") == 0);
    CHECK(count_rows(db) == 1);

    /* 开事务，插入一行，然后回滚 */
    CHECK(run_sql(db, "BEGIN;") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (2, 'b');") == 0);
    CHECK(count_rows(db) == 2);      /* 事务内自己看得见 */
    CHECK(run_sql(db, "ROLLBACK;") == 0);

    /* 回滚后应该恢复成 1 行 */
    CHECK(count_rows(db) == 1);
    CHECK(has_id(db, 2) == 0);

    /* 数据在磁盘上也应该是回滚后的状态 */
    db_close(db);

    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    CHECK(count_rows(db2) == 1);
    db_close(db2);
}

/* ---- 5) UPDATE 产生新版本，旧快照仍看到旧值 ---- */
static void t4_update_version(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *r = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &r) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, val INT);") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 100);") == 0);

    /* 读方开事务，显式声明 RR（快照固定）*/
    CHECK(run_sql(r, "BEGIN;") == 0);
    CHECK(run_sql(r, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);

    Result *res = NULL;
    CHECK(db_exec(r, "SELECT val FROM t WHERE id = 1;", &res) == DB_OK);
    CHECK(db_result_rows(res) == 1);
    CHECK(strcmp(db_result_value(res, 0, 0), "100") == 0);
    db_result_free(res);

    /* 写方更新并提交 */
    CHECK(run_sql(w, "UPDATE t SET val = 200 WHERE id = 1;") == 0);

    /* 读方的快照里还是旧值 */
    res = NULL;
    CHECK(db_exec(r, "SELECT val FROM t WHERE id = 1;", &res) == DB_OK);
    CHECK(db_result_rows(res) == 1);
    CHECK(strcmp(db_result_value(res, 0, 0), "100") == 0);
    db_result_free(res);

    /* 读方结束事务后，新快照看到新值 */
    CHECK(run_sql(r, "COMMIT;") == 0);
    res = NULL;
    CHECK(db_exec(r, "SELECT val FROM t WHERE id = 1;", &res) == DB_OK);
    CHECK(db_result_rows(res) == 1);
    CHECK(strcmp(db_result_value(res, 0, 0), "200") == 0);
    db_result_free(res);

    db_conn_close(r);
    db_close(w);
}

/* ---- 6) DELETE 后旧快照仍可见 ---- */
static void t5_delete_visibility(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *r = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &r) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 'a');") == 0);

    CHECK(run_sql(r, "BEGIN;") == 0);
    CHECK(run_sql(r, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);
    CHECK(count_rows(r) == 1);

    CHECK(run_sql(w, "DELETE FROM t WHERE id = 1;") == 0);

    /* 读方的快照里这一行还在（删除对它不可见） */
    CHECK(count_rows(r) == 1);

    CHECK(run_sql(r, "COMMIT;") == 0);
    CHECK(count_rows(r) == 0);

    db_conn_close(r);
    db_close(w);
}

/* ---- 7) 重启后可见性一致（clog 从日志重建） ---- */
static void t6_restart_consistency(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (1, 'a');") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (2, 'b');") == 0);
    CHECK(run_sql(db, "UPDATE t SET name = 'B' WHERE id = 2;") == 0);
    db_close(db);

    /*
     * 重启后 clog 必须从日志重建 —— 否则所有历史事务都会被当成
     * "进行中"，重启后数据全都看不见（这是 v0.7 最容易踩的坑）。
     */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    CHECK(count_rows(db2) == 2);

    Result *res = NULL;
    CHECK(db_exec(db2, "SELECT name FROM t WHERE id = 2;", &res) == DB_OK);
    CHECK(db_result_rows(res) == 1);
    CHECK(strcmp(db_result_value(res, 0, 0), "B") == 0);
    db_result_free(res);

    db_close(db2);
}

/* ---- 8) WAL 的 xid 不能跨连接串号（回归测试） ---- */

/*
 * 【这个 bug 有多严重】
 *
 * 最初 WAL 的 xid 是对象上的共享状态（wal_set_xid(w, xid)）。
 * WAL 是实例级共享的，所以：
 *   连接 A：wal_set_xid(w, 5)
 *   连接 B：wal_set_xid(w, 6)      ← 覆盖了 A 的设置
 *   连接 A 写数据 → 日志记录的 xid 是 6，不是 5
 *
 * 后果：崩溃恢复时分析"哪些事务提交了"，A 的记录被算到 B 名下。
 * A 明明没提交，恢复后却被当成已提交 —— **未提交的数据留在了库里**。
 *
 * 表现是：事务 A 回滚了，但重启后 A 写的行还在。
 * 这类 bug 极难查：功能测试全过，只有"回滚 + 崩溃 + 重启"才暴露。
 */
static void t7_wal_xid_not_shared(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *r = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &r) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, name TEXT);") == 0);

    /* 连接 A 开事务，写入但不提交 */
    CHECK(run_sql(w, "BEGIN;") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 'uncommitted');") == 0);

    /*
     * 连接 B 开事务并提交 —— 这一步会覆盖共享状态里的 xid。
     * 在有 bug 的实现里，A 的日志记录会带上 B 的 xid。
     */
    CHECK(run_sql(r, "BEGIN;") == 0);
    CHECK(run_sql(r, "INSERT INTO t VALUES (2, 'committed');") == 0);
    CHECK(run_sql(r, "COMMIT;") == 0);

    /* A 回滚 */
    CHECK(run_sql(w, "ROLLBACK;") == 0);

    /* 正常情况下：A 的行已经不可见（回滚了），B 的行可见 */
    CHECK(count_rows(w) == 1);
    CHECK(has_id(w, 1) == 0);
    CHECK(has_id(w, 2) == 1);

    /*
     * 关键：模拟崩溃（不刷脏页、不写 CHECKPOINT），再重开。
     * 恢复时如果 xid 串了号，A 那条未提交的记录会被当成
     * "属于 B 的已提交事务" → 数据复活。
     */
    db_conn_close(r);
    db_abandon(w);

    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    CHECK(count_rows(db2) == 1);
    CHECK(has_id(db2, 1) == 0);      /* 未提交的绝不能复活 */
    CHECK(has_id(db2, 2) == 1);
    db_close(db2);
}

/*
 * ---- 8) RC 与 RR 的差异（同一场景，两个级别，两种结果）----
 *
 * 这是 v0.7 最重要的一项验证：**两种隔离级别的差别必须是可观测的**。
 *
 * 场景完全一样，只改一个 SET TRANSACTION：
 *
 *   读方 BEGIN
 *   读方 SELECT            → 看到 1 行
 *   写方 INSERT + COMMIT   → 库里变成 2 行
 *   读方 SELECT            → RC 看到 2 行，RR 看到 1 行
 *
 * 如果两个级别表现一样，说明隔离级别根本没生效 —— 这类
 * "实现了但没起作用"的 bug 比"没实现"更危险，因为它看起来一切正常。
 *
 * 为什么 RC 也能在同一事务里看到别人刚提交的：
 * RC 的语义是"每条语句一个快照"，不是"整个事务一个快照"。
 */
static void t8_rc_vs_rr(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *w = NULL, *rc_conn = NULL, *rr_conn = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    CHECK(db_connect(w, &rc_conn) == DB_OK);
    CHECK(db_connect(w, &rr_conn) == DB_OK);

    CHECK(run_sql(w, "CREATE TABLE t (id INT, name TEXT);") == 0);
    CHECK(run_sql(w, "INSERT INTO t VALUES (1, 'a');") == 0);

    /* --- RC 连接：默认级别就是 RC --- */
    CHECK(run_sql(rc_conn, "BEGIN;") == 0);
    CHECK(count_rows(rc_conn) == 1);

    /* --- RR 连接：显式声明 --- */
    CHECK(run_sql(rr_conn, "BEGIN;") == 0);
    CHECK(run_sql(rr_conn, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);
    CHECK(count_rows(rr_conn) == 1);

    /* 写方插入并提交 —— 两个读方都还没重新读 */
    CHECK(run_sql(w, "INSERT INTO t VALUES (2, 'b');") == 0);

    /* RC：每条语句刷新快照 → 看得见新提交的行 */
    CHECK(count_rows(rc_conn) == 2);
    CHECK(has_id(rc_conn, 2) == 1);

    /* RR：快照固定在事务开始时 → 看不见 */
    CHECK(count_rows(rr_conn) == 1);
    CHECK(has_id(rr_conn, 2) == 0);

    /* RR 结束事务后，新快照才看得到 */
    CHECK(run_sql(rr_conn, "COMMIT;") == 0);
    CHECK(count_rows(rr_conn) == 2);

    CHECK(run_sql(rc_conn, "COMMIT;") == 0);

    db_conn_close(rr_conn);
    db_conn_close(rc_conn);
    db_close(w);
}

/*
 * ---- 9) 隔离级别的设置规则 ----
 *
 * PG 的规则：
 *   - SET TRANSACTION 必须在事务内
 *   - 一个事务内只能设一次
 */
static void t9_isolation_rules(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);

    /* 事务外设置 → 报错 */
    CHECK(run_sql(db, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") != 0);

    /* 事务内设置 → 成功 */
    CHECK(run_sql(db, "BEGIN;") == 0);
    CHECK(run_sql(db, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);

    /* 同一事务内再设一次 → 报错 */
    CHECK(run_sql(db, "SET TRANSACTION ISOLATION LEVEL READ COMMITTED;") != 0);

    CHECK(run_sql(db, "COMMIT;") == 0);

    /* 新事务可以重新设置（SET 只对当前事务生效）*/
    CHECK(run_sql(db, "BEGIN;") == 0);
    CHECK(run_sql(db, "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ;") == 0);
    CHECK(run_sql(db, "COMMIT;") == 0);

    db_close(db);
}

/*
 * ---- 10) VACUUM 的 SQL 入口 ----
 *
 * 场景：
 *   插入一行 → UPDATE 它（产生旧版本）→ DELETE 它（再打一个 xmax）
 *   → VACUUM → 死版本被回收
 *
 * 【一个必须注意的前提】
 *
 * mvcc_is_dead 的保守规则是"只要还有别的活跃事务就不动"。
 * 而 VACUUM 自己跑在一个事务里 —— 这个事务就是活跃事务！
 *
 * 所以判断"还有没有活跃事务"时**必须排除执行者自己**。
 * 否则 vacuum 变成一个只会空转的语句（永远回收不掉任何东西）。
 *
 * 这就是 mvcc_is_dead_except 的存在理由。
 */
static void t10_vacuum_sql(void)
{
    remove(PATH);
    remove("test/tmp/mvcc_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    CHECK(run_sql(db, "CREATE TABLE t (id INT, val INT);") == 0);
    CHECK(run_sql(db, "INSERT INTO t VALUES (1, 100);") == 0);

    /* UPDATE 产生旧版本；DELETE 产生更多死版本 */
    CHECK(run_sql(db, "UPDATE t SET val = 200 WHERE id = 1;") == 0);
    CHECK(run_sql(db, "DELETE FROM t WHERE id = 1;") == 0);

    /* 表里现在 0 行可见，但物理上有 3 个版本 */
    CHECK(count_rows(db) == 0);

    /* VACUUM 能执行（SQL 入口通）*/
    Result *r = NULL;
    CHECK(db_exec(db, "VACUUM t;", &r) == DB_OK);
    CHECK(r != NULL);
    if (r) {
        CHECK(db_result_rows(r) == 1);
        /*
         * 六列（v1.2 步骤 5 起）：
         *   scanned / removed / kept / compacted_pages / freed_bytes / frag_pct
         * 前三列是老的三列，后面三列是页内 compact 的成果。
         */
        CHECK(db_result_cols(r) == 6);
        /*
         * 物理版本数是 2，不是 3：
         *   原始版本（INSERT）→ 被 UPDATE 替换，打 xmax
         *   UPDATE 的新版本    → 被 DELETE 标记，打 xmax
         * DELETE 不新增版本，只是给现有版本打标记。
         *
         * 两个都是死版本 → removed = 2，kept = 0。
         */
        CHECK(atoi(db_result_value(r, 0, 0)) == 2);
        CHECK(atoi(db_result_value(r, 0, 1)) == 2);
        CHECK(atoi(db_result_value(r, 0, 2)) == 0);
        db_result_free(r);
    }

    /* VACUUM 之后数据依然正确（0 行可见）*/
    CHECK(count_rows(db) == 0);

    /* 重启后也一致 */
    db_close(db);
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    CHECK(count_rows(db2) == 0);
    db_close(db2);
}

int main(void)
{
    printf("test_mvcc（集成）:\n");

    t1_dirty_read();
    t2_repeatable_read();
    t3_rollback();
    t4_update_version();
    t5_delete_visibility();
    t6_restart_consistency();
    t7_wal_xid_not_shared();
    t8_rc_vs_rr();
    t9_isolation_rules();
    t10_vacuum_sql();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  10 项验收全部通过 [PASS]\n");
    return 0;
}
