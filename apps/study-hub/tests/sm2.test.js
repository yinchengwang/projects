"use strict"
/**
 * sm2.test.js —— SM-2 间隔重复：公式、quality 映射、到期判断与排序
 */

const test = require("node:test")
const assert = require("node:assert")

const sm2 = require("../core/review/sm2.js")

/** 造一张卡片（snake_case，与 M1 的 review_cards 表同形） */
function card(overrides) {
  return Object.assign(
    {
      problem_id: "lc-00001",
      easiness: 2.5,
      interval_days: 1,
      repetitions: 0,
      last_quality: null,
      last_at: null,
      next_review_at: "2026-11-11T00:00:00.000Z",
      lapses: 0,
    },
    overrides || {}
  )
}

test("DEFAULT_EASE 是 2.5，下限是 1.3", function () {
  assert.strictEqual(sm2.DEFAULT_EASE, 2.5)
  assert.strictEqual(sm2.MIN_EASE, 1.3)
})

test("quality=5：repetitions 0→1，间隔保持 1，easiness 上升", function () {
  const out = sm2.grade(card(), 5, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.repetitions, 1)
  assert.strictEqual(out.interval_days, 1)
  assert.ok(out.easiness > 2.5, "easiness 应上升")
  assert.strictEqual(out.last_quality, 5)
})

test("quality=4：repetitions 1→2，间隔跳到 6", function () {
  const out = sm2.grade(card({ easiness: 2.6, repetitions: 1 }), 4, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.repetitions, 2)
  assert.strictEqual(out.interval_days, 6)
})

test("quality=5 且 repetitions=2：间隔 = round(间隔 × easiness)", function () {
  const out = sm2.grade(
    card({ easiness: 2.6, interval_days: 6, repetitions: 2 }),
    5,
    "2026-11-11T00:00:00.000Z"
  )
  assert.strictEqual(out.repetitions, 3)
  assert.strictEqual(out.interval_days, Math.round(6 * out.easiness))
})

test("quality=1（忘了）：间隔重置为 1、repetitions 归 0、lapses 加 1", function () {
  const out = sm2.grade(
    card({ easiness: 2.8, interval_days: 30, repetitions: 5, lapses: 2 }),
    1,
    "2026-11-11T00:00:00.000Z"
  )
  assert.strictEqual(out.interval_days, 1)
  assert.strictEqual(out.repetitions, 0)
  assert.strictEqual(out.lapses, 3)
})

test("quality=2 也是重置分支（间隔 1、repetitions 0）", function () {
  const out = sm2.grade(card({ interval_days: 20, repetitions: 4 }), 2, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.interval_days, 1)
  assert.strictEqual(out.repetitions, 0)
})

test("easiness 不低于 1.3", function () {
  const out = sm2.grade(card({ easiness: 1.3 }), 0, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.easiness, 1.3)
})

test("grade 保留 problem_id 并按间隔推进下次复习时间", function () {
  const out = sm2.grade(card({ problem_id: "lc-00042" }), 5, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.problem_id, "lc-00042")
  assert.strictEqual(out.last_at, "2026-11-11T00:00:00.000Z")
  assert.strictEqual(out.next_review_at, "2026-11-12T00:00:00.000Z")
})

test("grade 拒绝 0~5 以外的评分", function () {
  assert.throws(function () {
    sm2.grade(card(), 6, "2026-11-11T00:00:00.000Z")
  })
  assert.throws(function () {
    sm2.grade(card(), -1, "2026-11-11T00:00:00.000Z")
  })
  assert.throws(function () {
    sm2.grade(card(), 3.5, "2026-11-11T00:00:00.000Z")
  })
  assert.throws(function () {
    sm2.grade(card(), "好", "2026-11-11T00:00:00.000Z")
  })
})

test("grade 接受空卡片并补默认值", function () {
  const out = sm2.grade({ problem_id: "self-00001" }, 5, "2026-11-11T00:00:00.000Z")
  // 空卡片的 ease 输入补成 DEFAULT_EASE(2.5)，评分 5 后按公式抬到 2.6——
  // 这里断言的是公式算出来的结果，不是补进去的输入值。
  assert.strictEqual(out.easiness, sm2.DEFAULT_EASE + 0.1)
  assert.strictEqual(out.interval_days, 1)
  assert.strictEqual(out.repetitions, 1)
})

test("qualityFromOutcome：抄参考答案 → 0", function () {
  assert.strictEqual(
    sm2.qualityFromOutcome({ passed: true, copiedReference: true, durationMs: 10, medianMs: 100 }),
    0
  )
})

test("qualityFromOutcome：失败没改对 → 1", function () {
  assert.strictEqual(sm2.qualityFromOutcome({ passed: false }), 1)
})

test("qualityFromOutcome：失败一次后自己改对 → 2", function () {
  assert.strictEqual(sm2.qualityFromOutcome({ passed: true, failedFirst: true }), 2)
})

test("qualityFromOutcome：一次通过但参照旧解 → 3", function () {
  assert.strictEqual(sm2.qualityFromOutcome({ passed: true, usedOldSolution: true }), 3)
})

test("qualityFromOutcome：一次通过但偏慢 → 4", function () {
  assert.strictEqual(
    sm2.qualityFromOutcome({ passed: true, durationMs: 900, medianMs: 300 }),
    4
  )
})

test("qualityFromOutcome：一次通过且快于中位 → 5", function () {
  assert.strictEqual(
    sm2.qualityFromOutcome({ passed: true, durationMs: 100, medianMs: 300 }),
    5
  )
})

test("qualityFromOutcome：没有中位用时参考时按通过处理 → 5", function () {
  assert.strictEqual(sm2.qualityFromOutcome({ passed: true, durationMs: 100, medianMs: null }), 5)
})

test("isDue：到期与未到期", function () {
  assert.strictEqual(
    sm2.isDue(card({ next_review_at: "2026-11-10T00:00:00.000Z" }), "2026-11-11T00:00:00.000Z"),
    true
  )
  assert.strictEqual(
    sm2.isDue(card({ next_review_at: "2026-11-12T00:00:00.000Z" }), "2026-11-11T00:00:00.000Z"),
    false
  )
})

test("sortByOverdue：逾期最久的排最前，且不改动原数组", function () {
  const cards = [
    card({ problem_id: "b", next_review_at: "2026-11-11T00:00:00.000Z" }),
    card({ problem_id: "a", next_review_at: "2026-11-01T00:00:00.000Z" }),
    card({ problem_id: "c", next_review_at: "2026-11-10T00:00:00.000Z" }),
  ]
  const out = sm2.sortByOverdue(cards, "2026-11-11T00:00:00.000Z")
  assert.deepStrictEqual(
    out.map(function (c) {
      return c.problem_id
    }),
    ["a", "c", "b"]
  )
  assert.strictEqual(cards[0].problem_id, "b", "原数组不应被改动")
})
