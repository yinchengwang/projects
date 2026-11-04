#ifndef DB_MODALITIES_GEO_RTREE_SCAN_H
#define DB_MODALITIES_GEO_RTREE_SCAN_H

#include "common/error.h"
#include "common/types.h"
#include "storage/pager.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * rtree_scan.h —— R-tree 的**全树枚举**与**死条目标记**（v1.4 步骤 5）
 *
 * 【为什么单独一个文件】
 *
 * rtree.c 已经 785 行（上限 800）。这两个能力再加进去就超限了，
 * 所以拆成独立模块 —— 与 rtree_knn.c 同样的理由。
 *
 * 【为什么自己读页，而不是用 RTree 对象】
 *
 * 与 rtree_knn.c 同款：这两个能力只需要"页布局 + pager"，
 * 不需要 RTree 的私有状态。RTree 的结构体是 rtree.c 的私有实现，
 * 别的文件碰不到（所以本模块的入口都收 (Pager *, root_page)）。
 *
 * 【死条目怎么表示】
 *
 * 叶子条目的 id 字段的**最高位**是"已删除"标志：
 *
 *   bit31 = 1  → 这条已被标记删除（查询时跳过）
 *   bit30..0   → row_id（所以 row_id 上限是 2^31-1）
 *
 * 为什么不用"再加一个字段"：条目是定长 20 字节，页里塞得下 203 条。
 * 加一个字节就要重排条目布局（MBR 就没法 4 字节对齐了），
 * 而 flag 方案**一个字节都不用加**。
 *
 * 内部节点的 id 是 child_page，**不带这个标志** ——
 * 区分靠页里的 level 字段：level == 0 才是叶子。
 *
 * 【删除不重排（不 rebalance）】
 *
 * 标记死条目之后：
 *   - 窗口查询 / KNN 跳过它；
 *   - 树的结构**不变**（不合并、不搬条目）；
 *   - 页还是占着 —— 空间不回收。
 *
 * 这与 B+tree 的"标记死条目"是同一个先例：
 * 死条目留着，等 reindex / vacuum 统一处理。
 * 重建由 db_geo_reindex() 负责（它把死条目**真正**清掉）。
 */

/* 一个叶子点（解码后的形式） */
typedef struct {
    u32   row_id;   /* 条目 id 的低 31 位 */
    float x;        /* 叶子的退化矩形：x == minx == maxx */
    float y;        /* 叶子的退化矩形：y == miny == maxy */
    bool  dead;     /* true = 已被标记删除 */
} RTreePoint;

/*
 * 枚举回调。
 *
 * 返回非 OK 即中止整个枚举（该错误码原样返回给调用方）。
 * 回调里**不要**再调用本模块的函数（会重入读页）。
 */
typedef int (*RTreePointCb)(const RTreePoint *p, void *ctx);

/*
 * 枚举整棵树的所有叶子点（**含死条目**）。
 *
 * root_page == PAGE_ID_NIL（空树）→ 直接返回 OK，一个点都不回调。
 *
 * 顺序是"树序"（深度优先），**不是 row_id 序**。
 * 要按 row_id 排序请调用方自己排（geo_ids_sort）。
 *
 * 返回 OK / E_CORRUPT（坏页）/ E_NOSPACE / 回调返回的错误码。
 *
 * page_reads_out —— 可为 NULL；非 NULL 时写入本次的**逻辑页读**
 *   （pager_get 次数，含缓冲池命中）。与 rtree_knn 的同名参数同一个口径。
 */
int rtree_scan_all(Pager *pg, u32 root_page, RTreePointCb cb, void *ctx,
                    u64 *page_reads_out);

/*
 * 把 row_id 对应的那条叶子条目标记为"已删除"。
 *
 * - 找不到该 row_id → E_NOTFOUND；
 * - 已经是死条目 → 幂等，返回 OK（不算错误：重复删是合法的空操作）；
 * - 树是空的 → E_NOTFOUND。
 *
 * page_reads_out —— 可为 NULL；非 NULL 时写入本次的逻辑页读。
 *
 * 【代价：O(n)】
 *
 * 删除时**不知道点在哪片叶子**（没有"按 row_id 查坐标"的索引），
 * 所以只能全树扫一遍找到它。这是"标记而不重排"的代价，
 * 如实记录在文档里。
 */
int rtree_mark_dead(Pager *pg, u32 root_page, u32 row_id, u64 *page_reads_out);

/*
 * 数一遍树里的活点数与总条目数。
 *
 * n_live  = 没被标记删除的条目数
 * n_total = 全部条目数（含死条目）
 *
 * 两个指针都可以为 NULL（不关心就别取）。
 * 空树 → 两个都是 0。
 *
 * page_reads_out —— 可为 NULL；非 NULL 时写入本次的逻辑页读。
 */
int rtree_count_live(Pager *pg, u32 root_page, u32 *n_live, u32 *n_total,
                     u64 *page_reads_out);

#ifdef __cplusplus
}
#endif

#endif /* DB_MODALITIES_GEO_RTREE_SCAN_H */
