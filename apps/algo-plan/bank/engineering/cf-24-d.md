---
id: cf-24-d
source: codeforces
source_id: 24-D
title: 坏掉的机器人
difficulty: hard
topics: [动态规划, 数学, 概率]
tracks: [engineering]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/24/D
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 24**　24-D　（官方难度 rating 2400）

原题名：Broken robot

**题意（中文）**

N×M 棋盘，机器人在第 i 行第 j 列。每一步等概率地选择：原地不动 / 左移 / 右移 / 下移（超出边界的方向不算）。求走到第 N 行的**期望步数**。

**原题面（英文）**

You received as a gift a very clever robot walking on a rectangular board. Unfortunately, you understood that it is broken and behaves rather strangely (randomly). The board consists of N rows and M columns of cells. The robot is initially at some cell on the i-th row and the j-th column. Then at every step the robot could go to some another cell. The aim is to go to the bottommost (N-th) row. The robot can stay at it's current cell, move to the left, move to the right, or move to the cell below the current. If the robot is in the leftmost column it cannot move to the left, and if it is in the rightmost column it cannot move to the right. At every step all possible moves are equally probable. Return the expected number of step to reach the bottommost row.

**输入**

On the first line you will be given two space separated integers N and M (1 ≤ N, M ≤ 1000). On the second line you will be given another two space separated integers i and j (1 ≤ i ≤ N, 1 ≤ j ≤ M) — the number of the initial row and the number of the initial column. Note that, (1, 1) is the upper left corner of the board and (N, M) is the bottom right corner.

**输出**

Output the expected number of steps on a line of itself with at least 4 digits after the decimal point.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 10 10<br>10 4 | 0.0000000000 |

## 参考解法


## 题目信息

- 来源：Codeforces 24-D
- 比赛：Codeforces Beta Round 24
- 官方难度：rating 2400
- 官方标签：dp、math、probabilities
- 原题链接：https://codeforces.com/problemset/problem/24/D

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
