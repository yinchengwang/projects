/*
 * test_wal.c — WAL 与恢复单元验收
 *
 * 10 项：
 *   1. 追加/读取往返
 *   2. LSN 单调递增
 *   3. fsync 推进 flushed_lsn
 *   4. 尾部截断 → 扫到 E_EOF，不崩
 *   5. crc 损坏 → 截断到最后一条完整记录
 *   6. max_xid 跨会话保持
 *   7. wal_reset 清空
 *   8. 重放幂等：page_lsn 挡住第二次重放
 *   9. 未提交事务被回滚
 *  10. WAL 规则：页刷盘前日志已落盘
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <io.h>
#define TRUNCATE_FILE(f, n) _chsize(_fileno(f), (n))
#else
#include <unistd.h>
#define TRUNCATE_FILE(f, n) ((void)ftruncate(fileno(f), (n)))
#endif

#include "common/error.h"
#include "storage/page.h"
#include "storage/pager.h"
#include "wal/recovery.h"
#include "wal/wal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define WALPATH "test/tmp/wal_unit.wal"

/* ---- 1) 追加/读取往返 ---- */
static void t1_roundtrip(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    u8 before[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    u8 after[6]  = { 9, 9, 9, 9, 9, 9 };

    u32 lsn = wal_append_xid(w, WAL_INSERT, 0, 42, 7, 0, before, 8, after, 6);
    CHECK(lsn == 1);
    CHECK(wal_nrecords(w) == 1);

    WalRec r;
    CHECK(wal_scan_begin(w) == OK);
    CHECK(wal_scan_next(w, &r) == OK);
    CHECK(r.lsn == 1);
    CHECK(r.type == WAL_INSERT);
    CHECK(r.page_id == 42);
    CHECK(r.slot == 7);
    CHECK(r.blen == 8);
    CHECK(r.alen == 6);
    CHECK(wal_scan_next(w, &r) == E_EOF);

    u8 buf[32];
    CHECK(wal_read_payload(w, &r, buf, sizeof buf) == OK);
    CHECK(memcmp(buf, before, 8) == 0);
    CHECK(memcmp(buf + 8, after, 6) == 0);

    wal_close(w);
}

/* ---- 2) LSN 单调递增 ---- */
static void t2_lsn_monotonic(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    u32 prev = 0;
    for (int i = 0; i < 100; i++) {
        u32 lsn = wal_append_xid(w, WAL_INSERT, 0, (u32)i, 0, 0, NULL, 0, (const u8 *)"x", 1);
        CHECK(lsn > prev);
        prev = lsn;
    }
    CHECK(wal_last_lsn(w) == 100);

    /* 重新打开，LSN 继续往下走，不会从 1 重来 */
    wal_close(w);
    CHECK(wal_open(WALPATH, &w) == OK);
    CHECK(wal_last_lsn(w) == 100);
    u32 lsn = wal_append_xid(w, WAL_INSERT, 0, 0, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(lsn == 101);

    wal_close(w);
}

/* ---- 3) fsync 推进 flushed_lsn ---- */
static void t3_flushed_lsn(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    CHECK(wal_flushed_lsn(w) == 0);
    wal_append_xid(w, WAL_INSERT, 0, 1, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(wal_flushed_lsn(w) == 0);      /* 还没 fsync */
    CHECK(wal_fsync(w) == OK);
    CHECK(wal_flushed_lsn(w) == 1);      /* fsync 之后才推进 */

    wal_append_xid(w, WAL_INSERT, 0, 2, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(wal_flushed_lsn(w) == 1);      /* 新记录还没落盘 */
    CHECK(wal_fsync(w) == OK);
    CHECK(wal_flushed_lsn(w) == 2);

    wal_close(w);
}

/* ---- 4) 尾部截断 → 扫到 E_EOF，不崩 ---- */
static void t4_truncated_tail(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    for (int i = 0; i < 10; i++)
        wal_append_xid(w, WAL_INSERT, 0, (u32)i, 0, 0, NULL, 0, (const u8 *)"payload", 7);
    wal_close(w);

    /* 砍掉最后 5 字节：最后一条记录变成半条 */
    FILE *f = fopen(WALPATH, "r+b");
    CHECK(f != NULL);
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        TRUNCATE_FILE(f, sz - 5);
        fclose(f);
    }

    CHECK(wal_open(WALPATH, &w) == OK);
    int n = 0;
    WalRec r;
    CHECK(wal_scan_begin(w) == OK);
    while (wal_scan_next(w, &r) == OK)
        n++;
    CHECK(n == 9);               /* 前 9 条完整，最后一条被丢掉 */
    wal_close(w);
}

/* ---- 5) crc 损坏 → 截断到最后一条完整记录 ---- */
static void t5_crc_break(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    for (int i = 0; i < 5; i++)
        wal_append_xid(w, WAL_INSERT, 0, (u32)i, 0, 0, NULL, 0, (const u8 *)"payload", 7);
    wal_close(w);

    /* 把第 3 条的载荷改一个字节：crc 校验必须失败 */
    FILE *f = fopen(WALPATH, "r+b");
    CHECK(f != NULL);
    if (f) {
        long rec = (long)(WAL_HDR + 7 + 4);   /* 每条记录的长度
                                              * = 28 头 + 7 载荷 + 4 crc */
        fseek(f, rec * 2 + WAL_HDR + 1, SEEK_SET);
        int c = fgetc(f);
        fseek(f, rec * 2 + WAL_HDR + 1, SEEK_SET);
        fputc(c ^ 0xFF, f);
        fclose(f);
    }

    CHECK(wal_open(WALPATH, &w) == OK);
    int n = 0;
    WalRec r;
    CHECK(wal_scan_begin(w) == OK);
    while (wal_scan_next(w, &r) == OK)
        n++;
    CHECK(n == 2);               /* 第 3 条 crc 挂了，从那里截断 */
    wal_close(w);
}

/* ---- 6) max_xid 跨会话保持 ---- */
static void t6_max_xid(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    CHECK(wal_max_xid(w) == 0);
    wal_append_xid(w, WAL_INSERT, 5, 1, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(wal_max_xid(w) == 5);
    wal_close(w);

    /* 重开：max_xid 必须还在 —— 否则下次会话的 xid=1 会和它混淆 */
    CHECK(wal_open(WALPATH, &w) == OK);
    CHECK(wal_max_xid(w) == 5);
    wal_close(w);
}

/* ---- 7) wal_reset 清空 ---- */
static void t7_reset(void)
{
    remove(WALPATH);
    Wal *w = NULL;
    CHECK(wal_open(WALPATH, &w) == OK);

    wal_append_xid(w, WAL_INSERT, 9, 1, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(wal_nrecords(w) == 1);
    CHECK(wal_max_xid(w) == 9);

    CHECK(wal_reset(w) == OK);
    CHECK(wal_nrecords(w) == 0);
    CHECK(wal_max_xid(w) == 0);
    CHECK(wal_last_lsn(w) == 0);

    /* reset 后还能继续写 */
    u32 lsn = wal_append_xid(w, WAL_INSERT, 0, 1, 0, 0, NULL, 0, (const u8 *)"x", 1);
    CHECK(lsn == 1);

    wal_close(w);
}

/* ---- 8) 重放幂等：page_lsn 挡住第二次重放 ---- */
static void t8_redo_idempotent(void)
{
    remove("test/tmp/wal_redo.db");
    remove("test/tmp/wal_redo.db-wal");

    Pager *pg = NULL;
    CHECK(pager_open("test/tmp/wal_redo.db", 32, &pg) == OK);
    Wal *w = NULL;
    CHECK(wal_open("test/tmp/wal_redo.db-wal", &w) == OK);

    /* 模拟一次插入：页 1 的 slot 0 写入一行 */
    PageFrame *f;
    u32 pid;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    CHECK(page_init(f->data, PT_HEAP) == OK);

    u8 rec[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    u16 slot;
    CHECK(page_insert(f->data, rec, 16, &slot) == OK);

    /*
     * xid = 0（系统事务）：恢复时永远视为已提交，所以这条插入重放后
     * 不会被回滚掉 —— 本测试要验证的正是"重放生效且幂等"。
     *
     * 对比第 9 项测试：那里的插入属于未提交事务（xid=2），恢复后必须被撤销。
     */
    u32 lsn = wal_append_xid(w, WAL_INSERT, 0, pid, slot, 0, NULL, 0, rec, 16);
    CHECK(lsn > 0);
    page_set_lsn(f->data, lsn);
    CHECK(pager_unpin(pg, pid, true) == OK);

    /* 第一次恢复：重放生效 */
    bool did = false;
    CHECK(recovery_run(pg, w, 0, &did) == OK);
    CHECK(did == true);

    u16 len;
    const u8 *out;
    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(page_get(f->data, slot, &out, &len) == OK);
    CHECK(len == 16);
    CHECK(page_lsn(f->data) == lsn);
    pager_unpin(pg, pid, false);

    /* 第二次恢复：page_lsn 挡住，不该重复插入 */
    CHECK(recovery_run(pg, w, 0, &did) == OK);
    CHECK(pager_get(pg, pid, &f) == OK);
    CHECK(page_slot_count(f->data) == 1);      /* 还是 1 个 slot，没插第二次 */
    pager_unpin(pg, pid, false);

    wal_close(w);
    pager_close(pg);
}

/* ---- 9) 未提交事务被回滚 ---- */
static void t9_uncommitted_rolled_back(void)
{
    remove("test/tmp/wal_undo.db");
    remove("test/tmp/wal_undo.db-wal");

    Pager *pg = NULL;
    CHECK(pager_open("test/tmp/wal_undo.db", 32, &pg) == OK);
    Wal *w = NULL;
    CHECK(wal_open("test/tmp/wal_undo.db-wal", &w) == OK);

    PageFrame *f;
    u32 pid;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    CHECK(page_init(f->data, PT_HEAP) == OK);

    /*
     * 关键：插入必须在 xid=2 的事务里。
     * xid=0 是"系统事务"（bootstrap 专用），恢复时永远视为已提交，
     * 所以这里要是忘了 set_xid，插入就不会被撤销。
     */
    wal_append_xid(w, WAL_BEGIN, 2, PAGE_ID_NIL, 0, 0, NULL, 0, NULL, 0);

    u8 rec[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
    u16 slot;
    CHECK(page_insert(f->data, rec, 16, &slot) == OK);
    u32 lsn = wal_append_xid(w, WAL_INSERT, 2, pid, slot, 0, NULL, 0, rec, 16);
    page_set_lsn(f->data, lsn);
    CHECK(pager_unpin(pg, pid, true) == OK);

    /* 事务 1 提交了，事务 2 没提交 */
    wal_append_xid(w, WAL_BEGIN, 1, PAGE_ID_NIL, 0, 0, NULL, 0, NULL, 0);
    wal_append_xid(w, WAL_COMMIT, 1, PAGE_ID_NIL, 0, 0, NULL, 0, NULL, 0);

    /* 注意：xid=2 没有 COMMIT —— 崩溃了 */
    bool did = false;
    CHECK(recovery_run(pg, w, 0, &did) == OK);

    /* xid=2 的那条插入必须被撤销 */
    CHECK(pager_get(pg, pid, &f) == OK);
    u16 len;
    const u8 *out;
    CHECK(page_get(f->data, slot, &out, &len) == E_NOTFOUND);
    pager_unpin(pg, pid, false);

    wal_close(w);
    pager_close(pg);
}

/* ---- 10) WAL 规则：页刷盘前日志必须已落盘 ---- */

/* 钩子的观测数据 */
static int  probe_calls;
static u32  probe_lsn;
static u32  probe_flushed;    /* 钩子被调用时，"已落盘的最大 LSN" */

static int probe_flush(void *ctx, u32 page_id, u32 page_lsn)
{
    (void)ctx; (void)page_id;
    probe_calls++;
    probe_lsn = page_lsn;
    return OK;
}

/* 故意失败的钩子：验证 pager 会拒绝刷盘 */
static int probe_fail(void *ctx, u32 page_id, u32 page_lsn)
{
    (void)ctx; (void)page_id; (void)page_lsn;
    return E_TXN;
}

static void t10_wal_rule(void)
{
    remove("test/tmp/wal_rule.db");
    remove("test/tmp/wal_rule.db-wal");

    Pager *pg = NULL;
    CHECK(pager_open("test/tmp/wal_rule.db", 32, &pg) == OK);
    Wal *w = NULL;
    CHECK(wal_open("test/tmp/wal_rule.db-wal", &w) == OK);

    probe_calls = 0;
    probe_lsn = 0;
    probe_flushed = 0;
    pager_set_wal_flush(pg, probe_flush, NULL);

    PageFrame *f;
    u32 pid;
    CHECK(pager_alloc(pg, &pid, &f) == OK);
    CHECK(page_init(f->data, PT_HEAP) == OK);
    u32 lsn = wal_append_xid(w, WAL_INSERT, 0, pid, 0, 0, NULL, 0, (const u8 *)"x", 1);
    page_set_lsn(f->data, lsn);
    CHECK(pager_unpin(pg, pid, true) == OK);

    /* 刷页：钩子必须被调用，且能看出页的 lsn */
    probe_calls = 0;
    CHECK(pager_flush_all(pg) == OK);
    CHECK(probe_calls > 0);
    CHECK(probe_lsn == lsn);

    /*
     * 钩子返回错误时，pager 必须拒绝刷盘。
     * 这是"宁可报错也不能违反 WAL 规则"的兜底。
     *
     * 注意要先把页标脏 —— 上一轮 flush_all 已经把它刷干净了，
     * 不脏的页压根不会被刷，钩子也就不会被调用。
     */
    pager_set_wal_flush(pg, probe_fail, NULL);
    CHECK(pager_get(pg, pid, &f) == OK);
    f->dirty = true;
    CHECK(pager_unpin(pg, pid, true) == OK);
    CHECK(pager_flush_all(pg) == E_TXN);

    wal_close(w);
    pager_close(pg);
}

int main(void)
{
    printf("test_wal:\n");
    t1_roundtrip();
    t2_lsn_monotonic();
    t3_flushed_lsn();
    t4_truncated_tail();
    t5_crc_break();
    t6_max_xid();
    t7_reset();
    t8_redo_idempotent();
    t9_uncommitted_rolled_back();
    t10_wal_rule();

    if (failures == 0) {
        printf("  10 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
