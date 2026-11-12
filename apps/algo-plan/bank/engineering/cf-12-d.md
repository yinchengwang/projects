---
id: cf-12-d
source: codeforces
source_id: 12-D
title: 舞会
difficulty: hard
topics: [排序, 数据结构]
tracks: [engineering]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/12/D
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 12 (Div 2 Only)**　12-D　（官方难度 rating 2400）

原题名：Ball

**题意（中文）**

n 位女士，各有美貌 B_i、智慧 I_i、财富 R_i。若存在某位 j 满足 B_j > B_i、I_j > I_i、R_j > R_i，则 i 会跳楼。求这样的人数（**三维偏序**计数）。

**原题面（英文）**

N ladies attend the ball in the King's palace. Every lady can be described with three values: beauty, intellect and richness. King's Master of Ceremonies knows that ladies are very special creatures. If some lady understands that there is other lady at the ball which is more beautiful, smarter and more rich, she can jump out of the window. He knows values of all ladies and wants to find out how many probable self-murderers will be on the ball. Lets denote beauty of the i-th lady by Bi, her intellect by Ii and her richness by Ri. Then i-th lady is a probable self-murderer if there is some j-th lady that Bi < Bj, Ii < Ij, Ri < Rj. Find the number of probable self-murderers.

**输入**

The first line contains one integer N (1 ≤ N ≤ 500000). The second line contains N integer numbers Bi, separated by single spaces. The third and the fourth lines contain sequences Ii and Ri in the same format. It is guaranteed that 0 ≤ Bi, Ii, Ri ≤ 109.

**输出**

Output the answer to the problem.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 3<br>1 4 2<br>4 3 2<br>2 5 3 | 1 |

## 参考解法


## 题目信息

- 来源：Codeforces 12-D
- 比赛：Codeforces Beta Round 12 (Div 2 Only)
- 官方难度：rating 2400
- 官方标签：data structures、sortings
- 原题链接：https://codeforces.com/problemset/problem/12/D

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
