/*
 * exec_cost.c — 规则 CBO 的代价公式（v1.1 步骤 4）
 *
 * 设计说明见 exec_cost.h。这里只有三个纯函数：
 *   选择率 / 树高估算 / 两个代价 + 决策
 *
 * 【为什么是纯函数】
 * 代价模型最容易变成"到处 if"的一团泥。把它写成纯函数的好处：
 *   1. 可以单独测（不用建表、不用扫描）
 *   2. EXPLAIN 和真正的执行路径**调用同一个函数** ——
 *      不会出现"EXPLAIN 说走索引、实际走了全扫"
 */

#include "query/exec_cost.h"

#include <stdio.h>

double exec_cost_sel(int op, i64 nrows)
{
    switch (op) {
    case '=':
        /* 等值：假设均匀分布，每行一个不同的值 */
        return 1.0 / (double)(nrows > 0 ? nrows : 1);

    case '<':
    case '>':
    case 'L':
    case 'G':
        /*
         * 范围：1/3。
         *
         * 【为什么是 1/3】
         * 没有直方图时，这是教科书（和早期 PG / System R）的默认值。
         * 它的来历：假设数据在值域上均匀分布，而谓词通常落在值域的
         * 中间三分之一附近。粗糙，但比"一律 1/2"更接近现实。
         *
         * 有了直方图之后这一条会被替换掉 —— 这正是"公式可替换"的意思。
         */
        return 1.0 / 3.0;

    default:
        /* 不知道的谓词：按"全中"算，倾向全表扫（保守） */
        return 1.0;
    }
}

double exec_cost_tree_height(i64 nrows)
{
    if (nrows <= 0)
        return 1.0;

    /*
     * 每页约 170 条（4KB 数据区 / 24 字节复合键），每层扇出同样按 170 估。
     * 高度 = 从叶子往上还要几层才能容纳 nrows 条。
     */
    double cap = 170.0;
    double h = 1.0;

    while (cap < (double)nrows && h < 12.0) {
        cap *= 170.0;
        h += 1.0;
    }
    return h;
}

void exec_cost_decide(i64 nrows, double sel, CostPlan *out)
{
    if (!out)
        return;

    if (nrows < 0)
        nrows = 0;
    if (sel < 0.0)
        sel = 0.0;
    if (sel > 1.0)
        sel = 1.0;

    out->nrows = nrows;
    out->sel = sel;
    out->cost_seq = (double)nrows;                       /* 每行一次元组访问 */
    out->cost_idx = exec_cost_tree_height(nrows)
                  + sel * (double)nrows * CBO_HEAP_FETCH;

    /*
     * 【小表恒全扫】
     *
     * 64 行以下的表，索引扫描的固定开销（树高次页访问）已经超过
     * 把 64 行顺序读一遍的代价。而且小表常常整张待在一个页里 ——
     * 全扫只碰一次缓存，走索引反而要多跳几层。
     *
     * 这条是**规则**，不是算出来的：代价公式里 nrows 小的时候
     * cost_idx 的固定项会占主导，但公式对"页内局部性"一无所知，
     * 所以用一条显式规则兜住。
     */
    if (nrows < CBO_SMALL_TABLE) {
        out->use_index = false;
        out->why = "小表（nrows_est < 64）→ 恒全扫";
        return;
    }

    if (out->cost_idx < out->cost_seq) {
        out->use_index = true;
        out->why = "cost_idx < cost_seq";
    } else {
        out->use_index = false;
        out->why = "cost_idx >= cost_seq";
    }
}
