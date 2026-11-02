#include "txn/txn.h"

#include <string.h>

/*
 * 事务表 + clog 实现。
 *
 * 核心不变量：
 *   1. next_xid 单调递增，永不回退
 *   2. clog 环形窗口：clog[xid % SLOTS] 存 xid 的状态
 *   3. 一个 xid 在活跃表里最多出现一次
 *
 * 第 3 条靠"分配时扫描找空槽"保证 —— 事务数很少（几十个），
 * 线性扫描比任何哈希结构都简单且不会出错。
 */

/* ---- clog 的位操作：一个字节存 4 个 2bit 状态 ---- */

static u8 clog_get(const u8 *clog, u32 xid)
{
    u32 idx = xid % TXN_CLOG_SLOTS;
    u8 b = clog[idx / 4u];
    return (u8)((b >> ((idx % 4u) * 2u)) & 0x03u);
}

static void clog_set(u8 *clog, u32 xid, u8 state)
{
    u32 idx = xid % TXN_CLOG_SLOTS;
    u32 sh = (idx % 4u) * 2u;
    u8 b = clog[idx / 4u];
    b = (u8)((b & ~(0x03u << sh)) | ((state & 0x03u) << sh));
    clog[idx / 4u] = b;
}

void txn_clog_set(TxnTable *t, u32 xid, u8 state)
{
    if (!t || xid == 0)
        return;
    clog_set(t->clog, xid, state);
}

void txn_table_init(TxnTable *t, u32 start_xid)
{
    memset(t, 0, sizeof(*t));
    /*
     * next_xid 从 start_xid + 1 开始。
     *
     * 为什么 +1：start_xid 是"上一个会话用到的最大 xid"，
     * 这个 xid 本身已经结束了（提交或中止），新事务不能用它。
     */
    t->next_xid = start_xid + 1;
    if (t->next_xid == 0)
        t->next_xid = 1;        /* 0 保留给系统事务 */
}

int txn_begin(TxnTable *t, u32 *xid_out)
{
    if (!t || !xid_out)
        return E_INVAL;

    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (t->active[i].state == TXN_IDLE) {
            u32 xid = t->next_xid++;
            if (t->next_xid == 0)
                t->next_xid = 1;    /* 回绕保护（实际上 40 亿次才可能）*/

            t->active[i].xid = xid;
            t->active[i].state = TXN_ACTIVE;
            t->n_active++;

            /* clog 里先标"进行中"，提交/中止时再改 */
            clog_set(t->clog, xid, TXN_CLOG_INPROGRESS);
            *xid_out = xid;
            return OK;
        }
    }
    return E_NOSPACE;
}

static int txn_finish(TxnTable *t, u32 xid, u8 state)
{
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (t->active[i].state == TXN_ACTIVE && t->active[i].xid == xid) {
            t->active[i].state = TXN_IDLE;
            t->active[i].xid = 0;
            t->n_active--;
            clog_set(t->clog, xid, state);
            return OK;
        }
    }
    return E_TXN;      /* 不在活跃表里：已经结束过，或根本不是本会话的事务 */
}

int txn_commit(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    int rc = txn_finish(t, xid, TXN_CLOG_COMMITTED);
    if (rc != OK)
        return rc;

    if (xid > t->committed_xmax)
        t->committed_xmax = xid;
    return OK;
}

int txn_abort(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    int rc = txn_finish(t, xid, TXN_CLOG_ABORTED);
    if (rc != OK)
        return rc;

    if (xid > t->aborted_xmax)
        t->aborted_xmax = xid;
    return OK;
}

u8 txn_clog_state(const TxnTable *t, u32 xid)
{
    if (!t)
        return TXN_CLOG_COMMITTED;
    if (xid == 0)
        return TXN_CLOG_COMMITTED;      /* 系统事务 */
    if (xid >= t->next_xid)
        return TXN_CLOG_INPROGRESS;      /* 还没分配出来的事务 */

    /*
     * 窗口外的 xid 一律当作已提交。
     *
     * 什么时候会走到这里：xid + TXN_CLOG_SLOTS < next_xid，
     * 说明这个 xid 已经很老了 —— 中间过去了 4096 个事务。
     * 这时它不可能还在进行中（要么提交要么中止）。
     *
     * 中止的情况怎么办？中止的版本会被 vacuum 清掉，
     * 不会一直留在表里被人读到。所以按"已提交"处理是安全的。
     */
    if (t->next_xid - xid > TXN_CLOG_SLOTS)
        return TXN_CLOG_COMMITTED;

    return clog_get(t->clog, xid);
}

bool txn_committed(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_COMMITTED;
}

bool txn_aborted(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_ABORTED;
}

bool txn_has_active(const TxnTable *t)
{
    return t && t->n_active > 0;
}

bool txn_has_active_except(const TxnTable *t, u32 exclude_xid)
{
    if (!t)
        return false;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (t->active[i].state != TXN_ACTIVE)
            continue;
        if (t->active[i].xid == exclude_xid)
            continue;         /* 忽略自己 */
        return true;
    }
    return false;
}

u32 txn_oldest_active(const TxnTable *t)
{
    if (!t)
        return 0;

    u32 oldest = 0;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (t->active[i].state != TXN_ACTIVE)
            continue;
        if (oldest == 0 || t->active[i].xid < oldest)
            oldest = t->active[i].xid;
    }
    return oldest;
}

int txn_nactive(const TxnTable *t)
{
    return t ? t->n_active : 0;
}
