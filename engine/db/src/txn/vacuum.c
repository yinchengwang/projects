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
 *   4. **页内 compact**（v1.2 步骤 5）：把删除造出来的空洞压掉
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

    /*
     * ---- 第三遍：页内 compact（v1.2 步骤 5）----
     *
     * ==================== 为什么必须排在删除之后 ====================
     *
     * 页内空洞是**删除**造出来的 —— 不删就没有空洞可整理。
     * 所以整理必须排在删除之后，而不是"扫描时顺手"：
     * 扫描的时候那些死版本还在页里，整理它们等于白整理。
     *
     * ==================== 阈值为什么是 0 ====================
     *
     * 计划（PLAN-v1.2 §1.4）写的是"碎片率 > 50% 的页进入 compact 队列"。
     * 那是**插入路径**的取舍标准：插入时顺手整理一下，
     * 碎片率不到一半就不值得为它写一条整页日志。
     *
     * 但 VACUUM 是**显式的维护操作** —— 用户就是要"把表收拾干净"，
     * 而且验收 A5 要求"全表碎片率 < 20%"：
     * 逐页 50% 的阈值**保证不了**这个结果
     * （一堆 40% 碎片的页摊在一起，全表就是 40%）。
     *
     * 所以这里的阈值取 0：**只要有空洞就整理**。
     * 代价是每个整理过的页写一条 8KB 的日志记录
     * （before + after 两份整页镜像）—— WAL 增长换空间回收，
     * 这个取舍交给步骤 6 的 checkpoint 去兜（日志会被截断）。
     */
    if (rc == OK) {
        HeapCompactStats cst;
        rc = heap_compact_table(h, xid, 0, &cst);
        if (rc == OK) {
            local.compacted_pages = cst.pages_compacted;
            local.freed_bytes = cst.bytes_freed;
            local.frag_pct = heap_compact_frag_pct(&cst);
        }
    }

    if (stats)
        *stats = local;
    return (rc == E_EOF) ? OK : rc;
}
