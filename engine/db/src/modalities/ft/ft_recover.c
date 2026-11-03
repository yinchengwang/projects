/*
 * ft_recover.c —— 崩溃恢复的编排（v1.3 步骤 5）
 *
 * 【恢复要做两件事】
 *
 *   1. 段目录：从磁盘把"还活着的段"找齐（ft_catalog.c）；
 *   2. tombstone 集合：把"被删的 doc_id"找齐（ft_tomb.c）。
 *
 * 两件事都得**扫页空间**才能做 —— 因为段目录与 tombstone 集合
 * 都没有"根页"（坑 M4-20 / M4-22）。
 *
 * 【tombstone 的最终集合是怎么算出来的】
 *
 *   最终集合 = 页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)
 *
 *   - 页里的内容是**权威**的：它不会被 checkpoint 截断（坑 B）；
 *   - WAL 那一路补的是"页还没落盘、日志里已经记了"的删除；
 *   - 减掉"撤销"的那一份：未提交事务的删除 = 这个删除没发生过。
 *
 * 【谁调用它】
 *
 * db 层：db_open → 注册恢复钩子 → recovery_run（钩子把 WAL 里的
 * tombstone 记录收进 redo / undo 两个集合）→ ft_index_recover。
 */

#include "modalities/ft/ft_index.h"

#include <stdlib.h>

#include "common/error.h"
#include "modalities/ft/ft_index_internal.h"

/*
 * 从磁盘恢复索引。
 *
 * redo_ids / n_redo —— 恢复重放时看到的删除（WAL 里的 tombstone 记录）。
 * undo_ids / n_undo —— 恢复回滚时撤销的删除（未提交事务的）。
 *
 * 两个集合都可以是空的（没有 FT 数据的新库就是这种情况）。
 */
int ft_index_recover(FtIndex *ix, const u32 *redo_ids, u32 n_redo,
                      const u32 *undo_ids, u32 n_undo)
{
    int rc;

    if (ix == NULL) {
        return E_INVAL;
    }

    /* 第一步：段目录（扫页空间，把活着的段打开） */
    rc = ft_catalog_recover(ix);
    if (rc != OK) {
        return rc;
    }

    /* 第二步：tombstone 集合（页里的内容 ∪ (重放 − 回滚)） */
    rc = ft_tomb_assemble(ix, redo_ids, n_redo, undo_ids, n_undo);
    if (rc != OK) {
        return rc;
    }
    return OK;
}
