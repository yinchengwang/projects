"use strict"
/**
 * api-m4.test.js —— M4 的接口：下一题 / 评分 / 轮次 / 勾选写回 / 错题订正 / 今日进度
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { createApp } = require("../server/app.js")

const PROBLEM_MD = [
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
  "",
  "## 参考解法",
  "",
  "```cpp",
  "int main() { return 0; }",
  "```",
  "",
].join("\n")

const ROUND_MD = [
  "# R33 Grokking 算法篇 — 学习进度",
  "",
  "## 基本信息",
  "",
  "| 字段 | 值 |",
  "|------|-----|",
  "| 开始时间 | 2026-07-12 |",
  "| 目标 | Grokking 算法篇 16 张卡片 |",
  "| 状态 | TBD-deferred |",
  "",
  "## 卡片清单",
  "",
  "| 序号 | ID | 名称 | 状态 | 完成时间 |",
  "|------|-----|------|------|----------|",
  "| 1 | two_sum | 两数之和 | todo |  |",
  "| 2 | arrays | 数组技巧 | todo |  |",
  "",
].join("\n")

/** 造一个干净的临时 learning 仓（git 仓库 + 题库 + 轮次） */
function tempLearning() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-m4-"))
  const bank = path.join(root, "bank", "interview")
  fs.mkdirSync(bank, { recursive: true })
  fs.writeFileSync(path.join(bank, "lc-00001.md"), PROBLEM_MD, "utf8")
  const progress = path.join(root, "progress")
  fs.mkdirSync(progress, { recursive: true })
  fs.writeFileSync(path.join(progress, "r33-progress.md"), ROUND_MD, "utf8")

  execFileSync("git", ["init", "-q"], { cwd: root })
  execFileSync("git", ["config", "user.email", "test@example.com"], { cwd: root })
  execFileSync("git", ["config", "user.name", "test"], { cwd: root })
  execFileSync("git", ["add", "-A"], { cwd: root })
  execFileSync("git", ["-c", "commit.gpgsign=false", "commit", "-q", "-m", "初始化"], { cwd: root })
  return root
}

async function withApp(fn) {
  const root = tempLearning()
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, root)
  } finally {
    await app.close()
  }
}

test("GET /api/next 返回契约要求的字段", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/next")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok("action" in body)
    assert.ok("reviewQueueLength" in body)
    assert.ok(body.action, "应该给出一个动作")
    // 契约 §3 要求的字段一个都不能少；额外字段（roundId / itemId）是允许的增量
    const keys = Object.keys(body.action)
    ;["estimatedMinutes", "kind", "problemId", "reason", "reviewQueueLength", "title"].forEach(
      function (key) {
        assert.ok(keys.indexOf(key) !== -1, "缺少字段 " + key)
      }
    )
    assert.strictEqual(body.action.kind, "round")
    assert.strictEqual(body.action.problemId, "lc-00001")
    assert.strictEqual(body.action.title, "两数之和")
    assert.ok(body.action.estimatedMinutes > 0)
    // 轮次分支要带上轮次号与条目号，前端才知道去轮次页勾掉哪一张卡片
    assert.strictEqual(body.action.roundId, "r33")
    assert.strictEqual(body.action.itemId, "two_sum")
  })
})

test("GET /api/next 在没有题也没有轮次时 action 为 null", async function () {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-empty-"))
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/next")
    const body = await res.json()
    assert.strictEqual(body.action, null)
    assert.strictEqual(body.reviewQueueLength, 0)
  } finally {
    await app.close()
  }
})

test("POST /api/review/:id/grade 返回下一次复习时间等五个字段", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/review/lc-00001/grade", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ quality: 5 }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.deepStrictEqual(Object.keys(body).sort(), [
      "easiness",
      "intervalDays",
      "lapses",
      "nextReviewAt",
      "problemId",
    ].sort())
    assert.strictEqual(body.problemId, "lc-00001")
    assert.strictEqual(body.intervalDays, 1)
    assert.strictEqual(body.lapses, 0)
    assert.ok(Math.abs(body.easiness - 2.6) < 1e-9, String(body.easiness))
    assert.ok(!Number.isNaN(Date.parse(body.nextReviewAt)))
  })
})

test("POST /api/review/:id/grade 拒绝非法 quality", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/review/lc-00001/grade", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ quality: 9 }),
    })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error, "要给出中文错误说明")
  })
})

test("GET /api/rounds 返回轮次与告警", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/rounds")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(Array.isArray(body.warnings))
    assert.strictEqual(body.rounds.length, 1)
    const round = body.rounds[0]
    assert.strictEqual(round.id, "r33")
    assert.strictEqual(round.title, "Grokking 算法篇 — 学习进度")
    assert.strictEqual(round.status, "active")
    assert.ok(round.goal.indexOf("Grokking") !== -1, round.goal)
    assert.strictEqual(round.items.length, 2)
    assert.strictEqual(round.items[0].id, "two_sum")
    assert.strictEqual(round.items[0].status, "todo")
    assert.ok("file" in round)
  })
})

test("PATCH /api/rounds/:id/items/:itemId 只改目标行并返回 diff", async function () {
  await withApp(async function (base, root) {
    const file = path.join(root, "progress", "r33-progress.md")
    const before = fs.readFileSync(file, "utf8").split("\n")

    const res = await fetch(base + "/api/rounds/r33/items/arrays", {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ status: "done", doneDate: "2026-11-11" }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok("diff" in body)
    assert.ok(Array.isArray(body.warnings))
    assert.ok(body.diff.indexOf("arrays") !== -1, body.diff)

    const after = fs.readFileSync(file, "utf8").split("\n")
    let changed = 0
    for (let i = 0; i < before.length; i += 1) {
      if (before[i] !== after[i]) changed += 1
    }
    assert.strictEqual(changed, 1, "只应改一行")
  })
})

test("learning 仓不干净时写回被拒绝并指出被动过的文件", async function () {
  await withApp(async function (base, root) {
    fs.writeFileSync(path.join(root, "随手改的.md"), "脏一下\n", "utf8")
    const res = await fetch(base + "/api/rounds/r33/items/arrays", {
      method: "PATCH",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ status: "done" }),
    })
    assert.strictEqual(res.status, 409)
    const body = await res.json()
    assert.ok(body.error.indexOf("不干净") !== -1, body.error)
    assert.ok(Array.isArray(body.dirtyFiles))
    assert.ok(body.dirtyFiles.length > 0)
  })
})

test("POST /api/correction 写入订正笔记并返回 diff", async function () {
  await withApp(async function (base, root) {
    const res = await fetch(base + "/api/correction", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "lc-00001", text: "先查再插，别先插再查。" }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok("file" in body)
    assert.ok("diff" in body)
    const file = path.join(root, "notes", "practice", "lc-00001-订正.md")
    assert.strictEqual(body.file, file)
    assert.ok(fs.existsSync(file), "订正笔记要真的写出来")
    const text = fs.readFileSync(file, "utf8")
    assert.ok(text.indexOf("先查再插") !== -1)
    assert.ok(text.indexOf("problem: lc-00001") !== -1)
  })
})

test("GET /api/progress 给出今日进度与配额", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/progress")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok("date" in body)
    assert.strictEqual(typeof body.solved, "number")
    assert.strictEqual(typeof body.reviewed, "number")
    assert.strictEqual(typeof body.reviewMinutes, "number")
    assert.strictEqual(body.quota.limitQuestions, 10)
    assert.strictEqual(body.quota.limitMinutes, 20)
    assert.strictEqual(body.quota.exhausted, false)
  })
})

test("M3 的 POST /api/attempt 没有被 M4 改掉（跨里程碑回归闸门）", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/attempt", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "不存在的题", code: "", language: "cpp" }),
    })
    const body = await res.json()
    // 路由被删掉时，走的是「接口不存在」分支；路由还在时，走的是 M3 的业务分支
    // （题不存在 → 404 +「题目不存在」；代码为空 → 400 +「代码不能为空」）。
    assert.notStrictEqual(
      body.error,
      "接口不存在",
      "/api/attempt 被 M4 误删了——检查 buildRoutes 是否被整体替换"
    )
  })
})
