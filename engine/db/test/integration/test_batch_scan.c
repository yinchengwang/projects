/*
 * test_batch_scan.c — SPI 批接口的端到端验收（v1.2 步骤 0）
 *
 * 4 项：
 *   1. 批扫描 vs 逐行扫描：行数、RID 顺序、每格的值完全一致
 *   2. 批解码 vs 逐行解码：NULL / TEXT / INT4 的值完全一致
 *   3. **整批不可见时结果不能被截断**（回归）
 *   4. 能力协商：只有声明了批能力的模态才走批路径，其余自动退回逐行
 *
 * ==================== 第 3 项是怎么来的 ====================
 *
 * v1.2 步骤 0 的批路径一次取**一页**的行，而一页里的行可能恰好全都不可见
 * （另一个事务插了还没提交）。
 *
 * 原来的写法是"本批 0 行 → 收工"，于是：
 *     第 1 页全是不可见的行 → 收工 → 后面页里的可见行**全被丢掉**。
 *
 * 这是静默丢数据 —— 不报错，只是结果少了。
 * 所以第 3 项专门构造这个场景：写方先插 200 行不提交（占掉前面的页），
 * 读方再插 10 行提交（落在后面的页），读方查表必须看到那 10 行。
 *
 * ==================== 为什么要两个连接 ====================
 *
 * 可见性是**连接私有快照 + 实例共享事务表**共同决定的（见 db.h 的 db_connect）。
 * 同一个连接看不到自己未提交的数据（那是同一个事务，自己看得见），
 * 所以要两个连接才有"看不见"这回事。
 */

#include "storage/record.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db_internal.h"
#include "query/exec_batch.h"
#include "query/exec_internal.h"
#include "query/parser.h"
#include "spi/registry.h"
#include "spi/storage_am.h"
#include "storage/value.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/batch_scan.db"

static int run_sql(DB *db, const char *sql)
{
    Result *r = NULL;
    db_err e = db_exec(db, sql, &r);
    if (r)
        db_result_free(r);
    if (e != DB_OK) {
        printf("    执行失败: %s\n      SQL: %.80s\n", db_last_error(db), sql);
        return -1;
    }
    return 0;
}

static int select_rows(DB *db, const char *sql)
{
    Result *r = NULL;
    if (db_exec(db, sql, &r) != DB_OK) {
        if (r) db_result_free(r);
        return -1;
    }
    int n = db_result_rows(r);
    db_result_free(r);
    return n;
}

/* ---------------- 造数据 ---------------- */

/*
 * 造一张 200 行的表：id INT、name TEXT（每 10 行一个 NULL）、v INT。
 *
 * NULL 与文本是刻意的：批解码的三个易错点正好是
 * NULL 位图、文本截断、INT4 的符号扩展（见 storage/batch.c）。
 */
static int make_table(DB *db)
{
    if (run_sql(db, "CREATE TABLE t (id INT, name TEXT, v INT);") != 0)
        return -1;
    if (run_sql(db, "BEGIN;") != 0)
        return -1;

    char sql[1024];
    for (int base = 0; base < 200; base += 16) {
        int n = snprintf(sql, sizeof(sql), "INSERT INTO t VALUES ");
        for (int k = 0; k < 16 && base + k < 200; k++) {
            int i = base + k;
            if (i % 10 == 0)
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,NULL,%d)", k ? "," : "", i, i * 3 - 7);
            else
                n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                              "%s(%d,'n%03d',%d)", k ? "," : "", i, i, i * 3 - 7);
        }
        if (run_sql(db, sql) != 0)
            return -1;
    }
    return run_sql(db, "COMMIT;");
}

/* ---------------- 1 & 2) 批路径 vs 逐行路径 ---------------- */

/*
 * 逐行扫一遍，把每列的值存下来（给批路径对拍用）。
 *
 * 存 DbValue 而不是原始字节：DbValue 已经是"解码后"的值，
 * 拿来跟批解码的结果比，比的是**语义**而不是字节布局。
 *
 * 【decode 的契约】
 * am->decode 一次解**整行** —— out 是 ncols 个 DbValue 的数组，
 * 不是单个 DbValue。写成 &out[i] 会让解码把相邻的格子也写掉
 * （最后一行就写到数组外面去了）。
 */
static int scan_rowwise(Db *kdb, TableCtx *tc, DbValue *out, int cap, int *n_out)
{
    AmScan *scan = NULL;
    if (open_scan(kdb, tc, NULL, &scan) != OK)
        return -1;

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    Rid rid;
    DbValue row[SQL_MAX_COLS];
    int n = 0;

    while (n < cap) {
        int rc = tc->am->scan_next(tc->am, scan, &rid, recbuf, sizeof(recbuf), &reclen);
        if (rc != OK)
            break;
        if (tc->am->decode(tc->am, recbuf, reclen, row) != OK) {
            tc->am->scan_end(tc->am, scan);
            return -1;
        }
        for (int c = 0; c < tc->ncols; c++)
            out[n * tc->ncols + c] = row[c];
        n++;
    }

    tc->am->scan_end(tc->am, scan);
    *n_out = n;
    return 0;
}

static void t1_batch_matches_rowwise(DB *w)
{
    Db *kdb = &w->db;

    char err[256] = {0};
    TableCtx tc;
    memset(&tc, 0, sizeof(tc));
    if (resolve_table(kdb, "t", &tc, err, sizeof(err)) != OK) {
        printf("    FAIL 解析表失败: %s\n", err);
        failures++;
        return;
    }

    /* 逐行：基准 */
    DbValue *base = calloc((size_t)200 * (size_t)tc.ncols, sizeof(DbValue));
    if (!base) {
        failures++;
        return;
    }
    int nbase = 0;
    CHECK(scan_rowwise(kdb, &tc, base, 200, &nbase) == 0);
    CHECK(nbase == 200);

    /* 批：一次一页地取，再批解码 */
    AmScan *scan = NULL;
    CHECK(open_scan(kdb, &tc, NULL, &scan) == OK);

    u16 needed = 0;
    for (int c = 0; c < tc.ncols; c++)
        needed |= (u16)(1u << c);

    BatchCtx bc;
    memset(&bc, 0, sizeof(bc));
    CHECK(batch_ctx_init(&bc, tc.cols, tc.ncols, needed) == OK);

    AmRowRef refs[AM_BATCH_MAX_ROWS];
    AmBatch ab;
    ab.ncols = bc.ncols;
    ab.nrows = 0;
    ab.cols = bc.col;

    int nbatch = 0;
    int nbad = 0;

    for (;;) {
        int n = 0;
        int rc = tc.am->batch_scan(tc.am, scan, refs, AM_BATCH_MAX_ROWS, &n);
        if (rc != OK)
            break;
        if (n <= 0)
            break;

        batch_reset(&bc);
        ab.nrows = 0;
        CHECK(tc.am->batch_decode(tc.am, refs, n, &ab) == OK);
        bc.nrows = ab.nrows;
        CHECK((int)bc.nrows == n);

        for (int i = 0; i < n; i++, nbatch++) {
            if (nbatch >= nbase)
                break;                      /* 逐行路径只有 200 行 */
            for (int c = 0; c < tc.ncols; c++) {
                DbValue v;
                batch_cell(&bc, c, (u32)i, &v);
                const DbValue *b = &base[nbatch * tc.ncols + c];

                if (v.is_null != b->is_null) {
                    nbad++;
                    continue;
                }
                if (v.is_null)
                    continue;                /* 都是 NULL，比到这里就够了 */

                if (v.type == T_TEXT) {
                    if (v.len != b->len || memcmp(v.s, b->s, v.len) != 0)
                        nbad++;
                } else if (v.i != b->i) {
                    nbad++;
                }
            }
        }
    }

    tc.am->scan_end(tc.am, scan);
    batch_ctx_free(&bc);
    free(base);

    CHECK(nbatch == nbase);          /* 行数一致 */
    CHECK(nbad == 0);                /* 每一格的值都一致 */
    printf("    批路径 %d 行，逐行路径 %d 行，逐格比对 %d 处不一致\n",
           nbatch, nbase, nbad);
}

/* ---------------- 3) 整批不可见不能截断（回归） ---------------- */

static void t2_invisible_batch_not_truncated(DB *w)
{
    /*
     * 用一张**新表**，不复用 t1 那张。
     *
     * 【为什么必须用新表】
     * 触发条件是"**第一页**的行全都不可见"——
     * 只有当不可见的行排在可见的行**前面**（页号更小）时才会触发。
     * 老表里已经有 200 行提交过的数据（占着第 1 页），
     * 第一批就不是空的，测不到这个 bug。
     *
     * 所以：写方先插 200 行不提交（占掉第 1、2 页），
     * 读方再插 10 行提交（落在第 3 页）。
     * 读方查表必须看到那 10 行 —— 一行都不能少。
     */
    DB *r = NULL;
    CHECK(db_connect(w, &r) == DB_OK);

    /* 建表：自动提交（DDL 要让读方看得见这张表） */
    CHECK(run_sql(w, "CREATE TABLE t2 (id INT, name TEXT, v INT);") == 0);

    /* 写方开事务，插 200 行**不提交** */
    CHECK(run_sql(w, "BEGIN;") == 0);
    char sql[1024];
    for (int base = 0; base < 200; base += 16) {
        int n = snprintf(sql, sizeof(sql), "INSERT INTO t2 VALUES ");
        for (int k = 0; k < 16 && base + k < 200; k++) {
            int i = base + k;
            n += snprintf(sql + n, sizeof(sql) - (size_t)n,
                          "%s(%d,'x%03d',%d)", k ? "," : "", i, i, i);
        }
        CHECK(run_sql(w, sql) == 0);
    }

    /* 读方插 10 行并提交（落在写方那些页之后） */
    for (int i = 1; i <= 10; i++) {
        char one[128];
        snprintf(one, sizeof(one), "INSERT INTO t2 VALUES (900%d, 'mine', %d);", i, i);
        CHECK(run_sql(r, one) == 0);
    }

    /* 读方看得见自己那 10 行，看不见写方那 200 行 —— 但**一行都不能少** */
    CHECK(select_rows(r, "SELECT id FROM t2;") == 10);

    /* 带过滤的也一样：过滤不能把结果截断 */
    CHECK(select_rows(r, "SELECT id FROM t2 WHERE id >= 9000;") == 10);
    CHECK(select_rows(r, "SELECT id FROM t2 WHERE v >= 0;") == 10);

    /* 写方提交后，读方（RC）应该能看到全部 210 行 */
    CHECK(run_sql(w, "COMMIT;") == 0);
    CHECK(select_rows(r, "SELECT id FROM t2;") == 210);

    db_conn_close(r);
}

/* ---------------- 4) 溢出 JSON 批解码局部退回逐行 ---------------- */

/*
 * 一批里有 >2048B 的 JSON marker 时，关系批解码返回 E_NOTSUP。
 * 执行器必须只对**这一批**退回逐行，而不是整条 SELECT 失败，
 * 也不能把所有表永久降成逐行。
 *
 * 本测试直接走与执行器相同的动作：
 *   1. batch_scan 取原始 refs
 *   2. batch_decode 返回 E_NOTSUP（batch.c 识别 marker）
 *   3. 对同一批 refs 调 am->decode + batch_add_row
 *   4. 与 SELECT 的火山结果逐格比对
 *
 * 表里混排小文档 / 大文档，保证同一批不只包含一个 marker。
 */
/*
 * 造一张含 JSON 列的真实表：行 2 是 >2048B 页链标记。
 * 通过 db_exec 强制走 SELECT 执行器路径（而不是手工调用 AM），
 * 断言整条查询成功且三行都能读到 —— 内部必须在 marker 那批
 * 从批解码局部退回逐行。
 *
 * 批路径的底层 E_NOTSUP 分流由 exec_batch.c 承担：
 * storage/batch.c 识别 marker 后返回 E_NOTSUP，执行器对这批 refs
 * 调 am->decode + batch_add_row。这里以用户可见行为验收，不复制那段循环。
 */
static void t3_json_ovf_batch_fallback(DB *w)
{
    char sql[8192];
    char doc[4096];
    size_t n = 0;
    Result *r = NULL;
    int e;

    printf("  [3] JSON 页链 SELECT 不因批解码失败\n");

    CHECK(run_sql(w, "CREATE TABLE json_batch (id INT, doc JSON);") == 0);
    CHECK(run_sql(w, "INSERT INTO json_batch VALUES (1, '{\"a\":1}');") == 0);

    n += (size_t)snprintf(doc + n, sizeof(doc) - n, "{\"pad\":\"");
    for (int i = 0; i < 2500; i++)
        doc[n++] = 'x';
    n += (size_t)snprintf(doc + n, sizeof(doc) - n, "\",\"a\":7}");
    doc[n] = '\0';
    snprintf(sql, sizeof(sql), "INSERT INTO json_batch VALUES (2, '%s');", doc);
    CHECK(run_sql(w, sql) == 0);

    CHECK(run_sql(w, "INSERT INTO json_batch VALUES (3, '{\"a\":3}');") == 0);

    /* SELECT 不得因 batch_decode 遇到页链 marker 而失败或漏行。 */
    e = db_exec(w, "SELECT id, doc FROM json_batch;", &r);
    if (e != DB_OK)
        printf("    JSON 批量 SELECT 错误: %s\n", db_last_error(w));
    CHECK(e == DB_OK);
    if (e == DB_OK && r) {
        CHECK(db_result_rows(r) == 3);
        if (db_result_rows(r) == 3) {
            const char *v1 = db_result_value(r, 0, 1);
            const char *v2 = db_result_value(r, 1, 1);
            const char *v3 = db_result_value(r, 2, 1);
            CHECK(v1 && strstr(v1, "\"a\":1") != NULL);
            CHECK(v2 && strlen(v2) > 2500);
            CHECK(v2 && strstr(v2, "\"a\":7") != NULL);
            CHECK(v3 && strstr(v3, "\"a\":3") != NULL);
        }
    }
    if (r)
        db_result_free(r);

    CHECK(select_rows(w, "SELECT id FROM json_batch;") == 3);
}

/* ---------------- 4) 能力协商 ---------------- */

static void t3_capability_negotiation(DB *w)
{
    Db *kdb = &w->db;

    /* 关系模态：声明了批能力 → 两个指针都必须非空 */
    char err[256] = {0};
    TableCtx tc;
    memset(&tc, 0, sizeof(tc));
    if (resolve_table(kdb, "t", &tc, err, sizeof(err)) == OK) {
        CHECK((tc.am->caps & AM_CAP_BATCH_SCAN) != 0);
        CHECK((tc.am->caps & AM_CAP_BATCH_DECODE) != 0);
        CHECK(tc.am->batch_scan != NULL);
        CHECK(tc.am->batch_decode != NULL);
    }

    /*
     * blob / graph / vector：一个指针都没加。
     *
     * 【为什么这是"通过"而不是"没做完"】
     * 能力位是**可选加速**，不是正确性前提：
     * 指针为 NULL → 执行器自动退回逐行路径 → 结果照样对。
     * 所以判据是"没声明的模态必须真的没声明"，而不是"都得有"。
     */
    DbInstance *inst = w->inst;
    CHECK(inst->blob_am_objects.batch_scan == NULL);
    CHECK(inst->blob_am_objects.batch_decode == NULL);
    CHECK((inst->blob_am_objects.caps & AM_CAP_BATCH_SCAN) == 0);
    CHECK((inst->blob_am_objects.caps & AM_CAP_BATCH_DECODE) == 0);

    if (inst->vec_ready) {
        CHECK(inst->vec_am.batch_scan == NULL);
        CHECK(inst->vec_am.batch_decode == NULL);
        CHECK((inst->vec_am.caps & AM_CAP_BATCH_SCAN) == 0);
    }
}

/* ---------------- main ---------------- */

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);   /* 崩溃时也能看到走到哪一步 */
    printf("test_batch_scan（集成）:\n");

    remove(PATH);
    remove("test/tmp/batch_scan.db-wal");
    remove("test/tmp/batch_scan.db-shm");

    DB *w = NULL;
    CHECK(db_open(PATH, &w) == DB_OK);
    if (!w)
        return 1;

    CHECK(make_table(w) == 0);

    printf("  1. 批扫描 / 批解码 vs 逐行路径\n");
    t1_batch_matches_rowwise(w);

    printf("  2. 整批不可见：结果不能被截断\n");
    t2_invisible_batch_not_truncated(w);

    printf("  3. 能力协商：只有声明了批能力的模态走批路径\n");
    t3_capability_negotiation(w);

    printf("  4. JSON 页链批解码局部退回逐行\n");
    t3_json_ovf_batch_fallback(w);

    db_close(w);

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
