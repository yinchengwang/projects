"use strict"
/**
 * router.js —— knowledge-hub 模块内的路由表
 *
 * 路径沿用小程序的页面路径，挂在 /knowledge/ 前缀下 ——
 * 最终 URL 是 /knowledge/pages/learn/quiz/index。
 *
 * 用懒加载：require 在函数体里，首屏只加载真正要用的页面。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/dashboard/index": function () { return require("./pages/dashboard/index.js") },
  "/pages/learn/index": function () { return require("./pages/learn/index.js") },
  "/pages/learn/quiz/index": function () { return require("./pages/learn/quiz/index.js") },
  "/pages/learn/review/index": function () { return require("./pages/learn/review/index.js") },
  "/pages/learn/deep/index": function () { return require("./pages/learn/deep/index.js") },
  "/pages/learn/path/index": function () { return require("./pages/learn/path/index.js") },
  "/pages/interview/index": function () { return require("./pages/interview/index.js") },
  "/pages/interview/tracker/index": function () { return require("./pages/interview/tracker/index.js") },
  "/pages/interview/bank/index": function () { return require("./pages/interview/bank/index.js") },
  "/pages/interview/mock/index": function () { return require("./pages/interview/mock/index.js") },
  "/pages/knowledge/index": function () { return require("./pages/knowledge/index.js") },
  "/pages/knowledge/graph/index": function () { return require("./pages/knowledge/graph/index.js") },
  "/pages/knowledge/excerpt/index": function () { return require("./pages/knowledge/excerpt/index.js") },
  "/pages/knowledge/radar/index": function () { return require("./pages/knowledge/radar/index.js") },
  "/pages/profile/index": function () { return require("./pages/profile/index.js") },
  "/pages/plan/five-year/index": function () { return require("./pages/plan/five-year/index.js") },
  "/pages/plan/roadmap/index": function () { return require("./pages/plan/roadmap/index.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /knowledge/pages/learn/quiz/index
 * @returns {string|null} 模块内路径；不在本模块内返回 null
 */
function resolve (pathname) {
  const prefix = "/knowledge"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
