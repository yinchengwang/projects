"use strict"
/**
 * unit-index.test.js —— core/unit/index.js
 *
 * 汇总三个 loader：去重（先到先得）、排序、索引缓存，以及把结果物化进 units 表。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const index = require("../core/unit/index.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一份算法题 md。 */
function makeProblemMd(id, title, withCases) {
  const cases = withCases
    ? ["## 测试用例", "", "| input | expect |", "|---|---|", "| 1 | 2 |", ""].join("\n")
    : ""
  return [
    "---",
    "id: " + id,
    "source: leetcode",
    "title: " + title,
    "difficulty: medium",
    "topics: [数组]",
    "tracks: [interview]",
    "status: todo",
    "---",
    "",
    "## 题面",
    "",
    "题面 " + title,
    "",
    cases,
  ].join("\n")
}

/** 造一棵内容树。 */
function makeTree() {
  const root = tmpDir("study-hub-tree-")

  // learning 仓的题库
  const bankDir = path.join(root, "learning", "bank", "interview")
  fs.mkdirSync(bankDir, { recursive: true })
  fs.writeFileSync(path.join(bankDir, "lc-00007.md"), makeProblemMd("lc-00007", "反转整数", true), "utf8")
  fs.writeFileSync(path.join(bankDir, "lc-00008.md"), makeProblemMd("lc-00008", "回文数", false), "utf8")

  // 工程仓的本地题库
  const localDir = path.join(root, "app", "bank", "engineering")
  fs.mkdirSync(localDir, { recursive: true })
  fs.writeFileSync(path.join(localDir, "cf-12-d.md"), makeProblemMd("cf-12-d", "舞会", true), "utf8")

  // 新内容的落点
  const unitsDir = path.join(root, "learning", "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "database", "mvcc.md"),
    [
      "---",
      "id: db:mvcc",
      "track: database",
      "kind: card",
      "title: MVCC",
      "graded: script",
      "script: units/database/mvcc.assert.py",
      "status: verified",
      "---",
      "",
      "## 讲解",
      "",
      "MVCC 讲解。",
      "",
    ].join("\n"),
    "utf8"
  )

  // grokking 卡
  const cardDir = path.join(root, "learning", "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC 卡\n\n正文\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")

  return {
    learningRoot: path.join(root, "learning"),
    localBank: path.join(root, "app", "bank"),
    unitsDir: unitsDir,
    grokkingDir: path.join(root, "learning", "scaffold", "grokking"),
  }
}

test("三个 loader 的结果都进来了", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  const ids = res.units.map(function (u) { return u.id })
  assert.ok(ids.indexOf("algo:lc-00007") !== -1, JSON.stringify(ids))
  assert.ok(ids.indexOf("algo:lc-00008") !== -1, JSON.stringify(ids))
  assert.ok(ids.indexOf("algo:cf-12-d") !== -1, JSON.stringify(ids))
  assert.ok(ids.indexOf("db:mvcc") !== -1, JSON.stringify(ids))
})

test("loader C 优先级高于 loader B：同一个 id 保留 C 的", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  const mvcc = res.byId["db:mvcc"]
  assert.ok(mvcc)
  // C 版本（单元文件）压过 B 版本（教材卡）：标题与正文都是单元文件里那份
  assert.equal(mvcc.title, "MVCC")
  assert.equal(mvcc.statement, "MVCC 讲解。")
  assert.equal(mvcc.source, "generated")
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("单元 id 重复：db:mvcc（保留先出现的）") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("units 按 id 升序排，byId 与数组一致", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  const ids = res.units.map(function (u) { return u.id })
  const sorted = ids.slice().sort()
  assert.deepEqual(ids, sorted)
  assert.equal(res.byId["algo:lc-00007"].id, "algo:lc-00007")
  assert.equal(Object.keys(res.byId).length, ids.length)
})

test("目录不存在不抛错，只给警告", function () {
  const res = index.loadUnits({
    learningRoot: null,
    localBank: null,
    unitsDir: null,
    grokkingDir: null,
  })
  assert.deepEqual(res.units, [])
  assert.deepEqual(res.warnings, [])
})

test("坏文件不打断整体加载", function () {
  const opts = makeTree()
  const badDir = path.join(opts.unitsDir, "database")
  fs.writeFileSync(path.join(badDir, "broken.md"), "---\nid: 非法\n---\n\n## 讲解\n\n坏文件\n", "utf8")
  const res = index.loadUnits(opts)
  assert.ok(res.units.length >= 4)
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("单元 id 非法：非法") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("refreshUnitIndex 把结果物化进 units 表", function () {
  const opts = makeTree()
  const rows = []
  const fakeStore = {
    replaceUnits: function (items) {
      rows.length = 0
      items.forEach(function (item) { rows.push(item) })
    },
    listUnits: function () {
      return rows.slice()
    },
  }
  const res = index.refreshUnitIndex(fakeStore, opts)
  assert.equal(res.count, rows.length)
  assert.ok(rows.length >= 4)
  const row = rows.filter(function (r) { return r.id === "algo:lc-00007" })[0]
  assert.ok(row)
  assert.equal(row.track, "algorithm")
  assert.equal(row.kind, "problem")
  assert.equal(row.status, "verified")
  assert.equal(row.path, "bank/interview/lc-00007.md")
  // topics / subtracks 存 JSON 数组字符串
  assert.deepEqual(JSON.parse(row.topics), ["数组"])
  assert.deepEqual(JSON.parse(row.subtracks), ["interview"])
})

test("refreshUnitIndex 是整体覆盖语义", function () {
  const opts = makeTree()
  const rows = []
  const fakeStore = {
    replaceUnits: function (items) {
      rows.length = 0
      items.forEach(function (item) { rows.push(item) })
    },
    listUnits: function () {
      return rows.slice()
    },
  }
  index.refreshUnitIndex(fakeStore, opts)
  const first = rows.length
  // 树里删掉一份题库文件再刷新，行数应当变少（不是追加）
  fs.unlinkSync(path.join(opts.learningRoot, "bank", "interview", "lc-00008.md"))
  index.refreshUnitIndex(fakeStore, opts)
  assert.equal(rows.length, first - 1)
})
