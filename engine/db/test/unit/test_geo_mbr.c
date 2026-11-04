/*
 * test_geo_mbr.c —— MBR 数学的验收（v1.4 步骤 1 · A1）
 *
 * 【这一整个文件在证明什么】
 *
 * 空间查询的正确性最后要靠"与暴力结果全等"来保证，
 * 而暴力与 R-tree 共用同一套 MBR 算术（modalities/geo/mbr.c）。
 *
 * 所以 MBR 这一层必须**先**被钉死：
 *   - 相交/包含的**边界语义**（共边算不算相交？点落在边上算不算在窗内？）
 *   - mindist 在**退化矩形**（点、线）与**负坐标**下的取值
 *
 * 这两件事只要错一处，后面的"全等"就全成了"两边一起错、恰好相等"，
 * 验收就失去了意义。所以这一层用**手算值**对照，不与任何实现对照。
 *
 * 【为什么边界语义要单独钉】
 *
 * 剪枝用开区间、查询用闭区间，是空间索引最经典的漏点来源：
 * 窗口查询返回的点集少几个，**而且不报错**。
 * 所以这里逐条列出"共边 / 共角点 / 恰好在边界上"的期望值，
 * 让"闭区间"这个约定变成**可执行的断言**。
 */

#include <stdio.h>
#include <stdlib.h>
#include <math.h>

#include "common/types.h"
#include "modalities/geo/mbr.h"

static int failures = 0;

#define CHECK(cond) do { \
    if (!(cond)) { \
        printf("    FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        failures++; \
    } \
} while (0)

/* 浮点比较：只在 sqrt 这类不精确的地方用，其余一律用 == 精确比 */
#define CHECK_NEAR(a, b) do { \
    double va = (double)(a); \
    double vb = (double)(b); \
    if (!(fabs(va - vb) < 1e-4)) { \
        printf("    FAIL %s:%d: %s (%.9f) != %s (%.9f)\n", \
               __FILE__, __LINE__, #a, va, #b, vb); \
        failures++; \
    } \
} while (0)

/* ---------------- 1. 面积与半周长 ---------------- */

static void test_area(void)
{
    Mbr m;

    mbr_set_rect(&m, 0.0f, 0.0f, 2.0f, 3.0f);
    CHECK(mbr_area(&m) == 6.0f);

    /* 负坐标：面积与位置无关 */
    mbr_set_rect(&m, -3.0f, -2.0f, -1.0f, 0.0f);
    CHECK(mbr_area(&m) == 4.0f);

    /* 退化：点 → 面积 0 */
    mbr_set_point(&m, 1.0f, 1.0f);
    CHECK(mbr_area(&m) == 0.0f);

    /* 退化：线 → 面积 0 */
    mbr_set_rect(&m, 0.0f, 0.0f, 5.0f, 0.0f);
    CHECK(mbr_area(&m) == 0.0f);

    /* 半周长 = 宽 + 高 */
    mbr_set_rect(&m, 0.0f, 0.0f, 2.0f, 3.0f);
    CHECK(mbr_margin(&m) == 5.0f);
    mbr_set_point(&m, 1.0f, 1.0f);
    CHECK(mbr_margin(&m) == 0.0f);
}

/* ---------------- 2. 相交（闭区间语义） ---------------- */

static void test_overlap(void)
{
    Mbr a, b;

    /* 部分重叠 */
    mbr_set_rect(&a, 0.0f, 0.0f, 10.0f, 10.0f);
    mbr_set_rect(&b, 5.0f, 5.0f, 15.0f, 15.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 完全相同 */
    mbr_set_rect(&b, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 一者包含另一者 */
    mbr_set_rect(&b, 2.0f, 2.0f, 3.0f, 3.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 【共边】—— 闭区间：算相交。
     * 这一条是"边界语义"的核心：若这里判成不相交，
     * 窗口查询就会漏掉落在公共边上的点。 */
    mbr_set_rect(&b, 10.0f, 0.0f, 20.0f, 10.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 只差一点点 → 不相交 */
    mbr_set_rect(&b, 10.001f, 0.0f, 20.0f, 10.0f);
    CHECK(mbr_overlap(&a, &b) == 0);

    /* x 重叠但 y 分离 → 不相交 */
    mbr_set_rect(&b, 5.0f, 20.0f, 15.0f, 30.0f);
    CHECK(mbr_overlap(&a, &b) == 0);

    /* 退化：点落在矩形内 → 相交 */
    mbr_set_point(&b, 5.0f, 5.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 退化：点落在矩形角上 → 相交（闭区间） */
    mbr_set_point(&b, 0.0f, 0.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 退化：两个相同的点 → 相交 */
    mbr_set_point(&a, 1.0f, 1.0f);
    mbr_set_point(&b, 1.0f, 1.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    /* 退化：两个不同的点 → 不相交 */
    mbr_set_point(&b, 1.0f, 1.001f);
    CHECK(mbr_overlap(&a, &b) == 0);

    /* 负坐标 */
    mbr_set_rect(&a, -10.0f, -10.0f, 0.0f, 0.0f);
    mbr_set_rect(&b, -5.0f, -5.0f, 5.0f, 5.0f);
    CHECK(mbr_overlap(&a, &b) == 1);

    mbr_set_rect(&b, 1.0f, 1.0f, 5.0f, 5.0f);
    CHECK(mbr_overlap(&a, &b) == 0);
}

/* ---------------- 3. 包含 ---------------- */

static void test_contains(void)
{
    Mbr outer, inner;

    mbr_set_rect(&outer, 0.0f, 0.0f, 10.0f, 10.0f);

    mbr_set_rect(&inner, 2.0f, 2.0f, 3.0f, 3.0f);
    CHECK(mbr_contains(&outer, &inner) == 1);

    /* 相等也算包含 */
    mbr_set_rect(&inner, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(mbr_contains(&outer, &inner) == 1);

    /* 越界 → 不包含 */
    mbr_set_rect(&inner, 0.0f, 0.0f, 20.0f, 10.0f);
    CHECK(mbr_contains(&outer, &inner) == 0);

    /* 共角点 → 包含（闭区间） */
    mbr_set_point(&inner, 10.0f, 10.0f);
    CHECK(mbr_contains(&outer, &inner) == 1);

    /* 共右边 → 包含 */
    mbr_set_rect(&inner, 10.0f, 0.0f, 10.0f, 10.0f);
    CHECK(mbr_contains(&outer, &inner) == 1);

    /* 点的包含：边界上算在窗内 */
    mbr_set_rect(&outer, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK(mbr_contains_point(&outer, 5.0f, 5.0f) == 1);
    CHECK(mbr_contains_point(&outer, 0.0f, 0.0f) == 1);
    CHECK(mbr_contains_point(&outer, 10.0f, 10.0f) == 1);
    CHECK(mbr_contains_point(&outer, 0.0f, 10.0f) == 1);
    CHECK(mbr_contains_point(&outer, -0.001f, 5.0f) == 0);
    CHECK(mbr_contains_point(&outer, 10.001f, 5.0f) == 0);
    CHECK(mbr_contains_point(&outer, 5.0f, -0.001f) == 0);
    CHECK(mbr_contains_point(&outer, 5.0f, 10.001f) == 0);
}

/* ---------------- 4. mindist ---------------- */

static void test_mindist(void)
{
    Mbr m;

    mbr_set_rect(&m, 0.0f, 0.0f, 10.0f, 10.0f);

    /* 矩形内 → 0 */
    CHECK(mbr_mindist2(&m, 5.0f, 5.0f) == 0.0f);

    /* 角上 → 0 */
    CHECK(mbr_mindist2(&m, 0.0f, 0.0f) == 0.0f);

    /* 边上 → 0 */
    CHECK(mbr_mindist2(&m, 0.0f, 5.0f) == 0.0f);

    /* 右边外 5 → 25 */
    CHECK(mbr_mindist2(&m, 15.0f, 5.0f) == 25.0f);

    /* 右上外 (5,10) → 25 + 100 = 125 */
    CHECK(mbr_mindist2(&m, 15.0f, 20.0f) == 125.0f);

    /* 左边外 3 → 9 */
    CHECK(mbr_mindist2(&m, -3.0f, 0.0f) == 9.0f);

    /* 负坐标矩形 */
    mbr_set_rect(&m, -10.0f, -10.0f, 0.0f, 0.0f);
    CHECK(mbr_mindist2(&m, -20.0f, -20.0f) == 200.0f);

    /* 退化：点矩形的 mindist 就是欧氏距离平方 */
    mbr_set_point(&m, 2.0f, 3.0f);
    CHECK(mbr_mindist2(&m, 0.0f, 0.0f) == 13.0f);

    /* 真实距离（含 sqrt，用容差比） */
    mbr_set_rect(&m, 0.0f, 0.0f, 10.0f, 10.0f);
    CHECK_NEAR(mbr_mindist(&m, 15.0f, 5.0f), 5.0);
    CHECK_NEAR(mbr_mindist(&m, 15.0f, 20.0f), sqrt(125.0));
    CHECK_NEAR(mbr_mindist(&m, 5.0f, 5.0f), 0.0);

    /* 两点距离平方 */
    CHECK(geo_dist2(0.0f, 0.0f, 3.0f, 4.0f) == 25.0f);
    CHECK(geo_dist2(1.0f, 1.0f, 1.0f, 1.0f) == 0.0f);
}

/* ---------------- 5. 并集 / 扩张 / 扩张面积 ---------------- */

static void test_union(void)
{
    Mbr a, b, u;

    mbr_set_rect(&a, 0.0f, 0.0f, 2.0f, 3.0f);
    mbr_set_rect(&b, 1.0f, 1.0f, 5.0f, 2.0f);
    mbr_union(&a, &b, &u);
    CHECK(u.minx == 0.0f && u.miny == 0.0f && u.maxx == 5.0f && u.maxy == 3.0f);

    /* 含负坐标 */
    mbr_set_rect(&a, -5.0f, -5.0f, 0.0f, 0.0f);
    mbr_set_rect(&b, -1.0f, -1.0f, 2.0f, 2.0f);
    mbr_union(&a, &b, &u);
    CHECK(u.minx == -5.0f && u.miny == -5.0f && u.maxx == 2.0f && u.maxy == 2.0f);

    /* out 与输入同址也要正确（就地合并） */
    mbr_set_rect(&a, 0.0f, 0.0f, 2.0f, 3.0f);
    mbr_set_rect(&b, 1.0f, 1.0f, 5.0f, 2.0f);
    mbr_union(&a, &b, &a);
    CHECK(a.minx == 0.0f && a.miny == 0.0f && a.maxx == 5.0f && a.maxy == 3.0f);
}

static void test_expand(void)
{
    Mbr m;

    mbr_set_rect(&m, 0.0f, 0.0f, 10.0f, 10.0f);
    mbr_expand_point(&m, -5.0f, 15.0f);
    CHECK(m.minx == -5.0f && m.miny == 0.0f && m.maxx == 10.0f && m.maxy == 15.0f);

    /* 落在内部的点不改变矩形 */
    mbr_expand_point(&m, 1.0f, 1.0f);
    CHECK(m.minx == -5.0f && m.miny == 0.0f && m.maxx == 10.0f && m.maxy == 15.0f);
}

static void test_enlarge_area(void)
{
    Mbr m, o;

    /* 装一个已经在里面的点 → 面积不变，扩张 0 */
    mbr_set_rect(&m, 0.0f, 0.0f, 10.0f, 10.0f);
    mbr_set_point(&o, 10.0f, 10.0f);
    CHECK(mbr_enlarge_area(&m, &o) == 0.0f);

    /* 右上角外扩 → 并集 20×20 = 400，原来 100，扩张 300 */
    mbr_set_rect(&o, 10.0f, 10.0f, 20.0f, 20.0f);
    CHECK(mbr_enlarge_area(&m, &o) == 300.0f);

    /* 左下角外扩 → 同样 300 */
    mbr_set_rect(&o, -10.0f, -10.0f, 0.0f, 0.0f);
    CHECK(mbr_enlarge_area(&m, &o) == 300.0f);
}

/* ---------------- 6. 归一化 ---------------- */

static void test_normalize(void)
{
    Mbr m;

    /* min > max 时自动交换，不产生"面积为负"的矩形 */
    mbr_set_rect(&m, 3.0f, 4.0f, 1.0f, 2.0f);
    CHECK(m.minx == 1.0f && m.miny == 2.0f && m.maxx == 3.0f && m.maxy == 4.0f);
    CHECK(mbr_area(&m) == 4.0f);
}

int main(void)
{
    printf("test_geo_mbr ...\n");

    test_area();
    test_overlap();
    test_contains();
    test_mindist();
    test_union();
    test_expand();
    test_enlarge_area();
    test_normalize();

    if (failures == 0) {
        printf("  [PASS]\n");
        return 0;
    }
    printf("  [FAIL] %d 处断言失败\n", failures);
    return 1;
}
