---
id: cf-35-e
source: codeforces
source_id: 35-E
title: 阅兵
difficulty: hard
topics: [排序, 数据结构]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/35/E
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 35 (Div. 2)**　35-E　（官方难度 rating 2100）

原题名：Parade

**题意（中文）**

n 栋矩形高楼（高度 h_i，横向范围 [l_i, r_i]）。用一条**水平/垂直折线**从西到东把这些楼全部围住：围出的面积最小；面积相同时折线总长度最短；相邻两段必须垂直；端点高度为 0。

**原题面（英文）**

No Great Victory anniversary in Berland has ever passed without the war parade. This year is not an exception. That’s why the preparations are on in full strength. Tanks are building a line, artillery mounts are ready to fire, soldiers are marching on the main square... And the air forces general Mr. Generalov is in trouble again. This year a lot of sky-scrapers have been built which makes it difficult for the airplanes to fly above the city. It was decided that the planes should fly strictly from south to north. Moreover, there must be no sky scraper on a plane’s route, otherwise the anniversary will become a tragedy. The Ministry of Building gave the data on n sky scrapers (the rest of the buildings are rather small and will not be a problem to the planes). When looking at the city from south to north as a geometrical plane, the i-th building is a rectangle of height hi. Its westernmost point has the x-coordinate of li and the easternmost — of ri. The terrain of the area is plain so that all the buildings stand on one level. Your task as the Ministry of Defence’s head programmer is to find an enveloping polyline using the data on the sky-scrapers. The polyline’s properties are as follows:

  If you look at the city from south to north as a plane, then any part of any building will be inside or on the boarder of the area that the polyline encloses together with the land surface. 

 The polyline starts and ends on the land level, i.e. at the height equal to 0. 

 The segments of the polyline are parallel to the coordinate axes, i.e. they can only be vertical or horizontal. 

 The polyline’s vertices should have integer coordinates. 

 If you look at the city from south to north the polyline (together with the land surface) must enclose the minimum possible area. 

 The polyline must have the smallest length among all the polylines, enclosing the minimum possible area with the land. 

 The consecutive segments of the polyline must be perpendicular. 

   Picture to the second sample test (the enveloping polyline is marked on the right).

**输入**

The first input line contains integer n (1 ≤ n ≤ 100000). Then follow n lines, each containing three integers hi, li, ri (1 ≤ hi ≤ 109,  - 109 ≤ li < ri ≤ 109).

**输出**

In the first line output integer m — amount of vertices of the enveloping polyline. The next m lines should contain 2 integers each — the position and the height of the polyline’s vertex. Output the coordinates of each vertex in the order of traversing the polyline from west to east. Remember that the first and the last vertices of the polyline should have the height of 0.

**限制**：每组测试 2 seconds，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 2<br>3 0 2<br>4 1 3 | 6<br>0 0<br>0 3<br>1 3<br>1 4<br>3 4<br>3 0 |

## 参考解法


## 题目信息

- 来源：Codeforces 35-E
- 比赛：Codeforces Beta Round 35 (Div. 2)
- 官方难度：rating 2100
- 官方标签：data structures、sortings
- 原题链接：https://codeforces.com/problemset/problem/35/E

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
