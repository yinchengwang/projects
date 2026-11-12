---
id: cf-1-b
source: codeforces
source_id: 1-B
title: 电子表格
difficulty: medium
topics: [数学, 模拟]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/1/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 1**　1-B　（官方难度 rating 1600）

原题名：Spreadsheet

**题意（中文）**

电子表格用列名 A…Z、AA…AZ、BA… 编号（第 26 列是 Z，第 27 列是 AA）。另一种写法是 RXCY（R 行 C 列）。给定 n 个单元格坐标，两种写法**互相转换**输出。

**原题面（英文）**

In the popular spreadsheets systems (for example, in Excel) the following numeration of columns is used. The first column has number A, the second — number B, etc. till column 26 that is marked by Z. Then there are two-letter numbers: column 27 has number AA, 28 — AB, column 52 is marked by AZ. After ZZ there follow three-letter numbers, etc.

The rows are marked by integer numbers starting with 1. The cell name is the concatenation of the column and the row numbers. For example, BC23 is the name for the cell that is in column 55, row 23. 

Sometimes another numeration system is used: RXCY, where X and Y are integer numbers, showing the column and the row numbers respectfully. For instance, R23C55 is the cell from the previous example.

Your task is to write a program that reads the given sequence of cell coordinates and produce each item written according to the rules of another numeration system.

**输入**

The first line of the input contains integer number n (1 ≤ n ≤ 105), the number of coordinates in the test. Then there follow n lines, each of them contains coordinates. All the coordinates are correct, there are no cells with the column and/or the row numbers larger than 106 .

**输出**

Write n lines, each line should contain a cell coordinates in the other numeration system.

**限制**：每组测试 10 seconds，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 2<br>R23C55<br>BC23 | BC23<br>R23C55 |

## 参考解法


## 题目信息

- 来源：Codeforces 1-B
- 比赛：Codeforces Beta Round 1
- 官方难度：rating 1600
- 官方标签：implementation、math
- 原题链接：https://codeforces.com/problemset/problem/1/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
