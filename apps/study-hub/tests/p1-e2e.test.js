"use strict"
/**
 * p1-e2e.test.js —— P1 的端到端
 *
 * 这一份是真的端到端：
 *   - 服务端是真的 HTTP 服务（createApp 装配）
 *   - 数据库是真的 SQLite（内存库）
 *   - 前端是 web/pages/session.js / web/pages/settings.js 的真实渲染代码
 *   - 前端的 fetch 被包一层，把相对路径补成绝对地址
 *
 * 只连 127.0.0.1 上自己起的那个服务：不联网、不 spawn Python、不连 learning 仓。
 */

const test = require("node:test")
const assert = require("node:assert")
const fs = require("node:fs")
const os = require("node:os")
const path = require("node:path")

const stub = require("./helpers/dom-stub.js")

const dom = stub.createDocument()
global.document = dom.document
global.window = global

const { createApp } = require("../server/app.js")
require("../web/app.js")
const sessionPage = require("../web/pages/session.js")
const settingsPage = require("../web/pages/settings.js")

/** 真的 fetch（前端那层包装之前先存一份） */
const realFetch = globalThis.fetch

/** 当前这个测试用的服务地址 */
let base = ""

/** 前端用的是相对路径——包一层补成绝对地址 */
global.fetch = function (url, options) {
  const target = String(url).indexOf("http") === 0 ? String(url) : base + String(url)
  return realFetch(target, options)
}

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
    id: "algo:lc-00002",
    track: "algorithm",
    kind: "problem",
    title: "反转链表",
    path: "bank/interview/lc-00002.md",
    status: "verified",
    topics: ["链表"],
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
  {
    id: "ai:cnn",
    track: "ai",
    kind: "card",
    title: "卷积神经网络",
    path: "scaffold/grokking/cnn/",
    status: "verified",
    topics: ["深度学习"],
    difficulty: "medium",
    subtracks: [],
  },
]

function probeOk() {
  return {
    python: { ok: true, detail: "python3（测试桩）" },
    numpy: { ok: true, detail: "numpy 可用（测试桩）" },
  }
}

/** 起一个真服务，跑完关掉 */
async function withApp(fn) {
  const root = fs.mkdtempSync(path.join(os.tmpdir(), "study-hub-p1-e2e-"))
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
  base = "http://127.0.0.1:" + port
  try {
    await fn(base, app)
  } finally {
    await app.close()
    fs.rmSync(root, { recursive: true, force: true })
  }
}

function postJson(url, body) {
  return realFetch(base + url, {
    method: "POST",
    headers: { "content-type": "application/json" },
    body: JSON.stringify(body || {}),
  })
}

function findAllByClass(node, className) {
  const out = []
  function walk(n) {
    if (!n) return
    if (n.className === className) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function findByClass(node, className) {
  return findAllByClass(node, className)[0] || null
}

/** 按 class 词元找（队列项是 queue-item queue-done / queue-item queue-current） */
function findAllByToken(node, token) {
  const out = []
  function walk(n) {
    if (!n) return
    const parts = String(n.className || "").split(/\s+/)
    if (parts.indexOf(token) !== -1) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function findButton(node, label) {
  let hit = null
  function walk(n) {
    if (hit || !n) return
    if (n.tagName === "button" && String(n.textContent || "").indexOf(label) !== -1) {
      hit = n
      return
    }
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return hit
}

function findByAttr(node, attr, value) {
  let hit = null
  function walk(n) {
    if (hit || !n) return
    if (n.attributes && n.attributes[attr] === value) {
      hit = n
      return
    }
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return hit
}

function collectText(node) {
  let out = ""
  function walk(n) {
    if (!n) return
    if (n.textContent) out += " " + n.textContent
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

function mountView() {
  return dom.document.createElement("div")
}

/** 等一等——把微任务与定时器都放过去 */
function tick(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms || 20)
  })
}

test("端到端：建会话 → 首页排好队 → 完成当前项 → 队列与库都更新", async function () {
  await withApp(async function () {
    const started = await postJson("/api/session/start", { n: 3 })
    const startBody = await started.json()
    assert.ok(startBody.session, "要有会话")
    assert.strictEqual(startBody.queue.length, 3, "队列要有三条")
    assert.strictEqual(startBody.queue[0].state, "current", "第一条是正在学")

    // 首页渲染（走真接口）
    const view = mountView()
    await sessionPage.render(view)
    assert.strictEqual(findAllByToken(view, "queue-item").length, 3, "首页要摆出三条队列")
    assert.ok(findByClass(view, "queue-item queue-current"), "第一条要标黄")
    assert.ok(findAllByClass(view, "queue-item queue-pending").length === 2, "后两条要置灰")
    assert.ok(collectText(view).indexOf("上次还有") !== -1, "顶部要有未做完提示")

    // 点「完成这个」——真提交
    const btn = findButton(view, "完成这个")
    assert.ok(btn, "当前项要有完成按钮")
    btn.dispatch("click")
    await tick()

    // 库里更新了
    const after = await realFetch(base + "/api/session")
    const afterBody = await after.json()
    assert.strictEqual(afterBody.queue[0].state, "done", "第一项应该变成已完成")
    assert.strictEqual(afterBody.session.done_count, 1, "已完成数要 +1")

    // 首页重画后第一项是已完成
    const view2 = mountView()
    await sessionPage.render(view2)
    assert.ok(findByClass(view2, "queue-item queue-done"), "重画后第一项是已完成")
  })
})

test("端到端：暂停之后队列还在，回来接着学", async function () {
  await withApp(async function () {
    await postJson("/api/session/start", { n: 2 })
    const paused = await postJson("/api/session/pause", {})
    const pauseBody = await paused.json()
    assert.ok(pauseBody.session.ended_at, "要写上结束时间")
    assert.strictEqual(pauseBody.queue.length, 2, "队列不能丢")

    const view = mountView()
    await sessionPage.render(view)
    assert.strictEqual(findAllByToken(view, "queue-item").length, 2, "首页还要摆出这两条")

    // 再次 start 不带 reset —— 接着上次的会话
    const again = await postJson("/api/session/start", {})
    const againBody = await again.json()
    assert.strictEqual(againBody.session.id, pauseBody.session.id, "要接着同一个会话")
  })
})

test("端到端：设置页改了会话长度，下一次会话按新长度排", async function () {
  await withApp(async function () {
    const settingsView = mountView()
    await settingsPage.render(settingsView)

    const input = findByAttr(settingsView, "data-field", "session_count")
    assert.ok(input, "要有会话长度输入框")
    input.value = "2"
    input.dispatch("change")
    const saveBtn = findButton(settingsView, "保存设置")
    assert.ok(saveBtn, "要有保存按钮")
    saveBtn.dispatch("click")
    await tick()

    const settings = await (await realFetch(base + "/api/settings")).json()
    assert.strictEqual(settings.session.default_count, 2, "设置要真的写进去")

    // 不带 n —— 用设置里的默认值
    const started = await postJson("/api/session/start", {})
    const body = await started.json()
    assert.strictEqual(body.queue.length, 2, "下一次会话要按新的长度排")
  })
})
