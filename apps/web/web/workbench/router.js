"use strict"
/**
 * router.js —— workbench 模块内的路由表
 *
 * 路径沿用小程序的页面路径（/pages/tech/notes/index），
 * 挂在 /workbench/ 前缀下 —— 最终 URL 是 /workbench/pages/tech/notes/index。
 *
 * 用懒加载：require 在函数体里，首屏只加载真正要用的页面。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/dashboard/index": function () { return require("./pages/dashboard/index.js") },
  "/pages/profile/index": function () { return require("./pages/profile/index.js") },
  "/pages/tech/index": function () { return require("./pages/tech/index.js") },
  "/pages/tech/notes/index": function () { return require("./pages/tech/notes/index.js") },
  "/pages/tech/notes/detail": function () { return require("./pages/tech/notes/detail.js") },
  "/pages/tech/snippets/index": function () { return require("./pages/tech/snippets/index.js") },
  "/pages/tech/snippets/detail": function () { return require("./pages/tech/snippets/detail.js") },
  "/pages/tech/interview/index": function () { return require("./pages/tech/interview/index.js") },
  "/pages/tech/interview/detail": function () { return require("./pages/tech/interview/detail.js") },
  "/pages/tech/resources/index": function () { return require("./pages/tech/resources/index.js") },
  "/pages/life/index": function () { return require("./pages/life/index.js") },
  "/pages/life/reading/index": function () { return require("./pages/life/reading/index.js") },
  "/pages/life/fitness/index": function () { return require("./pages/life/fitness/index.js") },
  "/pages/life/mood/index": function () { return require("./pages/life/mood/index.js") },
  "/pages/life/schedule/index": function () { return require("./pages/life/schedule/index.js") },
  "/pages/projects/index": function () { return require("./pages/projects/index.js") },
  "/pages/insights/index": function () { return require("./pages/insights/index.js") },
  "/pages/tools/index": function () { return require("./pages/tools/index.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /workbench/pages/tech/notes/index
 * @returns {string|null} 模块内路径；不在本模块内返回 null
 */
function resolve (pathname) {
  const prefix = "/workbench"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  // 带查询串的详情页：剥掉 ?id=xxx 再试
  const stripped = clean.split("?")[0]
  if (ROUTES[stripped]) return stripped
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
