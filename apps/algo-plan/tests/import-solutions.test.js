"use strict"
/**
 * import-solutions.test.js —— 存量题解导入 learning/bank
 *
 * 夹具是 learning 仓里的真实文件节选，落进临时的 learning 根目录。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const solutions = require("../tools/import-solutions.js")

// 真实片段：learning/code-solutions/c/src/c/leetcode_1_100.c
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

// 真实片段：learning/algo-c/problems/leetcode_1455/leetcode_1455.cpp
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

// 真实片段：learning/algo-c/sliding_window/README.md 的题号表
const README_EXCERPT = `# 滑动窗口 (Sliding Window)

## LeetCode 题目

| 题号 | 题目 | 难度 |
|------|------|------|
| 1984 | [学生分数的最小差值](../1984_minimum_difference/) | 简单 |
`

function tempDir(prefix) {
  return fs.mkdtempSync(path.join(os.tmpdir(), prefix))
}

/** 造一个临时的 learning 仓根目录，塞进真实的题解文件片段 */
function tempLearningRoot() {
  const root = tempDir("algo-plan-sol-")
  const cDir = path.join(root, "code-solutions", "c", "src", "c")
  fs.mkdirSync(cDir, { recursive: true })
  fs.writeFileSync(path.join(cDir, "leetcode_1_100.c"), C_RANGE_EXCERPT, "utf8")

  const singleDir = path.join(root, "algo-c", "problems", "leetcode_1455")
  fs.mkdirSync(singleDir, { recursive: true })
  fs.writeFileSync(path.join(singleDir, "leetcode_1455.cpp"), SINGLE_FILE_EXCERPT, "utf8")

  const topicDir = path.join(root, "algo-c", "sliding_window", "problems", "1984_minimum_difference")
  fs.mkdirSync(topicDir, { recursive: true })
  fs.writeFileSync(path.join(topicDir, "leetcode_1984.cpp"), SINGLE_FILE_EXCERPT, "utf8")
  fs.writeFileSync(path.join(root, "algo-c", "sliding_window", "README.md"), README_EXCERPT, "utf8")
  return root
}

test("题解导入器把题目写进 learning/bank/<track>/<id>.md", function () {
  const root = tempLearningRoot()
  const result = solutions.run({ env: {}, learningRoot: root })

  assert.deepStrictEqual(result.errors, [])
  assert.strictEqual(result.written.length, 4)
  // 区间文件里的两道题
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00056.md")))
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00075.md")))
  // 单题文件（没有专题信息 → 落到默认轨道）
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-01455.md")))
  // 专题目录里的题（滑动窗口 → interview 轨道）
  assert.ok(fs.existsSync(path.join(root, "bank", "interview", "lc-01984.md")))
})

test("落盘的题带参考解法，needs_fill 为 true", function () {
  const root = tempLearningRoot()
  solutions.run({ env: {}, learningRoot: root })

  const text = fs.readFileSync(path.join(root, "bank", "fundamentals", "lc-00056.md"), "utf8")
  assert.ok(text.indexOf("needs_fill: true") !== -1, text)
  assert.ok(text.indexOf("## 参考解法") !== -1, text)
  assert.ok(text.indexOf("```c") !== -1, text)
  assert.ok(text.indexOf("int **merge(") !== -1, text)
  assert.ok(text.indexOf("int main") === -1, "存量题解没有 main，不该凭空生成")
})

test("README 索引里的中文题名与难度被用上", function () {
  const root = tempLearningRoot()
  solutions.run({ env: {}, learningRoot: root })

  const text = fs.readFileSync(path.join(root, "bank", "interview", "lc-01984.md"), "utf8")
  assert.ok(text.indexOf("title: 学生分数的最小差值") !== -1, text)
  assert.ok(text.indexOf("difficulty: easy") !== -1, text)
  assert.ok(text.indexOf("topics: [滑动窗口]") !== -1, text)
  assert.ok(text.indexOf("tracks: [interview]") !== -1, text)
})

test("命名不符合约定的文件被跳过并记告警", function () {
  const root = tempLearningRoot()
  const interviewDir = path.join(root, "code-solutions", "c", "src", "c")
  fs.writeFileSync(path.join(interviewDir, "interview_17.c"), "// 17.14\n", "utf8")

  const result = solutions.run({ env: {}, learningRoot: root })
  assert.deepStrictEqual(result.errors, [])
  const joined = result.warnings.join("\n")
  assert.ok(joined.indexOf("interview_17.c") !== -1, joined)
})

test("重复导入默认跳过，--force 才覆盖", function () {
  const root = tempLearningRoot()
  solutions.run({ env: {}, learningRoot: root })

  const again = solutions.run({ env: {}, learningRoot: root })
  assert.strictEqual(again.written.length, 0)
  assert.strictEqual(again.skipped.length, 4)

  const forced = solutions.run({ env: {}, learningRoot: root, force: true })
  assert.strictEqual(forced.written.length, 4)
})

test("dry-run 只报告，不落盘", function () {
  const root = tempLearningRoot()
  const result = solutions.run({ env: {}, learningRoot: root, dryRun: true })
  assert.strictEqual(result.written.length, 4)
  // dry-run 一个字节都不写，连目录都不建
  assert.ok(!fs.existsSync(path.join(root, "bank")))
})

test("没设 learning 仓时导入报错而不是静默成功", function () {
  const result = solutions.run({ env: {} })
  assert.strictEqual(result.written.length, 0)
  assert.strictEqual(result.errors.length, 1)
})
