"use strict"
/**
 * p1-plan.test.js —— 组装一次会话的学习计划
 *
 * 到期 → 环境门禁 → 候选池 → 轨内优先级 → 跨轨配额 → 跨轨交错。
 */

const test = require("node:test")
const assert = require("node:assert")

const { buildPlan } = require("../core/scheduler/plan.js")

function unit(id, track, extra) {
  return Object.assign(
    {
      id: id,
      track: track,
      kind: "problem",
      title: id,
      status: "verified",
      topics: [],
      subtracks: [],
    },
    extra || {}
  )
}

const TRACKS = [
  { code: "algorithm", label: "算法", weight: 0.4, enabled: true },
  { code: "database", label: "数据库", weight: 0.3, enabled: true },
  { code: "ai", label: "AI", weight: 0.3, enabled: true },
]

function plan(over) {
  const input = Object.assign(
    {
      n: 3,
      nowIso: "2026-01-01T00:00:00.000Z",
      k: 5,
      units: [],
      progressByUnit: {},
      cards: [],
      masteryByUnit: {},
      rounds: [],
      tracks: TRACKS,
      env: { numpy: { ok: true } },
    },
    over || {}
  )
  return buildPlan(input)
}

function ids(r) {
  return r.queue.map(function (item) {
    return item.unit.id
  })
}

test("三轨各有候选时，队列跨轨混合", function () {
  const r = plan({
    units: [
      unit("algo:lc-00001", "algorithm"),
      unit("algo:lc-00002", "algorithm"),
      unit("algo:lc-00003", "algorithm"),
      unit("db:mvcc", "database"),
      unit("db:transaction", "database"),
      unit("ai:cnn", "ai"),
    ],
  })
  assert.strictEqual(r.queue.length, 3)
  const tracks = r.queue.map(function (item) {
    return item.track
  })
  assert.ok(tracks.indexOf("algorithm") !== -1, tracks.join(","))
  assert.ok(tracks.indexOf("database") !== -1, tracks.join(","))
})

test("缺 numpy 时 AI 轨整轨跳过，并给出中文说明", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("ai:cnn", "ai")],
    env: { numpy: { ok: false } },
  })
  assert.deepStrictEqual(ids(r), ["algo:lc-00001"])
  assert.ok(r.warnings.join(" ").indexOf("numpy") !== -1, r.warnings.join("|"))
})

test("缺 numpy 时数据库轨只跳过 clustering 这张卡", function () {
  const r = plan({
    units: [unit("db:mvcc", "database"), unit("db:clustering", "database")],
    env: { numpy: { ok: false } },
    n: 2,
  })
  assert.deepStrictEqual(ids(r), ["db:mvcc"])
})

test("日期通道到期的单元排最前", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    cards: [
      {
        problem_id: "lc-00002",
        unit_id: "algo:lc-00002",
        next_review_at: "2020-01-01T00:00:00.000Z",
        units_since: 0,
      },
    ],
    n: 1,
  })
  assert.strictEqual(ids(r)[0], "algo:lc-00002")
})

test("次数通道到期（units_since >= K）也算到期", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    cards: [
      {
        problem_id: "lc-00002",
        unit_id: "algo:lc-00002",
        next_review_at: "2099-01-01T00:00:00.000Z",
        units_since: 5,
      },
    ],
    k: 5,
    n: 1,
  })
  assert.strictEqual(ids(r)[0], "algo:lc-00002")
})

test("当前轮次里的未完成单元排在其它新单元前面", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    rounds: [
      {
        id: "r33",
        status: "active",
        items: [
          { raw: "| 1 | lc-00002 | 两数之和 | todo |  |", id: "lc-00002", title: "两数之和", status: "todo" },
        ],
      },
    ],
    n: 1,
  })
  assert.strictEqual(ids(r)[0], "algo:lc-00002")
  assert.strictEqual(r.roundId, "r33")
})

test("已完成（done）的单元不进池", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    progressByUnit: { "algo:lc-00001": "done" },
    n: 1,
  })
  assert.strictEqual(ids(r)[0], "algo:lc-00002")
})

test("draft 单元不进池", function () {
  const r = plan({
    units: [
      unit("algo:lc-00001", "algorithm", { status: "draft" }),
      unit("algo:lc-00002", "algorithm"),
    ],
    n: 1,
  })
  assert.strictEqual(ids(r)[0], "algo:lc-00002")
})

test("关掉的轨不参与配额，配额记 0", function () {
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("db:mvcc", "database")],
    tracks: [
      { code: "algorithm", label: "算法", weight: 0.4, enabled: true },
      { code: "database", label: "数据库", weight: 0.3, enabled: false },
      { code: "ai", label: "AI", weight: 0.3, enabled: true },
    ],
    n: 2,
  })
  assert.strictEqual(r.quota.database, 0)
  // 数据库轨被关掉，它的候选不参与配额；算法轨只有 1 个候选，排不满 2 个
  assert.strictEqual(r.quota.algorithm, 1)
  assert.deepStrictEqual(ids(r), ["algo:lc-00001"])
})

test("一个单元都没有：needMore 为 true，队列为空", function () {
  const r = plan({ units: [], n: 3 })
  assert.strictEqual(r.queue.length, 0)
  assert.strictEqual(r.needMore, true)
  assert.ok(
    r.warnings.join(" ").indexOf("没有可学的单元，建议去供给流水线补内容") !== -1,
    r.warnings.join("|")
  )
})

test("同输入必同输出", function () {
  const over = {
    units: [unit("algo:lc-00001", "algorithm"), unit("db:mvcc", "database")],
    n: 2,
  }
  assert.deepStrictEqual(ids(plan(over)), ids(plan(over)))
})

test("掌握度参与排序：同一轨内越弱的越先", function () {
  // 原 11 条用例里**没有任何一条**用 masteryByUnit —— 把 plan.js 里
  // `masteryByUnit: masteryByUnit` 改成 `{}` 依然全绿（实测存活）。
  const r = plan({
    units: [
      unit("algo:lc-00001", "algorithm"),
      unit("algo:lc-00002", "algorithm"),
      unit("algo:lc-00003", "algorithm"),
    ],
    masteryByUnit: { "algo:lc-00001": 0.9, "algo:lc-00002": 0.1, "algo:lc-00003": 0.5 },
    n: 2,
    tracks: [{ code: "algorithm", label: "算法", weight: 1, enabled: true }],
  })
  assert.deepStrictEqual(ids(r), ["algo:lc-00002", "algo:lc-00003"])
})

test("轮次里已完成的单元进 doneUnitIds，排到最后", function () {
  // 原用例只覆盖「轮次里 status: todo」，没有覆盖「轮次里 status: done」。
  // 把 `round.doneUnitIds.forEach(...)` 整块删掉依然全绿（实测存活）。
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    rounds: [
      {
        id: "r1",
        status: "active",
        items: [
          { id: "lc-00001", title: "甲", status: "done" },
          { id: "lc-00002", title: "乙", status: "todo" },
        ],
      },
    ],
    n: 2,
    tracks: [{ code: "algorithm", label: "算法", weight: 1, enabled: true }],
  })
  assert.ok(r.doneUnitIds.has("algo:lc-00001"), "轮次里 done 的单元要进 doneUnitIds")
  assert.strictEqual(ids(r)[0], "algo:lc-00002", "未完成的排前面：" + ids(r).join(","))
})

test("roundOrder 参与排序：同一档内按轮次顺序", function () {
  // 原用例的轮次里只有一个 todo 项，roundOrder 只有 1 个元素、顺序无从体现。
  // 把 `roundOrder: round.roundOrder` 改成 `[]` 依然全绿（实测存活）。
  const r = plan({
    units: [
      unit("algo:lc-00001", "algorithm"),
      unit("algo:lc-00002", "algorithm"),
      unit("algo:lc-00003", "algorithm"),
    ],
    rounds: [
      {
        id: "r1",
        status: "active",
        items: [
          { id: "lc-00003", title: "丙", status: "todo" },
          { id: "lc-00001", title: "甲", status: "todo" },
        ],
      },
    ],
    n: 2,
    tracks: [{ code: "algorithm", label: "算法", weight: 1, enabled: true }],
  })
  assert.deepStrictEqual(ids(r), ["algo:lc-00003", "algo:lc-00001"])
})

test("到期集合同时影响候选池：做完但到期的单元会被重新排进来", function () {
  // 原用例「已完成（done）的单元不进池」只验了「没到期 → 不进」，
  // 没有验「到期 → 照样进」。把 plan.js 里传给 collectPools 的 dueUnitIds
  // 换成空 Set 依然全绿（实测存活）。
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("algo:lc-00002", "algorithm")],
    progressByUnit: { "algo:lc-00001": "done" },
    cards: [
      {
        problem_id: "lc-00001",
        unit_id: "algo:lc-00001",
        next_review_at: "2020-01-01T00:00:00.000Z",
        units_since: 0,
      },
    ],
    n: 1,
    tracks: [{ code: "algorithm", label: "算法", weight: 1, enabled: true }],
  })
  assert.deepStrictEqual(ids(r), ["algo:lc-00001"], "到期要复习，哪怕是 done：" + ids(r).join(","))
})

test("counts 与 dueUnitIds 原样回传（供 P1 的会话页显示）", function () {
  // 把 `counts: raw.counts` 换成 `counts: {}` 依然全绿（实测存活）。
  const r = plan({
    units: [unit("algo:lc-00001", "algorithm"), unit("db:mvcc", "database")],
    n: 2,
  })
  assert.strictEqual(r.counts.algorithm.candidates, 1)
  assert.strictEqual(r.counts.database.candidates, 1)
  assert.ok(r.dueUnitIds instanceof Set)
})
