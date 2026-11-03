/* lockstat.c —— 一次性诊断用：锁等待时间的统计与打印 */
#include "common/lockstat.h"

#ifdef DB_LOCKSTAT

#include <stdio.h>

atomic_ullong g_ls_ns[LS_N];
atomic_ullong g_ls_cnt[LS_N];
atomic_ullong g_ls_cont[LS_N];
atomic_ullong g_ls_recur[LS_N];
atomic_ullong g_ls_page_cont[4096];
atomic_ullong g_ls_tbl_cont[64];

static const char *names[LS_N] = {
    "池级锁(pool)", "catalog 域锁", "帧内容锁", "WAL 锁", "txn 域锁", "freelist 锁"
};

void lockstat_dump(void)
{
    fprintf(stderr, "\n=== 锁等待统计 ===\n");
    for (int i = 0; i < LS_N; i++) {
        unsigned long long ns = atomic_load(&g_ls_ns[i]);
        unsigned long long cnt = atomic_load(&g_ls_cnt[i]);
        unsigned long long cont = atomic_load(&g_ls_cont[i]);
        unsigned long long recur = atomic_load(&g_ls_recur[i]);
        fprintf(stderr,
                "  %-16s : 总等待 %8.1f ms / %8llu 次 = %6.2f us/次"
                "（真争用 %llu 次，递归 %llu 次）\n",
                names[i], (double)ns / 1e6, cnt,
                cnt ? (double)ns / (double)cnt / 1000.0 : 0.0, cont, recur);
    }

    fprintf(stderr, "  --- 争用最凶的页（页号 → 争用次数）---\n");
    for (int top = 0; top < 8; top++) {
        int best = -1;
        unsigned long long bv = 0;
        for (int i = 0; i < 4096; i++) {
            unsigned long long v = atomic_load(&g_ls_page_cont[i]);
            if (v > bv) { bv = v; best = i; }
        }
        if (best < 0)
            break;
        fprintf(stderr, "    页 %4d : %llu 次\n", best, bv);
        atomic_store(&g_ls_page_cont[best], 0);
    }

    fprintf(stderr, "  --- 争用最凶的表（table_id → 争用次数）---\n");
    for (int top = 0; top < 8; top++) {
        int best = -1;
        unsigned long long bv = 0;
        for (int i = 0; i < 64; i++) {
            unsigned long long v = atomic_load(&g_ls_tbl_cont[i]);
            if (v > bv) { bv = v; best = i; }
        }
        if (best < 0)
            break;
        fprintf(stderr, "    表 %4d : %llu 次\n", best, bv);
        atomic_store(&g_ls_tbl_cont[best], 0);
    }
}

#endif /* DB_LOCKSTAT */
