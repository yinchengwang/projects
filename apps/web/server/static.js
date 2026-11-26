"use strict"
/**
 * static.js —— 静态文件托管
 *
 * 只服务 webRoot 内的文件。路径越界（../）一律拒绝。
 *
 * 支持**多个挂载点**：把一个 URL 前缀映射到磁盘目录。
 * 之所以需要多挂载点：`apps/web/web/` 是页面静态根，而 `apps/web/shared/`
 * 是共用层（浏览器要能 fetch 到 shared/core/*.js 才能用 cjs-loader 加载）。
 * 两者不在同一个目录下，但都要能通过 HTTP 取到。
 */

const fs = require("fs")
const path = require("path")

const MIME = {
  ".html": "text/html; charset=utf-8",
  ".js": "text/javascript; charset=utf-8",
  ".css": "text/css; charset=utf-8",
  ".json": "application/json; charset=utf-8",
  ".svg": "image/svg+xml",
  ".png": "image/png",
  ".jpg": "image/jpeg",
  ".webp": "image/webp",
  ".ico": "image/x-icon",
  ".woff2": "font/woff2",
  ".md": "text/markdown; charset=utf-8"
}

/**
 * 把 URL 路径映射到 baseDir 内的文件；越界或编码非法一律拒绝。
 * @param {string} urlPath 已经切掉查询串的路径
 * @param {string} baseDir 静态根目录
 * @returns {string|null} 绝对路径，不合法返回 null
 */
function resolveStatic (urlPath, baseDir) {
  const base = path.resolve(baseDir)
  let clean
  try {
    clean = decodeURIComponent(urlPath)
  } catch (e) {
    return null
  }
  const rel = clean === "/" ? "index.html" : clean.replace(/^\/+/, "")
  const abs = path.resolve(base, rel)
  if (abs !== base && abs.indexOf(base + path.sep) !== 0) return null
  return abs
}

/**
 * 尝试把一个绝对路径的文件发出去。
 * @returns {boolean} 发出去了返回 true；文件不存在或不是普通文件返回 false
 */
function trySendFile (req, res, abs) {
  if (abs === null) {
    res.writeHead(403, { "Content-Type": "text/plain; charset=utf-8" })
    res.end("禁止访问")
    return true
  }
  let stat
  try {
    stat = fs.statSync(abs)
  } catch (e) {
    return false
  }
  if (!stat.isFile()) return false
  const ext = path.extname(abs).toLowerCase()
  const headers = {
    "Content-Type": MIME[ext] || "application/octet-stream",
    "Content-Length": String(stat.size)
  }
  res.writeHead(200, headers)
  if (req.method === "HEAD") {
    res.end()
    return true
  }
  res.end(fs.readFileSync(abs))
  return true
}

/**
 * 创建静态文件处理器。
 *
 * @param {{webRoot: string, mounts?: Array<{prefix: string, dir: string}>}} opts
 *   webRoot 是主根（URL 路径直接映射）；
 *   mounts 是额外挂载点（URL 前缀 → 磁盘目录），按前缀长度从长到短匹配。
 * @returns {(req: object, res: object) => boolean} 命中并已响应返回 true
 */
function createStaticHandler (opts) {
  const options = opts || {}
  const webRoot = options.webRoot
  const mounts = (options.mounts || []).slice().sort(function (a, b) {
    return b.prefix.length - a.prefix.length
  })

  return function serveStatic (req, res) {
    if (req.method !== "GET" && req.method !== "HEAD") return false
    const rawUrl = req.url || "/"
    const rawPath = rawUrl.split("?")[0]

    // 先试额外挂载点
    for (const mount of mounts) {
      if (rawPath.indexOf(mount.prefix) !== 0) continue
      const rest = rawPath.slice(mount.prefix.length)
      const abs = resolveStatic(rest === "" ? "/" : rest, mount.dir)
      if (trySendFile(req, res, abs)) return true
      // 挂载点前缀匹配但文件不存在 → 继续往下试（不阻断主根）
    }

    // 再试主根
    return trySendFile(req, res, resolveStatic(rawPath, webRoot))
  }
}

module.exports = {
  MIME: MIME,
  resolveStatic: resolveStatic,
  trySendFile: trySendFile,
  createStaticHandler: createStaticHandler
}
