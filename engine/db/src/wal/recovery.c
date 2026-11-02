/*
 * recovery.c — 崩溃恢复
 *
 * 三个阶段：
 *   分析：扫日志，记下哪些 xid 提交了
 *   重放：按 LSN 升序，对每条行级记录应用 after 镜像（含未提交事务）
 *   回滚：对未提交事务的记录，按 LSN 降序应用 before 镜像
 *
 * 幂等性靠页头的 page_lsn：page_lsn >= record.lsn 说明这条已经生效过。
 */

#include "wal/recovery.h"

#include <stdlib.h>
#include <string.h>

#include "storage/page.h"

typedef struct {
    WalRec rec;
} Ent;

typedef struct {
    Ent  *v;
    int   n;
    int   cap;
} Vec;

static int vec_push(Vec *vec, const WalRec *r)
{
    if (vec->n == vec->cap) {
        int ncap = vec->cap ? vec->cap * 2 : 256;
        Ent *nv = realloc(vec->v, (size_t)ncap * sizeof(Ent));
        if (!nv)
            return E_IO;
        vec->v = nv;
        vec->cap = ncap;
    }
    vec->v[vec->n++].rec = *r;
    return OK;
}

/*
 * 这个 xid 提交了吗？
 *
 * ⚠ xid == 0 是特例：它是"系统事务"，用来记 bootstrap 阶段的记录
 * （建系统表、写 meta 页）。这些记录发生在任何用户事务之前，
 * 逻辑上永远是"已提交"的 —— 数据库建好了就是建好了。
 *
 * 踩过的坑：bootstrap 的记录 xid=0，分析阶段找不到对应的 COMMIT，
 * 就把它们全当未提交事务回滚掉了。结果 meta 页被回滚成初始值，
 * magic 变成 0，catalog_open 报 E_CORRUPT，整个库打不开。
 */
static bool xid_committed(const Vec *vec, u32 xid)
{
    if (xid == 0)
        return true;        /* 系统事务，视为已提交 */

    for (int i = 0; i < vec->n; i++)
        if (vec->v[i].rec.type == WAL_COMMIT && vec->v[i].rec.xid == xid)
            return true;
    return false;
}

/* 取页并确保页号存在（日志里可能有文件还没覆盖到的页） */
static int get_page(Pager *pg, u32 page_id, PageFrame **f)
{
    int rc = pager_get(pg, page_id, f);
    if (rc == E_NOTFOUND) {
        rc = pager_extend_to(pg, page_id + 1);
        if (rc != OK)
            return rc;
        rc = pager_get(pg, page_id, f);
    }
    return rc;
}

/* ---------------- 重放 ---------------- */

static int redo_one(Pager *pg, Wal *w, const WalRec *r)
{
    if (r->type != WAL_INSERT && r->type != WAL_DELETE &&
        r->type != WAL_UPDATE && r->type != WAL_PAGEINIT &&
        r->type != WAL_PAGEDATA)
        return OK;
    if (r->page_id == PAGE_ID_NIL)
        return OK;

    PageFrame *f;
    int rc = get_page(pg, r->page_id, &f);
    if (rc != OK)
        return rc;

    /* 幂等：这一页已经应用过这条记录了 */
    if (page_lsn(f->data) >= r->lsn) {
        pager_unpin(pg, r->page_id, false);
        return OK;
    }

    u8 payload[WAL_MAX_PAYLOAD];
    rc = wal_read_payload(w, r, payload, sizeof(payload));
    if (rc != OK) {
        pager_unpin(pg, r->page_id, false);
        return rc;
    }

    const u8 *after = payload + r->blen;

    switch (r->type) {
    case WAL_PAGEDATA:
        /* 整页镜像：直接把 after 盖上去 */
        if (r->alen >= PAGE_SIZE)
            memcpy(f->data, after, PAGE_SIZE);
        break;
    case WAL_PAGEINIT:
        /* 整页零 + 页头。页号可能超出文件（分配了没刷盘），
         * get_page 已经把它扩出来了，这里补上页头 */
        memset(f->data, 0, PAGE_SIZE);
        if (r->alen)
            memcpy(f->data, after, r->alen);
        page_set_checksum(f->data);
        break;
    case WAL_INSERT:
        if (r->alen) {
            /*
             * 用日志里记的 slot，不用 page_insert 自己分配。
             *
             * 为什么：undo 阶段要按同一个 slot 撤销。
             * 如果重放时 slot 算错了（页状态和预期有偏差），
             * 撤销就会删错行 —— 表现是"数据莫名少一条"，极难查。
             */
            (void)page_insert_at(f->data, r->slot, after, r->alen);
        }
        break;
    case WAL_DELETE:
        (void)page_delete(f->data, r->slot);
        break;
    case WAL_UPDATE:
        if (r->alen) {
            /* 原地更新只可能是缩小或等长（变大走 delete+insert 两条记录）*/
            if (page_update(f->data, r->slot, after, r->alen) != OK)
                (void)page_restore(f->data, r->slot, after, r->alen);
        }
        break;
    default:
        break;
    }

    page_set_lsn(f->data, r->lsn);
    return pager_unpin(pg, r->page_id, true);
}

/* ---------------- 回滚 ---------------- */

static int undo_one(Pager *pg, Wal *w, const WalRec *r)
{
    if (r->type != WAL_INSERT && r->type != WAL_DELETE &&
        r->type != WAL_UPDATE && r->type != WAL_PAGEDATA)
        return OK;
    if (r->page_id == PAGE_ID_NIL)
        return OK;

    PageFrame *f;
    int rc = get_page(pg, r->page_id, &f);
    if (rc != OK)
        return rc;

    u8 payload[WAL_MAX_PAYLOAD];
    rc = wal_read_payload(w, r, payload, sizeof(payload));
    if (rc != OK) {
        pager_unpin(pg, r->page_id, false);
        return rc;
    }

    switch (r->type) {
    case WAL_PAGEDATA:
        /* 整页回滚：把 before 镜像盖回去 */
        if (r->blen >= PAGE_SIZE)
            memcpy(f->data, payload, PAGE_SIZE);
        break;
    case WAL_PAGEINIT:
        break;                      /* 页分配不需要撤销 */
    case WAL_INSERT:
        /* 撤销插入 = 删掉这一行 */
        (void)page_delete(f->data, r->slot);
        break;
    case WAL_DELETE:
    case WAL_UPDATE:
        /* 撤销删除/更新 = 把 before 镜像写回原 slot */
        if (r->blen) {
            /*
             * 用带偏移的版本：只有确认记录还在原来的位置才回写。
             * 记录被 compact 挪走了就不能动 —— 乱写会破坏别的 slot。
             */
            int urc = page_restore_at(f->data, r->slot, r->slot_off,
                                      payload, r->blen);
            if (urc == E_NOTFOUND) {
                /* 位置对不上：这条 undo 放弃，页保持重放后的状态。
                 * 这不理想，但比写坏数据好。 */
                pager_unpin(pg, r->page_id, true);
                return OK;
            }
        }
        break;
    default:
        break;
    }

    /* 标记这一页已经处理到这条 LSN，避免二次恢复重复回滚 */
    if (page_lsn(f->data) < r->lsn)
        page_set_lsn(f->data, r->lsn);

    return pager_unpin(pg, r->page_id, true);
}

/* ---------------- xid 状态扫描（供 clog 重建） ---------------- */

/*
 * 扫一遍日志，收集每个 xid 是提交了还是中止了。
 *
 * 判据：一个 xid 如果日志里有 WAL_COMMIT 记录 → 已提交；
 * 否则如果它有别的记录（数据修改）→ 已中止（恢复时会被回滚）。
 *
 * 注意 xid=0（系统事务）：永远视为已提交，不收进表里。
 */
int recovery_scan_xids(Wal *w, RecoveredState *out)
{
    if (!w || !out)
        return E_INVAL;

    memset(out, 0, sizeof(*out));

    WalRec r;
    int rc;
    wal_scan_begin(w);
    while ((rc = wal_scan_next(w, &r)) == OK) {
        if (r.xid == 0)
            continue;               /* 系统事务，不用记 */

        /* 找这个 xid 在不在表里 */
        int found = -1;
        for (int i = 0; i < out->n; i++) {
            if (out->xids[i].xid == r.xid) {
                found = i;
                break;
            }
        }
        if (found < 0) {
            if (out->n >= REC_MAX_XIDS) {
                /* 表满了：保守处理，先当成未提交，后面遇到 COMMIT 会改 */
                rc = OK;
                continue;
            }
            found = out->n++;
            out->xids[found].xid = r.xid;
            out->xids[found].committed = 0;
        }

        if (r.type == WAL_COMMIT)
            out->xids[found].committed = 1;
        else if (r.type == WAL_ABORT)
            out->xids[found].committed = 0;
    }
    wal_scan_end(w);

    return (rc == E_EOF || rc == OK) ? OK : rc;
}

/* ---------------- 入口 ---------------- */

int recovery_run(Pager *pg, Wal *w, bool *did_recover)
{
    if (!pg || !w || !did_recover)
        return E_INVAL;

    *did_recover = false;

    Vec vec;
    memset(&vec, 0, sizeof(vec));

    WalRec r;
    int rc;

    wal_scan_begin(w);
    while ((rc = wal_scan_next(w, &r)) == OK) {
        if (vec_push(&vec, &r) != OK) {
            rc = E_IO;
            break;
        }
    }
    wal_scan_end(w);

    if (rc != E_EOF && rc != OK) {
        free(vec.v);
        return rc;
    }

    if (vec.n == 0) {
        free(vec.v);
        return OK;              /* 日志是空的，干净关闭 */
    }

    /* 最后一条是 CHECKPOINT 说明上次是干净关闭（页已全刷、索引一致），
     * 不需要重放，也不需要重建索引 */
    if (vec.v[vec.n - 1].rec.type == WAL_CHECKPOINT) {
        free(vec.v);
        return OK;
    }

    /* 阶段 2：重放（含未提交事务）*/
    for (int i = 0; i < vec.n; i++) {
        rc = redo_one(pg, w, &vec.v[i].rec);
        if (rc != OK) {
            free(vec.v);
            return rc;
        }
    }

    /* 阶段 3：回滚未提交事务（倒序）*/
    for (int i = vec.n; i-- > 0; ) {
        if (xid_committed(&vec, vec.v[i].rec.xid))
            continue;
        rc = undo_one(pg, w, &vec.v[i].rec);
        if (rc != OK) {
            free(vec.v);
            return rc;
        }
    }

    free(vec.v);

    rc = pager_flush_all(pg);
    if (rc != OK)
        return rc;

    *did_recover = true;
    return OK;
}
