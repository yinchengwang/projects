"use strict"
/**
 * p2-hash-route.test.js —— hash 路由与前端接口封装的闸门
 *
 * hash 路由是手工实现的（契约 §29：不建前端路由框架），
 * 所以解析规则必须钉死：#/unit/db:mvcc 要能拆出 id 来。
 */

const test = require("node:test")
const assert = require("node:assert")

const stub = require("./helpers/dom-stub.js")
const dom = stub.createDocument()
global.document = dom.document
global.window = global

const app = require("../web/app.js")

test("parseHash：空 hash 与 #/ 都回落到会话页", function () {
  assert.deepStrictEqual(app.parseHash(""), { page: "session", params: {}, query: {} })
  assert.deepStrictEqual(app.parseHash("#/"), { page: "session", params: {}, query: {} })
})

test("parseHash：#/unit/:id 拆出 id", function () {
  const r = app.parseHash("#/unit/db:mvcc")
  assert.strictEqual(r.page, "unit")
  assert.strictEqual(r.params.id, "db:mvcc")
})

test("parseHash：id 是百分号编码的也能解开", function () {
  const r = app.parseHash("#/unit/algo%3Alc-00007")
  assert.strictEqual(r.page, "unit")
  assert.strictEqual(r.params.id, "algo:lc-00007")
})

test("parseHash：#/library 的 query 拆成键值对", function () {
  const r = app.parseHash("#/library?track=database&q=mvcc")
  assert.strictEqual(r.page, "library")
  assert.deepStrictEqual(r.params, {})
  assert.strictEqual(r.query.track, "database")
  assert.strictEqual(r.query.q, "mvcc")
})

test("parseHash：query 的值要解百分号编码", function () {
  const r = app.parseHash("#/library?track=%E6%95%B0%E6%8D%AE%E5%BA%93")
  assert.strictEqual(r.query.track, "数据库")
})

test("parseHash：加号当空格处理", function () {
  const r = app.parseHash("#/library?q=mvcc+%E7%A7%92")
  assert.strictEqual(r.query.q, "mvcc 秒")
})

test("parseHash：认得契约 §9.2 里的每个页面", function () {
  assert.strictEqual(app.parseHash("#/diagnose").page, "diagnose")
  assert.strictEqual(app.parseHash("#/supply").page, "supply")
  assert.strictEqual(app.parseHash("#/settings").page, "settings")
  assert.strictEqual(app.parseHash("#/library").page, "library")
})

test("parseHash：认不出的 hash 回落到会话页，不抛错", function () {
  assert.strictEqual(app.parseHash("#/nonsense").page, "session")
  assert.strictEqual(app.parseHash("#/unit").page, "unit")
  assert.strictEqual(app.parseHash("garbage").page, "session")
})

test("putJson：发的是 PUT 方法", async function () {
  const seen = []
  global.fetch = async function (url, opts) {
    seen.push({ url: url, method: opts && opts.method, body: opts && opts.body })
    return { ok: true, status: 200, json: async function () { return { ok: true } } }
  }
  await app.putJson("/api/llm/settings", { enabled: false })
  assert.strictEqual(seen.length, 1)
  assert.strictEqual(seen[0].method, "PUT")
  assert.strictEqual(seen[0].url, "/api/llm/settings")
  assert.deepStrictEqual(JSON.parse(seen[0].body), { enabled: false })
})

test("putJson：失败时抛出带中文说明的错误", async function () {
  global.fetch = async function () {
    return { ok: false, status: 400, json: async function () { return { error: "参数不对" } } }
  }
  await assert.rejects(
    function () {
      return app.putJson("/api/llm/settings", {})
    },
    /参数不对/
  )
})

test("全局对象上挂着 parseHash 与 putJson（页面脚本靠它们读路由、发 PUT）", function () {
  const A = global.window.StudyHub || global.window.AlgoPlan
  assert.ok(A, "全局对象必须在")
  assert.strictEqual(typeof A.parseHash, "function", "单元库页要靠 A.parseHash 读过滤条件")
  assert.strictEqual(typeof A.putJson, "function", "契约 §9.3 要求全局对象上有 putJson")
})

test("全局对象：window.AlgoPlan 仍然在，且与 window.StudyHub 是同一个对象", function () {
  // P1 的约定：window.StudyHub 为主，window.AlgoPlan 是**同一个对象的别名**，
  // 旧页脚本一行不改。tests/pages.test.js 的 stubWindow 只给 AlgoPlan——
  // 别名没了，那些旧页脚本 require 进来就直接崩。
  assert.ok(global.window.AlgoPlan, "window.AlgoPlan 必须还在")
  assert.ok(global.window.AlgoPlan.pages, "pages 注册表必须在")
  if (global.window.StudyHub) {
    assert.strictEqual(
      global.window.StudyHub,
      global.window.AlgoPlan,
      "window.StudyHub 与 window.AlgoPlan 必须是同一个对象，不能是两份"
    )
  }
})
