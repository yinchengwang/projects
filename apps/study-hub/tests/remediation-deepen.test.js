"use strict"
/**
 * remediation-deepen.test.js —— 深挖概念接线闸门
 *
 * 原设计 §7.1 场景 5「深挖概念」：core/llm/service.js 的 deepen 早就实现了，
 * 但 server/ 没有路由、web/ 没有入口——功能等于不存在。本任务补上。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const http = require("node:http")

const unitRoutes = require("../server/api-unit-run.js")
const { compile } = require("../server/router.js")

const UNIT = {
  id: "db:mvcc",
  track: "database",
  kind: "card",
  title: "MVCC",
  statement: "讲解",
  entry: { demo: "", reference: "", graded: "script", script: "" },
}

/**
 * 起一个只挂单元运行路由的假服务。
 *
 * 路由的 handler 签名是 (req, res, params)：
 * body 由 readJsonBody 从 req 的流里读，所以这里不解析请求体，直接交给 handler。
 * 路径匹配用 router.js 自己的 compile（与线上完全同一套规则），不手搓。
 * **必须在 finally 里 close**，否则 node:test 的进程不会退出。
 */
async function withServer(ctx, fn) {
  const routes = unitRoutes.buildUnitRunRoutes(ctx)
  const matchers = routes.map(function (r) {
    return { route: r, match: compile(r.pattern) }
  })
  const server = http.createServer(function (req, res) {
    const url = new URL(req.url, "http://127.0.0.1")
    let hit = null
    for (const item of matchers) {
      if (item.route.method !== req.method) continue
      const params = item.match(url.pathname)
      if (params !== null) {
        hit = { route: item.route, params: params }
        break
      }
    }
    if (!hit) {
      res.writeHead(404, { "content-type": "application/json" })
      res.end(JSON.stringify({ error: "没这条路由" }))
      return
    }
    hit.route.handler(req, res, hit.params).catch(function () {
      res.writeHead(500, { "content-type": "application/json" })
      res.end(JSON.stringify({ error: "炸了" }))
    })
  })
  const base = await new Promise(function (resolve) {
    server.listen(0, "127.0.0.1", function () {
      resolve("http://127.0.0.1:" + server.address().port)
    })
  })
  try {
    return await fn(base)
  } finally {
    await new Promise(function (resolve) {
      server.close(resolve)
    })
  }
}

function baseCtx(extra) {
  return Object.assign(
    {
      // pickUnit 读的是 ctx.units.byId（或 ctx.units 本身就是 id→unit 的映射），
      // 不是数组——传数组会让所有单元都 404。
      units: { byId: { "db:mvcc": UNIT } },
      learning: { ok: false, root: null, reason: "" },
      sendJson: function (res, code, body) {
        res.writeHead(code, { "content-type": "application/json" })
        res.end(JSON.stringify(body))
      },
    },
    extra || {}
  )
}

test("POST /api/units/:id/deepen 把问题交给 LLM 并回文本", async function () {
  let got = null
  await withServer(
    baseCtx({
      llm: {
        deepen: async function (req) {
          got = req
          return { text: "再挖一层：版本链怎么长出来的", model: "fake-model" }
        },
      },
    }),
    async function (base) {
      const res = await fetch(base + "/api/units/db:mvcc/deepen", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ question: "版本链怎么长出来的" }),
      })
      assert.strictEqual(res.status, 200)
      const body = await res.json()
      assert.strictEqual(body.text, "再挖一层：版本链怎么长出来的")
      assert.strictEqual(body.model, "fake-model")
      assert.ok(got, "deepen 应当被调用")
      assert.strictEqual(got.unit.id, "db:mvcc")
      assert.strictEqual(got.question, "版本链怎么长出来的")
    }
  )
})

test("LLM 没装配时深挖回 200 + 中文降级文案", async function () {
  await withServer(baseCtx({ llm: null }), async function (base) {
    const res = await fetch(base + "/api/units/db:mvcc/deepen", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ question: "x" }),
    })
    assert.strictEqual(res.status, 200)
    const body = await res.json()
    assert.ok(body.text.indexOf("LLM") !== -1, JSON.stringify(body))
  })
})

test("深挖不存在的单元 → 404", async function () {
  await withServer(baseCtx({ llm: null }), async function (base) {
    const res = await fetch(base + "/api/units/db:nope/deepen", {
      method: "POST",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ question: "x" }),
    })
    assert.strictEqual(res.status, 404)
  })
})
