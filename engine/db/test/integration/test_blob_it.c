/*
 * test_blob_it.c — blob 模态端到端验收
 *
 * 5 项：
 *   1. 写 1MB 对象 → 读回一致（跨 256 帧）
 *   2. 重启后读回一致（持久化）
 *   3. 崩溃恢复后读回一致（不刷脏页直接重开）
 *   4. blob 与关系模态共存：表的数据和对象的数据都完好（页不串）
 *   5. blob 与关系模态共享 WAL：崩溃后两者都恢复
 *
 * 【为什么必须有第 3、5 项】
 *
 * blob 复用 WAL_PAGEDATA 写日志。这条路一旦有问题，
 * 后果是**静默的数据丢失** —— 对象内容在盘上，但日志里没有，
 * 恢复时重放不出来。
 *
 * 而第 3 项（模拟崩溃）恰好能抓到它。
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

#define PATH "test/tmp/blob_it.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e == DB_OK ? 0 : -1;
}

static void fill_pattern(unsigned char *buf, uint64_t off, uint64_t len)
{
    for (uint64_t i = 0; i < len; i++)
        buf[i] = (unsigned char)((off + i) & 0xFF);
}

/* ---------------- 1) 1MB 对象 ---------------- */

/*
 * 1MB / 4KB = 256 帧。跨多帧是基本要求。
 */
static void t1_one_mb(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    const uint64_t nframes = 256;
    const uint64_t len = nframes * 4076;      /* 1MB 稍多 */

    unsigned char *src = malloc((size_t)len);
    CHECK(src != NULL);
    fill_pattern(src, 0, len);

    db_blob_id id = 0;
    CHECK(db_blob_create(db, nframes, 0, &id) == DB_OK);
    CHECK(id != 0);
    CHECK(db_blob_write(db, id, src, len) == DB_OK);

    unsigned char *dst = malloc((size_t)len);
    CHECK(dst != NULL);
    memset(dst, 0xAA, (size_t)len);

    uint64_t got = 0;
    CHECK(db_blob_read(db, id, dst, len, &got) == DB_OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    /* 单帧读也要对 */
    unsigned char frame[4096];
    uint32_t n = 0;
    CHECK(db_blob_read_frame(db, id, 200, frame, sizeof(frame), &n) == DB_OK);
    CHECK(n == 4076);
    CHECK(memcmp(frame, src + 200 * 4076, 4076) == 0);

    free(src);
    free(dst);
    db_close(db);
}

/* ---------------- 2) 重启持久化 ---------------- */

static void t2_restart(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    const uint64_t nframes = 16;
    const uint64_t len = nframes * 4076;

    unsigned char *src = malloc((size_t)len);
    fill_pattern(src, 0, len);

    db_blob_id id = 0;
    CHECK(db_blob_create(db, nframes, 0, &id) == DB_OK);
    CHECK(db_blob_write(db, id, src, len) == DB_OK);
    db_close(db);

    /* 重开 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);

    unsigned char *dst = malloc((size_t)len);
    memset(dst, 0xAA, (size_t)len);
    uint64_t got = 0;
    CHECK(db_blob_read(db2, id, dst, len, &got) == DB_OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    /* 元信息也要对 */
    uint64_t nf = 0, total = 0;
    uint32_t fs = 0;
    CHECK(db_blob_stat(db2, id, &nf, &fs, &total) == DB_OK);
    CHECK(nf == nframes);
    CHECK(total == len);

    free(src);
    free(dst);
    db_close(db2);
}

/* ---------------- 3) 崩溃恢复 ---------------- */

/*
 * 模拟崩溃：db_abandon 不刷脏页、不写 CHECKPOINT。
 * 重开后恢复阶段必须靠 WAL 把 blob 页重放回来。
 */
static void t3_crash_recovery(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    const uint64_t nframes = 8;
    const uint64_t len = nframes * 4076;

    unsigned char *src = malloc((size_t)len);
    fill_pattern(src, 0, len);

    db_blob_id id = 0;
    CHECK(db_blob_create(db, nframes, 0, &id) == DB_OK);
    CHECK(db_blob_write(db, id, src, len) == DB_OK);

    /* 崩溃：不刷脏页 */
    db_abandon(db);

    /* 重开 → 恢复 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);

    unsigned char *dst = malloc((size_t)len);
    memset(dst, 0xAA, (size_t)len);
    uint64_t got = 0;
    CHECK(db_blob_read(db2, id, dst, len, &got) == DB_OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    free(src);
    free(dst);
    db_close(db2);
}

/* ---------------- 4) 两个模态共存 ---------------- */

/*
 * 表用 heap 页，blob 用 PT_BLOB 页。
 * 两种页在同一个文件里，必须互不干扰。
 *
 * 特别要测：**blob 页不能被表的扫描撞上**，
 * **表的页不能被 blob 的帧号算术撞上**。
 */
static void t4_coexist(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    /* 建表插数据 */
    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);
    for (int i = 0; i < 20; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'row%d');", i, i);
        CHECK(run_sql(db, sql) == 0);
    }

    /* 写 blob */
    const uint64_t nframes = 32;
    const uint64_t len = nframes * 4076;
    unsigned char *src = malloc((size_t)len);
    fill_pattern(src, 0, len);

    db_blob_id id = 0;
    CHECK(db_blob_create(db, nframes, 0, &id) == DB_OK);
    CHECK(db_blob_write(db, id, src, len) == DB_OK);

    /* 表的数据还在 */
    Result *r = NULL;
    CHECK(db_exec(db, "SELECT id FROM t;", &r) == DB_OK);
    CHECK(db_result_rows(r) == 20);
    db_result_free(r);

    /* blob 的数据也对 */
    unsigned char *dst = malloc((size_t)len);
    memset(dst, 0xAA, (size_t)len);
    uint64_t got = 0;
    CHECK(db_blob_read(db, id, dst, len, &got) == DB_OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    /* 再查一次表 —— blob 页不能混进表扫描 */
    r = NULL;
    CHECK(db_exec(db, "SELECT id FROM t;", &r) == DB_OK);
    CHECK(db_result_rows(r) == 20);
    db_result_free(r);

    free(src);
    free(dst);
    db_close(db);
}

/* ---------------- 5) 两个模态一起崩溃恢复 ---------------- */

/*
 * 最严的场景：表和 blob 都有数据，一起崩溃。
 *
 * 恢复阶段要同时处理：
 *   - heap 页的行级日志（INSERT）
 *   - blob 页的整页镜像日志（PAGEDATA）
 * 两者在同一个 WAL 文件里，LSN 交错。
 */
static void t5_both_crash(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    CHECK(run_sql(db, "CREATE TABLE t (id INT, name TEXT);") == 0);
    for (int i = 0; i < 5; i++) {
        char sql[128];
        snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, 'v%d');", i, i);
        CHECK(run_sql(db, sql) == 0);
    }

    const uint64_t nframes = 4;
    const uint64_t len = nframes * 4076;
    unsigned char *src = malloc((size_t)len);
    fill_pattern(src, 0, len);

    db_blob_id id = 0;
    CHECK(db_blob_create(db, nframes, 0, &id) == DB_OK);
    CHECK(db_blob_write(db, id, src, len) == DB_OK);

    db_abandon(db);

    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);

    /* 表恢复 */
    Result *r = NULL;
    CHECK(db_exec(db2, "SELECT id FROM t;", &r) == DB_OK);
    CHECK(db_result_rows(r) == 5);
    db_result_free(r);

    /* blob 恢复 */
    unsigned char *dst = malloc((size_t)len);
    memset(dst, 0xAA, (size_t)len);
    uint64_t got = 0;
    CHECK(db_blob_read(db2, id, dst, len, &got) == DB_OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    free(src);
    free(dst);
    db_close(db2);
}

/* ---------------- 6) 对象目录重建 ---------------- */

/*
 * blob 有自己的对象目录（不依赖关系模态的 catalog）。
 * 重启后目录必须重建 —— 否则之前创建的对象就找不到了。
 *
 * 目录的重建方式是「扫全文件找 blob 头页」。
 * 这个测试验证：建 3 个对象 → 重启 → 目录里还是 3 个，id 都对。
 */
static void t6_dir_rebuild(void)
{
    remove(PATH);
    remove("test/tmp/blob_it.db-wal");

    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);

    db_blob_id ids[3];
    for (int i = 0; i < 3; i++) {
        CHECK(db_blob_create(db, 2, 0, &ids[i]) == DB_OK);
        char text[64];
        snprintf(text, sizeof(text), "object-%d", i);
        CHECK(db_blob_write(db, ids[i], text, strlen(text)) == DB_OK);
    }

    CHECK(db_blob_count(db) == 3);

    db_close(db);

    /* 重启 → 目录重建 */
    DB *db2 = NULL;
    CHECK(db_open(PATH, &db2) == DB_OK);
    CHECK(db_blob_count(db2) == 3);

    /* 每个对象的内容都要对 */
    for (int i = 0; i < 3; i++) {
        char text[64];
        snprintf(text, sizeof(text), "object-%d", i);

        unsigned char buf[256];
        uint32_t n = 0;
        CHECK(db_blob_read_frame(db2, ids[i], 0, buf, sizeof(buf), &n) == DB_OK);
        CHECK(n == strlen(text));
        CHECK(memcmp(buf, text, n) == 0);
    }

    db_close(db2);
}

int main(void)
{
    printf("test_blob（集成）:\n");

    t1_one_mb();
    t2_restart();
    t3_crash_recovery();
    t4_coexist();
    t5_both_crash();
    t6_dir_rebuild();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过 [PASS]\n");
    return 0;
}
