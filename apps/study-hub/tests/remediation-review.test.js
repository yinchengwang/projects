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
