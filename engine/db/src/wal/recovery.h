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

/*
 * xid 状态表：开放寻址哈希表（槽位数是 2 的幂）。
 *
 * ==================== 为什么不能是固定长度的数组 ====================
 *
 * v0.7 这里是 `RecoveredXid xids[256]` —— 固定 256 项。
 * 事务一多，**后面的 xid 状态直接丢掉**：
 *
 *   扫日志时发现表满了 → `continue` → 这个 xid 既没记"已提交"
 *   也没记"已中止" → clog 里它是"进行中"。
 *
 * 后果是**静默的数据错误**（v1.2 步骤 5 的集成测试撞出来的）：
 * 一个库跑了 450 个事务（300 插入 + 150 删除），重启后：
 *   - 关闭前 COUNT(*) = 150
 *   - 重启后 COUNT(*) = 255      ← 删掉的行又"活"了
 *
 * 因为"删掉了哪些行"这件事记在 xmax 里，而 xmax 的那个 xid
 * 的提交状态被丢了 → clog 说"进行中" → 可见性判断认为这行还活着。
 *
 * 所以这里改成**按需增长的哈希表**：事务多少都不会丢状态。
 * 真正的容量上限在别处 —— clog 本身是个 4096 槽的环形窗口
 * （见 txn.h），xid 跨度超过 4096 时老状态会被覆盖，
 * 那是另一个问题，属于 v1.2 步骤 6（日志截断 + clog 落盘）的范围。
 */
typedef struct {
    RecoveredXid *slots;   /* 开放寻址表；xid == 0 表示空槽 */
    u32 cap;               /* 槽位数（2 的幂） */
    u32 n;                 /* 已占用的槽位数 */
} RecoveredState;

void recovered_state_init(RecoveredState *st);
void recovered_state_free(RecoveredState *st);

/*
 * 扫一遍日志，收集每个 xid 的提交状态。
 *
 * 单独一个函数，因为 db_open 需要在恢复**之前**就知道状态，
 * 好把 clog 建好 —— 否则恢复过程中做可见性判断就没依据了。
 */
int recovery_scan_xids(Wal *w, RecoveredState *out);

#endif /* DB_WAL_RECOVERY_H */
