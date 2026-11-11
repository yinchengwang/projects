"use strict"
/**
 * api.test.js —— M1 的三个接口：健康检查、题库列表、单题详情
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")

/** 造一个带一道题的 learning 仓 */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-learning-"))
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(
    path.join(dir, "lc-00001.md"),
    [
      "---",
      "id: lc-00001",
      "source: leetcode",
      "title: 两数之和",
      "difficulty: easy",
      "topics: [哈希表, 数组]",
      "tracks: [interview, fundamentals]",
      "status: todo",
      "needs_fill: false",
      "created: 2026-11-11",
      "updated: 2026-11-11",
      "---",
      "## 题面",
      "",
      "给定整数数组与目标值。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "| [2,7,11,15], 9 | [0,1] |",
      "| 3,2,4, 6 | 1,2 |",
      "| 3,3,0,0,0, 6 | 0,1 |",
      "",
      "## 参考解法",
      "",
      "```cpp",
      "int main() { return 0; }",
      "```",
      "",
    ].join("\n"),
    "utf8"
  )
  return root
}

async function withApp(fn, opts) {
  const app = createApp(
    Object.assign({ dbPath: ":memory:", learningRoot: tempLearning() }, opts || {})
  )
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port)
  } finally {
    await app.close()
  }
}

test("GET /api/health 返回运行状态", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/health")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.ok, true)
    assert.strictEqual(body.learning.ok, true)
    assert.strictEqual(body.bank.problemCount, 1)
    assert.strictEqual(body.attempts, 0)
  })
})

test("未设置 learning 仓时降级为纯本地模式", async function () {
  const app = createApp({ dbPath: ":memory:", learningRoot: "", env: {} })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/health")
    const body = await res.json()
    assert.strictEqual(body.learning.ok, false)
    assert.strictEqual(body.learning.root, null)
    assert.strictEqual(body.bank.problemCount, 0)
  } finally {
    await app.close()
  }
})

test("GET /api/bank 返回题目摘要", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/bank")
    const body = await res.json()
    assert.strictEqual(body.total, 1)
    assert.strictEqual(body.items[0].id, "lc-00001")
    assert.strictEqual(body.items[0].title, "两数之和")
    assert.strictEqual(body.items[0].statement, undefined, "列表不应带题面")
  })
})

test("GET /api/bank 支持 track 过滤", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/bank?track=advanced")
    const body = await res.json()
    assert.strictEqual(body.total, 0)
  })
})

test("GET /api/problem/:id 返回题面与样例用例", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/problem/lc-00001")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.id, "lc-00001")
    assert.strictEqual(body.statement, "给定整数数组与目标值。")
    assert.strictEqual(body.cases.total, 3)
    assert.strictEqual(body.cases.sample.length, 2)
  })
})

test("单题接口不泄露参考解法", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/problem/lc-00001")
    const raw = await res.text()
    assert.strictEqual(raw.indexOf("int main"), -1, "响应里不应出现参考解法")
    const body = JSON.parse(raw)
    assert.strictEqual(body.solution, undefined)
  })
})

test("单题接口最多暴露 2 个样例用例，其余用例不外泄", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/problem/lc-00001")
    const body = await res.json()
    assert.strictEqual(body.cases.total, 3)
    assert.strictEqual(body.cases.sample.length, 2)
    const raw = await fetch(base + "/api/problem/lc-00001").then(function (r) {
      return r.text()
    })
    assert.strictEqual(raw.indexOf("3,3,0,0,0"), -1, "第 3 个用例不应出现在响应里")
  })
})

test("GET /api/problem/:id 题目不存在返回 404", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/problem/lc-99999")
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "题目不存在")
  })
})
