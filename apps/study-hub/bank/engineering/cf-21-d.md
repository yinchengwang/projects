---
id: cf-21-d
source: codeforces
source_id: 21-D
title: 旅行图
difficulty: hard
topics: [图论, 位运算, 图匹配]
tracks: [engineering]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/21/D
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Alpha Round 21 (Codeforces format)**　21-D　（官方难度 rating 2400）

原题名：Traveling Graph

**题意（中文）**

给无向带权图（可能有重边和自环）。求一条**从 1 号点出发、经过每条边至少一次、并回到 1 号点**的最短回路长度；不存在输出 -1。

**原题面（英文）**

You are given undirected weighted graph. Find the length of the shortest cycle which starts from the vertex 1 and passes throught all the edges at least once. Graph may contain multiply edges between a pair of vertices and loops (edges from the vertex to itself).

**输入**

The first line of the input contains two integers n and m (1 ≤ n ≤ 15, 0 ≤ m ≤ 2000), n is the amount of vertices, and m is the amount of edges. Following m lines contain edges as a triples x, y, w (1 ≤ x, y ≤ n, 1 ≤ w ≤ 10000), x, y are edge endpoints, and w is the edge length.

**输出**

Output minimal cycle length or -1 if it doesn't exists.

**限制**：每组测试 0.5 second，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 3 3<br>1 2 1<br>2 3 1<br>3 1 1 | 3 |

## 参考解法


## 题目信息

- 来源：Codeforces 21-D
- 比赛：Codeforces Alpha Round 21 (Codeforces format)
- 官方难度：rating 2400
- 官方标签：bitmasks、graph matchings、graphs
- 原题链接：https://codeforces.com/problemset/problem/21/D

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
