#ifndef DB_MODALITIES_GEO_RTREE_PRIV_H
#define DB_MODALITIES_GEO_RTREE_PRIV_H

#include "common/types.h"
#include "modalities/geo/mbr.h"
#include "modalities/geo/rtree.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rtree_priv.h —— R-tree 的**私有胶水**（rtree.c 与 rtree_split.c 之间）
 *
 * ==================== 为什么要有这个文件 ====================
 *
 * rtree.c 已经贴着 800 行的上限；v1.4 步骤 6（A7）要在里面加
 * 写日志的接线，还得再加行数。所以把**二次分裂**那一段拆出去
 * （rtree_split.c），两边共用的东西放这个私有头里。
 *
 * 与 rtree_knn.c / rtree_scan.c 的"自己读页"不同：这两个模块
 * 只用到页布局；而分裂逻辑需要 R-tree 的条目解码形式，
 * 所以共用件必须有个落点 —— 就是本文件。
 *
 * ==================== 条目的内存解码形式 ====================
 *
 * 叶子 id = row_id，内部 id = child_page；MBR 四个 float。
 * 磁盘上条目是 20 字节的定长块，内存里解码成结构体才好算。
 */

/* 条目的内存解码形式：叶子 id = row_id，内部 id = child_page */
typedef struct {
    u32   id;
    float minx, miny, maxx, maxy;
} RtEntry;

/* 解码：页内条目块 → 内存条目 */
static inline void rt_ent_get(const u8 *p, RtEntry *e)
{
    e->id = get_u32(p);
    e->minx = rt_u2f(get_u32(p + 4));
    e->miny = rt_u2f(get_u32(p + 8));
    e->maxx = rt_u2f(get_u32(p + 12));
    e->maxy = rt_u2f(get_u32(p + 16));
}

/* 编码：内存条目 → 页内条目块 */
static inline void rt_ent_put(u8 *p, const RtEntry *e)
{
    put_u32(p, e->id);
    put_u32(p + 4, rt_f2u(e->minx));
    put_u32(p + 8, rt_f2u(e->miny));
    put_u32(p + 12, rt_f2u(e->maxx));
    put_u32(p + 16, rt_f2u(e->maxy));
}

/* 条目的包围盒 */
static inline void rt_ent_mbr(const RtEntry *e, Mbr *m)
{
    *m = (Mbr){ e->minx, e->miny, e->maxx, e->maxy };
}

/* 把一组下标排成升序（原地） */
void rt_sort_idx(u32 *a, u32 n);

/*
 * QuadraticSplit（Guttman 1984 §3.3）。
 *
 * 输入：溢出的节点页（n = M + 1 = 204 条）；输出：两组条目的下标。
 */
void rt_quadratic_split(const RtEntry *ents, u32 n,
                        u32 *g1, u32 *n1_out, Mbr *mbr1,
                        u32 *g2, u32 *n2_out, Mbr *mbr2);

/* 把一组（解码后的）条目写进目标页的条目区 */
void rt_group_write(u8 *dst, const RtEntry *src, const u32 *g, u32 n);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_RTREE_PRIV_H */
