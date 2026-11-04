#include "modalities/geo/rtree.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "modalities/geo/geo_wal.h"
#include "modalities/geo/rtree_priv.h"
#include "storage/page.h"

/*
 * rtree.c —— R-tree 的插入 / 二次分裂 / 窗口查询（v1.4 步骤 2 · A2）
 *
 * 实现 Guttman 1984 的**原始版**（不是后来的 R* / R+ 变体）：
 * 每一步的判据都是明确的数，写错了能立刻从"树退化成链"看出来。
 *
 * 坑 M4-37：几何算术一行都不自己写 —— 剪枝判据（mbr_overlap）与叶子命中
 * 判据（mbr_contains_point）必须与暴力基准 geo_brute.c **逐位相同**。两边各
 * 写一份"差不多的相交判断"，迟早在边界上差一个点；而空间查询漏点**不报错**，
 * 只是答案悄悄不对。
 *
 * 坑 M4-38（**已在 v1.4 步骤 6 消除**）：以前树页不进 WAL，页又是**原地
 * 改写**的，断电会留下半截的树。现在每个改页的写点都先写日志
 * （geo_wal.h 的 geo_pg_commit），崩溃后由 WAL 重放整页镜像补回来。
 * 二次分裂那一段因此搬到了 rtree_split.c —— 本文件要腾行数给写日志。
 */

/* ---------------- 内部结构 ---------------- */

struct RTree {
    Pager *pg;
    u32    root;        /* 根页号；PAGE_ID_NIL = 空树 */
    u32    height;      /* 层数 = 根的 level + 1；空树为 0 */
    u32    npoints;     /* 树里的点数 */
    u64    page_reads;  /* 逻辑页读（pager_get 次数） */
    GeoWal wal;         /* 写日志上下文（钩子为 NULL = 本库不写日志） */
};

/* ---------------- 页内访问 ---------------- */

/* 页头读写 —— 与 src/storage/page.c 的写法一致：单行 static inline */
static inline bool rt_is_leaf(const u8 *pg)  { return (pg[RT_FLAGS_OFF] & 1u) != 0; }
static inline u16 rt_nent(const u8 *pg)      { return get_u16(pg + RT_NENT_OFF); }
static inline u32 rt_level(const u8 *pg)     { return get_u32(pg + RT_LEVEL_OFF); }
static inline u32 rt_npoints(const u8 *pg)   { return get_u32(pg + RT_NPOINTS_OFF); }
static inline const u8 *rt_ents_c(const u8 *pg) { return pg + RT_ENT_OFF; }
static inline u8 *rt_ents(u8 *pg)           { return pg + RT_ENT_OFF; }

/* 只改条目的 MBR（id 不动）—— 父节点更新孩子包围盒时用 */
static void rt_ent_set_mbr(u8 *p, const Mbr *m)
{
    put_u32(p + 4, rt_f2u(m->minx));
    put_u32(p + 8, rt_f2u(m->miny));
    put_u32(p + 12, rt_f2u(m->maxx));
    put_u32(p + 16, rt_f2u(m->maxy));
}

/* 页里的 n 条条目解码到数组 */
static void rt_ent_decode(const u8 *ents, u32 n, RtEntry *out)
{
    u32 i;

    for (i = 0; i < n; i++)
        rt_ent_get(ents + (size_t)i * RT_ENT_SIZE, &out[i]);
}

/* ---------------- 节点页的建立与读入 ---------------- */

/*
 * 初始化一个空的节点页。
 *
 * 走 page_init() 把标准页头一次性铺好（与本仓其它模态一致）；本页不用
 * 槽目录，那几个字段只是闲置。
 */
static void rt_node_init(u8 *pg, bool leaf, u32 level)
{
    page_init(pg, (u8)PT_RTREE);
    pg[RT_FLAGS_OFF] = leaf ? (u8)1u : (u8)0u;
    pg[RT_RSVD_OFF] = 0;
    put_u16(pg + RT_NENT_OFF, 0);
    put_u32(pg + RT_LEVEL_OFF, level);
    put_u32(pg + RT_NPOINTS_OFF, 0);
}

/*
 * 取一个节点页并做完整性校验。
 *
 * 页类型、校验和、条目数三条都不过就当**坏页**（E_CORRUPT）——
 * 宁可报错，也不要返回一棵"看起来能查、结果悄悄不对"的树。
 */
static int rt_node_get(RTree *t, u32 pid, PageFrame **fr){
    int rc = pager_get(t->pg, pid, fr);

    if (rc != OK)
        return rc;
    t->page_reads++;
    if ((*fr)->data[H_TYPE] != (u8)PT_RTREE ||
        !page_verify_checksum((*fr)->data) ||
        rt_nent((*fr)->data) > RT_CAP) {
        pager_unpin(t->pg, pid, false);
        return E_CORRUPT;
    }
    return OK;
}

/* ---------------- 包围盒 ---------------- */

/*
 * 节点自己的包围盒 = 它所有条目的 MBR 的并集。
 *
 * 坑 M4-41：由内向外归纳，节点的包围盒 = 全部点的**精确**包围盒，
 * 既不多也不少 —— 多了会漏点，少了会多读。最小/最大运算是精确的，
 * 与顺序无关，所以两边算出来的数逐位相同。
 */
static void rt_node_mbr(const u8 *pg, Mbr *out)
{
    u32 n = rt_nent(pg);
    const u8 *ents = rt_ents_c(pg);
    RtEntry e;
    Mbr m;
    u32 i;

    if (n == 0) {
        mbr_set_rect(out, 0.0f, 0.0f, 0.0f, 0.0f);
        return;
    }
    rt_ent_get(ents, &e);
    rt_ent_mbr(&e, &m);
    for (i = 1; i < n; i++) {
        RtEntry ei;
        Mbr mi;

        rt_ent_get(ents + (size_t)i * RT_ENT_SIZE, &ei);
        rt_ent_mbr(&ei, &mi);
        mbr_union(&m, &mi, &m);
    }
    *out = m;
}

/* ---------------- ChooseSubtree ---------------- */

/*
 * ChooseSubtree（Guttman 1984 §3.1）：挑"装下这个条目后膨胀最小"的孩子。
 *
 * 判据三级，严格依次：扩张面积最小 → 平局取原来的面积更小 → 再平局取**更
 * 靠前**的那个条目。第 3 条不是随便定的：同样的数据、同样的插入顺序必须
 * 得到**同一棵树**，否则"结果全等"这条验收没法复现。
 */
static u32 rt_choose_subtree(const u8 *pg, const Mbr *e)
{
    u32 n = rt_nent(pg);
    const u8 *ents = rt_ents_c(pg);
    u32 best = 0;
    float best_enl = 0.0f;
    float best_area = 0.0f;
    u32 i;

    for (i = 0; i < n; i++) {
        RtEntry ent;
        Mbr m;
        float enl;
        float area;

        rt_ent_get(ents + (size_t)i * RT_ENT_SIZE, &ent);
        rt_ent_mbr(&ent, &m);
        enl = mbr_enlarge_area(&m, e);
        area = mbr_area(&m);
        if (i == 0 || enl < best_enl ||
            (enl == best_enl && area < best_area)) {
            best = i;
            best_enl = enl;
            best_area = area;
        }
    }
    return best;
}

/* ---------------- 二次分裂（QuadraticSplit） ---------------- */

/*
 * 分裂算法搬到 rtree_split.c 了（v1.4 步骤 6：给写日志腾行数）。
 * 私有胶水（条目解码、分裂函数原型）在 rtree_priv.h。
 */

/*
 * 把分裂结果落到磁盘上：组 1 留在原页，组 2 写进新页。
 *
 * 坑 M4-42：**先写新页** —— 两组的数据都来自同一个解码数组，顺序其实
 * 无所谓，但新页写完才能把组 1 压回原页（页里只放得下组 1）。
 */
static int rt_split_commit(RTree *t, PageFrame *fr, const RtEntry *ents,
                           const u32 *g1, u32 n1,
                           const u32 *g2, u32 n2,
                           u32 *new_pid)
{
    u8 *pg = fr->data;
    PageFrame *nfr = NULL;
    u32 npid = PAGE_ID_NIL;
    int rc;

    rc = pager_alloc(t->pg, &npid, &nfr);
    if (rc != OK)
        return rc;

    rt_node_init(nfr->data, rt_is_leaf(pg), rt_level(pg));
    rt_group_write(rt_ents(nfr->data), ents, g2, n2);
    put_u16(nfr->data + RT_NENT_OFF, (u16)n2);
    /*
     * 新页的前像就是全零（pager_alloc 把页清零了）。
     *
     * 写日志在交页**之前**：页先于日志落盘的话，崩溃后日志里缺这条
     * 记录，而页上却有它的内容 —— 数据"没根"。
     *
     * 注意顺序：新页先落日志，**原页还没动**。所以这里失败可以直接
     * 返回（调用方会把原页按原样归还，不用回滚）。
     */
    rc = geo_pg_commit(t->pg, npid, nfr, &t->wal, GEO_ZERO_PAGE);
    if (rc != OK)
        return rc;

    rt_group_write(rt_ents(pg), ents, g1, n1);
    put_u16(pg + RT_NENT_OFF, (u16)n1);

    *new_pid = npid;
    return OK;
}

/* ---------------- 插入 ---------------- */

/*
 * 插入一个点（Guttman 1984 §3.2）。
 *
 * 1. 从根往下 ChooseSubtree，记下路径；2. 叶子追加条目，溢出就二次分裂；
 * 3. 往回走：孩子分裂则父节点多一条，父节点也溢出就继续分裂；
 * 4. 根分裂 → 新建更高一层的根。
 *
 * 坑 M4-40：显式记路径而不是递归 —— 递归要一路把父页 pin 着（父页的帧指针
 * 在子调用期间必须有效），层数一深就吃掉帧池。显式路径 + 逐层重读，pin 数
 * 恒为 1，帧池压力与树高无关。
 *
 * 坑 M4-41：父节点条目 MBR 每往上一层就更新一次，更新成"刚改过的那个孩子"
 * 的 MBR（= 孩子所有条目的并集）。由内向外归纳，父节点记的 MBR 永远是那棵
 * 子树的**精确**包围盒：多了会漏点，少了会多读。
 *
 * 坑 M4-43：一页只放得下 RT_CAP = 203 条（32 + 203*20 = 4092 字节），
 * 第 204 条**放不进页里**。所以"溢出"的那一条**不写进页**，而是留在
 * 内存数组里参与分裂 —— 页里永远不超过 203 条。
 */
int rtree_insert(RTree *t, u32 row_id, float x, float y)
{
    Mbr e;
    Mbr extra_mbr = { 0.0f, 0.0f, 0.0f, 0.0f };
    Mbr child_mbr;
    RtEntry sbuf[RT_CAP + 1];
    u32 path[RT_MAX_DEPTH];
    u32 idx[RT_MAX_DEPTH];
    u32 depth = 0;
    u32 leaf_pid = PAGE_ID_NIL;
    u32 extra_pid = PAGE_ID_NIL;
    u32 newn;
    u32 cur;
    u32 i;
    int rc;

    if (t == NULL)
        return E_INVAL;
    if (!isfinite(x) || !isfinite(y))
        return E_INVAL;
    /*
     * 叶子条目 id 的最高位是"已删除"标志（见 rtree.h 的 RT_ENT_DEAD），
     * 所以 row_id 只能用低 31 位 —— 超了就直接拒绝，
     * 不能让两个不同的 row_id 撞成同一个键。
     */
    if (row_id > RT_ROWID_MAX)
        return E_INVAL;

    mbr_set_point(&e, x, y);
    newn = t->npoints + 1u;

    /*
     * 空树：先建一个**空的**叶子根，后面的流程与普通树完全一致
     * （下降会落到这个空叶子上，追加与分裂走同一段代码）。
     */
    if (t->root == PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 pid = PAGE_ID_NIL;

        rc = pager_alloc(t->pg, &pid, &fr);
        if (rc != OK)
            return rc;
        rt_node_init(fr->data, true, 0);
        /* 新页：前像全零；先写日志，交页在后 */
        rc = geo_pg_commit(t->pg, pid, fr, &t->wal, GEO_ZERO_PAGE);
        if (rc != OK)
            return rc;
        t->root = pid;
        t->height = 1;
    }

    /* ---- 下降：从根一路 ChooseSubtree 到叶子 ---- */
    cur = t->root;
    for (;;) {
        PageFrame *fr = NULL;
        RtEntry ent;
        u32 k;
        u32 next;

        rc = rt_node_get(t, cur, &fr);
        if (rc != OK)
            return rc;
        if (rt_is_leaf(fr->data)) {
            leaf_pid = cur;
            pager_unpin(t->pg, cur, false);
            break;
        }
        if (rt_nent(fr->data) == 0 || depth >= RT_MAX_DEPTH) {
            pager_unpin(t->pg, cur, false);
            return E_CORRUPT;
        }
        k = rt_choose_subtree(fr->data, &e);
        rt_ent_get(rt_ents_c(fr->data) + (size_t)k * RT_ENT_SIZE, &ent);
        next = ent.id;
        path[depth] = cur;
        idx[depth] = k;
        depth++;
        pager_unpin(t->pg, cur, false);
        cur = next;
    }

    /* ---- 叶子追加条目 ---- */
    {
        PageFrame *fr = NULL;
        u8 before[PAGE_SIZE];   /* 前像：改之前把整页拍下来 */
        u32 n;
        RtEntry ent;

        rc = rt_node_get(t, leaf_pid, &fr);
        if (rc != OK)
            return rc;
        memcpy(before, fr->data, PAGE_SIZE);
        n = rt_nent(fr->data);
        ent.id = row_id;
        ent.minx = x;
        ent.miny = y;
        ent.maxx = x;
        ent.maxy = y;

        if (n < RT_CAP) {
            rt_ent_put(rt_ents(fr->data) + (size_t)n * RT_ENT_SIZE, &ent);
            n++;
            put_u16(fr->data + RT_NENT_OFF, (u16)n);
            rt_node_mbr(fr->data, &child_mbr);
            extra_pid = PAGE_ID_NIL;
        } else {
            /* 叶子满了 → 页里的 203 条 + 新条目 = 204 条，一起分裂 */
            u32 g1[RT_CAP + 1], g2[RT_CAP + 1];
            u32 n1 = 0, n2 = 0, npid = PAGE_ID_NIL;
            Mbr m1, m2;

            rt_ent_decode(rt_ents_c(fr->data), n, sbuf);
            sbuf[n] = ent;
            rt_quadratic_split(sbuf, n + 1u, g1, &n1, &m1, g2, &n2, &m2);
            rc = rt_split_commit(t, fr, sbuf, g1, n1, g2, n2, &npid);
            if (rc != OK) {
                pager_unpin(t->pg, leaf_pid, true);
                return rc;
            }
            child_mbr = m1;
            extra_pid = npid;
            extra_mbr = m2;
        }

        /* 叶子页上的 n_points：只有当叶子就是根时才记全树点数 */
        put_u32(fr->data + RT_NPOINTS_OFF, (depth == 0) ? newn : 0u);
        /* 先写日志（整页镜像），再把 LSN 写进页头，最后交页 */
        rc = geo_pg_commit(t->pg, leaf_pid, fr, &t->wal, before);
        if (rc != OK)
            return rc;
    }

    /* ---- 往回走：处理每一层父节点 ---- */
    for (i = depth; i-- > 0; ) {
        u32 ppid = path[i];
        PageFrame *fr = NULL;
        u8 before[PAGE_SIZE];   /* 前像：改之前把整页拍下来 */
        u32 n;

        rc = rt_node_get(t, ppid, &fr);
        if (rc != OK)
            return rc;
        memcpy(before, fr->data, PAGE_SIZE);
        n = rt_nent(fr->data);

        /* 把"刚改过的那个孩子"的 MBR 写回指向它的那条条目 */
        rt_ent_set_mbr(rt_ents(fr->data) + (size_t)idx[i] * RT_ENT_SIZE, &child_mbr);

        if (extra_pid != PAGE_ID_NIL) {
            /* 孩子分裂了 → 父节点多一条指向新页的条目 */
            RtEntry ent;

            ent.id = extra_pid;
            ent.minx = extra_mbr.minx;
            ent.miny = extra_mbr.miny;
            ent.maxx = extra_mbr.maxx;
            ent.maxy = extra_mbr.maxy;

            if (n < RT_CAP) {
                rt_ent_put(rt_ents(fr->data) + (size_t)n * RT_ENT_SIZE, &ent);
                n++;
                put_u16(fr->data + RT_NENT_OFF, (u16)n);
                rt_node_mbr(fr->data, &child_mbr);
                extra_pid = PAGE_ID_NIL;
            } else {
                /* 父节点也满了 → 同样把第 204 条留在内存里分裂 */
                u32 g1[RT_CAP + 1], g2[RT_CAP + 1];
                u32 n1 = 0, n2 = 0, npid = PAGE_ID_NIL;
                Mbr m1, m2;

                rt_ent_decode(rt_ents_c(fr->data), n, sbuf);
                sbuf[n] = ent;
                rt_quadratic_split(sbuf, n + 1u, g1, &n1, &m1, g2, &n2, &m2);
                rc = rt_split_commit(t, fr, sbuf, g1, n1, g2, n2, &npid);
                if (rc != OK) {
                    pager_unpin(t->pg, ppid, true);
                    return rc;
                }
                child_mbr = m1;
                extra_pid = npid;
                extra_mbr = m2;
            }
        } else {
            rt_node_mbr(fr->data, &child_mbr);
            extra_pid = PAGE_ID_NIL;
        }

        /* 只有根页记全树点数 */
        put_u32(fr->data + RT_NPOINTS_OFF, (i == 0) ? newn : 0u);
        /* 先写日志（整页镜像），再把 LSN 写进页头，最后交页 */
        rc = geo_pg_commit(t->pg, ppid, fr, &t->wal, before);
        if (rc != OK)
            return rc;
    }

    /* ---- 根分裂 → 新建更高一层的根 ---- */
    if (extra_pid != PAGE_ID_NIL) {
        PageFrame *fr = NULL;
        u32 npid = PAGE_ID_NIL;
        RtEntry e1, e2;

        rc = pager_alloc(t->pg, &npid, &fr);
        if (rc != OK)
            return rc;
        /* 新根的 level = 旧根的 level + 1 = t->height */
        rt_node_init(fr->data, false, t->height);
        e1.id = t->root;
        e1.minx = child_mbr.minx;
        e1.miny = child_mbr.miny;
        e1.maxx = child_mbr.maxx;
        e1.maxy = child_mbr.maxy;
        e2.id = extra_pid;
        e2.minx = extra_mbr.minx;
        e2.miny = extra_mbr.miny;
        e2.maxx = extra_mbr.maxx;
        e2.maxy = extra_mbr.maxy;
        rt_ent_put(rt_ents(fr->data), &e1);
        rt_ent_put(rt_ents(fr->data) + RT_ENT_SIZE, &e2);
        put_u16(fr->data + RT_NENT_OFF, 2);
        put_u32(fr->data + RT_NPOINTS_OFF, newn);
        /* 新根页：前像全零；先写日志，交页在后 */
        rc = geo_pg_commit(t->pg, npid, fr, &t->wal, GEO_ZERO_PAGE);
        if (rc != OK)
            return rc;
        t->root = npid;
        t->height++;
    }

    t->npoints = newn;
    return OK;
}

/* ---------------- 窗口查询 ---------------- */

/*
 * 递归窗口查询（内部实现）。
 *
 * 叶节点：逐条判 mbr_contains_point(win, x, y) —— 闭区间，边界上的点算命
 * 中；内节点：逐条判 mbr_overlap(条目 MBR, win) —— 不相交就整棵子树剪掉。
 *
 * 坑 M4-37：叶子必须写"窗口包含点"而不是"点与窗口相交" —— 叶子条目的 MBR
 * 是退化矩形，两者本来等价，但**判据要逐字一致**：暴力基准写的是
 * mbr_contains_point(win, x, y)，这里就写同一个函数、同一组参数。
 */
static int rt_window_rec(RTree *t, u32 pid, const Mbr *win,
                         u32 *out, u32 max_out, u32 *count, u32 depth)
{
    PageFrame *fr = NULL;
    const u8 *ents;
    u32 n;
    u32 i;
    int rc;

    if (depth > RT_MAX_DEPTH)
        return E_CORRUPT;
    rc = rt_node_get(t, pid, &fr);
    if (rc != OK)
        return rc;

    n = rt_nent(fr->data);
    ents = rt_ents_c(fr->data);

    if (rt_is_leaf(fr->data)) {
        for (i = 0; i < n; i++) {
            RtEntry e;

            rt_ent_get(ents + (size_t)i * RT_ENT_SIZE, &e);
            /* 死条目（id 最高位）直接跳过 —— 标记删除不等于从树上消失 */
            if (e.id & RT_ENT_DEAD)
                continue;
            if (mbr_contains_point(win, e.minx, e.miny)) {
                if (*count >= max_out) {
                    /* 装不下就报错，不静默截断 */
                    pager_unpin(t->pg, pid, false);
                    return E_NOSPACE;
                }
                out[(*count)++] = e.id & RT_ROWID_MAX;
            }
        }
    } else {
        for (i = 0; i < n; i++) {
            RtEntry e;
            Mbr m;

            rt_ent_get(ents + (size_t)i * RT_ENT_SIZE, &e);
            rt_ent_mbr(&e, &m);
            if (!mbr_overlap(&m, win))
                continue;               /* 剪枝：整棵子树都不看 */
            rc = rt_window_rec(t, e.id, win, out, max_out, count, depth + 1);
            if (rc != OK) {
                pager_unpin(t->pg, pid, false);
                return rc;
            }
        }
    }
    pager_unpin(t->pg, pid, false);
    return OK;
}

int rtree_window(RTree *t, const Mbr *win, u32 *out_ids, u32 max_out, u32 *n_out)
{
    u32 count = 0;
    int rc;

    if (t == NULL || win == NULL || out_ids == NULL || n_out == NULL)
        return E_INVAL;
    *n_out = 0;
    if (t->root == PAGE_ID_NIL)
        return OK;                    /* 空树：0 个命中，不是错误 */
    if (max_out == 0)
        return E_NOSPACE;            /* 树非空但一个位置都不给 */

    rc = rt_window_rec(t, t->root, win, out_ids, max_out, &count, 0);
    if (rc != OK)
        return rc;
    *n_out = count;
    return OK;
}

/* ---------------- 对象生命周期 ---------------- */

int rtree_create(Pager *pg, RTree **out)
{
    RTree *t;

    if (pg == NULL || out == NULL)
        return E_INVAL;
    t = (RTree *)calloc(1, sizeof(*t));
    if (t == NULL)
        return E_NOSPACE;
    t->pg = pg;
    t->root = PAGE_ID_NIL;
    *out = t;
    return OK;
}

int rtree_open(Pager *pg, u32 root_page, RTree **out)
{
    RTree *t;
    PageFrame *fr = NULL;
    int rc;

    if (pg == NULL || out == NULL)
        return E_INVAL;
    t = (RTree *)calloc(1, sizeof(*t));
    if (t == NULL)
        return E_NOSPACE;
    t->pg = pg;
    t->root = PAGE_ID_NIL;

    if (root_page != PAGE_ID_NIL) {
        /* 复用 rt_node_get 校验：页类型 / 校验和 / 条目数三条 */
        rc = rt_node_get(t, root_page, &fr);
        if (rc != OK) {
            free(t);
            return rc;
        }
        t->root = root_page;
        t->height = rt_level(fr->data) + 1u;
        t->npoints = rt_npoints(fr->data);
        pager_unpin(pg, root_page, false);
    }
    *out = t;
    return OK;
}

void rtree_destroy(RTree *t)
{
    free(t);
}

/*
 * 接上写日志钩子（v1.4 步骤 6 · A7）。
 *
 * 为什么是"接上去"而不是"创建时传进来"：树对象归 geo_state 管，
 * 由 geo_state_create_wal / geo_state_open_wal 在拿到钩子之后立刻接上，
 * 之后树里每个写页点都会先写日志（见 geo_pg_commit）。
 *
 * 钩子为 NULL ⇒ 本库不写日志（单元测试直接用模态时走这条路）。
 */
void rtree_set_wal(RTree *t, LogHookFn fn, void *ctx)
{
    if (!t)
        return;
    t->wal.fn = fn;
    t->wal.ctx = ctx;
}

u32 rtree_root_page(const RTree *t)
{
    return (t == NULL) ? PAGE_ID_NIL : t->root;
}

u32 rtree_height(const RTree *t)
{
    return (t == NULL) ? 0u : t->height;
}

u32 rtree_npoints(const RTree *t)
{
    return (t == NULL) ? 0u : t->npoints;
}

u64 rtree_page_reads(const RTree *t)
{
    return (t == NULL) ? 0u : t->page_reads;
}
