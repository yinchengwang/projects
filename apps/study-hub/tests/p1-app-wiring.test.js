"use strict"
/**
 * p1-app-wiring.test.js —— 组合根装配 + 旧接口标废弃
 *
 * 走真的 createApp（内存库 + 临时 learning 仓），验证：
 *   1. 会话七条路由装上了
 *   2. 旧路由没被弄坏（/api/health、/api/rounds、/api/next）
 *   3. GET /api/next 仍返回 200，但标了 deprecated
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const { createApp } = require("../server/app.js")

const PROBLEM_MD = [
  "---",
  "id: lc-00001",
  "source: leetcode",
  "title: 两数之和",
  "difficulty: easy",
  "topics: [哈希表, 数组]",
  "tracks: [interview]",
  "status: verified",
  "needs_fill: false",
  "created: 2026-11-11",
  "updated: 2026-11-11",
  "---",
  "## 题面",
  "",
  "给定整数数组与目标值。",
  "",
].join("\n")

const UNITS = [
  {
    id: "algo:lc-00001",
    track: "algorithm",
    kind: "problem",
    title: "两数之和",
    path: "bank/interview/lc-00001.md",
    status: "verified",
    topics: ["哈希表"],
    difficulty: "easy",
    subtracks: ["interview"],
  },
  {
    id: "db:mvcc",
    track: "database",
    kind: "card",
    title: "MVCC：版本链与 ReadView",
    path: "scaffold/grokking/mvcc/",
    status: "verified",
    topics: ["事务"],
    difficulty: "medium",
    subtracks: [],
  },
]

/** 环境探针的假实现：不 spawn 真的 Python */
function probeOk() {
  return {
    python: { ok: true, detail: "python3（测试桩）" },
    numpy: { ok: true, detail: "numpy 可用（测试桩）" },
  }
}

async function withApp(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p1-"))
  const bank = path.join(root, "bank", "interview")
  fs.mkdirSync(bank, { recursive: true })
  fs.writeFileSync(path.join(bank, "lc-00001.md"), PROBLEM_MD, "utf8")
  fs.mkdirSync(path.join(root, "progress"), { recursive: true })

  const app = createApp({
    dbPath: ":memory:",
    learningRoot: root,
    localBank: null,
    env: {},
    envProbe: probeOk,
  })
  app.store.replaceUnits(UNITS)
  const port = await app.listen(0)
  const base = "http://127.0.0.1:" + port
  try {
    await fn(base, app)
  } finally {
    await app.close()
    fs.rmSync(root, { recursive: true, force: true })
  }
}

function postJson(base, url, body) {
  return fetch(base + url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
}

test("createApp 把会话七条路由装上了", async function () {
  await withApp(async function (base, app) {
    const patterns = app.sessionRoutes.map(function (route) {
      return route.method + " " + route.pattern
    })
    const expected = [
      "POST /api/session/start",
      "GET /api/session",
      "GET /api/session/next",
      "POST /api/session/pause",
      "POST /api/session/submit",
      "GET /api/settings",
      "PUT /api/settings",
    ]
    expected.forEach(function (want) {
      assert.ok(patterns.indexOf(want) !== -1, "少了路由：" + want + "，实际：" + patterns.join(" | "))
    })
  })
})

test("GET /api/session 在组合根上返回 200，字段齐全", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/session")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(Object.prototype.hasOwnProperty.call(body, "session"))
    assert.ok(Array.isArray(body.queue))
    assert.ok(Object.prototype.hasOwnProperty.call(body, "next"))
    assert.strictEqual(typeof body.notice, "string")
    assert.ok(Array.isArray(body.warnings))
  })
})

test("POST /api/session/start 在组合根上能建出会话", async function () {
  await withApp(async function (base) {
    const res = await postJson(base, "/api/session/start", { n: 2 })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.session, "要有会话")
    assert.strictEqual(body.queue.length, 2)
    assert.strictEqual(body.queue[0].state, "current")

    const again = await fetch(base + "/api/session")
    const againBody = await again.json()
    assert.strictEqual(againBody.session.id, body.session.id)
    assert.strictEqual(againBody.notice, "上次还有 2 个没做完")
  })
})

test("GET /api/settings 在组合根上给得出会话设置", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/settings")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.session.default_count, 3)
    assert.strictEqual(body.tracks.length, 3)
    assert.strictEqual(body.env.numpy.ok, true)
  })
})

test("旧路由没被弄坏：/api/health 与 /api/rounds 仍然是 200", async function () {
  await withApp(async function (base) {
    const health = await fetch(base + "/api/health")
    assert.strictEqual(health.status, 200)
    const rounds = await fetch(base + "/api/rounds")
    assert.strictEqual(rounds.status, 200)
  })
})

test("GET /api/next 仍然 200，但标了 deprecated", async function () {
  await withApp(async function (base) {
    const res = await fetch(base + "/api/next")
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.strictEqual(body.deprecated, true)
    assert.ok(
      String(body.deprecatedNote).indexOf("/api/session/next") !== -1,
      "废弃说明要指向新接口：" + String(body.deprecatedNote)
    )
  })
})
