#include "txn/txn.h"

#include <pthread.h>
#include <string.h>

#include "common/latch.h"

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
 *
 * ==================== v1.2 步骤 4：txn 域的锁 ====================
 *
 * txn 域保护的东西：next_xid、clog、活跃表。
 *
 * 【写路径：域锁】
 *   xid 分配 / 活跃表增删 / clog 写入 —— 一律在域锁里。
 *   域锁是**进程级**的一把互斥量：一个进程里同时开两个库的情况极少，
 *   而进程级锁换来"不需要 init/destroy、不需要改任何调用点"。
 *
 * 【读路径：无锁】
 *   可见性判断每读一行就要查一次 clog —— 这是最热的读路径。
 *   每次取锁会把 10 万行的扫描拖慢好几毫秒（A9 门槛会破），
 *   所以读路径走**原子读**（见 txn.h 里 TxnTable 的说明）。
 *
 * 【为什么写路径不能也用原子】
 *   分配 xid 要"找一个空槽 + 占住它"，两个线程可能同时看上同一个空槽 →
 *   必须互斥。clog 的读-改-写也是同理（这正是改成"一 xid 一字节"的原因：
 *   2bit 打包时两个线程改同一字节里的不同 xid 会互相覆盖）。
 */

/* ---- txn 域的锁（进程级） ---- */

static pthread_mutex_t g_txn_lock = PTHREAD_MUTEX_INITIALIZER;

/*
 * 取域锁 + 向 latch 框架记账。
 *
 * 记账让"catalog → txn → wal → freelist → frame"这条全序
 * 从步骤 2 起就是**机器检查**的（步骤 2 只立框架，步骤 3/4 才接线）。
 */
static void txn_lock(void)
{
    pthread_mutex_lock(&g_txn_lock);
    latch_order_acquire(LATCH_TXN);
}

static void txn_unlock(void)
{
    latch_order_release(LATCH_TXN);
    pthread_mutex_unlock(&g_txn_lock);
}

/* ---- clog：一个 xid 一字节（原子读，无锁） ---- */

static void clog_set(TxnTable *t, u32 xid, u8 state)
{
    atomic_store(&t->clog[xid % TXN_CLOG_SLOTS], state);
}

static u8 clog_get(const TxnTable *t, u32 xid)
{
    return atomic_load(&t->clog[xid % TXN_CLOG_SLOTS]);
}

void txn_clog_set(TxnTable *t, u32 xid, u8 state)
{
    if (!t || xid == 0)
        return;
    txn_lock();
    clog_set(t, xid, state);
    txn_unlock();
}

void txn_table_init(TxnTable *t, u32 start_xid)
{
    /*
     * 【为什么不用 memset 清整个结构】
     *
     * 结构里有原子成员。memset 是"按字节写"，对原子对象来说
     * 不是合法的初始化方式（虽然实际效果一样）。逐个 atomic_init 更干净。
     */
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        atomic_init(&t->active[i].xid, 0);
        atomic_init(&t->active[i].state, TXN_IDLE);
    }
    atomic_init(&t->n_active, 0);
    atomic_init(&t->committed_xmax, 0);
    atomic_init(&t->aborted_xmax, 0);

    /*
     * next_xid 从 start_xid + 1 开始。
     *
     * 为什么 +1：start_xid 是"上一个会话用到的最大 xid"，
     * 这个 xid 本身已经结束了（提交或中止），新事务不能用它。
     */
    u32 nx = start_xid + 1;
    if (nx == 0)
        nx = 1;        /* 0 保留给系统事务 */
    atomic_init(&t->next_xid, nx);

    /*
     * clog 的默认值是"进行中"。
     *
     * 为什么不是"已提交"：clog 只对**已分配**的 xid 有意义，
     * 而已分配的 xid 一律先被标成"进行中"，提交/中止时再改。
     * 还没分配的 xid 由 `xid >= next_xid` 那条规则挡住。
     */
    for (u32 i = 0; i < TXN_CLOG_SLOTS; i++)
        atomic_init(&t->clog[i], TXN_CLOG_INPROGRESS);
}

int txn_begin(TxnTable *t, u32 *xid_out)
{
    if (!t || !xid_out)
        return E_INVAL;

    txn_lock();

    int rc = E_NOSPACE;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) != TXN_IDLE)
            continue;

        u32 xid = atomic_load(&t->next_xid);
        u32 nx = xid + 1;
        if (nx == 0)
            nx = 1;    /* 回绕保护（实际上 40 亿次才可能）*/
        /*
         * 【发布顺序：先 clog，再 next_xid】
         *
         * 反过来的话，读者可能看到 "xid < next_xid"（像是已分配）
         * 但 clog[xid] 还是默认值 —— 默认值是"进行中"，所以其实也安全？
         * 不对：默认值是 INPROGRESS，读者会判成"不可见"，那是**保守**的，
         * 安全。真正危险的是把默认值当成"已提交"。
         *
         * 那为什么还要先 clog 后 next_xid？因为 clog 的槽位是**环形复用**的：
         * xid % SLOTS 这个槽位可能是 4096 个事务之前的老 xid 用过的，
         * 里面还留着"已提交"。如果先发布 next_xid，读者会读到那个陈旧的
         * "已提交" → **脏读**。所以必须先把这个槽位重置成"进行中"。
         */
        clog_set(t, xid, TXN_CLOG_INPROGRESS);
        atomic_store(&t->next_xid, nx);

        atomic_store(&t->active[i].xid, xid);
        atomic_store(&t->active[i].state, TXN_ACTIVE);
        atomic_fetch_add(&t->n_active, 1);

        *xid_out = xid;
        rc = OK;
        break;
    }

    txn_unlock();
    return rc;
}

/* 结束一个事务（调用方已持域锁） */
static int txn_finish(TxnTable *t, u32 xid, u8 state)
{
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) == TXN_ACTIVE &&
            atomic_load(&t->active[i].xid) == xid) {
            /* 先从活跃表摘掉，再改 clog —— 顺序反过来会让快照多看见一个活跃事务（保守，安全） */
            atomic_store(&t->active[i].xid, 0);
            atomic_store(&t->active[i].state, TXN_IDLE);
            atomic_fetch_sub(&t->n_active, 1);
            clog_set(t, xid, state);
            return OK;
        }
    }
    return E_TXN;      /* 不在活跃表里：已经结束过，或根本不是本会话的事务 */
}

int txn_commit(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    txn_lock();
    int rc = txn_finish(t, xid, TXN_CLOG_COMMITTED);
    if (rc == OK) {
        /* 写者都持域锁 → 直接读写即可，不需要 CAS */
        u32 cur = atomic_load(&t->committed_xmax);
        if (xid > cur)
            atomic_store(&t->committed_xmax, xid);
    }
    txn_unlock();
    return rc;
}

int txn_abort(TxnTable *t, u32 xid)
{
    if (!t)
        return E_INVAL;

    txn_lock();
    int rc = txn_finish(t, xid, TXN_CLOG_ABORTED);
    if (rc == OK) {
        u32 cur = atomic_load(&t->aborted_xmax);
        if (xid > cur)
            atomic_store(&t->aborted_xmax, xid);
    }
    txn_unlock();
    return rc;
}

/*
 * 查 clog 的提交状态 —— **无锁**（原子读）。
 *
 * 这是最热的读路径（每读一行都要调），所以一把锁都不取。
 */
u8 txn_clog_state(const TxnTable *t, u32 xid)
{
    if (!t)
        return TXN_CLOG_COMMITTED;
    if (xid == 0)
        return TXN_CLOG_COMMITTED;      /* 系统事务 */

    /*
     * next_xid 只读一次：两次读之间它可能又涨了，
     * 用同一个值判断才不会前后不一致。
     */
    u32 nx = atomic_load(&t->next_xid);
    if (xid >= nx)
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
    if (nx - xid > TXN_CLOG_SLOTS)
        return TXN_CLOG_COMMITTED;

    return clog_get(t, xid);
}

bool txn_committed(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_COMMITTED;
}

bool txn_aborted(const TxnTable *t, u32 xid)
{
    return txn_clog_state(t, xid) == TXN_CLOG_ABORTED;
}

/* ---- 活跃表（读路径也是无锁的：原子读） ---- */

bool txn_has_active(const TxnTable *t)
{
    return t && atomic_load(&t->n_active) > 0;
}

bool txn_has_active_except(const TxnTable *t, u32 exclude_xid)
{
    if (!t)
        return false;
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        if (atomic_load(&t->active[i].state) != TXN_ACTIVE)
            continue;
        if (atomic_load(&t->active[i].xid) == exclude_xid)
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
        if (atomic_load(&t->active[i].state) != TXN_ACTIVE)
            continue;
        u32 xid = atomic_load(&t->active[i].xid);
        if (oldest == 0 || xid < oldest)
            oldest = xid;
    }
    return oldest;
}

int txn_nactive(const TxnTable *t)
{
    return t ? atomic_load(&t->n_active) : 0;
}
