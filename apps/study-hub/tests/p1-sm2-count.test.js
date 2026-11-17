"use strict"
/**
 * p1-sm2-count.test.js —— SM-2 的次数通道
 *
 * 触发条件多一条（units_since >= K），公式一个字不动。
 */

const test = require("node:test")
const assert = require("node:assert")

const sm2 = require("../core/review/sm2.js")

test("isDueByCount：units_since 达到 K 就算到期", function () {
  assert.strictEqual(sm2.isDueByCount({ units_since: 4 }, 5), false)
  assert.strictEqual(sm2.isDueByCount({ units_since: 5 }, 5), true)
  assert.strictEqual(sm2.isDueByCount({ units_since: 9 }, 5), true)
})

test("isDueByCount：没有 units_since 或 K 不合法时不算到期", function () {
  assert.strictEqual(sm2.isDueByCount({}, 5), false)
  assert.strictEqual(sm2.isDueByCount({ units_since: 9 }, 0), false)
  assert.strictEqual(sm2.isDueByCount({ units_since: 9 }, -1), false)
})

test("bumpUnitsSince：所有卡 +1，返回新数组，不改原数组", function () {
  const cards = [
    { problem_id: "lc-00001", units_since: 0 },
    { problem_id: "lc-00002", units_since: 3 },
  ]
  const out = sm2.bumpUnitsSince(cards)
  assert.deepStrictEqual(
    out.map(function (c) {
      return c.units_since
    }),
    [1, 4]
  )
  assert.deepStrictEqual(
    cards.map(function (c) {
      return c.units_since
    }),
    [0, 3]
  )
  assert.notStrictEqual(out[0], cards[0])
})

test("bumpUnitsSince：步长可以指定，缺省是 1", function () {
  const cards = [{ problem_id: "lc-00001", units_since: 2 }]
  assert.strictEqual(sm2.bumpUnitsSince(cards, 3)[0].units_since, 5)
})

test("gradeWithCount：跑完 SM-2 更新，units_since 归零，unit_id 留着", function () {
  const card = {
    problem_id: "lc-00007",
    unit_id: "algo:lc-00007",
    easiness: 2.5,
    interval_days: 1,
    repetitions: 1,
    units_since: 6,
    next_review_at: "2026-01-01T00:00:00.000Z",
    lapses: 0,
  }
  const graded = sm2.gradeWithCount(card, 4, { nowIso: "2026-01-01T00:00:00.000Z" })
  assert.strictEqual(graded.units_since, 0)
  assert.strictEqual(graded.unit_id, "algo:lc-00007")
  // 公式没动：quality=4、repetitions 1 → interval=6、repetitions=2、间隔加 6 个日历日
  assert.strictEqual(graded.repetitions, 2)
  assert.strictEqual(graded.interval_days, 6)
  assert.strictEqual(graded.next_review_at, "2026-01-07T00:00:00.000Z")
})

test("gradeWithCount：没有 unit_id 时按 problem_id 补 algo: 前缀", function () {
  const graded = sm2.gradeWithCount(
    { problem_id: "lc-00008", units_since: 5 },
    3,
    { nowIso: "2026-01-01T00:00:00.000Z" }
  )
  assert.strictEqual(graded.unit_id, "algo:lc-00008")
  assert.strictEqual(graded.units_since, 0)
})

test("gradeWithCount：problem_id 已带前缀时原样用，不补 algo:", function () {
  const graded = sm2.gradeWithCount(
    { problem_id: "db:mvcc", units_since: 2 },
    3,
    { nowIso: "2026-01-01T00:00:00.000Z" }
  )
  assert.strictEqual(graded.unit_id, "db:mvcc")
})

test("isDueEither：日期通道或次数通道任一成立就算到期", function () {
  const byCount = { problem_id: "lc-00001", next_review_at: "2099-01-01T00:00:00.000Z", units_since: 5 }
  const byDate = { problem_id: "lc-00002", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 }
  const neither = { problem_id: "lc-00003", next_review_at: "2099-01-01T00:00:00.000Z", units_since: 1 }
  assert.strictEqual(sm2.isDueEither(byCount, "2026-01-01T00:00:00.000Z", 5), true)
  assert.strictEqual(sm2.isDueEither(byDate, "2026-01-01T00:00:00.000Z", 5), true)
  assert.strictEqual(sm2.isDueEither(neither, "2026-01-01T00:00:00.000Z", 5), false)
})

test("老导出还在，公式常量没变", function () {
  assert.strictEqual(typeof sm2.grade, "function")
  assert.strictEqual(typeof sm2.isDue, "function")
  assert.strictEqual(typeof sm2.sortByOverdue, "function")
  assert.strictEqual(typeof sm2.qualityFromOutcome, "function")
  assert.strictEqual(sm2.DEFAULT_EASE, 2.5)
  assert.strictEqual(sm2.MIN_EASE, 1.3)
})
