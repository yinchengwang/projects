/*
 * test_query_bulk.c — 批量 INSERT 与语句容量上限
 *
 * ==================== 这里验两条**不同的**上限 ====================
 *
 *   行数：单条 INSERT 的 VALUES 行数**无上限**，按需增长
 *   列数：单表 / 单语句的列数受 SQL_MAX_COLS 约束
 *
 * 原来这两条都是 16，混在一起很难分清谁限制了什么。
 * 行数在前一个切片里放开（Stmt.vals 改成行指针数组），
 * 列数是执行器与所有模态共享的行表示，另算。
 *
 * ==================== 为什么列数上限要在这里测 ====================
 *
 * 列数超限时**整条语句一行都不插**（不是插一半），这是原子性要求；
 * 而且错误必须是**语法错**（DB_E_PARSE）而不是执行期的半路失败 ——
 * 调用方需要能区分"语法不支持"和"数据有问题"。
 */

#include "query_test_common.h"

#include "query/parser.h"   /* SQL_MAX_COLS：断言要跟着它走，不写死 16 */

#define PATH "test/tmp/query_bulk.db"

/* 造一条 n 行的 INSERT；buf 由调用方给 */
static void build_insert(char *buf, size_t cap, const char *table,
                         int nrows, int mark_null_every)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "INSERT INTO %s VALUES ", table);
    for (int i = 1; i <= nrows; i++) {
        if (mark_null_every > 0 && i % (unsigned)mark_null_every == 0)
            off += (size_t)snprintf(buf + off, cap - off,
                                    "%s(%d, NULL)", i > 1 ? "," : "", i);
        else
            off += (size_t)snprintf(buf + off, cap - off,
                                    "%s(%d, %d)", i > 1 ? "," : "", i, i * 10);
    }
    off += (size_t)snprintf(buf + off, cap - off, ";");
}

/* 造一条 n 列的 CREATE TABLE */
static void build_create(char *buf, size_t cap, const char *table, int ncols)
{
    size_t off = 0;
    off += (size_t)snprintf(buf + off, cap - off, "CREATE TABLE %s (", table);
    for (int i = 0; i < ncols; i++)
        off += (size_t)snprintf(buf + off, cap - off,
                                "%sc%d INT", i ? "," : "", i);
    snprintf(buf + off, cap - off, ");");
}

int main(void)
{
    remove(PATH);
    printf("test_query_bulk:\n");

    CHECK(db_open(PATH, &db) == DB_OK);

    exec_ok("CREATE TABLE bulk (id INT NOT NULL, v INT);");

    /*
     * ---- (1) 20 行一批：超过原来的 16 行硬上限 ----
     *
     * 改前：整条语句报「VALUES 行数超限」，实际插入 0 行。
     */
    {
        char sql[2048];
        build_insert(sql, sizeof(sql), "bulk", 20, 0);
        CHECK(affected(sql) == 20);
        CHECK(count_rows("SELECT id FROM bulk;") == 20);
    }

    /*
     * ---- (2) 100 行一批 × 2，每 7 行一个 NULL ----
     *
     * 批量里的 NULL 也必须进结果集位图 —— 这条同时压
     * "批量 INSERT"与"NULL 可观察性"两个切片的交界。
     */
    for (int batch = 0; batch < 2; batch++) {
        char sql[8192];
        size_t off = 0;
        off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                "INSERT INTO bulk VALUES ");
        for (int i = 0; i < 100; i++) {
            int id = 100 + batch * 100 + i;
            off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                    "%s(%d, %s)", i ? "," : "",
                                    id, (id % 7 == 0) ? "NULL" : "1");
        }
        CHECK(affected(sql) == 100);
    }
    CHECK(count_rows("SELECT id FROM bulk;") == 220);
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT id, v FROM bulk WHERE id >= 100;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 200);
        for (int i = 0; i < 200; i++) {
            int id = 100 + i;
            CHECK(db_result_is_null(r, i, 0) == 0);
            CHECK(db_result_is_null(r, i, 1) == ((id % 7 == 0) ? 1 : 0));
        }
        db_result_free(r);
    }

    /*
     * ---- (3) 1000 行一批：确认没有隐藏的次级上限 ----
     *
     * 翻倍扩容路径要走好几轮（16 → 32 → … → 1024）。
     * 若某处还有"16 的倍数"之类的隐含假设，这一步会露出来。
     */
    exec_ok("CREATE TABLE bulk2 (id INT, v INT);");
    {
        char sql[16384];
        size_t off = 0;
        off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                "INSERT INTO bulk2 VALUES ");
        for (int i = 1; i <= 1000; i++)
            off += (size_t)snprintf(sql + off, sizeof(sql) - off,
                                    "%s(%d, %d)", i > 1 ? "," : "", i, i);
        CHECK(affected(sql) == 1000);
        CHECK(count_rows("SELECT id FROM bulk2;") == 1000);
    }

    /*
     * ---- (4) 列数上限：正好 SQL_MAX_COLS 列可以通过 ----
     */
    {
        char sql[8192];
        build_create(sql, sizeof(sql), "cols_max", SQL_MAX_COLS);
        CHECK(err_of(sql) == DB_OK);

        char ins[16384];
        size_t off = 0;
        off += (size_t)snprintf(ins + off, sizeof(ins) - off,
                                "INSERT INTO cols_max VALUES (");
        for (int i = 0; i < SQL_MAX_COLS; i++)
            off += (size_t)snprintf(ins + off, sizeof(ins) - off,
                                    "%s%d", i ? "," : "", i);
        snprintf(ins + off, sizeof(ins) - off, ");");
        CHECK(affected(ins) == 1);

        Result *r = NULL;
        CHECK(db_exec(db, "SELECT * FROM cols_max;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 1);
        CHECK(db_result_cols(r) == SQL_MAX_COLS);
        CHECK(db_result_is_null(r, 0, SQL_MAX_COLS - 1) == 0);
        db_result_free(r);
    }

    /*
     * ---- (5) 列数 +1 必须被拒，且是**语法错**、一行都不插 ----
     *
     * 用 SQL_MAX_COLS+1 而不是写死 17：上限变化时这条断言自动跟随。
     */
    {
        char sql[8192];
        build_create(sql, sizeof(sql), "cols_over", SQL_MAX_COLS + 1);
        CHECK(err_of(sql) == DB_E_PARSE);

        /* 越界列数同样前进不了——表都没建出来 */
        CHECK(count_rows("SELECT * FROM cols_over;") == -1);
    }

    /*
     * ---- (6) 行内值个数不一致 → 整条语句失败，一行都不插 ----
     *
     * 原子性：不能"前面几行插进去了，后面报错"。
     */
    {
        int before = count_rows("SELECT id FROM bulk;");
        CHECK(err_of("INSERT INTO bulk VALUES (9001, 1), (9002, 2, 3);") == DB_E_PARSE);
        CHECK(count_rows("SELECT id FROM bulk;") == before);
    }

    /*
     * ---- (7) 批量插入的数据重启后还在 ----
     *
     * 批量路径若在 WAL 上走了不同的分支，重启才能看出来。
     */
    db_close(db);
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(count_rows("SELECT id FROM bulk;") == 220);
    CHECK(count_rows("SELECT id FROM bulk2;") == 1000);
    CHECK(count_rows("SELECT * FROM cols_max;") == 1);
    db_close(db);

    return qt_finish("7 组断言全部通过");
}
