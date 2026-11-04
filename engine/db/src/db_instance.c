/*
 * db_instance.c — 实例与连接的关闭路径（include/db.h）
 *
 * 【为什么要拆出来】
 *
 * 硬规则：单文件不超过 800 行。db.c 一度到 888 行。
 * 这一块（关库 / 关连接 / 断电）和开库、结果集访问器互不相干，
 * 单独成文件正好 —— 而且它和 db_open 的**顺序**要求必须写在同一个文件里，
 * 才看得见（先刷页再写 CHECKPOINT、checkpoint 在 db_vec_close 之前）。
 *
 * 【实例与连接的关系】
 *
 * 实例（DbInstance）是共享的：缓冲池、catalog、WAL、事务表、模态注册表。
 * 连接（DB）每人一份：当前事务号、快照、错误信息。
 *
 * 用引用计数：最后一个连接关掉时才真正关库。
 * 下面的 db_close / db_abandon 都在 latch 保护下动这个计数。
 */

#include "db_internal.h"

#include <stdlib.h>

#include "common/latch.h"

/* 真正关闭实例：刷页、写 CHECKPOINT、关 WAL */
static void instance_close(DbInstance *inst)
{
    /*
     * ==================== v1.2 步骤 6：关库 = 一次完整 checkpoint ====================
     *
     * 关库时做 checkpoint 有两个好处：
     *   1. 日志被截断 → 下次启动几乎不用恢复
     *   2. clog 文件落盘 → 提交状态不会丢
     *
     * 【顺序：checkpoint 在 db_vec_close 之前】
     *
     * 向量索引是 checkpoint 式的（不写日志）。
     * 它写完页之后那些页是脏的、rec_lsn 是 0 ——
     * 如果 checkpoint 在它之后跑，那些页就"不约束重放点"，倒也没问题；
     * 但如果 checkpoint 在它之前跑，索引页就是干净的，不影响重放点的计算。
     *
     * 两种顺序都能work，但"checkpoint 在前"更保守：
     * checkpoint 算出来的重放点只涉及**有日志**的页。
     */
    (void)db_checkpoint_internal(inst, true);

    /*
     * 向量索引先落盘。
     *
     * 索引是 checkpoint 式的：不落盘的话，下次开库要重建一遍
     * （读全部向量 + 重新构建）。而且这一步要写页，
     * 所以必须在 pager_close **之前**。
     */
    db_vec_close(inst);

    /*
     * 全文索引：释放内存里的段清单与 tombstone 集合。
     *
     * 必须在 pager_close **之前** —— 段持有 pager 的引用，
     * 先关 pager 再释放段就是访问已释放的东西。
     *
     * 本版的索引**不落盘**（段目录页是步骤 5 的事），
     * 所以这里没有"flush"这一步 —— 段的内容在数据文件里，
     * 丢的只是"清单"。
     */
    db_ft_close(inst);

    /*
     * 地理空间模态（v1.4）。
     *
     * geo 不持有页（页归 pager 管），所以顺序上 pager_close 之前或之后都能跑；
     * 放在这里是为了和 db_vec_close / db_ft_close 保持同一处、同一套释放次序。
     *
     * geo 的页是**普通脏页**，pager_close 会把它们刷盘 ⇒
     * **数据不依赖这个顺序**；把 GeoState 的内存放在 pager 之前释放更干净。
     */
    db_geo_close(inst);

    /*
     * 关闭顺序很重要：**先刷页，再写 CHECKPOINT**。
     *
     * 反过来的话：CHECKPOINT 先落盘，然后刷页时崩溃了 ——
     * 恢复时看到最后一条是 CHECKPOINT，就认为"上次是干净关闭"，
     * 不做任何重放。可实际上有些页还没落盘，数据就丢了。
     *
     * 正确顺序：
     *   1. 刷所有脏页（此时 WAL 钩子会保证日志先落盘）
     *   2. 写 CHECKPOINT 记录并 fsync
     *   3. 之后再有崩溃也不怕：所有页都已经在盘上了
     */
    pager_close(inst->pager);

    /*
     * "干净关闭"标记 —— 只在日志里的事务全都提交了时才写。
     *
     * 这个标记的含义是"上次是干净关闭，不用重放"。
     * 如果还有没提交的事务却写了标记，恢复阶段会跳过重放**和回滚**
     * → 那个事务物理删掉的行就永久消失了。
     */
    bool ok = false;
    if (db_wal_all_committed(inst->wal, &ok) && ok) {
        wal_append_xid(inst->wal, WAL_CHECKPOINT, 0, PAGE_ID_NIL, 0, 0,
                       NULL, 0, NULL, 0);
        wal_fsync(inst->wal);
    }
    wal_close(inst->wal);

    /* clog 文件最后关 —— 关之前会再 sync 一次 */
    txn_clog_close(&inst->txns);

    catalog_close(inst->cat);
    blob_store_destroy(inst->blobs);
    graph_destroy(inst->graph);
    spi_registry_destroy(inst->reg);
    free(inst->clogpath);
    free(inst->walpath);
    free(inst);
}

db_err db_close(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = db->inst;

    /*
     * 整个关闭过程持锁：引用计数 + 实例销毁。
     *
     * 销毁时要 flush 所有脏页、写 CHECKPOINT、关 WAL ——
     * 这些都在动实例级共享状态。
     */
    latch_lock();
    inst->nconns--;

    if (inst->nconns <= 0) {
        instance_close(inst);
    } else {
        /* 还有别的连接在用：只关这个连接 */
        free(db);
    }
    latch_unlock();

    return DB_OK;
}

/*
 * 关一个连接（不是实例所有者那个）。
 *
 * 语义上和 db_close 一样，但明确表示"我只是关连接"。
 * 如果关的是最后一个连接，实例也会被关掉。
 */
void db_conn_close(DB *conn)
{
    if (!conn)
        return;
    db_close(conn);
}

void db_abandon(DB *db)
{
    if (!db)
        return;

    DbInstance *inst = db->inst;

    /* 同 db_close：引用计数 + 实例销毁都在临界区里 */
    latch_lock();
    inst->nconns--;

    if (inst->nconns <= 0) {
        catalog_close(inst->cat);
        blob_store_destroy(inst->blobs);
        graph_destroy(inst->graph);
        /*
         * 向量：断电时**不**落盘索引 ——
         * 索引本来就是 checkpoint 式的，脏页全丢了它就过期，
         * 下次开库自动重建。这正是"索引可以算出来"的价值。
         */
        if (inst->vstate) {
            VectorState *st = inst->vstate;
            if (st->hnsw)
                hnsw_destroy(st->hnsw);
            vindex_destroy(st->vindex);
            vstore_destroy(st->vs);
            free(st);
            inst->vstate = NULL;
        }
        /*
         * 全文索引：断电时同样**不**落盘清单（本来就是内存里的）。
         * 段的内容在数据文件里（脏页丢了就丢了），
         * 清单更是一开始就没落盘 —— 所以这里只是释放内存。
         */
        db_ft_close(inst);
        /*
         * 地理空间模态（v1.4）：断电时树页**不**落盘 ——
         * geo 本来就没有 WAL，脏页丢了就是丢了（坑 M4-38，
         * 崩溃恢复是步骤 6 的账）。这里只是释放内存。
         */
        db_geo_close(inst);
        spi_registry_destroy(inst->reg);
        pager_abandon(inst->pager);     /* 脏页全丢 —— 这就是"断电" */
        wal_close(inst->wal);            /* 日志已经 fsync 过了，关掉即可 */
        txn_clog_discard(&inst->txns);  /* clog 也不落盘 —— 断电就该没有 */
        free(inst->clogpath);
        free(inst->walpath);
        free(inst);
        free(db);
    } else {
        free(db);
    }
    latch_unlock();
}
