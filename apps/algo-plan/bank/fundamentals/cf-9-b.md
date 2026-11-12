---
id: cf-9-b
source: codeforces
source_id: 9-B
title: 赶考的学生
difficulty: easy
topics: [暴力枚举, 模拟]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/9/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 9 (Div. 2 Only)**　9-B　（官方难度 rating 1200）

原题名：Running Student

**题意（中文）**

男生坐在小巴上沿着 x 轴正向行驶（速度 v_b），各站点坐标已知；他自己跑向大学的速度是 v_s，大学在 (x_u, y_u)。他只能在站点下车。求**在哪个站下车最快**（并列时取离大学最近的站点）。

**原题面（英文）**

And again a misfortune fell on Poor Student. He is being late for an exam.

Having rushed to a bus stop that is in point (0, 0), he got on a minibus and they drove along a straight line, parallel to axis OX, in the direction of increasing x.

Poor Student knows the following: 

  during one run the minibus makes n stops, the i-th stop is in point (xi, 0) 

 coordinates of all the stops are different 

 the minibus drives at a constant speed, equal to vb 

 it can be assumed the passengers get on and off the minibus at a bus stop momentarily 

 Student can get off the minibus only at a bus stop 

 Student will have to get off the minibus at a terminal stop, if he does not get off earlier 

 the University, where the exam will be held, is in point (xu, yu) 

 Student can run from a bus stop to the University at a constant speed vs as long as needed 

 a distance between two points can be calculated according to the following formula:  

 Student is already on the minibus, so, he cannot get off at the first bus stop 

Poor Student wants to get to the University as soon as possible. Help him to choose the bus stop, where he should get off. If such bus stops are multiple, choose the bus stop closest to the University.

**输入**

The first line contains three integer numbers: 2 ≤ n ≤ 100, 1 ≤ vb, vs ≤ 1000. The second line contains n non-negative integers in ascending order: coordinates xi of the bus stop with index i. It is guaranteed that x1 equals to zero, and xn ≤ 105. The third line contains the coordinates of the University, integers xu and yu, not exceeding 105 in absolute value.

**输出**

In the only line output the answer to the problem — index of the optimum bus stop.

**限制**：每组测试 1 second，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 4 5 2<br>0 2 4 6<br>4 1 | 3 |

## 参考解法


## 题目信息

- 来源：Codeforces 9-B
- 比赛：Codeforces Beta Round 9 (Div. 2 Only)
- 官方难度：rating 1200
- 官方标签：brute force、geometry、implementation
- 原题链接：https://codeforces.com/problemset/problem/9/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
