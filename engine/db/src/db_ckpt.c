/*
 * db_ckpt.c — checkpoint + 日志截断（v1.2 步骤 6）
 *
 * ==================== 这一步要解决什么 ====================
 *
 * v0.7 到 v1.2 步骤 5，日志只有两个状态：空的，或者一堆。
 * 恢复阶段要把**整个日志**从头扫一遍。
 *
 * 这意味着：库跑得越久，日志越大，重启越慢 ——
 * 而且慢得没有上限。日志 1 GB，恢复就要扫 1 GB。
 *
 * checkpoint 的作用就是把这件事变成有界的：
 *
 *   "这个 LSN 之前的日志记录，都已经反映在数据文件里了，可以扔掉。"
 *
 * 于是恢复只需要处理 checkpoint 之后的日志 ——
 * 恢复时间只取决于**上次 checkpoint 之后写了多少**，与库的总大小无关。
 *
 * ==================== 四个必须同时成立的前提 ====================
 *
 * 1. **页已经落盘**
 *    被截掉的记录造成的效果必须在数据文件里。
 *    判据：脏页的最小 recLSN（见 pager_min_dirty_lsn）。
 *
 * 2. **提交状态已落盘**
 *    "哪些事务提交过"记在 clog 文件里，不在日志里。
 *    日志一截，这个信息就只剩 clog 文件了 ——
 *    所以必须先 fsync clog，再截日志。顺序反了 = 静默丢数据。
 *
 * 3. **没有需要回滚的事务**
 *    未提交事务的记录是**回滚的依据**（before 镜像）。
 *    截掉了就没法回滚 —— 被物理删掉的行会永久消失。
 *
 * 4. **LSN 不能倒退**
 *    日志被截空之后重开库，last_lsn 会算成 0，
 *    新记录的 LSN 从 1 重新开始 —— 而 checkpoint_lsn 可能是 5000，
 *    于是新记录全部落在 checkpoint_lsn 之下，恢复时被跳过 → 数据丢。
 *    （wal_truncate 内部保证至少留一条。）
 *
 * ==================== 为什么要单独一个文件 ====================
 *
 * db.c 已经 738 行（硬规则：单文件不超过 800 行）。
 * checkpoint 是一块完整的功能（判断阈值 + 刷页 + 写 meta + 落 clog + 截日志），
 * 单独成文件正好。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------- 触发阈值 ---------------- */

/*
 * 日志超过这么多字节就自动做一次 checkpoint。
 *
 * 默认 16 MB：足够大（不会频繁停顿），又足够小
 * （恢复时最多要扫 16 MB 的日志）。
 */
#define DB_CKPT_BYTES_DEFAULT (16u * 1024u * 1024u)

/*
 * 阈值可以用环境变量改。
 *
 * 没有配置文件机制（db_open 只有 path 一个参数），
 * 环境变量是唯一不破坏现有 API 的办法。
 * 下限 4 KB（比它还小的阈值会让 checkpoint 疯狂触发），
 * 上限 1 GB（再大就没有截断的意义了）。
 */
static u64 ckpt_threshold(void)
{
    const char *e = getenv("DB_CKPT_BYTES");
    if (e && e[0]) {
        long v = strtol(e, NULL, 10);
        if (v >= 4096L && v <= 1073741824L)
            return (u64)v;
    }
    return DB_CKPT_BYTES_DEFAULT;
}

/* ---------------- 截断前提：日志里的事务是不是都已提交 ---------------- */

/*
 * 日志里出现过的每个 xid，是不是都已经"结束了"？
 *
 * 【为什么这是截断的前提】
 *
 * 未提交事务的日志记录是**回滚的依据**（before 镜像）。
 * 我们的删除是**物理删除**（页里的 slot 直接清掉），
 * 所以回滚不做的话，那一行就永久消失了 ——
 * 即使那个事务后来中止了。
 *
 * 所以只有"日志里的事务全都结束了"才能截断。
 *
 * ==================== "结束了"有三种，只有两种算数 ====================
 *
 *   REC_XID_COMMITTED   已提交 → 它的记录不需要回滚 → 可以扔
 *   REC_XID_ABORT_DONE  恢复阶段已回滚完 → 前像已用掉 → 可以扔
 *   REC_XID_ABORTED     运行时中止，**回滚还没做** → 前像还得留着 → 不能扔
 *   REC_XID_UNKNOWN     只有数据记录，没看到结束标记 → 不能扔
 *
 * 顺带也覆盖了"有活跃事务"的情况：
 * 活跃事务在日志里只有 BEGIN 记录（没有 COMMIT）→ 这里就会拦下。
 */
static bool wal_all_committed(Wal *w, bool *ok)
{
    if (ok)
        *ok = false;
    if (!w)
        return false;

    RecoveredState xs;
    recovered_state_init(&xs);
    int rc = recovery_scan_xids(w, &xs);
    if (rc != OK) {
        recovered_state_free(&xs);
        return false;
    }

    bool all = true;
    for (u32 i = 0; i < xs.cap; i++) {
        if (xs.slots[i].xid == 0)
            continue;               /* 系统事务（bootstrap / checkpoint），永远已提交 */
        if (xs.slots[i].state != REC_XID_COMMITTED &&
            xs.slots[i].state != REC_XID_ABORT_DONE) {
            all = false;
            break;
        }
    }
    recovered_state_free(&xs);

    if (ok)
        *ok = true;
    return all;
}

/*
 * 公开版本：关库时判断"能不能写干净关闭标记"用。
 */
bool db_wal_all_committed(Wal *w, bool *ok)
{
    return wal_all_committed(w, ok);
}

/* ---------------- 主流程 ---------------- */

/*
 * 做一次 checkpoint。
 *
 * allow_truncate = false 时只刷页、不截日志
 * （还有未提交事务时的"保守模式"）。
 */
int db_checkpoint_internal(DbInstance *inst, bool allow_truncate)
{
    if (!inst)
        return E_INVAL;

    /*
     * 1. 先把日志刷到盘上。
     *
     * WAL 规则："页刷盘前，对应的日志必须已落盘"。
     * 先刷日志，后面刷页才不会踩规则。
     */
    int rc = wal_fsync(inst->wal);
    if (rc != OK)
        return rc;

    /*
     * 2. 记下此刻的日志末尾。
     *
     * 它是重放点的上界：如果之后没有新记录，重放点就是它。
     * 必须在刷页**之前**读 —— 因为"日志末尾"和"页在盘上"这两件事
     * 之间可能又冒出新记录。
     */
    u32 L = wal_last_lsn(inst->wal);

    /*
     * 3. 刷所有脏页。
     *
     * 刷完之后，脏页表里剩下的只有"刷页过程中又被改过的页"。
     */
    rc = pager_flush_all(inst->pager);
    if (rc != OK)
        return rc;

    /*
     * 4. 算重放点。
     *
     * d = 脏页里最小的 recLSN = "最早一条还没落盘的记录"的 LSN。
     * 那条记录必须重放 → 重放点必须 < d。
     *
     * 没有能约束的脏页 → 日志末尾之前的记录全都在盘上了 → 重放点 = L。
     */
    u32 d = pager_min_dirty_lsn(inst->pager);
    u32 old_ckpt = catalog_checkpoint_lsn(inst->cat);
    u32 ckpt_lsn;
    if (d > 0)
        ckpt_lsn = d - 1u;            /* d 那条还要重放，所以重放点必须比它小 */
    else
        ckpt_lsn = L;

    /*
     * 5. 把重放点写进 meta 页。
     *
     * meta 页的整页镜像会进日志，所以这个值本身是崩溃安全的。
     */
    if (ckpt_lsn > old_ckpt) {
        rc = catalog_set_checkpoint_lsn(inst->cat, 0, ckpt_lsn);
        if (rc != OK)
            return rc;

        /*
         * 6. meta 页要落盘，它的日志必须先落盘（WAL 规则）。
         *
         * 顺序反了：meta 页带着"新的 checkpoint_lsn"上了盘，
         * 而对应的日志记录还没落盘 → 崩溃后这个值回滚成旧的
         * → 恢复起点变老 → 多重放一遍（无害，但白费功夫）。
         * 反过来才是对的：新值要么完整落盘，要么根本没上去。
         */
        rc = wal_fsync(inst->wal);
        if (rc != OK)
            return rc;
        rc = pager_flush_all(inst->pager);
        if (rc != OK)
            return rc;
    }

    /*
     * 7. clog 落盘。
     *
     * 【必须在截断日志之前】
     * 提交状态记在 clog 文件里。日志一截，
     * "哪些事务提交过"就只剩 clog 文件了。
     * 顺序反了 = 提交状态随日志一起消失
     * → 重启后已提交的数据全部不可见（静默丢数据）。
     */
    rc = txn_clog_sync(&inst->txns);
    if (rc != OK)
        return rc;

    /*
     * 8. 截断日志。
     *
     * 前提：日志里的事务全都提交了（否则回滚依据没了）。
     */
    if (allow_truncate && ckpt_lsn > old_ckpt) {
        bool ok = false;
        bool all = wal_all_committed(inst->wal, &ok);
        if (ok && all) {
            rc = wal_truncate(inst->wal, ckpt_lsn);
            if (rc != OK)
                return rc;
        }
    }
    return OK;
}

/*
 * 阈值触发的 checkpoint：日志太大了就顺手做一次。
 *
 * 放在语句结束之后 —— 那时页面的改动已经写进日志并提交了，
 * 是做 checkpoint 的好时机。
 */
void db_checkpoint_maybe(DbInstance *inst)
{
    if (!inst)
        return;
    if (wal_bytes(inst->wal) > ckpt_threshold())
        (void)db_checkpoint_internal(inst, true);
}

/* ---------------- 公开 API ---------------- */

db_err db_checkpoint(DB *db)
{
    if (!db)
        return DB_E_INVAL;
    int rc = db_checkpoint_internal(db->inst, true);
    if (rc != OK)
        return map_rc(rc);
    return DB_OK;
}
