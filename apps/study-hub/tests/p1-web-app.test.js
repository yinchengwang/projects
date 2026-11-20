"use strict"
/**
 * p1-web-app.test.js —— 前端骨架：全局名、hash 解析、标签清单
 *
 * 只测纯逻辑（全局名 / parseHash / TABS），不碰渲染。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")

// 必须在 require web/app.js 之前把 document 铺好
const dom = stub.createDocument()
global.document = dom.document
global.window = global

const app = require("../web/app.js")
// 浏览器里这些页面是 index.html 的 <script src> 逐个加载的。
// P3 起 today / review / rounds 三个旧页已从 index.html 摘掉、也不再注册（契约 §9.1），
// 所以这里**不再**手工 require 它们——否则会把它们又注册回 pages，让退役断言误判。
require("../web/diagnose.js")

test("全局名换成 window.StudyHub，window.AlgoPlan 是同一个对象的别名", function () {
  assert.ok(global.window.StudyHub, "要有 window.StudyHub")
  assert.strictEqual(global.window.AlgoPlan, global.window.StudyHub)
})

test("公共工具齐了：getJson / postJson / patchJson / putJson / pages / go", function () {
  const A = global.window.StudyHub
  assert.strictEqual(typeof A.getJson, "function")
  assert.strictEqual(typeof A.postJson, "function")
  assert.strictEqual(typeof A.patchJson, "function")
  assert.strictEqual(typeof A.putJson, "function")
  assert.strictEqual(typeof A.go, "function")
  assert.strictEqual(typeof A.pages, "object")
})

test("putJson 发的是 PUT，body 原样发出去", async function () {
  let seen = null
  global.fetch = async function (url, options) {
    seen = { url: url, method: options.method, body: JSON.parse(options.body) }
    return {
      ok: true,
      status: 200,
      json: async function () {
        return { ok: true }
      },
    }
  }
  const body = await global.window.StudyHub.putJson("/api/settings", { session_count: 5 })
  assert.strictEqual(seen.method, "PUT")
  assert.strictEqual(seen.url, "/api/settings")
  assert.deepStrictEqual(seen.body, { session_count: 5 })
  assert.deepStrictEqual(body, { ok: true })
})

test("parseHash：空 hash、# 与 #/ 都落到 session", function () {
  assert.strictEqual(app.parseHash("").page, "session")
  assert.strictEqual(app.parseHash("#").page, "session")
  assert.strictEqual(app.parseHash("#/").page, "session")
})

test("parseHash：#/settings 解析出 settings", function () {
  const r = app.parseHash("#/settings")
  assert.strictEqual(r.page, "settings")
  assert.deepStrictEqual(r.params, {})
  assert.deepStrictEqual(r.query, {})
})

test("parseHash：单元页的 id 会解码（%3A → :）", function () {
  const r = app.parseHash("#/unit/algo%3Alc-00007")
  assert.strictEqual(r.page, "unit")
  assert.strictEqual(r.params.id, "algo:lc-00007")
})

test("parseHash：查询串进 query", function () {
  const r = app.parseHash("#/library?track=database&q=mvcc")
  assert.strictEqual(r.page, "library")
  assert.strictEqual(r.query.track, "database")
  assert.strictEqual(r.query.q, "mvcc")
})

test("parseHash：不传参数时读 location.hash", function () {
  global.location = { hash: "#/settings" }
  assert.strictEqual(app.parseHash().page, "settings")
  delete global.location
})

test("applyHash：页没注册就不动，注册了就切过去", function () {
  // 用 `diagnose` 而不是 `settings`：**settings 页要到 Task 13 才创建**，
  // 本条用例在 Task 11 跑时它还没注册（用它会让用例恒红）。
  // 这条用例要验的是「已注册 → true / 未注册 → false」这个语义，不关心具体是哪一页。
  const A = global.window.StudyHub
  assert.ok(A.pages.diagnose, "前提：diagnose 页要已注册")
  assert.strictEqual(app.applyHash("#/diagnose"), true)
  assert.strictEqual(app.applyHash("#/根本没有这个页"), false)
  assert.ok(A.pages)
})

test("标签清单：P4 起含供给（session / library / diagnose / supply / settings）", function () {
  const keys = app.TABS.map(function (tab) {
    return tab.key
  })
  assert.deepStrictEqual(keys, ["session", "library", "diagnose", "supply", "settings"])
})

test("旧页按契约 §9.1 分批退役：answer 在 P2，today / review / rounds 在 P3", function () {
  const PAGES = global.window.StudyHub.pages
  // P2 退役 answer.js，P3 退役 today / review / rounds——文件都还在仓库里，只是不再注册
  assert.strictEqual(PAGES.answer, undefined, "answer 页已在 P2 退役")
  assert.strictEqual(PAGES.today, undefined, "today 页已在 P3 退役")
  assert.strictEqual(PAGES.review, undefined, "review 页已在 P3 退役")
  assert.strictEqual(PAGES.rounds, undefined, "rounds 页已在 P3 退役")
})

test("诊断页靠别名照样注册得上", function () {
  assert.ok(global.window.StudyHub.pages.diagnose, "诊断页没注册")
  assert.strictEqual(global.window.StudyHub.pages.diagnose.title, "诊断")
})
