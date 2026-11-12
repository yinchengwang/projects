---
id: cf-30-c
source: codeforces
source_id: 30-C
title: 射击场
difficulty: hard
topics: [动态规划, 概率]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/30/C
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 30 (Codeforces format)**　30-C　（官方难度 rating 1800）

原题名：Shooting Gallery

**题意（中文）**

n 个靶子，各有坐标 (x_i, y_i)、出现时刻 t_i、命中概率 p_i。准星在平面上移动速度为 1（t=0 时可任意瞄准）。求**最大期望命中数**。

**原题面（英文）**

One warm and sunny day king Copa decided to visit the shooting gallery, located at the Central Park, and try to win the main prize — big pink plush panda. The king is not good at shooting, so he invited you to help him.

The shooting gallery is an infinite vertical plane with Cartesian coordinate system on it. The targets are points on this plane. Each target is described by it's coordinates xi, and yi, by the time of it's appearance ti and by the number pi, which gives the probability that Copa hits this target if he aims at it.

A target appears and disappears instantly, so Copa can hit the target only if at the moment ti his gun sight aimed at (xi, yi). Speed of movement of the gun sight on the plane is equal to 1. Copa knows all the information about the targets beforehand (remember, he is a king!). He wants to play in the optimal way, which maximizes the expected value of the amount of hit targets. He can aim at any target at the moment 0.

**输入**

The first line contains integer n (1 ≤ n ≤ 1000) — amount of targets in the shooting gallery. Then n lines follow, each describing one target. Each description consists of four numbers xi, yi, ti, pi (where xi, yi, ti — integers,  - 1000 ≤ xi, yi ≤ 1000, 0 ≤ ti ≤ 109, real number pi is given with no more than 6 digits after the decimal point, 0 ≤ pi ≤ 1). No two targets may be at the same point.

**输出**

Output the maximum expected value of the amount of targets that was shot by the king. Your answer will be accepted if it differs from the correct answer by not more than 10 - 6.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 1<br>0 0 0 0.5 | 0.5000000000 |

## 参考解法


## 题目信息

- 来源：Codeforces 30-C
- 比赛：Codeforces Beta Round 30 (Codeforces format)
- 官方难度：rating 1800
- 官方标签：dp、probabilities
- 原题链接：https://codeforces.com/problemset/problem/30/C

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
