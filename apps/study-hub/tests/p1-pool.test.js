"use strict"
/**
 * p1-pool.test.js —— 候选池与到期集合
 *
 * 候选池口径（冻结）：status === "verified" ∧（未完成 ∨ 已到期）。
 * draft / rejected / todo 一律不进池。
 */

const test = require("node:test")
const assert = require("node:assert")

const { collectDue, collectPools, resolveUnitId } = require("../core/scheduler/pool.js")

function unit(id, track, status) {
  return {
    id: id,
    track: track,
    kind: "problem",
    title: id,
    status: status || "verified",
    difficulty: "medium",
    topics: [],
    subtracks: [],
  }
}

test("只收 verified 且没做完的单元", function () {
  const r = collectPools({
    units: [
      unit("algo:lc-00001", "algorithm", "verified"),
      unit("algo:lc-00002", "algorithm", "draft"),
      unit("algo:lc-00003", "algorithm", "rejected"),
      unit("algo:lc-00004", "algorithm", "todo"),
    ],
    tracks: ["algorithm"],
  })
  assert.deepStrictEqual(
    r.pools.algorithm.map(function (u) {
      return u.id
    }),
    ["algo:lc-00001"]
  )
  assert.strictEqual(r.counts.algorithm.candidates, 1)
  assert.strictEqual(r.counts.algorithm.due, 0)
})

test("做完了的单元不进池，除非它到期了", function () {
  const units = [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")]
  const done = collectPools({
    units: units,
    progressByUnit: { "algo:lc-00001": "done" },
    tracks: ["algorithm"],
  })
  assert.deepStrictEqual(
    done.pools.algorithm.map(function (u) {
      return u.id
    }),
    ["algo:lc-00002"]
  )

  const due = collectPools({
    units: units,
    progressByUnit: { "algo:lc-00001": "done" },
    dueUnitIds: new Set(["algo:lc-00001"]),
    tracks: ["algorithm"],
  })
  assert.deepStrictEqual(
    due.pools.algorithm.map(function (u) {
      return u.id
    }),
    ["algo:lc-00001", "algo:lc-00002"]
  )
  assert.strictEqual(due.counts.algorithm.candidates, 2)
  assert.strictEqual(due.counts.algorithm.due, 1)
})

test("in-progress 算未完成，照样进池", function () {
  const r = collectPools({
    units: [unit("algo:lc-00001", "algorithm")],
    progressByUnit: { "algo:lc-00001": "in-progress" },
    tracks: ["algorithm"],
  })
  assert.strictEqual(r.counts.algorithm.candidates, 1)
})

test("三轨分开统计，没单元的轨也要有零计数", function () {
  const r = collectPools({
    units: [unit("algo:lc-00001", "algorithm"), unit("db:mvcc", "database")],
  })
  assert.strictEqual(r.counts.algorithm.candidates, 1)
  assert.strictEqual(r.counts.database.candidates, 1)
  assert.strictEqual(r.counts.ai.candidates, 0)
  assert.deepStrictEqual(r.pools.ai, [])
})

test("到期判定：日期通道或次数通道任一成立", function () {
  const cards = [
    { unit_id: "algo:lc-00001", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { unit_id: "algo:lc-00002", next_review_at: "2099-01-01T00:00:00.000Z", units_since: 5 },
    { unit_id: "algo:lc-00003", next_review_at: "2099-01-01T00:00:00.000Z", units_since: 1 },
  ]
  const byId = {
    "algo:lc-00001": unit("algo:lc-00001", "algorithm"),
    "algo:lc-00002": unit("algo:lc-00002", "algorithm"),
    "algo:lc-00003": unit("algo:lc-00003", "algorithm"),
  }
  const r = collectDue(cards, byId, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.deepStrictEqual(
    Array.from(r.dueUnitIds).sort(),
    ["algo:lc-00001", "algo:lc-00002"]
  )
  assert.strictEqual(r.dueByTrack.algorithm, 2)
})

test("卡上没有 unit_id 时按旧的 problem_id 补 algo: 前缀", function () {
  const cards = [
    { problem_id: "lc-00001", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
  ]
  const byId = { "algo:lc-00001": unit("algo:lc-00001", "algorithm") }
  const r = collectDue(cards, byId, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.ok(r.dueUnitIds.has("algo:lc-00001"))
})

test("卡指向的单元已经不在库里：记进 unknownUnitIds，不进池", function () {
  const cards = [
    { unit_id: "algo:lc-99999", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
  ]
  const r = collectDue(cards, {}, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.deepStrictEqual(r.unknownUnitIds, ["algo:lc-99999"])
  assert.strictEqual(r.dueUnitIds.size, 0)
})

test("unknownUnitIds 去重：同一张卡的 unit_id 与 problem_id 都指向它的情形只能记一次", function () {
  // 一张卡同时有 unit_id 与 problem_id 时 resolveUnitId 只取 unit_id，
  // 所以「同一单元被记两次」只能出现在**两张卡**指向同一个失联单元时。
  // 不去重的话，`dueByTrack` 之外的诊断列表会重复刷屏（P1 的 notice 会直接展示它）。
  const cards = [
    { unit_id: "algo:lc-99999", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { problem_id: "lc-99999", next_review_at: "2021-01-01T00:00:00.000Z", units_since: 0 },
  ]
  const r = collectDue(cards, {}, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.deepStrictEqual(r.unknownUnitIds, ["algo:lc-99999"], "同一个单元只许记一次：" + r.unknownUnitIds.join(","))
})

test("到期的卡按轨计数，不同轨分开累加", function () {
  // 上一条只验了单轨 algo 的计数。多轨时 dueByTrack 要按键分开累加，
  // 写成「increment 同一个变量」或「用最后一条的轨覆盖」都会在这里露馅。
  const cards = [
    { unit_id: "algo:lc-00001", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { unit_id: "algo:lc-00002", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { unit_id: "db:mvcc", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { unit_id: "ai:cnn", next_review_at: "2099-01-01T00:00:00.000Z", units_since: 0 },
  ]
  const byId = {
    "algo:lc-00001": unit("algo:lc-00001", "algorithm"),
    "algo:lc-00002": unit("algo:lc-00002", "algorithm"),
    "db:mvcc": unit("db:mvcc", "database"),
    "ai:cnn": unit("ai:cnn", "ai"),
  }
  const r = collectDue(cards, byId, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.deepStrictEqual(r.dueByTrack, { algorithm: 2, database: 1 })
  assert.strictEqual(r.dueUnitIds.size, 3)
})

test("dueByTrack 按单元去重：同一单元有两张到期卡时只算一次", function () {
  // 契约 §6.0：**「到期卡对应的单元在候选池里只算一次」**。
  // 这条对 `dueUnitIds`（Set）自动成立，但对 `dueByTrack` 不会——
  // 它是按「卡」累加的。两张卡指向同一单元时，`dueByTrack` 会给 2，
  // 而 `collectPools().counts[track].due` 会给 1，两个数字互相矛盾。
  // 实现已改成按单元去重（先查 dueUnitIds 是否已有）。
  const cards = [
    { unit_id: "algo:lc-00001", problem_id: "lc-00001", next_review_at: "2020-01-01T00:00:00.000Z", units_since: 0 },
    { unit_id: "algo:lc-00001", problem_id: "lc-00001-old", next_review_at: "2021-01-01T00:00:00.000Z", units_since: 0 },
  ]
  const byId = { "algo:lc-00001": unit("algo:lc-00001", "algorithm") }
  const r = collectDue(cards, byId, { nowIso: "2026-01-01T00:00:00.000Z", k: 5 })
  assert.strictEqual(r.dueUnitIds.size, 1)
  assert.deepStrictEqual(r.dueByTrack, { algorithm: 1 }, "同一单元只许算一次：" + JSON.stringify(r.dueByTrack))

  // 与 collectPools 的 counts.due 必须一致（这是同一条规则的两种算法）
  const pools = collectPools({
    units: [unit("algo:lc-00001", "algorithm")],
    dueUnitIds: r.dueUnitIds,
    tracks: ["algorithm"],
  })
  assert.strictEqual(pools.counts.algorithm.due, 1)
  assert.strictEqual(pools.counts.algorithm.due, r.dueByTrack.algorithm)
})

test("没有 dueUnitIds 时也能建池", function () {
  const r = collectPools({
    units: [unit("db:mvcc", "database")],
    tracks: ["database"],
  })
  assert.strictEqual(r.counts.database.candidates, 1)
  assert.strictEqual(r.dueUnitIds.size, 0)
})

test("resolveUnitId：优先 unit_id，其次 problem_id（带前缀原样，不带前缀补 algo:）", function () {
  assert.strictEqual(resolveUnitId({ unit_id: "db:mvcc", problem_id: "lc-00001" }), "db:mvcc")
  assert.strictEqual(resolveUnitId({ problem_id: "db:mvcc" }), "db:mvcc")
  assert.strictEqual(resolveUnitId({ problem_id: "lc-00001" }), "algo:lc-00001")
  assert.strictEqual(resolveUnitId({}), "")
})
