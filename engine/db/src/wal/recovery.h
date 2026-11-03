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
 *
 * ==================== checkpoint_lsn（v1.2 步骤 6） ====================
 *
 * checkpoint_lsn = "这个 LSN 之前的日志记录都不用重放了"。
 * 它记在 meta 页里（见 catalog.h 的 M_CKPT_LSN）。
 *
 * 【重放按它过滤，回滚不按它过滤 —— 这两者的理由不同】
 *
 *   重放：lsn <= checkpoint_lsn 的记录造成的效果**已经在数据文件里**
 *         （checkpoint 时刷过页了）→ 重放是多余的。
 *
 *   回滚：未提交事务的记录可能横跨 checkpoint 点
 *         （事务在 checkpoint 时还开着，它的日志记录一部分在点前、一部分在点后）。
 *         这些记录的前像（before image）是回滚的依据，
 *         哪怕 lsn <= checkpoint_lsn 也必须留着、必须回滚。
 *
 * 所以回滚阶段遍历**全部**记录，不按 lsn 过滤。
 */
int recovery_run(Pager *pg, Wal *w, u32 checkpoint_lsn, bool *did_recover);

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
    u8  state;     /* REC_XID_UNKNOWN / REC_XID_COMMITTED / REC_XID_ABORTED */
} RecoveredXid;

/* 恢复阶段扫出来的 xid 状态 */
#define REC_XID_UNKNOWN   0u   /* 只有数据记录，没看到提交/中止 */
#define REC_XID_COMMITTED 1u   /* 看到了 WAL_COMMIT */
#define REC_XID_ABORTED   2u   /* 看到了 WAL_ABORT（回滚还没做）*/
#define REC_XID_ABORT_DONE 3u  /* 看到了 WAL_ABORT_DONE（已回滚完，日志可扔）*/

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
 *
 * ==================== 为什么状态是三态而不是两态 ====================
 *
 * v1.2 步骤 6 之前是二态："有 COMMIT 记录 → 已提交，否则 → 已中止"。
 *
 * 两态在"日志被截断"之后就不够用了：
 * 一个已提交的事务，它的 COMMIT 记录可能**已经被截掉**
 * （提交发生在 checkpoint 之前）。这时候日志里只剩它的数据记录
 * ——按两态就会被判成"已中止"→ 它的行在恢复后不可见 → 数据丢。
 *
 * 所以改成三态：只看到数据记录时是"未知"，
 * 由调用方拿 clog 文件里的权威状态来定夺
 * （见 db.c 里的合并逻辑）。
 */
int recovery_scan_xids(Wal *w, RecoveredState *out);

#endif /* DB_WAL_RECOVERY_H */
