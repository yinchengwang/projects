/*
 * db_blob.c — blob 模态的公开 API（v0.8 起，v0.9 拆文件）
 *
 * 设计说明见 include/db.h 和 src/storage/blob.h。
 *
 * 【blob 的事务语义】
 *
 * blob 对象是**不可变**的 —— 创建 → 写 → 定稿，写完就不能改。
 * 所以 blob 的 API **不参与事务**：
 *
 *   - 没有"回滚到旧版本"这回事（对象没有旧版本）
 *   - 没有并发写同一对象的问题（写完就定稿）
 *   - 但它**写 WAL**，所以崩溃后数据不丢
 *
 * 这和关系模态形成鲜明对比 —— 后者的行会经历 insert/update/delete，
 * 需要 xmin/xmax、需要 MVCC、需要 vacuum。
 *
 * 【为什么 blob 用实例的 WAL 而不是连接的事务】
 *
 * WAL 里的 xid 只在"分析阶段判断哪些事务提交了"时才有意义。
 * blob 的页在写完的瞬间就是最终状态 —— 不存在"这个页属于一个
 * 还没提交的事务"的情况。所以 xid 传 0（系统事务，恢复时永远视为已提交）
 * 是正确的。
 *
 * 如果将来 blob 要支持"多段上传、最后合并"，
 * 那时就需要给它一个真正的 xid，让未完成的对象在恢复时被清理。
 */

#include "db_internal.h"

#include <string.h>

#include "common/latch.h"

/* blob_id 是 u64，blob.h 里也是 u64 —— 直接转换 */
#define BLOB_ID_CAST(x) ((BlobId)(x))

/*
 * ---------------- blob 模态（v0.8） ----------------
 *
 * 【blob 的事务语义】
 *
 * blob 对象是**不可变**的 —— 创建 → 写 → 定稿，写完就不能改。
 * 所以 blob 的 API **不参与事务**：
 *
 *   - 没有"回滚到旧版本"这回事（对象没有旧版本）
 *   - 没有并发写同一对象的问题（写完就定稿）
 *   - 但它**写 WAL**，所以崩溃后数据不丢
 *
 * 这和关系模态形成鲜明对比 —— 后者的行会经历 insert/update/delete，
 * 需要 xmin/xmax、需要 MVCC、需要 vacuum。
 *
 * 【为什么 blob 用实例的 WAL 而不是连接的事务】
 *
 * WAL 里的 xid 只在"分析阶段判断哪些事务提交了"时才有意义。
 * blob 的页在写完的瞬间就是最终状态 —— 不存在"这个页属于一个
 * 还没提交的事务"的情况。所以 xid 传 0（系统事务，恢复时永远视为已提交）
 * 是正确的。
 *
 * 如果将来 blob 要支持"多段上传、最后合并"，
 * 那时就需要给它一个真正的 xid，让未完成的对象在恢复时被清理。
 */

/* blob_id 是 u64，blob.h 里也是 u64 —— 直接转换 */
#define BLOB_ID_CAST(x) ((BlobId)(x))

db_err db_blob_create(DB *db, uint64_t nframes, uint32_t frame_size,
                      db_blob_id *blob_id_out)
{
    if (!db || !blob_id_out)
        return DB_E_INVAL;

    if (frame_size == 0)
        frame_size = BLOB_FRAME_SIZE;

    /*
     * 全局 latch：blob 的对象目录、页分配、WAL 都是实例级共享的。
     * blob 的公开 API **不走 db_exec**，所以要自己持锁。
     */
    latch_lock();
    BlobId id = 0;
    int rc = blob_create(db->inst->blobs, nframes, frame_size, &id);
    latch_unlock();

    if (rc != OK)
        return map_rc(rc);

    *blob_id_out = (db_blob_id)id;
    return DB_OK;
}

db_err db_blob_write(DB *db, db_blob_id id, const void *data, uint64_t len)
{
    if (!db)
        return DB_E_INVAL;

    /*
     * xid = 0（系统事务）：blob 对象写完即定稿，
     * 不存在"未提交的大对象"这种状态。恢复时永远视为已提交。
     */
    latch_lock();
    int rc = blob_write(db->inst->blobs, 0, BLOB_ID_CAST(id),
                        (const u8 *)data, len);
    latch_unlock();

    return map_rc(rc);
}

db_err db_blob_read_frame(DB *db, db_blob_id id, uint64_t frame_no,
                           void *buf, uint32_t cap, uint32_t *len_out)
{
    if (!db || !buf)
        return DB_E_INVAL;

    latch_lock();
    u32 n = 0;
    int rc = blob_read_frame(db->inst->blobs, BLOB_ID_CAST(id), frame_no,
                             (u8 *)buf, cap, &n);
    latch_unlock();

    if (rc != OK)
        return map_rc(rc);

    if (len_out)
        *len_out = n;
    return DB_OK;
}

db_err db_blob_read(DB *db, db_blob_id id, void *buf, uint64_t cap, uint64_t *len_out)
{
    if (!db || !buf)
        return DB_E_INVAL;

    latch_lock();
    u64 n = 0;
    int rc = blob_read(db->inst->blobs, BLOB_ID_CAST(id), (u8 *)buf, cap, &n);
    latch_unlock();

    if (rc != OK)
        return map_rc(rc);

    if (len_out)
        *len_out = n;
    return DB_OK;
}

db_err db_blob_stat(DB *db, db_blob_id id,
                    uint64_t *nframes, uint32_t *frame_size, uint64_t *total_len)
{
    if (!db)
        return DB_E_INVAL;

    latch_lock();
    int rc = blob_stat(db->inst->blobs, BLOB_ID_CAST(id),
                       nframes, frame_size, total_len);
    latch_unlock();

    return map_rc(rc);
}

uint32_t db_blob_count(DB *db)
{
    if (!db)
        return 0;

    latch_lock();
    u32 n = blob_count(db->inst->blobs);
    latch_unlock();

    return n;
}

db_blob_id db_blob_id_at(DB *db, uint32_t i)
{
    if (!db)
        return 0;

    latch_lock();
    db_blob_id id = (db_blob_id)blob_id_at(db->inst->blobs, i);
    latch_unlock();

    return id;
}

uint64_t db_blob_page_reads(DB *db)
{
    if (!db)
        return 0;

    latch_lock();
    const BlobStats *st = blob_stats(db->inst->blobs);
    uint64_t n = st ? st->page_reads : 0;
    latch_unlock();

    return n;
}
