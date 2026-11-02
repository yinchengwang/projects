/*
 * test_crash.c — 崩溃恢复验收
 *
 * "断电"用 db_abandon 模拟：不刷脏页、不写 CHECKPOINT，直接丢句柄。
 * 等价于进程被 kill -9 —— 日志已 fsync，页还在内存里没落盘。
 *
 * 验收：每一轮"写一批 → 断电 → 重开"，已提交的数据必须一行不少。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "common/crc.h"
#include "common/types.h"
#include "wal/wal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/crash.db"

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

int main(void)
{
    printf("test_crash:\n");

    remove(PATH);
    remove(PATH "-wal");

    /* ---- 1) 干净建表 ---- */
    DB *db = NULL;
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(run_sql(db, "CREATE TABLE t (id INT NOT NULL, name TEXT, score INT);") == 0);
    CHECK(run_sql(db, "CREATE INDEX idx_id ON t (id);") == 0);
    CHECK(db_close(db) == DB_OK);

    /* ---- 2) 20 轮：写一批 → 断电 → 恢复 ---- */
    int expect = 0;

    for (int round = 0; round < 20; round++) {
        int batch = 1 + (round % 5) * 3;     /* 1,4,7,10,13 循环 */

        db = NULL;
        if (db_open(PATH, &db) != DB_OK) {
            printf("    FAIL 第 %d 轮打开失败\n", round);
            failures++;
            break;
        }

        for (int i = 0; i < batch; i++) {
            char sql[256];
            snprintf(sql, sizeof(sql),
                     "INSERT INTO t VALUES (%d, 'row', %d);", expect + i, i);
            if (run_sql(db, sql) != 0) {
                printf("    FAIL 第 %d 轮插入失败\n", round);
                failures++;
                break;
            }
        }
        expect += batch;

        db_abandon(db);          /* 断电：脏页全丢 */
        db = NULL;

        if (failures)
            break;

        if (db_open(PATH, &db) != DB_OK) {
            printf("    FAIL 第 %d 轮恢复失败\n", round);
            failures++;
            break;
        }
        int n = count_rows(db);
        if (n != expect) {
            printf("    FAIL 第 %d 轮：期望 %d 行，恢复出 %d 行\n", round, expect, n);
            failures++;
        }
        CHECK(db_close(db) == DB_OK);
        if (failures)
            break;
    }
    printf("    （20 轮断电 + 恢复，最终 %d 行）\n", expect);

    /* ---- 3) 恢复后索引必须仍然可用，且是 key 顺序 ---- */
    CHECK(db_open(PATH, &db) == DB_OK);
    {
        CHECK(has_id(db, 3) == 1);
        CHECK(has_id(db, 999) == 0);

        Result *r = NULL;
        CHECK(db_exec(db, "SELECT id FROM t LIMIT 5;", &r) == DB_OK);
        if (r) {
            for (int i = 0; i < db_result_rows(r); i++)
                CHECK(atoi(db_result_value(r, i, 0)) == i);   /* 走索引 → 有序 */
            db_result_free(r);
        }
    }
    CHECK(db_close(db) == DB_OK);

    /* ---- 4) 日志尾部被截断（写到一半断电）---- */
    {
        FILE *f = fopen(PATH "-wal", "r+b");
        CHECK(f != NULL);
        if (f) {
            fseek(f, 0, SEEK_END);
            long sz = ftell(f);
            if (sz > 10) {
#ifdef _WIN32
                extern int _chsize(int, long);
                _chsize(_fileno(f), sz - 10);
#else
                (void)ftruncate(fileno(f), sz - 10);
#endif
            }
            fclose(f);
        }

        db = NULL;
        CHECK(db_open(PATH, &db) == DB_OK);    /* 不能崩 */
        if (db) {
            int n = count_rows(db);
            CHECK(n >= expect - 3);            /* 最多丢最后几条 */
            CHECK(n <= expect);
            CHECK(db_close(db) == DB_OK);
        }
    }

    /* ---- 5) 恢复幂等：连开两次结果一致 ---- */
    CHECK(db_open(PATH, &db) == DB_OK);
    int n1 = count_rows(db);
    CHECK(db_close(db) == DB_OK);
    CHECK(db_open(PATH, &db) == DB_OK);
    int n2 = count_rows(db);
    CHECK(db_close(db) == DB_OK);
    CHECK(n1 == n2);

    /* ---- 6) 未提交的事务必须被撤销 ----
     *
     * 这是 WAL 语义的核心：崩溃瞬间没提交的事务，重开后不能存在。
     *
     * 怎么造这个场景？正常路径每条语句结束就 COMMIT 了，
     * 所以要手动往 WAL 里塞一条没有 COMMIT 的 INSERT。
     */
    {
        DB *d2 = NULL;
        CHECK(db_open(PATH, &d2) == DB_OK);
        if (d2) {
            int before = count_rows(d2);

            /* 手动往 WAL 尾部追加一条"插入"，但不写 COMMIT */
            FILE *f = fopen(PATH "-wal", "r+b");
            CHECK(f != NULL);
            if (f) {
                /*
                 * WAL 记录格式（28 字节头 + 载荷 + crc32）：
                 *   0 lsn / 4 prev_lsn / 8 xid / 12 type / 16 page_id
                 *   20 slot / 22 blen / 24 alen / 26 pad
                 *
                 * 用一个真实存在的页号（用户表的根页），
                 * 但用一个绝不会被提交的 xid。
                 */
                u8 hdr[WAL_HDR];
                u32 xid = 999999;       /* 这个 xid 从来没有 COMMIT 过 */
                u32 lsn = 1000000;
                u32 page_id = 1;        /* 用户表 t 的根页（bootstrap 后的第 1 页）*/
                u16 slot = 999;         /* 一个肯定不存在的 slot */
                u8 payload[8] = { 0xFF, 0xFF, 0xFF, 0xFF, 0, 0, 0, 0 };

                memset(hdr, 0, sizeof(hdr));
                put_u32(hdr + 0, lsn);
                put_u32(hdr + 4, 0);            /* prev_lsn */
                put_u32(hdr + 8, xid);
                hdr[12] = WAL_INSERT;
                put_u32(hdr + 16, page_id);
                put_u16(hdr + 20, slot);
                put_u16(hdr + 22, 0);           /* blen */
                put_u16(hdr + 24, (u16)sizeof(payload));

                /* crc32 覆盖 "header + payload"：
                 * 分段累加，种子 0xFFFFFFFF，最后取反 */
                u32 crc = crc32_update(0xFFFFFFFFu, hdr, sizeof(hdr));
                crc = crc32_update(crc, payload, sizeof(payload));
                crc = ~crc;

                fseek(f, 0, SEEK_END);
                fwrite(hdr, 1, sizeof(hdr), f);
                fwrite(payload, 1, sizeof(payload), f);
                fwrite(&crc, 1, 4, f);
                fclose(f);
            }

            db_abandon(d2);      /* 断电：这条日志没 COMMIT */

            /* 重开：这条未提交的插入必须被撤销，行数不能变 */
            DB *d3 = NULL;
            CHECK(db_open(PATH, &d3) == DB_OK);
            if (d3) {
                int after = count_rows(d3);
                CHECK(after == before);   /* 一行都不能多 */
                CHECK(db_close(d3) == DB_OK);
            }
        }
    }

    if (failures == 0) {
        printf("  崩溃恢复验收通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
