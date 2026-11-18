/*
 * test_catalog_index_corrupt.c —— 损坏的索引必须让开库失败，不能静默丢
 *
 * ==================== 这个测试防的是什么 ====================
 *
 * `catalog_load.c` 的 `load_indexes()` 原来是：
 *
 *     if (btree_open(...) != OK) { freelist_destroy(fl); continue; }
 *
 * 后果是**索引静默消失**：catalog 里明明登记着这条索引，它的根页却打不开，
 * 于是加载时被跳过 —— 库照样能打开、查询照样能跑，只是那条索引不见了。
 *
 * 这件事有多坏，取决于谁在看：
 *   - 查询侧：退化成全表扫描，**慢，但不报错** —— 没人会发现
 *   - 写入侧：还在按 catalog 里记着的索引去维护，索引与数据可能分叉
 *   - 用户侧：一把"好好的"库，性能突然掉了 10 倍，查不出原因
 *
 * 所以这属于"宁可打不开，也不能假装打开"的一类。
 * 现在 `btree_open` 失败会 `return E_CORRUPT`，冒泡到 `catalog_open`
 * → `db_open` 失败，调用方拿到明确的 `DB_E_CORRUPT`。
 *
 * ==================== 怎么造这个坏库 ====================
 *
 * 不用破坏文件字节（那种测试对布局变化很脆）。直接走内部 API 写一条
 * `sys_index` 记录，把 `root_page` 填成 `PAGE_ID_NIL`（0）——
 * `btree_open` 对这个值返回 `E_INVAL`，正好模拟"记录在、索引结构不可用"。
 *
 * 用内部 API 的理由：这正是 `test_catalog.c` 已经在做的事（include
 * `catalog/catalog_priv.h`）。公共 API 造不出损坏态。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

#include "catalog/catalog.h"
#include "catalog/catalog_priv.h"

/* 内部写入口（catalog_write.c） */
int write_index_row(Catalog *cat, u32 xid, const IndexMeta *m);
int write_indexcol_row(Catalog *cat, u32 xid, u32 index_id, u32 seq, u32 col_no);

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/catalog_index_corrupt.db"

static void rm_db(const char *p)
{
    char aux[256];
    remove(p);
    snprintf(aux, sizeof(aux), "%s-wal", p);   remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", p);  remove(aux);
    snprintf(aux, sizeof(aux), "%s-shm", p);   remove(aux);
}

/* 造一个"有表、sys_index 里登记了索引、但根页无效"的库 */
static int make_corrupt_db(void)
{
    Pager *pg = NULL;
    Catalog *cat = NULL;

    rm_db(PATH);
    if (pager_open(PATH, 16, &pg) != OK)
        return -1;
    if (catalog_open(pg, NULL, NULL, &cat) != OK) {
        pager_close(pg);
        return -1;
    }

    ColDesc cols[2] = {
        { "id", T_INT4, false },
        { "v",  T_INT4, true  },
    };
    u32 tid = 0;
    if (catalog_create_table(cat, 0, "t", cols, 2, &tid) != OK) {
        catalog_close(cat);
        pager_close(pg);
        return -1;
    }

    IndexMeta m;
    memset(&m, 0, sizeof(m));
    m.index_id = 1;
    snprintf(m.name, sizeof(m.name), "bad_ix");
    m.table_id = tid;
    /*
     * ← 关键：root_page = PAGE_ID_NIL（0xFFFFFFFF）。
     * btree_open 对这个值直接返回 E_INVAL。
     *
     * ⚠️ 不能填 0：0 是 PAGE_ID_META（meta 页），btree_open 会把 meta 页
     * 当成一个 B+tree 根页打开 —— 恰好能成功，于是这条用例什么都测不到。
     * 我第一次写的就是 0，结果是"坏库也能正常打开"。
     */
    m.root_page = PAGE_ID_NIL;
    m.col_no = 0;
    m.is_unique = 0;
    if (write_index_row(cat, 0, &m) != OK ||
        write_indexcol_row(cat, 0, m.index_id, 0, 0) != OK) {
        catalog_close(cat);
        pager_close(pg);
        return -1;
    }

    catalog_close(cat);
    pager_close(pg);
    return 0;
}

int main(void)
{
    printf("test_catalog_index_corrupt:\n");

    /*
     * ---- (1) 对照：正常库里能建索引、能重开 ----
     *
     * 先确认"能正常打开"这条基线本身成立 —— 否则下面那条
     * "打不开"可能只是因为造库方式本身有问题。
     */
    {
        const char *p = "test/tmp/catalog_index_ok.db";
        rm_db(p);

        DB *db = NULL;
        Result *r = NULL;
        CHECK(db_open(p, &db) == DB_OK);
        CHECK(db_exec(db, "CREATE TABLE t (id INT, v INT);", &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }
        CHECK(db_exec(db, "INSERT INTO t VALUES (1, 10);", &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }
        CHECK(db_exec(db, "CREATE INDEX ix ON t (id);", &r) == DB_OK);
        if (r) { db_result_free(r); r = NULL; }
        db_close(db);

        db = NULL;
        CHECK(db_open(p, &db) == DB_OK);          /* 重开后索引仍在，能打开 */
        if (db) {
            /* 索引确实在用：等值查询应走索引路径，结果正确 */
            CHECK(db_exec(db, "SELECT v FROM t WHERE id = 1;", &r) == DB_OK);
            CHECK(db_result_rows(r) == 1);
            CHECK(strcmp(db_result_value(r, 0, 0), "10") == 0);
            db_result_free(r);
            db_close(db);
        }
        rm_db(p);
        printf("  [1] 对照：正常库带索引可重开、查询正确\n");
    }

    /*
     * ---- (2) 坏库：开库必须明确失败，而不是静默丢索引 ----
     *
     * 修复前的行为：db_open 返回 DB_OK，db_result 一切正常，
     *              只是 te->nidx == 0（索引没了）—— 断言看不出任何异常。
     * 修复后：db_open 返回 DB_E_CORRUPT。
     */
    {
        CHECK(make_corrupt_db() == 0);

        DB *db = NULL;
        db_err e = db_open(PATH, &db);

        CHECK(e == DB_E_CORRUPT);
        if (e != DB_E_CORRUPT)
            printf("    → 实际返回 %d (%s)，期望 DB_E_CORRUPT\n",
                   (int)e, db_strerror(e));
        CHECK(db == NULL);
        if (db) {
            printf("    → 开库失败但 db 非 NULL：调用方会误以为可用\n");
            db_close(db);
        }
        printf("  [2] 坏索引库开库被拒（DB_E_CORRUPT）\n");
    }

    /*
     * ---- (3) 直接查 catalog 层：修好后它也不该"假装成功" ----
     *
     * 绕过 db_open 直接调 catalog_open —— 确认错误码确实是从
     * load_indexes 冒泡上来的，而不是被 db_open 那一层自己编的。
     */
    {
        Pager *pg = NULL;
        CHECK(pager_open(PATH, 16, &pg) == OK);
        if (pg) {
            Catalog *cat = NULL;
            CHECK(catalog_open(pg, NULL, NULL, &cat) == E_CORRUPT);
            CHECK(cat == NULL);
            if (cat)
                catalog_close(cat);
            pager_close(pg);
        }
        printf("  [3] catalog_open 同一错误码冒泡\n");
    }

    rm_db(PATH);

    if (failures == 0) {
        printf("  3 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
