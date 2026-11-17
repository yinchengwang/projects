"use strict"
/**
 * p1-prioritize.test.js —— 轨内优先级
 *
 * 排序键（从高到低）：到期复习 → 当前轮次未完成 → 其余新单元（掌握度升序）→ 已完成。
 * 纯函数，不碰数据库。
 */

const test = require("node:test")
const assert = require("node:assert")

const { prioritize } = require("../core/scheduler/prioritize.js")

function u(id) {
  return { id: id, track: "algorithm", kind: "problem", title: id, status: "verified" }
}

test("到期复习的排最前", function () {
  const out = prioritize([u("algo:lc-00001"), u("algo:lc-00002"), u("algo:lc-00003")], {
    dueUnitIds: new Set(["algo:lc-00002"]),
  })
  assert.strictEqual(out[0].id, "algo:lc-00002")
})

test("当前轮次未完成的排第二，并按 roundOrder 的次序", function () {
  const out = prioritize([u("algo:lc-00003"), u("algo:lc-00001"), u("algo:lc-00002")], {
    currentRoundUnitIds: new Set(["algo:lc-00001", "algo:lc-00002"]),
    roundOrder: ["algo:lc-00002", "algo:lc-00001"],
  })
  assert.deepStrictEqual(
    out.map(function (x) {
      return x.id
    }),
    ["algo:lc-00002", "algo:lc-00001", "algo:lc-00003"]
  )
})

test("其余新单元按掌握度升序（越弱越先），再按 id 升序定序", function () {
  const out = prioritize([u("algo:lc-00002"), u("algo:lc-00001"), u("algo:lc-00003")], {
    masteryByUnit: { "algo:lc-00001": 0.8, "algo:lc-00002": 0.2, "algo:lc-00003": 0.2 },
  })
  assert.deepStrictEqual(
    out.map(function (x) {
      return x.id
    }),
    ["algo:lc-00002", "algo:lc-00003", "algo:lc-00001"]
  )
})

test("掌握度全部相同时按 id 升序，而不是保持传入顺序", function () {
  // 上一条用例的输入顺序恰好是 [02, 01, 03]，去掉 id tie-break 靠「稳定排序」
  // 也能蒙对 lc-00002 < lc-00003 的相对顺序。这条**故意让传入顺序与 id 升序相反**：
  // 只有真的比较 id 才能得到 01/02/03。
  const out = prioritize([u("algo:lc-00003"), u("algo:lc-00001"), u("algo:lc-00002")], {
    masteryByUnit: {},
  })
  assert.deepStrictEqual(
    out.map(function (x) {
      return x.id
    }),
    ["algo:lc-00001", "algo:lc-00002", "algo:lc-00003"]
  )
})

test("档位冲突时的优先级：到期压过当前轮次", function () {
  // 契约 §6.3 的四档：到期(1) → 当前轮次(2) → 新单元(3) → 已完成(4)。
  //
  // 「到期 vs 轮次」的先后只在一个特定构造下可观测：
  //   A = 只到期（不在轮次）        B = 既到期又在轮次
  //   正确实现：两者都是第 1 档 → 靠 id 升序定序
  //   颠倒实现：A 第 1 档、B 第 2 档 → A **无条件**排在 B 前面
  // 所以必须让 **A 的 id 更大**，两种实现的顺序才会分岔。
  // （若让 A 的 id 更小，两种实现都得到 A 在前 —— 变异体会存活，这正是这条的由来。）
  const dueOnly = u("algo:lc-00002")   // A：只到期，id 更大
  const dueAndRound = u("algo:lc-00001") // B：既到期又在轮次，id 更小
  const out = prioritize([dueOnly, dueAndRound], {
    dueUnitIds: new Set(["algo:lc-00001", "algo:lc-00002"]),
    currentRoundUnitIds: new Set(["algo:lc-00001"]),
    roundOrder: ["algo:lc-00001"],
  })
  assert.deepStrictEqual(
    out.map(function (x) {
      return x.id
    }),
    ["algo:lc-00001", "algo:lc-00002"],
    "两者都该落进『到期』档，再按 id 升序：" + JSON.stringify(out.map(function (x) { return x.id }))
  )

  // 对照：一个已完成的单元（第 4 档），到期的单元必须压在它前面
  const overdueBeatsDone = prioritize([u("algo:lc-00001"), u("algo:lc-00002")], {
    dueUnitIds: new Set(["algo:lc-00002"]),
    doneUnitIds: new Set(["algo:lc-00001"]),
  })
  assert.deepStrictEqual(
    overdueBeatsDone.map(function (x) {
      return x.id
    }),
    ["algo:lc-00002", "algo:lc-00001"]
  )
  //
  // 注意：**「既在 currentRoundUnitIds 又在 doneUnitIds」是契约之外的输入**——
  // 契约把 currentRoundUnitIds 定义为「属于当前轮次**且未完成**的单元 id」，
  // 两者按定义互斥。因此实现里 done 与 round 两个分支的先后顺序不可观测，
  // 这里不钉它（钉了会与契约的档位序号打架，属于测试越权）。

  // 互斥输入下的正常次序：轮次档（第 2 档）排在已完成档（第 4 档）之前
  const roundBeforeDone = prioritize([u("algo:lc-00001"), u("algo:lc-00002"), u("algo:lc-00003")], {
    currentRoundUnitIds: new Set(["algo:lc-00003"]),
    roundOrder: ["algo:lc-00003"],
    doneUnitIds: new Set(["algo:lc-00001"]),
  })
  assert.deepStrictEqual(
    roundBeforeDone.map(function (x) {
      return x.id
    }),
    ["algo:lc-00003", "algo:lc-00002", "algo:lc-00001"]
  )
})

test("已完成的排到最后", function () {
  const out = prioritize([u("algo:lc-00001"), u("algo:lc-00002")], {
    doneUnitIds: new Set(["algo:lc-00001"]),
  })
  assert.deepStrictEqual(
    out.map(function (x) {
      return x.id
    }),
    ["algo:lc-00002", "algo:lc-00001"]
  )
})

test("到期的已完成单元仍然排最前——要复习", function () {
  const out = prioritize([u("algo:lc-00001"), u("algo:lc-00002")], {
    dueUnitIds: new Set(["algo:lc-00001"]),
    doneUnitIds: new Set(["algo:lc-00001"]),
  })
  assert.strictEqual(out[0].id, "algo:lc-00001")
})

test("不改动传入的数组", function () {
  const units = [u("algo:lc-00002"), u("algo:lc-00001")]
  prioritize(units, {})
  assert.deepStrictEqual(
    units.map(function (x) {
      return x.id
    }),
    ["algo:lc-00002", "algo:lc-00001"]
  )
})

test("同输入必同输出", function () {
  const units = [u("algo:lc-00003"), u("algo:lc-00001"), u("algo:lc-00002")]
  const ctx = { masteryByUnit: { "algo:lc-00002": 0.1 } }
  assert.deepStrictEqual(
    prioritize(units, ctx).map(function (x) {
      return x.id
    }),
    prioritize(units, ctx).map(function (x) {
      return x.id
    })
  )
})

test("ctx 可以整个省略", function () {
  assert.deepStrictEqual(
    prioritize([u("algo:lc-00001")]).map(function (x) {
      return x.id
    }),
    ["algo:lc-00001"]
  )
})
