"use strict"
/**
 * supply-algorithm-acceptance.test.js —— 交付验收 12 条自查表的机器版
 *
 * 纯文件检查 + 假 git：不联网、不编译（需要真编译的那条默认标 manual）。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const acceptance = require("../core/supply/acceptance.js")
const fill = require("../core/supply/algorithm-fill.js")

const REF = [
  "#include <iostream>",
  "int main() {",
  '  std::cout << "ok" << "\\n";',
  "  return 0;",
  "}",
].join("\n")

const NEG = [
  "#include <iostream>",
  "int main() {",
  '  std::cout << "WRONG_LINE_MARKER" << "\\n";',
  "  return 0;",
  "}",
].join("\n")

/** 内存 settings 仓储 */
function fakeStore() {
  const data = {}
  return {
    getSetting: function (key, fallback) {
      return data[key] === undefined ? fallback : data[key]
    },
    setSetting: function (key, value) {
      data[key] = String(value)
    },
  }
}

/** 干净 git */
function cleanGit() {
  return function () { return { ok: true, entries: [], reason: "" } }
}

/** 有别人改动的 git */
function dirtyGit(paths) {
  return function () {
    return { ok: true, entries: paths.map(function (p) { return "?? " + p }), reason: "" }
  }
}

/** 造一个 learning 仓：bank 里放一道空骨架（还有欠账） */
function makeRepo(tag, opts) {
  const o = opts || {}
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p5-acc-" + tag + "-"))
  const bank = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(bank, { recursive: true })
  const skeleton = [
    "---", "id: lc-00001", "source: leetcode", "title: 空骨架", "difficulty: easy",
    "topics: [字符串]", "tracks: [fundamentals]", "status: todo", "---", "",
    "## 题面", "", "", "## 测试用例", "", "| input | expect |", "|---|---|", "", "## 参考解法", "",
  ].join("\n")
  fs.writeFileSync(path.join(bank, "lc-00001.md"), skeleton, "utf8")
  return { root: root, unitsDir: path.join(root, "units", "algorithm") }
}

/** 写一个合格的已交付单元 */
function writeDelivered(unitsDir, stem) {
  fill.writeTriplet({
    unitsDir: unitsDir,
    stem: stem,
    draft: {
      stem: stem,
      title: "已交付单元",
      difficulty: "easy",
      topics: ["字符串"],
      subtracks: ["fundamentals"],
      source: "leetcode",
      unitSource: "leetcode",
      sourceId: "7",
      statement: "给你一个字符串。",
      cases: [{ input: "1", expect: "1" }],
      solution: REF,
      negative: NEG,
      status: "verified",
    },
  })
}

test("全部齐备且干净 → 12 条里没有不合格项（3/4/11 标 manual）", async function () {
  const repo = makeRepo("ok")
  try {
    writeDelivered(repo.unitsDir, "lc-00001")
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: cleanGit(),
    })
    assert.equal(res.ok, true, JSON.stringify(res.rows.filter(function (r) { return r.status === "bad" })))
    const manual = res.rows.filter(function (r) { return r.status === "manual" })
    // 没跑真编译 + 没跑全量测试 + 没让机器跑 test 命令
    assert.equal(manual.length, 3, JSON.stringify(manual.map(function (r) { return r.no })))
    // 字符串排序（11 会排在 3 前面），这里只比集合
    assert.deepEqual(
      manual.map(function (r) { return r.no }).sort(function (a, b) { return a - b }),
      [3, 4, 11]
    )
    assert.equal(res.counts.md, 1)
    assert.equal(res.counts.debt, 0)
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("单元还没补齐 → 第 1/5/6 条不合格", async function () {
  const repo = makeRepo("debt")
  try {
    // 仓库里已有一道空骨架，不补单元 → md 0，debt 1
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: cleanGit(),
    })
    assert.equal(res.ok, false)
    const bad = res.rows.filter(function (r) { return r.status === "bad" }).map(function (r) { return r.no })
    assert.ok(bad.indexOf(1) !== -1, JSON.stringify(bad))
    assert.ok(bad.indexOf(5) !== -1, JSON.stringify(bad))
    assert.ok(bad.indexOf(6) !== -1, JSON.stringify(bad))
    assert.equal(res.counts.debt, 1)
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("learning 仓有别人改动 → 第 7 条不合格", async function () {
  const repo = makeRepo("dirty")
  try {
    writeDelivered(repo.unitsDir, "lc-00009")
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: dirtyGit(["notes/x.md"]),
    })
    const row7 = res.rows.find(function (r) { return r.no === 7 })
    assert.equal(row7.status, "bad", JSON.stringify(row7))
    assert.ok(row7.detail.indexOf("notes/x.md") !== -1, row7.detail)
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("本计划产物在外面、别人没动 → 第 12 条算通过", async function () {
  const repo = makeRepo("own")
  try {
    writeDelivered(repo.unitsDir, "lc-00009")
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: function () {
        return { ok: true, entries: ["?? units/algorithm/lc-00009.md"], reason: "" }
      },
    })
    const row12 = res.rows.find(function (r) { return r.no === 12 })
    assert.equal(row12.status, "ok", JSON.stringify(row12))
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("三件套缺一份 → 第 2/8 条不合格", async function () {
  const repo = makeRepo("orphan")
  try {
    writeDelivered(repo.unitsDir, "lc-00009")
    fs.rmSync(path.join(repo.unitsDir, "lc-00009.negative.cpp"))
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: cleanGit(),
    })
    const bad = res.rows.filter(function (r) { return r.status === "bad" }).map(function (r) { return r.no })
    assert.ok(bad.indexOf(2) !== -1, JSON.stringify(bad))
    assert.ok(bad.indexOf(8) !== -1, JSON.stringify(bad))
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("renderAcceptance 打得出一张带 ✅/❌/… 的中文表", async function () {
  const repo = makeRepo("render")
  try {
    writeDelivered(repo.unitsDir, "lc-00009")
    const res = await acceptance.runAcceptance({
      learningRoot: repo.root,
      unitsDir: repo.unitsDir,
      store: fakeStore(),
      gitRunner: cleanGit(),
    })
    const text = acceptance.renderAcceptance(res)
    assert.ok(text.indexOf("算法轨交付验收") !== -1, text)
    assert.ok(text.indexOf("✅") !== -1, text)
    assert.ok(text.indexOf("…") !== -1, text)
    assert.ok(text.indexOf("12 条") !== -1, text)
    assert.ok(text.indexOf("需要人确认") !== -1, text)
  } finally {
    fs.rmSync(repo.root, { recursive: true, force: true })
  }
})

test("ROWS 正好 12 条，编号连续", function () {
  assert.equal(acceptance.ROWS.length, 12)
  acceptance.ROWS.forEach(function (row, index) {
    assert.equal(row.no, index + 1)
    assert.ok(row.name.length > 0)
  })
})
