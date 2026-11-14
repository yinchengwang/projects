---
id: cf-5-e
source: codeforces
source_id: 5-E
title: 宾吉安信号
difficulty: hard
topics: [数据结构]
tracks: [engineering]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/5/E
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 5**　5-E　（官方难度 rating 2400）

原题名：Bindian Signalizing

**题意（中文）**

n 座山围成一圈，每座山有个瞭望人。山 A 能看见山 B，当且仅当连接两座山的两段圆弧中**至少有一段**上没有比它们两者都高的山。求互相能看见（至少一方能看见）的瞭望人对数。

**原题面（英文）**

Everyone knows that long ago on the territory of present-day Berland there lived Bindian tribes. Their capital was surrounded by n hills, forming a circle. On each hill there was a watchman, who watched the neighbourhood day and night.

In case of any danger the watchman could make a fire on the hill. One watchman could see the signal of another watchman, if on the circle arc connecting the two hills there was no hill higher than any of the two. As for any two hills there are two different circle arcs connecting them, the signal was seen if the above mentioned condition was satisfied on at least one of the arcs. For example, for any two neighbouring watchmen it is true that the signal of one will be seen by the other.

An important characteristics of this watch system was the amount of pairs of watchmen able to see each other's signals. You are to find this amount by the given heights of the hills.

**输入**

The first line of the input data contains an integer number n (3 ≤ n ≤ 106), n — the amount of hills around the capital. The second line contains n numbers — heights of the hills in clockwise order. All height numbers are integer and lie between 1 and 109.

**输出**

Print the required amount of pairs.

**限制**：每组测试 4 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 5<br>1 2 4 5 3 | 7 |

## 参考解法


## 题目信息

- 来源：Codeforces 5-E
- 比赛：Codeforces Beta Round 5
- 官方难度：rating 2400
- 官方标签：data structures
- 原题链接：https://codeforces.com/problemset/problem/5/E

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
