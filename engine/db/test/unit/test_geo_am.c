/*
 * test_geo_am.c —— 地理空间模态的 SPI 适配层验收（v1.4 步骤 5）
 *
 * 六项验收：
 *   1. **扫描对得上**：插 N 个点 → 用 StorageAM 的 scan_begin / scan_next /
 *      decode 把它们扫出来 → 行数 == N，每行的 row_id / x / y / deleted
 *      都与插进去的值**逐位相等**，且列类型（INT8 / REAL / REAL / BOOL）正确。
 *   2. **schema**：columns() 返回 4 列，名字与顺序是 row_id / x / y / deleted。
 *   3. **RID 可回取**：scan_next 给的 RID 能用 get() 原样取回同一行。
 *   4. **删除状态由行内容表达**：rtree_mark_dead 之后，
 *      那一行**仍在**结果里，但 deleted == true；别的行不受影响。
 *   5. **只读**：insert / update / remove / vacuum / reindex 一律 E_NOTSUP；
 *      visible() 恒为 true。
 *   6. **重开**：关库再开，geo_state_open 能找回 meta 页与根页号，
 *      点数与扫描结果与关库前一致；没有 geo 存储的库返回 E_NOTFOUND。
 *
 * 【为什么逐位相等而不是"差不多"】
 *
 * 坐标是 float，存的是**位模式**。如果编码/解码哪一头用了
 * 别的舍入方式，差一点点也能"看起来对"——
 * 只有逐位相等才抓得住这类错。
 * 测试里的坐标都取**二进制可精确表示**的值（1.5 / 2.25），
 * 所以"精确相等"是应当成立的，不成立就是 bug。
 *
 * 【为什么删除后还要检查"行仍在"】
 *
 * 删除语义是"打标记"，不是"行消失"。
 * 被标记的行必须**照样出现在结果里**（deleted = true）——
 * 如果它消失了，就等于把同一件事说了两遍（标记 + 消失），
 * 语义就乱了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_am.h"
#include "modalities/geo/geo_state.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_scan.h"
#include "spi/storage_am.h"
#include "storage/pager.h"
#include "storage/value.h"

#define PATH "test/tmp/g6_geo_am.db"

#define N_PTS 64

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---------------- 测试数据 ---------------- */

/* 第 i 个点（0 起）：row_id = i+1，x = i*1.5，y = -i*2.25 */
static float px(int i)
{
    return (float)i * 1.5f;
}

static float py(int i)
{
    return (float)i * -2.25f;
}

/* 插 N 个点，顺带把根页号写回 meta 页 */
static int insert_points(GeoState *st, int n)
{
    int i;

    for (i = 0; i < n; i++) {
        int rc = rtree_insert(st->tree, (u32)i + 1u, px(i), py(i));

        if (rc != OK) {
            return rc;
        }
    }
    return geo_state_sync_root(st);
}

/* ---------------- 1~3：扫描、schema、RID 回取 ---------------- */

static void t_scan_rows(void)
{
    Pager *pg = NULL;
    GeoState *st = NULL;
    StorageAM am;
    GeoAmCtx ctx;
    AmScan *scan = NULL;
    const ColDesc *cols = NULL;
    DbValue out[GEO_POINTS_NCOLS];
    u8 buf[64];
    u8 buf2[64];
    u16 len = 0;
    u16 len2 = 0;
    Rid rid;
    Rid rid2;
    int ncols = 0;
    int nrows = 0;
    int i;

    memset(&am, 0, sizeof(am));
    memset(&ctx, 0, sizeof(ctx));
    memset(out, 0, sizeof(out));

    remove(PATH);
    CHECK(pager_open(PATH, 256, &pg) == OK);
    CHECK(geo_state_create(pg, &st) == OK);

    /*
     * 坑 M5-9：页号 0 归 catalog 的元数据页，地理模态不能占。
     */
    CHECK(st->meta_page != PAGE_ID_META);

    CHECK(insert_points(st, N_PTS) == OK);

    /* 计数：全活 */
    CHECK(geo_state_refresh_counts(st) == OK);
    CHECK(st->n_live == (u32)N_PTS);
    CHECK(st->n_total == (u32)N_PTS);

    CHECK(geo_am_build_points(st, &am, &ctx) == OK);
    CHECK(ctx.st == st);
    CHECK(am.impl == &ctx);
    CHECK(am.name != NULL && strcmp(am.name, "geo_points") == 0);
    /* 只读：没有写入能力位 */
    CHECK((am.caps & AM_CAP_SCAN) != 0u);
    CHECK((am.caps & AM_CAP_INSERT) == 0u);
    CHECK((am.caps & AM_CAP_UPDATE) == 0u);
    CHECK((am.caps & AM_CAP_REMOVE) == 0u);
    CHECK((am.caps & AM_CAP_VACUUM) == 0u);

    /* ---- schema ---- */
    CHECK(am.columns(&am, &cols, &ncols) == OK);
    CHECK(ncols == GEO_POINTS_NCOLS);
    if (ncols == GEO_POINTS_NCOLS && cols != NULL) {
        CHECK(strcmp(cols[GEO_COL_ID].name, "row_id") == 0);
        CHECK(strcmp(cols[GEO_COL_X].name, "x") == 0);
        CHECK(strcmp(cols[GEO_COL_Y].name, "y") == 0);
        CHECK(strcmp(cols[GEO_COL_DEAD].name, "deleted") == 0);
        CHECK(cols[GEO_COL_ID].type == T_INT8);
        CHECK(cols[GEO_COL_X].type == T_REAL);
        CHECK(cols[GEO_COL_Y].type == T_REAL);
        CHECK(cols[GEO_COL_DEAD].type == T_BOOL);
    }

    /* ---- 扫描 ---- */
    CHECK(am.scan_begin(&am, &scan) == OK);
    CHECK(scan != NULL);

    for (;;) {
        u64 rid_id;
        int rc = am.scan_next(&am, scan, &rid, buf, (u16)sizeof(buf), &len);

        if (rc == E_EOF) {
            break;
        }
        CHECK(rc == OK);
        if (rc != OK) {
            break;
        }
        CHECK(len == 16);
        memset(out, 0, sizeof(out));
        CHECK(am.decode(&am, buf, len, out) == OK);

        /* 列类型必须正确 */
        CHECK(out[GEO_COL_ID].type == T_INT8);
        CHECK(out[GEO_COL_X].type == T_REAL);
        CHECK(out[GEO_COL_Y].type == T_REAL);
        CHECK(out[GEO_COL_DEAD].type == T_BOOL);
        CHECK(!out[GEO_COL_ID].is_null);
        CHECK(!out[GEO_COL_X].is_null);
        CHECK(!out[GEO_COL_Y].is_null);
        CHECK(!out[GEO_COL_DEAD].is_null);

        rid_id = (u64)out[GEO_COL_ID].i;
        CHECK(rid_id >= 1u && rid_id <= (u64)N_PTS);
        if (rid_id < 1u || rid_id > (u64)N_PTS) {
            continue;
        }
        i = (int)(rid_id - 1u);

        /* 坐标必须逐位相等（存的值的位模式） */
        CHECK(out[GEO_COL_X].d == (double)px(i));
        CHECK(out[GEO_COL_Y].d == (double)py(i));
        /* 还没删过 ⇒ deleted == false */
        CHECK(out[GEO_COL_DEAD].i == 0);

        nrows++;

        /* ---- 3：RID 能原样取回同一行 ---- */
        memset(buf2, 0, sizeof(buf2));
        CHECK(am.get(&am, rid, buf2, (u16)sizeof(buf2), &len2) == OK);
        CHECK(len2 == 16);
        CHECK(memcmp(buf, buf2, 16) == 0);

        /* get 一个不存在的行 → E_NOTFOUND */
        rid2 = rid_make((u32)N_PTS + 100u, 0);
        CHECK(am.get(&am, rid2, buf2, (u16)sizeof(buf2), &len2) == E_NOTFOUND);
    }
    CHECK(nrows == N_PTS);
    CHECK(am.scan_end(&am, scan) == OK);

    /* 缓冲不够 → E_NOSPACE（不静默截断） */
    CHECK(am.scan_begin(&am, &scan) == OK);
    {
        u16 l = 0;
        Rid r2;

        CHECK(am.scan_next(&am, scan, &r2, buf, 8, &l) == E_NOSPACE);
    }
    CHECK(am.scan_end(&am, scan) == OK);

    /* ---- 5：只读 ---- */
    CHECK(am.insert(&am, 1u, out, &rid) == E_NOTSUP);
    CHECK(am.update(&am, 1u, rid, out, &rid2) == E_NOTSUP);
    CHECK(am.remove(&am, 1u, rid) == E_NOTSUP);
    {
        AmVacuumStats vs;

        memset(&vs, 0, sizeof(vs));
        CHECK(am.vacuum(&am, NULL, 1u, &vs) == E_NOTSUP);
    }
    {
        u64 rows_out = 0;

        CHECK(am.reindex(&am, 0u, &rows_out) == E_NOTSUP);
    }
    /* visible 恒为 true */
    CHECK(am.visible(&am, buf, 16, NULL) == true);

    geo_state_destroy(st);
    pager_close(pg);
}

/* ---------------- 4：删除状态由行内容表达 ---------------- */

static void t_mark_dead(void)
{
    Pager *pg = NULL;
    GeoState *st = NULL;
    StorageAM am;
    GeoAmCtx ctx;
    AmScan *scan = NULL;
    DbValue out[GEO_POINTS_NCOLS];
    u8 buf[64];
    u16 len = 0;
    Rid rid;
    u64 reads = 0;
    int nrows = 0;
    int saw_dead = 0;
    int saw_live = 0;
    int bad_dead = 0;

    memset(&am, 0, sizeof(am));
    memset(&ctx, 0, sizeof(ctx));

    remove(PATH);
    CHECK(pager_open(PATH, 256, &pg) == OK);
    CHECK(geo_state_create(pg, &st) == OK);
    CHECK(insert_points(st, N_PTS) == OK);

    /* 标记第 5 个点（row_id = 5）为已删除 */
    CHECK(rtree_mark_dead(pg, rtree_root_page(st->tree), 5u, &reads) == OK);

    /* 计数：活点少一个，总条目数不变 */
    CHECK(geo_state_refresh_counts(st) == OK);
    CHECK(st->n_live == (u32)(N_PTS - 1));
    CHECK(st->n_total == (u32)N_PTS);

    CHECK(geo_am_build_points(st, &am, &ctx) == OK);
    CHECK(am.scan_begin(&am, &scan) == OK);

    for (;;) {
        u64 rid_id;
        int rc = am.scan_next(&am, scan, &rid, buf, (u16)sizeof(buf), &len);

        if (rc == E_EOF) {
            break;
        }
        CHECK(rc == OK);
        if (rc != OK) {
            break;
        }
        memset(out, 0, sizeof(out));
        CHECK(am.decode(&am, buf, len, out) == OK);
        rid_id = (u64)out[GEO_COL_ID].i;
        nrows++;

        if (rid_id == 5u) {
            /* 死点**仍在**结果里，只是 deleted 变成 true */
            saw_dead++;
            CHECK(out[GEO_COL_DEAD].i == 1);
        } else {
            saw_live++;
            if (out[GEO_COL_DEAD].i != 0) {
                bad_dead++;
            }
        }
    }
    CHECK(am.scan_end(&am, scan) == OK);

    /* 行数不变（删除是打标记，不是消失） */
    CHECK(nrows == N_PTS);
    CHECK(saw_dead == 1);
    CHECK(saw_live == N_PTS - 1);
    CHECK(bad_dead == 0);

    geo_state_destroy(st);
    pager_close(pg);
}

/* ---------------- 6：重开 ---------------- */

static void t_reopen(void)
{
    Pager *pg = NULL;
    GeoState *st = NULL;
    GeoState *st2 = NULL;
    StorageAM am;
    GeoAmCtx ctx;
    AmScan *scan = NULL;
    u8 buf[64];
    u16 len = 0;
    Rid rid;
    u32 meta_page = 0;
    int nrows = 0;

    memset(&am, 0, sizeof(am));
    memset(&ctx, 0, sizeof(ctx));

    remove(PATH);
    CHECK(pager_open(PATH, 256, &pg) == OK);
    CHECK(geo_state_create(pg, &st) == OK);
    CHECK(insert_points(st, N_PTS) == OK);
    meta_page = st->meta_page;

    geo_state_destroy(st);
    CHECK(pager_close(pg) == OK);

    /* 重开：geo_state_open 要能从页空间里找回 meta 页与根页号 */
    CHECK(pager_open(PATH, 256, &pg) == OK);
    CHECK(geo_state_open(pg, &st2) == OK);
    CHECK(st2 != NULL);
    if (st2 != NULL) {
        CHECK(st2->meta_page == meta_page);
        CHECK(st2->n_live == (u32)N_PTS);
        CHECK(st2->n_total == (u32)N_PTS);
        CHECK(rtree_npoints(st2->tree) == (u32)N_PTS);

        CHECK(geo_am_build_points(st2, &am, &ctx) == OK);
        CHECK(am.scan_begin(&am, &scan) == OK);
        for (;;) {
            int rc = am.scan_next(&am, scan, &rid, buf, (u16)sizeof(buf), &len);

            if (rc == E_EOF) {
                break;
            }
            CHECK(rc == OK);
            if (rc != OK) {
                break;
            }
            nrows++;
        }
        CHECK(am.scan_end(&am, scan) == OK);
        CHECK(nrows == N_PTS);
        geo_state_destroy(st2);
    }
    CHECK(pager_close(pg) == OK);
}

/* ---------------- 6b：没有 geo 存储的库 ---------------- */

static void t_no_meta(void)
{
    Pager *pg = NULL;
    GeoState *st = NULL;

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    /* E_NOTFOUND 不是错误：这是个还没建过几何存储的库 */
    CHECK(geo_state_open(pg, &st) == E_NOTFOUND);
    CHECK(st == NULL);

    CHECK(pager_close(pg) == OK);
}

/* ---------------- 7：参数校验 ---------------- */

static void t_invalid(void)
{
    Pager *pg = NULL;
    GeoState *st = NULL;
    StorageAM am;
    GeoAmCtx ctx;

    memset(&am, 0, sizeof(am));
    memset(&ctx, 0, sizeof(ctx));

    remove(PATH);
    CHECK(pager_open(PATH, 64, &pg) == OK);

    CHECK(geo_state_create(NULL, &st) == E_INVAL);
    CHECK(geo_state_create(pg, NULL) == E_INVAL);
    CHECK(geo_state_open(NULL, &st) == E_INVAL);
    CHECK(geo_state_open(pg, NULL) == E_INVAL);

    CHECK(geo_state_create(pg, &st) == OK);
    CHECK(geo_am_build_points(NULL, &am, &ctx) == E_INVAL);
    CHECK(geo_am_build_points(st, NULL, &ctx) == E_INVAL);
    CHECK(geo_am_build_points(st, &am, NULL) == E_INVAL);

    /* 空树：扫不出任何行 */
    {
        AmScan *scan = NULL;
        u8 buf[64];
        u16 len = 0;
        Rid rid;

        CHECK(geo_am_build_points(st, &am, &ctx) == OK);
        CHECK(am.scan_begin(&am, &scan) == OK);
        CHECK(am.scan_next(&am, scan, &rid, buf, (u16)sizeof(buf), &len) == E_EOF);
        CHECK(am.scan_end(&am, scan) == OK);
    }

    geo_state_destroy(st);
    CHECK(pager_close(pg) == OK);
}

int main(void)
{
    printf("test_geo_am ...\n");

    t_scan_rows();
    t_mark_dead();
    t_reopen();
    t_no_meta();
    t_invalid();

    remove(PATH);

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
