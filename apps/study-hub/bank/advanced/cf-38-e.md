---
id: cf-38-e
source: codeforces
source_id: 38-E
title: 一路滚下去
difficulty: hard
topics: [动态规划, 排序]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/38/E
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**School Personal Contest #1 (Winter Computer School 2010/11) - Codeforces Beta Round 38 (ACM-ICPC Rules)**　38-E　（官方难度 rating 1800）

原题名：Let's Go Rolling!

**题意（中文）**

数轴上 n 颗珠子，第 i 颗钉住要花 c_i（可以是负数）。没钉的珠子向左滚到左边最近的钉子处停下；若左边没有钉子就滚向无穷远（罚金无穷）。总花费 = 钉子成本之和 + 所有珠子的滚动距离之和，求最小值。

**原题面（英文）**

On a number axis directed from the left rightwards, n marbles with coordinates x1, x2, ..., xn are situated. Let's assume that the sizes of the marbles are infinitely small, that is in this task each of them is assumed to be a material point. You can stick pins in some of them and the cost of sticking in the marble number i is equal to ci, number ci may be negative. After you choose and stick the pins you need, the marbles will start to roll left according to the rule: if a marble has a pin stuck in it, then the marble doesn't move, otherwise the marble rolls all the way up to the next marble which has a pin stuck in it and stops moving there. If there is no pinned marble on the left to the given unpinned one, it is concluded that the marble rolls to the left to infinity and you will pay an infinitely large fine for it. If no marble rolled infinitely to the left, then the fine will consist of two summands: 

  the sum of the costs of stuck pins; 

 the sum of the lengths of the paths of each of the marbles, that is the sum of absolute values of differences between their initial and final positions. 

Your task is to choose and pin some marbles in the way that will make the fine for you to pay as little as possible.

**输入**

The first input line contains an integer n (1 ≤ n ≤ 3000) which is the number of marbles. The next n lines contain the descriptions of the marbles in pairs of integers xi, ci ( - 109 ≤ xi, ci ≤ 109). The numbers are space-separated. Each description is given on a separate line. No two marbles have identical initial positions.

**输出**

Output the single number — the least fine you will have to pay.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 3<br>2 3<br>3 4<br>1 2 | 5 |

## 参考解法


## 题目信息

- 来源：Codeforces 38-E
- 比赛：School Personal Contest #1 (Winter Computer School 2010/11) - Codeforces Beta Round 38 (ACM-ICPC Rules)
- 官方难度：rating 1800
- 官方标签：dp、sortings
- 原题链接：https://codeforces.com/problemset/problem/38/E

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
