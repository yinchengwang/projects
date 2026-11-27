"use strict"
/**
 * health.test.js —— 后端在线状态探测
 */

const test = require("node:test")
const assert = require("node:assert")
const http = require("node:http")

const { probe, checkBackends } = require("../server/health.js")

/** 起一个临时 HTTP 服务，用来测「在线」的情况 */
function startServer (handler) {
  return new Promise(function (resolve) {
    const server = http.createServer(handler)
    server.listen(0, "127.0.0.1", function () {
      resolve({ server: server, port: server.address().port })
    })
  })
}

test("probe：服务在线时 ok=true", async function () {
  const s = await startServer(function (req, res) {
    res.writeHead(200, { "Content-Type": "application/json" })
    res.end(JSON.stringify({ status: "ok" }))
  })
  try {
    const r = await probe("http://127.0.0.1:" + s.port + "/api/health")
    assert.strictEqual(r.ok, true)
    assert.strictEqual(r.status, 200)
  } finally {
    s.server.close()
  }
})

test("probe：服务未启动时 ok=false 且带中文说明", async function () {
  // 找一个没人监听的端口
  const s = await startServer(function () {})
  const port = s.port
  await new Promise(function (resolve) { s.server.close(resolve) })

  const r = await probe("http://127.0.0.1:" + port + "/api/health", 500)
  assert.strictEqual(r.ok, false)
  assert.ok(r.error && r.error.length > 0, "应有错误说明")
})

test("probe：超时返回 ok=false，不挂住", async function () {
  const s = await startServer(function () { /* 故意不响应 */ })
  try {
    const r = await probe("http://127.0.0.1:" + s.port + "/", 300)
    assert.strictEqual(r.ok, false)
    assert.ok(r.error.indexOf("超时") >= 0 || r.error.indexOf("连接") >= 0,
      "错误说明应提到超时或连接，实际：" + r.error)
  } finally {
    s.server.close()
  }
})

test("checkBackends：返回 study 与 wiki 两项", async function () {
  const r = await checkBackends()
  assert.ok(r.study, "应有 study 项")
  assert.ok(r.wiki, "应有 wiki 项")
  assert.strictEqual(typeof r.study.ok, "boolean")
  assert.strictEqual(typeof r.wiki.ok, "boolean")
})
