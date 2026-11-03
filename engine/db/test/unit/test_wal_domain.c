/*
 * test_wal_domain.c —— v1.2 步骤 4（子步骤 4b）：wal 域的并发保护
 *
 * 4 项：
 *   1. **LSN 唯一且连续**：8 线程并发 append → 拿到的 LSN 两两不同，
 *      且合起来正好是 1..N（既不重复，也不断号）
 *   2. **日志文件本身完整**：并发写出来的日志，一条不差地扫回来
 *      （扫不回来 = 文件里有撕裂的记录 = 恢复时会截断 = 后面全丢）
 *   3. **append 与 fsync 混跑**：读者看到的计数器单调不减、不崩
 *   4. **undo 链按 xid 独立**：同一 xid 串成链，不同 xid 互不串
 *
 * ==================== 第 1 项在抓什么 ====================
 *
 * wal_append_xid 里"取号 → 定位文件尾 → 写记录"三步必须是一个整体。
 * 两个线程同时进来：
 *
 *   线程 A 取到 lsn=100，线程 B 也取到 lsn=100  ← 两条记录同号
 *
 * LSN 是恢复阶段的**排序依据**（page_lsn、undo 链都靠它）。
 * 同号的两条记录，恢复时谁先谁后是未定义的 ——
 * 最坏情况是"已提交的改动被当成未提交回滚掉"。
 *
 * ==================== 第 2 项在抓什么 ====================
 *
 * 这一项比第 1 项更致命，因为它抓的是**静默的数据丢失**：
 *
 *   两个线程各自 fseek 到文件尾、各自 fwrite →
 *   字节交错 → 文件里出现半条 A、半条 B →
 *   CRC 校验失败 → 恢复阶段**在这里截断** →
 *   截断点之后的**所有**记录都作废。
 *
 * 也就是说：一次并发写日志的竞态，能把整段日志废掉。
 * 而且没有报错、没有崩溃 —— 只是"数据不见了"。
 *
 * 所以第 2 项不是"顺便看一眼"，它是这一域的**头号验收项**。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wal/wal.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

#define WPATH   "test/tmp/wal_domain.wal"
#define WPATH2  "test/tmp/wal_domain2.wal"

#define NTHREAD 8
#define NROUNDS 300          /* 8×300 = 2400 条记录 */

/* ---------------- 公共：并发写 ---------------- */

typedef struct {
    Wal *w;
    int  id;
    u32  lsns[NROUNDS];
    int  bad;               /* append 失败次数 */
} Worker;

static void *wal_worker(void *arg)
{
    Worker *wk = (Worker *)arg;
    u32 xid = (u32)(wk->id + 1);      /* 每个线程一个 xid */

    for (int i = 0; i < NROUNDS; i++) {
        u8 payload[8];
        memset(payload, wk->id + 1, sizeof(payload));

        u32 lsn = wal_append_xid(wk->w, WAL_INSERT, xid, (u32)(i + 1),
                                 (u16)i, 0, NULL, 0, payload, sizeof(payload));
        if (!lsn) {
            wk->bad++;
            continue;
        }
        wk->lsns[i] = lsn;
    }
    return NULL;
}

/* ---------------- 1) LSN 唯一且连续 ---------------- */

static int cmp_u32(const void *a, const void *b)
{
    u32 x = *(const u32 *)a, y = *(const u32 *)b;
    return (x < y) ? -1 : (x > y) ? 1 : 0;
}

static void t1_lsn_unique_and_contiguous(void)
{
    remove(WPATH);

    Wal *w = NULL;
    CHECK(wal_open(WPATH, &w) == OK);
    if (!w)
        return;

    pthread_t th[NTHREAD];
    Worker wk[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        wk[i].w = w;
        wk[i].id = i;
        wk[i].bad = 0;
        memset(wk[i].lsns, 0, sizeof(wk[i].lsns));
        CHECK(pthread_create(&th[i], NULL, wal_worker, &wk[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++) {
        pthread_join(th[i], NULL);
        CHECK(wk[i].bad == 0);
    }

    int total = NTHREAD * NROUNDS;
    u32 *all = calloc((size_t)total, sizeof(u32));
    CHECK(all != NULL);
    if (!all) {
        wal_close(w);
        return;
    }

    int n = 0;
    for (int i = 0; i < NTHREAD; i++)
        for (int j = 0; j < NROUNDS; j++)
            all[n++] = wk[i].lsns[j];

    qsort(all, (size_t)total, sizeof(u32), cmp_u32);

    int dup = 0, gap = 0;
    for (int i = 0; i < total; i++) {
        if (i > 0 && all[i] == all[i - 1])
            dup++;                       /* 同号 */
        if (all[i] != (u32)(i + 1))
            gap++;                       /* 断号或跳号 */
    }

    CHECK(dup == 0);
    CHECK(gap == 0);
    CHECK(wal_nrecords(w) == (u32)total);
    CHECK(wal_last_lsn(w) == (u32)total);

    printf("    %d 条并发记录，LSN 重复 %d 个，断号 %d 个\n", total, dup, gap);

    free(all);
    wal_close(w);
    remove(WPATH);
}

/* ---------------- 2) 日志文件本身完整 ---------------- */

/*
 * 把并发写出来的日志整个扫回来。
 *
 * wal_scan_next 内部会校验 CRC —— 校验失败就返回 E_EOF，
 * 也就是"日志到这里为止"。所以：
 *   扫到的条数 == 写进去的条数  ⇒  文件里没有撕裂的记录
 */
static void t2_log_file_intact(void)
{
    remove(WPATH);

    Wal *w = NULL;
    CHECK(wal_open(WPATH, &w) == OK);
    if (!w)
        return;

    pthread_t th[NTHREAD];
    Worker wk[NTHREAD];

    for (int i = 0; i < NTHREAD; i++) {
        wk[i].w = w;
        wk[i].id = i;
        wk[i].bad = 0;
        memset(wk[i].lsns, 0, sizeof(wk[i].lsns));
        CHECK(pthread_create(&th[i], NULL, wal_worker, &wk[i]) == 0);
    }
    for (int i = 0; i < NTHREAD; i++)
        pthread_join(th[i], NULL);

    int total = NTHREAD * NROUNDS;

    /* 扫一遍：条数、LSN 递增、xid 分布 */
    WalRec r;
    int scanned = 0, bad_order = 0, bad_xid = 0;
    int xid_count[NTHREAD];

    memset(xid_count, 0, sizeof(xid_count));

    CHECK(wal_scan_begin(w) == OK);
    while (wal_scan_next(w, &r) == OK) {
        if (r.lsn != (u32)(scanned + 1))
            bad_order++;
        if (r.xid >= 1 && r.xid <= NTHREAD)
            xid_count[r.xid - 1]++;
        else
            bad_xid++;
        scanned++;
    }
    wal_scan_end(w);

    CHECK(scanned == total);
    CHECK(bad_order == 0);
    CHECK(bad_xid == 0);

    int bad_dist = 0;
    for (int i = 0; i < NTHREAD; i++)
        if (xid_count[i] != NROUNDS)
            bad_dist++;

    CHECK(bad_dist == 0);
    printf("    扫回 %d/%d 条，LSN 乱序 %d 条，xid 分布错 %d 个\n",
           scanned, total, bad_order, bad_dist);

    wal_close(w);
    remove(WPATH);
}

/* ---------------- 3) append 与 fsync 混跑 ---------------- */

/*
 * 写线程：append 若干条，顺手 fsync 一次。
 * 读线程：不停读 last_lsn / nrecords / flushed_lsn。
 *
 * 读路径是**无锁原子读**，所以它必须保证：
 *   - 不会读到撕裂的值（32 位对齐的原子读，不会）
 *   - 计数只会**变大**，不会倒退
 *     （倒退意味着读到了"旧值"覆盖"新值" —— 内存序错了）
 */
static volatile int g_stop = 0;
static volatile int g_regress = 0;
static volatile int g_reader_bad = 0;

static void *writer_thread(void *arg)
{
    Wal *w = (Wal *)arg;

    for (int i = 0; i < 2000; i++) {
        u8 payload[4] = {1, 2, 3, 4};
        u32 lsn = wal_append_xid(w, WAL_INSERT, 1, (u32)i, 0, 0,
                                 NULL, 0, payload, sizeof(payload));
        if (!lsn) {
            g_reader_bad++;
            break;
        }
        if ((i % 64) == 0)
            wal_fsync(w);
    }
    return NULL;
}

static void *reader_thread(void *arg)
{
    Wal *w = (Wal *)arg;

    u32 last_lsn = 0, last_n = 0, last_flushed = 0;

    while (!g_stop) {
        u32 l = wal_last_lsn(w);
        u32 n = wal_nrecords(w);
        u32 f = wal_flushed_lsn(w);

        if (l < last_lsn || n < last_n || f < last_flushed)
            g_regress = 1;

        last_lsn = l;
        last_n = n;
        last_flushed = f;
    }
    return NULL;
}

static void t3_append_fsync_mixed(void)
{
    remove(WPATH2);

    Wal *w = NULL;
    CHECK(wal_open(WPATH2, &w) == OK);
    if (!w)
        return;

    g_stop = 0;
    g_regress = 0;
    g_reader_bad = 0;

    pthread_t tw, tr;
    CHECK(pthread_create(&tw, NULL, writer_thread, w) == 0);
    CHECK(pthread_create(&tr, NULL, reader_thread, w) == 0);

    pthread_join(tw, NULL);
    g_stop = 1;
    pthread_join(tr, NULL);

    CHECK(g_regress == 0);
    CHECK(g_reader_bad == 0);
    CHECK(wal_nrecords(w) == 2000);

    /* 显式 fsync 之后 flushed_lsn 必须追平 last_lsn */
    CHECK(wal_fsync(w) == OK);
    CHECK(wal_flushed_lsn(w) == wal_last_lsn(w));

    printf("    2000 条 append 混跑 fsync，计数器倒退 %d 次，append 失败 %d 次\n",
           g_regress, g_reader_bad);

    wal_close(w);
    remove(WPATH2);
}

/* ---------------- 4) undo 链按 xid 独立 ---------------- */

/*
 * undo 链（prev_lsn）是"同一事务的上一条记录"。
 * 它按 xid 分槽维护 —— 如果两个事务共用一个槽，
 * 回滚时会把这个事务的记录也回滚掉。
 */
static void t4_undo_chain_per_xid(void)
{
    remove(WPATH);

    Wal *w = NULL;
    CHECK(wal_open(WPATH, &w) == OK);
    if (!w)
        return;

    u32 l1 = wal_append_xid(w, WAL_INSERT, 7, 1, 0, 0, NULL, 0, NULL, 0);
    u32 l2 = wal_append_xid(w, WAL_INSERT, 7, 2, 0, 0, NULL, 0, NULL, 0);
    u32 l3 = wal_append_xid(w, WAL_INSERT, 7, 3, 0, 0, NULL, 0, NULL, 0);

    /* 另一个 xid：它的第一条必须自成一条链 */
    u32 o1 = wal_append_xid(w, WAL_INSERT, 8, 4, 0, 0, NULL, 0, NULL, 0);
    u32 o2 = wal_append_xid(w, WAL_INSERT, 8, 5, 0, 0, NULL, 0, NULL, 0);

    CHECK(l1 && l2 && l3 && o1 && o2);

    WalRec r;
    int n = 0;
    u32 prev_of[8];
    u32 xid_of[8];

    CHECK(wal_scan_begin(w) == OK);
    while (wal_scan_next(w, &r) == OK && n < 8) {
        prev_of[n] = r.prev_lsn;
        xid_of[n] = r.xid;
        n++;
    }
    wal_scan_end(w);

    CHECK(n == 5);

    /* xid=7 的三条：链是 l1 → l2 → l3 */
    if (n == 5) {
        CHECK(xid_of[0] == 7 && xid_of[1] == 7 && xid_of[2] == 7);
        CHECK(prev_of[0] == 0);
        CHECK(prev_of[1] == l1);
        CHECK(prev_of[2] == l2);

        /* xid=8 的两条：链自成一条，不接 xid=7 */
        CHECK(xid_of[3] == 8 && xid_of[4] == 8);
        CHECK(prev_of[3] == 0);
        CHECK(prev_of[4] == o1);
    }

    printf("    undo 链：xid=7 三条成链，xid=8 自成一条\n");

    wal_close(w);
    remove(WPATH);
}

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);

    printf("test_wal_domain（单元）:\n");

    printf("  1. 并发 append：LSN 唯一且连续\n");
    t1_lsn_unique_and_contiguous();

    printf("  2. 并发写出的日志文件完整（能一条不差地扫回来）\n");
    t2_log_file_intact();

    printf("  3. append 与 fsync 混跑：读路径单调不减\n");
    t3_append_fsync_mixed();

    printf("  4. undo 链按 xid 独立\n");
    t4_undo_chain_per_xid();

    if (failures == 0) {
        printf("  4 项验收全部通过\n");
        return 0;
    }
    printf("  %d 项断言失败\n", failures);
    return 1;
}
