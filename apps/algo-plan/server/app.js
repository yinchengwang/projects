"use strict"
/**
 * app.js —— 组合根：把 SQLite、题库 provider、API 装配到 HTTP 服务上
 *
 * 装配逻辑集中在这一个文件；server/main.js 只做命令行入口。
 */

const http = require("node:http")
const fs = require("node:fs")
const path = require("node:path")

const { createRouter } = require("./router.js")

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
 * 把 URL 路径映射到 web/ 内的文件；越界或编码非法一律拒绝（返回 null）。
 */
function resolveStatic(urlPath) {
  let clean
  try {
    clean = decodeURIComponent(urlPath.split("?")[0])
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
          if (res.headersSent) {
            res.end()
            return
          }
          const status = err && err.status ? err.status : 500
          sendJson(res, status, {
            error: (err && err.message) || "服务端出错",
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

    if (rawPath.indexOf("/api/") === 0) {
      sendJson(res, 404, { error: "接口不存在", path: rawPath })
      return
    }

    const file = resolveStatic(rawPath)
    if (!file) {
      res.writeHead(400, { "content-type": "text/plain; charset=utf-8" })
      res.end("400 非法路径")
      return
    }
    if (!fs.existsSync(file) || !fs.statSync(file).isFile()) {
      res.writeHead(404, { "content-type": "text/plain; charset=utf-8" })
      res.end("404 找不到")
      return
    }
    const body = fs.readFileSync(file)
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
        server.once("error", reject)
        server.listen(port, "127.0.0.1", function () {
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
