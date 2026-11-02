/*
 * exec_bench.c — 执行器基准与对拍框架（v1.1 步骤 1）
 *
 * ==================== 这个文件干什么 ====================
 *
 * 三件事：
 *
 *   1. **基准**：造 10 万行的表，量"扫描 + 过滤"这条热路径。
 *      —— "先有正确的基准，再谈快"（PLAN §1.0）。
 *
 *   2. **分解**：把查询拆成 scan / decode / filter / 完整查询四段，
 *      各自计时。
 *      —— R2 的原话："先测基准分解（scan/eval/project 各占多少）"。
 *      没有分解就不知道该优化哪一段，只能瞎猜。
 *
 *   3. **对拍**：同一批 SQL，分别走**新执行器**和**冻结的火山执行器**，
 *      逐格比对结果（行数、行序、列名、每格内容）。
 *      —— A1「结果全等」的机器判据。
 *
 * ==================== 为什么对拍是"同进程"而不是"两个二进制" ====================
 *
 * 两个二进制分别跑再比输出文件，看起来一样，但中间隔着：
 * 两次进程启动、两次 WAL 恢复、两次页缓存冷启动。
 * 任何一处不确定（比如 buffer pool 里残留的脏页）都可能让两次跑出不同结果，
 * 而差异会被误判成"执行器不一致" —— 假阳性比漏检更坏。
 *
 * 同进程比对没有这些中间变量：同一个 Db 句柄、同一个缓冲池、同一份快照。
 * 它测的**只有**"两条执行路径的结果是否一样"这一件事。
 *
 * ==================== 计时口径：为什么不走 db_exec ====================
 *
 * db_exec 是公共 API，它一圈下来要做：
 *   latch_lock → parse → 事务开始（mvcc_snapshot）→ exec_stmt → wal_fsync
 *
 * 其中 **wal_fsync** 是一次真实的磁盘同步（毫秒级），
 * 而我们要量的执行器开销是**毫秒以下**的量级。
 * 用 db_exec 量，等于在 3 毫秒的针尖上垫了一台机床 —— 量出来的是 fsync。
 *
 * 所以本基准直接调内核入口 exec_stmt(Db*, Stmt*, ...)：
 *   - 事务在数据装载阶段一次性做完（那才是该付 fsync 的地方）
 *   - 解析也在计时之外（解析是常量开销，对比 before/after 时会互相抵消）
 *   - 计时区间里只剩**执行器本身**
 *
 * before/after 用的是**同一个口径**，所以对比是公平的。
 *
 * ==================== 火山执行器住在哪 ====================
 *
 * bench/exec_volcano.c —— v1.1 开工前 exec_scan.c 里 exec_select 的冻结副本。
 * 它只被链进本基准，不进主二进制（Makefile 的 bench 目标单独编译它）。
 *
 * 用法:
 *   exec_bench [行数] [重复次数]
 *
 *   行数默认 100000（A2 的口径），重复次数默认 5（取最快的一次）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db_internal.h"
#include "query/exec_internal.h"
#include "query/parser.h"

/* 冻结的火山执行器（bench/exec_volcano.c） */
int exec_select_volcano(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int exec_select_agg_ref(Db *db, Stmt *st, QueryResult **out, char *err, int errcap);
int bench_run_diff(Db *kdb);
int bench_run_diff_agg(Db *kdb);

/* ---------------- 计时 ---------------- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ---------------- 参数 ---------------- */

#define DEFAULT_ROWS   100000
#define DEFAULT_REPEAT 5
#define BENCH_PATH     "test/tmp/exec_bench.db"

#define TBL_MAIN  "bench_t"    /* id INT, grp INT, val INT, name TEXT */
#define TBL_NULL  "bench_nt"   /* id INT, val INT（可空） */

/* A2 的主基准查询：10 万行、val 取值 0..999 均匀分布 → val >= 900 命中 10% */
#define MAIN_SQL      "SELECT id, grp FROM bench_t WHERE val >= 900"
#define MAIN_WHERE    "WHERE val >= 900"

/* ---------------- 造数据 ---------------- */

/*
 * 批量 INSERT。
 *
 * 【为什么一次插 16 行】
 * SQL_MAX_ROWS = 16（parser.h 的硬上限），语句数少一点，解析开销小一点。
 *
 * 【为什么包在一个事务里】
 * db_exec 对每条语句默认开一个事务 → 每个事务一次 wal_fsync。
 * 6250 个事务 = 6250 次 fsync，那不是"造数据"，那是"测磁盘"。
 */
static int load_table(DB *db, const char *table, int nrows, bool with_null)
{
    char sql[8192];
    Result *r = NULL;

    db_err rc = db_exec(db, with_null
        ? "CREATE TABLE bench_nt (id INT, val INT)"
        : "CREATE TABLE bench_t (id INT, grp INT, val INT, name TEXT)", &r);
    if (r)
        db_result_free(r);
    if (rc != DB_OK) {
        printf("  建表失败: %s\n", db_last_error(db));
        return -1;
    }

    rc = db_exec(db, "BEGIN", &r);
    if (r) db_result_free(r);
    if (rc != DB_OK) {
        printf("  BEGIN 失败: %s\n", db_last_error(db));
        return -1;
    }

    for (int base = 0; base < nrows; base += 16) {
        int n = 0;
        n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                      "INSERT INTO %s VALUES ", table);
        for (int k = 0; k < 16 && base + k < nrows; k++) {
            int i = base + k;
            if (with_null) {
                /* 每 10 行一个 NULL：给三值逻辑和 IS NULL 留料 */
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,%s)", k ? "," : "", i,
                              (i % 10 == 0) ? "NULL" : "1");
            } else {
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,%d,%d,'row%06d')", k ? "," : "",
                              i, i % 10, i % 1000, i);
            }
        }
        rc = db_exec(db, sql, &r);
        if (r) db_result_free(r);
        if (rc != DB_OK) {
            printf("  插入失败（第 %d 行起）: %s\n", base, db_last_error(db));
            db_exec(db, "ROLLBACK", &r);
            if (r) db_result_free(r);
            return -1;
        }
    }

    rc = db_exec(db, "COMMIT", &r);
    if (r) db_result_free(r);
    if (rc != DB_OK) {
        printf("  COMMIT 失败: %s\n", db_last_error(db));
        return -1;
    }
    return 0;
}

/* ---------------- 相位分解（量"改造前"的构成） ---------------- */

typedef struct {
    double scan_ms;      /* 只有 scan_next */
    double decode_ms;    /* + visible + decode */
    double filter_ms;    /* + WHERE 求值 */
    double full_ms;      /* 完整查询（含结果物化，走公共执行器 exec_stmt） */
    double volcano_ms;   /* 同一查询走冻结的火山执行器 */
    u64    scanned;
    u64    matched;
    int    nresult;
} Phases;

/*
 * 逐段累加计时。
 *
 * 【为什么要分段累加而不是分段单独跑】
 * 分段单独跑会重复付"上一段的成本"（比如 decode 单独跑时还要付 scan 的时间），
 * 差值算出来的"filter 耗时"就偏大。累加测的每一段都是**净增量**。
 *
 * 【where 表达式怎么来】
 * parse_sql 只认完整语句，没法直接解析 "WHERE val >= 900"。
 * 所以把它包成一条完整的 SELECT 再解析，取出 where 子句。
 *
 * 【生命周期的坑】
 * where 指向 wst 里的表达式树，wst 必须活到所有扫描结束才能 free ——
 * 提前 free 就是 use-after-free。
 */
static void measure_phases(Db *kdb, const char *sql, const char *table,
                           const char *where_sql, Phases *out)
{
    memset(out, 0, sizeof(*out));

    char err[256] = {0};
    Stmt st;
    memset(&st, 0, sizeof(st));
    if (parse_sql(sql, &st, err, sizeof(err)) != OK) {
        printf("  解析失败: %s\n", err);
        return;
    }

    TableCtx tc;
    if (resolve_table(kdb, table, &tc, err, sizeof(err)) != OK) {
        printf("  解析表失败: %s\n", err);
        free_stmt(&st);
        return;
    }

    Stmt wst;
    memset(&wst, 0, sizeof(wst));
    Expr *where = NULL;
    if (where_sql && where_sql[0]) {
        char wsql[512];
        snprintf(wsql, sizeof(wsql), "SELECT * FROM %s %s", table, where_sql);
        if (parse_sql(wsql, &wst, err, sizeof(err)) == OK && wst.where) {
            if (bind_expr(wst.where, tc.cols, tc.ncols, err, sizeof(err)) == OK)
                where = wst.where;
            else
                printf("  WHERE 绑定失败: %s\n", err);
        } else {
            printf("  WHERE 解析失败: %s\n", err);
        }
    }

    /*
     * 把执行器自己的 where 也先绑上（exec_select 里会再绑一次，幂等）。
     * 这样第 3 段量的就是**执行器真正在用的那个** where 表达式。
     */
    if (st.where)
        bind_expr(st.where, tc.cols, tc.ncols, err, sizeof(err));

    AmScan *scan = NULL;
    if (open_scan(kdb, &tc, where, &scan) != OK) {
        printf("  开扫描失败\n");
        free_stmt(&wst);
        free_stmt(&st);
        return;
    }

    AmVisCtx vis;
    make_vis_ctx(kdb, &vis);

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    DbValue row[SQL_MAX_COLS];
    Rid rid;

    double t0 = now_ms();
    u64 nscan = 0, nmatch = 0;
    int rc;

    /* 第 1 段：只有 scan_next（pager_get + memcpy） */
    while ((rc = tc.am->scan_next(tc.am, scan, &rid,
                                  recbuf, sizeof(recbuf), &reclen)) == OK) {
        nscan++;
    }
    double t1 = now_ms();
    out->scan_ms = t1 - t0;

    /* 第 2 段：+ visible + decode */
    AmScan *scan2 = NULL;
    if (open_scan(kdb, &tc, where, &scan2) == OK) {
        t0 = now_ms();
        while ((rc = tc.am->scan_next(tc.am, scan2, &rid,
                                      recbuf, sizeof(recbuf), &reclen)) == OK) {
            if (!tc.am->visible(tc.am, recbuf, reclen, &vis))
                continue;
            if (tc.am->decode(tc.am, recbuf, reclen, row) != OK)
                continue;
        }
        t1 = now_ms();
        out->decode_ms = t1 - t0;
        tc.am->scan_end(tc.am, scan2);
    }

    /* 第 3 段：+ WHERE 求值 */
    AmScan *scan3 = NULL;
    if (open_scan(kdb, &tc, where, &scan3) == OK) {
        t0 = now_ms();
        while ((rc = tc.am->scan_next(tc.am, scan3, &rid,
                                      recbuf, sizeof(recbuf), &reclen)) == OK) {
            if (!tc.am->visible(tc.am, recbuf, reclen, &vis))
                continue;
            if (tc.am->decode(tc.am, recbuf, reclen, row) != OK)
                continue;
            if (where) {
                DbValue w;
                if (eval_expr(where, row, &w) != OK)
                    continue;
                if (val_truth(&w) != 1)
                    continue;
            }
            nmatch++;
        }
        t1 = now_ms();
        out->filter_ms = t1 - t0;
        tc.am->scan_end(tc.am, scan3);
    }

    tc.am->scan_end(tc.am, scan);
    out->scanned = nscan;
    out->matched = nmatch;

    /* 第 4 段：完整查询（走公共执行器；含结果物化，不含事务与 fsync） */
    {
        QueryResult *qr = NULL;
        t0 = now_ms();
        int erc = exec_stmt(kdb, &st, &qr, err, sizeof(err));
        t1 = now_ms();
        out->full_ms = t1 - t0;
        if (erc != OK)
            printf("  完整查询失败: %s\n", err);
        else
            out->nresult = qr ? qr->nrows : 0;
        qr_free(qr);
    }

    /* 同一查询走冻结的火山执行器（before/after 的对照基线） */
    {
        QueryResult *qr = NULL;
        t0 = now_ms();
        int erc = exec_select_volcano(kdb, &st, &qr, err, sizeof(err));
        t1 = now_ms();
        out->volcano_ms = t1 - t0;
        if (erc != OK)
            printf("  火山查询失败: %s\n", err);
        qr_free(qr);
    }

    free_stmt(&wst);   /* where 在这里才真正没人用了 */
    free_stmt(&st);
}

/* ---------------- 对拍 ---------------- */

/*
 * 跑一条 SQL，返回结果集。use_volcano 决定走哪条路径。
 */
/* ---------------- 索引一致性门禁 ---------------- */

/*
 * 建索引前后，同一批查询的结果行数必须**完全一样**。
 *
 * 【为什么这是门禁而不是可选检查】
 * v1.1 的执行器基准第一次跑就撞出了这个 bug：
 *   索引 key 原来只编码列值，同值多行共用一个 key，
 *   叶子分裂后内部节点的不变式破了，查找会跳过左子树里的同值行。
 *   症状是**走索引比全扫少行，而且不报任何错**。
 *
 * 只对比"新执行器 vs 火山执行器"抓不到它 ——
 * 两条路径都走同一个索引，错得一模一样。
 * 必须拿"有索引"和"无索引"对比才抓得到。
 */
static const char *const IDX_PROBES[] = {
    "SELECT id FROM bench_t WHERE val = 900",
    "SELECT id FROM bench_t WHERE val = 999",
    "SELECT id FROM bench_t WHERE val >= 899",
    "SELECT id FROM bench_t WHERE val > 899",
    "SELECT id FROM bench_t WHERE val <= 999",
    "SELECT id FROM bench_t WHERE val < 999",
    "SELECT id FROM bench_t WHERE val >= 900",
    "SELECT id FROM bench_t WHERE id = 99999",
    "SELECT id FROM bench_t WHERE id >= 99999",
    "SELECT id FROM bench_t",
};
#define N_IDX_PROBES ((int)(sizeof(IDX_PROBES) / sizeof(IDX_PROBES[0])))

/* 跑一遍探针，把每条查询的行数记进 out[] */
static int probe_counts(Db *kdb, int *out)
{
    int bad = 0;
    for (int i = 0; i < N_IDX_PROBES; i++) {
        QueryResult *qr = NULL;
        char err[256] = {0};
        Stmt st;
        memset(&st, 0, sizeof(st));
        if (parse_sql(IDX_PROBES[i], &st, err, sizeof(err)) != OK) {
            out[i] = -1;
            free_stmt(&st);
            continue;
        }
        if (exec_stmt(kdb, &st, &qr, err, sizeof(err)) != OK) {
            printf("  ✗ %s 执行失败: %s\n", IDX_PROBES[i], err);
            out[i] = -1;
            bad++;
        } else {
            out[i] = qr ? qr->nrows : -1;
        }
        qr_free(qr);
        free_stmt(&st);
    }
    return bad;
}

/* 对比建索引前后的行数。返回不一致的条数。 */
static int check_index_consistency(DB *db, Db *kdb)
{
    int before[N_IDX_PROBES];
    int after[N_IDX_PROBES];

    printf("\n--- 索引一致性门禁（建索引前后行数必须一致）---\n");

    if (probe_counts(kdb, before) != 0)
        return 1;

    /* 建索引：之后同样的查询会走下推扫描 */
    {
        Result *r = NULL;
        db_exec(db, "CREATE INDEX idx_bench_val ON bench_t(val)", &r);
        if (r) db_result_free(r);
    }

    if (probe_counts(kdb, after) != 0)
        return 1;

    int bad = 0;
    for (int i = 0; i < N_IDX_PROBES; i++) {
        if (before[i] != after[i]) {
            printf("  ✗ %s  建索引前 %d 行 / 建索引后 %d 行\n",
                   IDX_PROBES[i], before[i], after[i]);
            bad++;
        }
    }
    if (bad == 0)
        printf("  ✓ %d 条探针查询：建索引前后行数完全一致\n", N_IDX_PROBES);
    return bad;
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    int nrows = argc > 1 ? atoi(argv[1]) : DEFAULT_ROWS;
    int repeat = argc > 2 ? atoi(argv[2]) : DEFAULT_REPEAT;
    if (nrows <= 0)
        nrows = DEFAULT_ROWS;
    if (repeat <= 0)
        repeat = DEFAULT_REPEAT;

    remove(BENCH_PATH);
    remove("test/tmp/exec_bench.db-wal");
    remove("test/tmp/exec_bench.db-shm");

    DB *db = NULL;
    db_err rc = db_open(BENCH_PATH, &db);
    if (rc != DB_OK) {
        printf("开库失败\n");
        return 1;
    }
    Db *kdb = &db->db;

    printf("=== v1.1 执行器基准 ===\n");
    printf("数据：%d 行 × 2 张表（bench_t: 4 列；bench_nt: 2 列含 NULL）\n\n", nrows);

    printf("--- 造数据 ---\n");
    double t0 = now_ms();
    if (load_table(db, TBL_MAIN, nrows, false) != 0) {
        db_close(db);
        return 1;
    }
    if (load_table(db, TBL_NULL, nrows / 10, true) != 0) {
        db_close(db);
        return 1;
    }
    /* 空表：批量边界 */
    {
        Result *r = NULL;
        db_exec(db, "CREATE TABLE bench_empty (id INT, val INT)", &r);
        if (r) db_result_free(r);
    }

    /*
     * 聚合对拍专用的小表：200 行、7 个组。
     *
     * 【为什么不用 bench_t / bench_nt 做 GROUP BY】
     * 它们的分组基数是 10 和 10000：
     *   - 10000 组超过 MAX_GROUPS（4096）→ 当前实现会**明确报错**，
     *     而参照实现没有上限 → 两边行为不同，对拍就红了（但那是设计差异，不是 bug）。
     *   - 分组基数越大，参照实现的线性查找越慢。
     * 所以另建一张基数适中、组内有重复行的小表 ✓
     */
    {
        Result *r = NULL;
        db_exec(db, "CREATE TABLE bench_agg (id INT, grp INT, v INT)", &r);
        if (r) db_result_free(r);

        db_exec(db, "BEGIN", &r);
        if (r) db_result_free(r);
        for (int base = 0; base < 200; base += 16) {
            char sql[1024];
            int n = snprintf(sql, sizeof(sql),
                             "INSERT INTO bench_agg VALUES ");
            for (int k = 0; k < 16 && base + k < 200; k++) {
                int i = base + k;
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,%d,%d)", k ? "," : "", i, i % 7, i * 3);
            }
            db_exec(db, sql, &r);
            if (r) db_result_free(r);
        }
        db_exec(db, "COMMIT", &r);
        if (r) db_result_free(r);
    }
    double t1 = now_ms();
    printf("  装载耗时 %.1f ms\n", t1 - t0);

    /*
     * 刷新快照：装载事务已经提交，但内核里的快照还是事务开始时那份。
     * 跑一条自动事务语句，让它重新取一次快照（快照刷新在 RC 下每语句一次）。
     */
    {
        Result *r = NULL;
        db_exec(db, "SELECT id FROM bench_t LIMIT 1", &r);
        if (r) db_result_free(r);
    }

    /* ---- 相位分解（此时还没有索引 → 纯全扫） ---- */
    printf("\n--- 相位分解（扫描 + 过滤，选择性 10%%，无索引）---\n");
    Phases best;
    memset(&best, 0, sizeof(best));
    for (int i = 0; i < repeat; i++) {
        Phases p;
        measure_phases(kdb, MAIN_SQL, TBL_MAIN, MAIN_WHERE, &p);
        if (i == 0 || p.full_ms < best.full_ms)
            best = p;
    }
    printf("  扫过的行数        : %llu\n", (unsigned long long)best.scanned);
    printf("  匹配的行数        : %llu\n", (unsigned long long)best.matched);
    printf("  结果行数          : %d\n", best.nresult);
    printf("  [1] scan          : %.3f ms\n", best.scan_ms);
    printf("  [2] + decode      : %.3f ms  (净增 %.3f ms)\n",
           best.decode_ms, best.decode_ms - best.scan_ms);
    printf("  [3] + filter      : %.3f ms  (净增 %.3f ms)\n",
           best.filter_ms, best.filter_ms - best.decode_ms);
    printf("  [4] 完整查询      : %.3f ms  (含结果物化)\n", best.full_ms);
    printf("  [4'] 火山执行器   : %.3f ms  (同一查询的对照基线)\n", best.volcano_ms);
    printf("  → 分解：scan %.0f%% / decode %.0f%% / filter %.0f%% / 其余 %.0f%%\n",
           100.0 * best.scan_ms / best.full_ms,
           100.0 * (best.decode_ms - best.scan_ms) / best.full_ms,
           100.0 * (best.filter_ms - best.decode_ms) / best.full_ms,
           100.0 * (best.full_ms - best.filter_ms) / best.full_ms);
    if (best.volcano_ms > 0)
        printf("  → 当前/火山 = %.2fx\n", best.volcano_ms / best.full_ms);

    /* ---- 建索引：让下推路径进入对拍集 + 触发索引一致性门禁 ---- */
    int idx_bad = check_index_consistency(db, kdb);

    /* ---- 对拍 ---- */
    int fails = bench_run_diff(kdb) + bench_run_diff_agg(kdb) + idx_bad;

    printf("\n=== 结论 ===\n");
    if (fails == 0)
        printf("对拍全绿：当前执行器与火山执行器结果逐格全等\n");
    else
        printf("对拍失败 %d 项\n", fails);

    db_close(db);
    return fails == 0 ? 0 : 1;
}
