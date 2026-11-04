#ifndef DB_MODALITIES_GEO_GEO_BRUTE_H
#define DB_MODALITIES_GEO_GEO_BRUTE_H

#include "common/error.h"
#include "common/types.h"
#include "modalities/geo/mbr.h"

/*
 * 暴力基准实现（v1.4 步骤 1）。
 *
 * 【它是什么】
 *
 * R-tree 的**验收基准**：把全部点摊开扫一遍，按定义算出答案。
 * 步骤 2/3 的 R-tree 结果必须与它**全等**（窗口集合相同、KNN 顺序相同）。
 *
 * 【为什么"全等"是唯一站得住的验收】
 *
 * 空间查询最容易出的错是**静默漏点**：
 * 剪枝条件写反一个符号、少算一条边界，结果就少几个点，
 * 而且**不报错** —— 只是答案悄悄不对。
 * "看起来差不多"这种验收抓不住这类 bug，只有逐点全等能抓住。
 *
 * 【输出顺序的约定】
 *
 * - 暴力窗口：按**输入顺序**返回（调用方按 row_id 升序给点，输出就是升序）。
 * - 暴力 KNN：按 (距离升序, row_id 升序) 返回。
 *
 * R-tree 的窗口遍历天然是"树序"而不是 row_id 序，所以
 * **两边逐位对照前必须先各自按 row_id 排序** —— 用 geo_ids_sort()。
 * 集合全等才是验收的判据，顺序只是对照时的整理手段。
 *
 * 【tie-break 为什么定死为 row_id 升序】
 *
 * 浮点距离并列时（网格点、对称分布）"第 k 名"可能不唯一。
 * 若两边各自随机挑一个，全等验收会被浮点噪声随机击穿。
 * 所以并列一律按 row_id 升序 —— 树与暴力都用同一条规则。
 */

typedef struct {
    u32  row_id;   /* 行标识（R-tree 叶子条目里存的就是它） */
    float x;
    float y;
} GeoPoint;

/*
 * 暴力窗口查询。
 *
 * 返回所有落在窗口（含边界）内的 row_id。
 * 命中数 > max_out 时返回 E_NOSPACE（**不静默截断**）。
 */
int geo_brute_window(const GeoPoint *pts, u32 n, const Mbr *win,
                     u32 *out_ids, u32 max_out, u32 *n_out);

/*
 * 暴力 KNN（精确）。
 *
 * 按 (距离升序, row_id 升序) 返回前 k 个点的 row_id。
 * 结果个数 = min(k, 点数)；装不下（min(k,点数) > max_out）时返回 E_NOSPACE。
 */
int geo_brute_knn(const GeoPoint *pts, u32 n, float qx, float qy, u32 k,
                  u32 *out_ids, u32 max_out, u32 *n_out);

/* 把 row_id 数组按升序排好（两边逐位对照前的统一整理）。 */
void geo_ids_sort(u32 *ids, u32 n);

#endif /* DB_MODALITIES_GEO_GEO_BRUTE_H */
