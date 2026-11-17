/*
 * test_query_null.c — 结果集 NULL 语义（db_result_is_null）
 *
 * ==================== 为什么需要这个 API ====================
 *
 * 结果集把所有 cell 都格式化成字符串，SQL NULL 写成字面 "NULL"。
 * 用户真实存了一行文本 'NULL' 时，两者在 db_result_value 里完全同形，
 * 调用方**没有任何手段**区分。这不是"输出格式丑"，是信息丢失 ——
 * 嵌入式场景每个调用方都会碰到。
 *
 * 新增的 db_result_is_null 查结果集内的 NULL 位图：
 *   - 有效 NULL 格 → 1
 *   - 其余一切（含无效输入）→ 0
 * 并且 db_result_value 行为**一字不改**（向后兼容）。
 *
 * ==================== 为什么断言要这么写 ====================
 *
 * 全部按"先 RED 后 GREEN"设计：桩实现（恒返回 0）时，
 * 所有期望 1 的地方都会红，从而证明这些断言真的在测东西。
 * 变异测试的存活项也在注释里如实标注了能力边界 ——
 * 有些缺陷（字节数没向上取整、grow 没清零）断言根本看不见，
 * 需要哨兵分配器，那不在本文件的能力范围内。
 */

#include "query_test_common.h"

#define PATH "test/tmp/query_null.db"

int main(void)
{
    remove(PATH);
    printf("test_query_null:\n");

    CHECK(db_open(PATH, &db) == DB_OK);

    exec_ok("CREATE TABLE t (id INT NOT NULL, name TEXT, age INT);");
    exec_ok("INSERT INTO t VALUES (1, 'alice', 30);");
    exec_ok("INSERT INTO t VALUES (2, 'bob', 25), (3, NULL, 40);");

    /* ---- (1) 单格 NULL：新 API 说它是 NULL，旧 API 仍是字符串 "NULL" ---- */
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT name FROM t WHERE id = 3;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 1);
        CHECK(db_result_cols(r) == 1);
        CHECK(db_result_is_null(r, 0, 0) == 1);
        CHECK(strcmp(db_result_value(r, 0, 0), "NULL") == 0);
        db_result_free(r);
    }

    /* ---- (2) 字面 "'NULL'" 与真 NULL 同形不同义 —— 本 API 的核心用例 ---- */
    exec_ok("CREATE TABLE nullsec (id INT NOT NULL, name TEXT);");
    /* id=1 真 NULL；id=2 用户文本 'NULL'；id=3 普通文本 */
    exec_ok("INSERT INTO nullsec VALUES (1, NULL), (2, 'NULL'), (3, 'plain');");
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT id, name FROM nullsec;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 3);

        /* id 列（非 NULL）—— 三行都不是 NULL */
        CHECK(db_result_is_null(r, 0, 0) == 0);
        CHECK(db_result_is_null(r, 1, 0) == 0);
        CHECK(db_result_is_null(r, 2, 0) == 0);

        /* name 列：第一行才是真 NULL */
        CHECK(db_result_is_null(r, 0, 1) == 1);
        CHECK(db_result_is_null(r, 1, 1) == 0);   /* 文本 'NULL' */
        CHECK(db_result_is_null(r, 2, 1) == 0);

        /* 两种语义在旧 API 里同形 —— 这正是要补的洞 */
        CHECK(strcmp(db_result_value(r, 0, 1), "NULL") == 0);
        CHECK(strcmp(db_result_value(r, 1, 1), "NULL") == 0);

        db_result_free(r);
    }
    /* 顺带确认：SQL 层看这两行也不一样（'NULL' 不是 NULL） */
    CHECK(strcmp(qt_dump("SELECT id FROM nullsec WHERE name IS NULL;"), "1") == 0);

    /*
     * ---- (3) 跨 NULL 位图字节边界（8 位一字节 ⇒ 至少 9 格）+ 多行多列 ----
     *
     * 用 3 列 × 4 行 = 12 格，位图要跨两个字节。
     * 期望的 NULL 位置写在下面，任何一位错位都会被抓住。
     *
     *        col0    col1    col2
     *  row0   NULL    1       NULL
     *  row1   2       NULL    3
     *  row2   4       5       NULL
     *  row3   6       7       8
     */
    exec_ok("CREATE TABLE bits (a INT, b INT, c INT);");
    exec_ok("INSERT INTO bits VALUES "
            "(NULL, 1, NULL), (2, NULL, 3), (4, 5, NULL), (6, 7, 8);");
    {
        static const int expect_null[4][3] = {
            {1, 0, 1},
            {0, 1, 0},
            {0, 0, 1},
            {0, 0, 0},
        };
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT a, b, c FROM bits;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 4);
        CHECK(db_result_cols(r) == 3);
        for (int i = 0; i < 4; i++) {
            for (int c = 0; c < 3; c++) {
                CHECK(db_result_is_null(r, i, c) == expect_null[i][c]);
                /* 旧 API 对 NULL 格仍然是 "NULL" 字符串 */
                if (expect_null[i][c])
                    CHECK(strcmp(db_result_value(r, i, c), "NULL") == 0);
            }
        }
        db_result_free(r);
    }

    /*
     * ---- (4) 结果行数超过 Builder 初始 capacity 后，grow 不丢 NULL bits ----
     *
     * b_init 的 capacity 兜底逻辑是 `nrows_hint > 0 ? nrows_hint : 8`；
     * 而 exec_select 传的 hint 是 `st->limit > 0 ? st->limit : 64`。
     * 所以不带 LIMIT 的 SELECT 起点是 64 行 —— 要真的触发 b_grow，
     * 结果必须超过 64 行，翻倍到 128。
     *
     * 这里插 80 行（5 条 INSERT × 16 行；16 只是本用例选的分批大小，
     * 单条 INSERT 的行数已没有上限），每 7 行一个 NULL：
     * 位图在 grow 之后仍必须逐位对齐。
     *
     * ⚠️ 本组测不到"grow 漏初始化新增位"：默认分配器恰好给零页，
     * malloc 版变异会存活，只有毒化分配器能抓。
     */
    exec_ok("CREATE TABLE wide (id INT, v INT);");
    for (int i = 0; i < 80; i += 16) {
        char sql[512];
        size_t off = 0;
        off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                "INSERT INTO wide VALUES ");
        for (int j = 0; j < 16; j++) {
            int id = i + j + 1;
            off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                    "%s(%d, %s)", j ? "," : "",
                                    id, (id % 7 == 0) ? "NULL" : "1");
        }
        exec_ok(sql);
    }
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT id, v FROM wide;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 80);
        CHECK(db_result_cols(r) == 2);
        for (int i = 0; i < 80; i++) {
            int id = i + 1;
            CHECK(db_result_is_null(r, i, 0) == 0);
            CHECK(db_result_is_null(r, i, 1) == ((id % 7 == 0) ? 1 : 0));
            /* 跨过第 64 行（初始 capacity）之后依然要正确 */
            if (id % 7 == 0)
                CHECK(strcmp(db_result_value(r, i, 1), "NULL") == 0);
        }
        db_result_free(r);
    }

    /*
     * ---- (5) 无效输入一律返回 0，不崩、不误报 ----
     *
     * 包括：NULL Result 指针、无 QueryResult（非 SELECT 语句）、
     * 空结果、负索引、越界行列。
     * 列范围检查必须**独立**于行范围检查。
     */
    CHECK(db_result_is_null(NULL, 0, 0) == 0);
    {
        Result *r = NULL;
        exec_ok("CREATE TABLE nullprobe (a INT);");
        CHECK(db_exec(db, "INSERT INTO nullprobe VALUES (7);", &r) == DB_OK);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        CHECK(db_result_is_null(r, -1, 0) == 0);
        CHECK(db_result_is_null(r, 0, -1) == 0);
        db_result_free(r);
    }
    {
        Result *r = NULL;
        /* 空结果集：0 行，但行/列索引依然必须被判为无效 */
        CHECK(db_exec(db, "SELECT a FROM nullprobe WHERE a = 999;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 0);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        CHECK(db_result_is_null(r, -1, 0) == 0);
        CHECK(db_result_is_null(r, 0, -1) == 0);
        CHECK(db_result_is_null(r, 1, 0) == 0);
        db_result_free(r);
    }
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT a, b, c FROM bits WHERE a = 6;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 1);
        CHECK(db_result_cols(r) == 3);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        /*
         * 行合法但列越界 → 0。
         * 3 列的表里 (0,3) 的位号是 3，**(1,0) 才对应位号 3** ——
         * 但这里只有 1 行，位号 3 根本没被写过，所以"漏挡列上界"看不出来。
         * 真正能抓到漏挡的用例见下面 (6)，那里有第二行占用位号。
         */
        CHECK(db_result_is_null(r, 0, 3) == 0);
        CHECK(db_result_is_null(r, 0, -1) == 0);
        CHECK(db_result_is_null(r, 1, 0) == 0);
        CHECK(db_result_is_null(r, 1048576, 0) == 0);   /* 1<<20：必须被行上界挡下 */
        db_result_free(r);
    }
    /* 事务控制语句的 qr 为 NULL */
    {
        Result *r = NULL;
        CHECK(db_exec(db, "BEGIN;", &r) == DB_OK);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        db_result_free(r);
        r = NULL;
        CHECK(db_exec(db, "ROLLBACK;", &r) == DB_OK);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        db_result_free(r);
    }

    /*
     * ---- (6) 列越界不能"别名"到下一行 ----
     *
     * 位图是 row-major 连续的：row * ncols + col。
     * 所以 (0, 2) 与 (1, 0) 在 2 列的表里**恰好是同一个位号**。
     * 若 accessor 只挡列负值、不挡列上界，(0, 2) 就会读到下一行的格。
     *
     * 构造必须让**被别名撞上的那一格确实是 NULL**：
     *   row0 = ('w', 'x')   → bit0,bit1 非 NULL
     *   row1 = (NULL, 'z')  → bit2 = (1,0) NULL
     * 请求 (0, 2) → 位号 2 → 撞上 (1,0) 的 NULL → 不加列上界就错报 1。
     */
    exec_ok("CREATE TABLE alias (a TEXT, b TEXT);");
    exec_ok("INSERT INTO alias VALUES ('w', 'x'), (NULL, 'z');");
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT a, b FROM alias;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 2);
        CHECK(db_result_cols(r) == 2);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        CHECK(db_result_is_null(r, 0, 1) == 0);
        CHECK(db_result_is_null(r, 1, 0) == 1);   /* 被 (0,2) 别名撞上的格 */
        CHECK(db_result_is_null(r, 1, 1) == 0);
        /* 列越界：位号 2 撞上 (1,0) 的 NULL，必须被列上界挡下 */
        CHECK(db_result_is_null(r, 0, 2) == 0);
        db_result_free(r);
    }

    /*
     * ---- (7) 位图容量 = capacity（不是 nrows）：满容量的最后一行也要写得进 ----
     *
     * 用 LIMIT 3 把 hint 压到 3，再让结果恰好 3 行、且**最后一行的最后一格是 NULL**：
     *   b_push 写位图时 nrows 仍是旧值（2），若按 `nrows` 判容量/越界，
     *   第 3 行（下标 2）的 NULL 就写不进去 → 本断言红。
     *
     * ⚠️ 本组测不到"位图字节数漏掉向上取整"：3×2 = 6 格正确实现是 1 字节，
     * `ncells / 8` 会算成 0 字节，但那 1 位越界写落在分配器对齐填充里，
     * 断言看不见（实测存活）。该缺陷只有哨兵 / ASan 能抓。
     */
    exec_ok("CREATE TABLE capsmall (a INT, b TEXT);");
    exec_ok("INSERT INTO capsmall VALUES (1, 'v1'), (2, NULL), (3, 'v3');");
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT a, b FROM capsmall LIMIT 3;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 3);
        CHECK(db_result_cols(r) == 2);
        /* 只有第 2 行（下标 1）是 NULL */
        CHECK(db_result_is_null(r, 0, 1) == 0);
        CHECK(db_result_is_null(r, 1, 1) == 1);
        CHECK(db_result_is_null(r, 2, 1) == 0);

        /* 交换 NULL 位置到**最后一行最后一格**，专打"满容量最后一行" */
        db_result_free(r);
        r = NULL;
        exec_ok("UPDATE capsmall SET b = 'x' WHERE a = 2;");
        exec_ok("UPDATE capsmall SET b = NULL WHERE a = 3;");
        CHECK(db_exec(db, "SELECT a, b FROM capsmall LIMIT 3;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 3);
        CHECK(db_result_is_null(r, 0, 1) == 0);
        CHECK(db_result_is_null(r, 1, 1) == 0);
        CHECK(db_result_is_null(r, 2, 1) == 1);   /* 满容量的最后一行 */
        CHECK(strcmp(db_result_value(r, 2, 1), "NULL") == 0);
        db_result_free(r);
    }

    /*
     * ---- (8) 超大 LIMIT 不能把容量提示变成荒谬的分配（C1 回归）----
     *
     * nrows_hint 直接来自 SQL 的 LIMIT，parser 允许到 INT32_MAX。
     * 修复前：`null_bytes(cap * ncols)` 在 int 里算，
     *   LIMIT 2147483647 + 2 列 → cap*ncols 回绕成 -2 → 0 字节位图，
     *   随后 b_push 往 0 字节缓冲后面写 → 堆越界，free 时崩溃。
     * 现在 b_init 把容量提示 clamp 到 BUILDER_CAP_HINT_MAX（65536），
     * 且乘法全走 size_t，这条路径必须正常出结果且 NULL 位正确。
     *
     * 用 192 行（12 条 INSERT × 16 行，正好整除）压过原来的 128 行崩溃点。
     */
    exec_ok("CREATE TABLE biglimit (a INT, b INT);");
    for (int i = 0; i < 192; i += 16) {
        char sql[512];
        size_t off = 0;
        off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                "INSERT INTO biglimit VALUES ");
        for (int j = 0; j < 16; j++) {
            off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                    "%s(%d, NULL)", j ? "," : "", i + j + 1);
        }
        exec_ok(sql);
    }
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT * FROM biglimit LIMIT 2147483647;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 192);
        CHECK(db_result_cols(r) == 2);
        /* 首尾两行的 NULL 位都要对（跨过原来的崩溃行号） */
        CHECK(db_result_is_null(r, 0, 1) == 1);
        CHECK(db_result_is_null(r, 191, 1) == 1);
        CHECK(db_result_is_null(r, 0, 0) == 0);
        CHECK(db_result_is_null(r, 191, 0) == 0);
        db_result_free(r);
    }

    /*
     * ---- (9) 非 Builder 路径构造的结果集：nullbits 为 NULL，必须安全回退 0 ----
     *
     * exec_write.c 里 VACUUM 手工 calloc 一个 6 列 1 行的统计结果集，
     * 它的 cells 全是格式化数字、不存在 NULL 格，所以不建位图。
     * 若 db_result_is_null 少了"无位图 → 0"的回退，
     * 这里会直接解引用 NULL 崩掉（不是断言失败，是整个测试进程挂掉）。
     */
    exec_ok("CREATE TABLE vacprobe (a INT);");
    exec_ok("INSERT INTO vacprobe VALUES (1);");
    {
        Result *r = NULL;
        CHECK(db_exec(db, "VACUUM vacprobe;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 1);
        CHECK(db_result_cols(r) == 6);
        for (int c = 0; c < 6; c++) {
            CHECK(db_result_is_null(r, 0, c) == 0);
            CHECK(strcmp(db_result_value(r, 0, c), "NULL") != 0);
        }
        db_result_free(r);
    }

    db_close(db);

    return qt_finish("9 组断言全部通过");
}
