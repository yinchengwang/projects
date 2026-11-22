"use strict"
/**
 * remediation-settings-python.test.js —— 判题解释器配置闸门
 *
 * 补齐设计 §5：本机装了多个 Python，默认探测到的那个可能没有 numpy
 * （实测：python 3.11 无 numpy，py -3 3.12 有 numpy 2.5.5）。
 * 解释器要能从设置页改，且环境自检、判题、离线 CLI 用同一个口径。
 */

const test = require("node:test")
const assert = require("node:assert/strict")
const fs = require("node:fs")
const http = require("node:http")
const path = require("node:path")

const sessionRoutes = require("../server/api-session.js")

/**
 * 起一个只挂设置路由的假服务。
 * 路由的 handler 签名是 (req, res)，body 由 readJsonBody 从 req 流里读，
 * 所以这里要把请求体原样交给它。
 *
 * **调用方必须在 finally 里 close**：服务不关，node:test 的进程就不会退出。
 */
async function withServer(ctx, fn) {
  const routes = sessionRoutes.buildRoutes(ctx)
  const server = http.createServer(function (req, res) {
    const url = new URL(req.url, "http://127.0.0.1")
    const route = routes.find(function (r) {
      return r.method === req.method && r.pattern === url.pathname
    })
    if (!route) {
      res.writeHead(404)
      res.end("{}")
      return
    }
    route.handler(req, res).catch(function () {
      res.writeHead(500)
      res.end("{}")
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

function fakeStore(settings) {
  const map = Object.assign({}, settings || {})
  return {
    getSetting: function (k) { return map[k] === undefined ? null : map[k] },
    setSetting: function (k, v) { map[k] = String(v) },
    listUnits: function () { return [] },
    listSessions: function () { return [] },
    listQueue: function () { return [] },
    listReviewCards: function () { return [] },
    getSession: function () { return null },
    _map: map,
  }
}

/** 一个只够设置接口用的 ctx。 */
function fakeCtx(store, extra) {
  return Object.assign(
    {
      store: store,
      learning: { ok: false, root: null, reason: "没连上" },
      sendJson: function (res, code, body) {
        res.writeHead(code, { "content-type": "application/json" })
        res.end(JSON.stringify(body))
      },
      envProbe: function () {
        return { python: { ok: true, detail: "python" }, numpy: { ok: false, detail: "缺 numpy" } }
      },
    },
    extra || {}
  )
}

test("PUT /api/settings 接受 python_cmd，并写进 settings", async function () {
  const store = fakeStore()
  await withServer(fakeCtx(store), async function (base) {
    const res = await fetch(base + "/api/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ python_cmd: "py -3" }),
    })
    assert.strictEqual(res.status, 200)
    assert.strictEqual(store._map["judge.python.cmd"], "py -3")
  })
})

test("PUT /api/settings 拒绝空白的 python_cmd", async function () {
  const store = fakeStore()
  await withServer(fakeCtx(store), async function (base) {
    const res = await fetch(base + "/api/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ python_cmd: "   " }),
    })
    assert.strictEqual(res.status, 400)
    const body = await res.json()
    assert.ok(body.error.indexOf("解释器") !== -1, JSON.stringify(body))
  })
})

test("PUT /api/settings 拒绝探测不到的解释器，不写入", async function () {
  const store = fakeStore()
  await withServer(fakeCtx(store), async function (base) {
    const res = await fetch(base + "/api/settings", {
      method: "PUT",
      headers: { "content-type": "application/json" },
      body: JSON.stringify({ python_cmd: "definitely-not-a-python-xyz --bogus-flag" }),
    })
    assert.strictEqual(res.status, 400)
    assert.strictEqual(store._map["judge.python.cmd"], undefined, "探测不到就不该写入")
  })
})

test("改解释器会触发单元索引重扫", async function () {
  const store = fakeStore()
  let reloaded = 0
  await withServer(
    fakeCtx(store, {
      reloadUnits: function () {
        reloaded += 1
      },
    }),
    async function (base) {
      const res = await fetch(base + "/api/settings", {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ python_cmd: "py -3" }),
      })
      assert.strictEqual(res.status, 200)
      assert.strictEqual(reloaded, 1, "解释器改了要重扫一次")
    }
  )
})

test("只改别的设置时不重扫单元索引", async function () {
  const store = fakeStore()
  let reloaded = 0
  await withServer(
    fakeCtx(store, {
      reloadUnits: function () {
        reloaded += 1
      },
    }),
    async function (base) {
      const res = await fetch(base + "/api/settings", {
        method: "PUT",
        headers: { "content-type": "application/json" },
        body: JSON.stringify({ session_count: 5 }),
      })
      assert.strictEqual(res.status, 200)
      assert.strictEqual(reloaded, 0, "没改解释器就不该重扫")
    }
  )
})

test("probeEnv 回带解释器命令与 numpy 版本", function () {
  const env = sessionRoutes.probeEnv({
    // 假 spawn：只认 numpy 版本号那条命令
    spawn: function (cmd, args) {
      const joined = args.join(" ")
      if (joined.indexOf("numpy.__version__") !== -1) {
        return { status: 0, stdout: "2.5.5\n", stderr: "" }
      }
      return { status: 1, stdout: "", stderr: "boom" }
    },
    python: { cmd: "py", args: ["-3"], version: "Python 3.12.3" },
  })
  assert.strictEqual(env.python.cmd, "py")
  assert.deepStrictEqual(env.python.args, ["-3"])
  assert.strictEqual(env.numpy.ok, true)
  assert.strictEqual(env.numpy.version, "2.5.5")
})

test("CLI 的 loadUnits 与 verifyBatch 都带上同一个 pythonOverride", function () {
  // 这是差异清单 D-13b 的防回归闸门：离线 CLI 必须与判题读同一个解释器配置。
  const src = fs.readFileSync(path.join(__dirname, "..", "tools", "supply.js"), "utf8")
  const loadUnitsAt = src.indexOf("const loaded = loadUnits({")
  assert.ok(loadUnitsAt !== -1, "找不到 loadUnits 调用")
  const loadUnitsBlock = src.slice(loadUnitsAt, loadUnitsAt + 900)
  assert.ok(loadUnitsBlock.indexOf("pythonOverride") !== -1, "loadUnits 没传 pythonOverride")

  const batchAt = src.indexOf("await verifyBatch({")
  assert.ok(batchAt !== -1, "找不到 verifyBatch 调用")
  const batchBlock = src.slice(batchAt, batchAt + 600)
  assert.ok(batchBlock.indexOf("pythonOverride") !== -1, "verifyBatch 没传 pythonOverride")
})
