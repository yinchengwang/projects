#include "txn/vacuum.h"

#include <stdlib.h>
#include <string.h>

#include "common/json_ovf_lock.h"
#include "storage/record.h"
#include "txn/mvcc.h"

/*
 * vacuum 实现。
 *
 * 流程：
 *   1. 扫全表的所有版本
 *   2. 对每个版本判断是不是死版本
 *   3. 是死版本 → （有 JSON 溢出链就先回收链）→ heap_delete（写日志 + 置空 slot）
 *   4. **页内 compact**（v1.2 步骤 5）：把删除造出来的空洞压掉
 *
 * 【为什么必须先收集再删除】
 *
 * 边扫边删会打乱扫描器（heap_scan_next 依赖 slot 计数和页号递增）。
 * 更危险的是：删掉当前 slot 后，扫描器继续往下走是安全的，
 * 但如果同时有别的页在变动（比如新版本插入），就可能出现
 * "同一个 slot 被扫两次"或"漏扫"。
 *
 * 所以分两遍：第一遍收集 RID（+ 溢出链首页），第二遍删除。
 *
 * 【为什么删除要走 heap_delete 而不是直接改页】
 *
 * heap_delete 会写 WAL 日志（带 before 镜像）。
 * 如果直接改页不写日志，崩溃后重放会把这个删除"撤销"——
 * 数据又回来了。而用户明明已经提交了删除。
 * 这是静默的数据不一致。
 *
 * 【C5.3b-b Task 5：JSON 溢出页链的回收顺序】
 *
 * 设计 §7.1 定的顺序是"先读标记 → 再删 slot → 最后释放链"，
 * 而实现上是"先读标记（第一遍）→ 释放链 → 删 slot（第二遍）"。
 * 这个偏差是**有意的**，理由见下面的详细说明。
 */

/* 一条死版本最多带几条 JSON 溢出链 */
#define VACUUM_MAX_JSON_HEADS 8

/* 收集阶段的死版本条目 */
typedef struct {
    Rid  rid;
    u32  nheads;
    u64  heads[VACUUM_MAX_JSON_HEADS];
} DeadVer;

/*
 * 第二遍：回收一条死版本持有的 JSON 链。
 *
 * ==================== 顺序为什么是"先还链、后删 slot" ====================
 *
 * 设计 §7.1 写的是"先读标记、再删 slot、最后释放链"。按字面实现有一个
 * 致命窗口：**slot 删掉之后这条 tuple 就读不出来了**，链首页随之丢失
 * ⇒ 链变成孤儿（要靠下次开库的启动扫描才回收，见 Task 6）。
 *
 * 更要紧的是幂等：如果先把 slot 删了、崩在还链之前，重跑时这条 tuple
 * 已经不在了 —— 那条链就只能等启动扫描。虽然 Task 6 会兜住，但
 * VACUUM 自己能把账结掉显然更好。
 *
 * ⇒ 实现选择"**先还链、后删 slot**"，与设计文本不同，但满足设计的两条
 *   硬要求（不 double-free、不半释放），代价是反过来有一个窗口：
 *
 *   链已还、slot 还在 → 崩溃 → 重跑时这条 tuple 仍被读出来，
 *   又去还一次链。
 *
 * 这个窗口由 **json_ovf_free 自身的幂等性**兜住（设计 §7.4 的最终方案）：
 * 链页还回去之前会被清零，首页不再是 PT_JSON_OVF ⇒ 第二次调用走
 * "首页不是 PT_JSON_OVF ⇒ OK 空操作"那条路径，不会重复还页。
 *
 * 所以"先还链后删 slot + 释放幂等"是唯一能覆盖两个崩溃窗口的组合。
 *
 * ==================== 为什么必须在表级写锁里 ====================
 *
 * 读者沿 next 走完整条链的期间持读锁（common/json_ovf_lock.h）。
 * 释放者不取写锁 ⇒ 读者可能读到刚被清零的中间页 ⇒ E_CORRUPT。
 * "库自己回收空间就让查询报损坏"不可接受。
 */
static int reclaim_json_chains(Heap *h, const DeadVer *dv,
                               VacuumJsonChainFreeFn json_free,
                               void *json_free_ctx, u64 *count_out)
{
    JsonOvfTableLock *lock;
    int rc;

    if (dv->nheads == 0u)
        return OK;

    lock = heap_json_ovf_lock(h);

    /*
     * 一把写锁罩住整条 tuple 的全部链，而不是每条链各取一次：
     * 同一条 tuple 的链是同时发布的，读者也只有读到这条 tuple 才会
     * 同时需要它们。逐条取锁会让"读者看到一半的链被回收"这件事
     * 变得可能（读锁放开的瞬间写锁进来了）。
     */
    rc = json_ovf_lock_wrlock(lock);
    if (rc != OK)
        return rc;

    for (u32 i = 0; i < dv->nheads; i++) {
        rc = json_free(json_free_ctx, dv->heads[i]);
        if (rc != OK) {
            /*
             * 链坏了（E_CORRUPT）或页读不出来。
             *
             * 【为什么不继续释放剩下的链】
             * 一条坏链说明这条 tuple 的标记本身可疑；接着还别的链
             * 可能把"其实还有人在用"的页还回去。
             * 已经还掉的那几条是幂等的，重跑安全 ⇒ 直接放弃这条 tuple。
             *
             * 整条 tuple 的 slot 也不删（下面的调用方会看到非 OK 并跳过），
             * 让下次 VACUUM 重试；启动扫描是最后一道兜底。
             */
            (void)json_ovf_lock_wrunlock(lock);
            return rc;
        }
        (*count_out)++;
    }

    return json_ovf_lock_wrunlock(lock);
}

int vacuum_table(Heap *h, TxnTable *txn, u32 xid, VacuumStats *stats,
                 VacuumJsonHeadsFn json_heads, void *json_heads_ctx,
                 VacuumJsonChainFreeFn json_free, void *json_free_ctx)
{
    if (!h || !txn)
        return E_INVAL;

    /* 两个回调用一个就必须都用 —— 只给释放不解析等于没接 */
    if ((json_heads == NULL) != (json_free == NULL))
        return E_INVAL;

    VacuumStats local;
    memset(&local, 0, sizeof(local));

    /* ---- 第一遍：收集死版本的 RID（顺带把 JSON 链首页读出来） ---- */

    HeapScan *scan = NULL;
    int rc = heap_scan_begin(h, &scan);
    if (rc != OK)
        return rc;

    DeadVer *dead = NULL;
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
            DeadVer *nd = realloc(dead, (size_t)ncap * sizeof(DeadVer));
            if (!nd) {
                rc = E_IO;
                break;
            }
            dead = nd;
            cap = ncap;
        }

        memset(&dead[n_dead], 0, sizeof(dead[n_dead]));
        dead[n_dead].rid = rid;

        /*
         * 链首页必须在**这里**读，而不是在第二遍删除时读 ——
         * 第二遍要删 slot，删了就再也读不出这条 tuple 的变长区了。
         *
         * 【为什么解析失败不中止整个 VACUUM】
         * 一条 tuple 的标记坏了，不该让整张表的回收失败。
         * 把它记成"没有链"（nheads = 0），那条 tuple 照常删；
         * 它指向的链（如果有）会留给启动扫描（Task 6）。
         * 空间浪费是安全方向，读出错数据不是。
         */
        if (json_heads) {
            u32 nheads = 0;
            if (json_heads(json_heads_ctx, recbuf, reclen,
                           dead[n_dead].heads,
                           (u32)VACUUM_MAX_JSON_HEADS, &nheads) == OK)
                dead[n_dead].nheads = nheads;
        }

        n_dead++;
    }
    heap_scan_end(scan);

    if (rc != OK && rc != E_EOF) {
        free(dead);
        return rc;
    }

    /* ---- 第二遍：回收 JSON 链 + 真正删除 ---- */

    for (int i = 0; i < n_dead; i++) {
        int crc = reclaim_json_chains(h, &dead[i], json_free, json_free_ctx,
                                      &local.json_chains);
        if (crc != OK) {
            /*
             * 链回收失败：**保这条 slot 不删**。
             *
             * 【为什么不能"链没还成也照删 slot"】
             * 删了 slot，链首页就丢了 ⇒ 那条链只能等下次开库的启动扫描。
             * 留着 slot，下次 VACUUM 还能再试一次（链释放是幂等的）。
             * 两条路都不丢数据，但留着更有机会主动回收。
             */
            rc = crc;
            continue;
        }

        rc = heap_delete(h, xid, dead[i].rid);
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
