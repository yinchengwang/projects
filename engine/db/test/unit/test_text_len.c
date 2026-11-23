/*
 * test_text_len.c —— 外部 T_TEXT 进记录：不能写成空串
 *
 * 【这个测试在钉什么】
 * val_to_record() 的 T_TEXT 分支原来直接取 vals[i].s / vals[i].len，
 * 而外部 T_TEXT 的 len 恒为 0（协议是"长度靠 val_detoast 问"）、
 * s[] 里也没有内容 ——
 * 于是 300 字节的外部文本写进记录后只剩 23 字节（只有头），**静默丢数据**。
 *
 * 词法器放宽到 8192B 之后，长 SQL 字面量一律是外部值 ⇒ 这条路径必然被踩到。
 *
 * 另外还钉住 SQL 层的行为：TEXT 列超过 255 字节必须**明确报错**，
 * 而不是截断成 63 字节或存成空串。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "storage/record.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

int main(void)
{
    printf("test_text_len:\n");

    /* ---------------- 1. 外部 T_TEXT 进记录：长度必须完整 ---------------- */
    {
        char payload[512];
        memset(payload, 'z', 300);
        payload[300] = '\0';

        ColDesc col;
        memset(&col, 0, sizeof(col));
        snprintf(col.name, sizeof(col.name), "txt");
        col.type = T_TEXT;
        col.nullable = true;

        /* 外部 T_TEXT：长度靠 detoast 问 */
        DbValue v;
        val_external_text(&v, (u64)(uintptr_t)payload, (u64)strlen(payload),
                          val_mem_detoast, payload);

        u8 rec[4096];
        u16 rlen = 0;
        int rc = val_to_record(&col, 1, &v, rec, sizeof(rec), &rlen);
        CHECK(rc == OK);
        /* 记录 = 头 + 300 字节；此前是 23（内容全丢） */
        CHECK(rlen > 300);

        /*
         * 读回时 DbValue 里只装得下 255 字节（TEXT 列的既有契约：
         * 内联上限 VAL_INLINE_MAX）。
         *
         * ⚠️ 所以这里断言的是"读回不超 255"，
         *    而不是"读回 300" —— 记录里那 300 字节是完整的，
         *    但 DbValue 的内联空间只有 255。
         */
        DbValue back;
        int drc = val_from_record(&col, 1, rec, rlen, &back);
        CHECK(drc == OK);
        CHECK(back.len <= VAL_INLINE_MAX);
    }

    /* ---------------- 2. SQL 层：TEXT 列超 255B 必须报错 ---------------- */
    {
        DB *db = NULL;
        const char *path = "test/tmp/text_len.db";
        remove(path);
        remove("test/tmp/text_len.db-wal");
        remove("test/tmp/text_len.db-clog");

        CHECK(db_open(path, &db) == DB_OK);
        if (!db)
            return 1;

        Result *r = NULL;
        CHECK(db_exec(db, "CREATE TABLE s (id INT, txt TEXT)", &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }

        char sql[1024];
        char doc[512];
        memset(doc, 'y', 300);
        doc[300] = '\0';
        snprintf(sql, sizeof(sql),
                 "INSERT INTO s (id, txt) VALUES (1, '%s')", doc);

        /* 300 字节写 TEXT 列 ⇒ 必须报错（不能静默截断或存空串） */
        CHECK(db_exec(db, sql, &r) != DB_OK);
        if (r) { db_result_free(r); r = NULL; }

        /* 表里不能留下这一行 */
        CHECK(db_exec(db, "SELECT COUNT(*) FROM s", &r) == DB_OK);
        if (r) {
            CHECK(db_result_rows(r) == 1);
            if (db_result_rows(r) == 1)
                CHECK(strcmp(db_result_value(r, 0, 0), "0") == 0);
            db_result_free(r);
            r = NULL;
        }

        /* 255 字节必须能正常写入（上限边界，不是"一律拒绝"） */
        memset(doc, 'y', 255);
        doc[255] = '\0';
        snprintf(sql, sizeof(sql),
                 "INSERT INTO s (id, txt) VALUES (2, '%s')", doc);
        CHECK(db_exec(db, sql, &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }

        CHECK(db_exec(db, "SELECT txt FROM s WHERE id = 2", &r) == DB_OK);
        if (r) {
            CHECK(db_result_rows(r) == 1);
            if (db_result_rows(r) == 1)
                CHECK(strlen(db_result_value(r, 0, 0)) == 255);
            db_result_free(r);
            r = NULL;
        }

        db_close(db);
        remove(path);
        remove("test/tmp/text_len.db-wal");
        remove("test/tmp/text_len.db-clog");
    }

    if (failures == 0) {
        printf("  外部 T_TEXT 记录编码验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
