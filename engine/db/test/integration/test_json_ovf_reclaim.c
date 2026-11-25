/*
 * test_json_ovf_reclaim.c —— JSON 溢出页链的 VACUUM 回收与启动孤儿回收（C5.3b-b Task 5/6）
 *
 * ==================== 这个测试盯什么 ====================
 *
 * 前面几步已经证明"链能写、能读、free 幂等"。但**没有任何人回收它**时，
 * 一条 6000 字节的 JSON 改十次就留下十条链，文件只增不减。本测试证明：
 *
 *   ① VACUUM 物理删掉带大 JSON 的行之后，链页真的回到页分配器
 *      （"空闲页计数"增加 —— 这是唯一能区分"回收跑了"和"回收没跑"的观测量）；
 *   ② 启动扫描能回收**没有任何 tuple 引用**的链（INSERT 窗口 / abort 留下的）；
 *   ③ 被活 tuple 引用的链**一个页都不动**（回收器最危险的错误方向）；
 *   ④ 回收之后库还能正常读写（没把页发给两个主人）。
 *
 * ==================== 为什么要读内部 API ====================
 *
 * "空闲页计数"公开 API 不暴露（与 test_ft_reclaim_boot.c 读池大小、
 * test_catalog.c 读 catalog_priv.h 是同一个理由）。
 * pager_free_count 是 storage/pager.h 的内部观测接口，本测试直接用。
 *
 * ==================== 为什么手工造孤儿链 ====================
 *
 * 真实产生孤儿需要"写完链、tuple 没写成"那个窗口，公开 API 做不到
 * （val_json_to_column 是原子的一步）。所以这里用 json_ovf_write 直接
 * 造一条没有任何 tuple 引用的链 —— 那正是回收器要处理的目标形态。
 *
 * ==================== 为什么同一进程里反复 db_open ====================
 *
 * 启动回收挂在 db_open 里，所以必须"关库 → 重开"才能验证它跑过。
 * 而这一条恰好暴露了一个既有缺陷：val_set_json_to_column 原来
 * "已注册就拒"，导致第二次开库后注册表里的 ctx 仍指向上次已释放的
 * DbInstance（重开库后写 JSON 列直接 E_INVAL 或段错误）。
 * 该缺陷已在本轮修掉（见 value_json.c 里的说明），本测试是它的回归防线。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "db.h"

#include "db_internal.h"
#include "modalities/doc/json_ovf.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/json_ovf_reclaim.db"

static void rm_db(void)
{
    char aux[256];
    remove(PATH);
    snprintf(aux, sizeof(aux), "%s-wal", PATH);   remove(aux);
    snprintf(aux, sizeof(aux), "%s-clog", PATH);  remove(aux);
}

/* 造一份 > 2048B（走页链）但 < 65535B 的 JSON 文本 */
static void big_json(char *buf, size_t cap, int n)
{
    size_t off = (size_t)snprintf(buf, cap, "{\"v\":%d,\"pad\":\"", n);
    while (off + 8 < cap && off < 6000u) {
        buf[off++] = 'x';
    }
    buf[off++] = '"';
    buf[off++] = '}';
    buf[off] = '\0';
}

/* 数一数库里有多少个 tuple 引用了 JSON 页链（逐表扫，解出 12 字节标记） */
static u32 count_chain_rows(DbInstance *inst)
{
    u32 found = 0;
    u32 ntab = catalog_ntables(inst->cat);

    for (u32 i = 0; i < ntab; i++) {
        u32 tid = catalog_table_id_at(inst->cat, i);
        Heap *h = catalog_heap(inst->cat, tid);
        if (!h)
            continue;

        ColDesc cols[REC_MAX_COLS];
        int ncols = 0;
        if (catalog_table_cols(inst->cat, tid, cols, REC_MAX_COLS, &ncols) != OK ||
            ncols <= 0)
            continue;

        RecLayout L;
        if (rec_layout(cols, ncols, &L) != OK)
            continue;

        HeapScan *scan = NULL;
        if (heap_scan_begin(h, &scan) != OK)
            continue;

        u8 recbuf[PAGE_SIZE];
        u16 reclen = 0;
        Rid rid;
        while (heap_scan_next(scan, &rid, recbuf, sizeof(recbuf), &reclen) == OK) {
            Value vals[REC_MAX_COLS];
            if (rec_decode_layout(&L, cols, recbuf, reclen, vals) != OK)
                continue;
            for (int c = 0; c < ncols; c++) {
                if (cols[c].type != T_JSON || vals[c].is_null)
                    continue;
                if (rec_json_ovf_is_marker(vals[c].u.text.p, vals[c].u.text.n)) {
                    found++;
                    break;
                }
            }
        }
        heap_scan_end(scan);
    }
    return found;
}

/* 建一张带 JSON 列的表（三条用例都要） */
static int make_table(DB *db)
{
    if (db_exec(db, "CREATE TABLE t (id INT, doc JSON)", NULL) != DB_OK) {
        printf("    FAIL CREATE TABLE: %s\n", db_last_error(db));
        return -1;
    }
    return 0;
}

/* 插一行 pad 约 6000 字节的 JSON（走页链，两页） */
static int insert_big(DB *db, int id, int seed)
{
    char json[8192];
    char sql[9216];

    big_json(json, sizeof(json), seed);
    snprintf(sql, sizeof(sql), "INSERT INTO t VALUES (%d, '%s')", id, json);
    if (db_exec(db, sql, NULL) != DB_OK) {
        printf("    FAIL INSERT %d: %s\n", id, db_last_error(db));
        return -1;
    }
    return 0;
}

/*
 * ① VACUUM 回收链页：插入大 JSON → 删行 → VACUUM → 空闲页增加。
 *
 * 【为什么观测"空闲页计数"而不是"文件变小"】
 * pager 从不 shrink 文件（页还进 freelist），所以文件大小不变。
 * 空闲页计数变多才是"页真的被还回去了"的证据。
 */
static int test_vacuum_reclaims_chain(void)
{
    rm_db();

    DB *db = NULL;
    if (db_open(PATH, &db) != DB_OK) {
        printf("    FAIL db_open: %s\n", db_last_error(db));
        return -1;
    }
    if (make_table(db) != 0 || insert_big(db, 1, 1) != 0) {
        db_close(db);
        return -1;
    }

    CHECK(count_chain_rows(db->inst) == 1);

    DbInstance *inst = db->inst;
    u32 free_before = pager_free_count(inst->pager);

    if (db_exec(db, "DELETE FROM t WHERE id = 1", NULL) != DB_OK) {
        printf("    FAIL DELETE: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    if (db_exec(db, "VACUUM t", NULL) != DB_OK) {
        printf("    FAIL VACUUM: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }

    u32 free_after = pager_free_count(inst->pager);

    /*
     * 链本身占 2 页（6000+ 字节 / 4068 = 2 页），所以空闲页至少要涨 2。
     * 页内 compact 也可能顺带还页，因此用 >= 而不是 ==。
     */
    printf("    VACUUM: 空闲页 %u -> %u\n", free_before, free_after);
    CHECK(free_after >= free_before + 2u);

    /* 死 tuple 的 slot 也删掉了 ⇒ 库里不该再有引用链的行 */
    CHECK(count_chain_rows(inst) == 0);

    /* 回收之后库还要能用：再插一行小 JSON 并读回来 */
    if (db_exec(db, "INSERT INTO t VALUES (2, '{\"a\":1}')", NULL) != DB_OK) {
        printf("    FAIL INSERT after vacuum: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    Result *r = NULL;
    if (db_exec(db, "SELECT doc FROM t WHERE id = 2", &r) != DB_OK) {
        printf("    FAIL SELECT: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    CHECK(db_result_rows(r) == 1);
    const char *txt = db_result_value(r, 0, 0);
    CHECK(txt != NULL && strstr(txt, "\"a\"") != NULL);
    db_result_free(r);

    db_close(db);
    rm_db();
    return failures == 0 ? 0 : -1;
}

/*
 * ② 启动孤儿扫描：手工造一条没人引用的链 → 重开库 → 页被回收。
 *
 * 【为什么要重开库】
 * 回收器挂在 db_open 的 catalog 打开之后（设计 §8）。同一会话里它不会跑 ——
 * 这不是缺陷：运行时留下的孤儿本来就等下次开库才回收（设计 §6.4）。
 */
static int test_boot_reclaim_orphan(void)
{
    rm_db();

    DB *db = NULL;
    if (db_open(PATH, &db) != DB_OK) {
        printf("    FAIL db_open: %s\n", db_last_error(db));
        return -1;
    }
    if (make_table(db) != 0) {
        db_close(db);
        return -1;
    }

    DbInstance *inst = db->inst;

    /*
     * 手工造一条孤儿链：json_ovf_write 只写页链、不碰任何 tuple。
     * 这正是"链建好了、tuple 没写成"那个窗口的产物形态。
     */
    JsonOvfWal wal;
    wal.fn = db_wal_hook;
    wal.ctx = inst;
    wal.xid = 0;

    u8 payload[5000];
    memset(payload, 'Z', sizeof(payload));

    u64 orphan_head = PAGE_ID_NIL;
    int rc = json_ovf_write(inst->pager, &wal, payload, (u32)sizeof(payload),
                            &orphan_head);
    if (rc != OK || orphan_head == PAGE_ID_NIL) {
        printf("    FAIL json_ovf_write: %d\n", rc);
        db_close(db);
        return -1;
    }

    u32 free_before = pager_free_count(inst->pager);
    db_close(db);

    /* 重开：db_open 里的启动扫描应该把这条孤儿链还回去 */
    db = NULL;
    if (db_open(PATH, &db) != DB_OK) {
        printf("    FAIL reopen: %s\n", db_last_error(db));
        return -1;
    }
    u32 free_after = pager_free_count(db->inst->pager);

    printf("    启动回收: 空闲页 %u -> %u\n", free_before, free_after);
    /* 5000 字节 = 2 页 */
    CHECK(free_after >= free_before + 2u);

    /* 库里没有任何 tuple 引用链 ⇒ 收集器该看到 0 条引用 */
    CHECK(count_chain_rows(db->inst) == 0);

    db_close(db);
    rm_db();
    return failures == 0 ? 0 : -1;
}

/*
 * ③ 活链不许动：库里还有引用时，回收器必须放过它。
 *
 * 这是回收器最危险的错误方向 —— 误释放活链 = 用户的 JSON 变断链。
 * 观测量：重开库后那条引用还在，且读得回原文。
 */
static int test_live_chain_is_kept(void)
{
    rm_db();

    DB *db = NULL;
    if (db_open(PATH, &db) != DB_OK) {
        printf("    FAIL db_open\n");
        return -1;
    }
    if (make_table(db) != 0 || insert_big(db, 1, 7) != 0) {
        db_close(db);
        return -1;
    }
    CHECK(count_chain_rows(db->inst) == 1);
    db_close(db);

    /* 重开：回收器必须看到"有 1 条链被引用"，一页都不动 */
    db = NULL;
    if (db_open(PATH, &db) != DB_OK) {
        printf("    FAIL reopen: %s\n", db_last_error(db));
        return -1;
    }
    CHECK(count_chain_rows(db->inst) == 1);

    Result *r = NULL;
    if (db_exec(db, "SELECT doc FROM t WHERE id = 1", &r) != DB_OK) {
        printf("    FAIL SELECT: %s\n", db_last_error(db));
        db_close(db);
        return -1;
    }
    CHECK(db_result_rows(r) == 1);
    const char *txt = db_result_value(r, 0, 0);
    /*
     * 读得回原文的关键证据：渲染出的文本里能找到 payload 的填充字符，
     * 且长度接近原始 JSON。断链会在这里报错或给出半截内容。
     */
    CHECK(txt != NULL);
    if (txt) {
        CHECK(strlen(txt) > 2048);
        CHECK(strstr(txt, "xxxx") != NULL);
    }
    db_result_free(r);

    db_close(db);
    rm_db();
    return failures == 0 ? 0 : -1;
}

int main(void)
{
    printf("test_json_ovf_reclaim —— JSON 溢出页链的 VACUUM / 启动回收\n");

    printf("  1) VACUUM 回收链页\n");
    (void)test_vacuum_reclaims_chain();

    printf("  2) 启动扫描回收孤儿链\n");
    (void)test_boot_reclaim_orphan();

    printf("  3) 活链不许动\n");
    (void)test_live_chain_is_kept();

    if (failures == 0) {
        printf("  3 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
