---
id: cf-24-c
source: codeforces
source_id: 24-C
title: 点列
difficulty: hard
topics: [数学, 模拟]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/24/C
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 24**　24-C　（官方难度 rating 1800）

原题名：Sequence of points

**题意（中文）**

给定奇数个点 A_0…A_{n-1} 和起点 M_0，定义 M_i 为 M_{i-1} 关于 A_{(i-1) mod n} 的**中心对称点**。求 M_j 的坐标（j 可达 10^18，要找规律）。

**原题面（英文）**

You are given the following points with integer coordinates on the plane: M0, A0, A1, ..., An - 1, where n is odd number. Now we define the following infinite sequence of points Mi: Mi is symmetric to Mi - 1 according  (for every natural number i). Here point B is symmetric to A according M, if M is the center of the line segment AB. Given index j find the point Mj.

**输入**

On the first line you will be given an integer n (1 ≤ n ≤ 105), which will be odd, and j (1 ≤ j ≤ 1018), where j is the index of the desired point. The next line contains two space separated integers, the coordinates of M0. After that n lines follow, where the i-th line contain the space separated integer coordinates of the point Ai - 1. The absolute values of all input coordinates will not be greater then 1000.

**输出**

On a single line output the coordinates of Mj, space separated.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 3 4<br>0 0<br>1 1<br>2 3<br>-5 3 | 14 0 |

## 参考解法


## 题目信息

- 来源：Codeforces 24-C
- 比赛：Codeforces Beta Round 24
- 官方难度：rating 1800
- 官方标签：geometry、implementation、math
- 原题链接：https://codeforces.com/problemset/problem/24/C

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
