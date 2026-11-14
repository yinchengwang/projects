---
id: cf-32-b
source: codeforces
source_id: 32-B
title: Borze 密码
difficulty: easy
topics: [表达式解析, 模拟]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/32/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 32 (Div. 2, Codeforces format)**　32-B　（官方难度 rating 800）

原题名：Borze

**题意（中文）**

Borze 密码是一种三进制写法：`.` 表示 0，`-.` 表示 1，`--` 表示 2。给一串 Borze 密码（保证合法），把它还原成对应的三进制数（输出可能有前导零）。

**原题面（英文）**

Ternary numeric notation is quite popular in Berland. To telegraph the ternary number the Borze alphabet is used. Digit 0 is transmitted as «.», 1 as «-.» and 2 as «--». You are to decode the Borze code, i.e. to find out the ternary number given its representation in Borze alphabet.

**输入**

The first line contains a number in Borze code. The length of the string is between 1 and 200 characters. It's guaranteed that the given string is a valid Borze code of some ternary number (this number can have leading zeroes).

**输出**

Output the decoded ternary number. It can have leading zeroes.

**限制**：每组测试 2 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| .-.-- | 012 |

## 参考解法


## 题目信息

- 来源：Codeforces 32-B
- 比赛：Codeforces Beta Round 32 (Div. 2, Codeforces format)
- 官方难度：rating 800
- 官方标签：expression parsing、implementation
- 原题链接：https://codeforces.com/problemset/problem/32/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
