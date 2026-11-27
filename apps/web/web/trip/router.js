"use strict"
/**
 * router.js —— trip 模块内的路由表
 *
 * 路径沿用小程序的页面路径，挂在 /trip/ 前缀下。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/index": function () { return require("./pages/index.js") },
  "/pages/new-trip": function () { return require("./pages/new-trip.js") },
  "/pages/detail": function () { return require("./pages/detail.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /trip/pages/detail?id=xxx
 * @returns {string|null}
 */
function resolve (pathname) {
  const prefix = "/trip"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  return null
}

/**
 * 取 URL 上的查询参数。
 * @param {string} pathname
 * @returns {object} 键值对
 */
function query (pathname) {
  const out = {}
  if (typeof pathname !== "string") return out
  const q = pathname.indexOf("?")
  if (q < 0) return out
  const params = new URLSearchParams(pathname.slice(q + 1))
  params.forEach(function (v, k) { out[k] = v })
  return out
}

module.exports = { ROUTES: ROUTES, resolve: resolve, query: query }
