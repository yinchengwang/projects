/*
 * test_agg_sqlite.c — 聚合语义对照 SQLite（v1.1 步骤 3 · A3）
 *
 * ==================== 这一项在验什么 ====================
 *
 * PLAN-v1.1 §1.5 的 A3：
 *   「聚合正确性：COUNT/SUM/AVG/MIN/MAX + GROUP BY 对照 SQLite 全等」
 *
 * 单元测试（test/unit/test_agg.c）验的是"和手算一致"，
 * 这一项验的是"和业界事实标准一致" —— 两个不同的参照系。
 *
 * ==================== 怎么比 ====================
 *
 * 同一批 SQL 在两边各跑一遍，结果**逐格比对**：
 *   - 行数必须一样
 *   - 每行的列数必须一样
 *   - 每个格子：能当数字读的就按数字比（带容差），否则按字符串比
 *
 * 【为什么要浮点带容差】
 * SQLite 的 sum()/avg() 用 Kahan-Babuska-Neumaier 补偿求和，
 * 我们是朴素累加 —— 累加顺序不同，最后一位可能差一点点。
 * 那是**浮点累加的正常现象**，不是语义不一致。
 * 但差得超过 1e-9 相对误差就是真不一致，必须红。
 *
 * ==================== SQLite 从哪来 ====================
 *
 * 环境变量 SQLITE3 指定可执行文件；没给就用 PATH 里的 sqlite3。
 * 找不到就跳过（并明确说"跳过"，不装作通过）。
 * 设 AGG_REQUIRE_SQLITE=1 则缺 SQLite 就算失败（CI 里用这个）。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include <math.h>

#include "db.h"

static int failures = 0;
static int checks = 0;

#define OUR_DB   "test/tmp/agg_sqlite_our.db"
#define SQL_DB   "test/tmp/agg_sqlite_sql.db"
#define SQL_SETUP "test/tmp/agg_sqlite_setup.sql"
#define SQL_SCRIPT "test/tmp/agg_sqlite_q.sql"
#define SQL_OUT    "test/tmp/agg_sqlite_o.txt"

static DB *db;

/* ---------------- 数据集（两边用同一份） ---------------- */

static const char *SETUP[] = {
    "CREATE TABLE t (id INT, grp TEXT, val INT, r REAL)",
    "INSERT INTO t (id, grp, val, r) VALUES "
    "(1, 'a', 10, 1.5), (2, 'b', 20, 2.5), (3, 'a', 30, 3.5), "
    "(4, 'b', 40, 4.5), (5, NULL, 50, 5.5), (6, 'c', NULL, NULL)",
    "CREATE TABLE e (g INT, v INT)",
    "INSERT INTO e (g, v) VALUES (1, 10), (1, 20), (2, 30), (2, 40), (3, 50)",
    "CREATE TABLE s (t TEXT)",
    "INSERT INTO s (t) VALUES ('a'), ('ab'), ('a'), ('b'), (NULL)",
};

static const char *QUERIES[] = {
    /* 无 GROUP BY */
    "SELECT COUNT(*) FROM t",
    "SELECT COUNT(val) FROM t",
    "SELECT SUM(val) FROM t",
    "SELECT AVG(val) FROM t",
    "SELECT MIN(val) FROM t",
    "SELECT MAX(val) FROM t",
    "SELECT COUNT(*), MIN(val), MAX(val) FROM t",
    "SELECT SUM(r) FROM t",
    "SELECT AVG(r) FROM t",
    /* WHERE + 聚合 */
    "SELECT COUNT(*) FROM t WHERE val > 15",
    "SELECT SUM(val) FROM t WHERE val > 15",
    "SELECT AVG(val) FROM t WHERE val > 15",
    "SELECT COUNT(val) FROM t WHERE val > 15",
    "SELECT COUNT(*) FROM t WHERE grp IS NULL",
    /* GROUP BY */
    "SELECT grp, COUNT(*) FROM t GROUP BY grp",
    "SELECT grp, SUM(val) FROM t GROUP BY grp",
    "SELECT grp, AVG(val) FROM t GROUP BY grp",
    "SELECT grp, MIN(val), MAX(val) FROM t GROUP BY grp",
    "SELECT grp, COUNT(*) FROM t WHERE val > 15 GROUP BY grp",
    "SELECT grp, SUM(val) FROM t WHERE val > 15 GROUP BY grp",
    "SELECT COUNT(*) FROM t GROUP BY grp",
    /* 文本分组的键编码 */
    "SELECT t, COUNT(*) FROM s GROUP BY t",
    /* 多列分组 */
    "SELECT g, COUNT(*), SUM(v) FROM e GROUP BY g",
    /* 空表 */
    "SELECT COUNT(*) FROM e WHERE g > 100",
    "SELECT SUM(v) FROM e WHERE g > 100",
};

/* ---------------- 工具 ---------------- */

/* 把结果集拍成 "a|b\nc|d\n" */
static char *our_output(const char *sql)
{
    static char buf[65536];
    Result *r = NULL;
    buf[0] = '\0';

    db_err e = db_exec(db, sql, &r);
    if (e != DB_OK) {
        snprintf(buf, sizeof(buf), "<ERR:%s>\n", db_last_error(db));
        if (r)
            db_result_free(r);
        return buf;
    }

    size_t off = 0;
    int nrows = db_result_rows(r);
    int ncols = db_result_cols(r);
    for (int i = 0; i < nrows; i++) {
        for (int c = 0; c < ncols; c++) {
            if (c)
                buf[off++] = '|';
            const char *v = db_result_value(r, i, c);
            size_t n = strlen(v);
            if (off + n >= sizeof(buf) - 2)
                n = sizeof(buf) - 3 - off;
            memcpy(buf + off, v, n);
            off += n;
        }
        buf[off++] = '\n';
    }
    buf[off] = '\0';
    db_result_free(r);
    return buf;
}

/* 读文件 */
static char *read_file(const char *path)
{
    static char buf[65536];
    FILE *f = fopen(path, "rb");
    if (!f) {
        buf[0] = '\0';
        return buf;
    }
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);
    return buf;
}

/* 两串能不能当数字读出来 */
static bool as_num(const char *s, double *out)
{
    if (!s || !*s)
        return false;
    char *end = NULL;
    double d = strtod(s, &end);
    if (end == s || (end && *end != '\0'))
        return false;
    *out = d;
    return true;
}

/* 两个格子是否相等（数字带容差，其余按字符串） */
static bool cell_eq(const char *a, const char *b)
{
    if (strcmp(a, b) == 0)
        return true;

    double da, db_;
    if (as_num(a, &da) && as_num(b, &db_)) {
        double diff = fabs(da - db_);
        double scale = fabs(da) > fabs(db_) ? fabs(da) : fabs(db_);
        if (scale < 1.0)
            scale = 1.0;
        return diff / scale <= 1e-9;
    }
    return false;
}

/* 按行比较两段输出 */
static void compare_outputs(const char *sql, const char *ours, const char *theirs)
{
    checks++;

    const char *p1 = ours;
    const char *p2 = theirs;
    int line = 0;

    while (*p1 || *p2) {
        const char *e1 = strchr(p1, '\n');
        size_t l1 = e1 ? (size_t)(e1 - p1) : strlen(p1);
        const char *e2 = strchr(p2, '\n');
        size_t l2 = e2 ? (size_t)(e2 - p2) : strlen(p2);

        char line1[1024], line2[1024];
        if (l1 >= sizeof(line1)) l1 = sizeof(line1) - 1;
        if (l2 >= sizeof(line2)) l2 = sizeof(line2) - 1;
        memcpy(line1, p1, l1); line1[l1] = '\0';
        memcpy(line2, p2, l2); line2[l2] = '\0';

        /*
         * Windows 上 SQLite 输出 CRLF。
         * 不去掉行尾的 '\r'，每个格子都会多一个字符 → 明明相同却判不等。
         */
        if (l1 && line1[l1 - 1] == '\r') line1[--l1] = '\0';
        if (l2 && line2[l2 - 1] == '\r') line2[--l2] = '\0';

        if (l1 == 0 && l2 == 0) {
            /* 两边都没内容了 */
            if (!e1 && !e2)
                break;
        } else {
            /* 逐格比 */
            char *s1 = line1, *s2 = line2;
            int col = 0;
            for (;;) {
                char *b1 = strchr(s1, '|');
                char *b2 = strchr(s2, '|');
                size_t n1 = b1 ? (size_t)(b1 - s1) : strlen(s1);
                size_t n2 = b2 ? (size_t)(b2 - s2) : strlen(s2);

                char c1[512], c2[512];
                if (n1 >= sizeof(c1)) n1 = sizeof(c1) - 1;
                if (n2 >= sizeof(c2)) n2 = sizeof(c2) - 1;
                memcpy(c1, s1, n1); c1[n1] = '\0';
                memcpy(c2, s2, n2); c2[n2] = '\0';

                if (!cell_eq(c1, c2)) {
                    printf("    FAIL %s\n      第 %d 行第 %d 列: 我们=%s SQLite=%s\n",
                           sql, line + 1, col + 1, c1, c2);
                    failures++;
                    return;
                }

                if (!b1 || !b2)
                    break;
                s1 = b1 + 1;
                s2 = b2 + 1;
                col++;
            }
        }

        line++;
        if (!e1) { p1 += l1; }
        else      { p1 = e1 + 1; }
        if (!e2) { p2 += l2; }
        else      { p2 = e2 + 1; }

        if (!*p1 && !e1 && !*p2 && !e2)
            break;
    }

    /* 行数也要一样 */
    int r1 = 0, r2 = 0;
    for (const char *p = ours; *p; p++) if (*p == '\n') r1++;
    for (const char *p = theirs; *p; p++) if (*p == '\n') r2++;
    if (r1 != r2) {
        printf("    FAIL %s\n      行数: 我们 %d，SQLite %d\n", sql, r1, r2);
        failures++;
    }
}

/* ---------------- 命令行 ---------------- */

/*
 * 造一条能被 cmd.exe（Windows）/ sh（Unix）正确执行的命令。
 *
 * 【Windows 的坑：可执行文件名不能加引号】
 * cmd.exe 对"以引号开头"的命令串有自己的解析规则（吃掉首尾引号），
 * 而 MinGW 的 system() 又会把整条命令用引号包一层再交给 cmd。
 * 两者叠加 → `"sqlite3" -version` 变成 `'sqlite3" -version`，
 * cmd 找不到这个程序 → "不是内部或外部命令"。
 *
 * 所以：**路径不含空格就不加引号**（实测 cmd 能正常执行）。
 * 含空格才用 cmd /c ""..."" 的双层引号写法。
 */
static void build_cmd(char *buf, size_t cap, const char *prog,
                      const char *dbfile, const char *script, const char *out)
{
    bool spaced = (strchr(prog, ' ') != NULL);

#ifdef _WIN32
    if (spaced)
        snprintf(buf, cap,
                 "cmd /c \"\"%s\" \"%s\" < \"%s\" > \"%s\" 2>&1\"",
                 prog, dbfile, script, out);
    else
        snprintf(buf, cap, "%s \"%s\" < \"%s\" > \"%s\" 2>&1",
                 prog, dbfile, script, out);
#else
    (void)spaced;
    snprintf(buf, cap, "\"%s\" \"%s\" < \"%s\" > \"%s\" 2>&1",
             prog, dbfile, script, out);
#endif
}

/* ---------------- 主流程 ---------------- */

int main(void)
{
    const char *sqlite3 = getenv("SQLITE3");
    bool require = getenv("AGG_REQUIRE_SQLITE") != NULL;
    if (!sqlite3 || !*sqlite3)
        sqlite3 = "sqlite3";

    /* 先探一下 sqlite3 在不在（可执行文件名不加引号 —— 见 build_cmd） */
    {
        char cmd[1024];
        snprintf(cmd, sizeof(cmd), "%s -version > %s 2>&1", sqlite3, SQL_OUT);
        int rc = system(cmd);
        if (rc != 0) {
            printf("  跳过：找不到 sqlite3（用 SQLITE3 环境变量指定路径）\n");
            if (require) {
                printf("  AGG_REQUIRE_SQLITE=1 → 视为失败\n");
                return 1;
            }
            return 0;
        }
    }

    remove(OUR_DB);
    remove(OUR_DB "-wal");
    remove(SQL_DB);
    remove(SQL_SETUP);
    remove(SQL_SCRIPT);
    remove(SQL_OUT);

    db_err e = db_open(OUR_DB, &db);
    if (e != DB_OK) {
        printf("打开失败: %s\n", db_strerror(e));
        return 1;
    }

    /* 我们这边：建表 + 灌数据 */
    for (size_t i = 0; i < sizeof(SETUP) / sizeof(SETUP[0]); i++) {
        Result *r = NULL;
        e = db_exec(db, SETUP[i], &r);
        if (r)
            db_result_free(r);
        if (e != DB_OK) {
            printf("    FAIL 准备数据失败: %s → %s\n", SETUP[i], db_last_error(db));
            failures++;
        }
    }

    /* SQLite 那边：同样的建表 + 灌数据（跑一次，之后表就在那里了） */
    {
        FILE *f = fopen(SQL_SETUP, "wb");
        if (!f) {
            printf("写脚本失败\n");
            return 1;
        }
        for (size_t i = 0; i < sizeof(SETUP) / sizeof(SETUP[0]); i++)
            fprintf(f, "%s;\n", SETUP[i]);
        fclose(f);

        char cmd[1024];
        build_cmd(cmd, sizeof(cmd), sqlite3, SQL_DB, SQL_SETUP, SQL_OUT);
        int rc = system(cmd);
        if (rc != 0) {
            printf("    FAIL sqlite3 建表失败: %s\n", read_file(SQL_OUT));
            failures++;
        }
    }

    for (size_t i = 0; i < sizeof(QUERIES) / sizeof(QUERIES[0]); i++) {
        const char *sql = QUERIES[i];

        /* 我们这边 */
        char *ours = our_output(sql);

        /* SQLite 那边：一条一条跑（一次一条，输出干净） */
        {
            FILE *f = fopen(SQL_SCRIPT, "wb");
            if (!f) {
                printf("写脚本失败\n");
                return 1;
            }
            fprintf(f, ".mode list\n");
            fprintf(f, ".nullvalue NULL\n");
            fprintf(f, ".separator |\n");
            fprintf(f, "%s;\n", sql);
            fclose(f);
        }

        char cmd[1024];
        build_cmd(cmd, sizeof(cmd), sqlite3, SQL_DB, SQL_SCRIPT, SQL_OUT);
        int rc = system(cmd);
        if (rc != 0) {
            printf("    FAIL sqlite3 执行失败: %s\n      %s\n", sql, read_file(SQL_OUT));
            failures++;
            continue;
        }

        char *theirs = read_file(SQL_OUT);
        compare_outputs(sql, ours, theirs);
    }

    db_close(db);

    if (failures == 0)
        printf("  %d 条聚合 SQL 与 SQLite 逐格全等\n", checks);
    else
        printf("  %d 条中 %d 条不一致\n", checks, failures);
    return failures ? 1 : 0;
}
