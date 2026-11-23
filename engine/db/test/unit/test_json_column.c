/*
 * test_json_column.c —— JSON 列的端到端读写（v1.6 C5.3b）
 *
 * 【这个测试在钉什么】
 * 前面 C5.3 只做了「值层怎么表示 JSONB」，C5.5/C5.7 只做了「类型名 + 两个函数」。
 * 但**写入路径一直是假的**：往 JSON 列 INSERT 一个 JSON 文本，
 * 会走 T_TEXT 的老路把原始文本字节直接塞进变长区；读回来时按 JSONB 解析，
 * 必然报 E_CORRUPT。
 *
 * 探针实测（C5.3b 之前）：
 *     CREATE TABLE t (id INT, doc JSON)              → OK
 *     INSERT INTO t VALUES (1, '{"a":5}')            → OK（假的成功）
 *     SELECT id FROM t                               → OK（1）
 *     SELECT doc FROM t                              → E_INVAL
 *     SELECT json_extract(doc, '$.a') FROM t         → E_CORRUPT
 *
 * 所以本测试要钉住七件事：
 *   1. INSERT 的**文本**被解释成 JSON 文本，存进去的必须是 JSONB 字节
 *   2. SELECT 一个 JSON 列，拿到的是合法 JSON 文本（不是二进制、不是报错）
 *   3. `json_extract` 能在 SQL 里取到路径标量
 *   4. `json_match` 能在 WHERE 里过滤
 *   5. UPDATE 使用相同的 JSON 文本编码入口
 *   6. SQL NULL 与 JSON 文本 null 保持区别
 *   6. 无效 JSON、非字符串输入和 >2048 字节被拒绝且不留下行
 *
 * 页链、tuple 所有权、abort/WAL/VACUUM 生命周期属于 C5.3b-b，不在本测试范围。
 *
 * 【第 5/6/7 项为什么必须一起测】
 * 只测"能存能读"会漏掉"到底存哪儿了"：
 *   - 全内联 ⇒ 大文档写不进 PAGE_SIZE，INSERT 直接失败；
 *   - 全外置 ⇒ 每行都吃一页，第 7 项会红。
 * 两者必须同时成立，才说明分流阈值真的生效了。
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

#define PATH "test/tmp/json_column.db"

static DB *g_db;

static void exec_ok(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(g_db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 执行失败: %s → %s (%s)\n", sql, db_strerror(e),
               db_last_error(g_db));
        failures++;
    }
    if (r)
        db_result_free(r);
}

/* 单行单列的文本结果；失败返回 NULL */
static const char *one(const char *sql, char *buf, size_t cap)
{
    Result *r = NULL;
    db_err e = db_exec(g_db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 查询失败: %s → %s (%s)\n", sql, db_strerror(e),
               db_last_error(g_db));
        failures++;
        if (r)
            db_result_free(r);
        return NULL;
    }
    const char *v = NULL;
    if (db_result_rows(r) == 1 && db_result_cols(r) == 1) {
        const char *s = db_result_value(r, 0, 0);
        if (s) {
            snprintf(buf, cap, "%s", s);
            v = buf;
        }
    } else {
        printf("    FAIL 形状不对: %s rows=%d cols=%d\n", sql,
               db_result_rows(r), db_result_cols(r));
        failures++;
    }
    db_result_free(r);
    return v;
}

static int count_rows(const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(g_db, sql, &r);
    if (e != DB_OK) {
        printf("    FAIL 查询失败: %s → %s (%s)\n", sql, db_strerror(e),
               db_last_error(g_db));
        failures++;
        if (r)
            db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

/*
 * 造一个 JSON 文本（用 pad 字段撑大），用于验证编码后超 2048B 被拒绝。
 */
static void make_big_doc(int pad_chars, char *out, size_t cap)
{
    size_t n = 0;
    n += (size_t)snprintf(out + n, cap - n, "{\"pad\":\"");
    for (int i = 0; i < pad_chars && n + 1 < cap; i++)
        out[n++] = 'x';
    snprintf(out + n, cap - n, "\",\"a\":7}");
}

int main(void)
{
    char buf[16384];

    printf("test_json_column:\n");

    remove(PATH);
    remove(PATH "-wal");
    remove(PATH "-clog");

    db_err e = db_open(PATH, &g_db);
    if (e != DB_OK) {
        printf("    FAIL 打不开库: %s\n", db_strerror(e));
        return 1;
    }

    exec_ok("CREATE TABLE t (id INT, doc JSON)");

    /* JSON 列拒绝非字符串标量；错误发生在任何 heap 写入之前。 */
    {
        Result *r = NULL;
        e = db_exec(g_db, "INSERT INTO t (id, doc) VALUES (90, 7)", &r);
        CHECK(e != DB_OK);
        if (r)
            db_result_free(r);
        CHECK(count_rows("SELECT id FROM t") == 0);
    }

    /* ---------------- 1. 小文档：文本 → JSONB 落盘 ---------------- */
    exec_ok("INSERT INTO t (id, doc) VALUES (1, '{\"a\":5}')");
    exec_ok("INSERT INTO t (id, doc) VALUES (2, '{\"a\":\"hi\",\"b\":true}')");
    exec_ok("INSERT INTO t (id, doc) VALUES (4, NULL)");
    exec_ok("INSERT INTO t (id, doc) VALUES (5, 'null')");

    /* ---------------- 2. SELECT 一个 JSON 列 → 合法 JSON 文本 ---------------- */
    CHECK(one("SELECT doc FROM t WHERE id = 1", buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "{\"a\":5}") == 0);

    /*
     * 键序按 JSONB 的排序输出（长度优先，再字典序）。
     * "a"(1) 在 "b"(1) 前 —— 都是 1 字节，按字典序。
     */
    CHECK(one("SELECT doc FROM t WHERE id = 2", buf, sizeof(buf)) != NULL);
    CHECK(strstr(buf, "\"a\":\"hi\"") != NULL);
    CHECK(strstr(buf, "\"b\":true") != NULL);

    /* SQL NULL 与 JSON 文本 null 必须区分。 */
    CHECK(one("SELECT doc FROM t WHERE id = 4", buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "NULL") == 0);
    CHECK(one("SELECT doc FROM t WHERE id = 5", buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "null") == 0);

    /* ---------------- 3. json_extract 在 SQL 里可用 ---------------- */
    CHECK(one("SELECT json_extract(doc, '$.a') FROM t WHERE id = 1",
              buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "5") == 0);

    CHECK(one("SELECT json_extract(doc, '$.a') FROM t WHERE id = 2",
              buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "hi") == 0);

    /* 路径不存在 → NULL（不是报错） */
    CHECK(one("SELECT json_extract(doc, '$.nope') FROM t WHERE id = 1",
              buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "NULL") == 0);

    /* ---------------- 4. json_match 在 WHERE 里可用 ---------------- */
    CHECK(count_rows("SELECT id FROM t WHERE json_match(doc, '$.a = 5')") == 1);
    CHECK(count_rows("SELECT id FROM t WHERE json_match(doc, '$.a = 6')") == 0);

    /*
     * 整表上调用（**不带 WHERE**）：表里既有 SQL NULL 行，也有 JSON 文本 null 行。
     *
     * 【为什么这一条必须存在】
     * 标量函数遇到 NULL 实参时必须返回 NULL（SQL 三值逻辑），
     * 不能报 E_INVAL —— 否则"表里有一行没填文档"就让整条查询失败。
     * 之前的实现正是这样：只要扫到 NULL 行就整条报错。
     */
    CHECK(count_rows("SELECT json_extract(doc, '$.a') FROM t") == 4);
    CHECK(count_rows("SELECT json_match(doc, '$.a = 5') FROM t") == 4);

    /* ---------------- 5. UPDATE 同样按 JSON 文本编码 ---------------- */
    exec_ok("UPDATE t SET doc = '{\"a\":9}' WHERE id = 1");
    CHECK(one("SELECT doc FROM t WHERE id = 1", buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "{\"a\":9}") == 0);
    CHECK(one("SELECT json_extract(doc, '$.a') FROM t WHERE id = 1",
              buf, sizeof(buf)) != NULL);
    CHECK(strcmp(buf, "9") == 0);

    /* ---------------- 6. 256–2048B：跨过 DbValue 内联阈值仍要完整往返 ---------------- */
    /*
     * 【这一组钉的是 v1.6 C5.3 的三层表示】
     *   ≤255B       → DbValue.s[] 内联
     *   256–2048B   → 记录里内联，运行时用**指向缓冲的外部值**
     *   >2048B      → C5.3b-b 页链（本步拒绝）
     *
     * 中间那层最容易写成"直接 memcpy 进 s[256]"。DbValue.s 就是 256 字节，
     * 多拷一个字节就踩到后面的 external 字段 —— 结果不是报错，
     * 而是把内联值变成"外部值 + 垃圾 ext_fn"，读的时候报 E_CORRUPT。
     * 所以这里必须断言 257 字节的 JSONB 能被完整存进去再读出来。
     */
    {
        char doc[4096];
        char sql[8192];

        /* pad=230 ⇒ 文本 246B，JSONB 257B，正好越过 255B 内联阈值。 */
        make_big_doc(230, doc, sizeof(doc));
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t (id, doc) VALUES (60, '%s')", doc);
        exec_ok(sql);

        CHECK(one("SELECT doc FROM t WHERE id = 60", buf, sizeof(buf)) != NULL);
        CHECK(strlen(buf) > 240);
        CHECK(strstr(buf, "\"a\":7") != NULL);
        CHECK(one("SELECT json_extract(doc, '$.a') FROM t WHERE id = 60",
                  buf, sizeof(buf)) != NULL);
        CHECK(strcmp(buf, "7") == 0);

        /* pad=1200 ⇒ 约 1216B：仍在阈值内，也必须完整 */
        make_big_doc(1200, doc, sizeof(doc));
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t (id, doc) VALUES (61, '%s')", doc);
        exec_ok(sql);
        CHECK(one("SELECT doc FROM t WHERE id = 61", buf, sizeof(buf)) != NULL);
        CHECK(strlen(buf) > 1200);
        CHECK(strstr(buf, "\"a\":7") != NULL);

        /* UPDATE 走同一条编码路径，同样不能踩阈值 */
        snprintf(sql, sizeof(sql),
                 "UPDATE t SET doc = '%s' WHERE id = 60", doc);
        exec_ok(sql);
        CHECK(one("SELECT doc FROM t WHERE id = 60", buf, sizeof(buf)) != NULL);
        CHECK(strlen(buf) > 1200);
    }

    /* ---------------- 7. 无效 JSON 与编码后超限都拒绝且不留下行 ---------------- */
    {
        char huge[4096];
        char sql[8192];
        Result *r = NULL;

        e = db_exec(g_db,
                    "INSERT INTO t (id, doc) VALUES (91, '{\"a\":}')", &r);
        CHECK(e != DB_OK);
        if (r) { db_result_free(r); r = NULL; }
        CHECK(count_rows("SELECT id FROM t WHERE id = 91") == 0);

        make_big_doc(2500, huge, sizeof(huge));
        snprintf(sql, sizeof(sql),
                 "INSERT INTO t (id, doc) VALUES (92, '%s')", huge);
        e = db_exec(g_db, sql, &r);
        CHECK(e != DB_OK);
        if (r) db_result_free(r);
        CHECK(count_rows("SELECT id FROM t WHERE id = 92") == 0);
    }

    /* ---------------- 7. 页链外置不属于 C5.3b-a ---------------- */
    /*
     * C5.3b-a 约定编码后 >2048B 拒绝；大文档页链与 tuple 生命周期
     * 留待 C5.3b-b 完成所有权/WAL/abort/VACUUM 设计后测试。
     */
    db_close(g_db);
    g_db = NULL;
    remove(PATH);
    remove(PATH "-wal");
    remove(PATH "-clog");

    if (failures == 0) {
        printf("  C5.3b-a JSON 列端到端验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}

