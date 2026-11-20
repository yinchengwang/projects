"use strict"
/**
 * topic-stats.test.js —— 从作答流水派生知识点统计
 *
 * 口径见设计文档 §4.3：
 *   attempts  = 提交总次数
 *   firstPass = 第一次尝试就通过的单元数（反复重试通过不算）
 *   lapses    = 没通过的提交次数
 *   unitCount = 练过的不同单元数
 *
 * P3 起知识点带轨，行里多三份信息：id（复合键）/ track / topic（裸名）。
 */

const test = require("node:test")
const assert = require("node:assert")

const topicStats = require("../core/mastery/topic-stats.js")

test("deriveTopicStats 统计每个 topic 的提交数、首次通过数与失误数", function () {
  const stats = topicStats.deriveTopicStats(
    [
      { id: 1, problem_id: "lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" },
      { id: 2, problem_id: "lc-00001", ts: "2026-11-02T10:00:00.000Z", verdict: "wrong_answer" },
      { id: 3, problem_id: "lc-00002", ts: "2026-11-02T11:00:00.000Z", verdict: "wrong_answer" },
      { id: 4, problem_id: "lc-00002", ts: "2026-11-03T11:00:00.000Z", verdict: "pass" },
    ],
    [
      { id: "lc-00001", topics: ["数组"] },
      { id: "lc-00002", topics: ["数组", "哈希表"] },
    ]
  )

  const array = stats.find(function (s) { return s.topic === "数组" })
  assert.strictEqual(array.attempts, 4)
  assert.strictEqual(array.firstPass, 1)
  assert.strictEqual(array.lapses, 2)
  assert.strictEqual(array.unitCount, 2)
  assert.strictEqual(array.lastAt, "2026-11-03T11:00:00.000Z")

  const hash = stats.find(function (s) { return s.topic === "哈希表" })
  assert.strictEqual(hash.attempts, 2)
  assert.strictEqual(hash.firstPass, 0)
  assert.strictEqual(hash.lapses, 1)
  assert.strictEqual(hash.unitCount, 1)
})

test("deriveTopicStats 题目首次尝试通过才算首次通过", function () {
  const stats = topicStats.deriveTopicStats(
    [
      { id: 1, problem_id: "lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "wrong_answer" },
      { id: 2, problem_id: "lc-00001", ts: "2026-11-02T10:00:00.000Z", verdict: "pass" },
    ],
    [{ id: "lc-00001", topics: ["数组"] }]
  )
  assert.strictEqual(stats[0].firstPass, 0)
  assert.strictEqual(stats[0].lapses, 1)
  assert.strictEqual(stats[0].attempts, 2)
})

test("deriveTopicStats 题库里查不到的题目不计入", function () {
  const stats = topicStats.deriveTopicStats(
    [{ id: 1, problem_id: "lc-99999", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" }],
    [{ id: "lc-00001", topics: ["数组"] }]
  )
  assert.deepStrictEqual(stats, [])
})

test("deriveTopicStats 一道题挂多个 topic 时两边都算", function () {
  const stats = topicStats.deriveTopicStats(
    [{ id: 1, problem_id: "lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" }],
    [{ id: "lc-00001", topics: ["数组", "哈希表"] }]
  )
  assert.strictEqual(stats.length, 2)
  stats.forEach(function (s) {
    assert.strictEqual(s.attempts, 1)
    assert.strictEqual(s.unitCount, 1)
  })
})

test("deriveTopicStats 空输入返回空数组", function () {
  assert.deepStrictEqual(topicStats.deriveTopicStats([], []), [])
  assert.deepStrictEqual(topicStats.deriveTopicStats(undefined, undefined), [])
})

test("deriveTopicStats 结果按 topic 名排序", function () {
  const stats = topicStats.deriveTopicStats(
    [{ id: 1, problem_id: "lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" }],
    [{ id: "lc-00001", topics: ["数组", "哈希表"] }]
  )
  const names = stats.map(function (s) { return s.topic })
  assert.deepStrictEqual(names, names.slice().sort())
})

/* ---------------- P3 Task4：复合键与轨 ---------------- */

test("deriveTopicStats 的行带复合键 id 与 track", function () {
  const stats = topicStats.deriveTopicStats(
    [{ id: 1, unit_id: "algo:lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" }],
    [{ id: "algo:lc-00001", track: "algorithm", topics: ["数组"] }]
  )
  assert.strictEqual(stats.length, 1)
  assert.strictEqual(stats[0].id, "algorithm:数组")
  assert.strictEqual(stats[0].track, "algorithm")
  assert.strictEqual(stats[0].topic, "数组")
  assert.strictEqual(stats[0].unitCount, 1)
})

test("deriveTopicStats 按单元归一 id：裸 id 与带前缀 id 归到同一个单元", function () {
  const stats = topicStats.deriveTopicStats(
    [{ id: 1, problem_id: "lc-00001", ts: "2026-11-01T10:00:00.000Z", verdict: "pass" }],
    [{ id: "algo:lc-00001", track: "algorithm", topics: ["数组"] }]
  )
  assert.strictEqual(stats.length, 1)
  assert.strictEqual(stats[0].attempts, 1)
  assert.strictEqual(stats[0].unitCount, 1)
})
