"use strict"
/**
 * expected.test.js —— 「期望题量」口径
 *
 * 期望题量的口径见本计划「统计口径」一节：
 *   ① settings 的逐 topic 覆盖值 → ② 题库里该 topic 的题数 → ③ 下限常量（默认 5）
 *   最终取 max(②, ③)
 */

const test = require("node:test")
const assert = require("node:assert")

const expected = require("../core/mastery/expected.js")

test("期望题量下限默认是 5", function () {
  assert.strictEqual(expected.DEFAULT_MIN_EXPECTED, 5)
  assert.strictEqual(expected.minExpectedFrom({}), 5)
  assert.strictEqual(expected.minExpectedFrom(undefined), 5)
})

test("期望题量下限可配置", function () {
  assert.strictEqual(expected.minExpectedFrom({ "insights.expected_min_questions": "8" }), 8)
  assert.strictEqual(expected.minExpectedFrom({ "insights.expected_min_questions": 3 }), 3)
})

test("期望题量下限非法时退回默认 5", function () {
  assert.strictEqual(expected.minExpectedFrom({ "insights.expected_min_questions": "abc" }), 5)
  assert.strictEqual(expected.minExpectedFrom({ "insights.expected_min_questions": "0" }), 5)
  assert.strictEqual(expected.minExpectedFrom({ "insights.expected_min_questions": "-2" }), 5)
})

test("bankTopicCounts 数出题库里每个 topic 的题数", function () {
  const counts = expected.bankTopicCounts([
    { id: "lc-00001", topics: ["数组", "哈希表"] },
    { id: "lc-00002", topics: ["数组"] },
    { id: "lc-00003", topics: [] },
  ])
  assert.strictEqual(counts["数组"], 2)
  assert.strictEqual(counts["哈希表"], 1)
})

test("期望题量取题库题数", function () {
  const r = expected.expectedFor("数组", { "数组": 12 }, {})
  assert.strictEqual(r.expected, 12)
  assert.strictEqual(r.source, "bank")
})

test("题库题数不足下限时抬到下限", function () {
  const r = expected.expectedFor("冷门", { "冷门": 2 }, {})
  assert.strictEqual(r.expected, 5)
  assert.strictEqual(r.source, "floor")
})

test("手工覆盖值优先于题库题数", function () {
  const r = expected.expectedFor("数组", { "数组": 12 }, { "insights.expected_override.数组": "20" })
  assert.strictEqual(r.expected, 20)
  assert.strictEqual(r.source, "override")
})

test("覆盖值非法时忽略，继续走题库/下限", function () {
  const r = expected.expectedFor("数组", { "数组": 12 }, { "insights.expected_override.数组": "abc" })
  assert.strictEqual(r.expected, 12)
  assert.strictEqual(r.source, "bank")
})

test("题库里没有这个 topic 时退到下限", function () {
  const r = expected.expectedFor("没进题库", {}, {})
  assert.strictEqual(r.expected, 5)
  assert.strictEqual(r.source, "floor")
})

test("buildExpectedMap 给每个 topic 都算一份", function () {
  const map = expected.buildExpectedMap(["数组", "冷门"], { "数组": 12 }, {})
  assert.strictEqual(map["数组"].expected, 12)
  assert.strictEqual(map["数组"].source, "bank")
  assert.strictEqual(map["冷门"].expected, 5)
  assert.strictEqual(map["冷门"].source, "floor")
})
