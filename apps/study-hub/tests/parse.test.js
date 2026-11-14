"use strict"
/**
 * parse.test.js —— markdown ↔ Problem 双向转换
 */

const test = require("node:test")
const assert = require("node:assert")

const parse = require("../core/bank/parse.js")

const SAMPLE = [
  "---",
  "id: lc-00001",
  "source: leetcode",
  "source_id: 1",
  "title: 两数之和",
  "difficulty: easy",
  "topics: [哈希表, 数组]",
  "tracks: [interview, fundamentals]",
  "status: todo",
  "needs_fill: false",
  "url: https://leetcode.cn/problems/two-sum/",
  "created: 2026-11-11",
  "updated: 2026-11-11",
  "---",
  "## 题面",
  "",
  "给定整数数组与目标值。",
  "",
  "## 测试用例",
  "",
  "| input | expect |",
  "|---|---|",
  "| [2,7,11,15], 9 | [0,1] |",
  "| 3,2,4, 6 | 1,2 | tol=1e-6 |",
  "",
  "## 参考解法",
  "",
  "```cpp",
  "int main() { return 0; }",
  "```",
  "",
].join("\n")

test("parseFrontmatter 解析标量、数组与空数组", function () {
  const fm = parse.parseFrontmatter(
    ["id: lc-00001", "title: 两数之和", "topics: [数组, 哈希表]", "links: []", "flag: true"].join("\n")
  )
  assert.strictEqual(fm.id, "lc-00001")
  assert.strictEqual(fm.title, "两数之和")
  assert.deepStrictEqual(fm.topics, ["数组", "哈希表"])
  assert.deepStrictEqual(fm.links, [])
  assert.strictEqual(fm.flag, "true")
})

test("parseFrontmatter 去掉引号", function () {
  const fm = parse.parseFrontmatter('title: "两数之和"')
  assert.strictEqual(fm.title, "两数之和")
})

test("parseFrontmatter 解析块列表", function () {
  const fm = parse.parseFrontmatter(["topics:", "  - 数组", "  - 哈希表"].join("\n"))
  assert.deepStrictEqual(fm.topics, ["数组", "哈希表"])
})

test("serializeFrontmatter 数组与布尔", function () {
  const text = parse.serializeFrontmatter({ id: "lc-00001", topics: ["数组"], needs_fill: true })
  assert.ok(text.indexOf("topics: [数组]") !== -1, text)
  assert.ok(text.indexOf("needs_fill: true") !== -1, text)
})

test("parseCases 解析用例表与容差", function () {
  const cases = parse.parseCases(
    ["| input | expect |", "|---|---|", "| a | b |", "| c | d | tol=1e-6 |"].join("\n")
  )
  assert.strictEqual(cases.length, 2)
  assert.deepStrictEqual(cases[0], { input: "a", expect: "b", tol: null })
  assert.strictEqual(cases[1].tol, 1e-6)
})

test("parseCases 认不出列就跳过，不报错", function () {
  const cases = parse.parseCases(["| a |", "|---|---|", "| 只有一列 |"].join("\n"))
  assert.strictEqual(cases.length, 0)
})

test("parseSolution 取代码块与语言", function () {
  const out = parse.parseSolution("```cpp\nint main() { return 0; }\n```")
  assert.strictEqual(out.lang, "cpp")
  assert.strictEqual(out.code, "int main() { return 0; }")
})

test("parseProblemMarkdown 解析整篇", function () {
  const result = parse.parseProblemMarkdown(SAMPLE)
  const p = result.problem
  assert.strictEqual(p.id, "lc-00001")
  assert.strictEqual(p.source, "leetcode")
  assert.strictEqual(p.sourceId, "1")
  assert.strictEqual(p.url, "https://leetcode.cn/problems/two-sum/")
  assert.strictEqual(p.title, "两数之和")
  assert.strictEqual(p.difficulty, "easy")
  assert.deepStrictEqual(p.topics, ["哈希表", "数组"])
  assert.deepStrictEqual(p.tracks, ["interview", "fundamentals"])
  assert.strictEqual(p.status, "todo")
  assert.strictEqual(p.needsFill, false)
  assert.strictEqual(p.statement, "给定整数数组与目标值。")
  assert.strictEqual(p.cases.length, 2)
  assert.strictEqual(p.cases[1].tol, 1e-6)
  assert.strictEqual(p.solution, "int main() { return 0; }")
  assert.strictEqual(p.solutionLang, "cpp")
  assert.deepStrictEqual(result.warnings, [])
})

test("parseProblemMarkdown 缺 frontmatter 时给告警而不是抛错", function () {
  const result = parse.parseProblemMarkdown("## 题面\n\n只有正文")
  assert.strictEqual(result.problem.id, "")
  assert.ok(result.warnings.length > 0)
})

test("parseProblemMarkdown 保留未识别的小节", function () {
  const text = SAMPLE + "\n## 备注\n\n一些补充说明\n"
  const result = parse.parseProblemMarkdown(text)
  assert.strictEqual(result.problem.extraSections.length, 1)
  assert.strictEqual(result.problem.extraSections[0].title, "备注")
})

test("Problem → markdown → Problem 往返等价", function () {
  const first = parse.parseProblemMarkdown(SAMPLE).problem
  const text = parse.serializeProblemMarkdown(first)
  const second = parse.parseProblemMarkdown(text).problem
  assert.deepStrictEqual(second, first)
})

test("source_id 与 url 为空时整行省掉，不留空值噪声", function () {
  const p = parse.parseProblemMarkdown(SAMPLE).problem
  p.sourceId = ""
  p.url = ""
  const text = parse.serializeProblemMarkdown(p)
  assert.strictEqual(text.indexOf("source_id"), -1, text)
  assert.strictEqual(text.indexOf("url:"), -1, text)
  const back = parse.parseProblemMarkdown(text).problem
  assert.strictEqual(back.sourceId, "")
  assert.strictEqual(back.url, "")
})
