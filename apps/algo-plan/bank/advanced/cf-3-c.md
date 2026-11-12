---
id: cf-3-c
source: codeforces
source_id: 3-C
title: 井字棋
difficulty: hard
topics: [暴力枚举, 博弈, 模拟]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/3/C
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**Codeforces Beta Round 3**　3-C　（官方难度 rating 1800）

原题名：Tic-tac-toe

**题意（中文）**

给一个 3×3 的井字棋局面（`.` 空、X 先手、0 后手），判断属于哪种情况：轮到先手 / 轮到后手 / 非法局面 / 先手刚赢 / 后手刚赢 / 平局。

**原题面（英文）**

Certainly, everyone is familiar with tic-tac-toe game. The rules are very simple indeed. Two players take turns marking the cells in a 3 × 3 grid (one player always draws crosses, the other — noughts). The player who succeeds first in placing three of his marks in a horizontal, vertical or diagonal line wins, and the game is finished. The player who draws crosses goes first. If the grid is filled, but neither Xs, nor 0s form the required line, a draw is announced.

You are given a 3 × 3 grid, each grid cell is empty, or occupied by a cross or a nought. You have to find the player (first or second), whose turn is next, or print one of the verdicts below: 

  illegal — if the given board layout can't appear during a valid game; 

 the first player won — if in the given board layout the first player has just won; 

 the second player won — if in the given board layout the second player has just won; 

 draw — if the given board layout has just let to a draw.

**输入**

The input consists of three lines, each of the lines contains characters ".", "X" or "0" (a period, a capital letter X, or a digit zero).

**输出**

Print one of the six verdicts: first, second, illegal, the first player won, the second player won or draw.

**限制**：每组测试 1 second，64 megabytes

## 测试用例

| input | expect |
|---|---|
| X0X<br>.0.<br>.X. | second |

## 参考解法


## 题目信息

- 来源：Codeforces 3-C
- 比赛：Codeforces Beta Round 3
- 官方难度：rating 1800
- 官方标签：brute force、games、implementation
- 原题链接：https://codeforces.com/problemset/problem/3/C

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
