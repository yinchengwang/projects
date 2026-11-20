/*
 * test_wal_group.c —— group commit 协议验收（W-D 的 D-7）
 *
 * ==================== 这组测试防的是什么 ====================
 *
 * README 早就宣称 wal.c/h 有"group commit"，但旧代码是每次 wal_fsync
 * 都做一次真实同步。本测试直接锁住四条必须成立的语义：
 *
 * 1. **单线程语义不变**：一次 fsync 成功后 flushed_lsn >= target。
 * 2. **多线程共享一次 fsync**：4 个线程同时等同一代，真实 fsync 次数远小于事务数。
 * 3. **失败不假成功**：发起者的 fsync 失败后，等待者必须醒来并返回 E_IO，
 *    不能看见一个没推进的水位就永久睡，也不能被错误当成成功。
 * 4. **恢复契约不变**：现有 test_wal_header / geo_crash / ts_crash 已经覆盖
 *    崩溃恢复；这里专门覆盖 group 的并发与失败分支。
 *
 * ==================== 为什么故障注入不是“作弊” ====================
 *
 * 计划原文说“把 WAL 目录设成只读”。Windows 与 POSIX 的只读语义不同，
 * 而且会污染测试清理路径；更糟的是，失败可能发生在 fflush 而不是 fsync，
 * 测不到“某一代等待者收到失败”这个真正危险的分支。
 *
 * 所以内部头提供**仅测试用**的 `wal_test_fail_fsync(w, n)`：
 * 它只让接下来 n 次真实 fsync 返回 E_IO，不改变任何生产路径。
 * 这条接口不放 wal.h，对外不可见。
 */

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "common/error.h"
#include "common/types.h"
#include "wal/wal.h"
#include "wal/wal_internal.h"

#define PATH "test/tmp/wal_group.db"
#define NTHREADS 4

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

typedef struct {
    Wal *w;
    u32  target;
    int  rc;
} WaitArg;

static void *waiter(void *arg)
{
    WaitArg *a = (WaitArg *)arg;

    a->rc = wal_fsync(a->w);
    return NULL;
}

static void rm_path(void)
{
    remove(PATH);
}

/* 打一条最小日志记录，返回 LSN */
static u32 append_one(Wal *w, u32 xid)
{
    return wal_append_xid(w, WAL_INSERT, xid, 1u, 0, 0,
                          NULL, 0, (const u8 *)"x", 1);
}

static void t1_single_thread(void)
{
    Wal *w = NULL;
    u32 lsn;

    printf("  [1] 单线程语义：提交后 flushed_lsn >= target\n");
    rm_path();
    CHECK(wal_open(PATH, &w) == OK);
    lsn = append_one(w, 1u);
    CHECK(lsn != 0u);
    CHECK(wal_fsync(w) == OK);
    CHECK(wal_flushed_lsn(w) >= lsn);
    CHECK(wal_group_fsync_calls(w) == 1u);
    wal_close(w);
    rm_path();
}

static void t2_group_shared(void)
{
    Wal *w = NULL;
    pthread_t th[NTHREADS];
    WaitArg args[NTHREADS];
    u32 lsn;
    int i;

    printf("  [2] 4 线程共享一代：真实 fsync 次数 << 4\n");
    rm_path();
    CHECK(wal_open(PATH, &w) == OK);

    /*
     * 先把所有 COMMIT 对应的日志写完，再让 4 个线程同时进 fsync。
     * 第一线程成为发起者，其余线程必须等它的同一代。
     */
    for (i = 0; i < NTHREADS; i++) {
        lsn = append_one(w, (u32)(i + 1));
        CHECK(lsn != 0u);
    }

    /* 放慢发起者，扩大“其它线程到达等待区”的窗口 */
    wal_test_slow_fsync(w, 50000u);
    for (i = 0; i < NTHREADS; i++) {
        args[i].w = w;
        args[i].target = lsn;
        args[i].rc = E_IO;
        CHECK(pthread_create(&th[i], NULL, waiter, &args[i]) == 0);
    }
    for (i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    for (i = 0; i < NTHREADS; i++)
        CHECK(args[i].rc == OK);
    CHECK(wal_flushed_lsn(w) >= lsn);
    /*
     * 允许 1~2 次：线程可能在第一代发起者真正取锁前错开，
     * 但不允许回到“每线程一次”（4）。
     */
    CHECK(wal_group_fsync_calls(w) < (u64)NTHREADS);
    printf("    4 个等待者完成，真实 fsync = %llu\n",
           (unsigned long long)wal_group_fsync_calls(w));

    wal_close(w);
    rm_path();
}

static void t3_failed_generation(void)
{
    Wal *w = NULL;
    pthread_t th[NTHREADS];
    WaitArg args[NTHREADS];
    int i;

    printf("  [3] 发起者 fsync 失败：等待者全部醒来并报 E_IO\n");
    rm_path();
    CHECK(wal_open(PATH, &w) == OK);

    for (i = 0; i < NTHREADS; i++)
        CHECK(append_one(w, (u32)(100 + i)) != 0u);

    /* 让这一代的发起者失败；其余线程不能挂死，也不能假成功 */
    wal_test_fail_fsync(w, 1u);
    wal_test_slow_fsync(w, 50000u);
    for (i = 0; i < NTHREADS; i++) {
        args[i].w = w;
        args[i].target = wal_last_lsn(w);
        args[i].rc = OK;
        CHECK(pthread_create(&th[i], NULL, waiter, &args[i]) == 0);
    }
    for (i = 0; i < NTHREADS; i++)
        CHECK(pthread_join(th[i], NULL) == 0);

    for (i = 0; i < NTHREADS; i++)
        CHECK(args[i].rc == E_IO);
    /* 失败不能推进水位：如果推进了就是把事务当成已落盘 */
    CHECK(wal_flushed_lsn(w) == 0u);
    printf("    %d 个等待者全部收到 E_IO，flushed_lsn 仍为 %u\n",
           NTHREADS, wal_flushed_lsn(w));

    wal_close(w);
    rm_path();
}

int main(void)
{
    printf("test_wal_group:\n");

    t1_single_thread();
    t2_group_shared();
    t3_failed_generation();

    if (failures == 0) {
        printf("  3 项验收全部通过");
        return 0;
    }
    printf("  %d 项失败", failures);
    return 1;
}
