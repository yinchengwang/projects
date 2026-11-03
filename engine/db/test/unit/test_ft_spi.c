/*
 * test_ft_spi.c —— 全文检索的 SPI 接线验收（v1.3 步骤 4）
 *
 * 5 项：
 *   1. ft_docs 的**列定义**与**能力位**：四列标量、只读
 *   2. ft_docs 的**行内容**：doc_id 集合与 (doc_len, nterms) 全对
 *   3. 走**执行器**查 ft_docs（SELECT ... FROM ft_docs）：能拿到行
 *   4. db_ft_search 的结果与内核 ft_index_search **逐位一致**
 *   5. db_ft_delete 之后：检索不再返回它，ft_docs 里 sealed = 1
 *
 * 【口径：删除后的语义】
 *
 * ft 的删除是"打 tombstone"，**不是**"把行删掉"：
 *   - 文档仍然在段里（posting 还在），所以 ft_docs 里**仍然有这一行**；
 *   - 它的 sealed 列 = 1（这是 tombstone 状态的可观测形式）；
 *   - 检索不再返回它；
 *   - 直到一次合并把 tombstone 的文档**物理丢弃**，这一行才消失。
 *
 * 这与 blob 的"对象不可变"、vector 的"行有版本"都不同 ——
 * ft 的删除语义是 Lucene 式的：先打标记，合并时再扔。
 *
 * 【doc_len / nterms 怎么算】
 *
 *   doc_len = 该文档的**词数**（分词器切出来的 token 个数）
 *   nterms = 该文档命中的**不同词数**
 *             （= 倒排链里该 doc_id 的 posting 条数：
 *              同一个词在同一篇文档里只出现一条 posting）
 *
 * 例："beta beta gamma" → 3 个词、2 个不同的词 → doc_len=3, nterms=2。
 *
 * 【第 4 项为什么要逐位比】
 *
 * db_ft_search 是"公开 API 的包装"，ft_index_search 是"内核的实现"。
 * 两边只要有一处口径不同（idf 公式、累加顺序、排序的平局规则），
 * 公开 API 就会给出**看起来合理但与基准不一致**的分数。
 * 所以这里不比较"排序对不对"，而比较**每个字节**：分数用 == 精确比。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"
#include "db_internal.h"
#include "modalities/ft/ft_index.h"
#include "spi/registry.h"
#include "spi/storage_am.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/ft_spi_test.db"

/* ---------------- 语料 ---------------- */

/*
 * 四篇文档，故意让词有重叠：
 *
 *   1: alpha beta gamma     → 3 词 / 3 个不同词
 *   2: alpha delta          → 2 词 / 2 个不同词
 *   3: beta beta gamma      → 3 词 / 2 个不同词（"beta" 出现两次）
 *   4: epsilon              → 1 词 / 1 个不同词
 */
static const struct {
    uint32_t    doc_id;
    const char *text;
    uint32_t    doc_len;
    uint32_t    nterms;
} DOCS[4] = {
    { 1, "alpha beta gamma", 3, 3 },
    { 2, "alpha delta",      2, 2 },
    { 3, "beta beta gamma",  3, 2 },
    { 4, "epsilon",          1, 1 }
};

/* ---------------- 1) 列定义与能力位 ---------------- */

static void t1_schema(DB *db)
{
    StorageAM *am = spi_registry_find(db->inst->reg, "ft_docs");
    const ColDesc *cols = NULL;
    int ncols = 0;

    CHECK(am != NULL);
    if (!am) {
        return;
    }
    CHECK(strcmp(am->name, "ft_docs") == 0);

    /* 只读：没有写能力位 */
    CHECK((am->caps & AM_CAP_SCAN) != 0);
    CHECK((am->caps & AM_CAP_PUSHDOWN) != 0);
    CHECK((am->caps & AM_CAP_INSERT) == 0);
    CHECK((am->caps & AM_CAP_UPDATE) == 0);
    CHECK((am->caps & AM_CAP_REMOVE) == 0);
    CHECK((am->caps & AM_CAP_VACUUM) == 0);

    CHECK(am->columns(am, &cols, &ncols) == OK);
    CHECK(ncols == 4);
    if (cols && ncols == 4) {
        CHECK(strcmp(cols[0].name, "doc_id") == 0);
        CHECK(strcmp(cols[1].name, "doc_len") == 0);
        CHECK(strcmp(cols[2].name, "nterms") == 0);
        CHECK(strcmp(cols[3].name, "sealed") == 0);
    }
}

/* ---------------- 2) 行内容（直接走 SPI 扫描） ---------------- */

/*
 * 直接拿 AM 扫一遍 —— 这是"模态自己产出什么"的原始视图，
 * 不经过执行器的任何加工。
 */
static void t2_spi_rows(DB *db)
{
    StorageAM *am = spi_registry_find(db->inst->reg, "ft_docs");
    AmScan *s = NULL;
    u8 buf[64];
    u16 len = 0;
    Rid rid;
    int i = 0;

    CHECK(am != NULL);
    if (!am) {
        return;
    }
    CHECK(am->scan_begin(am, &s) == OK);
    if (!s) {
        return;
    }

    for (;;) {
        DbValue vals[4];
        int rc = am->scan_next(am, s, &rid, buf, sizeof(buf), &len);

        if (rc == E_EOF) {
            break;
        }
        CHECK(rc == OK);
        if (rc != OK) {
            break;
        }
        CHECK(am->decode(am, buf, len, vals) == OK);

        if (i < 4) {
            CHECK(vals[0].i == (i64)DOCS[i].doc_id);
            CHECK(vals[1].i == (i64)DOCS[i].doc_len);
            CHECK(vals[2].i == (i64)DOCS[i].nterms);
            CHECK(vals[3].i == 0);       /* 还没删，sealed = 0 */
        }
        i++;
    }
    CHECK(i == 4);   /* 四篇文档四行 */

    CHECK(am->scan_end(am, s) == OK);
}

/* ---------------- 3) 走执行器 ---------------- */

static void t3_sql(DB *db)
{
    Result *r = NULL;

    CHECK(db_exec(db, "SELECT doc_id, doc_len, nterms, sealed FROM ft_docs;", &r) == DB_OK);
    if (!r) {
        return;
    }
    CHECK(db_result_cols(r) == 4);
    CHECK(db_result_rows(r) == 4);
    if (db_result_rows(r) == 4 && db_result_cols(r) == 4) {
        for (int i = 0; i < 4; i++) {
            char buf[32];

            snprintf(buf, sizeof(buf), "%u", DOCS[i].doc_id);
            CHECK(strcmp(db_result_value(r, i, 0), buf) == 0);
            snprintf(buf, sizeof(buf), "%u", DOCS[i].doc_len);
            CHECK(strcmp(db_result_value(r, i, 1), buf) == 0);
            snprintf(buf, sizeof(buf), "%u", DOCS[i].nterms);
            CHECK(strcmp(db_result_value(r, i, 2), buf) == 0);
            CHECK(strcmp(db_result_value(r, i, 3), "false") == 0);
        }
    }
    db_result_free(r);

    /* 谓词下推：doc_id 等值 */
    r = NULL;
    CHECK(db_exec(db, "SELECT doc_id FROM ft_docs WHERE doc_id = 2;", &r) == DB_OK);
    if (r) {
        CHECK(db_result_rows(r) == 1);
        if (db_result_rows(r) == 1) {
            CHECK(strcmp(db_result_value(r, 0, 0), "2") == 0);
        }
        db_result_free(r);
    }
}

/* ---------------- 4) 公开 API 与内核逐位一致 ---------------- */

static void t4_search(DB *db)
{
    db_ft_hit hits[16];
    uint32_t n = 0;
    FtHit ihits[16];
    uint32_t m = 0;

    CHECK(db_ft_search(db, "beta", 16, hits, 16, &n) == DB_OK);
    /* "beta" 命中 doc 1 与 doc 3 */
    CHECK(n == 2);

    /* 与内核直调逐位一致 */
    CHECK(ft_index_search(db->inst->ft, "beta", 4, 16, 16, ihits, &m) == OK);
    CHECK(m == n);
    for (uint32_t i = 0; i < n && i < m; i++) {
        CHECK(hits[i].doc_id == ihits[i].doc_id);
        CHECK(hits[i].score == ihits[i].score);   /* 分数逐位相同 */
    }

    /* 容量不够 → 明确报错，不静默截断 */
    {
        uint32_t n2 = 0;

        CHECK(db_ft_search(db, "beta", 16, hits, 1, &n2) == DB_E_NOSPACE);
        CHECK(n2 == 0);
    }

    /* 查不到 → 0 条，不是错误 */
    {
        uint32_t n3 = 0;

        CHECK(db_ft_search(db, "nosuchword", 16, hits, 16, &n3) == DB_OK);
        CHECK(n3 == 0);
    }
}

/* ---------------- 5) 删除：sealed = 1 + 检索不再返回 ---------------- */

static void t5_delete(DB *db)
{
    StorageAM *am = spi_registry_find(db->inst->reg, "ft_docs");
    AmScan *s = NULL;
    u8 buf[64];
    u16 len = 0;
    Rid rid;
    int sealed_seen = 0;
    int rows = 0;

    CHECK(db_ft_delete(db, 2) == DB_OK);

    /* 删一个不存在的文档是合法的空操作 */
    CHECK(db_ft_delete(db, 999) == DB_OK);

    /* 检索不再返回它 */
    {
        db_ft_hit hits[16];
        uint32_t n = 0;

        CHECK(db_ft_search(db, "delta", 16, hits, 16, &n) == DB_OK);
        CHECK(n == 0);
    }

    /*
     * ft_docs 里这一行**还在**（posting 还在段里），
     * 但 sealed = 1 —— 这就是 tombstone 状态的可观测形式。
     */
    CHECK(am != NULL);
    if (!am) {
        return;
    }
    CHECK(am->scan_begin(am, &s) == OK);
    if (!s) {
        return;
    }
    for (;;) {
        DbValue vals[4];
        int rc = am->scan_next(am, s, &rid, buf, sizeof(buf), &len);

        if (rc == E_EOF) {
            break;
        }
        CHECK(rc == OK);
        if (rc != OK) {
            break;
        }
        CHECK(am->decode(am, buf, len, vals) == OK);
        rows++;
        if (vals[0].i == 2) {
            CHECK(vals[3].i == 1);   /* sealed = 1 */
            sealed_seen++;
        } else {
            CHECK(vals[3].i == 0);
        }
    }
    CHECK(rows == 4);            /* 被删的文档仍然占一行 */
    CHECK(sealed_seen == 1);     /* 而且恰好一行 sealed = 1 */
    CHECK(am->scan_end(am, s) == OK);
}

int main(void)
{
    DB *db = NULL;

    printf("test_ft_spi（SPI 接线）:\n");

    remove(PATH);
    remove("test/tmp/ft_spi_test.db-wal");
    remove("test/tmp/ft_spi_test.db-clog");

    CHECK(db_open(PATH, &db) == DB_OK);
    if (!db) {
        printf("    FAIL 打开数据库失败\n");
        return 1;
    }

    /* 这个库一打开就有全文索引（打开时建的是**空**索引） */
    CHECK(db_ft_exists(db) == 1);
    CHECK(db_ft_ndocs(db) == 0);
    CHECK(db_ft_nsegs(db) == 0);

    for (int i = 0; i < 4; i++) {
        uint32_t len = (uint32_t)strlen(DOCS[i].text);

        CHECK(db_ft_add(db, DOCS[i].doc_id, DOCS[i].text, len) == DB_OK);
    }
    CHECK(db_ft_ndocs(db) == 4);
    CHECK(db_ft_nsegs(db) == 4);   /* 一次 add = 一个段 */

    t1_schema(db);
    t2_spi_rows(db);
    t3_sql(db);
    t4_search(db);
    t5_delete(db);

    db_close(db);

    if (failures) {
        printf("  %d 项失败\n", failures);
        return 1;
    }
    printf("  全部通过\n");
    return 0;
}
