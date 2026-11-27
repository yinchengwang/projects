"use strict"
/**
 * nav.js —— 模块内跳转
 *
 * 页面的视图模型里存的是完整路径（/knowledge/pages/...），但 handler 的
 * dataset 有可能拿到小程序形态的 /pages/...（沿用小程序版的写法）。
 * 这里统一补全前缀，避免 platform.navigateTo 把它当成 workbench 的页面。
 *
 * 单独成文件而不是每页各写一遍：17 个页面都要这段归一化。
 */

const platform = require("../../shared/platform.js")

/** 模块前缀 */
const PREFIX = "/knowledge"

/**
 * 把页面路径归一化成完整 URL。
 * @param {string} path 形如 /knowledge/pages/... 或 /pages/...
 * @returns {string}
 */
function toUrl (path) {
  const text = String(path === undefined || path === null ? "" : path)
  if (!text) return ""
  if (text.indexOf(PREFIX + "/") === 0) return text
  if (text.indexOf("/pages/") === 0) return PREFIX + text
  return text
}

/**
 * 跳到模块内页面。
 * @param {string} path
 */
function go (path) {
  const url = toUrl(path)
  if (url) platform.navigateTo(url)
}

module.exports = { toUrl: toUrl, go: go }
