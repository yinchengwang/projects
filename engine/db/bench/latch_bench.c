/*
 * latch_bench.c —— A3：细粒度 latch vs 大锁基线的吞吐对照
 *
 * ==================== 这个基准要回答一个问题 ====================
 *
 * 把 db_exec 的全局大锁退役之后，吞吐到底提升了多少？
 * 计划的 A3 判据是：**≥ 2 倍**（8:2 读写混合、4 线程）。
 *
 * ==================== 两个二进制 ====================
 *
 *   latch_bench          当前源码（细粒度 latch）
 *   latch_bench_biglock  同一份源码 + -DDB_BIGLOCK_BASELINE
 *                        （把步骤 4d 退役的全局大锁原样放回去）
 *
 * **同一份源码**是刻意的：只有锁的范围不同，其他一切（页格式、WAL、
 * compact、执行器）完全一样，比值才只反映"锁的粒度"这一件事。
 *
 * 为什么要重编一遍而不是加个运行期开关：
 * 运行期开关会让两种模式跑在同一个进程里 —— 页缓存、缓冲池、WAL 状态
 * 全部互相污染，比值就不干净了。两次独立进程、两次独立库文件，才是对照。
 *
 * ==================== 负载设计：为什么这么绕 ====================
 *
 * **① 8:2 读写混合**
 *   每轮 10 条语句：8 条读 + 2 条写。计划的 A3 原文。
 *
 * **② 每个线程一张自己的表**
 *   四张表 lb_t0..lb_t3，各 5 万行。
 *   为什么要分开：**四个线程扫同一张表时，它们会在同一批页上争内容锁** ——
 *   那样测出来的就不是"锁的粒度"，而是"同一页的争用"。
 *   A3 想量的是前者。
 *
 * **③ 读要足够重**
 *   读是 `SELECT id FROM lb_tK WHERE grp = g` —— 全表扫 5 万行，命中 1%。
 *   理由：读太轻的话，一次 commit 的 fsync（毫秒级）会把读的时间盖掉，
 *   比值就变成"量磁盘"而不是"量锁"。
 *   所以读必须重到 fsync 可以忽略 —— 这也是下面第 ④ 条的原因。
 *
 * **④ 事务开得大**
 *   一个线程的整轮次跑在**一个事务**里，每 10 轮提交一次。
 *   理由同上：fsync 的次数要少到可以忽略。
 *   计划的 A3 没有规定事务粒度，这里显式写清楚，免得读者以为是自动提交。
 *
 * **⑤ 预热一轮不计时**
 *   第一轮用来把页读进缓冲池、让分支预测器热起来。
 *   计时从预热之后开始。
 *
 * ==================== 输出 ====================
 *
 * 机器可读的一行（给 bench/latch_bench_run.sh 抓）：
 *
 *   RESULT ops_per_sec=... elapsed_ms=... statements=... reads=... writes=... errors=...
 *
 * 用法：
 *   latch_bench [每表行数] [每线程轮数] [线程数]
 *   默认 50000 / 30 / 4
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"

/* ---------------- 参数 ---------------- */

#define DEFAULT_ROWS    50000
#define DEFAULT_ROUNDS  30
#define DEFAULT_THREADS 4

#define READS_PER_ROUND  8
#define WRITES_PER_ROUND 2
#define COMMIT_EVERY     10      /* 每 10 轮提交一次（压 fsync 的噪音） */

#define BENCH_PATH "test/tmp/latch_bench.db"
#define TBL_FMT    "lb_t%d"
#define GRP_MOD    100          /* grp 的取值范围 → 读的选择性 1% */

/* 一条语句最多 16 行（parser.h 的硬上限） */
#define INSERT_BATCH 16

/* ---------------- 计时 ---------------- */

static double now_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

/* ---------------- 线程参数 ---------------- */

typedef struct {
    DB    *db;          /* 实例（共享） */
    int    tid;         /* 线程号 → 自己的表 */
    int    rounds;
    int    errors;
    long   statements;  /* 本线程执行的语句数 */
    double t_read;      /* 读语句累计耗时（ms） */
    double t_write;     /* 写语句累计耗时（ms） */
    double t_txn;       /* BEGIN/COMMIT 累计耗时（ms）—— fsync 在 COMMIT 里 */
} Worker;

static void exec_count(Worker *w, DB *conn, const char *sql)
{
    Result *r = NULL;
    if (db_exec(conn, sql, &r) != DB_OK) {
        w->errors++;
        fprintf(stderr, "  [线程 %d] 语句失败: %s\n    SQL: %s\n",
                w->tid, db_last_error(conn), sql);
    }
    if (r)
        db_result_free(r);
    w->statements++;
}

/* 事务控制语句（BEGIN / COMMIT）：单独计时 —— fsync 在 COMMIT 里 */
static void exec_txn(Worker *w, DB *conn, const char *sql)
{
    double t0 = now_ms();
    exec_count(w, conn, sql);
    w->t_txn += now_ms() - t0;
}

/* ---------------- 负载 ---------------- */

/*
 * 一轮 = 8 条读 + 2 条写。
 *
 * 写的 id 用 (tid, 轮次, 序号) 拼出来 —— 全局唯一，不撞主键。
 */
static void run_round(Worker *w, DB *conn, const char *tbl, int round)
{
    char sql[256];

    for (int i = 0; i < READS_PER_ROUND; i++) {
        int g = (w->tid * 7 + round * 3 + i) % GRP_MOD;
        snprintf(sql, sizeof(sql), "SELECT id FROM %s WHERE grp = %d", tbl, g);
        double t0 = now_ms();
        exec_count(w, conn, sql);
        w->t_read += now_ms() - t0;
    }

    for (int i = 0; i < WRITES_PER_ROUND; i++) {
        int id = w->tid * 1000000 + round * 100 + i;
        int g = id % GRP_MOD;
        snprintf(sql, sizeof(sql), "INSERT INTO %s VALUES (%d, %d)", tbl, id, g);
        double t0 = now_ms();
        exec_count(w, conn, sql);
        w->t_write += now_ms() - t0;
    }
}

static void *worker(void *arg)
{
    Worker *w = (Worker *)arg;
    DB *conn = NULL;

    if (db_connect(w->db, &conn) != DB_OK) {
        w->errors++;
        return NULL;
    }

    char tbl[32];
    snprintf(tbl, sizeof(tbl), TBL_FMT, w->tid);

    /* 预热一轮：把页读进缓冲池，不计时 */
    exec_txn(w, conn, "BEGIN");
    run_round(w, conn, tbl, -1);
    exec_txn(w, conn, "COMMIT");
    w->statements = 0;      /* 预热不计入 */
    w->t_read = w->t_write = w->t_txn = 0.0;

    exec_txn(w, conn, "BEGIN");
    for (int r = 0; r < w->rounds; r++) {
        run_round(w, conn, tbl, r);
        if ((r + 1) % COMMIT_EVERY == 0 && r + 1 < w->rounds) {
            exec_txn(w, conn, "COMMIT");
            exec_txn(w, conn, "BEGIN");
        }
    }
    exec_txn(w, conn, "COMMIT");

    db_conn_close(conn);
    return NULL;
}

/* ---------------- 造数据 ---------------- */

/*
 * 装载一张表。
 *
 * 【为什么要包在一个事务里】
 * db_exec 对每条语句默认开一个事务 → 每个事务一次 wal_fsync。
 * 3125 个事务 = 3125 次 fsync —— 那不是"造数据"，那是"测磁盘"。
 */
static int load_table(DB *db, const char *table, int nrows)
{
    char sql[8192];
    Result *r = NULL;

    char create[128];
    snprintf(create, sizeof(create), "CREATE TABLE %s (id INT, grp INT)", table);
    if (db_exec(db, create, &r) != DB_OK) {
        printf("  建表失败: %s\n", db_last_error(db));
        if (r) db_result_free(r);
        return -1;
    }
    if (r) db_result_free(r);

    if (db_exec(db, "BEGIN", &r) != DB_OK) {
        printf("  BEGIN 失败: %s\n", db_last_error(db));
        if (r) db_result_free(r);
        return -1;
    }
    if (r) db_result_free(r);

    for (int base = 0; base < nrows; base += INSERT_BATCH) {
        int n = 0;
        n += snprintf(sql + n, sizeof(sql) - (size_t)n, "INSERT INTO %s VALUES ", table);
        for (int k = 0; k < INSERT_BATCH && base + k < nrows; k++) {
            int i = base + k;
            n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                          "%s(%d,%d)", k ? "," : "", i, i % GRP_MOD);
        }
        if (db_exec(db, sql, &r) != DB_OK) {
            printf("  插入失败（第 %d 行起）: %s\n", base, db_last_error(db));
            if (r) db_result_free(r);
            db_exec(db, "ROLLBACK", &r);
            if (r) db_result_free(r);
            return -1;
        }
        if (r) db_result_free(r);
    }

    if (db_exec(db, "COMMIT", &r) != DB_OK) {
        printf("  COMMIT 失败: %s\n", db_last_error(db));
        if (r) db_result_free(r);
        return -1;
    }
    if (r) db_result_free(r);
    return 0;
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv)
{
    int rows    = argc > 1 ? atoi(argv[1]) : DEFAULT_ROWS;
    int rounds  = argc > 2 ? atoi(argv[2]) : DEFAULT_ROUNDS;
    int nthread = argc > 3 ? atoi(argv[3]) : DEFAULT_THREADS;

    if (rows <= 0)    rows = DEFAULT_ROWS;
    if (rounds <= 0)  rounds = DEFAULT_ROUNDS;
    if (nthread <= 0) nthread = DEFAULT_THREADS;
    if (nthread > 16) nthread = 16;

    /*
     * 缓冲池：装得下所有表。
     *
     * 理由与 exec_bench 相同：池装不下数据时，每次扫表都要把表从磁盘读一遍，
     * 量出来的是 fread，不是锁。
     * 每行约 24 字节，一页 4KB 装约 150 行 → 每表 pages ≈ rows/150。
     */
    {
        long pages = (long)rows / 150 + 64;
        long frames = pages * (long)nthread * 2;
        if (frames < 4096) frames = 4096;
        /*
         * 外部已经设了 DB_BP_FRAMES 就用它 ——
         * 诊断时需要把池调得很大（排除逐出的影响）。
         */
        if (!getenv("DB_BP_FRAMES")) {
            static char buf[64];   /* putenv 的参数必须活到进程结束 */
            snprintf(buf, sizeof(buf), "DB_BP_FRAMES=%ld", frames);
            putenv(buf);
        } else {
            frames = atol(getenv("DB_BP_FRAMES"));
        }
        printf("缓冲池：%ld 帧（%.1f MB）\n", frames, frames * 4.0 / 1024.0);
    }

    printf("=== A3 latch 吞吐基准 ===\n");
    printf("配置：%d 线程 × %d 轮 × (%d 读 + %d 写)，每表 %d 行\n\n",
           nthread, rounds, READS_PER_ROUND, WRITES_PER_ROUND, rows);

    /* --- 造数据（不计时） --- */
    double t0 = now_ms();
    DB *db = NULL;
    if (db_open(BENCH_PATH, &db) != DB_OK) {
        printf("开库失败\n");
        return 1;
    }

    for (int t = 0; t < nthread; t++) {
        char tbl[32];
        snprintf(tbl, sizeof(tbl), TBL_FMT, t);
        if (load_table(db, tbl, rows) != 0) {
            db_close(db);
            return 1;
        }
    }
    printf("  装载耗时 %.1f ms\n", now_ms() - t0);

    /* --- 计时阶段 --- */
    pthread_t *th = calloc((size_t)nthread, sizeof(pthread_t));
    Worker *w = calloc((size_t)nthread, sizeof(Worker));
    if (!th || !w) {
        printf("内存不足\n");
        db_close(db);
        return 1;
    }

    printf("  跑负载...\n");
    t0 = now_ms();
    for (int i = 0; i < nthread; i++) {
        w[i].db = db;
        w[i].tid = i;
        w[i].rounds = rounds;
        w[i].errors = 0;
        w[i].statements = 0;
        if (pthread_create(&th[i], NULL, worker, &w[i]) != 0) {
            printf("  建线程失败\n");
            free(th);
            free(w);
            db_close(db);
            return 1;
        }
    }
    for (int i = 0; i < nthread; i++)
        pthread_join(th[i], NULL);
    double elapsed = now_ms() - t0;

    long statements = 0;
    int errors = 0;
    double t_read = 0.0, t_write = 0.0, t_txn = 0.0;
    for (int i = 0; i < nthread; i++) {
        statements += w[i].statements;
        errors += w[i].errors;
        t_read += w[i].t_read;
        t_write += w[i].t_write;
        t_txn += w[i].t_txn;
    }

    double ops = (double)statements / (elapsed / 1000.0);

    printf("\n--- 结果 ---\n");
    printf("  耗时        : %.1f ms\n", elapsed);
    printf("  语句数      : %ld（其中读 %d、写 %d，每轮 8:2）\n",
           statements, (int)(statements * READS_PER_ROUND / (READS_PER_ROUND + WRITES_PER_ROUND)),
           (int)(statements * WRITES_PER_ROUND / (READS_PER_ROUND + WRITES_PER_ROUND)));
    printf("  失败语句    : %d\n", errors);
    printf("  吞吐        : %.1f 条语句/秒\n", ops);
    /*
     * 分相位耗时（各线程累加，所以总和可以大于墙钟时间）。
     *
     * 这一行是**诊断用的**：如果"事务"那一项占了大头，
     * 说明量的是 fsync 而不是锁 —— 判据就废了，必须把负载调重。
     */
    printf("  分相位      : 读 %.1f ms / 写 %.1f ms / 事务 %.1f ms（各线程累加）\n",
           t_read, t_write, t_txn);

    /* 机器可读的一行 */
    printf("RESULT ops_per_sec=%.1f elapsed_ms=%.1f statements=%ld errors=%d\n",
           ops, elapsed, statements, errors);

    free(th);
    free(w);
    db_close(db);
    return errors == 0 ? 0 : 1;
}
