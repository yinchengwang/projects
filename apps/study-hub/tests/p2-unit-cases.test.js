"use strict"
/**
 * p2-unit-cases.test.js —— /api/units/:id 的「样例用例只暴露 2 条 + 不泄露参考解法」
 *
 * 这两条保护原来由旧的 `GET /api/problem/:id` 扛着。实测发现新路由 `/api/units/:id`
 * 把 `unit.asserts` 全量返回——`algo:self-00010` 的 3 条 `input/expect` 全给了，
 * 比旧路由更漏。旧路由下线之后，这两条保护就只在这里了（契约 §8.1.1）。
 *
 * 本套件钉住契约 §8.1.1 的六条规则：
 *   1. kind=problem 必须带 cases，形状 {sample:[{input,expect}], total:number}
 *   2. cases.sample 最多 2 条，且与 unit.asserts 前 2 条同源同值
 *   3. kind=card 不带 cases
 *   4. 响应全文里不许出现参考解法（statement 不含、entry.reference 为空）
 *   5. kind=problem 时 unit.asserts 也裁到前 2 条——否则第 3 条用例会从
 *      unit.asserts 里漏出去，「第 3 条不外泄」这条保护就是假的
 *   6. 「一共几条断言」读 cases.total，不读 unit.asserts.length
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一个 learning 仓：一道 3 条用例的算法题（带参考解法）+ 一张 card */
function tempLearning() {
  const root = tmpDir("study-hub-cases-")

  const bankDir = path.join(root, "bank", "interview")
  fs.mkdirSync(bankDir, { recursive: true })
  fs.writeFileSync(
    path.join(bankDir, "lc-00009.md"),
    [
      "---",
      "id: lc-00009",
      "source: leetcode",
      "title: 三数之和",
      "difficulty: medium",
      "topics: [数组]",
      "tracks: [interview]",
      "status: todo",
      "---",
      "## 题面",
      "",
      "给定数组，找三个数。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "| 1 2 3 | 0 1 2 |",
      "| 4 5 6 | 0 1 2 |",
      "| 7 8 9 | -1 -1 -1 |",
      "",
      "## 参考解法",
      "",
      "```cpp",
      "int main() { /* 这是绝不应该出现在响应里的参考解法 */ return 0; }",
      "```",
      "",
    ].join("\n"),
    "utf8"
  )

  const cardDir = path.join(root, "scaffold", "grokking", "mvcc")
    fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "README.md"), "# MVCC\n\n讲解正文。\n", "utf8")
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('ok')\n", "utf8")

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

test("kind=problem：带 cases，sample 最多 2 条，total 是全量", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units/algo:lc-00009")
    const body = await res.json()
    assert.ok(body.cases, "problem 类必须带 cases")
    assert.strictEqual(body.cases.total, 3, "total 是全部用例条数")
    assert.strictEqual(body.cases.sample.length, 2, "sample 最多 2 条")
    assert.deepStrictEqual(Object.keys(body.cases.sample[0]).sort(), ["expect", "input"])
  })
})

test("kind=problem：cases.sample 与 unit.asserts 前 2 条同源同值", async function () {
  await withApp(async function (base) {
    const body = await (await fetch(base + "/api/units/algo:lc-00009")).json()
    assert.strictEqual(body.unit.asserts.length, 2, "asserts 也裁到 2 条（与 sample 一样多）")
    body.cases.sample.forEach(function (item, i) {
      assert.strictEqual(item.input, body.unit.asserts[i].input, "第 " + i + " 条 input 要同源")
      assert.strictEqual(item.expect, body.unit.asserts[i].expect, "第 " + i + " 条 expect 要同源")
    })
  })
})

test("kind=problem：第 3 条用例的 input/expect 不在响应里（asserts 也裁了）", async function () {
  await withApp(async function (base) {
    const raw = await (await fetch(base + "/api/units/algo:lc-00009")).text()
    // 夹具第 3 条是 `| 7 8 9 | -1 -1 -1 |`
    assert.strictEqual(raw.indexOf("7 8 9"), -1, "第 3 条用例的 input 漏出去了")
    assert.strictEqual(raw.indexOf("-1 -1 -1"), -1, "第 3 条用例的 expect 漏出去了")
    // total 仍然要说真话：用户知道有 3 条，只是只看得见 2 条
    assert.strictEqual(JSON.parse(raw).cases.total, 3)
  })
})

test("kind=card：不带 cases", async function () {
  await withApp(async function (base) {
    const body = await (await fetch(base + "/api/units/db:mvcc")).json()
    assert.strictEqual(body.unit.kind, "card")
    assert.strictEqual(body.cases, undefined, "card 类不该有 cases 字段")
  })
})

test("不泄露参考解法：全文里没有参考解法的痕迹", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/units/algo:lc-00009")
    const raw = await res.text()
    assert.strictEqual(raw.indexOf("绝不应该出现在响应里的参考解法"), -1, "参考解法漏到响应里了")
    assert.strictEqual(raw.indexOf("## 参考解法"), -1, "题面里不该带参考解法小节")
    const body = JSON.parse(raw)
    assert.strictEqual(body.unit.entry.reference, "", "entry.reference 必须为空")
    assert.strictEqual(body.solution, undefined, "不该有 solution 字段")
  })
})
