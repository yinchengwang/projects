#ifndef DB_TXN_MVCC_H
#define DB_TXN_MVCC_H

#include "common/error.h"
#include "common/types.h"
#include "txn/txn.h"

/*
 * MVCC 快照与可见性
 *
 * 一句话：给每个事务一个"世界的快照"，让它看到的是某一刻的状态，
 * 而不是"现在"的状态。
 *
 * 【快照存什么】
 *
 * PG 的快照存两个东西：
 *   xmin  —— 快照建立时，仍在运行的事务里最小的 xid。
 *            所有 xid < xmin 的事务，在快照建立前就已经结束了
 *            （提交或中止），所以状态是确定的。
 *   xmax  —— 快照建立时，下一个将要分配的事务号。
 *            所有 xid >= xmax 的事务，在快照建立后才启动，一定看不见。
 *   xip  —— 快照建立时，正在运行的事务的 xid 列表（排除 xmin 那个）。
 *
 * 这个设计和"快照 = 活跃事务列表 + 分界线"等价，但表达更紧凑：
 * 不用把每个活跃事务都列出来（只要列 min 和 max 之间的那些）。
 *
 * 【可见性判断】
 *
 * 对一个版本 (xmin_version, xmax_version)，快照 snap：
 *
 *   步骤 1：xmin_version 的提交状态
 *     - == 0                  → 系统事务，可见
 *     - == snap.my_xid        → 自己写的，可见
 *     - < snap.xmin           → 快照前就结束了 → 看 clog：提交则可见
 *     - >= snap.xmax          → 快照后才启动 → 不可见
 *     - 在 snap.xip 里        → 快照时还在跑 → 不可见
 *
 *   步骤 2：如果步骤 1 判定可见，再看删除标记 xmax_version
 *     - == 0                  → 没被删 → 可见
 *     - == snap.my_xid        → 自己删的 → 不可见
 *     - < snap.xmin           → 快照前就删了 → 看 clog：提交则不可见
 *     - >= snap.xmax          → 快照后才删 → 可见
 *     - 在 snap.xip 里        → 快照时删除事务还在跑 → 可见
 *
 * 【为什么"删除未提交"时旧版本仍可见】
 *
 * 事务 T 删了一行，但还没提交。此时：
 *   - 新事务看这一行：xmax=T，T 还在跑 → 可见（还没删掉）
 *   - T 提交后：新事务看 → xmax=T 已提交 → 不可见
 *
 * 这就是"删除也要走 MVCC"的意思。删除不是抹掉，是加一个标记。
 */

#define SNAP_MAX_XIP 128

typedef struct {
    u32  xmin;        /* 快照时活跃事务的最小 xid（没有活跃事务则等于 xmax） */
    u32  xmax;        /* 快照时 next_xid（快照后启动的事务 xid >= 它） */
    u32  my_xid;      /* 快照所属事务；0 表示只读快照 */
    int  n_xip;        /* 快照时正在运行的事务数（不含 my_xid） */
    u32  xip[SNAP_MAX_XIP];   /* 快照时正在运行的事务号 */
} Snapshot;

/* 从事务表建立快照。 */
void mvcc_snapshot(TxnTable *t, u32 my_xid, Snapshot *snap);

/*
 * 判断一个版本对某个快照是否可见。
 *
 * buf 是 tuple 的完整字节（含 20B 头），len 是 tuple 总长。
 * 可见返回 true。
 */
bool mvcc_visible(const Snapshot *snap, const TxnTable *t, const u8 *buf, u16 len);

/*
 * 判断版本是否"已死"（对任何可能的快照都不可见）→ 可以被 vacuum 回收。
 *
 * 条件：
 *   - xmin 已提交（否则它自己还没定型）
 *   - xmax != 0（xmax == 0 一定是活版本，绝不能回收）
 *   - 且 xmax 已提交
 *
 * exclude_xid 是执行这次判断的事务号（通常是 vacuum 自己）。
 * 判断"还有没有活跃事务"时要排除它 —— 否则 vacuum 自己的事务
 * 会让判定永远为 false，死版本永远回收不掉。
 *
 * 保守原则：除了执行者之外还有活跃事务，就认为可能还有快照能看到它，
 * 返回 false。宁可多留垃圾，不可错删数据。
 */
bool mvcc_is_dead_except(const TxnTable *t, u32 exclude_xid,
                         const u8 *buf, u16 len);

/* 兼容入口：等价于 exclude_xid = 0 */
bool mvcc_is_dead(const TxnTable *t, const u8 *buf, u16 len);

/* 便捷：直接读 tuple 头里的 xmin/xmax */
u32 mvcc_xmin_of(const u8 *buf);
u32 mvcc_xmax_of(const u8 *buf);

#endif /* DB_TXN_MVCC_H */
