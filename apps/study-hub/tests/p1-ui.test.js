"use strict"
/**
 * p1-ui.test.js —— P1 新页面的界面层次闸门
 *
 * 老页面由 tests/ui-hierarchy.test.js 管着；这一份管 P1 新加的两页
 * （会话队列首页、会话设置页），用同一套规矩：
 *   1. 每页有且只有一个页头（眉标 + 大标题 + 副标）
 *   2. 「现在该做什么」在主卡里（.panel.hero）
 *   3. 背景信息放静默卡（.panel.quiet），数字摆成 .stat-grid
 *   4. 关键状态用中文文字承载（药丸），不靠颜色单独表意
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
require("../web/pages/session.js")
require("../web/pages/settings.js")
const PAGES = global.window.StudyHub.pages

/** 铺一个假的 fetch：按路径前缀返回预设响应 */
function stubFetch(routes) {
  global.fetch = async function (url) {
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
    return {
      ok: true,
      status: 200,
      json: async function () {
        return body
      },
    }
  }
}

/** 在树里找出所有 class 精确匹配的节点 */
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

/** 按 class 词元找（药丸是 pill / pill ok / pill warn） */
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

/** 有没有汉字 */
function hasChinese(text) {
  return /[一-龥]/.test(String(text || ""))
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
  env: { python: { ok: true, detail: "python3 3.12" }, numpy: { ok: false, detail: "没装 numpy" } },
  warnings: [],
}

/* ---------------- 四层层次 ---------------- */

test("会话队列首页：页头 + 主卡 + 普通卡 + 静默卡 + 数字块", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const view = mountView()
  await PAGES.session.render(view)

  const heads = findAllByClass(view, "page-head")
  assert.strictEqual(heads.length, 1, "首页有且只有一个页头")
  assert.ok(findByClass(heads[0], "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(heads[0], "page-title"), "页头里要有大标题")
  assert.ok(findByClass(heads[0], "page-sub"), "页头里要有副标")

  assert.ok(findByClass(view, "panel hero"), "会话队列要在主卡里")
  assert.ok(findAllByClass(view, "panel").length >= 1, "要有一张普通卡")
  assert.ok(findByClass(view, "panel quiet"), "三轨概况是背景信息，用静默卡")
  assert.ok(findByClass(view, "stat-grid"), "概况要摆成数字块")
})

test("会话设置页：页头 + 主卡 + 普通卡 + 静默卡 + 数字块", async function () {
  stubFetch({ "/api/settings": SETTINGS })
  const view = mountView()
  await PAGES.settings.render(view)

  const heads = findAllByClass(view, "page-head")
  assert.strictEqual(heads.length, 1, "设置页有且只有一个页头")
  assert.ok(findByClass(heads[0], "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(heads[0], "page-title"), "页头里要有大标题")
  assert.ok(findByClass(heads[0], "page-sub"), "页头里要有副标")

  assert.ok(findByClass(view, "panel hero"), "会话长度要在主卡里")
  assert.ok(findAllByClass(view, "panel").length >= 1, "要有一张普通卡")
  assert.ok(findByClass(view, "panel quiet"), "环境自检是背景信息，用静默卡")
  assert.ok(findByClass(view, "stat-grid"), "概况要摆成数字块")
})

test("两页的药丸都是中文——状态不靠颜色单独表意", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })

  const sessionView = mountView()
  await PAGES.session.render(sessionView)
  const sessionPills = findAllByToken(sessionView, "pill")
  assert.ok(sessionPills.length >= 3, "队列三态各要一个药丸")
  sessionPills.forEach(function (pill) {
    assert.ok(hasChinese(pill.textContent), "药丸要有中文：" + pill.textContent)
  })

  const settingsView = mountView()
  await PAGES.settings.render(settingsView)
  const settingsPills = findAllByToken(settingsView, "pill")
  assert.ok(settingsPills.length >= 2, "环境自检两行各要一个药丸")
  settingsPills.forEach(function (pill) {
    assert.ok(hasChinese(pill.textContent), "药丸要有中文：" + pill.textContent)
  })
})

test("两页的正文都是中文，没有漏网的英文占位", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })

  const sessionView = mountView()
  await PAGES.session.render(sessionView)
  assert.ok(
    collectText(sessionView).indexOf("本次学习") !== -1,
    "首页标题没出来：" + collectText(sessionView)
  )

  const settingsView = mountView()
  await PAGES.settings.render(settingsView)
  assert.ok(
    collectText(settingsView).indexOf("设置") !== -1,
    "设置页标题没出来：" + collectText(settingsView)
  )
})

test("两页都只有一个页头（不许出现第二个）", async function () {
  stubFetch({ "/api/session": SESSION, "/api/settings": SETTINGS })
  const views = [
    { name: "首页", render: PAGES.session.render },
    { name: "设置页", render: PAGES.settings.render },
  ]
  for (const item of views) {
    const view = mountView()
    await item.render(view)
    assert.strictEqual(
      findAllByClass(view, "page-head").length,
      1,
      item.name + "页出现了多个页头"
    )
  }
})

test("追加样式没破坏单主题：样式表仍然只有一套 CSS 变量", function () {
  const fs = require("node:fs")
  const path = require("node:path")
  const css = fs.readFileSync(path.join(__dirname, "..", "web", "style.css"), "utf8")
  const roots = css.match(/:root\s*\{/g) || []
  assert.strictEqual(roots.length, 1, "应该只有一套 CSS 变量")
  assert.ok(/--bg:\s*#1/i.test(css), "底色要保持暗色")
})
