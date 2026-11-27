"use strict"
/**
 * theme-bridge.js —— 主题刷新后把结果同步回页面 data
 *
 * shared/core/with-theme.js 的 applyTheme 只写 page.state（Web 侧页面直接读 state），
 * 但页面自己的 setData 语义是「data 是唯一事实来源」，两者在同一页上会分叉。
 * 这里补一次 setData，把四个主题键写回 data，让 `this.data.isDark` 之类的
 * 取值与 state 保持一致。
 */

/** 主题键 */
const THEME_KEYS = ["themeClass", "isDark", "brand", "brandSoft"]

/**
 * 刷新主题并把结果同步进 data。
 * @param {object} page 页面实例（需有 refreshTheme）
 * @returns {object} 主题数据
 */
function syncTheme (page) {
  if (!page || typeof page.refreshTheme !== "function") return null
  const data = page.refreshTheme()
  if (!data) return null
  const patch = {}
  THEME_KEYS.forEach(function (key) {
    if (data[key] !== undefined) patch[key] = data[key]
  })
  if (typeof page.setData === "function") page.setData(patch)
  return data
}

module.exports = { THEME_KEYS: THEME_KEYS, syncTheme: syncTheme }
