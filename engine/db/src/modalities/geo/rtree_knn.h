#ifndef DB_MODALITIES_GEO_RTREE_KNN_H
#define DB_MODALITIES_GEO_RTREE_KNN_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/mbr.h"
#include "storage/pager.h"

/*
 * 一个带坐标与距离的 KNN 候选（W-D 的 D-5）。
 *
 * dist 是**平方距离**（`geo_dist2()` 的口径，与 db_geo_hit.dist /
 * geo_brute_knn 完全一致 —— 否则"结果全等"这条验收会被浮点末位差随机击穿）。
 * x / y 是那个点的坐标（叶子条目的 MBR 是退化矩形，minx/miny 就是它）。
 *
 * row_id —— 已剥掉 RT_ENT_DEAD 标志的实际行号。
 */
typedef struct {
    u32   row_id;
    float dist;
    float x;
    float y;
} KnPoint;

/*
 * R-tree 的**精确** KNN（mindist 优先队列）（v1.4 步骤 3）。
 *
 * 【与 v1.0 的 HNSW 形成对照】
 *
 * HNSW 的 KNN 是**近似**（用 ef 换召回）；这里是**精确**算法：
 *   优先队列（按 mindist(节点MBR, 查询点) 最小堆）：
 *     弹出 → 若是叶子条目：入结果集（真实距离）
 *           → 若是内部节点：把子条目按 mindist 压栈
 *   结果集满 k 个且**堆顶 mindist > 第 k 近的真实距离** → 提前终止
 *
 * 两版对照是设计教育点：近似的代价是召回、收益是跳过大部分树；
 * 精确的代价是必须展开所有"可能更近"的子树、收益是结果无懈可击。
 *
 * 【为什么是单独一个文件，而不是加进 rtree.c】
 *
 * rtree.c 已经 785 行（上限 800），再加 KNN 会超限；
 * 而且 rtree.c 的内部状态是私有的。
 * 所以 KNN 作为**自包含模块**：自己按 rtree.h 里公开的页布局读节点页。
 * 调用方持有 Pager 与根页号即可调用。
 *
 * 【与暴力基准同源】
 *
 * 叶子条目的真实距离一律走 `geo_dist2()`；
 * 剪枝用的 mindist 一律走 `mbr_mindist2()`。
 * 与 `geo_brute_knn()` 用的是**同一套算术** ——
 * 否则浮点末位差异会让"结果全等"这条验收被噪声随机击穿。
 *
 * 【tie-break：row_id 升序】
 *
 * 浮点距离并列时（网格点、对称分布 —— 空间数据的常态）
 * "第 k 名"不唯一。本实现定死 **row_id 升序**，
 * 与暴力基准、与计划 §1.2 判决 4 一致。
 */

/*
 * 精确 KNN。
 *
 * 按 (距离升序, row_id 升序) 返回离 (qx, qy) 最近的前 k 个点的 row_id。
 *
 * - 结果个数 = min(k, 树里的**活点**数)（被标记删除的点不进结果集）；
 *   装不下（min(k, 活点数) > max_out）返回 E_NOSPACE。
 *   ⚠️ 这个判据是**事后精确判**的（查完活点才判），不是查之前用"总点数"估的 ——
 *   用总点数（含死条目）估会在"活点数 ≤ max_out < 总点数"时保守误报，
 *   见坑 M5-11。
 * - root_page == PAGE_ID_NIL（空树）→ 返回 0 条。
 * - k == 0 → 返回 0 条。
 * - *page_reads_out 可为 NULL；非 NULL 时写入本次查询的**逻辑页读**
 *   （pager_get 次数，含缓冲池命中）。
 *
 * ⚠️ 只回传 row_id、**丢掉它自己算出来的距离与坐标** —— 这是 W-D 的 D-5 要治的
 * 那个毛病：需要距离的调用方（db_geo_knn）只好再摊平一遍树。用
 * `rtree_knn_points()` 就不用。
 */
int rtree_knn(Pager *pg, u32 root_page, float qx, float qy, u32 k,
              u32 *out_ids, u32 max_out, u32 *n_out, u64 *page_reads_out);

/*
 * 树里的点数（从根页的 n_points 字段读，O(1)）。
 * 根页不可用时返回 0。
 */
u32 rtree_knn_npoints(Pager *pg, u32 root_page);

/*
 * ==================== 带坐标与距离的精确 KNN（W-D 的 D-5） ====================
 *
 * 【为什么另开一个函数而不是改 rtree_knn】
 *
 * rtree_knn 的签名被 test_geo_knn.c 的十几处断言固定住了，改签名就要动别人的测试；
 * 而且它有一个更根本的问题：**它在叶子条目上算出了真实距离又丢掉了**
 * （`geo_dist2(qx, qy, m.minx, m.miny)` 只是拿去排序，结果只回传 row_id）。
 *
 * 调用方（db_geo_knn）为了填 db_geo_hit.dist，只好**再摊平整棵树**把坐标捞回来 ——
 * 实测 10 万点 k=10：rtree_knn 自己只要 **11** 次页读，
 * 而 db_geo_knn 一共花了 **739** 次（其中 728 次是那第二遍摊平）。
 *
 * 这个函数把**算出来的东西原样带回去**，于是第二遍扫描可以整个删掉。
 *
 * 【KnPoint 里的 x/y 与 dist 都是 rtree_knn 内部本来就有、却丢掉的】
 *
 * 叶子条目的 MBR 是点的退化矩形（`m.minx == m.maxx == x`），
 * 所以坐标不用额外读页，就是 `rtk_ent_get` 已经解出来的 minx / miny。
 * dist 是 `geo_dist2()` 的**平方距离**，与 db_geo_hit.dist、与 geo_brute_knn 同一口径。
 *
 * 【死条目】
 *
 * 与 rtree_knn 一致：死条目不进结果集。所以"要 k 个候选"就是 k 个**活点**，
 * 调用方**不需要**预先摊平全树去数死条目（那正是第二遍扫描的动机）。
 *
 * 【判据】结果个数 = min(k, 活点数)；> max_out 时返回 E_NOSPACE（事后精确判，
 * 见坑 M5-11）。装不下时 out 里是**半截结果**，调用方必须按返回值判断，
 * 不能只看 *n_out。
 */
int rtree_knn_points(Pager *pg, u32 root_page, float qx, float qy, u32 k,
                     KnPoint *out, u32 max_out, u32 *n_out, u64 *page_reads_out);

#endif /* DB_MODALITIES_GEO_RTREE_KNN_H */
