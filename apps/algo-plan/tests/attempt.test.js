"use strict"
/**
 * attempt.test.js —— POST /api/attempt：提交代码 → 真实判题 → 记流水
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")
const manifest = require("../fixtures/judge/manifest.js")
const { tmpDir } = require("./helpers/tmp.js")

/** 造一个带一道有用例的题的 learning 仓 */
function tempLearning() {
  const root = tmpDir("algo-plan-attempt-")
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(manifest.PROBLEMS.cppSum, path.join(dir, "self-00010.md"))
  return root
}

async function withApp(fn) {
  const app = createApp({ dbPath: ":memory:", learningRoot: tempLearning(), localBank: null })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, app)
  } finally {
    await app.close()
  }
}

function post(base, body) {
  return fetch(base + "/api/attempt", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  })
}

function cppCode() {
  return fs.readFileSync(manifest.CODE.cppOk, "utf8")
}

test("POST /api/attempt 判一道 C++ 题，响应形状是契约冻结的八个字段", async function () {
  await withApp(async function (base) {
    const res = await post(base, {
      problemId: "self-00010",
      code: cppCode(),
      language: "cpp",
      mode: "solve",
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.deepStrictEqual(
      Object.keys(body).sort(),
      [
        "attemptId", "compilerOutput", "durationMs", "failedCase",
        "message", "passedCases", "totalCases", "verdict",
      ].sort()
    )
    assert.strictEqual(body.verdict, "pass")
    assert.strictEqual(body.passedCases, 3)
    assert.strictEqual(body.totalCases, 3)
    assert.strictEqual(body.failedCase, null)
    assert.strictEqual(body.compilerOutput, "")
    assert.ok(body.message.indexOf("通过") !== -1, body.message)
    assert.ok(body.attemptId > 0, "要返回流水 id")
  })
})

test("作答流水真的写进 SQLite", async function () {
  await withApp(async function (base, app) {
    await post(base, { problemId: "self-00010", code: cppCode(), language: "cpp", mode: "review" })
    assert.strictEqual(app.store.countAttempts(), 1)
    const rows = app.store.listAttemptsByProblem("self-00010")
    assert.strictEqual(rows.length, 1)
    assert.strictEqual(rows[0].verdict, "pass")
    assert.strictEqual(rows[0].mode, "review")
    assert.strictEqual(rows[0].language, "cpp")
    assert.strictEqual(Number(rows[0].passed_cases), 3)
  })
})

test("题目不存在返回 404", async function () {
  await withApp(async function (base) {
    const res = await post(base, { problemId: "self-99999", code: "int main(){}", language: "cpp" })
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "题目不存在")
  })
})

test("代码为空返回 400", async function () {
  await withApp(async function (base) {
    const res = await post(base, { problemId: "self-00010", code: "   ", language: "cpp" })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.strictEqual(body.error, "代码不能为空")
  })
})

test("不支持的语言返回 400", async function () {
  await withApp(async function (base) {
    const res = await post(base, { problemId: "self-00010", code: "print(1)", language: "rust" })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error.indexOf("rust") !== -1, body.error)
  })
})

test("非法 mode 返回 400", async function () {
  await withApp(async function (base) {
    const res = await post(base, { problemId: "self-00010", code: cppCode(), language: "cpp", mode: "随便写" })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error.indexOf("mode") !== -1, body.error)
  })
})

test("没有测试用例的题目返回 400 并说清原因", async function () {
  const root = tmpDir("algo-plan-attempt-")
  const dir = path.join(root, "bank", "interview")
  fs.mkdirSync(dir, { recursive: true })
  fs.writeFileSync(
    path.join(dir, "self-00011.md"),
    [
      "---",
      "id: self-00011",
      "source: self",
      "title: 没有用例的题",
      "difficulty: easy",
      "topics: [数组]",
      "tracks: [interview]",
      "status: todo",
      "needs_fill: true",
      "created: 2026-11-11",
      "updated: 2026-11-11",
      "---",
      "## 题面",
      "",
      "没有用例。",
      "",
      "## 测试用例",
      "",
      "| input | expect |",
      "|---|---|",
      "",
    ].join("\n"),
    "utf8"
  )
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    const res = await post("http://127.0.0.1:" + port, {
      problemId: "self-00011",
      code: "int main(){}",
      language: "cpp",
    })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error.indexOf("测试用例") !== -1, body.error)
  } finally {
    await app.close()
  }
})

test("编译错误：verdict 与编译器原文都回给前端", async function () {
  await withApp(async function (base) {
    const res = await post(base, {
      problemId: "self-00010",
      code: fs.readFileSync(manifest.CODE.cppCompileError, "utf8"),
      language: "cpp",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "compile_error")
    assert.ok(body.compilerOutput.indexOf("error:") !== -1, body.compilerOutput)
    assert.ok(body.message.indexOf("第 4 行") !== -1, "要给出正确的错误行号：" + body.message)
  })
})

test("超时：settings 里的 judge.timeout_ms 生效", async function () {
  await withApp(async function (base, app) {
    app.store.setSetting("judge.timeout_ms", "800")
    const res = await post(base, {
      problemId: "self-00010",
      code: fs.readFileSync(manifest.CODE.cppTimeout, "utf8"),
      language: "cpp",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "timeout")
    assert.ok(body.message.indexOf("800") !== -1, body.message)
  })
})

test("Python 提交走真实解释器，判为 pass", async function () {
  const root = tmpDir("algo-plan-attempt-")
  const dir = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(manifest.PROBLEMS.pythonReverse, path.join(dir, "self-00011.md"))
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    const res = await post("http://127.0.0.1:" + port, {
      problemId: "self-00011",
      code: fs.readFileSync(manifest.CODE.pythonOk, "utf8"),
      language: "python",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "pass")
    assert.strictEqual(body.passedCases, 2)
    assert.ok(app.store.getSetting("judge.python.cmd"), "探测到的解释器要缓存进 settings")
  } finally {
    await app.close()
  }
})
