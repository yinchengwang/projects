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

/* ---------------- 重放钩子（v1.3 步骤 5） ---------------- */

/*
 * 钩子是**进程级**的一份：WAL 是实例级共享的，恢复只跑一次，
 * 所以用全局状态（而不是给 recovery_run 加参数 ——
 * 它的签名被别处调用着，改签名会波及一片）。
 */
static RecoveryHookFn g_hook = NULL;
static void          *g_hook_ctx = NULL;

void recovery_set_hook(RecoveryHookFn fn, void *ctx)
{
    g_hook = fn;
    g_hook_ctx = ctx;
}

/*
 * 这个记录类型是不是"页类"（恢复层自己能处理）？
 *
 * 页类 = 能直接落到某一页上的修改。**不是**页类的（逻辑记录，
 * 比如 WAL_FT_TOMB）一律转交给上层的钩子 ——
 * WAL 层不认识它们的语义，只负责原样转交（见 recovery.h）。
 */
static bool rec_is_page_type(u8 type)
{
    return type == WAL_INSERT || type == WAL_DELETE ||
           type == WAL_UPDATE || type == WAL_PAGEINIT ||
           type == WAL_PAGEDATA;
}

/*
 * 把一条非页类记录交给钩子。
 *
 * 载荷 = before + after 拼起来的字节（逻辑记录一般 blen = 0，
 * 所以载荷全在 after 里）。
 */
static int recovery_call_hook(Wal *w, const WalRec *r, RecHookPhase phase)
{
    u8 payload[WAL_MAX_PAYLOAD];
    u16 len;

    if (!g_hook)
        return OK;

    len = (u16)((u32)r->blen + r->alen);
    if (len > sizeof(payload))
        return E_CORRUPT;         /* 逻辑记录不该带整页镜像 */

    if (len > 0) {
        int rc = wal_read_payload(w, r, payload, (u16)sizeof(payload));

        if (rc != OK)
            return rc;
    }
    return g_hook(g_hook_ctx, r->type, r->xid, payload, len, phase);
}

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

/*
 * 这个 xid 是不是"已经回滚完了"（恢复阶段记过 WAL_ABORT_DONE）？
 *
 * 有了它，第二次恢复就不会把同一个事务再回滚一遍。
 * 回滚本身大体是幂等的（写回同一份前像、删同一个空 slot），
 * 但"页恢复原位失败就放弃"的那种情况不幂等 —— 重复回滚可能把
 * 别的行的位置也搞乱。宁可跳过。
 */
static bool xid_abort_done(const Vec *vec, u32 xid)
{
    for (int i = 0; i < vec->n; i++)
        if (vec->v[i].rec.type == WAL_ABORT_DONE && vec->v[i].rec.xid == xid)
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
 * xid 状态表：开放寻址哈希表。
 *
 * 为什么是哈希表而不是数组：
 *   - 数组要事先知道 xid 的范围（xid 可以很稀疏）；
 *   - 固定长度会**丢状态**（v0.7 的 256 项上限就是这么丢的，
 *     详见 recovery.h 里的说明）；
 *   - 哈希表按需增长，事务多少都不会丢。
 *
 * 槽位用 xid == 0 表示"空"—— xid 0 是系统事务，本来就不收。
 */

#define REC_HASH_INIT 64u

void recovered_state_init(RecoveredState *st)
{
    if (!st)
        return;
    st->slots = NULL;
    st->cap = 0;
    st->n = 0;
}

void recovered_state_free(RecoveredState *st)
{
    if (!st)
        return;
    free(st->slots);
    st->slots = NULL;
    st->cap = 0;
    st->n = 0;
}

/*
 * 散列：乘法散列取低位。
 *
 * 之所以低位可用：乘数是奇数，x*prime mod 2^k 是 x mod 2^k 的一个置换，
 * 连续 xid 落到的槽位互不相同（步长是固定的奇数）—— 不会成堆。
 */
static u32 rec_hash(u32 xid, u32 cap)
{
    return (xid * 2654435761u) & (cap - 1);
}

/* 扩容一倍并重新散列 */
static int rec_grow(RecoveredState *st)
{
    u32 ncap = st->cap ? st->cap * 2 : REC_HASH_INIT;
    RecoveredXid *ns = calloc(ncap, sizeof(RecoveredXid));
    if (!ns)
        return E_IO;

    u32 old_cap = st->cap;
    RecoveredXid *old = st->slots;

    st->slots = ns;
    st->cap = ncap;
    st->n = 0;

    u32 mask = ncap - 1;
    for (u32 i = 0; i < old_cap; i++) {
        if (old[i].xid == 0)
            continue;
        u32 s = rec_hash(old[i].xid, ncap);
        while (st->slots[s].xid != 0)
            s = (s + 1) & mask;
        st->slots[s] = old[i];
        st->n++;
    }
    free(old);
    return OK;
}

/*
 * 找到或插入 xid，返回它所在的槽位下标。
 *
 * 扩容发生在插入之前，所以返回的下标一定落在**当前**的 slots 数组里。
 */
static int rec_put(RecoveredState *st, u32 xid, u32 *idx_out)
{
    if (st->cap == 0 || (st->n + 1) * 10 >= st->cap * 7) {
        int rc = rec_grow(st);
        if (rc != OK)
            return rc;
    }

    u32 mask = st->cap - 1;
    u32 s = rec_hash(xid, st->cap);
    while (st->slots[s].xid != 0) {
        if (st->slots[s].xid == xid) {
            *idx_out = s;
            return OK;
        }
        s = (s + 1) & mask;
    }

    st->slots[s].xid = xid;
    st->slots[s].state = REC_XID_UNKNOWN;
    st->n++;
    *idx_out = s;
    return OK;
}

/*
 * 扫一遍日志，收集每个 xid 是提交了还是中止了。
 *
 * 判据：
 *   看到 WAL_COMMIT → 已提交
 *   看到 WAL_ABORT  → 已中止
 *   只有数据记录    → **未知**（不猜！）
 *
 * 【为什么"不猜"】
 * v1.2 步骤 6 之前这里猜的是"没看到 COMMIT 就是已中止"。
 * 日志被截断之后这个猜测会出错：一个已提交的事务，
 * 它的 COMMIT 记录可能已经被截掉了（提交发生在 checkpoint 之前），
 * 猜成"已中止" → 它的行在恢复后不可见 → 数据丢。
 *
 * 现在只报"未知"，由调用方拿 clog 文件里的权威状态来定夺。
 *
 * 注意 xid=0（系统事务）：永远视为已提交，不收进表里。
 */
int recovery_scan_xids(Wal *w, RecoveredState *out)
{
    if (!w || !out)
        return E_INVAL;

    WalRec r;
    int rc;
    wal_scan_begin(w);
    while ((rc = wal_scan_next(w, &r)) == OK) {
        if (r.xid == 0)
            continue;               /* 系统事务，不用记 */

        u32 idx = 0;
        int prc = rec_put(out, r.xid, &idx);
        if (prc != OK) {
            rc = prc;
            break;
        }

        if (r.type == WAL_COMMIT)
            out->slots[idx].state = REC_XID_COMMITTED;
        else if (r.type == WAL_ABORT)
            out->slots[idx].state = REC_XID_ABORTED;
        else if (r.type == WAL_ABORT_DONE)
            out->slots[idx].state = REC_XID_ABORT_DONE;
    }
    wal_scan_end(w);

    return (rc == E_EOF || rc == OK) ? OK : rc;
}

/* ---------------- 入口 ---------------- */

int recovery_run(Pager *pg, Wal *w, u32 checkpoint_lsn, bool *did_recover)
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

    /*
     * 阶段 2：重放（含未提交事务）
     *
     * 只重放 lsn > checkpoint_lsn 的记录 ——
     * 它之前的记录造成的效果已经在数据文件里了（checkpoint 时刷过页）。
     */
    for (int i = 0; i < vec.n; i++) {
        if (vec.v[i].rec.lsn <= checkpoint_lsn)
            continue;
        rc = redo_one(pg, w, &vec.v[i].rec);
        if (rc != OK) {
            free(vec.v);
            return rc;
        }
        /*
         * 非页类记录（逻辑记录）：WAL 层不认识它的语义，
         * 原样转交给上层的钩子（见 recovery.h 的说明）。
         */
        if (!rec_is_page_type(vec.v[i].rec.type)) {
            rc = recovery_call_hook(w, &vec.v[i].rec, REC_HOOK_REDO);
            if (rc != OK) {
                free(vec.v);
                return rc;
            }
        }
    }

    /*
     * 阶段 3：回滚未提交事务（倒序）
     *
     * 【这里不按 checkpoint_lsn 过滤】
     * 未提交事务的记录可能横跨 checkpoint 点（事务在 checkpoint 时还开着）。
     * 回滚要的是前像（before image），哪怕 lsn <= checkpoint_lsn 也得用。
     *
     * 而且截断日志的前提就是"没有活跃事务"——
     * 被截掉的记录一定是已结束事务的记录，不需要回滚。
     *
     * 【已经回滚完的事务跳过】
     * 上一轮恢复已经记过 WAL_ABORT_DONE 的 xid，这里不再重复回滚。
     */
    for (int i = vec.n; i-- > 0; ) {
        u32 xid = vec.v[i].rec.xid;
        if (xid_committed(&vec, xid))
            continue;
        if (xid_abort_done(&vec, xid))
            continue;
        rc = undo_one(pg, w, &vec.v[i].rec);
        if (rc != OK) {
            free(vec.v);
            return rc;
        }
        /*
         * 未提交事务的逻辑记录也要交给钩子（undo 阶段）——
         * 上层据此把"这个删除没发生过"的事实传下去。
         */
        if (!rec_is_page_type(vec.v[i].rec.type)) {
            rc = recovery_call_hook(w, &vec.v[i].rec, REC_HOOK_UNDO);
            if (rc != OK) {
                free(vec.v);
                return rc;
            }
        }
    }

    /*
     * 阶段 4：给回滚完的事务补一条 WAL_ABORT_DONE。
     *
     * 【为什么必须补】
     * 崩溃时开着的事务，在日志里只有 BEGIN + 数据记录，没有 COMMIT。
     * 恢复把它回滚了，但日志里看不出"它已经回滚完了"。
     *
     * 后果：日志截断的前提是"日志里的事务都结束了"，
     * 而这个事务永远显示"没结束" → **日志永远截不掉** →
     * 步骤 6 想要的"恢复时间只取决于 checkpoint 之后的日志量"就废了。
     *
     * 补一条 WAL_ABORT_DONE 就把这个状态明确下来了：
     * "它的前像已经用掉了，日志里这份可以扔"。
     *
     * 【为什么不用 WAL_ABORT】
     * WAL_ABORT 是**运行时**事务失败时记的 —— 那时回滚**还没做**
     * （本内核的删除是物理删除，运行时不做 undo）。
     * 那种事务的 before 镜像仍然有用（万一再崩一次，还要回滚）。
     * 两者必须分开，否则会把"没回滚的"当成"回滚完了" → 数据错。
     */
    for (int i = 0; i < vec.n; i++) {
        u32 xid = vec.v[i].rec.xid;
        if (xid == 0)
            continue;                       /* 系统事务 */
        if (xid_committed(&vec, xid))
            continue;
        if (xid_abort_done(&vec, xid))
            continue;
        /* 同一个 xid 只补一条 */
        bool seen = false;
        for (int j = 0; j < i; j++) {
            if (vec.v[j].rec.xid == xid) {
                seen = true;
                break;
            }
        }
        if (seen)
            continue;
        wal_append_xid(w, WAL_ABORT_DONE, xid, PAGE_ID_NIL, 0, 0,
                       NULL, 0, NULL, 0);
    }
    wal_fsync(w);

    free(vec.v);

    rc = pager_flush_all(pg);
    if (rc != OK)
        return rc;

    *did_recover = true;
    return OK;
}
