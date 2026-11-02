#include "txn/vacuum.h"

#include <stdlib.h>
#include <string.h>

#include "storage/record.h"
#include "txn/mvcc.h"

/*
 * vacuum 实现。
 *
 * 流程：
 *   1. 扫全表的所有版本
 *   2. 对每个版本判断是不是死版本
 *   3. 是死版本 → heap_delete（写日志 + 置空 slot）
 *
 * 【为什么必须先收集再删除】
 *
 * 边扫边删会打乱扫描器（heap_scan_next 依赖 slot 计数和页号递增）。
 * 更危险的是：删掉当前 slot 后，扫描器继续往下走是安全的，
 * 但如果同时有别的页在变动（比如新版本插入），就可能出现
 * "同一个 slot 被扫两次"或"漏扫"。
 *
 * 所以分两遍：第一遍收集 RID，第二遍删除。
 *
 * 【为什么删除要走 heap_delete 而不是直接改页】
 *
 * heap_delete 会写 WAL 日志（带 before 镜像）。
 * 如果直接改页不写日志，崩溃后重放会把这个删除"撤销"——
 * 数据又回来了。而用户明明已经提交了删除。
 * 这是静默的数据不一致。
 */

int vacuum_table(Heap *h, TxnTable *txn, u32 xid, VacuumStats *stats)
{
    if (!h || !txn)
        return E_INVAL;

    VacuumStats local;
    memset(&local, 0, sizeof(local));

    /* ---- 第一遍：收集死版本的 RID ---- */

    HeapScan *scan = NULL;
    int rc = heap_scan_begin(h, &scan);
    if (rc != OK)
        return rc;

    Rid *dead = NULL;
    int n_dead = 0, cap = 0;

    u8 recbuf[PAGE_SIZE];
    u16 reclen;
    Rid rid;

    while ((rc = heap_scan_next(scan, &rid, recbuf, sizeof(recbuf), &reclen)) == OK) {
        local.scanned++;

        /*
         * 判断死版本时排除执行者自己（xid）——
         * 否则 vacuum 自己的活跃事务会让判定永远为 false。
         */
        if (!mvcc_is_dead_except(txn, xid, recbuf, reclen)) {
            local.kept++;
            continue;
        }

        if (n_dead >= cap) {
            int ncap = cap ? cap * 2 : 64;
            Rid *nd = realloc(dead, (size_t)ncap * sizeof(Rid));
            if (!nd) {
                rc = E_IO;
                break;
            }
            dead = nd;
            cap = ncap;
        }
        dead[n_dead++] = rid;
    }
    heap_scan_end(scan);

    if (rc != OK && rc != E_EOF) {
        free(dead);
        return rc;
    }

    /* ---- 第二遍：真正删除 ---- */

    for (int i = 0; i < n_dead; i++) {
        rc = heap_delete(h, xid, dead[i]);
        if (rc != OK)
            break;
        local.removed++;
    }

    free(dead);

    if (stats)
        *stats = local;
    return (rc == E_EOF) ? OK : rc;
}
