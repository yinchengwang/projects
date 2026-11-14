---
id: cf-79-b
source: codeforces
source_id: 79-B
title: 彩色田野
difficulty: medium
topics: [排序, 模拟]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/79/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 71**　79-B　（官方难度 rating 1400）

原题名：Colorful Field

**题意（中文）**

n×m 的田地里有 k 块荒地，其余格子按**行优先顺序**（跳过荒地）依次种胡萝卜、猕猴桃、葡萄三种作物循环。问若干查询点上是哪种作物（荒地输出 Waste）。

**原题面（英文）**

Fox Ciel saw a large field while she was on a bus. The field was a n × m rectangle divided into 1 × 1 cells. Some cells were wasteland, and other each cell contained crop plants: either carrots or kiwis or grapes. 

After seeing the field carefully, Ciel found that the crop plants of each cell were planted in following procedure:

  Assume that the rows are numbered 1 to n from top to bottom and the columns are numbered 1 to m from left to right, and a cell in row i and column j is represented as (i, j). 

 First, each field is either cultivated or waste. Crop plants will be planted in the cultivated cells in the order of (1, 1) → ... → (1, m) → (2, 1) → ... → (2, m) → ... → (n, 1) → ... → (n, m). Waste cells will be ignored. 

 Crop plants (either carrots or kiwis or grapes) will be planted in each cell one after another cyclically. Carrots will be planted in the first cell, then kiwis in the second one, grapes in the third one, carrots in the forth one, kiwis in the fifth one, and so on. 

The following figure will show you the example of this procedure. Here, a white square represents a cultivated cell, and a black square represents a waste cell.

  Now she is wondering how to determine the crop plants in some certain cells.

**输入**

In the first line there are four positive integers n, m, k, t (1 ≤ n ≤ 4·104, 1 ≤ m ≤ 4·104, 1 ≤ k ≤ 103, 1 ≤ t ≤ 103), each of which represents the height of the field, the width of the field, the number of waste cells and the number of queries that ask the kind of crop plants in a certain cell.

Following each k lines contains two integers a, b (1 ≤ a ≤ n, 1 ≤ b ≤ m), which denotes a cell (a, b) is waste. It is guaranteed that the same cell will not appear twice in this section.

Following each t lines contains two integers i, j (1 ≤ i ≤ n, 1 ≤ j ≤ m), which is a query that asks you the kind of crop plants of a cell (i, j).

**输出**

For each query, if the cell is waste, print Waste. Otherwise, print the name of crop plants in the cell: either Carrots or Kiwis or Grapes.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 4 5 5 6<br>4 3<br>1 3<br>3 3<br>2 5<br>3 2<br>1 3<br>1 4<br>2 3<br>2 4<br>1 1<br>1 1 | Waste<br>Grapes<br>Carrots<br>Kiwis<br>Carrots<br>Carrots |

## 参考解法


## 题目信息

- 来源：Codeforces 79-B
- 比赛：Codeforces Beta Round 71
- 官方难度：rating 1400
- 官方标签：implementation、sortings
- 原题链接：https://codeforces.com/problemset/problem/79/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
