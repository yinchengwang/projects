---
id: cf-38-f
source: codeforces
source_id: 38-F
title: 聪明男孩
difficulty: hard
topics: [动态规划, 字符串, 博弈]
tracks: [advanced]
status: todo
needs_fill: true
url: https://codeforces.com/problemset/problem/38/F
created: 2026-11-12
updated: 2026-11-12
---

## 题面

**School Personal Contest #1 (Winter Computer School 2010/11) - Codeforces Beta Round 38 (ACM-ICPC Rules)**　38-F　（官方难度 rating 2100）

原题名：Smart Boy

**题意（中文）**

给定字典（若干小写字符串）。双方轮流在当前串的**首部或尾部**加一个字母，加完的串必须是字典中某个串的子串；加不了的人输。每一步结束时按公式计分（与串在字典中出现的次数有关）。求谁赢以及最终双方得分。

**原题面（英文）**

Once Petya and Vasya invented a new game and called it "Smart Boy". They located a certain set of words — the dictionary — for the game. It is admissible for the dictionary to contain similar words. 

The rules of the game are as follows: first the first player chooses any letter (a word as long as 1) from any word from the dictionary and writes it down on a piece of paper. The second player adds some other letter to this one's initial or final position, thus making a word as long as 2, then it's the first player's turn again, he adds a letter in the beginning or in the end thus making a word as long as 3 and so on. But the player mustn't break one condition: the newly created word must be a substring of a word from a dictionary. The player who can't add a letter to the current word without breaking the condition loses.

Also if by the end of a turn a certain string s is written on paper, then the player, whose turn it just has been, gets a number of points according to the formula:

where 

   is a sequence number of symbol c in Latin alphabet, numbered starting from 1. For example, , and . 

  is the number of words from the dictionary where the line s occurs as a substring at least once. 

Your task is to learn who will win the game and what the final score will be. Every player plays optimally and most of all tries to win, then — to maximize the number of his points, then — to minimize the number of the points of the opponent.

**输入**

The first input line contains an integer n which is the number of words in the located dictionary (1 ≤ n ≤ 30). The n lines contain the words from the dictionary — one word is written on one line. Those lines are nonempty, consisting of Latin lower-case characters no longer than 30 characters. Equal words can be in the list of words.

**输出**

On the first output line print a line "First" or "Second" which means who will win the game. On the second line output the number of points of the first player and the number of points of the second player after the game ends. Separate the numbers by a single space.

**限制**：每组测试 4 seconds，256 megabytes

## 测试用例

| input | expect |
|---|---|
| 2<br>aba<br>abac | Second<br>29 35 |

## 参考解法


## 题目信息

- 来源：Codeforces 38-F
- 比赛：School Personal Contest #1 (Winter Computer School 2010/11) - Codeforces Beta Round 38 (ACM-ICPC Rules)
- 官方难度：rating 2100
- 官方标签：dp、games、strings
- 原题链接：https://codeforces.com/problemset/problem/38/F

> 题面、输入输出说明与样例都是 Codeforces 的英文原文；上面的「题意（中文）」是中文摘要。
> 本题的测试用例来自题面里公开的 Examples（Codeforces 的完整测试数据不公开下载）。
> 参考解法尚未补充，`参考解法` 一节留空。
