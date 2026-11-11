"use strict"
/**
 * solutions-scan.test.js —— 存量题解的静态扫描
 *
 * 夹具全是真实文件的节选：learning/code-solutions/c/src/ 与 learning/algo-c/ 下的原文。
 */

const test = require("node:test")
const assert = require("node:assert")

const scan = require("../core/bank/solutions-scan.js")

// 真实片段：learning/code-solutions/c/src/c/leetcode_1_100.c（第 1–40 行，逐字）
const C_RANGE_EXCERPT = `#include <stdlib.h>
#include <math.h>

#include "common.h"

// leetcode 56

int **merge(int **intervals, int intervalsSize, int *intervalsColSize, int **returnColumnSizes)
{
    (void)intervalsColSize; // Suppress unused parameter warning

    int **ans = malloc(sizeof(int *) * intervalsSize);
    *returnColumnSizes = malloc(sizeof(int) * intervalsSize);

    for (int i = 0; i < intervalsSize; i++) {
        ans[i] = (int *)malloc(sizeof(int) * 2);
    }

    qsort(intervals, intervalsSize, sizeof(int *), int_cmp);

    int count = 0;
    for (int i = 0; i < intervalsSize; i++) {
        int L = intervals[i][0], R = intervals[i][1];
        if (count == 0 || ans[count - 1][1] < L) {
            returnColumnSizes[0][count] = 2;
            ans[count][0] = L;
            ans[count][1] = R;
            count++;
        } else {
            ans[count - 1][1] = fmax(ans[count - 1][1], R);
        }
    }

    *returnSize = count;
    return ans;
}

// leetcode 75
void sortColors_qsort(int *nums, int numsSize)
{
}
`

// 真实片段：learning/code-solutions/c/src/cpp/leetcode_1_100.cpp（第 1–22 行，逐字）
const CPP_RANGE_EXCERPT = `#include "leetcode/leetcode_cpp.h"

// leetcode 1 枚举法
vector<int> LeetCode_Solution::twoSumEnum(vector<int>& nums, int target) {
    vector<int> res;
    for (int i = 0; i < nums.size(); i++) {
        for (int j = i + 1; j < nums.size(); j++) {
            if (nums[i] + nums[j] == target) {
                res.push_back(i);
                res.push_back(j);
                return res;
            }
        }
    }

    return res;
}

// leetcode 1 hash map
vector<int> LeetCode_Solution::twoSumHash(vector<int>& nums, int target)
{
}
`

// 真实片段：learning/algo-c/problems/leetcode_1455/leetcode_1455.cpp（第 1–12 行，逐字）
const SINGLE_FILE_EXCERPT = `#include <iostream>
#include <string>
using namespace std;

class Solution {
public:
    bool isPrefix(string sentence, int start, int end, string searchWord)
    {
        for (int i = 0; i < searchWord.size(); i++) {
            if (start + i > end || sentence[i] != searchWord[i]) {
                return false;
            }
        }
    }
}
`

// 真实片段：learning/algo-c/sliding_window/README.md 的「LeetCode 题目」表（逐字）
const README_EXCERPT = `# 滑动窗口 (Sliding Window)

## LeetCode 题目

| 题号 | 题目 | 难度 |
|------|------|------|
| 1984 | [学生分数的最小差值](../1984_minimum_difference/) | 简单 |
| 2270 | [分割数组的方案数](../2270_ways_to_split/) | 中等 |
| 3297 | [统计相似字符串对的数目](../3297_count_matching/) | 简单 |
`

test("区间文件按 // leetcode 标记切分，id 与解法说明都对得上", function () {
  const blocks = scan.splitByMarker(C_RANGE_EXCERPT)
  assert.strictEqual(blocks.length, 2)
  assert.strictEqual(blocks[0].id, 56)
  assert.strictEqual(blocks[0].rest, "")
  assert.strictEqual(blocks[1].id, 75)
  assert.ok(blocks[0].code.indexOf("int **merge(") !== -1, "第一段应含 merge 实现")
  assert.ok(blocks[1].code.indexOf("sortColors_qsort") !== -1, "第二段应含 sortColors 实现")
})

test("区间文件第一段带上文件头的 #include", function () {
  const blocks = scan.splitByMarker(C_RANGE_EXCERPT)
  assert.ok(blocks[0].code.indexOf("#include <stdlib.h>") !== -1, "第一段应含 #include")
  assert.ok(blocks[0].code.indexOf('#include "common.h"') !== -1, "第一段应含 common.h")
})

test("标记后面带解法说明时，说明被记进 rest", function () {
  const blocks = scan.splitByMarker(CPP_RANGE_EXCERPT)
  assert.strictEqual(blocks.length, 2)
  assert.strictEqual(blocks[0].id, 1)
  assert.strictEqual(blocks[0].rest, "枚举法")
  assert.strictEqual(blocks[1].rest, "hash map")
})

test("文件名能读出题号，语言也能读出来", function () {
  assert.strictEqual(scan.idFromFileName("leetcode_1455.cpp"), 1455)
  assert.strictEqual(scan.idFromFileName("leetcode_2070.c"), 2070)
  assert.strictEqual(scan.idFromFileName("leetcode_1_100.c"), null)
  assert.strictEqual(scan.idFromFileName("interview_17.c"), null)
  assert.strictEqual(scan.langOf("a/leetcode_1455.cpp"), "cpp")
  assert.strictEqual(scan.langOf("a/leetcode_2070.c"), "c")
})

test("专题目录能认出标签，algo-c/problems 根目录认不出", function () {
  assert.strictEqual(
    scan.topicOfSolutionFile("learning/algo-c/sliding_window/problems/1984_minimum_difference/leetcode_1984.cpp"),
    "滑动窗口"
  )
  assert.strictEqual(
    scan.topicOfSolutionFile("learning/algo-c/prefix_sum/problems/3159_query_results/leetcode_3159.cpp"),
    "前缀和"
  )
  assert.strictEqual(scan.topicOfSolutionFile("learning/algo-c/problems/leetcode_1455/leetcode_1455.cpp"), "")
  assert.strictEqual(scan.topicOfSolutionFile("learning/code-solutions/c/src/c/leetcode_1_100.c"), "")
})

test("README 索引表能读出中文题名与难度", function () {
  const index = scan.parseSolutionIndex([
    { path: "learning/algo-c/sliding_window/README.md", text: README_EXCERPT },
  ])
  assert.strictEqual(index.byId[1984].title, "学生分数的最小差值")
  assert.strictEqual(index.byId[1984].difficulty, "easy")
  assert.strictEqual(index.byId[2270].difficulty, "medium")
  assert.strictEqual(index.byId[3297].title, "统计相似字符串对的数目")
})

test("parseSolutionFiles 产出契约形状的条目", function () {
  const parsed = scan.parseSolutionFiles([
    { path: "learning/code-solutions/c/src/c/leetcode_1_100.c", text: C_RANGE_EXCERPT },
    { path: "learning/algo-c/problems/leetcode_1455/leetcode_1455.cpp", text: SINGLE_FILE_EXCERPT },
  ])
  assert.strictEqual(parsed.items.length, 3)
  const first = parsed.items[0]
  assert.deepStrictEqual(Object.keys(first).sort(), ["code", "file", "lang", "source", "sourceId", "title"])
  assert.strictEqual(first.source, "leetcode")
  assert.strictEqual(first.sourceId, "56")
  assert.strictEqual(first.title, "LeetCode 56")
  assert.strictEqual(first.lang, "c")
  assert.strictEqual(parsed.items[2].sourceId, "1455")
  assert.strictEqual(parsed.items[2].title, "LeetCode 1455")
})

test("索引里有的题用中文题名，没有的用 LeetCode <题号>", function () {
  const index = scan.parseSolutionIndex([
    { path: "learning/algo-c/sliding_window/README.md", text: README_EXCERPT },
  ])
  const parsed = scan.parseSolutionFiles(
    [
      {
        path: "learning/algo-c/sliding_window/problems/1984_minimum_difference/leetcode_1984.cpp",
        text: SINGLE_FILE_EXCERPT,
      },
    ],
    index.byId
  )
  assert.strictEqual(parsed.items[0].title, "学生分数的最小差值")
})

test("命名不符合约定的文件不产出条目，只记告警", function () {
  const parsed = scan.parseSolutionFiles([
    { path: "learning/code-solutions/c/src/c/interview_17.c", text: "// 17.14\n" },
    { path: "learning/algo-c/contest/Weekly_match/433/433_1.cpp", text: "// 100548. 变长子数组求和\n" },
  ])
  assert.strictEqual(parsed.items.length, 0)
  assert.strictEqual(parsed.warnings.length, 2)
})
