/*
 * test_query.c — 查询层验收（走公开 API）
 *
 * 9 大项：建表/插入/查询/WHERE/三值逻辑/LIMIT/更新/删除/错误处理
 * 外加 4d：db_result_is_null —— NULL 与字面 "NULL" 的可观察区分（B3 首个切片）
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

#define PATH "test/tmp/query.db"

static DB *db;

/* 执行不关心结果的语句 */
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

/* 执行查询，把结果压成 "r1c1,r1c2|r2c1,r2c2" 便于断言 */
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

/* 返回受影响行数；出错返回 -1 */
/* 结果行数 */
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

static db_err err_of(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    return e;
}

int main(void)
{
    remove(PATH);
    printf("test_query:\n");

    CHECK(db_open(PATH, &db) == DB_OK);

    /* ---- 1) 建表 ---- */
    exec_ok("CREATE TABLE t (id INT NOT NULL, name TEXT, age INT);");
    exec_ok("CREATE TABLE empty_t (a INT);");

    /* ---- 2) 插入 ---- */
    CHECK(affected("INSERT INTO t VALUES (1, 'alice', 30);") == 1);
    CHECK(affected("INSERT INTO t VALUES (2, 'bob', 25), (3, NULL, 40);") == 2);
    CHECK(affected("INSERT INTO t VALUES (4, 'dave', 50), (5, 'eve', 60), (6, 'frank', 70);") == 3);

    /* ---- 3) 查询 ---- */
    CHECK(strcmp(q("SELECT * FROM t WHERE id = 1;"), "1,alice,30") == 0);
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 3;"), "NULL") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age > 45;"), "4|5|6") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age >= 50 AND age < 70;"), "4|5") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 1 OR id = 2;"), "1|2") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE NOT (age > 45);"), "1|2|3") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE name = 'bob';"), "2") == 0);

    /* ---- 4) NULL 与三值逻辑 ---- */
    CHECK(strcmp(q("SELECT id FROM t WHERE name IS NULL;"), "3") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE name IS NOT NULL;"), "1|2|4|5|6") == 0);
    /* NULL = 'x' 结果是 NULL，不匹配 */
    CHECK(strcmp(q("SELECT id FROM t WHERE name = 'zzz';"), "") == 0);
    /* false AND NULL = false；true OR NULL = true */
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 3 AND name = 'zzz';"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 3 OR name = 'zzz';"), "3") == 0);

    /*
     * ---- 4b) 真 AND 空 = 空（v1.1 步骤 6 反向验证补的） ----
     *
     * 【为什么原来抓不到】
     * 顶层 WHERE 里"空"和"假"都是**不匹配**，所以 AND 真值表里
     * (真, 空) 被写错成"假"也看不出任何差别。
     * 只有 NOT（或 IS NULL）能把两者区分开 —— 而原来的测试没有这两条。
     *
     * 这是反向验证的价值：注入一个 bug，测试不红，
     * 说明**测试有盲区**，不是"bug 不严重"。
     *
     * 只有 id=3 那行的 name 是 NULL（其余行是"假"不是"空"）：
     *
     *   id=3   : (id>0)=真, (name='zzz')=空 → 真 AND 空 = 空 → NOT 空 = 空 → **不匹配**
     *   其余行 : (id>0)=真, (name='zzz')=假 → 真 AND 假 = 假 → NOT 假 = 真 → 匹配
     *
     * 所以正确结果是 1,2,4,5,6（少了 id=3）。
     * 若把"真 AND 空"错写成"假"，id=3 会冒出来 → 结果变成 6 行 → 测试红 ✓
     */
    CHECK(strcmp(q("SELECT id FROM t WHERE NOT (id > 0 AND name = 'zzz');"),
                 "1|2|4|5|6") == 0);

    /*
     * 同一件事的另一条路：把"空"显式判出来。
     * 这条走的是**逐行路径**（IS NULL 的左边不是列），
     * 上一条走的是**列式路径** —— 两条路都要被覆盖。
     *
     * 只有 id=3 的表达式结果是"空"，其余行是"假"（IS NULL 为假）。
     * 若把"真 AND 空"错写成"假"，id=3 就不见了 → 结果变成空集 → 测试红 ✓
     */
    CHECK(strcmp(q("SELECT id FROM t WHERE (id > 0 AND name = 'zzz') IS NULL;"),
                 "3") == 0);

    /*
     * ---- 4c) 列式路径的"真 AND 空" ----
     *
     * 【为什么还要单独一条】
     * 上面两条的右操作数是 `name`（TEXT）—— 列式求值不支持文本比较，
     * 整条语句退回**逐行路径**。而列式路径的 AND 真值表是**另一份实现**
     * （exec_eval.c 的 AND3 表），不测它就等于没测。
     *
     * 列式要求**两边都是数值列**，所以另建一张带数值 NULL 的小表。
     *
     *   (1, NULL): a>0 真, b=99 空 → 真 AND 空 = 空 → NOT 空 = 空 → 不匹配
     *   (1, 5)   : a>0 真, b=99 假 → 真 AND 假 = 假 → NOT 假 = 真 → 匹配
     *
     * 若把"真 AND 空"错写成"假"，第一行会冒出来（2 行）→ 测试红 ✓
     */
    exec_ok("CREATE TABLE tri (a INT, b INT);");
    exec_ok("INSERT INTO tri VALUES (1, NULL), (1, 5);");
    CHECK(count_rows("SELECT a FROM tri WHERE NOT (a > 0 AND b = 99);") == 1);
    /* 同一份语义再验一次正向的（真 AND 空 在顶层 = 不匹配，两条路径都一样） */
    CHECK(count_rows("SELECT a FROM tri WHERE a > 0 AND b = 99;") == 0);

    /*
     * ---- 4d) NULL 可观察性：db_result_is_null ----
     *
     * 【为什么需要这个 API】
     * 结果集把所有 cell 都格式化成字符串，NULL 写成字面 "NULL"。
     * 用户真实存了一行文本 'NULL' 时，两者在 db_result_value 里完全同形，
     * 调用方**没有任何手段**区分。这不是"输出格式丑"，是信息丢失。
     *
     * 新增的 db_result_is_null 查结果集内的 NULL 位图：
     *   - 有效 NULL 格 → 1
     *   - 其余一切（含无效输入）→ 0
     * 并且 db_result_value 行为**一字不改**（向后兼容）。
     *
     * 下面的断言按"先 RED 后 GREEN"写：桩实现恒返回 0 时，
     * 所有期望 1 的地方都会红，从而证明这些断言真的在测东西。
     */

    /* (1) 单格 NULL：新 API 说它是 NULL，旧 API 仍是字符串 "NULL" */
    {
        Result *r = NULL;
        CHECK(db_exec(db, "SELECT name FROM t WHERE id = 3;", &r) == DB_OK);
        CHECK(db_result_rows(r) == 1);
        CHECK(db_result_cols(r) == 1);
        CHECK(db_result_is_null(r, 0, 0) == 1);
        CHECK(strcmp(db_result_value(r, 0, 0), "NULL") == 0);
        db_result_free(r);
    }

    /* (2) 字面 "'NULL'" 与真 NULL 同形不同义 —— 本 API 的核心用例 */
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
    CHECK(strcmp(q("SELECT id FROM nullsec WHERE name IS NULL;"), "1") == 0);

    /*
     * (3) 跨 NULL 位图字节边界（8 位一字节 ⇒ 至少 9 格）+ 多行多列
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
     * (4) 结果行数超过 Builder 初始 capacity 后，grow 不丢 NULL bits
     *
     * b_init 的 capacity 兜底逻辑是 `nrows_hint > 0 ? nrows_hint : 8`；
     * 而 exec_select 传的 hint 是 `st->limit > 0 ? st->limit : 64`。
     * 所以不带 LIMIT 的 SELECT 起点是 64 行 —— 要真的触发 b_grow，
     * 结果必须超过 64 行，翻倍到 128。
     *
     * 这里插 80 行（5 条 INSERT × 16 行，单条上限 SQL_MAX_ROWS=16），
     * 每 7 行一个 NULL：位图在 grow 之后仍必须逐位对齐，
     * 若 b_grow 漏拷或漏初始化新增位，第 65 行往后会错位。
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
     * (5) 无效输入一律返回 0，不崩、不误报
     *
     * 包括：NULL Result 指针、无 QueryResult（非 SELECT 语句）、
     * 空结果、负索引、越界行列。
     * 列范围检查必须**独立**于行范围检查。
     */
    CHECK(db_result_is_null(NULL, 0, 0) == 0);
    {
        Result *r = NULL;
        /* 非 SELECT（INSERT 只有 affected，qr 的存在不代表有格） */
        /* 用独立表 nullprobe，避免污染 empty_t（第 9 项断言它必须为空） */
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
     * (6) 列越界不能"别名"到下一行
     *
     * 位图是 row-major 连续的：row * ncols + col。
     * 所以 (0, 2) 与 (1, 0) 在 2 列的表里**恰好是同一个位号**。
     * 若 accessor 只挡列负值、不挡列上界，(0, 2) 就会读到下一行的格，
     * 把"(1,0) 是 NULL"错报成"(0,2) 是 NULL"。
     *
     * 构造必须让**被别名撞上的那一格确实是 NULL**：
     *   row0 = ('w', NULL)  → bit1 = (0,1) NULL
     *   row1 = ('y', 'z')   → bit2 = (1,0) 非 NULL
     * 请求 (0, 2) → 位号 2 → 不加列上界会读到 (1,0)，返回 0（测不出来）；
     * 所以反过来构造，让 (1,0) 是 NULL：
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
     * (7) 位图容量 = capacity（不是 nrows）：满容量的最后一行也要写得进
     *
     * 不带 LIMIT 的 SELECT 用 hint=64，测试覆盖不到"容量刚好用满"。
     * 用 LIMIT 3 把 hint 压到 3，再让结果恰好 3 行、且**最后一行的最后一格是 NULL**：
     *   b_push 写位图时 nrows 仍是旧值（2），若按 `nrows` 判容量/越界，
     *   第 3 行（下标 2）的 NULL 就写不进去 → 本断言红。
     *
     * 同时这一格 3×2 = 6 个 cell 不是 8 的整数倍，
     * 位图必须按字节向上取整分配（6 个 cell 也要 1 个字节），
     * 若漏掉取整就会越界写。
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
     * (8) 非 Builder 路径构造的结果集：nullbits 为 NULL，必须安全回退 0
     *
     * exec_write.c 里 VACUUM 手工 calloc 一个 6 列 1 行的统计结果集，
     * 它的 cells 全是格式化数字、不存在 NULL 格，所以不建位图。
     * 若 db_result_is_null 少了"无位图 → 0"的回退，
     * 这里会直接解引用 NULL 崩掉（不是断言失败，是整个测试进程挂掉）。
     *
     * 用独立表 vacprobe，避免动到 t / wide 等他处断言依赖的表。
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

    /* ---- 5) LIMIT ---- */
    CHECK(strcmp(q("SELECT id FROM t LIMIT 2;"), "1|2") == 0);
    CHECK(strcmp(q("SELECT id FROM t LIMIT 0;"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t WHERE age > 0 LIMIT 3;"), "1|2|3") == 0);

    /* ---- 6) 更新 ---- */
    CHECK(affected("UPDATE t SET age = 99 WHERE id = 2;") == 1);
    CHECK(strcmp(q("SELECT age FROM t WHERE id = 2;"), "99") == 0);
    CHECK(affected("UPDATE t SET name = 'ALL' WHERE age > 45;") == 4);  /* 99/50/60/70 */
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 4;"), "ALL") == 0);

    /* ---- 7) 删除 ---- */
    CHECK(affected("DELETE FROM t WHERE age = 99;") == 1);
    CHECK(strcmp(q("SELECT id FROM t WHERE id = 2;"), "") == 0);
    CHECK(strcmp(q("SELECT id FROM t;"), "1|3|4|5|6") == 0);

    /* ---- 8) 错误处理 ---- */
    CHECK(err_of("SELECT * FROM nope;") == DB_E_NOTFOUND);
    CHECK(err_of("SELECT nope FROM t;") == DB_E_NOTFOUND);
    CHECK(err_of("SELEC * FROM t;") == DB_E_PARSE);
    CHECK(err_of("SELECT * FROM t WHERE;") == DB_E_PARSE);
    CHECK(err_of("INSERT INTO t VALUES (1);") == DB_E_INVAL);        /* 列数不匹配 */
    CHECK(err_of("INSERT INTO t VALUES (NULL, 'x', 1);") == DB_E_INVAL);  /* id NOT NULL */

    /* ---- 9) 重启后数据还在 ---- */
    db_close(db);
    CHECK(db_open(PATH, &db) == DB_OK);
    CHECK(strcmp(q("SELECT id FROM t;"), "1|3|4|5|6") == 0);
    CHECK(strcmp(q("SELECT name FROM t WHERE id = 1;"), "alice") == 0);
    CHECK(strcmp(q("SELECT a FROM empty_t;"), "") == 0);

    db_close(db);

    if (failures == 0) {
        printf("  9 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
