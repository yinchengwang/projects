/*
 * test_index.c — 索引接入验收
 *
 * 9 项。其中第 2 项用"返回顺序"证明真的走了索引：
 * 全表扫按插入顺序吐行，索引扫按 key 顺序吐行。
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

#define PATH "test/tmp/index.db"

static DB *db;

static void exec_ok(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 执行失败: %s → %s\n", sql, db_strerror(e));
        failures++;
    }
    if (r)
        db_result_free(r);
}

static char *q(const char *sql)
{
    static char buf[8192];
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);

    buf[0] = '\0';
    if (e != DB_OK) {
        snprintf(buf, sizeof(buf), "<ERR:%d>", (int)e);
        return buf;
    }

    int nrows = db_result_rows(r);
    int ncols = db_result_cols(r);
    size_t off = 0;

    for (int i = 0; i < nrows; i++) {
        if (i && off < sizeof(buf) - 1)
            buf[off++] = '|';
        for (int c = 0; c < ncols; c++) {
            if (c && off < sizeof(buf) - 1)
                buf[off++] = ',';
            const char *v = db_result_value(r, i, c);
            size_t n = strlen(v);
            if (off + n >= sizeof(buf) - 1)
                n = sizeof(buf) - 2 - off;
            memcpy(buf + off, v, n);
            off += n;
        }
    }
    buf[off] = '\0';
    db_result_free(r);
    return buf;
}

/*
 * 结果集**按行排序后**拼接。
 *
 * 【为什么要这个】
 * v1.1 步骤 4 的 CBO 让小表（nrows_est < 64）**恒走全表扫** ——
 * 于是"返回顺序"不再等于索引的 key 顺序，而是插入顺序。
 *
 * 但绝大多数断言关心的是**集合语义**（有哪些行），不是顺序。
 * 所以这里按行排序后再比：既不受 CBO 决策影响，也仍然能抓到"漏行/多行"。
 *
 * "真的走了索引"由下面的大表用例单独验证（那里顺序是确定的）。
 */
static char *qs(const char *sql)
{
    static char buf[8192];
    static char rows[256][256];

    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);

    buf[0] = '\0';
    if (e != DB_OK) {
        snprintf(buf, sizeof(buf), "<ERR:%d>", (int)e);
        if (r)
            db_result_free(r);
        return buf;
    }

    int nrows = db_result_rows(r);
    int ncols = db_result_cols(r);
    if (nrows > 256)
        nrows = 256;

    for (int i = 0; i < nrows; i++) {
        size_t off = 0;
        rows[i][0] = '\0';
        for (int c = 0; c < ncols; c++) {
            if (c && off < sizeof(rows[i]) - 1)
                rows[i][off++] = ',';
            const char *v = db_result_value(r, i, c);
            size_t n = strlen(v);
            if (off + n >= sizeof(rows[i]) - 1)
                n = sizeof(rows[i]) - 2 - off;
            memcpy(rows[i] + off, v, n);
            off += n;
        }
        rows[i][off] = '\0';
    }
    db_result_free(r);

    /* 插入排序：行数不多，够用 */
    for (int i = 1; i < nrows; i++) {
        char tmp[256];
        memcpy(tmp, rows[i], sizeof(tmp));
        int j = i;
        while (j > 0 && strcmp(rows[j - 1], tmp) > 0) {
            memcpy(rows[j], rows[j - 1], sizeof(rows[j]));
            j--;
        }
        memcpy(rows[j], tmp, sizeof(rows[j]));
    }

    size_t off = 0;
    for (int i = 0; i < nrows; i++) {
        if (i && off < sizeof(buf) - 1)
            buf[off++] = '|';
        size_t n = strlen(rows[i]);
        if (off + n >= sizeof(buf) - 1)
            n = sizeof(buf) - 2 - off;
        memcpy(buf + off, rows[i], n);
        off += n;
    }
    buf[off] = '\0';
    return buf;
}

static int affected(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_affected(r);
    db_result_free(r);
    return n;
}

/* 结果行数（这个版本还没有 COUNT(*)，只能数行） */
static int count_rows(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

int main(void)
{
    remove(PATH);
    printf("test_index:\n");

    CHECK(db_open(PATH, &db) == DB_OK);

    /* ---- 1) 建表 + 乱序插入 ---- */
    exec_ok("CREATE TABLE t (id INT NOT NULL, name TEXT, score INT);");
    exec_ok("INSERT INTO t VALUES (5,'e',50),(3,'c',30),(1,'a',-10),(4,'d',40),(2,'b',-5);");

    /* ---- 2) CBO：小表恒全扫（A5 的一半）---- */
    CHECK(strcmp(q("SELECT id FROM t WHERE id > 0;"), "5|3|1|4|2") == 0);   /* 全表扫：插入顺序 */
    exec_ok("CREATE INDEX idx_id ON t (id);");
    /*
     * 建了索引，但表只有 5 行 < CBO_SMALL_TABLE(64) → CBO 仍选全扫
     * → 顺序还是插入顺序。
     *
     * 【这是 v1.1 步骤 4 的行为变化】
     * 以前是"有索引必走索引"（v0.9 把索引选择下沉到 AM 时的做法），
     * 现在多了"值不值得"这一层。小表走索引要多跳几层树，
     * 而整张表可能就待在一个页里 —— 全扫更快。
     */
    CHECK(strcmp(q("SELECT id FROM t WHERE id > 0;"), "5|3|1|4|2") == 0);

    /* ---- 2b) 大表（> 64 行）→ CBO 走索引：顺序 = key 顺序 ---- */
    {
        exec_ok("CREATE TABLE big (id INT NOT NULL)");
        exec_ok("BEGIN");
        /*
         * 乱序插入 200 行：i*37 % 200 是一个排列（37 与 200 互质）。
         * 插入顺序：0,37,74,111,... —— 前三个是 37,74,111
         * 索引顺序：0,1,2,...
         * 所以 LIMIT 3 的结果能直接区分"走了索引"和"全扫"。
         */
        for (int base = 0; base < 200; base += 16) {
            char sql[512];
            int n = snprintf(sql, sizeof(sql), "INSERT INTO big VALUES ");
            for (int k = 0; k < 16 && base + k < 200; k++) {
                int i = base + k;
                n += snprintf(sql + n, sizeof(sql) - (size_t)n, "%s(%d)",
                              k ? "," : "", (i * 37) % 200);
            }
            exec_ok(sql);
        }
        exec_ok("COMMIT");
        exec_ok("CREATE INDEX idx_big ON big (id);");

        /* 索引顺序：0,1,2（全扫会是 37,74,111） */
        CHECK(strcmp(q("SELECT id FROM big WHERE id >= 0 LIMIT 3;"), "0|1|2") == 0);
        /* 范围也走索引 */
        CHECK(strcmp(q("SELECT id FROM big WHERE id >= 197 LIMIT 3;"), "197|198|199") == 0);
        /* 集合语义：200 行一行不少 */
        CHECK(count_rows("SELECT id FROM big WHERE id >= 0;") == 200);
    }

    /* ---- 3) 等值 ---- */
    CHECK(strcmp(qs("SELECT name FROM t WHERE id = 3;"), "c") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id = 99;"), "") == 0);

    /* ---- 4) 范围 ---- */
    CHECK(strcmp(qs("SELECT id FROM t WHERE id >= 3;"), "3|4|5") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id > 3;"), "4|5") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id < 3;"), "1|2") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id <= 2;"), "1|2") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id >= 2 AND id <= 4;"), "2|3|4") == 0);

    /* ---- 5) 索引 + 剩余条件 ---- */
    exec_ok("CREATE INDEX idx_name ON t (name);");
    CHECK(strcmp(qs("SELECT id FROM t WHERE id >= 2 AND name = 'c';"), "3") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id > 1 AND name > 'c';"), "4|5") == 0);

    /* ---- 6) 负数 key（符号位翻转）---- */
    exec_ok("CREATE INDEX idx_score ON t (score);");
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = -10;"), "1") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score < 0;"), "1|2") == 0);       /* -10, -5 */
    CHECK(strcmp(qs("SELECT id FROM t WHERE score > -10;"), "2|3|4|5") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score >= -10 AND score <= 30;"), "1|2|3") == 0);

    /* ---- 7) TEXT 索引 ---- */
    CHECK(strcmp(qs("SELECT id FROM t WHERE name = 'd';"), "4") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE name >= 'c';"), "3|4|5") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE name < 'c';"), "1|2") == 0);

    /* ---- 8) 写路径维护索引 ---- */
    CHECK(affected("INSERT INTO t VALUES (6,'f',60);") == 1);
    CHECK(strcmp(qs("SELECT name FROM t WHERE id = 6;"), "f") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = 60;"), "6") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE name = 'f';"), "6") == 0);

    CHECK(affected("UPDATE t SET score = 999 WHERE id = 2;") == 1);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = 999;"), "2") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = -5;"), "") == 0);        /* 旧 key 已删 */

    CHECK(affected("UPDATE t SET id = 20 WHERE id = 3;") == 1);
    CHECK(strcmp(qs("SELECT name FROM t WHERE id = 20;"), "c") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id = 3;"), "") == 0);

    CHECK(affected("DELETE FROM t WHERE id = 4;") == 1);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id = 4;"), "") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE name = 'd';"), "") == 0);

    /* ---- 9) 重开：索引元数据与内容都还在 ---- */
    db_close(db);
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(strcmp(qs("SELECT id FROM t WHERE id = 20;"), "20") == 0);
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = 999;"), "2") == 0);
    /*
     * 小表 → CBO 选全扫；顺序不重要，比集合。
     * 注意 qs() 是**按字符串排序**，所以 20 排在 2 后面、5 前面。
     */
    CHECK(strcmp(qs("SELECT id FROM t WHERE id > 0;"), "1|2|20|5|6") == 0);

    /* 没索引的列仍然能查（全表扫）*/
    CHECK(strcmp(qs("SELECT id FROM t WHERE score = 999 AND id > 0;"), "2") == 0);

    /* ---- 10) 重复列值 + 索引：走索引不能丢行 ---- */
    /*
     * 【背景：v1.1 的执行器基准撞出来的既有 bug】
     *
     * 索引 key 原来只编码**列值**。同一列值的多行共用一个 key：
     *
     *   - 叶子分裂时提升的 separator = key[mid]，左页保留 key[0..mid-1]
     *   - 内部节点的不变式是「左子树所有 key **严格小于** separator」
     *   - 值重复时左页里可能有 key[mid-1] == key[mid] —— 不变式破了
     *   - 查找时 descend 用 upper_bound，K == sep 会走到**右**子树，
     *     左页里那些同值行**再也够不着**
     *
     * 症状：走索引的查询比全扫**少行**，而且不报任何错。
     *
     * 【为什么前面 9 项没抓到】
     * 它们只有 5 行、值全唯一 —— 一个叶子能放 ~170 条，
     * 5 行永远不会分裂，不变式破不了。
     *
     * 所以这里必须插够多行：2000 行、每个值 100 行，
     * 分裂点必然落在重复值组内部。
     */
    {
        exec_ok("CREATE TABLE dup (id INT, grp INT, tag TEXT);");
        char sql[8192];
        for (int base = 0; base < 2000; base += 16) {
            int n = 0;
            n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                          "INSERT INTO dup VALUES ");
            for (int k = 0; k < 16 && base + k < 2000; k++) {
                int i = base + k;
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,%d,'%c')", k ? "," : "",
                              i, i % 20, (i % 2) ? 'b' : 'a');
            }
            exec_ok(sql);
        }

        static const char *probes[] = {
            "SELECT id FROM dup WHERE grp = 0;",
            "SELECT id FROM dup WHERE grp = 19;",
            "SELECT id FROM dup WHERE grp >= 10;",
            "SELECT id FROM dup WHERE grp > 10;",
            "SELECT id FROM dup WHERE grp <= 10;",
            "SELECT id FROM dup WHERE grp < 10;",
            "SELECT id FROM dup WHERE grp = 7 AND id > 100;",
        };
        const int nprobe = (int)(sizeof(probes) / sizeof(probes[0]));

        int before[8];
        for (int i = 0; i < nprobe; i++)
            before[i] = count_rows(probes[i]);

        exec_ok("CREATE INDEX idx_dup_grp ON dup (grp);");

        /* 建索引前后，行数必须一模一样 —— 这就是"走索引不丢行" */
        for (int i = 0; i < nprobe; i++) {
            int after = count_rows(probes[i]);
            CHECK(before[i] == after);
        }

        /* 全表扫的答案是确定的，显式写死期望值 */
        CHECK(count_rows("SELECT id FROM dup WHERE grp = 0;") == 100);
        CHECK(count_rows("SELECT id FROM dup WHERE grp >= 10;") == 1000);
        CHECK(count_rows("SELECT id FROM dup WHERE grp > 10;") == 900);
        CHECK(count_rows("SELECT id FROM dup WHERE grp <= 10;") == 1100);
        CHECK(count_rows("SELECT id FROM dup WHERE grp < 10;") == 1000);
    }

    /* ---- 11) 重复 TEXT 值 + 索引（复合键的文本终止符路径）---- */
    {
        exec_ok("CREATE INDEX idx_dup_tag ON dup (tag);");
        CHECK(count_rows("SELECT id FROM dup WHERE tag = 'a';") == 1000);
        CHECK(count_rows("SELECT id FROM dup WHERE tag = 'b';") == 1000);
        CHECK(count_rows("SELECT id FROM dup WHERE tag >= 'a';") == 2000);
        CHECK(count_rows("SELECT id FROM dup WHERE tag < 'b';") == 1000);
        /* 文本前缀关系：'a' 是 'ab' 的前缀，复合键的比较不能跑进 RID 部分 */
        CHECK(count_rows("SELECT id FROM dup WHERE tag >= 'a' AND grp = 3;") == 100);
    }

    db_close(db);

    if (failures == 0) {
        printf("  11 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
