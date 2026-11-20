"use strict"
/**
 * supply-web.test.js —— 供给页的纯函数与四层信息层次
 *
 * 用 tests/helpers/dom-stub.js 驱动真实的页面渲染代码，
 * 断言的结构与 tests/ui-hierarchy.test.js 一致（那个文件是回归闸门，不许改）。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

require("../web/app.js")
const supplyPage = require("../web/pages/supply.js")
const PAGES = global.window.StudyHub.pages

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

/** 在树里找出第一个 class 精确匹配的节点 */
function findByClass(node, className) {
  return findAllByClass(node, className)[0] || null
}

/** 在树里找出所有指定标签的节点（按钮按 className 找不到——按钮的 class 是 "primary" 之类） */
function findAllByTag(node, tag) {
  const out = []
  function walk(n) {
    if (!n) return
    if (n.tagName === tag) out.push(n)
    ;(n.children || []).forEach(walk)
  }
  walk(node)
  return out
}

/** 把树里所有文本拼起来 */
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

/** 铺一个假的 fetch */
function stubFetch(routes, posted) {
  global.fetch = async function (url, options) {
    const method = options && options.method ? options.method : "GET"
    if (method === "POST") {
      if (posted) posted.push({ url: url, body: options.body })
      return { ok: true, status: 200, json: async function () { return { ok: true, unit: { id: "db:demo", status: "verified", reviewed: true, reviewed_at: "2026-11-13" } } } }
    }
    const key = Object.keys(routes).find(function (prefix) {
      return String(url).indexOf(prefix) === 0
    })
    if (!key) {
      return { ok: false, status: 404, json: async function () { return { error: "没有这条路由" } } }
    }
    const body = routes[key]
    return { ok: true, status: 200, json: async function () { return body } }
  }
}

/** 造一个空容器 */
function mountView() {
  const node = dom.document.createElement("div")
  node.setAttribute("id", "view")
  return node
}

/** 一份够喂饱供给页的响应 */
const STATUS = {
  tracks: [
    { track: "algorithm", total: 0, todo: 0, draft: 0, verified: 0, rejected: 0, reviewed: 0 },
    { track: "database", total: 1, todo: 0, draft: 1, verified: 0, rejected: 0, reviewed: 0 },
    { track: "ai", total: 0, todo: 0, draft: 0, verified: 0, rejected: 0, reviewed: 0 },
  ],
  gaps: [
    { id: "db:demo", track: "database", kind: "card", title: "演示卡", missing: ["asserts", "statement"] },
  ],
  warnings: [],
}

test("供给页注册进 window.StudyHub.pages", function () {
  assert.ok(PAGES.supply, "供给页要注册")
  assert.strictEqual(PAGES.supply.title, "供给")
})

test("导航清单 TABS 里有「供给」这一项", function () {
  const TABS = require("../web/app.js").TABS
  assert.ok(Array.isArray(TABS), "TABS 要从 web/app.js 导出来")
  const hit = TABS.find(function (tab) { return tab.key === "supply" })
  assert.ok(hit, "导航里要有供给：" + JSON.stringify(TABS))
  assert.strictEqual(hit.label, "供给")
})

test("供给页：页头 + 主卡 + 数字块 + 静默卡，四层都在", async function () {
  stubFetch({ "/api/supply/status": STATUS })
  const view = mountView()
  await PAGES.supply.render(view)

  const heads = findAllByClass(view, "page-head")
  assert.strictEqual(heads.length, 1, "供给页有且只有一个页头")
  assert.ok(findByClass(heads[0], "eyebrow"), "页头里要有眉标")
  assert.ok(findByClass(heads[0], "page-title"), "页头里要有大标题")
  assert.ok(findByClass(heads[0], "page-sub"), "页头里要有副标")

  assert.ok(findByClass(view, "panel hero"), "「下一步」要在主卡里")
  assert.ok(findByClass(view, "stat-grid"), "各轨计数要摆成数字块")
  assert.ok(findByClass(view, "panel quiet"), "运行状态是背景信息，用静默卡")
})

test("供给页：状态药丸带中文文字", async function () {
  stubFetch({ "/api/supply/status": STATUS })
  const view = mountView()
  await PAGES.supply.render(view)
  const pills = findAllByClass(view, "pill")
  assert.ok(pills.length > 0, "状态要用药丸承载")
  const texts = pills.map(function (p) { return p.textContent })
  assert.ok(texts.join(" ").indexOf("待校验") !== -1, "药丸要写中文：" + texts.join(","))
})

test("供给页：纯本地模式也不白屏，只给中文提醒", async function () {
  stubFetch({
    "/api/supply/status": {
      tracks: [],
      gaps: [],
      warnings: ["还没连上 learning 仓，供给状态只能看本地数据，写回不可用"],
    },
  })
  const view = mountView()
  await PAGES.supply.render(view)
  assert.strictEqual(findAllByClass(view, "page-head").length, 1)
  assert.ok(collectText(view).indexOf("还没连上 learning 仓") !== -1, collectText(view))
})

test("供给页：缺口清单里每个单元都有「通过」和「打回」两个入口", async function () {
  const posted = []
  stubFetch({ "/api/supply/status": STATUS }, posted)
  const view = mountView()
  await PAGES.supply.render(view)
  const buttons = findAllByTag(view, "button")
  const labels = buttons.map(function (b) { return b.textContent })
  assert.ok(labels.indexOf("通过") !== -1, "要有通过按钮")
  assert.ok(labels.indexOf("打回") !== -1, "要有打回按钮")
})

test("供给页：点「通过」会 POST 到 /api/supply/review/:id", async function () {
  const posted = []
  stubFetch({ "/api/supply/status": STATUS }, posted)
  const view = mountView()
  await PAGES.supply.render(view)
  const buttons = findAllByTag(view, "button")
  const pass = buttons.find(function (b) { return b.textContent === "通过" })
  assert.ok(pass, "要有通过按钮")
  pass.dispatch("click")
  await new Promise(function (resolve) { setTimeout(resolve, 0) })
  assert.strictEqual(posted.length, 1, "要点一次就发一次")
  assert.ok(posted[0].url.indexOf("/api/supply/review/db%3Ademo") !== -1, posted[0].url)
  assert.ok(posted[0].body.indexOf('"passed":true') !== -1, posted[0].body)
})

test("buildSupplyView 是纯函数：给空数据也不炸", function () {
  const nodes = supplyPage.buildSupplyView({})
  assert.ok(nodes.length > 0)
  const nodes2 = supplyPage.buildSupplyView(null)
  assert.ok(nodes2.length > 0)
})
