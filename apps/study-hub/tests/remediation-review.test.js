"use strict"
/**
 * remediation-review.test.js —— 抽检语义闸门
 *
 * 补齐设计 §6.2：缺口是「还没补齐的东西」，抽检是「人看过之后给个结论」。
 * 两者不能混：没达标（draft / todo / rejected）的单元不配被抽检，
 * 更不该能被打回——那等于用人的一次点击覆盖执行器的裁决。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const review = require("../core/supply/review.js")
const { tmpDir } = require("./helpers/tmp.js")

function unit(id, status, reviewed) {
  return {
    id: id,
    track: "database",
    kind: "card",
    title: id,
    statement: "讲解",
    entry: { demo: "scaffold/grokking/x/main.py", script: "units/database/x.assert.py" },
    status: status,
    reviewed: reviewed === true,
  }
}

test("待抽检列表只收 verified 且没抽检过的", function () {
  const targets = review.pickReviewTargets(
    [
      unit("db:a", "verified", false),
      unit("db:b", "draft", false),
      unit("db:c", "todo", false),
      unit("db:d", "rejected", false),
      unit("db:e", "verified", true),
    ],
    {}
  )
  const ids = targets.map(function (t) { return t.id })
  assert.deepStrictEqual(ids, ["db:a"])
})

test("给未达标的单元写回抽检结论要被拒", function () {
  const root = tmpDir("study-hub-review-")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "b.md"),
    "---\nid: db:b\ntrack: database\nkind: card\nstatus: draft\nreviewed: false\nreviewed_at: \"\"\n---\n\n## 讲解\n\n正文\n\n## 关联\n",
    "utf8"
  )
  const applied = review.applyReview({
    unit: unit("db:b", "draft", false),
    passed: true,
    unitsDir: path.join(root, "units"),
    repoRoot: root,
    stateFile: path.join(root, ".self-written.json"),
    today: "2026-11-22",
  })
  assert.strictEqual(applied.ok, false)
  assert.strictEqual(applied.code, "invalid")
  assert.ok(applied.reason.indexOf("还没通过校验") !== -1, applied.reason)
  // 文件一个字节都不该动
  const after = fs.readFileSync(path.join(unitsDir, "b.md"), "utf8")
  assert.ok(after.indexOf("reviewed: true") === -1, "被拒的抽检不该写文件")
})

test("分层抽样：每层至少 5 题，层内按比例", function () {
  const units = []
  const sources = [
    { src: "leetcode", dif: "medium", n: 404 },
    { src: "leetcode", dif: "easy", n: 226 },
    { src: "nowcoder", dif: "easy", n: 29 },
    { src: "nowcoder", dif: "medium", n: 28 },
  ]
  sources.forEach(function (g) {
    for (let i = 0; i < g.n; i += 1) {
      units.push({
        id: g.src + ":" + g.dif + ":" + i,
        track: "algorithm",
        kind: "problem",
        title: "题 " + i,
        statement: "题面",
        entry: { demo: "", reference: "", graded: "user", script: "" },
        status: "verified",
        reviewed: false,
        source: g.src,
        difficulty: g.dif,
      })
    }
  })
  const targets = review.pickReviewTargets(units, { stratify: true })
  const layers = {}
  targets.forEach(function (t) {
    const key = String(t.source || "") + "|" + String(t.difficulty || "")
    layers[key] = (layers[key] || 0) + 1
  })
  assert.strictEqual(targets.length >= 74, true, "至少抽 74 题，实际 " + targets.length)
  // 每层至少 5 题
  Object.keys(layers).forEach(function (key) {
    assert.ok(layers[key] >= 5, key + " 层只抽了 " + layers[key] + " 题")
  })
  // 四层都要抽到
  assert.strictEqual(Object.keys(layers).length, 4, JSON.stringify(layers))
})

test("分层抽样：同一层的题不重复", function () {
  const units = []
  for (let i = 0; i < 20; i += 1) {
    units.push({
      id: "leetcode:easy:" + i,
      track: "algorithm",
      kind: "problem",
      title: "题 " + i,
      statement: "题面",
      entry: { demo: "", reference: "", graded: "user", script: "" },
      status: "verified",
      reviewed: false,
      source: "leetcode",
      difficulty: "easy",
    })
  }
  const targets = review.pickReviewTargets(units, { stratify: true })
  const ids = targets.map(function (t) { return t.id })
  assert.strictEqual(new Set(ids).size, ids.length, "不该有重复")
})
