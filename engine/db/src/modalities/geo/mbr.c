#include "modalities/geo/mbr.h"

#include <math.h>

/*
 * MBR 纯函数实现（v1.4 步骤 1 · A1）。
 *
 * 【为什么要单独成文件】
 *
 * 这一层没有页、没有锁、没有分配 —— 它是"几何数学"本身。
 * 把它独立出来，好处有三：
 *   1. R-tree（步骤 2/3）与暴力基准（步骤 1）**共用同一份算术** ——
 *      这是"结果全等"验收能成立的前提；
 *   2. A1 验收可以脱离存储层单独跑（纯函数对照手算）；
 *   3. 以后要加 Z-order 对照实现（步骤 4）时，
 *      它也能引用同一套 mindist 语义。
 */

/* ---------------- 构造 ---------------- */

void mbr_set_point(Mbr *m, float x, float y)
{
    m->minx = x;
    m->miny = y;
    m->maxx = x;
    m->maxy = y;
}

void mbr_set_rect(Mbr *m, float minx, float miny, float maxx, float maxy)
{
    /* 归一化：传反了就交换，而不是产生一个面积为负的矩形 */
    if (minx > maxx) {
        float t = minx;
        minx = maxx;
        maxx = t;
    }
    if (miny > maxy) {
        float t = miny;
        miny = maxy;
        maxy = t;
    }
    m->minx = minx;
    m->miny = miny;
    m->maxx = maxx;
    m->maxy = maxy;
}

/* ---------------- 度量 ---------------- */

float mbr_area(const Mbr *m)
{
    return (m->maxx - m->minx) * (m->maxy - m->miny);
}

float mbr_margin(const Mbr *m)
{
    /* 周长的一半 = (宽 + 高) */
    return (m->maxx - m->minx) + (m->maxy - m->miny);
}

/* ---------------- 关系（闭区间语义） ---------------- */

int mbr_overlap(const Mbr *a, const Mbr *b)
{
    /*
     * 闭区间：共边也算相交。
     *
     * 写成 "<=" 而不是 "<"：两个矩形只是边贴边时，
     * 剪枝必须**不能**剪掉 —— 落在公共边上的点属于任何一个窗口。
     */
    if (a->maxx < b->minx || b->maxx < a->minx)
        return 0;
    if (a->maxy < b->miny || b->maxy < a->miny)
        return 0;
    return 1;
}

int mbr_contains(const Mbr *outer, const Mbr *inner)
{
    if (outer->minx > inner->minx || outer->maxx < inner->maxx)
        return 0;
    if (outer->miny > inner->miny || outer->maxy < inner->maxy)
        return 0;
    return 1;
}

int mbr_contains_point(const Mbr *m, float x, float y)
{
    if (x < m->minx || x > m->maxx)
        return 0;
    if (y < m->miny || y > m->maxy)
        return 0;
    return 1;
}

/* ---------------- 合并与扩张 ---------------- */

void mbr_union(const Mbr *a, const Mbr *b, Mbr *out)
{
    float minx = a->minx < b->minx ? a->minx : b->minx;
    float miny = a->miny < b->miny ? a->miny : b->miny;
    float maxx = a->maxx > b->maxx ? a->maxx : b->maxx;
    float maxy = a->maxy > b->maxy ? a->maxy : b->maxy;

    out->minx = minx;
    out->miny = miny;
    out->maxx = maxx;
    out->maxy = maxy;
}

void mbr_expand_point(Mbr *m, float x, float y)
{
    if (x < m->minx)
        m->minx = x;
    if (y < m->miny)
        m->miny = y;
    if (x > m->maxx)
        m->maxx = x;
    if (y > m->maxy)
        m->maxy = y;
}

float mbr_enlarge_area(const Mbr *m, const Mbr *other)
{
    Mbr u;

    mbr_union(m, other, &u);
    return mbr_area(&u) - mbr_area(m);
}

/* ---------------- 距离 ---------------- */

float mbr_mindist2(const Mbr *m, float x, float y)
{
    float dx = 0.0f;
    float dy = 0.0f;

    /* 逐轴"夹取"：落在区间内则为 0，落在外面就是到边界的距离 */
    if (x < m->minx)
        dx = m->minx - x;
    else if (x > m->maxx)
        dx = x - m->maxx;

    if (y < m->miny)
        dy = m->miny - y;
    else if (y > m->maxy)
        dy = y - m->maxy;

    return dx * dx + dy * dy;
}

float mbr_mindist(const Mbr *m, float x, float y)
{
    return (float)sqrt((double)mbr_mindist2(m, x, y));
}

float geo_dist2(float x1, float y1, float x2, float y2)
{
    float dx = x1 - x2;
    float dy = y1 - y2;

    return dx * dx + dy * dy;
}
