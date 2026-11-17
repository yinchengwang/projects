"use strict"
/**
 * p1-web-session.test.js —— 会话队列式首页
 *
 * 纯函数测口径，渲染测层次：页头一个、主卡一个、静默卡一个，
 * 队列三项的状态分别打勾 / 标黄 / 置灰。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
const sessionPage = require("../web/pages/session.js")

/** 铺一个假的 fetch */
function stubFetch(routes) {
  global.fetch = async function (url, options) {
    const key = Object.keys(routes).find(function (prefix) {
      return String(url).indexOf(prefix) === 0
    })
    if (!key) {
      return {
        ok: false,
        status: 404,
        json: async function () {
          return { error: "没有这条路由" }
        },
      }
    }
    const body = routes[key]
    if (typeof body === "function") return body(url, options)
    return {
      ok: true,
      status: 200,
      json: async function () {
        return body
      },
    }
  }
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

/**
 * 按 class **词元**找节点。
 * 药丸是 `pill` / `pill ok` / `pill warn`——className 带色调后缀，
 * 精确匹配找不到，所以按空格切词元来匹配。
 */
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

/** 找按钮：按文字找，不靠 class */
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
  const node = dom.document.createElement("div")
  node.setAttribute("id", "view")
  return node
}

const SESSION = {
  session: {
    id: "s-1",
    started_at: "2026-11-12T00:00:00.000Z",
    ended_at: null,
    planned_count: 3,
    done_count: 1,
    tracks: ["algorithm", "database"],
  },
  queue: [
    { unit_id: "algo:lc-00001", seq: 1, state: "done", title: "两数之和", track: "algorithm" },
    { unit_id: "db:mvcc", seq: 2, state: "current", title: "MVCC：版本链", track: "database" },
    { unit_id: "ai:cnn", seq: 3, state: "pending", title: "卷积神经网络", track: "ai" },
  ],
  next: { unit_id: "db:mvcc", seq: 2, total: 3 },
  notice: "上次还有 2 个没做完",
  warnings: [],
}

const SETTINGS = {
  session: { default_count: 3, min_count: 1, max_count: 10 },
  tracks: [
    { code: "algorithm", label: "算法", weight: 0.4, enabled: true, unit_count: 12 },
    { code: "database", label: "数据库", weight: 0.3, enabled: true, unit_count: 16 },
    { code: "ai", label: "AI", weight: 0.3, enabled: true, unit_count: 13 },
  ],
  review: { count_trigger_k: 5 },
  env: { python: { ok: true, detail: "python3" }, numpy: { ok: true, detail: "numpy 1.26" } },
  warnings: [],
}

/* ---------------- 纯函数 ---------------- */

test("describeQueueItem：已完成打勾、正在学标黄、没开始置灰", function () {
  const done = sessionPage.describeQueueItem({ unit_id: "a", seq: 1, state: "done" }, 0)
  const current = sessionPage.describeQueueItem({ unit_id: "b", seq: 2, state: "current" }, 1)
  const pending = sessionPage.describeQueueItem({ unit_id: "c", seq: 3, state: "pending" }, 2)
  assert.strictEqual(done.mark, "✓")
  assert.strictEqual(done.label, "已完成")
  assert.strictEqual(done.cls, "queue-done")
  assert.strictEqual(current.mark, "▶")
  assert.strictEqual(current.label, "正在学")
  assert.strictEqual(current.cls, "queue-current")
  assert.strictEqual(pending.mark, "○")
  assert.strictEqual(pending.label, "还没开始")
  assert.strictEqual(pending.cls, "queue-pending")
})

test("describeQueueItem：没有标题时回落成 unit_id", function () {
  const row = sessionPage.describeQueueItem({ unit_id: "algo:lc-00009", seq: 1, state: "pending" }, 0)
  assert.strictEqual(row.title, "algo:lc-00009")
})

test("buildQueueRows / queueSummary：数得清做完几个、剩几个", function () {
  const rows = sessionPage.buildQueueRows(SESSION.queue)
  assert.strictEqual(rows.length, 3)
  const summary = sessionPage.queueSummary(rows)
  assert.strictEqual(summary.total, 3)
  assert.strictEqual(summary.done, 1)
  assert.strictEqual(summary.current, 1)
  assert.strictEqual(summary.left, 2)
})

test("trackSummary：每轨这次排了几个", function () {
  const rows = sessionPage.buildQueueRows(SESSION.queue)
  const tracks = sessionPage.trackSummary(SETTINGS.tracks, rows)
  assert.strictEqual(tracks.length, 3)
  assert.strictEqual(tracks[0].code, "algorithm")
  assert.strictEqual(tracks[0].planned, 1)
  assert.strictEqual(tracks[1].planned, 1)
  assert.strictEqual(tracks[2].planned, 1)
  assert.strictEqual(tracks[0].unit_count, 12)
})

test("trackSummary：关掉的轨也摆出来，标成已关掉", function () {
  const rows = sessionPage.buildQueueRows(SESSION.queue)
  const tracks = sessionPage.trackSummary(
    [
      { code: "algorithm", label: "算法", enabled: true, unit_count: 12 },
      { code: "ai", label: "AI", enabled: false, unit_count: 13 },
    ],
    rows
  )
  assert.strictEqual(tracks[1].enabled, false)
  assert.strictEqual(tracks[1].planned, 1)
})

/* ---------------- 渲染 ---------------- */

test("首页：页头一个、主卡一个、静默卡一个", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const view = mountView()
  await sessionPage.render(view)

  assert.strictEqual(findAllByClass(view, "page-head").length, 1, "首页有且只有一个页头")
  assert.ok(findByClass(view, "panel hero"), "会话队列要在主卡里")
  assert.ok(findByClass(view, "panel quiet"), "三轨概况是背景信息，用静默卡")
  assert.ok(findByClass(view, "stat-grid"), "概况要摆成数字块")
  const head = findByClass(view, "page-head")
  assert.ok(findByClass(head, "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(head, "page-title"))
  assert.ok(findByClass(head, "page-sub"))
})

test("首页：队列三项分别是已完成 / 正在学 / 还没开始", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const view = mountView()
  await sessionPage.render(view)

  assert.ok(findByClass(view, "queue-item queue-done"), "已完成的要打勾")
  assert.ok(findByClass(view, "queue-item queue-current"), "当前的要标黄")
  assert.ok(findByClass(view, "queue-item queue-pending"), "没开始的要置灰")

  const pills = findAllByToken(view, "pill").map(function (p) {
    return p.textContent
  })
  assert.ok(pills.length >= 3, "三个状态各要一个药丸：" + pills.join(","))
  assert.ok(pills.join(" ").indexOf("已完成") !== -1, "状态要有中文文字：" + pills.join(","))
  assert.ok(pills.join(" ").indexOf("正在学") !== -1)
  assert.ok(pills.join(" ").indexOf("还没开始") !== -1)
})

test("首页：顶部有「上次还有 2 个没做完」的提示", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const view = mountView()
  await sessionPage.render(view)

  assert.ok(findByClass(view, "notice"), "要有提示条")
  assert.ok(
    collectText(view).indexOf("上次还有 2 个没做完") !== -1,
    "提示文字没出来：" + collectText(view)
  )
})

test("首页：队列里摆的是标题而不是裸 id", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const view = mountView()
  await sessionPage.render(view)

  const text = collectText(view)
  assert.ok(text.indexOf("两数之和") !== -1, "标题没出来")
  assert.ok(text.indexOf("MVCC：版本链") !== -1)
})

test("首页：还没有会话时给「开始一次学习」", async function () {
  stubFetch({
    "/api/session": { session: null, queue: [], next: null, notice: "", warnings: [] },
    "/api/settings": SETTINGS,
  })
  const view = mountView()
  await sessionPage.render(view)

  const text = collectText(view)
  assert.ok(text.indexOf("还没有会话") !== -1, "空态要说清楚：" + text)
  assert.ok(text.indexOf("开始一次学习") !== -1, "要给一个开始按钮")
})

test("首页：队列全做完时说「这次学完了」", async function () {
  stubFetch({
    "/api/session": {
      session: {
        id: "s-1",
        started_at: "2026-11-12T00:00:00.000Z",
        ended_at: "2026-11-12T01:00:00.000Z",
        planned_count: 3,
        done_count: 3,
        tracks: ["algorithm"],
      },
      queue: [],
      next: null,
      notice: "",
      warnings: [],
    },
    "/api/settings": SETTINGS,
  })
  const view = mountView()
  await sessionPage.render(view)

  const text = collectText(view)
  assert.ok(text.indexOf("这次学完了") !== -1, "要说清做完了：" + text)
  assert.ok(text.indexOf("开始一次学习") !== -1, "还是要能给下一次排队")
})

test("首页：点「开始一次学习」会发 start 请求", async function () {
  let started = false
  stubFetch({
    "/api/session/start": function () {
      started = true
      return { ok: true, status: 200, json: async function () { return SESSION } }
    },
    "/api/session": { session: null, queue: [], next: null, notice: "", warnings: [] },
    "/api/settings": SETTINGS,
  })
  const view = mountView()
  await sessionPage.render(view)

  const btn = findButton(view, "开始一次学习")
  assert.ok(btn, "要有开始按钮")
  btn.dispatch("click")
  await new Promise(function (resolve) { setTimeout(resolve, 0) })
  assert.ok(started, "点了要发 start 请求")
})

test("首页：当前项有「完成这个」按钮，点了会发 submit", async function () {
  let submitted = null
  stubFetch({
    "/api/session/submit": function (url, options) {
      submitted = JSON.parse(options.body)
      return { ok: true, status: 200, json: async function () { return { ok: true } } }
    },
    "/api/session": SESSION,
    "/api/settings": SETTINGS,
  })
  const view = mountView()
  await sessionPage.render(view)

  const btn = findButton(view, "完成这个")
  assert.ok(btn, "当前项要有完成按钮")
  btn.dispatch("click")
  await new Promise(function (resolve) { setTimeout(resolve, 0) })
  assert.ok(submitted, "点了要发 submit")
  assert.strictEqual(submitted.unit_id, "db:mvcc", "要带上单元 id")
})

test("首页：接口报错时显示中文错误，不崩", async function () {
  global.fetch = async function () {
    return {
      ok: false,
      status: 500,
      json: async function () {
        return { error: "服务端出错" }
      },
    }
  }
  const view = mountView()
  await sessionPage.render(view)
  assert.ok(collectText(view).indexOf("加载失败") !== -1, "要有错误提示：" + collectText(view))
})
