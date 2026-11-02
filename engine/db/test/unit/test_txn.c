/*
 * test_txn.c — 事务表状态机验收
 *
 * 5 项（PLAN-v0.7.md 承诺的验收项）：
 *   1. 状态机：IDLE → ACTIVE → COMMITTED / ABORTED
 *   2. COMMIT 后 clog 记为已提交，ABORT 后记为已中止
 *   3. xid 单调递增，永不重用
 *   4. 活跃事务表：begin/commit 的进出，oldest_active 的正确性
 *   5. clog 环形窗口：窗口外的 xid 一律视为已提交
 *
 * 【为什么这些必须单独测】
 *
 * 事务表的 bug 全是**静默**的：
 *   - clog 状态写错 → 可见性判断错 → 脏读或丢数据
 *   - xid 重用 → 旧版本无法判断 → 读到不该读的数据
 *   - 活跃表没清 → 事务永远"在跑" → 快照永远固定
 *
 * 都不会报错，只是结果不对。所以每条规则都要精确断言。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "txn/txn.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* ---- 1) 状态机：IDLE → ACTIVE → COMMITTED / ABORTED ---- */
static void t1_state_machine(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    /* 初始：没有活跃事务 */
    CHECK(txn_nactive(&tt) == 0);
    CHECK(txn_has_active(&tt) == false);

    /* IDLE → ACTIVE */
    u32 x1 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_nactive(&tt) == 1);
    CHECK(txn_has_active(&tt) == true);
    CHECK(txn_oldest_active(&tt) == x1);

    /* ACTIVE → COMMITTED */
    CHECK(txn_commit(&tt, x1) == OK);
    CHECK(txn_nactive(&tt) == 0);
    CHECK(txn_has_active(&tt) == false);
    CHECK(txn_oldest_active(&tt) == 0);

    /* 再次提交同一个 xid 必须失败 —— 它已经不在活跃表里了 */
    CHECK(txn_commit(&tt, x1) == E_TXN);
    CHECK(txn_abort(&tt, x1) == E_TXN);

    /* ACTIVE → ABORTED */
    u32 x2 = 0;
    CHECK(txn_begin(&tt, &x2) == OK);
    CHECK(txn_nactive(&tt) == 1);
    CHECK(txn_abort(&tt, x2) == OK);
    CHECK(txn_nactive(&tt) == 0);
    CHECK(txn_has_active(&tt) == false);

    /* 中止后再提交也失败 */
    CHECK(txn_commit(&tt, x2) == E_TXN);
}

/* ---- 2) COMMIT / ABORT 写进 clog ---- */
static void t2_clog_state(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);

    /* 进行中的事务：clog 记为 INPROGRESS */
    CHECK(txn_clog_state(&tt, x1) == TXN_CLOG_INPROGRESS);
    CHECK(txn_clog_state(&tt, x2) == TXN_CLOG_INPROGRESS);
    CHECK(txn_committed(&tt, x1) == false);
    CHECK(txn_aborted(&tt, x1) == false);

    /* 提交 → clog 记为 COMMITTED */
    CHECK(txn_commit(&tt, x1) == OK);
    CHECK(txn_clog_state(&tt, x1) == TXN_CLOG_COMMITTED);
    CHECK(txn_committed(&tt, x1) == true);
    CHECK(txn_aborted(&tt, x1) == false);
    CHECK(tt.committed_xmax == x1);

    /* 中止 → clog 记为 ABORTED */
    CHECK(txn_abort(&tt, x2) == OK);
    CHECK(txn_clog_state(&tt, x2) == TXN_CLOG_ABORTED);
    CHECK(txn_committed(&tt, x2) == false);
    CHECK(txn_aborted(&tt, x2) == true);
    CHECK(tt.aborted_xmax == x2);

    /* 关键：中止的事务绝不能被当成已提交 */
    /*
     * 这一条如果不成立，可见性判断会把中止事务写的数据当成可见 ——
     * 也就是"读到了从未真正存在的数据"。这是最危险的一类错误，
     * 因为它不报错，只是结果不对。
     */
    CHECK(txn_committed(&tt, x2) == false);
}

/* ---- 3) xid 单调递增，永不重用 ---- */
static void t3_xid_monotonic(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 prev = 0;
    for (int i = 0; i < 100; i++) {
        u32 x = 0;
        CHECK(txn_begin(&tt, &x) == OK);
        /* 严格递增 —— 不能相等，更不能倒退 */
        CHECK(x > prev);
        prev = x;
        CHECK(txn_commit(&tt, x) == OK);
    }

    /* 中止的事务也不能让 xid 回退 */
    u32 xa = 0, xb = 0;
    CHECK(txn_begin(&tt, &xa) == OK);
    CHECK(txn_begin(&tt, &xb) == OK);
    CHECK(xb > xa);
    CHECK(txn_abort(&tt, xa) == OK);
    CHECK(txn_commit(&tt, xb) == OK);

    u32 xc = 0;
    CHECK(txn_begin(&tt, &xc) == OK);
    CHECK(xc > xb);          /* 中止不回收 xid */
    CHECK(txn_commit(&tt, xc) == OK);
}

/* ---- 4) 活跃事务表与 oldest_active ---- */
static void t4_active_table(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0, x3 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);
    CHECK(txn_begin(&tt, &x3) == OK);
    CHECK(txn_nactive(&tt) == 3);

    /* oldest_active 是活跃事务里最小的 xid —— 快照的 xmin 就靠它 */
    CHECK(txn_oldest_active(&tt) == x1);

    /* 结束中间的 x2，oldest 不变 */
    CHECK(txn_commit(&tt, x2) == OK);
    CHECK(txn_nactive(&tt) == 2);
    CHECK(txn_oldest_active(&tt) == x1);

    /* 结束最小的 x1，oldest 前移到 x3 */
    CHECK(txn_commit(&tt, x1) == OK);
    CHECK(txn_nactive(&tt) == 1);
    CHECK(txn_oldest_active(&tt) == x3);

    /* 全结束 → oldest 归零 */
    CHECK(txn_commit(&tt, x3) == OK);
    CHECK(txn_nactive(&tt) == 0);
    CHECK(txn_oldest_active(&tt) == 0);

    /*
     * 活跃表满了要报错，不能覆盖已有的事务。
     *
     * 这条很重要：如果满了之后还分配 xid，会把某个活跃事务的槽位覆盖掉，
     * 那个事务就"从表里消失了" —— 提交时报 E_TXN，可见性判断也随之出错。
     */
    for (int i = 0; i < TXN_MAX_ACTIVE; i++) {
        u32 x = 0;
        CHECK(txn_begin(&tt, &x) == OK);
    }
    CHECK(txn_nactive(&tt) == TXN_MAX_ACTIVE);

    u32 over = 0;
    CHECK(txn_begin(&tt, &over) == E_NOSPACE);
}

/* ---- 5) clog 环形窗口 ---- */
static void t5_clog_window(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    /*
     * 窗口外的 xid 一律视为已提交。
     *
     * 为什么安全：走出窗口意味着中间过去了 4096 个事务，
     * 那个 xid 早就结束了（提交或中止）。中止的版本会被 vacuum 清掉，
     * 不会一直留在表里被人读到。
     */
    u32 x = 0;
    CHECK(txn_begin(&tt, &x) == OK);
    CHECK(txn_commit(&tt, x) == OK);

    /* 推进 next_xid 越过整个窗口 */
    for (u32 i = 0; i < TXN_CLOG_SLOTS + 10u; i++) {
        u32 y = 0;
        CHECK(txn_begin(&tt, &y) == OK);
        CHECK(txn_commit(&tt, y) == OK);
    }

    /* 第一个 xid 已经落在窗口外 → 视为已提交 */
    CHECK(txn_clog_state(&tt, x) == TXN_CLOG_COMMITTED);
    CHECK(txn_committed(&tt, x) == true);

    /* xid = 0 是系统事务，永远已提交 */
    CHECK(txn_clog_state(&tt, 0) == TXN_CLOG_COMMITTED);

    /* 还没分配出来的 xid → 进行中 */
    u32 nxt = tt.next_xid;
    CHECK(txn_clog_state(&tt, nxt) == TXN_CLOG_INPROGRESS);
    CHECK(txn_clog_state(&tt, nxt + 100u) == TXN_CLOG_INPROGRESS);
}

/* ---- 6) 跨会话：xid 必须接着上次继续 ---- */
static void t6_restart_xid(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x = 0;
    CHECK(txn_begin(&tt, &x) == OK);
    CHECK(txn_commit(&tt, x) == OK);

    /*
     * 重启后必须从 last_xid + 1 继续。
     *
     * 如果重启后从 1 开始，上次那个 xid=1 的事务（哪怕没提交）
     * 会被这次新的 xid=1 提交记录"连累"，判定成已提交 ——
     * 未提交的数据就留在了库里。这是跨会话的数据损坏。
     */
    TxnTable tt2;
    txn_table_init(&tt2, x);
    u32 y = 0;
    CHECK(txn_begin(&tt2, &y) == OK);
    CHECK(y > x);
    CHECK(txn_commit(&tt2, y) == OK);

    /* 上次的提交状态要能通过 clog_set 恢复出来 */
    TxnTable tt3;
    txn_table_init(&tt3, x);
    txn_clog_set(&tt3, x, TXN_CLOG_COMMITTED);
    CHECK(txn_committed(&tt3, x) == true);
}

int main(void)
{
    printf("test_txn:\n");

    t1_state_machine();
    t2_clog_state();
    t3_xid_monotonic();
    t4_active_table();
    t5_clog_window();
    t6_restart_xid();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  6 项验收全部通过 [PASS]\n");
    return 0;
}
