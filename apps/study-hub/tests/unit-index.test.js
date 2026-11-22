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
    "difficulty: easy",
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
  // 与 loader C 的单元文件撞 id 的题库 md：id 归一化后也是 algo:mvcc。
  // 契约 §3.4 的「C → A」去重优先级就靠这一对夹具钉住。
  fs.writeFileSync(path.join(bankDir, "mvcc.md"), makeProblemMd("mvcc", "MVCC 题库旧版", true), "utf8")
  // learning 题库与工程仓本地题库各有一份同 id 的 md：钉死 loader A 内部的先后顺序。
  fs.writeFileSync(path.join(bankDir, "dup.md"), makeProblemMd("dup", "学习仓版本", true), "utf8")

  // 工程仓的本地题库
  const localDir = path.join(root, "app", "bank", "engineering")
  fs.mkdirSync(localDir, { recursive: true })
  fs.writeFileSync(path.join(localDir, "cf-12-d.md"), makeProblemMd("cf-12-d", "舞会", true), "utf8")
  fs.writeFileSync(path.join(localDir, "dup.md"), makeProblemMd("dup", "本地仓版本", true), "utf8")

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

  // loader C 的算法轨单元文件：id 与 learning 题库里的 mvcc.md 撞车（都是 algo:mvcc），
  // 谁留下来就说明「C → A」的优先级还活着。
  fs.mkdirSync(path.join(unitsDir, "algorithm"), { recursive: true })
  fs.writeFileSync(
    path.join(unitsDir, "algorithm", "mvcc.md"),
    [
      "---",
      "id: algo:mvcc",
      "track: algorithm",
      "kind: problem",
      "title: MVCC",
      "graded: script",
      "script: units/algorithm/mvcc.assert.py",
      "status: verified",
      "---",
      "",
      "## 题面",
      "",
      "MVCC 单元文件版。",
      "",
    ].join("\n"),
    "utf8"
  )

  // grokking 卡
  const cardDir = path.join(root, "learning", "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC 卡\n\n正文\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")

  // 第二张卡：id 不与 loader C 的单元文件撞车（db:transaction），
  // loader B 的成功路径就靠这张卡来验收。
  const txCardDir = path.join(root, "learning", "scaffold", "grokking", "transaction")
  fs.mkdirSync(txCardDir, { recursive: true })
  fs.writeFileSync(path.join(txCardDir, "README.md"), "# 事务卡\n\n事务正文\n", "utf8")
  fs.writeFileSync(path.join(txCardDir, "main.py"), "print('ok')\n", "utf8")

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

test("单元目录存在却读不到 → 进 warnings（中文），不静默吞掉", function () {
  // 「目录不存在」静默是对的（learning/units/ 在 Task 10 之前根本不存在，
  // 否则启动日志会被几十条警告淹没）。但「目录存在却读不到」（路径其实是个文件）
  // 以前被一并静默 → 单元会无声无息地从索引里消失，没有任何诊断线索。
  const unitsDir = tmpDir("study-hub-units-")
  // 把 <unitsDir>/database 做成**文件**而不是目录：readdirSync 报 ENOTDIR，不是 ENOENT。
  fs.writeFileSync(path.join(unitsDir, "database"), "这不是目录\n", "utf8")
  const res = index.loadUnits({ unitsDir: unitsDir })
  const badDir = path.join(unitsDir, "database")
  assert.ok(
    res.warnings.indexOf("单元目录读不到：" + badDir) !== -1,
    JSON.stringify(res.warnings)
  )
  // 目录不存在的那一支仍然静默：只有 database 这一条警告
  assert.equal(res.warnings.length, 1)
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

// —— 以下是评审 I-1 / I-2 / I-3 / M-6 补的用例 ——

test("loader B 的成功路径：卡目录产出的单元字段都对", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  // db:transaction 这张卡的 id 不与 loader C 的单元文件撞车，所以它一定活到了 byId 里
  const unit = res.byId["db:transaction"]
  assert.ok(unit, JSON.stringify(Object.keys(res.byId)))
  assert.equal(unit.track, "database")
  assert.equal(unit.kind, "card")
  // path 相对内容根，带结尾斜杠（G42：必填且非空）
  assert.equal(unit.path, "scaffold/grokking/transaction/")
  assert.equal(unit.source, "grokking")
  // 补齐设计 §3.2：loader B 单独加载时最多给 draft。
  // verified 只能由 loader C 的单元文件承载（它才是 verify 跑过执行器与变异测试的裁决）。
  assert.equal(unit.status, "draft")
  assert.equal(unit.title, "事务卡")
})

test("loader C 优先级高于 loader A：同一个 id 保留 C 的", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  // units/algorithm/mvcc.md（loader C）与 learning/bank/interview/mvcc.md（loader A）撞 id
  const unit = res.byId["algo:mvcc"]
  assert.ok(unit, JSON.stringify(Object.keys(res.byId)))
  // 保留下来的是 C 那份：标题与 path 都是单元文件里的
  assert.equal(unit.title, "MVCC")
  assert.equal(unit.path, "units/algorithm/mvcc.md")
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("单元 id 重复：algo:mvcc（保留先出现的）") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("loader A 内部顺序：learning 题库先于工程仓本地题库", function () {
  const opts = makeTree()
  const res = index.loadUnits(opts)
  // 两份 md 同 id（归一化后都是 algo:dup），分别在 learning 题库与工程仓本地题库里
  const unit = res.byId["algo:dup"]
  assert.ok(unit, JSON.stringify(Object.keys(res.byId)))
  // 先到先得：learning 题库那份在前
  assert.equal(unit.title, "学习仓版本")
  assert.equal(unit.path, "bank/interview/dup.md")
  assert.ok(
    res.warnings.some(function (w) {
      return w.indexOf("单元 id 重复：algo:dup（保留先出现的）") !== -1
    }),
    JSON.stringify(res.warnings)
  )
})

test("物化行带 title / difficulty / updated 三列", function () {
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
  const row = rows.filter(function (r) { return r.id === "algo:lc-00007" })[0]
  assert.ok(row)
  assert.equal(row.title, "反转整数")
  // 夹具的 difficulty 特意取 easy（不是 medium）：以前夹具本身就是 medium，
  // 物化链路上「合法难度原样透传」这一条**零覆盖**——
  // 把 difficultyOf 改成「无论合法非法一律返回 medium」，测试依然全绿。
  assert.equal(row.difficulty, "easy")
  // updated 是 ISO 时间戳
  assert.ok(/^\d{4}-\d{2}-\d{2}T/.test(row.updated), String(row.updated))
})

test("preloaded 缺 warnings 时返回空数组，不返回 undefined", function () {
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
  // 调用方没带 warnings：返回值也必须是数组，否则调用方 res.warnings.length 直接炸
  const res = index.refreshUnitIndex(fakeStore, null, {
    units: [
      {
        id: "algo:x",
        track: "algorithm",
        kind: "problem",
        title: "X",
        path: "bank/x.md",
        status: "todo",
        topics: [],
        difficulty: "medium",
        subtracks: [],
      },
    ],
  })
  assert.equal(res.count, 1)
  assert.ok(Array.isArray(res.warnings), String(res.warnings))
  assert.deepEqual(res.warnings, [])
})

// —— 以下是 Task 9 补的用例：核对 refreshUnitIndex 的第三个参数 preloaded 真的生效 ——

test("refreshUnitIndex 收到 preloaded 时不重新扫盘", function () {
  // 造一个「坏单元文件」目录：loader C 真扫到它，warnings 必然非空。
  // 目录层级要对：loader C 扫的是 <unitsDir>/<track>/，所以坏文件放在 <unitsDir>/database/ 下。
  const unitsDir = tmpDir("study-hub-units-")
  const brokenDir = path.join(unitsDir, "database")
  fs.mkdirSync(brokenDir, { recursive: true })
  fs.writeFileSync(path.join(brokenDir, "broken.md"), "## 讲解\n\n没有 frontmatter。\n", "utf8")

  const rows = []
  const fakeStore = {
    replaceUnits: function (r) {
      rows.push.apply(rows, r)
    },
  }
  const preloaded = {
    units: [
      {
        id: "db:mvcc",
        track: "database",
        kind: "card",
        title: "MVCC",
        path: "units/database/mvcc.md",
        status: "verified",
        topics: [],
        difficulty: "medium",
        subtracks: [],
      },
    ],
    warnings: [],
    byId: {},
  }
  const res = index.refreshUnitIndex(fakeStore, { unitsDir: unitsDir }, preloaded)
  assert.equal(res.count, 1)
  assert.equal(rows.length, 1)
  assert.equal(rows[0].id, "db:mvcc")
  assert.equal(rows[0].path, "units/database/mvcc.md")
  // 关键断言：给了 preloaded，就**不该**去扫 unitsDir。那个目录里躺着一个坏单元文件，
  // 真扫了 loader C 必然报「缺少 frontmatter」「单元 id 非法」，warnings 就非空了。
  assert.deepEqual(res.warnings, [])
})
