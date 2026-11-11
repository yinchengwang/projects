"use strict"
/**
 * errors.test.js —— 错误路径：请求体超限、异常兜底、状态码透传
 *
 * 这些都是「只有出错时才走」的分支，最容易因为没测而悄悄坏掉。
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")
const { spawnSync } = require("node:child_process")

const { createServer } = require("../server/app.js")
const { HttpError, MAX_BODY_BYTES, readJsonBody } = require("../server/router.js")

/** 起一个服务，跑一段逻辑，最后一定关掉 */
async function withServer(routes, fn) {
  const app = createServer({ routes: routes })
  const port = await app.listen(0)
  try {
    return await fn(port)
  } finally {
    await app.close()
  }
}

/** 临时接管 console.error，把服务端日志抓下来 */
function captureConsoleError() {
  const original = console.error
  const lines = []
  console.error = function () {
    const args = Array.prototype.slice.call(arguments)
    lines.push(
      args
        .map(function (arg) {
          return arg instanceof Error ? arg.stack : String(arg)
        })
        .join("\n")
    )
  }
  return {
    lines: lines,
    restore: function () {
      console.error = original
    },
  }
}

test("请求体超过 1MB 时客户端收到 413 与中文 JSON", async function () {
  await withServer(
    [
      {
        method: "POST",
        pattern: "/api/echo",
        handler: async function (req, res) {
          await readJsonBody(req)
          res.end("ok")
        },
      },
    ],
    async function (port) {
      const big = "x".repeat(MAX_BODY_BYTES + 1024)
      const res = await fetch("http://127.0.0.1:" + port + "/api/echo", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: big,
        signal: AbortSignal.timeout(15000),
      })
      assert.strictEqual(res.status, 413)
      const body = await res.json()
      assert.strictEqual(body.error, "请求体过大（上限 1MB）")
    }
  )
})

test("handler 抛非 HttpError 时返回 500 中文文案", async function () {
  // 这里顺手把服务端日志接住，免得测试输出里混进一坨堆栈
  const capture = captureConsoleError()
  try {
    await withServer(
      [
        {
          method: "GET",
          pattern: "/api/boom",
          handler: function () {
            throw new TypeError("Cannot read properties of undefined (reading 'id')")
          },
        },
      ],
      async function (port) {
        const res = await fetch("http://127.0.0.1:" + port + "/api/boom")
        assert.strictEqual(res.status, 500)
        const body = await res.json()
        assert.strictEqual(body.error, "服务端出错")
        assert.strictEqual(body.error.indexOf("Cannot read"), -1, "不应把英文原始报错透给浏览器")
      }
    )
  } finally {
    capture.restore()
  }
})

test("handler 抛非 HttpError 时服务端留下完整堆栈日志", async function () {
  const capture = captureConsoleError()
  try {
    await withServer(
      [
        {
          method: "GET",
          pattern: "/api/boom",
          handler: function () {
            throw new TypeError("Cannot read properties of undefined (reading 'id')")
          },
        },
      ],
      async function (port) {
        const res = await fetch("http://127.0.0.1:" + port + "/api/boom")
        assert.strictEqual(res.status, 500)
      }
    )
    assert.ok(capture.lines.length > 0, "服务端应该打日志，不能把异常静默吞掉")
    const text = capture.lines.join("\n")
    assert.ok(text.indexOf("TypeError") !== -1, "日志里应有异常类型")
    assert.ok(text.indexOf("Cannot read properties") !== -1, "日志里应有异常信息")
    assert.ok(text.indexOf("at ") !== -1, "日志里应有调用堆栈")
  } finally {
    capture.restore()
  }
})

test("HttpError 的 status 透传到 HTTP 响应状态码", async function () {
  await withServer(
    [
      {
        method: "GET",
        pattern: "/api/missing",
        handler: function () {
          throw new HttpError(404, "题目不存在")
        },
      },
      {
        method: "GET",
        pattern: "/api/teapot",
        handler: function () {
          throw new HttpError(418, "我是茶壶")
        },
      },
    ],
    async function (port) {
      const res404 = await fetch("http://127.0.0.1:" + port + "/api/missing")
      assert.strictEqual(res404.status, 404)
      const body404 = await res404.json()
      assert.strictEqual(body404.error, "题目不存在")

      const res418 = await fetch("http://127.0.0.1:" + port + "/api/teapot")
      assert.strictEqual(res418.status, 418)
      const body418 = await res418.json()
      assert.strictEqual(body418.error, "我是茶壶")
    }
  )
})

test("handler 收到的 query 是 URLSearchParams 实例", async function () {
  let received = null
  await withServer(
    [
      {
        method: "GET",
        pattern: "/api/query",
        handler: function (req, res, params, query) {
          received = query
          res.end("ok")
        },
      },
    ],
    async function (port) {
      const res = await fetch("http://127.0.0.1:" + port + "/api/query?a=1&b=%E4%B8%AD")
      assert.strictEqual(res.status, 200)
      assert.ok(received instanceof URLSearchParams, "query 应该是 URLSearchParams 实例")
      assert.strictEqual(received.get("a"), "1")
      assert.strictEqual(received.get("b"), "中")
    }
  )
})

test("/api（不带尾斜杠）返回与 /api/nope 一致的 JSON 404", async function () {
  await withServer([], async function (port) {
    const res = await fetch("http://127.0.0.1:" + port + "/api")
    assert.strictEqual(res.status, 404)
    assert.strictEqual(res.headers.get("content-type"), "application/json; charset=utf-8")
    const body = await res.json()
    assert.strictEqual(body.error, "接口不存在")
  })
})

test("静态文件读失败时按错误类型分流（EPERM 走 500，服务不挂）", async function () {
  const fs = require("node:fs")
  const original = fs.readFileSync
  const capture = captureConsoleError()
  try {
    // 模拟 Windows 上文件被独占锁定：readFileSync 抛 EPERM
    fs.readFileSync = function (file) {
      if (String(file).indexOf("style.css") !== -1) {
        const err = new Error("EPERM: operation not permitted, open '" + file + "'")
        err.code = "EPERM"
        throw err
      }
      return original.apply(fs, arguments)
    }
    await withServer([], async function (port) {
      const res = await fetch("http://127.0.0.1:" + port + "/style.css")
      assert.strictEqual(res.status, 500)
      assert.ok((await res.text()).indexOf("500") !== -1)
      // 服务还活着：再请求一次首页
      const ok = await fetch("http://127.0.0.1:" + port + "/")
      assert.strictEqual(ok.status, 200)
    })
  } finally {
    fs.readFileSync = original
    capture.restore()
  }
})

test("listen 成功后运行期错误只记日志，不会变成未捕获异常", async function () {
  const capture = captureConsoleError()
  try {
    const app = createServer({ routes: [] })
    const port = await app.listen(0)
    try {
      // 旧实现：第一次 error 被已 settle 的 reject 吞掉（无日志），
      // 第二次 error 就没监听器了 → 未捕获异常，整个进程挂掉
      app.server.emit("error", new Error("模拟第一次错误"))
      app.server.emit("error", new Error("模拟第二次错误"))
      assert.strictEqual(capture.lines.length, 2, "两次运行期错误都应该留下中文日志")
      assert.ok(capture.lines.join("\n").indexOf("服务运行期错误") !== -1)
    } finally {
      await app.close()
    }
  } finally {
    capture.restore()
  }
})

test("main.js 启动失败时打印中文错误并以退出码 1 退出", function () {
  // createApp 要到 Task 6 才落地，这里把 app.js 的导出换成假的 listen，
  // 用子进程跑 main()，验证端口被占用这类失败不会变成未捕获 rejection
  const appPath = path.join(__dirname, "..", "server", "app.js")
  const mainPath = path.join(__dirname, "..", "server", "main.js")
  const script = [
    "const appModule = require(" + JSON.stringify(appPath) + ")",
    "appModule.createApp = function () {",
    "  return {",
    "    learning: { ok: false, reason: '测试' },",
    "    listen: function () {",
    "      const err = new Error('listen EADDRINUSE 127.0.0.1:5173')",
    "      err.code = 'EADDRINUSE'",
    "      return Promise.reject(err)",
    "    },",
    "  }",
    "}",
    "require(" + JSON.stringify(mainPath) + ").main()",
  ].join("\n")
  const res = spawnSync(process.execPath, ["-e", script], { encoding: "utf8" })
  assert.strictEqual(res.status, 1, "启动失败时应以退出码 1 退出，实际输出：\n" + res.stderr)
  assert.ok(
    (res.stderr || "").indexOf("启动失败") !== -1,
    "应该打印中文提示，实际输出：\n" + res.stderr
  )
  assert.ok(
    (res.stderr || "").indexOf("EADDRINUSE") !== -1,
    "应该带上错误码，实际输出：\n" + res.stderr
  )
})
