/*
 * geo_state.c —— 地理空间模态的**实例状态**（v1.4 步骤 5）
 *
 * 契约见 geo_state.h（格式、字段、返回值都在那里定死了）。
 * 这里只补实现层面的说明。
 *
 * ==================== 这一层在干什么 ====================
 *
 * R-tree 管的是"树"：页在哪、根是谁、怎么插怎么查。
 * 但模态还需要三件树不管的事：
 *
 *   1. **根页号要落在某处** —— 根页会变（根分裂时换页），
 *      重开库时得知道该从哪一页把树打开。
 *   2. **活点数 / 总条目数** —— 树里的 n_points 记的是"总条目数"，
 *      死条目也算在里面；对外的"有多少个点"要的是**活**点数。
 *   3. **这一套东西在磁盘上的落点** —— 模态不许碰 catalog
 *      （硬规则 6：模态各自管自己的元数据），所以要自己的一页。
 *
 * ==================== meta 页 ====================
 *
 * meta 页（页类型 PT_GEO_META）里只有两样东西：格式版本 + 根页号。
 *
 * 为什么不把活点数也记进去：那样每次插入/删除都要改 meta 页；
 * 而活点数**本来就能从树里数出来**（rtree_count_live），
 * 开库时数一次即可 —— 少一处会被写坏的状态。
 *
 * meta 页只在**根页号变了**的时候才写（geo_state_sync_root）。
 * 根分裂是罕见事件，所以绝大多数插入不碰 meta 页。
 *
 * ==================== 怎么在开库时找到 meta 页 ====================
 *
 * 扫页空间：0 .. pager_npages()-1 逐页看页类型。
 *
 * ⚠️ **这是已知欠账**（坑 M5-9）：开库时间随库大小线性增长。
 * 本轮没有照抄全文检索的"双根页"目录链机制（那是 FT 专用的），
 * 先用扫页空间把链路跑通，如实记下代价。
 *
 * ==================== WAL：v1.4 步骤 6（A7）已接入 ====================
 *
 * meta 页与树页现在都**进 WAL**（整页镜像，见 geo_wal.h）：
 * 写页的顺序是"先写日志 → 再改页 → LSN 写进页头"，
 * 断电后由 WAL 重放把页补回来。
 *
 * ⚠️ geo 的写**没有事务边界**：日志只用于 redo，**不用于 undo**
 * （xid 固定 0 = 系统事务，恢复时永不回滚）。
 * 将来要把 geo 的写纳入用户事务，必须同时补真正的 undo。
 *
 * ⚠️ 坑 M5-16：地理模态的点**只存在树里**（没有第二份行存），
 * 所以 WAL 是**唯一**的恢复路径 —— 没有 WAL ⇒ 崩溃后这批点就是没了。
 */

#include <stdlib.h>
#include <string.h>

#include "common/config.h"
#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/geo_state.h"
#include "modalities/geo/rtree.h"
#include "modalities/geo/rtree_scan.h"
#include "storage/page.h"
#include "storage/pager.h"

/*
 * 写 meta 页：版本 + 根页号。
 *
 * 【为什么写完要重新算校验和】
 *
 * 校验和覆盖整页（见 page.c）。改了根页号却不重算，
 * 校验和就与内容对不上 —— 以后再打开会被当成坏页。
 * （有写日志钩子时，page_set_lsn 会顺带重算 —— 见 geo_pg_commit。）
 *
 * 【为什么 meta 页也要写日志】
 *
 * meta 页记的是**根页号**。根分裂时根页号会变，这时 meta 页是
 * 唯一知道"新根是谁"的地方 —— 它丢了，整棵树就找不回来了。
 */
static int geo_meta_store(Pager *pg, u32 pid, u32 root, const GeoWal *wal)
{
    PageFrame *fr = NULL;
    u8 before[PAGE_SIZE];
    int rc;

    rc = pager_get(pg, pid, &fr);
    if (rc != OK) {
        return rc;
    }
    memcpy(before, fr->data, PAGE_SIZE);   /* 前像：改之前拍下来 */
    fr->data[GEO_META_VER_OFF] = (u8)GEO_META_VER;
    put_u32(fr->data + GEO_META_ROOT_OFF, root);
    /* 先写日志（整页镜像），再交页 */
    return geo_pg_commit(pg, pid, fr, wal, before);
}

/* ---------------- 新建 ---------------- */

int geo_state_create(Pager *pg, GeoState **out)
{
    return geo_state_create_wal(pg, NULL, NULL, out);
}

int geo_state_create_wal(Pager *pg, LogHookFn fn, void *ctx, GeoState **out)
{
    GeoState *st;
    RTree *tree = NULL;
    PageFrame *fr = NULL;
    GeoWal wal;
    u32 pid = 0;
    int rc;

    if (!pg || !out) {
        return E_INVAL;
    }
    *out = NULL;

    wal.fn = fn;
    wal.ctx = ctx;

    /*
     * 分配 meta 页。
     *
     * pager_alloc 返回的帧**已经 pin 过**（而且是脏的、已清零的），
     * 所以这里直接写它，最后 unpin 一次 —— 不需要再 get。
     */
    rc = pager_alloc(pg, &pid, &fr);
    if (rc != OK) {
        return rc;
    }

    /*
     * 【坑 M5-10：页号 0 归 catalog 的元数据页】
     *
     * catalog 的根指针页固定是 PAGE_ID_META(0)（见 catalog.c 的
     * bootstrap：它断言 pager_alloc 给出的第一个页就是 0）。
     * 地理模态要是占了页 0，catalog 之后 bootstrap 就会因为
     * "文件不空却要走 bootstrap" 而报 E_CORRUPT —— 整库开不起来。
     *
     * 所以这里**主动避开页 0**：万一拿到 0，就再要一页，
     * 把第一页留成一张全零的闲页（页归 pager 管，本层不回收）。
     *
     * ⚠️ 注意：这只能保证"地理模态不占页 0"，
     * 救不了"在 catalog 之前分配任何页"这个更大的次序问题 ——
     * db 层必须**先开 catalog、再开 geo**（步骤 6 接线时要注意）。
     */
    if (pid == PAGE_ID_META) {
        u32 pid2 = 0;
        PageFrame *fr2 = NULL;

        rc = pager_alloc(pg, &pid2, &fr2);
        /* 闲页照样要还（pin/unpin 必须配对） */
        (void)pager_unpin(pg, pid, false);
        if (rc != OK) {
            return rc;
        }
        pid = pid2;
        fr = fr2;
    }

    page_init(fr->data, (u8)PT_GEO_META);
    fr->data[GEO_META_VER_OFF] = (u8)GEO_META_VER;
    /* 空树：还没有根页 */
    put_u32(fr->data + GEO_META_ROOT_OFF, PAGE_ID_NIL);

    /* 新页：前像全零；先写日志，交页在后 */
    rc = geo_pg_commit(pg, pid, fr, &wal, GEO_ZERO_PAGE);
    if (rc != OK) {
        return rc;
    }

    /*
     * 空树：**不分配任何页**。
     *
     * 根页在第一次插入时才分配 —— 空树没有根页可言。
     * 这样"建了 geo 存储但一个点都没插"的库只花一页（meta 页）。
     */
    rc = rtree_create(pg, &tree);
    if (rc != OK) {
        return rc;
    }
    /* 树也接上同一个钩子 —— 之后每次插入/删除都会写日志 */
    rtree_set_wal(tree, fn, ctx);

    st = (GeoState *)calloc(1, sizeof(GeoState));
    if (st == NULL) {
        rtree_destroy(tree);
        return E_NOSPACE;
    }
    st->pg = pg;
    st->tree = tree;
    st->meta_page = pid;
    st->root_cached = PAGE_ID_NIL;
    st->n_live = 0;
    st->n_total = 0;
    st->page_reads = 0;
    st->wal = wal;
    *out = st;
    return OK;
}

/* ---------------- 打开 ---------------- */

int geo_state_open(Pager *pg, GeoState **out)
{
    return geo_state_open_wal(pg, NULL, NULL, out);
}

int geo_state_open_wal(Pager *pg, LogHookFn fn, void *ctx, GeoState **out)
{
    GeoState *st;
    RTree *tree = NULL;
    PageFrame *fr = NULL;
    GeoWal wal;
    u64 npages;
    u32 pid;
    u32 meta = PAGE_ID_NIL;
    u32 root = PAGE_ID_NIL;
    int rc;

    if (!pg || !out) {
        return E_INVAL;
    }
    *out = NULL;

    wal.fn = fn;
    wal.ctx = ctx;

    /*
     * 扫页空间找 meta 页（坑 M5-9：开库时间随库大小线性增长）。
     *
     * 【pin/unpin 必须配对】
     *
     * 每取一页都要还回去 —— 哪怕这一页不是 meta 页。
     * 漏一次 unpin 会把帧钉死，帧池很快就会耗尽
     * （pager_get 返回 E_NOSPACE）。
     */
    npages = pager_npages(pg);
    for (pid = 0; (u64)pid < npages; pid++) {
        rc = pager_get(pg, pid, &fr);
        if (rc != OK) {
            return rc;
        }
        if (fr->data[H_TYPE] == (u8)PT_GEO_META) {
            meta = pid;
            (void)pager_unpin(pg, pid, false);
            break;
        }
        (void)pager_unpin(pg, pid, false);
    }

    /*
     * 磁盘上没有 meta 页 → E_NOTFOUND。
     *
     * 这**不是错误**：那是个还没建过几何存储的库
     * （与图模态同一个口径）—— 上层据此决定"要不要建一个"。
     */
    if (meta == PAGE_ID_NIL) {
        return E_NOTFOUND;
    }

    /* 读 meta 页：校验类型、校验和、版本、根页号 */
    rc = pager_get(pg, meta, &fr);
    if (rc != OK) {
        return rc;
    }
    if (fr->data[H_TYPE] != (u8)PT_GEO_META) {
        (void)pager_unpin(pg, meta, false);
        return E_CORRUPT;
    }
    if (!page_verify_checksum(fr->data)) {
        (void)pager_unpin(pg, meta, false);
        return E_CORRUPT;
    }
    if (fr->data[GEO_META_VER_OFF] != (u8)GEO_META_VER) {
        /* 版本不认识 ⇒ 这个页不是本版本写的 ⇒ 不能猜着读 */
        (void)pager_unpin(pg, meta, false);
        return E_CORRUPT;
    }
    root = get_u32(fr->data + GEO_META_ROOT_OFF);
    rc = pager_unpin(pg, meta, false);
    if (rc != OK) {
        return rc;
    }

    /*
     * 打开树。
     *
     * root == PAGE_ID_NIL = 空树（建了但一个点都没插）。
     * rtree_open 会校验页类型与校验和，不对就报 E_CORRUPT ——
     * 不返回一棵"看起来能查但结果悄悄不对"的树。
     */
    rc = rtree_open(pg, root, &tree);
    if (rc != OK) {
        return rc;
    }
    /* 树接上写日志钩子 —— 之后每次插入/删除都会写日志 */
    rtree_set_wal(tree, fn, ctx);

    st = (GeoState *)calloc(1, sizeof(GeoState));
    if (st == NULL) {
        rtree_destroy(tree);
        return E_NOSPACE;
    }
    st->pg = pg;
    st->tree = tree;
    st->meta_page = meta;
    st->root_cached = root;
    st->n_live = 0;
    st->n_total = 0;
    st->page_reads = 0;
    st->wal = wal;

    /* 开库时数一遍活点数 / 总条目数 */
    rc = geo_state_refresh_counts(st);
    if (rc != OK) {
        geo_state_destroy(st);
        return rc;
    }

    *out = st;
    return OK;
}

/* ---------------- 释放 ---------------- */

void geo_state_destroy(GeoState *st)
{
    if (!st) {
        return;
    }
    /* 页归 pager 管 —— 这里只放内存 */
    rtree_destroy(st->tree);
    free(st);
}

/* ---------------- 根页号回写 ---------------- */

int geo_state_sync_root(GeoState *st)
{
    u32 cur;
    int rc;

    if (!st || !st->tree || !st->pg) {
        return E_INVAL;
    }
    cur = rtree_root_page(st->tree);

    /*
     * 和**缓存的**根页号比，而不是回读 meta 页。
     *
     * 第一版每次都回读一遍 meta 页再比 —— 那多一次页读，
     * 而这次页读发生在"每次插入"这条最热的路径上。
     * 缓存根页号之后：根没变 ⇒ **一个页都不碰**（绝大多数插入都走这条路）。
     */
    if (st->root_cached == cur) {
        return OK;
    }

    rc = geo_meta_store(st->pg, st->meta_page, cur, &st->wal);
    if (rc != OK) {
        return rc;
    }
    st->root_cached = cur;
    return OK;
}

/* ---------------- 计数刷新 ---------------- */

int geo_state_refresh_counts(GeoState *st)
{
    u32 live = 0;
    u32 total = 0;
    u64 reads = 0;
    int rc;

    if (!st || !st->tree || !st->pg) {
        return E_INVAL;
    }
    rc = rtree_count_live(st->pg, rtree_root_page(st->tree), &live, &total, &reads);
    if (rc != OK) {
        return rc;
    }
    st->n_live = live;
    st->n_total = total;
    /* 页读是**累计**的（诊断用：这次开库扫了多少页） */
    st->page_reads += reads;
    return OK;
}
