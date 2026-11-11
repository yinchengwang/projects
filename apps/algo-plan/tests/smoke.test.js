"use strict"
/**
 * smoke.test.js —— 服务起得来、静态页能开、越界被拒
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")

const { createServer } = require("../server/app.js")

test("静态首页可访问且返回 200", async function () {
  const app = createServer({ routes: [] })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/")
    assert.strictEqual(res.status, 200)
    const body = await res.text()
    assert.ok(body.indexOf("算法能力计划") !== -1, "首页应包含标题")
  } finally {
    await app.close()
  }
})

test("未知静态路径返回 404", async function () {
  const app = createServer({ routes: [] })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/nope.html")
    assert.strictEqual(res.status, 404)
  } finally {
    await app.close()
  }
})

test("路径越界被拒绝（400）", async function () {
  const app = createServer({ routes: [] })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/%2e%2e%2fpackage.json")
    assert.strictEqual(res.status, 400)
  } finally {
    await app.close()
  }
})

test("未知 API 返回 404 JSON", async function () {
  const app = createServer({ routes: [] })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/nope")
    assert.strictEqual(res.status, 404)
    const body = await res.json()
    assert.strictEqual(body.error, "接口不存在")
  } finally {
    await app.close()
  }
})

test("路径对但方法不对返回 405", async function () {
  const app = createServer({
    routes: [{ method: "GET", pattern: "/api/ping", handler: function (req, res) { res.end("ok") } }],
  })
  const port = await app.listen(0)
  try {
    const res = await fetch("http://127.0.0.1:" + port + "/api/ping", { method: "POST" })
    assert.strictEqual(res.status, 405)
  } finally {
    await app.close()
  }
})

test("静态资源根目录指向 web/", function () {
  const { WEB_DIR } = require("../server/app.js")
  assert.strictEqual(path.basename(WEB_DIR), "web")
})
