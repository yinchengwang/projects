"use strict"
/**
 * app.js —— 组合根
 *
 * 只做三件事：路由分发、静态托管、错误兜底。
 * 各业务模块（study-hub、wiki-rag 反代、各前端模块）由后续计划挂进来。
 */

const http = require("node:http")
const fs = require("node:fs")
const path = require("node:path")

const { createRouter, HttpError } = require("./router.js")
const { createStaticHandler } = require("./static.js")
const { checkBackends } = require("./health.js")
const { registerAiTimerRoutes } = require("./aitimer.js")

const ROOT = path.resolve(__dirname, "..")
const WEB_DIR = path.join(ROOT, "web")
const SHARED_DIR = path.join(ROOT, "shared")

/** workbench 模块的 HTML 外壳：所有 /workbench/pages/* 都返回它 */
const WORKBENCH_SHELL = path.join(WEB_DIR, "workbench", "shell.html")

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

  // 后端在线状态：study-hub 与 wiki-rag 是独立进程，外壳要用它提示「还没启动」
  router.add("GET", "/api/health", function (req, res) {
    checkBackends().then(function (result) {
      sendJson(res, 200, { ok: true, study: result.study, wiki: result.wiki })
    }).catch(function (err) {
      sendJson(res, 200, { ok: false, error: (err && err.message) || "探测失败" })
    })
  })

  // workbench 模块：所有 /workbench/pages/* 都返回同一个 HTML 壳，
  // 实际页面由前端的 cjs-loader 按路径加载。
  // 带扩展名的请求（页面模块 .js、样式 .css）必须落到静态托管，否则壳会盖住源码。
  const shellPath = options.workbenchShell || WORKBENCH_SHELL
  const workbenchShell = function (req, res) {
    const reqPath = (req.url || "/").split("?")[0]
    if (/\.[a-z0-9]+$/i.test(reqPath)) {
      if (serveStatic(req, res)) return
      sendJson(res, 404, { error: "未找到该路径：" + reqPath })
      return
    }
    let html
    try {
      html = fs.readFileSync(shellPath, "utf8")
    } catch (e) {
      sendJson(res, 500, { error: "workbench 页面壳读取失败" })
      return
    }
    res.writeHead(200, {
      "Content-Type": "text/html; charset=utf-8",
      "Content-Length": Buffer.byteLength(html)
    })
    res.end(html)
  }
  // 静态挂载点：调用方显式传 mounts 就用它，否则用默认（含 /shared/）。
  // 显式传 webRoot 但没传 mounts 时，仍带上 /shared/ —— 那是共用层，永远需要。
  const mounts = options.mounts !== undefined
    ? options.mounts
    : [{ prefix: "/shared/", dir: options.sharedDir || SHARED_DIR }]
  const serveStatic = createStaticHandler({ webRoot: webRoot, mounts: mounts })

  router.add("GET", "/workbench/pages/:a/:b/:c", workbenchShell)
  router.add("GET", "/workbench/pages/:a/:b", workbenchShell)
  // 模块入口：/workbench/ 与 /workbench 直接进今日页，避免落到目录列表。
  const workbenchEntry = function (req, res) {
    res.writeHead(302, { Location: "/workbench/pages/dashboard/index" })
    res.end()
  }
  router.add("GET", "/workbench", workbenchEntry)
  router.add("GET", "/workbench/", workbenchEntry)

  /* ------------------------------------------------------------
   * trip 与 games 模块：模式与 workbench 一致 —— /模块/pages/* 一律返回
   * 该模块的 HTML 壳，实际页面由前端 cjs-loader 按路径加载。
   * 带扩展名的请求（页面模块 .js、样式 .css）落到静态托管。
   * ---------------------------------------------------------- */

  /**
   * 造一个「返回模块 HTML 壳」的 handler。
   * @param {string} file 壳文件绝对路径
   * @param {string} errMsg 读不到壳时的中文错误
   * @returns {Function} (req, res) => void
   */
  const makeModuleShell = function (file, errMsg) {
    return function (req, res) {
      const reqPath = (req.url || "/").split("?")[0]
      if (/\.[a-z0-9]+$/i.test(reqPath)) {
        if (serveStatic(req, res)) return
        sendJson(res, 404, { error: "未找到该路径：" + reqPath })
        return
      }
      let html
      try {
        html = fs.readFileSync(file, "utf8")
      } catch (e) {
        sendJson(res, 500, { error: errMsg })
        return
      }
      res.writeHead(200, {
        "Content-Type": "text/html; charset=utf-8",
        "Content-Length": Buffer.byteLength(html)
      })
      res.end(html)
    }
  }

  const tripShell = makeModuleShell(
    path.join(WEB_DIR, "trip", "shell.html"),
    "trip 模块页面壳读取失败"
  )
  const gamesShell = makeModuleShell(
    path.join(WEB_DIR, "games", "shell.html"),
    "games 模块页面壳读取失败"
  )

  router.add("GET", "/trip/pages/:a", tripShell)
  // 兼容「页面路径本身以 index 结尾」的写法（如 /trip/pages/index/index）：
  // 与 workbench 一样，多一段的路径也返回同一个壳，前端再做归一。
  router.add("GET", "/trip/pages/:a/:b", tripShell)
  router.add("GET", "/games/pages/:a", gamesShell)
  router.add("GET", "/games/pages/:a/:b", gamesShell)

  /** 造一个「模块入口跳到首页」的 handler */
  const makeModuleEntry = function (target) {
    return function (req, res) {
      res.writeHead(302, { Location: target })
      res.end()
    }
  }
  const tripEntry = makeModuleEntry("/trip/pages/index")
  const gamesEntry = makeModuleEntry("/games/pages/index")
  router.add("GET", "/trip", tripEntry)
  router.add("GET", "/trip/", tripEntry)
  router.add("GET", "/games", gamesEntry)
  router.add("GET", "/games/", gamesEntry)

  /* ------------------------------------------------------------
   * knowledge-hub 模块：同样模式。页面路径有两级（/pages/learn/index）
   * 与三级（/pages/knowledge/graph/index）两种，两个段数都要登记。
   * ---------------------------------------------------------- */
  const knowledgeShell = makeModuleShell(
    path.join(WEB_DIR, "knowledge", "shell.html"),
    "knowledge 模块页面壳读取失败"
  )
  router.add("GET", "/knowledge/pages/:a/:b", knowledgeShell)
  router.add("GET", "/knowledge/pages/:a/:b/:c", knowledgeShell)
  const knowledgeEntry = makeModuleEntry("/knowledge/pages/dashboard/index")
  router.add("GET", "/knowledge", knowledgeEntry)
  router.add("GET", "/knowledge/", knowledgeEntry)
  // 段数不对的 /knowledge/pages/xxx 也返回壳，由前端提示「页面不存在」。
  router.add("GET", "/knowledge/pages/:a", knowledgeShell)

  /* ------------------------------------------------------------
   * aitimer 模块：AI 定时任务产物（AI 简报 / 代码仓巡检 / 每日一特性）。
   * 页面模式与其它模块一致 —— /aitimer/pages/* 返回模块 HTML 壳，
   * 实际页面由前端 cjs-loader 按路径加载。
   * ---------------------------------------------------------- */
  const aitimerShell = makeModuleShell(
    path.join(WEB_DIR, "aitimer", "shell.html"),
    "aitimer 模块页面壳读取失败"
  )
  router.add("GET", "/aitimer/pages/:a", aitimerShell)
  const aitimerEntry = makeModuleEntry("/aitimer/pages/index")
  router.add("GET", "/aitimer", aitimerEntry)
  router.add("GET", "/aitimer/", aitimerEntry)

  // AI 定时任务产物的三个接口（索引 / 单篇 / 原文）。
  registerAiTimerRoutes(router, sendJson)

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
