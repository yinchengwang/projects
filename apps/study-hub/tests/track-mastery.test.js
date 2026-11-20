"use strict"
/**
 * track-mastery.test.js —— 跨轨掌握度三层聚合
 *
 * 口径见契约 §8.3.2：
 *   单元级 = masteryOf 的公式
 *   知识点级 / 轨道级 = 练过的单元的单元掌握度算术平均（空集合 = 0）
 *   六轴 = 前三根取轨道级，后三根按契约 §8.3.1 的定义取单元均值
 */

const test = require("node:test")
const assert = require("node:assert")

const tm = require("../core/mastery/track-mastery.js")
const mastery = require("../core/mastery/mastery.js")

/** 固定时刻，断言可以写死数字 */
const NOW = "2026-11-11T00:00:00.000Z"

/** 三轨各一个单元 + 一个没练过的算法单元 + 一个挂 SQL 知识点的单元 */
const UNITS = [
  {
    id: "algo:lc-00001",
    track: "algorithm",
    title: "两数之和",
    topics: ["数组"],
    subtracks: ["interview"],
    entry: { language: "cpp" },
  },
  {
    id: "algo:lc-00002",
    track: "algorithm",
    title: "合并区间",
    topics: ["数组", "SQL"],
    subtracks: ["engineering"],
    entry: { language: "cpp" },
  },
  {
    id: "algo:lc-00003",
    track: "algorithm",
    title: "爬楼梯",
    topics: [],
    subtracks: [],
    entry: { language: "cpp" },
  },
  {
    id: "db:mvcc",
    track: "database",
    title: "MVCC",
    topics: ["事务"],
    subtracks: [],
    entry: { language: "python" },
  },
  {
    id: "ai:cnn",
    track: "ai",
    title: "卷积神经网络",
    topics: [],
    subtracks: [],
    entry: { language: "python" },
  },
]

/* ---------------- deriveUnitStats / computeUnitMastery ---------------- */

test("deriveUnitStats 按单元汇总提交数、首次通过数与失误数", function () {
  const stats = tm.deriveUnitStats([
    { id: 1, unit_id: "algo:lc-00001", ts: "2026-11-10T08:00:00.000Z", verdict: "pass" },
    { id: 2, unit_id: "algo:lc-00001", ts: "2026-11-10T09:00:00.000Z", verdict: "wrong_answer" },
    { id: 3, unit_id: "algo:lc-00002", ts: "2026-11-09T08:00:00.000Z", verdict: "wrong_answer" },
    { id: 4, unit_id: "algo:lc-00002", ts: "2026-11-09T09:00:00.000Z", verdict: "pass" },
  ])

  const first = stats["algo:lc-00001"]
  assert.strictEqual(first.attempts, 2)
  assert.strictEqual(first.firstPass, 1)
  assert.strictEqual(first.lapses, 1)
  assert.strictEqual(first.lastAt, "2026-11-10T09:00:00.000Z")

  const second = stats["algo:lc-00002"]
  assert.strictEqual(second.attempts, 2)
  assert.strictEqual(second.firstPass, 0)
  assert.strictEqual(second.lapses, 1)
})

test("deriveUnitStats 按时间排序，第一次尝试没过就不算首次通过", function () {
  const stats = tm.deriveUnitStats([
    { id: 2, unit_id: "algo:lc-00001", ts: "2026-11-10T09:00:00.000Z", verdict: "pass" },
    { id: 1, unit_id: "algo:lc-00001", ts: "2026-11-10T08:00:00.000Z", verdict: "wrong_answer" },
  ])
  assert.strictEqual(stats["algo:lc-00001"].firstPass, 0)
  assert.strictEqual(stats["algo:lc-00001"].lapses, 1)
  assert.strictEqual(stats["algo:lc-00001"].attempts, 2)
})

test("deriveUnitStats 没有 unit_id 时按 problem_id 归一，裸 id 归算法轨", function () {
  const stats = tm.deriveUnitStats([
    { id: 1, problem_id: "lc-00001", ts: "2026-11-10T08:00:00.000Z", verdict: "pass" },
    { id: 2, problem_id: "algo:lc-00002", ts: "2026-11-10T08:00:00.000Z", verdict: "pass" },
  ])
  assert.ok(stats["algo:lc-00001"], "裸 id 要归算法轨")
  assert.ok(stats["algo:lc-00002"], "带前缀的 id 原样保留")
  assert.strictEqual(stats["algo:lc-00001"].attempts, 1)
})

test("computeUnitMastery 没练过的单元不出现在结果里", function () {
  const stats = tm.deriveUnitStats([
    { id: 1, unit_id: "algo:lc-00001", ts: "2026-11-10T08:00:00.000Z", verdict: "pass" },
  ])
  const values = tm.computeUnitMastery(stats, NOW)
  assert.deepStrictEqual(Object.keys(values), ["algo:lc-00001"])
})

test("computeUnitMastery 数值与 masteryOfUnit 完全一致", function () {
  const stats = tm.deriveUnitStats([
    { id: 1, unit_id: "algo:lc-00001", ts: "2026-11-10T08:00:00.000Z", verdict: "pass" },
    { id: 2, unit_id: "algo:lc-00001", ts: "2026-11-10T09:00:00.000Z", verdict: "wrong_answer" },
  ])
  const values = tm.computeUnitMastery(stats, NOW)
  assert.strictEqual(values["algo:lc-00001"], mastery.masteryOfUnit("algo:lc-00001", stats["algo:lc-00001"], NOW))
})

/* ---------------- topicRowsFor / trackRowsFor ---------------- */

test("topicRowsFor 知识点掌握度是练过单元的均值，覆盖度用练过的单元数除以期望题量", function () {
  const values = { "algo:lc-00001": 0.8, "algo:lc-00002": 0.4 }
  const expectedMap = { "algorithm:数组": { expected: 5, source: "floor" } }
  const rows = tm.topicRowsFor(UNITS, values, expectedMap)

  const array = rows.find(function (row) {
    return row.id === "algorithm:数组"
  })
  assert.ok(array, "要有 algorithm:数组 这一行")
  assert.strictEqual(array.track, "algorithm")
  assert.strictEqual(array.topic, "数组")
  assert.strictEqual(array.mastery, 0.6, "(0.8 + 0.4) / 2")
  assert.strictEqual(array.coverage, 0.4, "2 / 5")
  assert.strictEqual(array.unitCount, 2, "练过的单元数")
  assert.strictEqual(array.bucket, "strong-sparse")
  assert.strictEqual(array.label, "偶然会，没练透")
  assert.ok(array.action.length > 0, "要带中文建议")
})

test("topicRowsFor 复合键把不同轨的同名知识点分开", function () {
  const units = UNITS.concat([
    { id: "db:arr", track: "database", title: "数组索引", topics: ["数组"], entry: { language: "python" } },
  ])
  const values = { "algo:lc-00001": 0.8, "db:arr": 0.2 }
  const expectedMap = {
    "algorithm:数组": { expected: 5, source: "floor" },
    "database:数组": { expected: 5, source: "floor" },
  }
  const rows = tm.topicRowsFor(units, values, expectedMap)

  const algo = rows.find(function (row) {
    return row.id === "algorithm:数组"
  })
  const db = rows.find(function (row) {
    return row.id === "database:数组"
  })
  assert.ok(algo && db, "同名知识点要分成两行")
  assert.strictEqual(algo.mastery, 0.8)
  assert.strictEqual(db.mastery, 0.2)
  assert.strictEqual(algo.track, "algorithm")
  assert.strictEqual(db.track, "database")
})

test("trackRowsFor 三轨顺序固定，字段按契约 §6.6 齐", function () {
  const values = { "algo:lc-00001": 0.8, "algo:lc-00002": 0.4 }
  const rows = tm.trackRowsFor(UNITS, values)

  assert.deepStrictEqual(
    rows.map(function (row) {
      return row.track
    }),
    ["algorithm", "database", "ai"]
  )
  assert.deepStrictEqual(
    rows.map(function (row) {
      return row.label
    }),
    ["算法", "数据库", "AI"]
  )
  assert.strictEqual(rows[0].unitCount, 3, "算法轨全部单元数")
  assert.strictEqual(rows[0].mastery, 0.6)
  assert.ok(Math.abs(rows[0].coverage - 2 / 3) < 1e-9, "覆盖度 = 练过 2 / 全部 3")
  assert.strictEqual(rows[0].bucket, "strong-dense")
  assert.strictEqual(rows[0].label2, "掌握")
  assert.ok(rows[0].action.length > 0)
  assert.strictEqual(rows[1].mastery, 0, "没练过的轨掌握度是 0")
  assert.strictEqual(rows[1].coverage, 0)
  assert.strictEqual(rows[2].unitCount, 1)
})

test("trackRowsFor 掌握度为 0 的单元不进轨道均值", function () {
  const rows = tm.trackRowsFor(UNITS, {
    "algo:lc-00001": 0.8,
    "algo:lc-00002": 0.4,
    "algo:lc-00003": 0,
  })
  assert.strictEqual(rows[0].mastery, 0.6, "没练过的单元不进均值")
  assert.strictEqual(rows[0].unitCount, 3, "但单元总数照算")
  assert.ok(Math.abs(rows[0].coverage - 2 / 3) < 1e-9)
})

/* ---------------- radarAxesFor / weak / strong / materialize ---------------- */

test("radarAxesFor 六根轴顺序固定，c / python / sql 按契约口径", function () {
  const values = { "algo:lc-00001": 0.8, "algo:lc-00002": 0.4, "db:mvcc": 0.5 }
  const tracks = tm.trackRowsFor(UNITS, values)
  const axes = tm.radarAxesFor(tracks, UNITS, values)

  assert.deepStrictEqual(
    axes.map(function (axis) {
      return axis.id
    }),
    ["algorithm", "database", "ai", "c", "python", "sql"]
  )
  assert.deepStrictEqual(
    axes.map(function (axis) {
      return axis.label
    }),
    ["算法", "数据库", "AI", "C/C++", "Python", "SQL"]
  )
  assert.strictEqual(axes[0].value, tracks[0].mastery, "前三根轴取轨道掌握度")
  assert.strictEqual(axes[1].value, tracks[1].mastery)
  assert.strictEqual(axes[2].value, tracks[2].mastery)
  assert.strictEqual(axes[3].value, 0.6, "c 轴 = 算法轨里 cpp / engineering 单元的均值")
  assert.strictEqual(axes[4].value, 0.5, "python 轴 = language 为 python 的单元均值")
  assert.strictEqual(axes[5].value, 0.4, "sql 轴 = 知识点含 SQL 的单元均值")
})

test("weakUnitsFor 与 strongUnitsFor 只收练过的单元并按掌握度排序", function () {
  const values = { "algo:lc-00001": 0.8, "algo:lc-00002": 0.4, "db:mvcc": 0.5 }
  const weak = tm.weakUnitsFor(UNITS, values)
  assert.deepStrictEqual(
    weak.map(function (row) {
      return row.unit_id
    }),
    ["algo:lc-00002"]
  )
  assert.strictEqual(weak[0].track, "algorithm")
  assert.strictEqual(weak[0].title, "合并区间")
  assert.strictEqual(weak[0].topic, "数组")
  assert.strictEqual(weak[0].mastery, 0.4)

  const strong = tm.strongUnitsFor(UNITS, values)
  assert.deepStrictEqual(
    strong.map(function (row) {
      return row.unit_id
    }),
    ["algo:lc-00001", "db:mvcc"]
  )
})

test("materializeMastery 把三层行交给 store.replaceMastery", function () {
  let captured = null
  const store = {
    replaceMastery: function (rows) {
      captured = rows
    },
  }
  tm.materializeMastery(store, [
    { scope: "track", scope_id: "algorithm", level: 0.5, updated_at: NOW },
  ])
  assert.strictEqual(captured.length, 1)
  assert.strictEqual(captured[0].scope, "track")
  assert.strictEqual(captured[0].scope_id, "algorithm")
  assert.strictEqual(captured[0].level, 0.5)

  assert.throws(function () {
    tm.materializeMastery({}, [])
  }, /replaceMastery/)
})
