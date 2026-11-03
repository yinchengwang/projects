/*
 * db_ft.c —— 全文检索模态的公开 API（v1.3 步骤 4）
 *
 * 设计说明见 include/db.h 与 src/modalities/ft/ft_am.h。
 *
 * 【这一层负责什么】
 *
 *   - 建索引 / 注册 ft_docs 虚拟表（db_ft_open / db_ft_register）
 *   - 全部入口加锁（硬规则 8：新入口必须成对 latch_lock / latch_unlock）
 *   - 把内部错误码翻译成公共错误码（map_rc）
 *
 * 【为什么检索走专用 API 而不是 SQL 谓词】
 *
 * 相关性排序依赖**全局统计量**（idf 随语料变、avgdl 随段合并变），
 * 执行器拿不到也没法独立重求值 —— "下推给超集、执行器重判"这条契约
 * 在相关性排序上不成立。详见 PLAN-v1.3 §1.2 的判决。
 *
 * 【索引现在落盘了】（v1.3 步骤 5）
 *
 * 段的内容本来就在磁盘上；现在**段的清单**与 **tombstone 集合**
 * 也落盘了：
 *   - 段清单 = 段头页里的状态字段（live / dead / merging），
 *     恢复时扫页空间把段找齐（坑 A）；
 *   - tombstone 集合 = 专门的 tombstone 页（PT_FT_TOMB），
 *     恢复时取"页里的内容 ∪ (WAL 重放 − WAL 回滚)"（坑 B）。
 *
 * 所以重开库**恢复得回来**：上次灌进去的文档还在，被删的还在删着。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"
#include "common/types.h"

/* ---------------- 小工具 ---------------- */

static void dft_err(DB *db, const char *msg)
{
    if (db) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
    }
}

/* ---------------- 恢复重放集合 ---------------- */

/* 往集合里追加一个 doc_id */
int db_ft_replay_add(FtReplaySet *set, u32 doc_id)
{
    if (set == NULL) {
        return E_INVAL;
    }
    if (set->n == set->cap) {
        u32 ncap = (set->cap == 0) ? 16u : set->cap * 2u;
        u32 *ni = (u32 *)realloc(set->ids, sizeof(u32) * (size_t)ncap);

        if (ni == NULL) {
            return E_NOSPACE;
        }
        set->ids = ni;
        set->cap = ncap;
    }
    set->ids[set->n++] = doc_id;
    return OK;
}

void db_ft_replay_free(FtReplaySet *set)
{
    if (set == NULL) {
        return;
    }
    free(set->ids);
    set->ids = NULL;
    set->n = 0;
    set->cap = 0;
}

/*
 * 恢复钩子：WAL 层把**非页类**记录原样转交过来，由这一层解释。
 *
 * 【为什么要分 redo / undo 两个集合】
 *
 *   - redo（重放看到的）：这个删除**生效了**；
 *   - undo（回滚撤销的）：未提交事务的删除 = **这个删除没发生过**。
 *
 * 两者必须分开：最终集合 = 页里的内容 ∪ (redo − undo)。
 * 只看 redo 的话，未提交事务的删除会"诈尸"（页被恢复了，集合却还删着）。
 *
 * 【只看 WAL_FT_TOMB】
 *
 * 其它类型的记录（BEGIN / COMMIT / ABORT……）不是 FT 的事，直接忽略。
 */
int db_ft_recovery_hook(void *ctx, u8 type, u32 xid,
                        const u8 *payload, u16 payload_len, RecHookPhase phase)
{
    DbInstance *inst = (DbInstance *)ctx;
    u32 doc_id;
    int rc;

    (void)xid;   /* FT 的删除一律记在系统事务（xid=0）下，见 db_ft_delete */

    if (inst == NULL) {
        return E_INVAL;
    }
    if (type != (u8)WAL_FT_TOMB) {
        return OK;       /* 不是 FT 的记录：不管 */
    }
    if (payload_len != 4 || payload == NULL) {
        return E_CORRUPT;    /* 载荷必须是 u32 doc_id */
    }
    doc_id = get_u32(payload);

    if (phase == REC_HOOK_UNDO) {
        rc = db_ft_replay_add(&inst->ft_replay_undo, doc_id);
    } else {
        rc = db_ft_replay_add(&inst->ft_replay_redo, doc_id);
    }
    return rc;
}

/* ---------------- 注册表接入 ---------------- */

/*
 * 把 ft_docs 接进注册表。
 *
 * 和 blob / graph / vec 同款：注册表只知道"名字 → 一个 StorageAM"，
 * 它不知道全文检索是什么。所以加第 4 个模态不需要改注册表，
 * 更不需要改执行器 —— 这正是 SPI 这层的全部意义。
 */
int db_ft_register(DbInstance *inst)
{
    int rc;

    if (!inst || !inst->ft || !inst->reg) {
        return E_INVAL;
    }
    rc = ft_am_build_docs(inst->ft, &inst->ft_am, &inst->ft_ctx);
    if (rc != OK) {
        return rc;
    }
    return spi_registry_add(inst->reg, "ft_docs", &inst->ft_am);
}

/*
 * 开库：建索引 + **从磁盘恢复**。
 *
 * 【恢复什么】
 *
 *   - 段目录：扫页空间，把还活着的段找齐并打开（死段不算数，坑 A）；
 *   - tombstone 集合：页里的内容 ∪ (WAL 重放看到的 − WAL 回滚撤销的)（坑 B）。
 *
 * 必须在**恢复重放之后**调 —— WAL 里的删除要先进集合（inst->ft_replay_*）。
 *
 * 注册 ft_docs 虚拟表是**另一步**（db_ft_register）：
 * 注册表在恢复之后才建，所以这一步得等注册表好了再做。
 *
 * 【为什么"没有 FT 数据"不是错误】
 *
 * 磁盘上一个段都没有（新库、或还没灌过文档）→ 扫出来是空的 → 空索引。
 * 这与 graph 的"没有图 → 不注册表"不同：ft 的磁盘上**根本没有**
 * "有没有索引"这个标记，所以"没有"就是"空"，不是错误。
 */
int db_ft_open(DbInstance *inst)
{
    FtIndex *ix = NULL;
    int rc;

    if (!inst) {
        return E_INVAL;
    }
    rc = ft_index_create(inst->pager, db_wal_hook, inst, 0, &ix);
    if (rc != OK) {
        return rc;
    }
    inst->ft = ix;

    /* 从磁盘恢复：段目录 + tombstone 集合 */
    rc = ft_index_recover(ix,
                           inst->ft_replay_redo.ids, inst->ft_replay_redo.n,
                           inst->ft_replay_undo.ids, inst->ft_replay_undo.n);
    /* 重放集合用完了，释放（它只活到这一刻） */
    db_ft_replay_free(&inst->ft_replay_redo);
    db_ft_replay_free(&inst->ft_replay_undo);

    if (rc != OK) {
        ft_index_close(ix);
        inst->ft = NULL;
        return rc;
    }
    inst->ft_ready = true;
    return OK;
}

/*
 * 关库：释放索引。
 *
 * 必须在 pager_close **之前**调 —— 索引持有段的内存表示，
 * 段持有 pager 的引用；先关 pager 再释放段就是访问已释放的东西。
 */
void db_ft_close(DbInstance *inst)
{
    if (!inst || !inst->ft) {
        return;
    }
    ft_index_close(inst->ft);
    inst->ft = NULL;
    inst->ft_ready = false;
}

/* ---------------- 公开 API ---------------- */

/*
 * 灌一篇文档。
 *
 * 【xid 传 0（系统事务）】
 *
 * 段页要进 WAL，而段的**清单**（段头页里的状态字段）也在盘上，
 * 恢复时靠扫页空间把段找齐 —— 所以这些页必须永远视为已提交：
 * 用系统事务就不会因为"没提交"而被回滚掉
 * （那会让段的内容和清单更加对不上）。
 */
db_err db_ft_add(DB *db, uint32_t doc_id, const char *text, uint32_t len)
{
    DbInstance *inst;
    FtDocText doc;
    u32 seg_id = 0;
    int rc;

    if (!db || !text) {
        return DB_E_INVAL;
    }
    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        dft_err(db, "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    doc.doc_id = doc_id;
    doc.text = text;
    doc.text_len = len;

    latch_lock();
    rc = ft_index_add(inst->ft, 0, &doc, 1, &seg_id);
    latch_unlock();

    if (rc != OK) {
        dft_err(db, "灌文档失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/*
 * 检索。
 *
 * 【口径与内核完全一致】（坑：不能在这里"帮忙"截断）
 *
 *   - n_out 为 NULL → E_INVAL
 *   - 出错时 *n_out 一律是 0（不给"半截结果"）
 *   - out 为 NULL 且 top_k > 0 → E_NOSPACE（没地方放结果）
 *   - out 为 NULL 且 top_k == 0 → E_INVAL（这个参数本身没意义）
 *   - top_k > max_out → E_NOSPACE（**严格**：不静默截断）
 *
 * 顺序与 ft_index_search / bm25.c 完全一致 ——
 * 三个 API 必须行为一致，否则 A1 的对照就不成立。
 */
db_err db_ft_search(DB *db, const char *query, uint32_t top_k,
                    db_ft_hit *out, uint32_t max_out, uint32_t *n_out)
{
    DbInstance *inst;
    FtHit *hits = NULL;
    uint32_t n = 0;
    uint32_t i;
    int rc;

    if (!db || !query || !n_out) {
        return DB_E_INVAL;
    }
    *n_out = 0;

    if (!out) {
        return (top_k > 0) ? DB_E_NOSPACE : DB_E_INVAL;
    }
    if (top_k > max_out) {
        /* 严格口径：直接报错，不去看实际命中几条 */
        return DB_E_NOSPACE;
    }

    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        dft_err(db, "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    if (top_k == 0) {
        return DB_OK;   /* 不要结果：没有要读的东西 */
    }

    hits = (FtHit *)malloc(sizeof(FtHit) * (size_t)top_k);
    if (!hits) {
        return DB_E_IO;
    }

    latch_lock();
    rc = ft_index_search(inst->ft, query, (u32)strlen(query), top_k, max_out,
                          hits, &n);
    latch_unlock();

    if (rc != OK) {
        free(hits);
        dft_err(db, "检索失败");
        return map_rc(rc);
    }
    for (i = 0; i < n; i++) {
        out[i].doc_id = hits[i].doc_id;
        out[i].score = hits[i].score;
    }
    free(hits);
    *n_out = n;
    return DB_OK;
}

/*
 * 写一条 WAL_FT_TOMB 记录（载荷 = u32 doc_id）。
 *
 * 【为什么要这条逻辑记录】
 *
 * tombstone 的**权威存储**是 tombstone 页（整页镜像进 WAL）。
 * 但"这篇文档被删了"这件事本身，用一条 4 字节的逻辑记录表达更直接：
 *   - 重放时不必依赖"页镜像在不在"；
 *   - 未提交事务的删除可以被**撤销**（语义：这次删除没发生过）。
 *
 * WAL 层不认识 FT 语义（分层铁律），所以由**这一层**直接写记录。
 */
static int dft_write_tomb_wal(DbInstance *inst, u32 xid, u32 doc_id)
{
    u8 payload[4];

    put_u32(payload, doc_id);
    /* page_id = PAGE_ID_NIL：这条记录不落在任何一页上 */
    (void)wal_append_xid(inst->wal, (u8)WAL_FT_TOMB, xid, PAGE_ID_NIL, 0, 0,
                          NULL, 0, payload, 4);
    return OK;
}

/*
 * 删一篇文档（打 tombstone）。
 *
 * 立刻生效：之后的检索不再返回它；ft_docs 里它的 sealed = 1。
 * 删一个不存在的文档是合法的空操作（内核返回 OK）。
 *
 * 【xid 怎么取】
 *
 *   - 在事务里（显式 BEGIN 或语句级事务）→ 用当前事务号：
 *     这样崩溃恢复时，**未提交**事务的删除会被撤销（"这个删除没发生过"）；
 *   - 不在事务里 → 用系统事务（xid = 0，永远视为已提交）。
 *
 * tombstone 页的整页镜像与这条逻辑记录用的是**同一个 xid** ——
 * 两边必须一致，否则会出现"页被撤销了、日志说删了"这种自相矛盾。
 */
db_err db_ft_delete(DB *db, uint32_t doc_id)
{
    DbInstance *inst;
    u32 xid;
    int rc;

    if (!db) {
        return DB_E_INVAL;
    }
    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        dft_err(db, "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    xid = db->in_txn ? db->xid : 0u;

    latch_lock();
    rc = ft_index_delete(inst->ft, xid, doc_id);
    if (rc == OK) {
        rc = dft_write_tomb_wal(inst, xid, doc_id);
    }
    latch_unlock();

    if (rc != OK) {
        dft_err(db, "删除文档失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/*
 * 触发一次 Tiered 合并。
 *
 * 段数 ≤ 8 时什么都不做（*merged_out = 0）。
 * 合并时 tombstone 的文档被**物理丢弃**（不只是不返回）。
 */
db_err db_ft_merge(DB *db, uint32_t *merged_out)
{
    DbInstance *inst;
    u32 merged = 0;
    int rc;

    if (!db || !merged_out) {
        return DB_E_INVAL;
    }
    *merged_out = 0;

    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        dft_err(db, "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    rc = ft_index_merge(inst->ft, 0, &merged);
    latch_unlock();

    if (rc != OK) {
        dft_err(db, "合并失败");
        return map_rc(rc);
    }
    *merged_out = merged;
    return DB_OK;
}

/* ---------------- 统计 ---------------- */

int db_ft_exists(DB *db)
{
    DbInstance *inst = db ? db->inst : NULL;

    return (inst && inst->ft_ready) ? 1 : 0;
}

uint32_t db_ft_ndocs(DB *db)
{
    DbInstance *inst = db ? db->inst : NULL;

    if (!inst || !inst->ft_ready) {
        return 0;
    }
    return ft_index_ndocs(inst->ft);
}

uint32_t db_ft_nsegs(DB *db)
{
    DbInstance *inst = db ? db->inst : NULL;

    if (!inst || !inst->ft_ready) {
        return 0;
    }
    return ft_index_nsegs(inst->ft);
}

uint32_t db_ft_ntombstones(DB *db)
{
    DbInstance *inst = db ? db->inst : NULL;

    if (!inst || !inst->ft_ready) {
        return 0;
    }
    return ft_index_ntombstones(inst->ft);
}

/*
 * 索引累计读了多少页（诊断用，A5 的度量）。
 *
 * 与 db_vec_page_reads / db_blob_page_reads 同一个思路：
 * **用可观测量证明"少读了"**，而不是靠读代码相信。
 */
uint64_t db_ft_page_reads(DB *db)
{
    DbInstance *inst = db ? db->inst : NULL;

    if (!inst || !inst->ft_ready) {
        return 0;
    }
    return ft_index_page_reads(inst->ft);
}
