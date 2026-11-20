/*
 * test_ts_reuse_across_session.c —— 回收的页跨会话真能复用（W-D 的 D-6.3）
 *
 * ==================== 这个测试要证明什么 ====================
 *
 * D-6.2 让封窗旧裸写页进了全局空闲栈。但那时栈**只在内存里**（坑 M6-52）：
 * 关库之后栈丢了 ⇒ 那些页在文件里是永久空洞 ⇒ 空间不会真的回来。
 *
 * D-6.3 之后：`pager_free()` 会把页标成 `PT_FREE`，开库时按标记重建栈。
 *
 * 本测试走**真实路径**（SQL + `db_ts_*`）证明：回收的页在**下一个会话**里
 * 真的被复用了 —— 判据是文件的**总页数**。
 *
 * ==================== 为什么用"文件页数"而不是"分配到的页号" ====================
 *
 * 页号的发放顺序不可预测（栈是后进先出，TTL 与封窗各收一批）。
 * 但文件总页数是稳定的判据：写的数据量翻倍、而文件没有翻倍，
 * 就说明"收回来的页真的被用上了"。
 * 只看"空闲页计数对得上"证明不了这一点 —— 计数对而分配还走文件尾，
 * 空间一样没回来。
 *
 * ==================== 两个场景 ====================
 *
 *   A（基线）：6 个分区，什么都不回收
 *   B（对照）：6 个分区 → TTL 掉最老 3 个（页还回空闲栈）→ 关库重开
 *             → 再写 3 个分区（数据量是 A 的 1.5 倍）
 *
 * 判据：`B*20 < A*23`，即 B 的文件页数必须**不到** A 的 1.15 倍。
 *
 * 【这个阈值是实测标定的，不是拍的】
 *
 * 实测（同一台机器、同一份代码）：
 *   重建开着（D-6.3 生效）⇒ A = 13 页，B = 13 页（比值 1.00）
 *   重建关掉（变异验证）  ⇒ A = 13 页，B = 18 页（比值 1.38）
 *
 * 所以 1.15 落在两个实测值中间，两边都有余量。
 * 一开始写的是 1.75 —— 那个太松，**变异验证时没红**，等于判据没生效。
 * 这条记在这里，免得下次有人又“给足余量”。
 */

#include <stdio.h>
#include <stdint.h>
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

#define PATH_A "test/tmp/ts_reuse_norec.db"
#define PATH_B "test/tmp/ts_reuse_rec.db"
#define DAY_MS 86400000LL
#define NROWS  169u            /* 刚好一页裸写数据 */
#define NPARTS 6
#define NPART2 3               /* B 再多写这么多个分区 */

static void rm_db(const char *p)
{
    char aux[256];

    remove(p);
    snprintf(aux, sizeof(aux), "%s-wal", p);  remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", p); remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", p);  remove(aux);
}

static int64_t day_base(void)
{
    return (1780000000000LL / DAY_MS) * DAY_MS;
}

/* 从第 from_day 天开始，灌 n_days 天、每天 NROWS 条 */
static void seed_range(DB *db, int from_day, int n_days)
{
    int64_t base = day_base();
    int day;
    uint32_t i;

    for (day = from_day; day < from_day + n_days; day++) {
        for (i = 0; i < NROWS; i++) {
            db_ts_reading r;

            r.time = (uint64_t)(base + (int64_t)day * DAY_MS
                                + (int64_t)i * 1000LL);
            r.tag = (int64_t)(i % 7);
            r.value = (double)i;
            CHECK(db_ts_insert(db, &r) == DB_OK);
        }
    }
}

/* 文件有多少页（4KB 口径，与 PAGE_SIZE 一致） */
static uint64_t file_pages(const char *path)
{
    FILE *f = fopen(path, "rb");
    long sz;

    if (!f)
        return 0;
    fseek(f, 0, SEEK_END);
    sz = ftell(f);
    fclose(f);
    return (uint64_t)(sz / 4096);
}

int main(void)
{
    DB *db = NULL;
    uint64_t pages_a;
    uint64_t pages_b;

    printf("test_ts_reuse_across_session:\n");

    /* ================= 场景 A：不回收（基线）================= */
    rm_db(PATH_A);
    CHECK(db_open(PATH_A, &db) == DB_OK);
    CHECK(db_ts_create(db) == DB_OK);
    seed_range(db, 0, NPARTS);
    CHECK(db_close(db) == DB_OK);

    pages_a = file_pages(PATH_A);
    CHECK(pages_a > 0u);
    printf("  [基线] %d 个分区、不回收：文件 %llu 页\n",
           NPARTS, (unsigned long long)pages_a);

    /* ================= 场景 B：回收之后再写 ================= */
    rm_db(PATH_B);
    CHECK(db_open(PATH_B, &db) == DB_OK);
    CHECK(db_ts_create(db) == DB_OK);
    seed_range(db, 0, NPARTS);

    /* TTL：只留最近 3 天 ⇒ 最老 3 个分区的页被回收进空闲栈 */
    CHECK(db_ts_retention(db, 3u * 86400u) == DB_OK);

    /*
     * ★ 关库重开：内存栈丢了。
     * D-6.3 之前 ⇒ 那些页是永久空洞；之后 ⇒ 由 PT_FREE 标记重建栈。
     */
    CHECK(db_close(db) == DB_OK);
    CHECK(db_open(PATH_B, &db) == DB_OK);

    /* 重开后再写 NPART2 个新分区（需要页 —— 优先从重建出的空闲栈拿） */
    seed_range(db, NPARTS, NPART2);
    CHECK(db_close(db) == DB_OK);

    pages_b = file_pages(PATH_B);
    CHECK(pages_b > 0u);
    printf("  [对照] 回收 %d 个分区、重开会又写 %d 个：文件 %llu 页\n",
           3, NPART2, (unsigned long long)pages_b);

    /*
     * ★ 判据：B 写的数据量是 A 的 (6+3)/6 = 1.5 倍，
     * 但文件必须**不到** A 的 1.15 倍（阈值标定见文件头）。
     *
     * 变异验证：把 db_open 里的 pager_free_rebuild 关掉 ⇒ B 从 13 页变 18 页
     * ⇒ 这条断言**变红**。说明它真的在盯着"跨会话复用"这件事，不是摆设。
     */
    CHECK(pages_b * 20u < pages_a * 23u);
    printf("  [判定] B*20 = %llu < A*23 = %llu ⇒ [%s]\n",
           (unsigned long long)(pages_b * 20u),
           (unsigned long long)(pages_a * 23u),
           (pages_b * 20u < pages_a * 23u) ? "PASS" : "FAIL");

    rm_db(PATH_A);
    rm_db(PATH_B);

    if (failures == 0) {
        printf("  3 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
