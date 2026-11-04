/*
 * rtree_scan.c —— R-tree 的**全树枚举**与**死条目标记**（v1.4 步骤 5）
 *
 * 【这个模块做什么】
 *
 *   rtree_scan_all()   —— 深度优先枚举整棵树的所有叶子点（含死条目）
 *   rtree_mark_dead()  —— 把某个 row_id 的叶子条目标记为"已删除"
 *   rtree_count_live() —— 数一遍活点数与总条目数
 *
 * 【为什么自己读页，而不是用 RTree 对象】
 *
 * 与 rtree_knn.c 同一个理由：这三个能力只需要"页布局 + pager"，
 * 不需要 RTree 的私有状态（RTree 的结构体是 rtree.c 的私有实现，
 * 别的文件碰不到）。所以入口都收 (Pager *, root_page)。
 *
 * 【死条目怎么表示】
 *
 * 叶子条目的 id 字段的**最高位**是"已删除"标志：
 *
 *   bit31 = 1  → 已标记删除（查询时跳过）
 *   bit30..0   → row_id（所以 row_id 上限是 2^31-1）
 *
 * 为什么不用"再加一个字段"：条目是定长 20 字节，一页塞得下 203 条。
 * 加一个字节就要重排条目布局（MBR 就没法 4 字节对齐了），
 * 而 flag 方案**一个字节都不用加**。
 *
 * 内部节点的 id 是 child_page，**不带这个标志** ——
 * 区分靠页里的 level 字段：level == 0 才是叶子。
 *
 * 【删除不重排】
 *
 * 标记死条目之后：窗口查询 / KNN 跳过它；树的结构**不变**
 * （不合并、不搬条目）；页还是占着 —— 空间不回收。
 * 重建由 db_geo_reindex() 负责（它把死条目**真正**清掉）。
 *
 * 【代价：删除是 O(n)】
 *
 * 删除时**不知道点在哪片叶子**（没有"按 row_id 查坐标"的索引），
 * 所以只能全树扫一遍找到它。这是"标记而不重排"的代价，
 * 如实记录在文档里，而不是假装它很快。
 *
 * 【pin/unpin 纪律】
 *
 * 每次 pager_get 都必须配一次 pager_unpin —— 内容锁泄漏会让后面的
 * latch_order_acquire(LATCH_TXN) 断言崩。
 *
 * 遍历时的做法（与 rtree.c 的坑 M4-40 同款）：
 *   - **内部节点**：读一次页，把孩子页号拷进本地数组，随即 unpin，
 *     再递归 —— 递归期间**一个帧都不占**（帧池压力与树高无关）。
 *   - **叶子节点**：直接捏着帧扫描；要改就**在同一个帧里改**，
 *     page_set_checksum 之后 unpin(dirty=true) —— 同一页不重复 get。
 */

#include "modalities/geo/rtree_scan.h"

#include <stdlib.h>
#include <string.h>

#include "modalities/geo/geo_wal.h"
#include "modalities/geo/rtree.h"
#include "storage/page.h"

/* ---------------- 页内访问（布局见 rtree.h） ---------------- */

static inline bool rts_is_leaf(const u8 *pg) { return (pg[RT_FLAGS_OFF] & 1u) != 0; }
static inline u16 rts_nent(const u8 *pg)     { return get_u16(pg + RT_NENT_OFF); }

/* 条目的 id 字段（叶子 = row_id|标志，内部 = child_page） */
static inline u32 rts_ent_id(const u8 *p)    { return get_u32(p); }

/* ---------------- 读页 ---------------- */

/*
 * 把一个节点页读进本地缓冲（读完立刻 unpin）。
 *
 * 坏页（页类型不对 / 校验和不过 / 条目数超限）一律 E_CORRUPT ——
 * 宁可报错，也不要在坏树上给出"看起来能查、结果悄悄不对"的答案。
 */
static int rts_load(Pager *pg, u32 pid, u8 *buf, u64 *reads)
{
    PageFrame *fr = NULL;
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK)
        return rc;
    (*reads)++;

    if (fr->data[H_TYPE] != (u8)PT_RTREE ||
        !page_verify_checksum(fr->data) ||
        rts_nent(fr->data) > RT_CAP) {
        pager_unpin(pg, pid, false);
        return E_CORRUPT;
    }
    memcpy(buf, fr->data, PAGE_SIZE);
    pager_unpin(pg, pid, false);
    return OK;
}

/* ---------------- 全树枚举 ---------------- */

typedef struct {
    Pager       *pg;
    u8          *buf;    /* 共用页缓冲：每层用完就交给下一层复用 */
    u64         *reads;
    RTreePointCb cb;
    void        *ctx;
} RtsScan;

/*
 * 深度优先遍历。
 *
 * depth > RT_MAX_DEPTH ⇒ E_CORRUPT：层数异常说明树坏了
 * （环 / 坏页），必须报错而不是无限递归。
 */
static int rts_scan_rec(RtsScan *s, u32 pid, u32 depth)
{
    u32 child[RT_CAP];
    u32 nchild = 0;
    u16 n;
    u32 i;
    int rc;

    if (depth > RT_MAX_DEPTH)
        return E_CORRUPT;

    rc = rts_load(s->pg, pid, s->buf, s->reads);
    if (rc != OK)
        return rc;

    n = rts_nent(s->buf);

    if (rts_is_leaf(s->buf)) {
        for (i = 0; i < n; i++) {
            const u8 *p = s->buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
            u32 id = rts_ent_id(p);
            RTreePoint pt;

            pt.row_id = id & RT_ROWID_MAX;
            /* 叶条目的 MBR 是退化矩形 ⇒ minx/miny 就是那个点的坐标 */
            pt.x = rt_u2f(get_u32(p + 4));
            pt.y = rt_u2f(get_u32(p + 8));
            pt.dead = (id & RT_ENT_DEAD) != 0;

            rc = s->cb(&pt, s->ctx);
            if (rc != OK)
                return rc;       /* 回调要中止 ⇒ 原样把错误码带出去 */
        }
        return OK;
    }

    /*
     * 内部节点：先把孩子页号拷出来，随后 s->buf 会被下一层覆盖。
     * 所以拷贝必须在这一层做完 —— 不能"先递归再读下一条"。
     */
    for (i = 0; i < n; i++) {
        const u8 *p = s->buf + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;

        child[nchild++] = rts_ent_id(p);
    }
    for (i = 0; i < nchild; i++) {
        rc = rts_scan_rec(s, child[i], depth + 1);
        if (rc != OK)
            return rc;
    }
    return OK;
}

int rtree_scan_all(Pager *pg, u32 root_page, RTreePointCb cb, void *ctx,
                    u64 *page_reads_out)
{
    RtsScan s;
    u8 *buf = NULL;
    u64 reads = 0;
    int rc;

    if (pg == NULL || cb == NULL)
        return E_INVAL;
    if (page_reads_out != NULL)
        *page_reads_out = 0;

    /* 空树：一个点都没有，不是错误 */
    if (root_page == PAGE_ID_NIL)
        return OK;

    buf = (u8 *)malloc(PAGE_SIZE);
    if (buf == NULL)
        return E_NOSPACE;

    s.pg = pg;
    s.buf = buf;
    s.reads = &reads;
    s.cb = cb;
    s.ctx = ctx;

    rc = rts_scan_rec(&s, root_page, 0);

    if (page_reads_out != NULL)
        *page_reads_out = reads;
    free(buf);
    return rc;
}

/* ---------------- 死条目标记 ---------------- */

/*
 * 在一棵子树里找 row_id，找到就标死。
 *
 * found 置 true 表示"找到了"（不论这次是不是刚标的）。
 *
 * 叶子：捏着帧扫，**在同一个帧里**改 id 的最高位，
 *       先写日志（整页镜像）再交页 —— 同一页不重复 get。
 * 内部：读一次页拷出孩子页号，随即 unpin，再递归（帧池压力与树高无关）。
 *
 * wal == NULL ⇒ 不写日志（单元测试直接用模态时）。
 */
static int rts_mark_rec(Pager *pg, u32 pid, u32 row_id, u64 *reads,
                        bool *found, u32 depth, const GeoWal *wal)
{
    u32 child[RT_CAP];
    u32 nchild = 0;
    PageFrame *fr = NULL;
    u16 n;
    u32 i;
    int rc;

    if (depth > RT_MAX_DEPTH)
        return E_CORRUPT;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK)
        return rc;
    (*reads)++;

    if (fr->data[H_TYPE] != (u8)PT_RTREE ||
        !page_verify_checksum(fr->data) ||
        rts_nent(fr->data) > RT_CAP) {
        pager_unpin(pg, pid, false);
        return E_CORRUPT;
    }

    n = rts_nent(fr->data);

    if (rts_is_leaf(fr->data)) {
        for (i = 0; i < n; i++) {
            u8 *p = fr->data + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;
            u32 id = rts_ent_id(p);

            if ((id & RT_ROWID_MAX) != row_id)
                continue;

            if (id & RT_ENT_DEAD) {
                /* 已经是死条目 ⇒ 幂等：不改页，直接成功 */
                pager_unpin(pg, pid, false);
                *found = true;
                return OK;
            }

            /* 置最高位 + 写日志 + 重算校验和 + 脏页归还 */
            u8 before[PAGE_SIZE];

            memcpy(before, fr->data, PAGE_SIZE);
            put_u32(p, id | RT_ENT_DEAD);
            rc = geo_pg_commit(pg, pid, fr, wal, before);
            if (rc != OK)
                return rc;
            *found = true;
            return OK;
        }
        /* 这个叶子没有目标 ⇒ unpin 后继续找别的叶子 */
        pager_unpin(pg, pid, false);
        return OK;
    }

    /* 内部节点：拷出孩子页号后立刻放掉帧 */
    for (i = 0; i < n; i++) {
        const u8 *p = fr->data + RT_ENT_OFF + (size_t)i * RT_ENT_SIZE;

        child[nchild++] = rts_ent_id(p);
    }
    pager_unpin(pg, pid, false);

    for (i = 0; i < nchild; i++) {
        rc = rts_mark_rec(pg, child[i], row_id, reads, found, depth + 1, wal);
        if (rc != OK)
            return rc;
        if (*found)
            return OK;      /* 找到了就不必再找 */
    }
    return OK;
}

/*
 * 标记死条目（可写日志的版本）。
 *
 * 死标记改了叶子页 —— 不写日志的话，崩溃重放后这个删除会**消失**，
 * 被删的点诈尸。这是坑 M4-38 留下的窟窿，A7 补上。
 */
int rtree_mark_dead_wal(Pager *pg, u32 root_page, u32 row_id, const GeoWal *wal,
                         u64 *page_reads_out)
{
    u64 reads = 0;
    bool found = false;
    int rc;

    if (pg == NULL)
        return E_INVAL;
    if (page_reads_out != NULL)
        *page_reads_out = 0;
    if (row_id > RT_ROWID_MAX)
        return E_INVAL;      /* row_id 只能用低 31 位 */

    /* 空树：没有可删的东西 */
    if (root_page == PAGE_ID_NIL)
        return E_NOTFOUND;

    rc = rts_mark_rec(pg, root_page, row_id, &reads, &found, 0, wal);
    if (page_reads_out != NULL)
        *page_reads_out = reads;
    if (rc != OK)
        return rc;
    if (!found)
        return E_NOTFOUND;   /* 树里没有这个 row_id */
    return OK;
}

/* 不写日志的版本（单元测试直接用模态时） */
int rtree_mark_dead(Pager *pg, u32 root_page, u32 row_id, u64 *page_reads_out)
{
    return rtree_mark_dead_wal(pg, root_page, row_id, NULL, page_reads_out);
}

/* ---------------- 活点计数 ---------------- */

typedef struct {
    u32 live;
    u32 total;
} RtsCount;

static int rts_count_cb(const RTreePoint *p, void *ctx)
{
    RtsCount *c = (RtsCount *)ctx;

    c->total++;
    if (!p->dead)
        c->live++;
    return OK;
}

int rtree_count_live(Pager *pg, u32 root_page, u32 *n_live, u32 *n_total,
                      u64 *page_reads_out)
{
    RtsCount c;
    u64 reads = 0;
    int rc;

    if (pg == NULL)
        return E_INVAL;
    if (n_live != NULL)
        *n_live = 0;
    if (n_total != NULL)
        *n_total = 0;
    if (page_reads_out != NULL)
        *page_reads_out = 0;

    c.live = 0;
    c.total = 0;

    rc = rtree_scan_all(pg, root_page, rts_count_cb, &c, &reads);
    if (rc != OK)
        return rc;

    if (n_live != NULL)
        *n_live = c.live;
    if (n_total != NULL)
        *n_total = c.total;
    if (page_reads_out != NULL)
        *page_reads_out = reads;
    return OK;
}
