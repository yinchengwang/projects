/*
 * lockstat.h —— 诊断用：统计各处锁的**等待时间**与**真争用次数**
 *
 * 只在 DB_LOCKSTAT 打开时生效（否则全是空实现，**零开销**）。
 *
 * 【为什么留着它】
 * 2026-11-03 的 A3 收口（PLAN-v1.3 §3 步骤 0）就是靠它定位到
 * 「页内容锁真实争用 12,713 次」的 —— 那个结论**光看代码看不出来**，
 * 只能靠实测。留着它，下一个人排查并发问题时才有同一把尺子。
 *
 * 用法：
 *   sh bench/probe_lockstat.sh        # 另编一份带插桩的基准
 *   ./build/latch_bench_stat.exe 2000 200 4
 */
#ifndef DB_LOCKSTAT_H
#define DB_LOCKSTAT_H

#include <stdatomic.h>
#include <time.h>

#ifdef DB_LOCKSTAT

enum {
    LS_POOL = 0,      /* 缓冲池池级锁 */
    LS_CAT,           /* catalog 域锁 */
    LS_FRAME,         /* 每帧的内容锁 */
    LS_WAL,           /* WAL 锁 */
    LS_TXN,           /* txn 域锁 */
    LS_FREE,          /* 空闲页链表锁 */
    LS_N
};

extern atomic_ullong g_ls_ns[LS_N];
extern atomic_ullong g_ls_cnt[LS_N];
extern atomic_ullong g_ls_cont[LS_N];    /* 真争用：锁被别的线程占着 */
extern atomic_ullong g_ls_recur[LS_N];   /* 递归重入：本线程自己又拿了一遍 */
extern atomic_ullong g_ls_page_cont[4096];  /* 按 page_id 统计争用（诊断用） */
extern atomic_ullong g_ls_tbl_cont[64];     /* 按 table_id 统计争用（诊断用） */

static inline unsigned long long ls_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long long)ts.tv_sec * 1000000000ull + (unsigned long long)ts.tv_nsec;
}

#define LS_ENTER(id) unsigned long long ls__t0 = ls_now_ns()
#define LS_LEAVE(id)                                                      \
    do {                                                                 \
        atomic_fetch_add(&g_ls_ns[(id)], ls_now_ns() - ls__t0);          \
        atomic_fetch_add(&g_ls_cnt[(id)], 1ull);                          \
    } while (0)

void lockstat_dump(void);

#else

#define LS_ENTER(id) ((void)0)
#define LS_LEAVE(id) ((void)0)

#endif /* DB_LOCKSTAT */

#endif /* DB_LOCKSTAT_H */
