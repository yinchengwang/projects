"use strict"
/**
 * m2-e2e.test.js —— M2 端到端：死脚本 / 存量题解 → learning/bank/<track>/<id>.md → 再读回来
 *
 * 全程用临时目录当 learning 仓，绝不碰真实的 C:/code/StillEvolving/learning。
 * 夹具是真实文件格式的小样本：死脚本 4 道题、题解 4 个文件。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const importBank = require("../tools/import-bank.js")
const importCatalog = require("../tools/import-catalog.js")
const importSolutions = require("../tools/import-solutions.js")
const { parseProblemMarkdown } = require("../core/bank/parse.js")
const { isValidId } = require("../core/bank/problem.js")
const { tmpDir: makeTmpDir } = require("./helpers/tmp.js")

/** 建临时目录 */
function tmpDir(tag) {
  return makeTmpDir("algo-plan-m2-" + tag + "-")
}

/** 写文件（自动建目录） */
function writeFile(file, text) {
  fs.mkdirSync(path.dirname(file), { recursive: true })
  fs.writeFileSync(file, text, "utf8")
}

/** 死脚本夹具：照抄 projects/scripts/gen-practice-data.js 的真实写法 */
const FIXTURE_SCRIPT = [
  "function lc(title, id, diff, tags) {",
  "  return {",
  "    title,",
  '    platform: "leetcode",',
  "    problemId: id,",
  "    difficulty: diff,",
  "    tags: Array.isArray(tags) ? tags : [tags],",
  "  };",
  "}",
  "var arrayItems = [",
  '  lc("两数之和", 1, "easy", ["哈希表"]),',
  '  lc("移除元素", 27, "easy", ["双指针"]),',
  '  lc("三数之和", 15, "medium", ["排序"]),',
  "];",
  "var hashItems = [",
  '  lc("两数之和", 1, "easy", ["哈希表"]),',
  "];",
  "var dpItems = [",
  '  lc("爬楼梯", 70, "easy", ["动态规划"]),',
  "];",
  "var categories = [",
  '  cat("array", "数组", 1, 1, "📗", "数组是最基本的数据结构。", arrayItems),',
  '  cat("hash-table", "哈希表", 2, 2, "📕", "哈希表。", hashItems),',
  '  cat("dp-basic", "动态规划", 3, 3, "📘", "动态规划。", dpItems),',
  "];",
  "",
].join("\n")

/** 题解夹具 1：区间文件 + 标记注释（照抄 learning/code-solutions/c/src/c/leetcode_1_100.c） */
const FIXTURE_RANGE_C = [
  "#include <stdlib.h>",
  "#include <math.h>",
  "",
  '#include "common.h"',
  "",
  "// leetcode 56",
  "",
  "int **merge(int **intervals, int intervalsSize, int *intervalsColSize, int **returnColumnSizes)",
  "{",
  "    return intervals;",
  "}",
  "",
].join("\n")

/** 题解夹具 2：单题文件（照抄 learning/algo-c/problems/leetcode_1455/leetcode_1455.cpp） */
const FIXTURE_SINGLE_CPP = [
  "#include <iostream>",
  "#include <string>",
  "using namespace std;",
  "",
  "class Solution {",
  "public:",
  "    bool isPrefix(string sentence, int start, int end, string searchWord)",
  "    {",
  "        for (int i = 0; i < searchWord.size(); i++) {",
  "        }",
  "        return true;",
  "    }",
  "};",
  "",
].join("\n")

/** 题解夹具 3：专题目录下的单题文件（照抄 learning/algo-c/sliding_window/problems/1984_minimum_difference/leetcode_1984.cpp） */
const FIXTURE_TOPIC_CPP = [
  "#include <vector>",
  "#include <algorithm>",
  "using namespace std;",
  "",
  "class Solution {",
  "public:",
  "    int minimumDifference(vector<int>& nums, int k) {",
  "        sort(nums.begin(), nums.end());",
  "        return 0;",
  "    }",
  "};",
  "",
].join("\n")

/** 题解夹具 4：与目录来源同 id 的题（lc-00070），两边算出的轨道不同，专门验 id 级冲突判断
 *  （真实数据里同样的情况有 5 道：lc-00004 / lc-00007 / lc-00009 / lc-00075 / lc-00326） */
const FIXTURE_SAME_ID_CPP = [
  "#include <vector>",
  "using namespace std;",
  "",
  "class Solution {",
  "public:",
  "    int climbStairs(int n) {",
  "        return n <= 2 ? n : climbStairs(n - 1) + climbStairs(n - 2);",
  "    }",
  "};",
  "",
].join("\n")

/** 专题索引表：照抄 learning/algo-c/sliding_window/README.md 结尾的表 */
const FIXTURE_INDEX_README = [
  "# 滑动窗口",
  "",
  "## LeetCode 题目",
  "",
  "| 题号 | 题目 | 难度 |",
  "|------|------|------|",
  "| 1984 | [学生分数的最小差值](../1984_minimum_difference/) | 简单 |",
  "",
].join("\n")

/** 必须跳过的两个文件：竞赛题与面经章节号（照抄真实文件名反例） */
const FIXTURE_CONTEST = "// 循环数组中相邻元素的最大差值\nint main() { return 0; }\n"
const FIXTURE_INTERVIEW = "// 17.14\nint main() { return 0; }\n"

/** 建一个带夹具的 learning 仓 */
function makeLearning() {
  const root = tmpDir("learning")
  writeFile(path.join(root, "code-solutions", "c", "src", "c", "leetcode_1_100.c"), FIXTURE_RANGE_C)
  writeFile(path.join(root, "code-solutions", "c", "src", "c", "interview_17.c"), FIXTURE_INTERVIEW)
  writeFile(path.join(root, "algo-c", "problems", "leetcode_1455", "leetcode_1455.cpp"), FIXTURE_SINGLE_CPP)
  writeFile(path.join(root, "algo-c", "problems", "leetcode_70", "leetcode_70.cpp"), FIXTURE_SAME_ID_CPP)
  writeFile(
    path.join(root, "algo-c", "sliding_window", "problems", "1984_minimum_difference", "leetcode_1984.cpp"),
    FIXTURE_TOPIC_CPP
  )
  writeFile(path.join(root, "algo-c", "sliding_window", "README.md"), FIXTURE_INDEX_README)
  writeFile(path.join(root, "algo-c", "contest", "Weekly_match", "433", "433_1.cpp"), FIXTURE_CONTEST)
  return root
}

/** 建一个带夹具的死脚本目录 */
function makeScripts() {
  const dir = tmpDir("scripts")
  writeFile(path.join(dir, "gen-practice-data.js"), FIXTURE_SCRIPT)
  return dir
}

/** 列出 bank 下的全部文件（绝对路径） */
function listBankFiles(root) {
  const out = []
  function walk(dir) {
    let entries
    try {
      entries = fs.readdirSync(dir, { withFileTypes: true })
    } catch (e) {
      return
    }
    entries.forEach(function (entry) {
      const abs = path.join(dir, entry.name)
      if (entry.isDirectory()) {
        walk(abs)
        return
      }
      out.push(abs)
    })
  }
  walk(path.join(root, "bank"))
  return out
}

/** 读回一个题库文件并解析 */
function readProblem(root, track, id) {
  const file = path.join(root, "bank", track, id + ".md")
  return { file: file, parsed: parseProblemMarkdown(fs.readFileSync(file, "utf8")) }
}

test("目录导入：死脚本的题落成 learning/bank/<track>/<id>.md", function () {
  const root = makeLearning()
  const result = importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  assert.deepStrictEqual(result.errors, [], result.errors.join("\n"))
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00027.md")), "移除元素应落在 fundamentals")
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00015.md")), "三数之和应落在 fundamentals")
  assert.ok(fs.existsSync(path.join(root, "bank", "advanced", "lc-00070.md")), "爬楼梯属 dp-basic，应落在 advanced")
})

test("目录导入：frontmatter 里 needs_fill 为 true", function () {
  const root = makeLearning()
  importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  const text = fs.readFileSync(path.join(root, "bank", "fundamentals", "lc-00027.md"), "utf8")
  assert.ok(text.indexOf("needs_fill: true") !== -1, text)
})

test("目录导入：题库文件能被 M1 的 parseProblemMarkdown 读回来", function () {
  const root = makeLearning()
  importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  const parsed = readProblem(root, "fundamentals", "lc-00015").parsed
  assert.deepStrictEqual(parsed.warnings, [], parsed.warnings.join("\n"))
  assert.strictEqual(parsed.problem.id, "lc-00015")
  assert.strictEqual(parsed.problem.title, "三数之和")
  assert.strictEqual(parsed.problem.difficulty, "medium")
  assert.deepStrictEqual(parsed.problem.topics, ["排序"])
  assert.strictEqual(parsed.problem.needsFill, true)
})

test("目录导入：同一题出现在两个分类时只写一个文件，轨道取并集", function () {
  const root = makeLearning()
  importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  const files = listBankFiles(root)
  const ones = files.filter(function (f) {
    return path.basename(f) === "lc-00001.md"
  })
  assert.strictEqual(ones.length, 1, "lc-00001 只应有一个文件，实际 " + ones.length)
  const parsed = parseProblemMarkdown(fs.readFileSync(ones[0], "utf8"))
  assert.ok(parsed.problem.tracks.indexOf("fundamentals") !== -1, parsed.problem.tracks.join(","))
  assert.ok(parsed.problem.tracks.indexOf("interview") !== -1, parsed.problem.tracks.join(","))
})

test("目录导入：dry-run 一个字节都不写", function () {
  const root = makeLearning()
  const result = importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts(), dryRun: true })
  assert.strictEqual(result.written.length, 4, "应报告将要写 4 道")
  assert.strictEqual(fs.existsSync(path.join(root, "bank")), false, "dry-run 不该建 bank 目录")
})

test("目录导入：id 冲突跳过，--force 才覆盖", function () {
  const root = makeLearning()
  const scripts = makeScripts()
  importCatalog.run({ env: {}, learningRoot: root, scriptsDir: scripts })
  const target = path.join(root, "bank", "fundamentals", "lc-00027.md")
  fs.writeFileSync(target, "改坏了", "utf8")
  const second = importCatalog.run({ env: {}, learningRoot: root, scriptsDir: scripts })
  assert.ok(
    second.skipped.some(function (p) {
      return p === target
    }),
    second.skipped.join("\n")
  )
  assert.strictEqual(fs.readFileSync(target, "utf8"), "改坏了", "没加 force 不该覆盖")
  const third = importCatalog.run({ env: {}, learningRoot: root, scriptsDir: scripts, force: true })
  assert.ok(
    third.written.some(function (p) {
      return p === target
    }),
    third.written.join("\n")
  )
  assert.ok(fs.readFileSync(target, "utf8").indexOf("id: lc-00027") !== -1, "force 后应写回题库文件")
})

test("题解导入：单题文件按文件名取题号", function () {
  const root = makeLearning()
  const result = importSolutions.run({ env: {}, learningRoot: root })
  assert.deepStrictEqual(result.errors, [], result.errors.join("\n"))
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-01455.md")), "leetcode_1455 应落成 lc-01455")
})

test("题解导入：区间文件按 // leetcode 标记切分", function () {
  const root = makeLearning()
  importSolutions.run({ env: {}, learningRoot: root })
  const target = path.join(root, "bank", "fundamentals", "lc-00056.md")
  assert.ok(fs.existsSync(target), "leetcode_1_100.c 里的 // leetcode 56 应落成 lc-00056")
  assert.ok(fs.readFileSync(target, "utf8").indexOf("int **merge") !== -1, "参考解法应带上原代码")
})

test("题解导入：专题目录认成标签与轨道", function () {
  const root = makeLearning()
  importSolutions.run({ env: {}, learningRoot: root })
  const target = path.join(root, "bank", "interview", "lc-01984.md")
  assert.ok(fs.existsSync(target), "sliding_window 专题应落成 interview 轨道")
  const parsed = parseProblemMarkdown(fs.readFileSync(target, "utf8"))
  assert.deepStrictEqual(parsed.problem.topics, ["滑动窗口"])
})

test("题解导入：README 索引里的中文题名与难度被带进来", function () {
  const root = makeLearning()
  importSolutions.run({ env: {}, learningRoot: root })
  const parsed = readProblem(root, "interview", "lc-01984").parsed
  assert.strictEqual(parsed.problem.title, "学生分数的最小差值")
  assert.strictEqual(parsed.problem.difficulty, "easy")
})

test("题解导入：参考解法写进「## 参考解法」段落", function () {
  const root = makeLearning()
  importSolutions.run({ env: {}, learningRoot: root })
  const text = fs.readFileSync(path.join(root, "bank", "interview", "lc-01984.md"), "utf8")
  assert.ok(text.indexOf("## 参考解法") !== -1, text)
  assert.ok(text.indexOf("```cpp") !== -1, text)
  assert.ok(text.indexOf("minimumDifference") !== -1, text)
})

test("题解导入：命名不合约定的文件跳过并记告警，且不建 bank/data 目录", function () {
  const root = makeLearning()
  const result = importSolutions.run({ env: {}, learningRoot: root })
  assert.ok(
    result.warnings.some(function (w) {
      return w.indexOf("interview_17.c") !== -1
    }),
    result.warnings.join("\n")
  )
  assert.ok(
    result.warnings.some(function (w) {
      return w.indexOf("433_1.cpp") !== -1
    }),
    result.warnings.join("\n")
  )
  assert.strictEqual(fs.existsSync(path.join(root, "bank", "data")), false, "不许建 bank/data 目录")
})

test("题解导入：与目录同 id 的题按 id 判冲突，不写出第二份", function () {
  const root = makeLearning()
  importCatalog.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  const result = importSolutions.run({ env: {}, learningRoot: root })
  assert.ok(
    result.skipped.some(function (item) {
      return item.indexOf("lc-00070") !== -1
    }),
    "lc-00070 应被跳过：" + result.skipped.join("\n")
  )
  const sevens = listBankFiles(root).filter(function (f) {
    return path.basename(f) === "lc-00070.md"
  })
  assert.strictEqual(sevens.length, 1, "lc-00070 只应有一份，实际 " + sevens.length + "：" + sevens.join(", "))
})

test("import-bank.js：先目录后题解，一次把两路来源都导进去", function () {
  const root = makeLearning()
  const result = importBank.run({ env: {}, learningRoot: root, scriptsDir: makeScripts() })
  assert.ok(result.learningRoot, "应解析出 learning 根目录")
  assert.strictEqual(result.catalog.written.length, 4, "目录来源应写 4 道")
  assert.strictEqual(result.solutions.written.length, 3, "题解来源应写 3 道（lc-00070 与目录同 id，被跳过）")
  const files = listBankFiles(root)
  assert.strictEqual(files.length, 7, "两路来源合计 7 个题库文件")
  files.forEach(function (file) {
    const parsed = parseProblemMarkdown(fs.readFileSync(file, "utf8"))
    assert.ok(isValidId(parsed.problem.id), "id 非法：" + parsed.problem.id)
    assert.ok(
      parsed.problem.difficulty === "easy" || parsed.problem.difficulty === "medium",
      "难度只能 easy/medium：" + parsed.problem.difficulty
    )
    assert.ok(
      ["interview", "fundamentals", "advanced", "engineering"].indexOf(parsed.problem.tracks[0]) !== -1,
      "轨道非法：" + parsed.problem.tracks[0]
    )
  })
})
