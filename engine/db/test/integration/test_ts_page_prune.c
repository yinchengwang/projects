/*
 * test_ts_page_prune.c —— 时间谓词必须**真的少读页**（W-D 的 D-6.1）
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * 实测（探针）发现的**真缺陷**：谓词下推只做到"行级过滤"，没做到"分区级跳过"。
 * 3 个分区（每天 1 个）、查询只要其中 1 天，改之前：
 *
 *   无 WHERE          900 行，6 页
 *   只覆盖第 1 天      300 行，6 页   ← 行数对，但一页都没少读
 *
 * 根因：`ts_am_mat_readings` 走的是 `ts_store_scan`（**全表扫描**）+ 行过滤，
 * 而能**整分区跳过**的 `ts_prune_scan` 只被 bench 用过，真实查询路径没接它。
 *
 * 于是"时间谓词参与分区裁剪"这句话在**行数上成立、在 IO 上不成立**：
 * 用户以为少了 2/3 的 IO，实际一页没少。
 *
 * ⚠️ 而且 T1–T4 那套判据（bench/ts_bench_io.c）测的是**直调** `ts_prune_scan`，
 * 绕开了真实查询路径 —— 所以它们证明不了这件事。本测试走的是 SQL。
 *
 * ==================== 一个必须如实记录的结构性限制 ====================
 *
 * SPI 的下推接口只能带**一个** `Predicate`：
 *
 *     typedef int (*PushFn)(StorageAM *am, const Predicate *p, AmScan **out);
 *
 * 而 `WHERE time >= a AND time < b` 是**两个**谓词，执行器只挑其中一个下推
 *（`collect_pushable` 挑选择性最好的那一个），另一边由执行器事后过滤。
 *
 * ⇒ 所以**双边界范围**下推之后只是一个半平面：
 *   - `>= a` 覆盖几个分区就按几个分区算（可能全读）；
 *   - 行数仍然正确（执行器会完整求值 WHERE）。
 *
 * 要真正解决必须让下推带**谓词合取**（扩展 `src/spi/pushdown.h`），
 * 而 `src/spi/` 是本计划 Global Constraint 5 明令不碰的地盘（W-C 的地盘）。
 * ⇒ 所以本测试对**双边界**只断言"行数精确 + 页读不高于不裁剪"，
 *    不假装它做到了"只读 1 个分区"。这条限制记在计划里，不藏着。
 *
 * ==================== 判据为什么是"比"而不是"绝对页数" ====================
 *
 * 绝对页数依赖页容量（169 行/页）与分区划分，改一个常量就要重算期望值 ——
 * 那种断言会变成"跟着实现调参"，抓不住回归。
 * 页读比只依赖"分区数"这个结构性事实，稳定得多。
 * 而**行数**必须**精确**断言（漏一行就是静默丢数据，坑 M6-48）。
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

#define PATH      "test/tmp/ts_page_prune.db"
#define DAY_MS    86400000LL
#define NROWS     300
#define NPARTS    3

static void rm_db(void)
{
    char aux[256];

    remove(PATH);
    snprintf(aux, sizeof(aux), "%s-wal", PATH);  remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", PATH); remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", PATH);  remove(aux);
}

/* 某个整天的起点（与分区边界对齐，避免一条读数落进两个分区） */
static int64_t day_base(void)
{
    return (1780000000000LL / DAY_MS) * DAY_MS;
}

/* 执行一条 SELECT，返回行数；页读增量写进 *reads */
static uint64_t run_query(DB *db, const char *sql, uint64_t *reads)
{
    Result *r = NULL;
    uint64_t n = 0;
    uint64_t before = db_ts_page_reads(db);

    if (db_exec(db, sql, &r) != DB_OK) {
        printf("    SQL 失败：%s\n", db_last_error(db));
        failures++;
        *reads = 0;
        return (uint64_t)-1;
    }
    if (r) {
        n = (uint64_t)db_result_rows(r);
        db_result_free(r);
    }
    *reads = db_ts_page_reads(db) - before;
    return n;
}

/* 灌 NPARTS 天、每天 NROWS 条（每条间隔 1 秒，稳稳落在同一个分区里） */
static void seed(DB *db)
{
    int64_t base = day_base();
    int day;
    int i;

    for (day = 0; day < NPARTS; day++) {
        for (i = 0; i < NROWS; i++) {
            db_ts_reading rec;

            rec.time = (uint64_t)(base + (int64_t)day * DAY_MS + (int64_t)i * 1000LL);
            rec.tag = (int64_t)(i % 7);
            rec.value = (double)i;
            CHECK(db_ts_insert(db, &rec) == DB_OK);
        }
    }
}

int main(void)
{
    DB *db = NULL;
    int64_t base;
    char sql[512];
    uint64_t n, reads;
    uint64_t full_reads;

    printf("test_ts_page_prune:\n");

    rm_db();
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(db_ts_create(db) == DB_OK);
    seed(db);
    base = day_base();

    /*
     * ---- 1. 不设范围：一行不少、一页不少（超集铁律，坑 M6-48）----
     */
    n = run_query(db, "SELECT time, tag, value FROM ts_readings", &reads);
    CHECK(n == (uint64_t)(NPARTS * NROWS));
    CHECK(reads > 0u);
    full_reads = reads;
    printf("  [1] 无 WHERE：%llu 行、%llu 页（必须全读）\n",
           (unsigned long long)n, (unsigned long long)reads);

    /*
     * ---- 2. 单谓词下界 `>= 第 2 天`：2 个分区要读，1 个要跳 ----
     */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings WHERE time >= %lld",
             (long long)(base + DAY_MS));
    n = run_query(db, sql, &reads);
    CHECK(n == (uint64_t)((NPARTS - 1) * NROWS));
    /*
     * ★ 核心判据：页读必须**真的下降**。
     * 改之前这里是 6/6（一点没降）⇒ 红。给足余量，不写死比例。
     */
    CHECK(reads < full_reads);
    printf("  [2] >= 第 2 天：%llu 行、%llu 页（须 < %llu）\n",
           (unsigned long long)n, (unsigned long long)reads,
           (unsigned long long)full_reads);

    /*
     * ---- 3. 单谓词上界 `< 第 2 天`：只覆盖第 1 天 ----
     *
     * 裁剪是**超集**语义（相邻窗口的边界会带进下一个分区），
     * 所以不要求"只读 1 个分区"，只要求比全读少。
     */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings WHERE time < %lld",
             (long long)(base + DAY_MS));
    n = run_query(db, sql, &reads);
    CHECK(n == (uint64_t)NROWS);
    CHECK(reads < full_reads);
    printf("  [3] < 第 2 天：%llu 行、%llu 页（须 < %llu）\n",
           (unsigned long long)n, (unsigned long long)reads,
           (unsigned long long)full_reads);

    /*
     * ---- 4. 范围完全之外：0 行、0 页（最强的判据）----
     *
     * 裁剪对了 ⇒ 连一个分区都不该碰。改之前这里是 0 行 / 6 页。
     */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings "
             "WHERE time >= %lld AND time < %lld",
             (long long)(base + 10LL * DAY_MS),
             (long long)(base + 11LL * DAY_MS));
    n = run_query(db, sql, &reads);
    CHECK(n == 0u);
    CHECK(reads == 0u);
    printf("  [4] 范围完全之外：%llu 行、%llu 页（必须 0 页）\n",
           (unsigned long long)n, (unsigned long long)reads);

    /*
     * ---- 5. 双边界范围：行数必须精确；页读不高于不裁剪 ----
     *
     * ⚠️ 这里**不**断言"只读 1 个分区"：SPI 只能下推一个谓词（见文件头的说明），
     * 双边界下推之后只是一个半平面。能保证的是：
     *   - 行数精确（执行器完整求值 WHERE）；
     *   - 页读不多于不裁剪（下推的那一边至少不会让情况变坏）。
     * 想要"双边界 ⇒ 只读 1 个分区"，得先扩展 SPI 的谓词下推契约。
     */
    snprintf(sql, sizeof(sql),
             "SELECT time, tag, value FROM ts_readings "
             "WHERE time >= %lld AND time < %lld",
             (long long)(base + DAY_MS), (long long)(base + 2LL * DAY_MS));
    n = run_query(db, sql, &reads);
    CHECK(n == (uint64_t)NROWS);
    CHECK(reads <= full_reads);
    printf("  [5] 双边界（第 2 天）：%llu 行、%llu 页"
           "（行须精确；页读 ≤ %llu。只推一边 ⇒ 不一定最少）\n",
           (unsigned long long)n, (unsigned long long)reads,
           (unsigned long long)full_reads);

    /*
     * ---- 6. 裁剪不影响数据完整性：把每天各查一次，拼起来必须等于全量 ----
     *
     * 这一条防的是"裁剪多给了没关系、少给了致命"那个方向的回归：
     * 逐天查询的行数之和必须恰好等于全量行数。
     */
    {
        uint64_t sum = 0;
        int day;

        for (day = 0; day < NPARTS; day++) {
            snprintf(sql, sizeof(sql),
                     "SELECT time, tag, value FROM ts_readings "
                     "WHERE time >= %lld AND time < %lld",
                     (long long)(base + (int64_t)day * DAY_MS),
                     (long long)(base + (int64_t)(day + 1) * DAY_MS));
            n = run_query(db, sql, &reads);
            CHECK(n == (uint64_t)NROWS);
            sum += n;
        }
        CHECK(sum == (uint64_t)(NPARTS * NROWS));
        printf("  [6] 逐天查 %d 次，行数之和 %llu = 全量 %d（一条不丢、不重）\n",
               NPARTS, (unsigned long long)sum, NPARTS * NROWS);
    }

    CHECK(db_close(db) == DB_OK);
    rm_db();

    if (failures == 0) {
        printf("  6 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
