"use strict"
/**
 * unit-loader-b.test.js —— core/unit/loader-b.js
 *
 * loader B 读 scaffold/grokking/<card>/ 目录，产出 Unit。
 * 契约 §3.2 冻结了行为。教学资产零污染：断言脚本写在**独立文件**里，main.py 原文不动。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const loaderB = require("../core/unit/loader-b.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一张卡目录。 */
function makeCard(stem, opts) {
  const options = opts || {}
  const dir = path.join(tmpDir("study-hub-card-"), stem)
  fs.mkdirSync(dir, { recursive: true })
  if (options.readme !== null) {
    fs.writeFileSync(
      path.join(dir, "README.md"),
      options.readme === undefined ? "# " + stem + " 卡\n\n这是讲解正文。\n" : options.readme,
      "utf8"
    )
  }
  if (options.notes !== null) {
    fs.writeFileSync(
      path.join(dir, "NOTES.md"),
      options.notes === undefined ? "这是提纲。\n" : options.notes,
      "utf8"
    )
  }
  if (options.main !== null) {
    fs.writeFileSync(path.join(dir, "main.py"), options.main === undefined ? "print('hi')\n" : options.main, "utf8")
  }
  return dir
}

test("目录名即 stem，id 带 db: 前缀", function () {
  const dir = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.ok(res.unit)
  assert.equal(res.unit.id, "db:mvcc")
  assert.equal(res.unit.track, "database")
  assert.equal(res.unit.kind, "card")
})

test("AI 轨用 ai: 前缀", function () {
  const dir = makeCard("cnn")
  const res = loaderB.parseCardDirectory(dir, { track: "ai" })
  assert.equal(res.unit.id, "ai:cnn")
})

test("README 第一行的 # 标题当标题", function () {
  const dir = makeCard("mvcc", { readme: "# MVCC：版本链与 ReadView\n\n正文\n" })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.title, "MVCC：版本链与 ReadView")
})

test("README 没有标题行时用 stem 兜底", function () {
  const dir = makeCard("mvcc", { readme: "直接就是正文，没有标题。\n" })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.title, "mvcc")
})

test("statement 是 README + 分隔线 + NOTES", function () {
  const dir = makeCard("mvcc", { readme: "# 标题\n\nREADME 正文\n", notes: "NOTES 提纲\n" })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  // 契约 §3.2：statement = README 全文 + "\n\n---\n\n" + NOTES 全文。
  // README 全文自带结尾换行，所以拼出来是 README 的换行 + 分隔线的两个换行 = **三个**换行。
  assert.equal(res.unit.statement, "# 标题\n\nREADME 正文\n\n\n---\n\nNOTES 提纲\n")
})

test("没有 NOTES.md 时只留 README", function () {
  const dir = makeCard("mvcc", { notes: null, readme: "# 标题\n\nREADME 正文\n" })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.statement.indexOf("---"), -1)
  assert.ok(res.unit.statement.indexOf("README 正文") !== -1)
})

test("README 与 main.py 都在 → verified", function () {
  const dir = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.status, "verified")
})

test("README 在、main.py 不在 → draft", function () {
  const dir = makeCard("mvcc", { main: null })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.status, "draft")
})

test("README 缺失 → 跳过这张卡，unit 为 null", function () {
  const dir = makeCard("mvcc", { readme: null })
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit, null)
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("卡目录没有 README.md，跳过：mvcc") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("entry.demo 指向 main.py；不存在时为空串", function () {
  const dir = makeCard("mvcc")
  const withDemo = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(withDemo.unit.entry.demo, "scaffold/grokking/mvcc/main.py")

  const dir2 = makeCard("tx", { main: null })
  const noDemo = loaderB.parseCardDirectory(dir2, { track: "database" })
  assert.equal(noDemo.unit.entry.demo, "")
})

test("entry.script 恒填期望落点；AI 轨落在 units/ai 下", function () {
  const dir = makeCard("mvcc")
  const db = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(db.unit.entry.script, "units/database/mvcc.assert.py")
  assert.equal(db.unit.entry.graded, "script")
  assert.equal(db.unit.entry.language, "python")

  const dir2 = makeCard("cnn")
  const ai = loaderB.parseCardDirectory(dir2, { track: "ai" })
  assert.equal(ai.unit.entry.script, "units/ai/cnn.assert.py")
})

test("asserts 与 topics 恒为空数组", function () {
  const dir = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.deepEqual(res.unit.asserts, [])
  assert.deepEqual(res.unit.topics, [])
})

test("difficulty 恒 medium，source 恒 grokking", function () {
  const dir = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.difficulty, "medium")
  assert.equal(res.unit.source, "grokking")
})

test("path 指向卡目录，progress 恒 todo", function () {
  const dir = makeCard("mvcc")
  const res = loaderB.parseCardDirectory(dir, { track: "database" })
  assert.equal(res.unit.path, "scaffold/grokking/mvcc/")
  assert.equal(res.unit.progress, "todo")
})

test("断言脚本还没写时给一句中文提醒", function () {
  const dir = makeCard("mvcc")
  // unitsDir 是 learning/units 的**根**（契约 §3.2），脚本落点 = <unitsDir>/<track>/<stem>.assert.py
  const unitsDir = tmpDir("study-hub-units-")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  const res = loaderB.parseCardDirectory(dir, { track: "database", unitsDir: unitsDir })
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("断言脚本还没写：units/database/mvcc.assert.py") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("断言脚本已经写了就不提醒", function () {
  const dir = makeCard("mvcc")
  // unitsDir 是 learning/units 的**根**，脚本要落在 <unitsDir>/database/mvcc.assert.py
  const unitsDir = tmpDir("study-hub-units-")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  fs.writeFileSync(path.join(unitsDir, "database", "mvcc.assert.py"), "# ok\n", "utf8")
  const res = loaderB.parseCardDirectory(dir, { track: "database", unitsDir: unitsDir })
  assert.equal(
    res.warnings.some(function (w) {
      return w.indexOf("断言脚本还没写") !== -1
    }),
    false
  )
})
