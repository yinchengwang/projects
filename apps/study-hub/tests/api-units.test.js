"use strict"
/**
 * api-units.test.js —— 单元查询接口
 *
 * 契约 §8.1：GET /api/units 与 GET /api/units/:id。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一个 learning 仓：一道算法题 + 一张 grokking 卡 + 一个新单元文件。 */
function tempLearning() {
  const root = tmpDir("study-hub-learning-")

  const bankDir = path.join(root, "bank", "interview")
  fs.mkdirSync(bankDir, { recursive: true })
  fs.writeFileSync(
    path.join(bankDir, "lc-00007.md"),
    [
      "---",
      "id: lc-00007",
      "source: leetcode",
      "title: 反转整数",
      "difficulty: medium",
      "topics: [数学, 字符串]",
      "tracks: [interview]",
      "status: todo",
      "---",
      "## 题面",
      "",
      "给你一个整数 x，返回它反转后的整数。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "| 123 | 321 |",
      "",
      "## 参考解法",
      "",
      "```cpp",
      "int reverse(int x) { return 0; }",
      "```",
      "",
    ].join("\n"),
    "utf8"
  )

  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC：版本链与 ReadView\n\n讲解正文。\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")

  // 第二道算法题：只有题面、没有测试用例 → loader A 判它是 draft。
  // 有了它，「按 status 过滤」那条测试才不是空集恒真。
  const draftDir = path.join(root, "bank", "interview")
  fs.writeFileSync(
    path.join(draftDir, "lc-00008.md"),
    [
      "---",
      "id: lc-00008",
      "source: leetcode",
      "title: 回文数",
      "difficulty: easy",
      "topics: [数学]",
      "tracks: [interview]",
      "status: todo",
      "---",
      "## 题面",
      "",
      "给你一个整数 x，判断它是不是回文数。",
      "",
    ].join("\n"),
    "utf8"
  )

  return root
}

async function withApp(fn) {
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: tempLearning(),
    localBank: null,
  })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port)
  } finally {
    await app.close()
  }
}

test("GET /api/units 列出算法题与卡", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(typeof body.total, "number")
    assert.ok(body.total >= 2, JSON.stringify(body))
    const ids = body.items.map(function (item) {
      return item.id
    })
    assert.ok(ids.indexOf("algo:lc-00007") !== -1, JSON.stringify(ids))
    assert.ok(ids.indexOf("db:mvcc") !== -1, JSON.stringify(ids))
  })
})

test("列表项带 track / kind / status / topics / subtracks", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units")
    const body = await res.json()
    const item = body.items.filter(function (x) {
      return x.id === "algo:lc-00007"
    })[0]
    assert.ok(item)
    assert.equal(item.track, "algorithm")
    assert.equal(item.kind, "problem")
    assert.equal(item.status, "verified")
    assert.deepEqual(item.topics, ["数学", "字符串"])
    assert.deepEqual(item.subtracks, ["interview"])
  })
})

test("按 track / kind / status 过滤", async function () {
  await withApp(async function (base) {
    const db = await (await fetch(base + "/api/units?track=database")).json()
    assert.ok(db.items.every(function (x) { return x.track === "database" }))
    assert.ok(db.items.length >= 1)

    const kind = await (await fetch(base + "/api/units?kind=problem")).json()
    // 长度断言必须有：只有 every 的话，过滤实现错了返回空集，测试照样绿。
    assert.ok(kind.items.length >= 1, JSON.stringify(kind.items))
    assert.ok(kind.items.every(function (x) { return x.kind === "problem" }))

    const status = await (await fetch(base + "/api/units?status=draft")).json()
    assert.ok(status.items.length >= 1, JSON.stringify(status.items))
    assert.ok(status.items.every(function (x) { return x.status === "draft" }))
  })
})

test("q 按标题搜索", async function () {
  await withApp(async function (base) {
    const body = await (await fetch(base + "/api/units?q=MVCC")).json()
    assert.equal(body.total, 1)
    assert.equal(body.items[0].id, "db:mvcc")
  })
})

test("limit 截断；limit 非正整数按不限制处理", async function () {
  await withApp(async function (base) {
    const one = await (await fetch(base + "/api/units?limit=1")).json()
    assert.equal(one.items.length, 1)
    assert.equal(one.total >= 1, true)

    const all = await (await fetch(base + "/api/units?limit=0")).json()
    assert.equal(all.items.length, all.total)
  })
})

test("GET /api/units/:id 返回全字段与 statement_html", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units/algo:lc-00007")
    assert.equal(res.status, 200)
    const body = await res.json()
    assert.equal(body.unit.id, "algo:lc-00007")
    assert.equal(body.unit.track, "algorithm")
    assert.equal(body.unit.kind, "problem")
    assert.equal(body.unit.entry.graded, "user")
    assert.ok(body.unit.asserts.length === 1)
    assert.ok(body.statement_html.indexOf("<p>") !== -1, body.statement_html)
  })
})

test("单元不存在 → 404 且带 id", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units/algo:不存在的题")
    assert.equal(res.status, 404)
    const body = await res.json()
    assert.equal(body.error, "单元不存在")
    assert.equal(body.id, "algo:不存在的题")
  })
})

test("旧接口没有回归", async function () {
  await withApp(async function (base) {
    const health = await fetch(base + "/api/health")
    assert.equal(health.status, 200)
    const body = await health.json()
    assert.equal(body.ok, true)

    const bank = await fetch(base + "/api/bank")
    assert.equal(bank.status, 200)
  })
})
