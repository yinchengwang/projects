"use strict"
/**
 * supply-api.test.js —— 供给接口（契约 §8.4）
 *
 * 真起一个 HTTP 服务打过去；learning 仓是临时目录里的真 git 仓。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")
const { execFileSync } = require("node:child_process")

const { createApp } = require("../server/app.js")

/** 契约 §3.3 的 loader C 单元文件 */
const UNIT_FILE = [
  "---",
  "id: db:demo",
  "track: database",
  "kind: card",
  "title: 演示卡",
  "difficulty: medium",
  "topics: []",
  "source: generated",
  "language: python",
  "demo: scaffold/grokking/demo/main.py",
  'reference: ""',
  "graded: script",
  "script: units/database/demo.assert.py",
  "status: draft",
  "reviewed: false",
  'reviewed_at: ""',
  "subtracks: []",
  "---",
  "",
  "## 讲解",
  "",
  "演示卡的讲解正文。",
  "",
  "## 关联",
  "",
  "- 轮次：R37 Grokking 数据库",
  "",
].join("\n")

/** 造一个假的 learning 仓 */
function makeRepo() {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "supply-api-repo-"))
  execFileSync("git", ["init"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.email", "t@t"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["config", "user.name", "t"], { cwd: root, stdio: "ignore" })
  const cardDir = path.join(root, "scaffold", "grokking", "demo")
  fs.mkdirSync(cardDir, { recursive: true })
  fs.writeFileSync(path.join(cardDir, "main.py"), "print('hi')\n", "utf8")
  const unitsDir = path.join(root, "units")
  fs.mkdirSync(path.join(unitsDir, "database"), { recursive: true })
  fs.writeFileSync(path.join(unitsDir, "database", "demo.md"), UNIT_FILE, "utf8")
  execFileSync("git", ["add", "-A"], { cwd: root, stdio: "ignore" })
  execFileSync("git", ["commit", "-m", "init"], { cwd: root, stdio: "ignore" })
  const stateDir = fs.mkdtempSync(path.join(os.tmpdir(), "supply-api-state-"))
  return { root: root, unitsDir: unitsDir, stateFile: path.join(stateDir, "self-written.json") }
}

/** 起一个服务。repo 为 null = 纯本地模式（没连上 learning 仓） */
async function withApp(repo, fn) {
  const app = createApp({
    dbPath: ":memory:",
    learningRoot: repo ? repo.root : null,
    localBank: null,
    env: {},
    // 「自己写过」清单指到临时目录——测试不许往仓库里写东西
    stateFile: repo ? repo.stateFile : null,
  })
  const port = await app.listen(0)
  try {
    await fn("http://127.0.0.1:" + port, app)
  } finally {
    await app.close()
  }
}

test("GET /api/supply/status：返回每轨计数、缺口与提醒", async function () {
  const repo = makeRepo()
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/status")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(Array.isArray(body.tracks), JSON.stringify(body))
    assert.ok(Array.isArray(body.gaps), JSON.stringify(body))
    assert.ok(Array.isArray(body.warnings), JSON.stringify(body))
    const db = body.tracks.find(function (t) { return t.track === "database" })
    assert.ok(db, JSON.stringify(body.tracks))
    assert.strictEqual(db.total, 1)
  })
})

test("GET /api/supply/status?track=database：能按轨过滤", async function () {
  const repo = makeRepo()
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/status?track=database")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.tracks.length, 1)
    assert.strictEqual(body.tracks[0].track, "database")
  })
})

test("纯本地模式：status 仍返回 200 + 空数据，不白屏（G17）", async function () {
  await withApp(null, async function (base) {
    const res = await fetch(base + "/api/supply/status")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.deepStrictEqual(body.tracks, [])
    assert.deepStrictEqual(body.gaps, [])
    assert.ok(body.warnings.length > 0, JSON.stringify(body))
    assert.ok(body.warnings[0].indexOf("learning") !== -1, JSON.stringify(body.warnings))
  })
})

test("POST /api/supply/review/:id：写回 reviewed 与 reviewed_at", async function () {
  const repo = makeRepo()
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/review/db:demo", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ passed: true, note: "讲得清楚" }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.ok, true)
    assert.strictEqual(body.unit.id, "db:demo")
    assert.strictEqual(body.unit.reviewed, true)
    assert.ok(/^\d{4}-\d{2}-\d{2}$/.test(body.unit.reviewed_at), JSON.stringify(body))
    const text = fs.readFileSync(path.join(repo.unitsDir, "database", "demo.md"), "utf8")
    assert.ok(text.indexOf("reviewed: true") !== -1, text)
    assert.ok(text.indexOf("讲得清楚") !== -1, text)
  })
})

test("POST /api/supply/review/:id：单元不存在 → 404", async function () {
  const repo = makeRepo()
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/review/db:nope", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ passed: true }),
    })
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "单元不存在")
    assert.strictEqual(body.id, "db:nope")
  })
})

test("POST /api/supply/review/:id：passed 不是布尔 → 400", async function () {
  const repo = makeRepo()
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/review/db:demo", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ passed: "yes" }),
    })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.strictEqual(body.error, "passed 必须是布尔值")
  })
})

test("纯本地模式：review → 503（G17）", async function () {
  await withApp(null, async function (base) {
    const res = await fetch(base + "/api/supply/review/db:demo", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ passed: true }),
    })
    assert.strictEqual(res.status, 503)
    const body = await res.json()
    assert.strictEqual(body.error, "还没连上 learning 仓，写回不可用")
  })
})

test("别人动过的文件 → 409，写回被阻塞（G14）", async function () {
  const repo = makeRepo()
  const file = path.join(repo.unitsDir, "database", "demo.md")
  fs.writeFileSync(file, fs.readFileSync(file, "utf8") + "\n别人加的一行\n", "utf8")
  await withApp(repo, async function (base) {
    const res = await fetch(base + "/api/supply/review/db:demo", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ passed: true }),
    })
    assert.strictEqual(res.status, 409)
    const body = await res.json()
    assert.ok(String(body.error).indexOf("写回已阻塞") !== -1, JSON.stringify(body))
    assert.ok(fs.readFileSync(file, "utf8").indexOf("reviewed: true") === -1)
  })
})
