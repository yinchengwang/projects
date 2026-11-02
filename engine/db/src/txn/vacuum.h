#ifndef DB_TXN_VACUUM_H
#define DB_TXN_VACUUM_H

#include "common/error.h"
#include "common/types.h"
#include "storage/heap.h"
#include "txn/txn.h"

/*
 * vacuum —— 回收死版本
 *
 * 【为什么需要】
 *
 * MVCC 的代价：更新不覆盖旧版本，而是插新版本。所以一行被改 10 次，
 * 页里就有 10 个版本。不清理的话，表的物理大小会随着更新次数
 * 无限增长，而有效数据只有一份。
 *
 * 【死版本是什么】
 *
 * 一个版本是"死的"，当且仅当**没有任何事务**可能看到它。
 * 判据（保守版）：
 *   1. 插入者（xmin）已提交
 *   2. 删除者（xmax）已提交，或者没有删除者且该版本已被替换
 *   3. 当前没有任何活跃事务（保守：宁可多留垃圾，不可错删）
 *
 * 【v0.7 做什么、不做什么】
 *
 * 做：把死版本的 slot 置为删除（page_delete）→ 空间变成页内空洞。
 * 不做：页内 compact（把活记录压到页尾回收空洞）。
 *
 * 为什么不做 compact：compact 会移动记录位置 → RID 变化 → 索引失效。
 * PG 的做法是搬移 + 同步改索引（它的索引存 TID）。
 * 我们这里索引存的是 (page, slot)，搬移就得重写所有索引项，
 * 复杂度太高。v0.7 先做"标记删除"，页空间回收留给 v0.8。
 *
 * 【一个重要的教学点】
 *
 * 「slot 永不复用」这条铁律在这里依然成立 —— vacuum 删掉 slot 后，
 * 那个 slot 编号永久空着，不给新记录用。
 * 为什么？因为索引里可能还有指向这个 RID 的项（索引不写日志，
 * 重建前是旧的）。复用 slot 会让旧索引项指向新记录 —— 静默返回错数据。
 */

typedef struct {
    u64 scanned;      /* 扫描过的版本数 */
    u64 removed;      /* 回收的死版本数 */
    u64 kept;         /* 保留的活版本数 */
} VacuumStats;

/*
 * 对一张表做 vacuum。
 *
 * txn 是当前的事务表（用来判断哪些 xid 已提交、还有谁活跃）。
 *
 * xid 是"执行这次 vacuum 的事务号" —— vacuum 的删除动作本身也要写
 * WAL 日志，日志里的 xid 必须是真正干活的这个事务，
 * 不能是"某个共享状态里的当前值"（多连接下会串号）。
 *
 * 返回值：统计信息。错误码 E_* 表示失败。
 */
int  vacuum_table(Heap *h, TxnTable *txn, u32 xid, VacuumStats *stats);

#endif /* DB_TXN_VACUUM_H */
