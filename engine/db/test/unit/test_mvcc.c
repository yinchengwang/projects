/*
 * test_mvcc.c — MVCC 可见性单元验收
 *
 * 10 项：
 *   1. 自己插入的行，自己看得见
 *   2. 未提交的行，别人看不见
 *   3. 提交后，别人看得见
 *   4. RR：快照固定后，别人后来提交的看不见
 *   5. RC：快照刷新后，别人新提交的看得见
 *   6. UPDATE 产生新版本：旧快照看旧值，新快照看新值
 *   7. DELETE 之后：旧快照仍看得见，新快照看不见
 *   8. vacuum 之后：死版本被清理，活版本完好
 *   9. 中止的事务不可见
 *  10. 区间内但不在 xip 的已提交事务 → 必须可见（回归测试）
 *  11. vacuum 判断死版本时必须排除执行者自己（回归测试）
 *
 * 【这些测试为什么重要】
 *
 * MVCC 的 bug 全是"静默"的：读到自己不该看到的（脏读），
 * 或者读不到自己该看到的（丢数据）。两种都不报错，只是结果不对。
 * 所以每一项都要精确地断言"谁能看到什么"。
 *
 * 【第 10、11 项是回归测试】
 *
 * 它们锁定的两个 bug 都是"多连接才会暴露"的类型：
 * 单连接时 xmin == xmax，区间是空的，永远走不到那个分支。
 * 所以单连接测试全绿并不能说明可见性逻辑是对的 ——
 * 必须专门构造多连接场景才能测到。
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/error.h"
#include "common/types.h"
#include "storage/record.h"
#include "txn/mvcc.h"
#include "txn/txn.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/*
 * 造一条 tuple：只有版本头 + 一个 int 值。
 * 格式与 record.c 的 REC_HDR_SIZE 对齐（20B 头）。
 */
static u16 make_tuple(u8 *buf, u32 xmin, u32 xmax, i32 val)
{
    memset(buf, 0, REC_HDR_SIZE);
    rec_set_xmin(buf, (u64)xmin);
    rec_set_xmax(buf, (u64)xmax);
    put_u32(buf + REC_HDR_SIZE, (u32)val);
    return (u16)(REC_HDR_SIZE + 4);
}

/* ---- 1 & 2 & 3) 插入的可见性 ---- */

/*
 * 三个场景一起测，因为它们共享同一个事务表演进：
 *
 *   T1 插入 → T2 看不见（T1 未提交）
 *   T1 提交 → T2 建新快照 → 看得见
 *
 * 这就是"未提交不可见、提交后可见"的完整证明。
 */
static void t1_insert_visibility(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);
    CHECK(x1 != x2);

    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 42);

    /* T2 先建快照（T1 还没提交） */
    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);

    /* T1 未提交 → T2 看不见 */
    CHECK(mvcc_visible(&s2, &tt, tup, len) == false);

    /* T1 自己看得见 */
    Snapshot s1;
    mvcc_snapshot(&tt, x1, &s1);
    CHECK(mvcc_visible(&s1, &tt, tup, len) == true);

    /* T1 提交 */
    CHECK(txn_commit(&tt, x1) == OK);

    /* T2 的旧快照仍然看不见（快照是固定的） */
    CHECK(mvcc_visible(&s2, &tt, tup, len) == false);

    /* T2 建新快照 → 看得见了 */
    Snapshot s2b;
    mvcc_snapshot(&tt, x2, &s2b);
    CHECK(mvcc_visible(&s2b, &tt, tup, len) == true);
}

/* ---- 4) RR：快照固定 ---- */
static void t2_rr_snapshot_fixed(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);

    /* T2 先建快照（模拟 BEGIN 时的快照） */
    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);

    /* T1 插入并提交 */
    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 7);
    CHECK(txn_commit(&tt, x1) == OK);

    /* T2 的快照是固定的 → 看不见 T1 提交的数据 */
    CHECK(mvcc_visible(&s2, &tt, tup, len) == false);

    /* 但如果 T2 刷新快照（RC 语义），就能看见 */
    Snapshot s2b;
    mvcc_snapshot(&tt, x2, &s2b);
    CHECK(mvcc_visible(&s2b, &tt, tup, len) == true);
}

/* ---- 5) RC：快照刷新 ---- */
static void t3_rc_snapshot_refresh(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);

    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);

    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 99);
    CHECK(txn_commit(&tt, x1) == OK);

    /* 旧快照看不见 */
    CHECK(mvcc_visible(&s2, &tt, tup, len) == false);

    /* RC：每条语句重建快照 → 现在看得见 */
    mvcc_snapshot(&tt, x2, &s2);
    CHECK(mvcc_visible(&s2, &tt, tup, len) == true);
}

/* ---- 6) UPDATE 的新旧版本 ---- */
static void t4_update_versions(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);

    /* 原始版本：xmin=x1, xmax=0 */
    u8 oldv[64];
    u16 oldlen = make_tuple(oldv, x1, 0, 100);
    CHECK(txn_commit(&tt, x1) == OK);

    /* 提交后，任何新快照都应看到原始版本 */
    Snapshot s_check;
    mvcc_snapshot(&tt, x2, &s_check);
    CHECK(mvcc_visible(&s_check, &tt, oldv, oldlen) == true);

    /* T2 建快照（看到原始版本） */
    CHECK(txn_begin(&tt, &x2) == OK);
    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);

    /* T1 更新：旧版本打 xmax=x1，新版本 xmin=x1 */
    u32 x3 = 0;
    CHECK(txn_begin(&tt, &x3) == OK);

    u8 marked[64];
    u16 mlen = make_tuple(marked, x1, x3, 100);   /* 旧版本被 x3 标记删除 */
    u8 newv[64];
    u16 nlen = make_tuple(newv, x3, 0, 200);      /* 新版本 */

    /* T2 的快照：旧版本可见（删除未提交），新版本不可见（插入未提交） */
    CHECK(mvcc_visible(&s2, &tt, marked, mlen) == true);
    CHECK(mvcc_visible(&s2, &tt, newv, nlen) == false);

    /* T3 提交后，T2 的旧快照：旧版本仍可见，新版本仍不可见（快照固定） */
    CHECK(txn_commit(&tt, x3) == OK);
    CHECK(mvcc_visible(&s2, &tt, marked, mlen) == true);
    CHECK(mvcc_visible(&s2, &tt, newv, nlen) == false);

    /* T2 刷新快照：旧版本不可见（删除已提交），新版本可见 */
    Snapshot s2b;
    mvcc_snapshot(&tt, x2, &s2b);
    CHECK(mvcc_visible(&s2b, &tt, marked, mlen) == false);
    CHECK(mvcc_visible(&s2b, &tt, newv, nlen) == true);
}

/* ---- 7) DELETE 的可见性 ---- */
static void t5_delete_visibility(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);

    u8 orig[64];
    u16 olen = make_tuple(orig, x1, 0, 55);
    CHECK(txn_commit(&tt, x1) == OK);

    /* 提交后新快照能看到原版 */
    Snapshot s_check;
    mvcc_snapshot(&tt, x2, &s_check);
    CHECK(mvcc_visible(&s_check, &tt, orig, olen) == true);

    CHECK(txn_begin(&tt, &x2) == OK);
    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);

    /* T3 删除：打 xmax 标记 */
    u32 x3 = 0;
    CHECK(txn_begin(&tt, &x3) == OK);
    u8 deleted[64];
    u16 dlen = make_tuple(deleted, x1, x3, 55);

    /* 删除未提交 → 别人还看得见 */
    CHECK(mvcc_visible(&s2, &tt, deleted, dlen) == true);

    CHECK(txn_commit(&tt, x3) == OK);

    /* 删除已提交 → 旧快照仍看得见（快照在删除前建立） */
    CHECK(mvcc_visible(&s2, &tt, deleted, dlen) == true);

    /* 新快照 → 看不见了 */
    Snapshot s2b;
    mvcc_snapshot(&tt, x2, &s2b);
    CHECK(mvcc_visible(&s2b, &tt, deleted, dlen) == false);
}

/* ---- 8) vacuum 的死版本判定 ---- */
static void t6_vacuum_dead(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);

    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 1);

    /* 未提交 → 不能动 */
    CHECK(mvcc_is_dead(&tt, tup, len) == false);

    CHECK(txn_commit(&tt, x1) == OK);

    /*
     * xmax == 0 是"当前活版本" → **绝不能回收**。
     * 这是最危险的一条：判错就会把活数据删光。
     */
    CHECK(mvcc_is_dead(&tt, tup, len) == false);

    /* 有活跃事务时保守起见不动 */
    u32 x2 = 0;
    CHECK(txn_begin(&tt, &x2) == OK);
    CHECK(mvcc_is_dead(&tt, tup, len) == false);
    CHECK(txn_commit(&tt, x2) == OK);

    /* 删除标记未提交 → 更不能动 */
    u32 x3 = 0;
    CHECK(txn_begin(&tt, &x3) == OK);
    u8 marked[64];
    u16 mlen = make_tuple(marked, x1, x3, 1);
    CHECK(mvcc_is_dead(&tt, marked, mlen) == false);

    /* 删除已提交 + 无活跃事务 → 死版本，可以回收 */
    CHECK(txn_commit(&tt, x3) == OK);
    CHECK(mvcc_is_dead(&tt, marked, mlen) == true);
}

/* ---- 9) xid=0 是系统事务，永远可见 ---- */
static void t7_system_xact(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x2 = 0;
    CHECK(txn_begin(&tt, &x2) == OK);

    u8 tup[64];
    u16 len = make_tuple(tup, 0, 0, 1);

    Snapshot s;
    mvcc_snapshot(&tt, x2, &s);
    CHECK(mvcc_visible(&s, &tt, tup, len) == true);
}

/* ---- 10) 中止的事务不可见 ---- */
static void t8_abort_invisible(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);
    CHECK(txn_begin(&tt, &x2) == OK);

    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 1);

    CHECK(txn_abort(&tt, x1) == OK);

    Snapshot s2;
    mvcc_snapshot(&tt, x2, &s2);
    CHECK(mvcc_visible(&s2, &tt, tup, len) == false);
}

/*
 * ---- 9) 区间内但不在 xip 的事务 → 查 clog ----
 *
 * 【这是 v0.7 最隐蔽的一个 bug 的回归测试】
 *
 * 场景：
 *   T1（xid=1）开始
 *   T2（xid=2）开始
 *   T2 建快照 —— 此时 T1 活跃，所以 T1 的 xid 落在快照区间内，
 *                但因为 T1 还在跑，**不在 xip 里**（xip 只记快照时活跃的）
 *
 *   等等 —— T1 活跃就应该在 xip 里。重新设计场景：
 *
 *   T1（xid=1）开始 → T2（xid=2）开始 → T1 提交
 *   → T3（xid=3）建快照
 *
 *   此时快照：xmin = 2（T2 活跃，排除 my_xid），xmax = 4
 *   T1 的 xid = 1 < xmin = 2 → 走 "xid < xmin" 分支，查 clog。
 *
 *   这个也能覆盖到区间内的情况：让 T2 在 T3 建快照**之后**提交。
 *   → T3 的快照建好时 T2 还在跑（在 xip 里），但 T2 随后提交了。
 *   → 再建一个快照 S4（此时 T2 已提交，不在活跃表，也不在 xip 里）
 *   → S4 的 xmin 会被别的活跃事务撑开，让 T2 的 xid 落在区间内。
 *
 * 最直接的构造：
 *   T1(xid=1) 开始
 *   T2(xid=2) 开始
 *   T1 提交          ← T1 的 xid 现在"已结束但不在活跃表"
 *   T3(xid=3) 开始
 *   T4(xid=4) 开始   ← 撑开区间
 *   T3 建快照: xmin=2（排除 my_xid=3，活跃的是 2 和 4，最小是 2）
 *               xmax=5
 *               xip = {2, 4}
 *   → T1 的 xid=1 < xmin=2，走 "< xmin" 分支。
 *
 *   要让"已提交的 xid"落在区间内，需要它在快照建立时就已经结束，
 *   但它的 xid 又比 xmin 大 —— 这在 xid 单调分配下不可能
 *   （xmin 是最小的活跃 xid，所有更早的 xid 都 < xmin）。
 *
 *   所以真正的"区间内不在 xip"场景是：
 *   **快照建立之后，区间内的某个事务提交了**。
 *   这时用**旧快照**判断可见性：
 *
 *   T1(xid=1) 开始
 *   T2(xid=2) 开始并提交    ← 提交发生在快照建立之后
 *   T3(xid=3) 建立快照 S3   ← S3 的 xip 里没有 T2（已提交）
 *                           ← 但 T2 的 xid=2 落在 [xmin, xmax) 区间内
 *                           ← xmin 此时 = 3（只有 T3 自己活跃，排除后 = xmax）
 *
 *   嗯，如果只剩 T3 自己活跃，xmin = xmax，区间是空的。
 *   需要另一个活跃事务 T4 撑住区间下界：
 *
 *   T1(xid=1) 开始
 *   T2(xid=2) 开始
 *   T3(xid=3) 建立快照 S3  ← 此刻 T1、T2 活跃
 *                              xmin = 1（排除 my_xid=3，取活跃最小 = 1）
 *                              xmax = 4
 *                              xip = {1, 2}
 *   T1 提交             ← 发生在快照建立之后
 *   T2 提交             ← 也发生在快照建立之后
 *   用 S3 判断 T1 写的行：
 *     xmin=1，T1 在 xip 里 → 不可见 ✓（这是对的，RR 语义）
 *
 *   那"区间内不在 xip 且应该可见"到底怎么构造？
 *
 *   关键：xip 记的是**快照建立时**活跃的事务。
 *   快照建立**之后**才启动的事务，xid >= snap.xmax → 不可见。
 *   快照建立时活跃的事务 → 在 xip 里 → 不可见。
 *   快照建立时已结束的事务 → xid < snap.xmin（因为 xmin 是活跃最小值，
 *                             已结束的更早的事务 xid 更小）→ 走 "< xmin" 分支。
 *
 *   结论：区间 [xmin, xmax) 内不在 xip 的事务，
 *         只可能是"快照建立时已结束、但 xid >= xmin"的那些。
 *         这在 xid 单调分配 + xmin 取活跃最小值的前提下**不该存在**。
 *
 *   那我原来写的"区间内不在 xip → 当作未提交"错在哪？
 *
 *   错在：**xmin 的定义**。我的 mvcc_snapshot 里 xmin = 活跃事务的最小 xid
 *   （排除 my_xid）。但 PG 的 xmin 语义是"**所有**活跃事务的最小 xid"
 *   —— **不排除 my_xid**！
 *
 *   PG 这样定义的原因：my_xid 自己的事务，xmin==my_xid 的特判先处理了；
 *   而 xmin 作为"分界线"必须包含自己，否则会出现"自己 < xmin"
 *   的诡异情况（自己明明是活跃的）。
 *
 *   当 my_xid 是最小活跃 xid 时：
 *     我的实现：xmin = 下一个活跃事务的 xid（更大）
 *     PG：    xmin = my_xid（更小）
 *
 *   于是 PG 的区间更大，落在 (PG_xmin, my_xmax) 的都是"已结束的更早事务"，
 *   必须查 clog。
 *
 *   而我原来那个"当作未提交"的分支，正是被多连接场景踩中的：
 *   写方 W 提交后，读方 R 建快照，R 的 xid 更大，
 *   W 的 xid 落在 R 的区间内但不在 xip 里 → 被判为不可见
 *   → **已提交的数据读不出来**。
 *
 *   这个测试就锁定这个场景。
 */
static void t9_xid_in_range_not_in_xip(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0, x3 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);      /* T1 保持活跃 */
    CHECK(txn_begin(&tt, &x2) == OK);

    /* T2 提交 —— 它的 xid 会落在 T3 的快照区间内，但不在 xip 里 */
    CHECK(txn_commit(&tt, x2) == OK);

    /* T3 开始 */
    CHECK(txn_begin(&tt, &x3) == OK);

    u8 tup[64];
    u16 len = make_tuple(tup, x2, 0, 42);

    Snapshot s3;
    mvcc_snapshot(&tt, x3, &s3);

    /* 确认场景构造正确：T2 的 xid 确实落在区间内且不在 xip 里 */
    CHECK(s3.xmin <= x2);
    CHECK(x2 < s3.xmax);
    CHECK(s3.n_xip == 1);
    CHECK(s3.xip[0] != x2);

    /*
     * T2 已提交 → 它写的行对该快照可见。
     *
     * 原来的实现走"区间内不在 xip → 当作未提交" → 不可见 → 丢数据。
     */
    CHECK(mvcc_visible(&s3, &tt, tup, len) == true);

    /* 对照：T1 还没提交 → 不可见（它在 xip 里）*/
    u8 tup2[64];
    u16 len2 = make_tuple(tup2, x1, 0, 99);
    CHECK(mvcc_visible(&s3, &tt, tup2, len2) == false);
}

/*
 * ---- 10) vacuum 必须排除执行者自己 ----
 *
 * vacuum 自己跑在一个事务里。如果判断"还有没有活跃事务"时
 * 把自己也算上，死版本永远回收不掉 —— vacuum 变成空转语句。
 */
static void t10_vacuum_excludes_self(void)
{
    TxnTable tt;
    txn_table_init(&tt, 0);

    u32 x1 = 0, x2 = 0;
    CHECK(txn_begin(&tt, &x1) == OK);

    /* T1 插入，T2 删除并提交 → T1 的版本成为死版本 */
    u8 tup[64];
    u16 len = make_tuple(tup, x1, 0, 1);

    CHECK(txn_begin(&tt, &x2) == OK);
    rec_set_xmax(tup, (u64)x2);
    CHECK(txn_commit(&tt, x2) == OK);

    /* T1 也提交 */
    CHECK(txn_commit(&tt, x1) == OK);

    /* 现在没有活跃事务 → 是死版本 */
    CHECK(mvcc_is_dead(&tt, tup, len) == true);

    /* T1 活跃（模拟 vacuum 自己的事务）→ 不算死 */
    CHECK(txn_begin(&tt, &x2) == OK);
    CHECK(mvcc_is_dead(&tt, tup, len) == false);

    /* 排除执行者自己 → 算死版本 ✓ */
    CHECK(mvcc_is_dead_except(&tt, x2, tup, len) == true);
}

int main(void)
{
    printf("test_mvcc:\n");

    t1_insert_visibility();
    t2_rr_snapshot_fixed();
    t3_rc_snapshot_refresh();
    t4_update_versions();
    t5_delete_visibility();
    t6_vacuum_dead();
    t7_system_xact();
    t8_abort_invisible();
    t9_xid_in_range_not_in_xip();
    t10_vacuum_excludes_self();

    if (failures) {
        printf("  %d 项失败 [FAIL]\n", failures);
        return 1;
    }
    printf("  10 项验收全部通过 [PASS]\n");
    return 0;
}
