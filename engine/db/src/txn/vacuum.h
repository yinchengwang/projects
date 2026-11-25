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
 * ==================== v1.2 步骤 5 的更正 ====================
 *
 * 上面那段"compact 会让 RID 变化"的判断**是错的** ——
 * 我们的 `page_compact` 只改**物理位置**，slot 编号不变，
 * 所以 RID = (page, slot) 稳定，索引与游标都不受影响。
 *
 * v1.2 步骤 5 把 compact 接上了（见 vacuum.c 里的说明）。
 *
 * 【一个重要的教学点】
 *
 * 「slot 永不复用」这条铁律在这里依然成立 —— vacuum 删掉 slot 后，
 * 那个 slot 编号永久空着，不给新记录用。
 * 为什么？因为索引里可能还有指向这个 RID 的项（索引不写日志，
 * 重建前是旧的）。复用 slot 会让旧索引项指向新记录 —— 静默返回错数据。
 */

typedef struct {
    u64 scanned;         /* 扫描过的版本数 */
    u64 removed;         /* 回收的死版本数 */
    u64 kept;            /* 保留的活版本数 */
    u64 compacted_pages; /* 整理过（页内 compact）的页数 */
    u64 freed_bytes;     /* 页内 compact 回收的空间（字节） */
    u64 frag_pct;        /* 整理**前**的全表碎片率（百分比） */
    u64 json_chains;     /* 回收的 JSON 溢出页链数（C5.3b-b） */
} VacuumStats;

/* ---------------- JSON 溢出页链的回收回调（C5.3b-b Task 5） ---------------- */

/*
 * 为什么是回调而不是直接调用
 *
 * txn/ 是 L2，`modalities/doc/json_ovf.h` 是 L4。`#include` 它就是
 * L2 → L4 的反向依赖，`tools/check_deps.sh` 会直接红。
 *
 * 而且"12 字节溢出标记在哪一列的变长区里"这件事 txn 层根本不知道 ——
 * 那要 schema（列定义）。schema 只有 heap AM（modalities/relational）有。
 *
 * 所以 txn 层只负责**时序**（什么时候读标记、什么时候释放、什么时候删 slot），
 * "怎么从一条记录里找出链首页"和"怎么把链还给分配器"都由调用方注入。
 */

/*
 * 从一条 tuple 记录里取出所有 JSON 溢出链的首页。
 *
 * rec/len —— 这条记录的字节（**仍然可读**时才调）
 * heads   —— 输出数组，每项一条链的首页页号
 * cap     —— heads 的容量（调用方按表的 JSON 列数给）
 * n_out   —— 实际写出几个
 *
 * 返回 OK 表示解析成功（没有 JSON 链也是 OK，n_out = 0）。
 * 返回值不是 OK ⇒ 这条 tuple 的链这次不回收（宁可留垃圾，不可错还页）。
 */
typedef int (*VacuumJsonHeadsFn)(void *ctx, const u8 *rec, u16 len,
                                 u64 *heads, u32 cap, u32 *n_out);

/*
 * 把一条链还给页分配器。
 *
 * 实现方（关系 AM）必须在**持有表级写锁**的情况下调用真正的释放，
 * 否则可能与正在读这条链的查询撞上（见 common/json_ovf_lock.h）。
 *
 * 幂等由释放实现本身保证：重复释放同一条链不得重复还页。
 */
typedef int (*VacuumJsonChainFreeFn)(void *ctx, u64 head);

/*
 * 对一张表做 vacuum。
 *
 * txn 是当前的事务表（用来判断哪些 xid 已提交、还有谁活跃）。
 *
 * xid 是"执行这次 vacuum 的事务号" —— vacuum 的删除动作本身也要写
 * WAL 日志，日志里的 xid 必须是真正干活的这个事务，
 * 不能是"某个共享状态里的当前值"（多连接下会串号）。
 *
 * json_heads / json_heads_ctx、json_free / json_free_ctx 为 NULL 表示
 * 这张表没有 JSON 溢出链（或调用方不关心）—— 行为与改造前完全一致。
 *
 * 返回值：统计信息。错误码 E_* 表示失败。
 */
int  vacuum_table(Heap *h, TxnTable *txn, u32 xid, VacuumStats *stats,
                  VacuumJsonHeadsFn json_heads, void *json_heads_ctx,
                  VacuumJsonChainFreeFn json_free, void *json_free_ctx);

#endif /* DB_TXN_VACUUM_H */
