---
id: cf-8-b
source: codeforces
source_id: 8-B
title: 痴迷机器人
difficulty: medium
topics: [图论, 构造, 模拟]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/8/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 8**　8-B　（官方难度 rating 1400）

原题名：Obsession with Robots

**题意（中文）**

给一串机器人的移动记录（由 L/R/U/D 组成）。问是否存在一张无限棋盘地图（空格与障碍物），使得机器人从某个空格出发、沿记录走完不撞墙，且**这条路是起点到终点的最短路**。存在输出 OK，否则 BUG。

**原题面（英文）**

The whole world got obsessed with robots,and to keep pace with the progress, great Berland's programmer Draude decided to build his own robot. He was working hard at the robot. He taught it to walk the shortest path from one point to another, to record all its movements, but like in many Draude's programs, there was a bug — the robot didn't always walk the shortest path. Fortunately, the robot recorded its own movements correctly. Now Draude wants to find out when his robot functions wrong. Heh, if Draude only remembered the map of the field, where he tested the robot, he would easily say if the robot walked in the right direction or not. But the field map was lost never to be found, that's why he asks you to find out if there exist at least one map, where the path recorded by the robot is the shortest.

The map is an infinite checkered field, where each square is either empty, or contains an obstruction. It is also known that the robot never tries to run into the obstruction. By the recorded robot's movements find out if there exist at least one such map, that it is possible to choose for the robot a starting square (the starting square should be empty) such that when the robot moves from this square its movements coincide with the recorded ones (the robot doesn't run into anything, moving along empty squares only), and the path from the starting square to the end one is the shortest.

In one movement the robot can move into the square (providing there are no obstrutions in this square) that has common sides with the square the robot is currently in.

**输入**

The first line of the input file contains the recording of the robot's movements. This recording is a non-empty string, consisting of uppercase Latin letters L, R, U and D, standing for movements left, right, up and down respectively. The length of the string does not exceed 100.

**输出**

In the first line output the only word OK (if the above described map exists), or BUG (if such a map does not exist).

**限制**：每组测试 2 seconds，64 megabytes

## 测试用例

| input | expect |
|---|---|
| LLUUUR | OK |

## 参考解法


## 题目信息

- 来源：Codeforces 8-B
- 比赛：Codeforces Beta Round 8
- 官方难度：rating 1400
- 官方标签：constructive algorithms、graphs、implementation
- 原题链接：https://codeforces.com/problemset/problem/8/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
