/*
 * test_vacuum_compact.c — v1.2 步骤 5 集成验收（A5：全表碎片率 < 20%）
 *
 * 3 个阶段：
 *   A. 大量增删改造出死版本 → VACUUM 真的把空间收了回来
 *      （removed / compacted_pages / freed_bytes 都 > 0），
 *      并且**再 VACUUM 一次**报告 frag_pct == 0 → A5 达成
 *   B. 整理不能改数据：活着的行还在、值还对、COUNT 不变、
 *      删掉的行读不到、索引点查还准（RID 没变）
 *   C. 整理后照常能插、能建索引；重启后仍然一致（落盘了）
 *
 * 【为什么"第二次 VACUUM 的 frag_pct == 0"就是 A5 的证据】
 *
 * VACUUM 汇报的 frag_pct 是**整理之前**这张表的碎片率。
 * 所以第一次 VACUUM 报 frag_pct = 50% 并不意味着"整理失败"，
 * 它说的是"我收拾的是多碎的一张表"。
 *
 * 真正说明结果的是**第二次**：
 *   - 如果第一次没整理干净，第二次还会看到碎片（frag_pct > 0，
 *     compacted_pages > 0）；
 *   - 如果第一次整理干净了，第二次 frag_pct == 0、
 *     compacted_pages == 0 —— 碎片率 0% < 20%，A5 达成。
 *
 * 【为什么 UPDATE 也要算进来】
 *
 * UPDATE 产生**新版本**（新 RID），旧版本留在原地等 VACUUM 回收。
 * 所以 UPDATE 和 DELETE 一样会造出页内空洞。
 *
 * 【规模为什么不是计划里写的"10 万次随机增删"】
 *
 * 每条 SQL 语句都是独立事务，提交时都要 fsync 一次日志 ——
 * 10 万条语句光 fsync 就要几分钟，集成测试不能这么跑。
 *
 * 这里用 2000 行 + 750 次改删（约 2750 个事务），
 * 足以把表撑到 20 多页、造出足够密的空洞。
 * "10 万次"级别的压测放在 v1.2 步骤 7 的吞吐基准里做。
 *
 * 【事务数为什么不能太多】
 *
 * clog 是个 4096 槽的环形窗口（见 txn.h），xid 超过 4096 之后
 * 老事务的提交状态会被覆盖 —— 那是 v1.2 步骤 6（clog 落盘）要解决的事。
 * 所以这个测试的事务数必须留在 4096 以内。
 */

#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>

#include "db.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/vacuum_compact.db"

/* 造数据的规模：够散到二十几页上，又不至于让测试跑太久 */
#define NROWS   2000
#define DEL_MOD 4   /* id % 4 == 0 的行被 DELETE */
#define UPD_MOD 8   /* id % 8 == 3 的行被 UPDATE */

/* 原始值：val = id * 3 */
static int base_val(int id) { return id * 3; }

/* UPDATE 之后的值：id % 8 == 3 的行 +1 */
static int live_val(int id)
{
    return base_val(id) + ((id % UPD_MOD == 3) ? 1 : 0);
}

/* 这一行最后是否还活着 */
static bool row_alive(int id)
{
    return (id % DEL_MOD) != 0;
}

/* 造完数据之后活着的行数 */
static int expect_alive_count(void)
{
    int n = 0;
    for (int i = 0; i < NROWS; i++)
        if (row_alive(i))
            n++;
    return n;
}

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

/* VACUUM 的一行六列 */
typedef struct {
    long long scanned;
    long long removed;
    long long kept;
    long long compacted_pages;
    long long freed_bytes;
    long long frag_pct;
} VacStat;

static int do_vacuum(DB *db, VacStat *st)
{
    Result *r = NULL;
    if (db_exec(db, "VACUUM t;", &r) != DB_OK) {
        if (r)
            db_result_free(r);
        return -1;
    }
    if (!r || db_result_rows(r) != 1 || db_result_cols(r) != 6) {
        printf("    FAIL VACUUM 的结果形状不对：rows=%d cols=%d\n",
               r ? db_result_rows(r) : -1, r ? db_result_cols(r) : -1);
        if (r)
            db_result_free(r);
        failures++;
        return -1;
    }
    st->scanned = atoll(db_result_value(r, 0, 0));
    st->removed = atoll(db_result_value(r, 0, 1));
    st->kept = atoll(db_result_value(r, 0, 2));
    st->compacted_pages = atoll(db_result_value(r, 0, 3));
    st->freed_bytes = atoll(db_result_value(r, 0, 4));
    st->frag_pct = atoll(db_result_value(r, 0, 5));
    db_result_free(r);
    return 0;
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

/* 取某一行的 val；取不到返回 -1 */
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

/* ---- 造数据 ---- */
static void load_data(DB *db)
{
    char sql[128];
    for (int i = 0; i < NROWS; i++) {
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, %d, 'note-%d');",
                 i, base_val(i), i);
        if (run_sql(db, sql) != 0) {
            printf("    FAIL 插入第 %d 行失败\n", i);
            failures++;
            return;
        }
    }
}

/* 删一部分、改一部分 —— 造出死版本和页内空洞 */
static void churn(DB *db)
{
    char sql[128];
    for (int i = 0; i < NROWS; i++) {
        if (i % DEL_MOD == 0) {
            snprintf(sql, sizeof(sql), "DELETE FROM t WHERE id = %d;", i);
            if (run_sql(db, sql) != 0) {
                printf("    FAIL 删除 id=%d 失败\n", i);
                failures++;
                return;
            }
        } else if (i % UPD_MOD == 3) {
            snprintf(sql, sizeof(sql), "UPDATE t SET val = %d WHERE id = %d;",
                     live_val(i), i);
            if (run_sql(db, sql) != 0) {
                printf("    FAIL 更新 id=%d 失败\n", i);
                failures++;
                return;
            }
        }
    }
}

/*
 * ---- 阶段 A：VACUUM 收空间 + A5 全表碎片率 < 20% ----
 */
static void phase_a_vacuum(DB *db)
{
    /* 第一次 VACUUM：死版本被回收，空洞被压掉 */
    VacStat v1;
    CHECK(do_vacuum(db, &v1) == 0);
    printf("    第一次 VACUUM：scanned=%lld removed=%lld kept=%lld "
           "compacted=%lld freed=%lld frag=%lld%%\n",
           v1.scanned, v1.removed, v1.kept,
           v1.compacted_pages, v1.freed_bytes, v1.frag_pct);

    CHECK(v1.removed > 0);            /* 死版本确实被回收了 */
    CHECK(v1.compacted_pages > 0);    /* 确实有页被整理了 */
    CHECK(v1.freed_bytes > 0);        /* 确实回收到了空间 */
    CHECK(v1.frag_pct > 0);           /* 整理前确实是碎的 */

    /*
     * 第二次 VACUUM：这一步是 A5 的判据。
     * 如果第一次整理干净了，这里什么空洞都看不到。
     */
    VacStat v2;
    CHECK(do_vacuum(db, &v2) == 0);
    printf("    第二次 VACUUM：scanned=%lld removed=%lld kept=%lld "
           "compacted=%lld freed=%lld frag=%lld%%\n",
           v2.scanned, v2.removed, v2.kept,
           v2.compacted_pages, v2.freed_bytes, v2.frag_pct);

    CHECK(v2.frag_pct == 0);          /* 全表碎片率 0% < 20% → A5 达成 */
    CHECK(v2.compacted_pages == 0);   /* 没有页需要再整理 */
    CHECK(v2.freed_bytes == 0);
    CHECK(v2.removed == 0);           /* 没有死版本残留 */
}

/*
 * ---- 阶段 B：整理不能改数据 ----
 */
static void phase_b_data_intact(DB *db)
{
    /* 活着的行数：被删掉的那些不在了 */
    long long cnt = scalar(db, "SELECT COUNT(*) FROM t;");
    printf("    活着的行数 = %lld（应为 %d）\n", cnt, expect_alive_count());
    CHECK(cnt == expect_alive_count());

    /* 逐行抽查值：整理只挪位置，不改内容 */
    int bad = 0;
    for (int i = 0; i < NROWS; i += 17) {
        if (!row_alive(i))
            continue;
        long long got = row_val(db, i);
        if (got != live_val(i)) {
            if (bad < 5)
                printf("    FAIL id=%d 的值 = %lld，应为 %d\n",
                       i, got, live_val(i));
            bad++;
        }
    }
    CHECK(bad == 0);

    /* 被删掉的行确实读不到了 */
    for (int i = 0; i < NROWS; i += DEL_MOD * 7) {
        long long got = row_val(db, i);
        if (got != -1) {
            printf("    FAIL 已删除的 id=%d 还能读到（val=%lld）\n", i, got);
            failures++;
            break;
        }
    }

    /*
     * 索引点查还准 → RID 没变。
     *
     * 整理把记录在页内挪了位置，但 **slot 编号不变**，
     * 所以索引里的 (页号, slot) 依然指向同一行。
     */
    for (int i = 1; i < NROWS; i += 53) {
        if (!row_alive(i) || i % UPD_MOD == 3)
            continue;
        long long got = row_val(db, i);
        if (got != base_val(i)) {
            printf("    FAIL 索引点查 id=%d 得到 %lld，应为 %d\n",
                   i, got, base_val(i));
            failures++;
            break;
        }
    }
}

/*
 * ---- 阶段 C：整理后照常能写、能建索引；重启后一致 ----
 */
static void phase_c_usable_and_durable(DB *db)
{
    /* 整理过的页要还能继续用：插入、索引、扫描 */
    CHECK(run_sql(db, "INSERT INTO t VALUES (999999, 42, 'after');") == 0);
    CHECK(row_val(db, 999999) == 42);

    long long cnt = scalar(db, "SELECT COUNT(*) FROM t;");
    CHECK(cnt == expect_alive_count() + 1);

    db_close(db);

    /* 重启：整理结果必须落盘了 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    if (!db2)
        return;
    CHECK(scalar(db2, "SELECT COUNT(*) FROM t;") == expect_alive_count() + 1);
    CHECK(row_val(db2, 999999) == 42);

    /* 重启后再 VACUUM：仍然没有碎片 */
    VacStat v2;
    CHECK(do_vacuum(db2, &v2) == 0);
    CHECK(v2.frag_pct == 0);
    CHECK(v2.compacted_pages == 0);

    db_close(db2);
}

int main(void)
{
    printf("test_vacuum_compact（集成）:\n");

    remove(PATH);
    remove("test/tmp/vacuum_compact.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db)
        return 1;

    CHECK(run_sql(db, "CREATE TABLE t (id INT, val INT, note TEXT);") == 0);

    load_data(db);
    churn(db);

    /*
     * 索引建在整理**之前** ——
     * 这样"整理后索引点查还准"才真的在验证 RID 不变。
     */
    CHECK(run_sql(db, "CREATE INDEX idx_t_id ON t (id);") == 0);

    phase_a_vacuum(db);
    phase_b_data_intact(db);
    phase_c_usable_and_durable(db);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  3 项验收全部通过 [PASS]\n");
    return 0;
}
