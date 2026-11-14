"use strict"
/**
 * import-catalog.test.js —— 死脚本清单导入 learning/bank
 *
 * 夹具是真实源码片段，落进临时的 scripts 目录与临时的 learning 根目录。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const catalog = require("../tools/import-catalog.js")
const { tmpDir } = require("./helpers/tmp.js")

// 与 tests/legacy.test.js 里同一份真实片段
const GEN_EXCERPT = `
function lc(title, id, diff, tags) {
  return { title, platform: "leetcode", problemId: id, difficulty: diff,
    tags: Array.isArray(tags) ? tags : [tags] };
}
function jz(title, id, diff, tags) {
  return { title: "剑指 Offer " + id + ". " + title, platform: "nowcoder",
    problemId: "" + id, difficulty: diff, tags: Array.isArray(tags) ? tags : [tags] };
}

var arrayItems = [
  lc("两数之和", 1, "easy", ["哈希表"]),
  lc("移除元素", 27, "easy", ["双指针"]),
  lc("搜索插入位置", 35, "easy", ["二分查找"]),
  jz("数组中重复的数字", "JZ03", "easy", ["哈希表"]),
  lc("链表中倒数第k个节点", offer22, "easy", ["快慢指针"]),
];

var categories = [
  cat("array", "数组", 1, 1, "📗",
    "数组是最基本的数据结构，连续内存存储、随机访问 O(1)。", arrayItems),
];
`

const EXP_EXCERPT = `
function addProblems(catId, newItems) {
  var cat = cats.filter(function (c) { return c.id === catId; })[0];
}

addProblems("two-pointers", [
  lc("三数之和", 15, "medium", ["排序", "双指针"]),
  lc("两数之和 II", 167, "easy", ["对撞指针"]),
  jz("合并两个排序的链表", "JZ25", "easy", ["链表"]),
]);
`

const SPLIT_EXCERPT = `
var SRC = path.resolve(
  __dirname, "..", "project", "reading-radar", "data", "app", "practice-data.js",
);
var src = fs.readFileSync(SRC, "utf-8");
`

function tempDir(prefix) {
  return tmpDir(prefix)
}

/** 造一个临时 scripts 目录，把三个死脚本的片段放进去 */
function tempScriptsDir() {
  const dir = tempDir("study-hub-scripts-")
  fs.writeFileSync(path.join(dir, "gen-practice-data.js"), GEN_EXCERPT, "utf8")
  fs.writeFileSync(path.join(dir, "expand-practice.js"), EXP_EXCERPT, "utf8")
  fs.writeFileSync(path.join(dir, "split-practice-data.js"), SPLIT_EXCERPT, "utf8")
  return dir
}

test("导入器把题目写进 learning/bank/<track>/<id>.md", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  const result = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })

  assert.deepStrictEqual(result.errors, [])
  assert.strictEqual(result.written.length, 7)
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00001.md")))
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "nc-00003.md")))
  assert.ok(fs.existsSync(path.join(root, "bank", "interview", "lc-00015.md")))
  assert.ok(fs.existsSync(path.join(root, "bank", "interview", "nc-00025.md")))
  // 绝不能出现 learning/bank/data/（learning/.gitignore 忽略了 data/）
  assert.ok(!fs.existsSync(path.join(root, "bank", "data")))
})

test("落盘的题目 needs_fill 为 true，难度与标签都对", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })

  const text = fs.readFileSync(path.join(root, "bank", "fundamentals", "lc-00001.md"), "utf8")
  assert.ok(text.indexOf("needs_fill: true") !== -1, text)
  assert.ok(text.indexOf("title: 两数之和") !== -1, text)
  assert.ok(text.indexOf("difficulty: easy") !== -1, text)
  assert.ok(text.indexOf("source: leetcode") !== -1, text)
  assert.ok(text.indexOf("tracks: [fundamentals]") !== -1, text)

  const jzText = fs.readFileSync(path.join(root, "bank", "fundamentals", "nc-00003.md"), "utf8")
  assert.ok(jzText.indexOf("source: nowcoder") !== -1, jzText)
  assert.ok(jzText.indexOf("title: 剑指 Offer JZ03. 数组中重复的数字") !== -1, jzText)
})

test("题号不可用的条目被跳过并记告警，不猜题号", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  const result = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })
  assert.ok(!fs.existsSync(path.join(root, "bank", "fundamentals", "lc-00022.md")))
  const joined = result.warnings.join("\n")
  assert.ok(joined.indexOf("offer22") !== -1, joined)
})

test("没有内联清单的脚本记告警，不算错误", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  const result = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })
  assert.deepStrictEqual(result.errors, [])
  const joined = result.warnings.join("\n")
  assert.ok(joined.indexOf("split-practice-data.js") !== -1, joined)
  assert.ok(joined.indexOf("没有找到内联题目清单") !== -1, joined)
})

test("重复导入默认跳过，--force 才覆盖", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })

  const again = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root })
  assert.strictEqual(again.written.length, 0)
  assert.strictEqual(again.skipped.length, 7)

  const forced = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root, force: true })
  assert.strictEqual(forced.written.length, 7)
  assert.strictEqual(forced.skipped.length, 0)
})

test("dry-run 只报告，不落盘", function () {
  const root = tempDir("study-hub-bank-")
  const scripts = tempScriptsDir()
  const result = catalog.run({ env: {}, scriptsDir: scripts, learningRoot: root, dryRun: true })
  assert.strictEqual(result.written.length, 7)
  // dry-run 一个字节都不写，连目录都不建
  assert.ok(!fs.existsSync(path.join(root, "bank")), "dry-run 不应在 learning 仓下建出 bank/")
})

test("没设 learning 仓时导入报错而不是静默成功", function () {
  const result = catalog.run({ env: {} })
  assert.strictEqual(result.written.length, 0)
  assert.strictEqual(result.errors.length, 1)
})

test("死脚本目录不存在时记错误", function () {
  const root = tempDir("study-hub-bank-")
  const result = catalog.run({
    env: {},
    scriptsDir: path.join(root, "根本没有这个目录"),
    learningRoot: root,
  })
  assert.strictEqual(result.written.length, 0)
  assert.strictEqual(result.errors.length, 1)
})
