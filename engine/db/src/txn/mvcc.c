#include "txn/mvcc.h"

#include <string.h>

/*
 * MVCC 可见性实现。
 *
 * 核心两个函数：mvcc_visible（读路径用，每行都要调）
 * 和 mvcc_is_dead（vacuum 用）。
 *
 * 读路径的性能很重要 —— 每读一行都要做 1~2 次可见性判断。
 * 所以判断逻辑要短：
 *   - 常见的"老数据"（xmin 已提交、xmax=0）走最快路径，2 次比较就返回
 *   - 只有边界情况（xmin 或 xmax 落在快照的活跃区间里）才查 clog
 */

u32 mvcc_xmin_of(const u8 *buf) { return (u32)get_u64(buf + 0); }
u32 mvcc_xmax_of(const u8 *buf) { return (u32)get_u64(buf + 8); }

void mvcc_snapshot(TxnTable *t, u32 my_xid, Snapshot *snap)
{
    if (!snap)
        return;

    memset(snap, 0, sizeof(*snap));
    if (!t) {
        snap->xmin = 0;
        snap->xmax = 0;
        snap->my_xid = my_xid;
        return;
    }

    snap->my_xid = my_xid;
    snap->xmax = t->next_xid;

    /*
     * 收集快照时正在运行的事务。
     *
     * 排除 my_xid：自己当然看得见自己的修改，
     * 不该被算进"快照时还在跑"的列表里（否则 xmin == my_xid
     * 这条特判就永远走不到了）。
     */
    u32 oldest = 0;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (t->active[i].state != TXN_ACTIVE)
            continue;
        if (t->active[i].xid == my_xid)
            continue;
        if (oldest == 0 || t->active[i].xid < oldest)
            oldest = t->active[i].xid;
        if (snap->n_xip < SNAP_MAX_XIP)
            snap->xip[snap->n_xip++] = t->active[i].xid;
    }

    /*
     * xmin = 活跃事务里最小的 xid。
     * 没有活跃事务时，xmin = xmax（表示"所有 xid < xmax 的都已结束"）。
     */
    snap->xmin = oldest ? oldest : snap->xmax;
}

/* xid 是否在快照的活跃列表里 */
static bool in_xip(const Snapshot *snap, u32 xid)
{
    for (int i = 0; i < snap->n_xip; i++) {
        if (snap->xip[i] == xid)
            return true;
    }
    return false;
}

/*
 * 判断一个事务在快照建立时是否已提交。
 *
 * 返回 true = 已提交（对快照可见）
 * 返回 false = 未提交 / 还在跑 / 快照后才启动（对快照不可见）
 */
static bool xid_committed_in_snap(const Snapshot *snap, const TxnTable *t, u32 xid)
{
    if (xid == 0)
        return true;                     /* 系统事务 */
    if (xid == snap->my_xid)
        return true;                     /* 自己写的 */

    if (xid < snap->xmin) {
        /* 快照建立前就结束了 —— 查 clog 定论 */
        return txn_clog_state(t, xid) == TXN_CLOG_COMMITTED;
    }
    if (xid >= snap->xmax)
        return false;                    /* 快照后才启动 */
    if (in_xip(snap, xid))
        return false;                    /* 快照时还在跑 */

    /*
     * 落在 [xmin, xmax) 之间但不在 xip 里 —— **查 clog 定论**。
     *
     * 【这个分支非常容易写错，我第一次就写错了】
     *
     * 我原本写的是"保守起见当作未提交"。但这个分支**不是理论情况，
     * 而是常态** —— 想想它什么时候发生：
     *
     *   快照建立时：事务 X 在跑（所以它在 [xmin,xmax) 区间内）
     *   快照建立后：事务 X 提交了（所以它不在活跃表里，也就不在 xip 里）
     *   现在判断可见性：X 落在 [xmin,xmax) 且不在 xip 里
     *
     * 这就是"快照建立之后才提交的事务"。
     * PG 的语义是：它对**这个快照**不可见（快照是时间点截面的），
     * 但它已经提交了，对**更新的快照**可见。
     *
     * 等等 —— 这里要仔细区分。PG 的规则是：
     *   快照建立时正在跑的事务 → 对该快照不可见
     *   快照建立后才提交的事务 → 对该快照不可见（快照冻结在建立那一刻）
     *
     * 那为什么 PG 要把 xip 记下来？因为 xip 就是"快照建立时正在跑"的清单。
     * 区间内但不在 xip 的事务，说明它在快照建立时**已经结束了**
     *（已提交或已中止）—— 因为如果还在跑，一定会被记进 xip。
     *
     * 所以正确答案是：**查 clog**。
     *   clog = COMMITTED → 快照建立前就提交了 → 可见
     *   clog = ABORTED   → 中止了 → 不可见
     *
     * 我原来写"当作未提交"，后果是：
     *   一个刚提交的事务，它写的行对所有"区间内"的快照都不可见
     *   → **已提交的数据读不出来**
     *
     * 症状极其隐蔽：单连接测试全过（单连接时 xmin == xmax，
     * 区间是空的，永远走不到这个分支），
     * 只有多连接、且有一个长事务占着区间时才会暴露。
     */
    return txn_clog_state(t, xid) == TXN_CLOG_COMMITTED;
}

bool mvcc_visible(const Snapshot *snap, const TxnTable *t, const u8 *buf, u16 len)
{
    if (!snap || !buf || len < 20u)
        return false;

    u32 xmin = mvcc_xmin_of(buf);
    u32 xmax = mvcc_xmax_of(buf);

    /* 步骤 1：插入者可见吗？ */
    if (!xid_committed_in_snap(snap, t, xmin))
        return false;

    /* 步骤 2：被删了吗？（xmax == 0 表示没被删） */
    if (xmax == 0)
        return true;

    if (xmax == snap->my_xid)
        return false;                    /* 自己删的，自己看不到 */

    /*
     * 删除者是否在快照时就已提交？
     * 已提交 → 这一行在快照时就已经不存在了 → 不可见。
     * 未提交 → 快照时删除还没生效 → 可见。
     */
    if (xmin == 0 && xmax == 0)
        return true;

    return !xid_committed_in_snap(snap, t, xmax);
}

bool mvcc_is_dead_except(const TxnTable *t, u32 exclude_xid,
                         const u8 *buf, u16 len)
{
    if (!t || !buf || len < 20u)
        return false;

    u32 xmin = mvcc_xmin_of(buf);
    u32 xmax = mvcc_xmax_of(buf);

    /*
     * 【关键】xmax == 0 表示"这个版本没被删除也没被替换" ——
     * 它就是当前的活版本。**绝不能回收**。
     *
     * 死版本一定是"曾经被删除或被替换"的，所以 xmax 必然非零。
     * 漏掉这一条就会把活数据删光 —— 这是最危险的一类 bug。
     */
    if (xmax == 0)
        return false;

    /*
     * 插入者还没提交 → 不能动。
     * 这一行可能正被一个活跃事务读着，或者即将被提交。
     */
    if (txn_clog_state(t, xmin) != TXN_CLOG_COMMITTED)
        return false;

    /*
     * 删除者还没提交 → 更不能动。
     * 删除一旦提交，这一行就彻底不可见了，但删除还没提交，
     * 现在删掉它会让删除者提交后找不到要删的东西。
     */
    if (txn_clog_state(t, xmax) != TXN_CLOG_COMMITTED)
        return false;

    /*
     * 保守判定：除了执行者之外，只要还有活跃事务，
     * 就认为可能还有快照能看到它。
     *
     * 为什么不做得更精确（比如比较活跃事务的快照 xmin）：
     * 因为那样要维护"每个活跃事务各自的快照"，复杂度显著上升。
     * v0.7 先用保守版 —— 代价是垃圾回收得不够及时，
     * 但绝不会错删数据。宁可多留垃圾，不可错删数据。
     *
     * 【为什么必须排除执行者自己】
     *
     * vacuum 自己跑在一个事务里。如果不算排除它，
     * txn_has_active 永远为 true，vacuum 就永远回收不掉任何东西。
     * 这不是"保守"，这是"失效" —— 一个只会空转的语句。
     */
    if (txn_has_active_except(t, exclude_xid))
        return false;

    return true;
}

bool mvcc_is_dead(const TxnTable *t, const u8 *buf, u16 len)
{
    return mvcc_is_dead_except(t, 0, buf, len);
}
