/*
 * test_ts_crash_mid.c —— 时序模态崩溃恢复验收 · 崩在**多页操作中间**
 *                        （v1.5 步骤 6 · T10 · A7）
 *
 * 另一半（db 层的真 WAL、真断电）在 test_ts_crash.c 里做。
 * 这一份专盯**多页操作的中间崩**：
 *
 *   4) 崩在**封窗中间** → 重开：要么完整旧分区、要么完整新分区，**绝不是半截**
 *   5) 崩在 **TTL 中间** → 重开：不会出现"目录说分区还在、页却被回收了"
 *
 * ==================== 为什么这两项不能在 db 层做 ====================
 *
 * db 层的写日志钩子（db_wal_hook）是写死的，没有"在第 N 个写点上崩"的开关。
 * 而这两项要测的恰恰是"**多页操作进行到一半**"的状态。
 *
 * 所以这一份在**模态层**做：接一个**按剧本失败的假钩子**。
 *
 * 假钩子失败 = "这一刻写日志失败"（等价于崩在写日志那一步）。
 * 这时 ts_pg_commit 会把页**回滚成改之前的样子**、按"不是脏页"归还
 * ⇒ 半改的页绝不落盘（ts_wal.h 的硬要求）。
 * 之后再 pager_abandon（脏页全丢 = 断电），重开库读一遍 ⇒ 断言"不是半截"。
 *
 * ⚠️ 假钩子**不写真日志** ⇒ 它测的不是"重放能不能补回来"，
 *    而是"**崩在中间时，磁盘上的状态是不是自洽的**"。
 *    前者由 test_ts_crash.c 的真 WAL 那几项负责。
 *
 * ==================== 第 4 项的判据是"不是半截" ====================
 *
 * 封窗 = 写列式页（多页）+ 翻目录项（一步）。
 * 崩在中间时，重开库可能看到：
 *
 *   目录项 ACTIVE  ⇒ 读路径走裸写页 ⇒ 完整的**旧**分区
 *   目录项 SEALED  ⇒ 读路径走列式页 ⇒ 完整的**新**分区
 *
 * 绝不能是"目录项说 SEALED、列式页却只写了一半" ——
 * 那会读出**半截数据**，而且往往"看起来正常"，比直接报错坏得多。
 *
 * 保证它的不是 WAL（WAL 只保证"改了就有日志、能重放补齐"），
 * 而是"**先写列式页、最后才翻目录项**"这个顺序
 * —— 目录项的状态是唯一的权威（坑 M6-40、M6-63）。
 *
 * 三个子场景：
 *   4a 写日志失败（列式页写了一半）⇒ 目录项没翻 ⇒ 完整的旧分区
 *   4b 列式页写完了、目录项还没翻就崩 ⇒ 完整的旧分区
 *   4c 目录项翻了 ⇒ 完整的新分区（列式）
 *
 * ==================== 第 5 项的判据是"方向对" ====================
 *
 * TTL = 改目录项（一步）+ 回收页（多步）。
 * 顺序必须"先改目录、后回收页"（坑 M6-49）：
 *
 *   先回收页、后改目录 ⇒ 崩在中间 ⇒ 目录说"这个分区还在"，
 *   但页已经被别人拿去用了 ⇒ 查询读到**别人的数据**，而且不报错。
 *
 *   先改目录、后回收页 ⇒ 崩在中间 ⇒ 目录说"已过期"、页还占着
 *   ⇒ 只是**空间泄漏**（下次 TTL 再收一次），数据没有错。
 *
 * 两个子场景：
 *   5a 崩在"改目录项"那一步 ⇒ 目录项还是 ACTIVE ⇒ 数据一条不少
 *   5b 崩在"回收页"那一步 ⇒ 目录项已经是 DROPPED ⇒ 页还占着（空间泄漏）
 *
 * 两个场景合起来说明：**绝不会**出现"目录说分区还在、页却被回收了"。
 */

#include "ts_crash_common.h"

#include "common/error.h"
#include "common/loghook.h"
#include "modalities/ts/ts_dir.h"
#include "modalities/ts/ts_seal.h"
#include "modalities/ts/ts_ttl.h"
#include "modalities/ts/ts_wal.h"
#include "modalities/ts/tsstore.h"
#include "storage/page.h"
#include "storage/pager.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define PATH4 "test/tmp/ts_crash4.db"
#define PATH5 "test/tmp/ts_crash5.db"
#define PATH6 "test/tmp/ts_crash6.db"

/* ==================== 假钩子 ==================== */

/*
 * 假钩子：按剧本失败。
 *
 * 两种失败方式：
 *   fail_at      —— 从第 N 次调用开始失败（模拟"崩在第 N 个写点上"）
 *   fail_on_zero —— after 镜像**全零**时失败
 *                   （全零 = 这一页被清零了 = TTL 回收页那一步）
 */
typedef struct {
    u32  calls;
    u32  fail_at;
    bool fail_on_zero;
    u32  lsn;
} FakeWal;

static int fake_log(void *ctx, u32 xid, u8 logtype, u32 page_id, u16 slot,
                    u16 slot_off, const u8 *before, u16 before_len,
                    const u8 *after, u16 after_len, u32 *lsn_out)
{
    FakeWal *f = (FakeWal *)ctx;

    (void)xid;
    (void)logtype;
    (void)page_id;
    (void)slot;
    (void)slot_off;
    (void)before;
    (void)before_len;

    f->calls++;

    if (f->fail_on_zero && after && after_len == PAGE_SIZE) {
        u16 i;

        for (i = 0; i < PAGE_SIZE; i++) {
            if (after[i] != 0) {
                break;
            }
        }
        if (i == PAGE_SIZE) {
            return E_IO;       /* 清零页那一步失败 */
        }
    }

    if (f->fail_at != 0u && f->calls >= f->fail_at) {
        return E_IO;
    }

    if (lsn_out) {
        *lsn_out = ++f->lsn;
    }
    return OK;
}

/* ---------------- 模态层的扫描回调 ---------------- */

typedef struct {
    const Row *expect;
    int n;
    int i;
    int mismatch;
} ScanCtx;

static int scan_cb(const TsReading *r, void *ctx)
{
    ScanCtx *s = (ScanCtx *)ctx;

    if (s->i >= s->n) {
        s->mismatch++;             /* 读出来的比写进去的多 */
        s->i++;
        return 0;
    }
    if (r->time != s->expect[s->i].time || r->tag != s->expect[s->i].tag) {
        s->mismatch++;
    }
    if (ts_crash_dbits(r->value) != ts_crash_dbits(s->expect[s->i].value)) {
        s->mismatch++;
    }
    s->i++;
    return 0;
}

/* ==================== 第 4 项：崩在封窗中间 ==================== */

static void t4_seal_crash(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    FakeWal fw;
    TsWal w;
    Row expect[64];
    TsPartInfo pi;
    ScanCtx sc;
    u64 reads = 0;
    u32 head = 0;
    u32 pages = 0;
    u32 rows = 0;
    const int n = 40;
    int i;
    int rc;

    printf("  4) 崩在封窗中间：要么完整旧分区、要么完整新分区，绝不是半截\n");

    /* ---- 4a：写日志失败（列式页只写了一半） ---- */
    memset(&fw, 0, sizeof(fw));
    w.fn = fake_log;
    w.ctx = &fw;

    remove(PATH4);
    CHECK(pager_open(PATH4, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_create_wal(pg, 1, &w, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }

    ts_crash_fill(expect, n, 1000000LL, 1);
    for (i = 0; i < n; i++) {
        TsReading r;

        r.time = expect[i].time;
        r.tag = expect[i].tag;
        r.value = expect[i].value;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);

    /* 落盘：模拟"封窗之前的状态已经在盘上" */
    CHECK(pager_flush_all(pg) == OK);

    /* 剧本：再写 2 条日志就失败（列式页写到一半） */
    fw.fail_at = fw.calls + 2u;
    rc = ts_seal_write_wal(pg, pi.first_page, 1u, &reads, &w, &head, &pages, &rows);
    CHECK(rc != OK);
    printf("    4a 写日志在第 %u 条失败（返回 %d）⇒ 列式页只写了一半\n",
           fw.calls, rc);

    /* 断电：脏页全丢（写了一半的列式页绝不落盘） */
    ts_store_destroy(st);
    st = NULL;
    pager_abandon(pg);
    pg = NULL;

    /* 重开：目录项还是 ACTIVE ⇒ 读到的必须是**完整的旧分区** */
    CHECK(pager_open(PATH4, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_open(pg, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    sc.expect = expect;
    sc.n = n;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);
    printf("    4a 重开后：目录项 ACTIVE，%d 条一行不差（旧分区完整）\n", sc.i);

    /* ---- 4b：列式页写完了、目录项还没翻就崩 ---- */
    memset(&fw, 0, sizeof(fw));
    ts_store_destroy(st);
    st = NULL;
    pager_close(pg);
    pg = NULL;

    remove(PATH4);
    CHECK(pager_open(PATH4, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_create_wal(pg, 1, &w, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    for (i = 0; i < n; i++) {
        TsReading r;

        r.time = expect[i].time;
        r.tag = expect[i].tag;
        r.value = expect[i].value;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pager_flush_all(pg) == OK);

    /* 列式页写成功（不翻目录）⇒ 目录项还是 ACTIVE */
    CHECK(ts_seal_write_wal(pg, pi.first_page, 1u, &reads, &w, &head, &pages,
                            &rows) == OK);
    CHECK(rows == (u32)n);
    CHECK(pager_flush_all(pg) == OK);

    /* 崩在这里：目录项没翻 */
    ts_store_destroy(st);
    st = NULL;
    pager_abandon(pg);
    pg = NULL;

    CHECK(pager_open(PATH4, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_open(pg, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);
    printf("    4b 重开后：列式页在盘上但目录项没翻 ⇒ 仍是完整的旧分区\n");

    /* ---- 4c：目录项翻了 ⇒ 完整的新分区（列式） ---- */
    CHECK(ts_store_seal_commit(st, 0, head, pages, rows) == OK);
    CHECK(pager_flush_all(pg) == OK);

    ts_store_destroy(st);
    st = NULL;
    pager_abandon(pg);
    pg = NULL;

    CHECK(pager_open(PATH4, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_open(pg, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_SEALED);
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n);
    CHECK(sc.mismatch == 0);
    printf("    4c 重开后：目录项 SEALED ⇒ 完整的新分区（列式），%d 条一行不差\n",
           sc.i);

    ts_store_destroy(st);
    pager_close(pg);
    remove(PATH4);
}

/* ==================== 第 5 项：崩在 TTL 中间 ==================== */

static void t5_ttl_crash(void)
{
    Pager *pg = NULL;
    TsStore *st = NULL;
    FakeWal fw;
    TsWal w;
    Row expect[64];       /* 旧分区 + 新分区（时间序） */
    Row expect_new[64];   /* 只有新分区 */
    TsPartInfo pi;
    TsTtlStats stats;
    ScanCtx sc;
    const int n_old = 10;
    const int n_new = 5;
    const i64 t_old = 1000000LL;
    const i64 t_new = 1000000LL + 10LL * DAY_MS;
    const i64 now = t_new + 1000LL;
    const u64 ttl = 7u * 24u * 3600u;
    int i;
    int rc;

    printf("  5) 崩在 TTL 中间：目录说还在、数据就得在；目录说过期、页可以还占着\n");

    /* ---- 5a：崩在"改目录项"那一步 ---- */
    memset(&fw, 0, sizeof(fw));
    w.fn = fake_log;
    w.ctx = &fw;

    remove(PATH5);
    CHECK(pager_open(PATH5, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_create_wal(pg, 60, &w, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }

    for (i = 0; i < n_old; i++) {
        TsReading r;

        r.time = t_old + (int64_t)i;
        r.tag = (int64_t)i;
        r.value = (double)i * 0.5;
        expect[i].time = r.time;
        expect[i].tag = r.tag;
        expect[i].value = r.value;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    for (i = 0; i < n_new; i++) {
        TsReading r;

        r.time = t_new + (int64_t)i;
        r.tag = 1000 + (int64_t)i;
        r.value = 100.0 + (double)i;
        expect[n_old + i].time = r.time;
        expect[n_old + i].tag = r.tag;
        expect[n_old + i].value = r.value;
        expect_new[i].time = r.time;
        expect_new[i].tag = r.tag;
        expect_new[i].value = r.value;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(pager_flush_all(pg) == OK);

    /* 剧本：下一条日志就失败（= 崩在"改目录项"那一步） */
    fw.fail_at = fw.calls + 1u;
    memset(&stats, 0, sizeof(stats));
    rc = ts_ttl_expire(pg, (TsDir *)ts_store_dir(st), now, ttl, &stats);
    CHECK(rc != OK);
    printf("    5a 改目录项那一步写日志失败（返回 %d）\n", rc);

    ts_store_destroy(st);
    st = NULL;
    pager_abandon(pg);
    pg = NULL;

    /* 重开：目录项还是 ACTIVE ⇒ 过期分区必须一条不少 */
    CHECK(pager_open(PATH5, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_open(pg, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_ACTIVE);
    sc.expect = expect;
    sc.n = n_old + n_new;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n_old + n_new);
    CHECK(sc.mismatch == 0);
    printf("    5a 重开后：目录项 ACTIVE，%d 条一行不差（页也没被回收）\n", sc.i);

    ts_store_destroy(st);
    st = NULL;
    pager_close(pg);
    pg = NULL;

    /* ---- 5b：崩在"回收页"那一步 ---- */
    memset(&fw, 0, sizeof(fw));

    remove(PATH6);
    CHECK(pager_open(PATH6, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_create_wal(pg, 60, &w, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    for (i = 0; i < n_old; i++) {
        TsReading r;

        r.time = t_old + (int64_t)i;
        r.tag = (int64_t)i;
        r.value = (double)i * 0.5;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    for (i = 0; i < n_new; i++) {
        TsReading r;

        r.time = t_new + (int64_t)i;
        r.tag = 1000 + (int64_t)i;
        r.value = 100.0 + (double)i;
        CHECK(ts_store_insert(st, &r) == OK);
    }
    CHECK(pager_flush_all(pg) == OK);

    /* 剧本：清零页那一步失败（= 崩在"回收页"那一步） */
    fw.fail_on_zero = true;
    memset(&stats, 0, sizeof(stats));
    rc = ts_ttl_expire(pg, (TsDir *)ts_store_dir(st), now, ttl, &stats);
    CHECK(rc != OK);
    printf("    5b 清零页那一步写日志失败（返回 %d）\n", rc);

    ts_store_destroy(st);
    st = NULL;
    pager_abandon(pg);
    pg = NULL;

    /* 重开：目录项已经是 DROPPED（落盘了），页还占着（空间泄漏，安全方向） */
    CHECK(pager_open(PATH6, 64, &pg) == OK);
    if (!pg) {
        return;
    }
    CHECK(ts_store_open(pg, &st) == OK);
    if (!st) {
        pager_close(pg);
        return;
    }
    CHECK(ts_store_part_info(st, 0, &pi) == OK);
    CHECK(pi.state == TS_PART_DROPPED);
    CHECK(pi.first_page == PAGE_ID_NIL);
    CHECK(pi.page_count == 0u);
    sc.expect = expect_new;
    sc.n = n_new;
    sc.i = 0;
    sc.mismatch = 0;
    CHECK(ts_store_scan(st, scan_cb, &sc) == OK);
    CHECK(sc.i == n_new);
    CHECK(sc.mismatch == 0);
    printf("    5b 重开后：目录项 DROPPED，活着的分区 %d 条逐条相等\n", sc.i);

    ts_store_destroy(st);
    pager_close(pg);
    remove(PATH5);
    remove(PATH6);
}

/* ---------------- 入口 ---------------- */

int main(void)
{
    printf("test_ts_crash_mid:\n");

    t4_seal_crash();
    t5_ttl_crash();

    if (failures) {
        printf("  %d 项断言失败\n", failures);
        return 1;
    }
    printf("  2 项验收全部通过\n");
    return 0;
}
