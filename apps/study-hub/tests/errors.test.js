"use strict"
/**
 * errors.test.js —— 错误路径：请求体超限、异常兜底、状态码透传
 *
 * 这些都是「只有出错时才走」的分支，最容易因为没测而悄悄坏掉。
 */

const test = require("node:test")
const assert = require("node:assert")
const path = require("node:path")
const { spawn, spawnSync } = require("node:child_process")

const { createServer } = require("../server/app.js")
const { HttpError, MAX_BODY_BYTES, readJsonBody } = require("../server/router.js")

/**
 * 病态大 body 的字节数：10MB，远超旧实现里 8MB 的「中途断连」阈值。
 * 用它来钉住「大 body 也必须收到 413，而不是连接重置」这条契约。
 */
const HUGE_BODY_BYTES = 10 * 1024 * 1024

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

/** 等一会儿 */
function sleep(ms) {
  return new Promise(function (resolve) {
    setTimeout(resolve, ms)
  })
}

/** 轮询等待条件成立，最多等 5 秒；超时返回 null */
async function waitFor(fn) {
  const deadline = Date.now() + 5000
  while (Date.now() < deadline) {
    const value = fn()
    if (value) return value
    await sleep(50)
  }
  return null
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

test("超过 8MB 的大 body 也收到 413 与中文 JSON（不是连接重置）", async function () {
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
      const big = "x".repeat(HUGE_BODY_BYTES)
      const res = await fetch("http://127.0.0.1:" + port + "/api/echo", {
        method: "POST",
        headers: { "content-type": "application/json" },
        body: big,
        signal: AbortSignal.timeout(30000),
      })
      // 旧实现：读到 8MB 兜底阈值就 req.destroy()，内核回 RST 把 413 响应冲掉，
      // 客户端拿到的是 fetch failed / ECONNRESET，而不是 413
      assert.strictEqual(res.status, 413, "大 body 也必须回 413，不能是连接重置")
      const body = await res.json()
      assert.strictEqual(body.error, "请求体过大（上限 1MB）")
    }
  )
})

test("响应已经发出之后再抛的异常，服务端必须留下日志", async function () {
  const capture = captureConsoleError()
  try {
    await withServer(
      [
        {
          method: "GET",
          pattern: "/api/late",
          handler: function (req, res) {
            res.writeHead(200, { "content-type": "text/plain; charset=utf-8" })
            res.end("ok")
            // 响应已经落地才炸：状态码改不了了，但异常不能凭空消失
            throw new TypeError("响应发出之后才炸的异常")
          },
        },
      ],
      async function (port) {
        const res = await fetch("http://127.0.0.1:" + port + "/api/late")
        assert.strictEqual(res.status, 200)
        assert.strictEqual(await res.text(), "ok")
      }
    )
    assert.ok(capture.lines.length > 0, "响应发出之后的异常也必须留下日志，不能被静默吞掉")
    const text = capture.lines.join("\n")
    assert.ok(text.indexOf("TypeError") !== -1, "日志里应有异常类型，实际：\n" + text)
    assert.ok(text.indexOf("响应发出之后才炸的异常") !== -1, "日志里应有异常信息，实际：\n" + text)
    assert.ok(text.indexOf("at ") !== -1, "日志里应有调用堆栈，实际：\n" + text)
  } finally {
    capture.restore()
  }
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

test("端口被占用时明确告诉用户是端口占用", function () {
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
  const stderr = res.stderr || ""
  assert.strictEqual(res.status, 1, "端口被占用时应以退出码 1 退出，实际输出：\n" + stderr)
  assert.ok(
    stderr.indexOf("端口") !== -1 && stderr.indexOf("占用") !== -1,
    "端口被占用时必须明确说出来，实际输出：\n" + stderr
  )
})

test("listen 成功但启动回调出错时，不再误报「无法监听端口」且不杀掉服务", async function () {
  const appPath = path.join(__dirname, "..", "server", "app.js")
  const mainPath = path.join(__dirname, "..", "server", "main.js")
  const script = [
    "const http = require('node:http')",
    "const appModule = require(" + JSON.stringify(appPath) + ")",
    "appModule.createApp = function () {",
    "  return {",
    "    learning: undefined,", // 读 app.learning.ok 会抛 TypeError
    "    listen: function () {",
    "      return new Promise(function (resolve) {",
    "        const s = http.createServer(function (req, res) { res.end('still alive') })",
    "        s.listen(0, '127.0.0.1', function () {",
    "          console.log('PORT=' + s.address().port)",
    "          resolve(s.address().port)",
    "        })",
    "      })",
    "    },",
    "  }",
    "}",
    "require(" + JSON.stringify(mainPath) + ").main()",
  ].join("\n")
  const child = spawn(process.execPath, ["-e", script], { stdio: ["ignore", "pipe", "pipe"] })
  let stdout = ""
  let stderr = ""
  child.stdout.on("data", function (chunk) {
    stdout += chunk.toString()
  })
  child.stderr.on("data", function (chunk) {
    stderr += chunk.toString()
  })
  try {
    // 等假服务把端口号打出来
    const port = await waitFor(function () {
      const m = /PORT=(\d+)/.exec(stdout)
      return m ? m[1] : null
    })
    assert.ok(port, "假服务没能启动起来，实际输出：\n" + stdout + stderr)
    // 再等一会儿，让 .then 回调里的异常冒出来
    await sleep(500)
    assert.strictEqual(
      child.exitCode,
      null,
      "listen 已经成功，服务不该被 process.exit(1) 杀掉，实际退出码：" + child.exitCode
    )
    assert.ok(
      stderr.indexOf("无法监听端口") === -1,
      "异常不是端口问题，不该说「无法监听端口」，实际输出：\n" + stderr
    )
    assert.ok(
      stderr.indexOf("占用") === -1,
      "异常不是端口问题，不该提端口占用，实际输出：\n" + stderr
    )
    assert.ok(stderr.indexOf("启动") !== -1, "应该打印中文提示，实际输出：\n" + stderr)
    assert.ok(stderr.indexOf("TypeError") !== -1, "应该带上异常类型，实际输出：\n" + stderr)
    // 服务还活着：说明异常没有把已经监听的服务器杀掉
    const res = await fetch("http://127.0.0.1:" + port + "/")
    assert.strictEqual(await res.text(), "still alive")
  } finally {
    child.kill()
  }
})
