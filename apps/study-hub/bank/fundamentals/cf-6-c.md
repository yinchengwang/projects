---
id: cf-6-c
source: codeforces
source_id: 6-C
title: 爱丽丝、鲍勃与巧克力
difficulty: easy
topics: [贪心, 双指针]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/6/C
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 6 (Div. 2 Only)**　6-C　（官方难度 rating 1200）

原题名：Alice, Bob and Chocolate

**题意（中文）**

n 块巧克力排成一行，各有各自的食用耗时 t_i。爱丽丝从左往右吃，鲍勃从右往左吃，两人速度相同、不能同时吃同一块（同时开始时鲍勃让给爱丽丝）。问两人最终各吃了几块。

**原题面（英文）**

Alice and Bob like games. And now they are ready to start a new game. They have placed n chocolate bars in a line. Alice starts to eat chocolate bars one by one from left to right, and Bob — from right to left. For each chocololate bar the time, needed for the player to consume it, is known (Alice and Bob eat them with equal speed). When the player consumes a chocolate bar, he immediately starts with another. It is not allowed to eat two chocolate bars at the same time, to leave the bar unfinished and to make pauses. If both players start to eat the same bar simultaneously, Bob leaves it to Alice as a true gentleman.

How many bars each of the players will consume?

**输入**

The first line contains one integer n (1 ≤ n ≤ 105) — the amount of bars on the table. The second line contains a sequence t1, t2, ..., tn (1 ≤ ti ≤ 1000), where ti is the time (in seconds) needed to consume the i-th bar (in the order from left to right).

**输出**

Print two numbers a and b, where a is the amount of bars consumed by Alice, and b is the amount of bars consumed by Bob.

**限制**：每组测试 2 seconds，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 5<br>2 9 8 2 7 | 2 3 |

## 参考解法


## 题目信息

- 来源：Codeforces 6-C
- 比赛：Codeforces Beta Round 6 (Div. 2 Only)
- 官方难度：rating 1200
- 官方标签：greedy、two pointers
- 原题链接：https://codeforces.com/problemset/problem/6/C

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
