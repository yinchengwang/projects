---
id: cf-15-a
source: codeforces
source_id: 15-A
title: 新村安置
difficulty: easy
topics: [排序, 模拟]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/15/A
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 15**　15-A　（官方难度 rating 1200）

原题名：Cottage Village

**题意（中文）**

x 轴上已经建了 n 栋边长为 a_i、中心在 x_i 的正方形房子（互不重叠，可以相接）。现在要建一栋边长为 t 的新房子（中心在 x 轴上、边平行于坐标轴），要求它**至少贴住一栋已有房子**且不与任何房子重叠。问新房子有多少种可放位置。

**原题面（英文）**

A new cottage village called «Flatville» is being built in Flatland. By now they have already built in «Flatville» n square houses with the centres on the Оx-axis. The houses' sides are parallel to the coordinate axes. It's known that no two houses overlap, but they can touch each other.

The architect bureau, where Peter works, was commissioned to build a new house in «Flatville». The customer wants his future house to be on the Оx-axis, to be square in shape, have a side t, and touch at least one of the already built houses. For sure, its sides should be parallel to the coordinate axes, its centre should be on the Ox-axis and it shouldn't overlap any of the houses in the village.

Peter was given a list of all the houses in «Flatville». Would you help him find the amount of possible positions of the new house?

**输入**

The first line of the input data contains numbers n and t (1 ≤ n, t ≤ 1000). Then there follow n lines, each of them contains two space-separated integer numbers: xi ai, where xi — x-coordinate of the centre of the i-th house, and ai — length of its side ( - 1000 ≤ xi ≤ 1000, 1 ≤ ai ≤ 1000).

**输出**

Output the amount of possible positions of the new house.

**限制**：每组测试 2 seconds，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 2 2<br>0 4<br>6 2 | 4 |

## 参考解法


## 题目信息

- 来源：Codeforces 15-A
- 比赛：Codeforces Beta Round 15
- 官方难度：rating 1200
- 官方标签：implementation、sortings
- 原题链接：https://codeforces.com/problemset/problem/15/A

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
