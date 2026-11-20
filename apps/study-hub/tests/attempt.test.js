"use strict"
/**
 * attempt.test.js —— POST /api/units/:id/judge：提交代码 → 真实判题 → 记流水
 *
 * P2 起这个套件从旧的 POST /api/attempt 迁过来了（契约 §8.1.2.2）：
 *   - problemId 进路径了：POST /api/units/algo:self-00010/judge
 *   - mode 整个去掉（只有写、没有读的字段）
 *   - 响应形状换成契约 §8.5 冻结的 9 个键
 *   - 「题目没有用例」不再是 400，而是 200 + wrong_answer + 中文说明
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
  const root = tmpDir("study-hub-attempt-")
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

/** 判一道题：单元 id 进路径 */
function post(base, unitId, body) {
  return fetch(base + "/api/units/" + encodeURIComponent(unitId) + "/judge", {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body),
  })
}

function cppCode() {
  return fs.readFileSync(manifest.CODE.cppOk, "utf8")
}

test("POST /api/units/:id/judge 判一道 C++ 题，响应形状是契约冻结的九个字段", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-00010", {
      code: cppCode(),
      language: "cpp",
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.deepStrictEqual(
      Object.keys(body).sort(),
      [
        "durationMs", "failed", "message", "passed",
        "stderr", "total", "unit", "verdict", "stdout",
      ].sort()
    )
    assert.strictEqual(body.verdict, "pass")
    assert.strictEqual(body.passed, 3)
    assert.strictEqual(body.total, 3)
    assert.strictEqual(body.failed, null)
    assert.strictEqual(body.stdout, "")
    assert.strictEqual(body.stderr, "")
    assert.ok(body.message.indexOf("通过") !== -1, body.message)
    assert.strictEqual(body.unit.id, "algo:self-00010")
    assert.strictEqual(body.unit.kind, "problem")
  })
})

test("作答流水真的写进 SQLite", async function () {
  await withApp(async function (base, app) {
    await post(base, "algo:self-00010", { code: cppCode(), language: "cpp" })
    assert.strictEqual(app.store.countAttempts(), 1)
    const rows = app.store.listAttemptsByProblem("self-00010")
    assert.strictEqual(rows.length, 1)
    assert.strictEqual(rows[0].verdict, "pass")
    // mode 字段整个去掉了：不再有 solve / review / probe 三态，一律记默认的 solve
    assert.strictEqual(rows[0].mode, "solve")
    assert.strictEqual(rows[0].language, "cpp")
    assert.strictEqual(Number(rows[0].passed_cases), 3)
  })
})

test("单元不存在返回 404", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-99999", { code: "int main(){}", language: "cpp" })
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "单元不存在")
    assert.strictEqual(body.id, "algo:self-99999")
  })
})

test("代码为空返回 400", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-00010", { code: "   ", language: "cpp" })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.strictEqual(body.error, "代码不能为空")
  })
})

test("不支持的语言返回 400", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-00010", { code: "print(1)", language: "rust" })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error.indexOf("rust") !== -1, body.error)
    assert.deepStrictEqual(body.supported, ["cpp", "python"])
  })
})

test("mode 不再接受：传了也不看，不会因此报错", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-00010", {
      code: cppCode(),
      language: "cpp",
      mode: "随便写",
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.verdict, "pass")
  })
})

test("没有测试用例的单元：200 + wrong_answer + 中文说明（不再是 400）", async function () {
  const root = tmpDir("study-hub-attempt-")
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
    const res = await post("http://127.0.0.1:" + port, "algo:self-00011", {
      code: "int main(){}",
      language: "cpp",
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.verdict, "wrong_answer")
    assert.strictEqual(body.total, 0)
    assert.ok(body.message.indexOf("测试用例") !== -1, body.message)
  } finally {
    await app.close()
  }
})

test("编译错误：verdict 与出错行号回给前端", async function () {
  await withApp(async function (base) {
    const res = await post(base, "algo:self-00010", {
      code: fs.readFileSync(manifest.CODE.cppCompileError, "utf8"),
      language: "cpp",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "compile_error")
    assert.ok(body.message.indexOf("第 4 行") !== -1, "要给出正确的错误行号：" + body.message)
    // 编译器原文不再进 HTTP 响应（契约 §8.1.2.2）：它只在判题器内部的结果对象里
    assert.strictEqual(body.compilerOutput, undefined)
  })
})

test("超时：settings 里的 judge.timeout_ms 生效", async function () {
  await withApp(async function (base, app) {
    app.store.setSetting("judge.timeout_ms", "800")
    const res = await post(base, "algo:self-00010", {
      code: fs.readFileSync(manifest.CODE.cppTimeout, "utf8"),
      language: "cpp",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "timeout")
    assert.ok(body.message.indexOf("800") !== -1, body.message)
  })
})

test("Python 提交走真实解释器，判为 pass", async function () {
  const root = tmpDir("study-hub-attempt-")
  const dir = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(manifest.PROBLEMS.pythonReverse, path.join(dir, "self-00011.md"))
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    const res = await post("http://127.0.0.1:" + port, "algo:self-00011", {
      code: fs.readFileSync(manifest.CODE.pythonOk, "utf8"),
      language: "python",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "pass")
    assert.strictEqual(body.passed, 2)
    assert.strictEqual(body.total, 2)
  } finally {
    await app.close()
  }
})

test("judge.python.cmd：settings 里没配过时**不**回写", async function () {
  // 契约 §8.1.2.3：旧路由「无论有没有 override 都回写」，会把一次自动探测的结果
  // 钉死进 settings；而 settings 的优先级高于 STUDY_HUB_PYTHON，钉死之后用户
  // 再设环境变量就不生效了。所以新路由只在「原本配的那条跑不起来」时才回写。
  const root = tmpDir("study-hub-attempt-")
  const dir = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(manifest.PROBLEMS.pythonReverse, path.join(dir, "self-00011.md"))
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  const port = await app.listen(0)
  try {
    const res = await post("http://127.0.0.1:" + port, "algo:self-00011", {
      code: fs.readFileSync(manifest.CODE.pythonOk, "utf8"),
      language: "python",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "pass")
    assert.strictEqual(
      app.store.getSetting("judge.python.cmd"),
      null,
      "settings 里没配过时不该被自动探测的结果钉死"
    )
  } finally {
    await app.close()
  }
})

test("judge.python.cmd：settings 里配的解释器跑不起来时，用真身顶掉", async function () {
  // 自愈：用户配的解释器被删了/写错了，探测到的真身把它顶掉，下次不必再走一遍
  // 注定失败的探测（契约 §8.1.2.3）。
  const 坏的 = "绝对不存在的解释器-xyz"
  const root = tmpDir("study-hub-attempt-")
  const dir = path.join(root, "bank", "fundamentals")
  fs.mkdirSync(dir, { recursive: true })
  fs.copyFileSync(manifest.PROBLEMS.pythonReverse, path.join(dir, "self-00011.md"))
  const app = createApp({ dbPath: ":memory:", learningRoot: root, localBank: null })
  app.store.setSetting("judge.python.cmd", 坏的)
  const port = await app.listen(0)
  try {
    const res = await post("http://127.0.0.1:" + port, "algo:self-00011", {
      code: fs.readFileSync(manifest.CODE.pythonOk, "utf8"),
      language: "python",
    })
    const body = await res.json()
    assert.strictEqual(body.verdict, "pass", "坏解释器应当回落到自动探测：" + body.message)
    const healed = app.store.getSetting("judge.python.cmd")
    assert.ok(healed, "坏的解释器应当被真身顶掉")
    assert.notStrictEqual(healed, 坏的, "不该把那条坏配置原样留着")
  } finally {
    await app.close()
  }
})

test("旧路由已下线：POST /api/attempt 返回 404", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/attempt", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ problemId: "self-00010", code: cppCode(), language: "cpp" }),
    })
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "接口不存在")
  })
})
