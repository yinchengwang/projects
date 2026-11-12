---
id: cf-28-b
source: codeforces
source_id: 28-B
title: pSort
difficulty: medium
topics: [图论, 深度优先搜索, 并查集]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/28/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 28 (Codeforces format)**　28-B　（官方难度 rating 1600）

原题名：pSort

**题意（中文）**

数组第 i 个位置初始放着编号 i；第 i 个位置可以和距离恰好为 d_i 的位置交换两者的值（交换次数不限）。问给定的排列能否通过这些交换得到。

**原题面（英文）**

One day n cells of some array decided to play the following game. Initially each cell contains a number which is equal to it's ordinal number (starting from 1). Also each cell determined it's favourite number. On it's move i-th cell can exchange it's value with the value of some other j-th cell, if |i - j| = di, where di is a favourite number of i-th cell. Cells make moves in any order, the number of moves is unlimited.

The favourite number of each cell will be given to you. You will also be given a permutation of numbers from 1 to n. You are to determine whether the game could move to this state.

**输入**

The first line contains positive integer n (1 ≤ n ≤ 100) — the number of cells in the array. The second line contains n distinct integers from 1 to n — permutation. The last line contains n integers from 1 to n — favourite numbers of the cells.

**输出**

If the given state is reachable in the described game, output YES, otherwise NO.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 5<br>5 4 3 2 1<br>1 1 1 1 1 | YES |

## 参考解法


## 题目信息

- 来源：Codeforces 28-B
- 比赛：Codeforces Beta Round 28 (Codeforces format)
- 官方难度：rating 1600
- 官方标签：dfs and similar、dsu、graphs
- 原题链接：https://codeforces.com/problemset/problem/28/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
