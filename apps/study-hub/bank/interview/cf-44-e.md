---
id: cf-44-e
source: codeforces
source_id: 44-E
title: 猴子女巫
difficulty: medium
topics: [动态规划]
tracks: [interview]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/44/E
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**School Team Contest 2 (Winter Computer School 2010/11)**　44-E　（官方难度 rating 1400）

原题名：Anfisa the Monkey

**题意（中文）**

猴子打出了一长串只有小写字母的文本，想把它分成 **k 行**，每行长度在 [a, b] 之间。不能增删或改动任何字符。输出一种切法；无解输出 `No solution`。

**原题面（英文）**

Anfisa the monkey learns to type. She is yet unfamiliar with the "space" key and can only type in lower-case Latin letters. Having typed for a fairly long line, Anfisa understood that it would be great to divide what she has written into k lines not shorter than a and not longer than b, for the text to resemble human speech more. Help Anfisa.

**输入**

The first line contains three integers k, a and b (1 ≤ k ≤ 200, 1 ≤ a ≤ b ≤ 200). The second line contains a sequence of lowercase Latin letters — the text typed by Anfisa. It is guaranteed that the given line is not empty and its length does not exceed 200 symbols.

**输出**

Print k lines, each of which contains no less than a and no more than b symbols — Anfisa's text divided into lines. It is not allowed to perform any changes in the text, such as: deleting or adding symbols, changing their order, etc. If the solution is not unique, print any of them. If there is no solution, print "No solution" (without quotes).

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 3 2 5<br>abrakadabra | ab<br>rakad<br>abra |

## 参考解法


## 题目信息

- 来源：Codeforces 44-E
- 比赛：School Team Contest 2 (Winter Computer School 2010/11)
- 官方难度：rating 1400
- 官方标签：dp
- 原题链接：https://codeforces.com/problemset/problem/44/E

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
