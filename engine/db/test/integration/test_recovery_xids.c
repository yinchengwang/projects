/*
 * test_recovery_xids.c — 崩溃恢复的 xid 状态表回归测试
 *
 * ==================== 这个测试守的是什么 ====================
 *
 * v1.2 步骤 5 的集成测试撞出来的一个**既有缺陷**：
 *
 *   recovery.c 里收集 xid 提交状态的表是**固定 256 项**的数组
 *   （RecoveredXid xids[REC_MAX_XIDS]）。
 *   事务一多，扫日志时表满了就 `continue` ——
 *   后面的 xid 既没记"已提交"也没记"已中止"。
 *
 *   后果是**静默的数据错误**（不是崩溃，是数据悄悄变错）：
 *
 *     300 次插入 + 150 次删除 = 450 个事务
 *       关闭前 COUNT(*) = 150
 *       重启后 COUNT(*) = 255   ← 删掉的 105 行又"活"了
 *
 *   为什么是"活"而不是"死"：
 *   删除不是物理删除，是给记录打 xmax 标记。
 *   "这行删了没有"这件事记在 xmax 的那个 xid 上，
 *   而那个 xid 的提交状态被丢了 → clog 说"进行中" → 行还可见。
 *
 * 修复：把固定数组换成**按需增长的开放寻址哈希表**
 *       （src/wal/recovery.c 的 rec_put / rec_grow）。
 *
 * ==================== 为什么这个测试只做 400 + 200 ====================
 *
 * 事务数必须**超过 256** 才能压到这个缺陷 —— 少了就测不出来。
 * 也不需要更多：缺陷只跟"事务数有没有超过 256"有关，
 * 与具体数字无关（修复后 450、2750 都在别处验过了）。
 *
 * 另外 clog 本身是个 4096 槽的环形窗口（见 txn.h），
 * xid 超过 4096 之后老事务的提交状态会被覆盖 ——
 * 那是 v1.2 步骤 6（clog 落盘）要解决的事，不在本测试范围内。
 */

#include <stdio.h>
#include <stdlib.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/recovery_xids.db"

#define NINS  400   /* 插入次数 → 400 个事务 */
#define NDEL  200   /* 删除次数 → 再加 200 个事务（合计 600 > 256） */

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

static long long scalar(DB *db, const char *sql)
{
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r)
            db_result_free(r);
        return -1;
    }
    if (!r || db_result_rows(r) != 1) {
        if (r)
            db_result_free(r);
        return -1;
    }
    long long v = atoll(db_result_value(r, 0, 0));
    db_result_free(r);
    return v;
}

static long long row_val(DB *db, int id)
{
    char sql[64];
    snprintf(sql, sizeof(sql), "SELECT val FROM t WHERE id = %d;", id);
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r)
            db_result_free(r);
        return -1;
    }
    if (!r || db_result_rows(r) != 1) {
        if (r)
            db_result_free(r);
        return -1;
    }
    long long v = atoll(db_result_value(r, 0, 0));
    db_result_free(r);
    return v;
}

int main(void)
{
    printf("test_recovery_xids（集成）:\n");

    remove(PATH);
    remove("test/tmp/recovery_xids.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return 1;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, val INT);") == 0);

    char sql[128];
    for (int i = 0; i < NINS; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d);", i, i * 3);
        CHECK(run_sql(db, sql) == 0);
    }
    /* 删掉偶数 id —— 每一行都是"打了 xmax 标记"，靠 clog 才能判死 */
    for (int i = 0; i < NINS; i += 2) {
        snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = %d;", i);
        CHECK(run_sql(db, sql) == 0);
    }

    long long before = scalar(db, "SELECT COUNT(*) FROM t;");
    printf("    事务数 = %d，关闭前行数 = %lld\n", NINS + NDEL, before);
    CHECK(before == NINS - NDEL);

    db_close(db);

    /* 重启：clog 是从日志重建的 —— xid 状态一个都不能丢 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    if (!db2)
        return 1;

    long long after = scalar(db2, "SELECT COUNT(*) FROM t;");
    printf("    重启后行数 = %lld\n", after);
    CHECK(after == before);

    /* 活着的行（奇数 id）：值必须还在 */
    for (int i = 1; i < NINS; i += 74) {
        long long got = row_val(db2, i);
        if (got != i * 3) {
            printf("    FAIL 重启后 id=%d 的值 = %lld，应为 %d\n", i, got, i * 3);
            failures++;
            break;
        }
    }

    /* 删掉的行：必须还是死的 */
    for (int i = 0; i < NINS; i += 74) {
        long long got = row_val(db2, i);
        if (got != -1) {
            printf("    FAIL 重启后已删除的 id=%d 还能读到（val=%lld）\n", i, got);
            failures++;
            break;
        }
    }

    db_close(db2);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  1 项验收全部通过 [PASS]\n");
    return 0;
}
