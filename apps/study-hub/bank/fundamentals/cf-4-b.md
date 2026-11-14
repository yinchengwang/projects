---
id: cf-4-b
source: codeforces
source_id: 4-B
title: 考前复习
difficulty: easy
topics: [贪心, 构造]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/4/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 4 (Div. 2 Only)**　4-B　（官方难度 rating 1200）

原题名：Before an Exam

**题意（中文）**

父母规定：第 i 天学习生物的时间要在 [minTime_i, maxTime_i] 之间，总时间必须正好是 sumTime。输出一个满足条件的日程安排；无解输出 NO。

**原题面（英文）**

Tomorrow Peter has a Biology exam. He does not like this subject much, but d days ago he learnt that he would have to take this exam. Peter's strict parents made him prepare for the exam immediately, for this purpose he has to study not less than minTimei and not more than maxTimei hours per each i-th day. Moreover, they warned Peter that a day before the exam they would check how he has followed their instructions.

So, today is the day when Peter's parents ask him to show the timetable of his preparatory studies. But the boy has counted only the sum of hours sumTime spent him on preparation, and now he wants to know if he can show his parents a timetable sсhedule with d numbers, where each number sсhedulei stands for the time in hours spent by Peter each i-th day on biology studies, and satisfying the limitations imposed by his parents, and at the same time the sum total of all schedulei should equal to sumTime.

**输入**

The first input line contains two integer numbers d, sumTime (1 ≤ d ≤ 30, 0 ≤ sumTime ≤ 240) — the amount of days, during which Peter studied, and the total amount of hours, spent on preparation. Each of the following d lines contains two integer numbers minTimei, maxTimei (0 ≤ minTimei ≤ maxTimei ≤ 8), separated by a space — minimum and maximum amount of hours that Peter could spent in the i-th day.

**输出**

In the first line print YES, and in the second line print d numbers (separated by a space), each of the numbers — amount of hours, spent by Peter on preparation in the corresponding day, if he followed his parents' instructions; or print NO in the unique line. If there are many solutions, print any of them.

**限制**：每组测试 0.5 second，64 megabytes

## 测试用例

| input | expect |
|---|---|
| 1 48<br>5 7 | NO |

## 参考解法


## 题目信息

- 来源：Codeforces 4-B
- 比赛：Codeforces Beta Round 4 (Div. 2 Only)
- 官方难度：rating 1200
- 官方标签：constructive algorithms、greedy
- 原题链接：https://codeforces.com/problemset/problem/4/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
