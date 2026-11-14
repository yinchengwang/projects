---
id: cf-45-h
source: codeforces
source_id: 45-H
title: 道路问题
difficulty: hard
topics: [图论]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/45/H
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**School Team Contest 3 (Winter Computer School 2010/11)**　45-H　（官方难度 rating 2100）

原题名：Road Problem

**题意（中文）**

给一个连通的无向图。要**最少加几条边**，使得任意两点之间都存在两条**边不相交**的路径（即把图变成边双连通）。输出要新增的边；不需要改造输出 0，无解输出 -1。

**原题面（英文）**

The Berland capital (as you very well know) contains n junctions, some pairs of which are connected by two-way roads. Unfortunately, the number of traffic jams in the capital has increased dramatically, that's why it was decided to build several new roads. Every road should connect two junctions. 

The city administration noticed that in the cities of all the developed countries between any two roads one can drive along at least two paths so that the paths don't share any roads (but they may share the same junction). The administration decided to add the minimal number of roads so that this rules was fulfilled in the Berland capital as well. In the city road network should exist no more than one road between every pair of junctions before or after the reform.

**输入**

The first input line contains a pair of integers n, m (2 ≤ n ≤ 900, 1 ≤ m ≤ 100000), where n is the number of junctions and m is the number of roads. Each of the following m lines contains a description of a road that is given by the numbers of the connected junctions ai, bi (1 ≤ ai, bi ≤ n, ai ≠ bi). The junctions are numbered from 1 to n. It is possible to reach any junction of the city from any other one moving along roads.

**输出**

On the first line print t — the number of added roads. Then on t lines print the descriptions of the added roads in the format of the input data. You can use any order of printing the roads themselves as well as the junctions linked by every road. If there are several solutions to that problem, print any of them.

If the capital doesn't need the reform, print the single number 0.

If there's no solution, print the single number -1.

**限制**：每组测试 3 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 4 3<br>1 2<br>2 3<br>3 4 | 1<br>1 4 |

## 参考解法


## 题目信息

- 来源：Codeforces 45-H
- 比赛：School Team Contest 3 (Winter Computer School 2010/11)
- 官方难度：rating 2100
- 官方标签：graphs
- 原题链接：https://codeforces.com/problemset/problem/45/H

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
