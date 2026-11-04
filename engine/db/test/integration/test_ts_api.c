/*
 * test_ts_api.c —— 时序模态的公开 API 验收（v1.5 步骤 5）
 *
 * 8 项：
 *   1. **建之前的行为**：没有时序存储时，插入 / 计数 / 分区数 / TTL 一律 DB_E_NOTFOUND；
 *      查虚拟表也报错（表还没注册）；第二次建 → DB_E_INVAL。
 *   2. **写入**：乱序写（先写后写的时间戳更早）也能写进去；计数 == 写入数。
 *   3. **SQL 读回**：`SELECT time, tag, value FROM ts_readings` 的行
 *      与写进去的**逐条相等**（排序后逐位对照）。
 *   4. **分区视图**：`SELECT * FROM ts_partitions` 看得到分区（时间窗 / 状态 / 行数 / 页数）。
 *   5. **TTL**：回收过期分区 ⇒ 计数变小、那个时间窗查不到东西、再跑一次**幂等**。
 *   6. **关库重开**：数据还在（计数 + SQL 读回）。
 *   7. **封窗拒写**：分区封窗后再往那个时间窗写 ⇒ DB_E_NOTSUP（不能悄悄换个分区）。
 *   8. **REPL**：`TS EXISTS / COUNT / INSERT / PARTS` 的输出符合预期。
 *
 * 【为什么要跟"写进去的东西"逐条对照】
 *
 * 时序最容易出的错是**静默漏读**或者**读串行**：
 * 物化视图的时候把行摊平错了顺序、或者裁剪把某个分区漏掉了，
 * 结果就是少几条、或者某条的值对不上 —— 而且**不报错**。
 * 所以判据只能是"逐条全等"。
 *
 * 【为什么乱序写也要测】
 *
 * 时序的写入**允许乱序**（迟到写入）—— 只要不落进已封窗的分区。
 * 而分区内部的时间是乱的 ⇒ 物化时不能假设"按时间排序"，
 * 裁剪时也只能按**分区**做，不能按行做。
 *
 * 【时间戳为什么不用"现在"】
 *
 * TTL 的判据是"分区结束时间 < now − TTL"，now 是**真实的当前时间**。
 * 所以"该被回收的分区"必须造在**过去**（比如 10 天前、20 天前），
 * "该留着的分区"必须落在**今天**这个时间窗里。
 * ⇒ 用"今天这一天里的第 1 小时"当基准点：
 * 今天这个窗口一定不会被 TTL 回收（它的结束时间在将来），
 * 而 10 天前 / 20 天前的窗口一定会被回收（保留期取 7 天）。
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "db.h"
#include "repl_ts.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH_MAIN "test/tmp/ts_api.db"
#define PATH_SEAL "test/tmp/ts_seal.db"
#define PATH_REPL "test/tmp/ts_repl_out.txt"

/* 一天（毫秒）—— 分区窗口的默认跨度 */
#define DAY_MS 86400000LL

/* 今天这个时间窗里的"基准点"（第 1 小时） */
#define BASE_OFFSET_MS 3600000LL

/* TTL 的保留期：7 天 */
#define RETENTION_SEC 604800u

/* 读数条数（当前分区） */
#define N_CUR 5

/* 封窗测试：分区封窗的阈值是 10000 行 */
#define N_SEAL 10000

/* ---------------- 一条读数 ---------------- */

typedef struct {
    int64_t   time;
    int64_t   tag;
    double value;
} Row;

/* 排序用：先按时间，再按标签 */
static int row_cmp(const void *a, const void *b)
{
    const Row *x = (const Row *)a;
    const Row *y = (const Row *)b;

    if (x->time != y->time) {
        return (x->time < y->time) ? -1 : 1;
    }
    if (x->tag != y->tag) {
        return (x->tag < y->tag) ? -1 : 1;
    }
    return 0;
}

/* 现在的毫秒时间戳 */
static int64_t now_ms(void)
{
    return (int64_t)time(NULL) * 1000;
}

/*
 * 今天这个时间窗里的基准点。
 *
 * 取"今天 + 1 小时"：不管现在几点，它都落在**今天这一个分区**里
 * （除非正好在跨日的那一秒，测试偶尔撞上也不影响结论：
 * 最多多分出一个分区，而分区数那项是分开断言的）。
 */
static int64_t today_base(void)
{
    return (now_ms() / DAY_MS) * DAY_MS + BASE_OFFSET_MS;
}

/* ---------------- SQL 读回 ---------------- */

/*
 * 查 ts_readings，把结果收进行数组。
 *
 * 返回 0 成功；-1 失败（SQL 报错）。
 */
static int fetch_readings(DB *db, const char *sql, Row *out, int cap, int *nout)
{
    Result *r = NULL;
    db_err e;
    int nrow, i;

    *nout = 0;
    e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) {
            db_result_free(r);
        }
        return -1;
    }

    nrow = db_result_rows(r);
    if (nrow > cap) {
        db_result_free(r);
        return -1;
    }
    for (i = 0; i < nrow; i++) {
        out[i].time = (int64_t)strtoll(db_result_value(r, i, 0), NULL, 10);
        out[i].tag = (int64_t)strtoll(db_result_value(r, i, 1), NULL, 10);
        out[i].value = strtod(db_result_value(r, i, 2), NULL);
    }
    db_result_free(r);
    *nout = nrow;
    return 0;
}

/* 两组读数是否逐条相等（先排序再比） */
static int rows_equal(Row *a, int na, Row *b, int nb)
{
    int i;

    if (na != nb) {
        return 0;
    }
    qsort(a, (size_t)na, sizeof(Row), row_cmp);
    qsort(b, (size_t)nb, sizeof(Row), row_cmp);
    for (i = 0; i < na; i++) {
        if (a[i].time != b[i].time || a[i].tag != b[i].tag) {
            return 0;
        }
        if (fabs(a[i].value - b[i].value) > 1e-9) {
            return 0;
        }
    }
    return 1;
}

/* ---------------- 各项检查 ---------------- */

/* 第 1 项：建之前的行为 */
static void check_no_store(DB *db)
{
    db_ts_reading r;
    uint64_t n = 0;
    uint32_t np = 0;
    Result *res = NULL;
    db_err e;

    printf("  [1] 没有时序存储时的行为\n");

    CHECK(db_ts_exists(db) == 0);

    r.time = 1000;
    r.tag = 7;
    r.value = 1.5;
    CHECK(db_ts_insert(db, &r) == DB_E_NOTFOUND);
    CHECK(db_ts_count(db, &n) == DB_E_NOTFOUND);
    CHECK(db_ts_part_count(db, &np) == DB_E_NOTFOUND);
    CHECK(db_ts_retention(db, 60) == DB_E_NOTFOUND);

    /* 虚拟表还没注册 ⇒ 查它要报错（而不是查出一张空表） */
    e = db_exec(db, "SELECT time, tag, value FROM ts_readings", &res);
    CHECK(e != DB_OK);
    if (res) {
        db_result_free(res);
        res = NULL;
    }

    /* 建 ⇒ 有 ⇒ 再建 ⇒ DB_E_INVAL */
    CHECK(db_ts_create(db) == DB_OK);
    CHECK(db_ts_exists(db) == 1);
    CHECK(db_ts_create(db) == DB_E_INVAL);
    CHECK(db_ts_exists(db) == 1);
}

/* 第 2 项：写入（乱序） */
static void check_insert(DB *db, Row *cur, int ncur)
{
    int64_t base = today_base();
    int order[N_CUR];
    uint64_t n = 0;
    uint32_t np = 0;
    int i;

    printf("  [2] 写入（乱序）\n");

    for (i = 0; i < ncur; i++) {
        cur[i].time = base + (int64_t)i * 1000;
        cur[i].tag = 100 + i;
        cur[i].value = 1.5 * (double)i;
    }

    /* 故意乱序写：先写后面的时间戳，再写前面的 */
    order[0] = 2;
    order[1] = 0;
    order[2] = 4;
    order[3] = 1;
    order[4] = 3;

    for (i = 0; i < ncur; i++) {
        db_ts_reading r;

        r.time = cur[order[i]].time;
        r.tag = cur[order[i]].tag;
        r.value = cur[order[i]].value;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }

    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)ncur);
    CHECK(db_ts_part_count(db, &np) == DB_OK);
    CHECK(np == 1u);
}

/* 第 3 项：SQL 读回与写进去的逐条相等 */
static void check_read_back(DB *db, Row *cur, int ncur)
{
    Row got[64];
    int n = 0;

    printf("  [3] SQL 读回\n");

    CHECK(fetch_readings(db, "SELECT time, tag, value FROM ts_readings",
                         got, 64, &n) == 0);
    CHECK(n == ncur);
    if (!rows_equal(got, n, cur, ncur)) {
        printf("    FAIL 读回的行与写进去的不一致\n");
        failures++;
    }
}

/* 第 4 项：分区视图 */
static void check_partitions(DB *db, int ncur)
{
    Result *r = NULL;
    db_err e;
    int64_t base = today_base();
    int64_t start, end;
    long long state, rows, pages;

    printf("  [4] ts_partitions 视图\n");

    e = db_exec(db, "SELECT * FROM ts_partitions", &r);
    CHECK(e == DB_OK);
    if (e != DB_OK) {
        return;
    }
    CHECK(db_result_rows(r) == 1);
    CHECK(db_result_cols(r) == 5);

    start = (int64_t)strtoll(db_result_value(r, 0, 0), NULL, 10);
    end = (int64_t)strtoll(db_result_value(r, 0, 1), NULL, 10);
    state = strtoll(db_result_value(r, 0, 2), NULL, 10);
    rows = strtoll(db_result_value(r, 0, 3), NULL, 10);
    pages = strtoll(db_result_value(r, 0, 4), NULL, 10);

    /* 时间窗必须盖住基准点 */
    CHECK(start <= base);
    CHECK(end > base);
    CHECK(state == 0);            /* ACTIVE */
    CHECK(rows == ncur);
    CHECK(pages >= 1);

    db_result_free(r);
}

/* 第 5 项：TTL */
static void check_ttl(DB *db)
{
    int64_t base = today_base();
    int64_t p10 = base - 10 * DAY_MS;
    int64_t p20 = base - 20 * DAY_MS;
    uint64_t n = 0;
    uint32_t np = 0;
    Row got[64];
    int nread = 0;
    char sql[256];
    int i;

    printf("  [5] TTL 回收\n");

    /* 两个过去的时间窗：10 天前 3 条、20 天前 2 条 */
    for (i = 0; i < 3; i++) {
        db_ts_reading r;

        r.time = p10 + (int64_t)i * 1000;
        r.tag = 200 + i;
        r.value = 10.0 + i;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }
    for (i = 0; i < 2; i++) {
        db_ts_reading r;

        r.time = p20 + (int64_t)i * 1000;
        r.tag = 300 + i;
        r.value = 20.0 + i;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }

    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)(N_CUR + 5));
    CHECK(db_ts_part_count(db, &np) == DB_OK);
    CHECK(np == 3u);

    /* 跑一次 TTL：保留 7 天 ⇒ 10 天前 / 20 天前的分区被回收 */
    CHECK(db_ts_retention(db, RETENTION_SEC) == DB_OK);

    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_CUR);

    /* 目录里还留着历史（分区数不变，状态变成 DROPPED） */
    CHECK(db_ts_part_count(db, &np) == DB_OK);
    CHECK(np == 3u);

    /* 被回收的时间窗查不到东西 */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings "
             "WHERE time >= %lld AND time <= %lld",
             (long long)p10, (long long)(p10 + 3600000));
    CHECK(fetch_readings(db, sql, got, 64, &nread) == 0);
    CHECK(nread == 0);

    /* 幂等：再跑一次，结果不变，也不报错 */
    CHECK(db_ts_retention(db, RETENTION_SEC) == DB_OK);
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_CUR);
}

/* 第 6 项：关库重开 */
static void check_reopen(const char *path, Row *cur, int ncur)
{
    DB *db = NULL;
    Row got[64];
    int n = 0;
    uint64_t cnt = 0;

    printf("  [6] 关库重开\n");

    CHECK(db_open(path, &db) == DB_OK);
    if (!db) {
        return;
    }
    CHECK(db_ts_exists(db) == 1);
    CHECK(db_ts_count(db, &cnt) == DB_OK);
    CHECK(cnt == (uint64_t)ncur);
    CHECK(fetch_readings(db, "SELECT time, tag, value FROM ts_readings",
                         got, 64, &n) == 0);
    CHECK(rows_equal(got, n, cur, ncur));
    db_close(db);
}

/* 第 7 项：封窗分区拒写 */
static void check_sealed(void)
{
    DB *db = NULL;
    db_ts_reading r;
    int64_t base = today_base();
    uint64_t n = 0;
    int i;

    printf("  [7] 封窗分区拒写\n");

    remove(PATH_SEAL);
    CHECK(db_open(PATH_SEAL, &db) == DB_OK);
    if (!db) {
        return;
    }
    CHECK(db_ts_create(db) == DB_OK);

    /* 写满阈值 ⇒ 分区自动封窗 */
    for (i = 0; i < N_SEAL; i++) {
        r.time = base + (int64_t)i * 10;
        r.tag = 900 + (int64_t)(i % 7);
        r.value = (double)i;
        if (db_ts_insert(db, &r) != DB_OK) {
            printf("    FAIL 第 %d 条写入失败\n", i);
            failures++;
            break;
        }
    }

    /* 再往这个时间窗写 ⇒ 必须明确报错（不能悄悄换个分区） */
    r.time = base + (int64_t)N_SEAL * 10 + 5;
    r.tag = 999;
    r.value = 1.0;
    CHECK(db_ts_insert(db, &r) == DB_E_NOTSUP);

    /* 报错的内容要说清楚是"落进已封窗的分区" */
    CHECK(strstr(db_last_error(db), "封窗") != NULL);

    /* 行数没变（被拒的那条没写进去） */
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_SEAL);

    db_close(db);
}

/* 第 8 项：REPL 命令 */
static void check_repl(void)
{
    DB *db = NULL;
    FILE *f;
    char cmd[128];
    char buf[4096];
    size_t got;
    uint64_t n = 0;

    printf("  [8] REPL 的 TS 命令\n");

    remove(PATH_MAIN);
    CHECK(db_open(PATH_MAIN, &db) == DB_OK);
    if (!db) {
        return;
    }
    CHECK(db_ts_create(db) == DB_OK);

    remove(PATH_REPL);
    f = fopen(PATH_REPL, "w+");
    CHECK(f != NULL);
    if (!f) {
        db_close(db);
        return;
    }
    repl_ts_set_output(f);

    snprintf(cmd, sizeof(cmd), "TS EXISTS");
    handle_ts(db, cmd);
    snprintf(cmd, sizeof(cmd), "TS COUNT");
    handle_ts(db, cmd);
    snprintf(cmd, sizeof(cmd), "TS INSERT %lld 42 3.5",
             (long long)(today_base() + 500));
    handle_ts(db, cmd);
    snprintf(cmd, sizeof(cmd), "TS PARTS");
    handle_ts(db, cmd);
    snprintf(cmd, sizeof(cmd), "TS READS");
    handle_ts(db, cmd);

    repl_ts_set_output(NULL);

    fflush(f);
    rewind(f);
    got = fread(buf, 1, sizeof(buf) - 1, f);
    buf[got] = '\0';
    fclose(f);

    CHECK(strstr(buf, "有时序存储") != NULL);
    /* COUNT 的输出：刚插了一条 ⇒ 1 */
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == 1u);
    CHECK(strstr(buf, "start_ms=") != NULL);
    CHECK(strstr(buf, "已插入") != NULL);

    db_close(db);
}

/* ---------------- 入口 ---------------- */

int main(void)
{
    DB *db = NULL;
    Row cur[N_CUR];

    remove(PATH_MAIN);

    printf("== 时序模态公开 API 验收 ==\n");

    /* 第 1~5 项共用一个库（顺序有关：建 → 写 → 读 → TTL） */
    CHECK(db_open(PATH_MAIN, &db) == DB_OK);
    if (!db) {
        printf("打不开库 %s\n", PATH_MAIN);
        return 1;
    }

    check_no_store(db);
    check_insert(db, cur, N_CUR);
    check_read_back(db, cur, N_CUR);
    check_partitions(db, N_CUR);
    check_ttl(db);

    db_close(db);

    /* 第 6 项：重开 */
    check_reopen(PATH_MAIN, cur, N_CUR);

    /* 第 7 项：封窗（单独一个库文件） */
    check_sealed();

    /* 第 8 项：REPL（单独一个库文件） */
    check_repl();

    printf(failures == 0 ? "全部通过\n" : "有失败项\n");
    return failures == 0 ? 0 : 1;
}
