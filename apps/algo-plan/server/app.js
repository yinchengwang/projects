"use strict"
/**
 * app.js —— 组合根：把 SQLite、题库 provider、API 装配到 HTTP 服务上
 *
 * 装配逻辑集中在这一个文件；server/main.js 只做命令行入口。
 */

const http = require("node:http")
const fs = require("node:fs")
const path = require("node:path")

const { createRouter, HttpError } = require("./router.js")

const ROOT = path.resolve(__dirname, "..")
const WEB_DIR = path.join(ROOT, "web")

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".ico": "image/x-icon",
}

/**
 * 回 JSON 响应。
 */
function sendJson(res, status, body) {
  const text = JSON.stringify(body)
  res.writeHead(status, {
    "content-type": "application/json; charset=utf-8",
    "content-length": Buffer.byteLength(text),
    "cache-control": "no-store",
  })
  res.end(text)
}

/**
 * API 请求前缀判定：/api 与 /api/... 都算 API 请求。
 * 统一成一个 helper，免得 /api 漏到静态分支去回 text/plain 的 404，
 * 跟 /api/nope 的 JSON 404 对不上。
 */
function isApiPath(urlPath) {
  return urlPath === "/api" || urlPath.indexOf("/api/") === 0
}

/**
 * 把 URL 路径映射到 web/ 内的文件；越界或编码非法一律拒绝（返回 null）。
 * 入参是已经切掉查询串的路径。
 */
function resolveStatic(urlPath) {
  let clean
  try {
    clean = decodeURIComponent(urlPath)
  } catch (e) {
    return null
  }
  const rel = clean === "/" ? "index.html" : clean.replace(/^\/+/, "")
  const abs = path.resolve(WEB_DIR, rel)
  if (abs !== WEB_DIR && abs.indexOf(WEB_DIR + path.sep) !== 0) return null
  return abs
}

/**
 * 创建 HTTP 服务。
 * @param {{routes?: Array}} opts
 */
function createServer(opts) {
  const options = opts || {}
  const router = createRouter()
  const routes = options.routes || []
  /** 常驻的 server 错误监听器只装一次，避免 listen 被调多次时重复挂 */
  let runtimeErrorHandled = false
  routes.forEach(function (route) {
    router.add(route.method, route.pattern, route.handler)
  })

  const server = http.createServer(function (req, res) {
    const rawUrl = req.url || "/"
    const rawPath = rawUrl.split("?")[0]
    const query = new URLSearchParams(rawUrl.indexOf("?") === -1 ? "" : rawUrl.slice(rawUrl.indexOf("?") + 1))
    const found = router.find(req.method, rawPath)

    if (found && found.route) {
      Promise.resolve()
        .then(function () {
          return found.route.handler(req, res, found.params, query)
        })
        .catch(function (err) {
          // 响应已经开始发出、或连接已经断了：不能再改状态码，只能收尾
          if (res.headersSent || res.writableEnded || res.destroyed) {
            if (!res.writableEnded && !res.destroyed) res.end()
            return
          }
          // 只有 HttpError 的文案是「我们自己写的中文」，可以直接透给浏览器；
          // 其它异常（TypeError 之类）是原生英文报错，透出去既看不懂也暴露实现细节
          const isHttpError = err instanceof HttpError
          // 只认 4xx/5xx：非法状态码会让 res.writeHead 抛错，反而把服务带崩
          const status = isHttpError && err.status >= 400 && err.status <= 599 ? err.status : 500
          if (!isHttpError) {
            // 服务端留一份完整堆栈，不然异常在终端里毫无痕迹
            console.error("[algo-plan] 接口 " + req.method + " " + rawPath + " 处理异常：", err)
          }
          sendJson(res, status, {
            error: isHttpError ? err.message : "服务端出错",
          })
        })
      return
    }

    if (found && found.methodNotAllowed) {
      sendJson(res, 405, {
        error: "方法不允许",
        method: req.method,
        path: rawPath,
      })
      return
    }

    if (isApiPath(rawPath)) {
      sendJson(res, 404, { error: "接口不存在", path: rawPath })
      return
    }

    const file = resolveStatic(rawPath)
    if (!file) {
      res.writeHead(400, { "content-type": "text/plain; charset=utf-8" })
      res.end("400 非法路径")
      return
    }
    let body
    try {
      if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
        res.writeHead(404, { "content-type": "text/plain; charset=utf-8" })
        res.end("404 找不到")
        return
      }
      body = fs.readFileSync(file)
    } catch (err) {
      // Windows 上文件被独占锁定（EPERM/EBUSY）时 statSync/readFileSync 会抛错。
      // 不兜住的话异常会从请求回调逃出去，Node 没有 uncaughtException 处理，整个服务挂掉。
      const code = err && err.code ? err.code : "未知错误"
      console.error("[algo-plan] 读取静态文件失败：" + file + "（" + code + "）")
      if (code === "ENOENT" || code === "ENOTDIR") {
        res.writeHead(404, { "content-type": "text/plain; charset=utf-8" })
        res.end("404 找不到")
        return
      }
      res.writeHead(500, { "content-type": "text/plain; charset=utf-8" })
      res.end("500 服务器内部错误")
      return
    }
    res.writeHead(200, {
      "content-type": MIME[path.extname(file)] || "application/octet-stream",
      "content-length": body.length,
      "cache-control": "no-store",
    })
    res.end(body)
  })

  return {
    server: server,
    listen: function (port) {
      return new Promise(function (resolve, reject) {
        function onListenError(err) {
          reject(err)
        }
        server.once("error", onListenError)
        server.listen(port, "127.0.0.1", function () {
          // listen 成功后撤掉这个一次性监听器：它留在 server 上就是空转，
          // 第一次 error 被已经 settle 的 reject 吞掉，第二次 error 就没人接（未捕获异常）。
          server.removeListener("error", onListenError)
          if (!runtimeErrorHandled) {
            runtimeErrorHandled = true
            // 常驻错误处理器：服务运行期的 socket 错误只记中文日志，不让进程崩掉
            server.on("error", function (err) {
              console.error(
                "[algo-plan] 服务运行期错误：" + (err && err.code ? err.code : "未知错误"),
                err
              )
            })
          }
          resolve(server.address().port)
        })
      })
    },
    close: function () {
      return new Promise(function (resolve) {
        server.close(function () {
          resolve()
        })
      })
    },
  }
}

/**
 * 装配整个应用。
 * @param {{dbPath?: string, learningRoot?: string, env?: object}} opts
 *   dbPath：SQLite 文件路径，默认 data/app.db；传 ":memory:" 用内存库
 *   learningRoot：learning 仓根目录；不传则读环境变量
 */
function createApp(opts) {
  const options = opts || {}

  // 延迟 require：core/ 与 db/api 在后续 Task 才落地，createServer 不依赖它们
  const { resolveLearningRoot } = require("../core/paths.js")
  const { FileProvider } = require("../core/bank/provider.js")
  const { openDb, createStore } = require("./db.js")

  const db = openDb(options.dbPath || path.join(ROOT, "data", "app.db"))
  const store = createStore(db)

  const learning = resolveLearningRoot(options.env || process.env, options.learningRoot)
  const provider = new FileProvider(learning.ok ? path.join(learning.root, "bank") : null)

  const routes = require("./api.js").buildRoutes({
    store: store,
    provider: provider,
    learning: learning,
    sendJson: sendJson,
  })

  const server = createServer({ routes: routes })

  return {
    server: server.server,
    listen: server.listen,
    close: function () {
      return server.close().then(function () {
        db.close()
      })
    },
    db: db,
    store: store,
    provider: provider,
    learning: learning,
  }
}

module.exports = { ROOT: ROOT, WEB_DIR: WEB_DIR, createServer: createServer, createApp: createApp, sendJson: sendJson }
