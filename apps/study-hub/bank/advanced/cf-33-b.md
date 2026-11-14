---
id: cf-33-b
source: codeforces
source_id: 33-B
title: 字符串问题
difficulty: hard
topics: [最短路]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/33/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 33 (Codeforces format)**　33-B　（官方难度 rating 1800）

原题名：String Problem

**题意（中文）**

给两个字符串 s、t，以及一组「把字母 A 改成 B 需要 W 元」的替换规则（两串都能改）。求把两个串变得**完全相同**的最小花费，并输出一个结果串；无解输出 -1。

**原题面（英文）**

Boy Valera likes strings. And even more he likes them, when they are identical. That's why in his spare time Valera plays the following game. He takes any two strings, consisting of lower case Latin letters, and tries to make them identical. According to the game rules, with each move Valera can change one arbitrary character Ai in one of the strings into arbitrary character Bi, but he has to pay for every move a particular sum of money, equal to Wi. He is allowed to make as many moves as he needs. Since Valera is a very economical boy and never wastes his money, he asked you, an experienced programmer, to help him answer the question: what minimum amount of money should Valera have to get identical strings.

**输入**

The first input line contains two initial non-empty strings s and t, consisting of lower case Latin letters. The length of each string doesn't exceed 105. The following line contains integer n (0 ≤ n ≤ 500) — amount of possible changings. Then follow n lines, each containing characters Ai and Bi (lower case Latin letters) and integer Wi (0 ≤ Wi ≤ 100), saying that it's allowed to change character Ai into character Bi in any of the strings and spend sum of money Wi.

**输出**

If the answer exists, output the answer to the problem, and the resulting string. Otherwise output -1 in the only line. If the answer is not unique, output any.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| uayd<br>uxxd<br>3<br>a x 8<br>x y 13<br>d c 3 | 21<br>uxyd |

## 参考解法


## 题目信息

- 来源：Codeforces 33-B
- 比赛：Codeforces Beta Round 33 (Codeforces format)
- 官方难度：rating 1800
- 官方标签：shortest paths
- 原题链接：https://codeforces.com/problemset/problem/33/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
