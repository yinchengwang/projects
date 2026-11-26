"use strict"
/**
 * app.js —— 组合根
 *
 * 只做三件事：路由分发、静态托管、错误兜底。
 * 各业务模块（study-hub、wiki-rag 反代、各前端模块）由后续计划挂进来。
 */

const http = require("node:http")
const path = require("node:path")

const { createRouter, HttpError } = require("./router.js")
const { createStaticHandler } = require("./static.js")

const ROOT = path.resolve(__dirname, "..")
const WEB_DIR = path.join(ROOT, "web")
const SHARED_DIR = path.join(ROOT, "shared")

/**
 * 默认的静态挂载点。
 *
 * /shared/* 必须单独挂 —— 共用层在 apps/web/shared/，与页面静态根
 * apps/web/web/ 不在同一个目录。浏览器要用 cjs-loader fetch 到
 * /shared/core/storage.js 这类文件，就得把 shared 目录也暴露出去。
 */
const DEFAULT_MOUNTS = [{ prefix: "/shared/", dir: SHARED_DIR }]

/** 发 JSON 响应 */
function sendJson (res, status, body, extraHeaders) {
  const text = JSON.stringify(body)
  const headers = {
    "Content-Type": "application/json; charset=utf-8",
    "Content-Length": Buffer.byteLength(text)
  }
  if (extraHeaders) {
    Object.keys(extraHeaders).forEach(function (k) { headers[k] = extraHeaders[k] })
  }
  res.writeHead(status, headers)
  res.end(text)
}

/**
 * 创建 HTTP 服务。
 * @param {{routes?: Array, webRoot?: string, mounts?: Array<{prefix: string, dir: string}>,
 *          sharedDir?: string}} opts
 * @returns {{server: object, listen: Function, close: Function}}
 */
function createApp (opts) {
  const options = opts || {}
  const router = createRouter()
  const webRoot = options.webRoot || WEB_DIR
  const routes = options.routes || []
  routes.forEach(function (route) {
    router.add(route.method, route.pattern, route.handler)
  })
  // 静态挂载点：调用方显式传 mounts 就用它，否则用默认（含 /shared/）。
  // 显式传 webRoot 但没传 mounts 时，仍带上 /shared/ —— 那是共用层，永远需要。
  const mounts = options.mounts !== undefined
    ? options.mounts
    : [{ prefix: "/shared/", dir: options.sharedDir || SHARED_DIR }]
  const serveStatic = createStaticHandler({ webRoot: webRoot, mounts: mounts })

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
          if (res.headersSent || res.writableEnded || res.destroyed) {
            console.error("[apps/web] 接口 " + req.method + " " + rawPath + " 处理异常（响应已发出）：", err)
            if (!res.writableEnded && !res.destroyed) res.end()
            return
          }
          const isHttpError = err instanceof HttpError
          const status = isHttpError && err.status >= 400 && err.status <= 599 ? err.status : 500
          if (!isHttpError) {
            console.error("[apps/web] 接口 " + req.method + " " + rawPath + " 处理异常：", err)
          }
          sendJson(res, status, { error: isHttpError ? err.message : "服务端出错" })
        })
      return
    }

    if (found && found.methodNotAllowed) {
      const allow = (found.allowed && found.allowed.length) ? found.allowed.join(", ") : "GET"
      sendJson(res, 405, { error: "方法不允许", method: req.method }, { Allow: allow })
      return
    }

    if (serveStatic(req, res)) return

    sendJson(res, 404, { error: "未找到该路径：" + rawPath })
  })

  return {
    server: server,
    listen: function (port) {
      return new Promise(function (resolve, reject) {
        server.listen(port, "127.0.0.1", function () { resolve(server.address().port) })
        server.on("error", reject)
      })
    },
    close: function () {
      return new Promise(function (resolve) { server.close(function () { resolve() }) })
    }
  }
}

module.exports = {
  ROOT: ROOT,
  WEB_DIR: WEB_DIR,
  SHARED_DIR: SHARED_DIR,
  DEFAULT_MOUNTS: DEFAULT_MOUNTS,
  createApp: createApp,
  sendJson: sendJson
}
