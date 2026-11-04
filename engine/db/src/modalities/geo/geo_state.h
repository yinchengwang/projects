#ifndef DB_MODALITIES_GEO_STATE_H
#define DB_MODALITIES_GEO_STATE_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/rtree.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * geo_state.h —— 地理空间模态的**实例状态**（v1.4 步骤 5）
 *
 * 【这个结构解决什么】
 *
 * R-tree 本身只管"树"：页在哪、根是谁、怎么插怎么查。
 * 但模态还需要三件树不管的事：
 *
 *   1. **根页号要落在某处** —— 根页会变（根分裂时换页），
 *      重开库时得知道该从哪一页把树打开。
 *   2. **活点数 / 总条目数** —— 树里的 n_points 记的是"总条目数"，
 *      死条目也算在里面；对外的"有多少个点"要的是**活**点数。
 *   3. **这一套东西在磁盘上的落点** —— 模态不许碰 catalog
 *      （硬规则 6：模态各自管自己的元数据），所以要自己的一页。
 *
 * 【meta 页：只记根页号】
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
 * 【怎么在开库时找到 meta 页】
 *
 * 扫页空间：0 .. pager_npages()-1 逐页看页类型。
 *
 * ⚠️ **这是已知欠账**（记为坑 M5-9）：开库时间随库大小线性增长。
 * 全文检索在 v1.3 欠账 3 里已经用"双根页"解决过同一个问题，
 * 本轮**没有**照抄那套（那是 FT 专用的目录链机制），
 * 先用扫页空间把链路跑通，如实记下代价。
 *
 * 【没有 WAL】
 *
 * meta 页与树页都**不进 WAL**（与 R-tree 本身一致，坑 M4-38）。
 * 断电可能留下半截的树 —— 已知状态，不是 bug。
 * 崩溃恢复属于步骤 6（A7）。
 */

/* 页类型：接在 PT_FT_CATALOG(0x14) 之后，与 common/config.h 不冲突 */
#define PT_GEO_META 0x16u

/* meta 页的格式版本 */
#define GEO_META_VER 1u

/*
 * meta 页的自定义区（从偏移 20 开始 —— 前 20 字节是标准页头）。
 *
 *  20 GEO_META_VER   u8   格式版本
 *  24 GEO_META_ROOT  u32  根页号（PAGE_ID_NIL = 空树）
 */
enum {
    GEO_META_VER_OFF  = 20,
    GEO_META_ROOT_OFF = 24
};

/* 模态的实例状态 */
typedef struct {
    Pager *pg;
    RTree *tree;       /* 树对象（内存态：根页号 / 树高 / 计数） */
    u32    meta_page;  /* meta 页页号 */
    u32    root_cached;/* 缓存的根页号：避免每次插入都去读一遍 meta 页 */
    u32    n_live;     /* 活点数（不含死条目） */
    u32    n_total;    /* 总条目数（含死条目）；下一个 row_id 就是它 */

    /*
     * 累计逻辑页读（诊断用，喂给 db_geo_page_reads）。
     *
     * 三个来源相加：
     *   rtree_page_reads(tree)  —— rtree.c 自己的（插入 / 窗口查询）
     *   rtree_knn 的 page_reads —— KNN 的
     *   rtree_scan 的 page_reads —— 枚举 / 标记死条目 / 计数的
     *
     * 【口径：逻辑页读，不是磁盘 IO】
     *
     * 与 db_vec_page_reads / db_ft_page_reads 同一个口径：
     * 数的是 pager_get 的次数，缓冲池命中也算。
     * 真要量磁盘 IO 得用 pager_physical_reads()（那是**整个 pager** 的，
     * 没法只算地理模态的）。
     */
    u64 page_reads;
} GeoState;

/*
 * 新建一库的几何存储：分配 meta 页，建一棵空树。
 *
 * 返回 OK / E_NOSPACE / E_INVAL。
 */
int geo_state_create(Pager *pg, GeoState **out);

/*
 * 打开磁盘上已有的几何存储。
 *
 * 磁盘上没有 meta 页 → E_NOTFOUND（**不是错误**：
 * 那是个还没建过几何存储的库，和图模态一样）。
 * meta 页读不懂 → E_CORRUPT。
 *
 * 打开后会扫一遍树，把 n_live / n_total 数出来。
 */
int geo_state_open(Pager *pg, GeoState **out);

/* 释放（**不释放页** —— 页归 pager 管）。 */
void geo_state_destroy(GeoState *st);

/*
 * 把当前的根页号写回 meta 页（**只在根页号变了的时候才真的写**）。
 *
 * 每次插入/删除后都该调一次：它内部比较新旧根页号，没变就直接返回 OK。
 */
int geo_state_sync_root(GeoState *st);

/*
 * 扫一遍树，重算 n_live / n_total。
 *
 * 开库时用；reindex 之后也用。
 */
int geo_state_refresh_counts(GeoState *st);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_STATE_H */
