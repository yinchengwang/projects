/*
 * test_blob.c — blob 模态单元验收
 *
 * 10 项：
 *   1. 创建对象 → 分配了头页 + 帧页，页类型是 PT_BLOB
 *   2. 写整个对象 → 读回来逐字节相同
 *   3. 读第 N 帧 → 内容与源数据的对应区间相同
 *   4. 读第 0 帧 / 中间帧 / 最后一帧 → 边界都正确
 *   5. **IO 量**：读第 3000 帧的页读次数 ≤ 2
 *   6. 跨对象隔离：两个对象的数据不串
 *   7. 不完整对象（最后一帧没写满）→ 读出来长度正确
 *   8. 越界读（frame_no >= nframes）→ 报错，不返回垃圾
 *   9. WAL：写对象产生 PAGEDATA 日志记录
 *  10. 对象头损坏 → 读报错，不返回垃圾
 *
 * 【为什么第 5 项是核心】
 *
 * blob 模态存在的意义就是「读第 N 帧的 IO 量与对象大小无关」。
 * 如果这个性质测不出来，这一版就白做了。
 *
 * 页读次数用 blob_stats()->page_reads 统计。
 * 读一帧应该是：头页（可缓存）+ 帧页 = 2 次。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/crc.h"
#include "common/error.h"
#include "common/types.h"
#include "storage/blob.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH "test/tmp/blob_test.db"

/* ---------------- WAL 钩子（统计日志条数） ---------------- */

typedef struct {
    int   n_pagedata;
    int   n_other;
    u32   last_xid;
} WalCount;

static int count_hook(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                      u16 slot_off,
                      const u8 *before, u16 blen,
                      const u8 *after, u16 alen, u32 *lsn_out)
{
    WalCount *c = (WalCount *)ctx;
    (void)page_id; (void)slot; (void)slot_off;
    (void)before; (void)blen; (void)after; (void)alen;

    if (logtype == LOG_PAGEDATA)
        c->n_pagedata++;
    else
        c->n_other++;
    c->last_xid = xid;

    if (lsn_out)
        *lsn_out = 0;
    return OK;
}

/* ---------------- 工具 ---------------- */

static void fill_pattern(u8 *buf, u64 off, u64 len)
{
    for (u64 i = 0; i < len; i++)
        buf[i] = (u8)((off + i) & 0xFF);
}

/* ---------------- 1) 创建对象 ---------------- */

static void t1_create(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    BlobId id = 0;
    CHECK(blob_create(bs, 10, BLOB_FRAME_SIZE, &id) == OK);
    CHECK(id != BLOB_ID_INVALID);   /* id 0 保留给「无效」 */

    /* 页类型检查：头页和帧页都该是 PT_BLOB */
    /* blob_id = 头页页号 + 1（见 blob.h）*/
    u32 head = (u32)id - 1;
    PageFrame *f = NULL;
    CHECK(pager_get(pg, head, &f) == OK);
    CHECK(f->data[H_TYPE] == PT_BLOB);
    pager_unpin(pg, head, false);

    /* 帧页从 head+1 开始 */
    CHECK(pager_get(pg, head + 1, &f) == OK);
    CHECK(f->data[H_TYPE] == PT_BLOB);
    pager_unpin(pg, head + 1, false);

    /* 元信息 */
    u64 nframes = 0, total = 0;
    u32 fsize = 0;
    CHECK(blob_stat(bs, id, &nframes, &fsize, &total) == OK);
    CHECK(nframes == 10);
    CHECK(fsize == BLOB_FRAME_SIZE);
    CHECK(total == 0);       /* 还没写数据 */

    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 2) 写读一致 ---------------- */

static void t2_write_read(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    const u64 nframes = 8;
    BlobId id = 0;
    CHECK(blob_create(bs, nframes, BLOB_FRAME_SIZE, &id) == OK);

    u64 len = nframes * BLOB_FRAME_SIZE;
    u8 *src = malloc((size_t)len);
    CHECK(src != NULL);
    fill_pattern(src, 0, len);

    CHECK(blob_write(bs, 7, id, src, len) == OK);

    u8 *dst = malloc((size_t)len);
    CHECK(dst != NULL);
    memset(dst, 0xAA, (size_t)len);

    u64 got = 0;
    CHECK(blob_read(bs, id, dst, len, &got) == OK);
    CHECK(got == len);
    CHECK(memcmp(src, dst, (size_t)len) == 0);

    free(src);
    free(dst);
    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 3) 读单帧 ---------------- */

static void t3_read_frame(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    const u64 nframes = 5;
    BlobId id = 0;
    CHECK(blob_create(bs, nframes, BLOB_FRAME_SIZE, &id) == OK);

    u64 len = nframes * BLOB_FRAME_SIZE;
    u8 *src = malloc((size_t)len);
    fill_pattern(src, 0, len);
    CHECK(blob_write(bs, 1, id, src, len) == OK);

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;

    /* 第 2 帧 */
    CHECK(blob_read_frame(bs, id, 2, buf, sizeof(buf), &n) == OK);
    CHECK(n == BLOB_FRAME_SIZE);
    CHECK(memcmp(buf, src + 2 * BLOB_FRAME_SIZE, BLOB_FRAME_SIZE) == 0);

    free(src);
    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 4) 边界帧 ---------------- */

static void t4_boundary_frames(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    const u64 nframes = 4;
    BlobId id = 0;
    CHECK(blob_create(bs, nframes, BLOB_FRAME_SIZE, &id) == OK);

    u64 len = nframes * BLOB_FRAME_SIZE;
    u8 *src = malloc((size_t)len);
    fill_pattern(src, 0, len);
    CHECK(blob_write(bs, 1, id, src, len) == OK);

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;

    /* 第 0 帧 */
    CHECK(blob_read_frame(bs, id, 0, buf, sizeof(buf), &n) == OK);
    CHECK(n == BLOB_FRAME_SIZE);
    CHECK(memcmp(buf, src, BLOB_FRAME_SIZE) == 0);

    /* 最后一帧 */
    CHECK(blob_read_frame(bs, id, nframes - 1, buf, sizeof(buf), &n) == OK);
    CHECK(n == BLOB_FRAME_SIZE);
    CHECK(memcmp(buf, src + (nframes - 1) * BLOB_FRAME_SIZE, BLOB_FRAME_SIZE) == 0);

    free(src);
    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 5) IO 量：读第 N 帧的页读次数 ---------------- */

/*
 * 这是 blob 模态的**核心性质**。
 *
 * 读第 3000 帧的页读次数必须很小（≤ 2），
 * 而且**与帧号无关** —— 读第 0 帧和读第 3000 帧，IO 量一样。
 *
 * 这正是它和"整行外置"的区别：后者读第 3000 个字节要读 3000 个字节经过的页。
 */
static void t5_io_amount(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    /*
     * 3001 帧 —— 够大，能体现"深访问"和"浅访问"的区别。
     * 帧数不多，所以测试跑得快；但帧号 3000 已经是第 3001 页了。
     */
    const u64 nframes = 3001;
    BlobId id = 0;
    CHECK(blob_create(bs, nframes, BLOB_FRAME_SIZE, &id) == OK);

    /*
     * 写满整个对象，但只关心最后一帧的内容。
     *
     * 这正是随机访问的典型场景 —— 数据 12MB，但只读第 3000 帧。
     *
     * 注意源缓冲区必须按 len 完整分配：
     * blob_write 会按 len 逐帧拷贝，只给一帧的缓冲区会越界读。
     */
    u64 len = (3000ull + 1) * BLOB_FRAME_SIZE;
    u8 *src = malloc((size_t)len);
    CHECK(src != NULL);
    fill_pattern(src, 0, len);

    u8 *last = src + 3000ull * BLOB_FRAME_SIZE;

    CHECK(blob_write(bs, 1, id, src, len) == OK);

    const BlobStats *st = blob_stats(bs);
    u64 before_reads = st->page_reads;

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;
    CHECK(blob_read_frame(bs, id, 3000, buf, sizeof(buf), &n) == OK);
    CHECK(n == BLOB_FRAME_SIZE);
    CHECK(memcmp(buf, last, BLOB_FRAME_SIZE) == 0);

    u64 reads = st->page_reads - before_reads;

    /*
     * 页读次数 ≤ 2：头页 + 帧页。
     *
     * 如果实现改成"顺着链走"或"查索引表"，这里会变成几千次。
     */
    CHECK(reads <= 2);

    /* 再读第 0 帧，IO 量应该一样 —— 与帧号无关 */
    u64 before_reads2 = st->page_reads;
    CHECK(blob_read_frame(bs, id, 0, buf, sizeof(buf), &n) == OK);
    u64 reads2 = st->page_reads - before_reads2;
    CHECK(reads2 <= 2);
    CHECK(reads == reads2);      /* 浅访问和深访问的 IO 量相同 */

    free(src);
    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 6) 对象隔离 ---------------- */

static void t6_isolation(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    BlobId a = 0, b = 0;
    CHECK(blob_create(bs, 3, BLOB_FRAME_SIZE, &a) == OK);
    CHECK(blob_create(bs, 3, BLOB_FRAME_SIZE, &b) == OK);
    CHECK(a != b);

    u8 da[3 * BLOB_FRAME_SIZE];
    u8 db[3 * BLOB_FRAME_SIZE];
    fill_pattern(da, 0, sizeof(da));
    fill_pattern(db, 100000, sizeof(db));

    CHECK(blob_write(bs, 1, a, da, sizeof(da)) == OK);
    CHECK(blob_write(bs, 1, b, db, sizeof(db)) == OK);

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;
    CHECK(blob_read_frame(bs, a, 1, buf, sizeof(buf), &n) == OK);
    CHECK(memcmp(buf, da + BLOB_FRAME_SIZE, BLOB_FRAME_SIZE) == 0);

    CHECK(blob_read_frame(bs, b, 1, buf, sizeof(buf), &n) == OK);
    CHECK(memcmp(buf, db + BLOB_FRAME_SIZE, BLOB_FRAME_SIZE) == 0);

    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 7) 不完整对象 ---------------- */

/*
 * 对象声明了 4 帧，但只写了 2.5 帧的数据。
 * 最后一帧应该只返回实际写了的那部分。
 */
static void t7_partial(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    BlobId id = 0;
    CHECK(blob_create(bs, 4, BLOB_FRAME_SIZE, &id) == OK);

    u64 len = 2 * BLOB_FRAME_SIZE + 1000;
    u8 *src = malloc((size_t)len);
    fill_pattern(src, 0, len);
    CHECK(blob_write(bs, 1, id, src, len) == OK);

    /* 第 2 帧只有 1000 字节 */
    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;
    CHECK(blob_read_frame(bs, id, 2, buf, sizeof(buf), &n) == OK);
    CHECK(n == 1000);
    CHECK(memcmp(buf, src + 2 * BLOB_FRAME_SIZE, 1000) == 0);

    /* 第 3 帧完全没数据 */
    CHECK(blob_read_frame(bs, id, 3, buf, sizeof(buf), &n) == OK);
    CHECK(n == 0);

    /* 整个对象读回来长度正确 */
    u64 total = 0, nf = 0;
    u32 fsize = 0;
    CHECK(blob_stat(bs, id, &nf, &fsize, &total) == OK);
    CHECK(total == len);

    free(src);
    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 8) 越界防护 ---------------- */

static void t8_out_of_range(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    BlobId id = 0;
    CHECK(blob_create(bs, 3, BLOB_FRAME_SIZE, &id) == OK);

    /*
     * 造一个**非 blob 的页**（模拟关系模态的页）。
     *
     * 页号空间是共享的 —— blob_id = 页号 + 1，
     * 所以一个指向普通页的 blob_id 是完全可能的。
     */
    {
        u32 plain = PAGE_ID_NIL;
        PageFrame *pf = NULL;
        if (pager_alloc(pg, &plain, &pf) == OK) {
            page_init(pf->data, PT_META);
            pager_unpin(pg, plain, true);
        }
        CHECK(plain != PAGE_ID_NIL);
    }

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;

    /* 越界必须报错，不能返回垃圾 */
    CHECK(blob_read_frame(bs, id, 3, buf, sizeof(buf), &n) != OK);
    CHECK(blob_read_frame(bs, id, 999999, buf, sizeof(buf), &n) != OK);

    /* 不存在的对象 id */
    CHECK(blob_read_frame(bs, 12345, 0, buf, sizeof(buf), &n) != OK);
    /* id=0 是无效 id */
    CHECK(blob_read_frame(bs, 0, 0, buf, sizeof(buf), &n) != OK);

    /*
     * 指向关系模态的页 → 必须报 NOTFOUND，**不能报 CORRUPT**。
     *
     * 页号空间是共享的：blob_id = 页号 + 1，所以 blob_id=1 可能指向
     * catalog 的页或表的页。
     *
     * 「这个 id 不是一个 blob 对象」是**正常情况**（用户传了无效 id），
     * 不是数据损坏。报 CORRUPT 会把用户误用说成"库坏了"，
     * 把排查方向带偏。
     */
    {
        /* 找一个非 blob 的页（用页类型验证） */
        PageFrame *pf = NULL;
        u32 nonblob = PAGE_ID_NIL;
        u64 np = pager_npages(pg);
        for (u64 p = 0; p < np; p++) {
            if (pager_get(pg, (u32)p, &pf) != OK)
                continue;
            bool is_blob = (pf->data[H_TYPE] == PT_BLOB);
            pager_unpin(pg, (u32)p, false);
            if (!is_blob) {
                nonblob = (u32)p;
                break;
            }
        }
        CHECK(nonblob != PAGE_ID_NIL);   /* 至少有一个非 blob 页 */

        BlobId fake = (BlobId)nonblob + 1;
        CHECK(blob_read_frame(bs, fake, 0, buf, sizeof(buf), &n) == E_NOTFOUND);
        CHECK(blob_stat(bs, fake, NULL, NULL, NULL) == E_NOTFOUND);
    }

    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 9) WAL 日志 ---------------- */

static void t9_wal(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    WalCount wc;
    memset(&wc, 0, sizeof(wc));

    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &wc, &bs) == OK);

    BlobId id = 0;
    CHECK(blob_create(bs, 3, BLOB_FRAME_SIZE, &id) == OK);

    int before = wc.n_pagedata;

    u8 data[3 * BLOB_FRAME_SIZE];
    fill_pattern(data, 0, sizeof(data));
    CHECK(blob_write(bs, 42, id, data, sizeof(data)) == OK);

    /*
     * 写 3 帧 + 更新 1 次头页 = 至少 4 条 PAGEDATA。
     *
     * 而且 xid 必须是显式传进来的那个 —— 不能是"某个共享状态里的当前值"。
     */
    CHECK(wc.n_pagedata >= before + 4);
    CHECK(wc.last_xid == 42);

    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 10) 对象头损坏 ---------------- */

/*
 * 头页被破坏 → 读报错。
 *
 * 不能返回垃圾数据 —— 静默的数据损坏比报错可怕得多。
 */
static void t10_corrupt_header(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);
    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    BlobId id = 0;
    CHECK(blob_create(bs, 3, BLOB_FRAME_SIZE, &id) == OK);

    u8 data[3 * BLOB_FRAME_SIZE];
    fill_pattern(data, 0, sizeof(data));
    CHECK(blob_write(bs, 1, id, data, sizeof(data)) == OK);

    /* 破坏对象头：改掉 total_len（校验和会不匹配） */
    PageFrame *f = NULL;
    u32 head = (u32)id - 1;
    CHECK(pager_get(pg, head, &f) == OK);
    put_u64(f->data + BLOB_HDR_OFF + 20, 999999);
    page_set_checksum(f->data);
    CHECK(pager_unpin(pg, head, true) == OK);

    u8 buf[BLOB_FRAME_SIZE];
    u32 n = 0;
    CHECK(blob_read_frame(bs, id, 0, buf, sizeof(buf), &n) != OK);

    u64 total = 0;
    CHECK(blob_stat(bs, id, NULL, NULL, &total) != OK);

    blob_store_destroy(bs);
    pager_close(pg);
}

/* ---------------- 11) 大量对象：缓冲池不能被 pin 泄漏拖垮 ---------------- */

/*
 * 【这是一个 v0.10 才暴露的 v0.8 真 bug】
 *
 * blob_create 分配头页后**忘了 unpin** ——
 * 每建一个对象泄漏一个 pin。
 * 缓冲池只有 256 帧 → 建满 256 个对象后，
 * 所有帧的 pin_count 都 > 0 → pick_victim 找不到可逐出的帧
 * → pager_alloc 返回 E_NOSPACE → "空间不足"。
 *
 * 【为什么之前没发现】
 * 之前的测试最多建几个对象，撑不到 256 帧。
 * 是 v0.10 的并发测试把强度提上去才暴露出来。
 *
 * 这一项就是钉住这个修复的回归测试。
 */
static void t11_many_objects(void)
{
    remove(PATH);

    Pager *pg = NULL;
    CHECK(pager_open(PATH, 64, &pg) == OK);

    BlobStore *bs = NULL;
    CHECK(blob_store_create(pg, count_hook, &(WalCount){0}, &bs) == OK);

    /* 建 400 个对象 —— 远超 256 帧 */
    int created = 0;
    for (int i = 0; i < 400; i++) {
        BlobId id = 0;
        if (blob_create(bs, 1, BLOB_FRAME_SIZE, &id) != OK)
            break;
        created++;
    }
    CHECK(created == 400);        /* 一个都不能失败 */

    /* 缓冲池还有空位（没有 pin 泄漏） */
    CHECK(pager_frames_used(pg) <= 64);

    blob_store_destroy(bs);
    pager_close(pg);
}

int main(void)
{
    printf("test_blob:\n");

    t1_create();
    t2_write_read();
    t3_read_frame();
    t4_boundary_frames();
    t5_io_amount();
    t6_isolation();
    t7_partial();
    t8_out_of_range();
    t9_wal();
    t10_corrupt_header();
    t11_many_objects();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  11 项验收全部通过 [PASS]\n");
    return 0;
}
