"use strict"
/**
 * supply-algorithm-report.test.js —— 覆盖报告：还欠什么，按来源/难度/子轨/知识点分组
 *
 * 纯函数，不联网、不写盘。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const batch = require("../core/supply/algorithm-batch.js")
const fill = require("../core/supply/algorithm-fill.js")

/** 内存里的 settings 仓储 */
function fakeStore(initial) {
  const data = Object.assign({}, initial || {})
  return {
    getSetting: function (key, fallback) {
      return data[key] === undefined ? fallback : data[key]
    },
    setSetting: function (key, value) {
      data[key] = String(value)
    },
  }
}

/** 造一个 learning 仓根：三种形态各一道 */
function fakeLearningRoot() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-report-"))
  const bank = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(bank, { recursive: true })

  function skeleton(stem, extra) {
    const lines = []
    lines.push("---")
    lines.push("id: " + stem)
    lines.push("source: leetcode")
    lines.push("source_id: 7")
    lines.push("title: 反转字符串")
    lines.push("difficulty: easy")
    lines.push("topics: [字符串]")
    lines.push("tracks: [fundamentals]")
    lines.push("status: todo")
    lines.push("---")
    lines.push("")
    lines.push("## 题面")
    lines.push("")
    lines.push("## 测试用例")
    lines.push("")
    lines.push("| input | expect |")
    lines.push("|---|---|")
    lines.push("")
    lines.push("## 参考解法")
    lines.push("")
    return lines.join("\n")
  }

  // A：题面空、用例空
  fs.writeFileSync(path.join(bank, "lc-00001.md"), skeleton("lc-00001"), "utf8")
  // C：题面与用例齐、参考解法空
  fs.writeFileSync(
    path.join(bank, "lc-00002.md"),
    skeleton("lc-00002").replace("## 题面\n\n", "## 题面\n\n给你一个字符串。\n\n").replace("|---|---|", "|---|---|\n| 123 | 321 |"),
    "utf8"
  )
  // ready：三样齐
  fs.writeFileSync(
    path.join(bank, "lc-00003.md"),
    skeleton("lc-00003")
      .replace("## 题面\n\n", "## 题面\n\n给你一个字符串。\n\n")
      .replace("|---|---|", "|---|---|\n| 123 | 321 |")
      .replace("## 参考解法\n", "## 参考解法\n\n```cpp\nint main(){return 0;}\n```\n"),
    "utf8"
  )
  return root
}

test("覆盖报告给出总数、已齐备与还欠", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const report = batch.buildCoverageReport({ items: items, store: fakeStore() })
    assert.equal(report.total, 3)
    assert.equal(report.bankReady, 1, "题库里本来就齐的 1 题")
    assert.equal(report.filled, 0, "还没补")
    assert.equal(report.ready, 1)
    assert.equal(report.debt, 2)
    assert.equal(report.byShape.A, 1)
    assert.equal(report.byShape.C, 1)
    assert.equal(report.byShape.ready, 1)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("补出来的 verified 单元算已齐备，缺口跟着降", function () {
  const root = fakeLearningRoot()
  try {
    // 造一个补好的单元：units/algorithm/lc-00001.md，status: verified
    const unitsDir = path.join(root, "units", "algorithm")
    fs.mkdirSync(unitsDir, { recursive: true })
    fs.writeFileSync(
      path.join(unitsDir, "lc-00001.md"),
      "---\nid: algo:lc-00001\nstatus: verified\n---\n\n## 题面\n\n给你一个字符串。\n",
      "utf8"
    )
    const items = fill.scanBank({ learningRoot: root }).items
    const units = fill.readUnitStatuses(unitsDir)
    assert.equal(units["lc-00001"], "verified")
    const report = batch.buildCoverageReport({ items: items, store: fakeStore(), units: units })
    assert.equal(report.filled, 1)
    assert.equal(report.ready, 2)
    assert.equal(report.debt, 1, "补好一道，欠账少一道")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("覆盖报告分开数缺题面 / 缺用例 / 缺参考解法", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const report = batch.buildCoverageReport({ items: items, store: fakeStore() })
    assert.equal(report.missing.statement, 1, "只有形态 A 缺题面")
    assert.equal(report.missing.cases, 1, "只有形态 A 缺用例")
    assert.equal(report.missing.solution, 2, "形态 A 与形态 C 都缺参考解法")
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("覆盖报告按来源 / 难度 / 子轨 / 知识点分组", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const report = batch.buildCoverageReport({ items: items, store: fakeStore() })
    assert.equal(report.debtBySource["leetcode"], 2)
    assert.equal(report.debtByDifficulty["easy"], 2)
    assert.equal(report.debtBySubtrack["fundamentals"], 2)
    assert.equal(report.debtByTopic["字符串"], 2)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("覆盖报告带上最近批次的汇总", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const store = fakeStore()
    store.setSetting(
      "supply.batch.run.batch-20261113",
      JSON.stringify({
        batchId: "batch-20261113",
        track: "algorithm",
        planned: 10,
        done: 6,
        verified: 5,
        rejected: 1,
        failed: 0,
        skipped: 2,
        llmCalls: 9,
        stopped: true,
        startedAt: "2026-11-13T09:00:00.000Z",
        updatedAt: "2026-11-13T09:30:00.000Z",
      })
    )
    const report = batch.buildCoverageReport({ items: items, store: store, batchId: "batch-20261113" })
    assert.equal(report.run.batchId, "batch-20261113")
    assert.equal(report.run.verified, 5)
    assert.equal(report.run.stopped, true)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("覆盖报告渲染成中文文本", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const text = batch.renderCoverageReport(batch.buildCoverageReport({ items: items, store: fakeStore() }))
    assert.ok(text.indexOf("算法轨题库覆盖报告") !== -1, text)
    assert.ok(text.indexOf("还欠 2 题") !== -1, text)
    assert.ok(text.indexOf("缺题面 1") !== -1, text)
    assert.ok(text.indexOf("leetcode 2") !== -1, text)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})

test("planBatch 给出待补与跳过的清单", function () {
  const root = fakeLearningRoot()
  try {
    const items = fill.scanBank({ learningRoot: root }).items
    const store = fakeStore()
    const plan = batch.planBatch({ items: items, store: store })
    assert.equal(plan.planned.length, 2, "两题要补")
    assert.equal(plan.skipped.length, 1, "齐备的跳过")
    assert.equal(plan.stats.total, 3)

    // 已 verified 的题，续跑时不再排进来
    store.setSetting(
      "supply.batch.item.algo.lc-00001",
      JSON.stringify({ stem: "lc-00001", state: "verified", attempts: 1, reason: "", updatedAt: "" })
    )
    const again = batch.planBatch({ items: items, store: store, resume: true })
    assert.equal(again.planned.length, 1)
    assert.equal(again.skipped.length, 2)

    // 磁盘上已经有 verified 单元的题，不看 settings 也不排进来
    const withUnits = batch.planBatch({
      items: items,
      store: fakeStore(),
      units: { "lc-00002": "verified" },
    })
    assert.equal(withUnits.planned.length, 1)
    assert.equal(withUnits.skipped.length, 2)
  } finally {
    fs.rmSync(root, { recursive: true, force: true })
  }
})
