/*
 * test_blob_am.c — blob 模态的 SPI 适配验收
 *
 * 7 项：
 *   1. blob_objects 的 schema 和行内容
 *   2. blob_frames 的 schema 和行内容（含 RID）
 *   3. get(rid) 与扫描产出的行一致（往返）
 *   4. decode 产出的是**外部值**（不是截断的内联文本）
 *   5. 下推：blob_id 等值 → 只枚举那一个对象
 *   6. 能力协商：写接口返回 E_NOTSUP；可见性恒真
 *   7. **IO 量**：扫描帧表不读帧页（外部值是惰性的）
 *
 * 【第 4 项和第 7 项是核心】
 *
 * blob 的一帧是 4076 字节，塞不进 SQL 的标量值（上限 255）。
 * 所以帧的内容必须是**外部值**：值里只挂句柄，内容按需取。
 *
 * 第 4 项验"形态对不对"（是外部值，不是被截断的文本）；
 * 第 7 项验"惰性有没有生效"（扫表不读帧页，取内容才读）。
 * 少了任何一项，这版就白做了。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "spi/blob_am.h"
#include "storage/blob.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/blob_am.db"

/* ---------------- 环境 ---------------- */

typedef struct {
    Pager     *pg;
    BlobStore *bs;
    StorageAM  am_frames;
    StorageAM  am_objects;
    BlobAmCtx  ctx_frames;
    BlobAmCtx  ctx_objects;
} Env;

static int env_open(Env *e)
{
    memset(e, 0, sizeof(*e));

    remove(PATH);
    remove("test/tmp/blob_am.db-wal");

    int rc = pager_open(PATH, 64, &e->pg);
    if (rc != OK)
        return rc;

    rc = blob_store_create(e->pg, NULL, NULL, &e->bs);
    if (rc != OK)
        return rc;

    rc = blob_am_build_frames(e->bs, &e->am_frames, &e->ctx_frames);
    if (rc != OK)
        return rc;

    return blob_am_build_objects(e->bs, &e->am_objects, &e->ctx_objects);
}

static void env_close(Env *e)
{
    blob_store_destroy(e->bs);
    pager_close(e->pg);
    memset(e, 0, sizeof(*e));
}

/* 造一个对象：nframes 帧，内容是 "obj<N>" 重复 */
static BlobId make_object(Env *e, int nframes, const char *tag)
{
    BlobId id = 0;
    if (blob_create(e->bs, (u64)nframes, BLOB_FRAME_SIZE, &id) != OK)
        return BLOB_ID_INVALID;

    char payload[256];
    snprintf(payload, sizeof(payload), "%s", tag);
    size_t len = strlen(payload);
    blob_write(e->bs, 0, id, (const u8 *)payload, len);
    return id;
}

/* ---------------- 1) blob_objects ---------------- */

static void t1_objects(Env *e)
{
    const ColDesc *cols = NULL;
    int ncols = 0;
    CHECK(e->am_objects.columns(&e->am_objects, &cols, &ncols) == OK);
    CHECK(ncols == 4);
    CHECK(cols && strcmp(cols[0].name, "blob_id") == 0);
    CHECK(cols && strcmp(cols[1].name, "nframes") == 0);
    CHECK(cols && strcmp(cols[2].name, "frame_size") == 0);
    CHECK(cols && strcmp(cols[3].name, "total_len") == 0);

    AmScan *scan = NULL;
    CHECK(e->am_objects.scan_begin(&e->am_objects, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;

    int n = 0;
    while (e->am_objects.scan_next(&e->am_objects, scan, &rid,
                                   buf, sizeof(buf), &len) == OK) {
        CHECK(len == 28);
        CHECK(rid.slot == 0);
        /* RID.page = 头页页号 = blob_id - 1 */
        CHECK(rid.page == (u32)(get_u64(buf) - 1));

        DbValue v[4];
        CHECK(e->am_objects.decode(&e->am_objects, buf, len, v) == OK);
        CHECK(v[0].i == (i64)get_u64(buf));
        CHECK(v[1].i == (i64)get_u64(buf + 8));
        CHECK(v[2].i == (i64)get_u32(buf + 16));
        CHECK(v[3].i == (i64)get_u64(buf + 20));
        n++;
    }
    CHECK(n == 2);      /* 两个对象 */
    e->am_objects.scan_end(&e->am_objects, scan);
}

/* ---------------- 2) blob_frames ---------------- */

static void t2_frames(Env *e, BlobId id1)
{
    const ColDesc *cols = NULL;
    int ncols = 0;
    CHECK(e->am_frames.columns(&e->am_frames, &cols, &ncols) == OK);
    CHECK(ncols == 5);
    CHECK(cols && strcmp(cols[0].name, "blob_id") == 0);
    CHECK(cols && strcmp(cols[1].name, "frame_no") == 0);
    CHECK(cols && strcmp(cols[2].name, "data_len") == 0);
    CHECK(cols && strcmp(cols[3].name, "data_off") == 0);
    CHECK(cols && strcmp(cols[4].name, "data") == 0);

    AmScan *scan = NULL;
    CHECK(e->am_frames.scan_begin(&e->am_frames, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;

    int n = 0;
    int seen_id1 = 0;
    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        CHECK(len == 16);
        u64 bid = get_u64(buf);
        u64 fno = get_u64(buf + 8);

        /* RID.page = base_page + frame_no */
        u64 nf = 0, tl = 0;
        u32 fs = 0, base = 0;
        CHECK(blob_header(e->bs, (BlobId)bid, &nf, &fs, &tl, &base) == OK);
        CHECK(rid.page == base + (u32)fno);
        CHECK(rid.slot == 0);

        if (bid == (u64)id1) {
            seen_id1++;
            CHECK(fno == (u64)seen_id1 - 1);
        }
        n++;
    }
    CHECK(n == 5);          /* 对象1: 3 帧 + 对象2: 2 帧 */
    CHECK(seen_id1 == 3);
    e->am_frames.scan_end(&e->am_frames, scan);
}

/* ---------------- 3) get(rid) 往返 ---------------- */

static void t3_get_roundtrip(Env *e)
{
    AmScan *scan = NULL;
    CHECK(e->am_frames.scan_begin(&e->am_frames, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;
    int checked = 0;

    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        u8 buf2[64];
        u16 len2 = 0;
        CHECK(e->am_frames.get(&e->am_frames, rid, buf2, sizeof(buf2), &len2) == OK);
        CHECK(len2 == len);
        CHECK(memcmp(buf, buf2, len) == 0);
        checked++;
    }
    CHECK(checked == 5);
    e->am_frames.scan_end(&e->am_frames, scan);

    /* 越界的 RID → 找不到，不是返回垃圾 */
    Rid bogus = rid_make(0xFFFFFF, 0);
    u8 tmp[64];
    u16 tmplen = 0;
    CHECK(e->am_frames.get(&e->am_frames, bogus, tmp, sizeof(tmp), &tmplen)
          == E_NOTFOUND);
}

/* ---------------- 4) decode 产出外部值 ---------------- */

static void t4_external_value(Env *e)
{
    AmScan *scan = NULL;
    CHECK(e->am_frames.scan_begin(&e->am_frames, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;
    int checked = 0;

    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        u64 bid = get_u64(buf);
        u64 fno = get_u64(buf + 8);

        DbValue v[5];
        CHECK(e->am_frames.decode(&e->am_frames, buf, len, v) == OK);

        /* 前四列是内联值 */
        CHECK(!v[0].external);
        CHECK(!v[1].external);
        CHECK(!v[2].external);
        CHECK(!v[3].external);

        /* 第五列（data）是外部值 —— 句柄 = (blob_id, frame_no) */
        CHECK(v[4].external);
        CHECK(v[4].ext_key == bid);
        CHECK(v[4].ext_off == fno);
        CHECK(v[4].ext_fn != NULL);

        /*
         * 物化之后内容必须和 blob API 读出来的一致。
         * 而且长度要一样 —— 如果是"截断成 255 字节"，这里就会露馅。
         */
        u8 *out = NULL;
        u32 outlen = 0;
        CHECK(val_detoast(&v[4], &out, &outlen) == OK);
        CHECK(out != NULL);

        u8 direct[512];
        u32 directlen = 0;
        CHECK(blob_read_frame(e->bs, (BlobId)bid, fno, direct,
                              sizeof(direct), &directlen) == OK);
        CHECK(outlen == directlen);
        /* 第 0 帧有内容（后面几帧是空的，对象没写那么长） */
        if (fno == 0)
            CHECK(outlen > 0);
        CHECK(out && memcmp(out, direct, directlen) == 0);
        free(out);

        checked++;
    }
    CHECK(checked == 5);
    e->am_frames.scan_end(&e->am_frames, scan);
}

/* ---------------- 5) 下推 ---------------- */

static void t5_pushdown(Env *e, BlobId id1)
{
    /* 只有 blob_id（第 0 列）能推 */
    Predicate p;
    memset(&p, 0, sizeof(p));

    p.col_idx = 0;
    p.op = PD_EQ;
    val_int(&p.val, (i64)id1);
    CHECK(e->am_frames.can_push(&e->am_frames, &p) == OK);

    /* 别的列推不了 */
    p.col_idx = 1;                 /* frame_no */
    CHECK(e->am_frames.can_push(&e->am_frames, &p) == E_NOTSUP);

    /* 不支持的算子推不了 */
    p.col_idx = 0;
    p.op = PD_NE;
    CHECK(e->am_frames.can_push(&e->am_frames, &p) == E_NOTSUP);

    /* 下推：只枚举那一个对象的帧 */
    p.op = PD_EQ;
    val_int(&p.val, (i64)id1);

    AmScan *scan = NULL;
    CHECK(e->am_frames.push(&e->am_frames, &p, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;
    int n = 0;
    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        CHECK(get_u64(buf) == (u64)id1);
        n++;
    }
    CHECK(n == 3);                 /* 只扫了 id1 的 3 帧，没有 id2 的 2 帧 */
    e->am_frames.scan_end(&e->am_frames, scan);

    /* 范围下推：两个对象都在范围内 */
    p.op = PD_GE;
    val_int(&p.val, 1);
    CHECK(e->am_frames.push(&e->am_frames, &p, &scan) == OK);
    n = 0;
    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK)
        n++;
    CHECK(n == 5);
    e->am_frames.scan_end(&e->am_frames, scan);
}

/* ---------------- 6) 能力协商 + 可见性 ---------------- */

static void t6_caps_and_visibility(Env *e)
{
    /* 只读模态：没有写能力位 */
    CHECK((e->am_frames.caps & AM_CAP_SCAN) != 0);
    CHECK((e->am_frames.caps & AM_CAP_EXTERNAL) != 0);
    CHECK((e->am_frames.caps & AM_CAP_PUSHDOWN) != 0);
    CHECK((e->am_frames.caps & AM_CAP_INSERT) == 0);
    CHECK((e->am_frames.caps & AM_CAP_UPDATE) == 0);
    CHECK((e->am_frames.caps & AM_CAP_REMOVE) == 0);
    CHECK((e->am_frames.caps & AM_CAP_VACUUM) == 0);
    CHECK((e->am_frames.caps & AM_CAP_INDEX) == 0);

    /* 写接口返回 E_NOTSUP（不是 E_INVAL） */
    DbValue v[5];
    memset(v, 0, sizeof(v));
    Rid rid;
    CHECK(e->am_frames.insert(&e->am_frames, 1, v, &rid) == E_NOTSUP);
    CHECK(e->am_frames.update(&e->am_frames, 1, rid, v, &rid) == E_NOTSUP);
    CHECK(e->am_frames.remove(&e->am_frames, 1, rid) == E_NOTSUP);

    /* 可见性恒真：对象不可变 → 没有版本 → 恒可见 */
    u8 buf[64];
    u16 len = 0;
    AmVisCtx vis;
    memset(&vis, 0, sizeof(vis));
    CHECK(e->am_frames.visible(&e->am_frames, buf, len, &vis) == true);
}

/* ---------------- 7) IO 量：扫描不读帧页 ---------------- */

/*
 * 这是外部值**惰性**的证明。
 *
 * 扫帧表只读对象头（每对象一次），不读帧页。
 * 如果 decode 把内容物化了（或者模态在扫描时读了帧），
 * page_reads 就会上涨 —— 测试会红。
 */
static void t7_io_amount(Env *e)
{
    const BlobStats *st = blob_stats(e->bs);

    u64 before = st->page_reads;

    /* 只扫元数据列 —— 不该读任何帧页 */
    AmScan *scan = NULL;
    CHECK(e->am_frames.scan_begin(&e->am_frames, &scan) == OK);

    u8 buf[64];
    u16 len = 0;
    Rid rid;
    DbValue v[5];
    int n = 0;

    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        CHECK(e->am_frames.decode(&e->am_frames, buf, len, v) == OK);
        /* 只看元数据列，不碰 data */
        CHECK(v[0].i > 0);
        n++;
    }
    CHECK(n == 5);
    e->am_frames.scan_end(&e->am_frames, scan);

    u64 after_scan = blob_stats(e->bs)->page_reads;
    CHECK(after_scan == before);      /* 扫描 + decode 都没读帧页 ✓ */

    /* 现在取 data —— 这才读帧页 */
    CHECK(e->am_frames.scan_begin(&e->am_frames, &scan) == OK);
    while (e->am_frames.scan_next(&e->am_frames, scan, &rid,
                                  buf, sizeof(buf), &len) == OK) {
        CHECK(e->am_frames.decode(&e->am_frames, buf, len, v) == OK);
        if (v[4].external) {
            u8 *out = NULL;
            u32 outlen = 0;
            if (val_detoast(&v[4], &out, &outlen) == OK)
                free(out);
        }
    }
    e->am_frames.scan_end(&e->am_frames, scan);

    u64 after_detoast = blob_stats(e->bs)->page_reads;
    CHECK(after_detoast > after_scan);    /* 物化才读帧页 ✓ */
}

int main(void)
{
    printf("test_blob_am:\n");

    Env e;
    if (env_open(&e) != OK) {
        printf("    FAIL 环境初始化失败\n");
        return 1;
    }

    BlobId id1 = make_object(&e, 3, "obj1-payload");
    BlobId id2 = make_object(&e, 2, "obj2-payload");
    CHECK(id1 != BLOB_ID_INVALID);
    CHECK(id2 != BLOB_ID_INVALID);
    CHECK(id1 != id2);

    t1_objects(&e);
    t2_frames(&e, id1);
    t3_get_roundtrip(&e);
    t4_external_value(&e);
    t5_pushdown(&e, id1);
    t6_caps_and_visibility(&e);
    t7_io_amount(&e);

    env_close(&e);

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  7 项验收全部通过 [PASS]\n");
    return 0;
}
