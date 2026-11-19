/*
 * db_ft_txn.c —— FT 删除的**连接级事务接线**（W-D 的 D-4b）
 *
 * ==================== 这一层解决的缺陷 ====================
 *
 * 实测（D-4.1 的探针）发现的**真缺陷**：
 *
 *   BEGIN → db_ft_delete(5) → ROLLBACK → 同一会话里再检索 ⇒ **查不到**那篇文档
 *
 * 而关库重开之后又对了 —— 因为重开时集合是按 tombstone 页重建的，
 * 页里那一条已经被 WAL 的 undo 撤掉了。
 * 于是同一个会话前后两次查询给出不同答案。
 *
 * 【根因】
 *
 * 运行时 ROLLBACK（db_stmt.c 的 txn_end_abort）只动 txn 表（clog 标 ABORTED），
 * 没人去撤 FT 的两份状态：内存里的升序集合 ix->tomb、tombstone 页里的那一条。
 * 崩溃恢复那一路是对的（WAL 钩子按 REC_HOOK_UNDO 调 ft_tomb_page_remove），
 * 但**运行时**这一路从来没接到 FT 上。
 *
 * 【为什么记在连接上，不记在实例上】
 *
 * 实例（DbInstance）是多个连接共享的。把"本事务删过哪些"记在实例上，
 * A 的回滚会把 B 的删除一起撤掉 —— 比原缺陷更严重的数据丢失。
 * 连接（DB）才是"事务"的归属。
 *
 * 【为什么不只改内存集合】
 *
 * 只改内存不碰页 ⇒ checkpoint 会把那条删除刷进数据文件 ⇒ WAL 截断之后
 * 页成了唯一权威 ⇒ 下次开库它又"复活"成一次真删除。两份都必须撤。
 *
 * 【为什么不需要写一条新 WAL 记录表示"运行时撤销"】
 *
 * 那条 WAL_FT_TOMB_APPEND 本来就在日志里，它所属的事务马上被打成 ABORTED。
 * 崩溃恢复按 clog 判定该事务未提交 → 自动 undo。运行时先撤一遍只是
 * "提前做同样的事"，两条路径语义一致（test_ft_tomb_txn.c 第 4 项钉这个一致性）。
 *
 * 代价：运行时撤销改了页但**没有对应的日志记录**。崩溃在"已改页、未落盘"
 * 之间时，重开按 WAL 重放再 undo，结果仍正确；干净关闭时 checkpoint
 * 刷的是撤过的页，也正确。两头都对，所以不需要新记录。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>

#include "common/error.h"
#include "common/latch.h"
#include "modalities/ft/ft_index.h"

/* 记录"本事务删了这一篇"。只在显式事务里会记（不在事务里走系统事务、不可能回滚）。 */
int db_ft_txn_track(DB *db, u32 doc_id)
{
    u32 i;

    if (db == NULL)
        return E_INVAL;

    /*
     * ⚠️ 必须自己判重：ft_index_delete 的"已经删过"路径也返回 OK，
     * 调用方分辨不出来 ⇒ 同一篇可能被记两次。
     * 多记一条不影响正确性，但会让回滚多做一次无用功、把 n 撑大。
     */
    for (i = 0; i < db->ft_del.n; i++) {
        if (db->ft_del.ids[i] == doc_id)
            return OK;
    }
    return db_ft_replay_add(&db->ft_del, doc_id);
}

/* 清空记录（提交、以及关连接时都走这里） */
void db_ft_txn_clear(DB *db)
{
    if (db == NULL)
        return;
    db_ft_replay_free(&db->ft_del);
}

/*
 * 回滚：把本事务删过的每一篇都撤回去（页 + 内存集合）。
 *
 * 出错也**继续往下撤** —— 半途停下会留下"撤了一半"的状态，比全撤完再报错更难解释。
 * 返回第一个出错的错误码。
 */
int db_ft_txn_rollback(DB *db)
{
    DbInstance *inst;
    int first_rc = OK;
    u32 i;

    if (db == NULL)
        return E_INVAL;

    inst = db->inst;
    if (inst == NULL || !inst->ft_ready) {
        db_ft_txn_clear(db);        /* 没有 FT 索引 ⇒ 不可能有记录 */
        return OK;
    }

    /*
     * FT 的内部状态由全局 latch 保护（db_ft_delete 也是这么取锁的）。
     * latch 可重入，所以外层已持有它时也不会自锁。
     */
    latch_lock();
    for (i = 0; i < db->ft_del.n; i++) {
        int rc = ft_index_undelete(inst->ft, db->ft_del.ids[i]);

        if (rc != OK && first_rc == OK)
            first_rc = rc;
    }
    latch_unlock();

    db_ft_txn_clear(db);
    return first_rc;
}

/*
 * db_ft_delete 的实现（原样搬过来 + 一行记录）。
 *
 * 【为什么搬到这个文件】
 * db_ft.c 已经 796 行（硬规则 4 的上限是 800），再加行就要破线；
 * 而"删除 + 事务记录"本来就是一件事，放一起比分在两个文件更清楚。
 *
 * 【xid 怎么取】在事务里用当前事务号（崩溃恢复时未提交的删除会被撤销）；
 * 不在事务里用系统事务 xid=0（永远视为已提交）。
 */
db_err db_ft_delete_wired(DB *db, uint32_t doc_id)
{
    DbInstance *inst;
    u32 xid;
    int rc;

    if (!db) {
        return DB_E_INVAL;
    }
    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    xid = db->in_txn ? db->xid : 0u;

    latch_lock();
    rc = ft_index_delete(inst->ft, xid, doc_id);
    latch_unlock();

    if (rc != OK) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", "删除文档失败");
        return map_rc(rc);
    }

    /*
     * 在显式事务里删的，记下来 —— 万一这个事务回滚，要能撤回去（D-4b）。
     * 记录失败不改变"删除已生效"这个事实，所以只放弃回滚能力，不报错：
     * 报错会让调用方以为删除没生效，而实际页与日志都已经写了。
     */
    if (xid != 0u) {
        (void)db_ft_txn_track(db, doc_id);
    }
    return DB_OK;
}
