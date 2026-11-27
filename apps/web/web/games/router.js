"use strict"
/**
 * router.js —— games 模块内的路由表
 *
 * 路径沿用小程序的页面路径，挂在 /games/ 前缀下。
 */

/** 路径 → 加载函数 */
const ROUTES = {
  "/pages/index": function () { return require("./pages/index.js") },
  "/pages/snake": function () { return require("./pages/snake.js") },
  "/pages/game2048": function () { return require("./pages/game2048.js") },
  "/pages/sudoku": function () { return require("./pages/sudoku.js") },
  "/pages/match3": function () { return require("./pages/match3.js") }
}

/**
 * 从完整 URL 里解出模块内路径。
 * @param {string} pathname 形如 /games/pages/snake
 * @returns {string|null}
 */
function resolve (pathname) {
  const prefix = "/games"
  if (typeof pathname !== "string") return null
  if (pathname.indexOf(prefix) !== 0) return null
  const rest = pathname.slice(prefix.length) || "/"
  const clean = rest.split("?")[0]
  if (ROUTES[clean]) return clean
  return null
}

module.exports = { ROUTES: ROUTES, resolve: resolve }
