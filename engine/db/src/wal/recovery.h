#ifndef DB_WAL_RECOVERY_H
#define DB_WAL_RECOVERY_H

#include "common/error.h"
#include "common/types.h"
#include "storage/pager.h"
#include "wal/wal.h"

/*
 * 崩溃恢复：分析 → 重放 → 回滚
 *
 * 关键点：
 *   1. 重放必须**包含未提交事务**（ARIES 的核心洞察）。
 *      先全部重放到崩溃瞬间的状态，再统一回滚。
 *      只重放已提交的会让页状态不一致。
 *   2. 幂等靠 page_lsn：page_lsn >= record.lsn 就跳过。
 *   3. 不写 CLR：恢复过程中不刷页，中途再崩也是从头再来，
 *      而重放是幂等的，所以不需要补偿日志。
 *
 * did_recover 返回是否真的做了恢复（日志非空）。调用方据此决定
 * 要不要重建索引（v0.6 里索引不写日志）。
 */
int recovery_run(Pager *pg, Wal *w, bool *did_recover);

/*
 * 恢复阶段收集到的 xid 状态（供 clog 重建用）。
 *
 * v0.7 引入 MVCC 后，多了一个问题：**恢复后怎么知道哪些事务提交过？**
 *
 * 未提交事务的数据会被回滚掉（页里的字节恢复了），
 * 但页里那些 tuple 的 xmin/xmax 字段还在 —— 比如一条被回滚的 INSERT，
 * 回滚后 slot 变空（长度归零），tuple 本身没了，没影响。
 * 但 UPDATE 打的 xmax 标记，回滚后是**还原成 before 镜像**的，
 * 所以 before 镜像里的 xmax 也得是对的。
 *
 * 更关键的是：恢复后 clog 必须是准确的。
 * 如果 clog 说"xid=5 已提交"但实际没提交（或者反过来），
 * 重启后读到的数据就是错的。
 *
 * 所以恢复阶段顺手把结论交给调用方：
 *   - COMMIT 记录里的 xid → 已提交
 *   - 有数据记录但没 COMMIT 的 xid → 已中止（会被回滚）
 */
typedef struct {
    u32 xid;
    u8  committed;   /* 1 = 已提交，0 = 已中止 */
} RecoveredXid;

#define REC_MAX_XIDS 256

typedef struct {
    int         n;
    RecoveredXid xids[REC_MAX_XIDS];
} RecoveredState;

/*
 * 扫一遍日志，收集每个 xid 的提交状态。
 *
 * 单独一个函数，因为 db_open 需要在恢复**之前**就知道状态，
 * 好把 clog 建好 —— 否则恢复过程中做可见性判断就没依据了。
 */
int recovery_scan_xids(Wal *w, RecoveredState *out);

#endif /* DB_WAL_RECOVERY_H */
