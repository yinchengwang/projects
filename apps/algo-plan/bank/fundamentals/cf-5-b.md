---
id: cf-5-b
source: codeforces
source_id: 5-B
title: 居中对齐
difficulty: easy
topics: [字符串, 模拟]
tracks: [fundamentals]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/5/B
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 5**　5-B　（官方难度 rating 1200）

原题名：Center Alignment

**题意（中文）**

把若干行文本**居中对齐**排版：用 `*` 补出一个最小矩形边框。宽度差为奇数时，多出来的空位左右交替放置（先靠左，再靠右，依次交替）。

**原题面（英文）**

Almost every text editor has a built-in function of center text alignment. The developers of the popular in Berland text editor «Textpad» decided to introduce this functionality into the fourth release of the product.

You are to implement the alignment in the shortest possible time. Good luck!

**输入**

The input file consists of one or more lines, each of the lines contains Latin letters, digits and/or spaces. The lines cannot start or end with a space. It is guaranteed that at least one of the lines has positive length. The length of each line and the total amount of the lines do not exceed 1000.

**输出**

Format the given text, aligning it center. Frame the whole text with characters «*» of the minimum size. If a line cannot be aligned perfectly (for example, the line has even length, while the width of the block is uneven), you should place such lines rounding down the distance to the left or to the right edge and bringing them closer left or right alternatively (you should start with bringing left). Study the sample tests carefully to understand the output format better.

**限制**：每组测试 1 second，64 megabytes

## 测试用例

| input | expect |
|---|---|
| This  is<br><br>Codeforces<br>Beta<br>Round<br>5 | ************<br>* This  is *<br>*          *<br>*Codeforces*<br>*   Beta   *<br>*  Round   *<br>*     5    *<br>************ |

## 参考解法


## 题目信息

- 来源：Codeforces 5-B
- 比赛：Codeforces Beta Round 5
- 官方难度：rating 1200
- 官方标签：implementation、strings
- 原题链接：https://codeforces.com/problemset/problem/5/B

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
