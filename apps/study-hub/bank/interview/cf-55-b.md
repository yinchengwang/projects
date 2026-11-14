---
id: cf-55-b
source: codeforces
source_id: 55-B
title: 最小的数
difficulty: medium
topics: [暴力枚举]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/55/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 51**　55-B　（官方难度 rating 1600）

原题名：Smallest number

**题意（中文）**

黑板上有四个非负整数。每分钟任选两个数，把它们换成和或积；共三分钟，按给定的三个运算符顺序做。求最终能得到的**最小值**。

**原题面（英文）**

Recently, Vladimir got bad mark in algebra again. To avoid such unpleasant events in future he decided to train his arithmetic skills. He wrote four integer numbers a, b, c, d on the blackboard. During each of the next three minutes he took two numbers from the blackboard (not necessarily adjacent) and replaced them with their sum or their product. In the end he got one number. Unfortunately, due to the awful memory he forgot that number, but he remembers four original numbers, sequence of the operations and his surprise because of the very small result. Help Vladimir remember the forgotten number: find the smallest number that can be obtained from the original numbers by the given sequence of operations.

**输入**

First line contains four integers separated by space: 0 ≤ a, b, c, d ≤ 1000 — the original numbers. Second line contains three signs ('+' or '*' each) separated by space — the sequence of the operations in the order of performing. ('+' stands for addition, '*' — multiplication)

**输出**

Output one integer number — the minimal result which can be obtained.

Please, do not use %lld specificator to read or write 64-bit integers in C++. It is preffered to use cin (also you may use %I64d).

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 1 1 1 1<br>+ + * | 3 |

## 参考解法


## 题目信息

- 来源：Codeforces 55-B
- 比赛：Codeforces Beta Round 51
- 官方难度：rating 1600
- 官方标签：brute force
- 原题链接：https://codeforces.com/problemset/problem/55/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
