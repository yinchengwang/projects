"use strict"
/**
 * mastery.test.js —— 掌握度模型与四档诊断
 *
 * 公式见设计文档 §4.3：
 *   首次通过率 = firstPass / attempts
 *   遗忘因子   = 0.5 ^ (距上次练习天数 / 14)
 *   掌握度     = (0.7 × 首次通过率 + 0.3 × e^(-失误次数/2)) × 遗忘因子
 */

const test = require("node:test")
const assert = require("node:assert")

const mastery = require("../core/mastery/mastery.js")

/** 造一个知识点统计 */
function stat(overrides) {
  return Object.assign(
    {
      topic: "数组",
      attempts: 0,
      firstPass: 0,
      lapses: 0,
      lastAt: null,
      problems: 0,
    },
    overrides || {}
  )
}

const NOW = "2026-11-11T00:00:00.000Z"

test("常量按契约固定", function () {
  assert.strictEqual(mastery.HALF_LIFE_DAYS, 14)
  assert.strictEqual(mastery.MASTERY_THRESHOLD, 0.5)
  assert.strictEqual(mastery.COVERAGE_THRESHOLD, 0.5)
})

test("没练过的知识点掌握度为 0", function () {
  assert.strictEqual(mastery.masteryOf(stat(), NOW), 0)
})

test("全一次通过且刚练过，掌握度为 1", function () {
  const s = mastery.masteryOf(
    stat({ attempts: 4, firstPass: 4, lapses: 0, lastAt: NOW, problems: 4 }),
    NOW
  )
  assert.ok(Math.abs(s - 1) < 1e-9, "实际 " + s)
})

test("首次通过率按 firstPass/attempts 算，反复重试通过不算掌握", function () {
  // 4 次提交、只有 1 次是「首次尝试就通过」→ 首次通过率 0.25
  const s = mastery.masteryOf(
    stat({ attempts: 4, firstPass: 1, lapses: 0, lastAt: NOW, problems: 1 }),
    NOW
  )
  // (0.7 × 0.25 + 0.3 × 1) × 1 = 0.475
  assert.ok(Math.abs(s - 0.475) < 1e-9, "实际 " + s)
})

test("遗忘因子按半衰期 14 天衰减", function () {
  const base = stat({ attempts: 4, firstPass: 4, lapses: 0, problems: 4 })
  const fresh = mastery.masteryOf(Object.assign({}, base, { lastAt: NOW }), NOW)
  const half = mastery.masteryOf(
    Object.assign({}, base, { lastAt: "2026-10-28T00:00:00.000Z" }),
    NOW
  )
  assert.ok(Math.abs(half / fresh - 0.5) < 1e-9, "14 天后应剩一半，实际 " + half / fresh)
})

test("30 天不练，掌握度掉到约一半以下", function () {
  const base = stat({ attempts: 4, firstPass: 4, lapses: 0, problems: 4 })
  const fresh = mastery.masteryOf(Object.assign({}, base, { lastAt: NOW }), NOW)
  const old = mastery.masteryOf(
    Object.assign({}, base, { lastAt: "2026-10-12T00:00:00.000Z" }),
    NOW
  )
  assert.ok(old < fresh, "旧练习的掌握度应更低")
  assert.ok(old / fresh < 0.25, "30 天后应衰减到约 1/4 强，实际 " + old / fresh)
})

test("失误会拉低掌握度", function () {
  const a = mastery.masteryOf(stat({ attempts: 4, firstPass: 4, lapses: 0, lastAt: NOW }), NOW)
  const b = mastery.masteryOf(stat({ attempts: 4, firstPass: 4, lapses: 3, lastAt: NOW }), NOW)
  assert.ok(b < a, "失误多的时候掌握度应更低")
})

test("时间倒流或 lastAt 为空时不衰减", function () {
  const future = mastery.masteryOf(
    stat({ attempts: 2, firstPass: 2, lapses: 0, lastAt: "2027-01-01T00:00:00.000Z" }),
    NOW
  )
  const none = mastery.masteryOf(stat({ attempts: 2, firstPass: 2, lapses: 0, lastAt: null }), NOW)
  assert.strictEqual(future, 1)
  assert.strictEqual(none, 1)
})

test("非法 stat 不抛错，掌握度落在 0~1", function () {
  assert.strictEqual(mastery.masteryOf(null, NOW), 0)
  assert.strictEqual(mastery.masteryOf(undefined, NOW), 0)
  const s = mastery.masteryOf(stat({ attempts: 10, firstPass: 99, lapses: -3, lastAt: "坏时间" }), NOW)
  assert.ok(s >= 0 && s <= 1, "实际 " + s)
})

test("覆盖度用练过的不同题目数除以期望题量", function () {
  assert.strictEqual(mastery.coverageOf(stat({ problems: 6 }), 12), 0.5)
  assert.strictEqual(mastery.coverageOf(stat({ problems: 20 }), 12), 1)
  assert.strictEqual(mastery.coverageOf(stat({ problems: 0 }), 12), 0)
})

test("覆盖度没有 problems 字段时退回用 attempts", function () {
  assert.strictEqual(mastery.coverageOf({ attempts: 3 }, 6), 0.5)
})

test("期望题量非法时覆盖度为 0，不抛错", function () {
  assert.strictEqual(mastery.coverageOf(stat({ problems: 3 }), 0), 0)
  assert.strictEqual(mastery.coverageOf(stat({ problems: 3 }), -1), 0)
  assert.strictEqual(mastery.coverageOf(stat({ problems: 3 }), null), 0)
})

test("四档诊断判定", function () {
  assert.strictEqual(mastery.diagnose(0.2, 0.2), "weak-sparse")
  assert.strictEqual(mastery.diagnose(0.2, 0.8), "weak-dense")
  assert.strictEqual(mastery.diagnose(0.9, 0.2), "strong-sparse")
  assert.strictEqual(mastery.diagnose(0.9, 0.8), "strong-dense")
})

test("阈值正好在 0.5 时算「高」", function () {
  assert.strictEqual(mastery.diagnose(0.5, 0.5), "strong-dense")
  assert.strictEqual(mastery.diagnose(0.4999, 0.5), "weak-dense")
})

test("四档中文标签", function () {
  assert.strictEqual(mastery.diagnoseLabel("weak-sparse"), "薄弱且练得少")
  assert.strictEqual(mastery.diagnoseLabel("weak-dense"), "练过但总错")
  assert.strictEqual(mastery.diagnoseLabel("strong-sparse"), "偶然会，没练透")
  assert.strictEqual(mastery.diagnoseLabel("strong-dense"), "掌握")
})

test("未知档位给中文兜底文案", function () {
  assert.strictEqual(mastery.diagnoseLabel("nope"), "未知档位")
})

test("四档的建议动作都能落到设计文档 §4.3 的处方上", function () {
  assert.ok(mastery.diagnoseAction("weak-sparse").indexOf("优先排") !== -1)
  assert.ok(mastery.diagnoseAction("weak-dense").indexOf("看概念") !== -1)
  assert.ok(mastery.diagnoseAction("strong-sparse").indexOf("复习") !== -1)
  assert.ok(mastery.diagnoseAction("strong-dense").indexOf("不再主动排") !== -1)
})

test("rankTopics 按掌握度升序，并带上档位与标签", function () {
  const stats = [
    stat({ topic: "强", attempts: 5, firstPass: 5, lapses: 0, lastAt: NOW, problems: 5 }),
    stat({ topic: "弱", attempts: 5, firstPass: 1, lapses: 3, lastAt: NOW, problems: 2 }),
  ]
  const ranked = mastery.rankTopics(stats, NOW, { 强: 5, 弱: 5 })
  assert.strictEqual(ranked.length, 2)
  assert.strictEqual(ranked[0].topic, "弱")
  assert.strictEqual(ranked[1].topic, "强")
  assert.strictEqual(ranked[0].label, mastery.diagnoseLabel(ranked[0].bucket))
  assert.ok("mastery" in ranked[0] && "coverage" in ranked[0])
})

test("rankTopics 缺期望题量时覆盖度为 0，不抛错", function () {
  const ranked = mastery.rankTopics([stat({ topic: "数组", attempts: 2, firstPass: 2, problems: 2 })], NOW, {})
  assert.strictEqual(ranked[0].coverage, 0)
})

test("sortByUrgency 把「练过但总错」排在最前", function () {
  const items = [
    { topic: "a", bucket: "strong-dense", mastery: 0.9 },
    { topic: "b", bucket: "weak-sparse", mastery: 0.1 },
    { topic: "c", bucket: "weak-dense", mastery: 0.2 },
    { topic: "d", bucket: "strong-sparse", mastery: 0.8 },
  ]
  const sorted = mastery.sortByUrgency(items)
  assert.deepStrictEqual(
    sorted.map(function (x) { return x.topic }),
    ["c", "b", "d", "a"]
  )
})

test("sortByUrgency 不改原数组", function () {
  const items = [
    { topic: "a", bucket: "strong-dense", mastery: 0.9 },
    { topic: "b", bucket: "weak-dense", mastery: 0.2 },
  ]
  const copy = items.slice()
  mastery.sortByUrgency(items)
  assert.deepStrictEqual(items, copy)
})
