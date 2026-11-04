/*
 * test_ts_crash.c —— 时序模态崩溃恢复验收 · db 层（v1.5 步骤 6 · T10 · A7）
 *
 * "断电"用 db_abandon 模拟：不刷脏页、不写 CHECKPOINT，直接丢句柄。
 * 等价于进程被 kill -9 —— 日志已经 fsync，页还在内存里没落盘。
 *
 * 这一份做 3 项（都是**真 WAL、真断电**）：
 *   1. 灌读数 → 断电 → 重开：条数对、每一条**逐条相等**；
 *      再断一次再重开 ⇒ 重放是**幂等**的
 *   2. 封窗（写满阈值自动封窗）→ 断电 → 重开：封窗分区仍读得对
 *   3. TTL 回收 → 断电 → 重开：被回收的分区没了，活着的分区数据完好
 *
 * 另一半（崩在**多页操作中间**）在 test_ts_crash_mid.c 里做 ——
 * 那一半需要在"第 N 个写点"上崩，db 层没有这个开关。
 *
 * ==================== 这三项在证明什么 ====================
 *
 * 时序的读数**只存在分区页里**（没有第二份行存，坑 M5-16 的同款事实）⇒
 * WAL 是**唯一**的恢复路径。所以：
 *
 *   断电时脏页全丢 ⇒ 磁盘上的分区页是"旧的"（甚至一张都没有），
 *   只有日志里有那些页的整页镜像 ⇒ 重开库时由重放把页补回来。
 *
 * 三项分别覆盖三类页：
 *   1) PT_TS_PART（分区数据页）+ PT_TS_DIR / PT_TS_META（目录与 meta）
 *   2) PT_TS_COL（列式页）+ 目录项翻成 SEALED
 *   3) 目录项翻成 DROPPED + 回收页时的清零
 *
 * ==================== 为什么第 1 项要"逐条相等" ====================
 *
 * 条数对**不代表**数据对。时序最容易出的错是**静默漏读**或者**读串行**：
 * 分区页链断了一截、目录项的行数与实际不符、列式页少了几行 ——
 * 结果就是少几条、或者某条的值对不上，**而且不报错**。
 * "条数差不多"这种验收抓不住这类 bug，所以判据只能是"逐条全等"。
 *
 * ==================== 第 2 项为什么值得单独测 ====================
 *
 * 封窗是**多页操作**：写列式页（好几张）+ 翻目录项（一步）。
 * 它同时覆盖"列式页进不进日志"和"目录项进不进日志"两件事，
 * 而且这两者的**顺序**决定重开后看到的是完整旧分区还是完整新分区
 * （详见 test_ts_crash_mid.c 的说明）。
 *
 * ==================== 第 3 项在盯什么 ====================
 *
 * TTL = 改目录项（一步）+ 回收页（多步）。顺序必须"先改目录、后回收页"
 * （坑 M6-49）：反过来就会出现"目录说分区还在、页却被回收了"，
 * 查询时读到**别人的数据**，而且不报错。
 *
 * 崩在中间只有两种结局，**都得是安全的**：
 *   目录说还在 ⇒ 数据一条不少（页还没被回收）
 *   目录说已过期 ⇒ 页可能还占着（空间泄漏，下次 TTL 再收一次）
 */

#include "ts_crash_common.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH1 "test/tmp/ts_crash1.db"
#define PATH2 "test/tmp/ts_crash2.db"
#define PATH3 "test/tmp/ts_crash3.db"

/* TTL 的保留期：7 天 */
#define RETENTION_SEC 604800u

/* 第 1 项：写多少条（一页装 169 行 ⇒ 200 行必然要 spill 到第二页） */
#define N_ROWS 200

/* 第 2 项：写满封窗阈值（与 TS_SEAL_ROW_THRESHOLD 相同） */
#define N_SEAL 10000

/* ==================== 第 1 项：写入 → 断电 → 重开 ==================== */

static void t1_crash_insert(void)
{
    DB *db = NULL;
    Row *expect = NULL;
    Row *got = NULL;
    uint64_t n = 0;
    int nread = 0;
    int64_t base = ts_crash_today_base();
    int i;

    printf("  1) 写 %d 条 → 断电 → 重开：条数对、逐条相等；再断一次 ⇒ 幂等\n", N_ROWS);

    remove(PATH1);
    remove("test/tmp/ts_crash1.db-wal");

    expect = (Row *)malloc(sizeof(Row) * (size_t)N_ROWS);
    got = (Row *)malloc(sizeof(Row) * (size_t)N_ROWS);
    CHECK(expect != NULL && got != NULL);
    if (!expect || !got) {
        free(expect);
        free(got);
        return;
    }
    ts_crash_fill(expect, N_ROWS, base, 10);

    CHECK(db_open(PATH1, &db) == DB_OK);
    if (!db) {
        free(expect);
        free(got);
        return;
    }
    CHECK(db_ts_create(db) == DB_OK);

    for (i = 0; i < N_ROWS; i++) {
        db_ts_reading r;

        r.time = expect[i].time;
        r.tag = expect[i].tag;
        r.value = expect[i].value;
        if (db_ts_insert(db, &r) != DB_OK) {
            printf("    FAIL 第 %d 条写入失败\n", i);
            failures++;
            break;
        }
    }

    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_ROWS);

    /* 断电：不刷脏页、不写 CHECKPOINT（日志已经 fsync 过） */
    db_abandon(db);
    db = NULL;

    /* 重开 → 恢复重放 → 分区页 / 目录页 / meta 页都被补回来 */
    CHECK(db_open(PATH1, &db) == DB_OK);
    if (!db) {
        free(expect);
        free(got);
        return;
    }
    CHECK(db_ts_exists(db) == 1);
    CHECK(db_ts_count(db, &n) == DB_OK);
    if (n != (uint64_t)N_ROWS) {
        printf("    FAIL 断电后条数变了：断电前 %d，断电后 %llu\n",
               N_ROWS, (unsigned long long)n);
        failures++;
    }
    CHECK(ts_crash_fetch(db, "SELECT time, tag, value FROM ts_readings",
                         got, N_ROWS, &nread) == 0);
    if (!ts_crash_rows_equal(got, nread, expect, N_ROWS)) {
        printf("    FAIL 断电后读回的行与写进去的不一致（读到 %d 条）\n", nread);
        failures++;
    } else {
        printf("    断电后：%d 条逐条相等\n", nread);
    }

    /* 再断一次电、再重开：重放是幂等的，结果不能变 */
    db_abandon(db);
    db = NULL;
    CHECK(db_open(PATH1, &db) == DB_OK);
    if (!db) {
        free(expect);
        free(got);
        return;
    }
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_ROWS);
    CHECK(ts_crash_fetch(db, "SELECT time, tag, value FROM ts_readings",
                         got, N_ROWS, &nread) == 0);
    CHECK(ts_crash_rows_equal(got, nread, expect, N_ROWS));
    printf("    断电两次后：仍是 %d 条，逐条相等\n", nread);

    db_close(db);
    remove(PATH1);
    remove("test/tmp/ts_crash1.db-wal");
    free(expect);
    free(got);
}

/* ==================== 第 2 项：封窗 → 断电 → 重开 ==================== */

static void t2_crash_seal(void)
{
    DB *db = NULL;
    Row *expect = NULL;
    Row *got = NULL;
    uint64_t n = 0;
    uint32_t np = 0;
    int nread = 0;
    int64_t base = ts_crash_today_base();
    int i;

    printf("  2) 写满 %d 条（自动封窗）→ 断电 → 重开：封窗分区仍读得对\n", N_SEAL);

    remove(PATH2);
    remove("test/tmp/ts_crash2.db-wal");

    expect = (Row *)malloc(sizeof(Row) * (size_t)N_SEAL);
    got = (Row *)malloc(sizeof(Row) * (size_t)N_SEAL);
    CHECK(expect != NULL && got != NULL);
    if (!expect || !got) {
        free(expect);
        free(got);
        return;
    }
    ts_crash_fill(expect, N_SEAL, base, 10);

    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db) {
        free(expect);
        free(got);
        return;
    }
    CHECK(db_ts_create(db) == DB_OK);

    for (i = 0; i < N_SEAL; i++) {
        db_ts_reading r;

        r.time = expect[i].time;
        r.tag = expect[i].tag;
        r.value = expect[i].value;
        if (db_ts_insert(db, &r) != DB_OK) {
            printf("    FAIL 第 %d 条写入失败\n", i);
            failures++;
            break;
        }
    }

    /* 到阈值 ⇒ 自动封窗（目录项翻成 SEALED，first_page 指向列式头部页） */
    CHECK(db_ts_part_count(db, &np) == DB_OK);
    CHECK(np == 1u);
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == (uint64_t)N_SEAL);

    db_abandon(db);
    db = NULL;

    CHECK(db_open(PATH2, &db) == DB_OK);
    if (!db) {
        free(expect);
        free(got);
        return;
    }
    CHECK(db_ts_exists(db) == 1);
    CHECK(db_ts_count(db, &n) == DB_OK);
    if (n != (uint64_t)N_SEAL) {
        printf("    FAIL 封窗后断电，条数变了：断电前 %d，断电后 %llu\n",
               N_SEAL, (unsigned long long)n);
        failures++;
    }
    CHECK(ts_crash_fetch(db, "SELECT time, tag, value FROM ts_readings",
                         got, N_SEAL, &nread) == 0);
    if (!ts_crash_rows_equal(got, nread, expect, N_SEAL)) {
        printf("    FAIL 封窗后断电，读回的行与写进去的不一致（读到 %d 条）\n",
               nread);
        failures++;
    } else {
        printf("    封窗分区断电后：%d 条逐条相等\n", nread);
    }

    db_close(db);
    remove(PATH2);
    remove("test/tmp/ts_crash2.db-wal");
    free(expect);
    free(got);
}

/* ==================== 第 3 项：TTL 回收 → 断电 → 重开 ==================== */

static void t3_crash_ttl(void)
{
    DB *db = NULL;
    Row expect[16];
    Row got[16];
    uint64_t n = 0;
    uint32_t np = 0;
    int nread = 0;
    int64_t base = ts_crash_today_base();
    int64_t p10 = base - 10 * DAY_MS;
    int64_t p20 = base - 20 * DAY_MS;
    char sql[256];
    int i;

    printf("  3) TTL 回收过期分区 → 断电 → 重开：回收的没了，活着的完好\n");

    remove(PATH3);
    remove("test/tmp/ts_crash3.db-wal");

    CHECK(db_open(PATH3, &db) == DB_OK);
    if (!db) {
        return;
    }
    CHECK(db_ts_create(db) == DB_OK);

    /* 今天这个分区：5 条（TTL 不能碰它） */
    for (i = 0; i < 5; i++) {
        db_ts_reading r;

        r.time = base + (int64_t)i * 1000;
        r.tag = 100 + i;
        r.value = 1.5 * (double)i;
        expect[i].time = r.time;
        expect[i].tag = r.tag;
        expect[i].value = r.value;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }
    /* 10 天前：3 条（要被回收） */
    for (i = 0; i < 3; i++) {
        db_ts_reading r;

        r.time = p10 + (int64_t)i * 1000;
        r.tag = 200 + i;
        r.value = 10.0 + i;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }
    /* 20 天前：2 条（要被回收） */
    for (i = 0; i < 2; i++) {
        db_ts_reading r;

        r.time = p20 + (int64_t)i * 1000;
        r.tag = 300 + i;
        r.value = 20.0 + i;
        CHECK(db_ts_insert(db, &r) == DB_OK);
    }

    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == 10u);
    CHECK(db_ts_part_count(db, &np) == DB_OK);
    CHECK(np == 3u);

    /* TTL：保留 7 天 ⇒ 两个过去的时间窗被回收 */
    CHECK(db_ts_retention(db, RETENTION_SEC) == DB_OK);
    CHECK(db_ts_count(db, &n) == DB_OK);
    CHECK(n == 5u);

    db_abandon(db);
    db = NULL;

    CHECK(db_open(PATH3, &db) == DB_OK);
    if (!db) {
        return;
    }
    CHECK(db_ts_exists(db) == 1);
    CHECK(db_ts_count(db, &n) == DB_OK);
    if (n != 5u) {
        printf("    FAIL TTL 后断电，条数变了：断电前 5，断电后 %llu\n",
               (unsigned long long)n);
        failures++;
    }

    /* 被回收的时间窗查不到东西（目录项仍然是 DROPPED） */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings "
             "WHERE time >= %lld AND time <= %lld",
             (long long)p10, (long long)(p10 + 3600000));
    CHECK(ts_crash_fetch(db, sql, got, 16, &nread) == 0);
    CHECK(nread == 0);

    /* 活着的分区数据一条不少、逐条相等 */
    CHECK(ts_crash_fetch(db, "SELECT time, tag, value FROM ts_readings",
                         got, 16, &nread) == 0);
    if (!ts_crash_rows_equal(got, nread, expect, 5)) {
        printf("    FAIL TTL 后断电，活着的分区的数据不对（读到 %d 条）\n", nread);
        failures++;
    } else {
        printf("    TTL 后断电：活着的分区 %d 条逐条相等，过期分区查不到\n", nread);
    }

    db_close(db);
    remove(PATH3);
    remove("test/tmp/ts_crash3.db-wal");
}

/* ---------------- 入口 ---------------- */

int main(void)
{
    printf("test_ts_crash:\n");

    t1_crash_insert();
    t2_crash_seal();
    t3_crash_ttl();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  3 项验收全部通过\n");
    return 0;
}
