---
id: cf-60-b
source: codeforces
source_id: 60-B
title: 串行时间
difficulty: medium
topics: [深度优先搜索, 并查集]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/60/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 56**　60-B　（官方难度 rating 1400）

原题名：Serial Time!

**题意（中文）**

盘子是 k×n×m 的长方体（层×行×列），每格是空格 `.` 或障碍 `#`。水龙头在最上层某格注水，水向六个方向流动。问盘子被注满需要多少分钟（即装满所有能流到的空格的时刻）。

**原题面（英文）**

The Cereal Guy's friend Serial Guy likes to watch soap operas. An episode is about to start, and he hasn't washed his plate yet. But he decided to at least put in under the tap to be filled with water. The plate can be represented by a parallelepiped k × n × m, that is, it has k layers (the first layer is the upper one), each of which is a rectangle n × m with empty squares ('.') and obstacles ('#'). The water can only be present in the empty squares. The tap is positioned above the square (x, y) of the first layer, it is guaranteed that this square is empty. Every minute a cubical unit of water falls into the plate. Find out in how many minutes the Serial Guy should unglue himself from the soap opera and turn the water off for it not to overfill the plate. That is, you should find the moment of time when the plate is absolutely full and is going to be overfilled in the next moment.

Note: the water fills all the area within reach (see sample 4). Water flows in each of the 6 directions, through faces of 1 × 1 × 1 cubes.

**输入**

The first line contains three numbers k, n, m (1 ≤ k, n, m ≤ 10) which are the sizes of the plate. Then follow k rectangles consisting of n lines each containing m characters '.' or '#', which represents the "layers" of the plate in the order from the top to the bottom. The rectangles are separated by empty lines (see the samples). The last line contains x and y (1 ≤ x ≤ n, 1 ≤ y ≤ m) which are the tap's coordinates. x is the number of the line and y is the number of the column. Lines of each layer are numbered from left to right by the integers from 1 to n, columns of each layer are numbered from top to bottom by the integers from 1 to m.

**输出**

The answer should contain a single number, showing in how many minutes the plate will be filled.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 1 1 1<br><br>.<br><br>1 1 | 1 |

## 参考解法


## 题目信息

- 来源：Codeforces 60-B
- 比赛：Codeforces Beta Round 56
- 官方难度：rating 1400
- 官方标签：dfs and similar、dsu
- 原题链接：https://codeforces.com/problemset/problem/60/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
