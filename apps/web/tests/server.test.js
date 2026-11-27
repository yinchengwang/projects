"use strict"
/**
 * server.test.js —— 组合根的路由分发与静态托管
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")
const path = require("node:path")
const fs = require("node:fs")
const os = require("node:os")

const { createApp } = require("../server/app.js")

/** 起一个临时静态目录，避免往源码树写文件 */
function makeTempWebRoot () {
  const dir = fs.mkdtempSync(path.join(os.tmpdir(), "appsweb-test-"))
  fs.writeFileSync(path.join(dir, "index.html"), "<!doctype html><title>外壳</title>")
  fs.mkdirSync(path.join(dir, "shared"))
  fs.writeFileSync(path.join(dir, "shared", "a.js"), "window.__A = 1")
  return dir
}

/** 请求一次并返回 {status, body} */
function request (server, urlPath) {
  return new Promise(function (resolve, reject) {
    const addr = server.address()
    const req = http.request(
      { host: "127.0.0.1", port: addr.port, path: urlPath, method: "GET" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
}

test("组合根：命中路由时走 handler", async function (t) {
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/ping", handler: function (req, res) {
      res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" })
      res.end(JSON.stringify({ ok: true, pong: true }))
    } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/api/ping")
  assert.strictEqual(r.status, 200)
  assert.deepStrictEqual(JSON.parse(r.body), { ok: true, pong: true })
})

test("组合根：路径参数能被解析出来", async function (t) {
  let got = null
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/items/:id", handler: function (req, res, params) {
      got = params.id
      res.writeHead(200, { "Content-Type": "application/json; charset=utf-8" })
      res.end("{}")
    } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  await request(app.server, "/api/items/abc123")
  assert.strictEqual(got, "abc123")
})

test("组合根：路径不存在时返回 404 中文提示", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/api/nothing-here")
  assert.strictEqual(r.status, 404)
  assert.ok(r.body.indexOf("未找到") >= 0, "响应体应含中文提示，实际是：" + r.body)
})

test("组合根：静态文件能被托管，根路径回落到 index.html", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("外壳") >= 0)

  const r2 = await request(app.server, "/shared/a.js")
  assert.strictEqual(r2.status, 200)
  assert.ok(r2.body.indexOf("__A") >= 0)
})

test("组合根：静态路径越界被拒绝", async function (t) {
  const app = createApp({ webRoot: makeTempWebRoot(), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/../package.json")
  assert.ok(r.status === 403 || r.status === 404, "越界请求应被拒绝，实际状态码：" + r.status)
})

test("组合根：同一路径方法不匹配时返回 405 并带 Allow 头", async function (t) {
  const app = createApp({
    webRoot: makeTempWebRoot(),
    routes: [{ method: "GET", pattern: "/api/only-get", handler: function (req, res) { res.end("{}") } }]
  })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await new Promise(function (resolve, reject) {
    const addr = app.server.address()
    const req = http.request(
      { host: "127.0.0.1", port: addr.port, path: "/api/only-get", method: "POST" },
      function (res) {
        let body = ""
        res.on("data", function (c) { body += c })
        res.on("end", function () { resolve({ status: res.statusCode, headers: res.headers, body: body }) })
      }
    )
    req.on("error", reject)
    req.end()
  })
  assert.strictEqual(r.status, 405)
  assert.ok(r.headers.allow && r.headers.allow.indexOf("GET") >= 0, "405 响应必须带 Allow 头")
})

test("外壳：首页含六个模块导航入口", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  // 四个本页模块 + 两个独立服务（后者走各自端口，不再是 /study/ 与 /wiki/ 本地路径）
  const expected = ["/workbench/", "/knowledge/", "/trip/", "/games/"]
  expected.forEach(function (href) {
    assert.ok(r.body.indexOf(href) >= 0, "外壳应含入口：" + href)
  })
  assert.ok(r.body.indexOf("127.0.0.1:5174") >= 0, "外壳应含学习中枢的独立端口入口")
  assert.ok(r.body.indexOf("127.0.0.1:8000") >= 0, "外壳应含 Wiki 检索的独立端口入口")
})

test("workbench 模块的页面能被访问", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/workbench/pages/dashboard/index")
  assert.strictEqual(r.status, 200)
  assert.ok(r.body.indexOf("app-main") >= 0, "应返回 workbench 外壳")
  assert.ok(r.body.indexOf("/workbench/browser.js") >= 0, "外壳应引入模块入口脚本")
})

test("workbench 模块的静态资源可访问", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const loader = await request(app.server, "/cjs-loader.js")
  assert.strictEqual(loader.status, 200)

  const page = await request(app.server, "/workbench/pages/dashboard/index.js")
  assert.strictEqual(page.status, 200)
  assert.ok(page.body.indexOf("module.exports") >= 0, "页面模块应是 CommonJS 源码")
})

test("外壳：四个本页模块入口齐全", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.strictEqual(r.status, 200)
  ;["/workbench/", "/knowledge/", "/trip/", "/games/"].forEach(function (href) {
    assert.ok(r.body.indexOf(href) >= 0, "外壳应含入口：" + href)
  })
})

test("外壳：两个独立服务指向各自端口且开新标签页", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/")
  assert.ok(r.body.indexOf("http://127.0.0.1:5174/") >= 0, "应含 study-hub 的独立端口链接")
  assert.ok(r.body.indexOf("http://127.0.0.1:8000/") >= 0, "应含 wiki-rag 的独立端口链接")
  assert.ok(r.body.indexOf('target="_blank"') >= 0, "独立服务应开新标签页")
  assert.ok(r.body.indexOf("/shell.js") >= 0, "外壳应引入在线状态脚本")
})

test("接口：/api/health 返回两个后端的状态", async function (t) {
  const app = createApp({ webRoot: path.resolve(__dirname, "..", "web"), routes: [] })
  await app.listen(0)
  t.after(function () { return app.close() })

  const r = await request(app.server, "/api/health")
  assert.strictEqual(r.status, 200)
  const body = JSON.parse(r.body)
  assert.strictEqual(body.ok, true)
  assert.strictEqual(typeof body.study.ok, "boolean")
  assert.strictEqual(typeof body.wiki.ok, "boolean")
})
