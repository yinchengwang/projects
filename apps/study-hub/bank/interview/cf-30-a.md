---
id: cf-30-a
source: codeforces
source_id: 30-A
title: 会计
difficulty: medium
topics: [数学, 暴力枚举]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/30/A
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 30 (Codeforces format)**　30-A　（官方难度 rating 1400）

原题名：Accounting

**题意（中文）**

已知第 0 年总收入 A、第 n 年总收入 B（都可能是负数）。求整数 X 使 A·X^n = B。若不存在整数解（或解出来是小数），输出 `No solution`。

**原题面（英文）**

A long time ago in some far country lived king Copa. After the recent king's reform, he got so large powers that started to keep the books by himself.

The total income A of his kingdom during 0-th year is known, as well as the total income B during n-th year (these numbers can be negative — it means that there was a loss in the correspondent year). 

King wants to show financial stability. To do this, he needs to find common coefficient X — the coefficient of income growth during one year. This coefficient should satisfy the equation:

A·Xn = B.Surely, the king is not going to do this job by himself, and demands you to find such number X.

It is necessary to point out that the fractional numbers are not used in kingdom's economy. That's why all input numbers as well as coefficient X must be integers. The number X may be zero or negative.

**输入**

The input contains three integers A, B, n (|A|, |B| ≤ 1000, 1 ≤ n ≤ 10).

**输出**

Output the required integer coefficient X, or «No solution», if such a coefficient does not exist or it is fractional. If there are several possible solutions, output any of them.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 2 18 2 | 3 |

## 参考解法


## 题目信息

- 来源：Codeforces 30-A
- 比赛：Codeforces Beta Round 30 (Codeforces format)
- 官方难度：rating 1400
- 官方标签：brute force、math
- 原题链接：https://codeforces.com/problemset/problem/30/A

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
