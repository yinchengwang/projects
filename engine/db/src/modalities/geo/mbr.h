#ifndef DB_MODALITIES_GEO_MBR_H
#define DB_MODALITIES_GEO_MBR_H

#include "common/types.h"

/*
 * MBR —— 最小包围矩形（Minimum Bounding Rectangle）。
 *
 * 【为什么空间数据的"键"是一个矩形，而不是一个值】
 *
 * B+tree 的键是**一维全序**：两个键要么小于、要么大于、要么相等。
 * 空间数据没有全序 —— 两个矩形可以"相交，但谁也不大于谁"。
 * 所以 R-tree 的键是**矩形本身**，树里的"比较"不是大小比较，
 * 而是三种几何关系：**相交 / 包含 / 距离**。
 *
 * 本文件是 v1.4 的纯函数底座：没有页、没有锁、没有分配、不碰 I/O。
 * 步骤 2 的 R-tree 与步骤 5 的暴力基准**共用**这里的每一个函数 ——
 * 两边算出来的数必须逐位相同，否则"结果全等"这条验收会被浮点噪声击穿。
 *
 * 【本文件的语义约定（v1.4 全程必须一致）】
 *
 * 1. **闭区间**：min/max 都是闭的。
 *    - 点落在矩形**边界上**算"在矩形内"；
 *    - 两个矩形**共边**（相接但不重叠）算"相交"。
 *    这条决定了窗口查询会不会漏掉边界点 —— 剪枝用开区间、
 *    查询用闭区间，是最经典的漏点来源。这里全库统一闭区间。
 *
 * 2. **约定 min <= max**（允许相等 → 退化成一条线或一个点）。
 *    mbr_set_rect 会**归一化**（传反了自动交换），
 *    免得调用方因为传反参数就得到一个"面积为负"的矩形。
 *
 * 3. 坐标必须是**有限值**（不是 NaN / Inf）。
 *    NaN 会让所有比较返回 false —— 排序与剪枝会**静默失效**，
 *    本内核不替调用方检查，但约定写在这里。
 *
 * 4. **距离一律用平方距离（dist2）参与比较**。
 *    少一次 sqrt，而且 sqrt 是单调的 —— 排序结果完全一致。
 *    唯一要注意的是：平方距离相等**不代表**距离相等吗？反之，
 *    对非负的数，dist2 相等 ⟺ dist 相等（sqrt 单调且可逆）。
 */

typedef struct {
    float minx;
    float miny;
    float maxx;
    float maxy;
} Mbr;

/* ---------------- 构造 ---------------- */

/* 点退化成一个面积为 0 的矩形（min == max）。 */
void mbr_set_point(Mbr *m, float x, float y);

/* 构造矩形；min > max 时自动交换（归一化）。 */
void mbr_set_rect(Mbr *m, float minx, float miny, float maxx, float maxy);

/* ---------------- 度量 ---------------- */

/* 面积 = (maxx-minx) * (maxy-miny)；退化矩形为 0。 */
float mbr_area(const Mbr *m);

/* 周长的一半（Guttman 原始 R-tree 用它做分裂拣选的第二个判据）。 */
float mbr_margin(const Mbr *m);

/* ---------------- 关系（闭区间语义） ---------------- */

/* 相交（含"共边"、"一者在另一者内"、"两个相同的点"）。 */
int mbr_overlap(const Mbr *a, const Mbr *b);

/* outer 包含 inner（边界可以重合）。 */
int mbr_contains(const Mbr *outer, const Mbr *inner);

/* 矩形是否包含这个点（边界上的点算包含）。 */
int mbr_contains_point(const Mbr *m, float x, float y);

/* ---------------- 合并与扩张 ---------------- */

/* 并集（结果写进 out；out 可以与 a 或 b 同址）。 */
void mbr_union(const Mbr *a, const Mbr *b, Mbr *out);

/* 把一个点并进矩形（原地）。 */
void mbr_expand_point(Mbr *m, float x, float y);

/*
 * 扩张面积 = area(union(m, other)) - area(m)。
 *
 * ChooseSubtree 用它挑"装下这个孩子后膨胀最小"的那个节点
 * （Guttman 1984 §3.1）。单独暴露成函数，是为了让判据**可测**：
 * 步骤 2 的反向验证（注入"总是选第一个孩子"）就是靠它打红的。
 */
float mbr_enlarge_area(const Mbr *m, const Mbr *other);

/* ---------------- 距离 ---------------- */

/* 点到矩形的**平方**最近距离：点在矩形内（或边界上）为 0。 */
float mbr_mindist2(const Mbr *m, float x, float y);

/* 点到矩形的最近距离（= sqrt(mbr_mindist2)）。 */
float mbr_mindist(const Mbr *m, float x, float y);

/*
 * 两点之间的**平方**距离。
 *
 * 【为什么要单独给一个函数】
 *
 * KNN 的"真实距离"必须与树里剪枝用的 mindist 用**同一套算术**，
 * 否则"提前终止"的判据会有末位差异，偶尔漏掉一个真正更近的点。
 * 叶子条目的距离一律走这里，暴力基准也一律走这里。
 */
float geo_dist2(float x1, float y1, float x2, float y2);

#endif /* DB_MODALITIES_GEO_MBR_H */
