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
 * 【本版的索引不落盘】
 *
 * 段的内容在磁盘上，但**段的清单与 tombstone 集合只在内存里**
 * （段目录页是步骤 5 的事）。所以：
 *   - 打开库 → 建一个**空索引**（不假装能从磁盘恢复）；
 *   - 关库 → 释放索引（内存里的东西，丢了就是丢了）。
 *
 * 索引的**页**（段的内容）是真的写进 WAL 和数据文件的 ——
 * 也就是说"数据在盘上，只是清单没了"。这是本版的边界，如实记录。
 */

#include "db_internal.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"

/* ---------------- 小工具 ---------------- */

static void dft_err(DB *db, const char *msg)
{
    if (db) {
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
    }
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
 * 开库：建索引 + 注册虚拟表。
 *
 * ⚠️ **建的是空索引** —— 磁盘上没有"段目录页"，
 * 所以读不到上次的段清单（步骤 5 才落盘）。
 * 这里**不假装能恢复**：如实建一个空的，让 ft_docs 是一张空表，
 * 而不是"打开库失败"。
 *
 * 为什么这与 graph/vector 的做法不同：graph/vector 磁盘上**有**自己的
 * 目录（能打开就接管，打开不了就是 E_NOTFOUND）。ft 本版磁盘上什么都没有，
 * 所以"没有"就是"空"，不是错误。
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

    rc = db_ft_register(inst);
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
 * 与 blob / vector 同款：段页要进 WAL，但 ft 的**段清单只在内存里**，
 * 恢复时段的清单本来就找不回来（步骤 5 落盘之后才有意义）。
 * 所以用系统事务，恢复时这些页永远视为已提交 —— 不会因为"没提交"
 * 而被回滚掉（那会让段的内容和清单更加对不上）。
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
 * 删一篇文档（打 tombstone）。
 *
 * 立刻生效：之后的检索不再返回它；ft_docs 里它的 sealed = 1。
 * 删一个不存在的文档是合法的空操作（内核返回 OK）。
 */
db_err db_ft_delete(DB *db, uint32_t doc_id)
{
    DbInstance *inst;
    int rc;

    if (!db) {
        return DB_E_INVAL;
    }
    inst = db->inst;
    if (!inst || !inst->ft_ready) {
        dft_err(db, "全文索引还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    rc = ft_index_delete(inst->ft, 0, doc_id);
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
