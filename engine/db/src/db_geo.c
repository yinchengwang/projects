/*
 * db_geo.c —— 地理空间模态的公开 API（v1.4 步骤 5）
 *
 * 这一层负责：把模态状态（GeoState：meta 页 + R-tree）挂进实例、
 * 把 geo_points 虚拟表接进注册表、全部入口加锁（硬规则 8）、
 * 把内部错误码翻译成公共错误码（map_rc）、提供"重建索引"这条自愈路径。
 *
 * 【与向量模态的差别】
 *
 * 向量模态是"行存 + 内存索引"两层；地理空间模态的 R-tree **就是数据本身**
 * —— 点坐标只存在树里，没有第二份行存。所以本层的写/查直接改树。
 *
 * 【WAL：v1.4 步骤 6（A7）已接入】
 *
 * 树页与 meta 页现在都进日志（整页镜像，见 geo_wal.h）：
 * 本层把 db_wal_hook 注入模态，模态每个改页的写点都"先写日志再改页"，
 * 崩溃后由 WAL 重放把页补回来（坑 M4-38 由此消除）。
 *
 * ⚠️ 但 geo 的写**没有事务边界**：日志只用于 redo，**不用于 undo**
 * （xid 固定 0 = 系统事务，恢复时永不回滚）。
 * 将来要把 geo 的写纳入用户事务，必须同时补真正的 undo。
 *
 * ⚠️ 坑 M5-16：地理模态的点**只存在树里**（没有第二份行存），
 * 所以 WAL 是**唯一**的恢复路径 —— 没有 WAL ⇒ 崩溃后这批点就是没了。
 *
 * 【本层要替模态挡的三件事】
 *
 * 1. **死条目**（坑 M5-12）：删除是"给叶子条目的 id 打最高位"，而
 *    rtree.c / rtree_knn.c 写得比 rtree_scan.c 早，不认这一位 ——
 *    死条目会被照常吐出来，过滤的活儿落在本层。
 * 2. **距离**（坑 M5-13）：rtree_knn 只返回 row_id，不返回距离；而公开
 *    API 的 db_geo_hit 要带平方距离 —— 所以要**额外扫一遍树**把坐标捞
 *    回来自己算。这一遍是本轮的额外成本，如实记下。
 * 3. **行号不复用**（坑 M5-14）：reindex 把死条目**物理**清掉后，
 *    "条目数"与"最大行号 + 1"分家了，按条目数发号会发出用过的号。
 */
#include "db_internal.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/latch.h"
#include "modalities/geo/rtree_scan.h"

/* 行号掩码：叶子条目 id 的最高位是"已删除"标志（rtree_scan.h 的约定） */
#define GEO_ROW_MASK  0x7FFFFFFFu

/* ---------------- 小工具 ---------------- */

static void dg_err(DB *db, const char *msg)
{
    if (db)
        snprintf(db->errmsg, sizeof(db->errmsg), "%s", msg);
}

static DbInstance *dg_inst(DB *db)
{
    return db ? db->inst : NULL;
}

/*
 * 把树自己的页读计数**增量**记进 GeoState.page_reads。
 *
 * rtree_page_reads(tree) 是**累计值**，每次查询后再整个加一遍会让历史
 * 读数被重复计入。所以实例里存"上次看到的累计值"，每次只记增量。
 */
static void dg_account_tree(DbInstance *inst)
{
    if (!inst || !inst->geo || !inst->geo->tree)
        return;

    u64 now = rtree_page_reads(inst->geo->tree);
    if (now > inst->geo_tree_reads_seen)
        inst->geo->page_reads += now - inst->geo_tree_reads_seen;
    inst->geo_tree_reads_seen = now;
}

/* 释放摊平表（任何一步失败都要走这里，所以单独一个函数） */
void db_geo_tab_free(GeoTab *t)
{
    if (!t)
        return;
    free(t->p);
    memset(t, 0, sizeof(*t));
}

/* 分配摊平表（cap == 0 = 树是空的，后面自然退化成"没有点"） */
static int dg_tab_alloc(GeoTab *t, u32 cap)
{
    memset(t, 0, sizeof(*t));
    t->cap = cap;
    if (cap == 0)
        return OK;

    t->p = (GeoPtEntry *)calloc(cap, sizeof(GeoPtEntry));
    if (!t->p) {
        db_geo_tab_free(t);
        return E_NOSPACE;
    }
    return OK;
}

/* 枚举回调：把点填进摊平表 */
static int dg_collect(const RTreePoint *p, void *ctx)
{
    GeoTab *t = (GeoTab *)ctx;

    if (p->row_id >= t->cap)
        return E_CORRUPT;      /* row_id 越界 = 计数与树不一致 */

    t->p[p->row_id].x = p->x;
    t->p[p->row_id].y = p->y;
    t->p[p->row_id].dead = p->dead;
    t->n++;
    if (p->dead)
        t->dead_n++;
    else
        t->live++;
    return OK;
}

/*
 * 枚举回调的上下文：一次扫描里同时干两件事 ——
 * 找指定 row_id（want），并记下整棵树里的最大 row_id（max_id）。
 */
typedef struct {
    u32  want;
    u32  max_id;
    bool any;
    bool found;
    bool dead;
} GeoFind;

static int dg_find_cb(const RTreePoint *p, void *ctx)
{
    GeoFind *f = (GeoFind *)ctx;

    if (p->row_id == f->want) {
        f->found = true;
        f->dead = p->dead;
    }
    if (!f->any || p->row_id > f->max_id) {
        f->max_id = p->row_id;
        f->any = true;
    }
    return OK;
}

/* 摊平全树（KNN 与 reindex 共用）：拿坐标 + 活/死条目数 */
int db_geo_flatten(DbInstance *inst, GeoTab *tab)
{
    GeoState *st = inst->geo;

    int rc = dg_tab_alloc(tab, st->n_total);
    if (rc != OK)
        return rc;

    u64 reads = 0;
    rc = rtree_scan_all(inst->pager, rtree_root_page(st->tree),
                        dg_collect, tab, &reads);
    st->page_reads += reads;
    if (rc != OK)
        db_geo_tab_free(tab);
    return rc;
}

/*
 * 把"总条目数"修正成"最大行号 + 1"（行号水位线）。
 *
 * 行号是从"总条目数"分配的（st->n_total），正常情况下
 * 总条目数 = 最大行号 + 1（行号连续分配，从不回收）。
 * 但 reindex 会把死条目**物理**清掉 —— 两个数就分家了：
 * 条目数变小，最大行号不变（活点的行号原样保留）。
 * 后果：重开库后按"条目数"发号，会发出一个**已经用过的行号**。
 */
static void dg_fix_high_water(DbInstance *inst)
{
    if (!inst || !inst->geo)
        return;

    GeoState *st = inst->geo;

    GeoFind f;
    memset(&f, 0, sizeof(f));
    f.want = 0xFFFFFFFFu;       /* 永远不会命中 */

    u64 reads = 0;
    int rc = rtree_scan_all(inst->pager, rtree_root_page(st->tree),
                            dg_find_cb, &f, &reads);
    st->page_reads += reads;

    if (rc != OK)
        return;                 /* 读不出来就保持原值，不猜 */

    if (f.any) {
        u32 need = f.max_id + 1u;
        if (st->n_total < need)
            st->n_total = need;
    }
}

/* ---------------- 注册表接入 ---------------- */

/*
 * 把 geo_points 接进注册表。
 *
 * 和 blob / graph / vec 同款：注册表只知道"名字 → 一个 StorageAM"，
 * 它不知道地理空间是什么。加第 5 个模态不需要改注册表或执行器。
 */
int db_geo_register(DbInstance *inst)
{
    if (!inst || !inst->geo)
        return E_INVAL;

    int rc = geo_am_build_points(inst->geo, &inst->geo_am, &inst->geo_ctx);
    if (rc != OK)
        return rc;

    return spi_registry_add(inst->reg, "geo_points", &inst->geo_am);
}

/* ---------------- 建 / 开 / 关 ---------------- */

/*
 * 开库时接管磁盘上已有的几何存储。
 *
 * 磁盘上没有 → E_NOTFOUND（**不是错误**：那是个还没建过几何存储的库，
 * 和图模态一样，由调用方决定算不算错）；meta 页读不懂 → E_CORRUPT。
 */
int db_geo_open(DbInstance *inst)
{
    if (!inst)
        return E_INVAL;

    GeoState *st = NULL;
    /*
     * 注入写日志钩子（v1.4 步骤 6 · A7）：
     * 模态只认 L0 的 LogHookFn，不碰 wal/（分层：模态不能依赖 L1）。
     * 钩子的实现是 db_wal_hook（src/db.c），它把记录翻译成 WAL 记录。
     */
    int rc = geo_state_open_wal(inst->pager, db_wal_hook, inst, &st);
    if (rc != OK) {
        inst->geo = NULL;
        inst->geo_ready = false;
        return rc;              /* E_NOTFOUND = 还没建过 */
    }

    inst->geo = st;
    inst->geo_ready = true;
    inst->geo_tree_reads_seen = 0;

    dg_fix_high_water(inst);    /* 行号水位线：抬到 max(row_id)+1 */

    rc = db_geo_register(inst);
    if (rc != OK) {
        geo_state_destroy(st);
        inst->geo = NULL;
        inst->geo_ready = false;
        inst->geo_tree_reads_seen = 0;
        return rc;
    }
    return OK;
}

/*
 * 关库：释放模态状态。
 *
 * 【幂等】inst->geo == NULL 时直接返回 —— 开库的失败清理路径有好多条，
 * 重复调用必须安全。geo 的页是**普通脏页**，pager_close 会刷盘，
 * 数据不依赖"先释放还是先关 pager"这个顺序。
 */
void db_geo_close(DbInstance *inst)
{
    if (!inst || !inst->geo)
        return;

    geo_state_destroy(inst->geo);
    inst->geo = NULL;
    inst->geo_ready = false;
    inst->geo_tree_reads_seen = 0;
}

/* ---------------- 公开 API ---------------- */

/*
 * 建几何存储（一个库只能建一次）。
 *
 * 第二次建 → DB_E_INVAL（与向量模态同一个口径："没有存储"与
 * "存储是空的"要能被区分开，所以不允许建第二次）。
 */
db_err db_geo_create(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dg_inst(db);

    latch_lock();

    if (inst->geo_ready) {
        latch_unlock();
        dg_err(db, "几何存储已经建过了");
        return DB_E_INVAL;
    }

    GeoState *st = NULL;
    /* 注入写日志钩子（v1.4 步骤 6 · A7）—— 见 db_geo_open 的说明 */
    int rc = geo_state_create_wal(inst->pager, db_wal_hook, inst, &st);
    if (rc == OK) {
        inst->geo = st;
        inst->geo_ready = true;
        inst->geo_tree_reads_seen = 0;

        rc = db_geo_register(inst);
        if (rc != OK) {
            /* 注册失败要回滚：不能留下"半建"的状态 */
            geo_state_destroy(st);
            inst->geo = NULL;
            inst->geo_ready = false;
        }
    }

    latch_unlock();

    if (rc != OK) {
        dg_err(db, "建几何存储失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/* 这个库有没有几何存储 */
int db_geo_exists(DB *db)
{
    DbInstance *inst = dg_inst(db);
    if (!inst)
        return 0;

    latch_lock();
    int r = inst->geo_ready ? 1 : 0;
    latch_unlock();
    return r;
}

/*
 * 插入一个点。
 *
 * row_id 从"总条目数"分配（0 起，单调递增，**不复用**）。
 * 坐标必须是有限值 —— NaN 会让所有几何比较返回 false，
 * 剪枝会**静默失效**（rtree_insert 也会拒，这里先拒是为了把话说清楚）。
 */
db_err db_geo_insert(DB *db, float x, float y, uint64_t *row_id_out)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dg_inst(db);
    if (!inst->geo_ready) {
        dg_err(db, "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    if (!isfinite(x) || !isfinite(y)) {
        dg_err(db, "坐标必须是有限值（NaN / Inf 不行）");
        return DB_E_INVAL;
    }

    GeoState *st = inst->geo;

    latch_lock();

    if (st->n_total >= GEO_ROW_MASK) {
        latch_unlock();
        dg_err(db, "行号用尽了");
        return DB_E_NOSPACE;
    }

    u64 row_id = (u64)st->n_total;

    int rc = rtree_insert(st->tree, (u32)row_id, x, y);
    if (rc == OK) {
        st->n_total++;
        st->n_live++;

        /* 根页可能变了（根分裂）——写回 meta 页 */
        rc = geo_state_sync_root(st);

        /*
         * 日志落盘（WAL 规则：日志先落盘，脏页才能落盘）。
         *
         * geo 的写**不在用户事务里**（没有事务边界），所以每次写完
         * 就把日志刷到盘上 —— 与堆表"每条语句结束时 fsync"同一个口径。
         * 不这么做的话，断电时日志还在缓冲区里，页又没落盘 ⇒ 点没了。
         */
        if (rc == OK)
            rc = wal_fsync(inst->wal);
    }

    dg_account_tree(inst);
    latch_unlock();

    if (rc != OK) {
        dg_err(db, "插入点失败");
        return map_rc(rc);
    }

    if (row_id_out)
        *row_id_out = row_id;
    return DB_OK;
}

/*
 * 删一个点（打删除标记，不重排树）。
 *
 * 【代价：O(n)】模态没有"按 row_id 查坐标"的索引（rtree_scan.h 已记下），
 * 删除要扫一遍树；本层为了知道"删之前它是活的还是死的"还要再扫一遍。
 */
db_err db_geo_delete(DB *db, uint64_t row_id)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dg_inst(db);
    if (!inst->geo_ready) {
        dg_err(db, "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    if (row_id > GEO_ROW_MASK) {
        dg_err(db, "行号超出范围");
        return DB_E_NOTFOUND;
    }

    GeoState *st = inst->geo;
    u32 root = rtree_root_page(st->tree);

    latch_lock();

    /* ① 先找：存在吗？是活的吗？ */
    GeoFind f;
    memset(&f, 0, sizeof(f));
    f.want = (u32)row_id;

    u64 reads = 0;
    int rc = rtree_scan_all(inst->pager, root, dg_find_cb, &f, &reads);
    st->page_reads += reads;

    if (rc != OK) {
        latch_unlock();
        dg_err(db, "扫描几何树失败");
        return map_rc(rc);
    }

    if (!f.found) {
        latch_unlock();
        dg_err(db, "这个点不存在");
        return DB_E_NOTFOUND;
    }

    if (f.dead) {
        /* 已经是死条目：幂等（重复删是合法的空操作） */
        latch_unlock();
        return DB_OK;
    }

    /* ② 打标记（写日志版：死标记改了叶子页，不记日志的话崩溃后会诈尸） */
    reads = 0;
    rc = rtree_mark_dead_wal(inst->pager, root, (u32)row_id, &st->wal, &reads);
    st->page_reads += reads;

    if (rc == OK && st->n_live > 0)
        st->n_live--;

    /* 日志落盘（理由同 db_geo_insert） */
    if (rc == OK)
        rc = wal_fsync(inst->wal);

    latch_unlock();

    if (rc != OK) {
        dg_err(db, "删除点失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/*
 * 重建索引：把**活点**重新插进一棵新树。
 *
 * 【代价与后果】
 *
 * - 老树的页**不回收**（本轮没有页回收机制），文件会变大。
 * - 行号**不复用**（坑 M5-14）：reindex 把死条目物理清掉之后，树里的
 *   条目数会变小；而行号是从"总条目数"分配的 —— 所以这里把总条目数
 *   **保住在旧值**上。
 *
 * ⚠️ **已知限制**：meta 页只记根页号（geo_state.h 的设计），总条目数
 *    是**开库时数出来的**。所以 reindex 之后**再重开库**，总条目数会
 *    重新变成"活点数" —— 那种情况下行号**可能**被复用（开库时的水位线
 *    修正能挡住大部分）。要彻底解决，得让 meta 页也记总条目数 ——
 *    步骤 6 的账。
 */
db_err db_geo_reindex(DB *db)
{
    if (!db)
        return DB_E_INVAL;

    DbInstance *inst = dg_inst(db);
    if (!inst->geo_ready) {
        dg_err(db, "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    GeoState *st = inst->geo;

    latch_lock();

    /* ① 摊平全树，取活点 */
    GeoTab tab;
    int rc = db_geo_flatten(inst, &tab);
    if (rc != OK) {
        latch_unlock();
        dg_err(db, "扫描几何树失败");
        return map_rc(rc);
    }

    /* ② 建新树，把活点插进去（row_id 原样保留） */
    RTree *nt = NULL;
    rc = rtree_create(inst->pager, &nt);
    if (rc == OK) {
        /* 新树也要接写日志钩子 —— 否则重建过程中的页不在 WAL 里 */
        rtree_set_wal(nt, db_wal_hook, inst);
    }
    if (rc == OK) {
        for (u32 id = 0; id < tab.cap; id++) {
            if (tab.p[id].dead)
                continue;
            rc = rtree_insert(nt, id, tab.p[id].x, tab.p[id].y);
            if (rc != OK)
                break;
        }
    }

    db_geo_tab_free(&tab);

    if (rc != OK) {
        if (nt)
            rtree_destroy(nt);
        latch_unlock();
        dg_err(db, "重建索引失败");
        return map_rc(rc);
    }

    /* ③ 换树 */
    u32 old_total = st->n_total;
    rtree_destroy(st->tree);
    st->tree = nt;
    inst->geo_tree_reads_seen = 0;      /* 新树从 0 开始计数 */

    /* ④ 根页号写回 meta 页 */
    rc = geo_state_sync_root(st);
    if (rc == OK)
        rc = geo_state_refresh_counts(st);

    /* ⑤ 行号不复用：总条目数只能涨不能跌 */
    if (rc == OK) {
        dg_fix_high_water(inst);
        if (st->n_total < old_total)
            st->n_total = old_total;
    }

    /* 日志落盘（理由同 db_geo_insert） */
    if (rc == OK)
        rc = wal_fsync(inst->wal);

    latch_unlock();

    if (rc != OK) {
        dg_err(db, "重建索引失败");
        return map_rc(rc);
    }
    return DB_OK;
}

/* 活着的点数（不含已删除的） */
db_err db_geo_count(DB *db, uint64_t *n)
{
    if (!db || !n)
        return DB_E_INVAL;

    DbInstance *inst = dg_inst(db);
    if (!inst->geo_ready) {
        dg_err(db, "几何存储还不存在");
        return DB_E_NOTFOUND;
    }

    latch_lock();
    *n = (uint64_t)inst->geo->n_live;
    latch_unlock();
    return DB_OK;
}

/*
 * 诊断：累计读了多少页。口径与 db_vec_page_reads / db_ft_page_reads 一致：
 * **逻辑页读**（pager_get 的次数，含缓冲池命中），不是磁盘 IO。
 */
uint64_t db_geo_page_reads(DB *db)
{
    DbInstance *inst = dg_inst(db);
    if (!inst || !inst->geo_ready)
        return 0;
    return inst->geo->page_reads;
}
