"use strict"
/**
 * e2e.test.js —— 端到端：导入样板题 → 起服务 → 打接口 → 校验落盘
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const seed = require("../tools/seed-bank.js")

function tempLearning() {
  return fs.mkdtempSync(path.join(os.tmpdir(), "algo-plan-e2e-"))
}

test("样板题能导入 learning 仓的 bank/", function () {
  const root = tempLearning()
  const result = seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root } })
  assert.deepStrictEqual(result.errors, [])
  assert.strictEqual(result.written.length, 2)
  assert.ok(fs.existsSync(path.join(root, "bank", "interview", "lc-00001.md")))
  assert.ok(fs.existsSync(path.join(root, "bank", "fundamentals", "self-00001.md")))
})

test("重复导入默认跳过，--force 才覆盖", function () {
  const root = tempLearning()
  seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root } })
  const again = seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root } })
  assert.strictEqual(again.written.length, 0)
  assert.strictEqual(again.skipped.length, 2)

  const forced = seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root }, force: true })
  assert.strictEqual(forced.written.length, 2)
})

test("未设置 learning 仓时导入报错而不是静默成功", function () {
  const result = seed.run({ env: {} })
  assert.strictEqual(result.written.length, 0)
  assert.strictEqual(result.errors.length, 1)
})

test("端到端：导入 → 起服务 → 打接口", async function () {
  const root = tempLearning()
  seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root } })

  const app = createApp({
    dbPath: path.join(root, "app.db"),
    learningRoot: root,
    env: { ALGO_PLAN_LEARNING_ROOT: root },
  })
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    const health = await (await fetch(base + "/api/health")).json()
    assert.strictEqual(health.ok, true)
    assert.strictEqual(health.learning.ok, true)
    assert.strictEqual(health.learning.root, root)
    assert.strictEqual(health.bank.problemCount, 2)

    const bank = await (await fetch(base + "/api/bank")).json()
    assert.strictEqual(bank.total, 2)
    assert.deepStrictEqual(
      bank.items.map(function (item) {
        return item.id
      }),
      ["lc-00001", "self-00001"]
    )

    const problem = await (await fetch(base + "/api/problem/self-00001")).json()
    assert.strictEqual(problem.title, "字符串反转")
    assert.strictEqual(problem.cases.total, 2)
    assert.strictEqual(problem.solution, undefined)
  } finally {
    await app.close()
  }
})

test("端到端：SQLite 文件真的落盘了", function () {
  const root = tempLearning()
  seed.run({ env: { ALGO_PLAN_LEARNING_ROOT: root } })
  const dbPath = path.join(root, "app.db")
  const app = createApp({ dbPath: dbPath, learningRoot: root, env: {} })
  return app.close().then(function () {
    assert.ok(fs.existsSync(dbPath), "应生成 SQLite 文件")
  })
})
