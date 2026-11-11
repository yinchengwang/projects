"use strict"
/**
 * problem.test.js —— Problem 模型：ID、topic 归一化、字段校验
 */

const test = require("node:test")
const assert = require("node:assert")

const problem = require("../core/bank/problem.js")

test("makeId 生成 5 位补零 ID", function () {
  assert.strictEqual(problem.makeId("leetcode", 1), "lc-00001")
  assert.strictEqual(problem.makeId("leetcode", 99999), "lc-99999")
  assert.strictEqual(problem.makeId("nowcoder", 12), "nc-00012")
  assert.strictEqual(problem.makeId("self", 7), "self-00007")
})

test("makeId 拒绝未知来源与非数字题号", function () {
  assert.throws(function () {
    problem.makeId("unknown", 1)
  })
  assert.throws(function () {
    problem.makeId("leetcode", "abc")
  })
})

test("makeLlmId 稳定且可复现", function () {
  const a = problem.makeLlmId("两数之和")
  const b = problem.makeLlmId("两数之和")
  assert.strictEqual(a, b)
  assert.ok(problem.isValidId(a), a + " 应是合法 ID")
  assert.notStrictEqual(a, problem.makeLlmId("三数之和"))
})

test("isValidId 只认已知前缀", function () {
  assert.ok(problem.isValidId("lc-00001"))
  assert.ok(problem.isValidId("llm-1a2b3c4d5e"))
  assert.ok(problem.isValidId("self-00007"))
  assert.ok(!problem.isValidId("xx-00001"))
  assert.ok(!problem.isValidId("lc-1"))
  assert.ok(!problem.isValidId(""))
})

test("normalizeTopics 去空白、去重、保序", function () {
  const out = problem.normalizeTopics([" 哈希表 ", "数组", "哈希表", "", "  "])
  assert.deepStrictEqual(out, ["哈希表", "数组"])
})

test("normalizeTopics 接受单个字符串", function () {
  assert.deepStrictEqual(problem.normalizeTopics("数组"), ["数组"])
})

test("normalizeTracks 丢掉非法轨道", function () {
  const out = problem.normalizeTracks(["interview", "不存在的轨道", "advanced"])
  assert.deepStrictEqual(out, ["interview", "advanced"])
})

test("validateProblem 报出具体错误", function () {
  const result = problem.validateProblem({
    id: "bad",
    source: "leetcode",
    title: "",
    difficulty: "hard",
    topics: [],
    tracks: ["interview"],
    status: "todo",
  })
  assert.strictEqual(result.ok, false)
  assert.ok(result.errors.some(function (e) { return e.indexOf("id 非法") === 0 }))
  assert.ok(result.errors.some(function (e) { return e.indexOf("title 为空") === 0 }))
  assert.ok(result.errors.some(function (e) { return e.indexOf("topics") === 0 }))
})

test("validateProblem 接受合法题目", function () {
  const result = problem.validateProblem({
    id: "lc-00001",
    source: "leetcode",
    title: "两数之和",
    difficulty: "easy",
    topics: ["数组"],
    tracks: ["interview"],
    status: "todo",
  })
  assert.deepStrictEqual(result.errors, [])
  assert.strictEqual(result.ok, true)
})

test("createProblem 填默认值并归一化", function () {
  const created = problem.createProblem({
    id: "lc-00002",
    source: "leetcode",
    title: "  三数之和  ",
    topics: [" 数组 ", "数组"],
    tracks: ["interview"],
  })
  assert.strictEqual(created.title, "三数之和")
  assert.deepStrictEqual(created.topics, ["数组"])
  assert.strictEqual(created.difficulty, "medium")
  assert.strictEqual(created.status, "todo")
  assert.strictEqual(created.needsFill, false)
  assert.ok(/^\d{4}-\d{2}-\d{2}$/.test(created.created), created.created)
})

test("createProblem 字段非法时抛错", function () {
  assert.throws(function () {
    problem.createProblem({ id: "x", source: "leetcode", title: "t", topics: ["a"], tracks: ["interview"] })
  })
})

test("toSummary 不带题面与解法", function () {
  const created = problem.createProblem({
    id: "lc-00003",
    source: "leetcode",
    title: "题目",
    topics: ["数组"],
    tracks: ["interview"],
    statement: "题面正文",
    solution: "int main(){}",
  })
  const summary = problem.toSummary(created)
  assert.strictEqual(summary.id, "lc-00003")
  assert.strictEqual(summary.statement, undefined)
  assert.strictEqual(summary.solution, undefined)
})
