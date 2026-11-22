"use strict"
/**
 * remediation-loader-status.test.js —— 状态门槛闸门
 *
 * 补齐设计 §3.2：verified 只能由 loader C（verify 写回的单元文件）承载。
 * loader B 单独加载一张卡时，最多给 draft——不管断言脚本在不在。
 * 理由：脚本存在 ≠ 校验通过。mvcc / index_design 的脚本存在但变异测试判 rejected，
 * 若「脚本存在即 verified」，这两张会被绕过裁决放回学习队列。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const loaderB = require("../core/unit/loader-b.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一张卡目录；unitsDir 下可放断言脚本。 */
function makeCard(stem, opts) {
  const options = opts || {}
  const root = path.join(tmpDir("study-hub-status-"), stem)
  const dir = path.join(root, "card")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(path.join(dir, "README.md"), "# " + stem + "\n\n正文\n", "utf8")
  fs.writeFileSync(path.join(dir, "main.py"), "print('hi')\n", "utf8")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  if (options.withScript) {
    fs.writeFileSync(path.join(unitsDir, stem + ".assert.py"), "print('ok')\n", "utf8")
  }
  return { dir: dir, unitsDir: path.join(root, "units") }
}

test("断言脚本不存在 → draft（不是 verified）", function () {
  const card = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(res.unit)
  assert.equal(res.unit.status, "draft")
})

test("断言脚本存在但没有 loader C 单元文件 → 仍是 draft", function () {
  const card = makeCard("mvcc", { withScript: true })
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(res.unit)
  assert.equal(res.unit.status, "draft", "脚本存在不等于校验通过，不能给 verified")
})

test("status 仍然只能是四档里的合法值", function () {
  const card = makeCard("mvcc", { withScript: true })
  const res = loaderB.parseCardDirectory(card.dir, { track: "database", unitsDir: card.unitsDir })
  assert.ok(["todo", "draft", "verified", "rejected"].indexOf(res.unit.status) !== -1)
})

const verify = require("../core/supply/verify.js")
const unitFile = require("../core/supply/unit-file.js")

test("verify 新建单元文件时讲解段是占位符，不是教材原文", function () {
  const root = tmpDir("study-hub-writeverdict-")
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  const unit = {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC",
    statement: "# MVCC\n\n这是 README 原文，不该进讲解段。\n",
    entry: { demo: "scaffold/grokking/mvcc/main.py", script: "units/database/mvcc.assert.py", language: "python" },
    source: "grokking",
  }
  const written = verify.writeVerdict(unit, "verified", {
    unitsDir: unitsDir,
    repoRoot: root,
    stateFile: path.join(root, ".self-written.json"),
  })
  assert.ok(written.ok, written.reason)
  const text = fs.readFileSync(path.join(unitsDir, "database", "mvcc.md"), "utf8")
  assert.ok(text.indexOf("README 原文") === -1, "教材原文不该塞进讲解段")
  assert.ok(text.indexOf(unitFile.EMPTY_EXPLAIN) !== -1, "讲解段应是占位符")
  assert.ok(/^status: verified$/m.test(text), "status 要写回 verified")
})

const plan = require("../core/supply/plan.js")

test("卡类单元：没有 loader C 单元文件时，讲解算缺口", function () {
  const root = tmpDir("study-hub-gap-")
  const unit = {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC",
    statement: "# MVCC\n\n教材原文\n",
    entry: { demo: "scaffold/grokking/mvcc/main.py", script: "units/database/mvcc.assert.py" },
  }
  const missing = plan.missingOf(unit, { learningRoot: root, contentRoots: [root] })
  assert.ok(missing.indexOf("asserts") !== -1, "缺断言脚本要算缺口：" + JSON.stringify(missing))
  assert.ok(missing.indexOf("statement") !== -1, "没有单元文件时讲解也算缺口：" + JSON.stringify(missing))
})

test("卡类单元：单元文件讲解段是占位符时，讲解算缺口", function () {
  const root = tmpDir("study-hub-gap2-")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "mvcc.md"),
    "---\nid: db:mvcc\ntrack: database\nkind: card\nstatus: verified\n---\n\n## 讲解\n\n（讲解正文还没生成）\n\n## 关联\n",
    "utf8"
  )
  const unit = {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC",
    statement: "（讲解正文还没生成）",
    entry: { demo: "scaffold/grokking/mvcc/main.py", script: "units/database/mvcc.assert.py" },
  }
  const missing = plan.missingOf(unit, { learningRoot: root, contentRoots: [root] })
  assert.ok(missing.indexOf("statement") !== -1, "占位符讲解要算缺口：" + JSON.stringify(missing))
})

test("卡类单元：单元文件讲解段有真正文时，讲解不算缺口", function () {
  const root = tmpDir("study-hub-gap3-")
  const unitsDir = path.join(root, "units", "database")
  fs.mkdirSync(unitsDir, { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "mvcc.md"),
    "---\nid: db:mvcc\ntrack: database\nkind: card\nstatus: verified\n---\n\n## 讲解\n\n这是真正的讲解正文，讲清了版本链怎么长出来的。\n\n## 关联\n",
    "utf8"
  )
  const unit = {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC",
    statement: "教材原文",
    entry: { demo: "scaffold/grokking/mvcc/main.py", script: "units/database/mvcc.assert.py" },
  }
  const missing = plan.missingOf(unit, { learningRoot: root, contentRoots: [root] })
  assert.strictEqual(missing.indexOf("statement"), -1, "有真讲解就不该报缺口：" + JSON.stringify(missing))
})
