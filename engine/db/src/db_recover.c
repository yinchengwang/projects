/*
 * db_recover.c —— 开库时的"恢复阶段"（L2）
 *
 * ==================== 为什么单独一个文件 ====================
 *
 * `db.c` 承担的是"公开 API 的实现"：`db_open` / `db_connect` / `db_close`。
 * 其中 `db_open` 有 570 行，里头是两件**互不相干**的事：
 *
 *   1. 恢复（本文件）：开 WAL → 陈旧日志处置 → clog 初始化 → 从日志重建 clog
 *      → 读 checkpoint_lsn → recovery_run
 *   2. 模态注册（留在 db.c）：catalog → blob → graph → vector → ft → geo → ts
 *
 * 这两件事的失败语义、坑、注释密度都不一样，混在一个函数里会让
 * "恢复阶段的顺序铁律"（坑 M4-48）淹没在"某个模态怎么打开"里。
 *
 * 拆出来之后，`db_open` 读起来就是一条主线：
 *   开 pager → 开实例 → **恢复** → 注册模态 → 建快照 → 完。
 *
 * ==================== 顺序铁律（坑 M4-48） ====================
 *
 * 本文件里的每一条失败路径都必须 **先 pager_close、再 wal_close**。
 * 反过来会踩 use-after-free（pager_close 刷脏页 → 触发 WAL 钩子 →
 * wal_fsync → 但 wal 已经 free 掉了）。
 *
 * ⚠️ 本文件**不做**清理 —— 清理由调用方（db.c 的 open_fail_cleanup）做。
 *    本文件只负责"恢复这一步"的成败，失败时把已开的 WAL/clog 状态
 *    留给调用方按 M4-48 的顺序关。
 *
 * ==================== 恢复做什么 ====================
 *
 * 恢复不是"回滚"那么简单，它有三件事，缺一不可：
 *
 *   1. **重放**（redo）：把已提交事务的页镜像盖回页里
 *   2. **回滚**（undo）：把未提交事务的页镜像还原
 *   3. **重建 clog**：把"哪些事务提交了"这条信息补齐
 *      —— 没有它，重启后所有已提交的数据都看不见
 *
 * 第 3 件事最容易被忽略：日志会被 checkpoint 截断，
 * 提交记录截掉之后，可见性判断就只剩 clog 文件可依。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "wal/recovery.h"

/*
 * 恢复阶段的入口。
 *
 * path        —— 数据文件路径（日志与 clog 的路径由它派生）
 * datafile_exists —— 数据文件开库前是否存在（区分"全新库"与"崩溃的库"）
 * pg / inst / db —— 已开好的 pager / 实例 / 连接（由调用方准备）
 * did_recover_out —— 本次是否真的重放过（调用方据此决定要不要重建索引）
 *
 * 返回 OK 表示恢复成功；失败时**不清理**任何资源（由调用方按 M4-48 清理）。
 */
int db_recover_stage(const char *path, bool datafile_exists,
                     Pager *pg, DbInstance *inst, DB *db,
                     bool *did_recover_out)
{
    int rc;
    size_t plen = strlen(path);
    bool did_recover = false;

    if (!path || !pg || !inst || !db)
        return E_INVAL;

    /* 日志文件：<db>-wal */
    inst->walpath = malloc(plen + 8);
    if (!inst->walpath)
        return E_IO;
    snprintf(inst->walpath, plen + 8, "%s-wal", path);

    rc = wal_open(inst->walpath, &inst->wal);
    if (rc != OK)
        return rc;

    /*
     * 陈旧日志检查。
     *
     * 数据文件**不存在**（全新库），但同名日志文件里有内容 ——
     * 这说明日志是上一个同名数据库的孤儿。数据文件都没了，日志留着只会
     * 把不属于这个库的记录重放进来，把新库搞脏。
     *
     * 这种情况在测试里特别容易出现：remove("x.db") 忘了 remove("x.db-wal")。
     *
     * 【注意：只看"文件是否存在"，不看"页数是不是 0"】
     *
     * 如果按页数判断，就会误伤这一种情况：
     *   上次会话建了库、写了数据，但在第一次刷页之前就崩了（断电）。
     *   此时数据文件存在但是 0 字节，而 WAL 里全是这个库的合法日志。
     *   按页数判断会把 WAL 当成孤儿清掉 → 数据全丢。
     *
     * 判断依据是"文件在不在"：被 remove 掉的库，文件不存在；
     * 崩溃的库，文件存在（哪怕是 0 字节，因为 pager_open 创建了它）。
     */
    if (!datafile_exists && wal_nrecords(inst->wal) > 0) {
        rc = wal_reset(inst->wal);
        if (rc != OK)
            return rc;
    }

    /*
     * 事务号必须接着日志里的最大 xid 往下走。
     *
     * 跨会话重置 xid 是隐蔽的数据错误：上次会话做到 xid=5 崩了，
     * 这次会话又从 xid=1 开始。恢复阶段判断"哪些事务提交了"时，
     * 上次那个未提交的 xid=1 会被这次新的 xid=1 提交记录"连累"，
     * 判定成已提交 —— 未提交的数据就留在了库里。
     */
    u32 max_xid = wal_max_xid(inst->wal);

    /*
     * 初始化事务表。
     *
     * 必须在 clog_open 之前 —— txn_table_init 会把分块 clog 的块指针表清空，
     * 而 clog_open 要往里填文件的内容。
     */
    txn_table_init(&inst->txns, max_xid);

    /*
     * clog 文件（v1.2 步骤 6）：<db>-clog
     *
     * 【为什么需要它】
     * "哪些事务提交过"这条信息必须有个**不在日志里**的存放处 ——
     * 因为步骤 6 的 checkpoint 会截断日志，
     * 提交记录被截掉之后，可见性判断就只剩这个文件可依。
     *
     * 【陈旧 clog 的处置，和陈旧日志一样】
     * 数据文件不存在（全新库）→ 同名 clog 一定是上一个同名库的孤儿 → 删掉。
     * 反过来（数据文件存在但 clog 不存在）→ 那是"老版本建的库"，
     * 从日志重建就行（下面就是这么做的）。
     */
    inst->clogpath = malloc(plen + 10);
    if (!inst->clogpath)
        return E_IO;
    snprintf(inst->clogpath, plen + 10, "%s-clog", path);

    if (!datafile_exists)
        remove(inst->clogpath);       /* 孤儿 clog：删掉（不存在也不报错）*/

    rc = txn_clog_open(&inst->txns, inst->clogpath);
    if (rc != OK)
        return rc;

    /*
     * xid 还要和 clog 文件里的最大 xid 取大。
     *
     * 日志被截断之后，日志里的最大 xid 会变小 ——
     * 真正"用过的最大 xid"记在 clog 文件里。
     * 不取大的话，新会话会重用旧 xid → 旧事务的提交状态被覆盖
     * → 可见性判断错乱。
     */
    txn_table_bump_xid(&inst->txns, txn_clog_max_xid(&inst->txns));

    /*
     * 从日志重建 clog（补上 clog 文件里没有的部分）。
     *
     * 为什么必须重建：MVCC 可见性判断完全依赖 clog —— "这个 xid 提交了没有"。
     * 如果重启后 clog 是空的，所有历史事务都会被当成"进行中"，
     * 于是重启后**所有已提交的数据都看不见了**。
     *
     * 两份信息的分工：
     *   clog 文件 —— checkpoint 之前的历史（提交记录可能已被截断）
     *   日志      —— checkpoint 之后的事务（提交记录还在日志里）
     */
    RecoveredState xs;
    recovered_state_init(&xs);
    rc = recovery_scan_xids(inst->wal, &xs);
    if (rc != OK) {
        recovered_state_free(&xs);
        return rc;
    }
    for (u32 i = 0; i < xs.cap; i++) {
        if (xs.slots[i].xid == 0)
            continue;               /* 空槽 */
        u32 xid = xs.slots[i].xid;
        switch (xs.slots[i].state) {
        case REC_XID_COMMITTED:
            txn_clog_set(&inst->txns, xid, TXN_CLOG_COMMITTED);
            break;
        case REC_XID_ABORTED:
            txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        case REC_XID_ABORT_DONE:
            /* 恢复阶段已经回滚完了 → 同样是"中止" */
            txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        default:
            /*
             * 未知：日志里有它的数据记录，但没看到提交/中止记录。
             *
             * 两种可能：
             *   (a) 它在崩溃时还没结束 → 该按"中止"处理（会被回滚）
             *   (b) 它的提交记录已经被 checkpoint 截掉了 → 它其实是已提交的
             *
             * 怎么区分：看 clog 文件。clog 文件是 checkpoint 时同步落盘的，
             * 提交状态在那儿是权威的 —— 文件说"已提交"就信它。
             */
            if (txn_clog_state(&inst->txns, xid) != TXN_CLOG_COMMITTED)
                txn_clog_set(&inst->txns, xid, TXN_CLOG_ABORTED);
            break;
        }
    }
    recovered_state_free(&xs);

    /*
     * 恢复起点：checkpoint_lsn（记在 meta 页里）。
     *
     * 必须在 catalog_open **之前**读 —— 那时 catalog 还没打开，
     * 而 meta 页的布局常量是公开的（catalog.h），可以直接读。
     *
     * 读到的值可能比日志里的实际情况旧（meta 页没来得及刷盘），
     * 那就多重放一遍 —— 无害。反过来（新值比日志新）不可能发生：
     * meta 页落盘之前它的日志记录必须先落盘（WAL 规则）。
     */
    u32 ckpt_lsn = 0;
    if (pager_npages(pg) > 0) {
        PageFrame *mf;
        if (pager_get(pg, PAGE_ID_META, &mf) == OK) {
            if (get_u32(mf->data + M_MAGIC) == CAT_MAGIC)
                ckpt_lsn = get_u32(mf->data + M_CKPT_LSN);
            pager_unpin(pg, PAGE_ID_META, false);
        }
    }

    /*
     * 恢复必须在打开 catalog 之前：catalog 也是普通表，也要恢复。
     *
     * 【全文检索的恢复钩子】（v1.3 步骤 5）
     *
     * WAL 层不认识 FT 语义（wal/ 是 L1，不许 include modalities/），
     * 所以恢复阶段遇到**非页类**记录（逻辑记录）时，
     * 由钩子把 (type, xid, payload) 转交给 db 层解释。
     *
     * 钩子是**进程级**的一份（recovery_set_hook 是全局状态），
     * 所以注册与注销必须配对：注册 → recovery_run → 注销。
     */
#if DB_HAS_FT
    recovery_set_hook(db_ft_recovery_hook, inst);
#endif
    rc = recovery_run(pg, inst->wal, ckpt_lsn, &did_recover);
    recovery_set_hook(NULL, NULL);
    if (rc != OK) {
#if DB_HAS_FT
        db_ft_replay_free(&inst->ft_replay_redo);
        db_ft_replay_free(&inst->ft_replay_undo);
#endif
        return rc;
    }

    if (did_recover_out)
        *did_recover_out = did_recover;
    return OK;
}
